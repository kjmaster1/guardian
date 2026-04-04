/*
 * logger.h — Structured log writer for guardian
 *
 * Provides a simple, thread-safe logging interface that writes lines in the format:
 *
 *   [INFO ][web-api ][2026-04-04T10:23:01Z] Server started on port 8080
 *   [WARN ][worker  ][2026-04-04T10:23:05Z] Retry 1/3 connecting to cache
 *   [ERROR][database][2026-04-04T10:23:09Z] Health check failed: connection refused
 *
 * Usage:
 *   logger_init(NULL);                        // log to stdout only
 *   logger_init("/var/log/guardian.log");     // log to file AND stdout
 *   LOG_INFO("web-api", "Started on :%d", 8080);
 *   LOG_ERROR("worker", "Exit code %d", exit_code);
 *   logger_close();
 */

#pragma once

#include <stdio.h>  /* FILE* — needed for the log file handle in logger_init() */

/*
 * LogLevel — severity of a log message
 *
 * Ordered from least to most severe. We can use this to filter:
 * "only show WARN and above." In Phase 1 we log everything; filtering
 * is a future enhancement.
 */
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
} LogLevel;

/*
 * logger_init — initialize the logger
 *
 * Must be called before any LOG_* macros are used.
 *
 * log_file_path: path to write logs to, OR NULL to log to stdout only.
 * If a path is provided, logs go to BOTH stdout and the file.
 *
 * The file is opened in append mode ("a") so existing logs are preserved
 * across guardian restarts.
 */
void logger_init(const char *log_file_path);

/*
 * logger_write — write one log line (use the macros below instead)
 *
 * This is the core function. The LOG_* macros call it with the right
 * LogLevel automatically. Direct calls are fine too.
 *
 * level:   severity (LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR)
 * service: name of the service this log line is about, or "guardian"
 *          for messages from the supervisor itself.
 * fmt:     printf-style format string
 * ...:     format arguments (same as printf)
 */
void logger_write(LogLevel level, const char *service, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
/*
 * __attribute__((format(printf, 3, 4))) is a GCC/Clang extension that tells
 * the compiler: "treat argument 3 as a printf format string and check that
 * argument 4 onwards matches the format specifiers." This means the compiler
 * will warn you if you write LOG_INFO("svc", "Port %d", "not-a-number").
 * Enterprise-grade projects always annotate variadic log functions this way.
 */

/*
 * logger_close — flush and close the log file
 *
 * Call this during shutdown. Ensures all buffered log lines are written
 * to disk before the process exits.
 */
void logger_close(void);

/*
 * Convenience macros — these are what the rest of the code uses.
 *
 * Why macros instead of functions?
 * 1. Zero overhead — macros are text substitution at compile time.
 * 2. They pass __VA_ARGS__ (the variadic "...") through to logger_write().
 * 3. The call site is cleaner: LOG_INFO("svc", "msg %d", n)
 *    instead of:             logger_write(LOG_INFO, "svc", "msg %d", n)
 *
 * The ## before __VA_ARGS__ is a GNU extension that removes the preceding
 * comma if __VA_ARGS__ is empty. This lets LOG_INFO("svc", "no args") work.
 */
/*
 * The format string is now part of __VA_ARGS__, not a separate parameter.
 * This means __VA_ARGS__ always has at least one argument (the format string),
 * so no GNU ##__VA_ARGS__ extension is needed. Strictly ISO C11 compliant.
 *
 * Usage: LOG_INFO("svc", "message")          — format string only
 *        LOG_INFO("svc", "port %d", port)    — format string + args
 */
#define LOG_DEBUG(svc, ...) logger_write(LOG_DEBUG, (svc), __VA_ARGS__)
#define LOG_INFO(svc,  ...) logger_write(LOG_INFO,  (svc), __VA_ARGS__)
#define LOG_WARN(svc,  ...) logger_write(LOG_WARN,  (svc), __VA_ARGS__)
#define LOG_ERROR(svc, ...) logger_write(LOG_ERROR, (svc), __VA_ARGS__)
