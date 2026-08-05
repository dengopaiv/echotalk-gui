# Pacing: MAME byte-stream comparison (session 10)

The trace `notes/pacing_investigation_restarts.md` asked for. The user
built MAME with `VERBOSE` enabled in `a2echoii.cpp` and `tms5220.cpp`
and captured a session that speaks "RUBBER BABY BUGGY BUMPERS." twice,
first expanded then compressed (17 MB log, 244,461 lines).

## Headline: Textalker's output is byte-identical to ours

The decisive question was whether MAME's Echo II receives a different
byte stream (divergence on the Textalker side, driven by status reads)
or the same stream with faster restarts (divergence in the chip model).

**It is the same stream.** Extracting every `Data written to latch of
%02x` line from the compressed utterance and comparing against a dump of
our own writes (`ECHOTALK_BYTE_DUMP`):

- Ours: 324 bytes. MAME: 409 bytes.
- Our 324 bytes are a **byte-exact prefix** of MAME's.
- `SPEAK EXTERNAL` ($60) appears at exactly the same offsets in both --
  1, 39, 71, 109, 141, 173, 211, 255 -- eight segments each.

The extra 85 bytes are not part of this utterance. Between MAME's 8th
segment and its 9th there is a 246-line gap in the log, where every
other segment boundary is 9-10 lines; that is a later print (the BASIC
prompt), not a continuation. Counting it as part of the phrase is what
made an earlier estimate suggest MAME restarts more often than we do.

So the 6502 side is exact: same commands, same data, same segmentation,
same number of restarts. Nothing about Textalker's behaviour differs.

## Where the time actually goes

| | ours | MAME |
|---|---|---|
| total | 1717 ms | 1447 ms |
| speaking (TALKD set) | 1250 ms | 1225 ms |
| idle (TALKD clear) | 467 ms | 222 ms |

Synthesis matches to within one frame. **All ~245 ms of the discrepancy
is idle time**, spread across 8 restarts.

MAME's own numbers here come from summing the `process called with size
of N` stream updates, which totals 129,121 samples (16.1 s) across the
whole session -- consistent, and its 1.447 s for this utterance agrees
with the 1.414 s measured directly from `mame.wav`, so the reference
recording is sound.

## Restart timeline from MAME's log

Reconstructed by accumulating stream-update sizes between events:

```
 172.0ms  HALT (stop frame) + RESETL4, SPEN=0 TALK=0 TALKD=0
 173.8ms  SPEAK EXTERNAL written      (1.8ms after the halt)
 180.6ms  SPEN set, FIFO past buffer-low
 222.0ms  RESETL4, SPEN=1 TALK=1 TALKD=1 -- speaking again
```

Ours, from `ECHOTALK_CHIP_TRACE`, for the same transition:

```
 700.1ms  TALKD -> 0
 702.9ms  first byte of the next segment arrives  (2.8ms)
 708.9ms  SPEN set                                 (8.8ms)
 725.1ms  TALK set at next RESETL4
 750.1ms  TALKD set at the RESETL4 after that
```

Both are 50 ms of TALKD-clear, and the 6502 refills at the same speed
(~2 ms to first byte, ~9 ms to cross buffer-low) in both. So the
per-restart sequence looks the same in isolation.

## Unresolved

That is the open contradiction. If each restart costs both
implementations 50 ms and there are 8 of them, MAME's idle total should
be near ours, yet its measured idle is 222 ms against our 467 ms. Either
not every MAME restart costs 50 ms -- the two sampled above may not be
representative -- or the speaking-frame count (49 logged RESETL4 events,
taken as 49 x 25 ms) understates MAME's speaking time and overstates its
idle.

Next step is to resolve that arithmetic properly: walk MAME's log in
order and accumulate the actual TALKD-clear intervals for all eight
restarts, rather than inferring idle by subtraction. The data to do it
is already in the log; only the extraction needs care.

## Tooling added
`ECHOTALK_BYTE_DUMP=<file>` on the loader harness writes every byte sent
to the Echo II latch, in order, in the same hex format MAME logs -- which
is what made the byte-for-byte comparison a one-line diff.
