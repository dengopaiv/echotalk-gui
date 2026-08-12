# EchoTalk project handoff

Rewritten at the end of session 11, when the project reached the point
where everything the NVDA backend needs exists and works. Read this
first; `notes/` holds the detailed writeups and `notes/README.md` indexes
them.

## Read this first: who you're working with

You are working with **Jayson Smith** (he/him). He is **blind and uses a
screen reader**. MAME's UI, including its interactive debugger, is not
accessible to him, so **never rely on a GUI or an interactive debugger
for anything** — any verification he has to perform must come out as
plain text, or as audio he can listen to.

He is an excellent collaborator and technically sharp. Two things about
how he works are worth knowing because they have repeatedly changed the
outcome:

- **He catches real bugs by ear.** The single-character "return" bug and
  the speech-pacing defect were both found that way, the second after
  five sessions of automated measurement said everything was fine.
- **He supplies discriminating evidence, often unprompted.** The two
  hardest bugs in session 11 were solved by things he volunteered: "I
  can't reproduce it by setting the clock rate to 0%" killed a wrong
  theory instantly and pointed at the step budget, and an NVDA log
  settled the Say All question after two plausible theories had failed.

When he says something sounds wrong, believe him and investigate. When a
theory can be tested cheaply from his side, ask — it beats reasoning.

## Project goal

A C library (Windows DLL, Linux `.so`) that emulates the Apple II Echo II
speech card (TMS5220) running Street Electronics' Textalker, for use as
an NVDA speech backend. It runs the **real Textalker binary** under a
6502 emulator driving a port of MAME's TMS5220. Nothing about the speech
is reimplemented or approximated, and no Apple ROM or DOS code is used.

## Current state: complete and working

There are **no known open bugs**. In place and tested:

- Emulation validated against a real-hardware capture and against MAME.
- Both Textalker **3.1.3 and 1.3** run from one binary.
- The library, `src/echotalk.[ch]`, with the full settings surface.
- **Windows DLLs** (32- and 64-bit) and a **Linux `.so`**, ABI 6,
  44 exports.
- **Streaming**: `speak()` queues and returns; `read()` synthesises.
- **Index events** for NVDA progress reporting.
- **An NVDA add-on**, `nvda-addon/`, working in real NVDA.
- Output is **bit-identical between Windows and Linux**.

What has *not* been tested is in "Untested ground" below. Read it before
claiming the project is finished.

## Repository layout

```
echotalk/
  third_party/
    fake6502/fake6502.c    public-domain 6502 core (Mike Chambers)
    tms5220_core/          our standalone C port of MAME's TMS5220.
                           This is what ships. BSD-3-Clause.
    tms5220/               UNMODIFIED MAME source, reference only, not
                           compiled. Diff against this when anything
                           looks wrong — it has paid off four times.
  roms/                    Textalker images, user-supplied, proprietary.
  src/
    echotalk.c/.h          THE LIBRARY. Public API, documented in the
                           header, which is the API reference.
    chunker.c/.h           splits text to fit Textalker's buffer
    text_prep.c/.h         modern text -> 7-bit ASCII, LF stripping
    resample.c/.h          linear resampling, phase carried across calls
  tools/
    say.c                  the CLI; drives the library as a host would
    render_text_loader.c   THE canonical harness, both Textalker
                           versions, and what the baselines are measured
                           with
    render_common.h        shared CLI, trimming, WAV output, chunking
    test_dll.py            loads the DLL via ctypes, as NVDA does
    test_dll_load.c        the same via GetProcAddress, so the 32-bit
                           DLL gets tested too
    test_nvda_driver.py    runs the NVDA driver with NVDA stubbed out
    listen_check.py        writes a self-narrating WAV to check by ear
    wav_stats.c            onset/duration/clipping measurement
    test_text_prep.c, test_chunker.c   pure-logic tests, no ROM needed
    render_v13.c, render_text_real_chip.c   kept as independent
                           cross-checks; the latter shows the onset
                           glitch on purpose
    (probe_*, render_from_bytes, render_trace, render_hi_real_chip,
     boot_and_probe, render_wav, resample_wav are historical probes.
     Safe to ignore, and safe to delete.)
  nvda-addon/              the NVDA add-on; see its own README
  notes/                   investigation writeups; notes/README.md
                           indexes them and flags the superseded ones
```

