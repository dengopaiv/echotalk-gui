# MinGW build system (session 9)

## What's here
A `Makefile` at the project root with four real targets: `native`,
`win64`, `win32`, `windows` (both), plus `clean`. Builds the three
tools that currently exist: `render_text_real_chip` (canonical v3.1.3
harness), `render_v13` (v1.3 harness), and `resample_wav` (naive
upsampler). There is no NVDA-consumable library/DLL yet -- see
"What's NOT here" below.

## Why static builds
The eventual NVDA add-on environment won't have MinGW's runtime DLLs
(`libgcc_s_seh-1.dll`, `libwinpthread-1.dll`, etc.) available, so
every Windows build uses `-static`. This project is plain C with no
threading, so `-static` alone is sufficient -- no need for
`-static-libgcc`/`-static-libstdc++` separately, and no
`-lwinpthread` dependency to worry about since nothing here uses
threads.

## Why win64 and win32 need different MSYS2 environments
This one's worth being explicit about, since it's easy to get wrong:

- **win64** should target UCRT (the newer Universal C Runtime),
  matching MSYS2's **UCRT64** shell. Running `make win64` from inside
  that shell just works -- its `gcc` already targets ucrt64.
- **win32** -- MSYS2 does **not** offer a UCRT-based 32-bit
  environment. UCRT64 is 64-bit only. For a 32-bit build (needed
  because older NVDA releases run as 32-bit processes), use MSYS2's
  **MinGW32** shell instead, which targets the older MSVCRT runtime.
  This is fine: MSVCRT ships on every Windows version, including the
  older ones a 32-bit NVDA build is likely to actually run on.

The Makefile's `CC_WIN64`/`CC_WIN32` variables auto-detect via the
`MSYSTEM` environment variable (which MSYS2 shells set automatically)
whether to use plain `gcc` (native MSYS2 build) or the
`x86_64-w64-mingw32-gcc`/`i686-w64-mingw32-gcc` cross-compiler prefixes
(cross-compiling from Linux/Mac). Override explicitly if needed, e.g.
`make win64 CC_WIN64=x86_64-w64-mingw32-gcc`.

## What was actually verified here (not just written and hoped)
This dev sandbox is Linux, so an actual MSYS2 UCRT64 shell wasn't
available to test directly. What WAS done:

1. Installed `gcc-mingw-w64-x86-64` and `gcc-mingw-w64-i686` (Ubuntu's
   MinGW-w64 cross-compilers -- these target MSVCRT, not UCRT, which
   is a real difference from what `make win64` will produce when run
   inside actual MSYS2 UCRT64; see caveat below).
2. Built all three tools for both architectures via the Makefile
   (`make win64`, `make win32`) -- clean compiles, only pre-existing
   warnings (unused variable, ignored `fread` return values) that
   also show up in the native Linux build; nothing Windows-specific.
3. Confirmed static linking actually worked: inspected import tables
   with `objdump -p` on all six resulting `.exe` files. Every one
   depends on exactly two DLLs -- `KERNEL32.dll` and `msvcrt.dll` --
   both of which ship with Windows itself. No MinGW runtime DLLs.
4. Installed Wine (both `wine64` and 32-bit `wine32:i386` support) and
   **actually ran all six binaries**, not just inspected them. Every
   one produced byte-identical output to the validated Linux
   baselines:
   - `render_text_real_chip.exe` on `hi_only.bin`: 2811 samples,
     amplitude -12127/26833 -- exact match, both architectures.
   - `render_v13.exe` on the same input: loader ran 551888 steps
     (matching the documented v1.3 calibration-delay figure), 13210
     samples total -- exact match, both architectures.
   - `resample_wav.exe`: correctly resampled 2811→7747 samples for
     22050 Hz -- exact match, both architectures.

## Caveat: MSVCRT vs UCRT wasn't directly tested
The cross-compilers available via Ubuntu's package manager target the
older MSVCRT runtime for both architectures, not UCRT. This is a
genuine, known gap: `make win64` run from an actual MSYS2 UCRT64 shell
will produce a binary linking `ucrtbase.dll` instead of `msvcrt.dll`,
which was not directly exercised here. This should be low-risk --
the source code only uses portable, standard C library calls (fopen,
fread, fprintf, malloc, etc.) with no MSVCRT-specific or UCRT-specific
behavior depended on anywhere -- but "should be low-risk" isn't the
same as verified, and it's worth actually running `make win64` from a
real UCRT64 shell once one is available, as a final check.

## UPDATE (session 10): both caveats above now closed on real Windows

The two open items -- "UCRT was never directly tested" and "win32 needs
the MinGW32 shell, untried" -- were both resolved by building on the
user's actual Windows machine (MSYS2 at `S:\msys`, gcc 15.2.0), not
under cross-compilation or Wine.

**win64 / UCRT64** (`S:\msys\ucrt64\bin\gcc.exe`):
- Compiles clean; the only warning is the same pre-existing
  `unused variable 'err'` in `tms5220_lattice_filter` that the Linux
  build also emits. Nothing UCRT-specific surfaced.
- Import table is `KERNEL32.dll` plus the `api-ms-win-crt-*` UCRT
  forwarder DLLs -- i.e. it really is linking UCRT rather than MSVCRT,
  which is what was never confirmed before. No MinGW runtime DLLs.
- `hi_only.bin` renders to 2811 samples, amplitude -12127/+26833 --
  exact match to the validated baseline.

**win32 / MinGW32** (`S:\msys\mingw32\bin\gcc.exe`, installed via
`pacman -S mingw-w64-i686-gcc`):
- All three tools build clean and static.
- Import table is exactly `KERNEL32.dll` + `msvcrt.dll` -- the older
  runtime that ships with every Windows version, which is the point of
  using MinGW32 for the 32-bit build.
- `render_text_real_chip`: 2811 samples, -12127/+26833. `render_v13`:
  loader ran 551888 steps (matching the documented v1.3 calibration
  figure exactly) and 13210 samples. Both exact matches.

So the MSVCRT-vs-UCRT question is settled empirically: identical output
from both runtimes, and the 32-bit MSVCRT build is the one to prefer for
distribution breadth.

### One trap worth knowing about
The Makefile auto-detects the compiler as `$(if $(MSYSTEM),gcc,...)`.
That is correct when you run `make win32` from the **MinGW32** shell,
but if you run `make win32` from the **UCRT64** shell it will silently
use UCRT64's `gcc` and produce a *64-bit* binary in `build/win32/`.
`check-mingw32` won't catch this, because `which gcc` succeeds either
way. Until that's hardened, check which shell you're in, or pass the
compiler explicitly:
`make win32 CC_WIN32=/s/msys/mingw32/bin/gcc`

## What's NOT here yet
This builds the existing *test/diagnostic tools*, which speak text
from a file to a WAV file -- useful for continued validation, but not
what an NVDA add-on would actually link against. There is no
`echotalk_init()`/`echotalk_speak()`-style C API, no DLL export
surface, and no incremental/streaming audio output (NVDA needs audio
as it's generated, not a completed WAV file after the fact). Building
that real library, and adding a `make win64-dll`/`win32-dll`-style
target for it, is future work.
