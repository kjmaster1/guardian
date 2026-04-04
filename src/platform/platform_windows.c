/*
 * platform_windows.c — Windows implementation of the platform abstraction layer
 *
 * Uses the Win32 API for process management. Key functions used:
 *   CreateProcess()        — launch a child process (replaces fork+exec)
 *   WaitForSingleObject()  — check if a process has exited (replaces waitpid)
 *   GetExitCodeProcess()   — get the exit code after a process ends
 *   TerminateProcess()     — forcefully end a process (replaces kill)
 *   CreateJobObject()      — create a container that auto-kills children
 *   SetConsoleCtrlHandler()— handle Ctrl+C / system shutdown signals
 *   GetTickCount64()       — monotonic millisecond timer
 *
 * This file is selected by the Makefile when building on Windows ($(OS)==Windows_NT).
 */

/* WIN32_LEAN_AND_MEAN tells windows.h to skip rarely-used headers.
 * Without it, windows.h pulls in winsock.h which conflicts with winsock2.h.
 * Always define this before including windows.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>      /* All Win32 API — CreateProcess, Job Objects, etc. */
#include <winsock2.h>     /* Windows Sockets 2 — TCP/IP networking */
#include <ws2tcpip.h>     /* getaddrinfo, inet_pton and other modern socket helpers */

#include "platform.h"

#include <stdio.h>     /* fprintf, stderr, snprintf */
#include <string.h>    /* memset, strncpy, strlen   */
#include <stdlib.h>    /* malloc, free               */

/* ==========================================================================
 * Argument string helpers
 *
 * On Linux, execvp() takes an array of strings: { "node", "server.js", NULL }
 * On Windows, CreateProcess() takes a single string: "node server.js"
 *
 * We need to BUILD that single string by joining command + args.
 * We also need to handle quoting: if the command path has spaces
 * (e.g. "C:\Program Files\node\node.exe"), it must be quoted.
 * ==========================================================================
 */

/*
 * build_command_line — join command and args into one quoted string
 *
 * CreateProcess expects: "\"C:\\path\\to\\exe\" arg1 arg2"
 * (The executable path is quoted in case it contains spaces.)
 *
 * We write into a caller-provided buffer. Returns 1 on success, 0 if
 * the result would overflow the buffer.
 */
static int build_command_line(const char *command,
                               const char *args,
                               char       *buf,
                               int         buf_size) {
    int written;

    if (args && args[0] != '\0') {
        /* "command" args */
        written = snprintf(buf, buf_size, "\"%s\" %s", command, args);
    } else {
        /* "command" */
        written = snprintf(buf, buf_size, "\"%s\"", command);
    }

    /* snprintf returns the number of characters it WOULD have written.
     * If that's >= buf_size, it means the output was truncated. */
    return (written > 0 && written < buf_size) ? 1 : 0;
}

/* ==========================================================================
 * Job Object — process group that auto-kills children on parent exit
 *
 * We create ONE global Job Object for the lifetime of guardian.
 * Every child process we spawn gets assigned to this Job Object.
 *
 * The key setting: JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
 * When guardian's process handle to the Job Object is closed (i.e., when
 * guardian exits for ANY reason — normal, crash, or kill), Windows
 * automatically terminates every process in the Job Object.
 *
 * This prevents "zombie" child processes from lingering after guardian dies.
 * It's one of the genuinely powerful features Windows has over basic Linux
 * process management (Linux requires process groups or cgroups for this).
 * ==========================================================================
 */
static HANDLE g_job_object = NULL;

/*
 * ensure_job_object — create the Job Object if it doesn't exist yet
 *
 * Called lazily on first spawn. Thread-safe? Not yet — Phase 5 adds locking.
 * For Phase 2, spawning happens only from the main thread.
 */
