# Streaming and index events

Session 11, the last two items on HANDOFF's list. The design note from
session 6 (`buffer_chunking_and_indexing.md`) proposed a worker thread
staying one chunk ahead of playback. That turned out not to be needed,
and the reason is worth recording.

## Measure first: synthesis runs at ~136x real time

The Hedge Trimmer Story is 23.6 s of audio and takes 174 ms to produce,
process startup and Textalker's boot included. At that ratio, running
synthesis inline from an audio callback has enormous headroom, and the
whole thread/lock/ring-buffer apparatus the design note anticipated buys
nothing but ways to get it wrong.

So: **synthesis is pulled by `echotalk_read()`.** When the queue runs
dry and text remains, read synthesises the next utterance and returns
it. No threads, no locking, and nothing to reconcile with Fake6502's
globals.

A host that would still rather not synthesise on its audio thread can
call `echotalk_synthesize(et, min_samples)` from a thread of its own.
The library stays thread-free; the host decides the threading. That is
the same split the design note reached for, minus the library owning it.

## What changed

`echotalk_speak()` no longer synthesises. It copies the text into a
pending buffer and returns -- measured at 0.0 ms. `echotalk_read()` does
the work, one utterance at a time.

For a 437-byte passage: first block of audio after **39 ms**, the whole
274,019 samples after **220 ms**. Before, nothing existed until the full
220 ms had elapsed.

Two consequences a host has to know about, both documented in the header:

- `echotalk_available()` is **0** immediately after `speak()`. It means
  "samples ready", not "speech outstanding". `echotalk_pending()` is the
  new call for the latter.
- `echotalk_read()` returning 0 still means "finished", because it
  synthesises before giving up. Existing read loops keep working
  unchanged, which is why `say.c` needed no restructuring.

`echotalk_stop()` now abandons pending text and index events too, not
just queued audio. A screen reader calls it because the user moved on;
leaving the text to resume on the next read would be wrong. This also
answers the design note's open question about interrupting in-flight
look-ahead: there is no in-flight state to abandon, because synthesis
always runs to the end of an utterance before returning.

## Index events

Placed with a Ctrl-D command, `\x04 7I`, alongside the other driver
commands. Collected with `echotalk_next_index()` once the audio before
the mark has been read.

**These are exact, not estimated.** The design note called this right:
we own the whole pipeline, so we are not inferring a position, we are
reporting the sample count we generated.

The implementation is almost free, and for a reason that only holds
because of a decision made earlier in the session: **a Ctrl-D command is
applied only once everything before it has been synthesised.** So at the
moment the mark is recorded, `et->count` already IS its position --
trimming and resampling have both already happened to every sample that
precedes it. No offsets need carrying through either transformation.

That is also why an index mark ends the current utterance, like every
other Ctrl-D command. A position *inside* an utterance is not knowable
until the utterance has been spoken, and by then it is too late to split
it. The cost is the same boundary cost the chunker already pays
everywhere, so marking at clause or sentence granularity is free.
Marking every word would make Textalker's prosody noticeably choppier --
worth saying out loud, since word-level is what the session-6 note
originally imagined.

### One thing that had to be fixed

A mark at the very end of the text only becomes ready on the
`echotalk_read()` call that returns 0. `say.c` collected events only
after successful reads, so a trailing `\x04 3I` never fired. An
end-of-speech marker is exactly what a host is most likely to put there,
so the header now says to drain after every read *including* the last,
and `say.c`, both DLL tests and the listening script all do.

## Restructuring, and what it cost

The old shape was: send every chunk, drain once, trim the whole run,
resample the whole run. Streaming needs audio to be safe to hand out as
soon as it exists, and trimming moves samples, so all of it moved inside
a per-utterance `emit_utterance()`.

Draining per utterance rather than per run shifts where an utterance's
tail is judged to end. Across 18 measurements -- nine reference files,
both Textalker versions -- **seventeen are byte-identical** and one
differs: Hedge Trimmer Story under v3.1.3, 188,811 to 188,812 samples.
One sample in 23.6 seconds. Confirmed against the pre-change build by
stashing and rebuilding, not assumed.

Resampling per utterance would have reset the interpolation phase at
every boundary. `echotalk_resample_stream()` now carries the phase
across calls, so the seams that per-utterance resampling would otherwise
have introduced -- one per utterance at any non-native output rate --
are not there. One approximation remains: the last output sample of each
piece has no following input sample to interpolate towards and holds
instead. One sample per utterance against a phase reset in every one.

Note the default path never resamples at all: at a 1.0 clock multiplier
and 8 kHz output, source and destination rates match and the whole block
is skipped. This matters only for `--rate` and `--clock`.

## A leak that had been there all along

The audio buffer was never wound back -- `count` and `read_pos` only
ever grew, for the life of the instance. Fine for a batch tool that
exits; not fine for a screen reader running for days. `recycle_buffer()`
now resets both once the queue is drained and nothing is outstanding,
rebasing any uncollected index events rather than dropping them.

## Verification

- Harness baselines: all nine files, both versions, exact. `hi_only`
  still at -12127/+26833.
- Library output: 17 of 18 byte-identical, one differing by a single
  sample as above.
- `make test-dll` (ctypes) and `make test-dll-load` (GetProcAddress,
  both architectures) pass, now including streaming, `synthesize()`,
  index ordering, end-of-text marks, and `stop()` discarding pending
  text. 32-bit and 64-bit still produce identical sample counts.
- `tools/listen_check.py` passes all 17 of its automated checks and
  writes a self-narrating WAV for the parts only an ear can judge.
- Run again on Linux against `libechotalk.so`: all 17 pass, and the WAV
  is **byte-identical** to the Windows one. The index marks land at
  samples 679837, 682085 and 685599 on both, which is a sharper check of
  the indexing arithmetic than anything written for the purpose. See
  `dll_packaging.md` for why the determinism matters.

## ABI

Bumped to **2**. Three new exports -- `echotalk_pending`,
`echotalk_synthesize`, `echotalk_next_index` -- and `speak()`'s
behaviour changed, which a host could notice even though its signature
did not.
