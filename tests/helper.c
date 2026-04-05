/*
 * tests/helper.c — Minimal test service for integration tests
 *
 * This binary is what guardian actually manages during integration tests.
 * It has two behaviors:
 *
 *   helper run    — loops forever until killed externally
 *                   (simulates a well-behaved long-running service)
 *
 *   helper crash  — exits immediately with code 1
 *                   (simulates a service that keeps crashing)
 *
 * Why a dedicated C binary instead of system commands (sleep, ping, false)?
 *
 *   Cross-platform:  'sleep' is not a built-in on Windows. 'ping -t' loops
 *                    forever but has different syntax. 'false' doesn't exist
 *                    on bare Windows. A small C binary works identically
 *                    on every platform with no PATH dependencies.
 *
 *   Controlled:      We know EXACTLY what this binary does. There are no
 *                    shell parsing surprises, no quoting issues, no
 *                    environment dependencies.
 *
 *   Fast:            The 'crash' command exits in microseconds — tests that
 *                    verify crash/restart behaviour complete quickly.
 *
 * This binary is compiled by 'make integration-test' into:
 *   Linux:   tests/helper
 *   Windows: tests/helper.exe
 */

/* _POSIX_C_SOURCE is required on Linux to access usleep() from <unistd.h>
 * without triggering deprecation warnings under -Wpedantic. usleep(3)
 * is defined in POSIX.1-2001. We only define this on non-Windows so we
 * don't confuse the Windows headers. */
/*
 * Feature test macro — must appear before ALL system header includes.
 *
 * _POSIX_C_SOURCE 199309L enables nanosleep() from <time.h> on Linux.
 * nanosleep() is the portable POSIX sub-second sleep (unlike usleep(),
 * which requires _XOPEN_SOURCE on newer glibc).
 *
 * We guard it with #ifndef _WIN32 so it doesn't interfere with Windows
 * headers (which have their own sleep mechanism and don't use this macro).
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 199309L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

/* sleep_ms — sleep for the given number of milliseconds.
 * Sleep() on Windows takes milliseconds natively. */
static void sleep_ms(int ms) { Sleep((DWORD)ms); }

#else
#include <time.h>

/* sleep_ms — sleep for the given number of milliseconds via nanosleep(). */
static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

#endif /* _WIN32 */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: helper <run|crash>\n");
        return 1;
    }

    if (strcmp(argv[1], "run") == 0) {
        /*
         * Loop forever in 100ms increments.
         *
         * Why not one long sleep(9999999)?
         * On Linux, a process sleeping in nanosleep/usleep is interruptible
         * by signals — guardian's SIGTERM would wake it up. But on Windows,
         * Sleep() is not interruptible by TerminateProcess(). Guardian uses
         * TerminateProcess() which kills the process regardless of what it's
         * doing. So the loop is mainly for clarity, not signal handling.
         *
         * The outer while(1) loop means guardian will always find this
         * process alive when it checks — it never exits on its own.
         */
        while (1) {
            sleep_ms(100);
        }
        return 0;  /* unreachable — here to satisfy the compiler */
    }

    if (strcmp(argv[1], "crash") == 0) {
        /*
         * Exit immediately with code 1.
         * Guardian sees a non-zero exit code and treats this as a crash,
         * applying the service's restart policy (backoff, max_retries, etc.).
         */
        return 1;
    }

    fprintf(stderr, "helper: unknown command '%s' (expected: run, crash)\n",
            argv[1]);
    return 1;
}
