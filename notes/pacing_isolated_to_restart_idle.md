# Pacing isolated: extra idle frame per restart (session 10)

Supersedes the open questions in
`notes/pacing_mame_bytestream_comparison.md`. Uses the user's VERBOSE
MAME capture, which deliberately speaks the phrase twice -- expanded
first, then compressed -- so the two modes can be compared.

## What matches exactly

**Byte streams, both modes.** Dumping our Echo II writes
(`ECHOTALK_BYTE_DUMP`) and diffing against MAME's `Data written to latch`
lines:

- compressed: our 324 bytes are a byte-exact prefix of MAME's; MAME's
  extra 85 bytes are a later print, separated by a 246-line gap in the
  log where real segment boundaries are 9-10 lines.
- expanded: identical for all 496 bytes; our extra 18 trailing `ff`
  bytes were simply outside the line range first extracted.

**Frame counts, both modes.** Counting frames the same way on both
sides -- MAME's `Processing new frame` lines against an IP-wrap counter
in the harness:

| | ours | MAME |
|---|---|---|
| expanded | 82 | 82 |
| compressed | 50 | 50 |

So Textalker's output is exact and the chip parses and plays exactly the
same frames. Neither the 6502 emulation nor the synthesis is at fault.

## What differs

Everything left is idle time -- the chip not speaking, between segments.

| | ours | MAME |
|---|---|---|
| compressed total | 1717 ms | 1447 ms |
| speaking (50 frames x 25 ms) | 1250 ms | 1225 ms |
| idle | **467 ms** | **~175 ms** |

MAME's frame interval is exactly 25.00 ms, measured directly; the only
other interval it ever shows is exactly 50.00 ms, which is a restart.
Since a logged interval spans one speaking frame plus any idle frames,
MAME's 50 ms restarts amount to **one** idle frame each. Ours, measured
from `ECHOTALK_CHIP_TRACE`, hold TALKD clear for 50 ms -- **two** idle
frames.

Roughly one extra idle frame per segment, eight segments, ~200-290 ms.
That is the whole discrepancy.

## On the "compressed only" question

The user's hypothesis was that the bug only affects compressed speech.
The measurements say: **the defect is present in both modes, and their
instinct about where it is audible is right.**

- expanded: ours 2517 ms trimmed vs MAME 2284 ms -- 10% long
- compressed: ours 1717 ms vs MAME 1447 ms -- 19% long

The absolute error is similar (both are ~8 restarts each carrying one
extra 25 ms frame). Compression removes speech but not the inter-segment
gaps, so the same absolute error is a much larger share of a shorter
utterance, and the pauses stand out. Expanded speech masks it.

This is worth stating plainly because it changes what "fixed" means: the
fix should remove the extra idle frame in both modes, and expanded
output should get ~10% shorter too.

## The mechanism, as far as it is pinned down

At a restart both implementations do the same thing:

```
RESETL4:  TALKD = TALK          (latch)
          update_fifo_status_and_ints()
          if (!TALK && SPEN) TALK = true
```

so speech resumes one frame after TALK is set, and TALK is only set at a
RESETL4 where SPEN is already true. Whether a restart costs one idle
frame or two therefore depends entirely on **whether SPEN is set before
or after the RESETL4 at which TALKD drops.**

Our trace shows SPEN being set about 8.8 ms after TALKD drops -- just
too late, so we wait for the following RESETL4 and pay a second idle
frame. MAME's log shows a very similar 8.6 ms delay, which is the part
that does not yet add up: the same relative timing should cost it the
same extra frame, yet its totals say otherwise.

## Frame-by-frame RESETL4 comparison: states match, counts do not

Done. `ECHOTALK_RESETL4=1` emits RESETL4 records in MAME's exact format,
detected from outside the core (an IP 7->0 wrap from PC=12), with the
pre-call state as "about to update" and the post-call state as "status
updated". Filtering ours to MAME's logging condition (TALKD set on
entry) and diffing:

**All 49 records are identical.** Ours has one extra at the very end,
which is the final shutdown, outside the line range extracted from
MAME's log. So the state machine takes the same transitions in the same
order, with the same SPEN/TALK/TALKD values throughout.

What differs is only how many *idle* frames sit between those records:

| | ours | MAME |
|---|---|---|
| gaps of 1 idle frame | 0 | **7** |
| gaps of 2 idle frames | **7** | 0 |

(Ours also shows one 4-frame and one 7-frame gap, which are startup and
shutdown, not restarts.)

That is the entire bug, stated exactly: **every restart costs us two
idle frames where it costs MAME one.** Seven restarts, 25 ms each, and
the arithmetic closes.

## What has been ruled out since

- **Frame-clock phase.** `ECHOTALK_PHASE=N` advances the chip's internal
  counters by N samples before anything else runs, shifting the frame
  clock relative to the 6502 without changing either rate. Swept across
  a full frame (0, 25, 50 ... 199): **13735 samples at every offset**,
  not one sample of difference. The extra frame is structural, not a
  quantisation accident -- unsurprising in hindsight, since Textalker
  waits on chip state and the whole system re-synchronises.
