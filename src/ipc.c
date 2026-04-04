/*
 * ipc.c — IPC server and client implementation
 *
 * Key C concepts demonstrated here:
 *   - snprintf for safe string building (constructing JSON output)
 *   - strstr for substring search (parsing JSON input)
 *   - strncat for safe string concatenation
 *   - The difference between a server fd and a client fd
 */

#include "ipc.h"
#include "logger.h"
#include "platform/platform.h"

#include <stdio.h>    /* snprintf  */
#include <string.h>   /* strstr, memset, strlen, strncat */
#include <stdlib.h>   /* NULL      */

/* ==========================================================================
 * Default IPC paths
 * ==========================================================================
 */

/* Single cross-platform default. On Windows, platform_ipc_listen/connect
 * will automatically convert this to \\.\pipe\guardian.sock */
#define DEFAULT_IPC_PATH "/tmp/guardian.sock"

/* ==========================================================================
 * JSON building helpers
 *
 * These functions write JSON into a caller-provided buffer.
 * We use snprintf throughout — it NEVER overflows the buffer.
 *
 * The pattern:
 *   int pos = 0;               // current write position
 *   pos += snprintf(buf + pos, size - pos, "...", ...);
 *
 * 'buf + pos' advances the write pointer.
 * 'size - pos' ensures we never write past the end.
 * Adding the return value of snprintf to pos advances past what was written.
 * ==========================================================================
 */

/*
 * build_status_response — serialize all services to a JSON string
 *
 * Produces output like:
 * {"ok":true,"services":[
 *   {"name":"api","state":"RUNNING","pid":1234,"retries":0,"uptime_s":142},
 *   {"name":"worker","state":"BACKOFF","pid":0,"retries":2,"uptime_s":0}
 * ]}
 */
static void build_status_response(const SupervisorContext *ctx,
                                   char *buf, int buf_size) {
    int pos = 0;

    pos += snprintf(buf + pos, buf_size - pos,
                    "{\"ok\":true,\"services\":[");

    for (int i = 0; i < ctx->service_count; i++) {
        const Service *svc = &ctx->services[i];

        /* Calculate uptime in seconds.
         * platform_now_ms() - started_at_ms gives elapsed milliseconds.
         * Divide by 1000 to get seconds.
         * If the service isn't running, uptime is 0. */
        int64_t uptime_s = 0;
        if (svc->rt.state == STATE_RUNNING ||
            svc->rt.state == STATE_STARTING) {
            uptime_s = (platform_now_ms() - svc->rt.started_at_ms) / 1000;
        }

        /* All values are quoted strings — keeps our simple parser uniform */
        pos += snprintf(buf + pos, buf_size - pos,
            "%s{"
            "\"name\":\"%s\","
            "\"state\":\"%s\","
            "\"pid\":\"%lu\","
            "\"retries\":\"%d\","
            "\"uptime_s\":\"%lld\""
            "}",
            i > 0 ? "," : "",
            svc->cfg.name,
            service_state_name(svc->rt.state),
            (unsigned long)svc->rt.pid,
            svc->rt.retry_count,
            (long long)uptime_s
        );

        /* Safety: if we're close to the buffer limit, stop early */
        if (pos >= buf_size - 64) break;
    }

    pos += snprintf(buf + pos, buf_size - pos, "]}");
}

/* ==========================================================================
 * Command dispatcher (SERVER side)
 *
 * Reads a JSON command from a connected client fd,
 * executes it, and writes back a JSON response.
 * ==========================================================================
 */

