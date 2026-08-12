# EchoTalk — a from-scratch Textalker / Echo II emulator

A C library — Windows DLL and Linux `.so` — that emulates Street
Electronics' TEXTALKER driving a TMS5220 on an Apple II Echo II card, so
it can be used as a speech backend. It ships with an **NVDA synthesizer
add-on**, plus CLI tools that turn text into a WAV.

It works by running the **real Textalker binary** under a 6502 emulator,
driving a port of MAME's TMS5220 emulation. Nothing about the speech is
reimplemented or approximated.

## Status

**Complete and working, with no known open bugs.** The add-on runs in
real NVDA.

- Speech matches a real-hardware capture exactly in amplitude
  (−12127/+26833 for "HI") and matches MAME frame-for-frame and
  byte-for-byte on the Echo II bus.
- Both Textalker **3.1.3 and 1.3** run from one binary, with the load
  address and entry point discovered from the files themselves rather
  than configured. Each becomes a voice, labelled from its own banner.
- Text handling: chunking at clause/word boundaries, UTF-8 and legacy
  encodings folded to the 7-bit ASCII Textalker understands, LF
  stripping, and Textalker's repeat filter disabled so `EEEEEEEEE` is not
  spoken as `EE`.
- **Streaming** — `echotalk_speak()` queues and returns; `echotalk_read()`
  synthesises an utterance at a time, so the first words are audible in
  39 ms rather than after the whole passage is made. Synthesis runs at
  90–155x real time, so it needs no thread of its own.
- **Index events** for host progress reporting.
- Builds as a self-contained `echotalk.dll` — 44 undecorated cdecl
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

First, download <https://bluegrasspals.com/mameapple.zip> and unzip it. In
the root folder of that zip is a file called `incorrect.dsk`. Using a tool
like [CiderPress for Windows](https://a2ciderpress.com), extract
`Textalker.ram` and `Textalker.obj` from that disk. Put them in this
repository's empty `roms/` folder, renamed to `textalker.ram.bin` and
`textalker.obj.bin`. That is Textalker 3.1.3.

Then, from `brlboot.dsk` in the mameapple package's `Disks` folder,
extract `Textalker.ram` and `Textalker.ram.obj`. Put those in `roms/` too,
renamed to `textalker_v13.ram.bin` and `textalker_v13.obj.bin`. That is
Textalker 1.3.

Nothing is keyed to a particular build, so other 3.1.x releases should
work without being recognised individually — the load address and entry
point are discovered from the files themselves. Each pair you supply
becomes a voice, labelled from its own banner.

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
