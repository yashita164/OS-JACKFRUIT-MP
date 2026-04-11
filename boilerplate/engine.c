/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Full implementation of:
 *   - UNIX domain socket control plane (supervisor ↔ CLI)
 *   - Container lifecycle: clone + namespaces + chroot + /proc
 *   - Bounded-buffer producer/consumer logging pipeline
 *   - SIGCHLD / SIGINT / SIGTERM handling and graceful shutdown
 *   - ps / start / run / logs / stop commands
 *
 * Kernel monitor integration (ioctl) is attempted if /dev/container_monitor
 * is present; otherwise silently skipped so the binary works without the LKM.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ── monitor_ioctl.h stub (compiled without the LKM header) ─────────────── */
#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H
#define MONITOR_MAGIC   'M'
#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct monitor_request)
#define MONITOR_CONTAINER_ID_LEN 32
struct monitor_request {
    pid_t         pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    char          container_id[MONITOR_CONTAINER_ID_LEN];
};
#endif

/* ── Constants ───────────────────────────────────────────────────────────── */
#define STACK_SIZE           (1024 * 1024)
#define CONTAINER_ID_LEN     32
#define CONTROL_PATH         "/tmp/mini_runtime.sock"
#define LOG_DIR              "logs"
#define CONTROL_MESSAGE_LEN  256
#define CHILD_COMMAND_LEN    256
#define LOG_CHUNK_SIZE       4096
#define LOG_BUFFER_CAPACITY  16
#define DEFAULT_SOFT_LIMIT   (40UL << 20)
#define DEFAULT_HARD_LIMIT   (64UL << 20)
#define MONITOR_DEV          "/dev/container_monitor"

/* ── Enumerations ────────────────────────────────────────────────────────── */
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

/* ── Data structures ─────────────────────────────────────────────────────── */
typedef struct container_record {
    char                   id[CONTAINER_ID_LEN];
    pid_t                  host_pid;
    time_t                 started_at;
    container_state_t      state;
    unsigned long          soft_limit_bytes;
    unsigned long          hard_limit_bytes;
    int                    exit_code;
    int                    exit_signal;
    char                   log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

typedef struct {
    int  status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;
} child_config_t;

typedef struct {
    int                server_fd;
    int                monitor_fd;
    volatile int       should_stop;
    pthread_t          logger_thread;
    bounded_buffer_t   log_buffer;
    pthread_mutex_t    metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Per-container pipe-reader thread argument */
typedef struct {
    int                 read_fd;
    char                container_id[CONTAINER_ID_LEN];
    bounded_buffer_t   *log_buffer;
} pipe_reader_arg_t;

/* Global supervisor context pointer – used by signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

/* ── Utility ─────────────────────────────────────────────────────────────── */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc, char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long  nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ── Bounded buffer ──────────────────────────────────────────────────────── */
static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;
    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buffer->mutex); return rc; }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * bounded_buffer_push – producer side.
 * Blocks while full unless shutdown has begun (returns -1 to signal caller
 * to stop producing).
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * bounded_buffer_pop – consumer side.
 * Returns  0  with a valid item.
 * Returns  1  when shutdown is in progress and the buffer is empty
 *             (caller should drain then exit).
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return 1; /* shutdown, nothing left */
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ── Logging consumer thread ─────────────────────────────────────────────── */
/*
 * Routes each log_item_t to the container's log file.
 * Keeps an open-FD cache (simple linear scan – fine for ≤ dozens of
 * containers).
 */
