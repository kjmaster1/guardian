/*
 * cli.h — Command-line interface for guardian
 *
 * Guardian is invoked as:
 *
 *   guardian start  <config-file>   Start the supervisor with the given config
 *   guardian stop   [config-file]   Stop a running supervisor instance
 *   guardian status [config-file]   Print the status of all managed services
 *   guardian logs   <service-name>  Tail the logs for one service
 *   guardian version                Print version and build information
 *
 * cli_run() is the single entry point. It reads argc/argv, validates the
 * arguments, and dispatches to the appropriate handler.
 */

#pragma once

/*
 * cli_run — parse and dispatch the command-line arguments
 *
 * argc, argv: forwarded directly from main()
 *
 * Returns: an exit code suitable for return from main().
 *   0 = success
 *   1 = usage error or runtime failure
 */
int cli_run(int argc, char *argv[]);
