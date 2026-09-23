#include "sensor_shared.h"
#include <sddl.h>

/* "Allow Generic Read to Authenticated Users, full control to the
 * object's owner/creator" - the minimum needed for a SYSTEM-owned
 * mapping to actually be openable by the interactive user's own GUI
 * process. Built via SDDL (ConvertStringSecurityDescriptorToSecurityDescriptorA)
 * rather than hand-assembling an ACL - far less Win32 boilerplate for
 * a one-shot security descriptor nothing else in this app needs. */
static bool make_shared_read_sa(SECURITY_ATTRIBUTES *sa, PSECURITY_DESCRIPTOR *out_sd) {
    ZeroMemory(sa, sizeof(*sa));
    sa->nLength = sizeof(*sa);
    sa->bInheritHandle = FALSE;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:(A;;GRGW;;;AU)(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, out_sd, NULL)) {
        *out_sd = NULL;
        sa->lpSecurityDescriptor = NULL;
        return false;
    }
    sa->lpSecurityDescriptor = *out_sd;
    return true;
}

bool sensor_shared_open_writer(SensorShared *sh) {
    SECURITY_ATTRIBUTES sa;
    PSECURITY_DESCRIPTOR sd = NULL;
    bool have_sd;

    sh->mapping = NULL;
    sh->view = NULL;

    have_sd = make_shared_read_sa(&sa, &sd);

    sh->mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, have_sd ? &sa : NULL,
                                      PAGE_READWRITE, 0, sizeof(SensorSharedBlock),
                                      SENSOR_SHARED_NAME);
    if (sd) {
        LocalFree(sd);
    }
    if (sh->mapping == NULL) {
        return false;
    }

    sh->view = (SensorSharedBlock *)MapViewOfFile(sh->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SensorSharedBlock));
    if (sh->view == NULL) {
        CloseHandle(sh->mapping);
        sh->mapping = NULL;
        return false;
    }

    ZeroMemory(sh->view, sizeof(SensorSharedBlock));
    return true;
}

bool sensor_shared_open_reader(SensorShared *sh) {
    sh->mapping = NULL;
    sh->view = NULL;

    sh->mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, SENSOR_SHARED_NAME);
    if (sh->mapping == NULL) {
        return false; /* the ordinary "no service running" case */
    }

    sh->view = (SensorSharedBlock *)MapViewOfFile(sh->mapping, FILE_MAP_READ, 0, 0, sizeof(SensorSharedBlock));
    if (sh->view == NULL) {
        CloseHandle(sh->mapping);
        sh->mapping = NULL;
        return false;
    }
    return true;
}

void sensor_shared_close(SensorShared *sh) {
    if (sh->view) {
        UnmapViewOfFile(sh->view);
        sh->view = NULL;
    }
    if (sh->mapping) {
        CloseHandle(sh->mapping);
        sh->mapping = NULL;
    }
}

void sensor_shared_publish(SensorShared *sh, const Sensor *s) {
    int i;

    if (sh->view == NULL) {
        return;
    }

    for (i = 0; i < SENSOR_MAX_UNITS; i++) {
        const SensorState *st = sensor_get_state(s, i);
        sh->view->unit_addr[i] = sensor_get_unit_address(s, i);
        sh->view->online[i] = st->online ? TRUE : FALSE;
        sh->view->has_reading[i] = st->has_reading ? TRUE : FALSE;
        sh->view->temperature_c[i] = st->temperature_c;
        sh->view->humidity_pct[i] = st->humidity_pct;
    }
    /* Stamped last, after every field is in place - a reader that
     * happens to sample heartbeat_tick right at this instant and finds
     * it fresh is guaranteed to see this cycle's complete data, not a
     * half-written previous+current mix. */
    sh->view->heartbeat_tick = GetTickCount();
}

bool sensor_shared_read_if_fresh(SensorShared *sh, Sensor *s) {
    int i;
    DWORD age;

    if (sh->view == NULL) {
        return false;
    }

    age = GetTickCount() - sh->view->heartbeat_tick; /* wraps correctly even across GetTickCount()'s own ~49-day rollover, same as GetTickCount64 callers elsewhere in this app already rely on */
    if (age > SENSOR_SHARED_FRESH_MS) {
        return false;
    }

    for (i = 0; i < SENSOR_MAX_UNITS; i++) {
        s->unit_addr[i] = sh->view->unit_addr[i];
        s->units[i].online = sh->view->online[i] ? true : false;
        s->units[i].has_reading = sh->view->has_reading[i] ? true : false;
        s->units[i].temperature_c = sh->view->temperature_c[i];
        s->units[i].humidity_pct = sh->view->humidity_pct[i];
    }
    return true;
}
