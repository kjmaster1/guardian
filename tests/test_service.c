/*
 * test_service.c — Unit tests for service.c
 *
 * Covers:
 *   service_state_name()       — human-readable name for each ServiceState
 *   service_compute_backoff_ms() — exponential backoff delay calculation
 *
 * Why are these the right functions to unit test?
 * Both are pure logic functions: they take inputs, do math/lookup, and return
 * a result. No network, no processes, no global state. This makes them ideal
 * for isolated unit testing — fast, deterministic, and simple.
 *
 * What we can NOT unit test here without running a real process:
 *   service_start() — calls platform_spawn() (actually launches a process)
 *   service_stop()  — calls platform_terminate() (sends a signal)
 * Those are covered by integration tests (running guardian against a real
 * command and observing its behavior).
 *
 * Key C concept: building a minimal ServiceConfig for testing.
 * We use memset(&cfg, 0, sizeof(cfg)) to zero-initialise the struct (all
 * fields start at 0/NULL/false), then set only the fields we care about.
 * This avoids undefined-behaviour from uninitialised struct members.
 */

#include "test_framework.h"
#include "../src/service.h"
#include "../src/platform/platform.h"  /* platform_now_ms() */

#include <string.h>   /* memset */
#include <stdlib.h>   /* EXIT_SUCCESS, EXIT_FAILURE */
#include <stdio.h>    /* printf */

/* ==========================================================================
 * service_state_name() tests
 * ==========================================================================
 */

static void test_state_name_stopped(void) {
    TEST("STATE_STOPPED name is \"STOPPED\"");
    ASSERT_STR_EQ("STOPPED", service_state_name(STATE_STOPPED));
}

static void test_state_name_starting(void) {
    TEST("STATE_STARTING name is \"STARTING\"");
    ASSERT_STR_EQ("STARTING", service_state_name(STATE_STARTING));
}

static void test_state_name_running(void) {
    TEST("STATE_RUNNING name is \"RUNNING\"");
    ASSERT_STR_EQ("RUNNING", service_state_name(STATE_RUNNING));
}

static void test_state_name_stopping(void) {
    TEST("STATE_STOPPING name is \"STOPPING\"");
    ASSERT_STR_EQ("STOPPING", service_state_name(STATE_STOPPING));
}

static void test_state_name_backoff(void) {
    TEST("STATE_BACKOFF name is \"BACKOFF\"");
    ASSERT_STR_EQ("BACKOFF", service_state_name(STATE_BACKOFF));
}

static void test_state_name_failed(void) {
    TEST("STATE_FAILED name is \"FAILED\"");
    ASSERT_STR_EQ("FAILED", service_state_name(STATE_FAILED));
}

static void test_state_name_unknown(void) {
    TEST("unrecognised state value returns \"UNKNOWN\"");
    /* Cast an out-of-range integer to ServiceState.
     * C enums are just ints — any integer value is valid as a cast,
     * even if it doesn't correspond to a named enum constant.
     * Our switch statement must have a default: branch to handle this. */
    ASSERT_STR_EQ("UNKNOWN", service_state_name((ServiceState)999));
}

/* ==========================================================================
 * service_compute_backoff_ms() tests
 *
 * Strategy: sandwich the function call between two platform_now_ms() calls.
 * The function itself calls platform_now_ms() internally just before returning:
 *     return platform_now_ms() + delay;
 *
 * So if we record:
 *   before = platform_now_ms()    <-- just before we call the function
 *   result = service_compute_backoff_ms(...)
 *   after  = platform_now_ms()    <-- just after
 *
 * We know:  before <= internal_now_ms <= after
 * Therefore: before + delay <= result <= after + delay
 *
 * The range [before + delay, after + delay] is typically 0-1ms wide on
 * modern hardware (the function is just integer math), giving us a
 * deterministic, race-free assertion that works even under CI load.
 * ==========================================================================
 */

/* Helper: build a minimal ServiceConfig with the given backoff settings. */
static ServiceConfig make_cfg(int base_ms, int max_ms) {
    ServiceConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backoff_base_ms = base_ms;
    cfg.backoff_max_ms  = max_ms;
    return cfg;
}

static void test_backoff_retry_zero(void) {
    TEST("retry 0: delay equals base_ms (no doubling yet)");
    ServiceConfig cfg = make_cfg(1000, 60000);

    int64_t before = platform_now_ms();
    int64_t result = service_compute_backoff_ms(&cfg, 0);
    int64_t after  = platform_now_ms();

    /* result should be between (before + 1000) and (after + 1000) */
    ASSERT_IN_RANGE(result, before + 1000, after + 1000);
}

