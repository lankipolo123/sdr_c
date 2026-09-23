#include "sensor_log.h"
#include <string.h>

#define SENSOR_LOG_INTERVAL_TICKS 600 /* caller ticks every ~100ms - 600 ticks = one CSV row every ~60s */
/* 7 days, expressed in FILETIME's own unit (100ns intervals) - what
 * GetSystemTimeAsFileTime()/the ULARGE_INTEGER comparison below need. */
#define SENSOR_LOG_WEEK_100NS ((ULONGLONG)7 * 24 * 60 * 60 * 10000000ULL)

static ULARGE_INTEGER g_sensor_log_week_start;
static int g_sensor_log_tick_counter = 0;

/* Fixed name, resolved next to WHICHEVER exe is currently running this
 * code - GetModuleFileNameA(NULL, ...) is the calling process's own
 * exe path, but the filename portion is replaced with a constant
 * rather than kept (see this file's header comment on why). */
void get_sensor_log_path(char *path /* at least MAX_PATH + 16 bytes */) {
    char *slash;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    slash = strrchr(path, '\\');
    if (slash) {
        slash[1] = '\0';
    } else {
        path[0] = '\0';
    }
    lstrcatA(path, "sensor_log.csv");
}

static void get_sensor_log_state_ini_path(char *path /* at least MAX_PATH + 24 bytes */) {
    char *slash;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    slash = strrchr(path, '\\');
    if (slash) {
        slash[1] = '\0';
    } else {
        path[0] = '\0';
    }
    lstrcatA(path, "sensor_log_state.ini");
}

static void sensor_log_now(ULARGE_INTEGER *out) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    out->LowPart = ft.dwLowDateTime;
    out->HighPart = ft.dwHighDateTime;
}

/* Rewrites the CSV down to just its header row (the "past data
 * deleted" half of the weekly reset) and records the new week's start
 * time, both in memory and in the state ini so a restart mid-week
 * doesn't lose it - a restart-only reset would never actually reach 7
 * days if whichever process is running this gets restarted more often
 * than that. */
static void sensor_log_start_new_week(void) {
    char path[MAX_PATH + 16];
    char ini_path[MAX_PATH + 24];
    char buf[24];
    HANDLE file;
    DWORD written;
    static const char header[] =
        "Timestamp,Bay1_C,Bay2_C,Bay3_C,Bay4_C,Bay1_RH,Bay2_RH,Bay3_RH,Bay4_RH\r\n";

    get_sensor_log_path(path);
    file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        WriteFile(file, header, (DWORD)(sizeof(header) - 1), &written, NULL);
        CloseHandle(file);
    }

    sensor_log_now(&g_sensor_log_week_start);

    get_sensor_log_state_ini_path(ini_path);
    wsprintfA(buf, "%lu", (unsigned long)g_sensor_log_week_start.LowPart);
    WritePrivateProfileStringA("SensorLog", "WeekStartLow", buf, ini_path);
    wsprintfA(buf, "%lu", (unsigned long)g_sensor_log_week_start.HighPart);
    WritePrivateProfileStringA("SensorLog", "WeekStartHigh", buf, ini_path);
}

void sensor_log_load_state(void) {
    char ini_path[MAX_PATH + 24];
    int has_saved_week;

    get_sensor_log_state_ini_path(ini_path);

    /* nDefault (-1) only comes back if the key is genuinely missing -
     * see load_channel_settings() in main.c for the same idiom. (A real
     * saved FILETIME high-part could theoretically also be exactly
     * 0xFFFFFFFF and get misread as "missing" here - the same
     * negligible risk main.c already accepts for Mode/Level.) */
    has_saved_week = GetPrivateProfileIntA("SensorLog", "WeekStartHigh", -1, ini_path);
    if (has_saved_week < 0) {
        sensor_log_start_new_week();
        return;
    }
    g_sensor_log_week_start.LowPart = (DWORD)GetPrivateProfileIntA("SensorLog", "WeekStartLow", 0, ini_path);
    g_sensor_log_week_start.HighPart = (DWORD)GetPrivateProfileIntA("SensorLog", "WeekStartHigh", 0, ini_path);
}

static void sensor_log_append_row(const Sensor *s) {
    char path[MAX_PATH + 16];
    char line[256];
    char ts[24];
    char temp_str[SENSOR_MAX_UNITS][16];
    char rh_str[SENSOR_MAX_UNITS][16];
    SYSTEMTIME st;
    HANDLE file;
    DWORD written;
    int i;
    int len;

    GetLocalTime(&st);
    wsprintfA(ts, "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    for (i = 0; i < SENSOR_MAX_UNITS; i++) {
        const SensorState *unit = sensor_get_state(s, i);
        if (unit->has_reading) {
            wsprintfA(temp_str[i], "%d.%d", (int)unit->temperature_c, (int)(unit->temperature_c * 10) % 10);
            wsprintfA(rh_str[i], "%d.%d", (int)unit->humidity_pct, (int)(unit->humidity_pct * 10) % 10);
        } else {
            temp_str[i][0] = '\0';
            rh_str[i][0] = '\0';
        }
    }

    len = wsprintfA(line, "%s,%s,%s,%s,%s,%s,%s,%s,%s\r\n", ts,
                     temp_str[0], temp_str[1], temp_str[2], temp_str[3],
                     rh_str[0], rh_str[1], rh_str[2], rh_str[3]);

    get_sensor_log_path(path);
    /* FILE_APPEND_DATA alone (not combined with GENERIC_WRITE) is the
     * standard Win32 idiom for "writes always land at the current end
     * of file" - no separate SetFilePointer needed. */
    file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        WriteFile(file, line, (DWORD)len, &written, NULL);
        CloseHandle(file);
    }
}

void sensor_log_tick(const Sensor *s) {
    ULARGE_INTEGER now;

    sensor_log_now(&now);
    if (now.QuadPart - g_sensor_log_week_start.QuadPart >= SENSOR_LOG_WEEK_100NS) {
        sensor_log_start_new_week();
    }

    g_sensor_log_tick_counter++;
    if (g_sensor_log_tick_counter >= SENSOR_LOG_INTERVAL_TICKS) {
        g_sensor_log_tick_counter = 0;
        sensor_log_append_row(s);
    }
}
