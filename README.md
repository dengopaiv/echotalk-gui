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

Build with MSYS2 (**not** the Cygwin `gcc` that may be on PATH):

```
make native
make test          # pure-logic unit tests, no ROMs needed
make dll           # echotalk.dll, 32- and 64-bit
make test-dll      # load it from Python, as NVDA does
make listen        # a self-narrating WAV to check by ear
```

Then, given a Textalker loader and OBJ image, speak through the library:

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

## The NVDA add-on

`nvda-addon/` holds a working NVDA speech synthesizer driver. Build the
DLLs, copy them and your Textalker images into
`nvda-addon/synthDrivers/echotalk/`, then run `./build_addon.sh`. It
exposes voice, rate, pitch, volume, word delay, repeat filter, chip
clock, output sample rate, monotone and compressed speech.

`build_addon.sh` is public-safe by default and excludes the proprietary
Textalker images; `--with-images` bundles them for personal use only. See
[nvda-addon/README.md](nvda-addon/README.md).

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

`roms/` holds Textalker images extracted from user-supplied disks. Those
are proprietary Street Electronics / American Printing House for the
Blind software, covered by none of the above, and are not distributed
here.

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
