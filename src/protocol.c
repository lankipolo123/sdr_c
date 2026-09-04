/* RS422 frame protocol implementation. See protocol.h for the format. */
#include "protocol.h"
#include <string.h>
#include <stdio.h>

static void proto_frame_build(ProtoFrame *out, uint8_t type_byte, uint8_t addr,
                               const uint8_t *buf, uint8_t buf_len) {
    uint8_t i = 0;
    out->data[0] = 0x7E;
    out->data[1] = 0x7E;
    out->data[2] = type_byte;
    out->data[3] = addr;
    out->data[4] = buf_len;
    for (i = 0; i < buf_len; i++) {
        out->data[5 + i] = buf[i];
    }
    out->data[5 + buf_len] = 0x0A;
    out->data[6 + buf_len] = 0x0D;
    out->len = (uint8_t)(7 + buf_len);
}

ProtoStatus proto_build_output_switch(ProtoFrame *out, uint8_t addr, bool on) {
    uint8_t buf[1] = { on ? PROTO_OUTPUT_ON : PROTO_OUTPUT_OFF };
    proto_frame_build(out, PROTO_TYPE_OUTPUT_SWITCH, addr, buf, 1);
    return PROTO_OK;
}

ProtoStatus proto_build_signal_control(ProtoFrame *out, uint8_t addr, uint8_t mode,
                                        uint16_t freq_mhz, uint16_t bandwidth_mhz, int power_db) {
    int bw_code, pw_code;
    uint8_t buf[5];

    if (mode >= PROTO_MODE_COUNT) {
        return PROTO_ERR_MODE;
    }
    if (freq_mhz < PROTO_FREQ_MIN_MHZ || freq_mhz > PROTO_FREQ_MAX_MHZ) {
        return PROTO_ERR_FREQ_RANGE;
    }
    bw_code = proto_bandwidth_code(bandwidth_mhz);
    if (bw_code < 0) {
        return PROTO_ERR_BANDWIDTH;
    }
    pw_code = proto_power_code(power_db);
    if (pw_code < 0) {
        return PROTO_ERR_POWER;
    }

    buf[0] = mode;
    buf[1] = (uint8_t)(freq_mhz >> 8);
    buf[2] = (uint8_t)(freq_mhz & 0xFF);
    buf[3] = (uint8_t)bw_code;
    buf[4] = (uint8_t)pw_code;
    proto_frame_build(out, PROTO_TYPE_SIGNAL_CONTROL, addr, buf, 5);
    return PROTO_OK;
}

ProtoStatus proto_build_status_query(ProtoFrame *out, uint8_t addr) {
    proto_frame_build(out, PROTO_TYPE_STATUS_QUERY, addr, NULL, 0);
    return PROTO_OK;
}

ProtoStatus proto_build_addr_query(ProtoFrame *out) {
    proto_frame_build(out, PROTO_TYPE_ADDR_QUERY, PROTO_BROADCAST_ADDR, NULL, 0);
    return PROTO_OK;
}

ProtoStatus proto_build_addr_set(ProtoFrame *out, uint8_t new_addr) {
    uint8_t buf[1];
    if (new_addr > PROTO_ADDR_MAX) {
        return PROTO_ERR_ADDR_RANGE;
    }
    buf[0] = new_addr;
    proto_frame_build(out, PROTO_TYPE_ADDR_SET, PROTO_BROADCAST_ADDR, buf, 1);
    return PROTO_OK;
}

/* ---- mapping tables ---- */

static const uint16_t BANDWIDTH_MHZ[] = { 10, 20, 50, 100, 150, 200, 250, 300 };
#define BANDWIDTH_TABLE_LEN (sizeof(BANDWIDTH_MHZ) / sizeof(BANDWIDTH_MHZ[0]))
/* BANDWIDTH_MHZ[i] <-> code i, matches BANDWIDTH_CODES in constants.py */

