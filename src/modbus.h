/* Modbus RTU - pure, no I/O, no Windows dependency. Not a general Modbus
 * library - just the one request/response shape this app needs: function
 * 0x04 (Read Input Registers), confirmed against the real XY-MD02 sensor
 * via QModMaster (see PLAN_temp_sensor.md). Frame format:
 *   Slave(1) | Function(1) | ...payload... | CRC16(2, low byte first)
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define MODBUS_FUNC_READ_INPUT_REGISTERS 0x04
#define MODBUS_EXCEPTION_FLAG            0x80

#define MODBUS_MAX_FRAME 32
#define MODBUS_MAX_REGISTERS 8 /* far more than the 2 this app actually reads */

typedef struct {
    uint8_t data[MODBUS_MAX_FRAME];
    uint8_t len;
} ModbusFrame;

/* Modbus RTU's specific CRC16 variant (poly 0xA001, init 0xFFFF). Known
 * test vector: CRC16("01 03 00 00 00 0A") == 0xC5CD - verified in
 * test_modbus.c before this is ever pointed at a real serial port. */
uint16_t modbus_crc16(const uint8_t *data, int len);

/* Builds a Read Input Registers (0x04) request frame. */
void modbus_build_read_input_registers(ModbusFrame *out, uint8_t slave_addr,
                                        uint16_t start_addr, uint16_t count);

typedef enum {
    MODBUS_PARSE_INCOMPLETE = 0, /* not enough bytes yet - keep feeding */
    MODBUS_PARSE_OK,
    MODBUS_PARSE_BAD_CRC,
    MODBUS_PARSE_EXCEPTION,      /* device replied with a Modbus exception */
} ModbusParseResult;

typedef struct {
    uint16_t registers[MODBUS_MAX_REGISTERS];
    uint8_t register_count;
    uint8_t exception_code; /* valid only when result == MODBUS_PARSE_EXCEPTION */
} ModbusReading;

/* Tries to parse one Read Input Registers response out of buf[0..len).
 * Returns MODBUS_PARSE_INCOMPLETE if len is too short to know yet (caller
 * should keep accumulating bytes and retry) - never blocks, never reads
 * past len. On MODBUS_PARSE_OK or MODBUS_PARSE_EXCEPTION, *consumed is set
 * to how many bytes were the actual frame (so the caller can drop them and
 * keep any trailing bytes for the next parse). */
ModbusParseResult modbus_parse_read_input_registers_response(
    const uint8_t *buf, uint16_t len, ModbusReading *out, uint16_t *consumed);
