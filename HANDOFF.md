# EchoTalk project handoff

Last substantially rewritten at the end of session 10, which resolved
every long-standing open bug; session 11 fixed the single-character
"return" bug found after that. Read this first; `notes/` holds the
detailed writeups, referenced from here where relevant.

## Read this first: who you're working with

The person you're working with is **blind and uses a screen reader**.
MAME's UI (including its interactive debugger) is not accessible to
them, so **do not rely on MAME's GUI or interactive debugger for
anything**. Any MAME-based verification must go through headless /
command-line output that can be read as plain text. This constraint
shaped many decisions in this project and should keep shaping them.

They are also extremely sharp technically and an excellent
collaborator. They catch real bugs by ear reliably, and they test
claims rather than taking them -- twice in session 10 that caught a
genuine error of mine (see "Working practices" at the end). When they
say something sounds wrong, believe them and investigate.

## Project goal

A C library (Windows DLL / Linux .so) that emulates the Apple II Echo II
speech synthesizer card (TMS5220) running Street Electronics' Textalker,
for use as an NVDA speech backend. It ships as a real 6502 emulation of
Textalker driving a ported TMS5220, with no dependency on Apple ROM or
DOS code.

## Current state: emulation correct, library working, DLL not packaged

Speech is validated against real hardware and against MAME. **All
previously open bugs are fixed**, and **the library now exists** --
`src/echotalk.[ch]`, driven by `tools/say.c`. What is not done is the
DLL export surface, streaming output, and NVDA index events.

Fixed in session 10, in order:

- **Onset glitch** (open since session 9). Cause: the harness skipped
  Textalker's real loader. Booting through it fixes the glitch and
  forced the language-card model below. `notes/onset_glitch_fixed_by_real_loader.md`
- **Speech pacing / "sluggish compressed speech"** (open since session
  7). Cause: `#define FAST_START_HACK 1` was lost in the port from MAME
  while both `#ifdef FAST_START_HACK` sites came across, so the blocks
  compiled to nothing and every restart cost an extra 25 ms frame.
  `notes/pacing_fast_start_hack.md`
- Long text split mid-word: the chunker existed but was never wired in.
  `notes/chunker_wired_in_buffer_bounds_uninitialised.md`
- Textalker's repeat filter mangling real words, LF being spoken as
  "linefeed", and no handling of non-ASCII input.
  `notes/input_handling_repeat_filter_and_encoding.md`
- Dead air before every utterance, proportional to its length.
- Four defects from the mechanical C++-to-C port.
  `notes/tms5220_port_dangling_statements.md`

## Repository layout

```
echotalk/
  third_party/
    fake6502/fake6502.c       public-domain 6502 core (Mike Chambers)
    tms5220_core/             our standalone C port of MAME's TMS5220.
                              This is what ships. BSD-3-Clause.
    tms5220/                  UNMODIFIED MAME source, reference only,
                              not compiled. Diff against this when
                              anything looks wrong -- it has paid off
                              four times.
  roms/                       Textalker binaries, user-supplied,
                              proprietary. See THIRD_PARTY_LICENSES.md.
  src/
    echotalk.c/.h             THE LIBRARY. Public API; the harness's
                              pipeline turned into something callable.
    chunker.c/.h              splits text to fit Textalker's buffer
    text_prep.c/.h            modern text -> 7-bit ASCII, LF stripping
    resample.c/.h             linear resampling, no anti-aliasing
  tools/
    say.c                     drives the library the way a host would;
                              the end-to-end test that the API works
    render_text_loader.c      THE canonical harness. Handles BOTH
                              Textalker versions from one binary.
    render_common.h           shared CLI, trimming, WAV output, chunking
    render_v13.c              v1.3-only harness, now redundant with the
                              above; kept as an independent cross-check
    render_text_real_chip.c   superseded direct-init harness, kept for
                              A/B comparison (it shows the onset glitch)
    wav_stats.c               onset/duration/clipping measurement
    test_text_prep.c,
    test_chunker.c            pure-logic unit tests, no ROM needed
    (probe_*, render_from_bytes, render_trace, render_hi_real_chip,
     boot_and_probe, render_wav are historical probes; safe to ignore)
  notes/                      investigation writeups, chronological
```

