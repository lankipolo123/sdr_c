#include "modbus.h"

uint16_t modbus_crc16(const uint8_t *data, int len) {
    uint16_t crc = 0xFFFF;
    int i, j;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001);
            } else {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }
    return crc;
}

void modbus_build_read_input_registers(ModbusFrame *out, uint8_t slave_addr,
                                        uint16_t start_addr, uint16_t count) {
    uint16_t crc;

    out->data[0] = slave_addr;
    out->data[1] = MODBUS_FUNC_READ_INPUT_REGISTERS;
    out->data[2] = (uint8_t)(start_addr >> 8);
    out->data[3] = (uint8_t)(start_addr & 0xFF);
    out->data[4] = (uint8_t)(count >> 8);
    out->data[5] = (uint8_t)(count & 0xFF);

    crc = modbus_crc16(out->data, 6);
    out->data[6] = (uint8_t)(crc & 0xFF); /* CRC goes low byte first on the wire */
    out->data[7] = (uint8_t)(crc >> 8);
    out->len = 8;
}

ModbusParseResult modbus_parse_read_input_registers_response(
    const uint8_t *buf, uint16_t len, ModbusReading *out, uint16_t *consumed) {
    uint8_t byte_count;
    uint16_t frame_len;
    uint16_t crc_calc, crc_recv;
    int i;

    if (len < 2) {
        return MODBUS_PARSE_INCOMPLETE;
    }

    if (buf[1] == (MODBUS_FUNC_READ_INPUT_REGISTERS | MODBUS_EXCEPTION_FLAG)) {
        frame_len = 5;
        if (len < frame_len) {
            return MODBUS_PARSE_INCOMPLETE;
        }
        crc_calc = modbus_crc16(buf, 3);
        crc_recv = (uint16_t)buf[3] | ((uint16_t)buf[4] << 8);
        if (crc_calc != crc_recv) {
            return MODBUS_PARSE_BAD_CRC;
        }
        out->exception_code = buf[2];
        out->register_count = 0;
        *consumed = frame_len;
        return MODBUS_PARSE_EXCEPTION;
    }

    if (buf[1] != MODBUS_FUNC_READ_INPUT_REGISTERS) {
        /* Not a reply to the request we sent (wrong function code echoed
         * back) - nothing usable here, and we don't know how many bytes
         * to skip, so treat it as unusable rather than guess. */
        return MODBUS_PARSE_BAD_CRC;
    }

    if (len < 3) {
        return MODBUS_PARSE_INCOMPLETE;
    }
    byte_count = buf[2];
    frame_len = (uint16_t)(3 + byte_count + 2);
    if (frame_len > MODBUS_MAX_FRAME || byte_count > MODBUS_MAX_REGISTERS * 2) {
        return MODBUS_PARSE_BAD_CRC; /* nonsense byte count - not our frame */
    }
    if (len < frame_len) {
        return MODBUS_PARSE_INCOMPLETE;
    }

    crc_calc = modbus_crc16(buf, (int)(3 + byte_count));
    crc_recv = (uint16_t)buf[3 + byte_count] | ((uint16_t)buf[3 + byte_count + 1] << 8);
    if (crc_calc != crc_recv) {
        return MODBUS_PARSE_BAD_CRC;
    }

    out->register_count = (uint8_t)(byte_count / 2);
    for (i = 0; i < out->register_count; i++) {
        out->registers[i] = ((uint16_t)buf[3 + i * 2] << 8) | (uint16_t)buf[3 + i * 2 + 1];
    }
    *consumed = frame_len;
    return MODBUS_PARSE_OK;
}
