# Textalker v1.3: working, with caveats (session 4)

Continuing from `v13_compatibility.md`. Root cause of the "only 1 byte"
result found and fixed.

## Root cause: our echo-card status stub was too dumb for v1.3
v1.3's byte-I/O routine (`$EC0A`, itself **self-modifying code** --
the raw file has `LDA $C000`/`STA $C000` template instructions that
get their operand patched at runtime to the real target, e.g. `$C0A9`/
`$C0A0`, reused for multiple purposes including keyboard-timing
calibration elsewhere) genuinely polls the TMS5220 status byte for
specific bits before proceeding, unlike v3.1.3's more forgiving
polling. Our original "always return BE-only" status stub (matching
what worked for v3.1.3) left v1.3 spinning forever waiting for TS
(talk status) and BL (buffer low) to look "in progress."

Confirmed the `$60` byte it was stuck on is the real TMS5220 **SPEAK
EXTERNAL** command (0x60, verified against MAME's tms5220.cpp command
table) -- v1.3 correctly starts a speak-external sequence, our stub
just wasn't acknowledging it as "in progress."

## Fix
Made the status stub minimally stateful: once a SPEAK/SPEAK EXTERNAL
command byte (`(byte & 0x70) == 0x50` or `0x60`) is written, subsequent
status reads report `TS|BL|BE` (0xE0, OR'd with the usual 0x1F
pull-ups) instead of a permanently static "idle" value.

## Result
Sending `Ctrl-E,T`, `"HI"`, RETURN now produces **61 bytes**, opening
with `60 0C 48 5A 94 00 45 9B` -- the *exact same first 8 bytes* as
v3.1.3's 58-byte output for the same input. Strong evidence both
versions share essentially the same TMS5220 command/data protocol.

## Remaining issue (same root cause as v3.1.3's trailing $FF run)
It doesn't cleanly terminate -- it hits our step budget rather than
returning, still emitting `$FF` padding. This is the same artifact
we already understood for v3.1.3: our stub always claims "buffer
needs data" and never simulates a real FIFO filling up/draining, so
Textalker (either version) has no signal to stop. This should resolve
naturally once the real TMS5220 core replaces the stub -- it's not a
new v1.3-specific problem.

## Verdict, updated
v1.3 works with the same architecture as v3.1.3: same push-character-
and-jump calling convention (just a different target address, `$D400`
instead of `$D006`), same Echo II card I/O protocol, and (per the
matching byte prefix) essentially the same command/data format sent to
the TMS5220. The extra wrinkle is that v1.3's polling logic is less
forgiving of an unrealistic status stub, so it needs the slightly
smarter (but still simple) stateful status simulation implemented
above. Both versions should be well-served by the same real TMS5220
core once that's built -- version selection in the eventual library
should mainly be a matter of swapping a small table of fixed addresses
(load address, entry point, a couple of init routines).