## Building

Toolchain is MSYS2. **The `gcc` on PATH is an old Cygwin install and
must not be used.**

```
make native     # host build
make win64      # 64-bit, UCRT runtime
make win32      # 32-bit, MSVCRT runtime (for older 32-bit NVDA)
make test       # pure-logic unit tests, needs no ROMs
```

Either Windows target builds from any MSYS2 shell -- the Makefile finds
the right compiler by absolute path and then verifies it with
`gcc -dumpmachine`, refusing to build rather than producing a
mislabeled binary. Both were verified on real Windows.
`notes/mingw_build_system.md`

## Running

```
render_text_loader [options] <loader.bin> <obj.bin> <input.bin> <out.wav>
```

Input is raw bytes -- Echo/Textalker control codes and text, high bit
NOT set; the tool sets it. Options:

- `-v` boot diagnostics and per-character progress
- `--no-trim` keep dead air (needed to reproduce raw sample counts)
- `--keep-chunk-gaps` keep dead air between utterances only
- `--no-repeat-fix` leave Textalker's repeat filter at its default
- `--raw` skip text preparation
- `--chunk N` / `--no-chunk` line-splitting control (default 80)

Diagnostic environment variables: `ECHOTALK_CHIP_TRACE`,
`ECHOTALK_RESETL4`, `ECHOTALK_BYTE_DUMP`, `ECHOTALK_ARRIVAL`,
`ECHOTALK_POLL_TRACE`, `ECHOTALK_BANK_TRACE`, `ECHOTALK_PC_WATCH`,
`ECHOTALK_PHASE`, `ECHOTALK_CPU_HZ`, `ECHOTALK_TRUE_TIMING`,
`ECHOTALK_INSTANT`.

## The library

`src/echotalk.h` is the API and documents itself; the short version:

```c
echotalk *et = echotalk_create(loader_path, obj_path, err, sizeof err);
echotalk_set_compressed(et, 1);      /* Textalker's own two rates    */
echotalk_set_frame_rate(et, 2);      /* 0-3, speed only, no pitch    */
echotalk_set_clock_multiplier(et, 1.5); /* speed AND pitch           */
echotalk_set_sample_rate(et, 22050); /* output format only           */
echotalk_speak(et, "Hello.");
while ((n = echotalk_read(et, buf, 1024)) > 0) { /* 16-bit mono PCM */ }
```

Plus `echotalk_set_pitch` (0-63), `_volume` (0-15), `_word_delay`
(0-15, **no effect under v1.3**, which never implemented that command),
`_repeat_filter` (0-99, default 99 so it never triggers), `_chunk_size`
(0 disables chunking) and `_raw`.

### Ctrl-D driver commands

Driver settings are also reachable from inside the text, in the shape of
Textalker's own Ctrl-E commands: `\x04`, optional whitespace, an
optional number, a letter. `notes/ctrl_d_driver_commands.md`

```
\x04 2F  frame rate    \x04 0.75C clock     \x04 0B  chunking off
\x04 1R  raw text      \x04F      default   \x04\x04 literal 0x04
```

Two things to know before using them:

- **A command ends the current utterance.** Textalker buffers a whole
  line and synthesises nothing until the CR, so a command's position in
  the text otherwise bears no relation to its position in the audio.
  Flushing first is what makes "before" and "after" mean anything. The
  cost is a pause wherever a command appears.
- **The namespaces are disjoint and the letters differ.** Ctrl-E `F` is
  flatness and Ctrl-E `C` is compressed; Ctrl-D `F` is frame rate and
  Ctrl-D `C` is clock. Ctrl-D does not duplicate Textalker's commands
  because it does not need to -- Ctrl-E sequences survive text
  preparation untouched.

