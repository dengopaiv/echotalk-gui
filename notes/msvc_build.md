# Building with MSVC

**Status: done, 2026-09-15.** The library compiles unmodified under
Microsoft's compiler, and its output is bit-identical to the MinGW build
that ships in the add-on. The GUI (`gui-native/`) is built this way.

## What was run

Visual Studio 18 Community, toolset 14.51, x64. First as a probe, by hand:

```
vcvars64.bat
cl /nologo /O2 /W3 /std:c11 /I src /I third_party\tms5220_core /I tools ^
   tools\say.c src\echotalk.c src\text_prep.c src\chunker.c src\resample.c ^
   third_party\fake6502\fake6502.c ^
   third_party\tms5220_core\tms5220_core.c third_party\tms5220_core\tms5220_reset.c
```

Exit 0 with five warnings and no source changes:

- three C4244 narrowing conversions in `third_party/tms5220_core/tms5220_reset.c`
  (lines 110, 111, 117: `int16_t`/`uint16_t` into `uint8_t`), which is the
  port's code and matches what MAME does;
- C4996 for `fopen` and `strcpy`, silenced with `_CRT_SECURE_NO_WARNINGS`.

They are left as they are. `gui-native\build.cmd` now does the same thing
in two passes: the library and emulators at `/W3`, the GUI alone at
`/W4 /WX`, and links `say-<arch>.exe` from the same objects.

## The comparison that proves it

The MSVC `say.exe` against the add-on's shipped `echotalk64.dll` (built by
MinGW-w64 UCRT), driven through ctypes as NVDA drives it, both reading the
images in `..\echotalk\synthDrivers\echotalk\`. Text:
`Hi. The quick brown fox jumps over the lazy dog, 1985.`

| Settings | Samples | MD5 of the samples (both builds) |
|---|---|---|
| defaults | 47274 | `67579800080983a24bbfcbaf1bd1dad5` |
| speed 1.5 | 33208 | `b1a666f7928d30d7e413983eace986f4` |
| clock 1.5, rate 12000 | 47274 | `67579800080983a24bbfcbaf1bd1dad5` |
| pitch 40, volume 9, word delay 5, compressed | 43525 | `0631cdb18a7bc52aaac22b332c1a1610` |

HANDOFF already recorded Windows and Linux as bit-identical; this is a third
compiler agreeing, despite doubles in the lattice filter, the speed
accumulator and the resampler.

Note the clock row: at clock 1.5 with a 12000 Hz output the *samples* equal
the defaults, and only the rate they are declared at differs. The whole
clock effect lives in the playback rate, which is why the GUI's WAV writer
takes the rate from `echotalk_sample_rate()` and never assumes 8000.

Since then `tools/verify_gui.py` extends the comparison to 114 cases through
the GUI itself — see `gui-native/README.md`.

## Building from Git Bash

`vcvarsall.bat` prints "'vswhere.exe' is not recognized" under this Visual
Studio install; it is harmless. Run `dumpbin` with `MSYS_NO_PATHCONV=1`, or
Git Bash rewrites `/dependents` into a path.