static void ensure_job_object(void) {
    if (g_job_object != NULL) return;

    /* CreateJobObject(security_attrs, name)
     * NULL security = default permissions
     * NULL name     = anonymous Job Object (we don't need a named one) */
    g_job_object = CreateJobObject(NULL, NULL);
    if (g_job_object == NULL) {
        fprintf(stderr, "[guardian] WARNING: CreateJobObject failed (error %lu). "
                        "Child processes will not be auto-killed on guardian exit.\n",
                GetLastError());
        return;
    }

    /* Configure the Job Object's limits.
     *
     * JOBOBJECT_EXTENDED_LIMIT_INFORMATION is the struct we fill in.
     * BasicLimitInformation.LimitFlags is where we set our flag.
     *
     * JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE:
     *   "When the last handle to the job object is closed, terminate all
     *    processes in the job." This is exactly what we want.
     */
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info;
    memset(&job_info, 0, sizeof(job_info));
    job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    BOOL ok = SetInformationJobObject(
        g_job_object,
        JobObjectExtendedLimitInformation,
        &job_info,
        sizeof(job_info)
    );

    if (!ok) {
        fprintf(stderr, "[guardian] WARNING: SetInformationJobObject failed (error %lu). "
                        "Job Object cleanup may not work.\n", GetLastError());
    }
}

/* ==========================================================================
 * platform_spawn — launch a child process
 *
 * This is the Windows equivalent of fork() + exec().
 * CreateProcess() does both in one call.
 * ==========================================================================
 */
