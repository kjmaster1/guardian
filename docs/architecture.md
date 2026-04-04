# Guardian — Architecture

This document explains the key design decisions behind guardian: why the code
is structured the way it is, what alternatives were considered, and what
trade-offs were made.

---

## Overview

Guardian is a **single-threaded event loop** with **one health-check thread per
service**. The main thread ticks every 100ms; health threads run independently
and push results into a shared queue.

```
┌─────────────────────────────────────────────────────┐
│  Main thread (100ms tick)                           │
│                                                     │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────┐  │
│  │ supervisor   │  │    ipc       │  │ platform │  │
│  │   _tick()    │  │  _tick()     │  │ _sleep   │  │
│  │              │  │              │  │  _ms()   │  │
│  │ • check each │  │ • accept one │  │          │  │
│  │   process    │  │   command    │  │  100ms   │  │
│  │ • drain      │  │ • send JSON  │  │          │  │
│  │   health q   │  │   response   │  │          │  │
│  └──────────────┘  └──────────────┘  └──────────┘  │
└─────────────────────────────────────────────────────┘
         ↑ push results (mutex-protected)
┌─────────────────────────────────────────────────────┐
│  Health threads (one per service with health check) │
│                                                     │
│  Thread A: sleep 10s → TCP probe → push result      │
│  Thread B: sleep 10s → HTTP probe → push result     │
└─────────────────────────────────────────────────────┘
```

---

## Why single-threaded main loop?

The main loop touches mutable state on every service every 100ms. If it ran on
multiple threads, every field of every `ServiceRuntime` struct would need a
lock. That's 64 services × ~10 fields = hundreds of potential data races.

A single-threaded loop eliminates all of that. The loop body runs sequentially:
check processes, drain health queue, accept IPC, sleep. No races possible
within the loop body.

The one exception is the health result queue, which health threads write to
concurrently. That single queue gets one `pthread_mutex_t`. The lock is held
for microseconds (copy one struct in or out); the main thread holds it for
well under 1% of each 100ms tick.

This is the same model used by Redis, nginx (worker processes), and Node.js
— single-threaded event loop, push blocking work to separate threads.

---

## Data model

Everything lives in `SupervisorContext`, stack-allocated in `main()`:

```c
SupervisorContext ctx;          // ~200 KB on the stack, lives for process lifetime
config_load(path, &ctx);        // fills in ctx.services[]
supervisor_run(&ctx);           // runs until Ctrl+C
```

The `Service` struct separates immutable config from mutable runtime:

```c
typedef struct {
    ServiceConfig  cfg;   // parsed from INI, never modified after load
    ServiceRuntime rt;    // updated every tick: state, pid, retry_count, ...
} Service;
```

