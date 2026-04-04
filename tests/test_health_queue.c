/*
 * test_health_queue.c — Unit tests for the HealthResultQueue circular buffer
 *
 * The health check subsystem uses a fixed-size circular buffer (the
 * HealthResultQueue) to pass probe results from health threads to the main
 * event loop. The queue has three operations:
 *
 *   health_queue_init() — zero the buffer and initialise the mutex
 *   health_queue_push() — write one result (producer side, thread-safe)
 *   health_queue_pop()  — read one result  (consumer side, thread-safe)
 *
 * These tests run single-threaded — they verify the queue's correctness in
 * the absence of concurrency. Thread-safety relies on the pthread_mutex_t,
 * which is separately guaranteed by the pthread library. What we CAN verify
 * unit-test style:
 *
 *   - Empty queue behaviour (pop returns 0, count is 0)
 *   - Items survive a push/pop round-trip without corruption
 *   - Items are delivered in FIFO order
 *   - count tracks push/pop correctly
 *   - Queue refuses to exceed GUARDIAN_HEALTH_QUEUE_CAP (overflow protection)
 *   - The circular wrap-around logic is correct (head/tail modulo arithmetic)
 *
 * Key C concept: mutex lifecycle in tests.
 * pthread_mutex_init() is called by health_queue_init(). We MUST call
 * pthread_mutex_destroy() when we're done — it releases OS resources.
 * Leaking a mutex in tests causes no visible failure on most platforms, but
 * it's incorrect and will surface as an error under tools like valgrind or
 * ThreadSanitizer. Always pair init with destroy.
 */

#include "test_framework.h"
#include "../src/health.h"    /* health_queue_init/push/pop, HealthResultQueue */
#include "../src/service.h"   /* HealthResult, GUARDIAN_HEALTH_QUEUE_CAP       */

#include <string.h>   /* memset, strcmp, strncpy */
#include <stdlib.h>   /* EXIT_SUCCESS, EXIT_FAILURE */
#include <stdio.h>    /* printf */
#include <pthread.h>  /* pthread_mutex_destroy */

/* ==========================================================================
 * Helpers
 * ==========================================================================
 */

/* Build a HealthResult with a given service name and healthy flag.
 * timestamp_ms is set to 0 — we don't test time here. */
static HealthResult make_result(const char *name, int healthy) {
    HealthResult r;
    memset(&r, 0, sizeof(r));
    strncpy(r.service_name, name, sizeof(r.service_name) - 1);
    r.service_name[sizeof(r.service_name) - 1] = '\0';
    r.healthy      = healthy;
    r.timestamp_ms = 0;
    return r;
}

/* ==========================================================================
 * Tests
 * ==========================================================================
 */

static void test_queue_starts_empty(void) {
    TEST("queue is empty after init: count == 0");
    HealthResultQueue q;
    health_queue_init(&q);

    ASSERT_INT_EQ(0, q.count);

    pthread_mutex_destroy(&q.lock);
}

static void test_pop_on_empty_returns_zero(void) {
    TEST("popping from an empty queue returns 0");
    HealthResultQueue q;
    health_queue_init(&q);

    HealthResult out;
    ASSERT_INT_EQ(0, health_queue_pop(&q, &out));

    pthread_mutex_destroy(&q.lock);
}

static void test_pop_on_empty_does_not_corrupt(void) {
    TEST("repeated pops on empty queue leave count at 0");
    HealthResultQueue q;
    health_queue_init(&q);

    HealthResult out;
    health_queue_pop(&q, &out);
    health_queue_pop(&q, &out);
    health_queue_pop(&q, &out);

    ASSERT_INT_EQ(0, q.count);

    pthread_mutex_destroy(&q.lock);
}

static void test_push_pop_round_trip(void) {
    TEST("a pushed item is recovered intact by pop");
    HealthResultQueue q;
    health_queue_init(&q);

    HealthResult r = make_result("my-service", 1);
    r.timestamp_ms = 99999;
    health_queue_push(&q, &r);

    HealthResult out;
    int got = health_queue_pop(&q, &out);

    ASSERT_INT_EQ(1, got);
    ASSERT_STR_EQ("my-service", out.service_name);

    TEST("pushed healthy=1 is recovered as healthy=1");
    ASSERT_INT_EQ(1, out.healthy);

    TEST("pushed timestamp_ms is recovered unchanged");
    ASSERT_INT_EQ((int)99999, (int)out.timestamp_ms);

    pthread_mutex_destroy(&q.lock);
}

static void test_push_pop_unhealthy(void) {
    TEST("pushed healthy=0 (unhealthy) is recovered as healthy=0");
    HealthResultQueue q;
    health_queue_init(&q);

    HealthResult r = make_result("failing-svc", 0);
    health_queue_push(&q, &r);

    HealthResult out;
    health_queue_pop(&q, &out);

    ASSERT_INT_EQ(0, out.healthy);

    pthread_mutex_destroy(&q.lock);
}

static void test_fifo_ordering(void) {
    TEST("queue delivers items in FIFO order: first pushed = first popped");
    HealthResultQueue q;
    health_queue_init(&q);

    HealthResult a = make_result("alpha",   1);
    HealthResult b = make_result("beta",    0);
    HealthResult c = make_result("gamma",   1);

    health_queue_push(&q, &a);
    health_queue_push(&q, &b);
    health_queue_push(&q, &c);

    HealthResult out;

    health_queue_pop(&q, &out);
    ASSERT_STR_EQ("alpha", out.service_name);

    TEST("second pop returns second-pushed item");
    health_queue_pop(&q, &out);
    ASSERT_STR_EQ("beta", out.service_name);

    TEST("third pop returns third-pushed item");
    health_queue_pop(&q, &out);
    ASSERT_STR_EQ("gamma", out.service_name);

    TEST("queue is empty after all items consumed");
    ASSERT_INT_EQ(0, q.count);

    pthread_mutex_destroy(&q.lock);
}

