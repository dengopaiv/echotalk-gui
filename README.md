# EchoTalk — a from-scratch Textalker/Echo II emulator

Goal: a small C/C++ library (Windows DLL / Linux .so) that emulates
Street Electronics' TEXTALKER 3.1.3 driving a TMS5220, so it can be used
as a synth backend (e.g. an NVDA driver), plus a CLI test tool
(text in -> WAV out).

## Status: early proof-of-concept, NOT yet producing speech

This is the honest state of the project after the first pass. It proves
the hard, uncertain parts are solvable, and sets up the pieces needed
to finish it.

## What's confirmed (validated two independent ways)

**The files you gave me:**
- `TEXTALKER.RAM` — a 543-byte boot loader. Copies the engine into the
  Apple II language card and originally lived at `$9300`.
- `TEXTALKER.OBJ` — the 12,282-byte engine itself. TEXTALKER (TM) v3.1.3,
  (c) 1985 Street Electronics Corp / 1986 American Printing House for the
  Blind, by Street, Levieux, Kory, and Skutchan. Loads at `$D000` and
  fills memory up to `$FFF9` (the file size, 0x2FFA bytes, lines up
  exactly with that range). Filename suffixes (`_069300`, `_06d000`) are
  CiderPress/ProDOS-style `_TTAAAAAA` tags confirming filetype $06 (BIN)
  and those load addresses.

**The Echo II card's bus protocol**, confirmed two ways:
1. By disassembling `TEXTALKER.OBJ` directly (see `notes/disasm_fd53.txt`
   below) — found the byte-send routine at `$FD53` (poll status at
   `$C0A9`, wait for a bit, write byte to `$C0A8`) and the status-poll
   routine at `$FD60`.
2. By pulling MAME's actual card driver, `a2echoii.cpp` (BSD-3-Clause,
   traced from real hardware by R. Belmont / Lord Nightmare / Tony Diaz):
   **every** address in `$C0A0-$C0AF` is wired to the same latch — reads
   alternate between a real TMS5220 status byte and a dummy `$FF`
   pull-up value, and any write latches a byte and pulses the chip's
   `/WS` pin. This is a nicely quirky, very "real 1985 hardware" protocol,
   and it matches what the disassembly showed.
3. **Tested it**: `tools/probe_writebyte.c` boots real Textalker code
   under a 6502 emulator, jams the PC straight into `$FD53` with a stub
   Echo-card model based on (2), and confirms Textalker correctly polls
   and writes a byte, exactly as MAME's hardware model predicts. Run it
   yourself with `make probe && ./probe`.

Slot: the card sits at `$C0A0-$C0AF`, which is Apple II slot 2 — the
traditional default slot for the Echo II.

## Third-party code vendored in `third_party/` (all permissive, DLL-safe)

- **`fake6502/`** — Mike Chambers' Fake6502, **public domain**. A tiny,
  well-tested 6502 core. You provide `read6502()`/`write6502()`; it does
  the rest. (I stripped a debugger-hook fork's extra dependencies to get
  back to the plain public-domain core — see file header.)
- **`tms5220/tms5220.cpp` + `.h`** — MAME's actual TMS5220/TMS5200
  emulator, **BSD-3-Clause**. This is the real prize: decades of
  hardware-accurate reverse engineering of the LPC synthesis chip, and
  it's license-compatible with a closed or open commercial DLL as long
  as you keep the copyright notice.
- **`tms5220/a2echoii.cpp` + `.h`** — MAME's Echo II card driver,
  **BSD-3-Clause**, kept as reference only (not compiled) — it's written
  against MAME's `device_t` framework, not standalone.

Both BSD-3 files just need their license header preserved somewhere in
your distribution (e.g. a THIRD_PARTY_LICENSES file);  they don't
obligate you to open-source EchoTalk itself.

## What's NOT done yet — the real remaining work

