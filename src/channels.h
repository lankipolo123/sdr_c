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
 * Frequency/bandwidth are NOT per-channel controls here (matching the
 * reference apps again) - every Signal Control frame uses the fixed
 * CHANNEL_BLIND_FREQ_MHZ/CHANNEL_BLIND_BANDWIDTH_MHZ. Only mode and
 * power level are real per-channel selections.
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

#define CHANNEL_BLIND_FREQ_MHZ      2450
#define CHANNEL_BLIND_BANDWIDTH_MHZ 100

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

/* Restores a saved mode/last_level straight into channel state (no serial
 * send, no output_on change) - for loading .ini settings at startup. */
void channel_restore_saved(int index, uint8_t mode, int last_level);

void channel_turn_output_on(int index);
void channel_turn_output_off(int index);
void channel_set_level(int index, int level); /* LEVEL_OFF turns output off */
void channel_set_mode(int index, uint8_t mode);

/* Call every timer tick: starts the next queued send if the bus is free,
 * and applies a settled send's state once its settle delay has passed. */
void channels_poll(void);
