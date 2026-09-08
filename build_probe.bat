@echo off
REM Builds transit_probe.exe - safely probes Transit.dll's undocumented
REM exports (DevTemp/GetCachedDevTemp/Start-StopBackgroundTempPolling/
REM GetDllPassword) against real hardware. Console app, needs
REM dll\Transit.dll next to the exe (same layout as the main app).

gcc -std=c99 -Wall -Wextra -Wpedantic -Os -s ^
    -o transit_probe.exe src\transit_probe.c src\transit_dll.c

if %ERRORLEVEL% NEQ 0 (
    echo Build failed.
    exit /b 1
)
echo Build OK: transit_probe.exe
