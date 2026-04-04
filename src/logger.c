/*
 * logger.c — Implementation of the structured logger
 *
 * Key C concepts demonstrated here:
 *   - Static variables (module-level state without global variables)
 *   - Variadic functions (va_list, va_start, vfprintf, va_end)
 *   - Time formatting (strftime, gmtime)
 *   - File I/O (fopen, fprintf, fflush, fclose)
 */

#include "logger.h"

#include <stdio.h>    /* printf, fprintf, fopen, fflush, fclose, FILE */
#include <stdlib.h>   /* NULL                                          */
#include <stdarg.h>   /* va_list, va_start, va_copy, va_end           */
#include <time.h>     /* time_t, struct tm, time(), gmtime(), strftime */
#include <string.h>   /* strncpy                                       */

/*
 * Static module-level state
 *
 * 'static' on a variable at file scope means: this variable is private to
 * this .c file. Other .c files cannot access it directly. This is how C
 * achieves encapsulation — the equivalent of 'private' in C++/Java.
 *
 * We store two things:
 *   g_log_file: the open file handle, or NULL if logging to stdout only.
 *   g_initialized: a guard so we don't use the logger before init.
 *
 * The 'g_' prefix is a convention meaning "global to this module" (not
 * truly global — 'static' keeps it private). Many professional C codebases
 * use this prefix for module-level state variables.
 */
static FILE *g_log_file    = NULL;
static int   g_initialized = 0;

/* Level labels — indexed by LogLevel enum value.
 * We pad them to 5 chars so the output columns align neatly. */
static const char *LEVEL_LABELS[] = {
    "DEBUG",   /* LOG_DEBUG = 0 */
    "INFO ",   /* LOG_INFO  = 1  (trailing space for alignment) */
    "WARN ",   /* LOG_WARN  = 2  (trailing space for alignment) */
    "ERROR"    /* LOG_ERROR = 3 */
};

void logger_init(const char *log_file_path) {
    g_initialized = 1;

    if (log_file_path == NULL || log_file_path[0] == '\0') {
        /* No file path provided — stdout only. */
        g_log_file = NULL;
        return;
    }

    /*
     * fopen(path, "a") opens for appending.
     * If the file doesn't exist, it's created.
     * If it does exist, new lines are added at the end (not overwritten).
     * This preserves log history across guardian restarts.
     */
    g_log_file = fopen(log_file_path, "a");
    if (g_log_file == NULL) {
        /*
         * fopen failed (permission denied, invalid path, etc.).
         * We don't crash — we fall back to stdout-only and warn the user.
         * This is the "fail soft" principle: degrade gracefully.
         */
        fprintf(stderr, "[guardian] WARNING: could not open log file '%s', "
                        "falling back to stdout\n", log_file_path);
    }
}

void logger_write(LogLevel level, const char *service, const char *fmt, ...) {
    /*
     * Step 1: Build the timestamp string.
     *
     * time(NULL) returns the current time as seconds since the Unix epoch
     * (midnight UTC, January 1, 1970). This is a 'time_t' value.
     *
     * gmtime() converts it to a 'struct tm' — a broken-down time struct
     * with fields: tm_year, tm_mon, tm_mday, tm_hour, tm_min, tm_sec, etc.
     * We use gmtime (UTC) rather than localtime to keep logs timezone-neutral.
     *
     * strftime() formats the struct tm into a human-readable string.
     * "%Y-%m-%dT%H:%M:%SZ" produces: "2026-04-04T10:23:01Z"
     * This is the ISO 8601 format — the international standard for timestamps.
     */
    time_t    now    = time(NULL);
    struct tm now_tm = *gmtime(&now);  /* dereference to copy — gmtime returns
                                          a pointer to a static buffer that
                                          would be overwritten on the next call */
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &now_tm);

    /*
     * Step 2: Format the service name into a fixed-width field.
     *
     * We pad the service name to 8 characters so log columns align.
     * Example output:
     *   [INFO ][web-api ][2026-04-04T10:23:01Z] message
     *   [WARN ][worker  ][2026-04-04T10:23:01Z] message
     *   [ERROR][database][2026-04-04T10:23:01Z] message
     *
     * snprintf(buf, size, fmt, ...) is the safe version of sprintf.
     * It will never write more than 'size' bytes, preventing buffer overflows.
     * Always use snprintf (with a size limit) — never sprintf (no limit).
     */
    char svc_field[12];
    snprintf(svc_field, sizeof(svc_field), "%-8s", service);
    /* %-8s: left-align the string in an 8-character wide field */

    /*
     * Step 3: Write the formatted line.
     *
     * We use va_list to handle the variadic arguments (the "..." in the
     * function signature). This is how printf-style functions work:
     *
     *   va_list args;           declares a variable to hold the arg list
     *   va_start(args, fmt);    initializes 'args' starting after 'fmt'
     *   vfprintf(f, fmt, args); writes formatted output using the arg list
     *   va_end(args);           cleans up the arg list
     *
     * We need two separate va_list instances (one for stdout, one for file)
     * because va_list is consumed as you read from it — after vfprintf,
     * the list is exhausted. va_copy() creates a fresh copy to use again.
     */
    va_list args;
    va_start(args, fmt);

    /* Write to stdout always */
    fprintf(stdout, "[%s][%s][%s] ", LEVEL_LABELS[level], svc_field, timestamp);
    vfprintf(stdout, fmt, args);
    fprintf(stdout, "\n");

    /* Write to file if configured */
    if (g_log_file != NULL) {
        va_list args_copy;
        va_copy(args_copy, args);  /* must copy — args is already partially consumed */
        fprintf(g_log_file, "[%s][%s][%s] ", LEVEL_LABELS[level], svc_field, timestamp);
        vfprintf(g_log_file, fmt, args_copy);
        fprintf(g_log_file, "\n");
        /*
         * fflush() forces the C runtime to write buffered output to the OS.
         * Without it, lines might sit in a buffer and only appear when the
         * buffer fills up or the file closes. For a log file, you always
         * want lines to appear immediately — especially on crashes.
         */
        fflush(g_log_file);
        va_end(args_copy);
    }

    va_end(args);
    (void)g_initialized; /* suppress unused variable warning in Phase 1 */
}

void logger_close(void) {
    if (g_log_file != NULL) {
        fflush(g_log_file);
        fclose(g_log_file);
        g_log_file = NULL;
    }
    g_initialized = 0;
}
