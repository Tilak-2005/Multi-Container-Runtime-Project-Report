/*
 * engine.c — Multi-Container Runtime with Parent Supervisor
 *
 * Two modes of operation:
 *   Daemon:  engine supervisor <base-rootfs>
 *   Client:  engine start|run|stop|ps|logs <args>
 *
 * Isolation: CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS + chroot
 * IPC Path A (logging):  container stdout/stderr → pipes → ring buffer → log files
 * IPC Path B (control):  CLI → UNIX domain socket → supervisor
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sched.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <limits.h>

#include "monitor_ioctl.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */
#define MAX_CONTAINERS      32
#define LOG_DIR             "/tmp/engine_logs"
#define SOCKET_PATH         "/tmp/engine_supervisor.sock"
#define LOG_BUF_CAPACITY    256          /* ring-buffer slot count     */
#define LOG_LINE_MAX        4096         /* max bytes per log entry    */
#define MONITOR_DEV         "/dev/container_monitor"
#define DEFAULT_SOFT_MIB    40
#define DEFAULT_HARD_MIB    64
#define CLONE_STACK_SIZE    (1024 * 1024)

/* ------------------------------------------------------------------ */
/* Container state                                                     */
/* ------------------------------------------------------------------ */
typedef enum {
    STATE_STARTING = 0,
    STATE_RUNNING,
    STATE_STOPPED,
    STATE_KILLED,
    STATE_HARD_LIMIT_KILLED,
    STATE_EXITED
} ContainerState;

