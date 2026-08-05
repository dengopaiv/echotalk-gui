# Fake6502 was compiled without BCD support (session 10)

## What was wrong
`third_party/fake6502/fake6502.c` shipped with `#define NES_CPU` active,
which is upstream Fake6502's default. That define compiles out the
decimal-mode correction blocks in `adc()` and `sbc()` entirely, because
the NES's Ricoh 2A03 has no working BCD. Fake6502's own header comment
says to comment it out if you are not emulating a NES.

The Apple II uses a standard MOS 6502 with working decimal mode, so this
was simply the wrong setting for this project, and had been since the
6502 core was first vendored in session 1. It was worth checking closely
rather than just flipping, because Textalker parses decimal digits for
its numeric commands (`nP` pitch, `nV` volume, `nD` inter-word delay) --
if any of that used BCD arithmetic, every numeric command would have
been computing wrong values all along, and `nD` in particular sits
suspiciously close to the open "speech pacing" complaint.

## What was actually measured
Two independent checks, rather than inferring from one:

**1. Direct instrumentation.** Added a temporary counter incremented
whenever `ADC` or `SBC` executed with `FLAG_DECIMAL` set, reported at
process exit. Result across all five `reference_text/` inputs, under
**both** Textalker v3.1.3 and v1.3 -- ten runs total:

```
count = 0        (every single run)
```

Textalker never sets decimal mode at all. Its digit parsing is plain
binary arithmetic.

**2. A/B output comparison.** Built the whole pipeline both ways and
compared the resulting WAVs by hash. All ten pairs byte-identical, with
sample counts matching every documented baseline:

| input                        | v3.1.3  | v1.3    |
|------------------------------|---------|---------|
| hi_only                      | 2811    | 13210   |
| chunked_paragraph_test       | 153283  | 157181  |
| command_coverage_test        | 268787  | 239098  |
| demo_bas_extracted_text      | 320698  | 319726  |
| alphabet_song_extracted_text | 120324  | 129813  |

## Resolution
`NES_CPU` is now commented out, so BCD behaves like real hardware. The
instrumentation was removed once it had answered the question; only the
one-line define change and an explanatory comment remain.

This is confirmed to have **zero** effect on current output -- it is a
correctness fix for future code paths, not a behavior change. Both
`hi_only` results still match the real-hardware capture exactly
(2811 samples, amplitude -12127/+26833).

## Note for whoever chases the pacing issue
This rules out one whole class of explanation: the numeric command
values Textalker computes (including `nD`, the inter-word delay) were
never being corrupted by missing BCD support. If pacing is wrong, it is
not because a delay parameter was mis-parsed.
