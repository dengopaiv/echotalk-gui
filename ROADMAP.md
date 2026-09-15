# EchoTalk GUI roadmap

A plan for a native C++ desktop application that drives the EchoTalk
emulation: every setting the NVDA add-on exposes, plus the ones the
library has and the add-on leaves out, with **Preview** through the sound
card and **Render to WAV**. It is built the way the Votrax SC-01 ROM GUI,
Votrax Native, SAM and STSPEECH GUIs already are (1.5).

Written 2026-09-15, before any GUI code existed, and followed the same day.
Status: **Phases 0–4 built and verified; Phase 5 partly done.** The GUI is
in [gui-native/](gui-native/README.md); "Progress" at the end of this file
says what is done, what the building corrected in this plan, and what is
left. Everything under "Established" below was checked on this machine that
day.

Read [HANDOFF.md](HANDOFF.md) first. This document assumes it and does
not repeat it.

---

## Part 1 — What was studied

### 1.1 The add-on copy in `..\echotalk\`

`C:\GIT\speech synthesis\echotalk\` is Jayson Smith's add-on, version
0.1.0, unpacked, with images:

| File | Size | MD5 |
|---|---|---|
| `synthDrivers/echotalk/textalker.ram.bin` (3.1.3 loader) | 543 | `08a4ad5c7afe3827880523fc5bef150e` |
| `synthDrivers/echotalk/textalker.obj.bin` (3.1.3 engine) | 12282 | `36901b087c4f618f905420206f7a9b64` |
| `synthDrivers/echotalk/textalker_v13.ram.bin` (1.3 loader) | 611 | `0c4c5aa4b126b53d4dc7232fb2184290` |
| `synthDrivers/echotalk/textalker_v13.obj.bin` (1.3 engine) | 11264 | `f58d390f7be9bb165c10701627515059` |
| `synthDrivers/echotalk/echotalk64.dll` | 82432 | — |
| `synthDrivers/echotalk/echotalk32.dll` | 115214 | — (legacy; see 1.5) |

Its `__init__.py` is **byte-identical** to
`nvda-addon/synthDrivers/echotalk/__init__.py` in this repository
(`diff -q`, no output). So the driver in this repository is the
authoritative description of what the add-on exposes, and the DLL is
built from `src/` here.

The images are proprietary (Street Electronics; 3.1.3 also carries an
APH copyright). They are **never** copied into this repository, into a
build directory that could be committed, or into a release. `roms/*.bin`
is already in `.gitignore`; the GUI plan below keeps it that way.

### 1.2 The parameter surface

Two sources: what the add-on shows the user
(`nvda-addon/synthDrivers/echotalk/__init__.py`, `supportedSettings`),
and what the library accepts (`src/echotalk.h`, ABI 6). The GUI exposes
the union, in the library's **native units** rather than NVDA's 0–100
percentages — a desktop tool has no reason to hide that pitch is 0–63.

| # | Setting | Library call | Native range | Default | In add-on | Notes for the GUI |
|---|---|---|---|---|---|---|
| 1 | Voice | `echotalk_create(loader, obj)` | one per image pair | 3.x if present | yes | Changing it destroys and re-creates the machine. Label from `echotalk_version()`. |
| 2 | Speed (add-on "Rate") | `set_speed` | 0.25–4.0, continuous | 1.0 | yes, log slider | Speed only, pitch held (measured 129 Hz from 0.5x to 3.0x). Not something real hardware did — say so. Sub-linear: 2.0 delivers 1.84x. |
| 3 | Pitch | `set_pitch` | 0–63 | 24 | yes | NVDA's percentage slider cannot hit every value; the GUI can. |
| 4 | Volume | `set_volume` | 0–15 | 12 | yes | Textalker's own; low is fuzzy, high distorts. |
| 5 | Word delay | `set_word_delay` | 0–15 | 0 | yes | **3.x only**; 1.3 discards the command. Grey out or label on 1.3. |
| 6 | Repeat filter | `set_repeat_filter` | 0–99 | 99 (never triggers) | yes | Textalker's own default collapses `EEEEEEEEE` to `EE`. |
| 7 | Chip clock | `set_clock_multiplier` | 0.25–4.0 | 1.0 | yes, log slider | Speed and pitch together. Raises the effective output rate — see 1.4. |
| 8 | Output sample rate | `set_sample_rate` | 8000, 11025, 16000, 22050, 32000, 44100, 48000 | 8000 | yes | A **floor**: effective rate = max(choice, round(8000 × clock)). Library does not enforce it; the host must. |
| 9 | Monotone | `set_flat` | 0/1 | 0 | yes | Same Textalker setting as pitch (`nP` vs `nF`). |
| 10 | Compressed speech | `set_compressed` | 0/1 | 0 (expanded) | yes | Skips phoneme segments; audibly different on soft G/J. |
| 11 | Frame rate | `set_frame_rate` | 0–3 (≈1.00x, 1.31x, 1.89x, 3.40x) | 0 | **no** | Real 5220 capability the Echo II never used. Add-on hides it because Speed is smoother; a sound-design tool should offer it. |
| 12 | Letter mode | `set_letter_mode` | 0 words / 1 spell | *not sent* | no | **Tri-state in the GUI**: nothing is sent to Textalker until it is set once. The GUI needs a "Textalker's startup mode" choice that sends nothing. |
| 13 | Punctuation | `set_punctuation` | 0 none / 1 some / 2 all | *not sent* | no | Same tri-state rule. At 2 a line end is spoken as "return". |
| 14 | Chunk size | `set_chunk_size` | 0 (off) or characters | 80 | no | 0 is experimental and documented as unsafe for anything that must be right. Advanced group only. |
| 15 | Raw text | `set_raw` | 0/1 | 0 | no | Bypasses text preparation. Advanced. |
| 16 | Index break | `set_index_break` | 0/1 | 0 | no | **Not exposed.** Only matters with Ctrl-D `I` marks, which a WAV renderer has no listener for. Recorded here so the omission is deliberate. |

Diagnostics the GUI shows but does not set: `echotalk_version()`,
`echotalk_overruns()` (**non-zero is always a fault** — show it loudly),
`echotalk_command_errors()` (bad Ctrl-D commands, silently swallowed
otherwise), and the effective output rate.

Embedded commands. Ctrl-E (Textalker: `nP nF nV nD nR C E T L W A S N`),
Ctrl-D (driver: `nS nC nF nB nR nI`) and Ctrl-V (phoneme mode) reach the
pipeline from the text itself. The add-on replaces all three with a space,
because text off a screen must not change the voice. A GUI user typing
into a box is the opposite case — see Decision D6.

### 1.3 Established on 2026-09-15 by experiment

All with the images from `..\echotalk\` read in place (not copied), text
`Hi. The quick brown fox jumps over the lazy dog, 1985.`

**The library builds under MSVC x64.** Visual Studio 18 Community,
`vcvars64.bat`, `cl /O2 /W3 /std:c11` over `tools/say.c` plus the eight
library sources in the Makefile's `LIB_SOURCES`. Exit 0, five warnings:
three C4244 narrowing conversions and two C4996 (`fopen`, `strcpy`). No
source changes were needed.

**MSVC output is bit-identical to the shipped MinGW DLL.** The same text
rendered by the MSVC `say.exe` and by `echotalk64.dll` from the add-on
(driven through ctypes, as NVDA does):

| Settings | Samples | MD5 (both builds) |
|---|---|---|
| defaults | 47274 | `67579800080983a24bbfcbaf1bd1dad5` |
| speed 1.5 | 33208 | `b1a666f7928d30d7e413983eace986f4` |
| clock 1.5, rate 12000 | 47274 | `67579800080983a24bbfcbaf1bd1dad5` |
| pitch 40, volume 9, word delay 5, compressed | 43525 | `0631cdb18a7bc52aaac22b332c1a1610` |

This extends HANDOFF's "Windows and Linux are byte-identical" to a third
compiler. It means the GUI can compile the library straight in with MSVC
and still be checked sample for sample against `say` and the add-on.

**The clock row is worth understanding before building export.** At
clock 1.5 and a 12000 Hz output the *samples* are identical to the
defaults; only the WAV header's rate differs. The clock effect lives
entirely in the rate the samples are played at. So the WAV writer must
use the effective rate, never a hard-coded 8000, or every clock setting
exports as the default voice.

**Textalker 1.3 boots and speaks** under the MSVC build ("Hi." → 3162
samples, banner `1.3`).

### 1.4 Constraints the GUI must design around

1. **Single instance.** Fake6502 keeps the CPU in globals;
   `echotalk_create()` fails while another instance lives. The GUI owns
   exactly one engine, and banner probing at startup must destroy each
   probe before the next.
2. **Not thread-safe.** Every call into one instance from one thread, or
   serialised.
3. **Ctrl-E settings persist inside Textalker.** A `\x05 40P` typed into
   the text changes the voice for everything after it, in this render and
   the next. An export must therefore not depend on what was previewed
   before it (Decision D3).
4. **Output rate is a floor**, raised to `round(8000 × clock)`.
5. **`echotalk_create` opens the images with narrow `fopen`.** A path
   with characters outside the ANSI code page — a user folder named in
   Estonian, say — fails to open. The ROM GUI's manifest already
   carries the fix, `activeCodePage UTF-8`, which makes narrow paths UTF-8
   with no library change (1.5). If that ever proves insufficient, the
   fallback is an `echotalk_create_from_memory()` at ABI 7.
6. **Synthesis is 90–155x real time**, so a whole paragraph renders in a
   fraction of a second. Preview can render first and play second without
   anyone noticing. That is far simpler than streaming, and it is what
   every sibling GUI does (1.5).
7. **The engine is 64-bit only in anything the GUI ships.** The
   repository's `make win32` / `win32-dll` targets and the add-on's
   `echotalk32.dll` predate the house rule and are legacy (listed in the
   head README). This plan adds no x86 target anywhere and does not remove
   the old ones — that is a separate decision.

### 1.5 The four sibling GUIs this one follows

Four native GUIs in this tree already solve this problem for other
engines, and they agree with each other closely. This one is built the
same way on purpose: same shape, same checks, and failure modes that have
already been paid for once.

| GUI | Where | Engine | Controls | Checked against |
|---|---|---|---|---|
| **Votrax SC-01 ROM GUI** | `votraxxion/dist/local-gui/votrax-sc01-mame-gui/` (archived, unreleased) | MAME's SC-01 via Tamas Geczy's add-on core, **user-supplied ROMs** | the add-on's settings as trackbars, with readouts | the add-on's own shipped `sc01-x64.dll`, driven as its driver drives it: 84 cases, byte-identical |
| Votrax Native | `votraxxion/gui-native/` | Votraxxion's C engine, ROMs compiled in | up-downs, preset combo | the Workbench presets |
| SAM (IHAMAICS) | `sam/gui-native/`, `sam/docs/native-gui.md` | SAM C port | up-downs, preset combo | the Python GUI |
| STSPEECH | `stspeech/gui-native/`, `stspeech/docs/native-gui.md` | STSPEECH C port | up-downs | the Python GUI: 14 cases, byte-identical |

**The ROM GUI is the direct template.** It is the same job for a
different chip: a one-window front end over an NVDA add-on's engine, the
add-on's settings in the add-on's order, ROM images the user supplies and
the program finds, and a check that the GUI's WAV holds exactly the bytes
the add-on would have played. EchoTalk is that, with Textalker images
instead of SC-01 dumps and a longer settings list.

What all four share, and this GUI takes as given:

- **One `.cpp` file** (1,077–1,575 lines), plus `.rc`, `resource.h` and a
  manifest. Controls are created in code by a small `Make()` helper, each
  preceded in z-order by its static label, with `IsDialogMessageW` in the
  message loop. No dialog template, no menu bar, no framework.
- **`build.cmd`**: `vswhere` finds Visual Studio, then `vcvarsall`, `rc`,
  and `cl /EHsc /MT /O2 /W4 /WX /DUNICODE`. The engine is compiled
  straight in and the exe needs nothing installed. The ROM GUI compiles the
  third-party engine **in a separate `cl` pass with that engine's own
  flags** (`/W3`, its warnings left alone) and only its own `.cpp` at
  `/W4 /WX`. It accepts `x64` or `arm64` and **refuses anything else**.
  (The older three still take `x86`; that is legacy.)
- **Manifest**: Common Controls 6, DPI aware, and in the ROM GUI
  `<activeCodePage>UTF-8</activeCodePage>`, which is exactly the fix
  Constraint 5 needs, already in use here.
- **`--selftest ... OUT.WAV TEXT`**: a headless mode of the *shipped exe*
  that runs the same function Render to WAV runs, with numbered exit
  codes. The checks drive the built binary, not a harness that merely
  shares its sources.
- **Render on a worker thread, then play.** A generation counter is bumped
  by Stop and by every new Speak; the worker posts `WM_APP_RENDERED` or
  `WM_APP_SYNTH_FAILED`. The ROM GUI plays with `PlaySoundW(SND_MEMORY |
  SND_ASYNC)`, stops with `PlaySoundW(NULL)` and frees the WAV image only
  after that, so stopping mid-utterance is clean. Escape also stops.
- **Render to WAV** through `GetSaveFileNameW`; the ROM folder through the
  Vista `IFileOpenDialog` folder picker, which "screen readers handle far
  better than the old SHBrowseForFolder tree".
- **ROM discovery** (ROM GUI): `--rom-dir`, else the folder in a ROM
  folder box with Browse, else the first of the exe's folder, `roms\`
  beside it, and the NVDA add-on's folder under `%APPDATA%\nvda\addons\`.
  A read-only, **tab-reachable** ROM status box reports each file as
  loaded, not found, or refused with the engine's own reason. A machine
  with the add-on installed needs no setup.
- **Checks**: `verify_gui.py` (exe against the reference, byte for byte),
  `verify_gui_keyboard.py` (real `VK_TAB` messages into the running
  window's queue, forwards and backwards, no trap, every accelerator
  distinct) and, in the ROM GUI, an oleacc pass reading every tab stop's
  MSAA name, role and value the way NVDA does. STSPEECH adds
  `build_release.py`: rebuild from a wiped tree, run every check, zip,
  extract into an empty folder and run from there.
- **A `NOTICE.md` beside the binary** naming every borrowed piece.

Lessons those projects wrote down, which apply here unchanged:

- **A multiline edit box is a keyboard trap** unless subclassed to let Tab
  through (`WM_GETDLGCODE` answers `DLGC_WANTALLKEYS`). It shipped that way
  once with every label correct. Read-only multiline status boxes too.
- **A duplicate `&` accelerator is silent**: Alt+letter cycles between the
  claimants instead of activating either. It appeared when one control
  was added, so the check runs on every build.
- **A slider with no label is announced as "slider 50"** and nothing else.
- **Quirks of the reference are reproduced, not fixed**, and written down
  (SAM's double space, STSPEECH's inverted monotone). A GUI that disagrees
  with the add-on is a second voice, not a better one.
- **Say what was not done.** The ROM GUI's README ends "Not done: a live
  listening pass with NVDA running." That line belongs in this README too
  until it is done.

---

## Part 2 — Decisions

Proposals, each with its reason, most of them "because the siblings do".
Change any of them before Phase 1 starts, not after.

**D1. The ROM GUI's shape.** `gui-native/echotalk_gui.cpp`,
`echotalk_gui.rc`, `resource.h`, `echotalk_gui.manifest`, `build.cmd`;
window class `EchoTalkGuiMainWindow`; output
`gui-native/build/echotalk_gui-x64.exe` (and `-arm64`). The folder is
`gui-native/` like the other three. Plain Win32 C++ in one file. The front
end is Windows-only and its README says so; the engine underneath stays
the portable C it already is.

**D2. `build.cmd`, two passes, x64 and arm64 only.** Pass one compiles the
Makefile's eight `LIB_SOURCES` with `/W3 /D_CRT_SECURE_NO_WARNINGS`; the
five MSVC warnings in 1.3 are in the port and vendored code and are left
alone. Pass two compiles `echotalk_gui.cpp` at `/W4 /WX` and links. No
CMake, no change to the Makefile: the add-on keeps building as it does.

**D3. Every render boots a fresh machine**: create, apply settings, speak,
read to the end, destroy. The ROM GUI does the same ("every utterance is
that first one") and documents how that differs from its add-on, which
keeps one instance. For EchoTalk it is cheap (0.3 ms or 7 ms to boot),
makes a WAV a pure function of images, settings and text, stops a Ctrl-E
command in one render leaking into the next (Constraint 3), and keeps
every library call on the one worker thread (Constraint 2).

**D4. Render exactly as the add-on does.** Settings are pushed in
`_applyAll`'s order (pitch, flat, volume, word delay, repeat filter,
compressed, speed, clock, sample rate). *Corrected while building:* this
was justified as "a different order is a different byte stream", which is
wrong — the setters only store values and mark the settings dirty, and the
library assembles its command block itself at the next speak, so call
order does not change the output. The order is kept because it reads the
same as the driver it copies. The output rate is the add-on's floor,
`max(choice, int(8000 × clock + 0.5))`. Text is sanitised with the
add-on's `[\x04\x05\x16]` → space and encoded as UTF-8, and read in blocks
of 1024. Settings the add-on lacks are applied after its nine, and only
when moved off the library default, so a GUI left at defaults is
byte-for-byte the add-on.

**D5. Sliders where the add-on has sliders, in native units.** The ROM GUI
uses trackbars "because they are sliders in NVDA's own voice settings",
which is the right instinct for "exposed like in the add-on". But the
add-on's 0–100 percentages exist only because NVDA forces them, and they
cannot reach every Textalker value: 64 pitches share 101 positions, and 16
volume steps use `minStep=7`. A trackbar's accessible value is its
position, so **making the position the native value** gives both: NVDA
announces "Pitch: slider 24", which is Textalker's own number.

| Setting | Control | Range and steps | Readout beside it |
|---|---|---|---|
| Voice | combo | discovered pairs | image file names |
| Rate | trackbar | 0–100 on the add-on's log map; line 2, page 10 | `1.00x speed, pitch held` |
| Pitch | trackbar | 0–63; line 1, page 4 | `NVDA 38%` |
| Volume | trackbar | 0–15; line 1 | `NVDA 80%` |
| Delay between words | trackbar | 0–15; line 1 | `NVDA 0%`, `no effect on 1.3` |
| Repeat-character filter | trackbar | 0–99; line 1, page 10 | `NVDA 100%`, `99 = off` |
| Chip clock | trackbar | 0–100 on the add-on's log map; line 2 | `1.00x clock` |
| Output sample rate | combo | the add-on's seven rates | `effective 12000 Hz` when raised |
| Monotone, Compressed speech | checkboxes | | |
| Frame rate | combo | 8/6/4/2 periods (≈1.00/1.31/1.89/3.40x) | |
| Reading mode, Punctuation | combos | first item **"Textalker's startup mode"** sends nothing | |
| Chunk size | trackbar | 0–255, default 80 | `0 = off, unsafe` |
| Raw text, Obey embedded commands | checkboxes | | |

Rate and chip clock keep the add-on's log positions (50 = 1.0x, every 25
doubles): a multiplier has no integer native unit, and those positions are
what an NVDA user already knows. The readout gives the multiplier. The
`NVDA n%` readouts use the add-on's `_toPct`, so a voice tuned here can be
set in NVDA's dialog and the other way round. This is the one deliberate
departure from the ROM GUI, which keeps pure 0–100 sliders, and it is
here to be decided.

**D6. Embedded commands are sanitised by default, with an opt-in to obey
them.** A checkbox, "Obey embedded commands", off by default to match the
add-on. When on, text is read with caret notation (`^E` → 0x05, `^D` →
0x04, `^V` → 0x16, `^^` → `^`), since control bytes cannot be typed into
an edit box. That is also what makes HANDOFF's deferred pitch 60–99
experiment (`^E99P`) reachable without a tool of its own.

**D7. Presets follow the Votrax Native and SAM rule.** A "Voice preset"
combo whose selection is **derived from the controls, never remembered**:
it names the preset the values describe, or says Custom. There is one
built-in preset, *Echo II defaults* (every row of table 1.2 at its
default), plus Save and Load for plain INI files (`[echotalk-preset]`,
native units, the voice's banner and image MD5s, with a warning when
loaded against different images). SAM's `verify_gui_presets.py` is the
model for checking it.

**D8. Licence and notices.** This repository is a public fork of
`jaybird110127/echotalk` (BSD-3-Clause), and the GUI is new code under the
same licence. `gui-native/NOTICE.md`, in the ROM GUI's format, names
Jayson Smith (the library, and the add-on whose behaviour the GUI
reproduces), Mike Chambers (Fake6502, public domain) and the MAME
TMS5220 authors (BSD-3-Clause, as `THIRD_PARTY_LICENSES.md` credits them),
and states that the Textalker images are **not included and never to be
included**. The About text says the same. Whether the GUI is offered
upstream is for the user and Jayson to decide.

---

## Part 3 — The window

One window, with controls in reading order, which is also tab order. No two
accelerator letters collide, and `verify_gui_keyboard.py` enforces that.
They cannot all match the add-on's labels (`&Rate`, `&Repeat` and
`&Chip clock` would clash in one window), so where the letters differ the
label text still matches.

```
&Text to speak:            [ multi-line edit, subclassed so Tab leaves it ]
ROM &folder:               [ C:\...\roms                    ] [ &Browse... ]
R&OM status:               [ read-only, tab-reachable, subclassed:
                             textalker: loaded, Textalker 3.1.3
                             textalker_v13: loaded, Textalker 1.3 ]

-- the add-on's settings, in the add-on's order --
Voi&ce:                    [ Textalker 3.1.3              v ]
&Rate:                     [-----|-----]  1.00x speed, pitch held
P&itch:                    [---|-------]  NVDA 38%
Vo&lume:                   [-------|---]  NVDA 80%
Delay between &words:      [|----------]  NVDA 0%
R&epeat-character filter:  [----------|]  NVDA 100%, 99 = off
Chip cloc&k:               [-----|-----]  1.00x clock
Output &sample rate:       [ 8 kHz (native)               v ]
&Monotone                  [ ]
Compressed speec&h         [ ]

-- beyond the add-on --
Fr&ame rate:               [ 8 periods (normal)           v ]
Rea&ding mode:             [ Textalker's startup mode     v ]
P&unctuation:              [ Textalker's startup mode     v ]
Chunk si&ze:               [--|--------]  80
Raw te&xt                  [ ]
Obey embedded comma&nds    [ ]   ^E ^D ^V ^^
Voice &preset:             [ Echo II defaults             v ]

[ Pre&view ] [ Stop ] [ Render to WAV... ] [ Save preset... ] [ Load preset... ]
[ Cop&y as say command line ]
Last result:               [ read-only: "Rendered 47274 samples at 8000 Hz. Overruns 0." ]
```

Behaviour that matters:

- **Stop, Render to WAV, Save preset and Load preset have no letter**,
  because every letter in them is taken. Escape stops from any control, as in the ROM
  GUI; Ctrl+S renders to WAV, Ctrl+Shift+S saves a preset, Ctrl+O loads
  one, and F5 previews.
- **Preview never moves focus**, so the user can nudge a slider and press
  F5 again while the screen reader is still talking.
- Moving a setting during playback does not restart it; the next Preview
  uses the new values.
- **Nothing is disabled just to explain itself.** On 1.3 the word-delay
  slider stays enabled and its readout says `no effect on 1.3`. A disabled
  control leaves the tab order, and the user can no longer find out why it
  does nothing.
- **Faults are message boxes**, not just status text: an overrun
  (`echotalk_overruns()` non-zero is always a fault), command errors when
  embedded commands are on, and no usable images. A changed static is
  silent to a screen reader.
- With no images found the window still opens and every control can be
  explored. The ROM status box says where it looked and points to the
  README's "Obtaining the Textalker images", and Preview and Render report
  the same.
- **Copy as say command line** puts the equivalent `say` invocation on the
  clipboard, so any exported WAV can be regenerated from a shell.

---

## Part 4 — Phases

Each phase has a deliverable and a **gate**. The gate is run, not asserted.

### Phase 0 — Groundwork

1. `say` gains `--flat`, `--letter-mode N` and `--punctuation N`, so every
   GUI setting has a command-line spelling.
2. `notes/msvc_build.md` records the MSVC build and the MD5 table from
   1.3, with exactly what was run. HANDOFF's Building section points to it.
3. Copy `verify_gui_keyboard.py` and the oleacc half of
   `verify_rom_gui_keyboard.py` from Votraxxion into `tools/`, with their
   origin kept in the header (same author, BSD-3-Clause; Votraxxion is
   public).

**Gate:** `make test`, `make dll` and `make test-dll` still pass
untouched, and the HANDOFF baselines still hold (`hi_only` 2297 samples
through `say`, amplitude −12127/+26833).

### Phase 1 — `--selftest` and parity with the add-on

The render path comes first, with no window yet, the order in which the ROM
GUI's checks were built.

1. `gui-native/` skeleton and `build.cmd` per D1 and D2. The window can be
   an empty frame.
2. Image discovery by the add-on's `_imagePairs` rule (`<stem>.ram.bin` or
   `<stem>.loader.bin`, plus `<stem>.obj.bin`), searched in `--rom-dir`,
   then the exe's folder, `roms\` beside it, and
   `%APPDATA%\nvda\addons\echotalk\synthDrivers\echotalk`. Each pair is
   probed for its banner (create, `echotalk_version`, destroy) one at a
   time.
3. `Render(voice, settings, text)` per D3 and D4, returning PCM, effective
   rate, overruns and command errors, with generation-counter cancellation
   between reads. `MakeWav` writes 16-bit mono at the **effective** rate;
   1.3 shows the clock lives in the header.
4. `echotalk_gui.exe --selftest ROMDIR STEM RATE PITCH VOLUME DELAY REPEAT
   CLOCK SRATE MONO COMP FRAMERATE READING PUNCT CHUNK RAW OBEY OUT.WAV
   TEXT`, with numbered exit codes like the siblings'.
5. `tools/verify_gui.py ROMDIR ADDON_DLL`, modelled on
   `verify_rom_gui.py`: an `AddonDriver` class holding the add-on's
   `_openVoice`, `_applyAll`, `speak` and `_speakOne` path with NVDA
   removed, running the add-on's **shipped** `echotalk64.dll` from
   `..\echotalk\synthDrivers\echotalk\`. Images are read there, never
   copied. The matrix covers both voices, each add-on setting at minimum,
   default, maximum and two values between, some combinations, sanitised
   control bytes, UTF-8 text and a single character.

**Gate:** every case is byte-identical to the add-on, at the same WAV
rate. The script is shown to **fail** when one setting is deliberately
applied out of order in `Render`. A second pass checks the settings beyond
the add-on against `say` from Phase 0. Overruns are zero throughout.

### Phase 2 — The window and Render to WAV

1. Every control in Part 3, built with the `Make()` pattern and the system
   message font, with the Tab subclass on the text box and both read-only
   boxes.
2. Readouts updated on `WM_HSCROLL`, and the output-rate floor shown.
3. Render to WAV on the worker thread, with buttons disabled while it runs
   and `WM_APP_RENDERED` / `WM_APP_SYNTH_FAILED` on completion. The WAV is
   written to a temporary name and renamed, so a failure never leaves half
   a file.
4. ROM folder box, Browse (`IFileOpenDialog`, `FOS_PICKFOLDERS`) and the
   status box, plus the manifest with the UTF-8 code page.
5. Settings remembered between runs in `%APPDATA%\EchoTalk GUI\settings.ini`.

**Gate:** `tools/verify_gui_keyboard.py`: real Tab and Shift+Tab reach
every stop in order with no trap; all accelerators are distinct (shown to
fail by giving two controls one letter); oleacc reads a non-empty name and
the expected role for every stop, and each slider's value equals its
native setting. Images in a folder named `rõõm ÄÖÜ` load and render. The
Phase 1 check still passes.

### Phase 3 — Preview

1. Render on the worker, then `PlaySoundW(SND_MEMORY | SND_ASYNC)`. Stop
   and Escape call `PlaySoundW(NULL)` and bump the generation, and the WAV
   image is freed only after that, as in the ROM GUI.
2. Preview during playback stops it and starts again with the current
   values.

**Gate:** Preview and Render share one render function, which the selftest
exercises. A smoke test like the ROM GUI's: Preview on each voice and Stop
mid-utterance leave the window alive with no error dialog. Then the part no
script settles, **a listening pass with NVDA running** (clock 0.5, 1.0 and
2.0; rate 25, 50 and 100; both voices), recorded with what was heard in
`notes/gui_nvda_walkthrough.md`. Until that is done, the README says so.

### Phase 4 — Presets and extras

1. The Voice preset combo with derived selection, and Save and Load (D7).
2. Copy as `say` command line.
3. Batch render: one WAV per line of the text box.
4. Pitch sweep: one sentence over a range of raw pitch values via `^E nP`,
   one WAV each, which is HANDOFF's deferred 60–99 experiment.

**Gate:** a `verify_gui_presets.py` on SAM's model. Selecting each preset
sets exactly its values; nudging any slider off it shows Custom and back
onto it shows the preset again; a preset saved, reloaded and rendered is
byte-identical to the render before saving.

### Phase 5 — Release

1. `gui-native/README.md` in the ROM GUI's layout (what it is, the
   settings table, the images and where they are looked for, building, how
   it was verified, where it differs from the add-on, what was not done),
   and `NOTICE.md` (D8). Pointers from the top-level README and HANDOFF.
2. `tools/build_release.py` on STSPEECH's model: wipe `gui-native/build`,
   build x64, run the checks from Phases 1–4, then zip the exe with
   `LICENSE`, `THIRD_PARTY_LICENSES.md`, `NOTICE.md` and an empty `roms\`
   with its README. Extract into an empty folder and run `--selftest` from
   there. **The script lists the zip and fails if any `.bin` is in it.**
3. `dumpbin /dependents` shows Windows system DLLs only.
4. The head directory's `README.md` gets `echotalk-gui` in its inventory,
   with its remote and public status.

**Gate:** the release script passes end to end on a clean checkout.

---

## Part 5 — Risks and open questions

- **D5 is the one real departure from the ROM GUI** (native-unit sliders
  rather than 0–100). Built that way; see Progress for the accessibility
  fix it needed.
- **Letter and punctuation "startup mode"** is an assumption in the
  library, not a measurement. The "Textalker's startup mode" combo item
  avoids asserting it; measuring it would let the GUI name it.
- **Speed extremes** are outside anything the chip was designed for; the
  Rate readout could say so above 2x and below 0.5x.
- **ARM64** is untried for the engine. `build.cmd arm64` is a one-word
  experiment, but the reference DLL is x64, so on ARM the comparison has to
  be against WAVs saved from an x64 run.
- **Legacy 32-bit targets** in the Makefile and the add-on remain. Removing
  them touches the shipping add-on and is not part of this plan, so ask.
- **Upstream**: whether this goes to `jaybird110127/echotalk` is open.

---

## Progress, 2026-09-15

**Built:** `gui-native/` (window, build script, manifest, NOTICE, README),
`tools/verify_gui.py`, `tools/verify_gui_keyboard.py`,
`tools/verify_gui_smoke.py`, `say --flat --letter-mode --punctuation`,
`notes/msvc_build.md`, `notes/pitch_above_63.md`.

| Phase | State | Evidence |
|---|---|---|
| 0 Groundwork | done | `say` flags match the GUI byte for byte (`verify_gui_smoke.py`) |
| 1 `--selftest` and parity | done | 114 cases byte-identical to the add-on's shipped DLL, both voices; a deliberate monotone bug fails exactly the six monotone cases |
| 2 Window and Render to WAV | done | 30 tab stops, closed ring both ways, 23 distinct accelerators, MSAA names, roles and values; a `rõõm ÄÖÜ ☃` images folder renders |
| 3 Preview | done, **except the listening pass with NVDA running** | Preview, Stop part-way, Preview on the other voice; window alive, no dialog |
| 4 Presets and extras | done | derived preset combo checked by keyboard script; preset round trip, batch render and pitch sweep byte-identical to the add-on |
| 5 Release | partly | README, NOTICE and dependency check done; `build_release.py` and the head-directory inventory line not yet |

**What building corrected in this plan:**

- **D4's reason** was wrong (see D4); the conclusion stands.
- **D5's premise was wrong, and the fix is now part of the design.** A
  trackbar's accessible value is *not* its position: the system MSAA proxy
  reports position as a percentage of the range. The first keyboard run
  read Pitch 24 as "38", Volume 12 as "80", Repeat 99 as "100", Chunk 80 as
  "31". The GUI now registers a Dynamic Annotation server
  (`IAccPropServer`, `PROPID_ACC_VALUE`) on every slider whose range is not
  0–100, answering with the live position; the check reads "24", "12",
  "99", "80", and "25" after an arrow press. Sliders with a 0–100 range
  (Rate, Chip clock) were already right.
- **Part 3's sketch** gained two buttons (batch render, pitch sweep) that
  have no letter left, and the Voice preset combo moved into the window
  proper. Enter outside the text box previews: this is not a dialog, so
  `IsDialogMessage` sends `IDOK` for Enter, which the window now handles.
- **HANDOFF's pitch 60–99 question is answered** by the sweep:
  `notes/pitch_above_63.md`. With intonation, 64 and 65 are real extra
  steps and 65–99 are identical; in monotone 63 is the ceiling.

**Left:**

1. A listening pass with NVDA 2026.1 running, written down in `notes/`.
2. `tools/build_release.py` (Phase 5 item 2).
3. The head-directory `README.md` inventory line for `echotalk-gui`.
4. ARM64 build and parity run, when there is hardware.
