# Continuous speech rate: the accumulator

Session 11. `library_plan_rate_and_pitch.md` sketched this and left it
undone; the NVDA driver needs it, because NVDA's rate slider wants a
smooth range either side of normal and the chip's own SET RATE offers
four fixed steps, all at or above normal speed, the top one rough.

## The idea

Speech rate and pitch come from two different places in the chip:

- **Pitch** is `m_pitch_count`, which advances once per output sample
  and drives the chirp excitation.
- **Speech rate** is how fast the parameter state machine
  (`m_subcycle` / `m_PC` / `m_IP`) walks a frame -- 25 output samples
  per interpolation period, eight periods per frame.

In MAME's loop these are locked together: one machine cycle per output
sample. Unlock them and speech rate becomes independent of pitch. Run
the machine twice per sample and speech goes twice as fast at the same
pitch; run it every other sample and it halves.

That is the whole mechanism. No resampling, no pitch correction, no
phase vocoder -- the chip's own interpolation does the work, because
stretching or compressing the parameter track is exactly what it is
built to do between frames.

## The implementation

`tms5220_process()`'s loop body was split where the chip's own timing
splits, into two static functions:

- `tms5220_step_parameters()` -- the frame-parse / interpolate block
- `tms5220_advance_counters()` -- the subcycle/PC/IP advance and the
  RESETF3 / RESETL4 latches

Everything between them (excitation, the 20x LFSR, the lattice filter,
the pitch counter) is the audio path and still runs exactly once per
output sample. The loop then reads:

```c
steps = 1;
if (tms->m_speech_rate != 1.0) {
    tms->m_rate_acc += tms->m_speech_rate;
    steps = (int)tms->m_rate_acc;
    tms->m_rate_acc -= steps;
}
for (i = 1; i < steps; i++) { step_parameters(); advance_counters(); }
if (steps > 0) step_parameters();
    ... generate the sample ...
if (steps > 0) advance_counters();
```

Note the shape: `steps - 1` complete machine cycles, then the last
`step_parameters()` before the sample and its `advance_counters()`
after. **At `steps == 1` that is step, generate, advance -- MAME's
original order exactly.** Byte-exactness at the default is structural,
not something to be checked and hoped for. `steps == 0` holds the frame
while the audio path keeps running, which is what slower-than-normal
means.

`m_speech_rate` survives reset alongside `m_configured_rate`, because
Textalker issues a RESET between every segment and the setting would
otherwise be wiped after the first utterance -- the same trap that bit
the frame-rate work.

The code inside the two functions is unchanged. **MAME has no
equivalent, so do not expect to find them when diffing against
`third_party/tms5220/`.**

## Verified

**Byte-exact at 1.0.** All nine reference files under both Textalker
versions reproduce exactly, `hi_only` still at -12127/+26833, and the
library's own counts are unchanged. Checked before and after adding the
API on top.

**Pitch really is untouched.** Autocorrelation F0 on a sustained vowel:

| setting | F0 |
|---|---|
| speed 0.5 | 129.0 Hz |
| speed 1.0 | 129.0 Hz |
| speed 2.0 | 129.0 Hz |
| speed 3.0 | 129.0 Hz |
| clock multiplier 2.0 | 258.1 Hz |

Identical across a 6x speed range, while the clock multiplier doubles it
as it should. That is the claim the whole design rests on, and it is
measured rather than assumed.

## The delivered ratio falls short at high speeds

Monotonic and smooth, but not linear. "The quick brown fox...", 32,335
samples at 1.0:

| asked | delivered |
|---|---|
| 0.50x | 0.52x |
| 0.75x | 0.77x |
| 1.25x | 1.22x |
| 1.50x | 1.44x |
| 2.00x | 1.84x |
| 3.00x | 2.56x |

Close below ~1.5x, drifting to about 85% of the request at 3x. The
shape matches the hypothesis recorded in `library_plan_rate_and_pitch.md`
for why the four fixed frame-rate steps also underdeliver: the chip
drains the FIFO faster than Textalker refills it, starves, and stalls,
so Textalker's flow control sets the pace rather than the frame clock.
That earlier note warned the accumulator "would run into exactly the
same wall", and it has -- but only partly, because the wall bends rather
than stops: 3.0 still delivers 2.56x, where the frame-rate steps topped
out well below their arithmetic.

Left uncalibrated for now. For a rate slider, monotonic and smooth
matters more than the number meaning exactly what it says, and a
correction curve would be fitting to one Textalker version on one kind
of text. Worth revisiting if a UI ever wants to display a real
multiplier.

## API

- `tms5220_set_speech_rate()` in the core, 0.1 to 8.0.
- `echotalk_set_speed()` / `echotalk_speed()`, 0.25 to 4.0, default 1.0.
- `\x04 1.5S` as a Ctrl-D command; bare `\x04S` restores 1.0.
- `say --speed MULT`.

The frame-rate control stays. It is real 5220C behaviour and worth
keeping for that reason, but the header now points callers at speed as
the one to use.

ABI bumped to **4**; 41 exports.

## Also fixed here

`listen_check.py`'s monotone check demanded that flat and normal
renderings be *exactly* the same length. They came out one sample apart
and it failed. The library was right; an utterance can land a sample
either side depending on where the interpolation boundary falls. Now
checked within a tolerance. Worth noting because it is the third time
this session a check has been too strict rather than the code being
wrong -- the failure mode of comparing whole buffers when the claim is
about content.
