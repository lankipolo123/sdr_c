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

/* Restores a saved mode/level from the .ini directly into channel state,
 * without going through channel_set_mode()/channel_set_level() - those
 * queue a real serial send, which would be wrong here: there's no
 * connection open yet at load time, and even once connected, the whole
 * point of a saved setting is to be ready without transmitting anything
 * until the user explicitly presses ON (never auto-resume RF output on
 * launch). Leaves output_on/level at their already-initialized OFF
 * state - only last_level (what ON will resume to) and mode change. */
void channel_restore_saved(int index, uint8_t mode, int last_level) {
    if (last_level < LEVEL_LOW || last_level > LEVEL_HIGH) {
        return;
    }
    if (mode >= PROTO_MODE_COUNT) {
        return;
    }
    g_channels[index].mode = mode;
    g_channels[index].last_level = last_level;
}

const ChannelState *channels_get(int index) {
    return &g_channels[index];
}

static int level_to_power_db(int level) {
    switch (level) {
        case LEVEL_LOW:    return -12;
        case LEVEL_MEDIUM: return -6;
        case LEVEL_HIGH:   return 0;
        default:           return 0;
    }
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

    power_db = level_to_power_db(level);
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
                                CHANNEL_BLIND_FREQ_MHZ, CHANNEL_BLIND_BANDWIDTH_MHZ, power_db);
    wsprintfA(label, "Level -> %d", level);
    enqueue(index, &frame, label, true, true, true, level, false, 0);
    ch->last_level = level;
}

void channel_set_mode(int index, uint8_t mode) {
    ChannelState *ch = &g_channels[index];
    int level = (ch->level != LEVEL_OFF) ? ch->level : ch->last_level;
    int power_db = level_to_power_db(level);
    ProtoFrame frame;
    char label[48];
    const char *mode_name;

    proto_build_signal_control(&frame, ch->address, mode,
                                CHANNEL_BLIND_FREQ_MHZ, CHANNEL_BLIND_BANDWIDTH_MHZ, power_db);
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
