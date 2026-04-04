/*
 * test_framework.h — Zero-dependency unit test framework
 *
 * This entire test framework is 5 macros. No external libraries.
 * No installation required. It works anywhere C works.
 *
 * Usage:
 *
 *   #include "test_framework.h"
 *
 *   static void test_something(void) {
 *       TEST("description of what we're testing");
 *       ASSERT(1 + 1 == 2);
 *       ASSERT_STR_EQ("hello", "hello");
 *       ASSERT_INT_EQ(42, some_function());
 *   }
 *
 *   int main(void) {
 *       test_something();
 *       SUMMARY();
 *       return g_tests_failed > 0 ? 1 : 0;
 *   }
 *
 * Output on success:
 *   PASS  description of what we're testing
 *
 * Output on failure:
 *   FAIL  description of what we're testing
 *         ASSERT failed: tests/test_config.c:42: some_function() == 42
 */

#pragma once

#include <stdio.h>   /* printf, fprintf */
#include <string.h>  /* strcmp */

/* Global counters — defined here, shared across all test files in one binary.
 * 'static' makes them local to each translation unit that includes this header,
 * which is fine because all our tests compile into one binary. */
static int g_tests_run    = 0;
static int g_tests_failed = 0;
static const char *g_current_test = "(no test active)";

/*
 * TEST(name) — declare the start of a new test case
 *
 * Sets the name of the current test (used in failure messages).
 * Also increments the test counter.
 *
 * Macro breakdown:
 *   do { ... } while(0) is a common C macro idiom. It wraps multiple
 *   statements so the macro behaves correctly in all contexts (e.g., after
 *   an if without braces). Always use this pattern for multi-statement macros.
 */
#define TEST(name)                                  \
    do {                                            \
        g_current_test = (name);                    \
        g_tests_run++;                              \
    } while (0)

/*
 * ASSERT(condition) — verify that a condition is true
 *
 * If condition is false:
 *   - Prints a failure message with the file name and line number
 *   - Increments the failure counter
 *   - Does NOT abort the test — execution continues so we see all failures
 *
 * __FILE__ and __LINE__ are predefined macros the compiler fills in with
 * the source file path and line number of the ASSERT call. This is how
 * test frameworks tell you exactly where a failure occurred.
 *
 * #condition stringifies the condition — the macro turns the code text
 * into a string literal. So ASSERT(x == 5) prints "x == 5" on failure.
 */
#define ASSERT(condition)                                                        \
    do {                                                                         \
        if (!(condition)) {                                                      \
            fprintf(stderr, "  FAIL  %s\n", g_current_test);                   \
            fprintf(stderr, "        Assert failed at %s:%d: %s\n",            \
                    __FILE__, __LINE__, #condition);                             \
            g_tests_failed++;                                                    \
        } else {                                                                 \
            printf("  PASS  %s\n", g_current_test);                            \
        }                                                                        \
    } while (0)

/*
 * ASSERT_STR_EQ(expected, actual) — verify two strings are equal
 *
 * Provides a more useful failure message than ASSERT(strcmp(a,b)==0)
 * by printing both the expected and actual values.
 */
#define ASSERT_STR_EQ(expected, actual)                                          \
    do {                                                                         \
        const char *_exp = (expected);                                           \
        const char *_act = (actual);                                             \
        if (strcmp(_exp, _act) != 0) {                                          \
            fprintf(stderr, "  FAIL  %s\n", g_current_test);                   \
            fprintf(stderr, "        String mismatch at %s:%d\n",              \
                    __FILE__, __LINE__);                                         \
            fprintf(stderr, "        expected: \"%s\"\n", _exp);               \
            fprintf(stderr, "        actual:   \"%s\"\n", _act);               \
            g_tests_failed++;                                                    \
        } else {                                                                 \
            printf("  PASS  %s\n", g_current_test);                            \
        }                                                                        \
    } while (0)

/*
 * ASSERT_INT_EQ(expected, actual) — verify two integers are equal
 */
#define ASSERT_INT_EQ(expected, actual)                                          \
    do {                                                                         \
        int _exp = (expected);                                                   \
        int _act = (actual);                                                     \
        if (_exp != _act) {                                                      \
            fprintf(stderr, "  FAIL  %s\n", g_current_test);                   \
            fprintf(stderr, "        Int mismatch at %s:%d: "                  \
                            "expected %d, got %d\n",                            \
                    __FILE__, __LINE__, _exp, _act);                            \
            g_tests_failed++;                                                    \
        } else {                                                                 \
            printf("  PASS  %s\n", g_current_test);                            \
        }                                                                        \
    } while (0)

/*
 * SUMMARY() — print the final pass/fail count
 *
 * Call this at the end of main() in each test file.
 */
#define SUMMARY()                                                                \
    do {                                                                         \
        printf("\n  %d test(s) run. %d passed. %d failed.\n\n",                \
               g_tests_run,                                                      \
               g_tests_run - g_tests_failed,                                    \
               g_tests_failed);                                                  \
    } while (0)
