/* Serial connection lifecycle + framing, wrapping serial_port.h + protocol.h.
 * Ported from sdr_controller's ConnectionController + SerialThread, minus
 * the QThread: the app polls conn_poll() from a WM_TIMER tick instead of a
 * background reader thread, since Win32 GUI apps don't need one for a link
 * this slow and it avoids all cross-thread marshaling.
 */
#pragma once
#include "serial_port.h"
#include "protocol.h"
#include <stdbool.h>

typedef struct Connection Connection;

typedef struct {
    void (*on_connected_changed)(bool connected, void *ctx);
    void (*on_frame)(const ProtoParsedFrame *frame, void *ctx);
    void (*on_raw_tx)(const uint8_t *data, uint8_t len, void *ctx);
    void (*on_raw_rx)(const uint8_t *data, uint16_t len, void *ctx);
    void (*on_error)(const char *message, void *ctx);
    void *ctx;
} ConnectionCallbacks;

struct Connection {
    SerialPort port;
    ProtoParser parser;
    bool connected;
    ConnectionCallbacks cb;
};

void conn_init(Connection *conn, ConnectionCallbacks cb);

/* Opens the port; on ERROR_ACCESS_DENIED (Windows can briefly hold a COM
 * port handle after a previous close) retries once after a short sleep,
 * matching the transient-access-error retry in the Python reference. */
bool conn_connect(Connection *conn, const char *port_name, DWORD baud, char parity, uint8_t data_bits);
void conn_disconnect(Connection *conn);
bool conn_is_connected(const Connection *conn);

bool conn_send(Connection *conn, const uint8_t *data, uint8_t len);

/* Non-blocking: drains whatever's waiting on the port, feeds it through the
 * frame parser, and fires on_raw_rx / on_frame as appropriate. Call this
 * from a timer tick. */
void conn_poll(Connection *conn);

/* Wraps serial_list_ports. */
int conn_list_ports(char names[][16], int max_ports);
