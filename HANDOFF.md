# EchoTalk project handoff (for Claude Code)

## Read this first: who you're working with

The person you're working with is **blind and uses a screen reader**.
MAME's UI (including its interactive debugger) is not accessible to
them, so **do not rely on MAME's GUI or interactive debugger for
anything**. Any MAME-based verification must go through headless /
command-line output (logerror text output redirected to a file, or a
batch/nogui run) that can be read as plain text. This constraint
shaped several earlier decisions in this project and should keep
shaping them.

They are also extremely sharp technically and have been an excellent
collaborator throughout -- they've caught real bugs by ear (see
"HI timing fixed" in the session log) and by reasoning about hardware
history (see the Cricket investigation). Take their observations
seriously; when something "sounds off" to them, there is reliably a
real, findable reason.

## Project goal

A C/C++ library (eventually Windows DLL / Linux .so) that emulates
the Apple II Echo II speech synthesizer card (TMS5220 chip) running
Street Electronics' Textalker software, for use in NVDA and similar
screen readers. No dependency on Apple ROM/DOS/Monitor code for the
shipping library (a couple of test harnesses stub a few ROM calls for
diagnostic purposes only -- see below). Ships as a direct 6502
emulation of Textalker driving a ported TMS5220 chip.

## Current state: it mostly works, with one well-understood gap

Short version: **synthesis is correct and validated** (matches a real
hardware reference recording exactly in duration and amplitude for
simple cases, and produces plausible, clean output for full sentences
and a wide command-coverage test). There is **one remaining, now
well-diagnosed issue**: a subtle "sluggishness" in speech pacing
compared to real hardware (confirmed by the user via direct
side-by-side comparison against a MAME recording), and **the fix is
now clear and scoped** -- see "THE thing to do next" below.

## Repository layout

```
echotalk/
  third_party/
    fake6502/fake6502.c        -- public-domain 6502 CPU emulator (Mike Chambers)
    tms5220_core/               -- OUR ported/simplified TMS5220 emulator (BSD-3-Clause,
                                    derived from MAME). This is what the library actually
                                    uses. Uses "hacky instant-completion" mode -- see below.
    tms5220/                    -- REAL, UNMODIFIED MAME source for tms5220.cpp/h and
                                    a2echoii.cpp/h (BSD-3-Clause), kept here as ground-truth
                                    REFERENCE ONLY -- not currently compiled into anything.
                                    This is what you need for the next step.
  roms/                         -- Textalker binaries (v3.1.3 and v1.3), extracted from
                                    user-provided disk images. No Apple ROM/DOS needed for
                                    v3.1.3. v1.3 needs a few Apple ROM stubs (see below).
  src/
    chunker.c / chunker.h       -- text-to-Textalker-buffer-size chunking logic. Fully
                                    implemented, unit-tested, working correctly. Not part
                                    of the open issue.
  tools/                        -- many test/probe harnesses built up over the project;
                                    the two that matter most going forward:
    render_text_real_chip.c     -- the CANONICAL v3.1.3 test harness. Takes a raw byte
                                    stream (Echo/Textalker control codes + text, high bit
                                    NOT set) and an output WAV path. This is the one to
                                    build on.
    render_v13.c                -- same, but for Textalker v1.3 (needs the RAM loader +
                                    OBJ, and a couple of ROM stubs -- see comments in file).
    resample_wav.c              -- naive/aliasing upsampler (e.g. to 22050 Hz) for nicer
                                    listening; unrelated to the open issue.
  notes/                        -- write-ups of investigation findings, in chronological
                                    order of discovery. Read buffer_chunking_and_indexing.md
                                    and v13_vs_v3_cricket_investigation.md for the most
                                    directly relevant background.
```

Build any tool with something like:
```
gcc -O2 -o /tmp/render tools/render_text_real_chip.c \
    third_party/fake6502/fake6502.c \
    third_party/tms5220_core/tms5220_core.c \
    third_party/tms5220_core/tms5220_reset.c \
    -Ithird_party/tms5220_core
```
Then: `/tmp/render roms/textalker.obj.bin <input_bytes_file> <output.wav>`

## What's been established (don't re-derive these)

1. **Textalker's per-line buffer is tied to screen width** (up to 80
   chars in 80-column mode), not a fixed size -- confirmed via
   disassembly of `$D781`. The chunker in `src/chunker.c` is built
   around this and works correctly (word-boundary and clause-boundary
   splitting, with a hard-split fallback for pathological unbroken
   text). Not part of the open issue.

