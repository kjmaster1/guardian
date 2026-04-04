/*
 * test_logger.c — Unit tests for logger.c
 *
 * The logger writes structured lines to stdout and (optionally) to a file:
 *
 *   [INFO ][web-api ][2026-04-04T10:23:01Z] Server started on port 8080
 *
 * We test the file output path — we can't easily capture stdout in a portable
 * way, but since both stdout and file use the same format string, testing the
 * file confirms the format is correct.
 *
 * Test strategy:
 *   1. Call logger_init(path) with a temp file path.
 *   2. Call logger_write() or a LOG_* macro.
 *   3. Call logger_close() to flush and close the file.
 *   4. Open and read the temp file.
 *   5. Assert that expected substrings appear in the file content.
 *   6. Delete the temp file.
 *
 * Why test for substrings rather than the exact line?
 * The timestamp changes on every run. If we asserted the full line, the test
 * would fail every second. Instead we check the parts we control:
 *   - The level tag:    "[INFO ]", "[WARN ]", "[ERROR]"
 *   - The service name: "my-svc"
 *   - The message text: "hello 42"
 *
 * Key C concept: the logger uses static module-level state (g_log_file).
 * Each test must call logger_close() before the next test calls logger_init(),
 * otherwise the second logger_init() would open a new file while the old one
 * is still open — a file handle leak. Order matters with stateful APIs.
 */

#include "test_framework.h"
#include "../src/logger.h"

#include <stdio.h>    /* FILE, fopen, fclose, fread, fprintf, remove, tmpnam */
#include <string.h>   /* strstr */
#include <stdlib.h>   /* EXIT_SUCCESS, EXIT_FAILURE */

/* ==========================================================================
 * Helpers
 * ==========================================================================
 */

/* Generate a temp file path. Returns a pointer to a static buffer. */
static const char *temp_path(void) {
    static char path[L_tmpnam];
    tmpnam(path);
    return path;
}

/*
 * file_contains — read up to 4KB of a file and check if needle appears.
 *
 * Returns 1 if needle is found, 0 if not found or file cannot be opened.
 * We only read 4KB because log lines from our tests are tiny. A production
 * helper would need to handle larger files.
 */
static int file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    return strstr(buf, needle) != NULL;
}

/* ==========================================================================
 * Tests
 * ==========================================================================
 */

static void test_logger_init_null_does_not_crash(void) {
    TEST("logger_init(NULL) initialises without creating a file");
    /* NULL means stdout-only. This just sets g_log_file = NULL and
     * g_initialized = 1. Should not crash or open any file. */
    logger_init(NULL);
    logger_close();
    ASSERT(1);  /* reaching here without crashing is the assertion */
}

static void test_logger_creates_file(void) {
    TEST("logger_init(path) creates the log file");
    const char *path = temp_path();

    logger_init(path);
    logger_close();

    /* The file should exist now (logger opens in "a" mode which creates it) */
    FILE *f = fopen(path, "r");
    ASSERT(f != NULL);
    if (f) fclose(f);
    remove(path);
}

static void test_logger_writes_info_tag(void) {
    TEST("LOG_INFO writes [INFO ] tag to log file");
    const char *path = temp_path();

    logger_init(path);
    LOG_INFO("svc", "test info message");
    logger_close();

    ASSERT(file_contains(path, "[INFO ]"));
    remove(path);
}

static void test_logger_writes_warn_tag(void) {
    TEST("LOG_WARN writes [WARN ] tag to log file");
    const char *path = temp_path();

    logger_init(path);
    LOG_WARN("svc", "test warn message");
    logger_close();

    ASSERT(file_contains(path, "[WARN ]"));
    remove(path);
}

static void test_logger_writes_error_tag(void) {
    TEST("LOG_ERROR writes [ERROR] tag to log file");
    const char *path = temp_path();

    logger_init(path);
    LOG_ERROR("svc", "test error message");
    logger_close();

    ASSERT(file_contains(path, "[ERROR]"));
    remove(path);
}

static void test_logger_includes_service_name(void) {
    TEST("log line contains the service name passed to the macro");
    const char *path = temp_path();

    logger_init(path);
    LOG_INFO("my-service", "some message");
    logger_close();

    ASSERT(file_contains(path, "my-service"));
    remove(path);
}

static void test_logger_includes_message_text(void) {
    TEST("log line contains the exact message text");
    const char *path = temp_path();

    logger_init(path);
    LOG_INFO("svc", "unique-sentinel-string");
    logger_close();

    ASSERT(file_contains(path, "unique-sentinel-string"));
    remove(path);
}

static void test_logger_formats_printf_arguments(void) {
    TEST("logger formats printf-style arguments into the message");
    const char *path = temp_path();

    logger_init(path);
    LOG_INFO("svc", "port %d pid %d", 8080, 12345);
    logger_close();

    /* The formatted string — not the format — should be in the file */
    ASSERT(file_contains(path, "port 8080 pid 12345"));
    remove(path);
}

static void test_logger_multiple_writes_all_appear(void) {
    TEST("multiple log writes all appear in the output file");
    const char *path = temp_path();

    logger_init(path);
    LOG_INFO("svc",  "first-line");
    LOG_WARN("svc",  "second-line");
    LOG_ERROR("svc", "third-line");
    logger_close();

    ASSERT(file_contains(path, "first-line"));

    TEST("second log line appears in file");
    ASSERT(file_contains(path, "second-line"));

    TEST("third log line appears in file");
    ASSERT(file_contains(path, "third-line"));

    remove(path);
}

static void test_logger_appends_across_sessions(void) {
    TEST("re-opening the same log file appends (does not overwrite)");
    const char *path = temp_path();

    /* First session */
    logger_init(path);
    LOG_INFO("svc", "session-one");
    logger_close();

    /* Second session — same path */
    logger_init(path);
    LOG_INFO("svc", "session-two");
    logger_close();

    /* Both lines should be in the file */
    ASSERT(file_contains(path, "session-one"));

    TEST("second session line is also present after re-open");
    ASSERT(file_contains(path, "session-two"));

    remove(path);
}

static void test_logger_close_allows_reinit(void) {
    TEST("logger can be closed and re-initialised with a different file");
    /* Use local buffers — temp_path() returns a static buffer and two
     * calls to it would return the same pointer, overwriting the first name. */
    char path1[L_tmpnam];
    char path2[L_tmpnam];
    tmpnam(path1);
    tmpnam(path2);

    logger_init(path1);
    LOG_INFO("svc", "written-to-first");
    logger_close();

    logger_init(path2);
    LOG_INFO("svc", "written-to-second");
    logger_close();

    /* Each message should only be in its own file */
    ASSERT( file_contains(path1, "written-to-first"));

    TEST("first-file message does not appear in second file");
    ASSERT(!file_contains(path2, "written-to-first"));

    TEST("second-file message appears in second file");
    ASSERT( file_contains(path2, "written-to-second"));

    remove(path1);
    remove(path2);
}

/* ==========================================================================
 * Test runner
 * ==========================================================================
 */

int main(void) {
    printf("\n  guardian — logger tests\n");
    printf("  ==========================\n\n");

    test_logger_init_null_does_not_crash();
    test_logger_creates_file();
    test_logger_writes_info_tag();
    test_logger_writes_warn_tag();
    test_logger_writes_error_tag();
    test_logger_includes_service_name();
    test_logger_includes_message_text();
    test_logger_formats_printf_arguments();
    test_logger_multiple_writes_all_appear();
    test_logger_appends_across_sessions();
    test_logger_close_allows_reinit();

    SUMMARY();
    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
