# notes/ index

Investigation writeups. `HANDOFF.md` in the project root summarises the
conclusions; these keep the reasoning, including approaches that turned
out to be wrong and why.

> **Sample counts in the older files are obsolete.** Anything written
> before session 10 predates the pacing fix and dead-air trimming;
> anything before session 11 predates the step-budget fix.
> **HANDOFF.md's baseline table is the only authority.**

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