static const char *state_str(ContainerState s)
{
    switch (s) {
    case STATE_STARTING:          return "starting";
    case STATE_RUNNING:           return "running";
    case STATE_STOPPED:           return "stopped";
    case STATE_KILLED:            return "killed";
    case STATE_HARD_LIMIT_KILLED: return "hard_limit_killed";
    case STATE_EXITED:            return "exited";
    default:                      return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Container metadata                                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    bool           active;
    char           id[64];
    pid_t          host_pid;
    time_t         start_time;
    ContainerState state;
    long           soft_mib;
    long           hard_mib;
    int            nice_val;
    char           log_path[PATH_MAX];
    char           rootfs[PATH_MAX];
    char           command[256];
    int            exit_code;
    int            exit_signal;
    bool           stop_requested;

    /* Pipe fds for reading container stdout/stderr (supervisor side) */
    int            pipe_stdout_rd;
    int            pipe_stderr_rd;
} Container;

static Container        containers[MAX_CONTAINERS];
static pthread_mutex_t  containers_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Ring buffer (bounded buffer) for logging — Path A                  */
/* ------------------------------------------------------------------ */
typedef struct {
    char     lines[LOG_BUF_CAPACITY][LOG_LINE_MAX];
    int      head;           /* consumer reads from here */
    int      tail;           /* producer writes here     */
    int      count;
    bool     done;           /* set when all producers have finished */
    pthread_mutex_t mu;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
} LogBuffer;

static LogBuffer log_buf;

static void logbuf_init(LogBuffer *lb)
{
    lb->head  = 0;
    lb->tail  = 0;
    lb->count = 0;
    lb->done  = false;
    pthread_mutex_init(&lb->mu, NULL);
    pthread_cond_init(&lb->not_full,  NULL);
    pthread_cond_init(&lb->not_empty, NULL);
}

/* Producer: block when buffer is full */
static void logbuf_push(LogBuffer *lb, const char *line)
{
    pthread_mutex_lock(&lb->mu);
    while (lb->count == LOG_BUF_CAPACITY && !lb->done)
        pthread_cond_wait(&lb->not_full, &lb->mu);
    if (lb->done) {
        pthread_mutex_unlock(&lb->mu);
        return;
    }
    strncpy(lb->lines[lb->tail], line, LOG_LINE_MAX - 1);
    lb->lines[lb->tail][LOG_LINE_MAX - 1] = '\0';
    lb->tail  = (lb->tail + 1) % LOG_BUF_CAPACITY;
    lb->count++;
    pthread_cond_signal(&lb->not_empty);
    pthread_mutex_unlock(&lb->mu);
}

/* Consumer: block when buffer is empty; returns false when done+empty */
static bool logbuf_pop(LogBuffer *lb, char *out)
{
    pthread_mutex_lock(&lb->mu);
    while (lb->count == 0) {
        if (lb->done) {
            pthread_mutex_unlock(&lb->mu);
            return false;
        }
        pthread_cond_wait(&lb->not_empty, &lb->mu);
    }
    memcpy(out, lb->lines[lb->head], LOG_LINE_MAX - 1); out[LOG_LINE_MAX - 1] = '\0';
    lb->head  = (lb->head + 1) % LOG_BUF_CAPACITY;
    lb->count--;
    pthread_cond_signal(&lb->not_full);
    pthread_mutex_unlock(&lb->mu);
    return true;
}

static void logbuf_set_done(LogBuffer *lb)
{
    pthread_mutex_lock(&lb->mu);
    lb->done = true;
    pthread_cond_broadcast(&lb->not_empty);
    pthread_cond_broadcast(&lb->not_full);
    pthread_mutex_unlock(&lb->mu);
}

/* ------------------------------------------------------------------ */
/* Consumer thread: drains ring buffer → log files                    */
/* ------------------------------------------------------------------ */
static void *consumer_thread(void *arg)
{
    (void)arg;
    char line[LOG_LINE_MAX];

    while (logbuf_pop(&log_buf, line)) {
        /* Format: "CONTAINERID|STREAM| message\n" */
        char container_id[64] = "";
        char stream[8]        = "";
        char *msg             = line;

        /* Parse the encoded prefix written by the producer */
        char *pipe1 = strchr(line, '|');
        if (pipe1) {
            size_t id_len = (size_t)(pipe1 - line);
            if (id_len < sizeof(container_id)) {
                strncpy(container_id, line, id_len);
                container_id[id_len] = '\0';
            }
            char *pipe2 = strchr(pipe1 + 1, '|');
            if (pipe2) {
                size_t s_len = (size_t)(pipe2 - pipe1 - 1);
                if (s_len < sizeof(stream)) {
                    strncpy(stream, pipe1 + 1, s_len);
                    stream[s_len] = '\0';
                }
                msg = pipe2 + 1;
            }
        }

        /* Find log file path */
        char log_path[PATH_MAX] = "";
        pthread_mutex_lock(&containers_mutex);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (containers[i].active &&
                strcmp(containers[i].id, container_id) == 0) {
                strncpy(log_path, containers[i].log_path, PATH_MAX - 1);
                break;
            }
        }
        pthread_mutex_unlock(&containers_mutex);

        if (log_path[0] == '\0') {
            /* Container gone — write to a fallback file */
            snprintf(log_path, PATH_MAX, "%s/%s.log", LOG_DIR, container_id);
        }

               int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            /* Write timestamp + stream + message */
            time_t now = time(NULL);
            char ts[32];
            struct tm *tm_info = localtime(&now);
            strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm_info);

            char payload[LOG_LINE_MAX + 64];
            int pay_len = snprintf(payload, sizeof(payload),
                                   "[%s] [%s] %s", ts, stream, msg);
            if (pay_len > 0)
                (void)write(fd, payload, (size_t)pay_len);
            if (pay_len > 0 && payload[pay_len - 1] != '\n')
                (void)write(fd, "\n", 1);
            close(fd);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Producer thread args                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    int  fd;
    char container_id[64];
    char stream[8];   /* "stdout" or "stderr" */
} ProducerArgs;

