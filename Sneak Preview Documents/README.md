# EchoTalk — the Apple II Echo II speech synthesizer, on your PC

This turns text into a WAV file that sounds like an Apple II with a
Street Electronics Echo II card in it — the speech synthesizer a lot of
blind computer users heard first, back when a talking computer was a
new idea.

It does not imitate that sound. It runs the real thing: the actual
Textalker software from 1981 and 1985, executing on an emulated 6502
processor, driving an emulation of the TMS5220 speech chip the card was
built around. The bytes going into the speech chip are the same bytes a
real Echo II would have received. Speech produced this way has been
checked against a recording of real hardware and matches it exactly.

**This is a sneak peek.** The real goal is a screen reader voice for
NVDA. This program is how you can hear it in the meantime.

## Written by an AI

The code was written by Claude, Anthropic's AI, working as an agentic
coder through Claude Code. A human — Jayson Smith, `@jaybird110127` on `dragonscave.space` —
directed the work, supplied the Textalker disks, made the design
decisions and, importantly, did the listening. Several real bugs in this were found by ear and would not
otherwise have been caught, including a speech-pacing fault that had
survived five sessions of automated measurement saying everything was
fine.

Mentioned because you deserve to know how something you are running was
made, not as a boast. Treat it as you would any other code from an
unfamiliar source.

## What is in this folder

| File | What it is |
|---|---|
| `say64.exe` | The program, 64-bit |
| `say32.exe` | The program, 32-bit — use this on older Windows |
| `Textalker 3.1.3.bin` + `Textalker 3.1.3 Loader.bin` | Textalker 3.1.3 (1985) |
| `Textalker 1.3.bin` + `Textalker 1.3 Loader.bin` | Textalker 1.3 (1981) |
| `THIRD_PARTY_LICENSES.md` | Required licence notices — keep this with the program |

Nothing needs installing and nothing is written outside this folder.
Both programs are self-contained; there are no DLLs to copy.

Use `say32.exe` if you are unsure. It runs on every version of Windows.
`say64.exe` needs Windows 10 or newer unless you have the Universal C
Runtime update installed.

## The two voices

The two Textalker versions sound noticeably different, and both are
included because both are worth hearing.

- **3.1.3** is the later, more polished release, and the one most people
  remember.
- **1.3** is from 1981 and rougher around the edges. It also ignores a
  couple of commands the newer one understands, noted below.

They come in pairs — each version has a loader file and a main file, and
**the two must match**. Mixing a 3.1.3 loader with the 1.3 program will
not work, because the loader is what says where the program belongs in
memory. `say` prints which version it detected when it starts, so you
can always check.

## Running it

`say` writes a WAV file. It does not play audio itself — open the
resulting file in whatever you normally use.

The general shape is:

```
say64 [options] <loader file> <Textalker file> <text> <output.wav>
```

The filenames here contain spaces, so put quotes around them.

### Say something

```
say64 "Textalker 3.1.3 Loader.bin" "Textalker 3.1.3.bin" "Hello there." hello.wav
```

### Read a text file instead

Use `--file` and leave the text out:

```
say64 --file story.txt "Textalker 3.1.3 Loader.bin" "Textalker 3.1.3.bin" story.wav
```

Plain text files work best. Modern typographic characters — curly
quotes, dashes, accented letters, emoji — are converted to something
Textalker can pronounce, since it predates all of them and only knows
the original 128 ASCII characters.

### Hear the older voice

```
say64 "Textalker 1.3 Loader.bin" "Textalker 1.3.bin" "Hello there." old.wav
```

## Options

| Option | What it does |
|---|---|
| `--file PATH` | Read the text from a file. Leave out the text argument. |
| `--compressed` | Textalker's faster speech mode. It works by dropping bits of each sound rather than playing faster, so it has a character of its own. |
| `--speed MULT` | Speed, 0.25 to 4. **Pitch does not change at all.** This is the one you probably want. |
| `--frame-rate N` | The speech chip's own four fixed speeds, 0 to 3. Roughly 1x, 1.3x, 1.9x and 3.4x, pitch unchanged, but rougher. `--speed` is smoother. |
| `--clock MULT` | Pretends the speech chip runs at a different clock, e.g. `1.5`. Changes speed **and** pitch together — the sped-up-tape effect. Range 0.25 to 4. |
| `--pitch N` | Voice pitch, 0 to 63. Default 24. |
| `--volume N` | Volume, 0 to 15. Default 12. |
| `--word-delay N` | Pause between words, 0 to 15. Default 0. |
| `--repeat-filter N` | 0 to 99, default 99. See below. |
| `--rate HZ` | Sample rate of the WAV, e.g. `22050`. Default 8000, which is what the real card produced. This is a minimum: `--clock` raises it to match the chip, since writing the file at a lower rate than the chip is producing would throw quality away. `say` says so when it happens. |
| `--chunk N` | Split long lines every N characters. Default 80. |
| `--no-chunk` | Never split. Prints a warning; see below. |
| `--raw` | Send bytes to Textalker untouched, skipping the conversion of modern characters. |

