/*
 * health.c — Health check thread implementation
 *
 * Key C concepts demonstrated here:
 *   - pthread_create/join   — spawning and waiting for threads
 *   - pthread_mutex_lock/unlock — protecting shared data
 *   - Circular buffers      — fixed-size lock-minimal queue
 *   - TCP sockets           — connecting to a host:port to verify it's up
 *   - Minimal HTTP client   — send GET, parse status line, check 2xx
 *   - malloc/free           — when stack allocation isn't sufficient
 *     (thread args must outlive the creating function's stack frame)
 */

#include "health.h"
#include "logger.h"
#include "platform/platform.h"

#include <stdio.h>    /* snprintf, sscanf    */
#include <string.h>   /* memset, strncpy, strncmp, strchr */
#include <stdlib.h>   /* malloc, free, atoi  */
#include <pthread.h>  /* pthread_*           */

/* ==========================================================================
 * Thread argument
 *
 * We pass this to each health thread via pthread_create's void* arg.
 *
 * WHY malloc and not a stack variable?
 * pthread_create returns immediately — the new thread runs concurrently
 * with the calling function. If we stored the arg on the stack:
 *
 *   void health_start_threads(SupervisorContext *ctx) {
 *       HealthThreadArg arg = { ctx, i };      ← stack variable
 *       pthread_create(..., &arg);             ← thread starts
 *   }  ← FUNCTION RETURNS — arg is gone, but thread still holds a pointer to it!
 *
 * malloc() allocates from the heap, which lives until we explicitly free().
 * The thread frees the arg at the very end of health_thread_fn.
 * ==========================================================================
 */
typedef struct {
    SupervisorContext *ctx;
    int               service_index;
} HealthThreadArg;

/* ==========================================================================
 * Queue operations — called from BOTH health threads and the main thread
 * ==========================================================================
 */

void health_queue_init(HealthResultQueue *q) {
    /*
     * memset zeroes the whole struct (head=0, tail=0, count=0, items all zero).
     * We still need to explicitly initialize the mutex — memset doesn't do that.
     *
     * pthread_mutex_init(&mutex, NULL) initializes with default attributes:
     *   - Not recursive (calling lock() twice from same thread = deadlock)
     *   - Not priority-inheriting
     * For our use, the defaults are correct.
     */
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
}

void health_queue_push(HealthResultQueue *q, const HealthResult *r) {
    /*
     * LOCK — only one thread can be in this critical section at a time.
     *
     * Without the lock, two health threads pushing simultaneously could both
     * read count, both see it's below capacity, both write to the same slot,
     * and both increment count — leaving a corrupted queue with one lost entry.
     * This is a classic "lost update" race condition.
     */
    pthread_mutex_lock(&q->lock);

    if (q->count < GUARDIAN_HEALTH_QUEUE_CAP) {
        /*
         * Write to the tail slot.
         * The % GUARDIAN_HEALTH_QUEUE_CAP wraps tail around when it reaches
         * the end of the array, making it circular.
         */
        q->items[q->tail] = *r;
        q->tail = (q->tail + 1) % GUARDIAN_HEALTH_QUEUE_CAP;
        q->count++;
    }
    /* If full: drop this result silently. The main thread will catch up. */

    pthread_mutex_unlock(&q->lock);
}

int health_queue_pop(HealthResultQueue *q, HealthResult *out) {
    pthread_mutex_lock(&q->lock);

    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return 0;  /* empty */
    }

    /* Read from the head slot and advance head */
    *out = q->items[q->head];
    q->head = (q->head + 1) % GUARDIAN_HEALTH_QUEUE_CAP;
    q->count--;

    pthread_mutex_unlock(&q->lock);
    return 1;
}

/* ==========================================================================
 * URL and address parsing helpers
 * ==========================================================================
 */

/*
 * parse_host_port — extract host and port from "host:port" string
 *
 * Used for TCP health checks where target = "127.0.0.1:8080"
 * Returns 1 on success, 0 if the string doesn't contain a colon.
 */
