# Say All read a wrapped sentence one line at a time

Reported by Jayson: a sentence split over several lines in a document is
read as several separate sentences under NVDA's Say All, where other
synthesisers read it as one.

## Two guesses, both wrong, and a log that settled it

The obvious theory was timing -- that other synths wait briefly to see
whether more text is coming. The second was that NVDA sends one line per
`speak()` and the fix would be to coalesce whatever is already queued.

A diagnostic in the driver (`logsequences.txt`, still there) captured
what NVDA actually sends. From a real Say All over the test file:

```
EchoTalk seq [queued=0 gen=52]:
  Index(75) | text(9) 'This is  ' | Index(76) | text(8) 'a test  ' |
  Index(77) | text(13) 'of multiple  ' | Index(78) | text(13) 'line breaks  ' |
  Index(79) | text(18) 'between words of  ' | Index(80) | text(13) 'a sentence.  ' |
  Index(81)
```

**NVDA sends the whole Say All as ONE sequence**, with the separators
already in the text and an index mark between every line. `queued=0`
throughout: nothing was waiting, so there was nothing to coalesce and
nothing to wait for.

So the text had always arrived together. The only thing splitting it was
our own rule that **an index mark ends the current utterance** -- the
decision from `streaming_and_indexing.md` that made a mark's position
exact. It was cutting the sentence at precisely the six points NVDA had
marked.

Counting the whole log makes the shape plain: 57 marks at the end of a
sequence, where ending the utterance costs nothing because it ends there
anyway, and 6 mid-sequence -- all six from the one Say All.

## The fix

Marks no longer end the utterance. They are collected with their
character offset into the segment and placed once the audio exists:
proportionally by offset for a mark inside an utterance, exactly for one
at a boundary.

The interpolation is an approximation and is documented as one. It has to
be: Textalker buffers a whole line and emits nothing until the
terminating CR, so there is no way to observe which character is being
spoken. The only way to place a mark exactly is to end the utterance
there, which is the thing causing the problem. Any synthesiser that keeps
speech continuous across a mark is doing the same approximation.

Marks at an utterance boundary -- 57 of the 63 in that log -- stay exact
either way, so nothing is lost in the interactive case.

`echotalk_set_index_break(et, 1)` restores the old behaviour for anyone
who wants exact mid-utterance positions and will accept the split.

## An implementation detail worth keeping

Text preparation changes lengths -- a curly quote becomes an apostrophe,
`©` becomes ` copyright` -- so a character offset taken before
preparation does not survive it. The gatherer therefore prepares each run
of text *between* marks separately and appends, recording each mark's
offset in the PREPARED buffer. Splitting there is safe because
preparation is per-character and Ctrl-D is ASCII, so no multi-byte
sequence is ever cut.

## Verified

- The exact sequence from the log now renders as one utterance: 44,937
  samples against 45,366 with marks still splitting, and 44,935 for the
  same text with no marks at all. All seven marks fire in order, spread
  through the audio rather than bunched.
- Harness baselines exact for all nine files under both Textalker
  versions, `hi_only` still -12127/+26833, library counts unchanged,
  single characters unchanged, `make test` passes.
- Both DLL suites and the NVDA driver harness pass, on both
  architectures.

## What this cost, and what it says

The two wrong guesses were both plausible and both would have produced
code -- a coalescing path and a timed wait -- that did nothing for the
actual fault while adding latency to every announcement. Fifteen minutes
of logging replaced them with a fact. When the question is "what does
some other program actually do", ask that program.
