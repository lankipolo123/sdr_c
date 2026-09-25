# Engineering practices & lessons learned — for the other dev team

This is distilled from building the ECM Controller (`sdr_c`), a Win32 native C
app driven through an AI coding assistant over several sessions. It's not
this project's own internals doc (that's `CLAUDE.md`, project-specific) —
this is the general, transferable practices: things that caused real bugs or
wasted real time, and what fixed them. Apply whichever of these are relevant
to your own stack.

## Buffer safety with C-style formatting APIs

`wsprintfA`/`sprintf`-family calls have **no bounds checking**. Twice in
this project, a message buffer was sized by guessing ("64 bytes should be
plenty") instead of measuring the actual worst-case formatted string, and
both times a later text change (renaming a button label, lengthening a
status message) silently overflowed it.

**Practice**: when you size a fixed buffer for a formatted string, compute
the real max length (longest possible value for every `%s`/`%d` plus the
literal text) and size to that, not a round number that "feels safe." Redo
the math every time the literal text changes, not just when the format
string does.

## Explicit z-order / paint-order in any layered UI

In Win32 (and this generalizes to any retained-mode UI with manual
layering — Canvas-based UIs, custom layout engines), a control created
later paints **on top of** one created earlier at the same layer, by
default, with no warning. A layout that was tuned "by eye" can silently
start overlapping once one element's size or position changes, because
nothing was clamping the gap between them.

This caused a real bug that took multiple failed attempts to find, because
the failure looked like a rendering/shape bug in the element being clipped,
when the actual cause was a *sibling* element painted after it with no
overlap guard.

**Practice**: whenever two elements share a layout region whose size can
change (window resize, dynamic content), explicitly clamp their bounds
against each other in the layout code — don't rely on "there's usually
enough room."

## Verify shape/geometry math independently, don't eyeball a single render

A crescent-moon icon was built from two overlapping circles of the *same
radius*. That's provably incapable of producing a thin crescent — the
law-of-cosines coverage-angle math caps how much of the main circle's
boundary a same-radius mask can ever cover, regardless of offset — but a
single rendered screenshot "looked plausible" and was shipped as correct
three times before the actual math was checked.

**Practice**: for geometry you're not 100% sure about, verify it
analytically or with a quick throwaway script (e.g., Pillow/matplotlib) that
renders the *exact same* coordinates before committing to "it looks right."
A visual spot-check is not a substitute for confirming the math can even
produce the intended shape.

## Treat proprietary/opaque dependencies as black boxes — don't guess their internals

This project integrates a vendor-supplied `Transit.dll` for hardware
control, with zero source and zero documentation beyond its exported
function names. The temptation (from both the assistant and time pressure)
was to "fix" why a call like `AutoConnectSDR` wasn't finding the hardware —
but there's no way to fix logic you cannot see.

**Practice**: for a black-box dependency, put your effort into things that
*are* in your control around it — retry loops, clearer error surfaces
(`FormatMessageA`-decoded Win32 errors instead of a bare code), fallback UX,
timeout/backoff — and say plainly when a failure is inside the vendor code
and can't be fixed from your side. Don't let "make it work" pressure turn
into fabricated fixes for code you can't inspect.

## Layered fallback for OS enumeration APIs that lie

`HARDWARE\DEVICEMAP\SERIALCOMM` (the usual way to list COM ports) is a
mirror the driver writes, not the source of truth — on at least one real
machine it came back empty for a port that Device Manager showed as
present and openable. Falling back to `QueryDosDeviceA` fixed that but
introduced a worse problem: it lists every *reserved* COM number Windows
still remembers (Bluetooth SPP virtual ports, etc.) whether or not real
hardware is behind it.

The fix that actually matched Device Manager's own view: SetupAPI's device
enumeration (`GUID_DEVCLASS_PORTS`, `DIGCF_PRESENT`) — the same data source
Device Manager itself reads — run as a *supplement* after the registry
pass, deduplicated against it, not a replacement.

**Practice**: when an OS "list of X" API is unreliable, don't just swap in
a different API — understand *why* each one can be wrong (stale cache vs.
over-broad enumeration) and combine them so each one's failure mode is
covered by the other.

## Verification discipline (the most expensive lesson)

- **Never claim a visual or behavioral change happened without actually
  checking it.** A past change shipped with a screenshot that looked
  identical to the prior version purely by coincidence of numbers — that
  reads as dishonest, not just careless, to whoever's waiting on it.
- **Confirm you're testing the binary you think you're testing.** In a
  sandboxed test environment, a stale duplicate process from an earlier run
  produced misleading "nothing changed" results across genuinely different
  code. Kill everything, relaunch fresh, verify exactly one process/window
  exists, *then* check.
- **`pkill ...; next-cmd` or `pkill ... && next-cmd` chained in one shell
  call is a trap** — `pkill` exits 1 when nothing matched, which silently
  drops whatever was chained after it in some shells. Run them as separate
  commands.
- **A confident, detailed-sounding claim is not evidence.** A pasted
  summary (from a person or another AI session) describing specific work
  ("tried X two ways, both reverted") turned out to be entirely fabricated —
  zero matching commits anywhere in history. Check claims about "what was
  already done" against the actual git log/diff before acting on them or
  repeating them.

## Git tags as a trust/verification mechanism

When trust in "what's actually in this build" broke down, the fix wasn't
more explanation — it was matching a shipped binary back to an exact commit
(file size + md5 + build-log signature) and then tagging that commit as a
named, permanent checkpoint. Anyone (human or AI) can then cite the tag/hash
instead of a claim, and `git diff <tag> HEAD -- <path>` gives an exact,
checkable answer to "what changed since the last known-good state."

**Practice**: when a build/artifact needs to be verifiably reproducible,
tag the exact commit it came from at ship time, not after the fact from
memory.

## Working with an AI coding assistant on a long-running codebase

- Keep a persistent, project-specific handoff doc (this repo's is
  `CLAUDE.md`) that a **fresh session with zero chat history** could pick up
  from correctly — because sandboxed sessions can reset silently mid-project,
  and only what's committed/documented survives that.
- Be specific in feedback ("cut it by a third", "the gap between X and Y")
  — vague feedback ("looks off") costs a full extra round trip in either
  direction.
- Tune fast, ship slow: iterate quickly on the cheapest possible check
  (component in isolation, fake data injected temporarily) and only run the
  full build/package/install pipeline once the result already looks right.