## Building

Toolchain is MSYS2. **The `gcc` on PATH is an old Cygwin install and
must not be used.**

```
make native          # host build
make win64 / win32   # 64-bit UCRT, 32-bit MSVCRT
make dll             # echotalk.dll, both architectures, + import libs
make so              # Linux/macOS libechotalk.so
make test            # pure-logic unit tests, no ROMs needed
make test-dll        # load the 64-bit DLL from Python, as NVDA does
make test-dll-load   # the same from C, BOTH architectures
make listen          # writes a self-narrating WAV to check by ear
```

Either Windows target builds from any MSYS2 shell: the Makefile finds the
right compiler by absolute path and verifies it with `gcc -dumpmachine`,
refusing to build rather than producing a mislabeled binary.
`notes/mingw_build_system.md`

**Trap when building from Git Bash rather than an MSYS2 shell.** Git for
Windows sets `MSYSTEM=MINGW64`, so the Makefile concludes it is already
in the right subsystem and reaches for the bare `gcc` — the Cygwin one.
The architecture guard catches it, so nothing mislabeled is produced, but
you must name the compilers:

```
make win64 win32 CC_WIN64=/s/msys/ucrt64/bin/gcc.exe CC_WIN32=/s/msys/mingw32/bin/gcc.exe
```

## Running

```
say [options] <loader.bin> <obj.bin> [text] <out.wav>
  --file PATH   read the text from a file (omit the text argument)
  --speed MULT  0.25-4.0, speed only, pitch unchanged — the one to use
  --rate HZ --clock MULT --frame-rate N --compressed
  --pitch N --volume N --word-delay N --repeat-filter N
  --chunk N --no-chunk --raw
```

```
render_text_loader [options] <loader.bin> <obj.bin> <input.bin> <out.wav>
```

The harness takes raw bytes — Echo control codes and text, high bit NOT
set; it sets it. Options: `-v`, `--no-trim`, `--keep-chunk-gaps`,
`--no-repeat-fix`, `--raw`, `--chunk N`, `--no-chunk`. Diagnostic
environment variables: `ECHOTALK_CHIP_TRACE`, `ECHOTALK_RESETL4`,
`ECHOTALK_BYTE_DUMP`, `ECHOTALK_ARRIVAL`, `ECHOTALK_POLL_TRACE`,
`ECHOTALK_BANK_TRACE`, `ECHOTALK_PC_WATCH`, `ECHOTALK_PHASE`,
`ECHOTALK_CPU_HZ`, `ECHOTALK_TRUE_TIMING`, `ECHOTALK_INSTANT`.

## The library

**`src/echotalk.h` is the API reference and documents itself.** What
follows is only what is hard to infer from it.

```c
echotalk *et = echotalk_create(loader_path, obj_path, err, sizeof err);
echotalk_set_speed(et, 1.5);          /* speed only, continuous       */
echotalk_speak(et, "Hello.");         /* queues; does NOT synthesise  */
while ((n = echotalk_read(et, buf, 1024)) > 0) { /* 16-bit mono PCM   */ }
```

### Streaming

`echotalk_speak()` queues text and returns immediately; `echotalk_read()`
synthesises one utterance at a time on demand. First audio for a 437-byte
passage arrives in 39 ms against 220 ms for the whole thing.

Synthesis measures **90–155x real time** depending on the machine, which
is why there is no worker thread — inline synthesis from an audio
callback has ample headroom. A host that disagrees can call
`echotalk_synthesize()` from its own thread; the library stays
thread-free and is **not thread-safe**.

