# notes/ index

Investigation writeups. `HANDOFF.md` in the project root summarises the
conclusions; these keep the reasoning, including approaches that turned
out to be wrong and why.

> **Sample counts in the older files are obsolete.** Anything written
> before session 10 predates the pacing fix and dead-air trimming;
> anything before session 11 predates the step-budget fix.
> **HANDOFF.md's baseline table is the only authority.**

## Active work: the C rewrite (branch `Textalker-RW`)

**Finished and byte-exact** against the emulation, for **both
Textalker versions**, in Python and now in C. Raw input goes in and the
exact TMS5220 byte stream comes out, with no 6502 in it: the whole
speech path, the Ctrl-E command set included. The two versions can be
mixed (`--rules` / `--voice`, though the voice data turns out to be
identical).

The C port is `src/tk_*.c`, one file per Python module, driven by
`tools/tk_phonemes.c` and `tools/tk_chip.c` and tested by
`tools/tk_diff.py` against either the Python (fast) or the emulator
(the acceptance test). No table is compiled in: everything is read out
of the user's own image at run time.

It is wired into the library as a selectable engine
(`echotalk_set_engine`), with `tools/engine_diff.py` comparing the two
engines' AUDIO. **The letter-to-sound seam it exists for is now built**:
`tk_g2p` in `src/tk.h`, with `src/tk_dict.c` (a CMUdict-format
dictionary read at run time) as the first source and
`tools/seam_diff.py` as its differential test.

The engine comparison is byte-exact at the chip bytes and not yet
phase-identical in the audio; HANDOFF's `Textalker-RW` section has the
numbers, the cycle model behind them, and why it stopped where it did.

- `phoneme_alphabet.md` -- **the table the seam stands on**: which of
  Textalker's 58 allophones is which sound, how a one-byte phoneme code
  splits into an allophone index and a pitch group, and how the
  correspondence was read (by putting every vowel of English into a
  frame whose pronunciation is not in doubt and asking stage 1, not by
  guessing at the Ctrl-V characters). Also the two things ARPABET
  cannot carry -- the pitch group, and the order of the /z/ pair -- and
  why what is written down there is a measurement rather than Street
  Electronics' data.
- `textalker3_rule_changes.md` -- **software archaeology: what Street
  Electronics changed between 1.3 and 3, and why it made some words
  worse.** Textalker 3 is not a patched 1.3 -- it is the rule set
  rebuilt on a syllabifier 1.3 does not have, trading enumeration for
  generalisation: the whole-word rules were deleted and replaced by
  general rules that ask the syllabifier questions. Names the rule
  behind each of BOOT/ROOT/SHOOT (`OO{FC}{FD}`, OO before any stop),
  COPY (the open-syllable rule, and why JOLLY survives it), NICKNAME
  (**`KN` in the cluster list**, which also breaks ACKNOWLEDGE and
  UNKNOWN) and THOUGH (a correct list of voiced-TH words replaced by an
  approximate rule). The G section is the control: 1.3 enumerates until
  its list runs out at GIRL, 3.1.3 generalises until its rule runs over
  GIANT, and both say TIGER as *tie-jer* -- method, not care. Also that
  somebody rewrote the GHOTI easter egg to survive the new syllabifier. Also what the four syllabifier word lists do (not their contents, in this fork), the
  74-of-200 measure of how far the two versions diverge, and the two
  explanations -- space pressure, and nobody testing -- that the
  evidence rules out.
- **Three disassembly listings are not in this fork:** `disasm_rule_engine.txt`
  (the 3.1.3 syllabifier and rule engine), `disasm_v13.txt` (Textalker 1.3) and
  `disasm_stage2.txt` (the synthesiser). They hold Textalker's own machine code,
  which is Street Electronics' and proprietary, and this fork leaves them out by
  its owner's decision of 2026-11-05. References to them elsewhere in these notes
  and in code comments are left as Jayson Smith wrote them. The listings are on
  his branch `Textalker-RW`, and `tools/disasm.py` makes them from your own image.
- `textalker_rewrite_table_discovery.md` -- **the full record of the C
  rewrite.** Textalker is table-driven, so replacing the 6502 means
  decoding its tables and porting a small interpreter, not writing a TTS
  engine. Covers the dispatch table, every metacharacter (all now read
  from the 6502), the syllabifier, the stress pass, the three
  differential checkpoints, and the mispronunciations that must be
  reproduced rather than fixed. Starts with a STATE block saying where
  to pick up, followed by "What was wrong with the model", which is the
  correction that finished stage 1. **Large parts of the rest are
  superseded** -- every reading that treats a syllable boundary as a
  zero-width assertion, and every reading of `$9C` as the coronal
  stops. Those sections say so at their head, and are kept because the
  measurements in them are sound even where the conclusions were not.

- `profile_discovery.md` -- **the C engine finds its tables rather than
  listing addresses.** What each table's shape is, why the punctuation
  and dispatch tables identify each other, why `rule_hi` is the end of
  the image, why a page of zeros looked exactly like a word list, and
  what is still per-family (the workspace) with the measurement that
  says it holds. Ends with the one branch displacement that separates two
  otherwise-identical 3.1.2 builds -- and is what made a comma in Ctrl-V
  phoneme mode swallow the rest of the line on one of them.
- `disk_image_extraction.md` -- **how Textalker gets off an Apple II
  floppy image**, in C, with no Python and no CiderPress: the DOS 3.3
  reader, why detecting the sector ordering from the VTOC alone does not
  work (sectors 0 and 15 are fixed points of the interleave, so the VTOC
  and the first catalogue sector look identical in both orders), and why
  the loader/engine pair is found structurally rather than by filename.
  Also what the round-trip test proves and what only a real disk can.

