# guardian

[![CI](https://github.com/kurti/guardian/actions/workflows/ci.yml/badge.svg)](https://github.com/kurti/guardian/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A lightweight, cross-platform process supervisor written in C11.

Guardian monitors long-running processes, restarts them automatically on crash
with exponential backoff, checks their health via TCP/HTTP/command probes, and
exposes a simple CLI for status and control — all in a single self-contained
binary with zero runtime dependencies.

---

## Why guardian exists

Every major language ecosystem has a process supervisor: Python has
[supervisord](http://supervisord.org/), Go has [overmind](https://github.com/DarthSim/overmind),
Node has [PM2](https://pm2.keymetrics.io/). The C options are either ancient
([daemontools](https://cr.yp.to/daemontools.html), 1997), Linux-only
([s6](https://skarnet.org/software/s6/), [runit](http://smarden.org/runit/)),
or require a full init system.

Guardian fills a gap: a **single-binary**, **Windows + Linux** supervisor with
**built-in health checks**, a **clean INI config**, and an **IPC command
interface** — built from scratch in standard C11 with no external libraries.

---

## Features

| Feature | Details |
|---|---|
| Process lifecycle | Start, stop, restart, exponential backoff |
| Health checks | TCP port, HTTP status code, or arbitrary command |
| Restart policies | `never`, `on_failure`, `always` |
| IPC control | `stop` and `status` via Unix socket / Named Pipe |
| Log tailing | `guardian logs` follows a service's stdout |
| Cross-platform | Windows (Win32 API) and Linux (POSIX) |
| Zero dependencies | No libraries beyond the OS and `-lpthread` |
| Single binary | One executable, one config file |

---

## Quick start

### Build

**Linux / WSL:**
```bash
sudo apt install gcc make      # one-time setup
make
./guardian version
```

**Windows (MinGW):**
```bat
# Install GCC: https://winlibs.com  (add C:\mingw64\bin to PATH)
make
guardian.exe version
```

### Run

```bash
# Start all configured services
./guardian start guardian.ini

# In another terminal: check status
./guardian status guardian.ini

# Tail a service's log
./guardian logs web-api guardian.ini

# Graceful shutdown
./guardian stop guardian.ini
```

---

## Configuration

Guardian reads an INI file. Here is a realistic example:

```ini
[supervisor]
log_file   = /var/log/guardian.log
ipc_socket = /tmp/guardian.sock

[web-api]
command     = /opt/myapp/api-server
args        = --port 8080
working_dir = /opt/myapp
stdout_log  = /var/log/web-api.log
autostart   = true
restart     = on_failure
max_retries = 5
backoff_base_ms = 1000
backoff_max_ms  = 60000

[health_check]
type                = http
target              = http://127.0.0.1:8080/healthz
interval_s          = 10
timeout_s           = 3
unhealthy_threshold = 3

[database]
command     = /usr/bin/postgres
args        = -D /var/lib/postgres/data
restart     = always
max_retries = 0

[health_check]
type                = tcp
target              = 127.0.0.1:5432
interval_s          = 15
timeout_s           = 2
unhealthy_threshold = 2
```

See [`guardian.ini.example`](guardian.ini.example) for full documentation of
every option.

---

## CLI reference

```
guardian start  <config-file>                 Start the supervisor
guardian stop   [config-file]                 Stop a running supervisor
guardian status [config-file]                 Show live service status
guardian logs   <service-name> <config-file>  Tail a service's log file
guardian version                              Print version and build info
```

### `guardian status` output

```
  SERVICE          STATE      PID       RESTARTS  UPTIME
  -------          -----      ---       --------  ------
  web-api          RUNNING    12483     0         4m 12s
  database         RUNNING    12485     0         4m 12s
  worker           BACKOFF    0         2         -
```

### Service states

| State | Meaning |
|---|---|
| `STOPPED` | Not running. Either never started or stopped intentionally. |
| `STARTING` | Process launched, not yet confirmed alive. |
| `RUNNING` | Process is alive and (if configured) passing health checks. |
| `BACKOFF` | Crashed; waiting before the next restart attempt. |
| `STOPPING` | Terminate signal sent; waiting for process to exit. |
| `FAILED` | Gave up after exhausting `max_retries`. |

---

## Health checks

Guardian supports three health check types:

**TCP** — verifies the port is open (lightest probe):
```ini
[health_check]
type   = tcp
target = 127.0.0.1:5432
```

**HTTP** — sends `GET /path HTTP/1.0` and checks for a 2xx status code:
```ini
[health_check]
type   = http
target = http://127.0.0.1:8080/healthz
```

**Command** — runs a script; `exit 0` = healthy, anything else = unhealthy:
```ini
[health_check]
type   = command
target = /opt/scripts/check-db.sh
```

Each health check runs in its own thread so a slow probe never blocks the
main event loop or delays crash detection for other services.

When `unhealthy_threshold` consecutive probes fail, guardian stops the service
and schedules a restart using the same exponential backoff as a crash restart.

---

## Exponential backoff

When a service crashes, guardian waits before restarting to avoid a tight
restart loop overwhelming the system:

```
Retry 1: wait 1s     (base_ms = 1000)
Retry 2: wait 2s
Retry 3: wait 4s
Retry 4: wait 8s
Retry 5: wait 16s
...
Retry N: wait min(base * 2^N, max_ms)
```

With `max_retries = 0`, guardian retries forever (suitable for critical services).
With `max_retries = 5`, the service transitions to `FAILED` after 5 attempts.

---

## Architecture

```
main.c
  └─ cli.c              CLI argument dispatch
       ├─ config.c       INI file parser → ServiceConfig[]
       └─ supervisor.c   Main event loop (100ms tick)
            ├─ service.c    spawn / stop / backoff logic
            ├─ ipc.c        IPC server (stop / status commands)
            ├─ health.c     Health check threads + result queue
            └─ platform/
                 ├─ platform.h          OS-neutral interface
                 ├─ platform_linux.c    fork/exec, POSIX sockets
                 └─ platform_windows.c  CreateProcess, Named Pipes
```

**Event loop model:** The main thread ticks every 100ms. Each tick: check all
process states (non-blocking), drain the health result queue, service one IPC
connection. Health checks run on dedicated threads (one per configured service)
because TCP connects and HTTP requests can block.

**Platform abstraction:** All `#ifdef _WIN32` lives in `platform_windows.c`.
Every other source file includes only `platform.h` and calls functions like
`platform_spawn()` and `platform_ipc_listen()`. This pattern is used in
production codebases like Chromium and SQLite.

See [`docs/architecture.md`](docs/architecture.md) for a deeper walkthrough.

---

## Building and testing

```bash
make              # build guardian (or guardian.exe on Windows)
make test         # run the unit test suite
make clean        # remove build artifacts
sudo make install # copy to /usr/local/bin (Linux)
```

The test suite covers the config parser (20 tests) with a zero-dependency
inline test framework (`tests/test_framework.h`).

Compiler flags: `-std=c11 -Wall -Wextra -Wpedantic -Werror` — no warnings
are tolerated in the codebase.

---

## Project structure

```
guardian/
├── Makefile
├── guardian.ini.example          # Fully-commented example config
├── src/
│   ├── main.c                    # Entry point
│   ├── cli.c / cli.h             # CLI command dispatch
│   ├── config.c / config.h       # INI parser
│   ├── supervisor.c / .h         # Event loop
│   ├── service.c / .h            # Process lifecycle + data model
│   ├── health.c / .h             # Health check threads
│   ├── logger.c / .h             # Structured log writer
│   ├── ipc.c / .h                # IPC server + client
│   └── platform/
│       ├── platform.h            # OS abstraction interface
│       ├── platform_linux.c      # Linux implementation
│       └── platform_windows.c    # Windows implementation
├── tests/
│   ├── test_framework.h          # Inline ASSERT/TEST macros
│   └── test_config.c             # Config parser unit tests
├── tools/
│   └── gen_version.py            # Writes src/version.h from git describe
└── docs/
    ├── architecture.md
    └── config_reference.md
```

---

## What I built this to learn

This project was built phase-by-phase as a deep dive into systems programming in C:

- **Memory model** — stack vs. heap allocation, when and why to use each
- **Process management** — `fork()`+`exec()` on Linux, `CreateProcess` on Windows, zombie reaping with `waitpid()`
- **State machines** — encoding service lifecycle (STOPPED → STARTING → RUNNING → BACKOFF → FAILED) as an explicit enum with defined transitions
- **Event loops** — single-threaded 100ms tick loop, non-blocking I/O everywhere in the hot path
- **Threads and mutexes** — health check threads with `pthread`, protecting the result queue with `pthread_mutex_t`
- **Platform abstraction** — isolating all OS-specific code behind a clean C interface; zero `#ifdef` outside `platform_*.c`
- **IPC** — Unix domain sockets (Linux) and Named Pipes (Windows) for live `stop`/`status` commands
- **TCP networking** — non-blocking `connect()` with `select()` timeout for health probes
- **Build systems** — GNU Make with platform detection, automated version header generation
- **Zero-dependency design** — custom INI parser, JSON serialiser, test framework, all under ~200 lines each

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the ground rules, code style guide,
and how to run the test suite on both platforms.

## License

MIT — see [LICENSE](LICENSE) for details.
