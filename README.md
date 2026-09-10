# Digital Noise Configuration - Multi

Windows GUI for driving all 16 channels of the SDR/RF module over one
shared RS422 bus, using the fire-once "blind send" pattern proven by the
other apps in this ecosystem ([sdr_controller](https://github.com/lankipolo123/sdr_controller),
[sdr_react](https://github.com/lankipolo123/sdr_react)): no per-channel
ACK wait, no retry - a frame is sent once, the new state is applied
optimistically after a fixed settle delay, and marked unconfirmed.

This is a separate app from [digital-noise-configuration](https://github.com/lankipolo123/digital-noise-configuration)
(the single-channel app with full read/write + ACK-gated confirmation),
not a mode inside it - the two apps' devices are addressed and used
differently enough that keeping them as separate executables (and
separate repos) was simpler than merging the two control models into one
UI.

No Qt, no Python runtime, no pyserial - just `user32.dll`, `gdi32.dll`,
`kernel32.dll`, `advapi32.dll`, `comctl32.dll` (all part of Windows
itself), plus the vendor's own `dll/Transit.dll` for the RS422 channel
bus (see "DLL integration" below). The Amplifier Temperature sensor bus
is unrelated and still talks raw serial directly.

## UI pattern

Each of the 16 channel cards matches the pattern used by
`ChannelCard.tsx` in sdr_react and `web/app.js` in sdr_app:

- Mode combo + an explicit **Set** button - selecting a mode is
  local/uncommitted until Set is clicked, never applied on selection
  alone.
- Two separate **ON**/**OFF** power buttons (not one toggle) - whichever
  is active gets a solid fill (green ON / red OFF).
- A status line reading `SENDING...` / `<LEVEL>` / `STANDBY`.
- A vertical level trackbar with High/Medium/Low/Off tick labels (the
  active one highlighted), in place of a plain dropdown.

Bandwidth is **not** a per-channel control (matching the reference
apps) - every Signal Control frame uses a fixed `CHANNEL_BLIND_BANDWIDTH_MHZ`.
Frequency **is** real per-channel: each of the 16 channels has its own
actual operating frequency (`channel_freq_mhz()` in `channels.c`), not
one shared default. Mode and power level are also real per-channel
selections, same as before.

## DLL integration

The RS422 channel bus talks to hardware through `dll/Transit.dll`, not
raw serial I/O - ported from sdr_app's `middleware.py`/
`use_connection.py`, the proven hardware-confirmed reference (same DLL,
same 5 exports: `AutoConnectSDR`, `CheckConnection`, `DisconnectSDR`,
`CommandTokens`, `SendCommandToSDR`). `AutoConnectSDR` auto-discovers the
dongle itself, so Port/Baud/Parity/Data Bits are inert now (kept in the
UI and in `conn_connect()`'s signature for stability, matching
middleware's own `ConnectionController.connect()` signature, which does
the same) - whatever's selected has no effect on what Connect actually
does.
Sending a frame means translating it one byte at a time through
`CommandTokens` before handing each token to `SendCommandToSDR` - never
the raw protocol bytes, matching the confirmed real mechanism. See
`src/transit_dll.h`/`.c` and `src/connection.c`.

## Building

Requires mingw-w64 (get it via [MSYS2](https://www.msys2.org/), then run
this from the "MSYS2 MinGW x64" shell, or any shell with mingw-w64's
`bin` on `PATH`):

```
build.bat
```

produces `digital_noise_config_multi.exe`.

## Layout

- `src/protocol.h` / `.c` - RS422 frame format (build/parse), shared
  unchanged with `digital-noise-configuration`.
- `src/serial_port.h` / `.c` - raw `CreateFile`/`ReadFile`/`WriteFile`
  COM port I/O + registry port enumeration; used by the Amplifier
  Temperature sensor bus only now (see below).
- `src/transit_dll.h` / `.c` - dynamic loader for the vendor's
  `Transit.dll`.
- `src/connection.h` / `.c` - RS422 channel-bus connection lifecycle on
  top of `transit_dll`, polled from a `WM_TIMER` tick.
- `src/channels.h` / `.c` - the 16-channel blind-send layer: a FIFO send
  queue (only one frame in flight at a time, since all 16 channels share
  one physical connection), a 300ms settle delay, and optimistic state
  application with an `unconfirmed` flag.
- `src/main.c` / `resource.h` - the Win32 window and the 4x4 channel-card
  grid.

## Verification

Built and exercised under Wine: window/grid creation, port list refresh,
Mode Set / ON / OFF / level-trackbar interaction, and the blind-send
queue -> settle -> optimistic-UI-update cycle across multiple channels.

The Transit.dll integration itself: confirmed the DLL loads under Wine
and `AutoConnectSDR` genuinely executes and returns (no crash) - with no
real RS422 dongle attached there, it correctly comes back "not
connected" rather than faking success. Not yet tested against real
hardware - that's the one thing this environment can't confirm.