## Current design -- read these to understand what exists

- `streaming_and_indexing.md` -- pull-driven synthesis and index events.
  Why the session-6 worker-thread design was dropped (synthesis runs at
  90-155x real time) and how the pieces fit.
- `say_all_index_breaks.md` -- why index marks no longer end an utterance,
  and what NVDA actually sends in Say All. **Supersedes the index-event
  behaviour described in `streaming_and_indexing.md`.**
- `continuous_speed_accumulator.md` -- continuous, pitch-preserving speech
  rate, by unlocking the parameter state machine from the audio path.
  Why 1.0 is byte-exact by construction, and pitch measured holding at
  129 Hz across a 6x range.
- `settings_mirroring.md` -- watching Ctrl-E commands go past so a voice
  can be read back and carried to a fresh instance. Records that `nP`
  and `nF` are one setting with two spellings.
- `ctrl_d_driver_commands.md` -- driver settings embedded in the text
  stream, and why a command ends the current utterance.
- `dll_packaging.md` -- export surface, calling convention, runtime
  dependencies, ABI version, why there are two DLL test programs, and
  the Windows/Linux bit-identical result.
- `multi_version_support_design.md` -- how one binary handles both
  Textalker versions, detected structurally rather than by hash.
- `msvc_build.md` -- the library under MSVC with no source changes, and
  four settings shown bit-identical to the MinGW add-on DLL. How the GUI
  in `gui-native/` is built.
- `pitch_above_63.md` -- what Textalker does with pitches 64-99, measured
  with the GUI's pitch sweep: two extra steps with intonation, none in
  monotone.
- `mingw_build_system.md` -- build targets, runtimes, and the
  wrong-architecture trap the Makefile guards against.
- `tms5220_port_and_audio_pipeline.md` -- how the TMS5220 port was made.

## Bugs worth reading for the lesson

- `step_budget_truncation.md` -- the 6502 step budget was four times too
  small and truncated speech **in silence**. Solved by a negative result
  Jayson volunteered. The reason any limit that can truncate must report
  it.
- `nvda_cancel_race.md` -- three cancellation defects, and why a flag was
  the wrong primitive for cancellation.
- `clock_output_rate_downsampling.md` -- a raised chip clock was being
  downsampled back to the output rate through a resampler with no
  anti-aliasing filter. Why the output rate has to be a floor, why the
  fix belongs in the hosts rather than the library, and why every
  existing check was blind to it.
- `pacing_fast_start_hack.md` -- **the pacing bug, solved.** A `#define`
  lost in the port from MAME. Explains why the audible symptom looked
  compression-specific when the defect was not.
- `onset_glitch_fixed_by_real_loader.md` -- **the onset glitch, solved**
  by booting through Textalker's real loader, which forced the
  language-card model everything now depends on.
- `single_char_return_bug_fixed.md` -- a lone character also spoke
  "return". Records that Textalker buffers a whole line and processes
  embedded Ctrl-E commands in sequence when the CR arrives, which is
  load-bearing for several later decisions.
- `tms5220_port_dangling_statements.md` -- five stripped log calls leaving
  dangling control flow. **Read before touching the port.**
- `chunker_wired_in_buffer_bounds_uninitialised.md` -- the chunker existed
  but was never called, and Textalker's own buffer bound is never
  initialised here.
- `input_handling_repeat_filter_and_encoding.md` -- repeat filter, LF
  handling, encoding conversion.
- `single_letter_word_bug_fixed.md` -- an infinite BRK loop from a missing
  ROM stub; also where the wild-jump trap came from.

## Superseded -- kept for the reasoning only, DO NOT follow

Everything below describes a state of the project that no longer exists,
or a conclusion later shown to be wrong. Read for the reasoning, never
for instructions.

- `buffer_chunking_and_indexing.md` -- the original look-ahead synthesis
  and index-event plan. **Both were implemented differently**, and the
  worker thread it proposes is unnecessary. Superseded by
  `streaming_and_indexing.md` and `say_all_index_breaks.md`. Its chunking
  rationale is still accurate.
- `library_plan_rate_and_pitch.md` -- the rate and pitch design. **Its
  later section claiming frame-rate control does not work is WRONG, and
  sits after its own correction**, which makes it a trap read top to
  bottom; the "MEASURED: it works" section is the accurate one. The
  accumulator it proposes was built -- see
  `continuous_speed_accumulator.md`.
- `onset_glitch_investigation_reverted.md` -- the session-9 dead end. Its
  lattice-filter analysis is sound but was not the cause.
- `pacing_investigation_restarts.md`,
  `pacing_mame_bytestream_comparison.md`,
  `pacing_isolated_to_restart_idle.md`, `pacing_device_sample_counter.md`,
  `pacing_arrival_phase_prediction.md` -- the pacing hunt in sequence,
  including a falsified prediction and a correction that was itself
  wrong. Useful mainly as a record of which measurements mislead.
- `true_timing_implemented_not_the_cause.md` -- **its reservation no
  longer applies.** True timing works, produces byte-identical output and
  clobbers nothing; it stays opt-in only because there is no reason to
  switch.
- `fake6502_bcd_decimal_mode.md` -- BCD enabled; measured to change
  nothing.
- `session2_findings.md` -- early reverse engineering. Still accurate that
  `$D009` blocks on a keypress, but the single-character problem it was
  investigating was solved a completely different way.
- `v13_compatibility.md`, `v13_working.md`,
  `v13_vs_v3_cricket_investigation.md`, `disasm_fd53.txt`,
  `dispatch_table_d817.txt` -- early v1.3 work, absorbed into
  `multi_version_support_design.md` and HANDOFF.
