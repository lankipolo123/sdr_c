# Temp/Humidity sensor integration plan (XY-MD02, RS-485/Modbus RTU)

Target: this app (`sdr_c`, the 16-channel multi build), not the single-channel
`digital-noise-configuration` app. Runs on its own second COM port, fully
independent of the RS-422 channel control path (different physical adapter,
different protocol, different serial settings).

## Confirmed on real hardware via QModMaster - use these, don't re-derive

- Sensor model: XY-MD02 (label read directly off the unit)
- Serial: 9600 baud, 8 data bits, no parity, 1 stop bit
- Slave/device address: 1
- Function code: **0x04 (Read Input Registers)** - NOT 0x03 (Read Holding
  Registers), which is what generic documentation for this device family
  suggests and what earlier assumed/guessed code got wrong
- Register 2 = temperature, register 3 = humidity, both raw value / 10
  (e.g. raw 281 -> 28.1 degC)
- Power: adapter's own +5V/GND breakout pins power the sensor directly - no
  separate wall adapter needed for this specific USB-RS485 dongle (confirmed
  it exposes A/B/GND/GND/+5V pins)

## Why wait-for-response here, unlike channels.c's blind send

The RS-422 channel controls use fire-once/no-ACK because silence on that bus
is ambiguous (shared bus, no reliable failure signal). Modbus RTU reads are
different: we're asking for a value and there is nothing to apply
optimistically without it - so this needs an actual send -> wait for
reply-or-timeout -> parse cycle, same shape as the single-channel app's
device.c response handling. Still non-blocking / no threads: driven off the
same WM_TIMER tick the rest of this app already uses.

## Architecture

1. `src/modbus.c` / `modbus.h` - Modbus RTU frame builder/parser: CRC16 +
   the 0x04 request/response format specifically (not a general Modbus
   library). Same shape as the existing `protocol.c`, different wire format.
   Testable standalone, no hardware needed (CRC16 has known test vectors).

2. A second `Connection`-style serial handle on its own COM port, reusing
   `serial_port.c` as-is (already port-agnostic, just needs a second
   instance opened on a different port at 9600/8N1 instead of whatever the
   RS-422 side uses).

3. `src/sensor.c` / `sensor.h` - polling state machine:
   idle -> send Read Input Registers (slave 1, addr 2, count 2) ->
   wait for reply or timeout -> parse two registers -> apply
   temperature_c / humidity_pct state, mark online -> wait a poll interval
   (a few seconds is plenty, this isn't a fast-changing value) -> repeat.
   On timeout: mark offline, don't block the rest of the app, try again
   next interval.

4. UI: a new panel (chamfered box + header icon, same visual language as
   the rest of this app) with:
   - Its own Port dropdown + Connect button, separate from the RS-422 one
   - `Temp: -- degC` / `Humidity: -- %` - uses the same "-" placeholder
     rule already established elsewhere in this app family: never show a
     value until a real reading has actually confirmed it
   - Plain-language status on failure ("Sensor not responding"), not raw
     Modbus exception codes

## Build order

1. `modbus.c` (CRC16 + frame format) - standalone, verify against known
   CRC16 test vectors before touching hardware
2. Second serial connection wiring (reuse serial_port.c)
3. `sensor.c` polling state machine
4. UI panel
5. Build, smoke-test in Wine (won't have real hardware there), then verify
   against the real XY-MD02 the same way QModMaster did