SpawnResult platform_spawn(const char  *command,
                            const char  *args,
                            const char  *working_dir,
                            const char **env_vars,
                            int          env_count,
                            const char  *stdout_log,
                            const char  *stderr_log) {
    SpawnResult result;
    memset(&result, 0, sizeof(result));
    result.pid    = PLATFORM_INVALID_PID;
    result.handle = PLATFORM_INVALID_HANDLE;

    /* Suppress unused warning — env vars support comes in a later step */
    (void)env_vars; (void)env_count;

    ensure_job_object();

    /* ----------------------------------------------------------------
     * Step 1: Build the command-line string.
     *
     * CreateProcess needs: "\"C:\\path\\to\\node.exe\" server.js --port 3000"
     * ---------------------------------------------------------------- */
    char cmd_line[1024];
    if (!build_command_line(command, args, cmd_line, sizeof(cmd_line))) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "command + args too long (max 1023 chars)");
        return result;
    }

    /* ----------------------------------------------------------------
     * Step 2: Set up STARTUPINFOA.
     *
     * STARTUPINFOA tells Windows how to configure the new process's
     * console window and standard I/O handles.
     *
     * The 'A' suffix means the ASCII (narrow string) version.
     * Windows has two versions of most string functions: A (char*)
     * and W (wchar_t*, wide Unicode). We use A for simplicity.
     * ---------------------------------------------------------------- */
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);  /* cb = "count of bytes" — Windows requires you
                            to set the struct size so it can version-check */

    /* Handles for stdout/stderr redirection (NULL = no redirection) */
    HANDLE h_stdout = INVALID_HANDLE_VALUE;
    HANDLE h_stderr = INVALID_HANDLE_VALUE;

    if (stdout_log && stdout_log[0] != '\0') {
        /*
         * CreateFile() opens (or creates) a file for I/O.
         * Parameters:
         *   lpFileName:            the file path
         *   dwDesiredAccess:       GENERIC_WRITE — we only write to it
         *   dwShareMode:           FILE_SHARE_READ — other processes can read it
         *                          (so you can `tail -f` the log while running)
         *   lpSecurityAttributes:  NULL — we want the child to INHERIT this handle
         *                          (more on this below)
         *   dwCreationDisposition: OPEN_ALWAYS — create if not exists, open if exists
         *   dwFlagsAndAttributes:  FILE_ATTRIBUTE_NORMAL — ordinary file
         *   hTemplateFile:         NULL — not using a template
         */
        SECURITY_ATTRIBUTES sa;
        memset(&sa, 0, sizeof(sa));
        sa.nLength              = sizeof(sa);
        sa.bInheritHandle       = TRUE;  /* ← This is crucial!
                                            Only handles with bInheritHandle=TRUE
                                            get copied into the child process.
                                            This is how we "pass" the file to the child. */
        sa.lpSecurityDescriptor = NULL;

        h_stdout = CreateFileA(
            stdout_log,
            GENERIC_WRITE,
            FILE_SHARE_READ,
            &sa,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );

        if (h_stdout == INVALID_HANDLE_VALUE) {
            snprintf(result.error_msg, sizeof(result.error_msg),
                     "failed to open stdout log '%s' (error %lu)",
                     stdout_log, GetLastError());
            return result;
        }

        /* Move the write position to the end of the file (append mode) */
        SetFilePointer(h_stdout, 0, NULL, FILE_END);
    }

    if (stderr_log && stderr_log[0] != '\0') {
        SECURITY_ATTRIBUTES sa;
        memset(&sa, 0, sizeof(sa));
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;

        h_stderr = CreateFileA(
            stderr_log,
            GENERIC_WRITE,
            FILE_SHARE_READ,
            &sa,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );

        if (h_stderr == INVALID_HANDLE_VALUE) {
            if (h_stdout != INVALID_HANDLE_VALUE) CloseHandle(h_stdout);
            snprintf(result.error_msg, sizeof(result.error_msg),
                     "failed to open stderr log '%s' (error %lu)",
                     stderr_log, GetLastError());
            return result;
        }

        SetFilePointer(h_stderr, 0, NULL, FILE_END);
    }

    /* If we opened log files, configure STARTUPINFO to use them */
    if (h_stdout != INVALID_HANDLE_VALUE || h_stderr != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        /* If no stdout log, inherit guardian's own stdout */
        si.hStdOutput = (h_stdout != INVALID_HANDLE_VALUE)
                        ? h_stdout
                        : GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError  = (h_stderr != INVALID_HANDLE_VALUE)
                        ? h_stderr
                        : GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    }

    /* ----------------------------------------------------------------
     * Step 3: Call CreateProcess()
     *
     * Parameters:
     *   lpApplicationName:    NULL — we embed the exe path in lpCommandLine
     *   lpCommandLine:        our built command string (MUST be writable — see note)
     *   lpProcessAttributes:  NULL — default process security
     *   lpThreadAttributes:   NULL — default thread security
     *   bInheritHandles:      TRUE — child inherits our open handles (the log files)
     *   dwCreationFlags:      CREATE_NO_WINDOW — don't open a console window
     *                         for background services
     *   lpEnvironment:        NULL — inherit guardian's environment for now
     *   lpCurrentDirectory:   working_dir, or NULL to inherit guardian's cwd
     *   lpStartupInfo:        our STARTUPINFOA struct
     *   lpProcessInformation: Windows fills this with the new process's info
     *
     * NOTE on lpCommandLine: Microsoft documents that CreateProcess may modify
     * this string internally. Passing a string literal (read-only memory) can
     * cause an access violation. We pass cmd_line which is a stack buffer —
     * writable memory. Always do this.
     * ---------------------------------------------------------------- */
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    BOOL ok = CreateProcessA(
        NULL,
        cmd_line,
        NULL,
        NULL,
        TRUE,                /* bInheritHandles — must be TRUE for log redirection */
        CREATE_NO_WINDOW,
        NULL,
        (working_dir && working_dir[0]) ? working_dir : NULL,
        &si,
        &pi
    );

    /* Close the log file handles in the PARENT — we don't need them here.
     * The child has its own copies (inherited). If we don't close ours,
     * the file handle count leaks every time we spawn a process. */
    if (h_stdout != INVALID_HANDLE_VALUE) CloseHandle(h_stdout);
    if (h_stderr != INVALID_HANDLE_VALUE) CloseHandle(h_stderr);

    if (!ok) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "CreateProcess failed for '%s' (error %lu)",
                 command, GetLastError());
        return result;
    }

    /* ----------------------------------------------------------------
     * Step 4: Close the thread handle and assign to Job Object.
     *
     * CreateProcess gives us TWO handles: one for the process, one for
     * its main thread. We don't need the thread handle — close it now.
     * Failing to close handles is a resource leak (like not free()ing malloc).
     * ---------------------------------------------------------------- */
    CloseHandle(pi.hThread);

    /* Add the new process to our Job Object */
    if (g_job_object != NULL) {
        AssignProcessToJobObject(g_job_object, pi.hProcess);
        /* We don't fail if this errors — Job Objects may not be available
         * in all Windows configurations (e.g., inside some sandboxes) */
    }

    result.pid     = pi.dwProcessId;
    result.handle  = pi.hProcess;  /* We KEEP this handle — needed for WaitForSingleObject */
    result.success = 1;
    return result;
}

/* ==========================================================================
 * platform_check_process — non-blocking check if a process is still running
 *
 * On Linux this is: waitpid(pid, &status, WNOHANG)
 * On Windows this is: WaitForSingleObject(handle, 0) — timeout of 0ms means
 * "return immediately, don't wait"
 * ==========================================================================
 */