Settings set this way persist past the call. Bad commands are swallowed
rather than spoken and counted in `echotalk_command_errors()`; `say`
warns about them, and about chunking being off however it got that way.

Which Textalker version you get is decided by the images passed;
`echotalk_version()` reports the parsed banner for display.

The four rate-ish controls are deliberately independent -- pitch changes
pitch only, frame rate changes speed only, the clock multiplier changes
both, and the sample rate changes neither. See
`notes/library_plan_rate_and_pitch.md`.

Behaviour worth knowing:

- An utterance that is exactly one character after preparation and
  chunking is wrapped in `Ctrl-E L`/`Ctrl-E A` and restored with
  `Ctrl-E S`/`Ctrl-E W` **before the terminating CR**, so a lone letter
  or punctuation mark is announced rather than swallowed. A bare `,` is
  silent without the wrapping; restoring after the CR instead of before
  it makes Textalker announce the CR as "return".
- Boot audio is discarded, which matters most for v1.3 and its ~1.37 s
  calibration delay.
- Dead air is trimmed at the head of **every** utterance, not just the
  first. Getting this wrong leaves an audible pause at every chunk
  boundary, which is how the bug was found.

**Single instance only.** Fake6502 keeps the CPU in globals, so
`echotalk_create()` fails if one already exists. Fine for a screen
reader; it forecloses two simultaneous voices.

```
say [options] <loader.bin> <obj.bin> [text] <out.wav>
  --file PATH   read the text from a file (omit the text argument)
  --rate HZ --clock MULT --frame-rate N --compressed
  --pitch N --volume N --word-delay N --repeat-filter N
  --chunk N --no-chunk --raw
```

## Reference baselines (regression check after any change)

**Measured with `render_text_loader`, not the library.** The library
differs slightly and legitimately: it sends a settings block ahead of
the text (~40 samples) and treats single-character utterances
differently by design. Compare like with like, or re-measure this table
with `say` and say so here.

Current harness defaults (trimmed, chunked, repeat filter off), in
samples at 8 kHz:

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

**The one that matters most**: `hi_only` must render with amplitude
range **-12127 / +26833** under both versions. That is an exact match to
a real-hardware capture and it has survived every change so far. If it
moves, something is wrong.

Also expect: no `WARNING`, no `*** WILD JUMP TRAP ***`, and `make test`
passing.

Older notes quote larger sample counts. Those predate the pacing fix and
the dead-air trimming; do not treat them as targets.

## Key facts established (do not re-derive)

- **Both Textalker versions run from one binary.** The OBJ load address
  comes from the trampoline templates in the loader (`$D0xx` -> `$D000`,
  `$D4xx` -> `$D400`); the character entry comes from running the loader,
  calling `$BA69` with the Z flag set, and reading CSWL at `$36`/`$37`
  (gives `$BA7C` for v3.1.3, `$BA82` for v1.3). Nothing is keyed to a
  specific build, so 3.1.2/3.1.4 should work unrecognised.
  `notes/multi_version_support_design.md`
- **Textalker lives in the language card at `$D000-$FFFF`, shadowing the
  monitor ROM.** `$FDED`, `$FDF0`, `$FD1B` name Textalker code or ROM
  depending on bank state; `LDA $C08B` selects the card, `LDA $C08A`
  selects ROM. Both harnesses model this with an all-RTS ROM shadow. A
  flat memory model silently writes "ROM stubs" over Textalker's image.
- The loader returns with **ROM** selected, so jumping straight at the
  OBJ entry lands in the shadow. Always enter via the trampoline.
- **`echotalk_chunk_text()` stops when the caller's array fills and says
  nothing about the text it never reached.** Callers must loop until the
  line is consumed. `src/echotalk.c` does; `tools/render_common.h` still
  calls it once with a 256-entry array and silently drops the tail of
  any single line needing more chunks than that (20,480 characters at
  the default chunk size). Worth fixing.
- **Textalker's own line-buffer bound is never initialised here.**
  `$FD80-$FD82` read as zero because `$D781` is never reached -- nothing
  plays the part of DOS/Applesoft setting up a screen. So its auto-flush
  point is undefined and we must chunk before it can be reached.
