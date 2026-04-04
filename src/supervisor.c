/*
 * supervisor.c — The main event loop
 *
 * Key concepts demonstrated here:
 *   - Event loops: the core pattern of long-running programs
 *   - State machine transitions: reading and writing ServiceState
 *   - Non-blocking I/O: checking processes without waiting
 *   - Graceful shutdown: stopping all children before exiting
 */

#include "supervisor.h"
#include "service.h"
#include "health.h"
#include "ipc.h"
#include "logger.h"
#include "platform/platform.h"

#include <string.h>   /* memset, strcmp */

/* ==========================================================================
 * Internal helpers
 * ==========================================================================
 */

/*
 * handle_exited_service — decide what to do when a process exits
 *
 * This is called when platform_check_process() reports a service has exited.
 * The decision tree:
 *
 *   Was it STATE_STOPPING?
 *     → It exited because WE asked it to. Transition to STATE_STOPPED. Done.
 *
 *   Was the exit clean (exit code 0, not by signal)?
 *     → If restart = always:     → BACKOFF (restart it)
 *     → If restart = on_failure: → STATE_STOPPED (clean exit, leave it alone)
 *     → If restart = never:      → STATE_STOPPED
 *
 *   Was the exit a crash (non-zero exit or signal)?
 *     → If restart = never:      → STATE_STOPPED (user said never restart)
 *     → Otherwise:               → BACKOFF (we'll restart it)
 *       Have we exceeded max_retries?
 *         → STATE_FAILED (give up)
 */
static void handle_exited_service(SupervisorContext *ctx, int index,
                                  ProcessStatus status) {
    Service *svc = &ctx->services[index];

    /* Release the OS handle — always do this when a process exits */
    platform_close_handle(svc->rt.process_handle);
    svc->rt.process_handle = PLATFORM_INVALID_HANDLE;
    svc->rt.last_exit_code = status.exit_code;
    svc->rt.killed_by_signal = status.by_signal;

    /* Case 1: We asked it to stop */
    if (svc->rt.state == STATE_STOPPING) {
        svc->rt.pid = PLATFORM_INVALID_PID;

        if (svc->rt.health_triggered_stop) {
            /*
             * This stop was triggered by repeated health check failures,
             * not by a user 'stop' command. We want to restart it —
             * go to BACKOFF so the event loop will re-launch it after a delay.
             *
             * We increment retry_count so backoff delays accumulate if the
             * service keeps failing its health check on each restart.
             */
            svc->rt.health_triggered_stop = 0;
            svc->rt.retry_count++;
            svc->rt.next_restart_ms = service_compute_backoff_ms(
                &svc->cfg, svc->rt.retry_count);
            svc->rt.state = STATE_BACKOFF;

            int64_t delay_ms = svc->rt.next_restart_ms - platform_now_ms();
            LOG_INFO(svc->cfg.name,
                     "Stopped after health failure — will restart in %.1fs",
                     (double)delay_ms / 1000.0);
        } else {
            /* Normal intentional stop */
            LOG_INFO(svc->cfg.name, "Stopped (exit code %d)", status.exit_code);
            svc->rt.state = STATE_STOPPED;
        }
        return;
    }

    /* Case 2: Unexpected exit — was it clean or a crash? */
    int is_crash = status.by_signal || (status.exit_code != 0);

    if (is_crash) {
        LOG_WARN(svc->cfg.name, "Crashed (exit code %d, by signal: %s)",
                 status.exit_code, status.by_signal ? "yes" : "no");
    } else {
        LOG_INFO(svc->cfg.name, "Exited cleanly (exit code 0)");
    }

    svc->rt.pid = PLATFORM_INVALID_PID;

    /* Should we restart? */
    int should_restart = 0;
    if (svc->cfg.restart_policy == RESTART_ALWAYS) {
        should_restart = 1;
    } else if (svc->cfg.restart_policy == RESTART_ON_FAILURE && is_crash) {
        should_restart = 1;
    }

    if (!should_restart) {
        svc->rt.state = STATE_STOPPED;
        return;
    }

    /*
     * Check if we've exceeded max_retries.
     * max_retries == 0 means unlimited — we never give up.
     */
    svc->rt.retry_count++;

    if (svc->cfg.max_retries > 0 &&
        svc->rt.retry_count > svc->cfg.max_retries) {
        LOG_ERROR(svc->cfg.name,
                  "Giving up after %d restart attempts (max_retries = %d)",
                  svc->rt.retry_count - 1, svc->cfg.max_retries);
        svc->rt.state = STATE_FAILED;
        return;
    }

    /*
     * Go to BACKOFF. The event loop will check next_restart_ms each tick
     * and call service_start() when the delay has elapsed.
     */
    svc->rt.next_restart_ms = service_compute_backoff_ms(&svc->cfg,
                                                          svc->rt.retry_count);
    svc->rt.state = STATE_BACKOFF;

    int64_t delay_ms = svc->rt.next_restart_ms - platform_now_ms();
    LOG_INFO(svc->cfg.name,
             "Will restart in %.1fs (attempt %d%s)",
             (double)delay_ms / 1000.0,
             svc->rt.retry_count,
             svc->cfg.max_retries > 0 ? "" : ", unlimited retries");
}