ProcessStatus platform_check_process(PlatformPid pid, PlatformHandle handle) {
    (void)pid;  /* on Windows we use the handle, not the PID */

    ProcessStatus status;
    memset(&status, 0, sizeof(status));

    if (handle == PLATFORM_INVALID_HANDLE) {
        return status;  /* not running */
    }

    /*
     * WaitForSingleObject(handle, milliseconds) waits up to 'milliseconds'
     * for the object to become "signaled" (for a process, that means exited).
     *
     * WAIT_OBJECT_0 = the process has exited (signaled)
     * WAIT_TIMEOUT  = still running (timeout elapsed, process not done)
     * WAIT_FAILED   = error
     *
     * With timeout=0, it returns immediately — this is the non-blocking check.
     */
    DWORD wait_result = WaitForSingleObject(handle, 0);

    if (wait_result == WAIT_OBJECT_0) {
        /* Process has exited. Get its exit code. */
        DWORD exit_code = 0;
        GetExitCodeProcess(handle, &exit_code);

        status.exited    = 1;
        status.exit_code = (int)exit_code;
        /* Windows doesn't have signals — by_signal is always 0 */
        status.by_signal = 0;
        status.signal_num = 0;
    }
    /* If WAIT_TIMEOUT: exited stays 0, meaning "still running" */

    return status;
}

/* ==========================================================================
 * platform_terminate — ask a process to stop gracefully
 *
 * On Linux: kill(pid, SIGTERM) — sends a signal the process can handle
 * On Windows: TerminateProcess() — there are no signals; this is immediate
 *
 * Note: Windows has no equivalent of SIGTERM (a "please stop" request that
 * the process can catch and handle gracefully). TerminateProcess() is always
 * forceful. For services that need graceful shutdown, you'd use a named pipe
 * or socket to send a shutdown message — an enhancement for later phases.
 * ==========================================================================
 */
int platform_terminate(PlatformPid pid, PlatformHandle handle) {
    (void)pid;
    if (handle == PLATFORM_INVALID_HANDLE) return -1;
    /* Exit code 1 = terminated by guardian (distinguishable from natural exit 0) */
    return TerminateProcess(handle, 1) ? 0 : -1;
}

int platform_kill(PlatformPid pid, PlatformHandle handle) {
    (void)pid;
    if (handle == PLATFORM_INVALID_HANDLE) return -1;
    return TerminateProcess(handle, 0) ? 0 : -1;
}

void platform_close_handle(PlatformHandle handle) {
    if (handle != PLATFORM_INVALID_HANDLE) {
        CloseHandle(handle);
    }
}

/* ==========================================================================
 * Signal handling — Ctrl+C and system shutdown
 *
 * Windows doesn't have Unix signals. Instead, it calls a "console control
 * handler" function when certain events occur (Ctrl+C, Ctrl+Break, system
 * shutdown, etc.).
 *
 * We use a static pointer to store the shutdown flag so the handler can
 * reach it. This is safe because there is only ever one supervisor instance.
 * ==========================================================================
 */
static volatile int *g_shutdown_flag = NULL;

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    /*
     * This function is called by Windows on a SEPARATE THREAD when a
     * control event arrives. That's why the shutdown flag must be volatile —
     * the main thread reads it, this thread writes it.
     *
     * We handle all event types the same way: set the flag and return TRUE.
     * Returning TRUE tells Windows "we handled it — don't terminate the process."
     * Returning FALSE would let Windows terminate the process immediately.
     */
    switch (ctrl_type) {
        case CTRL_C_EVENT:       /* Ctrl+C pressed */
        case CTRL_BREAK_EVENT:   /* Ctrl+Break pressed */
        case CTRL_CLOSE_EVENT:   /* console window closed */
        case CTRL_SHUTDOWN_EVENT:/* system shutting down */
        case CTRL_LOGOFF_EVENT:  /* user logging off */
            if (g_shutdown_flag) {
                *g_shutdown_flag = 0;  /* tell the main loop to stop */
            }
            return TRUE;
        default:
            return FALSE;
    }
}

void platform_install_signal_handlers(volatile int *shutdown_flag) {
    g_shutdown_flag = shutdown_flag;
    /*
     * SetConsoleCtrlHandler(handler, add)
     * add=TRUE  → install the handler
     * add=FALSE → remove it
     */
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
}

/* ==========================================================================
 * Time and sleep
 * ==========================================================================
 */