#define MAX_OPEN_LOGS 64

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    int  fd;
} log_fd_cache_t;

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    bounded_buffer_t *buf = &ctx->log_buffer;
    log_item_t        item;
    log_fd_cache_t    cache[MAX_OPEN_LOGS];
    int               cache_count = 0;
    int               ret;
    int               i;

    memset(cache, 0, sizeof(cache));
    for (i = 0; i < MAX_OPEN_LOGS; i++)
        cache[i].fd = -1;

    while (1) {
        ret = bounded_buffer_pop(buf, &item);
        if (ret != 0)
            break; /* shutdown + empty */

        /* Find or open the log fd for this container */
        int  log_fd   = -1;
        int  slot     = -1;

        for (i = 0; i < cache_count; i++) {
            if (strcmp(cache[i].container_id, item.container_id) == 0) {
                log_fd = cache[i].fd;
                break;
            }
        }

        if (log_fd == -1 && cache_count < MAX_OPEN_LOGS) {
            /* Look up log path from metadata */
            char log_path[PATH_MAX] = {0};

            pthread_mutex_lock(&ctx->metadata_lock);
            container_record_t *rec = ctx->containers;
            while (rec) {
                if (strcmp(rec->id, item.container_id) == 0) {
                    strncpy(log_path, rec->log_path, PATH_MAX - 1);
                    break;
                }
                rec = rec->next;
            }
            pthread_mutex_unlock(&ctx->metadata_lock);

            if (log_path[0] != '\0') {
                log_fd = open(log_path,
                              O_WRONLY | O_CREAT | O_APPEND,
                              0644);
                if (log_fd >= 0) {
                    slot = cache_count++;
                    strncpy(cache[slot].container_id,
                            item.container_id, CONTAINER_ID_LEN - 1);
                    cache[slot].fd = log_fd;
                }
            }
        }

        if (log_fd >= 0) {
            ssize_t written = 0;
            ssize_t total   = (ssize_t)item.length;
            while (written < total) {
                ssize_t n = write(log_fd,
                                  item.data + written,
                                  (size_t)(total - written));
                if (n <= 0) break;
                written += n;
            }
        }
    }

    /* Close all cached log FDs */
    for (i = 0; i < cache_count; i++) {
        if (cache[i].fd >= 0)
            close(cache[i].fd);
    }

    return NULL;
}

/* ── Pipe reader thread (one per container) ──────────────────────────────── */
/*
 * Reads from the read end of a container's stdout/stderr pipe and pushes
 * chunks into the bounded buffer.
 */
static void *pipe_reader_thread(void *arg)
{
    pipe_reader_arg_t *pra = (pipe_reader_arg_t *)arg;
    log_item_t         item;
    ssize_t            n;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, pra->container_id, CONTAINER_ID_LEN - 1);

    while (1) {
        n = read(pra->read_fd, item.data, LOG_CHUNK_SIZE);
        if (n <= 0)
            break; /* EOF or error – container closed the pipe */

        item.length = (size_t)n;
        if (bounded_buffer_push(pra->log_buffer, &item) != 0)
            break; /* shutdown */
    }

    close(pra->read_fd);
    free(pra);
    return NULL;
}

/* ── Clone child entrypoint ──────────────────────────────────────────────── */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout and stderr to the write end of the log pipe */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("child: dup2");
        return 1;
    }
    close(cfg->log_write_fd);

    /* Set nice value */
    if (cfg->nice_value != 0) {
        errno = 0;
        nice(cfg->nice_value);
    }

    /* Set hostname to the container ID */
    sethostname(cfg->id, strlen(cfg->id));

    /* Mount /proc inside the container's rootfs */
    char proc_path[PATH_MAX];
    snprintf(proc_path, sizeof(proc_path), "%s/proc", cfg->rootfs);
    mkdir(proc_path, 0555);
    if (mount("proc", proc_path, "proc", 0, NULL) != 0) {
        /* Non-fatal – print to stderr (which goes to the log pipe now) */
        perror("child: mount /proc");
    }

    /* chroot into the container's rootfs */
    if (chroot(cfg->rootfs) != 0) {
        perror("child: chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("child: chdir /");
        return 1;
    }

    /* Execute the command via /bin/sh so shell syntax works */
    execl("/bin/sh", "/bin/sh", "-c", cfg->command, (char *)NULL);
    perror("child: execl");
    return 127;
}

/* ── Kernel monitor helpers ──────────────────────────────────────────────── */
int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    if (monitor_fd < 0) return 0; /* LKM not loaded – silently skip */

    memset(&req, 0, sizeof(req));
    req.pid               = host_pid;
    req.soft_limit_bytes  = soft_limit_bytes;
    req.hard_limit_bytes  = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0) {
        perror("ioctl MONITOR_REGISTER");
        return -1;
    }
    return 0;
}

