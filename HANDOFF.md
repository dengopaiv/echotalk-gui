# EchoTalk project handoff

Rewritten at the end of session 11, when the project reached the point
where everything the NVDA backend needs exists and works. Read this
first; `notes/` holds the detailed writeups and `notes/README.md` indexes
them.

## Read this first: who you're working with

You are working with **Jayson Smith** (he/him). He is **blind and uses a
screen reader**. MAME's UI, including its interactive debugger, is not
accessible to him, so **never rely on a GUI or an interactive debugger
for anything** — any verification he has to perform must come out as
plain text, or as audio he can listen to.

He is an excellent collaborator and technically sharp. Two things about
how he works are worth knowing because they have repeatedly changed the
outcome:

- **He catches real bugs by ear.** The single-character "return" bug and
  the speech-pacing defect were both found that way, the second after
  five sessions of automated measurement said everything was fine.
- **He supplies discriminating evidence, often unprompted.** The two
  hardest bugs in session 11 were solved by things he volunteered: "I
  can't reproduce it by setting the clock rate to 0%" killed a wrong
  theory instantly and pointed at the step budget, and an NVDA log
  settled the Say All question after two plausible theories had failed.

When he says something sounds wrong, believe him and investigate. When a
theory can be tested cheaply from his side, ask — it beats reasoning.

## Project goal

A C library (Windows DLL, Linux `.so`) that emulates the Apple II Echo II
speech card (TMS5220) running Street Electronics' Textalker, for use as
an NVDA speech backend. It runs the **real Textalker binary** under a
6502 emulator driving a port of MAME's TMS5220. Nothing about the speech
is reimplemented or approximated, and no Apple ROM or DOS code is used.

## Current state: complete and working

**Branch `Textalker-RW` adds a second engine, and everything it was
for.** The library runs either the 6502 emulation (the default,
unchanged and bit-exact) or a C rewrite of Textalker's whole speech
path; `echotalk_set_engine()` chooses and ABI is 12. The rewrite is
byte-exact at the chip bytes for **all four known Textalker builds**,
and on top of it sit the three things the rewrite existed to make
possible: a pluggable letter-to-sound source with a CMUdict-format
dictionary, a much longer chunk window, and an optional extended line
buffer. Its AUDIO is not phase-identical to the emulation -- see the
branch section below for how far that got and why it stopped.

Two more things landed on this branch that are not about the engine at
all: **Apple II disk images are read directly** (`tk_extract`, and the
library opens one itself), and **the C engine finds its tables in the
image** instead of holding a list of addresses, which is what lets it
read builds nobody here has catalogued.

There are **no known open bugs**. In place and tested:

- Emulation validated against a real-hardware capture and against MAME.
- Both Textalker **3.1.3 and 1.3** run from one binary.
- The library, `src/echotalk.[ch]`, with the full settings surface.
- **Windows DLLs** (32- and 64-bit) and a **Linux `.so`**, ABI 12,
  56 exports.
- **Streaming**: `speak()` queues and returns; `read()` synthesises.
- **Index events** for NVDA progress reporting.
- **An NVDA add-on**, `nvda-addon/`, working in real NVDA.
- Output is **bit-identical between Windows and Linux**.

What has *not* been tested is in "Untested ground" below. Read it before
claiming the project is finished.

## Repository layout

```
echotalk/
  third_party/
    fake6502/fake6502.c    public-domain 6502 core (Mike Chambers)
    tms5220_core/          our standalone C port of MAME's TMS5220.
                           This is what ships. BSD-3-Clause.
    tms5220/               UNMODIFIED MAME source, reference only, not
                           compiled. Diff against this when anything
                           looks wrong — it has paid off four times.
  roms/                    Textalker images, user-supplied, proprietary.
  src/
    echotalk.c/.h          THE LIBRARY. Public API, documented in the
                           header, which is the API reference.
    dsk.c/.h               a DOS 3.3 disk-image reader, in C
    tk_disk.c/.h           finds Textalker on one, structurally
    chunker.c/.h           splits text to fit Textalker's buffer
    text_prep.c/.h         modern text -> 7-bit ASCII, LF stripping
    resample.c/.h          linear resampling, phase carried across calls
  tools/
    say.c                  the CLI; drives the library as a host would
    render_text_loader.c   THE canonical harness, both Textalker
                           versions, and what the baselines are measured
                           with
    render_common.h        shared CLI, trimming, WAV output, chunking
    test_dll.py            loads the DLL via ctypes, as NVDA does
    test_dll_load.c        the same via GetProcAddress, so the 32-bit
                           DLL gets tested too
    test_nvda_driver.py    runs the NVDA driver with NVDA stubbed out
    listen_check.py        writes a self-narrating WAV to check by ear
    wav_stats.c            onset/duration/clipping measurement
    rule_probe.py          which letter-to-sound rule fires for each
                           letter of a word; the instrument the C
                           rewrite's metacharacter decode runs on
    seam_diff.py           the letter-to-sound seam: that a source
                           declining a word costs nothing, and that
                           ARPABET round-trips through tk_arpabet.c
    tk_extract.c           Textalker off an Apple II disk image
    tk_profile.c           what the engine FOUND in an image; run this
                           first on a build nobody has seen
    test_text_prep.c, test_chunker.c, test_dsk.c   pure-logic tests,
                           no ROM needed
    render_v13.c, render_text_real_chip.c   kept as independent
                           cross-checks; the latter shows the onset
                           glitch on purpose
    (probe_*, render_from_bytes, render_trace, render_hi_real_chip,
     boot_and_probe, render_wav, resample_wav are historical probes.
     Safe to ignore, and safe to delete.)
  nvda-addon/              the NVDA add-on; see its own README
  examples/
    pronunciations.dict    a worked pronunciation dictionary: the
                           mispronunciations listed below, fixed, in all
                           three spellings the format accepts
  tmp/                     UNTRACKED scratch. Jayson keeps his disk
                           images and the nonsense-word corpus here, and
                           it is where any WAV he is meant to LISTEN to
                           should be written -- out/ is scratch too and
                           he has to hunt for things there. Nothing here
                           is in the repository, so do not cite a path
                           under it as evidence in a document.
  notes/                   investigation writeups; notes/README.md
                           indexes them and flags the superseded ones
```

**What is NOT in the repository**, and so must never be cited as though
a reader could open it: `roms/*.bin` (the Textalker images -- users
extract their own, see README), `tmp/` and `out/` entirely, every
`*.wav`, `build/`, and the add-on's staged binaries. Jayson's disk
images and the nonsense-word story corpus are his, not the project's.
A document may say WHAT was measured against them; it must not point at
a path. `reference_text/*.bin` and
`Rubber Baby Buggy Bumper Test/test.bin` ARE tracked -- the `.wav`
files beside the latter are not.

## Building

Toolchain is MSYS2. **The `gcc` on PATH is an old Cygwin install and
must not be used.**

```
make native          # host build
make win64 / win32   # 64-bit UCRT, 32-bit MSVCRT
make dll             # echotalk.dll, both architectures, + import libs
make so              # Linux/macOS libechotalk.so
make test            # pure-logic unit tests, no ROMs needed
                     # (text prep, the chunker, the DOS 3.3 reader)
make test-dll        # load the 64-bit DLL from Python, as NVDA does
make test-dll-load   # the same from C, BOTH architectures
make listen          # writes a self-narrating WAV to check by ear
```

Either Windows target builds from any MSYS2 shell: the Makefile finds the
right compiler by absolute path and verifies it with `gcc -dumpmachine`,
refusing to build rather than producing a mislabeled binary.
`notes/mingw_build_system.md`

**Trap when building from Git Bash or Cygwin rather than an MSYS2
shell.** Git for Windows sets `MSYSTEM=MINGW64`, so the Makefile
concludes it is already in the right subsystem and reaches for the bare
`gcc` — the Cygwin one. The architecture guard catches it, so nothing
mislabeled is produced.

**Naming the compiler by path is NOT enough**, and this is the part that
bites: an MSYS2 gcc invoked from a Cygwin shell cannot find its own
support DLLs and dies in a pile of loader error dialogs. What works is
to run MSYS2's own bash with the subsystem set — what
`s:\msys\ucrt64.exe` does interactively, and this does in a script:

```
MSYSTEM=UCRT64  CHERE_INVOKING=1 /s/msys/usr/bin/bash.exe -lc 'cd "$(cygpath X:/echotalk)"; make win64-dll'
MSYSTEM=MINGW32 CHERE_INVOKING=1 /s/msys/usr/bin/bash.exe -lc 'cd "$(cygpath X:/echotalk)"; make win32-dll'
```

Inside that shell plain `gcc` is already the right one, so no `CC_WIN64`
override is needed.

## Running

```
say [options] <loader.bin> <obj.bin> [text] <out.wav>
  --file PATH   read the text from a file (omit the text argument)
  --speed MULT  0.25-4.0, speed only, pitch unchanged — the one to use
  --rate HZ --clock MULT --frame-rate N --compressed
  --pitch N --volume N --word-delay N --repeat-filter N
  --chunk N --no-chunk --raw
```

```
render_text_loader [options] <loader.bin> <obj.bin> <input.bin> <out.wav>
```

The harness takes raw bytes — Echo control codes and text, high bit NOT
set; it sets it. Options: `-v`, `--no-trim`, `--keep-chunk-gaps`,
`--no-repeat-fix`, `--raw`, `--chunk N`, `--no-chunk`. Diagnostic
environment variables: `ECHOTALK_CHIP_TRACE`, `ECHOTALK_RESETL4`,
`ECHOTALK_BYTE_DUMP`, `ECHOTALK_ARRIVAL`, `ECHOTALK_POLL_TRACE`,
`ECHOTALK_BANK_TRACE`, `ECHOTALK_PC_WATCH`, `ECHOTALK_PHASE`,
`ECHOTALK_CPU_HZ`, `ECHOTALK_TRUE_TIMING`, `ECHOTALK_INSTANT`,
`ECHOTALK_PHONEME_DUMP`, `ECHOTALK_RAM_WATCH`, `ECHOTALK_READ_WATCH`,
`ECHOTALK_CYCLE_DUMP`, `ECHOTALK_CYCLE_PROFILE`.

The last five were added for the C rewrite on branch `Textalker-RW`.
`ECHOTALK_PHONEME_DUMP=<file>` writes the one-byte phoneme string
Textalker builds before any LPC frame; `ECHOTALK_RAM_WATCH=<lo>-<hi>`
logs writes into a hex address range with the PC that issued them; and
`ECHOTALK_READ_WATCH=<lo>-<hi>` does the same for reads, which is how
the letter-to-sound rule that actually fired is identified (its payload
is fetched at `PC=$E238`).

`ECHOTALK_CYCLE_DUMP=<file>` writes the 6502 cycle count at every chip
write, and `ECHOTALK_CYCLE_PROFILE=1` reports cycles spent per 256-byte
page at exit. Those two are how the native engine's cycle model was
aimed and checked, and they are the tools to reach for before assuming
where Textalker spends its time -- the answer (65% in the status polls
and the chip write, 26% in the bit packer, 3% in stage 1) is not what
anyone would guess.