1. **Port `tms5220.cpp`'s DSP core out of MAME's `device_t` wrapper into
   a standalone C/C++ struct+functions.** The class shell (device
   lifecycle, `save_item`, MAME sound-stream callbacks) needs stripping;
   the actual LPC lattice-filter/chirp-table synthesis code underneath
   is ordinary, portable DSP code. This is exactly what the
   `jotego/TMS5220_FPGA` project did for a different purpose — it's
   mechanical but has ~2200 lines to go through carefully so timing bugs
   don't creep in.
2. **Full disassembly of Textalker's dispatch loop** to find the real
   "feed me a string, please speak it" calling convention — right now
   I've only proven the *lowest-level* byte-out routine works; I haven't
   yet mapped the higher-level entry points (`$D003`/`$D006` look like
   the two public jump-table entries) or the buffer/state Textalker
   expects to be initialized before you call them. Some of this will
   involve tracing zero-page variable usage across the whole 12K image.
3. **A minimal Apple II memory shim** — language-card softswitches,
   keyboard stub, maybe a tiny monitor-ROM stub at `$F800+` so any
   incidental `JSR` into "ROM" territory doesn't crash — needed once we
   run the full loader/init path rather than jamming PC directly like
   the current probe does.
4. Wire real TMS5220 output into a WAV writer, then the library API
   (`echotalk_init`, `echotalk_speak(text)`, `echotalk_get_audio(...)`)
   and the CLI tool (stdin/file → WAV).

## MAJOR UPDATE: the real calling convention (from the official manual)

The Street Electronics Echo manual is on archive.org, and Appendix A
("Using Textalker From Assembly Language") gives the exact answer we
needed — this changes the plan for the better:

**All input characters must have the Apple II high bit set** (standard
Apple II text convention — 'H' is sent as `$C8`, not `$48`), and:

**To speak text, you don't need to reverse-engineer Textalker's internal
state machine at all.** You just do, per character:
```
LDA #char        ; with high bit set, e.g. $C8 for 'H'
JSR $FDED        ; GOUT -- the *standard* Apple II monitor character-out vector
```
Textalker's installer patches `$FDED` (a fixed, well-known address --
it's normally the Apple monitor ROM's COUT routine) to point into its
own intake routine. Once that patch is in place, feeding it text is just
a character-by-character `JSR $FDED` loop, terminated with a `$8D`
(high-bit RETURN) to flush/speak the buffered line. This matches
<cite index="71-1">manual guidance that programmers should JSR to "GOUT" ($FDED) with the character to be sent in the accumulator</cite>.

This is great news for the emulator: we don't need to fully map
Textalker's internal dispatch tables to drive it. We just need to:
1. Run the loader + init once so `$FDED` gets patched (real init code,
   not skipped).
2. Then for each character of the user's text: put it (high bit set) in
   `A`, `JSR $FDED`.
3. Send `$8D` at the end of each utterance to flush.

**Confirmed control codes, straight from the manual:**
- `Ctrl-E` (`$85` w/ high bit) precedes all Echo/Textalker commands --
  <cite index="71-1">a Control-E precedes all ECHO commands, and whenever a Control-E is printed Textalker interprets the following characters as a command</cite>.
  Single-letter commands include `O`/`T`/`B` (output-only / talk-only /
  both), `C`/`E` (compressed/expanded rate), `L`/`W` (letter/word mode),
  `S`/`M`/`A` (some/most/all punctuation spoken), plus numeric+letter
  combos: `nP` (pitch 1-63), `nF` (flat/monotone pitch), `nV` (volume
  0-15), `nD` (inter-word delay 0-15), `nR` (repeat-character filter).
  The command prefix character itself can even be reassigned (except to
  CR/Ctrl-U/Ctrl-H/Ctrl-J).
- `Ctrl-V` (`$96` w/ high bit) switches to **phoneme mode**: <cite index="71-1">once Textalker receives a Control-V it pronounces everything as phonemes until a RETURN character is received, which reverts it to normal text-to-speech</cite> -- exactly the mechanism you described. The manual's own assembly example sends `$96`, then phoneme-code bytes, then `$8D`, each via `JSR $FDED` just like normal text.

