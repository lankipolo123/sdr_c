/* Verifies modbus.c's CRC16 against the standard Modbus reference vector,
 * and the request/response builders against the exact bytes proven to
 * work against the real XY-MD02 sensor via QModMaster (see
 * PLAN_temp_sensor.md) - not invented examples. */
#include "modbus.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static void print_hex(const uint8_t *data, uint16_t len) {
    uint16_t i;
    for (i = 0; i < len; i++) {
        printf("%02X%s", data[i], (uint16_t)(i + 1) < len ? " " : "");
    }
}

static void test_crc16_reference_vector(void) {
    /* Classic Modbus CRC16 worked example: request "01 03 00 00 00 0A"
     * transmits CRC bytes C5 CD (low byte first) - i.e. as a 16-bit
     * value, low=0xC5, high=0xCD -> 0xCDC5. */
    uint8_t data[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x0A };
    uint16_t crc = modbus_crc16(data, sizeof(data));
    assert(crc == 0xCDC5);
    assert((crc & 0xFF) == 0xC5);
    assert((crc >> 8) == 0xCD);
    printf("crc16 reference vector OK: 0x%04X (wire bytes %02X %02X)\n",
           crc, crc & 0xFF, crc >> 8);
}

static void test_build_read_input_registers(void) {
    /* The exact request QModMaster sent that got a real reply from the
     * sensor: slave 1, function 0x04, start address 2, count 2. */
    ModbusFrame f;
    uint8_t expected[] = { 0x01, 0x04, 0x00, 0x02, 0x00, 0x02, 0xD0, 0x0B };
    modbus_build_read_input_registers(&f, 1, 2, 2);
    assert(f.len == sizeof(expected));
    assert(memcmp(f.data, expected, sizeof(expected)) == 0);
    printf("build_read_input_registers OK: ");
    print_hex(f.data, f.len);
    printf("\n");
}

static void test_parse_real_sensor_response(void) {
    /* The exact reply QModMaster showed: register 2 = 0x0119 (281 ->
     * 28.1 degC), register 3 = 0x0167 (359 -> 35.9%). */
    uint8_t resp[] = { 0x01, 0x04, 0x04, 0x01, 0x19, 0x01, 0x67, 0x6B, 0xC5 };
    ModbusReading reading;
    uint16_t consumed = 0;
    ModbusParseResult result = modbus_parse_read_input_registers_response(
        resp, sizeof(resp), &reading, &consumed);

    assert(result == MODBUS_PARSE_OK);
    assert(consumed == sizeof(resp));
    assert(reading.register_count == 2);
    assert(reading.registers[0] == 281);
    assert(reading.registers[1] == 359);
    printf("parse_real_sensor_response OK: temp=%.1f humidity=%.1f\n",
           reading.registers[0] / 10.0, reading.registers[1] / 10.0);
}

static void test_parse_incomplete(void) {
    /* Feeding a partial frame must report INCOMPLETE, never misread it
     * as something else - this is what lets the caller's poll-driven
     * state machine just keep accumulating bytes across timer ticks. */
    uint8_t partial[] = { 0x01, 0x04, 0x04, 0x01 };
    ModbusReading reading;
    uint16_t consumed = 0;
    ModbusParseResult result = modbus_parse_read_input_registers_response(
        partial, sizeof(partial), &reading, &consumed);
    assert(result == MODBUS_PARSE_INCOMPLETE);
    printf("parse_incomplete OK\n");
}

static void test_parse_exception(void) {
    /* Function 0x04 with the exception flag (0x84) plus exception code
     * 0x02 = "Illegal Data Address" - the exact error QModMaster showed
     * while we were still sweeping for the right register address. */
    uint8_t exc[] = { 0x01, 0x84, 0x02, 0xC2, 0xC1 };
    ModbusReading reading;
    uint16_t consumed = 0;
    ModbusParseResult result = modbus_parse_read_input_registers_response(
        exc, sizeof(exc), &reading, &consumed);
    assert(result == MODBUS_PARSE_EXCEPTION);
    assert(consumed == sizeof(exc));
    assert(reading.exception_code == 0x02);
    printf("parse_exception OK: code=0x%02X\n", reading.exception_code);
}

static void test_parse_bad_crc(void) {
    uint8_t corrupted[] = { 0x01, 0x04, 0x04, 0x01, 0x19, 0x01, 0x67, 0x00, 0x00 };
    ModbusReading reading;
    uint16_t consumed = 0;
    ModbusParseResult result = modbus_parse_read_input_registers_response(
        corrupted, sizeof(corrupted), &reading, &consumed);
    assert(result == MODBUS_PARSE_BAD_CRC);
    printf("parse_bad_crc OK\n");
}

int main(void) {
    test_crc16_reference_vector();
    test_build_read_input_registers();
    test_parse_real_sensor_response();
    test_parse_incomplete();
    test_parse_exception();
    test_parse_bad_crc();
    printf("All modbus tests passed.\n");
    return 0;
}
