# EchoTalk — NVDA add-on

An NVDA speech synthesizer that is an Apple II Echo II card: Street
Electronics' original Textalker software running on an emulated 6502,
driving an emulation of the TMS5220 the card was built around. Nothing
about the speech is imitated or approximated — it is produced by running
the real program.

Built on the library in the parent directory. See `../HANDOFF.md` for how
that works and `../notes/` for how it was worked out.

## Building

1. Build the DLLs from the repository root:

       make dll

   That produces `build/win64/echotalk.dll` and `build/win32/echotalk.dll`,
   each self-contained: `-static` leaves them depending only on
   `KERNEL32.dll` plus the C runtime the subsystem implies (UCRT for
   64-bit, MSVCRT for 32-bit), both of which ship with Windows. NVDA users
   have no MinGW runtime, so a stray `libwinpthread-1.dll` dependency shows
   up as the driver silently failing to load. Check with:

       objdump -p build/win64/echotalk.dll | grep 'DLL Name'

2. Copy them in under the names the driver looks for. NVDA 2025.2+ is a
   64-bit process; earlier versions are 32-bit, and the driver picks by
   its own bitness:

       cp ../build/win64/echotalk.dll synthDrivers/echotalk/echotalk64.dll
       cp ../build/win32/echotalk.dll synthDrivers/echotalk/echotalk32.dll

3. Copy in your Textalker images, one pair per version:

       cp ../roms/textalker.ram.bin ../roms/textalker.obj.bin synthDrivers/echotalk/
       cp ../roms/textalker_v13.ram.bin ../roms/textalker_v13.obj.bin synthDrivers/echotalk/

   A pair is `<name>.ram.bin` (or `<name>.loader.bin`) plus
   `<name>.obj.bin`. The driver finds every pair sitting next to it, boots
   each one briefly to read its version banner, and offers it as a voice
   under that name — so images this code has never seen still appear
   correctly labelled rather than as "unknown".

   **These files are not distributed here.** They are the original
   commercial Textalker, proprietary to Street Electronics, with the 3.1.3
   release also carrying an American Printing House for the Blind
   copyright. Supply your own.

4. The user documentation in `doc/en/` needs **pandoc** on PATH, which
   converts each `.md` there to the `.html` NVDA actually opens. Install
   it from <https://pandoc.org/> if `pandoc --version` says nothing.

   Without pandoc the build reuses whatever HTML is already there and
   says so; it refuses to package at all if a file is missing, rather
   than shipping an add-on whose help menu leads nowhere. `manifest.ini`
   names `readme.html`, so that one has to exist.

5. Package it:

       ./build_addon.sh                # public-safe: no images bundled
       ./build_addon.sh --with-images  # personal build — never distribute

6. Open the resulting `.nvda-addon` on the Windows machine, then choose
   **EchoTalk (emulated Echo II)** in NVDA's synthesizer dialog.

## Documentation

`doc/en/` holds what the user reads, and NVDA opens it from the add-on's
entry in the Add-on Store or the Tools menu.

| File | |
|---|---|
| `readme.md` | The add-on's own README — what it is, what every setting does. Edit this, not the HTML. |
| `THIRD_PARTY_LICENSES.md` | The notices, in the abbreviated form aimed at someone installing the add-on rather than reading the source. `../THIRD_PARTY_LICENSES.md` in the repo root stays the full version, and is what goes beside the driver as a `.txt`. |
| `copying` | The GNU GPL v2, verbatim, because an NVDA add-on is distributed under it. Not generated; shipped as-is. |

The `.html` files are build output and are gitignored. The Markdown
sources are excluded from the package.

## Testing without installing

From the repository root, and needing no NVDA:

    python tools/test_nvda_driver.py build/win64/echotalk.dll roms out.wav

That stubs the NVDA modules the driver imports and drives the real driver
against the real DLL — settings, index events, pitch commands, voice
switching, cancel, terminate — then writes what it spoke to a WAV. It
does not prove the settings look right in NVDA's own dialogs, but it
catches everything else before an install.

## Settings

