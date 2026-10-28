/*
 * platform_linux.c — Linux/POSIX implementation of the platform abstraction layer
 *
 * This file is selected when building on Linux (uname -s == "Linux").
 * It implements the same interface as platform_windows.c, but using:
 *   fork() + execvp()   instead of  CreateProcess()
 *   waitpid(WNOHANG)    instead of  WaitForSingleObject(handle, 0)
 *   kill(pid, SIGTERM)  instead of  TerminateProcess()
 *   sigaction()         instead of  SetConsoleCtrlHandler()
 *   Unix domain sockets instead of  Named Pipes
 *   POSIX BSD sockets   instead of  Winsock2
 *
 * All OS-specific #includes live here. No other source file ever includes
 * Linux-specific headers — they only include platform.h.
 *
 * Key C / POSIX concepts demonstrated:
 *   - fork():  create a copy of the current process
 *   - execvp(): replace the process image with a new program
 *   - waitpid(WNOHANG): non-blocking check if a child has exited
 *   - sigaction(): install a signal handler (preferred over signal())
 *   - AF_UNIX sockets: local IPC without TCP overhead
 *   - FD_CLOEXEC: prevent file descriptors from leaking into exec'd children
 */

/* _POSIX_C_SOURCE enables POSIX features not in standard C11:
 *   200809L = POSIX.1-2008, which covers sigaction, clock_gettime, etc.
 * Without this, some POSIX declarations are hidden by the C library headers. */
#define _POSIX_C_SOURCE 200809L

#include "platform.h"

#include <stdio.h>       /* fprintf, snprintf */
#include <string.h>      /* memset, strncpy, strchr, strncmp */
#include <stdlib.h>      /* NULL, malloc, free */
#include <errno.h>       /* errno, EAGAIN, EWOULDBLOCK, EINTR, ENOENT */

/* Process management */
#include <unistd.h>      /* fork, execvp, dup2, close, chdir, _exit, pipe */
#include <sys/wait.h>    /* waitpid, WNOHANG, WIFEXITED, WEXITSTATUS,
                            WIFSIGNALED, WTERMSIG */
/* Signals */
#include <signal.h>      /* sigaction, struct sigaction, sigemptyset,
                            SIGTERM, SIGINT, SIGKILL, SIGPIPE, SIG_IGN */
/* File descriptor control */
#include <fcntl.h>       /* fcntl, F_GETFL, F_SETFL, F_SETFD,
                            O_NONBLOCK, O_RDWR, O_CREAT, O_WRONLY, O_APPEND,
                            FD_CLOEXEC */
/* Time */
#include <time.h>        /* clock_gettime, CLOCK_MONOTONIC, struct timespec */

/* Networking */
#include <sys/socket.h>  /* socket, connect, accept, listen, bind, send, recv,
                            setsockopt, SOL_SOCKET, SO_RCVTIMEO */
#include <sys/select.h>  /* select(), fd_set, FD_ZERO, FD_SET */
#include <sys/time.h>    /* struct timeval */
#include <sys/un.h>      /* struct sockaddr_un, AF_UNIX */
#include <netdb.h>       /* getaddrinfo, freeaddrinfo, struct addrinfo */
#include <arpa/inet.h>   /* htons, IPPROTO_TCP */
#include <netinet/in.h>  /* AF_INET, SOCK_STREAM */

/* ==========================================================================
 * Time functions
 * ==========================================================================
 */