/* ==========================================================================
 * supervisor_tick — one iteration of the event loop
 *
 * Called every 100ms. Checks every service and takes appropriate action.
 * ==========================================================================
 */
/*
 * drain_health_queue — process all pending health check results
 *
 * Runs at the start of each tick. For each result:
 *   - healthy:   reset consecutive_failures counter
 *   - unhealthy: increment counter; if it reaches the threshold, stop and
 *                restart the service (via health_triggered_stop flag)
 *
 * We only act on services in STATE_RUNNING — there's no point restarting
 * a service that's already stopping, in backoff, or stopped.
 */
static void drain_health_queue(SupervisorContext *ctx) {
    HealthResult hr;
    while (health_queue_pop(&ctx->health_queue, &hr)) {
        /* Find the service this result belongs to */
        for (int i = 0; i < ctx->service_count; i++) {
            Service *svc = &ctx->services[i];
            if (strcmp(svc->cfg.name, hr.service_name) != 0) continue;

            if (hr.healthy) {
                if (svc->rt.consecutive_failures > 0) {
                    LOG_INFO(svc->cfg.name,
                             "Health check passed (was failing for %d probe(s))",
                             svc->rt.consecutive_failures);
                    svc->rt.consecutive_failures = 0;
                }
            } else {
                svc->rt.consecutive_failures++;
                int threshold = svc->cfg.health.unhealthy_threshold;

                LOG_WARN(svc->cfg.name,
                         "Health check failed (%d/%d)",
                         svc->rt.consecutive_failures, threshold);

                if (svc->rt.state == STATE_RUNNING &&
                    svc->rt.consecutive_failures >= threshold) {
                    LOG_WARN(svc->cfg.name,
                             "Unhealthy threshold reached — stopping for restart");
                    svc->rt.consecutive_failures   = 0;
                    svc->rt.health_triggered_stop  = 1;
                    service_stop(ctx, i);
                }
            }
            break;
        }
    }
}

static void supervisor_tick(SupervisorContext *ctx) {
    int64_t now = platform_now_ms();

    /* Process any health check results that arrived since the last tick */
    drain_health_queue(ctx);

    for (int i = 0; i < ctx->service_count; i++) {
        Service *svc = &ctx->services[i];

        switch (svc->rt.state) {

            case STATE_STARTING:
            case STATE_RUNNING:
            case STATE_STOPPING: {
                /*
                 * The process should be running. Check if it still is.
                 *
                 * platform_check_process() is NON-BLOCKING — it returns
                 * immediately. status.exited == 0 means "still running."
                 * status.exited == 1 means "it has exited."
                 */
                ProcessStatus status = platform_check_process(
                    svc->rt.pid, svc->rt.process_handle);

                if (status.exited) {
                    handle_exited_service(ctx, i, status);
                } else if (svc->rt.state == STATE_STARTING) {
                    /*
                     * Still alive and in STARTING state.
                     * Phase 2: promote to RUNNING immediately (no health checks yet).
                     * Phase 5 will keep it in STARTING until a health check passes.
                     */
                    svc->rt.state = STATE_RUNNING;
                    LOG_INFO(svc->cfg.name, "Running (PID %lu)",
                             (unsigned long)svc->rt.pid);
                }
                break;
            }

            case STATE_BACKOFF: {
                /*
                 * Waiting before restarting. Check if the delay has elapsed.
                 *
                 * platform_now_ms() returns current time in milliseconds.
                 * next_restart_ms is the absolute timestamp we computed when
                 * the process crashed. When now >= next_restart_ms, it's time.
                 */
                if (now >= svc->rt.next_restart_ms) {
                    service_start(ctx, i);
                }
                break;
            }

            case STATE_STOPPED:
            case STATE_FAILED:
                /* Nothing to do — these services are not running */
                break;
        }
    }
}

/* ==========================================================================
 * supervisor_start_autostart_services
 *
 * Called once at startup. Starts all services with autostart = true.
 * ==========================================================================
 */