This separation matters because health threads read `cfg.health` (immutable,
no lock needed) while the main thread writes `rt.state` (main thread only,
no lock needed because there's only one writer).

---

## State machine

Each service is always in exactly one state:

```
STOPPED ──autostart──► STARTING ──alive──► RUNNING ──crash──► BACKOFF
   ▲                                          │                    │
   │                                        stop                 timer
   │                                          │                elapsed
   └──────────────── STOPPING ◄──────────────┘         ┌──────────┘
                         │                              │
                     confirmed                     max_retries?
                      exited                            │
                         └──────────────────────────► FAILED
```

The state machine is implemented as a `switch` in `supervisor_tick()`. Each
case handles exactly one state; transitions are always explicit assignments.
There are no implicit transitions, no default cases that silently swallow
unexpected states.

Health-check-triggered restarts re-use the BACKOFF state: when the health
threshold is exceeded, the service is stopped (`STATE_STOPPING`) with a flag
set (`health_triggered_stop = 1`). When `handle_exited_service` sees that flag,
it goes to BACKOFF rather than STOPPED.

---

## Platform abstraction

The header `platform.h` declares the complete OS interface:

```c
SpawnResult   platform_spawn(command, args, working_dir, env, ...);
ProcessStatus platform_check_process(pid, handle);
int           platform_terminate(pid, handle);
int64_t       platform_now_ms(void);
void          platform_sleep_ms(int ms);
int           platform_tcp_connect(host, port, timeout_ms);
int           platform_ipc_listen(path);
int           platform_ipc_accept(server_fd);
// ... etc.
```

`platform_linux.c` and `platform_windows.c` each implement all of these.
Zero `#ifdef` appears outside those two files. The rest of the codebase doesn't
know what OS it's running on.

### Key difference: process creation

| Linux | Windows |
|---|---|
| `fork()` copies the current process | `CreateProcess()` creates a fresh process |
| `execvp()` replaces the child image | No exec needed — CreateProcess combines both |
| Between fork and exec: `chdir()`, `dup2()`, `setenv()` | STARTUPINFOA struct configures everything |
| `waitpid(WNOHANG)` reaps zombies | `WaitForSingleObject(handle, 0)` polls exit |

### Key difference: IPC

| Linux | Windows |
|---|---|
| Unix domain socket (AF_UNIX) | Named Pipe (`\\.\pipe\name`) |
| `socket()` returns an int fd | HANDLE stored in a lookup table → int fd |
| `O_NONBLOCK` on server socket | `PIPE_NOWAIT` on pipe |
| `accept()` returns a new fd per client | Same HANDLE for server and client |
| `unlink(path)` on close to clean up | OS cleans up when all handles close |

### Why a handle table on Windows?

The public API uses `int` file descriptors everywhere for portability. On
Linux, `socket()` returns an int directly. On Windows, `CreateNamedPipe()`
returns a `HANDLE` (a `void*`). 

`platform_windows.c` maintains a static array of 16 HANDLEs indexed by small
integers, converting between them transparently. This is precisely how the OS
kernel implements file descriptors internally — an fd is just an index into a
per-process handle table.

---

## Health check threading

Health threads are created with `pthread_create()` and joined at shutdown with
`pthread_join()`. The key design points:

**Thread argument lifetime.** The thread arg struct is `malloc`'d before
`pthread_create` and `free`'d at the start of `health_thread_fn`. Stack
allocation would be wrong — the creating function returns immediately after
spawning the thread, taking the stack frame (and the arg) with it.

**Responsive shutdown.** Threads sleep in 100ms increments rather than the
full `interval_s`:
```c
for (int i = 0; i < hc->interval_s * 10 && ctx->running; i++) {
    platform_sleep_ms(100);
}
```
This means guardian shuts down within 100ms of receiving SIGTERM, rather than
waiting up to `interval_s` (potentially 60 seconds) for each health thread
to wake up.

**Result queue as a circular buffer.** Health threads push `HealthResult`
structs into a fixed-size ring buffer; the main thread pops them every tick.
No `malloc()` in the hot path. If the queue fills (main thread is somehow
lagging), the oldest result is silently dropped — losing one probe result is
harmless, and the next probe will fire within `interval_s` anyway.

---

## IPC protocol

The protocol is deliberately minimal: newline-free JSON over a byte stream.

```
Client → Server:  {"cmd":"status"}
Server → Client:  {"ok":true,"services":[{"name":"web-api","state":"RUNNING",...}]}

Client → Server:  {"cmd":"stop"}
Server → Client:  {"ok":true}
```

No JSON library is used. Output is built with `snprintf`. Input is parsed with
`strstr` — just enough to distinguish two commands. For our fixed protocol this
is completely correct; a general-purpose parser would add complexity with no
benefit.

The server-side IPC is non-blocking: `platform_ipc_accept()` is called every
100ms tick and returns -1 immediately if no client is connected. One client
per tick is processed — sufficient for the expected use pattern (a human typing
`guardian status`).

---

## The config parser

The INI parser is a state machine with four states: NONE, SUPERVISOR, SERVICE,
HEALTH_CHECK. It reads line by line with `fgets`, strips comments and
whitespace, and dispatches on the line type:

- `[section]` → update current state
- `key = value` → parse according to current state
- blank / comment → skip

`[health_check]` sections are scoped to the immediately preceding service
section by tracking a `current_service_index`. This implicit scoping mirrors
how nginx and Apache handle nested block contexts.

The parser is ~300 lines. It handles: quoted values (none needed in practice),
inline comments, leading/trailing whitespace, integer parsing with range
validation, and unknown keys (silently ignored for forward compatibility).

---

## Logging

The logger writes structured lines to stdout and optionally a log file:

```
[INFO ][web-api  ][2026-04-04T10:23:01Z] Starting: /opt/myapp/api-server --port 8080
[WARN ][web-api  ][2026-04-04T10:23:02Z] Health check failed (1/3)
[ERROR][web-api  ][2026-04-04T10:23:22Z] Giving up after 5 restart attempts
```

Log macros accept variadic arguments the same way `printf` does:
```c
LOG_INFO("web-api", "Starting: %s %s", cfg->command, cfg->args);
```

The implementation uses `va_list`/`vfprintf` — the same mechanism the C
standard library uses to implement `printf` itself.

---

## Zero external dependencies

Every non-trivial piece that could have used a library was instead implemented
from scratch, specifically to demonstrate the underlying concepts:

| Feature | Implementation | Size |
|---|---|---|
| INI parsing | `fgets` + `strchr` + `strtol` | ~300 lines |
| JSON output | `snprintf` templates | ~30 lines |
| JSON input | `strstr` for two commands | ~10 lines |
| HTTP health check | Raw TCP + 4-line GET | ~60 lines |
| Logging | `fprintf` + `strftime` | ~80 lines |
| Test framework | `ASSERT`/`TEST`/`SUMMARY` macros | ~30 lines |

System libraries: `-lpthread` (always), `-lws2_32` (Windows only).
