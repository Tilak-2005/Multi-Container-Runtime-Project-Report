/*
 * monitor.c — Kernel-Space Memory Monitor LKM
 *
 * Tracks container processes by PID, checks RSS every second,
 * emits dmesg warnings on soft-limit crossings, and sends SIGKILL
 * on hard-limit violations.
 *
 * Communicates with user-space via /dev/container_monitor (ioctl).
 *
 * Build:  make module
 * Load:   sudo insmod monitor.ko
 * Verify: ls -l /dev/container_monitor && dmesg | tail -3
 * Unload: sudo rmmod monitor
 */


#include <linux/version.h>
#include <linux/timer.h>
#include <linux/time.h>        
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <linux/signal.h>
#include <linux/mm.h>
#include <linux/pid.h>

#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Container Runtime Team");
MODULE_DESCRIPTION("Container memory monitor — soft/hard RSS limit enforcement");
MODULE_VERSION("1.0");

#define DEVICE_NAME         "container_monitor"
#define CLASS_NAME          "cmon"
#define CHECK_INTERVAL_MS   1000   /* RSS poll interval */

/* ------------------------------------------------------------------ */
/* Per-container tracking entry (kernel linked list node)             */
/* ------------------------------------------------------------------ */
struct cmon_entry {
    struct list_head list;
    pid_t            pid;
    long             soft_mib;
    long             hard_mib;
    char             id[64];
    bool             soft_warned;   /* emit soft warning only once */
};

static LIST_HEAD(cmon_list);
static DEFINE_MUTEX(cmon_mutex);

/* ------------------------------------------------------------------ */
/* Character device bookkeeping                                        */
/* ------------------------------------------------------------------ */
static int            cmon_major;
static struct class  *cmon_class;
static struct device *cmon_device;
static struct cdev    cmon_cdev;

/* ------------------------------------------------------------------ */
/* Timer for periodic RSS checks                                       */
/* ------------------------------------------------------------------ */
static struct timer_list cmon_timer;

/* ------------------------------------------------------------------ */
/* Helper: send a signal to a PID                                     */
/* ------------------------------------------------------------------ */
static void send_signal_to_pid(pid_t pid, int sig)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(sig, task, 1);
    rcu_read_unlock();
}

/* ------------------------------------------------------------------ */
/* Helper: get RSS in MiB for a PID                                   */
/* Returns -1 if the process no longer exists.                        */
/* ------------------------------------------------------------------ */
static long get_rss_mib(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    long                rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }

    mm = get_task_mm(task);
    rcu_read_unlock();

    if (!mm)
        return 0;

    rss_pages = get_mm_rss(mm);
    mmput(mm);

    /* Convert pages → MiB (PAGE_SIZE is typically 4096) */
    return (rss_pages * PAGE_SIZE) >> 20;
}