Two things a host must know:

- `echotalk_available()` is **0** right after `speak()`. It means
  "samples ready", not "speech outstanding" — that is
  `echotalk_pending()`.
- `echotalk_read()` returning 0 still means finished, because it
  synthesises before giving up.

### Index events

`\x04 7I` places a mark; `echotalk_next_index()` collects it. Drain after
**every** read including the one that returns 0, or a mark at the end of
the text never fires.

A mark does **not** end the utterance. It used to, which made positions
exact and made NVDA's Say All read a wrapped sentence one line at a time.
A mark at an utterance boundary is still exact; one inside is placed
proportionally by character offset, which is necessarily approximate —
Textalker emits nothing until the terminating CR, so there is no way to
observe which character is being spoken.
`echotalk_set_index_break()` restores the old behaviour.
`notes/say_all_index_breaks.md`

### The five rate-ish controls are independent

| control | effect |
|---|---|
| pitch | pitch only, Textalker's own `nP` |
| **speed** | **speed only, continuous 0.25–4.0 — use this one** |
| frame rate | speed only, the chip's own four fixed steps |
| clock multiplier | speed *and* pitch, the sped-up-tape effect |
| sample rate | neither; output format only |

**The last two are not as independent as that table suggests.** The chip
produces 8000 x the clock multiplier, so at 1.5 it is really generating
12000 Hz, and an output rate below that downsamples through a resampler
with no anti-aliasing filter — the discarded band folds back in instead
of being removed. A host must treat the output rate as a **floor** and
raise it to at least `8000 * clock`. The library deliberately does not do
this itself; `say` and the NVDA driver both do.
`notes/clock_output_rate_downsampling.md`

Speed works by scaling how fast the chip's parameter state machine walks
a frame while the lattice filter keeps producing one sample per output
sample. **Pitch measured at 129.0 Hz from 0.5x to 3.0x**; the clock
multiplier takes it to 258.1 Hz as it should. 1.0 is byte-exact by
construction. The delivered ratio is monotonic but sub-linear (2.0 gives
1.84x, 3.0 gives 2.56x), which matches FIFO starvation — Textalker's flow
control setting the pace. Left uncalibrated on purpose.
`notes/continuous_speed_accumulator.md`

### Settings are mirrored and readable back

Ctrl-E commands embedded in text still reach Textalker untouched, but the
library also watches them go past and updates its own variables. Twelve
getters report the voice actually in force however it was set, and every
value a getter returns is accepted by its matching setter — which is what
lets a voice be carried to a fresh instance when switching Textalker
version. `notes/settings_mirroring.md`

**`nP` and `nF` are one Textalker setting, not two.** `nP` sets pitch and
normal intonation; `nF` sets the same pitch and monotone. There is no
separate flatness value and no default for one. **Command letters are
case-insensitive**, and **pitch does not saturate at 63** — `\x05 99P` is
audibly not `\x05 63P`, though sniffed values are clamped to 0–63 so
getters and setters always agree.

### Behaviour worth knowing

- A one-character utterance is wrapped in `Ctrl-E L`/`Ctrl-E A` and
  restored **before the terminating CR**. A bare `,` is silent without
  the wrapping; restoring after the CR instead makes Textalker announce
  the CR as "return". The restore goes back to the *tracked* modes, so a
  caller's choice of letter or punctuation mode survives.
- Dead air is trimmed at the head of **every** utterance, not just the
  first. Getting this wrong leaves an audible pause at every chunk
  boundary.
- Boot audio is discarded, which matters most for v1.3 and its ~1.37 s
  calibration delay.
- **Single instance only.** Fake6502 keeps the CPU in globals, so
  `echotalk_create()` fails if one already exists. Creation costs 0.3 ms
  (v3.1.3) or 7 ms (v1.3), so destroy-and-recreate is a perfectly good
  way to switch voice.

