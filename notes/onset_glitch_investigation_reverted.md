# Onset glitch investigation (session 9) -- reverted, documented, unresolved
#
# >>> RESOLVED IN SESSION 10. See notes/onset_glitch_fixed_by_real_loader.md.
# >>> The cause was not the lattice filter warming up, as concluded below.
# >>> It was that the harness skipped Textalker's real loader. Everything
# >>> under "What was tried" remains accurate as a record of dead ends.

## The symptom
A brief "weird noise" / click at the very start of speech, but only for
some words -- e.g. "This" in the command-coverage test, and the letter
"A" at the start of the alphabet song -- and only under Textalker
v3.1.3. Never observed under v1.3, which has its own ~2-second
calibration delay before it ever speaks. Not observed for "Hi",
"Testing", or "Hello" as first words.

## Root cause: confirmed, understood, genuine chip behavior
Traced to the TMS5220's lattice filter. Voiced phonemes use a full
10th-order filter (all of K1-K10 active); unvoiced phonemes use a
simplified 3-coefficient filter (K4-K10 zeroed). Confirmed directly by
instrumenting the core and reading `m_uv_zpar`/`m_current_pitch` at
the point speech starts:

- "Hi", "Hello", "Testing" all begin on an **unvoiced** phoneme
  (`P=0, uvzpar=1`) -- clean onset, no glitch.
- "This" and "A" both begin **voiced** (`P=62-72, uvzpar=0`) -- full
  10-coefficient filter, glitchy onset.

When a full 10th-order filter starts from zero internal history (`m_u[]`/
`m_x[]` all zero, as they are at a fresh reset) and is immediately
driven by a loud, pitched excitation, it briefly "rings up" toward full
scale over a dozen-ish samples before settling to the correct steady
-state amplitude. Energy stays flat and modest throughout (e.g. E=33
for "This") while the filter's own resonance transiently overshoots --
this is the click.

**Confirmed NOT a bug in this port**: `tms5220_lattice_filter()` in
`third_party/tms5220_core/tms5220_core.c` was compared line-by-line
against the real, unmodified MAME source in `third_party/tms5220/
tms5220.cpp` and is structurally identical. Real hardware would do the
exact same thing given the exact same "cold silicon, voiced phoneme,
zero delay" starting condition.

## Why it doesn't show up in practice
On real hardware, Textalker being loaded doesn't mean speech starts
immediately -- control returns to Applesoft BASIC, which has to parse
and execute a PRINT statement (or whatever calls into Textalker)
first. That's genuine, non-trivial 6502 time elapsing before the
first word, functioning as an incidental warm-up. v1.3's explicit
~2-second calibration delay is a more deliberate version of the same
thing. Our test harness calls straight from `$D003`/`$FCD6` init into
speech with zero delay, which is a harsher "cold start" than any real
usage would ever hit.

## What was tried and what happened

### Attempt 1: prepend a real spoken warm-up word, discard its audio
Worked for the glitch itself (reduced "This"'s onset peak from 26317
to 19093 -- confirmed to be an exact match for what "This" naturally
sounds like occurring mid-sentence after another word, not a residual
problem). **Did not help "A" at all** (single-letter words go through
the separate `$E690`/`$E98F` dictionary-lookup code path, which
apparently resets/ignores whatever the warm-up primed). **Introduced a
real regression**: the warm-up word is no longer truly absent from
Textalker's perspective -- it changes whether the real first word is
treated as sentence-initial, which measurably shifted "Hi"'s own
amplitude range away from the validated real-hardware match
(-12127/26833 became -16255/26833). Not viable as-is.

### Attempt 2: pure idle delay (tick_chip in a loop, no 6502 execution)
Made things **worse**, not better -- full-scale clipping. Root cause:
the naive loop truncates `CYCLES_PER_SAMPLE` (≈127.56) to 127 every
single iteration; over hundreds of thousands of iterations this
accumulates enough drift to land on an uncontrolled, effectively
random internal chip phase. Abandoned in favor of a real 6502-executed
delay (see below), which doesn't have this truncation problem since
elapsed cycles come from genuine instruction execution.

