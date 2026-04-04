# Guardian — Configuration Reference

Guardian reads a single INI file. Section names become service display names.
Comments begin with `;` or `#`. Whitespace around `=` is ignored.

---

## [supervisor]

Global settings for the guardian process itself. Optional — you can omit this
section entirely and rely on defaults.

| Key | Type | Default | Description |
|---|---|---|---|
| `log_file` | path | *(stdout only)* | Append guardian's own log to this file |
| `ipc_socket` | path | `/tmp/guardian.sock` | Socket / Named Pipe path for `stop` and `status` commands |

**Notes:**
- On Windows, `ipc_socket` is converted to a Named Pipe path automatically
  (e.g. `/tmp/guardian.sock` → `\\.\pipe\guardian.sock`). You can use the
  same path on both platforms.
- `log_file` uses `fopen` in append mode — safe across guardian restarts.

---

## [service-name]

Defines one managed process. The section name is the service's display name in
logs and `guardian status` output. You may define up to 64 services.

### Required

| Key | Type | Description |
|---|---|---|
| `command` | path | Full path to the executable |

### Process options

| Key | Type | Default | Description |
|---|---|---|---|
| `args` | string | *(empty)* | Command-line arguments, space-separated |
| `working_dir` | path | *(inherit)* | Working directory before exec |
| `env` | string | *(empty)* | Extra environment variables: `KEY=VAL,KEY2=VAL2` |
| `stdout_log` | path | *(inherit)* | Redirect process stdout to this file (append mode) |
| `stderr_log` | path | *(inherit)* | Redirect process stderr to this file (append mode) |

### Lifecycle options

| Key | Type | Default | Description |
|---|---|---|---|
| `autostart` | bool | `true` | Start automatically when guardian launches |
| `restart` | enum | `on_failure` | When to restart: `never`, `on_failure`, `always` |
| `max_retries` | int | `5` | Max restart attempts; `0` = unlimited |
| `backoff_base_ms` | int (ms) | `1000` | Initial restart delay |
| `backoff_max_ms` | int (ms) | `60000` | Maximum restart delay |

### Restart policies

| Value | Behaviour |
|---|---|
| `never` | Never restart. Use for one-shot jobs (database migrations, etc.) |
| `on_failure` | Restart only if exit code ≠ 0 or killed by a signal |
| `always` | Always restart, even on a clean `exit(0)` |

### Backoff formula

```
delay(n) = min(backoff_base_ms × 2ⁿ, backoff_max_ms)
```

With `backoff_base_ms = 1000` and `backoff_max_ms = 60000`:

| Attempt | Delay |
|---|---|
| 1 | 1 s |
| 2 | 2 s |
| 3 | 4 s |
| 4 | 8 s |
| 5 | 16 s |
| 6 | 32 s |
| 7+ | 60 s (capped) |

---

## [health_check]

An optional section that immediately follows a `[service]` section. It attaches
to that service. Each service may have at most one health check.

| Key | Type | Default | Description |
|---|---|---|---|
| `type` | enum | — | `tcp`, `http`, or `command` |
| `target` | string | — | What to probe (format depends on type) |
| `interval_s` | int (s) | `10` | Seconds between probes |
| `timeout_s` | int (s) | `3` | Seconds before a probe is considered failed |
| `unhealthy_threshold` | int | `3` | Consecutive failures before triggering a restart |

### Type: tcp

Attempts a TCP connection to `host:port`. Succeeds if the connection is
established within `timeout_s`.

```ini
[health_check]
type   = tcp
target = 127.0.0.1:5432
```

Use for: databases, caches, any service that just needs a port open.

### Type: http

Performs `GET /path HTTP/1.0` and checks that the response status is 2xx.

```ini
[health_check]
type   = http
target = http://127.0.0.1:8080/healthz
```

URL format: `http://host[:port]/path` (HTTPS not supported).

Use for: web APIs with a dedicated `/health` or `/healthz` endpoint.

### Type: command

Runs `target` as a command. `exit(0)` = healthy; any other exit code = unhealthy.
The command is killed after `timeout_s` seconds.

```ini
[health_check]
type   = command
target = /opt/scripts/check-db-schema.sh
```

Use for: custom health logic that TCP/HTTP can't express.

---

## Full example

```ini
; guardian.ini

[supervisor]
log_file   = /var/log/guardian.log
ipc_socket = /tmp/guardian.sock

; --- Web API ---------------------------------------------------------------
[web-api]
command          = /opt/myapp/api-server
args             = --port 8080 --env production
working_dir      = /opt/myapp
env              = DATABASE_URL=postgres://localhost/prod
stdout_log       = /var/log/web-api.log
stderr_log       = /var/log/web-api-err.log
autostart        = true
restart          = on_failure
max_retries      = 5
backoff_base_ms  = 1000
backoff_max_ms   = 60000

[health_check]
type                 = http
target               = http://127.0.0.1:8080/healthz
interval_s           = 10
timeout_s            = 3
unhealthy_threshold  = 3

; --- Database --------------------------------------------------------------
[postgres]
command          = /usr/bin/postgres
args             = -D /var/lib/postgres/data
working_dir      = /var/lib/postgres
autostart        = true
restart          = always
max_retries      = 0

[health_check]
type                 = tcp
target               = 127.0.0.1:5432
interval_s           = 15
timeout_s            = 2
unhealthy_threshold  = 2

; --- One-shot migration (runs once at startup, never restarts) -------------
[db-migrate]
command     = /opt/myapp/migrate
args        = --direction up
working_dir = /opt/myapp
autostart   = true
restart     = never
```

---

## Boolean values

`autostart` accepts: `true`, `yes`, `1` (enabled) or `false`, `no`, `0` (disabled).
Comparison is case-insensitive.

## Path handling

Paths are passed to the OS as-is. On Windows, use either forward slashes
(`C:/logs/guardian.log`) or escaped backslashes
(`C:\\logs\\guardian.log`). Both work with the Windows API.

## Unknown keys

Unknown keys are silently ignored, which allows adding new options in a future
guardian version without breaking existing configs.
