# Pacing: measuring MAME's chip time directly (session 10)

## Why the previous measurement was not enough

The arrival-phase prediction was falsified. MAME's SPEAK EXTERNAL bytes
arrive at `IP=0 PC=7 subcycle=2` with `fifo=6` -- byte for byte the same
phase as ours. Combined with everything already matched, that leaves
nothing in the chip model differing:

| | ours | MAME |
|---|---|---|
| byte streams | | identical |
| SPEAK EXTERNAL arrival phase | IP=0 PC=7 sub=2 | identical |
| frames, exact bounds | 82 / 50 | 82 / 50 |
| restarts | 8 | 8 |
| RESETL4 records | 49 | identical |
| TALKD==0 idle branch | | identical line for line |

And yet MAME's audio is 1400 ms against our 1570 ms.

## The contradiction that forced this measurement

50 frames at 25 ms is 1250 ms. The state machine requires two idle
frames per restart -- `TALK` is only set at a RESETL4 where `SPEN` is
already true, and `TALKD` follows `TALK` one frame later -- so eight
restarts add at least 400 ms. That is a **1650 ms floor**, and MAME's
recording measures 1400 ms, *below its own theoretical minimum*.

Two further signs that the recording is not chip time:

- MAME's chip emits exactly `-1` on every idle sample. Sixteen idle
  frames would be ~3200 such samples. The compressed utterance contains
  **244**, in scattered runs shorter than 20 samples.
- The shortfall is **exactly 317 ms in both modes** -- expanded
  2517 -> 2200, compressed 1717 -> 1400 -- despite expanded having
  nearly twice the speech.

Everything measured so far has come through MAME's sound mixer and
resampler on its way to a `.wav`. That is ground truth for what a
listener hears, which is what matters in the end, but it cannot
distinguish "the chip ran for N samples" from "N samples reached the
file".

## What was instrumented

`y:/src/devices/sound/tms5220.cpp`, four changes, all logging plus one
counter:

1. **`static uint64_t s_echotalk_samples`** at file scope.
2. **Incremented next to `buf_count++`**, the single point where the
   chip emits a sample -- so it counts chip time exactly, in both the
   speaking and idle branches, before the audio pipeline touches
   anything.
3. **`t=` added to the existing RESETL4 "status updated" log.**
4. **A new RESETL4 log in the `TALKD == 0` branch.** MAME logs nothing
   there, so every idle frame has been invisible in the trace to date --
   and idle frames are precisely what is in dispute.

Item 4 is the important one. It makes MAME's idle frames countable for
the first time.

## What the answer will look like

Take the compressed utterance, delimited as before by its eight
`data 60` writes, and difference the `t=` values.

- **Frame length.** Consecutive `RESETL4, status updated` lines should
  be 200 samples apart. If they are not, frames are not uniformly 25 ms
  and the entire arithmetic changes.
- **Idle frames per restart.** Count `RESETL4 (idle)` lines between one
  `halting speech` and the next resumption. The state machine predicts
  two. If MAME shows one, the two implementations genuinely differ in
  the restart handshake despite identical code, which would point at how
  the chip is clocked rather than what it computes.
- **Total chip time.** Difference `t=` across the whole utterance. If
  MAME's chip generated ~13,200 samples while its `.wav` holds 11,200,
  the discrepancy is in MAME's audio path and our emulation is already
  correct -- at which point the open question stops being "what is our
  bug" and becomes "which of the two should a screen reader sound
  like". If it generated ~11,200, the chip really does run shorter and
  the cause is in clocking.

All three fall out of one run of the same disk image.
