# EchoTalk project handoff

Last substantially rewritten at the end of session 10, which resolved
every long-standing open bug. Read this first; `notes/` holds the
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

## Current state: the emulation is finished and correct

Speech is validated against real hardware and against MAME. **All
previously open bugs are fixed.** What does not exist yet is the
library: everything so far is diagnostic tools that turn a file of text
into a WAV.

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
    chunker.c/.h              splits text to fit Textalker's buffer
    text_prep.c/.h            modern text -> 7-bit ASCII, LF stripping
  tools/
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

## Reference baselines (regression check after any change)

Current defaults (trimmed, chunked, repeat-fix on), in samples at 8 kHz:

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

## What is left

1. **The library.** No `echotalk_init()`/`echotalk_speak()`, no DLL
   exports, no streaming output. NVDA needs audio as it is generated,
   not a WAV afterwards. This is the main remaining work and the
   emulation underneath it is ready.
2. **Look-ahead synthesis and index events** for NVDA's `IndexReached`,
   designed in `notes/buffer_chunking_and_indexing.md`, not implemented.
3. Smaller: unmapped-character policy in `text_prep` is a UX decision
   worth revisiting; true timing is implemented but opt-in because it
   loses 17 writes to latch clobbering that MAME does not (see
   `notes/true_timing_implemented_not_the_cause.md`); `tools/` has
   several historical probes that could be deleted.

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
