#include "sensor.h"
#include "modbus.h"
#include <string.h>

void sensor_init(Sensor *s) {
    memset(s, 0, sizeof(*s));
    s->port.handle = INVALID_HANDLE_VALUE;
    s->poll_state = SENSOR_POLL_IDLE;
}

bool sensor_connect(Sensor *s, const char *port_name, DWORD baud, char parity, uint8_t data_bits) {
    if (!serial_open(&s->port, port_name, baud, parity, data_bits)) {
        if (GetLastError() == ERROR_ACCESS_DENIED) {
            Sleep(300);
            if (!serial_open(&s->port, port_name, baud, parity, data_bits)) {
                return false;
            }
        } else {
            return false;
        }
    }

    s->connected = true;
    s->poll_state = SENSOR_POLL_IDLE;
    s->next_poll_at = GetTickCount(); /* poll right away, don't wait a full interval first */
    s->rx_len = 0;
    s->state.online = false;
    s->state.has_reading = false;
    s->state.connected = true;
    s->state.attempt_count = 0;
    s->state.last_rx_len = 0;
    return true;
}

void sensor_disconnect(Sensor *s) {
    serial_close(&s->port);
    s->connected = false;
    s->poll_state = SENSOR_POLL_IDLE;
    s->rx_len = 0;
    /* Reset to unknown rather than leaving a stale reading on screen -
     * matches this app family's "never show a value we can't currently
     * vouch for" rule. */
    s->state.connected = false;
    s->state.online = false;
    s->state.has_reading = false;
    s->state.attempt_count = 0;
    s->state.last_rx_len = 0;
}

bool sensor_is_connected(const Sensor *s) {
    return s->connected && serial_is_open(&s->port);
}

const SensorState *sensor_get_state(const Sensor *s) {
    return &s->state;
}

static void sensor_send_request(Sensor *s) {
    ModbusFrame frame;
    modbus_build_read_input_registers(&frame, SENSOR_SLAVE_ADDR, SENSOR_START_REGISTER, SENSOR_REGISTER_COUNT);
    s->rx_len = 0;
    s->state.attempt_count++;
    if (!serial_write(&s->port, frame.data, frame.len, NULL)) {
        /* Couldn't even send - try again next interval rather than
         * spinning immediately. */
        s->poll_state = SENSOR_POLL_IDLE;
        s->next_poll_at = GetTickCount() + SENSOR_POLL_INTERVAL_MS;
        s->state.online = false;
        return;
    }
    s->poll_state = SENSOR_POLL_WAITING;
    s->response_deadline = GetTickCount() + SENSOR_RESPONSE_TIMEOUT_MS;
}

static void sensor_finish_cycle(Sensor *s, bool got_valid_reply) {
    s->state.last_rx_len = s->rx_len;
    s->rx_len = 0;
    s->poll_state = SENSOR_POLL_IDLE;
    s->next_poll_at = GetTickCount() + SENSOR_POLL_INTERVAL_MS;
    s->state.online = got_valid_reply;
}

void sensor_poll(Sensor *s) {
    DWORD now;
    DWORD read_len = 0;

    if (!sensor_is_connected(s)) {
        return;
    }

    if (s->poll_state == SENSOR_POLL_WAITING) {
        if (s->rx_len < sizeof(s->rx_buf)) {
            if (serial_read(&s->port, s->rx_buf + s->rx_len,
                             (DWORD)(sizeof(s->rx_buf) - s->rx_len), &read_len)) {
                s->rx_len = (uint16_t)(s->rx_len + read_len);
            }
        }

        {
            ModbusReading reading;
            uint16_t consumed = 0;
            ModbusParseResult result = modbus_parse_read_input_registers_response(
                s->rx_buf, s->rx_len, &reading, &consumed);

            if (result == MODBUS_PARSE_OK) {
                s->state.temperature_c = reading.registers[0] / 10.0f;
                s->state.humidity_pct = reading.registers[1] / 10.0f;
                s->state.has_reading = true;
                sensor_finish_cycle(s, true);
                return;
            }
            if (result == MODBUS_PARSE_EXCEPTION || result == MODBUS_PARSE_BAD_CRC) {
                sensor_finish_cycle(s, false);
                return;
            }
            /* MODBUS_PARSE_INCOMPLETE - keep waiting, unless we've timed out. */
        }

        now = GetTickCount();
        if ((int32_t)(now - s->response_deadline) >= 0) {
            sensor_finish_cycle(s, false);
        }
        return;
    }

    /* SENSOR_POLL_IDLE */
    now = GetTickCount();
    if ((int32_t)(now - s->next_poll_at) >= 0) {
        sensor_send_request(s);
    }
}