static int parse_host_port(const char *target,
                            char *host, int host_size,
                            int  *port) {
    const char *colon = strchr(target, ':');
    if (!colon) return 0;

    int len = (int)(colon - target);
    if (len >= host_size) len = host_size - 1;
    strncpy(host, target, (size_t)len);
    host[len] = '\0';

    *port = atoi(colon + 1);
    return 1;
}

/*
 * parse_http_url — split "http://host[:port]/path" into its components
 *
 * Examples:
 *   "http://127.0.0.1:8080/health"  → host="127.0.0.1", port=8080, path="/health"
 *   "http://localhost/ready"        → host="localhost",  port=80,   path="/ready"
 *   "http://example.com"            → host="example.com", port=80,  path="/"
 *
 * Returns 1 on success, 0 if the URL doesn't start with "http://".
 */
static int parse_http_url(const char *url,
                           char *host, int host_size,
                           int  *port,
                           char *path, int path_size) {
    if (strncmp(url, "http://", 7) != 0) return 0;

    const char *p = url + 7;  /* skip "http://" */
    *port = 80;                /* default HTTP port */

    /* Find the end of the host (either '/', ':', or end of string) */
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    /* The colon must come before the slash (if both exist) to be a port */
    if (colon && (!slash || colon < slash)) {
        /* Has explicit port: "host:port/path" */
        int len = (int)(colon - p);
        if (len >= host_size) len = host_size - 1;
        strncpy(host, p, (size_t)len);
        host[len] = '\0';
        *port = atoi(colon + 1);
    } else if (slash) {
        /* No port, has path: "host/path" */
        int len = (int)(slash - p);
        if (len >= host_size) len = host_size - 1;
        strncpy(host, p, (size_t)len);
        host[len] = '\0';
    } else {
        /* No port, no path: "host" */
        strncpy(host, p, (size_t)(host_size - 1));
        host[host_size - 1] = '\0';
    }

    /* Copy the path (everything from '/' onwards, or "/" if absent) */
    if (slash) {
        strncpy(path, slash, (size_t)(path_size - 1));
        path[path_size - 1] = '\0';
    } else {
        strncpy(path, "/", (size_t)(path_size - 1));
    }

    return 1;
}

/* ==========================================================================
 * Individual check functions
 * Each returns 1 (healthy) or 0 (unhealthy).
 * ==========================================================================
 */

/*
 * check_tcp — verify a TCP port is accepting connections
 *
 * Simply tries to connect. If it succeeds, the service is up.
 * We immediately close the connection — we're just checking reachability.
 *
 * This is the lightest possible probe. A database or API that is fully
 * dead (process crashed, port closed) will fail this check quickly.
 * A service that is running but deadlocked may still pass — that's what
 * HTTP health checks are for.
 */
static int check_tcp(const HealthCheckConfig *hc) {
    char host[256] = "";
    int  port      = 0;

    if (!parse_host_port(hc->target, host, sizeof(host), &port)) {
        return 0;  /* misconfigured target */
    }

    int fd = platform_tcp_connect(host, port, hc->timeout_s * 1000);
    if (fd < 0) return 0;  /* connection refused or timed out */

    platform_socket_close(fd);
    return 1;  /* connected successfully */
}

/*
 * check_http — send an HTTP GET request and verify a 2xx response
 *
 * We use HTTP/1.0 (not 1.1) so the server closes the connection after the
 * response — we don't need to parse Content-Length or handle chunked encoding.
 *
 * A 2xx status means the server is up AND the endpoint returned success.
 * 5xx means the server is up but broken — we treat that as unhealthy.
 * 4xx is debatable; we treat it as unhealthy since the endpoint isn't working.
 */
