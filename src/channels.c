#include "channels.h"
#include <string.h>

typedef struct {
    int channel_index;
    ProtoFrame frame;
    char label[48];

    bool set_output;
    bool output_value;
    bool set_level;
    int level_value;
    bool set_mode;
    uint8_t mode_value;
} SendRequest;

#define QUEUE_CAP 32

static Connection *g_conn;
static ChannelState g_channels[MAX_CHANNELS];

static SendRequest g_queue[QUEUE_CAP];
static int g_queue_head;
static int g_queue_len;

static bool g_inflight;
static DWORD g_settle_deadline;
static SendRequest g_inflight_req;

void channels_init(Connection *conn) {
    int i;
    g_conn = conn;
    g_queue_head = 0;
    g_queue_len = 0;
    g_inflight = false;

    for (i = 0; i < MAX_CHANNELS; i++) {
        g_channels[i].address = (uint8_t)(i + 1);
        g_channels[i].output_on = false;
        g_channels[i].level = LEVEL_OFF;
        g_channels[i].last_level = LEVEL_LOW;
        g_channels[i].mode = PROTO_MODE_WHITE_NOISE;
        g_channels[i].busy = false;
        g_channels[i].unconfirmed = false;
        lstrcpynA(g_channels[i].last_command, "-", (int)sizeof(g_channels[i].last_command));
    }
}

/* Restores a saved mode/level/output_on from the .ini directly into
 * channel state, without going through channel_set_mode()/
 * channel_turn_output_on() - those queue a real serial send, which would
 * be wrong here: there's no connection open yet at load time, and this
 * is just the app remembering what it already believed last time, not
 * commanding anything new. Matches the reference app's channelStore -
 * restoring output_on into state is NOT the same as re-arming RF: the
 * amplifier hardware holds its own last commanded state independently,
 * so this only makes the UI honest about what's actually still running
 * out there, without sending a single byte to get there. */
void channel_restore_saved(int index, uint8_t mode, int last_level, bool output_on) {
    if (last_level < LEVEL_LOW || last_level > LEVEL_HIGH) {
        return;
    }
    if (mode >= PROTO_MODE_COUNT) {
        return;
    }
    g_channels[index].mode = mode;
    g_channels[index].last_level = last_level;
    g_channels[index].output_on = output_on;
    g_channels[index].level = output_on ? last_level : LEVEL_OFF;
}

const ChannelState *channels_get(int index) {
    return &g_channels[index];
}

int channel_level_power_db(int level) {
    switch (level) {
        case LEVEL_LOW:    return -12;
        case LEVEL_MEDIUM: return -6;
        case LEVEL_HIGH:   return 0;
        default:           return 0;
    }
}

/* Each channel's real, fixed operating band (Unit 1..16), given as
 * exact low-high MHz ranges:
 *   1: 703-803    5: 1710-1880   9: 2400-2500   13: 5150-5350
 *   2: 824-894    6: 1920-2170  10: 3300-3450   14: 5350-5550
 *   3: 880-960    7: 2300-2350  11: 3450-3650   15: 5550-5750
 *   4: 1427-1512  8: 2350-2400  12: 3650-3800   16: 5750-6000
 * CHANNEL_FREQ_MHZ is each range's center (rounded to the nearest MHz
 * where the range is an odd width, e.g. Unit 4's 1427-1512 centers on
 * 1469.5, sent as 1470) - confirmed real values, not a shared guess
 * like the old CHANNEL_BLIND_FREQ_MHZ default this replaced. */
static const int CHANNEL_FREQ_MHZ[MAX_CHANNELS] = {
    753, 859, 920, 1470, 1795, 2045, 2325, 2375,
    2450, 3375, 3550, 3725, 5250, 5450, 5650, 5875
};

int channel_freq_mhz(int index) {
    return CHANNEL_FREQ_MHZ[index];
}

/* Each channel's real bandwidth (the range widths above), rounded to
 * the nearest of the protocol's 8 supported codes (see
 * proto_bandwidth_code() in protocol.c) where the real width isn't
 * exactly one of them - the hardware has no code for an arbitrary
 * width, and proto_build_signal_control() rejects anything that
 * doesn't match the table exactly. Units 2/3/4/5 are the ones that
 * needed rounding:
 *   1: 100 (exact)        5: 170 -> 150 (real range 1710-1880)
 *   2: 70  -> 50 (real range 824-894)     6: 250 (exact)
 *   3: 80  -> 100 (real range 880-960)    7-16: all exact
 *   4: 85  -> 100 (real range 1427-1512)
 * so units 2/3/4/5 transmit a band slightly narrower/wider than their
 * true range - flagged here rather than silently rounded, since it's
 * a real (if small) mismatch from the actual hardware band. */
