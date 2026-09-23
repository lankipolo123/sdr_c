/* Shared-memory bridge between the background sensor service
 * (sensor_service.c) and the GUI (main.c) - lets the GUI show live BAY
 * readings without itself opening the sensor COM port, since the
 * service now owns that port exclusively (a serial port can't be
 * opened by two processes at once). The service writes; the GUI only
 * ever reads.
 *
 * A named file mapping, not a pipe/socket - simplest thing that works
 * for "one writer, occasional readers, small fixed-size struct,
 * neither side needs to be notified of the other's presence to keep
 * running". No explicit lock: every field is a POD type (DWORD/BOOL/
 * float) whose writes are effectively atomic on x86/x64, so a reader
 * can at worst see one tick's values as slightly torn between old and
 * new - never a crash, never worth a mutex for a display value that's
 * about to be overwritten again a second later anyway.
 *
 * The Global\ prefix (not Local\) is load-bearing, not a style choice:
 * a Windows service runs in Session 0, while the interactive GUI runs
 * in the logged-in user's own session (1 or higher, since Vista's
 * session isolation) - a Local\ name is scoped to the CREATING
 * process's own session and would simply be invisible across that
 * boundary. Global\ is the session-spanning namespace. */
#pragma once
#include <windows.h>
#include <stdbool.h>
#include "sensor.h"

#define SENSOR_SHARED_NAME "Global\\ECMControllerSensorShared"

/* Service heartbeat_tick is GetTickCount() at its last successful
 * publish - GetTickCount() counts system uptime, not anything per-
 * process, so it's directly comparable across processes. If the GUI
 * sees a heartbeat older than this, it treats the service as not
 * really alive (crashed, still starting, or a stale mapping nobody's
 * writing to anymore) and falls back to polling the port itself -
 * better than showing frozen last-known values with no indication
 * they've stopped updating. Comfortably above SENSOR_PER_UNIT_GAP_MS *
 * SENSOR_MAX_UNITS (600ms for a full round-robin) with margin for a
 * slow/loaded machine. */
#define SENSOR_SHARED_FRESH_MS 5000

typedef struct {
    DWORD heartbeat_tick;
    uint8_t unit_addr[SENSOR_MAX_UNITS];
    BOOL online[SENSOR_MAX_UNITS];
    BOOL has_reading[SENSOR_MAX_UNITS];
    float temperature_c[SENSOR_MAX_UNITS];
    float humidity_pct[SENSOR_MAX_UNITS];
} SensorSharedBlock;

typedef struct {
    HANDLE mapping;
    SensorSharedBlock *view;
} SensorShared;

/* Service side: creates the shared segment (read-write to itself),
 * with an explicit DACL granting any authenticated user read access -
 * SYSTEM's default security descriptor for a new object is NOT
 * guaranteed to already permit that, and a service normally runs as
 * SYSTEM while the GUI runs as a plain logged-in user. Returns false
 * (mapping/view left NULL) only on a real failure - the service should
 * still run its polling loop and CSV logging even if this somehow
 * fails, just without the GUI live-view working. */
bool sensor_shared_open_writer(SensorShared *sh);

/* GUI side: opens the segment read-only. Returns false (leaves *sh
 * zeroed) whenever the service isn't running/hasn't published yet -
 * that is the ordinary "no service installed" case, not an error the
 * caller needs to report. */
bool sensor_shared_open_reader(SensorShared *sh);

void sensor_shared_close(SensorShared *sh);

/* Service side: publishes s's current unit states and stamps the
 * heartbeat. Call once per completed poll cycle (not every tick) -
 * see SENSOR_SHARED_FRESH_MS's own comment for why the heartbeat
 * cadence matters. */
void sensor_shared_publish(SensorShared *sh, const Sensor *s);

/* GUI side: true (and s's unit fields overwritten from the shared
 * block) only if sh has an open mapping AND its heartbeat is fresh -
 * false means "no live service data right now", not necessarily
 * "never install the service at all"; the caller's own fallback poll
 * path should run instead. Only touches s->units[]/s->unit_addr[],
 * never s->port/s->connected/s->poll_state - the GUI's own Sensor
 * struct keeps whatever connection state it already had. */
bool sensor_shared_read_if_fresh(SensorShared *sh, Sensor *s);
