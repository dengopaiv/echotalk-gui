# Single-letter-word ("B") hang: root cause found and fixed (session 8)

## Symptom
Any Textalker v3.1.3 input containing a single-letter word (e.g. "B",
used as a buffer-mode-toggle command that falls through to being
spoken as literal text) would hang / run away into tens of thousands
of seconds of garbage audio, hitting the step budget.

## Investigation path (what didn't turn out to be the cause)
Several plausible-looking leads were traced and ruled out with direct
evidence before finding the real cause:
- **`E9EB`'s triple-nested delay loop**: looked suspicious (repeated
  `0xFF`/RESET-command writes nearby), but instrumented tracing showed
  it completes correctly in ~24,600 cycles for exactly the number of
  outer iterations its input parameter specifies. Not the bug.
- **The `$DBE3` Y-indexed table walk** (`CPY $FFAC`): looked like a
  candidate for an uninitialized-bound infinite loop (same shape as
  the `$FD88`/READ BYTE bug from the previous session), but every
  observed instance terminated correctly, `Y` reaching `$FFAC`'s value
  exactly as intended.
- **A suspected outer loop via `$E5AA`** (checking `$FD87`/`$FD90`):
  traced and found to never even be entered on this code path. Wrong
  assumption.

Confirming a large step budget didn't help (20x more budget produced
~20x more garbage audio, proportionally, never terminating) ruled out
"just needs more budget" and confirmed this is a genuine infinite
loop, not a merely-slow one.

## Root cause
Instruction-level PC tracing found execution landing at `$0000` and
staying there, with the stack pointer decrementing by exactly 3 every
iteration -- the signature of a `BRK` instruction (opcode `0x00`)
repeatedly firing and vectoring through the interrupt vector at
`$FFFE`/`$FFFF`, which is also unmapped/zero in this simplified
harness, sending execution straight back to `$0000`. A genuine,
unrecoverable infinite loop at the 6502 level.

Tracing back one step further found the actual jump: at `$D66D`,
Textalker branches on carry between two entry points into what is
clearly a single real Apple II ROM routine (the same one already
partially stubbed):

```
D66D: BCC $D672
D66F: JMP $BA83   ; already stubbed (PLA;RTS) since early sessions
D672: JMP $BA88   ; NOT stubbed -- falls outside the loaded OBJ's
                    address range ($D000-$FFF9), so it was raw
                    zeroed memory
```

`$BA83` was stubbed as `PLA;RTS` back when getting basic synthesis
working (needed for *some* code path to complete). `$BA88`, five
bytes further into what's almost certainly the same ROM routine at a
different entry point, was never given the same treatment -- so any
code path landing there (which single-letter words apparently do,
via a carry-flag-dependent branch) executed straight into unmapped
memory, hit `BRK`, and spun forever.

## Fix
Added the missing stub, symmetric to the existing one, in
`tools/render_text_real_chip.c`:

```c
mem[0xBA88] = 0x68; /* PLA */
mem[0xBA89] = 0x60; /* RTS */
```

## Verification
- "B" alone: was an infinite hang; now completes cleanly in 0.030s
  (242 samples), no warnings.
- Full `demo.bas` narration (509 characters, includes "B" being
  spelled out plus extensive other command sequences): was a runaway
  producing 14,000+ seconds of garbage; now completes cleanly in
  40.087 seconds, zero warnings, only minor clipping (0.45% of
  samples) consistent with genuinely loud passages.
- Regression check: `hi_only.bin` (2811 samples, exact match) and
  `chunked_paragraph_test.bin` (153283 samples, exact match) both
  produce byte-for-byte identical output to the pre-fix validated
  baseline. No regressions.

## Follow-up worth doing
- `tools/render_v13.c` stubs a different set of Apple ROM addresses
  (`$FC58`, `$FBFD`, `$9EBD`) for Textalker v1.3's different call
  pattern. Whether v1.3 has an analogous missing-sibling-entry-point
  gap hasn't been checked -- worth a quick pass if v1.3 single-letter
  words are ever found to hang the same way.
- More generally: any other `JMP $BAxx`/similar branch-to-real-ROM
  patterns in the v3.1.3 disassembly are worth a quick audit for the
  same "only one of two sibling entries stubbed" gap, now that this
  specific failure mode is known to exist.

## Wild-jump trap (added same session, right after this fix)

Both `render_text_real_chip.c` and `render_v13.c` now point the 6502
IRQ/BRK vector (`$FFFE`/`$FFFF`) at `$0300` (an unused RAM page)
instead of leaving it as zeroed memory. If execution ever drifts into
unmapped memory again -- the exact failure mode above -- it hits BRK,
lands at `$0300`, and `check_wild_jump_trap()` (called once per
executed instruction in `run_to_halt`'s loop) immediately prints the
*exact* originating address (read directly off the pushed BRK return
address on the stack, minus 2) and aborts, rather than silently
spinning for a multi-hour trace-and-guess diagnosis. Verified against
this exact bug: temporarily removing the `$BA88` stub, the trap
caught it and correctly reported `$BA88` in 14ms.

## Alphabet song verification
`reference_text/alphabet_song_extracted_text.bin` (26 letters, each
with individual pitch variation, several as single-letter "words" --
exactly the shape that used to hit this bug repeatedly) now renders
cleanly end to end: 15.040s, zero warnings, no trap triggers, minor
clipping (0.13% of samples) consistent with normal loud passages.
