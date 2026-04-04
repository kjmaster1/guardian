/*
 * service.c — Service lifecycle management
 *
 * This module owns the functions that start, stop, and inspect individual
 * services. It is the bridge between:
 *   - The config data (ServiceConfig — what we want to run)
 *   - The platform layer (platform_spawn — how to actually run it)
 *   - The runtime state (ServiceRuntime — what's happening right now)
 *
 * Key concept: separation of concerns.
 * service.c doesn't know HOW processes are spawned (that's platform.h's job).
 * It only knows WHEN to spawn them and how to update the state machine.
 */

#include "service.h"
#include "logger.h"
#include "platform/platform.h"

#include <string.h>   /* memset */
#include <stdio.h>    /* fprintf */

/* ==========================================================================
 * service_state_name — human-readable state label
 * ==========================================================================
 */

const char *service_state_name(ServiceState state) {
    switch (state) {
        case STATE_STOPPED:  return "STOPPED";
        case STATE_STARTING: return "STARTING";
        case STATE_RUNNING:  return "RUNNING";
        case STATE_STOPPING: return "STOPPING";
        case STATE_BACKOFF:  return "BACKOFF";
        case STATE_FAILED:   return "FAILED";
        default:             return "UNKNOWN";
    }
}

/* ==========================================================================
 * service_compute_backoff_ms
 *
 * Exponential backoff: delay doubles on each retry, capped at max.
 *
 * Why integer math only?
 * Floating point (double/float) is non-deterministic across platforms —
 * the same calculation can give slightly different results on different
 * CPUs due to rounding. For timing logic, we want exact, reproducible
 * integer arithmetic.
 *
 * Why does this matter? If two guardian instances are computing restart
 * times for the same service, we want them to agree exactly.
 * ==========================================================================
 */

int64_t service_compute_backoff_ms(const ServiceConfig *cfg, int retry_count) {
    int64_t delay = (int64_t)cfg->backoff_base_ms;

    /*
     * Multiply by 2 for each retry. Stop early if we hit the cap.
     *
     * Why not use pow(2, retry_count)?
     * 1. <math.h> introduces floating point
     * 2. This loop is clearer about what's happening
     * 3. It naturally caps at backoff_max_ms without a separate min() call
     */
    for (int i = 0; i < retry_count; i++) {
        delay *= 2;
        if (delay >= (int64_t)cfg->backoff_max_ms) {
            delay = (int64_t)cfg->backoff_max_ms;
            break;  /* no point continuing — it's already at the cap */
        }
    }

    /* Return absolute time: "when to attempt the next restart" */
    return platform_now_ms() + delay;
}

/* ==========================================================================
 * service_start — spawn the process for one service
 * ==========================================================================
 */

int service_start(SupervisorContext *ctx, int index) {
    /*
     * We take a pointer to the specific Service in the array.
     * This avoids copying the struct and lets us modify the original directly.
     *
     * &ctx->services[index] is the address of the element at position 'index'
     * in the services array. The arrow operator (->) dereferences the pointer
     * and accesses a field: ctx->services is the same as (*ctx).services.
     */
    Service *svc = &ctx->services[index];

    LOG_INFO(svc->cfg.name, "Starting: %s %s",
             svc->cfg.command,
             svc->cfg.args[0] ? svc->cfg.args : "");

    /*
     * Build the env_vars pointer array for platform_spawn.
     *
     * platform_spawn() wants: const char *env_vars[]
     * ServiceConfig stores:   char env_vars[32][256]
     *
     * We build a temporary array of pointers to each string.
     * This is a common C pattern: convert a 2D array into an array of pointers.
     *
     * Memory layout of env_vars[32][256]:
     *   [  256 bytes  ][  256 bytes  ][ ... x32 total ]
     *   env_vars[0]    env_vars[1]
     *
     * env_ptrs[i] = &env_vars[i][0] = pointer to the start of string i
     */
    const char *env_ptrs[GUARDIAN_MAX_ENV_VARS];
    for (int i = 0; i < svc->cfg.env_count; i++) {
        env_ptrs[i] = svc->cfg.env_vars[i];
    }

    SpawnResult spawn = platform_spawn(
        svc->cfg.command,
        svc->cfg.args,
        svc->cfg.working_dir[0] ? svc->cfg.working_dir : NULL,
        svc->cfg.env_count > 0 ? env_ptrs : NULL,
        svc->cfg.env_count,
        svc->cfg.stdout_log[0] ? svc->cfg.stdout_log : NULL,
        svc->cfg.stderr_log[0] ? svc->cfg.stderr_log : NULL
    );

    if (!spawn.success) {
        LOG_ERROR(svc->cfg.name, "Failed to start: %s", spawn.error_msg);
        /*
         * If we can't even spawn the process, treat it like a crash —
         * go to BACKOFF so guardian will try again after a delay.
         * Don't give up immediately on a spawn failure; the executable
         * might not exist YET (e.g., built by another process).
         */
        svc->rt.state             = STATE_BACKOFF;
        svc->rt.retry_count++;
        svc->rt.next_restart_ms   = service_compute_backoff_ms(&svc->cfg,
                                                                svc->rt.retry_count);
        return 0;
    }

    /*
     * Spawn succeeded. Update the runtime state.
     *
     * We go to STATE_STARTING (not STATE_RUNNING) because we haven't
     * confirmed the process is healthy yet. In Phase 5, health checks
     * will transition STARTING -> RUNNING. For Phase 2, the supervisor
     * will promote to RUNNING after one successful check that the PID
     * is still alive.
     */
    svc->rt.pid            = spawn.pid;
    svc->rt.process_handle = spawn.handle;
    svc->rt.state          = STATE_STARTING;
    svc->rt.started_at_ms  = platform_now_ms();

    LOG_INFO(svc->cfg.name, "Started with PID %lu",
             (unsigned long)spawn.pid);

    return 1;
}

/* ==========================================================================
 * service_stop — gracefully stop a running service
 * ==========================================================================
 */

void service_stop(SupervisorContext *ctx, int index) {
    Service *svc = &ctx->services[index];

    if (svc->rt.state == STATE_STOPPED || svc->rt.state == STATE_FAILED) {
        return;  /* nothing to stop */
    }

    LOG_INFO(svc->cfg.name, "Stopping (PID %lu)", (unsigned long)svc->rt.pid);

    platform_terminate(svc->rt.pid, svc->rt.process_handle);
    svc->rt.state = STATE_STOPPING;

    /*
     * We don't wait here for the process to actually exit.
     * The supervisor event loop will call platform_check_process() each tick
     * and detect when the process has exited, then transition to STATE_STOPPED.
     *
     * This non-blocking approach is important: if we blocked here waiting for
     * one process, all other services would be frozen during that wait.
     */
}
