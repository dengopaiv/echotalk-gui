# Pacing issue: localised to speech restarts (session 10, in progress)

**Not yet fixed.** This records what the measurements now establish, so
the next session does not re-derive it.

## The test case
`Rubber Baby Buggy Bumper Test/` holds the user's material: `test.bin`
(`Ctrl-E C` for compressed speech, then "RUBBER BABY BUGGY BUMPERS.",
then CR), `mame.wav` (MAME reference, extracted from a session
recording) and `echotalk.wav` (ours).

Reproduces exactly: **13735 samples (1.717 s) for v3.1.3 and 13732 for
v1.3** -- the two Textalker versions agree to 3 samples, so this is not
version-specific. MAME is **11312 samples (1.414 s)**. We are 2423
samples / 303 ms long.

## It is not a stretch, it is inserted silence
Segmenting both renders by energy (40-sample windows, threshold 400):

| | MAME | EchoTalk |
|---|---|---|
| total speech | 1115 ms | 1085 ms |
| total silence between segments | 285 ms | 485 ms |

Speech content is essentially identical -- ours is 30 ms *shorter*. The
entire discrepancy is in the gaps, and it is not spread evenly: some
gaps match MAME exactly while others are ~25-30 ms longer, which is why
it sounds phoneme-dependent. 25 ms is exactly one TMS5220 frame at
8 kHz (200 samples).

## Mechanism: Textalker stops and restarts the chip per syllable
Chip-state tracing (`ECHOTALK_CHIP_TRACE=1` on the loader harness,
which reads the chip struct directly rather than instrumenting the
core) shows this at every gap:

```
678.2ms TALK=0 TALKD=1 SPEN=0 fifo=6 energy=15  (STOP frame in the data)
700.1ms TALK=0 TALKD=0 SPEN=0                   TALKD drops at next RESETL4
702.1ms                 BE=1  fifo=0            FIFO drains
702.9ms                       fifo=1            new data arrives
708.9ms         SPEN=1        fifo=9            buffer_low clears -> SPEN
725.1ms TALK=1                fifo=15           TALK set at next RESETL4
750.1ms TALK=1 TALKD=1                          TALKD latched at the one after
753.2ms                       energy=5          first real audio
```

So a restart costs about **72 ms, almost all of it frame-boundary
quantisation**: waiting for TALKD to fall, then TALK being set at a
RESETL4, then TALKD latching from TALK at the *following* RESETL4. Only
~6 ms is the 6502 refilling the FIFO.

Note the stop is genuine data (`energy_idx = 15`) with 6 bytes still in
the FIFO -- Textalker deliberately ends the segment. It is not an
underrun.

**Our render restarts 7 times for a 4-word phrase, roughly once per
syllable. 4 restarts x ~72 ms = ~290 ms, which is essentially the entire
303 ms discrepancy.** MAME's gap profile (mostly 15-20 ms, only two at
45 ms) implies it restarts about 3 times, not 7.

## What has been ruled out
- **CPU/chip clock ratio.** Sweeping the emulated 6502 from 1.02 MHz to
  3 MHz (`ECHOTALK_CPU_HZ`) only moves the total from 1.717 s to
  1.626 s and asymptotes there. The cost is frame quantisation, not the
  6502 failing to keep up.
- **`TMS5220_READ_COMMAND_DELAY_SAMPLES`.** v1.3 never issues the READ
  BYTE probe that hack exists for, and is documented as producing
  identical output with it disabled -- yet v1.3 shows the same pacing
  behaviour, to within 3 samples.
- **The restart sequence itself being wrong.** Checked line by line
  against `third_party/tms5220/tms5220.cpp`: `m_TALK = true` really is
  behind `#ifdef FAST_START_HACK` in both, so `SPEN=1, TALK=0` for one
  frame is correct MAME behaviour, as is the latch-then-set ordering in
  RESETL4 that costs the second frame.
- **Compression not working.** It is: the same phrase uncompressed is
  2.517 s versus 1.717 s compressed. It just under-delivers relative to
  MAME's 1.414 s.

## Where this points
The chip model matches MAME in the restart path, and the 6502 core is
executing real Textalker code. What differs must be **the byte stream
Textalker produces** -- specifically how many stop frames it emits. That
stream is a function of the status bytes Textalker reads back, so a
small difference in what status reports, at the moment Textalker polls,
plausibly cascades into ending a segment early and restarting.

## Suggested next step
Get the ground-truth byte stream from MAME. `a2echoii.cpp` already has
`LOG_WRITE` (every byte written to the latch) and `LOG_READYQ`, and
`tms5220.cpp` has command-level logging; they only need `VERBOSE`
defined at the top of the file. Running headless with logging redirected
to a file (no interactive debugger -- see the accessibility note in
HANDOFF.md) gives a directly comparable trace.

The decisive question that trace answers in one look: **does MAME's
Echo II receive fewer stop frames than ours, or the same stream with
faster restarts?** The first means the divergence is on the Textalker
side and driven by status reads; the second means it is in the chip
model after all. Everything above narrows it to exactly those two.

## Incidental fix made while investigating
Not the cause of the pacing issue, but a real defect found on the way --
see `notes/tms5220_port_dangling_statements.md`.