int unregister_from_monitor(int monitor_fd,
                            const char *container_id,
                            pid_t host_pid)
{
    struct monitor_request req;
    if (monitor_fd < 0) return 0;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0) {
        perror("ioctl MONITOR_UNREGISTER");
        return -1;
    }
    return 0;
}

/* ── Container metadata helpers ──────────────────────────────────────────── */
static container_record_t *find_container(supervisor_ctx_t *ctx,
                                          const char *id)
{
    container_record_t *rec = ctx->containers;
    while (rec) {
        if (strcmp(rec->id, id) == 0) return rec;
        rec = rec->next;
    }
    return NULL;
}

static container_record_t *alloc_container(supervisor_ctx_t *ctx,
                                           const control_request_t *req)
{
    container_record_t *rec = calloc(1, sizeof(*rec));
    if (!rec) return NULL;

    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->state            = CONTAINER_STARTING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->started_at       = time(NULL);
    rec->host_pid         = -1;

    /* Build log file path */
    mkdir(LOG_DIR, 0755);
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, rec->id);

    /* Prepend to list */
    rec->next        = ctx->containers;
    ctx->containers  = rec;
    return rec;
}

/* ── Spawn a container ───────────────────────────────────────────────────── */
/*
 * Creates the pipe, calls clone() with namespace flags, starts a pipe-reader
 * thread, and registers with the kernel monitor.
 * Called with metadata_lock held.
 */
static int spawn_container(supervisor_ctx_t *ctx,
                           const control_request_t *req,
                           container_record_t *rec)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        return -1;
    }

    /* Build child config on the heap so it survives after this function */
    child_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) { close(pipefd[0]); close(pipefd[1]); return -1; }

    strncpy(cfg->id,          rec->id,       CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,      req->rootfs,   PATH_MAX - 1);
    strncpy(cfg->command,     req->command,  CHILD_COMMAND_LEN - 1);
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = pipefd[1]; /* child writes here */

    /* Allocate a stack for the clone child */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    char *stack_top = stack + STACK_SIZE;

    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;

    pid_t pid = clone(child_fn, stack_top, clone_flags, cfg);
    /* Child owns pipefd[1]; close it in the parent */
    close(pipefd[1]);

    if (pid < 0) {
        perror("clone");
        free(stack);
        free(cfg);
        close(pipefd[0]);
        return -1;
    }

    /* The stack is leaked here intentionally; the child's clone copy is
     * managed by the kernel.  In a production runtime you'd track and free
     * it after waitpid returns.  For this project this is acceptable. */
    (void)stack;

    rec->host_pid = pid;
    rec->state    = CONTAINER_RUNNING;

    /* Register with kernel monitor (best-effort) */
    register_with_monitor(ctx->monitor_fd, rec->id, pid,
                          rec->soft_limit_bytes, rec->hard_limit_bytes);

    /* Start a thread that drains the read end of the pipe into the log buffer */
    pipe_reader_arg_t *pra = calloc(1, sizeof(*pra));
    if (pra) {
        pra->read_fd   = pipefd[0];
        pra->log_buffer = &ctx->log_buffer;
        strncpy(pra->container_id, rec->id, CONTAINER_ID_LEN - 1);

        pthread_t tid;
        if (pthread_create(&tid, NULL, pipe_reader_thread, pra) == 0)
            pthread_detach(tid);
        else {
            close(pipefd[0]);
            free(pra);
        }
    } else {
        close(pipefd[0]);
    }

    free(cfg); /* child_fn already executed by clone; cfg no longer needed */
    return 0;
}