/* Producer thread: reads from one pipe end, pushes into ring buffer  */
static void *producer_thread(void *arg)
{
    ProducerArgs *pa = (ProducerArgs *)arg;
    char buf[LOG_LINE_MAX];
    char line[LOG_LINE_MAX];
    int  line_pos = 0;

    while (1) {
        ssize_t n = read(pa->fd, buf, sizeof(buf) - 1);
        if (n <= 0)
            break;
        buf[n] = '\0';

        /* Split on newlines, push complete lines */
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n' || line_pos == LOG_LINE_MAX - 2) {
                line[line_pos] = '\0';
                char entry[LOG_LINE_MAX];
                snprintf(entry, sizeof(entry), "%s|%s|%s",
                         pa->container_id, pa->stream, line);
                logbuf_push(&log_buf, entry);
                line_pos = 0;
            } else {
                line[line_pos++] = buf[i];
            }
        }
    }

    /* Flush any partial line */
    if (line_pos > 0) {
        line[line_pos] = '\0';
        char entry[LOG_LINE_MAX];
        snprintf(entry, sizeof(entry), "%s|%s|%s",
                 pa->container_id, pa->stream, line);
        logbuf_push(&log_buf, entry);
    }

    close(pa->fd);
    free(pa);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Kernel monitor registration helpers                                 */
/* ------------------------------------------------------------------ */
static int monitor_fd = -1;

static void monitor_open(void)
{
    monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (monitor_fd < 0)
        fprintf(stderr, "[supervisor] Warning: cannot open %s — memory enforcement disabled\n",
                MONITOR_DEV);
}

static void monitor_register(const char *id, pid_t pid, long soft, long hard)
{
    if (monitor_fd < 0) return;
    struct monitor_reg reg;
    memset(&reg, 0, sizeof(reg));
    reg.pid      = pid;
    reg.soft_mib = soft;
    reg.hard_mib = hard;
    strncpy(reg.id, id, sizeof(reg.id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &reg) < 0)
        perror("ioctl MONITOR_REGISTER");
}

static void monitor_unregister(pid_t pid)
{
    if (monitor_fd < 0) return;
    struct monitor_unreg unreg;
    unreg.pid = pid;
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &unreg) < 0)
        perror("ioctl MONITOR_UNREGISTER");
}

/* ------------------------------------------------------------------ */
/* SIGCHLD handling                                                    */
/* ------------------------------------------------------------------ */
static volatile sig_atomic_t got_sigchld = 0;
static volatile sig_atomic_t got_sigterm = 0;

static void sigchld_handler(int sig)
{
    (void)sig;
    got_sigchld = 1;
}

static void sigterm_handler(int sig)
{
    (void)sig;
    got_sigterm = 1;
}

static void reap_children(void)
{
    int   status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&containers_mutex);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (!containers[i].active || containers[i].host_pid != pid)
                continue;

            if (WIFEXITED(status)) {
                containers[i].exit_code = WEXITSTATUS(status);
                containers[i].exit_signal = 0;
                if (containers[i].stop_requested)
                    containers[i].state = STATE_STOPPED;
                else
                    containers[i].state = STATE_EXITED;
            } else if (WIFSIGNALED(status)) {
                containers[i].exit_signal = WTERMSIG(status);
                containers[i].exit_code   = 128 + containers[i].exit_signal;
                if (WTERMSIG(status) == SIGKILL && !containers[i].stop_requested)
                    containers[i].state = STATE_HARD_LIMIT_KILLED;
                else if (containers[i].stop_requested)
                    containers[i].state = STATE_KILLED;
                else
                    containers[i].state = STATE_KILLED;
            }

            monitor_unregister(pid);
            fprintf(stderr, "[supervisor] container [%s] pid %d exited: %s\n",
                    containers[i].id, pid, state_str(containers[i].state));
            break;
        }
        pthread_mutex_unlock(&containers_mutex);
    }
}

/* ------------------------------------------------------------------ */
/* Container entry point — runs inside clone()                        */
/* ------------------------------------------------------------------ */
typedef struct {
    char  id[64];
    char  rootfs[PATH_MAX];
    char  command[256];
    char *argv[64];
    int   argc;
    int   nice_val;
    int   pipe_stdout[2];
    int   pipe_stderr[2];
} CloneArgs;