int64_t platform_now_ms(void) {
    /*
     * GetTickCount64() returns milliseconds since system boot.
     * It's monotonic — never goes backwards.
     * The 64-bit version doesn't overflow for ~584 million years.
     * (The 32-bit GetTickCount() overflows every ~49 days — a classic bug source.)
     */
    return (int64_t)GetTickCount64();
}

void platform_sleep_ms(int ms) {
    Sleep((DWORD)ms);  /* Windows Sleep() takes milliseconds directly */
}

/* ==========================================================================
 * IPC via Windows Named Pipes
 *
 * A Named Pipe is a communication channel with a filesystem-like name.
 * Any process that knows the name can connect to it.
 *
 * Named Pipe path format: \\.\pipe\name
 * In a C string:           "\\\\.\\pipe\\name"
 * (Each \\ in C source becomes one \ in the actual string)
 *
 * HANDLE TABLE
 * Our platform API uses 'int' fds for portability, but Windows uses HANDLE.
 * We maintain a small static table mapping int indices to real HANDLEs.
 * This is exactly how the OS kernel implements file descriptors internally —
 * your fd 3 is just an index into the kernel's handle table for your process.
 * ==========================================================================
 */

#define MAX_IPC_FDS 16

static HANDLE g_ipc_table[MAX_IPC_FDS];
static int    g_ipc_table_init  = 0;
static int    g_ipc_is_server[MAX_IPC_FDS] = {0}; /* 1 = server pipe, 0 = client */

/* Initialize all table slots to INVALID_HANDLE_VALUE */
static void ipc_table_init(void) {
    if (g_ipc_table_init) return;
    for (int i = 0; i < MAX_IPC_FDS; i++) {
        g_ipc_table[i] = INVALID_HANDLE_VALUE;
    }
    g_ipc_table_init = 1;
}

/* Allocate a slot, store the handle, return the index (our "fd") */
static int ipc_table_alloc(HANDLE h) {
    ipc_table_init();
    for (int i = 0; i < MAX_IPC_FDS; i++) {
        if (g_ipc_table[i] == INVALID_HANDLE_VALUE) {
            g_ipc_table[i] = h;
            return i;
        }
    }
    return -1;  /* table full */
}

/* Look up a handle by its index */
static HANDLE ipc_table_get(int fd) {
    if (fd < 0 || fd >= MAX_IPC_FDS) return INVALID_HANDLE_VALUE;
    return g_ipc_table[fd];
}

/* Free a slot */
static void ipc_table_free(int fd) {
    if (fd >= 0 && fd < MAX_IPC_FDS) {
        g_ipc_table[fd] = INVALID_HANDLE_VALUE;
    }
}

/* ==========================================================================
 * platform_ipc_listen — create the Named Pipe server
 *
 * We create the pipe in OVERLAPPED mode so that ConnectNamedPipe can
 * be called non-blocking (by pairing it with a manual-reset event object).
 * Actually the simpler approach: use PeekNamedPipe to check before reading.
 * We create with FILE_FLAG_OVERLAPPED so accept is non-blocking.
 * ==========================================================================
 */
/*
 * to_pipe_path — convert any path to a Windows Named Pipe path
 *
 * On Linux, IPC sockets live at paths like /tmp/guardian.sock.
 * On Windows, Named Pipes must use \\.\pipe\name format.
 *
 * This function normalizes any path to a valid pipe path so configs
 * are portable across platforms:
 *   /tmp/guardian.sock       → \\.\pipe\guardian.sock
 *   \\.\pipe\guardian        → \\.\pipe\guardian  (already correct)
 *   guardian                 → \\.\pipe\guardian
 */
