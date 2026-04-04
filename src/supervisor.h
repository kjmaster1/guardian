/*
 * supervisor.h — The main event loop for guardian
 *
 * The supervisor is the heart of guardian. It:
 *   1. Starts all autostart services on launch
 *   2. Watches them every 100ms
 *   3. Restarts crashed services according to their restart policy
 *   4. Shuts down cleanly when a signal arrives
 */

#pragma once

#include "service.h"  /* SupervisorContext */

/*
 * supervisor_run — start the event loop
 *
 * This function does not return until guardian is shut down (Ctrl+C or
 * a 'guardian stop' command sets ctx->running = 0).
 *
 * Before returning, it stops all running services and cleans up.
 */
void supervisor_run(SupervisorContext *ctx);