This was also enough to identify a 33-entry character dispatch table at
`$D817`/`$D838`/`$D859` in the OBJ image (see
`notes/dispatch_table_d817.txt`) that appears to be Textalker's
in-command-mode character classifier (handles CR/ESC/backspace/
linefeed and the single-letter mode commands E/C/S/M/A/L/W plus
digit accumulation for the numeric commands) -- consistent with
everything above, though we no longer strictly need to fully map it
since `$FDED` gives us a clean, documented entry point.

**Also from Appendix A**, confirmed memory map for the DOS 3.3
version (matches what we found by inspection): loader lives at
`$9300-$94FF` (installation only), engine at `$D000-$FEB3`.

### Next concrete step
Build a minimal "enough of an Apple II" stub so the real loader can run
to completion without crashing: a handful of monitor-ROM entry points
the loader/init code touches (e.g. `$FE1F`) need to at least return
harmlessly, and zero page needs to look plausible. Then: boot loader →
confirm `$FDED` got patched → `JSR $FDED` in a loop with test text →
confirm bytes come out the Echo I/O latch in the right pattern for real
words. That's the point where wiring in the ported TMS5220 turns it
into actual audio.



The disassembly/control-flow work (originally "step 2") turned out to
be largely solved by the manual rather than needing exhaustive tracing
-- so the next session should go straight at the ROM stub + `$FDED`
harness above, then the TMS5220 standalone port.

---

# SESSION 3 UPDATE (supersedes the "Next concrete step" above)

## BREAKTHROUGH: direct call into Textalker confirmed working (no $FDED, no DOS, no Apple ROM code)

**The bug in session 2:** `$D009` (reached via the loader's `$BCF0`
trampoline) looked plausible but is actually Textalker's **keyboard-input
echo / blinking review-cursor** logic -- every path through it blocks
waiting for a real keypress, which is why it never returned.

**The real entry point:** Textalker's own init routine at `$D682`
(reached from its *other* public jump-table entry, `$D003`) installs
the Apple's CSWL/CSWH output vector (`$36/$37`) to point at `$BA7C` --
which disassembles to:
```
BA7C: PHA           ; caller pushes the character here
      LDA $C08B      ; language-card softswitch (harmless in our flat model)
      JMP $D006      ; -> JMP $D5DE, Textalker's real per-character dispatcher
```
So the real calling convention is: **push the character on the stack,
then jump to `$D006`** (which is just `JMP $D5DE`). No `$FDED`, no DOS
hook chain, no zero-page CSWL vector needed at all -- we call `$D006`
directly. `tools/probe_d5de.c` does exactly this.

**Confirmed working end-to-end**: sending `Ctrl-E,T` (talk-only mode),
then `"HI"`, then RETURN, character by character via this entry point:
- Mode-set and letter characters process cleanly with no Echo-card
  traffic (correct -- Textalker buffers a word before speaking it).
- RETURN triggers real processing: **58 bytes written to the Echo
  card's data port**, ending in a run of `$FF` bytes (expected right
  now -- our echo-card stub always reports "ready" instantly instead
  of behaving like a real TMS5220 FIFO with proper timing/flow
  control, so Textalker dumps data without being throttled; this
  should resolve once the real ported TMS5220 replaces the stub).

This directly satisfies both things asked for this session:
- **No Apple ROM/DOS code needed at all.** The "stubs" in
  `probe_d5de.c` are just RTS placeholders for a handful of addresses
  the *loader* touches during its own housekeeping while booting --
  none of them reproduce actual Apple monitor ROM or DOS 3.3 code.
- **Calling Textalker directly**, bypassing `$FDED` and the DOS hook
  chain entirely.