### Attempt 3: real 6502-executed spin-loop delay before any text
A do-nothing nested X/Y counter loop written into unused RAM ($0400)
and run via the real 6502 core, so elapsed cycles accumulate exactly
as real code would, before the real text is ever processed (trimming
the resulting silent samples from the output). This is the approach
that should have worked, and got very close:

- **The onset glitch genuinely improved a lot** for "This" -- swept
  delay duration, found a real threshold effect (not a fragile
  knife-edge): below a certain delay the glitch remains or is worse,
  above it results land in a wide, stable plateau. At Y=$FF (one pass
  of the loop), "This" onset peak dropped to 2838 -- matching "Hi"'s
  own clean-onset peak almost exactly.
- **But it broke something else, caught late**: total utterance
  durations came out dramatically short (e.g. the full command-
  coverage test completed in 4.6s instead of the correct 33.6s; the
  chunked paragraph in 0.6s instead of 19.2s) -- entire words were
  being cut off partway through, not just the onset. This was only
  caught by checking full output duration against known-good
  baselines; checking only the onset peak (as had been done up to
  that point) completely missed it, since the very start of the
  truncated audio still sounded correct.
- Ruled out register/flag corruption as the cause: saving and
  restoring A/X/Y **and** the status register around the delay call
  made no difference -- the truncation happens regardless. Confirmed
  the delay call itself is the cause (removing it entirely, with
  everything else identical, immediately restored the correct 2811-
  sample "Hi" baseline).
- **Root cause of the truncation was not found.** Leading hypothesis,
  not yet verified: the chip's internal `m_subcycle`/`m_PC`/`m_IP`
  phase counters keep advancing even while idle (confirmed in earlier
  investigation -- see the `else // TALKD==0` branch of
  `tms5220_process()`), and a long enough delay could shift these to
  a phase where some frame-boundary or stop-condition check
  (`IP==7 && !TALK && !SPEN` type logic, or similar) fires
  prematurely once real speech starts. This is a plausible mechanism
  but was not traced through to confirmation before time ran out on
  this investigation.

## Current state: REVERTED
`tools/render_text_real_chip.c` has been reverted to the no-delay,
no-warm-up state -- speaks immediately after `$FCD6` init, exactly as
it did before this investigation began. Verified against both
baselines:
- `hi_only.bin`: 2811 samples, amplitude -12127/26833 (exact match to
  the pre-investigation validated baseline and the real-hardware
  reference capture).
- `chunked_paragraph_test.bin`: 153283 samples (exact match).

The onset glitch is real, understood, and NOT fixed. It's a known
cosmetic issue, not a functional bug -- speech is correct, just with
an audible click at the very start of some (not all) utterances when
speech begins immediately after a cold reset with no prior activity.

## If picking this back up
1. **Before touching the delay mechanism again, instrument and
   confirm the truncation's actual proximate cause first.** Don't
   repeat the mistake of only checking onset peaks -- always verify
   full output duration/sample-count against the known-good baselines
   in `reference_text/` before considering any change here validated.
   A good starting point: trace `m_subcycle`/`m_PC`/`m_IP` values at
   the exact moment speech starts, with and without a preceding
   delay, and compare against whatever specific check is causing
   early termination (likely somewhere checking `TALK`/`SPEN`/`FIFO`
   state against IP/PC phase).
2. Given the phase-counter hypothesis, it may be worth trying delays
   that are exact multiples of a full IP cycle (8 subcycle periods)
   rather than an arbitrary duration, so the delay ends up leaving the
   phase counters at IP=0/PC=0/subcycle=0 -- i.e. deterministically
   back at the exact same phase a true cold reset would have, rather
   than an arbitrary one.
3. Whatever the eventual fix, it needs to pass ALL of: onset peak
   check (the original glitch), full-duration check against every
   file in `reference_text/`, and the "Hi"-vs-real-hardware amplitude
   match (-12127/26833) -- attempt 1 passed the first, attempt 3
   passed the first and second but broke the third differently than
   attempt 1 did (attempt 1 shifted amplitude via prosody effects,
   attempt 3 via outright truncation) so the third check has now
   failed two different ways for two different reasons. Treat it as a
   real constraint, not a nice-to-have.
