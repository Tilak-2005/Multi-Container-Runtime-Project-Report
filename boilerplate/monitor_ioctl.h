#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

#define MONITOR_MAGIC 'M'

/*
 * Register a container PID with soft/hard memory limits (MiB).
 * Called by the supervisor after clone() returns the child PID.
 */
struct monitor_reg {
    pid_t  pid;
    long   soft_mib;
    long   hard_mib;
    char   id[64];       /* container name — used in dmesg messages */
};

/*
 * Unregister a PID when the container exits or is stopped.
 */
struct monitor_unreg {
    pid_t  pid;
};

#define MONITOR_REGISTER    _IOW(MONITOR_MAGIC, 1, struct monitor_reg)
#define MONITOR_UNREGISTER  _IOW(MONITOR_MAGIC, 2, struct monitor_unreg)

#endif /* MONITOR_IOCTL_H */