- Compressed/expanded speech works by skipping phoneme segments, not by
  changing playback rate.
- v1.3 does not implement the `D` (inter-word delay) command at all.
- Textalker never uses 6502 decimal mode (measured), though BCD is now
  enabled anyway for correctness.
- `TMS5220_READ_COMMAND_DELAY_SAMPLES = 600` in `tms5220_core.c` is
  still a tuned hack, not derived timing. It survived the pacing
  investigation but was never the cause of anything; treat with
  suspicion if it ever seems implicated.

### Echo/Textalker command reference

All prefixed with Ctrl-E (`\x05`) unless noted:
`nP` pitch 0-63 (default 24), `nF` flatness, `nV` volume 0-15
(default 12), `nD` inter-word delay 0-15 (v3.1.3 only), `C`/`E`
compressed/expanded, `T` talk-only, `nR` repeat filter, `\x16` (Ctrl-V)
phoneme mode terminated by CR.

**Send `\x05` `99` `R` once after init** to disable the repeat filter,
which otherwise speaks `EEEEEEEEE` as `EE`. The tools do this by
default.

## Fixed in session 11: single-character utterances also spoke "return"

Found by Jayson by ear after the library was written, and fixed by
reordering one line. `notes/single_char_return_bug_fixed.md`

The library wraps a one-character utterance in `Ctrl-E L` (letter mode)
and `Ctrl-E A` (all punctuation) and restores `Ctrl-E S` / `Ctrl-E W`
afterwards, so a lone letter or punctuation mark is announced rather
than swallowed. But the CR that terminates the utterance is itself a
character, and in all-punctuation mode Textalker announces it as
"return" -- so asking for one character got two spoken items.

The restore now goes **before** the CR rather than after it. Textalker
buffers the whole line and processes it in order when the CR arrives, so
a Ctrl-E command sitting in the buffer takes effect partway through that
pass: the character was buffered ahead of the restore and is still
announced, while the CR is reached after it and stays silent.

Two things worth keeping from that investigation:

- **`Ctrl-E W` is word mode, not punctuation mode.** Only `Ctrl-E S`
  (some-punctuation) turns the announcement off. `\x05A,\r\x05W` still
  says "return"; `\x05A,\x05S\r` does not.
- The `$D009` keyboard-echo trampoline was **not** needed. Session 2 set
  it aside because it blocks on a keypress, and that is still true; it
  simply is not a problem this bug required solving.

## The DLL

`make win64-dll` / `win32-dll` / `dll` build `echotalk.dll` plus its
import library. `notes/dll_packaging.md`

21 exports, undecorated on both architectures, plain cdecl, so
`ctypes.CDLL` finds them by plain C name. `-static` inside the shared
link leaves only KERNEL32 plus the C runtime the subsystem implies
(UCRT for win64, MSVCRT for win32) -- no MinGW runtime to ship. Check
with `objdump -p echotalk.dll | grep 'DLL Name'` after touching the link
line; the failure mode is a DLL that works here and not on a user's
machine.

`echotalk_abi_version()` returns 1. A host loading at runtime has no
compile-time check available, so bump it whenever the surface changes in
a way a caller could notice.

Two test programs, and both are needed:

```
make test-dll        # ctypes, as NVDA would, 64-bit only
make test-dll-load   # GetProcAddress from C, BOTH architectures
```

The second exists because the 32-bit DLL cannot be reached from Python
here -- the only interpreter on this machine is 64-bit and Windows
refuses a bitness mismatch at load time. Both pass, and produce
identical sample counts across 32- and 64-bit.

`make so` builds the Linux/macOS `libechotalk.so`, and it works:

```
make so
make listen ECHOTALK_LIB=build/native/libechotalk.so
```

`tools/listen_check.py` needs only ctypes, exercises the whole surface,
and writes a WAV whose speech announces what each section is about to
demonstrate, so it can be checked by ear without a transcript.