2. **Card/slot detection**: Textalker scans 8 candidate I/O addresses
   (`C0 D0 F0 B0 A0 90 E0 F2`, corresponding to slots 4,5,7,3,2,1,6,
   plus one extra) looking for a responding Echo card. This scan is
   identical in both v1.3 and v3.1.3 -- it's old, generic, and slot 2
   (5th tried) isn't privileged for any Cricket-related reason.

3. **A major bug was found and fixed**: our simplified TMS5220 port
   was completing the `READ BYTE` / `READ AND BRANCH` commands
   *instantly* within the same write, when real hardware takes real
   time. This corrupted the very first status read Textalker's card
   detection does (and, separately, corrupted a per-word VSM
   dictionary lookup that short common words like "HI" trigger). Fixed
   with a deferred-completion mechanism in `tms5220_core.c`
   (`TMS5220_READ_COMMAND_DELAY_SAMPLES`, currently `600`, found by
   empirical sweep against known-good output -- **this value is a
   hack tuned against symptoms, not derived from real timing, and
   should likely be replaced by the proper fix below**).

4. **v3.1.3 has a `READ BYTE` probe (`$0B`, `$9E` written during
   `$D682` init) that v1.3 does not have at all** -- confirmed by
   exhaustive byte-pattern search of both v1.3 files. v1.3 works
   correctly with the timing fix completely disabled (byte-for-byte
   identical output either way); v3.1.3 needs it. This was traced down
   while investigating a hypothesis (from the user, about Cricket
   detection) that turned out to be *partially* right: not "Cricket
   gets slot priority" (refuted), but "v3.1.3 added a new probe step
   ahead of the same old inherited scan, and that step is exactly what
   turned out to be timing-sensitive." See
   `notes/v13_vs_v3_cricket_investigation.md` for the full writeup.

5. **v1.3 doesn't support the `D` (delay-between-words) command at
   all** -- confirmed via disassembly of the command dispatch table at
   `$D857` in `roms/textalker_v13.obj.bin`: `D` (`0xC4`) is flat-out
   missing from the chain (jumps straight from testing `C` to `E`).
   Unrecognized commands fall through to a shared harmless exit point
   that discards any accumulated digits and does nothing else. This is
   correct, matches real v1.3 behavior, and explains why our v1.3
   command-test recording came out shorter than the v3.1.3 one (v3.1.3
   actually applies the delay it was told to; v1.3 just ignores it).

6. **Compressed/expanded speech (`C`/`E` commands) work by skipping
   phoneme segments, not by changing playback rate.** Confirmed by
   disassembling both branch targets in v1.3: the "compressed" path
   (`$ECEC`) decrements a counter and jumps ahead in the pronunciation
   table without transmitting that frame; the normal path (`$EE64`)
   does the actual frame-preparation work. This means the TMS5220's
   own per-frame playback rate is *not* affected by compression --
   whatever a fix for the open issue below turns out to be, it's not
   "compression exposes bad timing," because compression never
   touches that code path.

## THE thing to do next (this is the important part)

**We already have the real, unmodified MAME source for both
`tms5220.cpp` and `a2echoii.cpp` saved in `third_party/tms5220/`.**
Getting this was the whole reason a MAME build was being considered --
but it turns out we already had it from earlier in the session
(before a context compaction lost track of that). **Check there
first before building anything.**

Reading `third_party/tms5220/a2echoii.cpp` reveals the real
architectural gap: **real Echo II hardware uses MAME's "true timing"
mode** -- the Echo II card wires up `m_tms->rsq_w()`, `m_tms->wsq_w()`,
and a `ready_cb()` callback (`tms_readyq_callback`), meaning writes to
the chip are genuinely gated by the chip's real `/READY` line. If
Textalker writes before the chip is ready, **the data in the write
latch gets silently clobbered** (see the `logerror` warning in
`write_c0nx`).

Our port (`third_party/tms5220_core/`) never implemented this --
it uses what MAME itself calls "hacky instant write mode" (calling
`data_w()`/`status_r()` directly, with no `/READY` gating at all).
The `TMS5220_READ_COMMAND_DELAY_SAMPLES` hack was a workaround bolted
onto that instant-completion model, tuned by black-box sweeping
against symptoms -- it happens to produce correct output for the
cases tested so far, but it is not modeling the same mechanism real
hardware uses, which is almost certainly why the residual pacing
difference persists.