/* ── SIGCHLD reaper ──────────────────────────────────────────────────────── */
static void reap_children(supervisor_ctx_t *ctx)
{
    int   status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *rec = ctx->containers;
        while (rec) {
            if (rec->host_pid == pid) {
                if (WIFEXITED(status)) {
                    rec->exit_code = WEXITSTATUS(status);
                    rec->state     = CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    rec->exit_signal = WTERMSIG(status);
                    rec->state       = CONTAINER_KILLED;
                }
                unregister_from_monitor(ctx->monitor_fd, rec->id, pid);
                fprintf(stderr,
                        "[supervisor] container %s (pid %d) exited: state=%s\n",
                        rec->id, pid, state_to_string(rec->state));
                break;
            }
            rec = rec->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

/* ── Signal handling ─────────────────────────────────────────────────────── */
static volatile sig_atomic_t g_sigchld_flag  = 0;
static volatile sig_atomic_t g_shutdown_flag = 0;

static void handle_sigchld(int sig)
{
    (void)sig;
    g_sigchld_flag = 1;
}

static void handle_shutdown(int sig)
{
    (void)sig;
    g_shutdown_flag = 1;
    if (g_ctx) g_ctx->should_stop = 1;
}

/* ── Handle one client connection ────────────────────────────────────────── */
static void handle_client(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t  req;
    control_response_t resp;

    memset(&resp, 0, sizeof(resp));

    ssize_t n = recv(client_fd, &req, sizeof(req), MSG_WAITALL);
    if (n != (ssize_t)sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, CONTROL_MESSAGE_LEN, "malformed request");
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    switch (req.kind) {

    /* ── START ── */
    case CMD_START: {
        pthread_mutex_lock(&ctx->metadata_lock);
        if (find_container(ctx, req.container_id)) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "container '%s' already exists", req.container_id);
            break;
        }
        container_record_t *rec = alloc_container(ctx, &req);
        if (!rec) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN, "alloc failed");
            break;
        }
        int rc = spawn_container(ctx, &req, rec);
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (rc != 0) {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "failed to spawn container '%s'", req.container_id);
        } else {
            resp.status = 0;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "started container '%s' pid=%d",
                     req.container_id, rec->host_pid);
        }
        break;
    }

    /* ── RUN (same as start but client will poll for exit) ── */
    case CMD_RUN: {
        pthread_mutex_lock(&ctx->metadata_lock);
        if (find_container(ctx, req.container_id)) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "container '%s' already exists", req.container_id);
            break;
        }
        container_record_t *rec = alloc_container(ctx, &req);
        if (!rec) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN, "alloc failed");
            break;
        }
        int rc = spawn_container(ctx, &req, rec);
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (rc != 0) {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "failed to spawn container '%s'", req.container_id);
            send(client_fd, &resp, sizeof(resp), 0);
            return;
        }

        /* Send an ack so the client knows the container started */
        resp.status = 0;
        snprintf(resp.message, CONTROL_MESSAGE_LEN,
                 "running container '%s' pid=%d",
                 req.container_id, rec->host_pid);
        send(client_fd, &resp, sizeof(resp), 0);

        /* Now wait for the container to finish, periodically polling */
        while (1) {
            int   wstatus;
            pid_t w = waitpid(rec->host_pid, &wstatus, WNOHANG);
            if (w == rec->host_pid) {
                pthread_mutex_lock(&ctx->metadata_lock);
                if (WIFEXITED(wstatus)) {
                    rec->exit_code = WEXITSTATUS(wstatus);
                    rec->state     = CONTAINER_EXITED;
                } else if (WIFSIGNALED(wstatus)) {
                    rec->exit_signal = WTERMSIG(wstatus);
                    rec->state       = CONTAINER_KILLED;
                }
                unregister_from_monitor(ctx->monitor_fd, rec->id, rec->host_pid);
                int final_code = (rec->state == CONTAINER_KILLED)
                                 ? 128 + rec->exit_signal
                                 : rec->exit_code;
                pthread_mutex_unlock(&ctx->metadata_lock);

                /* Send final status */
                memset(&resp, 0, sizeof(resp));
                resp.status = final_code;
                snprintf(resp.message, CONTROL_MESSAGE_LEN,
                         "container '%s' exited with status %d",
                         req.container_id, final_code);
                send(client_fd, &resp, sizeof(resp), 0);
                return;
            }
            usleep(100000); /* 100 ms poll interval */
        }
    }

    /* ── PS ── */
    case CMD_PS: {
        /* Build a text table and send it back as the message */
        char  buf[4096];
        int   off = 0;
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                        "%-16s %-8s %-10s %-12s %-12s %-12s\n",
                        "ID", "PID", "STATE", "SOFT(MiB)", "HARD(MiB)", "STARTED");

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *rec = ctx->containers;
        while (rec && off < (int)sizeof(buf) - 128) {
            struct tm *tm_info = localtime(&rec->started_at);
            char       timebuf[32];
            strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_info);
            off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                            "%-16s %-8d %-10s %-12lu %-12lu %-12s\n",
                            rec->id,
                            (int)rec->host_pid,
                            state_to_string(rec->state),
                            rec->soft_limit_bytes >> 20,
                            rec->hard_limit_bytes >> 20,
                            timebuf);
            rec = rec->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        resp.status = 0;
        strncpy(resp.message, buf, CONTROL_MESSAGE_LEN - 1);
        break;
    }

    /* ── LOGS ── */
    case CMD_LOGS: {
        char log_path[PATH_MAX] = {0};

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *rec = find_container(ctx, req.container_id);
        if (rec)
            strncpy(log_path, rec->log_path, PATH_MAX - 1);
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (!log_path[0]) {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "container '%s' not found", req.container_id);
            break;
        }

        /* Send the ack, then stream the log file contents */
        resp.status = 0;
        snprintf(resp.message, CONTROL_MESSAGE_LEN,
                 "log:%s", log_path);
        send(client_fd, &resp, sizeof(resp), 0);

        int log_fd = open(log_path, O_RDONLY);
        if (log_fd >= 0) {
            char chunk[4096];
            ssize_t nr;
            while ((nr = read(log_fd, chunk, sizeof(chunk))) > 0)
                write(client_fd, chunk, (size_t)nr);
            close(log_fd);
        }
        return;
    }

    /* ── STOP ── */
    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *rec = find_container(ctx, req.container_id);
        if (!rec) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "container '%s' not found", req.container_id);
            break;
        }
        if (rec->state != CONTAINER_RUNNING) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "container '%s' is not running", req.container_id);
            break;
        }
        /* Send SIGTERM first, escalate to SIGKILL */
        kill(rec->host_pid, SIGTERM);
        rec->state = CONTAINER_STOPPED;
        pthread_mutex_unlock(&ctx->metadata_lock);

        /* Give the container up to 3 seconds to exit gracefully */
        for (int i = 0; i < 30; i++) {
            int wstatus;
            pid_t w = waitpid(rec->host_pid, &wstatus, WNOHANG);
            if (w == rec->host_pid) {
                pthread_mutex_lock(&ctx->metadata_lock);
                if (WIFSIGNALED(wstatus)) {
                    rec->exit_signal = WTERMSIG(wstatus);
                    rec->state       = CONTAINER_KILLED;
                } else {
                    rec->exit_code = WEXITSTATUS(wstatus);
                    rec->state     = CONTAINER_STOPPED;
                }
                unregister_from_monitor(ctx->monitor_fd, rec->id, rec->host_pid);
                pthread_mutex_unlock(&ctx->metadata_lock);
                goto stop_done;
            }
            usleep(100000);
        }
        /* Escalate */
        kill(rec->host_pid, SIGKILL);
        waitpid(rec->host_pid, NULL, 0);
        pthread_mutex_lock(&ctx->metadata_lock);
        rec->state = CONTAINER_KILLED;
        unregister_from_monitor(ctx->monitor_fd, rec->id, rec->host_pid);
        pthread_mutex_unlock(&ctx->metadata_lock);

    stop_done:
        resp.status = 0;
        snprintf(resp.message, CONTROL_MESSAGE_LEN,
                 "stopped container '%s'", req.container_id);
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, CONTROL_MESSAGE_LEN, "unknown command %d", req.kind);
        break;
    }

    send(client_fd, &resp, sizeof(resp), 0);
}