int64_t platform_now_ms(void) {
    /*
     * clock_gettime(CLOCK_MONOTONIC) returns time since some arbitrary
     * start point (usually system boot). It NEVER goes backwards — unlike
     * CLOCK_REALTIME, which can jump when the system clock is adjusted via
     * NTP or manually. For measuring elapsed time and scheduling restarts,
     * monotonic time is always correct.
     *
     * tv_sec  = whole seconds
     * tv_nsec = nanosecond part (0..999,999,999)
     *
     * Convert to ms: sec * 1000 + nsec / 1,000,000
     */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

void platform_sleep_ms(int ms) {
    /*
     * nanosleep() is the POSIX high-resolution sleep.
     * We convert ms → { seconds, nanoseconds }.
     * The second arg (remaining time) would be filled in if a signal
     * interrupted the sleep. We pass NULL since we handle interruption
     * by re-checking ctx->running at the call site.
     */
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ==========================================================================
 * Signal handlers
 * ==========================================================================
 */

/*
 * g_shutdown_flag — pointer to the supervisor's 'running' field
 *
 * When SIGTERM or SIGINT arrives, the signal handler sets *g_shutdown_flag = 0.
 * The main event loop checks 'running' each tick and exits when it becomes 0.
 *
 * 'static' means this variable is private to this file.
 * Passing the flag via a pointer (rather than using a global int) keeps
 * the signal handling decoupled from the supervisor's data structures.
 */
static atomic_int *g_shutdown_flag = NULL;

static void signal_handler(int signum) {
    /*
     * Signal handlers have severe restrictions — only "async-signal-safe"
     * functions are allowed. That rules out printf, malloc, mutex locks, etc.
     * (They could be mid-execution when the signal arrives, causing deadlock.)
     *
     * Storing to a lock-free atomic_int is safe. That's all we do here.
     */
    (void)signum;
    if (g_shutdown_flag) *g_shutdown_flag = 0;
}

void platform_install_signal_handlers(atomic_int *shutdown_flag) {
    g_shutdown_flag = shutdown_flag;

    /*
     * sigaction() is the modern, POSIX-correct way to install signal handlers.
     * The older signal() function has implementation-defined behavior and
     * should not be used in new code.
     *
     * struct sigaction fields:
     *   sa_handler  — the handler function (or SIG_IGN to ignore)
     *   sa_mask     — signals to block WHILE the handler is running
     *   sa_flags    — modifiers (SA_RESTART, SA_SIGINFO, etc.)
     *
     * sigemptyset(&sa.sa_mask) means: don't block any extra signals during
     * handler execution. The signal that triggered the handler is automatically
     * blocked while it runs (standard behavior).
     */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGTERM, &sa, NULL);  /* kill / systemctl stop */
    sigaction(SIGINT,  &sa, NULL);  /* Ctrl+C                */

    /*
     * Ignore SIGPIPE — sent when writing to a socket/pipe after the
     * remote end has closed. Without SIG_IGN, this would kill guardian.
     * We handle write errors via return values instead.
     */
    struct sigaction sa_pipe;
    memset(&sa_pipe, 0, sizeof(sa_pipe));
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sigaction(SIGPIPE, &sa_pipe, NULL);
}

/* ==========================================================================
 * Process management
 * ==========================================================================
 */

/* The process environment. POSIX requires programs to declare it themselves. */
extern char **environ;

/*
 * parse_args_into — split an args string into an argv array
 *
 * Splits 'args_copy' in place on spaces and fills argv[] starting at
 * argv[start_idx]. argv[0..start_idx-1] must already be set by the caller
 * (typically argv[0] = command path). Returns the total argc (not counting
 * the trailing NULL).
 *
 * Why not strtok()? strtok remembers its position in hidden static state,
 * so two threads tokenizing at once corrupt each other — and health threads
 * call platform_spawn() for 'command' probes. This loop keeps its state in
 * a local pointer instead.
 *
 * Limitation: doesn't handle quoted arguments with internal spaces.
 * For the use cases guardian targets (simple daemons), this is sufficient.
 */
static int parse_args_into(char *args_copy, const char **argv,
                            int start_idx, int max_args) {
    int argc = start_idx;
    char *p = args_copy;

    while (*p && argc < max_args - 1) {
        while (*p == ' ') p++;          /* skip separators */
        if (*p == '\0') break;
        argv[argc++] = p;               /* token starts here */
        while (*p && *p != ' ') p++;    /* find its end */
        if (*p) *p++ = '\0';            /* terminate it and move on */
    }
    argv[argc] = NULL;
    return argc;
}

/* Does environment entry "NAME=..." define the same NAME as "NAME=VALUE"?
 * (Linux variable names are case-sensitive.) */
static int same_env_name(const char *a, const char *b) {
    while (*a && *a != '=' && *a == *b) { a++; b++; }
    return *a == '=' && *b == '=';
}

/*
 * build_envp — guardian's environment with the service's env vars applied
 *
 * An override replaces an inherited variable of the same name; new names
 * are appended. Returns a malloc'd NULL-terminated array. Only the array is
 * owned by the caller: the strings themselves still belong to environ and
 * env_vars, which outlive the exec. NULL on allocation failure.
 */
static char **build_envp(const char **env_vars, int env_count) {
    int inherited = 0;
    while (environ && environ[inherited]) inherited++;

    char **envp = malloc((size_t)(inherited + env_count + 1) * sizeof(char *));
    if (envp == NULL) return NULL;

    int count = 0;
    for (int i = 0; i < inherited; i++) envp[count++] = environ[i];

    for (int i = 0; i < env_count; i++) {
        if (strchr(env_vars[i], '=') == NULL) continue;  /* not KEY=VALUE */
        int j = 0;
        while (j < count && !same_env_name(envp[j], env_vars[i])) j++;
        envp[j] = (char *)env_vars[i];   /* replace, or append when j == count */
        if (j == count) count++;
    }
    envp[count] = NULL;
    return envp;
}

/*
 * resolve_command — find the executable the way execvp() would
 *
 * A command containing '/' is used as-is (a relative one resolves against
 * working_dir, because the child chdirs before exec). Otherwise each PATH
 * directory is tried in order; an empty entry means the current directory.
 * PATH comes from envp, so a service's own 'env = PATH=...' is honoured,
 * just as execvp() in the child used to honour it.
 *
 * Returns 1 and fills 'out' on success, 0 if nothing executable was found.
 */
static int resolve_command(const char *command, char *const *envp,
                           char *out, size_t out_size) {
    if (strchr(command, '/') != NULL) {
        int n = snprintf(out, out_size, "%s", command);
        return n > 0 && (size_t)n < out_size;
    }

    const char *path = "/bin:/usr/bin";  /* glibc's default when PATH is unset */
    for (char *const *e = envp; *e; e++) {
        if (strncmp(*e, "PATH=", 5) == 0) { path = *e + 5; break; }
    }

    for (;;) {
        const char *colon = strchr(path, ':');
        int dir_len = colon ? (int)(colon - path) : (int)strlen(path);
        int n = dir_len == 0
              ? snprintf(out, out_size, "%s", command)
              : snprintf(out, out_size, "%.*s/%s", dir_len, path, command);
        if (n > 0 && (size_t)n < out_size && access(out, X_OK) == 0) return 1;
        if (colon == NULL) return 0;
        path = colon + 1;
    }
}

/* Report a setup failure from the child and exit. Only async-signal-safe
 * calls: write(), strlen() and _exit(). _exit (not exit) skips stdio
 * flushing and atexit handlers, which belong to the parent. */
static _Noreturn void child_fail(const char *msg) {
    ssize_t ignored = write(STDERR_FILENO, msg, strlen(msg));
    (void)ignored;
    _exit(1);
}

SpawnResult platform_spawn(const char  *command,
                            const char  *args,
                            const char  *working_dir,
                            const char **env_vars,
                            int          env_count,
                            const char  *stdout_log,
                            const char  *stderr_log) {
    SpawnResult result;
    memset(&result, 0, sizeof(result));
    result.handle = PLATFORM_INVALID_HANDLE;  /* unused on POSIX */

    /*
     * WHY SO MUCH HAPPENS BEFORE fork()
     *
     * guardian is multithreaded (one health thread per checked service).
     * fork() copies only the CALLING thread into the child. If another
     * thread held a lock at that instant — say, inside malloc() or
     * setenv() — the child inherits that lock still locked, with no thread
     * left to ever unlock it. The first time the child needs that lock it
     * deadlocks: the service just hangs, never exec'd.
     *
     * POSIX's rule: in the child of a multithreaded process, call only
     * async-signal-safe functions until exec. The old code called setenv()
     * (which locks and mallocs) and execvp() (not on the safe list) there.
     *
     * So the parent does all the unsafe work up front — splitting args,
     * building the environment, searching PATH — and the child only runs
     * chdir/open/dup2/close/execve, which are all on the safe list.
     */

    /* argv layout: [ command, arg1, arg2, ..., NULL ] */
    char args_copy[256];
    strncpy(args_copy, args ? args : "", sizeof(args_copy) - 1);
    args_copy[sizeof(args_copy) - 1] = '\0';

    const char *argv[64];
    argv[0] = command;
    parse_args_into(args_copy, argv, 1, 64);

    char **envp = build_envp(env_vars, env_vars ? env_count : 0);
    if (envp == NULL) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "out of memory building environment");
        return result;
    }

    /* Resolved here rather than by execvp() in the child. Bonus: a missing
     * command is now reported as a spawn failure in guardian's log instead
     * of a child that mysteriously exits with code 1. */
    char exe_path[4096];
    if (!resolve_command(command, envp, exe_path, sizeof(exe_path))) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "'%s' not found in PATH", command);
        free(envp);
        return result;
    }

    /* ----------------------------------------------------------------
     * fork() — THE KEY CALL
     *
     * After fork(), we have two processes running the same code.
     * We use the return value to split into two paths:
     *
     *   pid > 0 → we are the PARENT; pid is the child's process ID
     *   pid == 0 → we are the CHILD
     *   pid < 0 → fork failed
     * ---------------------------------------------------------------- */
    pid_t pid = fork();

    if (pid < 0) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "fork() failed: %s", strerror(errno));
        free(envp);
        return result;
    }

    if (pid == 0) {
        /* ================================================================
         * CHILD PROCESS — async-signal-safe calls only from here to exec.
         * Everything it needs (argv, envp, exe_path) was prepared by the
         * parent; fork() gave the child its own copy of that memory.
         * ================================================================ */

        /* 1. Change working directory if configured */
        if (working_dir && working_dir[0] != '\0') {
            if (chdir(working_dir) != 0) {
                child_fail("guardian: chdir failed\n");
            }
        }

        /* 2. Redirect stdout to log file (if configured)
         *
         * dup2(old_fd, new_fd) makes new_fd an alias for old_fd.
         * After dup2(file_fd, STDOUT_FILENO), anything the child writes
         * to stdout goes to the log file instead of the terminal.
         * We close the original file_fd after duplicating it (it's redundant).
         */
        if (stdout_log && stdout_log[0] != '\0') {
            int fd = open(stdout_log,
                          O_WRONLY | O_CREAT | O_APPEND,
                          0644);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
        }

        /* 3. Redirect stderr to log file (if configured) */
        if (stderr_log && stderr_log[0] != '\0') {
            int fd = open(stderr_log,
                          O_WRONLY | O_CREAT | O_APPEND,
                          0644);
            if (fd >= 0) {
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
        }

        /* 4. exec — replace this process image with the target program
         *
         * execve() takes the already-resolved path and the environment
         * we built, so it needs no PATH search and no setenv(). argv and
         * envp must end with a NULL sentinel.
         *
         * If execve() RETURNS, it failed. Normal exec doesn't return.
         */
        execve(exe_path, (char *const *)argv, envp);
        child_fail("guardian: exec failed\n");
    }

    /* ====================================================================
     * PARENT PROCESS — continues here after fork()
     *
     * The child is now running independently. It has its own copy of envp,
     * so the parent's array can be freed. We store its PID and return.
     * ==================================================================== */
    free(envp);
    result.success = 1;
    result.pid     = pid;
    return result;
}

