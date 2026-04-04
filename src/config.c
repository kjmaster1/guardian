/*
 * config.c — INI config file parser
 *
 * Key C concepts demonstrated here:
 *   - File I/O with fgets() — reading a text file line by line
 *   - String manipulation: strchr, strncpy, strncmp, strtol, sscanf
 *   - State machine pattern — tracking "which section are we in?"
 *   - Defensive programming — validate every input, never trust the file
 *   - memset() — zeroing a struct to initialize all fields safely
 */

#include "config.h"
#include "logger.h"

#include <stdio.h>    /* fopen, fclose, fgets, FILE                        */
#include <string.h>   /* memset, strchr, strncpy, strcmp, strncmp, strlen  */
#include <stdlib.h>   /* strtol                                             */
#include <ctype.h>    /* isspace — check if a char is a whitespace char    */
#include <errno.h>    /* errno — the OS sets this when operations fail      */

/* ==========================================================================
 * Internal helpers (static = private to this file)
 * ========================================================================== */

/*
 * trim_whitespace — remove leading and trailing whitespace from a string
 *
 * This modifies the string in-place. It works in two steps:
 * 1. Move the start pointer forward past any leading spaces/tabs.
 * 2. Walk backwards from the end and replace trailing whitespace with '\0'.
 *
 * Why do we need this? INI files often have lines like:
 *   "  command   =   /usr/bin/app  "
 * We need to extract "command" and "/usr/bin/app" cleanly.
 *
 * Returns a pointer to the first non-whitespace character (within the
 * original buffer — no allocation).
 */
static char *trim_whitespace(char *s) {
    /* Step 1: skip leading whitespace */
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }

    /* If the string is all whitespace, we're done */
    if (*s == '\0') {
        return s;
    }

    /* Step 2: walk back from the end and null-terminate after last non-space.
     *
     * strlen(s) returns the number of chars before the '\0'.
     * s + strlen(s) - 1 points to the LAST character.
     * We work backwards, overwriting whitespace chars with '\0'.
     */
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return s;
}

/*
 * parse_int — safely convert a string to an int
 *
 * strtol() is the safe string-to-long function. Unlike atoi(), it:
 *   - Sets errno if the string is not a valid number
 *   - Reports where parsing stopped (via the 'end' pointer)
 *   - Handles overflow
 *
 * Returns 1 on success (and writes the value to *out), 0 on failure.
 */
static int parse_int(const char *s, int *out) {
    char *end;
    errno = 0;  /* clear any previous error */

    long val = strtol(s, &end, 10);  /* base 10 */

    /* Failure conditions:
     *   errno != 0    → overflow or underflow
     *   end == s      → no digits were consumed (empty string or garbage)
     *   *end != '\0'  → trailing non-numeric characters */
    if (errno != 0 || end == s || *end != '\0') {
        return 0;  /* failure */
    }

    *out = (int)val;
    return 1;  /* success */
}

/*
 * parse_restart_policy — convert a string to a RestartPolicy enum
 *
 * strcmp() returns 0 if two strings are identical. We use it to check
 * which keyword the config file used. Case-sensitive (intentional —
 * config keys should be consistent).
 */
static int parse_restart_policy(const char *s, RestartPolicy *out) {
    if (strcmp(s, "never") == 0) {
        *out = RESTART_NEVER;
        return 1;
    }
    if (strcmp(s, "on_failure") == 0) {
        *out = RESTART_ON_FAILURE;
        return 1;
    }
    if (strcmp(s, "always") == 0) {
        *out = RESTART_ALWAYS;
        return 1;
    }
    return 0;  /* unrecognized value */
}

/*
 * parse_health_type — convert a string to a HealthCheckType enum
 */
static int parse_health_type(const char *s, HealthCheckType *out) {
    if (strcmp(s, "none") == 0)    { *out = HEALTH_NONE;    return 1; }
    if (strcmp(s, "tcp") == 0)     { *out = HEALTH_TCP;     return 1; }
    if (strcmp(s, "http") == 0)    { *out = HEALTH_HTTP;    return 1; }
    if (strcmp(s, "command") == 0) { *out = HEALTH_COMMAND; return 1; }
    return 0;
}

