# Pacing bug SOLVED: a missing #define (session 10)

The "sluggish compressed speech" the user had been hearing since early
in the project was a single line lost during the TMS5220 port.

## The bug

MAME's `tms5220.cpp` line 355:

```cpp
#define FAST_START_HACK 1
```

and, in `data_write`, where the FIFO crossing the buffer-low threshold
starts speech:

```cpp
m_SPEN = true;
#ifdef FAST_START_HACK
m_TALK = true;
#endif
```

**Our port carried across both `#ifdef FAST_START_HACK` sites but not
the `#define`.** Both blocks therefore compiled to nothing.

Without it, `TALK` is not set when `SPEN` goes active; it waits for the
next RESETL4. And since `TALKD` is latched from `TALK` at a RESETL4,
`TALKD` cannot follow until the RESETL4 *after that*. Every restart
consequently cost two idle frames where MAME spends one -- 25 ms per
restart, every time speech resumed.

## Why it took so long to find

The bug hid behind a real measurement trap. Everything comparable
matched: byte streams, frame counts, restart counts, arrival phase of
each byte within the frame, the RESETL4 state records, and the idle
branch line for line. The only visible symptom was a duration
difference, and the duration evidence was contaminated:

- **MAME's `.wav` is not chip time.** Its own sample counter shows the
  chip generated **12,911 samples** for the compressed utterance while
  the recording holds **11,200** -- 13% of the audio never reaches the
  file, presumably lost in the mixer/resampler. Chasing the recorded
  1400 ms produced a target below the state machine's own theoretical
  floor, which is what made the arithmetic keep failing to close.
- **MAME logs nothing at RESETL4 while idle.** Every idle frame was
  invisible in its trace, and idle frames were exactly the quantity in
  dispute.

Adding a device-level sample counter and a log line to the `TALKD == 0`
branch fixed both blind spots at once, and the answer was immediate:

```
t=91572  RESETL4: SPEN=0 TALK=0 TALKD=0        <- halt
t=91588  data 60 arrives, SPEN goes active
t=91772  RESETL4 (idle): SPEN=1 TALK=1 TALKD=1 <- one frame later, TALKD already 1
```

`TALKD=1` at that RESETL4 is impossible unless `TALK` was already set
before it -- and the only place that can happen is the `FAST_START_HACK`
line.

## The result

Idle frames per gap, compressed "RUBBER BABY BUGGY BUMPERS.":

```
MAME:         1, 1, 1, 1, 1, 1, 1, 1, 4
ours, after:  6, 1, 1, 1, 1, 1, 1, 1, 4
ours, before: 6, 2, 2, 2, 2, 2, 2, 2, 7
```

Every restart now matches exactly, and so does the trailing run. The
leading 6 is our cold-start init against MAME arriving warm from the
previous utterance -- different context, not a defect, and it is leading
dead air that the trimmer removes anyway.

## Effect across the corpus

| input | before | after | saved |
|---|---|---|---|
| hi_only | 0.281 s | 0.281 s | 0% |
| onset_a | 0.156 s | 0.156 s | 0% |
| repeat_letters | 0.956 s | 0.956 s | 0% |
| onset_this | 1.342 s | 1.292 s | 3.7% |
| Hedge Trimmer Story | 25.496 s | 23.596 s | 7.5% |
| command_coverage_test | 33.568 s | 30.364 s | 9.5% |
| chunked_paragraph_test | 18.711 s | 16.895 s | 9.7% |
| demo_bas_extracted_text | 39.764 s | 35.758 s | 10.1% |
| alphabet_song | 15.171 s | 11.937 s | 21.3% |

Single-utterance inputs are unchanged, exactly as expected: with no
restart there is no idle frame to save. The saving scales with how often
speech restarts, which is why the alphabet song -- 41 short lines --
gains most.

**The real-hardware validation is untouched**: `hi_only` still renders
2249 samples with amplitude -12127/+26833, matching the original
hardware capture, under both v3.1.3 and v1.3.

## Note on the earlier "compressed only" question

This confirms the analysis in `notes/pacing_isolated_to_restart_idle.md`.
The defect was never compression-specific -- it cost one frame per
restart in both modes. Compression removes speech without removing the
restarts, so the same absolute error was a far larger share of a
compressed utterance, which is why it was audible there and not in
expanded speech.

## Lesson for the port

This is the fourth defect traced to the mechanical extraction from
MAME's C++ (after the two in session 5, the dangling-statement family in
`notes/tms5220_port_dangling_statements.md`, and the dropped `/READY`
callback). The failure mode here is new and worth watching for
separately: **a `#define` that did not come across, leaving live
`#ifdef` blocks silently compiling to nothing.**

The other conditional in the port, `TMS5220_PERFECT_INTERPOLATION_HACK`,
was audited at the same time: MAME has `#ifdef` sites for it but no
`#define` either, so undefined is correct on both sides and it needs no
change. `FAST_START_HACK` was the only one.