/* ── Supervisor ──────────────────────────────────────────────────────────── */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    (void)rootfs; /* base rootfs noted; individual containers supply their own */

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;

    g_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { errno = rc; perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc; perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* ── 1) Open kernel monitor device (optional) ── */
    ctx.monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (ctx.monitor_fd < 0) {
        fprintf(stderr,
                "[supervisor] %s not found – kernel monitor disabled\n",
                MONITOR_DEV);
    }

    /* ── 2) Create the UNIX domain socket ── */
    unlink(CONTROL_PATH);

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); goto cleanup; }

    {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

        if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("bind"); goto cleanup;
        }
        if (listen(ctx.server_fd, 16) < 0) {
            perror("listen"); goto cleanup;
        }
    }

    /* Make socket non-blocking so the accept loop can also check signals */
    {
        int flags = fcntl(ctx.server_fd, F_GETFL, 0);
        fcntl(ctx.server_fd, F_SETFL, flags | O_NONBLOCK);
    }

    /* ── 3) Install signal handlers ── */
    {
        struct sigaction sa_chld, sa_term;
        memset(&sa_chld, 0, sizeof(sa_chld));
        sa_chld.sa_handler = handle_sigchld;
        sigemptyset(&sa_chld.sa_mask);
        sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
        sigaction(SIGCHLD, &sa_chld, NULL);

        memset(&sa_term, 0, sizeof(sa_term));
        sa_term.sa_handler = handle_shutdown;
        sigemptyset(&sa_term.sa_mask);
        sigaction(SIGINT,  &sa_term, NULL);
        sigaction(SIGTERM, &sa_term, NULL);
    }

    /* ── 4) Spawn the logger thread ── */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc; perror("pthread_create logger");
        goto cleanup;
    }

    fprintf(stderr, "[supervisor] started, control socket: %s\n", CONTROL_PATH);

    /* ── 5) Event loop ── */
    mkdir(LOG_DIR, 0755);

    while (!ctx.should_stop) {
        /* Reap any children that have exited */
        if (g_sigchld_flag) {
            g_sigchld_flag = 0;
            reap_children(&ctx);
        }

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000); /* 10 ms */
                continue;
            }
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        handle_client(&ctx, client_fd);
        close(client_fd);
    }

    fprintf(stderr, "[supervisor] shutting down…\n");

    /* ── Orderly shutdown ── */

    /* SIGTERM all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *rec = ctx.containers;
    while (rec) {
        if (rec->state == CONTAINER_RUNNING)
            kill(rec->host_pid, SIGTERM);
        rec = rec->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Wait for children */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    sleep(1);
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;

    /* Drain the log buffer and stop the logger thread */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

