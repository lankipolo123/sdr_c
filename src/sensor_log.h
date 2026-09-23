/* Weekly-rotating CSV log of BAY1-4 temperature/humidity readings.
 * Shared between the GUI (main.c, used when it's the one polling the
 * sensor port directly - no background service installed/running) and
 * the background service (sensor_service.c, the normal case once
 * installed - it owns the sensor port continuously, GUI or no GUI).
 *
 * Both processes must agree on the exact same file paths regardless of
 * which one is calling - get_sensor_log_path()/get_sensor_log_state_ini_path()
 * deliberately use a FIXED filename (not derived from the calling exe's
 * own name, unlike get_ini_path() in main.c) so this works whichever
 * process happens to be running it, as long as both are installed in
 * the same folder (which installer.nsi always does). The state ini is
 * its own small file, separate from the GUI's own settings ini
 * (digital_noise_config_multi.ini) - the service has no reason to know
 * that file's format, and a shared file two different-purposed writers
 * touch is asking for a merge conflict neither would ever see coming. */
#pragma once
#include <windows.h>
#include <stdbool.h>
#include "sensor.h"

void get_sensor_log_path(char *path /* at least MAX_PATH + 16 bytes */);

/* Call once at startup (after the caller's own module init). Loads the
 * saved week-start anchor from the state ini, or starts a fresh week
 * (writing the CSV header and a new anchor) on a genuinely first run. */
void sensor_log_load_state(void);

/* Call every ~100ms tick, same cadence as this app's other WM_TIMER
 * work - cheap enough to run unconditionally; the actual CSV write is
 * throttled internally to roughly once every 60s, and the weekly
 * rotation check is just one subtraction/comparison. */
void sensor_log_tick(const Sensor *s);