/*
 * platform_check_process — non-blocking check if a child has exited
 *
 * waitpid(pid, &status, WNOHANG):
 *   WNOHANG = "don't block if child hasn't changed state yet"
 *   Returns 0:   child is still running
 *   Returns pid: child has exited; inspect wstatus for how
 *   Returns -1:  error (e.g., invalid pid)
 *
 * WIFEXITED(wstatus)  — true if the process exited normally (exit() or return)
 * WEXITSTATUS(wstatus) — the exit code passed to exit()
 * WIFSIGNALED(wstatus) — true if the process was killed by a signal
 * WTERMSIG(wstatus)   — which signal killed it (SIGKILL=9, SIGSEGV=11, etc.)
 *
 * WHY DO WE NEED THIS? (zombie processes)
 * When a child exits on Linux, it enters "zombie" state — the process entry
 * stays in the process table to preserve the exit code until the parent
 * calls waitpid(). If we never call waitpid(), we accumulate zombies.
 * Calling waitpid() here "reaps" the zombie and frees the process entry.
 */
ProcessStatus platform_check_process(PlatformPid pid, PlatformHandle handle) {
    (void)handle;  /* unused on POSIX */

    ProcessStatus status;
    memset(&status, 0, sizeof(status));

    int wstatus = 0;
    pid_t result = waitpid(pid, &wstatus, WNOHANG);

    if (result == 0) {
        return status;  /* still running — exited=0 */
    }

    if (result < 0) {
        /* Process doesn't exist (already reaped, or invalid pid) */
        status.exited    = 1;
        status.exit_code = -1;
        return status;
    }

    /* result == pid: child changed state */
    status.exited = 1;

    if (WIFEXITED(wstatus)) {
        status.exit_code = WEXITSTATUS(wstatus);
    } else if (WIFSIGNALED(wstatus)) {
        status.by_signal  = 1;
        status.signal_num = WTERMSIG(wstatus);
        /* Convention: 128 + signal_num matches shell $? behavior */
        status.exit_code  = 128 + status.signal_num;
    }

    return status;
}

