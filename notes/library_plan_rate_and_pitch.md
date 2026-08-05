# Library plan: output rate, clock multiplier, and speech rate

Planning notes for the three audio features requested before the library
build. Written before implementation; revise as things are learned.

## The control surface these give us

The important realisation is that the three knobs are **independent**,
and together with Textalker's own pitch command they cover what a screen
reader actually wants:

| control | speed | pitch | mechanism |
|---|---|---|---|
| Textalker `nP` command | no | **yes** | Textalker, already works |
| frame rate (feature 3) | **yes** | no | TMS5220 interpolation periods |
| clock multiplier (feature 2) | **yes** | **yes** | declared sample rate |
| output rate (feature 1) | no | no | resampling |

So rate can be changed *without* the chipmunk effect (feature 3), and
the chipmunk effect is available deliberately if wanted (feature 2).
That is a better outcome than either alone.

## Feature 1: inline resampling — trivial

`resample_naive()` already exists in `tools/resample_wav.c`: linear
interpolation, deliberately **no anti-aliasing**, so content above the
new Nyquist aliases rather than being filtered. That was a deliberate
choice to keep the chip's gritty character rather than making it sound
like a better DAC than the Echo II had. Keep that.

Work: move it to `src/resample.c/.h`, call it on the way out of the
library, expose the target rate. `tools/resample_wav.c` stays as a
standalone utility.

## Feature 2: clock multiplier — trivial, already proven

The chip's state machine has no concept of wall-clock time;
`tms5220_process()` advances one internal sample per call regardless.
All clock-dependent behaviour (pitch period, frame duration) is
expressed in **sample counts**. So the classic over/underclocked
"chipmunk" effect needs no change to synthesis at all — just declare a
different rate for the same samples.

- Echo II's real clock is 640 kHz, giving 640000/80 = 8000 Hz native.
- Generate normally; declare the output as `8000 * multiplier`.
- Then resample that to the user's target rate (feature 1).

Verified previously in `render_wav.c`: 2x gives exactly half the
duration, 0.5x exactly double, with pitch moving to match. Order
matters -- apply the multiplier first, then resample, so the two
compose.

## Feature 3: finer speech rate — the interesting one

Textalker offers only compressed and expanded, and does it by skipping
phoneme segments rather than changing playback rate. So rate control has
to come from the chip.

### The lever already exists and needs no core change

In `parse_frame`:

```c
tms->m_IP = reload_table[tms->m_c_variant_rate & 0x3];   /* {0, 2, 4, 6} */
```

This is the `else` branch, taken by **every** variant including a plain
5220. `m_c_variant_rate` is only written by the SET RATE command, which
is a NOP unless the chip is a 5220C, so on our 5220 it stays 0 forever
and `IP` always reloads to 0 -- a full 8 interpolation periods per
frame.

Setting that field directly from the library gives four frame lengths:

| `m_c_variant_rate` | IP starts at | periods | frame | speed |
|---|---|---|---|---|
| 0 | 0 | 8 | 200 samples | 1.00x |
| 1 | 2 | 6 | 150 samples | 1.33x |
| 2 | 4 | 4 | 100 samples | 2.00x |
| 3 | 6 | 2 | 50 samples | 4.00x |

**Pitch is unaffected**, because the pitch period is counted in samples
and nothing here changes it. Only the rate at which frame parameters
advance changes. That is time compression without pitch shift, which is
exactly right for a screen reader.

Caveats to be honest about:

- **Speeds up only.** `reload_table` has no entry giving more than 8
  periods. Going slower needs the `m_subc_reload` route (0 instead of 1
  gives 3 samples per PC step instead of 2, so ~1.5x longer frames,
  about 0.66x speed) or the accumulator below.
- **Quality degrades with fewer periods.** Interpolation coefficients
  are indexed by IP, so starting at 4 or 6 skips the early, gentler
  interpolation steps and formant transitions get blockier. At 2
  periods it will likely sound rough. This is real 5220C behaviour, not
  invented, but it is a timbre change as well as a speed change.
- It is an **emulator capability, not something a real Echo II could
  do** -- that card has a plain 5220. Worth saying plainly in any UI.

### For continuous control, an accumulator in the core

Four steps, all faster than normal, is not enough for a screen reader
that wants roughly 0.5x to 3x continuously. The general mechanism is to
control **how many samples each interpolation period emits** -- 25 now,
38 with `subc_reload = 0` -- via a fractional accumulator rather than a
fixed choice.

Pitch stays correct under this too: `m_pitch_count` advances per sample,
so F0 is unchanged whether a period is stretched or compressed.

This does mean touching the `subcycle`/`PC`/`IP` machinery, which is
precisely where the pacing bug lived, so it needs a hard gate:

> **At rate 1.0 the output must be byte-identical to the current
> baselines, for every input in `reference_text/`, under both Textalker
> versions.** If it is not, the change is wrong regardless of how good
> the other rates sound.

## Suggested phasing

1. **Resampler inline** and **clock multiplier**. Both are settled
   mechanisms with no core risk; they can land with the first version of
   the library API.
2. **Coarse frame rate** via `m_c_variant_rate`. No core change, four
   speeds, immediately useful, and it tells us how the timbre change
   sounds before investing in the harder version.
3. **Continuous frame rate** via the accumulator, gated on the identity
   check above -- only if step 2 shows the approach sounds acceptable.

Step 2 is deliberately placed before step 3 because it answers cheaply
whether frame-rate manipulation sounds good enough to build on. If 2x
already sounds bad, the accumulator is not worth writing and rate
control should come from the clock multiplier instead, accepting the
pitch shift.

## Open question for the API

Whether "rate" is exposed as one number that internally picks a strategy
(frame rate first, clock multiplier beyond its range), or as two
separate controls the user can set independently. NVDA's model is a
single rate slider plus a separate pitch slider, which argues for: rate
-> frame rate, pitch -> Textalker `nP`, and the clock multiplier exposed
as a distinct "voice variant" style setting for people who want the
sped-up-tape character.
