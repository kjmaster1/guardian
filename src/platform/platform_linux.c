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
#include <string.h>      /* memset, strncpy, strtok, strchr */
#include <stdlib.h>      /* NULL, _exit, atoi, setenv */
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
static volatile int *g_shutdown_flag = NULL;

static void signal_handler(int signum) {
    /*
     * Signal handlers have severe restrictions — only "async-signal-safe"
     * functions are allowed. That rules out printf, malloc, mutex locks, etc.
     * (They could be mid-execution when the signal arrives, causing deadlock.)
     *
     * Writing to a volatile int is safe. That's all we do here.
     */
    (void)signum;
    if (g_shutdown_flag) *g_shutdown_flag = 0;
}

void platform_install_signal_handlers(volatile int *shutdown_flag) {
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

/*
 * parse_args_into — split an args string into an argv array
 *
 * Splits 'args_str' on spaces and fills argv[] starting at argv[start_idx].
 * argv[0..start_idx-1] must already be set by the caller (typically argv[0]
 * = command path). Returns the total argc (not counting the trailing NULL).
 *
 * Limitation: doesn't handle quoted arguments with internal spaces.
 * For the use cases guardian targets (simple daemons), this is sufficient.
 */
static int parse_args_into(char *args_copy, const char **argv,
                            int start_idx, int max_args) {
    int argc = start_idx;
    if (args_copy[0] == '\0') {
        argv[argc] = NULL;
        return argc;
    }

    char *token = strtok(args_copy, " ");
    while (token && argc < max_args - 1) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }
    argv[argc] = NULL;
    return argc;
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
     * Build argv before fork. strtok modifies the string in-place,
     * so we copy args into a local buffer first.
     *
     * argv layout:  [ command, arg1, arg2, ..., NULL ]
     * argv[0] is the program name (convention: use the command path).
     */
    char args_copy[256];
    strncpy(args_copy, args ? args : "", sizeof(args_copy) - 1);
    args_copy[sizeof(args_copy) - 1] = '\0';

    const char *argv[64];
    argv[0] = command;
    parse_args_into(args_copy, argv, 1, 64);

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
        return result;
    }

    if (pid == 0) {
        /* ================================================================
         * CHILD PROCESS — this code runs in the newly forked process
         *
         * Goal: set up the environment, then replace ourselves with the
         * target program via execvp(). If exec succeeds, this code
         * never continues past the execvp() call.
         *
         * IMPORTANT: after fork(), we must be careful what we call.
         * Async-signal-safe rules don't apply here (we're not in a signal
         * handler), but we should avoid functions that might interact
         * badly with the parent's state: no printf (may flush buffers),
         * no malloc (heap may be mid-operation). We use _exit() not exit()
         * so we don't flush stdio or run atexit() handlers.
         * ================================================================ */

        /* 1. Change working directory if configured */
        if (working_dir && working_dir[0] != '\0') {
            if (chdir(working_dir) != 0) {
                /* Write directly to stderr — can't use fprintf after fork */
                const char *msg = "guardian: chdir failed\n";
                write(STDERR_FILENO, msg, strlen(msg));
                _exit(1);
            }
        }

        /* 2. Add environment variables */
        for (int i = 0; i < env_count; i++) {
            /*
             * setenv() parses "KEY=VALUE" — we need to split it.
             * We find the '=' and temporarily null it to extract the key,
             * then restore it. A cleaner alternative is putenv(), but that
             * doesn't copy the string. setenv() copies, which is safer.
             */
            char env_copy[256];
            strncpy(env_copy, env_vars[i], sizeof(env_copy) - 1);
            char *eq = strchr(env_copy, '=');
            if (eq) {
                *eq = '\0';
                setenv(env_copy, eq + 1, 1 /* overwrite */);
            }
        }

        /* 3. Redirect stdout to log file (if configured)
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

        /* 4. Redirect stderr to log file (if configured) */
        if (stderr_log && stderr_log[0] != '\0') {
            int fd = open(stderr_log,
                          O_WRONLY | O_CREAT | O_APPEND,
                          0644);
            if (fd >= 0) {
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
        }

        /* 5. exec — replace this process image with the target program
         *
         * execvp() searches PATH for the command (like a shell does).
         * argv must end with a NULL sentinel.
         *
         * If execvp() RETURNS, it failed. Normal exec doesn't return.
         */
        execvp(command, (char *const *)argv);

        /* If we reach here, exec failed */
        const char *msg = "guardian: execvp failed\n";
        write(STDERR_FILENO, msg, strlen(msg));
        _exit(1);
    }

    /* ====================================================================
     * PARENT PROCESS — continues here after fork()
     *
     * The child is now running independently. We store its PID and return.
     * ==================================================================== */
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
     */
    (void)handle;
    return kill(pid, SIGTERM) == 0 ? 0 : -1;
}

int platform_kill(PlatformPid pid, PlatformHandle handle) {
    /*
     * SIGKILL cannot be caught or ignored — the kernel kills the process
     * immediately. Used as a last resort after SIGTERM + timeout.
     */
    (void)handle;
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

int platform_ipc_listen(const char *path) {
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