**The real timing constants are documented directly in MAME's own
source**, in `tms5220_device::wsq_w()`
(`third_party/tms5220/tms5220.cpp`, around line 1955):

```cpp
/* Now comes the complicated part: how long does /READY stay inactive,
   when /WS is pulled low? This depends ENTIRELY on the command
   written, or whether the chip is in speak external mode or not...
   Speak external mode: ~16 cycles
   Command Mode:
   SPK: ? cycles
   SPKEXT: ? cycles
   RDBY: between 60 and 140 cycles
   RB: ? cycles (80?)
   RST: between 60 and 140 cycles
   SET RATE (5220C and CD2501ECD only): ? cycles (probably ~16)
*/
// TODO: actually HANDLE the timing differences! currently just assuming always 16 cycles
m_timer_io_ready->adjust(clocks_to_attotime(16), 1);
```

Two important things fall out of this:

- These are **TMS5220 chip clock cycles** (640kHz for a standard Echo
  II, confirmed in `a2echoii.cpp`: `TMS5220(config, TMS_TAG, 640000)`)
  -- not 6502 CPU cycles, and not our audio-sample clock. At 640kHz,
  16 cycles is 25 microseconds; RDBY's 60-140 cycles is roughly
  94-219 microseconds. This is a **completely different order of
  magnitude** from the `TMS5220_READ_COMMAND_DELAY_SAMPLES=600`
  *samples* (at 8000 Hz, 600 samples is 75 *milliseconds*) our hack
  uses -- confirming that hack was compensating for something other
  than real per-command latency, likely by accident hitting a value
  large enough to survive several of Textalker's own internal
  polling/retry loops rather than modeling the chip correctly.