static void to_pipe_path(const char *path, char *out, int out_size) {
    /* Already a Named Pipe path */
    if (strncmp(path, "\\\\.\\pipe\\", 9) == 0) {
        strncpy(out, path, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    /* Extract the filename component (after the last slash) */
    const char *name = path;
    const char *p = path;
    while (*p) {
        if (*p == '/' || *p == '\\') name = p + 1;
        p++;
    }
    /* name now points to the last path component */
    snprintf(out, out_size, "\\\\.\\pipe\\%s", name);
}

int platform_ipc_listen(const char *path) {
    char pipe_path[256];
    to_pipe_path(path, pipe_path, sizeof(pipe_path));

    /*
     * CreateNamedPipe parameters:
     *   lpName:             the pipe path e.g. \\.\pipe\guardian
     *   dwOpenMode:         PIPE_ACCESS_DUPLEX = read AND write
     *                       FILE_FLAG_OVERLAPPED = async (non-blocking) I/O
     *   dwPipeMode:         PIPE_TYPE_BYTE = raw bytes (not message mode)
     *                       PIPE_READMODE_BYTE
     *                       PIPE_NOWAIT = non-blocking ConnectNamedPipe
     *   nMaxInstances:      1 = only one client at a time
     *   nOutBufferSize:     output buffer size hint (16KB)
     *   nInBufferSize:      input buffer size hint
     *   nDefaultTimeOut:    0 = use system default timeout
     *   lpSecurityAttributes: NULL = default (only same user can connect)
     */
    HANDLE h = CreateNamedPipeA(
        pipe_path,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
        1,
        16384,
        16384,
        0,
        NULL
    );

    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[guardian] CreateNamedPipe('%s') failed: error %lu\n",
                pipe_path, GetLastError());
        return -1;
    }

    int fd = ipc_table_alloc(h);
    if (fd >= 0) g_ipc_is_server[fd] = 1;  /* mark as server pipe */
    return fd;
}

/*
 * platform_ipc_accept — non-blocking check for a new client connection
 *
 * ConnectNamedPipe() normally BLOCKS until a client connects.
 * With PIPE_NOWAIT, it returns ERROR_PIPE_LISTENING if no client yet
 * (and returns FALSE — we must check GetLastError to distinguish).
 *
 * When a client connects: ConnectNamedPipe returns TRUE (or
 * ERROR_PIPE_CONNECTED if the client already connected), and we
 * return the same fd so the caller can read/write from it.
 *
 * The Named Pipe server fd IS also the connected client fd — unlike
 * TCP sockets where accept() creates a new fd. With Named Pipes, the
 * same handle is used for both listening and communicating with the client.
 * After the client disconnects, we call DisconnectNamedPipe() to reset
 * it back to listening state.
 */
int platform_ipc_accept(int server_fd) {
    HANDLE h = ipc_table_get(server_fd);
    if (h == INVALID_HANDLE_VALUE) return -1;

    BOOL connected = ConnectNamedPipe(h, NULL);

    if (connected) {
        /* Client just connected */
        return server_fd;
    }

    DWORD err = GetLastError();

    if (err == ERROR_PIPE_CONNECTED) {
        /* Client connected between CreateNamedPipe and ConnectNamedPipe — also fine */
        return server_fd;
    }

    if (err == ERROR_PIPE_LISTENING) {
        /* No client yet (standard PIPE_NOWAIT response) */
        return -1;
    }

    if (err == ERROR_NO_DATA) {
        /* No client yet — alternate response on newer Windows with PIPE_NOWAIT */
        return -1;
    }

    /* Any other error is unexpected */
    fprintf(stderr, "[guardian] ConnectNamedPipe error: %lu\n", err);
    return -1;
}

/*
 * platform_ipc_connect — CLIENT: open a connection to the server pipe
 *
 * On the client side, we use CreateFile() to open the Named Pipe by name.
 * This is the same function used to open regular files — Named Pipes look
 * like files from the client's perspective. That's part of Windows' design:
 * "everything is a file handle."
 */
int platform_ipc_connect(const char *path) {
    char pipe_path[256];
    to_pipe_path(path, pipe_path, sizeof(pipe_path));

    WaitNamedPipeA(pipe_path, 2000);  /* wait up to 2 seconds for pipe to be available */

    HANDLE h = CreateFileA(
        pipe_path,
        GENERIC_READ | GENERIC_WRITE,  /* we need both directions */
        0,                              /* no sharing */
        NULL,                           /* default security */
        OPEN_EXISTING,                  /* pipe must already exist */
        0,                              /* no special flags */
        NULL                            /* no template */
    );

    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }

    /* Switch to blocking mode for the client — we WANT to wait for the response */
    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(h, &mode, NULL, NULL);

    return ipc_table_alloc(h);
}

/*
 * platform_ipc_send — write bytes to a Named Pipe handle
 *
 * WriteFile() is the Windows function for writing to any HANDLE —
 * files, pipes, consoles, etc. It writes exactly 'len' bytes.
 */
