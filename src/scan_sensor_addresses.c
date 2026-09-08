/* Manual discovery tool: which Modbus slave address does each connected
 * XY-MD02 temp/humidity sensor actually answer on the wire?
 *
 * sensor.c's per-unit mode (unit_addr[]) needs each unit's real wired
 * address before it's useful, and real wiring may not be sequential
 * (see sensor.h's own comment on this). Run this with one sensor
 * connected at a time (or several sharing the RS-485 bus) and it reports
 * which address(es) actually respond, plus a live reading as a sanity
 * check that it's a real sensor and not noise.
 *
 * Separate console-mode .exe from the GUI app - reuses modbus.c and
 * serial_port.c directly, same connection settings sensor.c uses
 * (9600 8N1). Build: see build_scan.bat.
 *
 * Usage: scan_sensor_addresses.exe COM5 [start] [end]
 *   start/end default to 1..16 (MAX_CHANNELS).
 */
#include "modbus.h"
#include "serial_port.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#define SCAN_BAUD 9600
#define SCAN_PARITY 'N'
#define SCAN_DATABITS 8
#define SCAN_START_REGISTER 1
#define SCAN_REGISTER_COUNT 2
#define SCAN_RESPONSE_TIMEOUT_MS 500
#define SCAN_DEFAULT_START 1
#define SCAN_DEFAULT_END   16

/* Blocks (Sleep-polling serial_read, which itself never blocks) until a
 * full response is parsed, a timeout elapses, or the device answers with
 * a Modbus exception. This is a plain sequential console tool, not the
 * GUI app's WM_TIMER state machine - blocking here is fine, there's no
 * message loop to freeze. */
static bool poll_one_address(SerialPort *sp, uint8_t addr, ModbusReading *out) {
    ModbusFrame req;
    uint8_t buf[64];
    uint16_t buf_len = 0;
    DWORD deadline;

    modbus_build_read_input_registers(&req, addr, SCAN_START_REGISTER, SCAN_REGISTER_COUNT);
    if (!serial_write(sp, req.data, req.len, NULL)) {
        return false;
    }

    deadline = GetTickCount() + SCAN_RESPONSE_TIMEOUT_MS;
    for (;;) {
        DWORD read_len = 0;
        uint16_t consumed = 0;
        ModbusParseResult result;

        if (buf_len < sizeof(buf)) {
            if (!serial_read(sp, buf + buf_len, (DWORD)(sizeof(buf) - buf_len), &read_len)) {
                return false;
            }
            buf_len = (uint16_t)(buf_len + read_len);
        }

        result = modbus_parse_read_input_registers_response(buf, buf_len, out, &consumed);
        if (result == MODBUS_PARSE_OK) {
            return true;
        }
        if (result == MODBUS_PARSE_EXCEPTION || result == MODBUS_PARSE_BAD_CRC) {
            return false;
        }

        if ((int32_t)(GetTickCount() - deadline) >= 0) {
            return false; /* timed out - nothing answered */
        }
        Sleep(10);
    }
}

int main(int argc, char **argv) {
    SerialPort sp;
    int start = SCAN_DEFAULT_START;
    int end = SCAN_DEFAULT_END;
    int addr;
    int found_count = 0;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s COM5 [start] [end]\n", argv[0]);
        return 1;
    }
    if (argc >= 3) start = atoi(argv[2]);
    if (argc >= 4) end = atoi(argv[3]);

    if (!serial_open(&sp, argv[1], SCAN_BAUD, SCAN_PARITY, SCAN_DATABITS)) {
        fprintf(stderr, "Failed to open %s\n", argv[1]);
        return 1;
    }

    printf("Scanning addresses %d-%d on %s...\n\n", start, end, argv[1]);
    for (addr = start; addr <= end; addr++) {
        ModbusReading reading;
        if (poll_one_address(&sp, (uint8_t)addr, &reading)) {
            printf("  address %3d: FOUND  - %.1f C, %.1f%% RH\n",
                   addr, reading.registers[0] / 10.0, reading.registers[1] / 10.0);
            found_count++;
        } else {
            printf("  address %3d: no reply\n", addr);
        }
        Sleep(50); /* let the bus settle between addresses */
    }

    serial_close(&sp);
    printf("\n%d sensor(s) responded.\n", found_count);
    return 0;
}