/* ------------------------------------------------------------------ */
/* Timer callback: walk the list, enforce limits                      */
/* ------------------------------------------------------------------ */
static void cmon_check(struct timer_list *t)
{
    struct cmon_entry *entry, *tmp;

    mutex_lock(&cmon_mutex);

    list_for_each_entry_safe(entry, tmp, &cmon_list, list) {
        long rss = get_rss_mib(entry->pid);

        if (rss < 0) {
            /* Process has exited — remove stale entry */
            pr_info("cmon: [%s] pid %d exited, removing from watchlist\n",
                    entry->id, entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (rss >= entry->hard_mib) {
            pr_warn("cmon: [%s] pid %d RSS %ld MiB >= hard limit %ld MiB — sending SIGKILL\n",
                    entry->id, entry->pid, rss, entry->hard_mib);
            send_signal_to_pid(entry->pid, SIGKILL);
            /* Leave entry; supervisor will call UNREGISTER after reaping */
        } else if (rss >= entry->soft_mib && !entry->soft_warned) {
            pr_warn("cmon: [%s] pid %d RSS %ld MiB >= soft limit %ld MiB — WARNING\n",
                    entry->id, entry->pid, rss, entry->soft_mib);
            entry->soft_warned = true;
        }
    }

    mutex_unlock(&cmon_mutex);

    /* Re-arm the timer */
    mod_timer(&cmon_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));
}

/* ------------------------------------------------------------------ */
/* ioctl handler                                                       */
/* ------------------------------------------------------------------ */
static long cmon_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {

    case MONITOR_REGISTER: {
        struct monitor_reg reg;
        struct cmon_entry *entry;

        if (copy_from_user(&reg, (void __user *)arg, sizeof(reg)))
            return -EFAULT;

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid        = reg.pid;
        entry->soft_mib   = reg.soft_mib;
        entry->hard_mib   = reg.hard_mib;
        entry->soft_warned = false;
        strncpy(entry->id, reg.id, sizeof(entry->id) - 1);
        entry->id[sizeof(entry->id) - 1] = '\0';

        mutex_lock(&cmon_mutex);
        list_add_tail(&entry->list, &cmon_list);
        mutex_unlock(&cmon_mutex);

        pr_info("cmon: registered [%s] pid %d soft=%ldMiB hard=%ldMiB\n",
                entry->id, entry->pid, entry->soft_mib, entry->hard_mib);
        return 0;
    }

    case MONITOR_UNREGISTER: {
        struct monitor_unreg unreg;
        struct cmon_entry *entry, *tmp;
        bool found = false;

        if (copy_from_user(&unreg, (void __user *)arg, sizeof(unreg)))
            return -EFAULT;

        mutex_lock(&cmon_mutex);
        list_for_each_entry_safe(entry, tmp, &cmon_list, list) {
            if (entry->pid == unreg.pid) {
                list_del(&entry->list);
                kfree(entry);
                found = true;
                break;
            }
        }
        mutex_unlock(&cmon_mutex);

        if (!found)
            pr_warn("cmon: UNREGISTER pid %d not found\n", unreg.pid);
        else
            pr_info("cmon: unregistered pid %d\n", unreg.pid);
        return 0;
    }

    default:
        return -EINVAL;
    }
}

/* ------------------------------------------------------------------ */
/* File operations                                                     */
/* ------------------------------------------------------------------ */
static const struct file_operations cmon_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = cmon_ioctl,
};

/* ------------------------------------------------------------------ */
/* Module init                                                         */
/* ------------------------------------------------------------------ */
static int __init cmon_init(void)
{
    dev_t dev;
    int   ret;

    ret = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("cmon: alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }
    cmon_major = MAJOR(dev);

    cdev_init(&cmon_cdev, &cmon_fops);
    cmon_cdev.owner = THIS_MODULE;
    ret = cdev_add(&cmon_cdev, dev, 1);
    if (ret) {
        unregister_chrdev_region(dev, 1);
        return ret;
    }

    cmon_class = class_create(CLASS_NAME);
    if (IS_ERR(cmon_class)) {
        cdev_del(&cmon_cdev);
        unregister_chrdev_region(dev, 1);
        return PTR_ERR(cmon_class);
    }

    cmon_device = device_create(cmon_class, NULL, dev, NULL, DEVICE_NAME);
    if (IS_ERR(cmon_device)) {
        class_destroy(cmon_class);
        cdev_del(&cmon_cdev);
        unregister_chrdev_region(dev, 1);
        return PTR_ERR(cmon_device);
    }

    /* Start the periodic RSS timer */
    timer_setup(&cmon_timer, cmon_check, 0);
    mod_timer(&cmon_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));

    pr_info("cmon: loaded — /dev/%s major=%d\n", DEVICE_NAME, cmon_major);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Module exit: free all list entries, destroy device                 */
/* ------------------------------------------------------------------ */
static void __exit cmon_exit(void)
{
    struct cmon_entry *entry, *tmp;
    dev_t dev = MKDEV(cmon_major, 0);

    #if LINUX_VERSION_CODE >= KERNEL_VERSION(5,15,0)
    timer_shutdown_sync(&cmon_timer);
#else
    del_timer_sync(&cmon_timer);
#endif

    mutex_lock(&cmon_mutex);
    list_for_each_entry_safe(entry, tmp, &cmon_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&cmon_mutex);

    device_destroy(cmon_class, dev);
    class_destroy(cmon_class);
    cdev_del(&cmon_cdev);
    unregister_chrdev_region(dev, 1);

    pr_info("cmon: unloaded\n");
}

module_init(cmon_init);
module_exit(cmon_exit);