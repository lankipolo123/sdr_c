#include "serial_port.h"
#include <setupapi.h>
#include <stdio.h>

/* GUID_DEVCLASS_PORTS ({4D36E978-E325-11CE-BFC1-08002BE10318}) - the
 * device SETUP class Ports (COM & LPT) lives under, the exact same
 * grouping Device Manager itself lists. Defined locally instead of
 * pulling it from <devguid.h> (which needs initguid.h included first,
 * or -luuid, to get an actual symbol rather than just an extern
 * declaration) - a plain local constant sidesteps that entirely. */
static const GUID GUID_DEVCLASS_PORTS_LOCAL =
    { 0x4d36e978, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };


static BYTE parity_to_win32(char parity) {
    switch (parity) {
        case 'O': return ODDPARITY;
        case 'E': return EVENPARITY;
        case 'M': return MARKPARITY;
        case 'S': return SPACEPARITY;
        default:  return NOPARITY;
    }
}

bool serial_open(SerialPort *sp, const char *port_name, DWORD baud, char parity, uint8_t data_bits) {
    char path[32];
    DCB dcb;
    COMMTIMEOUTS timeouts;

    /* "\\.\" prefix is required for COM10+ and harmless for COM1-9. */
    snprintf(path, sizeof(path), "\\\\.\\%s", port_name);

    sp->handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                              OPEN_EXISTING, 0, NULL);
    if (sp->handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    ZeroMemory(&dcb, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(sp->handle, &dcb)) {
        CloseHandle(sp->handle);
        sp->handle = INVALID_HANDLE_VALUE;
        return false;
    }

    dcb.BaudRate = baud;
    dcb.ByteSize = (data_bits >= 5 && data_bits <= 8) ? data_bits : 8;
    dcb.Parity = parity_to_win32(parity);
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = (dcb.Parity != NOPARITY);

    /* Explicitly set rather than leaving whatever GetCommState() read as
     * the port's prior/driver-default state. Matters most for RS-485
     * USB adapters that use RTS as a transmit/receive direction switch:
     * an inherited "stuck asserted" RTS can leave the adapter latched in
     * transmit mode, so requests go out fine but it never listens for a
     * reply - indistinguishable from a dead sensor without this fix. */
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;

    if (!SetCommState(sp->handle, &dcb)) {
        CloseHandle(sp->handle);
        sp->handle = INVALID_HANDLE_VALUE;
        return false;
    }

    /* Non-blocking read: MAXDWORD interval timeout with both total-timeout
     * fields at 0 is the documented Win32 idiom for "return immediately
     * with whatever's already received, even if that's nothing". */
    ZeroMemory(&timeouts, sizeof(timeouts));
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 0;
    if (!SetCommTimeouts(sp->handle, &timeouts)) {
        CloseHandle(sp->handle);
        sp->handle = INVALID_HANDLE_VALUE;
        return false;
    }

    PurgeComm(sp->handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
}

void serial_close(SerialPort *sp) {
    if (sp->handle != NULL && sp->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(sp->handle);
    }
    sp->handle = INVALID_HANDLE_VALUE;
}

bool serial_is_open(const SerialPort *sp) {
    return sp->handle != NULL && sp->handle != INVALID_HANDLE_VALUE;
}

bool serial_write(SerialPort *sp, const uint8_t *data, DWORD len, DWORD *out_written) {
    DWORD written = 0;
    bool ok;
    if (!serial_is_open(sp)) {
        if (out_written) *out_written = 0;
        return false;
    }
    ok = WriteFile(sp->handle, data, len, &written, NULL);
    if (out_written) *out_written = written;
    return ok != 0;
}

bool serial_read(SerialPort *sp, uint8_t *buf, DWORD buf_size, DWORD *out_len) {
    DWORD read_len = 0;
    bool ok;
    if (!serial_is_open(sp)) {
        if (out_len) *out_len = 0;
        return false;
    }
    ok = ReadFile(sp->handle, buf, buf_size, &read_len, NULL);
    if (out_len) *out_len = read_len;
    return ok != 0;
}

int serial_list_ports(char names[][16], int max_ports) {
    HKEY key;
    int count = 0;
    DWORD index = 0;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                       0, KEY_READ, &key) == ERROR_SUCCESS) {
        for (;;) {
            char value_name[256];
            DWORD value_name_len = sizeof(value_name);
            char data[16];
            DWORD data_len = sizeof(data);
            DWORD type;
            LONG rc = RegEnumValueA(key, index, value_name, &value_name_len,
                                     NULL, &type, (BYTE *)data, &data_len);
            if (rc == ERROR_NO_MORE_ITEMS) {
                break;
            }
            if (rc == ERROR_SUCCESS && type == REG_SZ) {
                if (count < max_ports) {
                    snprintf(names[count], 16, "%s", data);
                }
                count++;
            }
            index++;
        }
        RegCloseKey(key);
    }

    /* HARDWARE\DEVICEMAP\SERIALCOMM is normally reliable, but it's a
     * mirror the serial class driver writes to - not the actual source
     * of truth - and on at least one real machine (a Lenovo ThinkPad)
     * it came back empty for a USB-to-serial adapter's COM port even
     * though the port was genuinely present, openable, and visible in
     * Device Manager. SetupAPI's device enumeration (GUID_DEVCLASS_PORTS,
     * DIGCF_PRESENT) is the same data Device Manager itself reads, so it
     * catches every port that's actually there without also catching
     * ports that AREN'T - an earlier QueryDosDeviceA-based version of
     * this fallback fixed the ThinkPad's empty list but then listed
     * every ghost/reserved COM number Windows keeps registered whether
     * or not real hardware backs it (Bluetooth's own reserved SPP
     * virtual ports being the most common - direct report: "com port
     * becoming 3 more" that didn't actually read anything when picked,
     * since there's no real device behind them). DIGCF_PRESENT only
     * enumerates devices Windows currently considers physically present,
     * matching Device Manager's default (non-"show hidden devices")
     * view. Run as a supplement after the registry pass (not a
     * replacement) so the common case keeps the registry's natural
     * ordering, with anything missed filled in and deduped against it. */
    {
        HDEVINFO hdi = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS_LOCAL, NULL, NULL, DIGCF_PRESENT);
        if (hdi != INVALID_HANDLE_VALUE) {
            SP_DEVINFO_DATA devinfo;
            DWORD idx;
            devinfo.cbSize = sizeof(devinfo);
            for (idx = 0; SetupDiEnumDeviceInfo(hdi, idx, &devinfo); idx++) {
                HKEY hkey = SetupDiOpenDevRegKey(hdi, &devinfo, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
                if (hkey != INVALID_HANDLE_VALUE) {
                    char port_name[16];
                    DWORD size = sizeof(port_name);
                    DWORD type = 0;
                    /* The Ports class also holds LPT (parallel) devices -
                     * PortName reads "LPT1" etc. for those. Only "COMn"
                     * entries are what this function promises callers. */
                    if (RegQueryValueExA(hkey, "PortName", NULL, &type, (BYTE *)port_name, &size) == ERROR_SUCCESS &&
                        type == REG_SZ &&
                        (port_name[0] == 'C' || port_name[0] == 'c') &&
                        (port_name[1] == 'O' || port_name[1] == 'o') &&
                        (port_name[2] == 'M' || port_name[2] == 'm')) {
                        int j;
                        bool already_have = false;
                        for (j = 0; j < count && j < max_ports; j++) {
                            if (lstrcmpiA(names[j], port_name) == 0) {
                                already_have = true;
                                break;
                            }
                        }
                        if (!already_have) {
                            if (count < max_ports) {
                                snprintf(names[count], 16, "%s", port_name);
                            }
                            count++;
                        }
                    }
                    RegCloseKey(hkey);
                }
            }
            SetupDiDestroyDeviceInfoList(hdi);
        }
    }

    return count;
}