int proto_bandwidth_code(uint16_t mhz) {
    unsigned i;
    for (i = 0; i < BANDWIDTH_TABLE_LEN; i++) {
        if (BANDWIDTH_MHZ[i] == mhz) {
            return (int)i;
        }
    }
    return -1;
}

int proto_bandwidth_mhz(uint8_t code) {
    if (code < BANDWIDTH_TABLE_LEN) {
        return (int)BANDWIDTH_MHZ[code];
    }
    return -1;
}

static const int POWER_DB[] = { 0, -6, -12 };
#define POWER_TABLE_LEN (sizeof(POWER_DB) / sizeof(POWER_DB[0]))

int proto_power_code(int db) {
    unsigned i;
    for (i = 0; i < POWER_TABLE_LEN; i++) {
        if (POWER_DB[i] == db) {
            return (int)i;
        }
    }
    return -1;
}

int proto_power_db(uint8_t code) {
    if (code < POWER_TABLE_LEN) {
        return POWER_DB[code];
    }
    return -1;
}

static const char *const MODE_NAMES[PROTO_MODE_COUNT] = {
    "Pseudo Random Noise", "Linear Sweep", "Comb Spectrum", "Continuous Wave"
};

const char *proto_mode_name(uint8_t mode) {
    if (mode < PROTO_MODE_COUNT) {
        return MODE_NAMES[mode];
    }
    return NULL;
}

/* ---- parsing ---- */

void proto_parser_init(ProtoParser *p) {
    p->len = 0;
}

/* Mirrors FrameParser._try_extract_one(): finds HEAD (discarding any garbage
 * before it), waits for a full header/frame, validates STOP, and on a bad
 * STOP discards 2 bytes and gives up for this call (matching the Python
 * port's behavior of breaking its extraction loop on a STOP mismatch). */
static bool proto_try_extract_one(ProtoParser *p, ProtoParsedFrame *out) {
    int head_idx = -1;
    int i;
    uint8_t type_byte, addr, buf_len;
    uint16_t total_len;

    for (i = 0; i + 1 < p->len; i++) {
        if (p->buf[i] == 0x7E && p->buf[i + 1] == 0x7E) {
            head_idx = i;
            break;
        }
    }
    if (head_idx == -1) {
        if (p->len > 1) {
            p->buf[0] = p->buf[p->len - 1];
            p->len = 1;
        }
        return false;
    }
    if (head_idx > 0) {
        memmove(p->buf, p->buf + head_idx, (size_t)(p->len - head_idx));
        p->len = (uint16_t)(p->len - head_idx);
    }

    if (p->len < 5) {
        return false;
    }

    type_byte = p->buf[2];
    addr = p->buf[3];
    buf_len = p->buf[4];
    total_len = (uint16_t)(5 + buf_len + 2);

    if (p->len < total_len) {
        return false;
    }

    if (p->buf[total_len - 2] != 0x0A || p->buf[total_len - 1] != 0x0D) {
        memmove(p->buf, p->buf + 2, (size_t)(p->len - 2));
        p->len = (uint16_t)(p->len - 2);
        return false;
    }

    out->type = type_byte;
    out->addr = addr;
    out->buf_len = buf_len;
    if (out->buf_len > sizeof(out->buf)) {
        out->buf_len = (uint8_t)sizeof(out->buf);
    }
    memcpy(out->buf, p->buf + 5, out->buf_len);

    out->raw_len = (uint8_t)(total_len > PROTO_MAX_FRAME ? PROTO_MAX_FRAME : total_len);
    memcpy(out->raw, p->buf, out->raw_len);

    memmove(p->buf, p->buf + total_len, (size_t)(p->len - total_len));
    p->len = (uint16_t)(p->len - total_len);
    return true;
}

