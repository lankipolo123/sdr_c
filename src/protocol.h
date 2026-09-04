/* RS422 frame protocol - pure, no I/O, no Windows dependency.
 * Ported from sdr_controller/protocol/ (Python). Frame format:
 *   Head(2)=0x7E7E | Type(1) | Addr(1) | BufLen(1) | Buf(n) | Stop(2)=0x0A0D
 * All buffers are fixed-size (no malloc) - every frame this protocol
 * actually sends or receives is well under 16 bytes.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define PROTO_BROADCAST_ADDR 0xFF

#define PROTO_TYPE_OUTPUT_SWITCH  0x01
#define PROTO_TYPE_SIGNAL_CONTROL 0x02
#define PROTO_TYPE_STATUS_QUERY   0xFF
#define PROTO_TYPE_ADDR_QUERY     0xBF
#define PROTO_TYPE_ADDR_SET       0xB1

#define PROTO_OUTPUT_OFF 0x00
#define PROTO_OUTPUT_ON  0x01

#define PROTO_MODE_WHITE_NOISE   0x00
#define PROTO_MODE_LINEAR_SWEEP  0x01
#define PROTO_MODE_COMB_SPECTRUM 0x02
#define PROTO_MODE_SINGLE        0x03
#define PROTO_MODE_COUNT         4

#define PROTO_RESP_FAILED  0x01
#define PROTO_RESP_SUCCESS 0xFF

#define PROTO_FREQ_MIN_MHZ 300
#define PROTO_FREQ_MAX_MHZ 6000
#define PROTO_ADDR_MIN 0
#define PROTO_ADDR_MAX 199

#define PROTO_MAX_FRAME 16

typedef enum {
    PROTO_OK = 0,
    PROTO_ERR_ADDR_RANGE,
    PROTO_ERR_FREQ_RANGE,
    PROTO_ERR_BANDWIDTH,
    PROTO_ERR_POWER,
    PROTO_ERR_MODE,
} ProtoStatus;

typedef struct {
    uint8_t data[PROTO_MAX_FRAME];
    uint8_t len;
} ProtoFrame;

ProtoStatus proto_build_output_switch(ProtoFrame *out, uint8_t addr, bool on);
ProtoStatus proto_build_signal_control(ProtoFrame *out, uint8_t addr, uint8_t mode,
                                        uint16_t freq_mhz, uint16_t bandwidth_mhz, int power_db);
ProtoStatus proto_build_status_query(ProtoFrame *out, uint8_t addr);
ProtoStatus proto_build_addr_query(ProtoFrame *out);
ProtoStatus proto_build_addr_set(ProtoFrame *out, uint8_t new_addr);

/* mapping tables - return -1 (or NULL for names) when the value isn't one
 * of the protocol's known codes/values */
int proto_bandwidth_code(uint16_t mhz);
int proto_bandwidth_mhz(uint8_t code);
int proto_power_code(int db);
int proto_power_db(uint8_t code);
const char *proto_mode_name(uint8_t mode);

/* ---- parsing ---- */

typedef struct {
    uint8_t type;
    uint8_t addr;
    uint8_t buf[8];      /* longest real payload (status query) is 6 bytes */
    uint8_t buf_len;
    uint8_t raw[PROTO_MAX_FRAME];
    uint8_t raw_len;
} ProtoParsedFrame;

#define PROTO_PARSE_BUF_SIZE 128

typedef struct {
    uint8_t buf[PROTO_PARSE_BUF_SIZE];
    uint16_t len;
} ProtoParser;

void proto_parser_init(ProtoParser *p);

/* Feeds raw bytes in; calls on_frame once per complete frame extracted.
 * Callback-based (not a returned list) so this stays malloc-free. */
void proto_parser_feed(ProtoParser *p, const uint8_t *data, uint16_t data_len,
                        void (*on_frame)(const ProtoParsedFrame *frame, void *ctx), void *ctx);

/* Human-readable description, written into a caller-supplied buffer. */
void proto_describe(const ProtoParsedFrame *frame, char *out, int out_size);
