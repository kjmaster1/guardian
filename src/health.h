/*
 * health.h — Health check subsystem
 *
 * Guardian actively probes each service according to its [health_check]
 * configuration. Probes run on dedicated background threads — one per service
 * with a health check — so that blocking TCP or HTTP calls never stall the
 * main event loop.
 *
 * Results flow through a circular buffer (HealthResultQueue) from the health
 * threads to the main thread. The main thread drains the queue every 100ms,
 * increments the consecutive_failures counter, and restarts the service when
 * the unhealthy_threshold is reached.
 *
 * Public API (called from supervisor.c):
 *   health_queue_init    — initialize the queue and its mutex
 *   health_start_threads — spawn one thread per service with a health check
 *   health_stop_threads  — wait for all health threads to exit (called at shutdown)
 *
 * Internal (called only within health.c):
 *   health_queue_push    — called by health threads to record a result
 *   health_queue_pop     — called by the main thread to consume results
 */

#pragma once

#include "service.h"

/* ==========================================================================
 * Queue operations
 * ==========================================================================
 */

/*
 * health_queue_init — set up the queue before any threads use it
 *
 * Zeroes the queue and initializes the pthread mutex.
 * MUST be called before health_start_threads().
 */
void health_queue_init(HealthResultQueue *q);

/*
 * health_queue_push — record one health probe result (thread-safe)
 *
 * Called from health check threads. If the queue is full (all 128 slots
 * occupied), the oldest entry is silently dropped — the main thread can't
 * keep up, and losing one result is better than blocking the health thread.
 */
void health_queue_push(HealthResultQueue *q, const HealthResult *r);

/*
 * health_queue_pop — consume the oldest result (thread-safe)
 *
 * Called from the main event loop. Returns 1 and fills *out if an entry was
 * available; returns 0 if the queue was empty.
 */
int health_queue_pop(HealthResultQueue *q, HealthResult *out);

/* ==========================================================================
 * Thread management
 * ==========================================================================
 */

/*
 * health_start_threads — launch one health check thread per configured service
 *
 * Iterates ctx->services and, for each service with has_health_check == 1,
 * spawns a pthread that loops: sleep(interval_s) → probe → push result.
 *
 * The number of started threads is stored in ctx->health_thread_count.
 * Threads access ctx->running to know when to exit; they also write to
 * ctx->health_queue. Both fields must be initialized before calling this.
 */
void health_start_threads(SupervisorContext *ctx);

/*
 * health_stop_threads — join all health threads (blocks until they exit)
 *
 * Threads check ctx->running every 100ms. Since the caller has already
 * set ctx->running = 0, threads will exit within 100ms of this call.
 * pthread_join() then waits for each one to finish cleanly.
 *
 * Call this after stopping services, before the process exits.
 */
void health_stop_threads(SupervisorContext *ctx);
