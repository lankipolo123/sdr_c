/* ECM Controller Sensor Logger - a background Windows Service that
 * owns the sensor COM port continuously (installed or not, GUI open or
 * closed) so BAY1-4 CSV logging (sensor_log.c) keeps running even
 * after the main app is closed - direct request. Publishes live
 * readings via shared memory (sensor_shared.c) so the GUI's own
 * heatmap/BAY readouts keep working too, without the GUI ever opening
 * the same port itself (a serial port can't be opened by two processes
 * at once - see sensor_shared.h's own comment).
 *
 * Deliberately reuses serial_port.c/modbus.c/sensor.c/sensor_log.c
 * completely unchanged - this service is just a different, headless
 * "front end" driving the exact same hardware-talking code the GUI's
 * own WM_TIMER loop drives, on a plain Sleep(100) loop instead of a
 * message-loop timer.
 *
 * Self-installing: run with --install (needs admin - see
 * service_admin.manifest, which makes Windows UAC-prompt for this exe
 * automatically) to register and start it, --uninstall to stop and
 * remove it. No separate installer tooling (sc.exe, third-party NSIS
 * plugins) needed - CreateServiceA/DeleteService are plain Win32.
 *
 * Also runs directly in a console (no service, no admin) as a plain
 * foreground loop, printing what it's doing - StartServiceCtrlDispatcherA
 * fails with ERROR_FAILED_SERVICE_CONTROLLER_CONNECT whenever this exe
 * wasn't actually launched by the SCM (i.e. someone just ran it), which
 * this treats as "run in debug/console mode" instead of a hard error -
 * useful for confirming the polling/logging/shared-memory logic works
 * without needing a real service install to test it.
 */
#include <windows.h>
#include <stdio.h>
#include "serial_port.h"
#include "sensor.h"
#include "sensor_log.h"
#include "sensor_shared.h"

#define SERVICE_NAME "ECMControllerSensorService"
#define SERVICE_DISPLAY_NAME "ECM Controller Sensor Logger"

static SERVICE_STATUS g_status;
static SERVICE_STATUS_HANDLE g_status_handle;
static HANDLE g_stop_event;

/* Hardcoded to the GUI's own installed name (installer.nsi's EXE_NAME) -
 * not derived from this service exe's own name (unlike get_sensor_log_path()
 * in sensor_log.c, which deliberately uses a name-independent fixed
 * filename for exactly this reason). This one file genuinely has to be
 * the GUI's specific settings file, since that's the only place the
 * configured sensor port lives - there's no shared/independent copy of
 * it to invent without also touching main.c's own save_settings(). */
static void get_gui_ini_path(char *path /* at least MAX_PATH + 20 bytes */) {
    char *slash;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    slash = strrchr(path, '\\');
    if (slash) {
        slash[1] = '\0';
    } else {
        path[0] = '\0';
    }
    lstrcatA(path, "ECMController.ini");
}

static void read_configured_sensor_port(char *port_out, int port_out_size) {
    char ini_path[MAX_PATH + 20];
    get_gui_ini_path(ini_path);
    if (GetPrivateProfileStringA("Sensor", "Port", "", port_out, port_out_size, ini_path) == 0) {
        lstrcpynA(port_out, "COM1", port_out_size); /* same fallback the GUI itself effectively starts on with nothing saved yet */
    }
}

/* The actual work - identical regardless of whether this is running as
 * a real service or the console debug fallback. console_mode only
 * changes whether it prints progress (a service has no console to
 * print to; GetStdHandle just returns an invalid handle harmlessly if
 * there genuinely isn't one, so the printf calls are guarded mainly to
 * avoid pointless overhead every tick, not because they'd crash). */
static void run_sensor_loop(bool console_mode) {
    Sensor sensor;
    SensorShared shared;
    char port[16];
    char last_port[16];
    DWORD last_connect_attempt;
    int publish_counter;
    bool have_shared;

    sensor_init(&sensor);
    {
        int i;
        for (i = 0; i < SENSOR_MAX_UNITS; i++) {
            sensor_set_unit_address(&sensor, i, SENSOR_UNIT_ADDR_DEFAULT[i]);
        }
    }

    have_shared = sensor_shared_open_writer(&shared);
    if (console_mode) {
        printf(have_shared ? "Shared memory published for the GUI.\n"
                            : "WARNING: could not open shared memory - GUI live view won't work, logging still will.\n");
    }

    sensor_log_load_state();

    last_connect_attempt = 0;
    publish_counter = 0;
    last_port[0] = '\0';

    for (;;) {
        if (g_stop_event != NULL && WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0) {
            break;
        }

        if (!sensor_is_connected(&sensor)) {
            DWORD now = GetTickCount();
            /* Re-reads the configured port every retry (not just once
             * at startup) so editing the Sensor Port in the GUI and
             * restarting just the connection - not the whole service -
             * still takes effect on the next attempt. */
            if (now - last_connect_attempt >= 5000) {
                last_connect_attempt = now;
                read_configured_sensor_port(port, sizeof(port));
                if (console_mode && lstrcmpA(port, last_port) != 0) {
                    printf("Connecting to %s...\n", port);
                    lstrcpynA(last_port, port, sizeof(last_port));
                }
                sensor_connect(&sensor, port, SENSOR_BAUD, SENSOR_PARITY, SENSOR_DATABITS);
                if (console_mode && sensor_is_connected(&sensor)) {
                    printf("Connected.\n");
                }
            }
        }

        sensor_poll(&sensor);

        publish_counter++;
        if (have_shared && publish_counter >= 10) { /* ~1s at the 100ms loop cadence below */
            publish_counter = 0;
            sensor_shared_publish(&shared, &sensor);
        }

        sensor_log_tick(&sensor);

        if (g_stop_event != NULL) {
            if (WaitForSingleObject(g_stop_event, 100) == WAIT_OBJECT_0) {
                break;
            }
        } else {
            Sleep(100);
        }
    }

    sensor_disconnect(&sensor);
    if (have_shared) {
        sensor_shared_close(&shared);
    }
}