## Eliminating TEXTALKER.RAM entirely (scoped, not yet done)
Since we now call `$D006` directly and never rely on the DOS hook
chain the loader installs (`$A22B`/`$BA69`, the Ctrl-C intercept) or
the keyboard-echo hook (`$BCF0`/`$D009`), most of what
`TEXTALKER.RAM` does is now irrelevant to us. What's left to check
before dropping it from the runtime image and setting up memory
directly from our own C code:
- The banner-print loop (cosmetic, skippable).
- `$D682`'s own init actions we likely still need: `$FD9E = 0` (a
  state flag) and the conditional `$C0AA`/`$C0AB` writes (speech-ROM
  table address setup) -- these come from `$D682` itself (part of
  OBJ, not the loader), so we may be able to call `$D682` (via `$D003`)
  *once* from C as the one piece of "loader-equivalent" setup we keep,
  and drop the 543-byte `TEXTALKER.RAM` file from the shipped product
  entirely.

## Next concrete step
1. Test calling `$D003` once directly from C (no `TEXTALKER.RAM` run
   at all), mimicking a "fresh install" (push `$00` before jumping in,
   matching the `PLA/PHA/BEQ` check inside `$D682`), then call `$D006`
   per character and confirm the same 58-byte stream comes out for
   "HI". If it does, `TEXTALKER.RAM` can be dropped entirely and the
   whole init sequence reimplemented natively in C (a handful of
   memory pokes), which is both simpler and avoids needing to execute
   *any* original 6502 loader code at all.
2. Begin the TMS5220 standalone port (extract the DSP core out of
   MAME's `device_t` wrapper) so the echo-card stub can be replaced
   with the real chip model and the `$FF`-padding artifact goes away,
   turning the byte stream into actual audio.

---

# SESSION 3, PART 2: TEXTALKER.RAM eliminated entirely (confirmed)

`tools/probe_no_loader.c` loads **only** `TEXTALKER.OBJ` (12,282 bytes)
-- no loader file at all -- and reproduces the *exact same* 58-byte
Echo-card output for "HI" as the loader-based version. The full,
minimal init sequence, with no Apple ROM or DOS code involved:

1. Load `TEXTALKER.OBJ` at `$D000` in a flat 64K image.
2. Call `$D003` once, having pushed `$00` on the stack first (mirrors
   a real `PHA #0` before the jump -- `$D682` peeks this value to
   decide "fresh install" vs "chain an existing hook").
3. Call `$FCD6` once (no setup needed) -- this is Textalker's own
   TMS5220 presence/signature check, and it turned out to matter: it
   pokes low-level jump targets (used by the byte-I/O routine at
   `$FD53`) that the speech pipeline depends on. This was the missing
   piece the first no-loader attempt lacked.
4. Per character: push the character (high bit set, per the manual),
   jump to `$D006`.

The only "stub" needed anywhere is two bytes at `$BA83` (`PLA`/`RTS`)
-- the fixed low-memory address `$D682`/`$D5DE` jump to when done,
normally populated by `TEXTALKER.RAM`'s DOS-hook installer, which we
don't need since we call Textalker directly. That's two bytes of code
we wrote ourselves, not anything copied from Apple or Street
Electronics -- so the whole pipeline now has **zero dependency on any
original Apple ROM/DOS code, and no dependency on TEXTALKER.RAM**.

This is now the reference calling convention for the eventual C
library: `echotalk_init()` does steps 2-3 once; `echotalk_putchar(c)`
does step 4 per character.

## Updated next steps
1. Extract the TMS5220 DSP core out of MAME's `device_t` wrapper into
   standalone C (still the main remaining task) -- this replaces the
   dumb "always ready" echo-card stub with real chip emulation,
   producing actual audio instead of a raw byte dump, and should also
   resolve the trailing `$FF`-padding artifact (real flow control).
2. Build the actual library API (`echotalk_init`, `echotalk_speak`,
   `echotalk_render_to_pcm`) and CLI test tool around
   `probe_no_loader.c`'s proven init/putchar sequence.
3. Map the remaining Textalker command set we haven't exercised yet
   (pitch/volume/rate/punctuation-mode numeric commands) by testing
   them the same empirical way -- send the command sequence, confirm
   sensible Echo-card output -- now that the harness works.
