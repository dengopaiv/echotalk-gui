# What Textalker does with pitches above 63

**Status: measured, 2026-09-15.** Answers item 4 of HANDOFF's "What is
left": Jayson heard that `\x05 99P` is audibly not `\x05 63P` and suspected
a Textalker bug.

## How it was measured

With the GUI's pitch sweep (`gui-native`, "Pitch sweep 0 to 99 to folder",
or headless `echotalk_gui-x64.exe --selftest-sweep`). For each pitch n from
0 to 99 it renders the same text on a fresh machine with the raw bytes
`\x05` `n` `P` (or `F` when Monotone is ticked) placed in front of it. The
library's `echotalk_set_pitch()` clamps to 63, so the sweep deliberately
does not use it; the command goes to Textalker as bytes.

Every recording was reduced to the MD5 of its samples and the pitches were
grouped by identical hash. Settings otherwise at the Echo II defaults
(speed 1.0, volume 12, clock 1.0, 8000 Hz), text
`The quick brown fox jumps over the lazy dog.`, images from the unpacked
add-on in `..\echotalk\synthDrivers\echotalk\`.

`tools/verify_gui.py` separately checks that the sweep's files for pitches
0, 24, 63, 64 and 99 are byte-identical to the add-on's own
`echotalk64.dll` speaking the text after the same command, so the result
is Textalker's behaviour and not something the GUI introduced.

## Result

| Textalker | Command | Distinct recordings of 100 | Pitches that share one recording |
|---|---|---|---|
| 3.1.3 | `nP` (intonation) | 66 | 65–99 |
| 3.1.3 | `nF` (monotone) | 63 | 0–1; 63–99 |
| 1.3 | `nP` (intonation) | 66 | 65–99 |
| 1.3 | `nF` (monotone) | 63 | 0–1; 63–99 |

Both versions behave identically.

## What it means

- **Known:** with normal intonation, pitch keeps changing past the
  documented top of 63 for exactly two more steps. 64 and 65 each sound
  different from 63 and from each other; 65 through 99 are
  indistinguishable to the sample. So `99P` differs from `63P` because it
  is really `65P`.
- **Known:** in monotone, 63 is the ceiling, as documented, and pitches 0
  and 1 produce the same audio.
- **Inferred, not checked in the 6502 code:** the two commands saturate
  at different points because intonation adds its own offsets on top of
  the base pitch before a later clamp, so the base can usefully go two
  higher; and monotone's 0/1 collision is a floor in the same arithmetic.
  Confirming that means finding the clamp in the Textalker image, which
  has not been done.
- **Only one sentence was measured.** A text whose intonation contour
  never reaches the clamp could separate more pitches, so "65–99 are
  identical" is established for this sentence, not for every text.

The library's clamp to 63 is unchanged: getters and setters agreeing is
still worth more than the two extra steps, and the GUI's sweep reaches them
anyway.
