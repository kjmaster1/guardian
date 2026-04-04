/*
 * config.h — INI config file parser for guardian
 *
 * Reads a guardian.ini file and populates a SupervisorContext with
 * the parsed service configurations.
 *
 * Config file format:
 *
 *   [supervisor]
 *   log_file   = /var/log/guardian.log
 *   ipc_socket = /tmp/guardian.sock
 *
 *   [my-service]
 *   command     = /usr/bin/my-app
 *   args        = --port 8080
 *   restart     = on_failure
 *   max_retries = 5
 *   ...
 *
 *   [health_check]          <- scoped to the service above it
 *   type        = http
 *   target      = http://127.0.0.1:8080/health
 *   interval_s  = 10
 *   timeout_s   = 3
 *   unhealthy_threshold = 3
 */

#pragma once

#include "service.h"  /* SupervisorContext — the struct we populate */

/*
 * ConfigError — result codes for config_load()
 *
 * Returning an integer code (not crashing) is how professional C libraries
 * signal errors. The caller decides what to do with the error.
 */
typedef enum {
    CONFIG_OK              = 0,   /* parsed successfully                     */
    CONFIG_ERR_FILE        = 1,   /* could not open the file                 */
    CONFIG_ERR_SYNTAX      = 2,   /* malformed line in the INI file          */
    CONFIG_ERR_TOO_MANY    = 3,   /* more than GUARDIAN_MAX_SERVICES defined */
    CONFIG_ERR_NO_COMMAND  = 4    /* a service section has no 'command' key  */
} ConfigError;

/*
 * config_load — parse a guardian INI file into a SupervisorContext
 *
 * path: path to the .ini file (e.g. "guardian.ini" or "/etc/guardian.ini")
 * ctx:  pointer to an already-allocated SupervisorContext to fill in.
 *       The caller is responsible for allocating ctx (typically on the stack
 *       in main()). This function zeros it out before writing to it.
 *
 * Returns: CONFIG_OK on success, or a ConfigError code on failure.
 *
 * On failure, 'ctx' may be partially written — do not use it.
 */
ConfigError config_load(const char *path, SupervisorContext *ctx);

/*
 * config_error_string — human-readable description of a ConfigError code
 *
 * Used by main() to print a useful error message before exiting.
 * Example: fprintf(stderr, "Config error: %s\n", config_error_string(err));
 */
const char *config_error_string(ConfigError err);
