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

## Tooling added
- `ECHOTALK_BYTE_DUMP=<file>` -- every byte written to the Echo II latch,
  in MAME's hex format.
- `ECHOTALK_CHIP_TRACE=1` -- TALK/TALKD/SPEN/DDIS/BE/FIFO/energy on every
  change, timestamped in ms.
- frame counter reported at end of run, comparable with MAME's
  `Processing new frame` count.
