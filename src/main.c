/*
 * main.c — Guardian entry point
 *
 * main() does exactly one thing: initialize the logger and hand off to cli_run().
 * Keeping main() this small is intentional. A clean main() is a sign of good
 * program structure — all real logic lives in appropriate modules.
 *
 * Why initialize the logger here before cli_run()?
 * Because cli_run() calls config_load(), which may emit log messages during
 * parsing. The logger must be ready before any other module is used.
 */

#include "cli.h"
#include "logger.h"

/*
 * main — program entry point
 *
 * int argc:    the number of command-line arguments (always >= 1)
 * char *argv[]: array of argument strings
 *   argv[0]: the name the program was invoked with ("./guardian" or "guardian")
 *   argv[1]: the subcommand ("start", "stop", "status", ...)
 *   argv[2]: the config file path or service name, depending on the command
 *
 * Returns: 0 on success, non-zero on failure.
 * The shell uses the return value: 'if guardian start ...; then ...; fi'
 */
int main(int argc, char *argv[]) {
    /*
     * Initialize with NULL = stdout only.
     * The config file may specify a log file path; the supervisor will
     * call logger_init() again with that path once config is loaded.
     * For CLI commands like 'status' and 'stop', stdout is always correct.
     */
    logger_init(NULL);

    int exit_code = cli_run(argc, argv);

    logger_close();
    return exit_code;
}
