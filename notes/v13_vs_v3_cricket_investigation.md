# v1.3 vs v3.1.3: testing the Cricket-detection hypothesis (session 7)

## The question
Textalker's card-detection routine tests I/O addresses for slots 4, 5,
7, 3, 2, 1, 6 (plus one extra), in that order -- slot 2 being the 5th
tried, not privileged. Separately, v3.1.3 issues a `READ BYTE` command
(`$0B`, `$9E` written to the echo port) during its `$D682` init routine
that is never consumed before the slot scan runs, and our simplified
TMS5220 port originally completed that command *instantly* rather than
over real chip time -- corrupting the very first status read the scan
does. Modeling realistic completion latency for that command (~600
generated samples, found empirically -- see the comment in
`tms5220_core.c`) fixed it.

The question raised: is any of this related to Cricket support (the
Apple IIc's serial-attached Echo synthesizer, which post-dates Echo
II), and would Textalker v1.3 -- which predates the Cricket entirely
-- behave differently?

## What v1.3's boot sequence actually contains
Traced v1.3's RAM loader (`TEXTALKER_RAM.OBJ_062400`, loads at $9300)
against v3.1.3's `$FCD6`:

- **The 8-candidate slot scan is byte-for-byte identical** in both
  versions: same candidate list (`C0 D0 F0 B0 A0 90 E0 F2`), same
  order, same structure (patch a template read instruction's operand,
  read twice, mask/compare). This scan is *not* new in v3.1.3 -- it's
  inherited unchanged from at least v1.3, meaning slot 2 isn't
  privileged for any Cricket-related reason. It's simply 5th in a
  generic multi-slot search that predates the Cricket by definition.
- **The `$0B`/`$9E` READ BYTE probe that v3.1.3 issues before the scan
  is completely absent from v1.3** -- searched both the OBJ and RAM
  loader files for that exact byte pair and for any `LDA #$9E`; zero
  matches in either v1.3 file. v3.1.3 added something here that v1.3
  never had.

## The direct test
Got v1.3 running through the same real-6502 + real-TMS5220 pipeline
(`tools/render_v13.c`) -- its loader needs a handful of real Apple ROM
monitor calls stubbed as harmless no-ops (`$FC58`, `$FBFD`, `$9EBD`,
apparent screen/calibration text, not detection logic) and a
workaround for the loader's own `TXS` mid-execution (it restores a
caller-saved stack pointer that doesn't exist in our synthetic boot,
so we cooperate with it rather than fight it -- see the comment in
render_v13.c). Once running:

- Candidate detection succeeds (matches slot 2, `$A0`) exactly as in
  v3.1.3.
- The loader's own calibration delay takes ~552,000 6502 steps --
  matching this project's very first session notes about v1.3 almost
  exactly ("~552K instructions with fake6502"), a nice independent
  cross-check that this boot sequence is being reproduced correctly.
- **"HI" comes out with the identical amplitude profile as v3.1.3's
  known-good result** (-12127/26833, matching both the real hardware
  reference capture and this project's validated v3.1.3 output).
- **Setting `TMS5220_READ_COMMAND_DELAY_SAMPLES` to 0 -- completely
  disabling the timing fix -- produces byte-for-byte identical output**
  for v1.3 (same step counts, same `$EC0B` match, same 13210 total
  samples). v1.3 simply never exercises the code path that fix
  addresses.

## Conclusion
The refined picture: this isn't "Textalker prefers slot 2 because it's
checking for a Cricket on serial port 2 first" -- the slot-scan itself
is old, generic, and unordered by any such preference. What's real is
narrower and, if anything, more interesting: **v3.1.3 added a new
probe step (the READ BYTE command) ahead of that same inherited scan,
and that new step is exactly the one piece of the boot sequence that
turned out to be timing-sensitive in a way v1.3 never was.** Whether
that specific probe is Cricket-detection logic specifically (as
opposed to some other hardware-capability check added in the same
era) isn't something this test can confirm directly -- doing so
would need something like a disassembly cross-reference against a
documented Cricket protocol, or a v2.x-era Textalker release to
narrow down when the probe was actually introduced. But the core
empirical claim -- that this is new-in-v3.1.3 behavior absent from
v1.3, not a Cricket-vs-slot-2 prioritization -- is now directly
verified rather than speculative.
