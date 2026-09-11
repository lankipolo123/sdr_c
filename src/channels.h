/* 16-channel "blind send" control layer, sharing one Connection.
 *
 * Unlike the single-channel app's device.c (which sets a PENDING_* state
 * and waits up to DEVICE_RESPONSE_TIMEOUT_MS for a real ACK/NAK before
 * confirming anything changed), this sends once, no retry, and applies
 * the new state optimistically after a short settle delay - marking it
 * unconfirmed rather than blocking on/reverting from a response. This
 * matches the reference apps' (sdr_controller, sdr_react) proven
 * behavior for this specific hardware: RS422/RS485 here is a shared bus
 * with no tri-state control, so silence on a send is genuinely ambiguous,
 * not a clean fail signal - and waiting per-channel across 16 addresses
 * would make the whole panel feel unresponsive for no real gain in
 * trustworthiness.
 *
 * Frequency and bandwidth are both real per-channel values now (see
 * channel_freq_mhz()/channel_bandwidth_mhz()) - each of the 16 channels
 * has its own actual operating band, not one shared default for
 * either. Bandwidth is constrained to the hardware protocol's 8
 * supported codes (10/20/50/100/150/200/250/300 MHz - see
 * proto_bandwidth_code() in protocol.c), so a few channels' real
 * bandwidth is rounded to the nearest supported code rather than sent
 * exactly - see the comment above CHANNEL_BANDWIDTH_MHZ in channels.c
 * for exactly which ones and by how much.
 *
 * All 16 channels share one physical serial connection, so sends queue
 * through a simple FIFO - only one frame is ever in flight at a time.
 */
#pragma once
#include "connection.h"
#include "protocol.h"
#include <stdint.h>
#include <stdbool.h>

#define MAX_CHANNELS 16
#define CHANNEL_SEND_SETTLE_MS 300

#define LEVEL_OFF    0
#define LEVEL_LOW    1
#define LEVEL_MEDIUM 2
#define LEVEL_HIGH   3

typedef struct {
    uint8_t address;        /* 1..MAX_CHANNELS, matches the wire ADDR byte */
    bool output_on;
    int level;               /* LEVEL_OFF..LEVEL_HIGH */
    int last_level;          /* resume-to level on toggle-on; never LEVEL_OFF */
    uint8_t mode;             /* PROTO_MODE_* */
    bool busy;                /* a send is queued or settling for this channel */
    bool unconfirmed;         /* last applied state was optimistic, not ACKed */
    char last_command[48];
} ChannelState;

void channels_init(Connection *conn);
const ChannelState *channels_get(int index); /* index 0..MAX_CHANNELS-1 */

/* Restores a saved mode/last_level/output_on straight into channel state
 * (no serial send) - for loading .ini settings at startup. */
void channel_restore_saved(int index, uint8_t mode, int last_level, bool output_on);

void channel_turn_output_on(int index);
void channel_turn_output_off(int index);
void channel_set_level(int index, int level); /* LEVEL_OFF turns output off */
void channel_set_mode(int index, uint8_t mode);

/* The actual dBm value a level commands (LOW=-12, MEDIUM=-6, HIGH=0) -
 * exposed for UI code that needs to show the real, accurate commanded
 * power rather than just the Low/Medium/High name. */
int channel_level_power_db(int level);

/* Each channel's real, fixed operating frequency in MHz (index 0..15 =
 * Unit 1..16) - exposed for UI code that needs to show the real,
 * accurate commanded frequency. */
int channel_freq_mhz(int index);

/* Each channel's real, fixed bandwidth in MHz - like channel_freq_mhz(),
 * exposed for UI code. Rounded to the nearest of the protocol's 8
 * supported bandwidth codes where the real band's width isn't exactly
 * one of them (see the comment above CHANNEL_BANDWIDTH_MHZ in
 * channels.c). */
int channel_bandwidth_mhz(int index);

/* Call every timer tick: starts the next queued send if the bus is free,
 * and applies a settled send's state once its settle delay has passed. */
void channels_poll(void);
