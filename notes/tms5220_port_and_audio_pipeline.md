# TMS5220 standalone port + audio pipeline (session 5)

## The port
`third_party/tms5220_core/` contains a standalone C port of MAME's
`tms5220.cpp`/`.h` (BSD-3-Clause), extracted mechanically (a Python
script pulled every `tms5220_device::method()` out of the C++ class
into a plain C function taking `tms5220_state *tms` instead of
`this`), then hand-fixed. Notable things the automated pass couldn't
do safely, fixed by hand:

- **Two real logic bugs introduced by stripping log-only statements.**
  Removing `LOGMASKED(...)` calls that were the *entire body* of an
  `if` left dangling-if constructs that silently changed control flow
  (`matrix_multiply` could fall off the end without returning;
  a frame-error path became unreachable). Caught via compiler warnings
  (`-Wdangling-else`, "control reaches end of non-void function") and
  fixed by hand -- this is exactly the kind of thing that's easy to
  miss with a purely mechanical port, so it's worth flagging.
- Dropped MAME's `device_t`/save-state/timer/devcb-callback machinery
  entirely, and the "true timing" `rsq_w`/`wsq_w` cycle-accurate
  handshake path -- EchoTalk drives the chip the same synchronous way
  MAME's own "hacky instant mode" (`data_w`/`status_r` without ever
  calling `rsq_w`/`wsq_w`) already does, which needed none of that.
- Dropped external VSM/TMS6100 speech-ROM support (stubbed as no-ops)
  since Textalker only uses SPEAK EXTERNAL (confirmed in the v1.3
  debugging session), never the internal-ROM SPEAK/LOAD ADDRESS/READ
  BYTE commands.
- Pulled in `tms5110r.hxx` (the coefficient/chirp tables), also
  BSD-3-Clause, unmodified.

**Compiles clean**, and **produces real audio**: fed the exact 58-byte
"HI" command stream captured from the working v3.1.3 harness, it
generates 3,201 samples (0.4s at 8kHz) with full dynamic range and
real waveform variation -- not silence, not garbage. `hi_test.wav` /
`hi_normal.wav` are that output; give them a listen.

## Clock rate (speed + pitch together)
The chip's internal state machine has no concept of wall-clock time --
`tms5220_process()` just advances one internal "sample" per call,
regardless of what real-world clock the chip is nominally running at.
All the actual clock-dependent behavior (pitch period length, frame
duration) is expressed in **sample counts**, which are fixed by the
LPC data itself. This means the classic over/underclocked "chipmunk"
effect doesn't require touching the synthesis core at all -- it falls
out of simply **declaring** a different sample rate for the same
generated samples:

- Echo II's real clock is 640kHz, giving 640000/80 = 8000 Hz native.
- Generate audio normally (samples are always computed the same way).
- To play it back faster/higher-pitched, just declare (and play back
  at) a higher rate -- e.g. 16000 Hz for a 2x "clock" -- without
  regenerating anything.

`tools/render_wav.c` exposes this directly: `clock_multiplier` (0.5 to
2.0, per your requested range) scales the *declared* rate
(`8000 * multiplier`), nothing else. Verified: 2x produces exactly
half the duration (0.40s -> 0.20s), 0.5x exactly double (0.40s ->
0.80s) -- confirms both speed and pitch move together as expected,
using only the sample-count/declared-rate relationship.

## Resampling (no anti-aliasing, by design)
`resample_naive()` in `render_wav.c` does straightforward linear
interpolation between the native (clock-adjusted) rate and any target
output rate (e.g. 44100/48000 Hz for normal audio hardware), with
**no low-pass filtering** before up/downsampling -- content above the
new Nyquist frequency aliases rather than getting filtered out, per
your request. This keeps the raw, slightly gritty character of the
original chip's output rather than smoothing it into something that
sounds like a nicer DAC than the real Echo II had.

Resampling happens *after* the clock-multiplier is applied, so the two
compose correctly: e.g. `clock_multiplier=1.5, target_rate=48000`
first computes samples at the 1.5x-adjusted 12000 Hz "native" rate
(the sped-up, higher-pitched content), then resamples that to 48000 Hz
for normal playback -- durations match exactly before and after
resampling (confirmed: 0.27s at both 12000 Hz native and after
resampling to 48000 Hz), i.e. resampling is purely a format-
compatibility step and doesn't cancel the clock effect.

## Known remaining imperfection
The captured "HI" byte stream's trailing `$FF` run (the FIFO-
backpressure artifact noted in earlier sessions -- Textalker kept
sending because our old echo-card stub never signaled "buffer full")
was dropped from the demo data in `render_wav.c` rather than fed to
the real chip, since `$FF` isn't meaningful LPC data and would just
produce noise at the end. The real fix is wiring this core *into* the
Echo-card I/O emulation itself (replacing the old stub), so Textalker
gets correct real-time backpressure from the actual chip instead of a
canned status byte -- that's the natural next step now that the core
works.

## Next steps
1. Replace the old dumb Echo-card status stub in the boot harnesses
   with real calls into this TMS5220 core (`tms5220_data_w` on writes,
   `tms5220_status_r` on reads) -- this should also make the full,
   untruncated byte streams (including v1.3's) terminate cleanly
   instead of hitting step limits, since the chip will report real
   FIFO status instead of "always need more data."
2. Build the actual library API and CLI around this: `echotalk_init()`
   / `echotalk_speak(text)` / `echotalk_render(sample_rate,
   clock_multiplier)`.