void proto_parser_feed(ProtoParser *p, const uint8_t *data, uint16_t data_len,
                        void (*on_frame)(const ProtoParsedFrame *frame, void *ctx), void *ctx) {
    uint16_t offset = 0;
    ProtoParsedFrame frame;

    while (offset < data_len) {
        uint16_t space = (uint16_t)(PROTO_PARSE_BUF_SIZE - p->len);
        uint16_t take;

        if (space == 0) {
            /* Corrupt/oversized claimed frame stalled us with a full buffer;
             * drop the oldest half to recover instead of wedging forever. */
            uint16_t drop = PROTO_PARSE_BUF_SIZE / 2;
            memmove(p->buf, p->buf + drop, (size_t)(p->len - drop));
            p->len = (uint16_t)(p->len - drop);
            space = (uint16_t)(PROTO_PARSE_BUF_SIZE - p->len);
        }

        take = (uint16_t)(data_len - offset);
        if (take > space) {
            take = space;
        }
        memcpy(p->buf + p->len, data + offset, take);
        p->len = (uint16_t)(p->len + take);
        offset = (uint16_t)(offset + take);

        while (proto_try_extract_one(p, &frame)) {
            on_frame(&frame, ctx);
        }
    }
}

void proto_describe(const ProtoParsedFrame *frame, char *out, int out_size) {
    if ((frame->type == PROTO_TYPE_OUTPUT_SWITCH || frame->type == PROTO_TYPE_SIGNAL_CONTROL) &&
        frame->buf_len == 1) {
        uint8_t code = frame->buf[0];
        if (code == PROTO_RESP_SUCCESS) {
            snprintf(out, (size_t)out_size, "%s", "Control succeeded");
        } else if (code == PROTO_RESP_FAILED) {
            snprintf(out, (size_t)out_size, "%s", "Control failed");
        } else {
            snprintf(out, (size_t)out_size, "Other/unknown response code: 0x%02X", code);
        }
        return;
    }

    if (frame->type == PROTO_TYPE_STATUS_QUERY && frame->buf_len >= 6) {
        uint8_t output = frame->buf[0];
        uint8_t mode = frame->buf[1];
        uint16_t freq = (uint16_t)(((uint16_t)frame->buf[2] << 8) | frame->buf[3]);
        uint8_t bw_code = frame->buf[4];
        uint8_t pw_code = frame->buf[5];
        const char *mode_name = proto_mode_name(mode);
        int bw_mhz = proto_bandwidth_mhz(bw_code);
        int pw_db = proto_power_db(pw_code);
        char mode_buf[16];
        char bw_buf[16];
        char pw_buf[16];

        if (!mode_name) {
            snprintf(mode_buf, sizeof(mode_buf), "%u", mode);
            mode_name = mode_buf;
        }
        if (bw_mhz >= 0) {
            snprintf(bw_buf, sizeof(bw_buf), "%d", bw_mhz);
        } else {
            snprintf(bw_buf, sizeof(bw_buf), "%u", bw_code);
        }
        if (pw_db != -1) {
            snprintf(pw_buf, sizeof(pw_buf), "%d", pw_db);
        } else {
            snprintf(pw_buf, sizeof(pw_buf), "%u", pw_code);
        }

        snprintf(out, (size_t)out_size,
                 "Status: output=%s, mode=%s, freq=%uMHz, bw=%sMHz, power=%sdB",
                 output ? "ON" : "OFF", mode_name, freq, bw_buf, pw_buf);
        return;
    }

    if (frame->type == PROTO_TYPE_ADDR_QUERY && frame->buf_len == 1) {
        snprintf(out, (size_t)out_size, "Module address = %u", frame->buf[0]);
        return;
    }

    if (frame->type == PROTO_TYPE_ADDR_SET && frame->buf_len == 1) {
        uint8_t code = frame->buf[0];
        snprintf(out, (size_t)out_size, "%s", code == PROTO_RESP_SUCCESS ? "Address set OK" : "Address set failed");
        return;
    }

    snprintf(out, (size_t)out_size, "Unrecognized/short payload for type 0x%02X", frame->type);
}
