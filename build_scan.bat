@echo off
REM Builds scan_sensor_addresses.exe - a console-mode tool to discover
REM which Modbus address each connected temp/humidity sensor answers on.
REM Same mingw-w64 gcc setup as build.bat.

gcc -std=c99 -Wall -Wextra -Wpedantic -Werror -Os -s ^
    -fno-ident -fno-asynchronous-unwind-tables ^
    -ffunction-sections -fdata-sections -Wl,--gc-sections ^
    -o scan_sensor_addresses.exe src\scan_sensor_addresses.c src\modbus.c src\serial_port.c ^
    -ladvapi32

if %ERRORLEVEL% NEQ 0 (
    echo Build failed.
    exit /b 1
)
echo Build OK: scan_sensor_addresses.exe
