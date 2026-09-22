# Handoff notes: ECM Controller (sdr_c)

Win32 native C app, no CRT-heavy deps. Sibling repos exist (e.g. `sdr_app`,
a separate Python/Qt app) — verify `git remote -v` shows `lankipolo123/sdr_c`
before doing anything, since a session can start pointed at the wrong repo.

## Environment resets happen mid-session, silently

The whole sandbox (filesystem, installed packages, uploaded files) can get
wiped between turns. Only what's been `git push`ed survives. Signs: a `Read`
that worked a moment ago now says "File does not exist"; `which <tool>` comes
back empty for something just installed. Recovery: re-clone, reinstall the
toolchain via apt, and **re-request any uploaded file from the user** —
uploads never survive a reset and can't be recovered locally.

**`Transit_2.dll` may be missing** (wiped in a past reset, not yet
re-uploaded as of the last session). Don't ship a real installer without it —
ask the user for it before doing a "final" build.

## Build commands (plain dev exe)

```
x86_64-w64-mingw32-windres src/app.rc -O coff -o src/app_res.o
x86_64-w64-mingw32-gcc -std=c99 -Wall -Wextra -Wpedantic -Werror -Wno-cast-function-type -mwindows -Os -s \
  -fno-ident -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -o digital_noise_config_multi.exe src/main.c src/connection.c src/channels.c src/protocol.c \
  src/serial_port.c src/modbus.c src/sensor.c src/transit_dll.c src/app_res.o \
  -ladvapi32 -lgdi32 -luser32 -lmsimg32 -lgdiplus -lsetupapi
```

`build.bat` in the repo root is stale (missing `-lgdiplus -lsetupapi`) —
don't trust it blindly, it hasn't been kept in sync.

**Before shipping anything to the user: always rebuild the protected/
obfuscated build AND the NSIS installer.** Never hand over the plain dev exe
as a deliverable.

## Obfuscation gotchas (`tools/obfuscate.py` -> `build_obf/src/`)

Regenerating the obfuscated source needs manual hand-fixes every time, for
things the script doesn't handle automatically:

- `PARITY_LABELS`, `LEVEL_LABELS` - need backing-buffer arrays +
  `obf_init_label_arrays()` called in `WinMain`
- `row_select_items` - inline decode-on-declaration (block-local array)
- `MODE_NAMES` (in `protocol.c`) - lazy-init-on-first-call
- `TRANSIT_DLL_PATH` macro (in `connection.c`) - manual conversion

Skipping any of these breaks the protected build silently or at runtime, not
at compile time.

## Testing in this sandbox (no real Windows available)

- `wine64` binary is at `/usr/lib/wine/wine64`, **not on PATH** - call it by
  full path.
- Fresh prefix: `rm -rf /root/.wine && DISPLAY=:99 /usr/lib/wine/wine64 wineboot --init`
- Wine has no real window manager here, so `SW_SHOWMAXIMIZED` does nothing -
  use `xdotool windowsize <hwnd> <w> <h>` to simulate a resized window.
- PIL/Pillow is often not installed after a reset - use `convert`
  (imagemagick) for cropping screenshots instead of assuming Python has PIL.

## Working with this user

- Terse, direct, technical. Don't over-explain; show the result.
- **Never claim a change happened without actually verifying it visually.**
  A past session shipped a code change (decoupling a UI label's position
  from its marker) whose screenshot looked identical to the prior version,
  because the numbers happened to coincide - that read as dishonest/not
  listening, not just an oversight. Always sanity-check that a screenshot
  actually shows the described change before sending it.
- This repo's established pattern (confirmed across sessions) is: commit +
  push after each shipped fix, without waiting to be asked each time -
  unlike the general default of "never commit unless asked."
- Rebuild + screenshot + get explicit confirmation before considering a
  visual/UI request done. Don't mark something finished on your own
  judgment alone when it was a direct visual request.
