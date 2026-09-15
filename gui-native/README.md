# gui-native — the EchoTalk desktop GUI

**Status: working, x64, verified by script. Not yet done: a live listening
pass with NVDA running** (see the end of this file).

A one-window Win32 program in C++ over this repository's library: Street
Electronics' real Textalker on an emulated 6502 driving the TMS5220 port,
exactly as the NVDA add-on runs it, with every setting exposed, **Preview**
through the sound card and **Render to WAV**. It follows the Votrax SC-01 ROM
GUI, Votrax Native, SAM and STSPEECH GUIs elsewhere in this tree; the plan
it was built from is [../ROADMAP.md](../ROADMAP.md).

One self-contained exe, about 450 KB, static CRT, Windows system DLLs only.
The front end is Windows-only; the engine under it is the same portable C
the Linux build uses.

## The Textalker images

**Not included, and never to be included.** A voice is a
`<name>.ram.bin` (or `<name>.loader.bin`) plus `<name>.obj.bin` pair,
exactly as the add-on finds them. See "Obtaining the Textalker images" in
the top-level README.

Where the program looks, first match wins:

1. `--rom-dir FOLDER` on the command line;
2. the ROM folder remembered from last time, if it still has images;
3. the folder the exe is in;
4. `roms\` beside the exe;
5. `..\..\roms\` from the exe — this repository's `roms\` when run from
   `gui-native\build\`;
6. `%APPDATA%\nvda\addons\echotalk\synthDrivers\echotalk` — the installed
   add-on, so a machine with EchoTalk in NVDA needs no setup.

The **ROM folder** box and **Browse** change it at any time. The **ROM
status** box, reachable with Tab, lists every pair as loaded (with its
Textalker banner and sizes), refused (with the engine's reason), or missing
its `.obj.bin`.

## The settings

The add-on's, in the add-on's order, then what the library has and the
add-on does not show.

| Setting | Control | Range | Default | What it does |
|---|---|---|---|---|
| Voice | combo | one per image pair | 3.x if present | Which Textalker. Labelled from the loader's own banner. |
| Rate | slider | 0–100 | 50 | Speed only, pitch held: `2^((rate-50)/25)`, 0.25x–4x. The add-on's mapping and its step of 2. |
| Pitch | slider | 0–63 | 24 | Textalker's pitch. |
| Volume | slider | 0–15 | 12 | Textalker's volume; low is fuzzy, high distorts. |
| Delay between words | slider | 0–15 | 0 | Textalker 3 only; 1.3 ignores it and the readout says so. |
| Repeat-character filter | slider | 0–99 | 99 | 99 never triggers. Lower gets Textalker's own collapsing of runs back. |
| Chip clock | slider | 0–100 | 50 | Speed and pitch together, the sped-up-tape effect: `2^((clock-50)/25)`. |
| Output sample rate | combo | 8, 11, 16, 22, 32, 44, 48 kHz | 8 kHz | A floor: raised to 8000 × clock when the clock outruns it, as the add-on does. The readout shows the rate really used. |
| Monotone | checkbox | | off | Same pitch, no intonation. |
| Compressed speech | checkbox | | off | Textalker's compressed rate, made by skipping segments. |
| Frame rate | combo | 8, 6, 4, 2 periods | 8 | The TMS5220's own rate steps (about 1.00x, 1.31x, 1.89x, 3.40x). Not something the Echo II used. |
| Reading mode | combo | startup, words, spell | startup | "Textalker's startup mode" sends nothing, as the library does until told. |
| Punctuation | combo | startup, none, some, all | startup | The same rule. At All a line end is spoken as "return". |
| Chunk size | slider | 0–255 | 80 | Longest run handed to Textalker between CRs. 0 turns chunking off and is unsafe. |
| Raw text | checkbox | | off | Bytes go to Textalker without text preparation. |
| Obey embedded commands | checkbox | | off | Off: Ctrl-D, Ctrl-E and Ctrl-V in the text become spaces, as in the add-on. On: `^E`, `^D`, `^V` in the text are those bytes, and `^^` is a caret. |
| Voice preset | combo | Echo II defaults, Custom | | Derived from the controls, never remembered: it says Echo II defaults when every setting above is at its default, Custom otherwise. Choosing Echo II defaults resets them. |

**Sliders read as Textalker's own numbers.** Windows' built-in accessibility
proxy reports a trackbar's value as a *percentage of its range*: Pitch at 24
of 0–63 reads "38", Volume at 12 reads "80". The GUI registers a Dynamic
Annotation server (`IAccPropServer`) on every slider whose range is not
0–100, which answers the value with the live position instead, so NVDA says
"Pitch slider 24". The readout beside each one gives the matching NVDA
percentage (`NVDA 38%`, or `NVDA about n%` where NVDA's slider cannot land on
that value exactly), so a voice can be copied between the two.

## Buttons and keys

| Button | Key | |
|---|---|---|
| Preview | F5, or Enter outside the text box | Renders on a worker thread, then plays. Focus does not move. Preview again restarts with the current values. |
| Stop | Escape | Stops playback, abandons a preview still rendering, ends a batch after the current file. |
| Render to WAV... | Ctrl+S | 16-bit mono at the effective rate. Written to a temporary name and renamed, so a failure never leaves half a file. |
| Save preset... | Ctrl+Shift+S | An INI file: every setting, plus the voice's name, banner and image MD5s. |
| Load preset... | Ctrl+O | Warns if the preset was made on different images. |
| Copy as say command line | Alt+Y | The same render as a `say-x64.exe` command, for a shell. |
| Batch render lines to folder... | | One WAV per non-blank line, named `001 first words.wav`. |
| Pitch sweep 0 to 99 to folder... | | The text at every raw pitch 0–99 (`pitch 00.wav` ...), sent as Ctrl-E commands so Textalker's behaviour above 63 is reachable; see `notes/pitch_above_63.md`. |

Faults open a message box — an emulation overrun (always a fault: that audio
is wrong), bad Ctrl-D commands when embedded commands are obeyed, missing
images — because a changed status line is silent to a screen reader. The
**Last result** box, the last tab stop, holds the most recent outcome.

Settings, the ROM folder and the voice are remembered in
`%APPDATA%\EchoTalk GUI\settings.ini`. The text is not. `--no-settings`
starts from the defaults and remembers nothing.

## Building

```
gui-native\build.cmd            x64 (default)
gui-native\build.cmd arm64      ARM64, untried
```

Needs Visual Studio with "Desktop development with C++". Output:
`gui-native\build\echotalk_gui-x64.exe` and `say-x64.exe`. There is no
32-bit build and the script refuses to make one. The five warnings from the
library pass are known and described in `notes/msvc_build.md`; the GUI
itself builds at `/W4 /WX`.

## How it was verified

Run on 2026-09-15 with the images and DLL of the unpacked add-on in
`..\echotalk\synthDrivers\echotalk\`:

```
python tools\verify_gui.py            the exe against the add-on's own DLL
python tools\verify_gui_keyboard.py   the real window, by keyboard and MSAA
python tools\verify_gui_smoke.py      Preview/Stop, a Unicode folder, say
python tools\build_release.py         all of the above from a clean build, then the zip
```

`build_release.py` wipes `gui-native\build`, builds, runs the three checks,
confirms the exe imports only Windows system DLLs, and writes
`dist\echotalk-gui-<version>-windows-x64.zip`: the GUI, say, README, NOTICE,
LICENSE, THIRD_PARTY_LICENSES.md, the library README, and an empty `roms\`
with a note. It **fails and deletes the zip if anything in it could be a
Textalker image**, then renders from a copy unpacked into an empty folder.
It passed end to end on 2026-09-15.

- **Against the add-on itself — 114 cases, all byte-identical.**
  `verify_gui.py` holds the add-on driver's speaking path with NVDA removed
  (`_openVoice`, `_applyAll` and its conversions, `speak`'s sanitising,
  `_speakOne`'s read loop) and runs it on the add-on's **shipped**
  `echotalk64.dll`. The GUI's `--selftest` mode runs the same function
  Render to WAV runs. For both Textalker versions: defaults; UTF-8 text,
  control bytes, a single letter, a paragraph; rate, pitch, volume, word
  delay, repeat filter and chip clock at several values each; all seven
  output rates and the clock raising them; monotone and compressed; every
  frame rate, reading and punctuation mode; chunk 0, 40 and 255; raw text;
  caret commands obeyed and sanitised; everything at once. Every WAV holds
  exactly the add-on's samples at the add-on's rate. Also: a preset written
  and rendered from the file matches the direct render; batch render writes
  the right files, each identical to the add-on speaking that line; the
  pitch sweep writes 100 files matching the add-on at pitches 0, 24, 63, 64
  and 99, with and without monotone.
- **The check can fail.** With Monotone deliberately ignored in `Render`, it
  reported exactly the six monotone cases and nothing else.
- **Keyboard and screen reader.** `verify_gui_keyboard.py` launches the real
  window and posts real Tab keystrokes: all 30 tab stops are reached in
  reading order, forwards and backwards, and the ring closes, so the
  multiline boxes do not trap focus. The 23 Alt accelerators are distinct.
  Through oleacc, as NVDA reads Win32 controls, every stop has a name and
  the expected role, and every slider reads its default in its own units
  ("Pitch: 24", "Chunk size: 80"). Arrow keys: Rate steps by 2, Pitch by 1,
  the annotated value follows ("25"), the readout says `NVDA 40%`, the preset
  combo turns Custom and back, and Chip clock at 100 shows the output rate
  raised to 32000 Hz.
- **Smoke.** Preview, Stop part-way and Preview on the other voice leave the
  window alive with no error dialog; a pair in a folder named `rõõm ÄÖÜ ☃`
  renders (the manifest's UTF-8 code page); `say --flat --letter-mode 1
  --punctuation 2` matches the GUI.
- `dumpbin /dependents`: USER32, GDI32, COMCTL32, WINMM, COMDLG32, SHELL32,
  ole32, bcrypt, OLEAUT32, KERNEL32.

**Not done:** a live listening pass with NVDA running — whether NVDA
announces each control, slider value and message box as the MSAA readings
say it will, and what Preview sounds like at the speed and clock extremes.
No script settles that. Until someone has done it with NVDA 2026.1 and
written down what they heard in `notes/`, this line stays.

## Where it differs from the add-on

- **Every render boots a fresh machine.** The add-on keeps one instance for
  the session; the GUI's audio is the add-on's first utterance after a
  voice opens, every time. A Ctrl-E command obeyed in one render therefore
  cannot carry into the next, and a WAV depends only on the images, the
  settings and the text.
- **Sliders are in Textalker's units**, not NVDA's percentages, so every
  pitch and volume step is reachable.
- **The extra settings** are applied only when moved off the library
  default, which is why a GUI left at defaults is byte-for-byte the add-on.
- The add-on's per-utterance handling of NVDA's own commands (index marks,
  capital-letter pitch changes) has no counterpart: there is no NVDA here.
