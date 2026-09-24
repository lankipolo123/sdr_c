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

**`Transit.dll` may be missing** after a reset (uploads never survive one) —
ask the user for it before doing a "final" build; a copy also isn't in git
(`dll/` is gitignored, proprietary vendor file). As of the last session the
real DLL is small (68KB) and dynamically links against the VC++ runtime
(`msvcp140.dll`/`vcruntime140.dll`/`vcruntime140_1.dll`) - see the vcredist/
section below, and transit_dll.h's header comment for which exports a given
DLL build has (`GetDllPassword` vs `ValidateDllPassword` - it's changed
between builds already, check before assuming either exists).

## Build commands (plain dev exe)

```
x86_64-w64-mingw32-windres src/app.rc -O coff -o src/app_res.o
x86_64-w64-mingw32-gcc -std=c99 -Wall -Wextra -Wpedantic -Werror -Wno-cast-function-type -mwindows -Os -s \
  -fno-ident -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -o digital_noise_config_multi.exe src/main.c src/connection.c src/channels.c src/protocol.c \
  src/serial_port.c src/modbus.c src/sensor.c src/sensor_log.c src/sensor_shared.c src/transit_dll.c src/app_res.o \
  -ladvapi32 -lgdi32 -luser32 -lmsimg32 -lgdiplus -lsetupapi
```

`build.bat` in the repo root is stale (missing `-lgdiplus -lsetupapi`, and the
`sensor_log.c`/`sensor_shared.c` files) — don't trust it blindly, it hasn't
been kept in sync.

### Background service exe (separate binary, separate build command)

```
x86_64-w64-mingw32-windres src/sensor_service.rc -O coff -o src/sensor_service_res.o
x86_64-w64-mingw32-gcc -std=c99 -Wall -Wextra -Wpedantic -Werror -Wno-cast-function-type -Os -s \
  -fno-ident -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -o ECMControllerSensorService.exe src/sensor_service.c src/serial_port.c src/modbus.c src/sensor.c \
  src/sensor_log.c src/sensor_shared.c src/sensor_service_res.o \
  -ladvapi32 -luser32
```

No `-mwindows` (console subsystem, so `printf` in `--debug` mode is actually
visible) and it does NOT get obfuscated/protected - it has no vendor DLL path
or password to hide, unlike the main exe. See "Background sensor service"
below for what this is and how to test it.

**Direct decision (2026-09-24): ship the PLAIN build, not the obfuscated one.**
Windows Defender was flagging real installs as a virus - unsigned exe, zero
accumulated hash reputation (changes every rebuild), and the obfuscated
build's own string-hiding + `LoadLibraryA`-ing a vendor DLL at runtime is a
textbook AV heuristic false-positive profile. Traded away for what the
obfuscation actually bought (hiding literal strings from a casual binary
viewer, not real protection). So: build straight from `src/` with the plain
dev exe command above, run `makensis installer.nsi` against THAT exe, ship
it. Do NOT run `tools/obfuscate.py`/build from `build_obf/src/` for a normal
ship - that pipeline is being kept (see gotchas below, still accurate) in
case a real signing story shows up later and obfuscation is worth
reconsidering, but it is NOT the default anymore. If asked to bring it back,
this is a reversal of a direct decision - confirm before doing it.

## Obfuscation gotchas (`tools/obfuscate.py` -> `build_obf/src/`) - NOT currently used for shipping, see above

Regenerating the obfuscated source needs manual hand-fixes every time, for
things the script doesn't handle automatically:

- `PARITY_LABELS`, `LEVEL_LABELS` - need backing-buffer arrays +
  `obf_init_label_arrays()` called in `WinMain`
- `row_select_items` - inline decode-on-declaration (block-local array)
- `MODE_NAMES` (in `protocol.c`) - lazy-init-on-first-call
- `TRANSIT_DLL_PATH` macro (in `connection.c`) - manual conversion

Skipping any of these breaks the protected build silently or at runtime, not
at compile time.