/* ==========================================================================
 * Section type — tracks where we are in the INI file
 * ========================================================================== */

/*
 * SectionType — the parser's "state machine" state
 *
 * A state machine is a programming pattern where the system is always in
 * exactly one "state", and each input can cause a state transition.
 *
 * Our parser states:
 *   SECTION_NONE:         haven't seen any section header yet
 *   SECTION_SUPERVISOR:   inside the [supervisor] block
 *   SECTION_SERVICE:      inside a [service-name] block
 *   SECTION_HEALTH_CHECK: inside a [health_check] block (belongs to current service)
 */
typedef enum {
    SECTION_NONE         = 0,
    SECTION_SUPERVISOR   = 1,
    SECTION_SERVICE      = 2,
    SECTION_HEALTH_CHECK = 3
} SectionType;

/* ==========================================================================
 * Public API
 * ========================================================================== */

ConfigError config_load(const char *path, SupervisorContext *ctx) {
    /*
     * memset(ptr, 0, size) fills every byte of the struct with zero.
     * This guarantees that:
     *   - All ints are 0
     *   - All char arrays are empty strings ('\0' at index 0)
     *   - All enum fields are 0 (which maps to our "none/default" values)
     *
     * ALWAYS zero-initialize structs before use in C. Uninitialized memory
     * contains whatever bytes happened to be there — reading it is undefined
     * behavior and a common source of bugs.
     */
    memset(ctx, 0, sizeof(SupervisorContext));
    ctx->running = 1;  /* supervisor starts in the running state */

    /* Open the config file for reading */
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        /* errno is set by fopen on failure. We log it for diagnostics. */
        LOG_ERROR("guardian", "Cannot open config file '%s': %s",
                  path, strerror(errno));
        return CONFIG_ERR_FILE;
    }

    /* Parser state */
    SectionType  current_section  = SECTION_NONE;
    int          current_svc_idx  = -1;  /* index into ctx->services[], -1 = none */
    int          line_number       = 0;
    char         line[1024];              /* one line of the file at a time */

    /*
     * fgets(buffer, size, file) reads up to (size-1) characters from 'file'
     * into 'buffer', stopping at newline or EOF. It always null-terminates.
     * Returns NULL when there's nothing more to read (EOF or error).
     *
     * This is the standard idiom for reading a text file line by line in C.
     */
    while (fgets(line, sizeof(line), f) != NULL) {
        line_number++;

        /*
         * Strip the newline. fgets keeps the '\n' at the end of the line.
         * strchr(s, c) returns a pointer to the first occurrence of char 'c'
         * in string 's', or NULL if not found. We set it to '\0' to truncate.
         */
        char *newline = strchr(line, '\n');
        if (newline) *newline = '\0';
        char *cr = strchr(line, '\r');  /* handle Windows CRLF line endings */
        if (cr) *cr = '\0';

        /* Trim leading/trailing whitespace */
        char *trimmed = trim_whitespace(line);

        /* Skip blank lines */
        if (trimmed[0] == '\0') continue;

        /* Skip comment lines (starting with ';' or '#') */
        if (trimmed[0] == ';' || trimmed[0] == '#') continue;

        /* ----------------------------------------------------------------
         * Case 1: Section header — "[something]"
         * ----------------------------------------------------------------
         * A section header starts with '[' and ends with ']'.
         * We extract the name between the brackets and determine which
         * section type it is.
         */
        if (trimmed[0] == '[') {
            char *close = strchr(trimmed, ']');
            if (close == NULL) {
                LOG_ERROR("guardian", "Config line %d: unclosed '[' in section header",
                          line_number);
                fclose(f);
                return CONFIG_ERR_SYNTAX;
            }

            /* Extract the section name: the text between '[' and ']' */
            *close = '\0';
            char *section_name = trim_whitespace(trimmed + 1);  /* +1 skips the '[' */

            if (strcmp(section_name, "supervisor") == 0) {
                current_section = SECTION_SUPERVISOR;
                current_svc_idx = -1;

            } else if (strcmp(section_name, "health_check") == 0) {
                /* [health_check] attaches to the most recently parsed service */
                if (current_svc_idx < 0) {
                    LOG_ERROR("guardian",
                              "Config line %d: [health_check] before any service section",
                              line_number);
                    fclose(f);
                    return CONFIG_ERR_SYNTAX;
                }
                current_section = SECTION_HEALTH_CHECK;
                ctx->services[current_svc_idx].cfg.has_health_check = 1;

            } else {
                /* Any other [name] is a new service section */
                if (ctx->service_count >= GUARDIAN_MAX_SERVICES) {
                    LOG_ERROR("guardian",
                              "Config line %d: too many services (max %d)",
                              line_number, GUARDIAN_MAX_SERVICES);
                    fclose(f);
                    return CONFIG_ERR_TOO_MANY;
                }

                current_svc_idx = ctx->service_count;
                ctx->service_count++;
                current_section = SECTION_SERVICE;

                /*
                 * strncpy(dest, src, n) copies up to n bytes from src to dest.
                 * It is safer than strcpy() because it won't overflow the buffer.
                 * IMPORTANT: strncpy does NOT guarantee null-termination if src
                 * is longer than n. We explicitly set the last byte to '\0'.
                 */
                strncpy(ctx->services[current_svc_idx].cfg.name,
                        section_name,
                        GUARDIAN_MAX_NAME - 1);
                ctx->services[current_svc_idx].cfg.name[GUARDIAN_MAX_NAME - 1] = '\0';

                /* Set sensible defaults for optional fields */
                ctx->services[current_svc_idx].cfg.restart_policy  = RESTART_ON_FAILURE;
                ctx->services[current_svc_idx].cfg.max_retries      = 5;
                ctx->services[current_svc_idx].cfg.backoff_base_ms  = 1000;
                ctx->services[current_svc_idx].cfg.backoff_max_ms   = 60000;
                ctx->services[current_svc_idx].cfg.autostart        = 1;
            }
            continue;
        }

        /* ----------------------------------------------------------------
         * Case 2: Key-value pair — "key = value"
         * ----------------------------------------------------------------
         * We find the '=' character to split the line into key and value.
         * Everything before '=' is the key; everything after is the value.
         * Both are trimmed of whitespace.
         */
        char *eq = strchr(trimmed, '=');
        if (eq == NULL) {
            /* A line that's not blank, not a comment, not a section, and
             * has no '=' is a syntax error. */
            LOG_ERROR("guardian", "Config line %d: expected 'key = value', got: %s",
                      line_number, trimmed);
            fclose(f);
            return CONFIG_ERR_SYNTAX;
        }

        *eq = '\0';  /* split the line at '=' */
        char *key   = trim_whitespace(trimmed);  /* the part before '=' */
        char *value = trim_whitespace(eq + 1);   /* the part after '='  */

        /* Strip inline comments from value: "value ; comment" -> "value" */
        char *inline_comment = strchr(value, ';');
        if (inline_comment) {
            *inline_comment = '\0';
            value = trim_whitespace(value);
        }

        /* ----------------------------------------------------------------
         * Dispatch: write the value into the right struct field
         * based on which section we're currently in.
         * ---------------------------------------------------------------- */

        if (current_section == SECTION_SUPERVISOR) {
            if (strcmp(key, "log_file") == 0) {
                strncpy(ctx->log_file, value, GUARDIAN_MAX_PATH - 1);
                ctx->log_file[GUARDIAN_MAX_PATH - 1] = '\0';
            } else if (strcmp(key, "ipc_socket") == 0) {
                strncpy(ctx->ipc_socket, value, GUARDIAN_MAX_PATH - 1);
                ctx->ipc_socket[GUARDIAN_MAX_PATH - 1] = '\0';
            } else {
                /* Unknown keys in [supervisor] are silently ignored.
                 * This allows forward-compatibility: newer config files
                 * with new keys will still load on older guardian binaries. */
                LOG_DEBUG("guardian", "Config line %d: unknown supervisor key '%s' (ignored)",
                          line_number, key);
            }

        } else if (current_section == SECTION_SERVICE) {
            ServiceConfig *cfg = &ctx->services[current_svc_idx].cfg;
            /*
             * '&ctx->services[current_svc_idx].cfg' takes the address of the
             * cfg sub-struct so we can write 'cfg->command' instead of the
             * much longer 'ctx->services[current_svc_idx].cfg.command'.
             * This is a very common C pattern: take a pointer to a sub-struct
             * for convenience within a code block.
             */

            if (strcmp(key, "command") == 0) {
                strncpy(cfg->command, value, GUARDIAN_MAX_CMD - 1);
                cfg->command[GUARDIAN_MAX_CMD - 1] = '\0';
            } else if (strcmp(key, "args") == 0) {
                strncpy(cfg->args, value, GUARDIAN_MAX_ARGS - 1);
                cfg->args[GUARDIAN_MAX_ARGS - 1] = '\0';
            } else if (strcmp(key, "working_dir") == 0) {
                strncpy(cfg->working_dir, value, GUARDIAN_MAX_PATH - 1);
                cfg->working_dir[GUARDIAN_MAX_PATH - 1] = '\0';
            } else if (strcmp(key, "stdout_log") == 0) {
                strncpy(cfg->stdout_log, value, GUARDIAN_MAX_PATH - 1);
                cfg->stdout_log[GUARDIAN_MAX_PATH - 1] = '\0';
            } else if (strcmp(key, "stderr_log") == 0) {
                strncpy(cfg->stderr_log, value, GUARDIAN_MAX_PATH - 1);
                cfg->stderr_log[GUARDIAN_MAX_PATH - 1] = '\0';
            } else if (strcmp(key, "autostart") == 0) {
                cfg->autostart = (strcmp(value, "true") == 0 ||
                                  strcmp(value, "1")    == 0) ? 1 : 0;
            } else if (strcmp(key, "restart") == 0) {
                if (!parse_restart_policy(value, &cfg->restart_policy)) {
                    LOG_ERROR("guardian",
                              "Config line %d: invalid restart value '%s' "
                              "(expected: never, on_failure, always)",
                              line_number, value);
                    fclose(f);
                    return CONFIG_ERR_SYNTAX;
                }
            } else if (strcmp(key, "max_retries") == 0) {
                if (!parse_int(value, &cfg->max_retries) || cfg->max_retries < 0) {
                    LOG_ERROR("guardian",
                              "Config line %d: invalid max_retries '%s'",
                              line_number, value);
                    fclose(f);
                    return CONFIG_ERR_SYNTAX;
                }
            } else if (strcmp(key, "backoff_base_ms") == 0) {
                if (!parse_int(value, &cfg->backoff_base_ms) || cfg->backoff_base_ms <= 0) {
                    LOG_ERROR("guardian",
                              "Config line %d: invalid backoff_base_ms '%s'",
                              line_number, value);
                    fclose(f);
                    return CONFIG_ERR_SYNTAX;
                }
            } else if (strcmp(key, "backoff_max_ms") == 0) {
                if (!parse_int(value, &cfg->backoff_max_ms) || cfg->backoff_max_ms <= 0) {
                    LOG_ERROR("guardian",
                              "Config line %d: invalid backoff_max_ms '%s'",
                              line_number, value);
                    fclose(f);
                    return CONFIG_ERR_SYNTAX;
                }
            } else if (strcmp(key, "env") == 0) {
                /* env = KEY1=VAL1,KEY2=VAL2
                 * Split on commas and store each pair.
                 * strtok() splits a string on a delimiter, returning tokens.
                 * NOTE: strtok modifies the string — it replaces delimiters with '\0'.
                 * We work on a copy to avoid corrupting 'value'.
                 */
                char env_copy[GUARDIAN_MAX_ARGS];
                strncpy(env_copy, value, sizeof(env_copy) - 1);
                env_copy[sizeof(env_copy) - 1] = '\0';

                char *token = strtok(env_copy, ",");
                while (token != NULL && cfg->env_count < GUARDIAN_MAX_ENV_VARS) {
                    token = trim_whitespace(token);
                    strncpy(cfg->env_vars[cfg->env_count], token, GUARDIAN_MAX_ENV_STR - 1);
                    cfg->env_vars[cfg->env_count][GUARDIAN_MAX_ENV_STR - 1] = '\0';
                    cfg->env_count++;
                    token = strtok(NULL, ",");  /* NULL = continue from where we left off */
                }
            } else {
                LOG_DEBUG("guardian", "Config line %d: unknown service key '%s' (ignored)",
                          line_number, key);
            }

        } else if (current_section == SECTION_HEALTH_CHECK) {
            HealthCheckConfig *hc = &ctx->services[current_svc_idx].cfg.health;

            if (strcmp(key, "type") == 0) {
                if (!parse_health_type(value, &hc->type)) {
                    LOG_ERROR("guardian",
                              "Config line %d: invalid health check type '%s' "
                              "(expected: tcp, http, command)",
                              line_number, value);
                    fclose(f);
                    return CONFIG_ERR_SYNTAX;
                }
            } else if (strcmp(key, "target") == 0) {
                strncpy(hc->target, value, GUARDIAN_MAX_PATH - 1);
                hc->target[GUARDIAN_MAX_PATH - 1] = '\0';
            } else if (strcmp(key, "interval_s") == 0) {
                if (!parse_int(value, &hc->interval_s) || hc->interval_s <= 0) {
                    LOG_ERROR("guardian", "Config line %d: invalid interval_s '%s'",
                              line_number, value);
                    fclose(f);
                    return CONFIG_ERR_SYNTAX;
                }
            } else if (strcmp(key, "timeout_s") == 0) {
                if (!parse_int(value, &hc->timeout_s) || hc->timeout_s <= 0) {
                    LOG_ERROR("guardian", "Config line %d: invalid timeout_s '%s'",
                              line_number, value);
                    fclose(f);
                    return CONFIG_ERR_SYNTAX;
                }
            } else if (strcmp(key, "unhealthy_threshold") == 0) {
                if (!parse_int(value, &hc->unhealthy_threshold) ||
                    hc->unhealthy_threshold <= 0) {
                    LOG_ERROR("guardian",
                              "Config line %d: invalid unhealthy_threshold '%s'",
                              line_number, value);
                    fclose(f);
                    return CONFIG_ERR_SYNTAX;
                }
            } else {
                LOG_DEBUG("guardian",
                          "Config line %d: unknown health_check key '%s' (ignored)",
                          line_number, key);
            }
        }
        /* SECTION_NONE: key-value pairs before any section are ignored */
    }

    fclose(f);

    /* ----------------------------------------------------------------
     * Post-parse validation
     * Verify that every service has the required 'command' field.
     * We do this after parsing (not during) so we can report all errors,
     * not just the first one.
     * ---------------------------------------------------------------- */
    for (int i = 0; i < ctx->service_count; i++) {
        if (ctx->services[i].cfg.command[0] == '\0') {
            LOG_ERROR("guardian", "Service '%s' has no 'command' key — required",
                      ctx->services[i].cfg.name);
            return CONFIG_ERR_NO_COMMAND;
        }
    }

    return CONFIG_OK;
}

const char *config_error_string(ConfigError err) {
    switch (err) {
        case CONFIG_OK:             return "success";
        case CONFIG_ERR_FILE:       return "could not open config file";
        case CONFIG_ERR_SYNTAX:     return "syntax error in config file";
        case CONFIG_ERR_TOO_MANY:   return "too many services defined";
        case CONFIG_ERR_NO_COMMAND: return "service is missing required 'command' key";
        default:                    return "unknown error";
    }
}