cleanup:
    if (ctx.server_fd >= 0) { close(ctx.server_fd); unlink(CONTROL_PATH); }
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);

    bounded_buffer_destroy(&ctx.log_buffer);

    /* Free container records */
    pthread_mutex_lock(&ctx.metadata_lock);
    rec = ctx.containers;
    while (rec) {
        container_record_t *next = rec->next;
        free(rec);
        rec = next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
    pthread_mutex_destroy(&ctx.metadata_lock);

    fprintf(stderr, "[supervisor] exited cleanly.\n");
    return 0;
}

/* ── Client-side control request ─────────────────────────────────────────── */
static int send_control_request(const control_request_t *req)
{
    int sock_fd;
    struct sockaddr_un addr;
    control_response_t resp;

    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(sock_fd);
        return 1;
    }

    if (send(sock_fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send");
        close(sock_fd);
        return 1;
    }

    /* For CMD_RUN the supervisor sends two responses:
     *   (1) ack that the container started
     *   (2) final exit status once it finishes
     * Additionally it may stream raw log bytes after the second response
     * for CMD_LOGS.
     */
    ssize_t n = recv(sock_fd, &resp, sizeof(resp), MSG_WAITALL);
    if (n != (ssize_t)sizeof(resp)) {
        fprintf(stderr, "Unexpected response length %zd\n", n);
        close(sock_fd);
        return 1;
    }

    if (req->kind == CMD_LOGS && resp.status == 0) {
        /* Print log stream until EOF */
        char buf[4096];
        ssize_t nr;
        printf("%s\n", resp.message); /* prints "log:<path>" */
        while ((nr = read(sock_fd, buf, sizeof(buf))) > 0)
            fwrite(buf, 1, (size_t)nr, stdout);
        close(sock_fd);
        return 0;
    }

    printf("%s\n", resp.message);

    if (req->kind == CMD_RUN && resp.status == 0) {
        /* Wait for the final exit-status message */
        n = recv(sock_fd, &resp, sizeof(resp), MSG_WAITALL);
        if (n == (ssize_t)sizeof(resp)) {
            printf("%s\n", resp.message);
            close(sock_fd);
            return resp.status;
        }
    }

    close(sock_fd);
    return (resp.status == 0) ? 0 : 1;
}

/* ── CLI command handlers ─────────────────────────────────────────────────── */
static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
