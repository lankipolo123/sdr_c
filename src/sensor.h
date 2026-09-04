/* XY-MD02 temperature/humidity sensor over Modbus RTU, on its own COM
 * port completely independent of the RS-422 channel control connection.
 *
 * Unlike channels.c's blind send (fire once, apply optimistically), a
 * register read genuinely needs the reply - there's no value to show
 * without it - so this waits for a real response, same shape as the
 * single-channel app's device.c. Still non-blocking / no threads: driven
 * off the same WM_TIMER tick as the rest of this app, one send in flight
 * at a time.
 *
 * Settings confirmed against the real hardware via QModMaster (see
 * PLAN_temp_sensor.md) - not guessed: slave address 1, function 0x04,
 * registers 2 (temperature) and 3 (humidity), both raw/10.
 */
#pragma once
#include "serial_port.h"
#include <stdbool.h>

#define SENSOR_SLAVE_ADDR      1
/* QModMaster's status bar showed "Base Addr: 1" throughout - its Start
 * Address field is very likely 1-based display over a 0-based wire
 * address, meaning its "Start Address: 2" (which worked) actually put
 * wire address 1 on the bus, not 2. Real hardware testing of address 2
 * got a 5-byte reply (the exact length of a Modbus exception frame,
 * i.e. a real "invalid register" answer, not a timeout) - consistent
 * with this off-by-one theory. Trying 1 here; not yet re-confirmed
 * against real hardware. */
#define SENSOR_START_REGISTER  1
#define SENSOR_REGISTER_COUNT  2
#define SENSOR_RESPONSE_TIMEOUT_MS 500
#define SENSOR_POLL_INTERVAL_MS    3000 /* temperature doesn't change fast */

typedef struct {
    bool connected;
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
const SensorState *sensor_get_state(const Sensor *s);

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

    enum { SENSOR_POLL_IDLE, SENSOR_POLL_WAITING } poll_state;
    DWORD next_poll_at;       /* GetTickCount() deadline, valid when idle */
    DWORD response_deadline;  /* GetTickCount() deadline, valid when waiting */

    uint8_t rx_buf[64];
    uint16_t rx_len;

    SensorState state;
};