static void test_count_increments_on_push(void) {
    TEST("count increments correctly as items are pushed");
    HealthResultQueue q;
    health_queue_init(&q);

    HealthResult r = make_result("svc", 1);

    health_queue_push(&q, &r);
    ASSERT_INT_EQ(1, q.count);

    TEST("count is 2 after second push");
    health_queue_push(&q, &r);
    ASSERT_INT_EQ(2, q.count);

    TEST("count is 3 after third push");
    health_queue_push(&q, &r);
    ASSERT_INT_EQ(3, q.count);

    pthread_mutex_destroy(&q.lock);
}

static void test_count_decrements_on_pop(void) {
    TEST("count decrements correctly as items are popped");
    HealthResultQueue q;
    health_queue_init(&q);

    HealthResult r = make_result("svc", 1);
    HealthResult out;

    health_queue_push(&q, &r);
    health_queue_push(&q, &r);
    health_queue_push(&q, &r);

    health_queue_pop(&q, &out);
    ASSERT_INT_EQ(2, q.count);

    TEST("count is 1 after second pop");
    health_queue_pop(&q, &out);
    ASSERT_INT_EQ(1, q.count);

    TEST("count is 0 after all items popped");
    health_queue_pop(&q, &out);
    ASSERT_INT_EQ(0, q.count);

    pthread_mutex_destroy(&q.lock);
}

static void test_fill_to_capacity(void) {
    TEST("queue accepts exactly GUARDIAN_HEALTH_QUEUE_CAP items");
    HealthResultQueue q;
    health_queue_init(&q);

    HealthResult r = make_result("svc", 1);

    for (int i = 0; i < GUARDIAN_HEALTH_QUEUE_CAP; i++) {
        health_queue_push(&q, &r);
    }

    ASSERT_INT_EQ(GUARDIAN_HEALTH_QUEUE_CAP, q.count);

    pthread_mutex_destroy(&q.lock);
}

static void test_overflow_drops_silently(void) {
    TEST("pushing past capacity: count stays at GUARDIAN_HEALTH_QUEUE_CAP");
    HealthResultQueue q;
    health_queue_init(&q);

    HealthResult r = make_result("svc", 1);

    /* Fill to capacity */
    for (int i = 0; i < GUARDIAN_HEALTH_QUEUE_CAP; i++) {
        health_queue_push(&q, &r);
    }

    /* Push one more — should be silently dropped */
    health_queue_push(&q, &r);

    ASSERT_INT_EQ(GUARDIAN_HEALTH_QUEUE_CAP, q.count);

    TEST("existing items are not corrupted by overflow push");
    HealthResult out;
    int ok = health_queue_pop(&q, &out);
    ASSERT_INT_EQ(1, ok);

    pthread_mutex_destroy(&q.lock);
}

static void test_wrap_around(void) {
    /*
     * This test verifies the modulo (%) arithmetic that makes the buffer circular.
     *
     * The idea: fill HALF the queue, then drain it entirely (so head advances
     * to the midpoint). Now fill the queue again — tail will lap around past
     * the array end. Then drain again and verify every item is intact.
     *
     * Without correct % wrapping, tail would go out of bounds (buffer overflow)
     * or items would be written to wrong slots.
     */
    TEST("circular wrap: fill half, drain, fill fully, drain — all items intact");
    HealthResultQueue q;
    health_queue_init(&q);

    HealthResult r = make_result("wrap-svc", 1);
    HealthResult out;

    int half = GUARDIAN_HEALTH_QUEUE_CAP / 2;

    /* Phase 1: fill half, drain half — head now sits at offset 'half' */
    for (int i = 0; i < half; i++) health_queue_push(&q, &r);
    for (int i = 0; i < half; i++) health_queue_pop(&q, &out);
    ASSERT_INT_EQ(0, q.count);

    /* Phase 2: fill the entire capacity — tail wraps around the array end */
    for (int i = 0; i < GUARDIAN_HEALTH_QUEUE_CAP; i++) health_queue_push(&q, &r);
    ASSERT_INT_EQ(GUARDIAN_HEALTH_QUEUE_CAP, q.count);

    /* Phase 3: drain and verify each item */
    TEST("after wrap-around, all items have the correct service name");
    int all_ok = 1;
    for (int i = 0; i < GUARDIAN_HEALTH_QUEUE_CAP; i++) {
        health_queue_pop(&q, &out);
        if (strcmp(out.service_name, "wrap-svc") != 0 || out.healthy != 1) {
            all_ok = 0;
        }
    }
    ASSERT_INT_EQ(1, all_ok);

    TEST("queue is empty after draining all wrapped items");
    ASSERT_INT_EQ(0, q.count);

    pthread_mutex_destroy(&q.lock);
}

/* ==========================================================================
 * Test runner
 * ==========================================================================
 */

int main(void) {
    printf("\n  guardian — health queue tests\n");
    printf("  ================================\n\n");

    test_queue_starts_empty();
    test_pop_on_empty_returns_zero();
    test_pop_on_empty_does_not_corrupt();
    test_push_pop_round_trip();
    test_push_pop_unhealthy();
    test_fifo_ordering();
    test_count_increments_on_push();
    test_count_decrements_on_pop();
    test_fill_to_capacity();
    test_overflow_drops_silently();
    test_wrap_around();

    SUMMARY();
    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