The LIBRARY has two of its own, both about the native engine:
`ECHOTALK_CHIP_BYTES=<file>` logs every byte EITHER engine hands the
chip together with the audio sample it arrived at, the FIFO depth and
the chip's interpolation phase -- so the two can be diffed directly --
and `ECHOTALK_TK_TRACE=<file>` logs the native engine's waits and
whether each SPEAK EXTERNAL went out or was skipped. Both found bugs
that reasoning had not.

All are off unless set. See `notes/textalker_rewrite_table_discovery.md`.

## Branch `Textalker-RW`: the C rewrite (sessions 12-19)

Replacing the 6502 emulation with a native implementation of the
text-to-phoneme stage. **`notes/textalker_rewrite_table_discovery.md` is
the full record** and `notes/disasm_rule_engine.txt` is the annotated
disassembly. This is a summary only.

Two decisions shape everything: **byte-exact against the emulation is
the acceptance test**, and **tables keep being extracted from the user's
own images** so the legal stance is unchanged.

### Where it stands: both stages are done

Text goes in and the exact byte stream Textalker writes to the TMS5220
comes out, with no 6502 anywhere in it.

**Both versions agree on every one of these**, at the chip bytes:

| | agreement, each version |
|---|---|
| whole lines, text -> **chip bytes** | **679 of 679** |
| random text through both stages, 4 seeds | **900 of 900** |
| Ctrl-V phoneme mode, 5 seeds | **1300 of 1300** |
| random Ctrl-E command streams, 9 seeds | **1080 of 1080** |

Those are checkpoint 3, the end of the pipeline. The chip emulation
below it is already validated against real hardware, so byte-exactness
here means the native path is indistinguishable from the emulation.

### Stage 1 in detail

| | agreement |
|---|---|
| `reference_text`, single words | **215 of 215** |
| the nonsense story, every distinct word | **1039 of 1039** |
| random words, four seeds, to 18 letters | **4400 of 4400** |
| whole lines with punctuation | **870 of 870** |
| the syllabifier alone | **700 of 700** |

Nothing is fitted. Every metacharacter, every table address and every
special case is read from the 6502; there is no tuned constant left
anywhere in stage 1. `tools/rule_fit.py`, which searched candidate
semantics against the corpus, has been deleted -- it had nothing left
to fit.

### Stage 2: phoneme codes -> TMS5220 bytes

`tools/allophone.py`, a transcription of `$DBD6-$EA43`. The synthesiser
is the same shape as the rule engine -- a small interpreter over packed
tables -- so the same method worked.

**The frame format.** An allophone is a bitmask followed by packed
frames. A frame is six bytes when bit 7 of its first byte is set
(voiced: energy, pitch and K1-K10) and three when it is clear (unvoiced:
energy and K1-K4 only, which is exactly what the TMS5220 wants). The
mask picks which of ten frames to emit, so one allophone's data serves
several durations, and the 26 low allophones carry **three masks, one
per pitch group** -- which is where the intonation lives. The pitch is a
running value, not a per-frame one.

**The tables**, all read from the user's own image:

```
$EA44  Ctrl-V character map, entry(c) = $EA24 + ord(c). 55 live
       characters, not the 51 in the random-phoneme program: ',' is a
       pause, '/' an allophone, and '+' and '-' step the energy.
$EA7F  pitch offset per group: FE 00 02, i.e. -2, 0, +2
$EA89  vowel variant per group      $EA93  58 allophone pointers
$E906  the ten K widths: 5 5 4 4 4 4 4 3 3 3, the TMS5220 frame
```

`$EA7F` confirms Jayson's ear exactly. `$E733` subtracts 5 from a pitch
value, so an offset of -2, 0, +2 is 3, 5 and 7 on the 1-9 scale -- the
three pitches he identified text as using, with 5 as the monotone
default. `<` and `>` set a per-frame glide of -1 and +1 (`$E6ED`), and
`=` cancels it.

**Five things had to be read rather than guessed**, and each was worth a
failing family:

- `$E982` is `EOR #$FF` then `ADC #$41` with the carry clear, which is
  `$40 - a`, not `$41 - a`. Textalker holds a period; the chip wants its
  complement.
- `$E7FE-$E804` is `CLC, ADC $FFC3, ADC $FFC2`, and the second `ADC`
  takes the first one's carry -- so a `-` borrows and immediately gives
  the borrow back.
- `$E5CA` branches straight to the `RTS` when `$E6B0` handled the code,
  skipping the `$E5D5` re-entry. That is why a lone `,` swallows the
  rest of the line.
- **Phoneme mode runs one character behind.** `$E623` holds a vowel or a
  stop until the next character arrives, because `$E674` needs to know
  what follows before it can pick a stop's release variant. The held
  sound is spoken on the next character and `$E5DF` then re-enters for
  that character.
- **`$E756` indexes `$EA89` with a stale X register.** `$E713` only does
  `LDX $FFBC` on two of its three paths, and `$E9AF` always returns with
  X at zero, so the vowel variant depends on what ran last. After `1N`
  it reads allophone data instead of the table. The X register has to be
  modelled to reproduce it.

### What turned stage 1 from 85% into 100%

Two readings were wrong, and both were *models* standing in for the
machine.

**`$9C` is a wildcard, not the coronal stops.** `$E378` does `CMP #$9C`
and branches straight to the match continuation without comparing
anything, exactly as `$E309` does for `$C0` in the forward direction.
The `HO?ED` probe that said "D and T" was really measuring `$9B` beside
it: `-ED` is its own syllable only after a coronal stop, so those are
the only members of that family with a boundary for `$9B` to find.

**A syllable boundary is a real `$E6` byte in the buffer**, not a
zero-width assertion over the input string. `$E6` masks to `$66`, whose
attribute at `$EF24` is `$00` -- neither vowel nor consonant nor
separator -- so a sentinel *blocks every class*. That is the whole of
the hand-written "magic E" special case, which is now deleted: HOPE is
`|HOPE|` and its `{F7}` reaches the O, while HOPESX is `|HO|PESX|` and
the same `{F7}` runs into the sentinel and fails.

So both tools were rewritten as transcriptions of the 6502 working on
the same buffer it works on, rather than models of what it seemed to
do. The lesson is in "Lessons that cost time" below.

### The metacharacters, all from the code

```
$C1-$DA  focus letter, matched AND consumed
$81-$9A  context letter, $80 + index; $E383/$E3D9 do ADC #$40
$C0      wildcard inside the literal run                    $E309
$9C      wildcard in either context                         $E378 $E3CE
$9B      the syllable sentinel: $A6 + $40 = $E6             $E380 $E3D6
$E6      the sentinel again, as a literal in the $A0-$EF range
$F3      skip one sentinel, then end-of-buffer or attr $02  $E439
$F4      attribute bit $40 -- consonant                     $E450
$F5      attribute bit $04 -- E, I, Y                       $E458
$F6      attribute bit $20 -- B D G J L M N R V W Z         $E460
$F7      one or more of bit $80 -- vowel, GREEDY            $E468
$F8      zero or more of bit $40 -- consonant, GREEDY       $E47F
$F9      an inflectional ending running to the word end     $E49E
$FA      attribute bit $01, or the C/S/T of CH, SH, TH      $E510
$FB      attribute bit $10, or the H of CH or SH            $E53F
$FC      skip zero or more sentinels; always succeeds       $E48F
$FD      one of P T K B D G C, from the table at $E57E      $E569
$FE      zero width: the syllable counter $FD93 is not 1    $E585
$FF      zero width: the syllable counter is 3 or more      $E590
```

`$F7` and `$F8` are greedy with **no backtracking**: the loop runs to
the end of the class and steps back exactly one. `$FE` and `$FF` are
zero width because `$E3A2` and `$E3F9` undo the caller's step before
dispatching. The syllable counter belongs to the **cursor**, not to the
position being matched, and `$E266` resets it at every non-alphanumeric.

### The buffer is the whole line

`$FDAC` does not hold one word. `$DB5E` copies a word in, the
syllabifier brackets it in place, then the next separator and word are
appended after it -- so `THE CAT.` lives in one buffer as
`|THE| |CAT|.` and the `.` sits beside CAT, which is what lets
`{F3}DR{E6}{AE}` match `DR.` across the trailing sentinel. A word is a
run of letters, digits and apostrophes; measured against the emulator's
own buffer, `AB1CD` and `AB'CD` are one word each while `AB-CD`,
`AB@CD`, `AB/CD` and `AB$CD` are three.

The stress pass runs when the cursor **reaches** one of `, ; ? . ! :`,
before that character is matched -- which is why `DR.` carries no
intonation marker at all: the abbreviation rule swallows the stop, so
the cursor never gets to it.

**A word's position in the line is not neutral.** `$DF3A` does
`CPX #$01`, an absolute buffer index, so the "vowel W vowel" syllable
rule at `$DF35` is disabled only for the word standing first. `OWETEO`
alone is `|O|WE|TE|O|` and gives `21 50 26 63 36 21`; in `AAA OWETEO`
it is `|OW|E|TE|O|` and gives `3C 26 63 36 21`. Any differential test
must therefore give the walker the same *line* the emulator gets, not
the same word -- `rule_walk.diff` does, and compares the whole line
before splitting on `$19` to attribute a failure.

### The architecture is validated

The phoneme string **fully determines** the chip byte stream: six pairs
of different words with identical phoneme strings (GHOTI/FISH under
v1.3, FOUR/FOR, B/BE, O/OW, TWO/TOO, OUR/HOUR) give byte-identical chip
output. So byte-exactness at the phoneme level implies it at the chip
level, and stage 2 can be built independently.

### Tools

```
tools/syllabify.py    the syllabifier, $DD10-$E126, with its own oracle
tools/rule_walk.py    the rule engine, $E1FA-$E5A9; walk() and walk_line()
tools/allophone.py    the synthesiser, $DBD6-$EA43; --phonemes/--codes/--text
tools/rule_probe.py   which rule the EMULATOR fires for each letter
tools/line_diff.py    whole lines against the emulator, at the phonemes
tools/textalker.py    the whole thing: raw input bytes -> chip bytes
tools/byte_diff.py    both stages against the emulator, at the CHIP BYTES
tools/rule_fuzz.py    random words against the emulator
tools/tk_phonemes.c   the C port's stage 1, on the command line
tools/tk_chip.c       the C port end to end: text, Ctrl-V strings, code
                      lists or raw input streams -> chip bytes
tools/tk_diff.py      the C port against the Python or the emulator, at
                      the phonemes or the chip bytes
tools/engine_diff.py  the two ENGINES against each other, at the audio
tools/seam_diff.py    the letter-to-sound seam: that declining a word
                      costs nothing, and that ARPABET round-trips
tools/tk_profile.c    the addresses the engine discovered in an image
tools/frame_diff.py   unpacked LPC frames, emulator vs port, by allophone
tools/profiles.py     where everything lives in each image, and detection
tools/disasm.py       recursive-descent 6502 disassembler
```

Every one of them takes `--version` (`rule_fuzz.py` spells it
`--version=1.3`, since it does not use argparse).

`tools/disasm.py` needs the **default** Python -- prepending the MSYS2
bin to PATH for a build shadows the interpreter that has py65.