static void test_backoff_retry_one(void) {
    TEST("retry 1: delay is 2 * base_ms");
    ServiceConfig cfg = make_cfg(1000, 60000);

    int64_t before = platform_now_ms();
    int64_t result = service_compute_backoff_ms(&cfg, 1);
    int64_t after  = platform_now_ms();

    ASSERT_IN_RANGE(result, before + 2000, after + 2000);
}

static void test_backoff_retry_two(void) {
    TEST("retry 2: delay is 4 * base_ms");
    ServiceConfig cfg = make_cfg(1000, 60000);

    int64_t before = platform_now_ms();
    int64_t result = service_compute_backoff_ms(&cfg, 2);
    int64_t after  = platform_now_ms();

    ASSERT_IN_RANGE(result, before + 4000, after + 4000);
}

static void test_backoff_retry_three(void) {
    TEST("retry 3: delay is 8 * base_ms");
    ServiceConfig cfg = make_cfg(1000, 60000);

    int64_t before = platform_now_ms();
    int64_t result = service_compute_backoff_ms(&cfg, 3);
    int64_t after  = platform_now_ms();

    ASSERT_IN_RANGE(result, before + 8000, after + 8000);
}

static void test_backoff_capped_at_max(void) {
    TEST("large retry count: delay is capped at backoff_max_ms");
    /* base=1000, max=4000: after 2 doublings we reach 4000 and hit the cap.
     * retry 10 would be 1024s without the cap — but the result must be 4000. */
    ServiceConfig cfg = make_cfg(1000, 4000);

    int64_t before = platform_now_ms();
    int64_t result = service_compute_backoff_ms(&cfg, 10);
    int64_t after  = platform_now_ms();

    ASSERT_IN_RANGE(result, before + 4000, after + 4000);
}

static void test_backoff_cap_hit_at_exact_doubling(void) {
    TEST("cap is applied at the doubling that first exceeds max_ms");
    /* base=1000, max=3000: retry 0=1000, retry 1=2000, retry 2 would be 4000
     * but is capped at 3000. */
    ServiceConfig cfg = make_cfg(1000, 3000);

    int64_t before = platform_now_ms();
    int64_t result = service_compute_backoff_ms(&cfg, 2);
    int64_t after  = platform_now_ms();

    ASSERT_IN_RANGE(result, before + 3000, after + 3000);
}

static void test_backoff_max_equals_base(void) {
    TEST("when max equals base, delay is always base_ms regardless of retry count");
    /* Setting max = base means the very first doubling (if any) hits the cap.
     * So every retry returns base. */
    ServiceConfig cfg = make_cfg(2000, 2000);

    int64_t before0 = platform_now_ms();
    int64_t r0      = service_compute_backoff_ms(&cfg, 0);
    int64_t after0  = platform_now_ms();
    ASSERT_IN_RANGE(r0, before0 + 2000, after0 + 2000);

    TEST("max == base still returns base at retry 5");
    int64_t before5 = platform_now_ms();
    int64_t r5      = service_compute_backoff_ms(&cfg, 5);
    int64_t after5  = platform_now_ms();
    ASSERT_IN_RANGE(r5, before5 + 2000, after5 + 2000);
}

static void test_backoff_small_base(void) {
    TEST("backoff works correctly with a small base (100ms)");
    ServiceConfig cfg = make_cfg(100, 60000);

    int64_t before = platform_now_ms();
    int64_t result = service_compute_backoff_ms(&cfg, 3); /* 100 * 2^3 = 800ms */
    int64_t after  = platform_now_ms();

    ASSERT_IN_RANGE(result, before + 800, after + 800);
}

/* ==========================================================================
 * Test runner
 * ==========================================================================
 */

int main(void) {
    printf("\n  guardian — service tests\n");
    printf("  ==========================\n\n");

    /* service_state_name */
    test_state_name_stopped();
    test_state_name_starting();
    test_state_name_running();
    test_state_name_stopping();
    test_state_name_backoff();
    test_state_name_failed();
    test_state_name_unknown();

    /* service_compute_backoff_ms */
    test_backoff_retry_zero();
    test_backoff_retry_one();
    test_backoff_retry_two();
    test_backoff_retry_three();
    test_backoff_capped_at_max();
    test_backoff_cap_hit_at_exact_doubling();
    test_backoff_max_equals_base();
    test_backoff_small_base();

    SUMMARY();
    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