static int container_main(void *arg)
{
    CloneArgs *ca = (CloneArgs *)arg;

    /* Redirect stdout/stderr to the supervisor's pipes */
    close(ca->pipe_stdout[0]);
    close(ca->pipe_stderr[0]);
    dup2(ca->pipe_stdout[1], STDOUT_FILENO);
    dup2(ca->pipe_stderr[1], STDERR_FILENO);
    close(ca->pipe_stdout[1]);
    close(ca->pipe_stderr[1]);

    /* Set nice value */
    if (ca->nice_val != 0) {
        if (nice(ca->nice_val) < 0 && errno != 0)
            perror("nice");
    }

    /* Set hostname to container ID (UTS namespace) */
    if (sethostname(ca->id, strlen(ca->id)) < 0) { /* non-fatal */ }

    /* chroot into the container's rootfs */
    if (chroot(ca->rootfs) < 0) {
        perror("chroot");
        _exit(1);
    }
    if (chdir("/") < 0) {
        perror("chdir /");
        _exit(1);
    }

    /* Mount /proc so ps, top, etc. work inside the container */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        /* Non-fatal — may already exist */
    }

    /* Execute the requested command */
    execv(ca->command, ca->argv);
    perror("execv");
    _exit(127);
}

/* ------------------------------------------------------------------ */
/* Supervisor: allocate a container slot and clone the child          */
/* ------------------------------------------------------------------ */
static int find_free_slot(void)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (!containers[i].active) return i;
    return -1;
}

static int do_start(const char *id, const char *rootfs, const char *command,
                    long soft_mib, long hard_mib, int nice_val,
                    char *err_out, size_t err_sz)
{
    pthread_mutex_lock(&containers_mutex);

    /* Check for duplicate ID */
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].active && strcmp(containers[i].id, id) == 0) {
            pthread_mutex_unlock(&containers_mutex);
            snprintf(err_out, err_sz, "ERROR container '%s' already running", id);
            return -1;
        }
    }

    int slot = find_free_slot();
    if (slot < 0) {
        pthread_mutex_unlock(&containers_mutex);
        snprintf(err_out, err_sz, "ERROR max containers reached");
        return -1;
    }

    Container *c   = &containers[slot];
    memset(c, 0, sizeof(*c));
    c->active       = true;
    c->soft_mib     = soft_mib;
    c->hard_mib     = hard_mib;
    c->nice_val     = nice_val;
    c->start_time   = time(NULL);
    c->state        = STATE_STARTING;
    c->pipe_stdout_rd = -1;
    c->pipe_stderr_rd = -1;
    strncpy(c->id,      id,      sizeof(c->id)      - 1);
    strncpy(c->rootfs,  rootfs,  sizeof(c->rootfs)  - 1);
    strncpy(c->command, command, sizeof(c->command) - 1);
    snprintf(c->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, id);

    pthread_mutex_unlock(&containers_mutex);

    /* Create pipes for stdout/stderr capture */
    int pout[2], perr[2];
    if (pipe(pout) < 0 || pipe(perr) < 0) {
        perror("pipe");
        pthread_mutex_lock(&containers_mutex);
        c->active = false;
        pthread_mutex_unlock(&containers_mutex);
        snprintf(err_out, err_sz, "ERROR pipe creation failed");
        return -1;
    }

    /* Build CloneArgs on the heap (child will access it after clone) */
    CloneArgs *ca = calloc(1, sizeof(CloneArgs));
    strncpy(ca->id,      id,      sizeof(ca->id)      - 1);
    strncpy(ca->rootfs,  rootfs,  sizeof(ca->rootfs)  - 1);
    strncpy(ca->command, command, sizeof(ca->command) - 1);
    ca->nice_val        = nice_val;
    ca->pipe_stdout[0]  = pout[0];
    ca->pipe_stdout[1]  = pout[1];
    ca->pipe_stderr[0]  = perr[0];
    ca->pipe_stderr[1]  = perr[1];

    /* Build argv: command + any spaces parsed as separate args */
    ca->argv[0] = ca->command;
    ca->argc    = 1;
    /* (Simple single-token command for this runtime; extend if needed) */
    ca->argv[ca->argc] = NULL;

    /* Allocate stack for child */
    char *stack = malloc(CLONE_STACK_SIZE);
    if (!stack) {
        free(ca);
        close(pout[0]); close(pout[1]);
        close(perr[0]); close(perr[1]);
        snprintf(err_out, err_sz, "ERROR malloc stack failed");
        return -1;
    }
    char *stack_top = stack + CLONE_STACK_SIZE;

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(container_main, stack_top, flags, ca);
    if (pid < 0) {
        perror("clone");
        free(stack);
        free(ca);
        close(pout[0]); close(pout[1]);
        close(perr[0]); close(perr[1]);
        snprintf(err_out, err_sz, "ERROR clone failed: %s", strerror(errno));
        return -1;
    }

    /* Supervisor keeps the read ends */
    close(pout[1]);
    close(perr[1]);

    pthread_mutex_lock(&containers_mutex);
    c->host_pid       = pid;
    c->state          = STATE_RUNNING;
    c->pipe_stdout_rd = pout[0];
    c->pipe_stderr_rd = perr[0];
    pthread_mutex_unlock(&containers_mutex);

    /* Register with kernel memory monitor */
    monitor_register(id, pid, soft_mib, hard_mib);

    /* Spawn producer threads for stdout and stderr */
    ProducerArgs *pa_out = malloc(sizeof(ProducerArgs));
    pa_out->fd = pout[0];
    strncpy(pa_out->container_id, id, sizeof(pa_out->container_id) - 1);
    strncpy(pa_out->stream, "stdout", sizeof(pa_out->stream) - 1);

    ProducerArgs *pa_err = malloc(sizeof(ProducerArgs));
    pa_err->fd = perr[0];
    strncpy(pa_err->container_id, id, sizeof(pa_err->container_id) - 1);
    strncpy(pa_err->stream, "stderr", sizeof(pa_err->stream) - 1);

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, producer_thread, pa_out);
    pthread_create(&t, &attr, producer_thread, pa_err);
    pthread_attr_destroy(&attr);

    free(stack);  /* stack was copied by clone — safe to free */
    free(ca);

    fprintf(stderr, "[supervisor] started container [%s] pid=%d\n", id, pid);
    return pid;
}

