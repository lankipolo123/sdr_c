#include "connection.h"
#include <string.h>

void conn_init(Connection *conn, ConnectionCallbacks cb) {
    memset(conn, 0, sizeof(*conn));
    conn->port.handle = INVALID_HANDLE_VALUE;
    conn->cb = cb;
    proto_parser_init(&conn->parser);
}

bool conn_connect(Connection *conn, const char *port_name, DWORD baud, char parity, uint8_t data_bits) {
    if (serial_open(&conn->port, port_name, baud, parity, data_bits)) {
        conn->connected = true;
        proto_parser_init(&conn->parser);
        if (conn->cb.on_connected_changed) {
            conn->cb.on_connected_changed(true, conn->cb.ctx);
        }
        return true;
    }

    if (GetLastError() == ERROR_ACCESS_DENIED) {
        Sleep(300);
        if (serial_open(&conn->port, port_name, baud, parity, data_bits)) {
            conn->connected = true;
            proto_parser_init(&conn->parser);
            if (conn->cb.on_connected_changed) {
                conn->cb.on_connected_changed(true, conn->cb.ctx);
            }
            return true;
        }
    }

    if (conn->cb.on_error) {
        char msg[128];
        wsprintfA(msg, "Failed to open %s", port_name);
        conn->cb.on_error(msg, conn->cb.ctx);
    }
    return false;
}

void conn_disconnect(Connection *conn) {
    serial_close(&conn->port);
    conn->connected = false;
    if (conn->cb.on_connected_changed) {
        conn->cb.on_connected_changed(false, conn->cb.ctx);
    }
}

bool conn_is_connected(const Connection *conn) {
    return conn->connected && serial_is_open(&conn->port);
}

bool conn_send(Connection *conn, const uint8_t *data, uint8_t len) {
    if (!conn_is_connected(conn)) {
        if (conn->cb.on_error) {
            conn->cb.on_error("Cannot send: not connected", conn->cb.ctx);
        }
        return false;
    }
    if (!serial_write(&conn->port, data, len, NULL)) {
        if (conn->cb.on_error) {
            conn->cb.on_error("Write failed", conn->cb.ctx);
        }
        return false;
    }
    if (conn->cb.on_raw_tx) {
        conn->cb.on_raw_tx(data, len, conn->cb.ctx);
    }
    return true;
}

void conn_poll(Connection *conn) {
    uint8_t chunk[256];
    DWORD read_len = 0;

    if (!conn_is_connected(conn)) {
        return;
    }

    if (!serial_read(&conn->port, chunk, sizeof(chunk), &read_len)) {
        if (conn->cb.on_error) {
            conn->cb.on_error("Read failed", conn->cb.ctx);
        }
        return;
    }
    if (read_len == 0) {
        return;
    }

    if (conn->cb.on_raw_rx) {
        conn->cb.on_raw_rx(chunk, (uint16_t)read_len, conn->cb.ctx);
    }

    proto_parser_feed(&conn->parser, chunk, (uint16_t)read_len, conn->cb.on_frame, conn->cb.ctx);
}

int conn_list_ports(char names[][16], int max_ports) {
    return serial_list_ports(names, max_ports);
}