- **MAME itself acknowledges (in that TODO) that it doesn't model
  RDBY's real 60-140 cycle latency precisely either** -- it uses a
  flat 16-cycle approximation for every command in true-timing mode.
  This matters for scoping the fix: the goal should be **parity with
  what MAME actually produces** (since that's the user's reference and
  what they're comparing against by ear), not chasing real-hardware
  perfection beyond what MAME itself claims. Replicating MAME's true
  timing model (including its known-approximate parts) is very likely
  sufficient and is the right target.

### Concrete plan

1. Implement proper `/RS`, `/WS`, and `/READY` (`m_io_ready`) modeling
   in `third_party/tms5220_core/tms5220_core.c`, porting the relevant
   logic from `third_party/tms5220/tms5220.cpp`'s `rsq_w()`, `wsq_w()`,
   and `set_io_ready()` (the `TIMER_CALLBACK_MEMBER`). Our port doesn't
   have MAME's timer/scheduler infrastructure, so this needs
   translating into the same "advance N chip clock cycles, then apply
   the deferred effect" pattern already used for
   `TMS5220_READ_COMMAND_DELAY_SAMPLES` -- except now driven by real
   chip-clock cycles (640kHz) rather than audio samples (8000Hz), and
   with the correct per-command cycle counts from the comment above
   (or MAME's actual flat-16-cycle behavior, if matching MAME exactly
   is preferred over matching the datasheet).
2. Update the harness's `write6502()`/`read6502()` (in
   `tools/render_text_real_chip.c` and `tools/render_v13.c`) to route
   through this real `/RS`/`/WS` protocol instead of directly calling
   `data_w()`/`status_r()`, matching what `a2echoii.cpp`'s
   `read_c0nx()`/`write_c0nx()` actually do (including the "data
   gets clobbered if written before ready" behavior).
3. Remove or retire `TMS5220_READ_COMMAND_DELAY_SAMPLES` once true
   timing subsumes what it was working around.
4. Re-validate against everything in the "test cases that must keep
   working" list below.
5. If, after that, a pacing difference *still* remains: that's the
   point where an actual MAME build becomes worth the effort, to
   capture either a reference WAV for direct waveform/timing
   comparison, or `logerror` trace output (`LOG_RS_WS`, `LOG_IO_READY`,
   `LOG_READYQ`, `LOG_READ`, `LOG_WRITE` -- all already defined as
   `LOGMASKED` flags in the reference source, just commented out via
   the `VERBOSE` define at the top of each file) for cross-referencing
   exact command timing. This would need to run **headless** (batch
   mode with logging redirected to a file, no interactive debugger --
   see the accessibility note at the top). Do this only if step 1-4
   doesn't resolve it, since we may already have everything needed.

## Test cases that must keep working (regression check after any change)

All of these were validated as correct by the user before the current
open issue was raised -- don't let a true-timing implementation break
them. Reference input files for all of these are bundled in
`reference_text/` (raw byte streams, high bit NOT set, ready to feed
straight into `render_text_real_chip.c` / `render_v13.c`):

- `reference_text/hi_only.bin`: should produce ~2811 samples at
  8000 Hz, amplitude range approximately -12127 to +26833, zero
  clipping. This is the one directly validated against a real
  hardware reference recording (originally uploaded by the user as
  `Real_Hi.wav`, not included here -- ask the user if you need it
  again).
- `reference_text/chunked_paragraph_test.bin`: a short multi-sentence
  paragraph, already split by the chunker. Should produce plausible,
  proportional-length output with minimal clipping (~19s / 153283
  samples was the validated baseline).
- `reference_text/command_coverage_test.bin`: the full
  pitch/flatness/volume/delay/compressed/expanded/phoneme-mode test.
  Should run without hitting any step budget / without hanging, for
  both v3.1.3 (`render_text_real_chip.c`, ~33.6s baseline) and v1.3
  (`render_v13.c`, ~29.9s baseline, includes its own ~1.3s calibration
  lead-in silence at the start that v3.1.3 doesn't have).
- `reference_text/demo_bas_extracted_text.bin` and
  `reference_text/alphabet_song_extracted_text.bin`: real Street
  Electronics demo program text (extracted from `demo.bas` and
  `Alphabet Song.bas`). These are **known to currently hang** on the
  single-letter-word bug described below -- useful for testing whether
  a true-timing fix incidentally resolves that too, not for pass/fail
  regression checking yet.
- v1.3 with the (eventual replacement for) the timing fix disabled vs
  enabled should still produce identical output for text that doesn't
  exercise the READ BYTE-adjacent code paths (`hi_only.bin` is a good
  test for this), and the loader's calibration delay should still land
  at ~552,000 6502 steps (this was an independent cross-check against
  this project's very first session notes and is a good canary that
  v1.3 boot is still being reproduced correctly).

### Echo/Textalker command syntax reference (confirmed working)

All values below use Ctrl-E (`\x05`) as prefix unless noted:
- Pitch: `\x05` + digits(0-63) + `P` (default 24)
- Flatness/monotone: `\x05` + digits(0-63) + `F` (same scale as pitch)
- Volume: `\x05` + digits(0-15) + `V` (default 12)
- Delay between words: `\x05` + digits(0-15) + `D` (default 0;
  **v1.3 does not support this at all, silently ignored**)
- Expanded/slow speech: `\x05E` (default)
- Compressed/fast speech: `\x05C`
- Talk-only mode (no screen echo assumed): `\x05T`
- Repeat-character filter: `\x05` + digits + `R`. Collapses runs of the
  same character (so `*****` is not read out one "star" at a time) but
  applies to letters too, so `EEEEEEEEE` is spoken as `EE`. **Send
  `\x05` + `99` + `R` once after init to effectively disable it** --
  the render tools do this by default (`--no-repeat-fix` to opt out).
  See `notes/input_handling_repeat_filter_and_encoding.md`.
- Phoneme mode: `\x16` (Ctrl-V) + phoneme string + `\r` to terminate.
  Known-good test string (from Street Electronics' own demo.bas,
  extracted this session): `OR3%2M@PRO3GRAMM&%SI/FO3N&1MZS,4W'R3DS#NDS`
  (pronounces roughly "or you may program me using phonemes, word
  sounds")
- All Echo command characters and plain text need the **high bit
  set** when actually sent to Textalker's `$D006` (v3.1.3) or `$D400`
  (v1.3) entry points -- the render tools do this automatically
  (`| 0x80`) given plain ASCII input, so input files should NOT have
  the high bit pre-set.
- That high bit is a **transport** convention, not an encoding. Text
  must already be 7-bit ASCII before it gets there: Textalker predates
  Latin-1, Windows-1252 and UTF-8 entirely, and a raw byte >= `$80`
  in the input collides with the transport convention. `src/text_prep.c`
  converts modern text (UTF-8 or legacy single-byte, detected per
  character) down to ASCII, and also strips LF -- the Apple II ends
  lines with CR, and Textalker announces a stray LF as "linefeed" in
  all-punctuation mode. Run it **before** the chunker, since it can
  change the text's length.

## Single-letter-word bug: SOLVED (session 8)

`reference_text/demo_bas_extracted_text.bin` used to hang/run away
when it hit a single-letter word ("B", used as a buffer-mode-toggle
command that falls through to being spoken as literal text). This
turned out to be **unrelated** to the READ BYTE timing issue --
root cause was a missing Apple ROM stub: `$D66D` branches on carry
between two entry points into the same real ROM routine (`$BA83`,
already stubbed as `PLA;RTS`, and `$BA88`, which was not). Landing on
the unstubbed `$BA88` executed into unmapped zeroed memory, hit
`BRK`, and looped forever through the zero interrupt vector. Fixed by
stubbing `$BA88` the same way as its sibling. Full writeup, including
the several ruled-out hypotheses along the way, in
`notes/single_letter_word_bug_fixed.md`. Verified against `hi_only.bin`,
`chunked_paragraph_test.bin` (both unchanged, exact match), the
isolated "B" case (was infinite hang, now 0.030s), and the full
`demo_bas_extracted_text.bin` (was runaway, now 40.087s, zero
warnings).

`tools/render_v13.c` (Textalker v1.3) was not checked for an
analogous gap -- worth a quick pass if v1.3 single-letter words are
ever found to hang the same way.

**Both harnesses now have a wild-jump trap**: the IRQ/BRK vector
points at `$0300` instead of zeroed memory, so any future wild jump
into unmapped memory (the exact failure mode here) is caught
immediately with the precise originating address printed, instead of
silently spinning for hours. If you ever see `*** WILD JUMP TRAP ***`
in output, that's this working as intended -- go stub whatever real
ROM address it names.

## Onset glitch investigation (session 9): understood, NOT fixed, reverted

A brief click at the very start of some utterances (e.g. "This",
single letter "A") when speech begins immediately after a cold reset
with no prior activity. Root cause fully understood and confirmed
genuine (not a bug -- our lattice filter is byte-for-byte identical to
MAME's): voiced phonemes use the full 10-coefficient filter, and
starting that from zero internal history briefly "rings up" before
settling. Three fix attempts were tried; all had a real, disqualifying
problem (a spoken warm-up word shifts unrelated prosody; a naive idle
loop has rounding-error phase drift and makes things worse; a real
6502-executed delay fixes the onset well but silently truncates
whatever speech follows it, for a not-yet-found reason). Current state
is **reverted** to the simple, no-delay, validated-correct behavior --
the glitch is present but nothing is broken. Full writeup, including
exactly what was ruled out and a concrete starting point for whoever
picks this up next, in `notes/onset_glitch_investigation_reverted.md`.
**Read that before touching this again** -- in particular, always
verify full output duration against the `reference_text/` baselines,
not just the onset, when testing any future fix here; that's exactly
what attempt 3 above got wrong until very late.

## Build system (session 9)

A real `Makefile` now exists at the project root: `make native` (Linux
dev builds), `make win64` / `make win32` (static Windows builds, for
the eventual NVDA add-on environment which won't have MinGW runtime
DLLs available). Both Windows targets were actually cross-compiled
and run (under Wine, with actual output verified byte-identical to the
Linux baselines) in this session, not just written and assumed to
work -- see `notes/mingw_build_system.md` for the full verification
writeup and an important caveat: the Linux-side cross-compilers used
to test this target the older MSVCRT runtime, not UCRT, so `make
win64` run from a real MSYS2 UCRT64 shell (which is what's actually
wanted) hasn't been directly verified, only reasoned to be low-risk.
Also note MSYS2 has no UCRT-based 32-bit environment -- `make win32`
needs to be run from MSYS2's MinGW32 shell instead, which is
documented in both the Makefile's comments and the notes file.



- The user notices real bugs by ear reliably. When they say something
  sounds wrong, believe them and investigate rather than assuming the
  measurement (duration, amplitude) that looks fine is the whole
  story.
- Prefer disassembly + direct evidence over speculation. Several
  earlier hypotheses in this project turned out to be wrong in
  specifics even when directionally reasonable (e.g. "Cricket gets
  slot priority") -- the fix each time was going back to the actual
  bytes rather than continuing to reason abstractly.
- Watch for debug instrumentation accidentally changing behavior --
  the biggest false lead this session was a debug logging statement
  that called `tms5220_status_r()` unconditionally (for a print
  statement) instead of only on the real status-read phase, which
  silently masked the very bug being investigated. When a "fix"
  seems to work, double check it isn't the debug harness itself doing
  the work.
