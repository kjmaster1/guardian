/*
 * service.h — Central data model for guardian
 *
 * This is the most important header in the project. Every other module
 * includes this file. It defines what a "service" IS — both its static
 * configuration (read from the INI file, never changes) and its live
 * runtime state (changes every tick of the event loop).
 *
 * Rule: if you change a struct here, every file that uses it is affected.
 * Get these definitions right before writing any other code.
 */

#pragma once

#include <stdint.h>             /* int64_t */
#include <pthread.h>            /* pthread_t, pthread_mutex_t — thread handles */
#include "platform/platform.h"  /* PlatformPid, PlatformHandle — real OS types */

/* ==========================================================================
 * Constants
 * ==========================================================================
 */

#define GUARDIAN_MAX_SERVICES      64
#define GUARDIAN_MAX_NAME          64
#define GUARDIAN_MAX_CMD          512
#define GUARDIAN_MAX_ARGS         256
#define GUARDIAN_MAX_PATH         512
#define GUARDIAN_MAX_ENV_VARS      32
#define GUARDIAN_MAX_ENV_STR      256
#define GUARDIAN_HEALTH_QUEUE_CAP 128

/* How long a service gets to exit after a terminate request before it is
 * force-killed. Applies both at runtime (health-triggered restarts) and
 * during guardian shutdown. */
#define GUARDIAN_STOP_TIMEOUT_MS 5000

/* ==========================================================================
 * Enumerations
 * ==========================================================================
 */

typedef enum {
    RESTART_NEVER      = 0,
    RESTART_ON_FAILURE = 1,
    RESTART_ALWAYS     = 2
} RestartPolicy;

typedef enum {
    HEALTH_NONE    = 0,
    HEALTH_TCP     = 1,
    HEALTH_HTTP    = 2,
    HEALTH_COMMAND = 3
} HealthCheckType;

/*
 * ServiceState — the lifecycle state machine for one service
 *
 * Every service is always in exactly one of these states.
 * The supervisor's event loop reads and transitions these states each tick.
 *
 * Valid transitions:
 *   STOPPED  -> STARTING  (autostart or manual start command)
 *   STARTING -> RUNNING   (process confirmed alive)
 *   STARTING -> BACKOFF   (process died before becoming healthy)
 *   RUNNING  -> STOPPING  (stop command received)
 *   RUNNING  -> BACKOFF   (process crashed)
 *   STOPPING -> STOPPED   (process exited after terminate signal)
 *   BACKOFF  -> STARTING  (backoff delay elapsed, attempt restart)
 *   BACKOFF  -> FAILED    (max_retries exhausted)
 */
typedef enum {
    STATE_STOPPED  = 0,
    STATE_STARTING = 1,
    STATE_RUNNING  = 2,
    STATE_STOPPING = 3,
    STATE_BACKOFF  = 4,
    STATE_FAILED   = 5
} ServiceState;

/* ==========================================================================
 * Structs
 * ==========================================================================
 */

typedef struct {
    HealthCheckType type;
    char            target[GUARDIAN_MAX_PATH];
    int             interval_s;
    int             timeout_s;
    int             unhealthy_threshold;
} HealthCheckConfig;

typedef struct {
    char             name[GUARDIAN_MAX_NAME];
    char             command[GUARDIAN_MAX_CMD];
    char             args[GUARDIAN_MAX_ARGS];
    char             working_dir[GUARDIAN_MAX_PATH];
    char             env_vars[GUARDIAN_MAX_ENV_VARS][GUARDIAN_MAX_ENV_STR];
    int              env_count;
    char             stdout_log[GUARDIAN_MAX_PATH];
    char             stderr_log[GUARDIAN_MAX_PATH];
    int              autostart;
    RestartPolicy    restart_policy;
    int              max_retries;
    int              backoff_base_ms;
    int              backoff_max_ms;
    int              stable_after_s;     /* uptime that resets retry_count; 0 = never */
    int              has_health_check;
    HealthCheckConfig health;
} ServiceConfig;

typedef struct {
    PlatformPid    pid;
    PlatformHandle process_handle;
    ServiceState   state;
    int            retry_count;            /* restart budget used (vs max_retries) */
    int            total_restarts;         /* lifetime count, shown by 'status' */
    int            consecutive_failures;   /* health check fails in a row */
    int64_t        next_restart_ms;
    int64_t        started_at_ms;
    int64_t        last_health_check_ms;
    int64_t        stop_requested_ms;      /* when service_stop() was called */
    int            last_exit_code;
    int            killed_by_signal;
    int            health_triggered_stop;  /* 1 = this stop was from a health check */
} ServiceRuntime;

typedef struct {
    ServiceConfig   cfg;
    ServiceRuntime  rt;
} Service;

/* HealthResult and HealthResultQueue must be defined before SupervisorContext,
 * which embeds a HealthResultQueue by value. */