static VOID WINAPI service_ctrl_handler(DWORD ctrl) {
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        g_status.dwCurrentState = SERVICE_STOP_PENDING;
        g_status.dwWaitHint = 3000;
        SetServiceStatus(g_status_handle, &g_status);
        SetEvent(g_stop_event);
    }
}

static VOID WINAPI service_main(DWORD argc, LPSTR *argv) {
    (void)argc;
    (void)argv;

    ZeroMemory(&g_status, sizeof(g_status));
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = SERVICE_START_PENDING;
    g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

    g_status_handle = RegisterServiceCtrlHandlerA(SERVICE_NAME, (LPHANDLER_FUNCTION)service_ctrl_handler);
    if (g_status_handle == NULL) {
        return;
    }

    g_stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (g_stop_event == NULL) {
        g_status.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_status_handle, &g_status);
        return;
    }

    g_status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_status_handle, &g_status);

    run_sensor_loop(false);

    CloseHandle(g_stop_event);
    g_stop_event = NULL;

    g_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_status_handle, &g_status);
}

/* --install: registers the service (auto-start, so it survives a
 * reboot with nobody logged in - the whole point) and starts it
 * immediately so the person doesn't need to also open Services.msc.
 * Points binPath at wherever THIS exe actually is right now
 * (GetModuleFileNameA), so it works from any install location. */
static int do_install(void) {
    char exe_path[MAX_PATH];
    SC_HANDLE scm;
    SC_HANDLE svc;

    GetModuleFileNameA(NULL, exe_path, MAX_PATH);

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (scm == NULL) {
        MessageBoxA(NULL,
            "Could not open the Service Control Manager - this needs to run as Administrator.",
            "Install failed", MB_ICONERROR);
        return 1;
    }

    svc = CreateServiceA(scm, SERVICE_NAME, SERVICE_DISPLAY_NAME,
                          SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                          SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                          exe_path, NULL, NULL, NULL, NULL, NULL);
    if (svc == NULL) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            MessageBoxA(NULL, "The sensor logger service is already installed.", "Install", MB_ICONINFORMATION);
            CloseServiceHandle(scm);
            return 0;
        }
        MessageBoxA(NULL, "Could not create the service.", "Install failed", MB_ICONERROR);
        CloseServiceHandle(scm);
        return 1;
    }

    if (!StartServiceA(svc, 0, NULL) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        MessageBoxA(NULL,
            "The service was installed but could not be started - check Services.msc for details.",
            "Install", MB_ICONWARNING);
    } else {
        MessageBoxA(NULL,
            "Sensor logger service installed and started. It will keep logging BAY1-4 readings even while the app is closed.",
            "Install", MB_ICONINFORMATION);
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

static int do_uninstall(void) {
    SC_HANDLE scm;
    SC_HANDLE svc;
    SERVICE_STATUS status;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm == NULL) {
        MessageBoxA(NULL,
            "Could not open the Service Control Manager - this needs to run as Administrator.",
            "Uninstall failed", MB_ICONERROR);
        return 1;
    }

    svc = OpenServiceA(scm, SERVICE_NAME, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (svc == NULL) {
        MessageBoxA(NULL, "The sensor logger service isn't installed.", "Uninstall", MB_ICONINFORMATION);
        CloseServiceHandle(scm);
        return 0;
    }

    ControlService(svc, SERVICE_CONTROL_STOP, &status);
    DeleteService(svc);

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    MessageBoxA(NULL, "Sensor logger service removed.", "Uninstall", MB_ICONINFORMATION);
    return 0;
}

int main(int argc, char **argv) {
    SERVICE_TABLE_ENTRYA table[2];

    setvbuf(stdout, NULL, _IONBF, 0); /* console debug mode's whole point is live visibility - a buffered printf that only appears on exit defeats it */

    if (argc >= 2 && lstrcmpiA(argv[1], "--install") == 0) {
        return do_install();
    }
    if (argc >= 2 && lstrcmpiA(argv[1], "--uninstall") == 0) {
        return do_uninstall();
    }
    if (argc >= 2 && lstrcmpiA(argv[1], "--debug") == 0) {
        /* Explicit, unconditional console mode - unlike the
         * StartServiceCtrlDispatcherA fallback below (which some SCM
         * implementations may not fail out of the same way real
         * Windows documents), this always runs the loop directly, no
         * ambiguity. Same thing a person would use to confirm the
         * polling/shared-memory/CSV logic works before trusting a real
         * service install. */
        printf("Debug mode - running in the foreground.\n(Ctrl+C to stop; this does not need Administrator.)\n");
        g_stop_event = NULL;
        run_sensor_loop(true);
        return 0;
    }

    table[0].lpServiceName = (LPSTR)SERVICE_NAME;
    table[0].lpServiceProc = service_main;
    table[1].lpServiceName = NULL;
    table[1].lpServiceProc = NULL;

    if (!StartServiceCtrlDispatcherA(table)) {
        if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            /* Not launched by the SCM - run the same loop directly in
             * this console instead, for debugging/manual testing. */
            printf("Not running as a service - running in the foreground instead.\n"
                   "(Ctrl+C to stop; this does not need Administrator.)\n");
            g_stop_event = NULL;
            run_sensor_loop(true);
            return 0;
        }
        return 1;
    }
    return 0;
}