/* ------------------------------------------------------------------ */
/* Command handler — called by the supervisor for each IPC message    */
/* ------------------------------------------------------------------ */
static void send_msg(int fd, const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    (void)write(fd, buf, strlen(buf));
}

static void handle_command(int client_fd, const char *cmd_line)
{
    /* Tokenise */
    char buf[4096];
    strncpy(buf, cmd_line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *tokens[128];
    int   ntok = 0;
    char *p    = strtok(buf, " \t\n");
    while (p && ntok < 127) {
        tokens[ntok++] = p;
        p = strtok(NULL, " \t\n");
    }
    tokens[ntok] = NULL;
    if (ntok == 0) { send_msg(client_fd, "ERROR empty command\n"); return; }

    /* ps */
    if (strcmp(tokens[0], "ps") == 0) {
        send_msg(client_fd,
                 "%-16s %-8s %-20s %-10s %5s %5s %5s\n",
                 "ID", "PID", "STARTED", "STATE", "SOFT", "HARD", "NICE");
        pthread_mutex_lock(&containers_mutex);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (!containers[i].active) continue;
            char ts[32];
            struct tm *tm_info = localtime(&containers[i].start_time);
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);
            send_msg(client_fd,
                     "%-16s %-8d %-20s %-10s %4ldM %4ldM %5d\n",
                     containers[i].id,
                     containers[i].host_pid,
                     ts,
                     state_str(containers[i].state),
                     containers[i].soft_mib,
                     containers[i].hard_mib,
                     containers[i].nice_val);
        }
        pthread_mutex_unlock(&containers_mutex);
        send_msg(client_fd, "OK\n");
        return;
    }

    /* logs <id> */
    if (strcmp(tokens[0], "logs") == 0 && ntok >= 2) {
        char log_path[PATH_MAX] = "";
        pthread_mutex_lock(&containers_mutex);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (containers[i].active &&
                strcmp(containers[i].id, tokens[1]) == 0) {
                strncpy(log_path, containers[i].log_path, PATH_MAX - 1);
                break;
            }
        }
        pthread_mutex_unlock(&containers_mutex);

        if (log_path[0] == '\0') {
            /* Try to read log even if container has exited */
            snprintf(log_path, PATH_MAX, "%s/%s.log", LOG_DIR, tokens[1]);
        }

        FILE *f = fopen(log_path, "r");
        if (!f) {
            send_msg(client_fd, "ERROR no log for '%s'\n", tokens[1]);
            return;
        }
        char line[4096];
        while (fgets(line, sizeof(line), f))
            send_msg(client_fd, "%s", line);
        fclose(f);
        send_msg(client_fd, "OK\n");
        return;
    }

    /* stop <id> */
    if (strcmp(tokens[0], "stop") == 0 && ntok >= 2) {
        pthread_mutex_lock(&containers_mutex);
        bool found = false;
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (!containers[i].active ||
                strcmp(containers[i].id, tokens[1]) != 0) continue;
            containers[i].stop_requested = true;
            kill(containers[i].host_pid, SIGTERM);
            found = true;
            break;
        }
        pthread_mutex_unlock(&containers_mutex);
        if (found)
            send_msg(client_fd, "OK stopping '%s'\n", tokens[1]);
        else
            send_msg(client_fd, "ERROR container '%s' not found\n", tokens[1]);
        return;
    }

    /* start / run <id> <rootfs> <cmd> [opts] */
    if ((strcmp(tokens[0], "start") == 0 ||
         strcmp(tokens[0], "run")   == 0) && ntok >= 4) {

        bool  is_run   = (strcmp(tokens[0], "run") == 0);
        char *id       = tokens[1];
        char *rootfs   = tokens[2];
        char *command  = tokens[3];
        long  soft_mib = DEFAULT_SOFT_MIB;
        long  hard_mib = DEFAULT_HARD_MIB;
        int   nice_val = 0;

        for (int i = 4; i < ntok; i++) {
            if (strcmp(tokens[i], "--soft-mib") == 0 && i + 1 < ntok)
                soft_mib = atol(tokens[++i]);
            else if (strcmp(tokens[i], "--hard-mib") == 0 && i + 1 < ntok)
                hard_mib = atol(tokens[++i]);
            else if (strcmp(tokens[i], "--nice") == 0 && i + 1 < ntok)
                nice_val = atoi(tokens[++i]);
        }

        char err[512] = "";
        int pid = do_start(id, rootfs, command, soft_mib, hard_mib, nice_val,
                           err, sizeof(err));
        if (pid < 0) {
            send_msg(client_fd, "%s\n", err);
            return;
        }

        if (!is_run) {
            send_msg(client_fd, "OK started '%s' pid=%d\n", id, pid);
            return;
        }

        /* run: wait for the container to exit, then report status */
        send_msg(client_fd, "OK started '%s' pid=%d (waiting...)\n", id, pid);

        /* Poll until the container state changes from RUNNING/STARTING */
        while (1) {
            usleep(200000);  /* 200 ms poll */
            pthread_mutex_lock(&containers_mutex);
            ContainerState st = STATE_RUNNING;
            int exit_code = 0;
            for (int i = 0; i < MAX_CONTAINERS; i++) {
                if (containers[i].active &&
                    strcmp(containers[i].id, id) == 0) {
                    st        = containers[i].state;
                    exit_code = containers[i].exit_code;
                    break;
                }
            }
            pthread_mutex_unlock(&containers_mutex);

            if (st != STATE_RUNNING && st != STATE_STARTING) {
                send_msg(client_fd,
                         "EXITED '%s' state=%s exit_code=%d\n",
                         id, state_str(st), exit_code);
                return;
            }
        }
    }

    send_msg(client_fd, "ERROR unknown command: %s\n", tokens[0]);
}

