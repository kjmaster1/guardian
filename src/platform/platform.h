/*
 * platform.h — Cross-platform OS abstraction interface for guardian
 *
 * This is the ONLY file in the entire codebase that contains platform-specific
 * type definitions. Every other source file includes this header and uses the
 * types and functions defined here — never using OS APIs directly.
 *
 * The implementations live in:
 *   platform_linux.c   — Linux (fork/exec, POSIX signals, POSIX sockets)
 *   platform_macos.c   — macOS (same as Linux with minor differences)
 *   platform_windows.c — Windows (CreateProcess, Job Objects, Named Pipes)
 *
 * The Makefile selects the correct implementation file at compile time.
 * This means the rest of the codebase is 100% platform-neutral.
 *
 * This pattern is called a "platform abstraction layer" or PAL. It's used
 * in production codebases like Chromium, LLVM, and SQLite.
 */

#pragma once

#include <stdint.h>   /* int64_t */

/* ==========================================================================
 * Platform-specific type aliases
 *
 * On Linux/macOS: process IDs are pid_t (typically int).
 * On Windows:     process IDs are DWORD (unsigned 32-bit int).
 *                 Process handles are HANDLE (a void*).
 *
 * We define PlatformPid and PlatformHandle so the rest of the code
 * can hold "a process identifier" without knowing which OS they're on.
 * ==========================================================================
 */

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    typedef DWORD   PlatformPid;
    typedef HANDLE  PlatformHandle;
    #define PLATFORM_INVALID_PID     ((PlatformPid)0)
    #define PLATFORM_INVALID_HANDLE  INVALID_HANDLE_VALUE
#else
    #include <unistd.h>   /* pid_t */
    typedef pid_t   PlatformPid;
    typedef int     PlatformHandle;   /* unused on POSIX, always -1 */
    #define PLATFORM_INVALID_PID     ((PlatformPid)(-1))
    #define PLATFORM_INVALID_HANDLE  (-1)
#endif

/* ==========================================================================
 * Return types for platform operations
 * ==========================================================================
 */

/*
 * SpawnResult — returned by platform_spawn()
 *
 * On success: success=1, pid holds the child PID, handle holds the Windows
 * HANDLE (on POSIX, handle is always PLATFORM_INVALID_HANDLE).
 * On failure: success=0, error_msg contains a human-readable description.
 */
typedef struct {
    PlatformPid    pid;
    PlatformHandle handle;
    int            success;
    char           error_msg[128];
} SpawnResult;

/*
 * ProcessStatus — returned by platform_check_process()
 *
 * exited=0 means the process is still running.
 * exited=1 means the process has terminated; check exit_code and by_signal.
 */
typedef struct {
    int exited;       /* 1 if the process has terminated, 0 if still running */
    int exit_code;    /* meaningful only when exited=1 and by_signal=0       */
    int by_signal;    /* 1 if killed by a signal (POSIX only)                */
    int signal_num;   /* the signal number, when by_signal=1                 */
} ProcessStatus;

/* ==========================================================================
 * Platform API — implemented in platform_{linux,macos,windows}.c
 * ==========================================================================
 */

/*
 * platform_spawn — launch a child process
 *
 * command:     full path to the executable (e.g., "/usr/bin/node")
 * args:        argument string (e.g., "server.js --port 3000")
 * working_dir: directory to chdir into before exec (NULL = inherit)
 * env_vars:    array of "KEY=VALUE" strings to add to environment (NULL = inherit)
 * env_count:   number of entries in env_vars
 * stdout_log:  path to redirect stdout to (NULL = inherit guardian's stdout)
 * stderr_log:  path to redirect stderr to (NULL = inherit guardian's stderr)
 */
SpawnResult platform_spawn(const char  *command,
                            const char  *args,
                            const char  *working_dir,
                            const char **env_vars,
                            int          env_count,
                            const char  *stdout_log,
                            const char  *stderr_log);

/*
 * platform_check_process — non-blocking check if a process is still running
 *
 * Does NOT block. Returns immediately with the current status.
 * On POSIX: uses waitpid(pid, WNOHANG).
 * On Windows: uses WaitForSingleObject(handle, 0).
 */
ProcessStatus platform_check_process(PlatformPid pid, PlatformHandle handle);

/* Send SIGTERM (POSIX) or TerminateProcess (Windows) — graceful stop */
int platform_terminate(PlatformPid pid, PlatformHandle handle);

/* Send SIGKILL (POSIX) or TerminateProcess with exit code 0 (Windows) */
int platform_kill(PlatformPid pid, PlatformHandle handle);

/* Release OS-level handle resources (no-op on POSIX; CloseHandle on Windows) */
void platform_close_handle(PlatformHandle handle);

/*
 * platform_install_signal_handlers — set up SIGTERM/SIGINT handling
 *
 * When SIGTERM or SIGINT arrives, sets *shutdown_flag = 0.
 * The main event loop checks this flag each tick to know when to stop.
 *
 * 'volatile int*' — the volatile qualifier is required here. See service.h
 * for a full explanation of why volatile is necessary for signal-touched flags.
 */
void platform_install_signal_handlers(volatile int *shutdown_flag);

/*
 * platform_now_ms — current time in milliseconds
 *
 * Uses CLOCK_MONOTONIC on POSIX (immune to wall-clock adjustments).
 * Uses GetTickCount64() on Windows.
 *
 * We use this for computing backoff delays and health check intervals.
 * Returns int64_t to avoid the Year 2038 problem.
 */
int64_t platform_now_ms(void);

/* Sleep for the specified number of milliseconds */
void platform_sleep_ms(int ms);

/*
 * platform_tcp_connect — open a TCP connection
 *
 * host:       hostname or IP address string (e.g., "127.0.0.1")
 * port:       TCP port number
 * timeout_ms: how long to wait before giving up
 *
 * Returns a socket file descriptor (>= 0) on success, or -1 on failure.
 * The caller must close the socket with platform_socket_close() when done.
 */
int platform_tcp_connect(const char *host, int port, int timeout_ms);

/* Send bytes over a TCP socket (for HTTP health checks) */
int platform_tcp_send(int fd, const char *buf, int len);

/* Receive bytes from a TCP socket (blocking, uses per-socket timeout set at connect) */
int platform_tcp_recv(int fd, char *buf, int len);

/* Close a socket returned by platform_tcp_connect() */
void platform_socket_close(int fd);

/* IPC server: create and bind a listening Unix socket / Named Pipe */
int platform_ipc_listen(const char *path);

/* IPC server: accept one pending connection (non-blocking; returns -1 if none) */
int platform_ipc_accept(int server_fd);

/* IPC client: connect to a running server at the given path */
int platform_ipc_connect(const char *path);

/* IPC: send bytes over a connected IPC fd */
int platform_ipc_send(int fd, const char *buf, int len);

/* IPC: receive bytes from a connected IPC fd (non-blocking) */
int platform_ipc_recv(int fd, char *buf, int len);

/* IPC: close a connection fd */
void platform_ipc_close(int fd);