**A 5th case has bitten this twice now**: any `static const char foo[] =
"literal";` at file/function scope that ISN'T in `KNOWN_ARRAY_NAMES` still
gets auto-transformed into `static const char foo[] = obf_decode(...);` -
which doesn't compile (a static initializer must be a compile-time constant,
and a function call isn't one). This has happened to the CSV header string in
`sensor_log.c`'s `sensor_log_start_new_week()` in both `build_obf/` regens so
far. Fix: change `static const char x[] = ...` to `const char *x = ...` (drop
`static`, use a pointer not an array) and switch any `sizeof(x)` on it to
`lstrlenA(x)`. Check the build error - it will say "invalid initializer" at
the exact line - rather than trying to spot these by eye beforehand.

Only the GUI (`digital_noise_config_multi.exe`) gets obfuscated. The
background service exe is built straight from `src/` (see its own build
command above) - it has nothing worth hiding.

## vcredist/ - Transit.dll's own runtime dependency

`vcredist/` holds 3 genuine Microsoft DLLs (`msvcp140.dll`, `vcruntime140.dll`,
`vcruntime140_1.dll`), committed to git (not proprietary, unlike Transit.dll -
see `vcredist/README.md` for exactly how they were extracted straight from
Microsoft's own `vc_redist.x64.exe` and why this is legitimate). `installer.nsi`
installs them next to the exe. This exists because Transit.dll is an MSVC
build that fails to load with error 126 ("module not found") on a machine
missing the VC++ Redistributable - a real, confirmed report, not a
hypothetical. If Microsoft ships a newer Transit.dll build requiring a newer
runtime version, re-run the extraction steps in `vcredist/README.md`.

## Background sensor service (ECMControllerSensorService.exe)

Direct request: keep BAY1-4 CSV logging running even after the GUI is
closed. Solved with a real Windows Service (`src/sensor_service.c`), not a
tray-minimize or a plain background process - see the commit that added
this for the full tradeoff writeup the user chose between. Key points for
picking this back up:

- **Opt-in, not automatic.** `installer.nsi` bundles the service exe but
  does NOT register it as a running service - that needs admin rights,
  which the base app install deliberately doesn't require (see that file's
  own top comment on why: writing to Program Files without elevation
  silently breaks all persisted settings). Registration happens via
  separate Start Menu shortcuts ("Install/Uninstall Background Logger
  Service") that run the service exe with `--install`/`--uninstall`, each
  triggering its own UAC prompt via `src/service_admin.manifest`
  (`requireAdministrator`) - independent of the main installer's own
  unprivileged level.
- **Owns the sensor COM port exclusively once running** - a serial port
  can't be opened by two processes at once. The GUI's own "Ambient
  Temperature" Connect button still exists and still tries to open the
  port directly; if the service already holds it, that attempt just fails
  harmlessly (existing error handling), while the shared-memory path below
  keeps the live display working regardless.
- **Shared memory** (`src/sensor_shared.c/.h`) is how the GUI gets live
  BAY readings without touching the port itself: a named file mapping
  (`Global\ECMControllerSensorShared` - the `Global\` prefix is load-
  bearing, not stylistic, since a service runs in Session 0 and a `Local\`
  name wouldn't be visible to the interactive GUI's own session) the
  service publishes into and the GUI reads, with a heartbeat timestamp so
  the GUI can tell "service crashed/stopped" from "service running but sensor
  disconnected" and fall back to polling the port itself.
- **CSV logging** (`src/sensor_log.c/.h`) is shared code, not duplicated -
  used by the service (the normal case once installed) and by the GUI
  itself (fallback, only when it's the one actually polling - never both
  at once, or two processes would race the weekly file rotation). Fixed
  filenames (`sensor_log.csv`, `sensor_log_state.ini`) resolved next to
  whichever exe is running the code, not derived from that exe's own name -
  both binaries need to agree on the same path regardless of which one
  is currently writing it.
- **The service reads the GUI's configured sensor port from
  `ECMController.ini`** (hardcoded filename in `sensor_service.c` -
  matches `installer.nsi`'s `EXE_NAME`) since that's the only place the
  chosen COM port lives; re-reads it every reconnect attempt (~5s), so
  changing the port in the GUI takes effect without restarting the service.
- **Not independently verified in this sandbox**: the actual SCM-managed
  install/start/stop lifecycle. Wine's own `services.exe` didn't behave
  like documented real-Windows behavior when this was tested
  (`StartServiceCtrlDispatcherA` blocked instead of failing fast when not
  actually SCM-launched) - which is exactly why `--debug` mode exists: it
  bypasses the SCM path entirely and just runs the same loop in a plain
  console, which IS how the core polling/shared-memory/CSV logic got
  verified end-to-end (including a real cross-process shared-memory read
  from a separately-launched GUI). The `--install`/`CreateServiceA`/real
  service-running path itself needs a real Windows machine to confirm.

## Testing in this sandbox (no real Windows available)

- `wine64` binary is at `/usr/lib/wine/wine64`, **not on PATH** - call it by
  full path.
- Fresh prefix: `rm -rf /root/.wine && DISPLAY=:99 /usr/lib/wine/wine64 wineboot --init`
- Wine has no real window manager here, so `SW_SHOWMAXIMIZED` does nothing -
  use `xdotool windowsize <hwnd> <w> <h>` to simulate a resized window.
- PIL/Pillow is often not installed after a reset - use `convert`
  (imagemagick) for cropping screenshots instead of assuming Python has PIL.

## Working efficiently (read this first if tokens/time are tight)

Lessons from where this project actually burned tokens on mistakes/re-work:

- **Tune visuals on the plain dev exe only, in Wine, before touching the
  obfuscated build.** The full pipeline (obfuscate -> reapply the 5 known
  hand-fixes -> protected build -> installer) should run ONCE, after the
  plain build already looks right - not on every color/alpha tweak. Building
  the installer to check a saturation change is a wasted cycle.
- **For heatmap/visual tuning, inject known fake values instead of relying on
  the user's real hardware to see a result.** In
  `sensor_heatmap_subclass_proc()`'s `WM_PAINT`, right after `GetClientRect`,
  temporarily loop `g_sensor.units[i].has_reading = true;
  g_sensor.units[i].temperature_c = <fixed value>;`, rebuild, screenshot via
  Wine+Xvfb (`DISPLAY=:99`, launch the exe, `import -window root out.png`),
  then remove the block before committing - it must never ship. This turns a
  "does it look right now?" round trip into one self-contained check.
- **Crop to the changed region and stack before/after with
  `convert a.png -crop WxH+X+Y crop_a.png` then
  `convert crop_a.png crop_b.png -append compare.png`** instead of sending
  two full screenshots - makes the actual difference visible in one image
  instead of asking the user to spot it across two.
- **Vague visual feedback costs a full extra round trip either direction.**
  A rough magnitude ("about half as strong", "cut it by a third") or a
  pointed location beats "looks off" - fewer guess-and-check cycles.
- **Never chain `pkill ...; next-cmd` or `pkill ... && next-cmd` in one
  call** - pkill exits 1 when nothing matched, which drops `next-cmd`
  silently in this tool's shell semantics (already bit this project's own
  testing more than once - always verify with `md5sum` before trusting a
  "rebuilt" binary). Always run them as separate tool calls.
- **This file is what survives a hard-refresh/new session, not the chat.**
  Before ending a work session, make sure whatever changed and why is
  reflected here (not just committed in code) - a fresh session with zero
  chat history should be able to pick up correctly from this file alone.

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
- **The user sometimes pastes in a summary from a different Claude session
  working on this same repo, asking whether it's accurate.** Don't take it at
  face value - check it against real `git log`/`git show` output. A past
  instance of this pasted a detailed, specific-sounding claim ("tried
  bundling the VC++ redistributable two ways, both reverted per direct
  request") that turned out to be entirely fabricated - zero matching
  commits anywhere in the repo's history, on any branch. It also mislabeled
  a real commit's content. Confident, detailed prose is not evidence; grep
  the actual history before repeating or acting on a claim about "what was
  already tried."
