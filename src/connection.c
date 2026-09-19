#include "connection.h"
#include "serial_port.h"
#include <string.h>

#define TRANSIT_DLL_PATH "dll\\Transit.dll"
#define TRANSIT_BUF_SIZE 256

static bool dll_buf_says_connected(const char *buf) {
    return lstrcmpiA(buf, "Connected") == 0;
}

void conn_init(Connection *conn, ConnectionCallbacks cb) {
    memset(conn, 0, sizeof(*conn));
    conn->cb = cb;
    proto_parser_init(&conn->parser);
}

bool conn_connect(Connection *conn, const char *port_name, DWORD baud, char parity, uint8_t data_bits) {
    char buf[TRANSIT_BUF_SIZE];
    long result;

    /* Transit.dll auto-discovers the RS422 dongle itself - see
     * connection.h's comment on why these stay unused. */
    (void)port_name;
    (void)baud;
    (void)parity;
    (void)data_bits;

    if (!transit_dll_is_loaded(&conn->dll) && !transit_dll_load(&conn->dll, TRANSIT_DLL_PATH)) {
        if (conn->cb.on_error) {
            /* The generic "not found/loadable" message alone left every
             * real-world failure (missing file, wrong architecture, a
             * missing dependency like the VC++ Redistributable Transit.dll
             * itself needs, or a genuinely incompatible DLL) looking
             * identical - direct report of exactly that ambiguity on real
             * hardware. FormatMessageA turns the actual Win32 reason
             * transit_dll_load() captured (see TransitDll.last_error) into
             * readable text instead of just a bare error number. */
            char msg[256];
            char reason[160];
            DWORD err = conn->dll.last_error;
            DWORD n;

            n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                NULL, err, 0, reason, (DWORD)sizeof(reason), NULL);
            if (n == 0) {
                lstrcpynA(reason, "unknown reason", (int)sizeof(reason));
            } else {
                while (n > 0 && (reason[n - 1] == '\r' || reason[n - 1] == '\n')) {
                    reason[--n] = '\0';
                }
            }
            wsprintfA(msg, "Transit.dll not found/loadable (error %lu: %s)", err, reason);
            conn->cb.on_error(msg, conn->cb.ctx);
        }
        return false;
    }

    ZeroMemory(buf, sizeof(buf));
    result = conn->dll.auto_connect_sdr(buf, (long)sizeof(buf));
    conn->connected = dll_buf_says_connected(buf);

    if (conn->connected) {
        proto_parser_init(&conn->parser);
    } else if (conn->cb.on_error) {
        char msg[160];
        wsprintfA(msg, "AutoConnectSDR: not connected (return=%ld, buffer=\"%s\")", result, buf);
        conn->cb.on_error(msg, conn->cb.ctx);
    }

    if (conn->cb.on_connected_changed) {
        conn->cb.on_connected_changed(conn->connected, conn->cb.ctx);
    }
    return conn->connected;
}

void conn_disconnect(Connection *conn) {
    char buf[TRANSIT_BUF_SIZE];
    if (transit_dll_is_loaded(&conn->dll)) {
        ZeroMemory(buf, sizeof(buf));
        conn->dll.disconnect_sdr(buf, (long)sizeof(buf));
    }
    conn->connected = false;
    if (conn->cb.on_connected_changed) {
        conn->cb.on_connected_changed(false, conn->cb.ctx);
    }
}

bool conn_is_connected(const Connection *conn) {
    return conn->connected;
}

/* One byte at a time, token-translated - the confirmed real mechanism
 * (see middleware.py's dll_send_command): CommandTokens looks each byte
 * up in the DLL's own translation table; an unmapped byte ("??") falls
 * back to its 2-digit hex text instead, matching the reference exactly
 * (frequency bytes mostly fall outside the small token table). */
bool conn_send(Connection *conn, const uint8_t *data, uint8_t len) {
    int i;

    if (!conn_is_connected(conn)) {
        if (conn->cb.on_error) {
            conn->cb.on_error("Cannot send: not connected", conn->cb.ctx);
        }
        return false;
    }

    for (i = 0; i < len; i++) {
        char hex[4];
        char token[TRANSIT_BUF_SIZE];

        wsprintfA(hex, "%02X", data[i]);
        ZeroMemory(token, sizeof(token));
        conn->dll.command_tokens(hex, token, (long)sizeof(token));

        if (token[0] == '\0' || (token[0] == '?' && token[1] == '?' && token[2] == '\0')) {
            lstrcpynA(token, hex, (int)sizeof(token));
        }
        conn->dll.send_command_to_sdr(token, (long)lstrlenA(token));
    }

    if (conn->cb.on_raw_tx) {
        conn->cb.on_raw_tx(data, len, conn->cb.ctx);
    }
    return true;
}

/* No raw-read/incoming-frame equivalent exists in the confirmed DLL API -
 * this just re-checks CheckConnection so a real disconnect (dongle
 * unplugged) gets noticed instead of the UI sitting on "Connected"
 * forever. */
void conn_poll(Connection *conn) {
    char buf[TRANSIT_BUF_SIZE];

    if (!conn_is_connected(conn)) {
        return;
    }

    ZeroMemory(buf, sizeof(buf));
    conn->dll.check_connection(buf, (long)sizeof(buf));
    if (!dll_buf_says_connected(buf)) {
        if (conn->cb.on_error) {
            conn->cb.on_error("CheckConnection: link lost", conn->cb.ctx);
        }
        conn_disconnect(conn);
    }
}

int conn_list_ports(char names[][16], int max_ports) {
    return serial_list_ports(names, max_ports);
}
