# EchoTalk — a from-scratch Textalker / Echo II emulator

A C library — Windows DLL and Linux `.so` — that emulates Street
Electronics' TEXTALKER driving a TMS5220 on an Apple II Echo II card, so
it can be used as a speech backend. It ships with an **NVDA synthesizer
add-on**, plus CLI tools that turn text into a WAV.

By default it works by running the **real Textalker binary** under a
6502 emulator, driving a port of MAME's TMS5220 emulation, so nothing
about the speech is approximated. A second engine runs Textalker's whole
speech path as native C with no 6502 in it, byte-exact against the
first. **Both read their tables out of a Textalker image you supply** —
see [why that does not change](#why-the-native-engine-still-needs-the-images).

## Status

**Complete and working, with no known open bugs.** The add-on runs in
real NVDA.

- Speech matches a real-hardware capture exactly in amplitude
  (−12127/+26833 for "HI") and matches MAME frame-for-frame and
  byte-for-byte on the Echo II bus.
- **Four Textalker builds** run from one binary — 3.1.3, 1.3 and two
  different 3.1.2s — with the load address, the entry point and every
  internal table discovered from the files themselves rather than
  configured. Each becomes a voice, labelled from its own banner.
- **Apple II disk images are read directly.** `tk_extract` pulls the
  pair off a `.dsk`/`.do`/`.po`/`.2mg`, and the library will simply open
  one, so there is no extraction step at all. Textalker is found on a
  disk structurally, never by filename.
- **Two engines.** The 6502 emulation is the default and the reference;
  a **native C rewrite** of Textalker's whole speech path runs the same
  images and is byte-exact against it at the chip. The rewrite is what
  makes the next two possible.
- **Pronunciation dictionaries** in CMUdict format, read at run time —
  EchoTalk ships none — with respellings (`BOOT =booht`) and Street
  Electronics' own Ctrl-V notation accepted alongside ARPABET. A
  dictionary belongs to one Textalker version; see below.
- **Longer utterances.** The 80-character chunk limit was the
  emulation's, not Textalker's; the native engine takes 130 or 200, and
  its line buffer can be grown further as an explicit, documented
  divergence.
- Text handling: chunking at clause/word boundaries, UTF-8 and legacy
  encodings folded to the 7-bit ASCII Textalker understands, LF
  stripping, and Textalker's repeat filter disabled so `EEEEEEEEE` is not
  spoken as `EE`.
- **Streaming** — `echotalk_speak()` queues and returns; `echotalk_read()`
  synthesises an utterance at a time, so the first words are audible in
  39 ms rather than after the whole passage is made. Synthesis runs at
  90–155x real time, so it needs no thread of its own.
- **Index events** for host progress reporting.
- Builds as a self-contained `echotalk.dll` — 56 undecorated cdecl
  exports, no MinGW runtime to ship, loadable straight from Python with
  `ctypes`. The Linux `.so` produces **byte-identical** audio to the
  Windows build: same MD5 over all 1,096,604 samples of the test run,
  despite doubles running through the lattice filter and the resampler.

Five controls cover rate and pitch independently. Textalker's pitch
command changes pitch alone; a **continuous speed control** changes speed
alone, with the fundamental measured holding at 129 Hz from half speed to
triple; the TMS5220's own frame rate offers four fixed steps; the clock
multiplier changes both, for the sped-up-tape character; and the output
sample rate changes neither.

**A native desktop GUI** — every add-on setting plus the ones the add-on
hides, Preview, Render to WAV, presets, batch render and a pitch sweep — is
in [gui-native/](gui-native/README.md): `gui-native\build.cmd`, then
`gui-native\build\echotalk_gui-x64.exe`. Its output is byte-identical to
the add-on's in 114 checked cases. The plan and its progress are in
[ROADMAP.md](ROADMAP.md).

**Start with [HANDOFF.md](HANDOFF.md)** — current state, build and run
instructions, reference baselines, and the facts worth not re-deriving.
`notes/` holds the investigation writeups, and
[nvda-addon/README.md](nvda-addon/README.md) covers the add-on.

## Quick start

Build with MSYS2 on Windows, or plain gcc elsewhere:

```
make native
make test          # pure-logic unit tests, no ROMs needed
make dll           # echotalk.dll, 32- and 64-bit
make test-dll      # load it from Python, as NVDA does
make listen        # a self-narrating WAV to check by ear
```

Then, given a Textalker loader and OBJ image — see
[Obtaining the Textalker images](#obtaining-the-textalker-images) below —
speak through the library:

```
say loader.bin obj.bin "Hello." out.wav
say --file story.txt --speed 1.5 --compressed loader.bin obj.bin out.wav
```

or drive the emulation directly with the diagnostic harness, which takes
raw bytes including Echo control codes:

```
render_text_loader loader.bin obj.bin input.bin out.wav
```

`say` also takes `--disk <image>` instead of the pair, and
`tk_extract <image>` pulls the pair out of an Apple II floppy image if
you want the files themselves.

Both work with either Textalker version — they read which one they have
from the loader rather than being told.

## Obtaining the Textalker images

This repository does not include Textalker binaries for legal reasons.
This appears to be one of those sad situations where nobody seems to know
who actually owns the intellectual property and could give permission for
their distribution. However, these files were widely distributed on Apple
II floppy disks, which are readily available. To obtain them, extract the
appropriate files from Apple II DOS 3.3 images you already have. If you
do not have disk images with these files, do the following.

First, download <https://bluegrasspals.com/mameapple.zip> and unzip it.
In the root folder of that zip is `incorrect.dsk`, which carries
Textalker 3.1.3; `Disks/brlboot.dsk` carries Textalker 1.3.

**You do not need any other tool.** `tk_extract` reads Apple II DOS 3.3
disk images directly:

```
tk_extract incorrect.dsk --out roms/
tk_extract Disks/brlboot.dsk --out roms/
```

That writes `textalker.ram.bin` + `textalker.obj.bin` and
`textalker_v13.ram.bin` + `textalker_v13.obj.bin`, named from the
version it finds — **every** Textalker on the disk, since one disk can
carry several. `--list` shows a disk's catalogue first if you want to
look. It takes `.do`, `.dsk`, `.po` and `.2mg`, works out the sector
order for itself, and finds Textalker structurally rather than by
filename — so a release nobody has catalogued works too.
`notes/disk_image_extraction.md` has the details.

**Or skip extraction entirely.** The library opens a disk image itself:

```
say --disk incorrect.dsk "Hello." out.wav
```

and the NVDA add-on treats a disk image dropped beside the driver as a
voice, exactly as it treats an extracted pair. Nothing has to be
unpacked at all.

If you would rather do it by hand, a tool like
[CiderPress](https://a2ciderpress.com) still works: extract
`Textalker.ram` and `Textalker.obj` from `incorrect.dsk` into `roms/` as
`textalker.ram.bin` and `textalker.obj.bin`, and `Textalker.ram` and
`Textalker.ram.obj` from `brlboot.dsk` as `textalker_v13.ram.bin` and
`textalker_v13.obj.bin`.

Nothing is keyed to a particular build, so other 3.1.x releases should
work without being recognised individually — the load address and entry
point are discovered from the files themselves. Each pair you supply
becomes a voice, labelled from its own banner.

### Why the native engine still needs the images

The obvious question about a native C rewrite is why it does not simply
remove the requirement. The answer is that **what the rewrite replaced
is Textalker's CODE, not Textalker's DATA**, and the data is the
proprietary part.

Textalker is a table-driven program. Its knowledge of English is not in
the 6502 instructions; it is in the tables, and in v3.1.3 those are
**4,846 bytes of the 12,282 — two fifths of the image**:

```
letter-to-sound rules         $F0AB-$FCC3   3097   568 rules
allophone pointers + frames   $EA93-$EF23   1169   the VOICE itself
dispatch table                $EFDF-$F0AA    204
syllabifier word lists        $E127-$E1CF    169   9 prefixes, 20 suffixes,
                                                   15 digraphs, 26 clusters
attribute table               $EF24-$EFA3    128
Ctrl-V map + both pitch tables $EA44-$EA92     79
```

That is Street Electronics' work — v3.1.3 carries an American Printing
House for the Blind copyright too — and it is what EchoTalk has no right
to distribute. So **no table is compiled in**, deliberately and as a
rule of the whole project: every one of the above is read out of the
user's own image at run time, by both engines equally. It is why the
library is BSD-3-Clause and the add-on can be given away.

So rewriting more of Textalker would not change the requirement, and
neither would going further than this project has. A hybrid — v1.3's
enumerated word rules with v3's syllabifier, say, which the architecture
already permits since the engine and the image are independent
choices — would still be reading somebody else's rules and somebody
else's voice out of a file the user supplied. The engine can be given
away; the voice cannot.

`notes/textalker3_rule_changes.md` is what those tables look like when
you read them, if the idea of a Franken-Textalker appeals.

## Pronunciation dictionaries

The native engine takes a pronunciation dictionary in **CMUdict's
format**, so `cmudict.dict` works unmodified, and it is read at run time
— EchoTalk ships none. It is an override list: words it names are said
its way, and everything else, punctuation included, still goes through
Textalker's own rules, so the voice and the intonation are untouched.
Three spellings are accepted and can be mixed in one file:

```
NICKNAME  N IH1 K N EY2 M      ARPABET, CMUdict's own
BOOT      =booht               a respelling, through Textalker's rules
ECHOTALK  /3EKOT3*K            Street Electronics' Ctrl-V notation
```

```
say --native --dict examples/pronunciations.dict loader.bin obj.bin "..." out.wav
```

**A dictionary belongs to one Textalker version.** The versions do not
mispronounce the same words, so an entry that fixes one can be useless
or a small loss on the other, and there is no way for a file to say
which it is for. Keep one per version.

`ROUTE` is the clearest case. Both versions rhyme it with *about*. To
make it rhyme with *shoot*, Textalker 1.3 takes `ROUTE =root`, because
1.3 says ROOT correctly — while under 3.1.3 that line does nothing
whatever, since 3.1.3 says ROOT wrongly in exactly the way it says BOOT.
There it has to be `ROUTE =rooht`. It runs the other way too: `BOOT
=booht` is right for 3.1.3 and a small loss under 1.3, which says BOOT
correctly already and would only gain the faint break the H puts in the
middle of the word. Some entries are safe in both — `NICKNAME =nicname`
gives 1.3 the pronunciation it had anyway — but that is a thing to check
per word.

`examples/pronunciations.dict` is a worked file, and
`nvda-addon/doc/en/readme.md` covers the same ground for add-on users.
`notes/phoneme_alphabet.md` is where the ARPABET correspondence came
from — measured out of Textalker, not assumed.

## The NVDA add-on

`nvda-addon/` holds a working NVDA speech synthesizer driver. Build the
DLLs, copy them and your Textalker images into
`nvda-addon/synthDrivers/echotalk/`, then run `./build_addon.sh`. It
exposes voice, rate, pitch, volume, word delay, repeat filter, chip
clock, output sample rate, monotone and compressed speech.

`build_addon.sh` is public-safe by default and excludes the proprietary
Textalker images; `--with-images` bundles them for personal use only. See
[nvda-addon/README.md](nvda-addon/README.md).

## License

EchoTalk is **BSD 3-Clause**. See [LICENSE](LICENSE). That covers
everything written for this project: the library in `src/`, the tools in
`tools/`, the NVDA add-on in `nvda-addon/`, the build system and the
documentation.

Two things it does not cover, both above:

- **`third_party/`** keeps its own licences, which are compatible — see
  below and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
- **The Textalker images are not licensed by anyone here.** They are the
  original commercial software and are neither included nor covered; see
  "Obtaining the Textalker images" above.

The NVDA add-on is additionally distributed under the **GPL v2**, as NVDA
add-ons must be. BSD-3-Clause is one-way compatible into the GPL, so the
same source serves both. The add-on package carries its own copy of the
GPL in `doc/en/copying`.

## Third-party code

Vendored in `third_party/`, all permissively licensed and safe for a
closed or open distribution provided the notices are kept. See
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

- **Fake6502** — Mike Chambers' 6502 core, public domain.
- **MAME's TMS5220** — BSD-3-Clause. `third_party/tms5220_core/` is our
  standalone C port of it and is what actually ships;
  `third_party/tms5220/` holds the unmodified MAME source as reference.
  Diffing against that reference has found four real defects in the port,
  so keep it.
- **MAME's `a2echoii`** — BSD-3-Clause, reference only. The Echo II bus
  protocol modelled here comes from it.

`roms/` is where the Textalker images you extract yourself go. Those are
proprietary Street Electronics / American Printing House for the Blind
software, covered by none of the above, and are not distributed here.

## How it fits together

```
text in
  -> text_prep      modern encodings -> 7-bit ASCII, LF stripped
  -> chunker        split at clause/word boundaries to fit the buffer
  -> 6502 emulation running the real Textalker loader and engine
  -> TMS5220 core   LPC synthesis
  -> 8 kHz PCM out  (resampled if a different rate is asked for)
```

The Apple II side is modelled only as far as Textalker actually needs:
the Echo II card's two latches at `$C0A0-$C0AF`, the language-card bank
switches at `$C08A`/`$C08B`, and a stand-in "monitor ROM" of RTS
instructions so that any call into ROM territory returns harmlessly. No
Apple ROM or DOS code is reproduced.

## Project history

The narrative of how this was worked out — the byte-level protocol
discovery, the calling convention, eliminating the DOS loader dependency,
the TMS5220 port, and the long investigations into the onset glitch and
speech pacing — lives in `notes/`, indexed by `notes/README.md`. HANDOFF
summarises the conclusions; the notes keep the reasoning, including the
approaches that turned out to be wrong and why.

## Written by an AI

The code was written by Claude, Anthropic's AI, working as an agentic
coder through Claude Code. Jayson Smith directed the work, supplied the
Textalker disks, made the design decisions and — importantly — did the
listening. Several real bugs were found by ear and would not otherwise
have been caught, including a speech-pacing fault that had survived five
sessions of automated measurement saying everything was fine.
