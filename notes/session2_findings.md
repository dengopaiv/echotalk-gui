> ## HISTORICAL — the problem this was chasing was solved elsewhere
>
> The `$D009` findings are still accurate: that entry point blocks on a
> keypress. But it was being investigated as a route to speaking a single
> character without a line terminator, and that turned out to need
> nothing of the sort — see `single_char_return_bug_fixed.md`. Kept for
> the disassembly and the boot-harness fixes, which are still true.

# Session 2: boot harness findings

## Fixed to get the real loader running cleanly under simulation
1. `$AA59` (DOS register-save slot the loader reads at $9300 and restores
   at $93DD via TXS) must be seeded with the SP value in effect right
   after our harness pushes its fake return address -- otherwise the
   loader's final TXS+RTS jumps into garbage.
2. `$FBB3` (a real Apple II ROM signature/hardware-ID byte) must read
   as `$EA`. Left at 0 (our default), the loader's machine-detection
   code at $936B misdetects "unusual hardware" and sets an internal
   flag ($FD84) that routes character processing down an unwanted path.

With both fixed, `tools/boot_and_probe.c`'s loader run completes in
~2053 instructions and halts cleanly.

## The $BCF0 -> $D009 trampoline is real and confirmed working
The loader copies 14 bytes (source $9511-951E in TEXTALKER.RAM) to
$BCF0. Disassembled, that becomes:
```
BCF0: PHA
BCF1: LDA $C08B      ; LC bank read/write-enable softswitch
BCF4: JMP $D009      ; Textalker's own entry -- receives char via PHA
BCF7: LDA $C08A      ; LC bank switch back
BCFA: PLA
BCFB: JMP $FD1B      ; hand off to real screen-print continuation
```
We built our own equivalent trampoline (skipping the LC-bank-switch
reads, since our flat memory model doesn't need them) and confirmed via
full instruction trace that a character pushed this way is received
correctly by `$D009` (`PLA` at `$D00F` pulls exactly what we pushed).

## Where it gets stuck: $D009's processing always reaches a
## flashing-cursor / keyboard-wait loop and never returns
Trace shows $D009 -> ... -> $D08E (JSR $D3F4) -> $D3F7 checks $FD83,
$C01F -> falls into $D40F -> $D412 checks $FD84 -> EITHER branch
($D417 or $D428) ends up repeatedly calling $D44E (a ~12,544-cycle
delay+keyboard-poll subroutine) and looping back if no key was
detected -- unbounded without an actual keypress, confirmed by running
with step budgets up to 2,000,000 with no exit. This reproduces
identically for plain letters AND for the Ctrl-E mode-set sequence, so
it isn't specific to "printable character" handling.

This is almost certainly Textalker's **keyboard-input echo / blinking
"audio cursor" review-mode** logic (see manual Appendix B), not the
plain `PRINT`-driven output path -- meaning `$D009`/`$BCF0` is likely
the **keyboard** echo hook DOS installs, not the `COUT`/print hook.
Real `JSR $FDED` on a real machine after a plain `PRINT` apparently
does NOT go through this path, so there must be a separate, still-
unidentified hook for straight program-driven output.

## Next concrete step
Go back through TEXTALKER.RAM's own patch code looking specifically
for a *second* hook installation distinct from the `$A22B`/`$BCF0`
pair we already mapped -- likely targeting the real DOS/Applesoft
`COUT`-hook zero-page vector ($36/$37 itself, or another fixed DOS
low-memory slot) rather than the keyboard vector. Cross-reference
against the two other public entry stubs at the top of TEXTALKER.OBJ
we haven't fully explored ($D006 -> $D5DE) and $D003's OTHER caller
paths (recall $94F2: JMP $D003 was reached specifically on Ctrl-C,
suggesting $D003/$D682 might be a reset/reinit entry, not the per-char
one either).