static int check_http(const HealthCheckConfig *hc) {
    char host[256] = "";
    char path[512] = "/";
    int  port      = 80;

    if (!parse_http_url(hc->target, host, sizeof(host), &port, path, sizeof(path))) {
        return 0;
    }

    int fd = platform_tcp_connect(host, port, hc->timeout_s * 1000);
    if (fd < 0) return 0;

    /*
     * Build the HTTP/1.0 request.
     * HTTP/1.0 is intentionally simpler than 1.1:
     *   - The server closes the connection after the response (no keep-alive)
     *   - No Transfer-Encoding: chunked
     *   - No need to parse Content-Length to know when the body ends
     */
    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host);

    if (platform_tcp_send(fd, req, req_len) < 0) {
        platform_socket_close(fd);
        return 0;
    }

    /*
     * Read the response. We only need the status line, which is always
     * the first line: "HTTP/1.x NNN reason\r\n"
     * We read up to 256 bytes — more than enough for the status line.
     */
    char resp[256];
    memset(resp, 0, sizeof(resp));
    int n = platform_tcp_recv(fd, resp, sizeof(resp) - 1);
    platform_socket_close(fd);

    if (n <= 0) return 0;

    /*
     * Parse the status code.
     * sscanf with "HTTP/%*s %d" matches:
     *   "HTTP/" — literal prefix
     *   "%*s"   — the version ("1.0" or "1.1"), discarded (the * suppresses storing)
     *   " %d"   — a space then the 3-digit status code
     */
    int status_code = 0;
    sscanf(resp, "HTTP/%*s %d", &status_code);

    /* 2xx = success */
    return (status_code >= 200 && status_code < 300) ? 1 : 0;
}

/*
 * check_command — run a command and treat exit code 0 as healthy
 *
 * Spawns the command, waits up to timeout_s seconds for it to exit,
 * then checks the exit code. A process that hangs is killed and treated
 * as unhealthy.
 *
 * Example use: a custom script that checks your database schema version,
 * or pings an internal endpoint with custom logic.
 */
static int check_command(const HealthCheckConfig *hc) {
    /*
     * We reuse platform_spawn, which is designed for long-running services.
     * For health check commands, we expect them to exit quickly.
     * We poll every 100ms rather than blocking — health threads can afford this.
     */
    SpawnResult r = platform_spawn(hc->target, "", NULL, NULL, 0, NULL, NULL);
    if (!r.success) return 0;

    int64_t deadline = platform_now_ms() + (int64_t)hc->timeout_s * 1000;

    while (platform_now_ms() < deadline) {
        ProcessStatus st = platform_check_process(r.pid, r.handle);
        if (st.exited) {
            platform_close_handle(r.handle);
            return st.exit_code == 0 ? 1 : 0;
        }
        platform_sleep_ms(100);
    }

    /* Timed out — kill it and report unhealthy */
    platform_kill(r.pid, r.handle);
    platform_close_handle(r.handle);
    return 0;
}

/* ==========================================================================
 * Health check thread
 *
 * One of these runs per service with a configured health check.
 * It loops until ctx->running becomes 0, sleeping in short increments
 * so it can respond to shutdown quickly.
 * ==========================================================================
 */

