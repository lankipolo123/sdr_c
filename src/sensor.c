#include "sensor.h"
#include "modbus.h"
#include <string.h>

static void sensor_reset_units(Sensor *s) {
    int i;
    for (i = 0; i < SENSOR_MAX_UNITS; i++) {
        memset(&s->units[i], 0, sizeof(s->units[i]));
    }
}

void sensor_init(Sensor *s) {
    memset(s, 0, sizeof(*s));
    s->port.handle = INVALID_HANDLE_VALUE;
    s->poll_state = SENSOR_POLL_IDLE;
    s->mode = SENSOR_MODE_SCAN;
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
    s->current_unit = 0;
    s->poll_state = SENSOR_POLL_IDLE;
    s->next_poll_at = GetTickCount(); /* poll right away, don't wait a full interval first */
    s->rx_len = 0;
    sensor_reset_units(s);
    return true;
}

void sensor_disconnect(Sensor *s) {
    serial_close(&s->port);
    s->connected = false;
    s->current_unit = 0;
    s->poll_state = SENSOR_POLL_IDLE;
    s->rx_len = 0;
    /* Reset to unknown rather than leaving a stale reading on screen -
     * matches this app family's "never show a value we can't currently
     * vouch for" rule. */
    sensor_reset_units(s);
}

bool sensor_is_connected(const Sensor *s) {
    return s->connected && serial_is_open(&s->port);
}

const SensorState *sensor_get_state(const Sensor *s, int unit_index) {
    if (unit_index < 0 || unit_index >= SENSOR_MAX_UNITS) {
        unit_index = 0;
    }
    return &s->units[unit_index];
}

SensorMode sensor_get_mode(const Sensor *s) {
    return s->mode;
}

void sensor_set_mode(Sensor *s, SensorMode mode) {
    if (s->mode == mode) {
        return;
    }
    s->mode = mode;
    s->current_unit = 0;
    sensor_reset_units(s);
    /* Re-poll right away under the new mode instead of waiting out
     * whatever interval was already in flight under the old one. */
    if (s->poll_state == SENSOR_POLL_IDLE) {
        s->next_poll_at = GetTickCount();
    }
}

static int sensor_current_slave_addr(const Sensor *s) {
    return (s->mode == SENSOR_MODE_PER_UNIT) ? (s->current_unit + 1) : SENSOR_SLAVE_ADDR;
}

static void sensor_send_request(Sensor *s) {
    ModbusFrame frame;
    int slave_addr = sensor_current_slave_addr(s);
    modbus_build_read_input_registers(&frame, (uint8_t)slave_addr, SENSOR_START_REGISTER, SENSOR_REGISTER_COUNT);
    s->rx_len = 0;
    s->units[s->current_unit].attempt_count++;
    if (!serial_write(&s->port, frame.data, frame.len, NULL)) {
        /* Unlike a Modbus timeout (the sensor just didn't answer this
         * cycle - normal, stays connected), a hard write failure means
         * the port itself is gone, most likely the USB adapter was
         * unplugged. Disconnect so the UI reflects that instead of
         * staying "Connected" against a dead handle - which is what
         * made re-plugging the adapter look like it wouldn't
         * reconnect (Connect looked like a no-op because the app
         * never noticed it had lost the port). */
        sensor_disconnect(s);
        return;
    }
    s->poll_state = SENSOR_POLL_WAITING;
    s->response_deadline = GetTickCount() + SENSOR_RESPONSE_TIMEOUT_MS;
}

/* Applies a finished cycle's result to whichever unit(s) it's for, then
 * schedules the next poll and advances current_unit (per-unit mode only -
 * scan mode always re-polls the same one address). */
static void sensor_finish_cycle(Sensor *s, bool got_valid_reply) {
    int polled_unit = s->current_unit;
    DWORD now = GetTickCount();

    s->units[polled_unit].last_rx_len = s->rx_len;
    s->units[polled_unit].online = got_valid_reply;
    s->rx_len = 0;
    s->poll_state = SENSOR_POLL_IDLE;

    if (s->mode == SENSOR_MODE_SCAN) {
        /* One shared reading for the whole rack - mirror it into every
         * unit's slot so callers displaying a specific unit's card never
         * need to know which mode is active. */
        int i;
        for (i = 0; i < SENSOR_MAX_UNITS; i++) {
            if (i != polled_unit) {
                s->units[i] = s->units[polled_unit];
            }
        }
        s->next_poll_at = now + SENSOR_POLL_INTERVAL_MS;
    } else {
        s->current_unit = (polled_unit + 1) % SENSOR_MAX_UNITS;
        s->next_poll_at = now + SENSOR_PER_UNIT_GAP_MS;
    }
}

void sensor_poll(Sensor *s) {
    DWORD now;
    DWORD read_len = 0;

    if (!sensor_is_connected(s)) {
        return;
    }

    if (s->poll_state == SENSOR_POLL_WAITING) {
        if (s->rx_len < sizeof(s->rx_buf)) {
            if (!serial_read(&s->port, s->rx_buf + s->rx_len,
                              (DWORD)(sizeof(s->rx_buf) - s->rx_len), &read_len)) {
                /* Hard read error, not just "no bytes yet" (that returns
                 * true with read_len 0) - the port is gone. Same
                 * disconnect-immediately reasoning as sensor_send_request(). */
                sensor_disconnect(s);
                return;
            }
            s->rx_len = (uint16_t)(s->rx_len + read_len);
        }

        {
            ModbusReading reading;
            uint16_t consumed = 0;
            ModbusParseResult result = modbus_parse_read_input_registers_response(
                s->rx_buf, s->rx_len, &reading, &consumed);

            if (result == MODBUS_PARSE_OK) {
                s->units[s->current_unit].temperature_c = reading.registers[0] / 10.0f;
                s->units[s->current_unit].humidity_pct = reading.registers[1] / 10.0f;
                s->units[s->current_unit].has_reading = true;
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
