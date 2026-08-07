# EchoTalk — a from-scratch Textalker / Echo II emulator

A C library (Windows DLL and Linux .so) that emulates Street
Electronics' TEXTALKER driving a TMS5220 on an Apple II Echo II card, so
it can be used as a speech backend — an NVDA synth driver in
particular — plus CLI tools that turn text into a WAV.

It works by running the **real Textalker binary** under a 6502 emulator,
driving a port of MAME's TMS5220 emulation. Nothing about the speech is
reimplemented or approximated.

## Status

**The emulation is complete and validated, the library works, it builds
as a DLL, it streams, and it reports index events.** Everything the NVDA
backend needs is in place.

- Speech matches a real-hardware capture exactly in amplitude
  (-12127/+26833 for "HI") and matches MAME frame-for-frame and
  byte-for-byte on the Echo II bus.
- Both Textalker **3.1.3 and 1.3** run from one binary, with the load
  address and entry point discovered from the files themselves rather
  than configured.
- Text handling is done: chunking at clause/word boundaries, UTF-8 and
  legacy encodings folded to the 7-bit ASCII Textalker understands, LF
  stripping, and Textalker's repeat filter disabled so `EEEEEEEEE` is
  not spoken as `EE`.
- Every bug found so far is fixed, including a speech-pacing defect
  traced to a `#define` lost during the port from MAME, and a
  single-character utterance that also announced its own line
  terminator as "return".

- The library is in `src/echotalk.[ch]`: create it with a loader and OBJ
  image, set pitch, volume, word delay, repeat filter, expanded or
  compressed speech, speed and output rate, then speak and pull PCM.
- Driver settings can also be embedded in the text as Ctrl-D commands,
  mirroring the shape of Textalker's own Ctrl-E ones — `\x04 2F` for
  frame rate, `\x04 0.75C` for the clock, and so on.

Four controls cover rate and pitch independently — Textalker's pitch
command changes pitch alone, the TMS5220 frame rate changes speed alone,
the clock multiplier changes both for the sped-up-tape character, and
the output sample rate changes neither.

It also builds as a self-contained `echotalk.dll` — 24 undecorated cdecl
exports, no MinGW runtime to ship, loadable straight from Python with
`ctypes` the way NVDA will. Both a Python and a C test drive it through
that boundary; the C one exists so the 32-bit build gets tested too. The
Linux `.so` builds and runs, and produces **byte-identical** audio to
the Windows build — same MD5 over all 1,096,604 samples of the test
run, despite doubles running through the lattice filter and resampler.

Speech streams: `echotalk_speak()` queues text and returns, and
`echotalk_read()` synthesises an utterance at a time, so the first words
are audible in 39 ms rather than after the whole passage is made.
Synthesis runs at roughly 136x real time, so it needs no thread of its
own. Index marks embedded in the text report progress exactly, since the
sample counts they refer to are ones we generated rather than estimated.

**Start with [HANDOFF.md](HANDOFF.md)** — it has the current state,
build and run instructions, reference baselines, and the facts worth not
re-deriving. `notes/` holds the detailed investigation writeups.

## Quick start

Build with MSYS2 (**not** the Cygwin `gcc` that may be on PATH):

```
make native
make test          # pure-logic unit tests, no ROMs needed
make dll           # echotalk.dll, 32- and 64-bit
make test-dll      # load it from Python, as NVDA would
```

Then, given a Textalker loader and OBJ image, speak through the library:

```
say loader.bin obj.bin "Hello." out.wav
say --file story.txt --compressed --frame-rate 2 loader.bin obj.bin out.wav
```

or drive the emulation directly with the diagnostic harness, which takes
raw bytes including Echo control codes:

```
render_text_loader loader.bin obj.bin input.bin out.wav
```

Both work with either Textalker version — they read which one they have
from the loader rather than being told.

## Third-party code

Vendored in `third_party/`, all permissively licensed and safe for a
closed or open distribution provided the notices are kept. See
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

- **Fake6502** — Mike Chambers' 6502 core, public domain.
- **MAME's TMS5220** — BSD-3-Clause. `third_party/tms5220_core/` is our
  standalone C port of it and is what actually ships;
  `third_party/tms5220/` holds the unmodified MAME source as
  reference. Diffing against that reference has found four real defects
  in the port, so keep it.
- **MAME's `a2echoii`** — BSD-3-Clause, reference only. The Echo II bus
  protocol modelled here comes from it.

`roms/` holds Textalker images extracted from user-supplied disks. Those
are proprietary Street Electronics / APH software, covered by none of
the above, and how an end user supplies them is a question a distributed
build has to answer separately.

## How it fits together

```
text in
  -> text_prep      modern encodings -> 7-bit ASCII, LF stripped
  -> chunker        split at clause/word boundaries to fit the buffer
  -> 6502 emulation running the real Textalker loader and engine
  -> TMS5220 core   LPC synthesis
  -> 8 kHz PCM out
```

The Apple II side is modelled only as far as Textalker actually needs:
the Echo II card's two latches at `$C0A0-$C0AF`, the language-card bank
switches at `$C08A`/`$C08B`, and a stand-in "monitor ROM" of RTS
instructions so that any call into ROM territory returns harmlessly. No
Apple ROM or DOS code is reproduced.

## Project history

The narrative of how this was worked out — the byte-level protocol
discovery, the calling convention, eliminating the DOS loader
dependency, the TMS5220 port, and the long investigations into the onset
glitch and speech pacing — lives in `notes/`, roughly in chronological
order. `HANDOFF.md` summarises the conclusions; the notes keep the
reasoning, including the approaches that turned out to be wrong and why.
