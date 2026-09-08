/* Dynamic loader for Transit.dll - the vendor DLL sdr_app already talks to
 * via ctypes (see sdr_app/services/middleware.py) for the RS-422/RF channel
 * bus. This app never used it before; talks to hardware directly over
 * WinAPI serial I/O instead (serial_port.c) for both buses.
 *
 * Five exports are confirmed working (same shapes middleware.py uses,
 * proven against real hardware there): AutoConnectSDR, CheckConnection,
 * DisconnectSDR, CommandTokens, SendCommandToSDR - all (char* buf, long
 * len[, char* outBuf]) -> long.
 *
 * A newer DLL build (2026-09) adds five more exports with no header and
 * no confirmed signature: DevTemp, GetCachedDevTemp,
 * StartBackgroundTempPolling, StopBackgroundTempPolling, GetDllPassword -
 * looks like the vendor's own built-in replacement for this app's
 * hand-rolled raw-Modbus temp/humidity sensor polling (sensor.c/modbus.c).
 * Calling an unconfirmed export with the wrong argument shape is
 * undefined behavior on a real FARPROC cast, not a catchable exception
 * like ctypes gives Python - see transit_probe.c, which calls these
 * candidate shapes wrapped in SEH (__try/__except) specifically so a
 * wrong guess reports a failure instead of crashing the process.
 */
#pragma once
#include <windows.h>
#include <stdbool.h>

typedef long (*TransitStatusFn)(char *buf, long buf_size);              /* AutoConnectSDR / CheckConnection / DisconnectSDR shape */
typedef long (*TransitCommandTokensFn)(char *cmd, char *out_buf, long buf_size);
typedef long (*TransitSendCommandFn)(char *cmd, long len);

typedef struct {
    HMODULE handle;

    /* Confirmed shape - safe to call directly. */
    TransitStatusFn auto_connect_sdr;
    TransitStatusFn check_connection;
    TransitStatusFn disconnect_sdr;
    TransitCommandTokensFn command_tokens;
    TransitSendCommandFn send_command_to_sdr;

    /* Unconfirmed shape - raw pointers only. Do not call directly; go
     * through transit_probe.c's SEH-guarded attempts until one is
     * confirmed against real hardware, then give it a typed wrapper
     * above like the others. NULL on a DLL build that doesn't export it
     * (the pre-2026-09 one sdr_app has been using). */
    FARPROC dev_temp;
    FARPROC get_cached_dev_temp;
    FARPROC start_background_temp_polling;
    FARPROC stop_background_temp_polling;
    FARPROC get_dll_password;
} TransitDll;

/* dll_path e.g. "dll\\Transit.dll" or "Transit.dll" if it's already on
 * the search path. Returns false (dll->handle left NULL) if the file
 * can't be loaded at all; a load that succeeds but is missing one of
 * the five confirmed exports also fails (that means it's not really
 * Transit.dll). Missing unconfirmed exports (older DLL build) is not a
 * failure - those fields are just left NULL. */
bool transit_dll_load(TransitDll *dll, const char *dll_path);
void transit_dll_unload(TransitDll *dll);
bool transit_dll_is_loaded(const TransitDll *dll);
