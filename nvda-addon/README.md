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

4. Package it:

       ./build_addon.sh                # public-safe: no images bundled
       ./build_addon.sh --with-images  # personal build — never distribute

5. Open the resulting `.nvda-addon` on the Windows machine, then choose
   **EchoTalk (emulated Echo II)** in NVDA's synthesizer dialog.

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
| Output sample rate | 8 kHz is what the card produced. Higher rates only resample — they add no detail, and exist because some output devices prefer their own rate. |
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

## Notes

- **Text is sanitised.** Ctrl-D, Ctrl-E and Ctrl-V are stripped from
  anything that came off the screen, because the pipeline would otherwise
  obey them as commands — a document containing one would silently change
  the voice. They are replaced with a space rather than deleted, so the
  rest of the line still gets spoken.
- **Index reporting is exact.** The library records the sample position of
  every mark as it generates the audio, rather than estimating, so
  progress reporting is ground truth.
- **One voice at a time.** The 6502 core keeps its registers in globals,
  so only one instance can exist. Switching voices tears the machine down
  and boots the other images, which costs under 10 ms and re-applies every
  setting — a fresh Textalker comes up at its own defaults.

## Licensing

The emulation is public domain (Fake6502) and BSD-3-Clause (MAME's
TMS5220 and `a2echoii`); `THIRD_PARTY_LICENSES.txt` is placed beside the
driver by the build script and must stay with it. NVDA is GPL-2 with an
explicit exception permitting non-GPL synthesizer drivers.

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
