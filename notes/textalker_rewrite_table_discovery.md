# Rewriting Textalker in C: table discovery and the differential harness

**STATE. The rewrite is finished and byte-exact for BOTH versions.**
Text goes in and the exact TMS5220 byte stream comes out, with no 6502
anywhere in it.

The Ctrl-E command driver included, for both versions.

Both versions, on the same tests, all exact:

    reference_text 215/215      story corpus 1039/1039
    4400 random words           679 whole lines to the chip bytes
    900 random text lines       1300 random Ctrl-V phoneme strings
    1080 random Ctrl-E command streams (nine seeds)

Nothing is fitted: every metacharacter, table address and special case
is read from the 6502. `tools/profiles.py` holds both address sets and
detects which image is which; every tool takes `--version`.

**The C port is done too**, and is byte-exact against the emulation for
both versions: `src/tk_image.c`, `tk_syllable.c`, `tk_rules.c`,
`tk_synth.c` and `tk_machine.c`, one per Python module, with the same
6502 addresses in the comments. No table is compiled in; everything is
still read out of the user's own image at run time.

Translating a transcription turned out to be exactly as mechanical as
intended -- stages 1 and 2 went in without a single semantic surprise.
Both bugs that did appear were C bugs rather than misreadings of the
6502, and both hid until the driver's persistent buffers were in play:
the punctuation-name lookup wants the MASKED character, and
`base + rom_len` overflows a `uint16_t` for v1.3, whose image ends at
exactly `$10000`, emptying every junk dispatch section. 3.1.3's image
ends at `$FFFA`, so it never showed there.

The second was found in one step because `tk_machine` counts characters
that reach stage 1 and find no rule at all, and the CLI prints the
count. It should always be zero.

**The port is wired into the library** as a second engine
(`echotalk_set_engine`), and both stages plus the driver are byte-exact
at the chip bytes. Its AUDIO is not phase-identical to the emulation:
HANDOFF's `Textalker-RW` section has the numbers, the cycle model, and
the reasoning for stopping where it did.

**Everything on the original "what is left" list is done**, and each
has its own note. Read those rather than this file for any of them:

1. **The rules seam** -- `tk_g2p` in `src/tk.h`, with a CMUdict-format
   dictionary as the first source. `notes/phoneme_alphabet.md` has the
   code-to-sound correspondence it stands on.
2. **The table extractor** -- retired rather than built. Reading every
   table out of the user's own image AT RUN TIME made a pre-extraction
   step pointless, and `src/tk_image.c` now FINDS the tables rather
   than listing addresses: `notes/profile_discovery.md`. The extractor
   people actually needed was for the DISK, and that is
   `notes/disk_image_extraction.md`.
3. **The v3.1.3 Ctrl-V phoneme-mode restart** is the one thing still
   open: a one-to-two sample chip phase difference, and the
   recommendation remains to leave it.
4. **Textalker's own input chunking IS reproduced**, and the claim here
   that it "is now gone" was wrong. `$DB6A` flushes the line buffer at
   240 bytes and `tk_machine` does the same. What went away is the
   EMULATION's 80-character limit, which was never Textalker's; see
   HANDOFF on the chunk window and the extended line buffer.

## Modelling the 6502's time (session 15)

The port produces the right bytes; the chip's output also depends on
WHEN they arrive, because SPEAK EXTERNAL resets the FIFO and the chip's
interpolation phase decides how many idle frames an utterance starts
with. Reproducing that meant costing Textalker's own work in cycles.

**Profile before modelling.** `ECHOTALK_CYCLE_PROFILE` buckets 6502
cycles by page: 65% goes to `$FC00` (the status polls and the chip
write), 26% to `$E900` (the bit packer) and about 3% to stage 1. Nobody
would have guessed that split, and guessing would have put the work in
the wrong place.

The constants live in `src/tk_synth.c`, `src/tk_rules.c`,
`src/tk_syllable.c` and `src/tk_machine.c`, each with its derivation in
a comment. Most are counted off the listing; `TK_S1_RULE` and
`TK_DRIVER_CYCLES` are CALIBRATED against the profiler instead, and say
so. `ECHOTALK_CYCLE_DUMP` gives the cycle count at every chip write,
which is the check: the model has to reproduce the gaps it shows
(a floor of 291 cycles, commonly 373/395/400).

**Where it got to, and why it stopped.** Whole utterances land within
about 0.01% in length, and a short utterance's first chip byte lands at
sample 71 where the emulator puts it at 76. But the fuzz agreement did
not move through ANY of it -- 27 of 40 for v3.1.3, 38 of 40 for v1.3,
identical before and after every refinement. A residual of a few cycles
per operation still compounds into a one-to-two sample phase error at
an utterance restart. Closing it means counting every instruction of
the stage-2 driver as well, which is a cycle-accurate 6502 model
specialised to Textalker -- and that already exists, and is exact: it
is the emulator.

**A measurement lesson worth more than the model.** Comparing the two
engines' audio was got wrong THREE times, and each version reported
defects that did not exist:

- fixed windows -- the engines differ in gap LENGTH, so a window
  containing a gap boundary never aligns;
- one alignment per speech run -- a gap can change by a single sample
  mid-run, and the half that did not fit was reported as corrupt;
- re-searching the alignment per block -- a 400-sample block finds
  false matches thousands of samples away, which condemned two
  identical runs as garbage.

What works is `tools/engine_diff.py`: one alignment carried across the
whole file, found once over a block long enough to be unambiguous, then
allowed to move a few samples per block. At least two phantom defects
were chased before the metric was fixed.

## Finishing v1.3's command driver (session 14)

It sat at 78% and the residue was assumed to be the stale-lookahead
family already solved for 3.1.3. It was not: it was seven separate
readings, and every one of them was a place where the port had modelled
*3.1.3's* behaviour and v1.3 does something else. They are listed here
because each is the kind of thing that is invisible until a rule reads
past the end of the buffer, which is exactly what the command fuzz
makes happen.

**v1.3's startup state comes from its LOADER, not its image.** The
tools read a variable's initial value out of the OBJ, which is right for
3.1.3 -- its image carries its own defaults -- and wrong for 1.3, whose
loader sets eleven variables at `$9410-$9445` before the first
character arrives. The flags byte reads `$00` from the image, speech
OFF, where the machine has `$09`. Every text that did not open with an
explicit Ctrl-E T was silent in the port and spoken by the emulator. The
fuzz never caught it because every case it generates starts with
`T`. `profiles.loader_init` now reads the `LDA #imm` / `STA abs`
block straight out of the loader.

**Neither buffer is initialised at boot either.** "Nothing ever clears
them" reaches back further than the previous utterance: before anything
is spoken they hold whatever the image carries at those addresses.
3.1.3's are zeros, so starting from zero worked by luck. v1.3's are
not -- `$F494` is `$A0`, inside the `$51-$B2` window `$DC7C` tests --
so the FIRST utterance's last code already sees a fake following "T"
and takes the `+8` stop release. "ED" flushed by a Ctrl-E is the whole
of the reproduction.