int platform_terminate(PlatformPid pid, PlatformHandle handle) {
    /*
     * SIGTERM is the polite termination signal — the process can catch it
     * and clean up gracefully (flush buffers, delete temp files, etc.).
     * Most well-behaved daemons handle SIGTERM.
     *
     * The pid <= 0 guard matters: kill(0, sig) signals our whole process
     * group and kill(-1, sig) signals EVERY process this user owns.
     * PLATFORM_INVALID_PID is -1, so a stale pid must never reach kill().
     */
    (void)handle;
    if (pid <= 0) return -1;
    return kill(pid, SIGTERM) == 0 ? 0 : -1;
}

int platform_kill(PlatformPid pid, PlatformHandle handle) {
    /*
     * SIGKILL cannot be caught or ignored — the kernel kills the process
     * immediately. Used as a last resort after SIGTERM + timeout.
     */
    (void)handle;
    if (pid <= 0) return -1;  /* see platform_terminate */
    return kill(pid, SIGKILL) == 0 ? 0 : -1;
}

void platform_close_handle(PlatformHandle handle) {
    /*
     * On POSIX, process handles don't exist — we use the PID directly.
     * PlatformHandle is defined as int and set to PLATFORM_INVALID_HANDLE (-1).
     * No cleanup needed.
     */
    (void)handle;
}

