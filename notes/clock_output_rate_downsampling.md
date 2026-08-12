# A raised chip clock was being downsampled back to 8 kHz

Reported by a friend of Jayson's, testing the add-on. Not found by any of
the automated checks, which had only ever compared sample counts and
amplitudes — both of which downsampling leaves looking perfectly healthy.

## The defect

The clock multiplier works by *declaring* a different rate for the same
samples: the chip's state machine has no notion of wall-clock time, so
over/underclocking it is expressed entirely as "these samples are really
12000 Hz, not 8000". `emit_utterance()` then resamples from that declared
rate to whatever output rate the host asked for:

```c
unsigned native = (unsigned)(CHIP_HZ * et->clock_mult + 0.5);
if (native != et->out_rate && et->count > start) { ...resample... }
```

With the clock at 1.5 and the output rate left at 8000, that is a
**downsample** from 12000 to 8000. `src/resample.c` is linear
interpolation with no anti-aliasing filter — a deliberate choice, made so
that upsampling keeps the chip's grit instead of smoothing it into a
nicer DAC than the Echo II ever had. That choice is right for upsampling
and actively harmful downward: everything above the new 4 kHz Nyquist
folds back into the audible band rather than being filtered out. Real
detail the chip generated, thrown away and converted into aliasing noise.

The default path never hit it. At a 1.0 clock, `native == out_rate` and
the whole block is skipped, which is why every baseline in HANDOFF is
untouched by the fix and why nothing caught this earlier.

## Where the fix belongs

**Not in the library.** A host may be feeding an output device whose
format it cannot renegotiate, and silently delivering audio at a rate
other than the one it asked for would be a worse failure than the
aliasing — the host would hand a 12 kHz buffer to an 8 kHz device and get
chipmunks it did not ask for. So `echotalk_set_sample_rate()` still does
exactly what it is told, and `echotalk.h` now documents the hazard and
the rule.

The rule is: **treat the output rate as a floor**, and raise it to at
least `8000 * clock` whenever the clock is above 1.0.

Both hosts enforce it:

- **The NVDA driver** gains `_effectiveSamplerate()`, `max(chosen,
  8000 * clock)`, and `_applySamplerate()`, which pushes it and rebuilds
  the `WavePlayer` when it moves. Both inputs route through it — the
  sample-rate setting and the chip-clock slider — because either can
  change the answer. The user's own choice is stored untouched and
  applies again the moment the clock comes back down, so the settings
  dialog keeps showing what they picked rather than a value that moved
  under them.
- **`say`** raises `--rate` to meet the chip and prints a note saying so.
  It also reads the rate back out of the library afterwards rather than
  recomputing it, which removed three copies of `rate ? rate : 8000` that
  would otherwise have had to learn the same rule.

Rounding is `(unsigned)(8000 * clock + 0.5)` in all three places, matching
what the library itself computes, so the comparison is against the rate it
will really declare rather than one either side of it.

## What it is worth

At a 1.5 clock the output now lands at exactly 12000 Hz, which means
`native == out_rate` and **the resampler is not called at all** — the
samples reach the WAV or the audio device exactly as the chip made them.
The best possible outcome, and it falls out of the floor rule rather than
being aimed at.

Speed and pitch are untouched: "Rubber baby buggy bumpers." at clock 1.5
renders 1.565 s at 12000 Hz against 1.564 s when forced through 22050.
The clock effect is in the samples; only the delivery format changed.

## Ctrl-D can still get past it

`\x04 2C` changes the clock from inside the text, after the output rate
has been fixed. There is no going back and re-resampling by then, so
`say` checks where the clock ended up and warns if it finished above the
output rate. That catches setting it and leaving it, and misses a run that
raised it and put it back — stated in the code rather than papered over.
The NVDA driver is not exposed to this at all: it strips Ctrl-D from
anything that came off the screen.

## Verified

- Library baselines through `say` unchanged: hi_only **2297**, onset_this
  **10384**, chunked_paragraph_test **135213**, Hedge Trimmer Story
  **188812**. Nothing at a 1.0 clock moves, by construction.
- `make test`, `make test-dll` and `tools/test_nvda_driver.py` all pass.
- Six new driver checks covering the floor, the player being rebuilt at
  the raised rate, the user's choice surviving, a rate already above the
  chip being left alone, and the rate coming back down with the clock.
- **The two checks that matter were confirmed to fail on the old
  behaviour**, by making `_effectiveSamplerate()` return the chosen rate
  again: `8000 vs 16000` and a player still at 8000. The other four pass
  either way, which is correct — they describe behaviour the bug did not
  change.

## Also fixed here

`tools/test_nvda_driver.py`'s stub logger had no `debug()`. NVDA's real
`logHandler` does, so the first `log.debug` in the driver would have
raised `AttributeError` on a user's machine and nowhere else. The stub is
the whole safety net for a driver that cannot be run here in anger, so a
method missing from it is a hole in the net, not a detail.

## The lesson, which is the familiar one

Every existing check compared sample counts and amplitude ranges. Both
are exactly the wrong instruments here: a downsampled utterance has the
sample count it should have, and aliasing does not push the peaks out of
range. The defect was only ever audible, and it was found by someone
listening. That is the fourth time on this project that the ear has found
what the measurements could not — and, as with the pacing bug, the
measurements were not wrong, they were measuring the wrong thing.
