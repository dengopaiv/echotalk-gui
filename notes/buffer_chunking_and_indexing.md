# Buffering, chunking, look-ahead synthesis, and indexing (session 6)

## Textalker's buffer size (confirmed via disassembly)

Textalker has no separate, fixed-size text buffer. It buffers
characters directly using **the current screen line width** as the
limit, traced to `$D781`:

```
D781: BIT $C01F        ; check 80-column firmware present flag
D784: BMI $D78C        ; if 80-column detected, skip clamping
D786: CMP #$28         ; compare width value against $28 (40 decimal)
D788: BCC $D78C        ; if already < 40, skip clamping
D78A: LDA #$28         ; otherwise clamp to 40
D78C: STA $FD80        ; width+1
D78F: STA $FD81        ; width-1   (three related bounds, checked at
D792: STA $FD82        ; width      slightly different points in the
D795: DEC $FD81        ;            per-character dispatch loop)
D798: INC $FD80
```

So: 40 characters on a plain 40-column Apple II, or whatever width
value is configured (practically up to 80) once 80-column mode is
signaled. The dispatch loop checks `Y >= width-1` and auto-flushes
(speaks) whatever's buffered right at that boundary -- a soft
word-wrap, not a hard overflow/crash risk.

**Decision:** configure the emulated environment for 80-column mode
(largest native buffer Textalker supports), and handle chunking
ourselves at the library layer rather than relying on Textalker's own
boundary-flush behavior -- see below.

## Chunking algorithm (implemented, tested)

`src/chunker.c` / `chunker.h`. Pure logic, no emulator dependency,
independently unit-tested via `tools/test_chunker.c`.

Split preference, in order, per chunk window:
1. **Clause boundary** (`, . ? ! ; :`) nearest the end of the window --
   preserves natural pause/intonation points.
2. **Word boundary** (whitespace) nearest the end of the window, if no
   clause boundary was found.
3. **Hard split** at exactly `max_chunk_size`, only if neither of the
   above exists in the window at all (a keysmash string, a URL, or any
   other unbroken token longer than the buffer).

Verified against 8 cases including short text, normal multi-clause
prose, punctuation-free prose (word-boundary fallback), a pure
keysmash string (hard-split, confirmed zero overflow), a realistic
mixed case (normal words -> long unbroken token -> more normal words,
confirming clean recovery back to word-boundary splitting immediately
after the hard-split token), empty input, and an input landing exactly
on the boundary.

## Look-ahead synthesis + indexing architecture

Per your direction: synthesize the *next* chunk while the current one
is still playing, rather than pausing between chunks -- this removes
the chunk-boundary latency chunking would otherwise introduce, and
Textalker's synthesis should run many times faster than real-time on
any modern machine (it was designed to just barely keep up with an
~1MHz 6502).

### Why we can do better than real hardware indexing here
DoubleTalk's real board (and its MAME model, per the repo you linked)
can report index progress because it's a separate coprocessor that
stays free to report status while speaking. Textalker has no such
thing -- the 6502 is *inside* the synthesis loop and returns only when
completely done. But we're not using real hardware: we own the whole
pipeline (6502 core + TMS5220 core), so we can record, with byte-exact
precision, how many audio samples the chip actually produced for each
piece of text, before any of it reaches a speaker. That's not
approximate the way most hardware index events are -- it's ground
truth, because we generated the ground truth.

### Pipeline
1. Chunk the incoming utterance with `echotalk_chunk_text()`, further
   subdividing each chunk into indexable units (word-level, matching
   NVDA's `IndexCommand` granularity) by tracking word-start offsets
   within the chunk.
2. Maintain a synthesis worker that stays one chunk ahead of playback:
   while chunk *N* is being played out, chunk *N+1* is already being
   fed through Textalker/TMS5220 in the background.
3. For each indexable unit, record `(sample_offset_start,
   sample_offset_end)` by reading the TMS5220 core's cumulative
   sample counter immediately before sending the unit's characters and
   again once its speech has drained (Talk Status back to idle, or the
   next unit's audio begins). Append `(unit_index, sample_offset_end)`
   to a shared index-event queue as each boundary is crossed.
4. The playback side tracks elapsed samples via the audio output
   clock. When playback position reaches a queued sample offset, fire
   the NVDA `IndexReached` callback for that unit, in order.
5. Because synthesis runs ahead of playback, the index-event queue
   should almost always already contain the next few boundaries by the
   time playback needs them -- avoiding any stall waiting on synthesis
   inline with real-time audio output.

### Open implementation questions for the actual library build
- Thread/callback model: whether the look-ahead synthesis runs on a
  dedicated worker thread signaled by a low-water mark on a small
  ring buffer of pending audio, or is pumped from the same callback
  driving audio output. Given NVDA add-ons run inside a Python host
  process calling into this as a native library, a worker thread with
  a lock-protected queue is probably the cleanest boundary.
- How much read-ahead is enough: one chunk should be sufficient given
  the expected synthesis/playback speed ratio, but worth confirming
  once real timing numbers exist end-to-end.
- Interruption (stop/pause mid-utterance, common for screen readers
  when the user navigates away): needs a clean way to abandon
  in-flight look-ahead synthesis and flush the index queue without
  leaving stale events that fire late.