/* ==========================================================================
 * TCP socket implementation
 *
 * On Linux, socket() returns a plain int file descriptor.
 * No lookup table needed — the fd IS the socket handle.
 * ==========================================================================
 */

int platform_tcp_connect(const char *host, int port, int timeout_ms) {
    /*
     * Same technique as Windows: non-blocking connect + select for timeout.
     * Difference: on Linux, select()'s first arg is nfds = highest fd + 1.
     */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
        return -1;
    }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { freeaddrinfo(res); return -1; }

    /* Set close-on-exec: don't pass this socket to exec'd children */
    fcntl(s, F_SETFD, FD_CLOEXEC);

    /* Switch to non-blocking so connect() returns immediately */
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);

    /* connect() returns EINPROGRESS immediately (not an error) */
    connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    /*
     * select() waits until the socket is writable (= connected) or timeout.
     * On Linux (unlike Windows), the first arg is max_fd + 1.
     */
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(s, &wset);

    int ready = select(s + 1, NULL, &wset, NULL, &tv);
    if (ready <= 0) { close(s); return -1; }

    /* Verify no error on the socket */
    int sock_err = 0;
    socklen_t optlen = sizeof(sock_err);
    getsockopt(s, SOL_SOCKET, SO_ERROR, &sock_err, &optlen);
    if (sock_err != 0) { close(s); return -1; }

    /* Restore blocking mode for send/recv */
    fcntl(s, F_SETFL, flags & ~O_NONBLOCK);

    /* Set recv timeout via socket option */
    struct timeval recv_tv;
    recv_tv.tv_sec  = timeout_ms / 1000;
    recv_tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));

    return s;  /* the fd is directly usable — no table needed */
}

int platform_tcp_send(int fd, const char *buf, int len) {
    /*
     * MSG_NOSIGNAL: don't send SIGPIPE if the remote end closed the connection.
     * Without this, writing to a closed socket kills the process.
     * We already install SIG_IGN for SIGPIPE in platform_install_signal_handlers,
     * but MSG_NOSIGNAL is the belt-and-suspenders approach.
     */
    ssize_t n = send(fd, buf, (size_t)len, MSG_NOSIGNAL);
    return n >= 0 ? (int)n : -1;
}

int platform_tcp_recv(int fd, char *buf, int len) {
    ssize_t n = recv(fd, buf, (size_t)len, 0);
    return n > 0 ? (int)n : -1;
}

void platform_socket_close(int fd) {
    if (fd >= 0) close(fd);
}