static void *health_thread_fn(void *arg) {
    HealthThreadArg *a   = (HealthThreadArg *)arg;
    SupervisorContext *ctx = a->ctx;
    int idx               = a->service_index;
    free(arg);  /* free the heap-allocated arg — we've copied what we need */

    Service           *svc = &ctx->services[idx];
    HealthCheckConfig *hc  = &svc->cfg.health;

    LOG_INFO(svc->cfg.name, "Health check thread started (%s, interval %ds)",
             hc->type == HEALTH_TCP     ? "tcp"     :
             hc->type == HEALTH_HTTP    ? "http"    :
             hc->type == HEALTH_COMMAND ? "command" : "?",
             hc->interval_s);

    while (ctx->running) {
        /*
         * Sleep for interval_s, but in 100ms increments.
         *
         * Why not just sleep(interval_s)?
         * Because ctx->running might become 0 at any moment (Ctrl+C, stop command).
         * If we slept for 10 full seconds, shutdown would stall for up to 10 seconds
         * waiting for this thread to wake up and notice ctx->running == 0.
         *
         * Sleeping in 100ms chunks means we respond to shutdown within 100ms.
         */
        for (int i = 0; i < hc->interval_s * 10 && ctx->running; i++) {
            platform_sleep_ms(100);
        }

        if (!ctx->running) break;

        /* Perform the probe */
        int healthy;
        switch (hc->type) {
            case HEALTH_TCP:
                healthy = check_tcp(hc);
                break;
            case HEALTH_HTTP:
                healthy = check_http(hc);
                break;
            case HEALTH_COMMAND:
                healthy = check_command(hc);
                break;
            default:
                healthy = 1;  /* unknown type — don't penalize the service */
                break;
        }

        /* Push the result into the shared queue for the main thread to consume */
        HealthResult result;
        strncpy(result.service_name, svc->cfg.name,
                sizeof(result.service_name) - 1);
        result.service_name[sizeof(result.service_name) - 1] = '\0';
        result.healthy      = healthy;
        result.timestamp_ms = platform_now_ms();

        health_queue_push(&ctx->health_queue, &result);
    }

    LOG_INFO(svc->cfg.name, "Health check thread exiting");
    return NULL;
}

/* ==========================================================================
 * Public API
 * ==========================================================================
 */

void health_start_threads(SupervisorContext *ctx) {
    ctx->health_thread_count = 0;

    for (int i = 0; i < ctx->service_count; i++) {
        if (!ctx->services[i].cfg.has_health_check) continue;

        /*
         * Allocate the thread argument on the heap.
         * The thread will free() it at the start of health_thread_fn.
         */
        HealthThreadArg *arg = malloc(sizeof(HealthThreadArg));
        if (!arg) {
            LOG_WARN(ctx->services[i].cfg.name,
                     "Failed to allocate health thread arg — skipping");
            continue;
        }
        arg->ctx           = ctx;
        arg->service_index = i;

        /*
         * pthread_create(thread, attr, fn, arg):
         *   thread — pthread_t handle, filled in by the OS
         *   attr   — NULL = default attributes (joinable, normal priority)
         *   fn     — the function to run in the new thread
         *   arg    — passed as the void* parameter to fn
         *
         * After this call returns, health_thread_fn is running concurrently.
         */
        int rc = pthread_create(
            &ctx->health_threads[ctx->health_thread_count],
            NULL,
            health_thread_fn,
            arg
        );

        if (rc != 0) {
            LOG_WARN(ctx->services[i].cfg.name,
                     "pthread_create failed (rc=%d) — health checks disabled", rc);
            free(arg);
            continue;
        }

        ctx->health_thread_count++;
        LOG_INFO(ctx->services[i].cfg.name, "Health check thread spawned");
    }
}

void health_stop_threads(SupervisorContext *ctx) {
    if (ctx->health_thread_count == 0) return;

    LOG_INFO("guardian", "Waiting for %d health thread(s) to exit...",
             ctx->health_thread_count);

    for (int i = 0; i < ctx->health_thread_count; i++) {
        /*
         * pthread_join(thread, retval) — wait for a thread to finish.
         * Blocks until the thread returns from health_thread_fn.
         * Since ctx->running is already 0, threads will exit within 100ms.
         *
         * 'retval' receives the thread's return value (our fn returns NULL).
         * We pass NULL since we don't need it.
         *
         * Why join instead of just letting threads die?
         * Detached threads that outlive the process can corrupt shared state.
         * Joining guarantees the thread has finished using ctx before we free it.
         */
        pthread_join(ctx->health_threads[i], NULL);
    }

    /* Destroy the mutex — releases OS resources */
    pthread_mutex_destroy(&ctx->health_queue.lock);

    LOG_INFO("guardian", "All health threads stopped");
}
