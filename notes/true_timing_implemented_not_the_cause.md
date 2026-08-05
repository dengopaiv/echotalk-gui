# True timing implemented; it is not the pacing bug (session 10)

The last structural difference between our chip model and MAME's was
that MAME drives the TMS5220 through the real `/RS`, `/WS` and `/READY`
handshake while our port used MAME's own "hacky instant write mode".
That was the gap the original HANDOFF identified, and it was the last
remaining suspect for the extra idle frame per restart. It has now been
implemented properly and **it is not the cause.**

## Why it could never have been

Worth stating first, because it should have been checked before writing
any code: MAME's `/READY` delays are **13 us** after `/RS` falls and
**16 chip clocks (25 us at 640 kHz)** after `/WS` falls. The pacing
discrepancy is **one 25 ms frame per restart**. Those are three orders
of magnitude apart. No arrangement of a 25 us handshake produces a 25 ms
difference.

Confirmed empirically: with true timing on, the compressed phrase
renders to **13735 samples, byte-identical to instant mode**.

## What was implemented anyway, and kept

The work was still worth doing, because it turned up three real defects
in the dormant code and one in the core:

1. **`update_ready_state` had lost its callback.** MAME calls
   `m_readyq_handler(!state)` on every change of the ready pin, and it
   reads `ready_read()`, not `m_io_ready` directly. Ours did neither --
   another casualty of the log-stripping, since the handler call sat
   next to a log statement. The Echo II card needs that edge: it is
   what releases the write latch (`tms_readyq_callback` in
   `a2echoii.cpp`). A `m_readyq_handler`/`m_readyq_ctx` pair now exists
   on the state struct, NULL-safe so the instant path is unaffected.

2. **`wsq_w` was not MAME's.** It peeked at the command byte and used
   520 us for READ BYTE / READ AND BRANCH against 25 us otherwise --
   the sample-based `TMS5220_READ_COMMAND_DELAY_SAMPLES` hack wearing a
   different hat. MAME uses a flat 16 clocks for every write. Now flat.

3. **The write path raised `/READY` unconditionally.** MAME's
   `set_io_ready` case 0x02 deliberately does *not* set `m_io_ready`
   when the command register is still busy; it leaves `/READY` inactive
   and keeps the data latch, which is what throttles the writer.
   `data_write` raises it by itself once the latch is consumed.

The harness now implements `a2echoii.cpp`'s protocol properly when the
path is enabled: alternating read latch driving `/RS`, write latch
pulling `/WS` low with clobber detection, and the `/READY` callback
releasing the latch.

## Why it is not the default

`ECHOTALK_TRUE_TIMING=1` to enable; instant mode remains the default.

With true timing on, **17 writes are clobbered** -- a byte latched and
then overwritten before the chip took it, i.e. data that never reached
the chip. MAME's log for the same phrase shows **zero** clobbers across
935 writes. Since the output is byte-identical either way, true timing
currently buys nothing and loses bytes, so defaulting to it would be a
pure regression. The clobbers are all `FF` overwriting `FF`, the RESET
padding Textalker sends between segments, which is why the audio does
not change -- but bytes going missing is a fidelity gap regardless.

That gap is a genuine open question and probably worth chasing on its
own: the same 6502 code, writing at the same rate, does not clobber in
MAME. Either MAME releases its write latch sooner than our 25 us, or
Textalker's write loop is being clocked differently than we think.

## Where this leaves the pacing bug

Still open, and now with no remaining suspect from the original
analysis. What is known, all measured rather than inferred:

- byte streams identical, both modes
- frame counts identical, both modes (82 expanded, 50 compressed)
- RESETL4 state records identical (49 of them)
- speech content matches within 15 ms
- every restart costs us one extra 25 ms frame; seven of them, ~190 ms
- frame-clock phase has no effect at all
- FIFO clearing on SPEAK EXTERNAL matches
- `talk_status` matches
- true timing changes nothing

A restart costs one idle frame if `SPEN` is set before the RESETL4 that
clears `TALKD`, two if after -- and ours lands after. Since the byte
stream is identical, the remaining question is narrow and specific:
**what makes the write of SPEAK EXTERNAL land one frame later, in chip
time, than it does in MAME?**

The next thing to try is the reverse of what has been tried so far.
Rather than looking for a difference in our chip, instrument MAME
directly: add a log line to `data_write` printing the chip's own
`m_IP`/`m_PC`/`m_subcycle` at the moment each byte arrives. That gives
the arrival phase within the frame for every byte on MAME's side, which
can be compared against the same figures from our harness. If MAME's
SPEAK EXTERNAL consistently arrives at an earlier IP than ours, the
question becomes why the 6502 gets there sooner; if it arrives at the
same IP and MAME still resumes a frame earlier, the fault is in the
chip after all and this measurement will say exactly where.
