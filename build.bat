@echo off
REM Builds digital_noise_config_multi.exe with mingw-w64 gcc. Install it via
REM MSYS2 (https://www.msys2.org/) - after installing, run this from the
REM "MSYS2 MinGW x64" shell or a cmd with mingw-w64\bin on PATH.

windres src\app.rc -O coff -o src\app_res.o
if %ERRORLEVEL% NEQ 0 (
    echo Resource compile failed.
    exit /b 1
)

gcc -std=c99 -Wall -Wextra -Wpedantic -Werror -mwindows -Os -s ^
    -fno-ident -fno-asynchronous-unwind-tables ^
    -ffunction-sections -fdata-sections -Wl,--gc-sections ^
    -o digital_noise_config_multi.exe src\main.c src\connection.c src\channels.c src\protocol.c src\serial_port.c src\modbus.c src\sensor.c src\app_res.o ^
    -ladvapi32 -lgdi32 -luser32 -lcomctl32 -lmsimg32

if %ERRORLEVEL% NEQ 0 (
    echo Build failed.
    exit /b 1
)
echo Build OK: digital_noise_config_multi.exe