| Setting | What it does |
|---|---|
| Voice | Which Textalker version. Only versions you supplied appear; with one installed there is nothing to switch to. |
| Rate | Speed, **pitch unchanged**. 50% is normal, every 25% doubles, so 25% is half speed and 75% is double. |
| Pitch | Textalker's own pitch, 0–63. Default 24. |
| Volume | Textalker's own volume, 0–15. Default 12. |
| Delay between words (Textalker 3 only) | Textalker 1.3 never implemented this command and discards it. |
| Repeat-character filter | Textalker collapses runs of the same character, so at its original setting `EEEEEEEEE` is spoken as `EE`. Default is high enough that it never triggers. |
| Chip clock | Over/underclocks the speech chip: speed **and** pitch together, the sped-up-tape effect. Quite different from Rate. |
| Output sample rate | 8 kHz is what the card produced. Higher rates only resample — they add no detail, and exist because some output devices prefer their own rate. A **floor, not a fixed value**: the chip clock scales what the chip produces, so this is raised to meet it rather than downsampling. See below. |
| Monotone | Flattens the intonation. Textalker treats this as part of the pitch setting, not a separate one. |
| Compressed speech | Textalker's own fast mode, which drops sounds rather than playing faster. |

The chip's own four-step frame rate is deliberately **not** exposed. The
continuous rate control above sounds better and is smooth; the frame rate
remains in the library for anyone who wants it.

### Rate, chip clock, and compressed are three different things

They compose, and they are worth understanding before reaching for the
wrong one:

- **Rate** changes how fast the chip walks through each frame of speech.
  Pitch does not move — measured, not assumed: the fundamental stays at
  129 Hz from half speed to triple.
- **Chip clock** pretends the chip runs at a different clock, so speed and
  pitch move together. Chipmunks.
- **Compressed** is Textalker's own idea of fast speech, achieved by
  leaving sounds out.

### The output rate is a floor, because the chip clock moves with it

The chip produces 8 kHz × the chip clock, so at 1.5x it is really
generating 12 kHz. Delivering that as 8 kHz means **downsampling**, and
the resampler has no anti-aliasing filter by design — everything above
the new Nyquist folds back into the audible band instead of being
removed. Detail the chip generated, discarded and turned into noise.

So `_effectiveSamplerate()` raises the rate to meet the chip whenever the
clock outruns it, and both the library and the `WavePlayer` are opened at
that rate. The user's own choice is stored untouched and applies again as
soon as the clock comes back down, so the settings dialog keeps showing
what they picked.

Reported by a user. `say` enforces the same floor and says so on stderr.

## Notes

- **Text is sanitised.** Ctrl-D, Ctrl-E and Ctrl-V are stripped from
  anything that came off the screen, because the pipeline would otherwise
  obey them as commands — a document containing one would silently change
  the voice. They are replaced with a space rather than deleted, so the
  rest of the line still gets spoken.
- **Say All reads a wrapped sentence as one sentence.** NVDA sends a
  whole Say All as a single sequence with an index mark between every
  line. Those marks used to end the utterance, which read a sentence
  split over several lines as several sentences. They no longer do.
- **Index positions are exact at an utterance boundary**, which is where
  almost all of NVDA's are, and interpolated by character offset inside
  one. The latter has to be approximate: Textalker emits nothing until
  the terminating CR, so there is no way to observe which character is
  being spoken.
- **One voice at a time.** The 6502 core keeps its registers in globals,
  so only one instance can exist. Switching voices tears the machine down
  and boots the other images, which costs under 10 ms and re-applies every
  setting — a fresh Textalker comes up at its own defaults.

## Licensing

The emulation is public domain (Fake6502) and BSD-3-Clause (MAME's
TMS5220 and `a2echoii`); `THIRD_PARTY_LICENSES.txt` is placed beside the
driver by the build script and must stay with it, and the same notices
appear in reader-facing form at `doc/en/THIRD_PARTY_LICENSES.html`. The
add-on itself is distributed under the GPL v2 in `doc/en/copying`. NVDA
is GPL-2 with an explicit exception permitting non-GPL synthesizer
drivers.

The Textalker images are covered by none of that and are not distributed
here.

## Diagnosing what NVDA sends

Create an empty file called `logsequences.txt` next to `__init__.py` in
the installed add-on and restart NVDA. Every speech sequence NVDA hands
the driver is then written to NVDA's log, like this:

```
EchoTalk seq [queued=0 gen=0]: text(12) 'First part. ' | Index(11) | text(13) 'Second part. ' | Index(22)
```

`queued` is how many sequences were already waiting to be spoken, which
says whether NVDA runs ahead or feeds one at a time. `Index(n)` marks
show where NVDA has placed its progress callbacks. Between them that is
enough to work out how NVDA splits a document rather than guessing.

Delete the file and restart to turn it off. It logs at info level, so it
will fill the log quickly — turn it on for one test, not for daily use.
