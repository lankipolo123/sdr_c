#include "transit_dll.h"

bool transit_dll_load(TransitDll *dll, const char *dll_path) {
    ZeroMemory(dll, sizeof(*dll));

    dll->handle = LoadLibraryA(dll_path);
    if (dll->handle == NULL) {
        return false;
    }

    /* GetProcAddress returns FARPROC (a function pointer) - casting it to
     * our specific function-pointer typedefs is the whole point of this
     * function, and is unavoidably outside strict ISO C (which forbids
     * object<->function pointer conversion in general) even though it's
     * exactly what every GetProcAddress call site in existence does; not
     * built with -Wpedantic -Werror for that reason (see build_probe.bat). */
    dll->auto_connect_sdr    = (TransitStatusFn)GetProcAddress(dll->handle, "AutoConnectSDR");
    dll->check_connection    = (TransitStatusFn)GetProcAddress(dll->handle, "CheckConnection");
    dll->disconnect_sdr      = (TransitStatusFn)GetProcAddress(dll->handle, "DisconnectSDR");
    dll->command_tokens      = (TransitCommandTokensFn)GetProcAddress(dll->handle, "CommandTokens");
    dll->send_command_to_sdr = (TransitSendCommandFn)GetProcAddress(dll->handle, "SendCommandToSDR");

    if (!dll->auto_connect_sdr || !dll->check_connection || !dll->disconnect_sdr ||
        !dll->command_tokens || !dll->send_command_to_sdr) {
        FreeLibrary(dll->handle);
        ZeroMemory(dll, sizeof(*dll));
        return false;
    }

    /* Newer-build-only exports - fine if missing (NULL), see transit_dll.h. */
    dll->dev_temp                       = GetProcAddress(dll->handle, "DevTemp");
    dll->get_cached_dev_temp            = GetProcAddress(dll->handle, "GetCachedDevTemp");
    dll->start_background_temp_polling  = GetProcAddress(dll->handle, "StartBackgroundTempPolling");
    dll->stop_background_temp_polling   = GetProcAddress(dll->handle, "StopBackgroundTempPolling");
    dll->get_dll_password               = GetProcAddress(dll->handle, "GetDllPassword");

    return true;
}

void transit_dll_unload(TransitDll *dll) {
    if (dll->handle != NULL) {
        FreeLibrary(dll->handle);
    }
    ZeroMemory(dll, sizeof(*dll));
}

bool transit_dll_is_loaded(const TransitDll *dll) {
    return dll->handle != NULL;
}