/* ------------------------------------------------------------------ */
/* UNIX socket helpers                                                 */
/* ------------------------------------------------------------------ */
static int create_server_socket(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    unlink(SOCKET_PATH);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, 16) < 0) {
        perror("listen"); close(fd); return -1;
    }
    return fd;
}

static int connect_to_supervisor(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect to supervisor — is it running?");
        close(fd);
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------ */
/* Supervisor main loop                                               */
/* ------------------------------------------------------------------ */
static void run_supervisor(const char *base_rootfs)
{
    (void)base_rootfs;

    /* Ensure log directory exists */
    mkdir(LOG_DIR, 0755);

    /* Install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* Open kernel monitor */
    monitor_open();

    /* Start the consumer thread */
    logbuf_init(&log_buf);
    pthread_t consumer_tid;
    pthread_create(&consumer_tid, NULL, consumer_thread, NULL);

    /* Create UNIX socket server */
    int server_fd = create_server_socket();
    if (server_fd < 0) {
        fprintf(stderr, "[supervisor] Failed to create socket\n");
        exit(1);
    }

    fprintf(stderr, "[supervisor] ready on %s\n", SOCKET_PATH);

    /* Main event loop */
    while (!got_sigterm) {
        if (got_sigchld) {
            got_sigchld = 0;
            reap_children();
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);

        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        int ret = select(server_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (ret > 0 && FD_ISSET(server_fd, &rfds)) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) {
                if (errno != EINTR) perror("accept");
                continue;
            }

            /* Read the command (simple single recv) */
            char cmd[4096] = "";
            ssize_t n = recv(client_fd, cmd, sizeof(cmd) - 1, 0);
            if (n > 0) {
                cmd[n] = '\0';
                handle_command(client_fd, cmd);
            }
            close(client_fd);
        }
    }

    /* Orderly shutdown */
    fprintf(stderr, "[supervisor] shutting down...\n");

    /* Stop all running containers */
    pthread_mutex_lock(&containers_mutex);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].active && containers[i].state == STATE_RUNNING) {
            containers[i].stop_requested = true;
            kill(containers[i].host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&containers_mutex);

    /* Wait for children */
    sleep(1);
    reap_children();

    /* Drain and stop the logging pipeline */
    logbuf_set_done(&log_buf);
    pthread_join(consumer_tid, NULL);

    close(server_fd);
    unlink(SOCKET_PATH);
    if (monitor_fd >= 0) close(monitor_fd);

    fprintf(stderr, "[supervisor] clean shutdown complete\n");
}