int platform_ipc_send(int fd, const char *buf, int len) {
    HANDLE h = ipc_table_get(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;

    DWORD written = 0;
    BOOL ok = WriteFile(h, buf, (DWORD)len, &written, NULL);
    return ok ? (int)written : -1;
}

/*
 * platform_ipc_recv — read bytes from a Named Pipe handle
 *
 * SERVER side (PIPE_NOWAIT): PeekNamedPipe first so we never block the
 * event loop. Returns 0 immediately if no data is available yet.
 *
 * CLIENT side (blocking mode): skip the peek and call ReadFile directly.
 * The client has already sent its command and is waiting for a response —
 * we WANT to block here until the daemon sends the reply (up to ~100ms,
 * the daemon's event-loop tick interval). Without this, the client would
 * peek, find 0 bytes (response not yet written), and return immediately
 * before the daemon ever gets a chance to process the command.
 */
int platform_ipc_recv(int fd, char *buf, int len) {
    HANDLE h = ipc_table_get(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;

    if (g_ipc_is_server[fd]) {
        /* Non-blocking path: check without committing to a read */
        DWORD available = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &available, NULL)) return -1;
        if (available == 0) return 0;  /* nothing to read yet */
    }
    /* else: client fd in blocking mode — fall straight through to ReadFile */

    DWORD read_bytes = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)(len - 1), &read_bytes, NULL);
    if (ok && read_bytes > 0) {
        buf[read_bytes] = '\0';  /* null-terminate for safety */
        return (int)read_bytes;
    }
    return -1;
}

/*
 * platform_ipc_close — close or disconnect a Named Pipe handle
 *
 * For the SERVER fd: we call DisconnectNamedPipe to reset it back to
 * listening state (so it can accept the next client), NOT CloseHandle.
 * The server pipe persists for the life of the daemon.
 *
 * For CLIENT fds: we call CloseHandle to release the connection.
 *
 * How do we tell them apart? We track which fds are server pipes
 * vs client connections using a separate flag array.
 */
void platform_ipc_close(int fd) {
    HANDLE h = ipc_table_get(fd);
    if (h == INVALID_HANDLE_VALUE) return;

    if (g_ipc_is_server[fd]) {
        /* Reset the server pipe to accept the next client */
        DisconnectNamedPipe(h);
        /* Don't free the table slot — the server pipe lives on */
    } else {
        /* Client connection — close it fully */
        CloseHandle(h);
        ipc_table_free(fd);
    }
}

/* ==========================================================================
 * TCP socket implementation — Phase 5
 *
 * Windows Sockets (Winsock) is Microsoft's implementation of the BSD sockets
 * API. It's very similar to POSIX sockets with two key differences:
 *
 *   1. WSAStartup() must be called once before any socket operations.
 *   2. Socket handles are SOCKET (UINT_PTR), not int file descriptors.
 *
 * We use a small lookup table (g_sock_table) to map between our int fd
 * abstraction and the real Windows SOCKET values.
 *
 * The table uses indices 100..115 to avoid collision with the IPC table
 * (which uses indices 0..15).
 * ==========================================================================
 */

#define MAX_SOCK_FDS   16
#define SOCK_FD_OFFSET 100  /* our TCP fds start at 100 to separate from IPC fds */

static SOCKET g_sock_table[MAX_SOCK_FDS];
static int    g_sock_table_init = 0;

static void sock_table_init_once(void) {
    if (g_sock_table_init) return;
    for (int i = 0; i < MAX_SOCK_FDS; i++) g_sock_table[i] = INVALID_SOCKET;
    g_sock_table_init = 1;
}

static int sock_table_alloc(SOCKET s) {
    sock_table_init_once();
    for (int i = 0; i < MAX_SOCK_FDS; i++) {
        if (g_sock_table[i] == INVALID_SOCKET) {
            g_sock_table[i] = s;
            return i + SOCK_FD_OFFSET;
        }
    }
    return -1;  /* table full */
}

static SOCKET sock_table_get(int fd) {
    int i = fd - SOCK_FD_OFFSET;
    if (i < 0 || i >= MAX_SOCK_FDS) return INVALID_SOCKET;
    return g_sock_table[i];
}

static void sock_table_free(int fd) {
    int i = fd - SOCK_FD_OFFSET;
    if (i >= 0 && i < MAX_SOCK_FDS) g_sock_table[i] = INVALID_SOCKET;
}