`line_diff.py` compares only **single-chunk** input. Textalker breaks
long text into chunks of its own, terminating each with `$1A` and
dropping the space it broke at; that chunk-size limit is one of the
things this rewrite exists to remove, so modelling it would measure the
wrong thing.

`rule_fuzz.py` retries, one word at a time, any batch the emulator
broke into chunks of its own -- otherwise a parse failure quietly hides
a real disagreement. It hid 78 of 1200 the first time it ran. It is
also what caught `$DF3A`: three words in 1500 disagreed, and they were
right to.

New harness diagnostics, all off unless set: `ECHOTALK_PHONEME_DUMP`,
`ECHOTALK_RAM_WATCH=<lo>-<hi>`, `ECHOTALK_READ_WATCH=<lo>-<hi>`. The
phoneme dump is `offset:value` in **write order**, so a stress-pass
shift appears as several writes to the same offset; reading only the
values counts them twice.

### Key addresses (Textalker 3.1.3)

```
$DB5E  text scanner: copies a word into $FDAC for the syllabifier
$DD10  syllabifier entry          $DD30  its vowel loop
$DD65  prefix test                $DD98  suffix test, which LOOPS
$DE4C  find the next two vowels   $DE7A  the -LE / -ES / -ED cases
$DF35  adjacent vowels            $DFFD  consonants between two vowels
$E06A  list matcher               $E0C9  vowel scan
$E0F8  insert a sentinel at X     $E11D  attribute of buf[X]
$E127  prefixes    $E141  suffixes    $E192  digraphs    $E1B1  clusters
$E1FA  rule engine main loop      $E256  the abbreviation path
$E29B  the stress pass            $E2F0  anchor and literal run
$E354  left context, backward     $E3A9  right context, forward
$E400  patches DEX/INX for direction into five sites
$E40D  class dispatch, AND #$0F through an RTS-trick table
$E439-$E590  the class handlers, one per metacharacter
$E596  attribute lookup: A AND $EF24[buf[X] AND $7F]
$DBD6  stage 2 driver     $DC10  each word starts at pitch 7
$E5AA  Ctrl-V phoneme mode, which runs one character behind
$E5C7  speak one allophone   $E6B0  the codes that are not allophones
$E674  stop-consonant release   $E713  pitch offset and vowel variant
$E7A3  read an allophone   $E7ED  unpack a frame   $E893  emit one
$E945  the running pitch   $E9AF  the bit packer   $EA1A  send $60
$EF24  128-byte attribute table   $E57E  the 7 stop consonants
$EFDF  dispatch table, entry(c) = $EFDF + 2*ord(c), chars $20-$65
$F0AB  rules begin; the area runs to about $FCC4
$FDAC  input buffer, the WHOLE LINE, split by $E6 sentinels
$FD93  syllable counter   $FD9E  buffer end   $FDA9  vowel-loop end
$FEAD  phoneme buffer (v3.1.3); $FFAC is its index
```

**v1.3, which loads at `$D400`** (`notes/disasm_v13.txt` is the listing):

```
$D891-$D957  the 13-command comparison chain     $DA09  dispatch load
$DA9D  anchor search      $DB2B  left-context chain
$DC4D  right-context chain (maps the codes DIFFERENTLY from backward)
$DCC4  inflectional ending -- its ING branch is dead code, a 1.3 bug
$DD43  payload copy       $DD53  attribute lookup
$DD80  stage-2 driver     $EAD6  speak one allophone
$EC50  bit packer         $ECFC  unpack a frame   $EEA0  unrolled emitter
$DE13  attributes   $DE79  punctuation names   $DECD  dispatch
$DF99  rules begin  $EFAA  Ctrl-V map   $EFE5/$EFEF  pitch / variant
$EFF9  58 allophone pointers    $F06D  allophone data
$F492  phoneme buffer ($F48A its length)   $F59F  input buffer
$F6A1  flags   $F6A2  group   $F6A3  index   $F6C8/$F6C9  pitch/monotone
```

### What is left

1. **The Ctrl-E driver is done, for both versions.**
   `tools/textalker.py` owns the machine state, parses the input the way
   `$D8D4-$DBA3` does and drives both stages. Verified exact against the
   emulator: text mode, Ctrl-V phoneme mode, and `A M S C E L P F V D K
   T`, over 1080 random command streams per version (nine seeds) as
   well as fixed cases. `B`, `O`, `R` and the `; , I ?` index marks are decoded but
   not differentially tested.

   Three things about the buffers had to be got right, and getting one
   without the others is worse than none -- the persistent buffer alone
   dropped the command fuzz from 98% to 75%:

   - **Neither buffer is ever cleared.** A flush resets only the LENGTH
     (`$DBB8` for `$FD9E`, and stage 1 overwrites only as many phoneme
     bytes as it produces). The tail of the previous utterance stays
     where it is, in both `$FDAC` and `$FEAD`, and is still readable.
   - **The literal run reads past the end; the contexts do not.**
     `$E305-$E310` compares `$FDAC,X` with no limit, while `$E35C` and
     `$E3B1` both stop at `$FD9E`. So a rule can reach past the text
     into the leftovers, match, and consume them.
   - **`$E247` tests the cursor for EQUALITY with `$FD9E`.** Put the
     three together and a rule that consumes more characters than are
     left steps straight over the end, the test never fires, and stage 1
     walks on through the leftovers until `$E243` stops it at 240
     phonemes. `VWVOUQDPWO45DZAA QW
` does exactly that:
     writing "PWO" over "WVOUQ" leaves "PWOUQ" in the buffer, the O
     matches the OU rule and takes the stale U with it, and Textalker
     emits 240 phonemes for a three-letter word. It is audible, and
     reproducing it is the point.

   `$DC71`'s one-code phoneme lookahead is the same effect one stage
   later: for the last code it reads one past the end, so spelling `XH`
   letter by letter makes H's `$64` find the `$5B` that X left, which
   fakes a following "T" and gives the stop its `$E674` +8 release
   variant. `X` alone and `H` alone are both exact; only the pair shows
   it. `tools/frame_diff.py` -- unpacked LPC frames, emulator against
   port, tagged by allophone -- is what found it, and is the right first
   move whenever the chip bytes diverge and the state looks identical.