typedef struct {
    char    service_name[GUARDIAN_MAX_NAME];
    int     healthy;
    int64_t timestamp_ms;
} HealthResult;

/*
 * HealthResultQueue — circular buffer carrying health probe results
 *
 * Health threads WRITE to this queue (push); the main event loop READS
 * from it (pop) every 100ms tick. One mutex protects both operations.
 *
 * Circular buffer recap:
 *   items[] is the storage array.
 *   tail = next write position (producer advances this).
 *   head = next read position  (consumer advances this).
 *   count = number of live entries.
 *   Both head and tail wrap at GUARDIAN_HEALTH_QUEUE_CAP using %.
 */
typedef struct {
    HealthResult    items[GUARDIAN_HEALTH_QUEUE_CAP];
    int             head;          /* next slot to read  */
    int             tail;          /* next slot to write */
    int             count;         /* entries currently in queue */
    pthread_mutex_t lock;          /* protects head, tail, count, items */
} HealthResultQueue;

typedef struct {
    Service      services[GUARDIAN_MAX_SERVICES];
    int          service_count;
    char         log_file[GUARDIAN_MAX_PATH];
    char         ipc_socket[GUARDIAN_MAX_PATH];

    /*
     * running — 1 while the supervisor should keep going, 0 to shut down.
     *
     * Written by: the signal handler (Ctrl+C / SIGTERM), the IPC 'stop'
     * command, and on Windows the console-control thread.
     * Read by:    the main loop AND every health thread.
     *
     * Why atomic_int and not 'volatile int'?
     * volatile only stops the compiler caching the value in a register.
     * It does NOT make concurrent access from several threads well-defined —
     * in C11 that is a data race, which is undefined behaviour (and
     * ThreadSanitizer reports it). atomic_int makes every read and write a
     * single indivisible operation that other threads are guaranteed to see.
     * A lock-free atomic is also allowed to be written from a signal handler.
     */
    atomic_int   running;

    /*
     * Health subsystem — owned by health.c, initialized by health_queue_init().
     *
     * health_queue: the circular buffer that health threads write to and the
     *   main event loop reads from. One entry per probe result.
     *
     * health_threads[]: one pthread per service that has a health check.
     *   Indexed by service index (not all slots are used).
     *
     * health_thread_count: how many threads were started (for joining).
     */
    HealthResultQueue health_queue;
    pthread_t         health_threads[GUARDIAN_MAX_SERVICES];
    int               health_thread_count;
} SupervisorContext;

/* ==========================================================================
 * Service lifecycle functions (implemented in service.c)
 * ==========================================================================
 */

/*
 * service_start — spawn the process for services[index] and update its state
 *
 * Calls platform_spawn(), stores the PID/handle in the runtime struct,
 * and transitions the state to STATE_STARTING.
 *
 * Returns 1 on success, 0 on failure.
 */
int service_start(SupervisorContext *ctx, int index);

/*
 * service_stop — send a terminate signal to services[index]
 *
 * Transitions the state to STATE_STOPPING.
 * The supervisor event loop will confirm the process actually exited.
 */
void service_stop(SupervisorContext *ctx, int index);

/*
 * service_compute_backoff_ms — calculate the next restart delay
 *
 * Returns the absolute timestamp (ms) when the next restart attempt
 * should happen, based on retry_count and the service's backoff config.
 *
 * The formula: delay = min(base * 2^retry, max)
 * e.g. base=1000, max=60000: 1s, 2s, 4s, 8s, 16s, 32s, 60s, 60s, ...
 */
int64_t service_compute_backoff_ms(const ServiceConfig *cfg, int retry_count);

/*
 * service_schedule_restart — count one restart attempt and pick the next state
 *
 * Every path that wants to restart a service (crash, health-check failure,
 * spawn failure) goes through here, so max_retries is enforced the same way
 * for all of them.
 *
 * Increments retry_count. If max_retries (> 0) is exceeded the service moves
 * to STATE_FAILED; otherwise to STATE_BACKOFF with next_restart_ms set.
 *
 * Returns 1 if a restart was scheduled, 0 if the service gave up.
 */
int service_schedule_restart(Service *svc);

/*
 * service_reset_retries_if_stable — forgive old crashes after stable uptime
 *
 * If the service is RUNNING, has used some of its restart budget, and has
 * been up for at least cfg.stable_after_s seconds, retry_count goes back to
 * 0. That restores the full max_retries budget and the shortest backoff
 * delay. total_restarts is left alone so 'status' still shows history.
 *
 * Returns 1 if the budget was reset, 0 otherwise.
 */
int service_reset_retries_if_stable(Service *svc, int64_t now_ms);

/*
 * service_state_name — return a human-readable name for a ServiceState
 *
 * Useful for log messages and status output.
 */
const char *service_state_name(ServiceState state);