static void supervisor_start_autostart_services(SupervisorContext *ctx) {
    int started = 0;
    for (int i = 0; i < ctx->service_count; i++) {
        if (ctx->services[i].cfg.autostart) {
            service_start(ctx, i);
            started++;
        }
    }
    LOG_INFO("guardian", "Started %d autostart service(s)", started);
}

/* ==========================================================================
 * supervisor_stop_all — gracefully stop all running services
 *
 * Called during shutdown. Sends terminate signal to every running service,
 * then waits up to 5 seconds for each to exit cleanly.
 * ==========================================================================
 */
static void supervisor_stop_all(SupervisorContext *ctx) {
    LOG_INFO("guardian", "Shutting down — stopping all services...");

    /* First pass: send terminate signal to every running service */
    for (int i = 0; i < ctx->service_count; i++) {
        Service *svc = &ctx->services[i];
        if (svc->rt.state == STATE_RUNNING ||
            svc->rt.state == STATE_STARTING) {
            service_stop(ctx, i);
        }
    }

    /*
     * Second pass: wait up to 5 seconds for each service to exit.
     *
     * We poll in 100ms increments. This is a simple approach — a production
     * implementation would use WaitForMultipleObjects (Windows) or poll/select
     * (Linux) to wait on all processes simultaneously rather than sequentially.
     */
    int max_wait_ms = 5000;
    int waited_ms   = 0;

    while (waited_ms < max_wait_ms) {
        int still_stopping = 0;

        for (int i = 0; i < ctx->service_count; i++) {
            Service *svc = &ctx->services[i];
            if (svc->rt.state != STATE_STOPPING) continue;

            ProcessStatus st = platform_check_process(
                svc->rt.pid, svc->rt.process_handle);

            if (st.exited) {
                platform_close_handle(svc->rt.process_handle);
                svc->rt.process_handle = PLATFORM_INVALID_HANDLE;
                svc->rt.state          = STATE_STOPPED;
                svc->rt.pid            = PLATFORM_INVALID_PID;
                LOG_INFO(svc->cfg.name, "Stopped");
            } else {
                still_stopping++;
            }
        }

        if (still_stopping == 0) break;

        platform_sleep_ms(100);
        waited_ms += 100;
    }

    /* Final pass: force-kill anything still alive after timeout */
    for (int i = 0; i < ctx->service_count; i++) {
        Service *svc = &ctx->services[i];
        if (svc->rt.state == STATE_STOPPING) {
            LOG_WARN(svc->cfg.name,
                     "Did not stop within 5s — force killing");
            platform_kill(svc->rt.pid, svc->rt.process_handle);
            platform_close_handle(svc->rt.process_handle);
            svc->rt.state = STATE_STOPPED;
        }
    }
}

/* ==========================================================================
 * supervisor_run — the main entry point (called from cli.c)
 * ==========================================================================
 */

void supervisor_run(SupervisorContext *ctx) {
    LOG_INFO("guardian", "Guardian starting up");

    /*
     * Install signal handlers BEFORE starting services.
     * If a signal arrives between starting the first and second service,
     * we want to catch it and shut down cleanly.
     *
     * platform_install_signal_handlers() sets up the handler that writes
     * ctx->running = 0 when Ctrl+C or SIGTERM arrives.
     */
    platform_install_signal_handlers(&ctx->running);

    /* Open IPC server before starting services — stop/status work immediately */
    int ipc_fd = ipc_server_open(ctx);

    /* Initialize the health result queue before spawning health threads */
    health_queue_init(&ctx->health_queue);

    supervisor_start_autostart_services(ctx);

    /* Start health check threads AFTER services are launched, so the
     * first probe fires after interval_s (not before the process is even up) */
    health_start_threads(ctx);

    LOG_INFO("guardian", "Watching %d service(s). Press Ctrl+C to stop.",
             ctx->service_count);

    /*
     * THE MAIN EVENT LOOP
     *
     * Each iteration:
     *   1. Check every service (supervisor_tick)
     *   2. Check for IPC commands — stop/status (ipc_server_tick)
     *   3. Sleep 100ms
     *   4. Loop back unless ctx->running was set to 0
     */
    while (ctx->running) {
        supervisor_tick(ctx);
        ipc_server_tick(ctx, ipc_fd);
        platform_sleep_ms(100);
    }

    LOG_INFO("guardian", "Shutdown signal received");
    ipc_server_close(ipc_fd);

    /* Stop health threads before stopping services — threads may inspect
     * service state, so we want them quiet before we mutate it */
    health_stop_threads(ctx);

    supervisor_stop_all(ctx);
    LOG_INFO("guardian", "Guardian stopped cleanly");
}