/* ------------------------------------------------------------------ */
/* CLI client: send command, print response                           */
/* ------------------------------------------------------------------ */
static void run_client(int argc, char *argv[])
{
    /* Build command string from argv */
    char cmd[4096] = "";
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
    }

    int fd = connect_to_supervisor();
    if (fd < 0) exit(1);

    /* Handle SIGINT/SIGTERM for "run" command */
    bool is_run = (argc >= 2 && strcmp(argv[1], "run") == 0);
    const char *run_id = (is_run && argc >= 3) ? argv[2] : NULL;

    if (send(fd, cmd, strlen(cmd), 0) < 0) {
        perror("send");
        close(fd);
        exit(1);
    }

    /* Stream response until connection closes */
    char buf[4096];
    int  exit_code = 0;
    while (1) {
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        printf("%s", buf);
        fflush(stdout);

        /* For "run", parse exit code from EXITED line */
        if (is_run) {
            char *p = strstr(buf, "EXITED");
            if (p) {
                char *ec = strstr(p, "exit_code=");
                if (ec) exit_code = atoi(ec + 10);
            }
        }

        /* Stop reading if we got a terminal line */
        if (strstr(buf, "\nOK") || strstr(buf, "OK\n") ||
            strstr(buf, "ERROR") || strstr(buf, "EXITED"))
            break;
    }

    (void)run_id;
    close(fd);
    exit(exit_code);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start  <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run    <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs   <id>\n"
            "  %s stop   <id>\n",
            argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        run_supervisor(argv[2]);
        return 0;
    }

    /* All other commands are CLI client invocations */
    run_client(argc, argv);
    return 0;
}
