# The NVDA cancel race: stale audio after a cancel

Reported by Jayson after testing the add-on in real NVDA:

> If I change the delay to max or the rate to 0 ... it goes all weird.
> NVDA only seems to want to speak the last thing it tried to say over
> and over again sometimes, and the only safe way out is to revert to
> saved configuration.

Three separate defects, all in the driver rather than the library. The
library reproduces nothing at those settings: a short sentence at speed
0.25 with word delay 15 renders in 170 ms for 14.86 s of audio, no
warnings, no wild-jump traps. What extreme settings change is not
correctness but **duration**, and duration is what widened the windows
below until they were hit every time.

## 1. Audio fed after a cancel

The synthesis loop was:

```python
while not self._stopping.is_set():
    with self._libLock:
        ... read ...          # synthesises a WHOLE utterance
    self._player.feed(data)   # a cancel in between lands here anyway
```

The cancel check happened *before* a call that can take a long time, and
the feed happened after it regardless. A `cancel()` landing in that
window pushed the cancelled utterance's audio into a player that had just
been stopped -- which is exactly what "the last thing said, over and
over" sounds like. Slow settings made the window wide enough to hit on
almost every keystroke.

Fixed by re-checking after the read, immediately before the feed.

## 2. cancel() blocked NVDA's main thread

`cancel()` took `_libLock` to call `echotalk_stop`. The synthesis thread
holds that lock across the read above. So NVDA's main thread -- which
calls `cancel()` on roughly every keystroke -- blocked for as long as an
utterance took to synthesise. That is the freeze that left no way out but
reverting the configuration.

`cancel()` now takes the lock only if it is free, and otherwise leaves
clearing the library to the synthesis thread, which notices the
generation change and does it there. It still calls `player.stop()`
directly, which needs no lock of ours and is what unblocks a feed in
progress.

## 3. A cancelled utterance reported "done speaking"

```python
if not self._stopping.is_set():
    self._player.idle()              # blocks for the whole utterance
    synthDoneSpeaking.notify(...)    # fires even if cancelled during idle
```

The check was made before a call that blocks until playback finishes.
Re-checked after it now.

## The flag was the wrong primitive

All three were patched, but the underlying problem was `_stopping` being
a flag that has to be **cleared** before the next utterance. There is no
safe moment to clear it: a cancel arriving just before the clear is lost,
and one arriving just after is applied to the wrong utterance.

Replaced with a generation counter. `cancel()` increments it; work
captures the generation it was started for and abandons itself the moment
the two differ. Nothing needs clearing, so there is no gap. Index-event
callbacks capture it too, so a mark belonging to cancelled speech cannot
report progress through it.

One window remains, and is documented rather than hidden: between the
final generation check and `feed()` there are a few bytecodes in which a
cancel could still slip through, costing one 1024-sample block. Closing
it fully would mean holding a lock across `feed()`, which blocks when the
player's buffer is full -- and that would put the stall back into
`cancel()`, trading a 128 ms artifact for the bug this note is about.

## Regression tests

`tools/test_nvda_driver.py` grew a stub `WavePlayer` that counts feeds
arriving after a `stop()`, and two cases:

- a single cancel partway through a deliberately slow utterance, which
  checks that `cancel()` returns promptly (measured: 0 ms) and that the
  utterance does not report done speaking;
- **24 rapid speak/cancel cycles at varied phases**, which is the one
  that actually catches the stale feed. The window opens just after a
  read returns, so one well-timed cancel is unreliable; several at
  different offsets is not.

Both were checked against a deliberately re-broken driver: removing the
post-read re-check makes the repeated-cancel case fail with a stale feed
and leaves the single-cancel case passing. A regression test that does
not fail on the broken code proves nothing, so this was verified rather
than assumed.
