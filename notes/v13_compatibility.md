# Textalker v1.3 (RAM CARD) compatibility check

Files: `TEXTALKER.RAM_069300` (611 bytes, different from the v3.1.3
loader despite the same filename) + `TEXTALKER_RAM.OBJ_062400`
(11,264 bytes). Decoded banner strings confirm: **TEXTALKER (TM), Echo
Speech Generator, VERSION 1.3 (RAM CARD), COPYRIGHT 1981** -- a much
older release than the v3.1.3 files from the main sessions.

## Load address
The uploaded filename suffix says $2400, but that's misleading -- the
loader's own embedded string says `BLOAD TEXTALKER.RAM.OBJ,A$D400`,
and this is confirmed correct by disassembly: the OBJ image is
11,264 = 0x2C00 bytes, and $D400 + 0x2C00 = exactly $10000, i.e. it
fills $D400-$FFFF precisely (no room reserved for vectors, unlike
v3.1.3's $D000-$FFF9).

## Same overall architecture, different addresses
- Same install-then-patch-a-fixed-hook-address pattern as v3.1.3.
- The loader patches `$A22B` -> `JMP $BA69` (identical mechanism,
  identical target address even) for what we now believe is the
  Ctrl-C/break vector, not the real speech path (consistent with the
  v3.1.3 finding).
- The real per-character entry point, found by decoding the relocated
  patch code the loader installs (source $9533 -> destination $BA82):
  ```
  BA82: PHA
        LDA $C08B
        JMP $D400      ; the very first byte of TEXTALKER_RAM.OBJ,
                        ; itself "JMP $D71C"
  ```
  So: **push the character, jump to $D400** -- same calling
  convention as v3.1.3's $D006, just a different target address (as
  expected, since this is a different, older binary).
- A TMS5220 presence/timing-calibration routine runs during the
  loader's own boot (calling into OBJ at `$EC0A`/`$EC17`, analogous to
  v3.1.3's `$FCD6`), including a real CPU-speed calibration delay loop
  (bounded, just needs a large step budget in emulation -- confirmed
  by raising it and watching the loader complete in ~552,000
  instructions).

## Confirmed: it runs under the same emulation approach
`tools/probe_v13_reference.c` boots the real loader against the same
minimal (non-Apple, non-copyrighted) stub ROM used for v3.1.3, then
calls `$D400` directly per character. Sending `Ctrl-E,T`, `"HI"`,
RETURN:
- Mode-set and letter characters process cleanly, no Echo-card traffic
  (correct, same buffering behavior as v3.1.3).
- RETURN completes normally (no hang, ends via a `PLA`/`RTS` exit
  trampoline structurally identical to v3.1.3's) but only writes
  **1 byte** (`$60`) to the Echo card, not a full phoneme/LPC frame
  like v3.1.3's 58 bytes for the same input.

## Verdict for this side quest
**Structurally, yes -- same emulation approach applies.** Same
push-character-and-jump calling convention, same Echo II I/O protocol,
same general loader/init shape. The two versions are close enough that
supporting both should mean mostly swapping in different fixed
addresses (per-version constants), not a different architecture.

**Not fully working yet for v1.3 specifically.** Only getting 1 byte
out where a full utterance is expected suggests either: a different
mode/command-character convention in this older version (v1.3 may
predate some of the Ctrl-E command set documented in the manual we
found, which describes later versions), a different word-buffering
trigger, or a missing piece of setup analogous to what `$FCD6` turned
out to matter for in v3.1.3. Worth another focused session if you want
v1.3 support specifically, but it's not needed to validate that the
project's overall approach generalizes.
