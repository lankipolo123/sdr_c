#include "serial_port.h"
#include <stdio.h>

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
                       0, KEY_READ, &key) != ERROR_SUCCESS) {
        return 0;
    }

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
    return count;
}