**The output is bit-identical across platforms.** Jayson ran this on
Linux against a WAV produced here on Windows: same size, same MD5, no
differences under `FC /b`. Every automated check reported the same
numbers too, down to the index marks landing at samples 679837, 682085
and 685599 on both.

That is worth more than it looks. The pipeline carries doubles through
the TMS5220 lattice filter, the cycle accumulator and the resampler, and
two different compilers on two different operating systems agreed on
every one of 1,096,604 samples. **Any future cross-platform difference
is a bug, not floating-point drift** -- there is now a baseline saying
so.

The only figures that legitimately differ are wall-clock: synthesis
measured 155x real time here and 91x on Jayson's Linux box. Both have
ample headroom for inline synthesis; treat the "~136x" quoted elsewhere
as the order of magnitude rather than a constant.

## Streaming and index events

Both done in session 11. `notes/streaming_and_indexing.md`

**`echotalk_speak()` no longer synthesises.** It queues the text and
returns; `echotalk_read()` synthesises one utterance at a time, on
demand. First audio for a 437-byte passage arrives in 39 ms against
220 ms for the whole thing.

Synthesis measures **~136x real time**, which is why there is no worker
thread: doing it inline from an audio callback has ample headroom, and
the session-6 look-ahead design would have bought only complexity. A
host that disagrees can call `echotalk_synthesize()` from its own
thread; the library stays thread-free.

Two things a host must know:

- `echotalk_available()` is **0** right after `speak()`. It means
  "samples ready", not "speech outstanding" -- that is
  `echotalk_pending()`.
- `echotalk_read()` returning 0 still means finished, because it
  synthesises before giving up. Existing read loops work unchanged.

`echotalk_stop()` now abandons pending text and index events as well as
queued audio.

**Index marks** are `\x04 7I`, collected with `echotalk_next_index()`.
They are exact rather than estimated -- the mark's position is the
sample count at the moment the text before it finished synthesising.
Drain them after every read **including the one that returns 0**, or a
mark at the end of the text never fires. Like every Ctrl-D command they
end the current utterance, so clause or sentence granularity is free but
marking every word will make the prosody choppy.

## What is left

1. Smaller: re-measure the baseline table with `say` if the library is
   to be the reference; the unmapped-character policy in `text_prep` is
   a UX decision worth revisiting; continuous frame-rate control beyond
   the four steps would need the accumulator described in
   `notes/library_plan_rate_and_pitch.md`; `tools/` has several
   historical probes that could be deleted.

Note that true timing (`ECHOTALK_TRUE_TIMING=1` on the harness) is
implemented and now clobbers nothing -- the write-latch losses that kept
it opt-in were caused by reset wiping the `/READY` callback, since
fixed. It still produces byte-identical output, so there is no reason to
switch the default, but the reservation recorded in
`notes/true_timing_implemented_not_the_cause.md` no longer applies.

## Working practices that paid off

- **Diff against the unmodified MAME source in `third_party/tms5220/`.**
  Four defects came from the mechanical port: two dangling-statement
  families, a dropped `/READY` callback, and a missing `#define`. The
  last one caused the pacing bug and was invisible to `-Wall`.
- **Check the magnitude before chasing a mechanism.** True timing was
  implemented in full before anyone noticed its delays are 13-25 us
  against a 25 ms symptom.
- **In MAME's log, trust counts, not timing.** Summing stream updates
  between line numbers is not a clock -- it swept up idle time outside
  the utterance and produced a target below the state machine's own
  floor. Frame counts, byte counts and event counts are reliable.
- **MAME's recorded audio is not chip time.** Its own sample counter
  showed 12,911 samples where the `.wav` held 11,200.
- **Instrument MAME directly when stuck.** Its `LOG_*` masks plus a few
  added lines settled in one run what days of indirect measurement
  could not. MAME logs nothing at RESETL4 while idle, which hid the
  decisive evidence until a line was added there.
- Verify full output duration against baselines, not just the symptom
  being chased. A session-9 fix attempt passed its onset check while
  silently truncating speech.
