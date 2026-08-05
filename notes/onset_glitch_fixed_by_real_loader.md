# Onset glitch: FIXED by booting through Textalker's real loader (session 10)

Confirmed by ear by the user, in all cases tested. This supersedes the
conclusion in `notes/onset_glitch_investigation_reverted.md`.

## The hypothesis that cracked it
From the user: v1.3 never exhibited the glitch, and the v1.3 harness
runs its **real loader** -- while the v3.1.3 harness had skipped its
loader entirely since session 3, when calling `$D003` + `$FCD6` directly
proved sufficient to produce speech. "Sufficient to produce speech" was
never the same claim as "does everything the loader does", and
`roms/textalker.ram.bin` had been sitting unused in the repo the whole
time.

Session 9 had attributed the glitch to the lattice filter ringing up
from zero history on a cold start, and treated it as genuine chip
behavior that only a warm-up could mask. That diagnosis of the filter's
mechanics was correct in isolation, but wrong about the cause: the real
problem was an incomplete boot, not cold silicon.

## What running the real loader exposed
It immediately hit the wild-jump trap at `$FDF3`, which turned out to be
a much deeper modelling gap than a missing stub.

**Textalker lives at `$D000-$FFF9` in the language card, shadowing the
Apple II monitor ROM.** So `$FDED`, `$FDF0` and `$FD1B` name Textalker
code or ROM code depending purely on bank state, and Textalker's own
trampolines switch banks deliberately (`LDA $C08B` selects the card,
`LDA $C08A` selects ROM). A flat 64K model cannot express this, and the
consequence was that the old harness's "monitor ROM stubs" were being
written directly over Textalker's own image.

The direct-init harness only got away with it because its hand-written
`PLA`/`RTS` stub at `$BA83` returns before the ROM-side exit path is
ever reached. With the real loader installed, that path runs.

`tools/render_text_loader.c` therefore models the two softswitches and
gives `$D000-$FFFF` a shadow bank filled entirely with `RTS` -- no Apple
ROM code is reproduced, and every monitor entry point becomes a harmless
no-op that returns to its caller, which is exactly right for a headless
renderer with no screen. Textalker's image is now left fully intact.

## The trampoline the loader installs, dumped from memory
```
BA7C: 48        PHA              ; entry: caller passes char in A
BA7D: AD 8B C0  LDA $C08B        ; switch the card in
BA80: 4C 06 D0  JMP $D006
BA83: AD 8A C0  LDA $C08A        ; exit path 1: switch ROM back
BA86: 68        PLA
BA87: 60        RTS              ; talk-only -- just return
BA88: AD 8A C0  LDA $C08A        ; exit path 2: switch ROM back
BA8B: 68        PLA
BA8C: 4C F0 FD  JMP $FDF0        ; also print the character to screen
```
This is the ground truth behind the session 8 single-letter-word bug.
`$BA88` is not a redundant second entry into the same routine, as was
assumed then -- it is the *screen-printing* variant, and `$D66D` picks
between the two on the carry flag. Stubbing both as `PLA`/`RTS` was an
approximation that happened to work.

Two further consequences, both load-bearing:
- The loader **returns with the ROM bank selected**, so characters must
  be fed through `$BA7C` rather than jumped straight into `$D006` --
  otherwise `$D006` does not even name Textalker at that moment.
- The loader must be entered with the ROM bank selected, as DOS does.
  Starting it on the card instead makes it take a different path
  (1865 steps versus 2053 in the old flat model) and speech comes out
  malformed.

## Measured effect
| case | direct init | real loader |
|---|---|---|
| "This is a test." | peak 32768, **728 clipped** | peak 26575, **0 clipped** |
| "A" | peak 32768, **15 clipped** | peak 21931, **0 clipped** |
| command_coverage | 1463 clipped | 1174 clipped |
| alphabet_song | 159 clipped | 154 clipped |
| hi_only | 2811 samples, -12127/+26833 | **identical** |

The real-hardware amplitude match for "HI" is preserved exactly, which
was the constraint that killed both earlier fix attempts. Durations move
by under 2% in both directions -- some outputs are *longer* -- so this is
not the truncation failure mode that invalidated attempt 3.

Worth noting for anyone re-reading the old notes: the measurable change
is not at the onset itself (the onset peak is identical either way) but
in full-scale clipping shortly after it, e.g. from 0.376s in "This".
What the user hears as the glitch corresponds to that clipping.

## Status of the old theory
The lattice filter analysis in the session 9 notes is not wrong about
what a 10th-order filter does from zero history -- it just was not what
was happening here. Nothing in the current code compensates for filter
warm-up, and no warm-up delay is needed.