**An allophone is eight frames in 1.3, ten in 3.1.3.** `$ECEC`
decrements the frame counter *before* unpacking and loops on `BNE`;
`$E7E0` decrements after and loops on `BPL`. Eight is the honest number
-- the frame mask is one byte -- and 3.1.3's last two passes emit
nothing because the mask has shifted itself empty. It matters anyway,
because Ctrl-E C tests the counter's bottom bit and the counter now
reads 8,7,..,1 instead of 9,8,..,0. The TEST is inverted to match:
`$EDB9`'s `BEQ` emits where `$E8AB`'s `BEQ` skips. The two inversions
cancel, so both versions drop the same half of the frames -- and
modelling either one alone gives compressed speech the other half.
(v1.3's compress flag is `$F6C7`; the profile had it as absent.)

**The dispatch pointer is followed wherever it goes.** The table has
entries for `$20-$65` only, so any character above that reads its
"pointer" out of the rules themselves. `$7F` -- which is what a leftover
`$FF` in the buffer masks to -- gives `$F3C5`, in the allophone data,
and the engine parses rule bytes there and emits `12 0C`. Refusing to
follow a pointer outside the rule area dropped two phonemes from the
first word that read past the end. Two further things fall out of it: a
section has no end (`$DADB` just steps to the next rule and tries
again, forever, so six rules is not enough for a junk pointer), and a
junk pointer can land mid-payload, where `$DA96`'s anchor search walks
over the payload bytes without caring.

**v1.3's greedy runs stop at a FIXED `$8F`.** `$DC90` and `$DD19` both
do `CPX #$8F`. 3.1.3 copies `$FD9E` into `$FD99` at `$E3EF` and
compares against that, so its runs stop where the text does. The
difference only shows once a rule is reading past the end:
`{F3}{F8}F{F8}{F3}` matches "F" against a bounded run and fails against
the real one, so "FR" flushed by a Ctrl-E said "eff" instead of "f-r".
The same `$8F` caps the inflectional-ending matcher at `$DD0D`, which
is the only way it can succeed without finding a boundary -- with the
end of the buffer there instead, `E{F4}{F9}` matches "EZES".

**The harness was truncating the oracle in silence.**
`render_text_loader` gave each character 5,000,000 steps -- the same
number `src/echotalk.c` records as "FAR too low, which cost a real
bug", for the same reason. A rule that consumes past the end of the
buffer emits 240 phonemes for a two-letter word, which is half a minute
of speech from one character, and the harness cut it off with a warning
nothing was reading. `byte_diff` was comparing the port against a
truncated emulator and calling the difference a defect. The budget now
matches the library's 64,000,000, and `byte_diff` reports a truncated
oracle as "not compared" rather than as a disagreement.

**Method, since it is the only thing that ever worked here:** take a
failing word, find which rule the *emulator* fires with
`tools/rule_probe.py`, and if that is not enough, watch the emulator
read its own tables and see how far into a rule it gets. Reasoning from
the listing produced several confident wrong answers; the machine
produced none.

**How to read this note.** It was written incrementally while the
readings were still being worked out, and large parts of it are
**superseded**. In particular, everything below that treats a syllable
boundary as a zero-width assertion, and everything that reads `$9C` as
"the coronal stops", is wrong; see "What was wrong with the model"
immediately below. The authoritative summary is the `Textalker-RW`
section of `HANDOFF.md`, and the authoritative *implementation* is
`tools/syllabify.py` and `tools/rule_walk.py`, both of which are now
transcriptions of the 6502 with the addresses in the comments.

## What was wrong with the model (session 13)

The score sat at 85% with a page of careful measurements behind it. Two
things were wrong, and both were *models* standing in for the machine.

**`$9C` is a wildcard, not the coronal stops.** `$E378` does `CMP #$9C`
and branches straight to the match continuation without comparing
anything -- exactly what `$E309` does for `$C0` in the forward
direction. The `HO?ED` probe that appeared to show "D and T" was
measuring `$9B` beside it: `-ED` is its own syllable only after a
coronal stop, so those are the only members of that family with a
boundary for `$9B` to find. Worth 17 words on its own.

**A syllable boundary is a real `$E6` byte in the buffer.** It is not an
assertion about a position: it is a character the matcher walks over,
and `$E6` masks to `$66`, whose attribute at `$EF24` is `$00` --
neither vowel nor consonant nor separator -- so it **blocks every
class**. This is the whole of the hand-written "magic E" special case
that this note spends a long section justifying. HOPE is `|HOPE|` and
the `{F7}` of `{F7}{F8}{F4}E` reaches the O; HOPESX is `|HO|PESX|` and
the same `{F7}` runs into the sentinel and fails. There is no special
case, only a byte in the way. Deleting the hack and matching the real
buffer took the story corpus from 85.2% to 97.1%.

The rest followed from reading the handlers instead of inferring them:
`$F7`/`$F8` are greedy with no backtracking; `$FA` and `$FB` are a pair
(the C/S/T and the H of a CH/SH/TH digraph) rather than "coronal" and
"sibilant"; `$FD` reads a literal 7-byte table at `$E57E`; `$FE`/`$FF`
read the syllable counter of the **cursor**, not of the position being
matched; the anchor range is `$A0-$EF`, so `$E6` can anchor a rule,
which is how `{F3}DR{E6}{AE}` matches "DR." across a sentinel.

Then the syllabifier's prefix/suffix stage turned out to loop rather
than match once (`CARE|FUL|LY` strips two), the `-ES` rule was exactly
backwards, and `$FDAC` holds the **whole line** rather than one word.
Those closed the last 3%.

One more came out of fuzzing after that: `$DF3A` does `CPX #$01`, an
**absolute** buffer index, so the `$DF35` "vowel W vowel" rule is
disabled only for the word standing first in the line. Building each
word in its own little buffer made every word look first. The bug was
invisible to the corpora because the differential test itself was
asking the wrong question -- the oracle ran six words per line while
the walker was called on each word alone, so the two sides were never
comparing the same thing.

**The lesson worth keeping: a special case you have to invent is a sign
the model is wrong.** The magic-E check had no counterpart anywhere in
the machine. It should have been suspected on that ground alone, before
any of the measurements that propped it up.

Session 12, branch `Textalker-RW`. Goal: reimplement the text-to-phoneme
and phoneme-to-TMS5220 stages in C so the 6502 emulation can be dropped,
lifting the limits the real binary imposes (chunk size, no pronunciation
dictionary).

Two decisions were taken before any of this, and they shape everything
below:

- **Byte-exact against the emulation is the acceptance test.** Any
  improvement is an opt-in layer around a core that still reproduces
  Textalker exactly. Without that, correctness could only be judged by
  ear across all of English, which is unaffordable.
- **Tables keep being extracted from the user's own Textalker images.**
  The engine is ours and ships; the data is Street Electronics' and does
  not. This keeps the existing legal stance exactly as it is -- see
  README's "Obtaining the Textalker images".

## The headline: Textalker is table-driven

This is not a from-scratch English TTS reimplementation. Both stages are
data walked by a small interpreter, so the work is decoding two tables
and writing the two walkers.

### Letter-to-sound rules

Rules are reached through a dispatch table of little-endian pointers.
It was first found as an apparent 26-entry letter table at `$F061`:

```
A -> F0AB   B -> F25B   C -> F26B   D -> F2AB   E -> F2E0   ...
Z -> FACD
```

**That reading is superseded.** `$F061` is simply the `'A'` entry of one
contiguous array indexed by raw ASCII, `entry(c) = $EFDF + 2*ord(c)`,
covering `0x20-0x65` and reaching digit and punctuation sections as well
as letters. The rule area also runs past `$FBF1`, which was taken for
its end. **See "The dispatch table, found" below** -- that section is
authoritative, and correcting this raised the rule count from 525 to 568
and made `69105` speakable.

Rule encoding, from the `A` section at `$F0CB`:

```
C1 C2 D3  F3  1F 5E 1C 26 5B    C1 D3 C3  F3  1F 26 5B 5B 1C
"A  B  S" {F3}  phoneme codes   "A  S  C" {F3}  phoneme codes
```

- match text is ASCII with the **high bit set**, Apple II style
- pattern bytes are **all >= `$80`**; phoneme codes are **all < `$80`**
- **rules are delimited implicitly by the high-to-low transition** --
  a run of `>= $80` is the pattern, the run of `< $80` after it is the
  payload, and the next `>= $80` byte starts the next rule

**The letter-table pointer is off by one from where the rules start.**
It lands on the **last payload byte of the preceding section**, and the
first pattern byte is at `ptr + 1`. Section `L` therefore spans
`ptr(L)+1 .. ptr(L+1)` inclusive. Proof: the byte at each pointer is the
previous letter's own phoneme -- `$F26B`, the `C` pointer, holds `5E`,
which is B. Slicing from `ptr` instead silently drops every section's
last payload and produces rules with no phonemes at all, which is how
this was noticed. Every section then begins with `F3 F8 <letter> F8 F3`,
the "letter spoken alone" rule.

**Correction.** An earlier version of this note said `$F3` delimits the
fields. It does not. `$F3` is simply the most common *metacharacter* --
the word boundary -- and it happened to sit at a field edge in the first
few rules looked at. Parsing on `$F3` produces fields of 96 and 67 bytes
containing many rules once you get past the start of a section. Parsing
on the high/low transition yields **525 rules across the 26 sections**,
all well-formed.

Non-letter bytes `>= $80` in a pattern are context metacharacters. The
top of the inventory, with how often each appears before/after the focus
letter:

| byte | uses | before | after |
|---|---|---|---|
| `9B` | 183 | 53 | 130 |
| `F3` | 182 | 98 | 82 |
| `F8` | 103 | 43 | 60 |
| `F4` | 72 | 22 | 50 |
| `FC` | 70 | 2 | 68 |
| `FE` | 36 | 35 | 1 |
| `F9` | 28 | 0 | 28 |
| `E6` | 20 | 0 | 20 |

`FE` is left-context only; `F9` and `E6` are right-context only.

### Allophone / LPC frame data

Not yet confirmed. `$EB00-$EE00` holds repeating fixed-width records --
`B2 8D C9 66 7A 90` three times, then `B3 2F 53 2A 9C 90` four times --
which is what an allophone table looks like when duration is encoded as
frame repetition. Treat as a strong hypothesis, not a result.

## The differential harness: three checkpoints

The existing emulation is a perfect oracle. Three places to compare a C
implementation against it, each independently checkable.

### 1. Text -> phoneme codes  (`ECHOTALK_PHONEME_DUMP=<file>`, new)

Textalker builds a string of one-byte phoneme codes before it makes any
LPC frame. Buffer base is **version-specific**:

| | base | writer |
|---|---|---|
| v3.1.3 | `$FEAD` | `$E23D` |
| v1.3 | `$F492` | `$DD4F` |

`$1A` terminates the string.

**How the buffer was found, and why it is trustworthy.** The decoded
rule for `"ABS"` predicts the codes `1F 5E 1C 26 5B`. Speaking `ABS` and
watching RAM produced exactly that sequence, contiguous, at `$FEAD`.
A prediction from the decoded table landing byte-for-byte in memory
confirms the rule format and locates the buffer in one test.

Two traps, both hit and both fixed in the dump implementation:

- **Watch the language card, not low RAM.** 52,134 of 52,245 writes for
  a single word go to page `$01` -- the stack. Textalker's workspace is
  at `$D000-$FFFF`. A watch over `$0000-$BFFF` sees nothing of interest.
- **A fixed 256-byte window is not enough.** An unrelated counter sits
  at exactly base+255 in *both* versions (`$FFAC` and `$F591`) and
  interleaves itself with the phonemes. The dump therefore tracks the
  contiguous run: a write at base+len appends, a write below len is a
  back-patch, anything else in the page is ignored.

### 2. Phoneme -> LPC frames

Textalker's Ctrl-V phoneme mode (0x16, terminated by CR) drives this
stage directly, isolating it from the rules. Not yet exercised.

### 3. End to end: the chip byte stream (`ECHOTALK_BYTE_DUMP`, already existed)

Every byte written to the Echo II latch, in order. `HI` is 67 bytes:

```
60 0c 48 5a 94 00 45 9b 19 20 1b d5 95 15 3b 69 e6 4d 56 56 5d ba 9a
2f 59 59 f3 66 e2 f1 64 65 dd ab 70 c4 92 95 4d af 4c 95 4b 46 36 bd
32 55 2e 69 d9 f4 ca 54 b9 e4 ff ff ff ff ff ff ff ff ff ff ff
```

**This is the real acceptance test.** The TMS5220 core is not changing,
so identical bytes in means identical audio out, and the whole existing
baseline table transfers unchanged. It converts "does this sound right
across all of English" into a mechanical pass/fail.

## What the phoneme strings show

`THE CAT SAT ON THE MAT.`

```
v3.1.3: 54 29 19 | 61 24 63 19 | 5b 24 63 19 | 1b 52 19 | 54 29 19 | 51 [b3] 24 63 | 34 34 1a
v1.3:   54 0f 7f | 61 24 63 7f | 5b 24 63 7f | 25 52 7f | 54 0f 7f | 51 [b3] 24 63 | 34 34 1a
```

- `$1A` terminates, `34 34` is the sentence-final fall
- word separator is `19` in v3.1.3 but `7f` in v1.3
- codes are consistent within a version: `5A` is H in both `HI` and
  `HELLO`; `CAT`/`SAT`/`MAT` share `24 63`

**The alphabets are NOT identical between versions.** Most codes agree,
but the separator, the `THE` vowel (`29` vs `0f`) and the `ON` vowel
(`1b` vs `25`) differ. An early claim in this session that the alphabet
was shared rested on `CAT` alone and was wrong.
**Each version needs its own phoneme table.**

## The stress pass back-patches the string (SOLVED -- see "The stress pass, ported")

For `MAT.` the buffer is written `51 24 63` by `$E23D`, and then two
*other* routines rewrite bytes already emitted:

```
$FEC1 <- 63  PC=$E2D8      shift right
$FEC0 <- 24  PC=$E2D8      shift right
$FEBF <- B3  PC=$E2EF      insert
```

leaving `51 B3 24 63`. So `B3` is **inserted** before the vowel of the
final stressed word by a pass that runs after the rules, copying the
tail forward to make room.

This is the intonation/stress logic, and it is **code, not table** --
the risk flagged at the start of this work, now confirmed to exist and,
usefully, confirmed to be *observable*. It is the part most likely to
resist clean extraction, so it should be characterised early rather than
discovered at the end. It is also why `nP` and `nF` are one setting.

## Build note

Prepend the MSYS2 UCRT64 bin to PATH; do **not** name gcc by absolute
path. Windows does not add an exe's own directory to the DLL search
path, so `CC_NATIVE=/s/msys/ucrt64/bin/gcc.exe` starts gcc and then
fails to load `libisl-23.dll` and friends from that same directory,
raising a burst of modal dialogs. `export PATH=/s/msys/ucrt64/bin:$PATH`
fixes the DLL resolution and shadows the stale Cygwin gcc at once.

## Decoding the metacharacters: the method that works

The classes are **not** stored as data. Searched for and did not find:
a letter bitmap (no 4-byte window in the image equals the AEIOU set),
and a class string (neither `AEIOU` nor its high-bit form appears).
They are almost certainly code, which is what a 6502 rule engine would
do -- a routine per class, reached through a dispatch table.

So decode them by experiment instead. **`ECHOTALK_READ_WATCH=<lo>-<hi>`
makes the emulator name the rule that fired.** The engine walks the rule
list, and the winning rule's payload is fetched at `PC=$E238`, one read
per phoneme, immediately before `$E23D` writes it. Watching reads over
`$F000-$FBF0` and filtering to `$E238` gives, for every letter of every
word, the exact address of the rule that produced it -- which maps
straight back to a parsed rule.

The other PCs in the trace are the matcher itself: `$E2F9`, `$E309`,
`$E314`, `$E358`, `$E374`, `$E3AD`, `$E3CA` compare pattern bytes, and
`$E340` reads whole rules in one run, so it is the "skip to the next
rule" scan. `$F061`/`$F062` are read at `$E21E`/`$E223`, which is the
letter-table lookup and independently confirms that table.

This beats inferring classes from phoneme output, which is confounded by
rule ordering: an earlier, more specific rule wins and hides the class
you are trying to measure.

### What is established

**`F3` is the word boundary.** `{F3}A{F3} -> 1F` is "A" as a whole word,
and speaking a lone `A` gives `1F 1A`. `{F3}{F8}J{F8}{F3} -> 65 1F` is
the letter J spoken as "JAY".

**`9B` is a vowel class, and it is not adjacency-tested.** Probing
word-initial A against every following letter, `{F3}A{9B}` fires for
exactly `AA` and `AO` -- the two vowels with no more specific rule, since
`AE`, `AI`, `AU` and `AY` are caught earlier. Extending to three letters
is the discriminating result:

```
ABA -> {F3}A{9B}      => 29   (schwa)
ABO -> {F3}A{9B}      => 29
ABU -> {F3}A{9B}      => 29
ABB -> A{F4}{F8}{9B}  => 24   (the "cat" vowel)
AB  -> A{F4}{F8}{9B}  => 24
```

`{F3}A{9B}` fires when a vowel appears **two** positions later, across an
intervening consonant. So `9B` is not "the next character is a vowel";
it behaves as "skip any consonants, then a vowel". That is the real
English open-syllable rule, and it gives the right answer: *about*,
*again*, *around* all take a schwa, while *ab-* takes the short vowel.

**`F8` is zero or more consonants.** Every section opens with the rule
`{F3}{F8}<L>{F8}{F3}`, which is "this letter, spoken as its own name" --
and it fires for every letter of an all-consonant word, because a word
with no vowel gets spelled out:

```
V     -> {F3}{F8}V{F8}{F3}   both F8 match nothing
VB    -> fires for V and B    right F8 matches one consonant
VBCD  -> fires for all four   right F8 matches BCD, three consonants
BCDV  -> fires for all four   left  F8 matches BCD
V + any vowel -> plain `V`    F8 will not match a vowel
```

`Y` groups with the vowels, not the consonants.

**`F4` is exactly one consonant.** Probing `{F3}V{F4}` and `{F3}X{F4}`
against `V?A`/`X?A` fires for exactly the 21 consonants and no vowel.
The arity is settled by `{F3}{F4}OW{F3} -> 3C`:

```
BOW, COW      -> fires    one consonant before OW
BROW, THROW   -> does NOT fire, falls to `OW` => 21
OW            -> does NOT fire
```

One consonant only, not one-or-more, and not zero.

Incidentally this is a real Textalker mispronunciation worth keeping as
a regression case: `BROW` and `THROW` both get `21`, the long O, which
is right for *throw* and wrong for *brow*.

**Trailing classes are satisfied when the input runs out; trailing
literals are not.** This is what reconciles `9B`:

```
A{F4}{F8}{9B} on "AB"   fires -- F4 takes B, then F8 and 9B are reached
                        with the input exhausted, and are satisfied
{F3}A{9B}    on "AB"    does NOT fire -- 9B is reached with "B" still
                        present, so it has to match, and it cannot
{F3}{F4}OW{F3} on "BO"  does NOT fire -- the literal W is reached with
                        the input exhausted and is NOT satisfied
```

So `9B` is **zero or more consonants followed by a vowel**, which is the
English open-syllable test, and the earlier doubt about it is resolved:
the `AB` case was the exhaustion rule, not a property of the class.

**There are two letter encodings, and the difference is what gets
consumed.** `$C1-$DA` is high-bit ASCII A-Z; `$81-$9A` is `$80` + the
1-based letter index, so `$82` is B, `$8B` is K, `$8E` is N, `$96` is V.
The `$8x` group is **not** a set of classes, which is what it looked
like at first.

- `$C1-$DA` letters are the **focus**: matched *and consumed*.
- `$81-$9A` letters are **context**: matched but *not* consumed.

Proved by consumption counts, which the probe shows directly, because a
consumed letter does not go on to fire a rule of its own:

```
ABS      {F3}ABS{F3}      fires once, swallows all three letters
NATION   TI{F7}           consumes T and I; the I never fires
PEOPLE   {90}E{E6}O       consumes E and O; 90 is context P
ALL      A{8C}{8C}        consumes only A; both L's still fire
RUBBER   B{FC}{82}        first B silent, second B fires separately
BACK     C{FC}{8B}        C silent, K fires -- 8B is K, so this is "CK"
```

So **a rule consumes exactly its focus-family letters**, and the pattern
is anchored so the first focus letter sits at the current position:
everything before it is left context, everything after is right context.

**`9B` is a zero-width lookahead.** It asserts "zero or more consonants
then a vowel" *without consuming anything*. This is what finally fits
every observation:

```
{F3}{84}A{9B}{96}{89}{84}{F3}   DAVID -- lookahead sees V,I; then V,I,D match
{F3}{84}{89}{9B}A{8E}{85}{F3}   DIANE
A{9B}{8E}{99}  => 26            ANY -- correctly the EH vowel
{F3}A{9B}      on AA, ABA       fires: a vowel is coming
{F3}A{9B}      on AB, ABB       fails: no vowel is coming
```

Rule order is not the explanation -- `{F3}A{9B}` is index 30 in section
A and `A{F4}{F8}{9B}` is index 61, so the first genuinely fails on `AB`.

### F5, F7 and F9 -- decoded, and an omission worth admitting

**`F5` and `F7` were missed** in the first pass through the inventory.
They are in the frequency table (41 and 27 uses) but the "what remains"
list named only `FC`, `FE`, `F9`, `E6`. They are ordinary classes, and
leaving them out is why the first walker run scored only 48.8%: it
refuses unknown classes, so every rule containing one silently failed.

- **`F5` is a front vowel: E, I or Y.** `C{F5} => 5B` and `G{F5} => 65`
  are soft C and soft G. Probing `C?A` fires for exactly `EIY`; `G?A`
  fires for E and Y, with I caught earlier by `{F3}G{89}`.
- **`F7` is any vowel.** `A{F8}{F7} => 39` and `I{F8}{F7} => 3A` are the
  magic-E long vowels, `TI{F7} => 5C` is *-tion*, and `{9B}H{F7} => 5A`
  is H-before-a-vowel. Probing `H?A` fires for exactly `AEIOUY`.
- **`F9` is the letter E.** `T{89}{F9}` fires only for `TIE` and
  `{F3}{F8}I{F9}` only for `LIE`. It is *not* a final-E marker: `TIES`
  and `LIED` fire it too. Why there is a second encoding of E when
  `$85` already exists is unexplained.

### `FE`: a vowel occurs earlier in the word

`{FE}OR{F3} => 2E` fires for `DOCTOR`, `MAJOR` and `ACTOR` but not for
`FOR` or `OR`. It is not "not word-initial" -- `FOR`'s O is not
word-initial either. What separates them is an **earlier vowel**, so
`FE` asserts this is not the first syllable. That is also what stops
`{FE}A{8E}{F8}{F3}` from stealing `AND` from `A{F4}{F8}{9B}`.

Worth **+4 points** of agreement when implemented, so it is probably
right, but it is a fitted hypothesis rather than a proof.

### Still uncharacterised: FC, E6

All four are **zero-width** -- they sit between letters that are
adjacent in the word and consume nothing:

```
{97}{88}A{FC}{94}    WHAT      B{FC}{82}  BB      C{FC}{8B}  CK
TI{E6}O              -tion     S{E6}CI    sci     G{E6}H     silent gh
```

Their positional signatures are distinctive and worth recording:

- **`F9` is always the last byte of a pattern** -- 28 of 28 uses.
- **`FE` is nearly always at the front**, 35 before the focus against 1
  after, and is followed by a focus letter or by `F4`.
- `FC` is 68-of-70 after the focus; `E6` is 20-of-20 after it.

Being zero-width and edge-anchored, they are more likely **annotations
-- stress, syllable boundary, or a matching mode** -- than character
matchers. Note the project already knows the stress pass is separate
code that back-patches the phoneme string, so an annotation feeding it
is plausible.

**Do not decode these by staring at more rules.** The efficient route is
the walker: implement the matcher with the semantics above, treat these
as no-ops, and diff against the oracle over a corpus. Whatever they do
shows up as a bounded set of divergent words.

## The walker, and agreement as a number

`tools/rule_walk.py` implements the matcher in Python -- cheap to
iterate before committing the semantics to C -- and `--diff-refs` scores
it against the emulator over a corpus taken from `reference_text/`.

**This is the important change in method.** Hypotheses about the
remaining metacharacters can now be accepted or rejected by
measurement instead of by argument.

| change | `reference_text` (215 words) |
|---|---|
| first run: `F3`, `F4`, `F8`, `9B`, letters, consumption | **48.8%** |
| add `F5`, `F7`, `F9` | **60.0%** |
| add the `FE` assertion | **64.2%** |
| narrow `9B` to "zero or ONE consonant" | **60.9% -- rejected** |
| longest-match selection + `9B` = `end_or_la2` | **69.8%** |
| add `F6`, `FA`, `FD` | **72.6%** |
| stop satisfying classes at end of input | **80.0%** |
| `9B` excludes a silent word-final E | **80.5%** |
| `9C`, and the hard-coded magic-E check | **81.4%** |
| `FB` and `FF` | **81.9%** |
| **`9B` is the syllable boundary, not a vowel lookahead** | **89.3%** |
| `F9` is an inflectional ending, not the letter E | **90.2%** |
| first-match, reversing the longest-match finding | **92.6%** |
| `FE`/`FF` as syllable tests, `F7` one-or-more | **93.0%** |

The nonsense-word story is a much harder corpus -- long words and
invented ones, which cannot be short-circuited by whole-word rules. It
is Jayson's own text and is NOT in the repository; the tools that use it
(`--diff-story`, and `tk_diff.py`'s story runs) expect it in `tmp/`
under the name their STORY constant gives.
It scores **88.3%** over 600 words at the same settings, and is the one
to optimise against.

**A second fit, run once `F6`/`FA`/`FD` were in, gained a single word**
(`FC` = `not_end`), and that word did not reproduce on the held-out
`reference_text` set. It was not adopted. One word is noise, and taking
it would have meant carrying a fitted constant that nothing supports.

That last row is the point. `INTO` looks like proof that `9B` allows at
most one consonant: the engine refuses `{F3}I{9B}` there, and after the
I comes `NTO`, two consonants before the vowel. Implementing that fixed
`INTO` and **broke `THERE`, `THEY` and `WHO`**, costing more than it
gained. The narrowing is reverted; `9B` stays "zero or more consonants
then a vowel", and `INTO` has some other explanation still to find.

A compelling single-word story was wrong twice in this session now --
this and the `$F3`-as-delimiter reading. Prefer the corpus score.

## The systematic fit, and what it found by NOT working

`tools/rule_fit.py` (since deleted; nothing was left to fit) searched
candidate meanings for the then-unsettled codes
by coordinate descent, scoring each against a cached oracle. Running it
moved agreement by **about one point**, from 50.0% to 51.2%.

That negative result is the useful part: **the remaining error was not
in those four codes at all.** Two much larger causes turned up once the
fit stopped being the suspect.

### The metacharacter inventory was incomplete

Auditing every distinct pattern byte against what the walker handles:
50 distinct codes, **10 handled and 40 not**, leaving **72 of the 525
rules (14%) permanently unreachable**. Most of the missing ones are not
classes at all -- they are **literal non-letter characters** in the same
high-bit ASCII scheme as the letters:

```
{B0}..{B9}  '0'-'9'  ->  ZERO ONE TWO THREE FOUR FIVE SIX SEVEN EIGHT NINE
{AC} ','  -> COMMA        {AD} '-' -> DASH     {BF} '?' -> QUESTION MARK
{A1} '!'  -> EXCLAMATION  {A2} '"' -> QUOTE    {C0} '@' -> LINEFEED
{DB} '['  -> LEFT BRACKET {E0} '`' -> GRAVE ACCENT
```

So **the digit-by-digit reading is table-driven**, not special-case
code: each digit is a rule whose payload is its own name, which is why
`69105` is "six nine one zero five" in both versions. Abbreviations use
the same mechanism -- `{F3}DR{E6}{AE}` is `DR.` -> DOCTOR and
`{F3}ST{E6}{AE}{A0}` is `ST. ` -> SAINT, with `{AE}` a literal `.` and
`{A0}` a literal space.

The walker now treats `$A0-$E5` (minus the known classes) as literal
characters. `F6`, `FA` and `FD` are decoded below. Still unhandled, but
only 5 uses between them: `9C`, `FB`, `FF`.

### F6, FA and FD are phonetic classes

The three codes the walker was still refusing outright. Each was probed
through two different rules and gave the same membership both times:

| code | members | what it is | the rule it drives |
|---|---|---|---|
| `F6` | B D G J L M N R V W Z | **voiced** consonant | the voiced plural: `{F6}S{F3} => 57 5B`, the /z/ of DOGS and YEARS |
| `FA` | D J L N R S T Z | **coronal** consonant | U as /oo/ not /yoo/ after a coronal: SUIT, JUICE, RULE, NUDE |
| `FD` | B C D G K P T | **stop** / plosive | `{FD}A{94}{85}` is *-ATE*; `{9B}{FD}OW{8E}` is the OW of DOWN, GOWN, TOWN |

`F6`'s complement is exactly the voiceless set C F H K P Q S T X, which
is what makes the voiced/voiceless reading solid rather than a guess.
Vowels are not in `F6`; they have their own plural rules
(`{9B}{F8}{F7}{F5}S{F3}` and friends), and Y is excluded too.

Worth **+2.8** on `reference_text` and **+6.0** on the story corpus.

### Rule selection is longest-match, not first-match

`YEARS` is the tell: the engine picks `EAR`, which consumes three
letters, over `E{9B}{F7}`, which consumes one and sits earlier in the
table. Preferring the rule that consumes the most letters, ties broken
by table order, is worth **+5.8 points** on the story corpus and
**+5.6** on `reference_text`, so it is not corpus-specific.

This had been an unexamined assumption since the walker was written --
worth remembering that the structural guesses deserve the same
measurement as the semantic ones.

## Auditing the anchoring and left-context assumptions

Done deliberately after longest-match, on the theory that other
structural guesses might be wrong too. **Most of them survived.**

**Left context is stored in normal order, not reversed.**
`{97}{88}A{FC}{94}` is W-H-A-T. Reversed storage would make it HWAT, so
matching the left context leftward from the cursor is right.

**Every rule has at least one literal**, so anchoring on the first one
never fails for want of an anchor.

**Selection strategies, measured** (story 600 / `reference_text` 215):

| strategy | story | refs |
|---|---|---|
| **most letters consumed, ties to table order** | **59.7%** | **72.6%** |
| count of literals incl. context | 58.7% | 74.0% |
| first match in table order | 52.2% | 67.9% |
| longest pattern in bytes | 23.5% | 36.7% |
| consumed, then pattern length | 38.8% | 48.4% |

Consumed-with-table-order-tiebreak stands. Literal-count is a wash --
better on refs, worse on story -- and the pattern-length variants are
catastrophic, which is itself reassuring: the ranking is not arbitrary.

### Where the remaining error actually is

Of 242 failures on the story corpus:

- **195 are the same length with wrong codes**; only 47 differ in
  length. So the walker is picking the right *number* of rules and
  consuming the right letters -- it is choosing the wrong *variant*.
- 88% are already correct at phoneme 0; the first wrong phoneme peaks
  at **index 1**, which is typically the word's first vowel.

**So the residue is vowel selection, not structure.** Anchoring, left
context and consumption are not where the remaining 40% lives, and
further work should go at the vowel rules -- `9B` above all, at 183 uses
and almost always in a vowel context.

### $E6 cannot be determined from this corpus

Setting it to `true`, `later_vowel` or `later_vowel_noy` gives byte-for-
byte the same score on **both** corpora: 59.7% and 72.6%. Its 20 rules
either rarely fire or never discriminate here.

`{F3}AL{E6}L` looked like a clean handle -- it fires for ALLOW but not
ALL (nothing follows) or ALLY (only a Y follows), which reads exactly as
"a later vowel excluding Y". Implementing that changed nothing overall,
so it is recorded as an observation about those three words, not as the
meaning of `E6`. **A new corpus with the right words in it is needed.**

Likewise `FE` as `earlier_vowel_noy` and `FC` as `not_end` each gained
exactly one word and were **not adopted**.

## Nothing is satisfied by the input running out

The single biggest correction so far, and it undoes an inference made
earlier in this same note.

Attributing each failing word to the rule the *oracle* fired -- using
`ECHOTALK_READ_WATCH` and `PC=$E238` -- showed one pair dominating:
`{8E}G` should fire and `{8E}G{F5}` fired instead, 19 times. That is
`-NG` at the end of a word, where `F5` (a front vowel) has **nothing
after it at all**. The walker was letting it match because trailing
classes were treated as satisfied once the input ran out.

**That rule was never supported by the evidence.** It came from
`A{F4}{F8}{9B}` matching `AB`, but that works because `F8` matches zero
consonants and `9B` is zero-width -- neither ever needed exhaustion.
The `BO` test had already shown literals are not satisfied that way, and
classes are not either.

Removing it is worth **+7.0** on the story corpus and **+7.4** on
`reference_text`, the largest single gain in the whole exercise:

```
exhaustion satisfies (old)   story 59.7%   refs 72.6%
class needs a real char      story 66.7%   refs 80.0%
```

## The remaining failures are over-matching, not mis-ordering

After that fix, 157 of the 200 first-divergence cases have the **same**
consumption with the oracle's rule sitting **later** in the table, which
looks like proof the tiebreak is backwards. It is not:

| selection | story | refs |
|---|---|---|
| **longest consumed, tie to earliest** | **66.7%** | **80.0%** |
| longest consumed, tie to latest | 20.2% | 25.1% |
| first match | 56.7% | 74.9% |
| last match | 6.7% | 9.8% |

Preferring the later rule collapses the score. So the statistic is a
**symptom**: the walker matches rules it should not, and then correctly
picks the earliest of a set that wrongly includes them. The cause is a
class predicate that is still too permissive, not the tiebreak.

`9B` remains the prime suspect -- it appears in most of the
wrongly-fired rules (`I{9B}{F4}{F9}`, `{F3}{F8}{F4}A{9B}`,
`U{9B}{F4}{F4}`, `{F3}A{9B}`). The crisp open case is `LIKE`, where the
oracle fires `I{F8}{F7}` and the walker fires the earlier
`I{9B}{F4}{F9}`; every element of the latter is satisfied under the
current semantics, so one of them must be wrong. Note the apparent
contradiction to resolve: `9B` must be **true** at `BA` in `ABA`, where
`{F3}A{9B}` does fire, and **false** at `KE` in `LIKE` -- both are one
consonant followed by a vowel, so a simple span limit cannot separate
them, and the difference must lie elsewhere.

## $9B is the syllable boundary. The section below is SUPERSEDED.

**Read this first.** `$9B` is not a vowel lookahead at all. It is the
**syllable boundary** -- the same thing as `$E6`. The left-context
matcher at `$E380` loads `#$A6` for it and `$E383` adds `#$40`, giving
`$E6`, which is then compared against the buffer like any literal. The
right-context path at `$E3D6` does the same.

Using the real meaning is worth **+9.5 points on the story corpus and
+7.4 on `reference_text`** -- 72.5% to 82.0% and 81.9% to 89.3% -- and it
makes `TABLE` and `PEOPLE` come out exactly right, `0F 4E` and all.

The reading below was an **empirical approximation that correlated**: a
syllable boundary usually does precede a vowel, so "zero or more
consonants then a vowel" agreed with the truth often enough to look
right and to survive a fit over eleven candidates. It is left in place
because the observations in it are real and the reasoning shows how a
plausible model can be fitted, measured, refined, and still be wrong.

It also explains why wiring `$E6` into the walker changed nothing: **the
real consumer of syllable boundaries is `$9B`, at 183 uses, not `$E6` at
20.** The syllabifier was the missing piece all along; it was simply
feeding the wrong metacharacter.

### Superseded reading: a silent word-final E is not a vowel (doubly so; see the top)

The `LIKE` / `ABA` contradiction is solved, and the answer was not a span
limit.

Gathering positive and negative examples of one rule straight from the
oracle is what did it. `I{9B}{F4}{F9}` fires for exactly **DECIDED** and
**PRIDED** and for nothing else tried -- not LIKE, TIME, SMILE, DRIVE,
GUIDE, MILE, WINE, all of which have the same I + consonant + E shape.
The difference is that DECIDED and PRIDED **continue past the E**.

So `9B` is: at most two consonants, then a vowel, **where a word-final
E does not count**. A silent final E is not a syllable nucleus, which is
exactly the linguistic content of the rule.

It cannot be `F9` doing this work: `F9` matches a word-final E happily
in `{F3}{F8}I{F9}`, which fires for `TIE`.

That resolves the contradiction cleanly -- `9B` is true at `BA` in `ABA`
(a real final A) and false at `KE` in `LIKE` (a silent final E) -- and
both `LIKE` and `TIME` now match the oracle exactly.

Measured across every variant, on both corpora:

```
la2, silent-E excluded, satisfied at end   story 67.7%   refs 80.5%   <-- best
la2, silent-E counts                       story 66.7%   refs 80.0%
la1, silent-E excluded                     story 67.0%   refs 78.6%
unlimited span, silent-E counts            story 65.7%   refs 79.1%
la1, plain                                 story 63.0%   refs 74.0%
```

## The -ED ending, and why $9C matters more than its use count

Jayson supplied the phonetics that made this legible, and the traces
match his ear exactly:

- **HOPED** keeps the silent E; the D just takes a consonant sound.
- **NOTED** has no silent E -- the E takes the *i* of *fig*.
- **MAKER** has no silent E -- it takes the *er* of *barber*.
- The nonsense **MAKED** behaves exactly like HOPED.
- **NAKED** is mispronounced by 3.1.3 exactly like MAKED.

Every one of those is confirmed in the fired rules: NOTED's E gives
`27`, MAKER's gives `2E`, and NAKED's gives `7F`, silent, from the same
rule as MAKED.

### Answering "did an earlier rule win, or did rule 23 fail?"

Both, one each -- which is why the two had to be separated:

| word | rule fired for the E | index | verdict |
|---|---|---|---|
| MAKER | `ER` | 18 | **earlier rule wins**; rule 23 never got a turn |
| NOTED | `{FE}E{84}{F3}` | 33 | **rule 23 genuinely failed** |
| DECIDED | `{FE}E{84}{F3}` | 33 | rule 23 genuinely failed |
| HOPED / MAKED / NAKED | `{F7}{F8}{F4}E` | 23 | fires, silent E |

### What actually blocks it

Nonsense probes isolate it exactly:

```
HO?ED  fires rule 23 for  B C F G H J K L M N P Q V X Z
       fails         for  D T          (they get {FE}E{84}{F3} => 27)
HO?E   fires rule 23 for  B C D F G H J K L M N P Q S T V X Z
                          -- D and T included
```

So D and T are fine before a **final** E, and only fail when the word
ends `-ED`. That is precisely the English rule Jayson described: after T
or D the ending is a separate syllable, /Id/, and elsewhere it is just
/t/ or /d/.

**No rule earlier than 23 in section E accounts for it** -- rules 0-23
are listed above and none touches the case. So rule 23's behaviour
depends on material *after* its focus, which its pattern
`F7 F8 F4 C5` does not express under the current model. The parse is
byte-exact, so this is a gap in the semantics, not a mis-read.

### The lead: $9C

The same distinction shows up on the **D**, and there it is visible in
the pattern:

```
HOPED  [D] {F7}{F8}{F4}{85}D{F3}      => 63   /t/
HOTED  [D] {9C}{9B}{F8}{85}D{F3}      => 5F   /d/
```

`9C` is one of the three codes previously written off as "5 uses
between them, so they cannot be worth much". **That judgement was wrong
in kind:** frequency is not importance, and `9C` sits exactly on the
live question. Decode `9C` next -- it is the most likely key to the
whole `-ED` family, and quite possibly to why rule 23 fails.

## SUPERSEDED: $9C read as the coronal stops

**`$9C` is a wildcard** (`$E378`). The probe below is genuine but it
was measuring `$9B` next to it: `-ED` is its own syllable only after a
coronal stop, so D and T are the only members of the `HO?ED` family
with a boundary for `$9B` to find. The past-tense allomorphy described
here is real and Textalker really does implement it -- through the
syllabifier at `$DEF1`, not through `$9C`.

**`$9C` is D and T** -- exactly `FA`-intersect-`FD`, the coronal stops.
Probing `HO?ED` for the final D splits the alphabet cleanly across three
rules, and together they are the complete English past-tense rule:

```
{F6}{85}D{F3}          => 5F  /d/    after B G J L M N V Z   (F6 = voiced)
{F7}{F8}{F4}{85}D{F3}  => 63  /t/    after C F H K P Q X     (voiceless)
{9C}{9B}{F8}{85}D{F3}  => 5F  /d/    after D T, with the E spoken as /Id/
```

Textalker implements the whole alternation, which is more linguistics
than a 12 KB image suggests. Note the lesson: `9C` has a **single use**
in the table and was written off earlier in this session as too rare to
matter. It turned out to be the key to the entire `-ED` family.
Frequency is not importance.

## SUPERSEDED: rule 23 "depends on material its pattern does not mention"

The silent-E rule `{F7}{F8}{F4}E => 7F` at section E index 23 is now
characterised exactly, by varying the letter *after* the E:

```
HOPE?   fires for  D and S only     -- everything else -> E{F4}{F8}{9B} => 26
HOTE?   fires for  S only           -- D -> {FE}E{84}{F3} => 27
```

So it fires **iff**:

- the E is word-final (HOPE, MAKE, LIKE, and HOTE), **or**
- the E is followed by S (HOPES, HOTES), **or**
- the E is followed by D **and** the consonant before it is not D or T
  (HOPED, MAKED, NAKED -- but not HOTED, NOTED, DECIDED)

**Its pattern is `F7 F8 F4 C5` and says nothing whatever about what
follows the focus.** The parse is byte-exact and the neighbouring rules
were checked, so this is not a mis-read. `F4` is not the discriminator
either: it accepts T and D in both left and right context elsewhere, and
it accepts the T of HOTE when the E is final.

The most likely explanation is that the engine handles **inflectional
endings** outside the pattern language -- a trailing `-S` or `-ED` is
treated as not breaking word-finality -- with `9C`'s complement
governing the `-ED` case so that /Id/ words are excluded. That is a
hypothesis, not a result. It is the single most valuable thing left to
settle, since it is the last known place where the model is structurally
incomplete rather than merely mistuned.

## The inflectional-suffix hypothesis: falsified, and what replaced it

The hypothesis was that the engine treats a trailing `-S` or `-ED` as
not breaking word-finality, generally. **It is false**, and the test
that killed it also showed what really happens:

```
DOCTOR   {FE}OR{F3}          DOCTORS  {FE}OR{93}{F3}
IRON     O{8E}{F3}           IRONS    O{8E}{93}{F3}
PEOPLE   {FE}{F4}{8C}E{F3}   PEOPLES  {FE}{F4}{8C}E{93}{F3}
```

`$93` is `$80+19`, the **literal letter S**. The table simply carries
explicit `-S` variants of these rules; nothing is stripped. `DOCTORED`
and `DOCTORX` both fall through to the generic `OR` with no special
handling at all.

A second generalisation was tried and also failed: "a pattern ending
with its focus requires word-end". `EW` fires for `NEWT` and `NEWER`,
`ER` for `HERT` and `HERO`.

### SUPERSEDED: what looked like a hard-coded magic-E check

**There is no such check.** The section below is kept because the
measurements in it are sound and the conclusion drawn from them is a
clean example of the failure mode: a special case invented to explain
behaviour that had a mechanical cause. The cause is that a syllable
boundary is a real `$E6` byte which blocks every class -- HOPE is
`|HOPE|` but HOPESX is `|HO|PESX|`. See "What was wrong with the
model" at the top.

The behaviour belongs specifically to **silencing a final E**. Both
rules that do it behave identically, and neither says anything about
what follows the focus:

```
{F7}{F8}{F4}E => 7F     HOPE HOPES HOPED fire;  HOPESX HOPEDX HOPEST do not
{F4}{99}E     => 7F     BYE  BYES  BYED  fire;  BYEX   BYEST         do not
```

So a rule that silences a final E additionally requires the E to be
**word-final, allowing one trailing D or S** -- and, for the D, only
when the consonant before the E is **not a coronal stop**, since after D
or T the ending is its own syllable. That last clause is the same
distinction `$9C` draws in the `-ED` rules, arrived at independently.

Implemented as an explicit engine check rather than pattern semantics,
because that is what the evidence supports. Worth a lot:

```
before                     story 67.5%   refs 80.5%
word-final +D/+S           story 70.3%   refs 80.9%
plus the /Id/ exclusion    story 72.2%   refs 81.4%
```

`NOTED` -> `52 3B 63 27 5F` and `DECIDED` -> `5F 26 5B 20 5F 27 5F` now
match the oracle exactly.

## $FB and $FF, and the complete metacharacter table

**`$FB` is C G J S X Z** -- the letters that can yield a sibilant or
affricate. `{FB}O{FC}{8D}{F5} => 43` is the vowel of COME and SOME;
probing `?OME` fires it for exactly those six and gives every other
letter the long O of HOME.

**`$FF` is "at least two earlier vowels"** -- the third-syllable-or-later
test, which is where English reduces a vowel. It pairs with `$FE`, which
is at least one. Measured on `{FF}I{8E}{85}` with a controlled prefix
family:

```
0 or 1 vowel before the I:  MINE AMINE FAMINE BRAMINE        do not fire
2 vowels:                   ABAMINE OBAMINE TELAMINE
                            EXAMINE ADAMINE DETERMINE        all fire
```

The `FAVORITE` residual recorded here earlier is **resolved, and it was
not an engine mystery -- the walker was wrong.** Jayson supplied the
missing fact: 3.1.3 says *faiv-or-ite*, with the I of *bike*. The oracle
giving `3A` is that mispronunciation, and it is correct behaviour to
reproduce; the walker firing `{FF}{F6}I{94}{85}` for `27` is the bug.

`FF` itself is fine. Probing the rule with controlled prefixes fires it
for `ABANITE`, `FAVONITE` and `DETERNITE` (two vowels) and not for
`NITE` or `ANITE`. **`FAVONITE` fires it and `FAVORITE` does not**, and
they differ only in R versus N at the tested position -- where, in
`FAVORITE`, the `OR` rule has already **consumed both the O and the R**.

That was tested and the model it suggested was rejected -- see "Does
left context see consumed letters?" below. The observation holds; the
generalisation does not.

### Every metacharacter, decoded

| code | meaning |
|---|---|
| `$F3` | word boundary, zero width |
| `$F4` | exactly one consonant |
| `$F5` | one front vowel (E, I, Y) -- soft C and soft G |
| `$F6` | one voiced consonant (B D G J L M N R V W Z) |
| `$F7` | one vowel |
| `$F8` | zero or more consonants |
| `$F9` | an inflectional ending running to the word end: ER, ES, EST, ED, ELY, ING, or a bare final E, plus an optional S |
| `$FA` | **SUPERSEDED** -- attribute bit `$01`, or the C/S/T of CH, SH, TH |
| `$FB` | **SUPERSEDED** -- attribute bit `$10`, or the H of CH or SH |
| `$FD` | one stop (B C D G K P T) |
| `$FE` | zero width: the CURSOR's syllable counter `$FD93` is not 1 |
| `$FF` | zero width: the CURSOR's syllable counter is 3 or more |
| `$9B` | **SUPERSEDED** -- the sentinel `$E6`, a real byte, matched literally |
| `$9C` | **SUPERSEDED -- a wildcard.** See "What was wrong with the model" |
| `$FC` | zero width. `$E48F` skips a syllable-boundary sentinel if present and returns without consuming a letter, which is a no-op once boundaries are zero-width. Making it consume gives 62.7% |
| `$E6`, `$9B` | **a syllable boundary.** `$9B` is `$E6` after the `+$40` conversion; `$9B` is the common one at 183 uses |
| `$81-$9A` | context letters, `$80` + 1-based index, matched not consumed |
| `$A0-$E5` | literal characters, high-bit ASCII, focus, consumed |

## Does left context see consumed letters? The observation is real, the model is not

**The observation.** Three matched pairs, each differing only in whether
the consonant before the focus was consumed alone or inside a
multi-letter rule:

| word | pre-I consonant | consumed | `{FF}{F6}I{94}{85}` |
|---|---|---|---|
| BAKE**N**ITE | N | alone | fires |
| BAKE**R**ITE | R | inside `ER` | fails |
| FAVO**N**ITE | N | alone | fires |
| FAVO**R**ITE | R | inside `OR` | fails |
| ABA**N**ITE | N | alone | fires |
| ABA**R**ITE | R | inside `{F4}AR{FC}{F7}` | fails |

The R is present in the string every time and the matcher cannot see
it. That is solid and reproducible.

**The model built from it is wrong.** Implemented as "a multi-letter
rule hides all but its first letter from later left-context matching",
it fixes all six and costs far more elsewhere:

| model | story | refs |
|---|---|---|
| **no hiding** | **72.5%** | **81.9%** |
| hide from every multi-letter rule | 66.2% | 77.2% |
| hide from the immediately preceding rule only | 67.2% | 77.7% |

So `LEFT_HIDES` stays `"none"`. Whatever the engine does is narrower
than either model -- perhaps specific to certain rules, or an effect of
something other than visibility.

One implementation trap found on the way, worth knowing before retrying
this: passing a **shortened** string as the left-context view makes
every zero-width predicate believe it is at the end of the word, which
turns `$F3` into always-true and drops agreement to 12.8%. The view has
to stay the same length as the word, with hidden letters blanked, so
positions still line up.

This is the third locally-compelling explanation this session to fail
the corpus, after narrowing `9B` and after the "oracle picks a later
rule" statistic. The pattern is consistent enough to state plainly:
**an explanation that accounts perfectly for a handful of hand-picked
words is weak evidence. Only the corpus decides.**

## Checkpoint 3, the architectural version: the phoneme string is sufficient

Before building stage 2 it was worth knowing whether the phoneme string
**fully determines** the chip byte stream. If it does, byte-exactness at
checkpoint 1 implies byte-exactness at checkpoint 3, stage 2 can be
built independently, and the corpus score is a valid proxy. That was the
riskiest untested assumption in the whole plan.

Ctrl-V phoneme mode turned out not to help -- it bypasses the `$FEAD`
buffer entirely and is a separate entry point, so it cannot be fed the
internal codes. The test that works needs no new machinery: **find two
different texts that produce identical phoneme strings and compare their
chip bytes.**

```
v1.3    GHOTI vs FISH   identical
v3.1.3  FOUR/FOR  B/BE  O/OW  TWO/TOO  OUR/HOUR   all identical
```

Six for six, byte-identical chip streams from different input text. The
decomposition is sound.

**Caveat on the corpus score.** It is measured on isolated words with
separators stripped: of 600 words, **none** contains `B3` (stress), `34`
(sentence fall) or `19` (word separator). So 81.9% covers the rule
walker on bare words and says nothing about the stress pass, sentence
contours or multi-word behaviour.

## Disassembling the rule engine

Black-box probing hit clear diminishing returns -- three
locally-perfect explanations failed the corpus. `tools/disasm.py` does
recursive descent from the PCs the read-watch identified, which avoids
the drift into data that `notes/disasm_fd53.txt` records for a linear
pass. Full listing in **`notes/disasm_rule_engine.txt`**.

It settles several things that were inference:

- **The rule parse is right.** `$E33D-$E352` skips a rule by running
  over bytes `>= $80` and then bytes `< $80`, exactly the split this
  note derived. `$E236-$E23E` reads payload bytes until one has the high
  bit set.
- **The anchor is the first pattern byte in `$A0..$EF`** (`$E2F0-$E301`)
  -- the literal range. Note this is `$A0-$EF`, wider than the `$A0-$E5`
  the walker assumes.
- **`$C0` is a wildcard** matching any input character (`$E309-$E30B`),
  which was not known at all.
- **Context letters really are `$80` + index**: `$E383` does `ADC #$40`,
  turning `$81-$9A` into `$C1-$DA`.
- **Left context is backward from the cursor into the raw input buffer**
  at `$FDAC,X`, with no hiding of consumed letters -- consistent with
  the corpus rejecting the hiding model, and leaving the FAVORITE
  observation still unexplained.
- **`$F3` and `$F8` are skipped when the scan runs off the word start**
  (`$E35C-$E371`), which is word-boundary and zero-or-more confirmed
  from the code.
- **`$FFAC` is the phoneme buffer index** -- `$FEAD + $FF`, which is
  exactly the "unrelated counter at base+255" that once interleaved
  itself into the phoneme dump.
- `$9C` is handled specially before the `+$40` conversion, and `$9B`
  substitutes `$A6`. The class dispatch for bytes `>= $F3` is at
  `$E38C`. **That dispatch is where `FC` and `E6` will be settled** --
  read it next.

## The class dispatch, and the character attribute table at $EF24

Reading `$E38C` onward answered more than it was asked to.

**Dispatch.** Pattern bytes `>= $F3` reach `$E40D`, which does
`AND #$0F` and jumps through an RTS-trick table (low bytes at `$E41C`,
high at `$E429`):

```
$F3 -> $E439   $F4 -> $E450   $F5 -> $E458   $F6 -> $E460   $F7 -> $E468
$F8 -> $E47F   $F9 -> $E49E   $FA -> $E510   $FB -> $E53F   $FC -> $E48F
$FD -> $E569   $FE -> $E585   $FF -> $E590
```

**Direction is self-modifying code.** `$CA` is `DEX` and `$E8` is `INX`,
patched into `$E486`, `$E496`, `$E46F`, `$E440` and `$E48C` by `$E400`,
so left and right context share one matcher with the direction swapped.
`$FE` and `$FF` get an extra `INX`/`DEX` first, which is why they behave
as zero width.

### $E6 is a sentinel in the input buffer, not a class

It is below `$F3`, so it never reaches the dispatch at all. Watching the
buffer shows why -- the word is written, shifted right by one, and then
bracketed:

```
$FDAC..$FDAF <- C3 C1 D4 D3        "CATS"
              shifted right one
$FDAC <- E6                        sentinel before
$FDB1 <- E6                        sentinel after
```

The buffer is `E6 C A T S E6 00`. The `$F3` handler at `$E439` opens
with `CMP #$E6`, so the word-boundary test is literally "is the current
character the sentinel". **`$FC`'s handler at `$E48F` skips `$E6` and
then matches one character**, which is what lets a rule reach across a
word boundary -- so `FC` is not the no-op the corpus fit settled for.

**Solved: `$E6` is a syllable boundary.** The premise that the buffer
carries sentinels only at the ends was wrong -- it was drawn from
`CATS`, which happens to be a monosyllable. The routine at
`$E105`/`$E116` shifts the tail right and inserts a sentinel, and it
runs **once per boundary**, not twice per word. Watching the final
buffer layout, with `|` for a sentinel:

```
CATS     |CATS|        SCIENCE  |SCIENCE|      HOPED   |HOPED|
GHOTI    |GHO|TI|      NATION   |NA|TION|      ALLOW   |AL|LOW|
GEORGE   |GE|ORGE|     DOCTOR   |DOC|TOR|      PEOPLE  |PE|O|PLE|
```

Every monosyllable has no internal marker; every polysyllable is split
at its syllable boundaries -- DOC-TOR, AL-LOW, NA-TION, PE-O-PLE. So a
mid-pattern `$E6` matches a real byte after all, and the literal compare
was right all along.

This is also why the corpus fit found `E6` **inert under every
candidate**, giving byte-identical scores for `true`, `later_vowel` and
`later_vowel_noy`: the walker has no syllabification, so all 19 `E6`
rules were unmatchable whatever `E6` was taken to mean. The fit was
measuring nothing.

**Consequence for the rewrite: a syllabifier is a missing component**,
not a detail. It must run before the rules, and its output is part of
the matcher's input. Nothing in the walker models it yet, and those 19
rules include `GHO{E6}TI`, `{F3}GE{E6}ORGE{F3}`, `{F3}DR{E6}{AE}` and
`{F3}MRS{E6}{AE}` -- the easter egg, the proper nouns and the
abbreviations. Finding the algorithm at `$E105`/`$E116` is the next
task; the boundaries are also inserted progressively during processing,
so the layouts above are final states, not necessarily what the matcher
sees at every step.

### The attribute table: every class is one bit

`$E596` is a character classifier:

```
E59A: LDA $FDAC,X     ; the input character
E59D: AND #$7F        ; strip the high bit
E59F: TAY
E5A1: AND $EF24,Y     ; AND the caller's mask with the character's attributes
```

So **`$EF24` is a 128-byte attribute table indexed by ASCII**, and each
class is a single bit. `$F3` passes `#$02`, `$F4` passes `#$40`.
Decoding it:

| bit | letters | metacharacter |
|---|---|---|
| `$80` | AEIOUY | `$F7` vowel |
| `$40` | BCDFGHJKLMNPQRSTVWXZ | `$F4` consonant |
| `$20` | BDGJLMNRVWZ | `$F6` voiced |
| `$10` | CGJSXZ | `$FB` sibilant |
| `$04` | EIY | `$F5` front vowel |
| `$02` | (no letters) | `$F3` word boundary / separator |
| `$01` | DJLNRSTZ | `$FA` coronal |

**Six of the classes match the empirically probed sets exactly** --
vowel, consonant, voiced, sibilant, front vowel and coronal, every
letter agreeing. The probing method was sound; this simply replaces
inference with data, and the extractor should read `$EF24` rather than
carry hardcoded sets.

`$FD` (stops, BCDGKPT) and `$9C` (D and T) are **not** single bits in
this table, so they use some other test -- their handlers are at `$E569`
and in the pre-dispatch path, and can be read the same way.

## The syllabifier, located: $DD00-$E070

Two primitives do the mechanical work, and both are small:

- **`$E0C9` scans forward for the next vowel.** It calls the `$EF24`
  classifier and tests `AND #$80`, the vowel bit -- so the syllabifier
  and the rule matcher share one attribute table. Start in `$FDA6`,
  bound in `$FDA5`, result position in `$FDA7`, or 0 for none.
- **`$E0F8` inserts a sentinel at X.** It stores X in `$FDA3` and falls
  into `$E0FE`, which shifts the tail right one byte, writes `$E6`, and
  bumps the buffer end at `$FD9E`. It is fallen into, never `JSR`-ed,
  which is why searching for a call found nothing.

The decision logic sits on top:

- **`$DE4C` finds the next TWO vowels**, leaving their positions in
  `$FDAA` and `$FDAB`. Those are the nuclei either side of a candidate
  boundary, and the consonants between them are what the rules divide.
  This is the standard shape of a syllabifier.
- **`$DE7A` onward applies letter-specific tests** -- `CMP #$C5` is E,
  `CMP #$CC` is L, `CMP #$C4` is D. That is the final-E and `-LE`
  handling, and it is what makes `PEOPLE` come out `|PE|O|PLE|`.
- **There are eight `JSR $E0F8` sites** -- `$DD17`, `$DD57`, `$DD94`,
  `$DE0A`, `$DE45`, `$DF2A`, `$DFF9`, `$E066` -- so **eight distinct
  syllable rules**, each with its own conditions.

Workspace: `$FDA3` insert position, `$FDA5` scan limit, `$FDA6` scan
start, `$FDA7` scan result, `$FDA8` current, `$FDA9` end of word,
`$FDAA`/`$FDAB` the two vowels, `$FD9E` end of buffer, and `$FDAC`
onward the word itself.

### Ported: `tools/syllabify.py`, 93.6%

The port carries its own oracle -- boundaries read straight out of the
emulator's buffer -- so it is measurable in isolation, the same way the
rule walker is. Both tables are read from the user's image rather than
hardcoded. (This fork does not reproduce the lists that stood here;
`tools/syllabify.py` prints them from your own image, and they are in this
note on Jayson Smith's branch `Textalker-RW`.)

Agreement against the emulator:

| | 250 words | 700 words |
|---|---|---|
| first cut | 89.2% | |
| silent final E, optionally carrying D or S | 92.8% | |
| leading Y before a vowel is a consonant (`$DD23`) | 93.6% | |
| R and X split `VC\|V`, cluster scan for the rest | 96.4% | |
| `-ED` is a syllable only after T or D | **100.0%** | **97.9%** |

**The two rules that closed it both came from reading the 6502, and
both echo the rule engine.**

- **`$DFFD`: a lone consonant splits `V|CV` -- except R and X, which
  split `VC|V`.** `$E00B` is `CMP #$D2` and `$E00F` is `CMP #$D8`. That
  is `VERY -> VER|Y` and `MARGARET -> MAR|GAR|ET` against
  `SEVEN -> SE|VEN`. `$E019` then walks the cluster table forward from
  `v1+1` and splits at the first cluster reaching `v2`, giving
  `DEE|PLY` and `SHAR|PLY`, and falls back to leaving the last
  consonant with the vowel, giving `DOC|TOR`.
- **`-ED` and `-ES` are silent except after T or D**, where the ending
  is its own syllable:

```
splits:        SOR-TED  DILA-TED  DECI-DED  HAN-DED  REPEA-TED
does not:      SMELLED  LOOKED  OPENED  ENTERED  BELIEVED  CLUTCHED
```

**This is the same coronal-stop distinction `$9C` draws for the /Id/
past tense in the rule engine.** It has now turned up three times from
independent evidence -- in the `-ED` rules, in the magic-E check, and
here in the syllabifier -- which is about as much corroboration as this
kind of reverse engineering offers.

The `700` column is the honest one: 250 words were the tuning set and
reached 100%, and the remaining 450 were held out. The suffix minimum
length of 7 is the one fitted constant, chosen on all 700; its margin
over 0, 5, 6 or 8 is one or two words, so **treat it as unsupported**
rather than established. It is at least the same threshold the prefix
test uses explicitly at `$DD6C CMP #$07`.

**The silent-E condition is the same one the rule engine's magic-E check
needs** -- final E, optionally with an inflectional D or S. Measured
against the alternatives: 92.8% for that, 89.2% for "within two of the
end", 88.4% for "strictly final". The syllabifier and the matcher share
one notion of a silent final E, which is a good sign both readings are
right.

Removing the suffix boundary was tried and costs 4 points, so it stays.

### The 15 remaining failures, over 700 words

```
SHARPLY    |SHAR|PLY|      GOING    |GO|ING|     OUNCES  |OUN|CES|
ROCHELLE   |RO|CHELLE|     BEING    |BE|ING|     NURSES  |NUR|SES|
INTENTIONS |IN|TEN|TIONS|  PURELY   |PURE|LY|    PROBABLY |PRO|BA|BLY|
SNIFFLED   |SNIF|FLED|     EXHAUSTED |EX|HAU|STED|
```

Three groups, none mysterious:

1. **`-ING` and `-ES` want a boundary the port suppresses** -- `GOING`,
   `BEING`, `OUNCES`, `NURSES`. `-ING` is not in the suffix table at
   all, and now we know why: 3.1.3 does not say *ing*, it says *in*.
   See the section on that below.
2. **Plural `-TIONS`** -- the table has `TION` but not `TIONS`, and
   `INTENTIONS` and `ACCOMMODATIONS` both want the `TION` boundary with
   the S attached.
3. **Cluster-versus-suffix ties** -- `SHARPLY`, `PROBABLY`, `SNIFFLED`,
   `PURELY`, `EXHAUSTED`, where a cluster and a suffix both offer a
   boundary and the engine picks the other one.

**This is a transcription job rather than a discovery one.** The
structure, the primitives, the workspace and all eight rule sites are
identified; what remains is reading roughly 880 bytes of 6502 carefully
and porting each of the eight conditions. The full listing is in
`notes/disasm_rule_engine.txt`, which now covers 925 instructions from
40 seeds.

Build it as a separate pass that runs before the rule walker and emits
the sentinel positions, then let the corpus say whether it is right --
the `E6` rules become measurable for the first time once it exists.

## Wiring the boundaries in: correct, but no measured gain

`rule_walk.py` now calls the syllabifier per word and treats `$E6` as a
zero-width "there is a syllable boundary here" assertion. In the engine
the sentinel is a literal byte occupying a buffer position; keeping the
word's own indexing and making it zero-width is equivalent and avoids
reindexing every position.

**It did not move the corpus**: story 72.5%, `reference_text` 81.9%,
both unchanged, and 6 of 9 hand-picked `E6` words correct either way.

The reason is worth recording rather than glossing: **`E6 = true` was
already accidentally right** for most `E6` rules. `GHO{E6}TI`,
`{F3}GE{E6}ORGE{F3}` and `TI{E6}O` carry enough literal letters that
they only match the intended words anyway, so asserting nothing at the
sentinel position gave the same answer as asserting the truth. The gain
is correctness by construction, not accuracy -- and the earlier fit
result that `E6` was "inert under every candidate" is now explained
twice over: the rules were unmatchable *and* would have agreed anyway.

The three still-wrong words fail for an unrelated reason -- `PEOPLE`
wants `62 36 62 0F 4E 7F` and the walker gives `62 36 62 4E 7F`, missing
the `0F` schwa before the `L`, as do `TABLE` and `SCIENCE`. That is a
rule-selection problem in the `-LE` family, not anything to do with
syllables.

## -ING is not a syllable suffix because 3.1.3 does not say "ing"

From Jayson: 3.1.3 renders `-ING` closer to *in*. "Starting to
translate" comes out as roughly *"startin to translit"*, with
`translit` also being its own mispronunciation of *translate*.

That explains a loose end from the syllabifier work: `-ING` is absent
from the suffix table at `$E127` entirely. It is not treated as a
syllable-forming suffix because the engine never forms that syllable --
the letter-to-sound rules simply produce the *in* ending. So `GOING`
and `BEING` wanting a boundary the port suppresses is not a missing
suffix-table entry; it comes from somewhere else.

**Both are mispronunciations to reproduce, not to fix.**

## $F9 is an inflectional ending, and first-match is right after all

Chasing `20` where the engine gives `27` -- `SILENCE`, `PRESIDENT`,
`ANTISEPTIC`, `CENTIMETERS`, `CONFIDENCE` -- produced two corrections,
the second of which reverses an earlier conclusion.

### $F9 is not "the letter E"

The handler at `$E49E` recognises an **inflectional ending that runs to
the word end**: `E+R`, `E+S`, `E+ST`, `E+D`, `E+LY`, `I+NG` or a bare
final E, then an optional trailing S, then it requires the end of the
word or a boundary (`$E4EB`, `$E4FE`, and the `JSR $E439` at `$E504`).

The earlier reading came from probing `TIE`/`LIE`/`TIES`/`LIED`, all of
which happen to end in exactly those endings, so "the letter E" fitted
every observation. `SILENCE` is the discriminating case: `F9` lands on
the E of `-LENCE`, which is followed by N, so the rule fails and the
engine falls through to `I{F8}{9B} => 27`.

Worth **+2.3 / +0.9**.

### First-match beats longest-match, reversing an earlier finding

With `$F9` fixed, `TIE` and `LIE` broke: the engine fires the one-letter
`{F3}{F8}I{F9}` where the walker took the two-letter `IE`. Re-running
the selection comparison:

| selection | story | refs |
|---|---|---|
| longest consumed | 84.3% | 90.2% |
| **first in table order** | **87.5%** | **92.6%** |

**The earlier measurement that longest-match won by a wide margin was an
artifact.** It was taken while `$9B` was modelled as a vowel lookahead
rather than the syllable boundary, which made many rules fail spuriously
and biased the comparison toward whichever strategy tolerated that best.
With the semantics right, first-match -- the natural reading of an
ordered rule table -- is correct.

**The lesson is specific and worth carrying:** after fixing a semantic
error, re-test the structural assumptions that were measured while it
was in place. Two separate conclusions in this note were distorted by
the `$9B` mistake, and both had looked well-supported at the time.

The magic-E check survives re-testing and is still worth **+6.5 / +4.7**.

## FE, FF, F7 and FC settled from the code: nothing is fitted any more

**`$FD93` is a syllable counter**, and two metacharacters simply read
it:

```
$FE  $E585:  LDA $fd93 / CMP #$01   succeeds when the syllable is NOT 1
$FF  $E590:  LDA $fd93 / CMP #$03   succeeds when it is 3 or more
```

So `FE` is "not the first syllable" and `FF` is "the third syllable or
later" -- which is exactly what "at least one / at least two earlier
vowels" had been approximating. With a syllabifier in place the real
test is available.

**They measure identically to the approximations** -- 93.0% either way
on `reference_text`, and one word apart on the story. The corpus cannot
distinguish them. They are adopted because the code says so, not because
they score better, and that is the right reason.

Two more come out of the same pass:

- **`$F7` is one or MORE vowels**, not exactly one. `$E468` loops with
  `DEX` while the vowel bit is set and fails if it consumed none
  (`$E475`/`$E47A`), exactly as `$F8` loops for consonants but succeeds
  on zero. Worth **+0.6 / +0.4**.
- **`$FD` is a 7-byte literal list** at `$E57E`: `P T K B D G C`. The
  probed set was `BCDGKPT` -- the same seven letters.
- **`$FC` is confirmed as zero width.** `$E48F` steps over a syllable
  boundary if one is present and then joins `$E48C`, which undoes one
  step and succeeds. `$E48C` is one of the self-modified bytes, so it is
  `INX` going left and `DEX` going right -- an undo either way. In a
  model where boundaries are zero-width, that is precisely a no-op, and
  making it consume a character scores 62.7%.

### Every metacharacter, now read from the code

| code | meaning | how |
|---|---|---|
| `$F3` | word boundary | `$E439`, `CMP #$E6` then class bit `$02` |
| `$F4` | one consonant | `$E450`, mask `$40` |
| `$F5` | one front vowel | `$E458`, mask `$04` |
| `$F6` | one voiced consonant | `$E460`, mask `$20` |
| `$F7` | **one or more** vowels | `$E468`, mask `$80`, looping |
| `$F8` | zero or more consonants | `$E47F`, mask `$40`, looping |
| `$F9` | an inflectional ending to the word end | `$E49E` |
| `$FA` | one coronal | `$E510`, mask `$01` |
| `$FB` | one sibilant | `$E53F`, mask `$10` |
| `$FC` | zero width; steps over a boundary | `$E48F` |
| `$FD` | one stop, P T K B D G C | `$E569`, list at `$E57E` |
| `$FE` | not the first syllable | `$E585`, `$FD93` |
| `$FF` | the third syllable or later | `$E590`, `$FD93` |
| `$9B`, `$E6` | a syllable boundary | `$E380`/`$E383`, and the buffer sentinel |
| `$9C` | one coronal stop, D or T | pre-dispatch path |
| `$81-$9A` | context letters, `+$40` gives `$C1-$DA` | `$E383` |
| `$A0-$EF` | literal characters; `$C0` is a wildcard | `$E2F0`, `$E309` |

**`UNSETTLED` is now empty.** Nothing in the walker is a fitted
constant except the syllabifier's suffix minimum length, which is
flagged where it appears.

## The stress pass, ported: whole sentences now match

`$E2A2-$E2EF` turned out to be small and completely mechanical, not the
hard problem it was billed as:

```
E2A5  CMP #$2C ','  #$3B ';'  #$3F '?'   -> marker $B7
E2B1  CMP #$2E '.'  #$21 '!'  #$3A ':'   -> marker $B3
E2D2  shift each phoneme one place right, walking BACKWARD
E2D8  CMP #$4E / BCC -> insert the marker here
```

It maps the trailing punctuation to a marker, then walks backward from
the end of the phoneme string until it meets a code **below `$4E`** --
the vowel range, since `$4E` is L and everything above is a consonant --
and puts the marker in front of that last vowel. If it finds none, a
`$7F` lands at the start.

That is exactly the `51 24 63` -> `51 B3 24 63` back-patch that the
write-watch first showed as a mystery from `$E2D8` and `$E2EF`.

**Two bugs surfaced while verifying it**, both mine:

- **`$C0` is a wildcard and the walker was treating it as a literal.**
  The disassembly had already recorded this and it was never
  implemented. It matters because the single rule `{C0} => 34 34` serves
  `,` `!` `?` `;` and `:`, all of which share the section pointer
  `$FBF3`.
- **Characters sharing a section pointer collided.** `load_rules` keyed
  on the payload address, so `,`, `!` and `?` overwrote one another and
  only the last kept its rules.

With those fixed, whole sentences reproduce exactly:

```
MAT.     WAIT!     WHY?     HELLO,     CAT;     DOG:
THE CAT SAT ON THE MAT.
    51 24 63 ... 51 B3 24 63 ... 54 29 19 61 24 63 19 ... 34 34 1A
```

**7 of 7 byte-exact**, including word separators, the stress marker, the
sentence-final fall and the terminator. That sentence is the one this
note opens with, and it is the first complete sentence to match.

The letter corpora are unchanged at 88.3% and 93.0%, as expected --
they are single words with no punctuation, so they never exercised any
of this.

## NICKNAME: two silent-letter rules with nothing to stop them

Jayson's diagnosis was exactly right, and the traces show it:

```
v3.1.3  [C] C{FC}{8B}  => 7F     C silent before K   (the CK rule)
        [K] {9B}K{8E}  => 7F     K silent before N   (the KN rule)
        phonemes 52 27 7f 7f 52 39 51 7f   -- "ni-name", no K at all
v1.3    phonemes 52 27 61 52 1f 51 7f      -- "nick-name", the K survives
```

Both rules fire in 3.1.3 and nothing guards against the combination, so
the consonant disappears entirely. **This must be reproduced, not
fixed** -- it is one of the version differences that distinguishes the
two voices.

## SOLVED (was: the silent-E rule fires inconsistently)

The next dominant failure is the walker silencing an E that should be
spoken -- SEVEN, MARGARET, DECIDED, SECRETIVE and CENTIMETERS all lose a
pronounced `26`. The walker fires `{F7}{F8}{F4}E => 7F` (section E index
23) where the oracle fires `E{F4}{F8}{9B} => 26` (index 35). Both consume
one letter, so the tiebreak is table order and the walker's choice of the
earlier rule is right; **index 23 must genuinely fail** in those words.

Probing it for which words it fires in:

```
FIRES : LIKE TIME MAKE MAKES HOPE HOPED HOPES NOTE NOTES BIKE BIKES
        LIVE LIVED LIVES CAME NAME NAMED NAMES FILE FILED DECIDE
NOT   : SEVEN MARGARET MAKER NOTED CAMEL SEVENTY DECIDED
```

**HOPED fires and NOTED does not**, though both are vowel-consonant-E-D
and identical in every element the pattern tests. So is MAKES against
MAKER, and DECIDE against DECIDED.

**That check has now been done** -- see "The -ED ending" above. `MAKER`
is an earlier rule winning (`ER`, index 18) and `NOTED` is rule 23
genuinely failing (its rule is index 33). The caution stands as a
method note: this probe only asks whether a rule appears anywhere in a
word's fired list, so always compare indices before concluding
anything about semantics from it.

## The dispatch table, found: one array indexed by raw ASCII

**`entry(c) = $EFDF + 2 * ord(c)`.** Found by watching the lookup reads
rather than by searching: speaking `A` reads `$F061`/`$F062` and speaking
`5` reads `$F049`/`$F04A`, both at `PC=$E21E`/`$E223`, and
`$F061 - $F049` is 24, exactly twice `'A' - '5'`.

So **what this note previously called "the 26-entry letter table at
`$F061`" is that same array sliced from `'A'`.** There is no separate
letter table. Digits and punctuation have entries in it too, which is
how `69105` reaches the rules that spell the digit names.

The table occupies `$F01F-$F0AA` (chars `0x20` through `0x65`) and the
rules begin immediately after at `$F0AB`. Two consequences:

- **The rule area runs past `$FBF1`**, which this note had assumed was
  its end. `'.'` points to `$FC3B` and `','`/`'?'`/`'!'` share `$FBF3`.
  The real upper bound is about `$FCC4`, where code begins.
- Sections must be delimited by the **sorted** pointers, not by ASCII
  order, since the digits point into the tail past `Z`.

Reparsing this way gives **568 rules** rather than 525, and fixes a
silent corruption: `Z` previously absorbed everything from `$FACD` to
`$FBF1` and appeared to have 46 rules. It has **4**. The other 42 were
the digit and punctuation rules, which is why they were unreachable.

`69105` now walks to `5B 41 61 5B 52 3A 52 50 43 52 5B 57 2C 07 59 3A 55`
-- SIX NINE ONE ZERO FIVE -- byte-identical to the oracle.

### The trap in it

**The table stops at `0x65`.** Reading further treats rule bytes as
pointers: `'f'` (`0x66`) computes to `$F0AB`, the start of `A`'s rules,
and several lowercase codes then land *inside* `E`'s section and
truncate it from 60 rules to 10. That cost **33 points** of agreement --
story fell to 38.5% and refs to 59.1% -- and it is invisible unless you
compare per-section rule counts against the previous parse. Do that
after any change to the bounds.

With the range correct, agreement is back to 72.5% / 81.9% *and* digits
work.

### One real bug: the non-letter rules are mis-sectioned (FIXED)

42 rules have a first literal that is not their section's letter, and
all of them are the digit and punctuation rules -- `{B0}`-`{B9}`,
`{C0}` and friends. They sit in the tail of the rule area *after* Z, so
slicing sections from the 26-entry letter table sweeps them into
section Z.

They are harmless for alphabetic text, since a rule whose first literal
is `'1'` can never match a letter, but they are **unreachable for the
digits and punctuation they exist to serve**. The dispatch table that
should reach them has not been identified: the 48-entry pointer table at
`$F001` points into `$FBF1-$FCFF`, past the end of the letter rules, so
it is something else again. Needed before the walker can read `69105`.

## State and next steps

`tools/rule_probe.py` reports which rule fires for each letter of a
word, and `--family "V?"` groups the alphabet by which rule fired. It is
the instrument the metacharacter decode runs on.

`render_text_loader` gained `ECHOTALK_PHONEME_DUMP` and
`ECHOTALK_RAM_WATCH=<lo>-<hi>`. Both are diagnostic, both are off unless
set, and the `hi_only` canary was re-verified at **2248 samples,
-12127/+26833** after every change.

0. **Fold the disassembly into the walker.** In rough order of value:
   read `$EF24` for the classes instead of hardcoding them; widen the
   literal range to `$A0-$EF`; add `$C0` as a wildcard; model `$E6` as
   the buffer sentinel and `$FC` as "skip sentinels, match one"; then
   read the handlers for `$FD`, `$9C`, `$F8`, `$F9`, `$FE`, `$FF` at the
   addresses tabulated above. Re-measure after each -- the disassembly
   says what the engine does, but only the corpus says whether the
   walker now does it too.
   **Every metacharacter is now read from the code**; `UNSETTLED` is
   empty and nothing in the walker is fitted. The stress pass is ported
   too, and whole sentences match byte-exactly. So the remaining ~7%
   and ~12% on single words is not missing vocabulary: work the
   divergence list, and look for anything the engine does outside the
   pattern language, the way the magic-E check turned out to. And
   **re-test structural assumptions after any semantic fix** -- the
   `$9B` error distorted both the selection strategy and the `$F9`
   reading, and both had looked well-supported.
1. **Raise agreement from 93.0% / 88.3%.** In priority order, based on
   **Every metacharacter is now decoded except `FC` and `E6`**, which
   two fits could not improve on and which `E6` in particular seems
   genuinely inert -- see the table above. So the remaining ~18%/27%
   is no longer missing vocabulary; it is either the engine doing
   things outside the pattern language, as the magic-E check turned
   out to be, or the non-letter sections that are still unreachable. Work from the divergence list as before:
   attribute a failing word to the rule the oracle fired, then gather
   positive and negative examples of that one rule. Every real advance
   in this session came from that loop, and none from guessing.
   The method that has worked every time: attribute a failing word to
   the rule the oracle actually fired with `ECHOTALK_READ_WATCH`, then
   gather positive and negative examples of that one rule. It found the
   exhaustion bug and the silent-E meaning of `9B`; guessing found
   neither. `FC` and `E6`
   remain no-ops and the fit cannot improve on that, which may mean
   they really are inert for matching and do something else entirely.
   Optimise against the story corpus, and check `reference_text` as a
   held-out set.
2. **Done** -- see "The dispatch table, found". Digits and punctuation
   are reachable and `69105` matches the oracle exactly.
3. Then port the settled walker to C.
4. Confirm the allophone table at `$EB00-$EE00` via Ctrl-V phoneme mode.
5. Write the extractor that reads both tables out of a user-supplied
   image, and a C walker for the rules; diff against checkpoint 1 over a
   large word corpus.
6. Characterise the stress pass at `$E2D8`/`$E2EF`.

## The two versions have genuinely different rules

From Jayson, who knows the two voices by ear. This is not a defect to be
fixed in either direction -- **both rule sets must be reproduced as they
are**, and the differences are useful regression cases precisely because
they are known and audible:

- **3.1.3 is generally the better rule set**, but not always.
- *copy* -- **1.3 is right**. 3.1.3 replaces the vowel with the O of
  *boat*.
- *begin* -- **3.1.3 is closer**: it says *begin* properly except that
  the E is the E of *bed*. 1.3 says *bih-jin*.
- The two also have **different intonation rules**, which is consistent
  with the stress pass being version-specific code rather than table.

These are the first entries in the differential corpus: any C walker
must reproduce each version's own wrongness exactly, including the
`copy` and `begin` cases above.

**Measured, and it matches the description by ear exactly** -- which is
a good independent check on the whole oracle chain:

```
COPY    v3.1.3: 61 3b 62 1c        v1.3: 61 1b 62 1c
BEGIN   v3.1.3: 5e 26 60 27 52     v1.3: 5e 0d 65 27 52
```

- `COPY` differs in the vowel alone. `1b` is the same code seen in `ON`,
  so 1.3 uses the short O; 3.1.3 uses `3b`, the O of *boat*.
- `BEGIN` in 3.1.3 uses `26`, the vowel of *bed*, exactly as reported.
- `BEGIN` in 1.3 emits `65` where 3.1.3 emits `60`. `65` is **J** -- it
  is the payload of the `{F3}{F8}J{F8}{F3} -> 65 1F` rule, "JAY". So 1.3
  substitutes J for G outright, which is the *bih-jin* being heard.

Phoneme codes identified so far, from these and the earlier probes:

| code | sound | code | sound |
|---|---|---|---|
| `1B` | short O (*on*, *cop*) | `5E` | B |
| `1C` | EE | `60` | G |
| `1F` | AY (long A) | `61` | K |
| `24` | short A (*cat*) | `62` | P |
| `26` | EH (*bed*) | `63` | T |
| `29` | schwa | `65` | J |
| `3B` | long O (*boat*) | `5A` | H |
| `27` | short I (3.1.3) | `52` | N |
| `0D` | short I (1.3) | `54` | TH |
| `19` | word sep (3.1.3) | `7F` | word sep (1.3) |
| `1A` | end of string | `B3` | stress marker |
