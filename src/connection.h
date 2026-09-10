/* RS422 channel-bus connection lifecycle, backed by Transit.dll (see
 * transit_dll.h) instead of raw serial I/O - ported from sdr_app's
 * middleware.py/use_connection.py, the proven hardware-confirmed
 * reference (same DLL, same 5 confirmed exports). The app polls
 * conn_poll() from a WM_TIMER tick rather than a background thread.
 */
#pragma once
#include "transit_dll.h"
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
    TransitDll dll;
    ProtoParser parser;
    bool connected;
    ConnectionCallbacks cb;
};

void conn_init(Connection *conn, ConnectionCallbacks cb);

/* Loads Transit.dll (once, kept loaded after) and calls AutoConnectSDR -
 * it auto-discovers the RS422 dongle itself, so port_name/baud/parity/
 * data_bits are accepted only for interface compatibility (matching
 * middleware's own ConnectionController.connect() signature) and are
 * otherwise unused. Success is the returned buffer TEXT reading
 * "Connected", not the numeric return code - matching the confirmed
 * real semantics. */
bool conn_connect(Connection *conn, const char *port_name, DWORD baud, char parity, uint8_t data_bits);
void conn_disconnect(Connection *conn);
bool conn_is_connected(const Connection *conn);

/* Sends one frame - one byte at a time, each looked up via CommandTokens
 * and translated to its DLL token (falling back to 2-digit hex text for
 * an unmapped byte) before SendCommandToSDR - the confirmed real
 * mechanism, not raw bytes. */
bool conn_send(Connection *conn, const uint8_t *data, uint8_t len);

/* No raw-read/incoming-frame equivalent exists in the confirmed DLL API -
 * this just re-checks CheckConnection so a real disconnect (dongle
 * unplugged) gets noticed instead of the UI sitting on "Connected"
 * forever. Call this from a timer tick. */
void conn_poll(Connection *conn);

/* No real ports to enumerate - Transit.dll auto-discovers the RS422
 * dongle itself. Always returns a single "DLL" placeholder entry,
 * matching middleware's own list_ports() -> ["DLL"]. */
int conn_list_ports(char names[][16], int max_ports);
