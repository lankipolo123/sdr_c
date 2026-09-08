/* Safely probes the newer Transit.dll build's undocumented exports -
 * DevTemp, GetCachedDevTemp, StartBackgroundTempPolling,
 * StopBackgroundTempPolling, GetDllPassword - see transit_dll.h's header
 * comment for why these need probing instead of just calling them: no
 * header shipped, no confirmed argument count/types, and a wrong FARPROC
 * cast is undefined behavior on a real call, not a catchable exception
 * like ctypes gives Python.
 *
 * mingw-w64 doesn't support MSVC's __try/__except keywords (confirmed:
 * plain syntax error), so "catch the crash" here is the standard
 * Win32 workaround - a vectored exception handler that, on an access
 * violation, longjmps back out to the call site instead of letting the
 * process die. Also leans on the x64 calling convention itself for
 * extra safety: the caller always cleans the stack and unused
 * argument registers are simply ignored by a callee that takes fewer
 * params, so every candidate call below is padded out to 4 argument
 * slots (long long) regardless of which hypothesis is being tried -
 * that removes most of the "under-supplied a pointer arg" crash risk
 * even before the VEH/longjmp net.
 *
 * Run this against real hardware (DLL connected, sensor(s) wired) and
 * report which candidates return something sane (0/positive, a
 * plausible temperature in a buffer) vs an obvious failure code vs a
 * caught crash - that confirms the real shape so sensor.c can call it
 * directly instead of guessing.
 */
#include "transit_dll.h"
#include <stdio.h>
#include <setjmp.h>

typedef long (*ProbeFn)(long long a, long long b, long long c, long long d);

static jmp_buf g_recovery;
static volatile int g_guarded = 0;

static LONG WINAPI crash_guard(EXCEPTION_POINTERS *info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (g_guarded && (code == EXCEPTION_ACCESS_VIOLATION ||
                       code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                       code == EXCEPTION_PRIV_INSTRUCTION ||
                       code == EXCEPTION_DATATYPE_MISALIGNMENT)) {
        longjmp(g_recovery, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Calls fn(a,b,c,d), catching a crash instead of dying. *out_result is
 * only valid when this returns true. */
static bool guarded_call(FARPROC fn, long long a, long long b, long long c, long long d, long *out_result) {
    ProbeFn probe = (ProbeFn)fn; /* see transit_dll.c's comment on FARPROC casts */
    g_guarded = 1;
    if (setjmp(g_recovery) == 0) {
        *out_result = probe(a, b, c, d);
        g_guarded = 0;
        return true;
    }
    g_guarded = 0;
    return false;
}

static void print_buf(const char *label, const char *buf, long len) {
    long i;
    printf("    %s (as text): \"", label);
    for (i = 0; i < len && buf[i] != '\0'; i++) {
        putchar((buf[i] >= 32 && buf[i] < 127) ? buf[i] : '.');
    }
    printf("\"\n    %s (hex, first 16 bytes): ", label);
    for (i = 0; i < len && i < 16; i++) {
        printf("%02X ", (unsigned char)buf[i]);
    }
    printf("\n");
}

static void try_candidate(const char *export_name, FARPROC fn, const char *hypothesis,
                           long long a, long long b, long long c, long long d,
                           const char *buf_slot /* "a","b","c","d", or NULL if no buffer arg */,
                           char *buf) {
    long result;
    printf("  %s - %s\n", export_name, hypothesis);
    if (fn == NULL) {
        printf("    -> not exported by this DLL build (older Transit.dll?)\n");
        return;
    }
    if (guarded_call(fn, a, b, c, d, &result)) {
        printf("    -> returned %ld\n", result);
        if (buf_slot != NULL) {
            print_buf("buffer", buf, 256);
        }
    } else {
        printf("    -> CRASHED (access violation) - this signature is wrong, ruled out\n");
    }
}

int main(void) {
    TransitDll dll;
    char buf[256];

    AddVectoredExceptionHandler(1, crash_guard);

    printf("Loading dll\\Transit.dll...\n");
    if (!transit_dll_load(&dll, "dll\\Transit.dll")) {
        printf("FAILED to load - is dll\\Transit.dll next to this exe, and does it "
               "export the 5 confirmed functions (AutoConnectSDR/CheckConnection/"
               "DisconnectSDR/CommandTokens/SendCommandToSDR)?\n");
        return 1;
    }
    printf("Loaded OK.\n\n");

    printf("Step 1: connect (needed before any temp polling probably means anything)\n");
    ZeroMemory(buf, sizeof(buf));
    {
        long r = dll.auto_connect_sdr(buf, sizeof(buf));
        printf("  AutoConnectSDR -> %ld, buffer=%.60s\n\n", r, buf);
    }

    printf("Step 2: probe StartBackgroundTempPolling\n");
    try_candidate("StartBackgroundTempPolling", dll.start_background_temp_polling,
                   "no args", 0, 0, 0, 0, NULL, NULL);
    try_candidate("StartBackgroundTempPolling", dll.start_background_temp_polling,
                   "1 arg: interval_ms=1000", 1000, 0, 0, 0, NULL, NULL);
    printf("\n");

    printf("Step 3: probe DevTemp (on-demand read) for unit addresses 1-16\n");
    {
        int addr;
        for (addr = 1; addr <= 16; addr++) {
            char label[64];
            ZeroMemory(buf, sizeof(buf));
            snprintf(label, sizeof(label), "DevTemp(unit=%d, buf, 256)", addr);
            try_candidate("DevTemp", dll.dev_temp, label,
                           addr, (long long)(intptr_t)buf, sizeof(buf), 0, "b", buf);
        }
        ZeroMemory(buf, sizeof(buf));
        try_candidate("DevTemp", dll.dev_temp, "DevTemp(buf, 256) - no unit arg",
                       (long long)(intptr_t)buf, sizeof(buf), 0, 0, "a", buf);
    }
    printf("\n");

    printf("Step 4: probe GetCachedDevTemp for unit addresses 1-16\n");
    {
        int addr;
        for (addr = 1; addr <= 16; addr++) {
            char label[64];
            ZeroMemory(buf, sizeof(buf));
            snprintf(label, sizeof(label), "GetCachedDevTemp(unit=%d, buf, 256)", addr);
            try_candidate("GetCachedDevTemp", dll.get_cached_dev_temp, label,
                           addr, (long long)(intptr_t)buf, sizeof(buf), 0, "b", buf);
        }
        ZeroMemory(buf, sizeof(buf));
        try_candidate("GetCachedDevTemp", dll.get_cached_dev_temp, "GetCachedDevTemp(buf, 256) - no unit arg",
                       (long long)(intptr_t)buf, sizeof(buf), 0, 0, "a", buf);
    }
    printf("\n");

    printf("Step 5: probe StopBackgroundTempPolling\n");
    try_candidate("StopBackgroundTempPolling", dll.stop_background_temp_polling,
                   "no args", 0, 0, 0, 0, NULL, NULL);
    printf("\n");

    printf("Step 6: probe GetDllPassword (probably unrelated/licensing, checking anyway)\n");
    ZeroMemory(buf, sizeof(buf));
    try_candidate("GetDllPassword", dll.get_dll_password, "(buf, 256)",
                   (long long)(intptr_t)buf, sizeof(buf), 0, 0, "a", buf);
    printf("\n");

    printf("Step 7: disconnect\n");
    ZeroMemory(buf, sizeof(buf));
    {
        long r = dll.disconnect_sdr(buf, sizeof(buf));
        printf("  DisconnectSDR -> %ld, buffer=%.60s\n", r, buf);
    }

    transit_dll_unload(&dll);
    return 0;
}