- **FIFO handling on SPEAK EXTERNAL.** Both implementations clear the
  FIFO and its counters on the command, verified line by line. The six
  residual bytes left over from the previous segment are discarded in
  both, so neither gets a head start toward the buffer-low threshold.
- **talk_status.** Identical: `m_SPEN || m_TALKD` in both.

## Where it must be

`TALK` is only set at a RESETL4 where `SPEN` is already true, and `TALKD`
follows `TALK` one frame later. So a restart costs one idle frame if
`SPEN` is set *before* the RESETL4 that clears `TALKD`, and two if it is
set after. MAME manages the former; we manage the latter. Since the byte
stream is identical, the difference is when those bytes reach the FIFO
relative to the frame clock -- and phase has been eliminated, so
something makes our 6502 reach the write later in a way that is stable
rather than accidental.

Textalker's status polling was located: a helper at `$FCC9` reading the
(self-modified) status port twice with six `ROR A` instructions of delay
between, called 21,558 times in this render. Its callers are the loop
that waits between segments. Reading that routine's callers to find
which status bit gates the next segment's first write is the next step,
since that is what determines which side of the boundary `SPEN` lands
on.

One candidate not yet tested: MAME runs the chip in true-timing mode,
where `status_r()` returns `m_read_latch` -- a value latched when /RS
last fell and refreshed continuously while it is held low -- whereas our
port returns the instantaneous status. If Textalker's poll sees a value
one read-cycle stale in MAME, the loop could exit on a different
iteration and reach the write earlier relative to the frame clock.

---

# CORRECTION: the "one idle frame vs two" result does not hold up

Investigating the above turned up a measurement error in the section
above it, which is retained only so the reasoning can be followed.

## The log's sample accounting is not a reliable clock

Durations above were obtained by summing MAME's `process called with
size of N` stream updates between two line numbers. That total is
extremely sensitive to where the region is cut: moving the *expanded*
utterance's start from line 430 to line 380 changes the sum from 18,275
samples to 24,111. The accumulator sweeps up large idle stretches either
side of the utterance, so it cannot be used to time restarts, and the
"7 gaps x 1 idle frame" figure derived from it is not trustworthy.

## MAME's own event ordering says it takes two idle frames as well

Reading the log in order around a restart, rather than timing it:

```
halting speech
RESETL4, status updated: m_SPEN=0, m_TALK=0, m_TALKD=0   <- TALKD drops
Data written to latch of 60                              <- SPEAK EXTERNAL, after
Speak External command received (60)
data_write triggered SPEN to go active!                  <- SPEN, after
RESETL4, status updated: m_SPEN=1, m_TALK=1, m_TALKD=1   <- speaking again
```

identical in structure to ours. SPEN is set *after* the RESETL4 that
clears TALKD, so by the state machine -- TALK only set at a RESETL4
where SPEN is already true, TALKD following TALK one frame later --
MAME must also spend two idle frames per restart. It cannot be doing it
in one while writing SPEAK EXTERNAL that late.

## What is solid

Within strict utterance boundaries, everything measurable matches:

| | ours | MAME |
|---|---|---|
| expanded frames | 82 | 82 |
| compressed frames | 50 | 50 |
| RESETL4 records | 49 | 49, values identical |
| byte stream | 324 / 496 | identical |
| restart ordering | TALKD down, then $60, then SPEN | same |

And our own output is exactly what the state machine predicts:

```
50 speaking frames x 25 ms          = 1250 ms
8 restarts x 2 idle frames x 25 ms  =  400 ms
                                      -------
                                       1650 ms   (we render 1717 ms
                                                  including lead-in/tail)
```

## So the open question has moved

Every direct comparison says the two implementations do the same thing.
The only evidence that MAME is faster is `mame.wav` at 1414 ms, which
the user extracted by hand from a longer session recording. For MAME to
produce that from 50 frames and 8 restarts it would need well under one
idle frame per restart, which its own log contradicts.

The likeliest explanation is now that **the reference recording is not a
faithful full-length capture** -- inter-word silence trimmed during
extraction would produce exactly this. That is a much more mundane
explanation than a chip-model defect, and it is consistent with every
measurement that does not depend on that file.

## How to settle it

Have MAME write the audio itself rather than extracting it from a
session capture:

```
mame.exe apple2e -wavwrite rubber.wav <the rest of the usual options>
```

`-wavwrite` records MAME's own audio output stream unedited. Comparing
that against `out/final.wav` (ours, 1717 ms) answers it in one step:

- If MAME's own capture is also ~1650-1700 ms, there is no pacing bug
  left to fix -- our output already matches, and the original
  "sluggishness" was an artifact of the hand-extracted reference.
- If it really is ~1414 ms, then MAME is genuinely resuming speech
  faster than its own log ordering can account for, and the next place
  to look is the true-timing status latch described above, since that
  is the last structural difference between the two chip models.

## Tooling added
- `ECHOTALK_BYTE_DUMP=<file>` -- every byte written to the Echo II latch,
  in MAME's hex format.
- `ECHOTALK_CHIP_TRACE=1` -- TALK/TALKD/SPEN/DDIS/BE/FIFO/energy on every
  change, timestamped in ms.
- frame counter reported at end of run, comparable with MAME's
  `Processing new frame` count.
