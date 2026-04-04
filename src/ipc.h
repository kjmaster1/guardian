/*
 * ipc.h — Inter-Process Communication for guardian
 *
 * Provides two distinct roles:
 *
 * SERVER role (the running daemon):
 *   - Opens a Named Pipe / Unix socket that listens for commands
 *   - Called once per event loop tick to check for incoming connections
 *   - Reads a command, executes it, writes the response, closes connection
 *
 * CLIENT role (guardian stop / guardian status):
 *   - Connects to the daemon's pipe/socket
 *   - Sends a JSON command string
 *   - Reads the JSON response
 *   - Disconnects and returns
 *
 * Protocol:
 *   Client sends:  {"cmd":"stop"}
 *                  {"cmd":"status"}
 *   Server replies: {"ok":true}
 *                   {"ok":true,"services":[{"name":"api","state":"RUNNING",...},...]}
 */

#pragma once

#include "service.h"   /* SupervisorContext */

/* Maximum length of one IPC message (request or response).
 * With 64 services, the status response can be large. 16KB is plenty. */
#define IPC_MSG_MAX 16384

/*
 * ipc_server_open — create the listening pipe/socket
 *
 * Must be called before the event loop starts.
 * Returns a server fd on success, -1 on failure.
 *
 * The path comes from ctx->ipc_socket. If that's empty, a platform-specific
 * default is used:
 *   Windows: \\.\pipe\guardian
 *   Linux:   /tmp/guardian.sock
 */
int ipc_server_open(SupervisorContext *ctx);

/*
 * ipc_server_tick — check for and handle one incoming command (non-blocking)
 *
 * Call this every event loop iteration. If no client is connected, returns
 * immediately. If a client is connected, reads the command, executes it,
 * writes the response, and closes the connection — all within this call.
 *
 * server_fd: the fd returned by ipc_server_open()
 */
void ipc_server_tick(SupervisorContext *ctx, int server_fd);

/*
 * ipc_server_close — close the listening pipe/socket
 *
 * Called during shutdown.
 */
void ipc_server_close(int server_fd);

/*
 * ipc_send_command — connect to a running daemon and send a command (CLIENT)
 *
 * ipc_path:  the Named Pipe path or Unix socket path
 * cmd_json:  the command to send, e.g. {"cmd":"stop"}
 * resp_buf:  buffer to write the response into
 * resp_len:  size of resp_buf
 *
 * Returns 1 on success (response written to resp_buf), 0 on failure.
 */
int ipc_send_command(const char *ipc_path,
                     const char *cmd_json,
                     char       *resp_buf,
                     int         resp_len);