### Ctrl-D driver commands

Driver settings are reachable from inside the text, in the shape of
Textalker's own Ctrl-E commands: `\x04`, optional whitespace, an optional
number, a letter. `notes/ctrl_d_driver_commands.md`

```
\x04 1.5S  speed          \x04 0.75C clock         \x04 2F  frame rate
\x04 0B    chunking off   \x04 1R    raw text      \x04 7I  index mark
\x04F      that setting's default      \x04\x04    one literal 0x04
```

- **A command ends the current utterance** (except index marks).
  Textalker buffers a whole line and synthesises nothing until the CR, so
  a command's position in the text otherwise bears no relation to its
  position in the audio.
- **The namespaces are disjoint and the letters differ.** Ctrl-E `F` is
  flatness and `C` is compressed; Ctrl-D `F` is frame rate and `C` is
  clock. Ctrl-D does not duplicate Textalker's commands because Ctrl-E
  sequences survive text preparation untouched.
- Bad commands are swallowed rather than spoken, and counted in
  `echotalk_command_errors()`.

## The DLL and the NVDA add-on

`make dll` builds both architectures plus import libraries. 44 exports,
undecorated cdecl on both, so `ctypes.CDLL` finds them by plain C name.
`-static` leaves only KERNEL32 plus the C runtime the subsystem implies
(UCRT for win64, MSVCRT for win32). **Check with
`objdump -p echotalk.dll | grep 'DLL Name'` after touching the link
line** — a stray `libwinpthread-1.dll` shows up as the driver silently
failing to load on a user's machine. `notes/dll_packaging.md`

`echotalk_abi_version()` returns **6**. A host loading at runtime has no
compile-time check, so bump it whenever the surface changes.

The add-on is in `nvda-addon/` and has **its own README** covering build,
packaging, settings and diagnostics. Points that are easy to get wrong:

- **Voices are discovered, not configured.** Any `<stem>.ram.bin` +
  `<stem>.obj.bin` pair beside the driver becomes a voice, labelled from
  the banner read by briefly booting it.
- **Text is sanitised in the driver**, not the library: Ctrl-D, Ctrl-E
  and Ctrl-V are replaced with a space, because the pipeline would
  otherwise obey them and a document containing one would silently change
  the voice. Replaced rather than deleted, so the rest of the line is
  still spoken.
- **Sliders are logarithmic for rate and chip clock** — 50% is 1.0x and
  every 25% doubles. Defaults are pinned to the Echo's own values;
  `defaultVal` on a `DriverSetting` is settable, NVDA does not insist on
  50%.
- **Cancellation is a generation counter, not a flag**, and `cancel()`
  must never take the library lock. Both cost real bugs.
  `notes/nvda_cancel_race.md`
- **A `PitchCommand` on a monotone voice must emit `nF`, not `nP`**, or
  NVDA's capital-letter pitch bump silently un-flattens the voice.
- `tools/test_nvda_driver.py` runs the driver without NVDA by stubbing
  what it imports — including the metaclass that turns `_get_x`/`_set_x`
  into properties, which the driver depends on and a naive stub misses.

## Reference baselines (regression check after any change)

**Measured with `render_text_loader`, not the library.** The library
legitimately differs: it sends a settings block ahead of the text (~49
samples) and treats single-character utterances differently by design.

Harness defaults (trimmed, chunked, repeat filter off), samples at 8 kHz:

| input | v3.1.3 | v1.3 |
|---|---|---|
| hi_only | 2248 | 2246 |
| onset_this | 10335 | 10332 |
| onset_a | 1246 | 1245 |
| repeat_letters | 7646 | 6445 |
| chunked_paragraph_test | 135164 | 132756 |
| command_coverage_test | 242912 | 208697 |
| alphabet_song_extracted_text | 95493 | 93999 |
| demo_bas_extracted_text | 286064 | 290753 |
| Hedge Trimmer Story | 188764 | 187953 |