2. **v1.3 is supported, and the two versions can be mixed.**
   `tools/profiles.py` holds both address sets and detects which image
   is which -- the dispatch table alone cannot tell them apart (each
   layout scores 70 of 70 on the other's image), so detection checks the
   attribute table's vowel bit. Every tool takes `--version`.

   v1.3 loads at `$D400`, not `$D000`, and **has no syllabifier at
   all**: watching `$F59F` while it speaks shows the text going in and
   nothing else. So `$9B`, `$9C`, `$E6`, `$FC`, `$FD`, `$FE` and `$FF`
   have no meaning there, and its rules are written to compensate. Four
   further differences, each worth a family of words:

   - **The two directions map the class codes differently**, which 3.1.3
     never does. Backward (`$DB2B`) `$F6` is the voiced test and `$F9` a
     second `$F8`; forward (`$DC4D`) `$F6` is just "a consonant" and
     `$F9` and everything above it is the inflectional-ending matcher.
   - **`$F7` does not step back after its run** (`$DC0F`, `$DD2E`),
     though `$F8` does. A vowel run therefore swallows one extra
     position -- which is exactly why `{F3}{82}{F7}G` fails for BEGIN
     and it comes out *bih-jin*.
   - **`$FA` matches the H of a digraph, not the C/S/T**, consuming two
     characters the way 3.1.3's `$FB` does.
   - **`$F9`'s ING branch is dead code, and that is a bug in
     Textalker.** `$DCF8` does `INX` but never reloads the character, so
     `$DCFD`'s `CMP #$CE` still sees the I. Reproducing the bug is what
     fixed DARLING, BOWLING and GARGLING.

   The **synthesiser is the same code** at different addresses, down to
   the pitch complement's `ADC #$41`; the only structural change is that
   v1.3 unrolls the parameter emitter, so the ten K widths are inline
   constants rather than a table. **The allophone data is byte-identical
   between the versions**, as are the character map and both pitch
   tables: Street Electronics changed the rules, not the voice.

   So `Machine(rules=, voice=)` is real but lopsided -- `--rules`
   changes the pronunciation, `--voice` changes nothing unless someone
   supplies a modified image. What the rules do change:

   | | 3.1.3 | 1.3 |
   |---|---|---|
   | ANGRY | `24 53 60 4F 1C` | `24 53 60 1C` -- the R is gone |
   | COPY | `61 3B 62 1C` | `61 1B 62 1C` -- 1.3 is right |
   | BEGIN | `5E 26 60 27 52` | `5E 0D 65 27 52` -- J, not G |
   | NICKNAME | `52 27 7F 7F 52...` | `52 27 61 52 1F 51 7F` -- 1.3 keeps the K |

   v1.3's command set is **13 letters** in a comparison chain at
   `$D891-$D957`: `A B C E F L M O P S T V W`. No `D`, no `K`/`N`, no
   `R`, and none of the `; , I ?` index marks -- and no `ADC #$05`
   anywhere in the image, so no capital lift in letter mode. The shared
   commands set the same bits as 3.1.3's.

   **Verified, and the command driver included**: everything in the
   checkpoint-3 table above, plus reference_text 215/215, the story
   corpus 1039/1039 and 4400 random words at the phonemes -- the same
   figures as 3.1.3, on the same tests.

   Finishing v1.3's driver took seven readings, each a place where the
   port had modelled 3.1.3 and v1.3 does something else. They only show
   once a rule reads past the end of the buffer, which is what the
   command fuzz makes happen:

   - **v1.3's startup state comes from its LOADER**, not its image.
     `$9410-$9445` sets eleven variables before the first character
     arrives. The flags byte reads `$00` from the OBJ -- speech OFF --
     where the machine has `$09`, so any text not opening with an
     explicit Ctrl-E T was silent in the port.
     `profiles.loader_init` reads that block out of the loader.
   - **Neither buffer is initialised at boot.** Before anything is
     spoken they hold the image bytes at those addresses. 3.1.3's are
     zeros; v1.3's `$F494` is `$A0`, inside the `$51-$B2` window
     `$DC7C` tests, so the very first utterance's last code takes the
     `+8` stop release.
   - **An allophone is eight frames, not ten.** `$ECEC` decrements
     before unpacking and loops on `BNE`. That moves the counter Ctrl-E
     C tests, and the test is inverted to match (`$EDB9`'s `BEQ` emits,
     `$E8AB`'s skips), so the two cancel. Model one alone and compressed
     speech comes out as the other half of the frames. v1.3's compress
     flag is `$F6C7`.
   - **The dispatch pointer is followed wherever it goes.** `$7F` --
     what a leftover `$FF` masks to -- points at `$F3C5` in the
     allophone data and yields `12 0C`. A section has no end either:
     `$DADB` steps to the next rule forever until one matches.
   - **The greedy runs stop at a fixed `$8F`** (`$DC90`, `$DD19`), where
     3.1.3 copies `$FD9E` into `$FD99` and stops where the text does.
     The same `$8F` caps the inflectional matcher at `$DD0D`.

3. **The C port is done.** `src/tk_*.c` is the whole speech path in C:
   raw input bytes in, the exact TMS5220 byte stream out, no 6502.

   ```
   src/tk.h          the shared surface
   src/tk_image.c    tools/profiles.py    where everything lives, and
                                          which version an image is
   src/tk_syllable.c tools/syllabify.py   $DD10-$E126, v3.1.3 only
   src/tk_rules.c    tools/rule_walk.py   $E1FA-$E5A9
   src/tk_synth.c    tools/allophone.py   $DBD6-$EA43
   src/tk_machine.c  tools/textalker.py   $D8D4-$DBA3, the driver
   src/tk_arpabet.c                       ARPABET <-> phoneme codes
   src/tk_dict.c                          a CMUdict-format dictionary
                                          as a letter-to-sound source
   tools/tk_phonemes.c  text -> phoneme codes, on the command line
   tools/tk_chip.c      text, Ctrl-V strings, code lists or raw input
                        streams -> chip bytes
   tools/tk_diff.py     the differential test, against either oracle
   ```

   **NO TABLE IS COMPILED IN.** The rules, the attribute table, the
   punctuation names, the syllabifier's four lists, the allophone
   frames -- all of it is read out of the user's own image at run time,
   exactly as the emulation path does. The legal stance is unchanged.

   Verified against the **emulator**, both versions: reference words
   208/208, whole lines 679/679 at the phonemes AND at the chip bytes,
   700 random words, 400 random Ctrl-V phoneme strings, and 360 random
   Ctrl-E command streams. Against the Python, both versions: 19,000
   stage-1 cases, the same corpora at the chip bytes, and 1080 command
   streams each.

   `tk_diff.py` takes two oracles and both matter. `--python` is fast
   -- no 6502 runs at all -- and is the one to iterate against;
   `--emulator` is the acceptance test, because a bug shared between
   the Python and the C would pass the first happily.

   Two bugs got through the corpora and were caught by the command
   fuzz, and both are worth knowing about: the punctuation-name lookup
   compares the MASKED character (nothing but Ctrl-E A and M reaches
   it, and the corpora all run at the default flags), and
   `base + rom_len` overflows a `uint16_t` for v1.3, whose image ends
   at exactly `$10000` -- which empties every junk dispatch section.
   3.1.3's image ends at `$FFFA` and hid it completely.

   **The port is wired into the library** as a selectable engine:
   `echotalk_set_engine(et, ECHOTALK_ENGINE_NATIVE)`, ABI 12, 56
   exports, `say --native`. The engine is orthogonal to the Textalker
   version -- the version is the loader/OBJ pair, both engines handle
   both -- so a host has two independent controls, which is what the
   NVDA add-on will need. Both engines are fed the identical byte
   stream through one dispatcher, so settings, sniffing, chunking,
   trimming and index marks are unaffected. The emulated path is
   unchanged and every baseline still matches.

   **The native engine's audio is right for most material and not all
   of it, and the gap is Ctrl-V phoneme mode.** The chip byte stream is
   byte-exact always; what differs is WHEN each byte reaches the chip,
   and the chip's output depends on that.

   Over random multi-line command/phoneme/text streams
   (`engine_diff.py --fuzz`): **v1.3 matches about 92%, v3.1.3 about
   75%, and every case that differed contained a Ctrl-V phoneme
   string** -- of nineteen payloads without one, none ever differed.
   The reference corpus flatters it at 8 of 9 for v1.3 and 5 of 9 for
   v3.1.3, which is precisely why the random material exists: Jayson
   asked whether it had been tried on new material, and it had not.
   The answer changed the picture.

   `tools/engine_diff.py` is the acceptance test, and **how it measures
   took three goes to get right** -- every version of it reported
   defects that were not there:

   - Fixed windows. The engines differ in the length of the gaps, so
     any window containing a gap boundary mismatches at every
     alignment however identical the speech is.
   - One alignment per speech run. A gap can change length by a SINGLE
     SAMPLE in the middle of what looks like one run; the two halves
     then need different shifts, and the first was reported as
     corrupted speech.
   - Re-searching the alignment per block over thousands of samples. A
     400-sample block will happily find a false match far away. That
     reported two entire runs as garbage when they were identical.

   What works: carry ONE alignment across the whole file, found once
   over a block long enough to be unambiguous, and let it move a few
   samples per block thereafter. Over `reference_text/`: **v1.3 matches
   on eight of the nine files. v3.1.3 matches on five outright and two
   more to within one block in a thousand.** (Those figures were read
   here as 9 and 6 until session 20, when re-measuring against an
   unchanged build printed 8 and 5. Nothing regressed -- the emulated
   baselines are still exact to the sample -- the numbers had been
   carried forward rather than re-run. Run it rather than quoting it.)

   The minimal case is `A;` -- a semicolon spoken by name, which
   is a long utterance -- followed by any Ctrl-V line: `TA;`
   CR `P` CR. The phoneme utterance renders differently while
   everything before it is identical. The chip byte stream and the FIFO depths
   are identical there -- 5678 bytes each, and the same depth at every
   write -- so it is the chip's internal phase at the restart rather
   than the data. v1.3 does not show it. The NVDA driver replaces
   Ctrl-V with a space before the library sees it, so it cannot arise
   through the add-on.

   **The cycle model now covers every stage**, and the profiler is what
   said where to aim it. `ECHOTALK_CYCLE_PROFILE=<file>` on the harness
   buckets 6502 cycles by page, and for ordinary text it reports **65%
   in `$FC00`** (the status polls and the chip write), **26% in
   `$E900`** (the bit packer) and only about **3% in stage 1**. Guessing
   would have put the effort in the wrong place entirely.

   Modelled, in rough order of size: the bit packer per bit and per
   byte; the three call sites around it separately; `$E7ED`'s unpack,
   which runs for all ten frames whether the mask emits them or not;
   the frame loop; both polling loops; stage 1 per character, per rule
   tried, per pattern byte, per class handler and per anchor-scan byte;
   the syllabifier's list matcher per comparison; and `$D8D4`'s driver
   chain per input character.

   Whole utterances now land within about 0.01% in length -- one case
   measured 12,527 samples against 12,528 -- and the first chip byte
   of a short utterance lands at sample 71 where the emulator puts it
   at 76.

   **It is still not enough**, and that is the finding. A residual of a
   few cycles per operation compounds into a one-to-two sample phase
   error at an utterance restart, and `$EA1A` starting a Ctrl-V
   utterance one interpolation step out of position gives it a
   different number of leading idle frames. Closing that means counting
   every instruction of the stage-2 driver as well -- a cycle-accurate
   6502 model specialised to Textalker, which is what the emulator
   already is, and exactly.

   **What made the earlier difference was reading the disassembly, not
   sweeping constants.** Every constant that was *guessed* turned out inert -- a
   per-byte cycle cost swept from 0 to 2400 moved nothing measurable,
   because the waits absorbed it. Constants *derived* from the listing
   are a different matter, and the cycle model is built from four:

   ```
   TK_BIT_CYCLES  26   one turn of $E9AF's bit loop
   TK_BYTE_CYCLES 77   ...when that bit completes a byte, via $FC8B
   TK_CALL_CYCLES 68   what $E8DC's K loop spends around each call
   TK_POLL_*      139 / 70   one turn of $E9CA's and $E9E3's polls
   ```

   259 cycles a byte from the packer plus two K loops is 395, and 395,
   373 and 400 are the three commonest gaps in the emulator's own cycle
   dump against a floor of 291. That is the check that the model is
   right, and `ECHOTALK_CYCLE_DUMP=<file>` on the harness is how to
   repeat it.

   Five omissions came before any of that mattered, all found the same
   way:

   - `$EA1A` reads the chip's talk status first and does NOTHING at all
     while it is still talking -- no SPEAK EXTERNAL. Since SPEAK
     EXTERNAL RESETS THE FIFO, sending one early discards queued frames
     and the chip decodes the remnant as a partial frame. It also
     resets `$FD88`.
   - `$E6A7`/`$E6AC` wait and then delay after a stop consonant's stop
     frames.
   - `$E6DA` waits at end of utterance.
   - `$E9CA` is the buffer-low wait -- Textalker's actual flow control.
     Without it the FIFO saturates at 16, where writes are DROPPED
     SILENTLY. The emulated FIFO never exceeds 15.
   - `$E9E3` waits for the TALK bit and nothing else. Versions of it
     that also waited on the FIFO, or gave up when it stopped draining,
     were papering over the two bugs above.

   v1.3 has all of these at `$EBB1`, `$EAF4`, `$EC61` and `$EC02`, so
   they are modelled for both versions rather than by profile.

   Two diagnostics earned their keep: `ECHOTALK_CHIP_BYTES=<file>`
   (every byte either engine hands the chip, with the audio sample it
   arrived at and the FIFO depth) and `ECHOTALK_TK_TRACE=<file>` (the
   native engine's waits, and whether each SPEAK EXTERNAL was sent or
   skipped). `ECHOTALK_CYCLE_DUMP=<file>` on the harness gives the 6502
   cycle count at every chip write, which is how the waits were shown
   to dominate: the work between bytes is 291-463 cycles and the waits
   are ~48,000.

### What this branch was FOR, and where that got to

Jayson's reason for wanting the rewrite, in his words, was to be able to
make future improvements: **discarding the 80-character chunk limit**,
and **possibly pulling in another set of letter-to-sound rules or a
dictionary** under a compatible licence, translating its output into
the phoneme codes Textalker already uses.

**Both are done**, and neither needed the audio to be phase-identical
with stock Textalker:

- **The rules seam** (session 16) -- `tk_g2p`, with a CMUdict-format
  dictionary as the first source. The section below describes it, and
  `notes/phoneme_alphabet.md` has the code-to-sound correspondence it
  stands on.
- **The chunk limit** (session 19). It really was the emulation's
  constraint: 80 there, 130 or 200 natively, and further still with an
  extended line buffer. "The DLL and the NVDA add-on" has the numbers.

Two more things came out of the same branch without being on that list:
**disk images** (session 17) and **finding the tables rather than
listing them** (session 18), the second because a disk turned up
carrying a build no list covered.

**The recommendation on the remaining audio difference is still to
leave it.** Once a different rule source is in, matching stock
Textalker's frame timing is meaningless -- and the emulated engine is
there, unchanged, for anyone who wants the original exactly. The
v3.1.3 Ctrl-V phoneme-mode restart is the one item never closed.

"The table extractor", which sat on the list for several sessions, is
retired rather than done: reading every table out of the user's own
image AT RUN TIME made a pre-extraction step pointless. The extractor
people actually needed was for the DISK.

### The letter-to-sound seam (session 16)

`tk_g2p` in `src/tk.h` is the seam: an input buffer in, phoneme codes
out, `tk_machine_set_g2p()` to put one in. Textalker's own `$E1FA`
rules are `tk_g2p_textalker`, the default, and nothing below the seam
-- the synthesiser, the chip, the library, the add-on -- knows or cares
which source is in force.

**What made it work is that a source can DECLINE.** `tk_run_range()` is
`$E1FA` over `buf[start..stop)` only, appending at `out[n]`, with the
rest of the buffer still visible as context; so a partial source hands
back the words it does not know, and the rules keep their contexts, the
word separators, the spoken punctuation names and the stress pass --
which walks BACKWARD over everything built so far, dictionary words
included. `tools/seam_diff.py` checks that declining costs nothing:
**a dictionary matching no word in the corpus gives byte-identical
output to no dictionary at all, 268 of 268 lines, both versions.**

**`src/tk_dict.c` is the first source**: a pronunciation dictionary in
**CMUdict's format**, so `cmudict.dict` works unmodified. It is an
override list, not a replacement -- unknown words, punctuation and
spaces all go back to Textalker. Two extensions:

```
BOOT      =booht        a RESPELLING, through Textalker's own rules
ECHOTALK  /3EKOT3*K     Street Electronics' Ctrl-V notation
NICKNAME  N IH1 K N EY2 M
```

**The respelling is the one to reach for.** Textalker says BOOT with
the vowel of BOOK; `=booht` fixes it, needs no phoneme chart, and is
how anyone who has lived with an Echo already thinks. It is resolved at
load time over the respelling alone, so it gets the syllabifier and the
whole rule engine but not the surrounding line's context.

**A dictionary belongs to ONE Textalker version**, and nothing in the
format says which. v3.1.3 says BOOT, ROOT and SHOOT with the vowel of
BOOK; **v1.3 says all three correctly** (`B UW2 T` against `B UH3 T`),
so `BOOT =booht` fixes 3.1.3 and costs 1.3 the `<sil>` the H puts in
the middle of the word for nothing. ROUTE is the sharpest case: both
versions rhyme it with *about*, so 1.3 takes `ROUTE =root` and 3.1.3
needs `ROUTE =rooht`, because 3.1.3's ROOT is wrong in the same way its
BOOT is. Some entries are safe in both -- `NICKNAME =nicname` gives 1.3
byte-identical output to NICKNAME itself -- but that is per word, not a
rule. Documented in the add-on's readme, the top-level README and
`echotalk.h`; `examples/pronunciations.dict` says at the top that it is
a 3.1.3 file.

```
echotalk_load_dictionary(et, path, err, sizeof err)   /* ABI 12 */
echotalk_dictionary_entries/hits/misses(et)
say --native --dict FILE ...
tk_phonemes --dict FILE --arpabet ...
tk_chip --dict FILE --lines|--raw ...
```

`hits` and `misses` exist because "my dictionary is not being
consulted" and "my dictionary does not have that word" are otherwise
indistinguishable by ear.

**EchoTalk SHIPS NO DICTIONARY**, deliberately, and that is the answer
to the licensing question: the file is read at run time, exactly as the
Textalker image is, so its terms never reach EchoTalk's BSD-3-Clause.
eSpeak NG is GPLv3 and cannot be linked in; CMUdict is permissive; the
NRL rules are a US Government work. **`THIRD_PARTY_LICENSES.md` has the
reasoning**, and it shaped the design rather than following it.

**`src/tk_arpabet.c` is the alphabet bridge**, and
`notes/phoneme_alphabet.md` is how it was derived -- by running stage 1
over words whose pronunciation is not in doubt (BEET, BIT, BAIT, BET,
BAT, COT...) and reading the codes off, not by guessing. It is a
measurement of how Textalker sounds, 40 lines long, and no table of
Street Electronics' is compiled in by it or anything else.

The encoding, which is the part worth memorising: **a code decodes to
an allophone index and a pitch group.** Index below `$1A` gets three
codes -- `idx`, `idx+$1A`, `idx+$34` for groups 1, 2, 3 -- and index
`$1A` and above gets one, `idx+$34`. The **pitch group is the stress**:
`$EA7F+1` holds `FE 00 02`, so groups 1/2/3 offset the pitch by
-2/0/+2. `$7F` is silence, `$19` the v3.1.3 word separator, `$B3`/`$B7`
the intonation markers, `$1A` the terminator. "CAT" is `61 24 63`.

**One reading was wrong and Jayson caught it by ear**: `$01` and `$0B`
were swapped. The first pass used ONE frame word each and picked COT
and BOUGHT -- the cot/caught minimal pair, where Textalker merges, and
BOUGHT is a word it gets wrong ("baht"). `$01` is **AA** (FATHER, and
R-coloured it is CAR), `$0B` is **AO** (LAW, TALK, CAUGHT, CHALK). Four
frames per vowel settle every one of them in a single run, and the
whole set is re-verified that way now. **The round trip could not have
caught this** and reported 151 of 155 throughout: encode and decode
share the table, so a swap is self-consistent.

**An R after a vowel is not a separate sound.** `$E62F` colours indices
1-8, so the rules emit ONE code for CAR, not AA + R. `tk_arpabet.c`
folds `<vowel> R` the same way -- without it, STARTING has an audible R
of its own. Two folds are not the arithmetic: `EH R` takes the EY-form
(HAIR, CARE) and `AO R` the OW-form (MORE, FOUR), because `$0B` has no
R-form at all.

Two things ARPABET cannot carry, both measured rather than assumed:

- **The pitch group does not survive it.** Three ARPABET stress levels
  and three pitch groups do not line up -- v3.1.3 speaks an unstressed
  vowel at group 2 and **v1.3 at group 1** (THE is `54 29` against
  `54 0F`). The `/` escape reaches a group directly.
- **/z/ is a PAIR, `$23` beside `$27`, and the order follows the
  SPELLING**: the letter Z gives "S Z", an S pronounced /z/ gives
  "Z S", and RESEARCH and SYNTHESIZER break even that. `tk_arpabet.c`
  emits "Z S" always and reads either back as one.

The round trip -- Textalker's own output written out as ARPABET, fed
back as a dictionary, compared -- is exact on **151 of 155** words in
v3.1.3 and **156 of 181** in v1.3, with **one genuine mismatch across
both** (SYNTHESIZER, the /z/ order); the remaining 28 differ only in
pitch group. `python tools/seam_diff.py --version 1.3`.

**What to try next with it**: CMUdict as the dictionary, which fixes
FAVORITE, NICKNAME, COPY and TOMATO in a line each and needs no code.
After that, a rule-based source for the words no dictionary has -- the
NRL rules are the obvious candidate and are public domain -- written as
a second `tk_g2p` that falls back to Textalker's, exactly as
`tk_dict.c` does.
4. **Textalker's own chunking IS reproduced -- this item used to say
   the opposite and was wrong.** `$DB6A` flushes the line buffer when
   240 of its 256 bytes are used, and `tk_machine` does the same.

   What is NOT reproduced, and cannot be, is the EMULATION's own
   truncation: nothing here plays the part of DOS setting up a screen,
   so `$FD80-$FD82` read as zero and a line loses its tail past 79
   characters. Measured: 78 is byte-identical between the two engines
   and 79 is where the emulation drops text. That is why the chunker
   exists, why `line_diff` and `byte_diff` compare single-chunk input
   only, and why the chunk window is 80 on the emulated engine and
   130/200 on the native one.

   The distinction matters because conflating the two makes "remove the
   chunk limit" sound free. It is not: turning chunking off hands the
   splitting to Textalker, which cuts at exactly 240 bytes wherever
   that lands. On one test passage it cut the word "on" in half and the
   result says "sentence oh ... in purpose" -- the wrong words, not a
   misplaced pause.

### Mispronunciations to REPRODUCE, not fix

Jayson identified these by ear and each is confirmed in the traces.
**`notes/textalker3_rule_changes.md` explains WHY** most of them exist:
Textalker 3 rebuilt 1.3's rule set on a syllabifier and traded
enumeration for generalisation, which is why several words v1.3 said
correctly came out worse in v3.

- `NICKNAME` loses its K entirely in 3.1.3. `C{FC}{8B}` silences the C
  before K and `{9B}K{8E}` silences the K before N -- but that is the
  mechanism, not the cause, and both rules are right in themselves.
  **The cause is that `KN` is in the syllabifier's cluster list at
  `$E1B1`**, so it moves whole to the next syllable: `|NIC|KNAME|`, the
  K starts a syllable before an N, and the silent-K rule that exists
  for KNIGHT fires. So it is not one word -- `ACKNOWLEDGE` is
  *a-nowledge* and `UNKNOWN` is *un-nown*, while `SICKNESS` survives
  because NESS is in the SUFFIX list and that boundary wins. v1.3, with
  no syllabifier, says all three correctly.
  `notes/textalker3_rule_changes.md`
- `COPY` takes the O of *boat* in 3.1.3; v1.3 is right. The rule is
  `{F3}{F8}O{9B}{F4}{99}` -- O at a syllable end before consonant + Y --
  and it is *correct* for PONY, HOLY, ROSY and STORY. JOLLY and HOBBY
  escape it because the doubled consonant moves the boundary.
- `THOUGH`, `ALTHOUGH` and `THUS` take the unvoiced TH of *thin* in
  3.1.3, and `THAW` and `THATCH` take the voiced one. v1.3 gets all
  five right, because it enumerates the voiced-TH words instead of
  generalising about them.
- `BEGIN` in v1.3 is *bih-jin*: it emits `65`, which is J, where 3.1.3
  emits `60`, which is G.
- `FAVORITE` is *faiv-or-ite*, with the I of *bike*.
- `-ING` is rendered closer to *in*: "starting to translate" comes out
  as roughly *"startin to translit"*.
- `BOOT`, `ROOT` and `SHOOT` take the vowel of BOOK in 3.1.3 -- all
  three come out `UH`. **v1.3 says all three correctly.** `ROUTE`
  rhymes with *about* in BOTH versions.
- `BROW` and `THROW` take the same vowel, right for one and wrong for
  the other.
- **`GHOTI` is an easter egg** and it is in BOTH versions. v1.3 renders
  it byte-identically to its own `FISH`.

All of these now reproduce byte-exactly, because everything does.

### Lessons that cost time

- **A special case you have to invent is a sign the model is wrong.**
  The "magic E" check was a hand-written rule with no counterpart in
  the machine, propped up by a page of measurements. It existed only
  because syllable boundaries were being modelled as assertions instead
  of the bytes they are. Transcribing the buffer deleted it and took
  the score from 85% to 97% in one change.
- **An explanation that fits a handful of hand-picked words is weak
  evidence. Only the corpus decides.** Three locally-perfect theories
  failed this way.
- **After fixing a semantic error, re-test the structural assumptions
  measured while it was in place.** Reading `$9B` as a vowel lookahead
  instead of the syllable boundary distorted both the selection
  strategy and the `$F9` reading, and both had looked well-supported.
- **Frequency is not importance.** `$9C` has a single use in the table
  and was dismissed as too rare to matter; chasing it led to the `-ED`
  family and then to the wildcard reading.
- **A measurement that cannot vary is not evidence.** The fit reported
  `$E6` as inert under every candidate; the rules were unmatchable, so
  it was measuring nothing.
- **A dropped test case is a hidden failure.** The batched oracle
  silently discarded any batch it could not parse, which was 78 of 1200
  fuzz words. Retry them individually or the run means less than it
  says.

## The library

**`src/echotalk.h` is the API reference and documents itself.** What
follows is only what is hard to infer from it.

```c
echotalk *et = echotalk_create(loader_path, obj_path, err, sizeof err);
echotalk_set_speed(et, 1.5);          /* speed only, continuous       */
echotalk_speak(et, "Hello.");         /* queues; does NOT synthesise  */
while ((n = echotalk_read(et, buf, 1024)) > 0) { /* 16-bit mono PCM   */ }
```

### Streaming

`echotalk_speak()` queues text and returns immediately; `echotalk_read()`
synthesises one utterance at a time on demand. First audio for a 437-byte
passage arrives in 39 ms against 220 ms for the whole thing.

Synthesis measures **90–155x real time** depending on the machine, which
is why there is no worker thread — inline synthesis from an audio
callback has ample headroom. A host that disagrees can call
`echotalk_synthesize()` from its own thread; the library stays
thread-free and is **not thread-safe**.

Two things a host must know:

- `echotalk_available()` is **0** right after `speak()`. It means
  "samples ready", not "speech outstanding" — that is
  `echotalk_pending()`.
- `echotalk_read()` returning 0 still means finished, because it
  synthesises before giving up.

### Index events

`\x04 7I` places a mark; `echotalk_next_index()` collects it. Drain after
**every** read including the one that returns 0, or a mark at the end of
the text never fires.

A mark does **not** end the utterance. It used to, which made positions
exact and made NVDA's Say All read a wrapped sentence one line at a time.
A mark at an utterance boundary is still exact; one inside is placed
proportionally by character offset, which is necessarily approximate —
Textalker emits nothing until the terminating CR, so there is no way to
observe which character is being spoken.
`echotalk_set_index_break()` restores the old behaviour.
`notes/say_all_index_breaks.md`

### The five rate-ish controls are independent

| control | effect |
|---|---|
| pitch | pitch only, Textalker's own `nP` |
| **speed** | **speed only, continuous 0.25–4.0 — use this one** |
| frame rate | speed only, the chip's own four fixed steps |
| clock multiplier | speed *and* pitch, the sped-up-tape effect |
| sample rate | neither; output format only |

**The last two are not as independent as that table suggests.** The chip
produces 8000 x the clock multiplier, so at 1.5 it is really generating
12000 Hz, and an output rate below that downsamples through a resampler
with no anti-aliasing filter — the discarded band folds back in instead
of being removed. A host must treat the output rate as a **floor** and
raise it to at least `8000 * clock`. The library deliberately does not do
this itself; `say` and the NVDA driver both do.
`notes/clock_output_rate_downsampling.md`

Speed works by scaling how fast the chip's parameter state machine walks
a frame while the lattice filter keeps producing one sample per output
sample. **Pitch measured at 129.0 Hz from 0.5x to 3.0x**; the clock
multiplier takes it to 258.1 Hz as it should. 1.0 is byte-exact by
construction. The delivered ratio is monotonic but sub-linear (2.0 gives
1.84x, 3.0 gives 2.56x), which matches FIFO starvation — Textalker's flow
control setting the pace. Left uncalibrated on purpose.
`notes/continuous_speed_accumulator.md`

### Settings are mirrored and readable back

Ctrl-E commands embedded in text still reach Textalker untouched, but the
library also watches them go past and updates its own variables. Twelve
getters report the voice actually in force however it was set, and every
value a getter returns is accepted by its matching setter — which is what
lets a voice be carried to a fresh instance when switching Textalker
version. `notes/settings_mirroring.md`

**`nP` and `nF` are one Textalker setting, not two.** `nP` sets pitch and
normal intonation; `nF` sets the same pitch and monotone. There is no
separate flatness value and no default for one. **Command letters are
case-insensitive**, and **pitch does not saturate at 63** — `\x05 99P` is
audibly not `\x05 63P`, though sniffed values are clamped to 0–63 so
getters and setters always agree.

### Behaviour worth knowing

- A one-character utterance is wrapped in `Ctrl-E L`/`Ctrl-E A` and
  restored **before the terminating CR**. A bare `,` is silent without
  the wrapping; restoring after the CR instead makes Textalker announce
  the CR as "return". The restore goes back to the *tracked* modes, so a
  caller's choice of letter or punctuation mode survives.
- Dead air is trimmed at the head of **every** utterance, not just the
  first. Getting this wrong leaves an audible pause at every chunk
  boundary.
- Boot audio is discarded, which matters most for v1.3 and its ~1.37 s
  calibration delay.
- **Single instance only.** Fake6502 keeps the CPU in globals, so
  `echotalk_create()` fails if one already exists. Creation costs 0.3 ms
  (v3.1.3) or 7 ms (v1.3), so destroy-and-recreate is a perfectly good
  way to switch voice.

### Ctrl-D driver commands

Driver settings are reachable from inside the text, in the shape of
Textalker's own Ctrl-E commands: `\x04`, optional whitespace, an optional
number, a letter. `notes/ctrl_d_driver_commands.md`

```
\x04 1.5S  speed          \x04 0.75C clock         \x04 2F  frame rate
\x04 0B    chunking off   \x04 1R    raw text      \x04 7I  index mark
\x04F      that setting's default      \x04\x04    one literal 0x04
```

- **A command ends the current utterance** (except index marks).
  Textalker buffers a whole line and synthesises nothing until the CR, so
  a command's position in the text otherwise bears no relation to its
  position in the audio.
- **The namespaces are disjoint and the letters differ.** Ctrl-E `F` is
  flatness and `C` is compressed; Ctrl-D `F` is frame rate and `C` is
  clock. Ctrl-D does not duplicate Textalker's commands because Ctrl-E
  sequences survive text preparation untouched.
- Bad commands are swallowed rather than spoken, and counted in
  `echotalk_command_errors()`.

## Getting Textalker off a disk (session 17)

`notes/disk_image_extraction.md` is the record. The short version:

The first step of using EchoTalk used to be "install CiderPress, open a
.dsk, extract two files, rename them" -- a Windows GUI, which is the
wrong shape for the person this is for and the wrong shape for an
installer. It is now:

```
tk_extract disk.dsk --list             what is on it
tk_extract disk.dsk --out roms/        write EVERY pair on it
tk_extract disk.dsk --file HELLO       one named file, verbatim
say --disk disk.dsk "Hello." out.wav   NO extraction step at all
say --disk d.dsk --disk-version 1.3    when it carries several
echotalk_create_from_disk(path, ...)   the same from the library
echotalk_create_from_disk_version(...) ...asking for one
echotalk_disk_versions(path, buf, n)   what it carries
```

**All C, no Python**, deliberately: a pre-runtime step written in Python
would need Python on the user's machine, and nothing here needs it. The
NVDA add-on discovers a disk image beside the driver as a voice exactly
as it discovers a `.ram.bin`/`.obj.bin` pair.

`.do`, `.dsk`, `.po` and `.2mg`. Nibble images are refused **by name**
rather than vaguely; Textalker disks are not copy-protected.

Three things worth knowing:

- **Detecting the sector order from the VTOC alone does not work.** The
  DOS/ProDOS interleave map is its own inverse and FIXES sectors 0 and
  15, so track 17 sector 0 (the VTOC) and the first catalogue sector
  (sector 15) are at the same offset in both orderings. Every `.po` gets
  called DOS-ordered and fails much later reading file data as a
  catalogue. `dsk.c` SCORES both orderings instead -- following the
  catalogue chain, which leaves the fixed points at the first hop, and
  checking that each entry's track/sector list looks like one.
- **Nothing is matched by filename**, and that is forced rather than
  preferred: the versions do not agree on them (`Textalker.obj` against
  `Textalker.ram.obj`, and PIZZA's `TEXTALKER` + `TT.OBJ`). A loader is
  a file carrying the entry trampoline `48 AD 8B C0 4C lo hi`. Its JMP
  targets are the ENGINE'S JUMP TABLE -- every one lands within the
  first few bytes of the engine, on a `4C` or on the `AD 8B C0` that
  enters it (v3.1.3 `$D003 $D006 $D009`, v1.3 `$D400 $D403`, 3.1.2
  `$D003 $D006 $D009`). That, and not a profile match, is what pairs
  them. `boot()` uses the same `tk_disk_obj_page()`, so there is one
  copy of the signature.

  **A profile match is a label, not a gate**, and PIZZA is why. The C
  engine's `tk_image.c` is keyed to one build's table addresses;
  3.1.2's engine is 326 bytes shorter with everything moved, so it is
  rejected. The EMULATED engine runs it perfectly. Requiring a profile
  would have refused a disk that works, so `native_ok` reports whether
  the rewrite is also available and `say --native` says so plainly.

  **Since session 18 the C engine finds its tables too** -- see "The
  profile is found, not listed" below -- so 3.1.2 now runs on both
  engines and `native_ok` is only about builds neither can read.
- **A binary file's header length is the authority, not its sector
  count.** The engine is 12,282 bytes inside 49 sectors.

### Verified on real disks

Jayson supplied two, and both now work end to end. **They also found two
defects that the synthetic tests could not**, which is exactly what
running against a disk this project did not write was for:

- **`both.dsk`** carries v3.1.3 (`TEXTALKER.RAM` + `TEXTALKER.OBJ`) AND
  v1.3 (`TT.RAM` + `TEXTALKER.RAM.OBJ`), alongside INTEGER, OREGON TRAIL
  and ATOM 20. Both pairs are found and extracted; the v3.1.3 output is
  byte-identical to `roms/`. Its v1.3 is a DIFFERENT BUILD from the one
  in `roms/` -- a 602-byte loader against 611, and the engines differ
  from byte 11 -- and it is still identified as 1.3 and still speaks,
  which is the structural detection doing its job.
  **Defect it found**: `tk_disk_find()` returned the FIRST pair and
  stopped. `tk_disk_list()` now returns all of them, `tk_extract` writes
  all of them, `say --disk-version` picks one and the NVDA add-on offers
  each as its own voice.
- **`PIZZA.DSK`** is an Echo software disk with 38 files, carrying
  **Textalker 3.1.2** as `TEXTALKER` + `TT.OBJ`.
  **Defect it found**: its VTOC records **496 bytes per sector** at
  `$36/$37` instead of 256. Nothing reads that field, so nothing ever
  noticed -- but `dsk.c` was rejecting the entire disk over it. The VTOC
  geometry fields are now EVIDENCE (they score, so a standard VTOC still
  wins a tie) rather than a gate; the catalogue is what decides.

The unchanged reservation: `tools/test_dsk.c`'s builder and `dsk.c`'s
reader share one reading of the format, so the synthetic tests can only
catch coding errors. Real disks are what settle the rest, and
`tk_extract <disk> --list` is the check to run on a new one.

**A bug this turned up**: the add-on's `REQUIRED_ABI` was 6 while the
library was on 8, so the driver had been refusing to load against a
current DLL for two sessions. Nothing noticed, because the DLL's own
tests check the ABI and `tools/test_nvda_driver.py` was not being run.
**Run it after any ABI change.**

## The profile is found, not listed (session 18)

`notes/profile_discovery.md` is the record. `src/tk_image.c` no longer
holds a table of addresses per release: it **searches the user's own
image** for the shape of every table, which is what the rest of this
project already does and what PIZZA.DSK forced.

`tools/tk_profile <loader> <obj>` or `--disk <disk.dsk>` prints what the
search decided, and is the first thing to run on a build nobody here has
seen. It names the table it could not find rather than saying
"unrecognised" -- and a failure only means the C REWRITE cannot run that
image; the emulation, the default engine, needs no profile at all.

**Against the two images whose addresses were already known, the search
reproduces every one of them exactly**, and the byte-exactness tests are
unchanged: 679/679 whole lines and 208/208 reference words on both, with
everything discovered rather than declared.

What it finds and by what shape:

```
attrs        128 bytes where exactly A E I O U Y carry the vowel bit
             among the letters                       1 of 1, all images
allo_ptr     58 ascending 16-bit pointers, each within $40 of the last
                                                     1 of 1, all images
charmap      $4F below allo_ptr; pitch_off $14, vowel_var $0A -- fixed
             offsets in every version, and VERIFIED after being applied
punct_names  28 {ptr, ptr, char} triples, first 25 chars ascending
dispatch     exactly $54 past punct_names, confirmed by counting its
             live entries -- so the pair identifies itself
rule_lo      the lowest dispatch pointer
the four syllabifier lists, and their presence is the FAMILY test
```

Three things are worth knowing:

- **`rule_hi` is the end of the image, not the end of the rules.**
  Walking the rules to find their end does not work -- 6502 code
  alternates high and low bytes exactly as a pattern-then-payload rule
  does, so the walk never stops. The image's end only ever bounds the
  LAST section, whose final rule is a catch-all, and the proof it is
  safe is that the C (with `$FFF9`) stays byte-identical to the Python
  (with `$FCC4`).
- **A page of zeros looks exactly like a word list** unless the search
  insists on words. The first version found blank workspace at `$FD78`
  and then decided v1.3 had a syllabifier, which handed it 3.1.3's
  workspace and produced garbage.
- **An image only reaches its last initialised byte.** 3.1.3's ends at
  `$FFFA` and covers the flags variable at `$FFAD`; **3.1.2's ends at
  `$FEB4`**, so reading zero there means speech OFF and silence until an
  explicit Ctrl-E T -- the same trap v1.3 sprang two sessions ago.
  `start_val()` falls back to the documented default outside the image.

### The workspace is found from the CODE, and a claim here was wrong

The buffers and the Ctrl-E variables are RAM addresses only the code
references. This section first said they were a per-family constant --
3.1.3's for any 3.1.x -- and that `tk_diff --version 3.1.2 --emulator`
had measured it.

**That was wrong, and the measurement did not catch it.** 3.1.2's
phoneme buffer is at `$FD5D`, not `$FEAD`; its whole workspace sits
`$150` lower. The text tests passed anyway because the port keeps its
own copy of the phoneme string and only SEEDS its buffers from those
addresses, so the error showed nowhere until a rule read past the end of
the text into the leftovers -- which took the command fuzz to reach.
**A test that exercises a thing only incidentally does not measure it.**

What finds it: the input buffer is read with `LDA abs,X` and the phoneme
buffer stored with `STA abs,X`, a fixed distance apart, so the search is
the one `STA abs,X` operand `W` whose `W + d` is also an `LDA abs,X`
operand.

```
3.1.3   $FEAD / $FDAC   d = -$101      1.3   $F492 / $F59F   d = +$10D
3.1.2   $FD5D / $FC5C   d = -$101            (input buffer ABOVE)
```

Unique in every image -- a Textalker has only nine to fifteen
`STA abs,X` operands. `d` is a family constant and the family is already
known from the syllabifier lists. Everything else hangs off the phoneme
buffer at fixed offsets, because the workspace is one rigid block:
sending `^E37P ^E11F ^EC ^E9D` and watching the writes puts every
3.1.2 variable exactly `$150` below its 3.1.3 counterpart.

### Two 3.1.2s, and the one byte between them

PIZZA.DSK and bex21my.dsk carry different 3.1.2 builds, the same length,
differing in **four engine bytes**: `$D8A2`'s `STA $FE5E` NOPed out, and
a `BCS` displacement at `$E47C` of `$09` against `$16`.

That second one is the whole of the audible difference, and it is the
branch HANDOFF has flagged since session 13. At `$E5CA` (3.1.3's
address) the engine branches when `$E6B0` has handled a code that is not
an allophone; three JSRs follow, so **a displacement of exactly 9 lands
ON the `$E5D5` re-entry and anything larger skips it**. 3.1.3 and bex's
3.1.2 skip; PIZZA's re-enters. That is why a lone `,` in a Ctrl-V string
swallows the rest of the line on one and not the other.

**It is not a property of the release.** v1.3 merges the two routines at
`$EAD6` and re-enters either way, so it behaves like PIZZA's 3.1.2 and
unlike 3.1.3 — the port used to infer this from `v13_metas`, "is this a
1.3?", which was right for the only two builds then known. It is now
read out of the image by shape: JSR, BCS, three more JSRs, LDA abs, BPL.
Unique in all three 3.1.x images, absent from v1.3, and a miss keeps the
family's value.

### All four known builds are exact

| vs the emulator, at the chip bytes | 3.1.3 | 1.3 | 3.1.2 PIZZA | 3.1.2 bex |
|---|---|---|---|---|
| whole lines | 679/679 | 679/679 | 679/679 | 679/679 |
| the nonsense-story corpus | — | — | 1053/1053 | — |
| random Ctrl-V phoneme strings | 300/300 | 300/300 | 300/300 | 300/300 |
| random Ctrl-E command streams | 200/200 | 200/200 | 200/200 | 200/200 |

**Nothing about a build is inferred from its version number any more.**
Every table, the whole workspace and this branch all come out of the
image itself, so the C engine has no list of releases in it at all.

`tools/tk_diff.py --images LOADER OBJ` runs any pair against the
emulator without a `profiles.py` entry, which is how the bex build was
tested: the C finds its own tables and the emulator needs none.

## The DLL and the NVDA add-on

`make dll` builds both architectures plus import libraries. 56 exports,
undecorated cdecl on both, so `ctypes.CDLL` finds them by plain C name.
`-static` leaves only KERNEL32 plus the C runtime the subsystem implies
(UCRT for win64, MSVCRT for win32). **Check with
`objdump -p echotalk.dll | grep 'DLL Name'` after touching the link
line** — a stray `libwinpthread-1.dll` shows up as the driver silently
failing to load on a user's machine. `notes/dll_packaging.md`

`echotalk_abi_version()` returns **12**. A host loading at runtime has no
compile-time check, so bump it whenever the surface changes.

The add-on is in `nvda-addon/` and has **its own README** covering build,
packaging, settings and diagnostics. Points that are easy to get wrong:

- **Voices are discovered, not configured.** Any `<stem>.ram.bin` +
  `<stem>.obj.bin` pair beside the driver becomes a voice, labelled from
  the banner read by briefly booting it. So does an Apple II **disk
  image** (`.dsk`/`.do`/`.po`/`.2mg`), and a disk carrying two
  Textalkers becomes two voices.
- **The engine is a setting** (session 19): "6502 emulation" (the
  default) or "Native". Both say the same words -- that is what the
  byte-exactness tests establish -- and only the native one can take a
  dictionary. If a build has no native engine the driver stays on the
  emulation and logs it rather than falling silent.
- **A pronunciation dictionary is a setting too**: any `<name>.dict`
  beside the driver, in CMUdict format, or "None". **Ignored on the
  emulated engine**, which runs the real 6502 and has nowhere to put
  one. Selecting a dictionary deliberately does NOT switch the engine
  -- changing a user's engine behind their back would be surprising --
  but it IS loaded either way, so it takes effect the moment they
  switch and a bad file is reported when they pick it.
  `_applyAll()` pushes both, last and inside the library lock, because
  a voice change builds a whole new machine and neither survives it.
- **The line buffer can be grown** (session 19), native engine only
  and as a stated divergence: `echotalk_set_buffer_size()`, `say
  --buffer N`, and a "Line buffer" setting in the add-on. 256 (exact)
  up to 4096. The chunk window scales with it, which is the point --
  at 1024 a chunk is four times longer. What it costs is the three
  audible behaviours that depend on the buffer's size: the walk into
  the previous utterance's leftovers, the 240-phoneme cap, and the
  one-code lookahead's wrap. `test_dll.py` checks that a short
  utterance is byte-identical at 256, 512, 1024 and 4096 -- **using a
  FRESH instance per size**, because the voice carry persists across
  utterances and comparing a cold render with a warm one is the same
  mistake as asserting equality of two renderings a phase apart.
- **The chunk window follows the engine** (session 19). 80 on the
  emulated engine, 130 on the native engine with a syllabifier and 200
  without, scaled by the line buffer. Measured, not chosen: the emulation truncates a line at 79
  characters, and the native engine's only bound is Textalker's own
  `$DB6A` flush at 240 BYTES, which the syllabifier's sentinels reach
  at up to 1.8 bytes per character. A host that calls
  `echotalk_set_chunk_size()` pins it;
  `echotalk_reset_chunk_size()` gives the engine's own back.
- **Text is sanitised in the driver**, not the library: Ctrl-D, Ctrl-E
  and Ctrl-V are replaced with a space, because the pipeline would
  otherwise obey them and a document containing one would silently change
  the voice. Replaced rather than deleted, so the rest of the line is
  still spoken.
- **Sliders are logarithmic for rate and chip clock** — 50% is 1.0x and
  every 25% doubles. Defaults are pinned to the Echo's own values;
  `defaultVal` on a `DriverSetting` is settable, NVDA does not insist on
  50%.
- **Cancellation is a generation counter, not a flag**, and `cancel()`
  must never take the library lock. Both cost real bugs.
  `notes/nvda_cancel_race.md`
- **A `PitchCommand` on a monotone voice must emit `nF`, not `nP`**, or
  NVDA's capital-letter pitch bump silently un-flattens the voice.
- `tools/test_nvda_driver.py` runs the driver without NVDA by stubbing
  what it imports — including the metaclass that turns `_get_x`/`_set_x`
  into properties, which the driver depends on and a naive stub misses.

## Reference baselines (regression check after any change)

**Measured with `render_text_loader`, not the library.** The library
legitimately differs: it sends a settings block ahead of the text (~49
samples) and treats single-character utterances differently by design.

Harness defaults (trimmed, chunked, repeat filter off), samples at 8 kHz:

| input | v3.1.3 | v1.3 |
|---|---|---|
| hi_only | 2248 | 2246 |
| onset_this | 10335 | 10332 |
| onset_a | 1246 | 1245 |
| repeat_letters | 7646 | 6445 |
| chunked_paragraph_test | 135164 | 132756 |
| command_coverage_test | 242912 | 208697 |
| alphabet_song_extracted_text | 95493 | 93999 |
| demo_bas_extracted_text | 286064 | 290753 |
| Hedge Trimmer Story | 188764 | 187953 |

Through the library (`say --file`), v3.1.3: hi_only **2297**, onset_this
**10384**, chunked_paragraph_test **135213**, Hedge Trimmer Story
**188812**. Single characters: `,` **3131**, `a` **1329**, `?` **8931**.

**The one that matters most**: `hi_only` must render with amplitude range
**−12127 / +26833** under both versions. That is an exact match to a
real-hardware capture and has survived every change so far. If it moves,
something is wrong.

Also expect: no `WARNING`, no `*** WILD JUMP TRAP ***`, `make test`
passing, and `echotalk_overruns()` zero.

Older notes quote larger sample counts. Those predate the session-10
pacing fix and dead-air trimming; **do not treat them as targets**.

### The whole regression, in order

Everything below passes as of session 19. Run it after any change to
the engine, the seam or the image handling; it takes a few minutes.

```
make test                     text prep, the chunker, the DOS 3.3 reader
make test-dll                 the 64-bit DLL through ctypes, as NVDA does
make test-dll-load            both DLLs from C, via GetProcAddress
python tools/test_nvda_driver.py build/win64/echotalk.dll <romdir>

# the port against the Python (fast) and the emulator (the acceptance
# test), for every build. --images runs a pair with no profiles entry.
python tools/tk_diff.py --lines  --chip --version 3.1.3      679/679
python tools/tk_diff.py --lines  --chip --version 1.3        679/679
python tools/tk_diff.py --lines  --chip --emulator --version 3.1.2
python tools/tk_diff.py --vfuzz 300 --chip --emulator --version V
python tools/tk_diff.py --cmdfuzz 200 --chip --emulator --version V

python tools/seam_diff.py --version V        268/268 declining is free
python tools/engine_diff.py --refs           5 of 9 on v3.1.3, 8 of 9 on 1.3

build/native/tk_profile --disk <disk.dsk>    what the engine found
build/native/tk_extract <disk.dsk> --list    what is on a disk
```

`<romdir>` is any directory holding image pairs, disk images and
`.dict` files; the driver test stages all three and offers each as a
voice or a setting.

**All four known builds are exact** against the emulator at the chip
bytes -- 3.1.3, 1.3, and the two different 3.1.2s carried by the disks
Jayson calls PIZZA.DSK and bex21my.dsk: 679/679 whole lines, 300/300
Ctrl-V phoneme strings and 200/200 Ctrl-E command streams each.

**The images and disks to test against are Jayson's own and are not in
the repository**; he keeps them in `tmp/`, which is also where any WAV
he is meant to listen to should be written -- `out/` is scratch and he
has to hunt for things left there.

## Key facts established (do not re-derive)

- **Each engine holds its OWN copy of Textalker's voice state**, and
  `echotalk_set_engine()` therefore marks the settings dirty so the
  block is pushed again. The settings are Ctrl-E commands in the input
  stream and the stream goes to one engine at a time, so everything sent
  before a switch reached the other engine's Textalker. Jayson found it
  from the add-on: compressed speech stayed ticked in the UI, the voice
  slowed down on switching to native, and un-ticking and re-ticking the
  box was the only way back -- that being what marked the settings dirty
  again. `test_dll.py` now checks a setting survives a switch.
- **`modes_chosen` had no reader.** `echotalk_set_letter_mode()` and
  `echotalk_set_punctuation()` moved the library's copy of the modes and
  set the flag, `apply_settings()` never looked at it, and so the
  command never went out -- although the header said it did, the getter
  agreed with the caller, and a one-character utterance's restore used
  the value. Found while fixing the engine switch, because it is the
  same fault: the library's model of Textalker drifting away from
  Textalker. The two are now sent, still only once a caller has chosen
  them, which is what keeps them off every other caller's byte stream.
- **Both Textalker versions run from one binary.** The OBJ load address
  comes from the trampoline templates in the loader (`$D0xx` -> `$D000`,
  `$D4xx` -> `$D400`); the character entry comes from running the loader,
  calling `$BA69` with the Z flag set, and reading CSWL at `$36`/`$37`
  (`$BA7C` for v3.1.3, `$BA82` for v1.3). Nothing is keyed to a specific
  build, so 3.1.2/3.1.4 should work unrecognised.
  `notes/multi_version_support_design.md`
- **Textalker lives in the language card at `$D000-$FFFF`, shadowing the
  monitor ROM.** `LDA $C08B` selects the card, `LDA $C08A` selects ROM.
  Both harnesses model this with an all-RTS ROM shadow; a flat memory
  model silently writes "ROM stubs" over Textalker's image.
- The loader returns with **ROM** selected, so jumping straight at the
  OBJ entry lands in the shadow. Always enter via the trampoline.
- **The runaway guards are not tuning knobs, and a non-zero
  `echotalk_overruns()` is always a fault.** `STEP_BUDGET` was 5,000,000
  and needed 20,000,000 at the slowest exposed settings; it truncated
  speech in complete silence. It is 64,000,000 now and every overrun is
  counted and reported. `notes/step_budget_truncation.md`
- **`echotalk_chunk_text()` stops when the caller's array fills and says
  nothing about the text it never reached.** Callers must loop.
  `src/echotalk.c` does; **`tools/render_common.h` still calls it once
  with a 256-entry array** and silently drops the tail of any line
  needing more (20,480 characters at the default chunk size). Worth
  fixing; left alone so far to avoid disturbing the reference harness.
- **Textalker's own line-buffer bound is never initialised here.**
  `$FD80-$FD82` read as zero because `$D781` is never reached — nothing
  plays the part of DOS setting up a screen. Its auto-flush point is
  therefore undefined, which is why chunking is not optional.
- **Text preparation changes lengths**, so a character offset taken
  before it does not survive it. Anything tracking offsets must prepare
  in runs and record offsets in the prepared buffer.
- Compressed/expanded speech works by skipping phoneme segments, not by
  changing playback rate.
- v1.3 does not implement the `D` (inter-word delay) command at all.
- Textalker never uses 6502 decimal mode (measured), though BCD is
  enabled anyway for correctness.
- `TMS5220_READ_COMMAND_DELAY_SAMPLES = 600` in `tms5220_core.c` is still
  a tuned hack, not derived timing. It survived the pacing investigation
  but was never the cause of anything; treat with suspicion if it ever
  seems implicated.
- True timing (`ECHOTALK_TRUE_TIMING=1`) is implemented, produces
  byte-identical output, and no longer clobbers anything. There is no
  reason to switch the default. The reservation in
  `notes/true_timing_implemented_not_the_cause.md` no longer applies.

### Echo/Textalker command reference

All prefixed with Ctrl-E (`\x05`) unless noted. Case-insensitive.

`nP` pitch 0-63 (default 24, normal intonation), `nF` same pitch but
monotone, `nV` volume 0-15 (default 12), `nD` inter-word delay 0-15
(v3.1.3 only), `C`/`E` compressed/expanded, `T` talk-only, `nR` repeat
filter, `L`/`W` letter/word mode, `A`/`S`/`N` all/some/no punctuation,
`\x16` (Ctrl-V) phoneme mode terminated by CR.

**Send `\x05` `99` `R` once after init** to disable the repeat filter,
which otherwise speaks `EEEEEEEEE` as `EE`. The tools do this by default.

## Untested ground

Everything above is verified on **x86 Windows**, plus one Linux run.
What has not been touched:

- **ARM is entirely untried** — no Apple Silicon, no Raspberry Pi. Jayson
  has no ARM hardware. The bit-identical Windows/Linux result was between
  two x86 builds and says nothing about a different architecture, which
  is precisely where a floating-point difference would be plausible.
  Re-run `make listen` and compare MD5s the first time anyone has one.
- **The 32-bit build has never run on 32-bit hardware.**
  `make test-dll-load` runs the real 32-bit binary against the real
  32-bit DLL, but under WoW64. That covers the code being 32-bit, not the
  machine.
- **macOS** has never been built or run.

**Cross-platform determinism is a recorded baseline, not an assumption.**
Windows and Linux produced byte-identical WAVs — same MD5 over 1,096,604
samples — despite doubles running through the lattice filter, the
cycle-to-sample accumulator and the resampler. So a future cross-platform
difference is a real bug, not drift.

## The desktop GUI

`gui-native/` is a Win32 GUI over the library, built with MSVC
(`gui-native\build.cmd`, x64), exposing every setting with Preview, Render
to WAV, presets, batch render and a pitch sweep. `tools/verify_gui.py`
checks it byte for byte against the add-on's shipped DLL (114 cases),
`tools/verify_gui_keyboard.py` by real keystrokes and MSAA, and
`tools/verify_gui_smoke.py` live. Read `gui-native/README.md`; the plan,
and what building it corrected, are in `ROADMAP.md`. **Not yet done: a
listening pass with NVDA running.**

## What is left

Nothing is blocking. In rough order of value:

1. **Ship it.** The add-on works in real NVDA, and how other people
   supply Textalker images is now answered: `tk_extract` reads an
   Apple II disk image, and the add-on takes one directly as a voice.
   What is left is packaging and an announcement.
2. Fix the `render_common.h` chunker truncation noted above.
3. Delete the historical probes in `tools/`.
4. ~~Pitches 60 through 99.~~ **Measured 2026-09-15** with the GUI's
   pitch sweep: with `nP`, 64 and 65 are real extra steps and 65–99 are
   identical; with `nF`, 63 is the ceiling. `\x05 99P` differs from
   `\x05 63P` because it is `65P`. Both versions agree.
   `notes/pitch_above_63.md`. The library still clamps to 63.
5. The unmapped-character policy in `text_prep` is a UX decision worth
   revisiting.
6. Re-measure the baseline table with `say` if the library is to become
   the reference rather than the harness.
7. The v3.1.3 Ctrl-V phoneme-mode restart, a one-to-two sample chip
   phase difference. The recommendation is still to leave it.
8. A **1.x with a different workspace** would need work: the
   phoneme/input buffer pair is found in the code, but the dozen
   Ctrl-E variables hang off it at offsets that are a family constant.
   Nothing known needs this.

## Working practices that paid off

- **Ask for a discriminating test rather than guessing.** Twice in
  session 11 a single observation from Jayson replaced a wrong theory
  that had already produced code. Both times the theory was plausible and
  both times the code would have been useless.
- **When the question is "what does that other program do", ask that
  program.** Fifteen minutes of logging what NVDA actually sends settled
  what two rounds of reasoning had not.
- **Diff against the unmodified MAME source in `third_party/tms5220/`.**
  Four defects came from the mechanical port, including a missing
  `#define` that caused the pacing bug and was invisible to `-Wall`.
- **A limit that can truncate must report it.** The step budget cut
  speech in silence. Picking a bigger number fixes today's bug; making it
  impossible to hit one quietly is what stops the next.
- **Check the magnitude before chasing a mechanism.** True timing was
  implemented in full before anyone noticed its delays are 13–25 µs
  against a 25 ms symptom.
- **Verify a regression test fails on the broken code.** A test that
  passes either way proves nothing. Done twice in session 11, and the
  first version of one of them did not catch its own bug.
- **Do not assert exact equality of audio.** Four checks broke this way
  in one session: comparing whole buffers when the claim was about
  content, and a one-sample alignment shift making sample-wise
  comparison meaningless. Compare content, allow a small shift, and say
  what you actually mean.
- **In MAME's log, trust counts, not timing.** Frame counts, byte counts
  and event counts are reliable; anything derived from ordering across
  the CPU/chip boundary is not. MAME's recorded audio is not chip time
  either — its own counter showed 12,911 samples where the `.wav` held
  11,200.