static void dispatch_command(SupervisorContext *ctx,
                              int client_fd,
                              const char *msg) {
    char response[IPC_MSG_MAX];

    /*
     * Parse the command using strstr — "does this string contain X?"
     *
     * strstr(haystack, needle) returns a pointer to the first occurrence
     * of 'needle' inside 'haystack', or NULL if not found.
     * We use it to check which command the client sent.
     */
    if (strstr(msg, "\"stop\"")) {
        /* Tell the main loop to stop on the next tick */
        ctx->running = 0;
        snprintf(response, sizeof(response), "{\"ok\":true}");
        LOG_INFO("guardian", "Stop command received via IPC");

    } else if (strstr(msg, "\"status\"")) {
        build_status_response(ctx, response, sizeof(response));

    } else {
        snprintf(response, sizeof(response),
                 "{\"ok\":false,\"error\":\"unknown command\"}");
        LOG_WARN("guardian", "Unknown IPC command: %.64s", msg);
    }

    platform_ipc_send(client_fd, response, (int)strlen(response));
}

/* ==========================================================================
 * Public API — SERVER
 * ==========================================================================
 */

int ipc_server_open(SupervisorContext *ctx) {
    /* Use config path if set, otherwise use the platform default */
    const char *path = (ctx->ipc_socket[0] != '\0')
                       ? ctx->ipc_socket
                       : DEFAULT_IPC_PATH;

    int server_fd = platform_ipc_listen(path);
    if (server_fd < 0) {
        LOG_WARN("guardian", "IPC server unavailable at '%s' "
                             "(stop/status commands will not work)", path);
    } else {
        LOG_INFO("guardian", "IPC listening at '%s'", path);
    }
    return server_fd;
}

void ipc_server_tick(SupervisorContext *ctx, int server_fd) {
    if (server_fd < 0) return;

    /*
     * platform_ipc_accept() is non-blocking.
     * Returns a client fd if someone connected, -1 if nobody is there.
     * This is called every 100ms from the event loop.
     */
    int client_fd = platform_ipc_accept(server_fd);
    if (client_fd < 0) return;  /* no client this tick */

    /* Read the incoming command */
    char msg[512];
    memset(msg, 0, sizeof(msg));
    int n = platform_ipc_recv(client_fd, msg, sizeof(msg) - 1);

    if (n > 0) {
        dispatch_command(ctx, client_fd, msg);
    }

    platform_ipc_close(client_fd);
}

void ipc_server_close(int server_fd) {
    if (server_fd >= 0) {
        platform_ipc_close(server_fd);
    }
}

/* ==========================================================================
 * Public API — CLIENT
 * ==========================================================================
 */

int ipc_send_command(const char *ipc_path,
                     const char *cmd_json,
                     char       *resp_buf,
                     int         resp_len) {
    /*
     * The client side is much simpler than the server:
     * 1. Open a connection to the named pipe / socket
     * 2. Write the command
     * 3. Read the response
     * 4. Close
     *
     * On failure (daemon not running), we get -1 back from platform_ipc_accept
     * and report a helpful error.
     *
     * We reuse the IPC fd interface here. platform_ipc_listen opens a SERVER.
     * For the client we need a different function — platform_ipc_connect.
     * Rather than add another platform function, we use a small trick:
     * platform_ipc_accept is overloaded to work as platform_ipc_connect
     * when passed the special value -1 along with the path embedded in
     * the server_fd... actually, let's just add the client function properly.
     *
     * For now: platform_ipc_accept(-1) is used as a client connect signal.
     * The platform implementation handles this distinction.
     * See platform_windows.c for the implementation detail.
     */

    /* Use a special sentinel fd (-2) to signal "I am a client, connect to path" */
    int fd = platform_ipc_connect(ipc_path);
    if (fd < 0) {
        fprintf(stderr, "guardian: cannot connect to daemon at '%s'\n"
                        "         Is guardian running? "
                        "Try: guardian start <config>\n", ipc_path);
        return 0;
    }

    platform_ipc_send(fd, cmd_json, (int)strlen(cmd_json));

    memset(resp_buf, 0, resp_len);
    int n = platform_ipc_recv(fd, resp_buf, resp_len - 1);
    platform_ipc_close(fd);

    return n > 0;
}