Through the library (`say --file`), v3.1.3: hi_only **2297**, onset_this
**10384**, chunked_paragraph_test **135213**, Hedge Trimmer Story
**188812**. Single characters: `,` **3131**, `a` **1329**, `?` **8931**.

**The one that matters most**: `hi_only` must render with amplitude range
**−12127 / +26833** under both versions. That is an exact match to a
real-hardware capture and has survived every change so far. If it moves,
something is wrong.

Also expect: no `WARNING`, no `*** WILD JUMP TRAP ***`, `make test`
passing, and `echotalk_overruns()` zero.

Older notes quote larger sample counts. Those predate the session-10
pacing fix and dead-air trimming; **do not treat them as targets**.

## Key facts established (do not re-derive)

- **Both Textalker versions run from one binary.** The OBJ load address
  comes from the trampoline templates in the loader (`$D0xx` -> `$D000`,
  `$D4xx` -> `$D400`); the character entry comes from running the loader,
  calling `$BA69` with the Z flag set, and reading CSWL at `$36`/`$37`
  (`$BA7C` for v3.1.3, `$BA82` for v1.3). Nothing is keyed to a specific
  build, so 3.1.2/3.1.4 should work unrecognised.
  `notes/multi_version_support_design.md`
- **Textalker lives in the language card at `$D000-$FFFF`, shadowing the
  monitor ROM.** `LDA $C08B` selects the card, `LDA $C08A` selects ROM.
  Both harnesses model this with an all-RTS ROM shadow; a flat memory
  model silently writes "ROM stubs" over Textalker's image.
- The loader returns with **ROM** selected, so jumping straight at the
  OBJ entry lands in the shadow. Always enter via the trampoline.
- **The runaway guards are not tuning knobs, and a non-zero
  `echotalk_overruns()` is always a fault.** `STEP_BUDGET` was 5,000,000
  and needed 20,000,000 at the slowest exposed settings; it truncated
  speech in complete silence. It is 64,000,000 now and every overrun is
  counted and reported. `notes/step_budget_truncation.md`
- **`echotalk_chunk_text()` stops when the caller's array fills and says
  nothing about the text it never reached.** Callers must loop.
  `src/echotalk.c` does; **`tools/render_common.h` still calls it once
  with a 256-entry array** and silently drops the tail of any line
  needing more (20,480 characters at the default chunk size). Worth
  fixing; left alone so far to avoid disturbing the reference harness.
- **Textalker's own line-buffer bound is never initialised here.**
  `$FD80-$FD82` read as zero because `$D781` is never reached — nothing
  plays the part of DOS setting up a screen. Its auto-flush point is
  therefore undefined, which is why chunking is not optional.
- **Text preparation changes lengths**, so a character offset taken
  before it does not survive it. Anything tracking offsets must prepare
  in runs and record offsets in the prepared buffer.
- Compressed/expanded speech works by skipping phoneme segments, not by
  changing playback rate.
- v1.3 does not implement the `D` (inter-word delay) command at all.
- Textalker never uses 6502 decimal mode (measured), though BCD is
  enabled anyway for correctness.
- `TMS5220_READ_COMMAND_DELAY_SAMPLES = 600` in `tms5220_core.c` is still
  a tuned hack, not derived timing. It survived the pacing investigation
  but was never the cause of anything; treat with suspicion if it ever
  seems implicated.
- True timing (`ECHOTALK_TRUE_TIMING=1`) is implemented, produces
  byte-identical output, and no longer clobbers anything. There is no
  reason to switch the default. The reservation in
  `notes/true_timing_implemented_not_the_cause.md` no longer applies.

### Echo/Textalker command reference

All prefixed with Ctrl-E (`\x05`) unless noted. Case-insensitive.