/*
 * ensure_wsa — initialize Windows Sockets exactly once
 *
 * WSAStartup(version, data) must be called before any socket function.
 * MAKEWORD(2,2) requests Winsock version 2.2 — the current standard.
 * Multiple calls are harmless (we guard with a flag).
 *
 * WSACleanup() is the counterpart. For a long-running daemon, calling it
 * at exit is good practice but not strictly required — the OS cleans up
 * all resources when the process ends.
 */
static int g_wsa_init = 0;
static void ensure_wsa(void) {
    if (g_wsa_init) return;
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    g_wsa_init = 1;
}

/*
 * platform_tcp_connect — open a TCP connection with a timeout
 *
 * The POSIX connect() blocks until connected or the OS gives up (~75s default).
 * We want a much shorter timeout (e.g. 3 seconds for health checks).
 *
 * Technique: make the socket non-blocking, call connect() (returns immediately
 * with WSAEWOULDBLOCK), then use select() to wait with our timeout.
 * If select() says the socket became writable, the connection succeeded.
 *
 * After connecting, we put the socket back in blocking mode (for send/recv),
 * and we set SO_RCVTIMEO so that future reads also respect the timeout.
 */
int platform_tcp_connect(const char *host, int port, int timeout_ms) {
    ensure_wsa();

    /*
     * getaddrinfo resolves hostname → IP address.
     * It handles both "127.0.0.1" (numeric) and "localhost" (DNS lookup).
     * The result is a linked list of addresses — we try the first one.
     */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;       /* IPv4 */
    hints.ai_socktype = SOCK_STREAM;   /* TCP  */

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
        return -1;  /* hostname resolution failed */
    }

    /* Create a TCP socket */
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        return -1;
    }

    /*
     * Switch to non-blocking mode.
     * ioctlsocket(FIONBIO, 1) = "set non-blocking I/O on"
     * After this, connect() returns immediately with WSAEWOULDBLOCK
     * instead of blocking until the connection is established.
     */
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);

    /* Start the connection attempt (returns immediately) */
    connect(s, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);

    /*
     * select() waits until the socket becomes writable (= connected)
     * or until our timeout expires.
     *
     * fd_set is a set of socket handles.
     * FD_ZERO clears it; FD_SET adds our socket to it.
     * On Windows, the first argument to select() (nfds) is ignored — pass 0.
     */
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(s, &write_set);

    int ready = select(0, NULL, &write_set, NULL, &tv);
    if (ready <= 0) {
        /* Timeout or error — connection failed */
        closesocket(s);
        return -1;
    }

    /*
     * select() returned positive, but that doesn't guarantee success.
     * A connection FAILURE also makes the socket writable (via the error set).
     * We check SO_ERROR to confirm there's no pending error.
     */
    int sock_err = 0;
    int optlen   = sizeof(sock_err);
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&sock_err, &optlen);
    if (sock_err != 0) {
        closesocket(s);
        return -1;
    }

    /* Switch back to blocking mode for send/recv */
    nonblocking = 0;
    ioctlsocket(s, FIONBIO, &nonblocking);

    /*
     * Set a receive timeout so ReadFile/recv won't block forever.
     * SO_RCVTIMEO takes a DWORD value in milliseconds on Windows.
     */
    DWORD recv_timeout = (DWORD)timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_timeout, sizeof(recv_timeout));

    return sock_table_alloc(s);
}

int platform_tcp_send(int fd, const char *buf, int len) {
    SOCKET s = sock_table_get(fd);
    if (s == INVALID_SOCKET) return -1;
    int n = send(s, buf, len, 0);
    return n == SOCKET_ERROR ? -1 : n;
}

/*
 * platform_tcp_recv — read available bytes from a TCP socket
 *
 * Calls recv() which blocks until data arrives or SO_RCVTIMEO elapses.
 * Returns the number of bytes read, or -1 on error/timeout/close.
 */
int platform_tcp_recv(int fd, char *buf, int len) {
    SOCKET s = sock_table_get(fd);
    if (s == INVALID_SOCKET) return -1;
    int n = recv(s, buf, len, 0);
    return n > 0 ? n : -1;
}

void platform_socket_close(int fd) {
    SOCKET s = sock_table_get(fd);
    if (s != INVALID_SOCKET) closesocket(s);
    sock_table_free(fd);
}
