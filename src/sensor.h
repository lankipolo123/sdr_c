/* XY-MD02 temperature/humidity sensor(s) over Modbus RTU, on their own
 * COM port completely independent of the RS-422 channel control
 * connection.
 *
 * NOT one sensor per RF channel - there are 6 physical sensor units
 * total, scanning the rack area collectively, independent of the 16 RF
 * channels (SENSOR_MAX_UNITS is deliberately its own constant, not tied
 * to MAX_CHANNELS). Each of the 6 has its own configured Modbus address
 * (defaults to unit number 1-6, but real wiring may not be sequential -
 * see sensor_set_unit_address()). Every unit's slot holds only its own
 * reading, polled round-robin.
 *
 * Unlike channels.c's blind send (fire once, apply optimistically), a
 * register read genuinely needs the reply - there's no value to show
 * without it - so this waits for a real response, same shape as the
 * single-channel app's device.c. Still non-blocking / no threads: driven
 * off the same WM_TIMER tick as the rest of this app, one send in flight
 * at a time.
 *
 * Settings confirmed against the real hardware via QModMaster (see
 * PLAN_temp_sensor.md) - not guessed: function 0x04, register 1, count
 * 2, both raw/10. The slave address for per-unit mode (1-6) has not
 * itself been confirmed against real per-unit hardware - only the
 * single-sensor-at-address-1 case has been.
 */
#pragma once
#include "serial_port.h"
#include <stdbool.h>

#define SENSOR_MAX_UNITS 6 /* 6 physical sensors scanning the rack area -
                             * independent of MAX_CHANNELS (16 RF
                             * channels), not one-to-one with them */
/* QModMaster's status bar showed "Base Addr: 1" throughout - its Start
 * Address field is very likely 1-based display over a 0-based wire
 * address, meaning its "Start Address: 2" (which worked) actually put
 * wire address 1 on the bus, not 2. Real hardware testing of address 2
 * got a 5-byte reply (the exact length of a Modbus exception frame,
 * i.e. a real "invalid register" answer, not a timeout) - consistent
 * with this off-by-one theory. Confirmed working against real hardware
 * with a single sensor at address 1. */
#define SENSOR_START_REGISTER  1
#define SENSOR_REGISTER_COUNT  2
#define SENSOR_RESPONSE_TIMEOUT_MS 500
#define SENSOR_PER_UNIT_GAP_MS     150  /* gap between finishing one unit
                                          * and moving to the next - round-
                                          * robins continuously rather than
                                          * waiting a full interval per
                                          * unit, or a full 16-unit round
                                          * would take too long to notice
                                          * an overtemp */

typedef struct {
    bool online;       /* true once a request has actually gotten a valid reply */
    bool has_reading;  /* false until a real reading has confirmed a value -
                         * the UI shows "-" rather than a guessed default
                         * until this is true, same rule used elsewhere in
                         * this app family. */
    float temperature_c;
    float humidity_pct;

    /* Diagnostics, so a stuck "waiting" state is debuggable without a
     * separate tool: how many requests have been sent, and how many
     * bytes came back on the most recent one (0 means truly nothing
     * replied - a wiring/adapter issue - vs >0 meaning something
     * answered but didn't parse as a valid response). */
    int attempt_count;
    uint16_t last_rx_len;
} SensorState;

typedef struct Sensor Sensor;

void sensor_init(Sensor *s);
bool sensor_connect(Sensor *s, const char *port_name, DWORD baud, char parity, uint8_t data_bits);
void sensor_disconnect(Sensor *s);
bool sensor_is_connected(const Sensor *s);

/* unit_index is 0-based (0..SENSOR_MAX_UNITS-1) - one of the 6 physical
 * sensor units, not an RF channel index. */
const SensorState *sensor_get_state(const Sensor *s, int unit_index);

/* Which Modbus slave address unit_index's own temperature sensor is
 * wired to. Defaults to unit_index + 1 (sensor_init()); real wiring may
 * not be sequential, so this is settable per unit. Takes effect on that
 * unit's next poll - doesn't interrupt one already in flight. */
void sensor_set_unit_address(Sensor *s, int unit_index, uint8_t addr);
uint8_t sensor_get_unit_address(const Sensor *s, int unit_index);

/* Rack-wide summary: the mean temperature across every unit that
 * currently has a real reading (has_reading true) - units still waiting
 * on their first reply don't skew it. Returns false (leaves *out_avg_c
 * untouched) if no unit has a reading yet, same "don't show a value we
 * can't vouch for" rule as everything else here. */
bool sensor_average_temperature(const Sensor *s, float *out_avg_c);

/* Non-blocking: call every timer tick. Advances the send/wait state
 * machine and applies a completed reading (or marks offline on
 * timeout/error) - never blocks waiting on the port. */
void sensor_poll(Sensor *s);

/* Full definition here (not opaque) so callers can declare a plain
 * static Sensor, same as Connection's style in this app - callers should
 * still only touch it through the functions above. */
struct Sensor {
    SerialPort port;
    bool connected;
    int current_unit; /* 0-based; which unit's address sensor_send_request()
                        * queries next */

    enum { SENSOR_POLL_IDLE, SENSOR_POLL_WAITING } poll_state;
    DWORD next_poll_at;       /* GetTickCount() deadline, valid when idle */
    DWORD response_deadline;  /* GetTickCount() deadline, valid when waiting */

    uint8_t rx_buf[64];
    uint16_t rx_len;

    uint8_t unit_addr[SENSOR_MAX_UNITS]; /* configured Modbus slave
                                           * address per unit - defaults
                                           * to unit_index + 1 */
    SensorState units[SENSOR_MAX_UNITS];
};