### The four speed controls, and why there are four

They do genuinely different things and can be combined:

- `--speed` makes speech faster or slower **without** touching the pitch
  at all, smoothly, anywhere from a quarter speed to four times. This is
  the one you probably want. It works by changing how fast the chip walks
  through each sound rather than by playing anything faster, so the voice
  keeps its character.
- `--frame-rate` is the speech chip's own four fixed steps. Also
  pitch-preserving, but coarse, and the top step is rough.
- `--clock` speeds it up **and** raises the pitch, exactly as
  overclocking the real chip would. Chipmunks.
- `--compressed` is Textalker's own idea of fast speech, which it
  achieves by leaving sounds out.

```
say64 --speed 1.8 "Textalker 3.1.3 Loader.bin" "Textalker 3.1.3.bin" "Reading quickly now." fast.wav
say64 --clock 1.6 "Textalker 3.1.3 Loader.bin" "Textalker 3.1.3.bin" "Help, I am a chipmunk." silly.wav
say64 --pitch 8 --rate 22050 "Textalker 3.1.3 Loader.bin" "Textalker 3.1.3.bin" "Low and slow." deep.wav
```

Higher `--frame-rate` values also change the texture of the voice, not
just its speed, because the chip has less time to slide between sounds.
3 is fast and rough; try 1 or 2 first.

### The repeat filter

Textalker has a feature that collapses runs of the same character, so a
line of `*****` is not read out as "star star star star star". It cannot
tell decoration from content, though, so at its original setting it also
turns `EEEEEEEEE` into `EE`.

It is switched off by default here. `--repeat-filter 2` or similar puts
the original behaviour back if you want to hear it.

## Echo/Textalker command reference

If you're preparing a file to send to the Echo and want to embed any of these commands, you'll need to use a hex editor or some other editor which lets you insert control characters.

All prefixed with Ctrl-E (`\x05`) unless noted:
`nP` pitch 0-63 (default 24), `nF` flatness, `nV` volume 0-15
(default 12), `nD` inter-word delay 0-15 (v3.1.3 only), `C`/`E`
compressed/expanded, `T` talk-only, `nR` repeat filter, `\x16` (Ctrl-V)
phoneme mode terminated by CR.

## Things worth knowing

- **A single character on its own is announced properly.** Type just a
  comma and you will hear "comma". Textalker would normally say nothing.
- **`--word-delay` does nothing on Textalker 1.3.** That version never
  implemented the command and quietly ignores it. Not a bug here.
- **Long text is split at sensible places.** Sentences and phrases are
  broken at punctuation or between words, never mid-word, because
  Textalker's own line buffer would otherwise cut wherever it happened
  to fill up.
- **The pauses are real.** Textalker takes a moment to think before each
  phrase, and on a real Apple II you would hear that. Most of it is
  removed here, but the natural pauses after commas and full stops are
  deliberately kept.

## What changed since the first release

If you have an earlier copy of `say`, these are the differences worth
knowing about:

- **A single character no longer says "return" after it.** In the first
  release, asking for a lone comma got you "comma return". That was a
  real bug and it is fixed.
- **`--speed`** is new, and is a better speed control than anything that
  was there before: smooth, and it genuinely does not move the pitch.
- **Long word delays and slow speeds used to mangle the speech.** The
  emulator gave up part-way through a character and said nothing about
  it. Fixed, and it now reports itself if it ever happens again.
- **`--clock` used to quietly cost you sound quality.** Raising the chip
  clock raises the rate the chip itself produces, and the WAV was still
  being written at 8000 — so the extra detail was not just wasted, it
  came back as noise. `--rate` is now a minimum and gets raised to match
  the chip, with a note on screen saying so.
- Ctrl-D driver commands, below, are new.

## Driver commands, with Ctrl-D

Ctrl-E commands go to Textalker. There is a second set, introduced by
Ctrl-D (``), that controls the emulator itself. Same shape: an
optional number, then a letter.

| Command | What it does |
|---|---|
| ` 1.5S` | Speed, as `--speed` |
| ` 0.75C` | Chip clock, as `--clock` |
| ` 2F` | Frame rate, as `--frame-rate` |
| ` 0B` | Turn chunking off; ` 80B` puts it back |
| ` 1R` | Raw text on; ` 0R` off |
| `F` | A bare letter restores that setting's default |
| `` | One literal Ctrl-D character, spoken rather than obeyed |

Each command ends the current phrase, so what comes before it is spoken
at the old setting and what follows at the new. Note that the letters
mean different things from the Ctrl-E ones: Ctrl-E `F` is monotone,
Ctrl-D `F` is frame rate.

## Licences

`THIRD_PARTY_LICENSES.md` covers the emulation code, which is public
domain and BSD-licensed, and must stay with the program.

The Textalker files themselves are the original commercial software from
Street Electronics, with the 3.1.3 release also carrying an American
Printing House for the Blind copyright. They are not covered by those
licences — `say` runs the genuine article rather than a reimplementation,
and those files remain the property of their copyright holders.