/* ==========================================================================
 * IPC via Unix domain sockets
 *
 * A Unix domain socket is a special file in the filesystem (typically under
 * /tmp) that acts as a communication endpoint. It uses the same API as TCP
 * sockets (socket/bind/listen/accept/connect/send/recv) but only works
 * between processes on the same machine — no network overhead.
 *
 * Socket file path: whatever is in ipc_socket in guardian.ini
 * (e.g. /tmp/guardian.sock)
 *
 * Key difference from Named Pipes: the accept() call produces a NEW socket fd
 * for the connected client. The server socket keeps listening. This is the
 * standard model for connection-oriented servers.
 * ==========================================================================
 */

/* Track the server socket fd and its path so we can unlink() on close */
static int  g_ipc_server_fd         = -1;
static char g_ipc_socket_path[512]  = "";

/*
 * fits_sun_path — sockaddr_un.sun_path is only 108 bytes on Linux
 *
 * strncpy into sun_path silently truncates a longer path, so guardian would
 * bind (or connect) to a DIFFERENT file than the one in the config — and
 * unlink() the full-length path, which isn't the socket at all. Reject
 * instead of guessing.
 */
static int fits_sun_path(const char *path) {
    struct sockaddr_un addr;
    return strlen(path) < sizeof(addr.sun_path);
}

int platform_ipc_listen(const char *path) {
    if (!fits_sun_path(path)) {
        fprintf(stderr, "[guardian] ipc_socket path too long (max %zu chars): %s\n",
                sizeof(((struct sockaddr_un *)0)->sun_path) - 1, path);
        return -1;
    }

    /*
     * AF_UNIX  = "this is a local socket, not TCP"
     * SOCK_STREAM = "reliable, in-order byte stream" (same as TCP semantics)
     */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    /* Close-on-exec: don't pass the server socket to managed processes */
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    /*
     * struct sockaddr_un is the address type for Unix domain sockets.
     * sun_path is the filesystem path — max 108 bytes on Linux.
     */
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    /*
     * Remove any leftover socket file from a previous run.
     * If guardian crashed without cleaning up, the old socket file would
     * cause bind() to fail with EADDRINUSE.
     */
    unlink(path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    /*
     * listen(fd, backlog) — start accepting connections.
     * backlog=5: queue up to 5 pending connections.
     * For our single-command IPC use, 5 is more than enough.
     */
    if (listen(fd, 5) != 0) {
        close(fd);
        unlink(path);
        return -1;
    }

    /*
     * Make accept() non-blocking so the event loop can call it every
     * 100ms without stalling. With O_NONBLOCK, accept() returns -1 with
     * errno=EAGAIN if no client is waiting — that's our "no client" signal.
     */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    /* Remember for cleanup in platform_ipc_close */
    g_ipc_server_fd = fd;
    strncpy(g_ipc_socket_path, path, sizeof(g_ipc_socket_path) - 1);

    return fd;
}

int platform_ipc_accept(int server_fd) {
    /*
     * accept() returns a NEW fd representing the connected client.
     * With O_NONBLOCK on the server socket, it returns immediately.
     * If no client is waiting, errno is EAGAIN or EWOULDBLOCK — both
     * mean "try again later" and both are our signal to return -1.
     */
    int client_fd = accept(server_fd, NULL, NULL);

    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return -1;  /* no client this tick — normal */
        }
        return -1;  /* real error */
    }

    /* Set receive timeout on the client socket */
    struct timeval tv;
    tv.tv_sec  = 5;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return client_fd;
}

int platform_ipc_connect(const char *path) {
    if (!fits_sun_path(path)) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    /* Set receive timeout so the client doesn't block forever waiting
     * for a daemon response */
    struct timeval tv;
    tv.tv_sec  = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return fd;
}

int platform_ipc_send(int fd, const char *buf, int len) {
    ssize_t n = send(fd, buf, (size_t)len, MSG_NOSIGNAL);
    return n >= 0 ? (int)n : -1;
}

int platform_ipc_recv(int fd, char *buf, int len) {
    ssize_t n = recv(fd, buf, (size_t)len, 0);
    return n > 0 ? (int)n : -1;
}

void platform_ipc_close(int fd) {
    if (fd < 0) return;

    if (fd == g_ipc_server_fd) {
        /*
         * This is the server socket. Remove the socket file from the
         * filesystem so the next guardian run can bind to the same path.
         * Without unlink(), the next bind() would fail with EADDRINUSE.
         */
        if (g_ipc_socket_path[0]) {
            unlink(g_ipc_socket_path);
            g_ipc_socket_path[0] = '\0';
        }
        g_ipc_server_fd = -1;
    }

    close(fd);
}