static const int CHANNEL_BANDWIDTH_MHZ[MAX_CHANNELS] = {
    100, 50, 100, 100, 150, 250, 50, 50,
    100, 150, 200, 150, 200, 200, 200, 250
};

int channel_bandwidth_mhz(int index) {
    return CHANNEL_BANDWIDTH_MHZ[index];
}

static void enqueue(int index, const ProtoFrame *frame, const char *label,
                     bool set_output, bool output_value,
                     bool set_level, int level_value,
                     bool set_mode, uint8_t mode_value) {
    int tail;
    SendRequest *req;

    if (g_queue_len >= QUEUE_CAP) {
        return; /* queue exhausted - shouldn't happen at 16 channels in practice */
    }
    tail = (g_queue_head + g_queue_len) % QUEUE_CAP;
    req = &g_queue[tail];
    req->channel_index = index;
    req->frame = *frame;
    lstrcpynA(req->label, label, (int)sizeof(req->label));
    req->set_output = set_output;
    req->output_value = output_value;
    req->set_level = set_level;
    req->level_value = level_value;
    req->set_mode = set_mode;
    req->mode_value = mode_value;
    g_queue_len++;

    g_channels[index].busy = true;
}

void channel_turn_output_on(int index) {
    ProtoFrame frame;
    ChannelState *ch = &g_channels[index];
    proto_build_output_switch(&frame, ch->address, true);
    enqueue(index, &frame, "Output ON", true, true, true, ch->last_level, false, 0);
}

void channel_turn_output_off(int index) {
    ProtoFrame frame;
    ChannelState *ch = &g_channels[index];
    proto_build_output_switch(&frame, ch->address, false);
    enqueue(index, &frame, "Output OFF", true, false, true, LEVEL_OFF, false, 0);
}

void channel_set_level(int index, int level) {
    ChannelState *ch = &g_channels[index];
    int power_db;
    int power_code;
    ProtoFrame frame;
    char label[48];

    if (level == LEVEL_OFF) {
        channel_turn_output_off(index);
        return;
    }

    power_db = channel_level_power_db(level);
    power_code = proto_power_code(power_db);
    (void)power_code; /* proto_build_signal_control re-derives this itself */

    if (!ch->output_on) {
        /* Signal Control alone doesn't re-enable RF output on this
         * hardware (confirmed in the reference app) - queue an explicit
         * Output ON first, same bus, same order. */
        ProtoFrame on_frame;
        proto_build_output_switch(&on_frame, ch->address, true);
        enqueue(index, &on_frame, "Output ON (resume)", true, true, false, 0, false, 0);
    }

    proto_build_signal_control(&frame, ch->address, ch->mode,
                                (uint16_t)channel_freq_mhz(index), (uint16_t)channel_bandwidth_mhz(index), power_db);
    wsprintfA(label, "Level -> %d", level);
    enqueue(index, &frame, label, true, true, true, level, false, 0);
    ch->last_level = level;
}

void channel_set_mode(int index, uint8_t mode) {
    ChannelState *ch = &g_channels[index];
    int level = (ch->level != LEVEL_OFF) ? ch->level : ch->last_level;
    int power_db = channel_level_power_db(level);
    ProtoFrame frame;
    char label[48];
    const char *mode_name;

    proto_build_signal_control(&frame, ch->address, mode,
                                (uint16_t)channel_freq_mhz(index), (uint16_t)channel_bandwidth_mhz(index), power_db);
    mode_name = proto_mode_name(mode);
    wsprintfA(label, "Mode -> %s", mode_name ? mode_name : "?");
    enqueue(index, &frame, label, false, false, false, 0, true, mode);
}

void channels_poll(void) {
    if (g_inflight) {
        if ((int32_t)(GetTickCount() - g_settle_deadline) >= 0) {
            ChannelState *ch = &g_channels[g_inflight_req.channel_index];
            if (g_inflight_req.set_output) {
                ch->output_on = g_inflight_req.output_value;
            }
            if (g_inflight_req.set_level) {
                ch->level = g_inflight_req.level_value;
            }
            if (g_inflight_req.set_mode) {
                ch->mode = g_inflight_req.mode_value;
            }
            ch->busy = false;
            ch->unconfirmed = true;
            lstrcpynA(ch->last_command, g_inflight_req.label, (int)sizeof(ch->last_command));
            g_inflight = false;
        }
        return;
    }

    if (g_queue_len > 0) {
        SendRequest *req = &g_queue[g_queue_head];
        conn_send(g_conn, req->frame.data, req->frame.len);
        g_inflight_req = *req;
        g_inflight = true;
        g_settle_deadline = GetTickCount() + CHANNEL_SEND_SETTLE_MS;
        g_queue_head = (g_queue_head + 1) % QUEUE_CAP;
        g_queue_len--;
    }
}