`nP` pitch 0-63 (default 24, normal intonation), `nF` same pitch but
monotone, `nV` volume 0-15 (default 12), `nD` inter-word delay 0-15
(v3.1.3 only), `C`/`E` compressed/expanded, `T` talk-only, `nR` repeat
filter, `L`/`W` letter/word mode, `A`/`S`/`N` all/some/no punctuation,
`\x16` (Ctrl-V) phoneme mode terminated by CR.

**Send `\x05` `99` `R` once after init** to disable the repeat filter,
which otherwise speaks `EEEEEEEEE` as `EE`. The tools do this by default.

## Untested ground

Everything above is verified on **x86 Windows**, plus one Linux run.
What has not been touched:

- **ARM is entirely untried** — no Apple Silicon, no Raspberry Pi. Jayson
  has no ARM hardware. The bit-identical Windows/Linux result was between
  two x86 builds and says nothing about a different architecture, which
  is precisely where a floating-point difference would be plausible.
  Re-run `make listen` and compare MD5s the first time anyone has one.
- **The 32-bit build has never run on 32-bit hardware.**
  `make test-dll-load` runs the real 32-bit binary against the real
  32-bit DLL, but under WoW64. That covers the code being 32-bit, not the
  machine.
- **macOS** has never been built or run.

**Cross-platform determinism is a recorded baseline, not an assumption.**
Windows and Linux produced byte-identical WAVs — same MD5 over 1,096,604
samples — despite doubles running through the lattice filter, the
cycle-to-sample accumulator and the resampler. So a future cross-platform
difference is a real bug, not drift.

## What is left

Nothing is blocking. In rough order of value:

1. **Ship it.** The add-on works in real NVDA. Packaging for other people
   means deciding how they supply Textalker images.
2. Fix the `render_common.h` chunker truncation noted above.
3. Delete the historical probes in `tools/`.
4. Jayson wants a test file speaking a short sentence at **pitches 60
   through 99**, to look at what Textalker does above its documented
   range — `\x05 99P` is audibly not `\x05 63P`, and he suspects a
   Textalker bug. Deferred by agreement; the library clamps to 63.
5. The unmapped-character policy in `text_prep` is a UX decision worth
   revisiting.
6. Re-measure the baseline table with `say` if the library is to become
   the reference rather than the harness.

## Working practices that paid off

- **Ask for a discriminating test rather than guessing.** Twice in
  session 11 a single observation from Jayson replaced a wrong theory
  that had already produced code. Both times the theory was plausible and
  both times the code would have been useless.
- **When the question is "what does that other program do", ask that
  program.** Fifteen minutes of logging what NVDA actually sends settled
  what two rounds of reasoning had not.
- **Diff against the unmodified MAME source in `third_party/tms5220/`.**
  Four defects came from the mechanical port, including a missing
  `#define` that caused the pacing bug and was invisible to `-Wall`.
- **A limit that can truncate must report it.** The step budget cut
  speech in silence. Picking a bigger number fixes today's bug; making it
  impossible to hit one quietly is what stops the next.
- **Check the magnitude before chasing a mechanism.** True timing was
  implemented in full before anyone noticed its delays are 13–25 µs
  against a 25 ms symptom.
- **Verify a regression test fails on the broken code.** A test that
  passes either way proves nothing. Done twice in session 11, and the
  first version of one of them did not catch its own bug.
- **Do not assert exact equality of audio.** Four checks broke this way
  in one session: comparing whole buffers when the claim was about
  content, and a one-sample alignment shift making sample-wise
  comparison meaningless. Compare content, allow a small shift, and say
  what you actually mean.
- **In MAME's log, trust counts, not timing.** Frame counts, byte counts
  and event counts are reliable; anything derived from ordering across
  the CPU/chip boundary is not. MAME's recorded audio is not chip time
  either — its own counter showed 12,911 samples where the `.wav` held
  11,200.
