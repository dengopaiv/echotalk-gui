# Pacing: arrival-phase measurement and a falsifiable prediction

The approach here is the reverse of everything before it. Rather than
hunting for a difference inside our chip, measure *when* each byte
reaches the chip, in the chip's own frame phase, on both sides.

## What was instrumented

**MAME** (`y:/src/devices/sound/tms5220.cpp`, two `LOGMASKED(LOG_DATA_W,
...)` lines, and `LOG_DATA_W` is already in that tree's `VERBOSE` mask):

- in `data_w`, after `m_stream->update()` so the phase is current rather
  than wherever the last catch-up left it -- when the CPU issues a write
- in `data_write` -- when the byte actually reaches the chip, which
  under true timing is a `/READY` handshake later

Both print `IP`, `PC`, `subcycle` plus `TALKD`, `TALK`, `SPEN`, `DDIS`
and the FIFO count.

**Ours**: `ECHOTALK_ARRIVAL=1` on the loader harness prints the same
fields in the same shape, so the two runs diff directly.

## Our measurement

Every restart's SPEAK EXTERNAL byte arrives at the same phase:

```
data 60 at IP=6 PC=12 subcycle=1  (fifo=0)   <- first, during init
data 60 at IP=0 PC=7  subcycle=2  (fifo=6)
data 60 at IP=0 PC=7  subcycle=2  (fifo=6)
data 60 at IP=0 PC=7  subcycle=2  (fifo=6)
data 60 at IP=0 PC=7  subcycle=2  (fifo=6)
data 60 at IP=0 PC=7  subcycle=2  (fifo=6)
data 60 at IP=0 PC=7  subcycle=2  (fifo=6)
data 60 at IP=0 PC=7  subcycle=2  (fifo=5)
```

Seven of eight land on `IP=0 PC=7 subcycle=2`, with the same six
residual FIFO bytes each time. This is not scatter; it is a fixed
relationship between Textalker's code and the frame clock.

**`IP=0` means the byte arrives just *after* a frame boundary.** RESETL4
is the IP 7 -> 0 wrap, so SPEAK EXTERNAL is consistently missing that
boundary by a fraction of a frame. That is exactly the condition that
costs the extra idle frame: `SPEN` cannot be set until the bytes are in,
so the RESETL4 that clears `TALKD` sees `SPEN` still low, `TALK` is not
set until the *next* RESETL4, and `TALKD` follows the one after that --
two idle frames instead of one.

For reference, the distribution over all our writes:

```
IP=0: 42   IP=1: 174   IP=2: 68   IP=3: 32   IP=6: 1   IP=7: 7
```

## The prediction

If the difference is arrival phase, **MAME's SPEAK EXTERNAL bytes should
arrive at IP=7**, late in the frame rather than at the start of the next
one, letting `SPEN` be set in time for that same RESETL4 and saving one
frame per restart.

This is falsifiable, and either answer is informative:

- **MAME shows IP=7 (or anything before the wrap).** Confirmed. The
  chip models agree; what differs is when the 6502 gets there, which
  makes it a CPU/chip interleaving question -- how MAME schedules the
  6502 against the sound stream versus our tick-after-every-instruction
  model. Note that our own phase sweep found no effect, so the answer
  would have to be a systematic offset rather than a tunable one.
- **MAME also shows IP=0.** Then arrival phase is not the difference,
  the byte genuinely arrives at the same point in the frame in both,
  and the extra frame is being spent inside the chip after all -- which
  narrows it to the RESETL4 handling itself, despite the state records
  matching.

## How to run it

Build as usual, run the same disk image, then:

```
grep "data 60 at" error.log
```

Compare the `IP=` values against the eight above. The compressed pass is
the second group of eight; the first eight belong to the expanded pass.
