# notes/ index

Investigation writeups, roughly chronological. `HANDOFF.md` in the
project root summarises the conclusions; these keep the reasoning,
including approaches that turned out to be wrong and why.

Sample counts quoted in the older files predate the session-10 pacing
fix and dead-air trimming. **Treat HANDOFF.md's baseline table as
authoritative**, not the numbers here.

## Resolved bugs (the useful ones to read)

- `pacing_fast_start_hack.md` — **the pacing bug, solved.** A `#define`
  lost in the port. Start here; it also explains why the audible symptom
  looked compression-specific when the defect was not.
- `onset_glitch_fixed_by_real_loader.md` — **the onset glitch, solved**
  by booting through Textalker's real loader, which forced the
  language-card model everything else now depends on.
- `chunker_wired_in_buffer_bounds_uninitialised.md` — long text split
  mid-word; the chunker existed but was never called, and Textalker's
  own buffer bound is never initialised here.
- `input_handling_repeat_filter_and_encoding.md` — repeat filter, LF
  handling, and encoding conversion.
- `single_letter_word_bug_fixed.md` — an infinite BRK loop from a
  missing ROM stub; also where the wild-jump trap came from.
- `single_char_return_bug_fixed.md` — a lone character also spoke
  "return". Fixed by moving the mode restore before the CR rather than
  after it. Records the ordering property that makes that work:
  Textalker buffers a whole line and processes embedded Ctrl-E commands
  in sequence when the CR arrives.
- `tms5220_port_dangling_statements.md` — five instances of stripped log
  calls leaving dangling control flow. Read before touching the port.

## Design and architecture

- `ctrl_d_driver_commands.md` — driver settings embedded in the text
  stream, in the shape of Textalker's own Ctrl-E commands. Explains why
  a command has to end the current utterance, and records a latent
  chunker-truncation bug it uncovered.

- `library_plan_rate_and_pitch.md` — the rate, pitch and resampling
  design behind the library's controls, and a worked example of a
  confident negative result that was wrong: frame-rate control was
  written off on a measurement that had silently not taken effect.
  **Read the "MEASURED: it works" section before the later one that says
  it does not** — the superseded conclusion sits after its own
  correction, which is a trap top-to-bottom. Superseded again by
  `continuous_speed_accumulator.md`, which built the accumulator this
  note proposes.
- `multi_version_support_design.md` — how one binary handles both
  Textalker versions, detected structurally rather than by hash.
- `buffer_chunking_and_indexing.md` — chunking rationale, plus the
  original look-ahead synthesis / index-event plan. **Superseded** by
  `streaming_and_indexing.md`, which implemented both differently:
  measurement showed the worker thread it proposes is unnecessary.
- `tms5220_port_and_audio_pipeline.md` — how the TMS5220 port was made.
- `mingw_build_system.md` — build targets, runtimes, and the
  wrong-architecture trap the Makefile now guards against.
- `dll_packaging.md` — the export surface, calling convention, runtime
  dependencies and ABI version, plus why there are two DLL test
  programs rather than one.
- `streaming_and_indexing.md` — pull-driven synthesis and exact index
  events. Why the session-6 worker-thread design was dropped (synthesis
  runs at ~136x real time), and why index marks being Ctrl-D commands
  makes their offsets free.
- `continuous_speed_accumulator.md` — continuous, pitch-preserving
  speech rate, by unlocking the parameter state machine from the audio
  path. Explains why 1.0 is byte-exact by construction, and measures
  that pitch really does stay put.
- `nvda_cancel_race.md` — three cancellation defects found by testing the
  add-on in real NVDA, and why a flag was the wrong primitive for
  cancellation in the first place. Also a worked example of checking that
  a regression test fails on the broken code.
- `settings_mirroring.md` — watching Ctrl-E commands go past and
  updating the library's own variables, so a voice can be read back and
  carried to a fresh instance. Records that `nP` and `nF` are one
  setting with two spellings, that command letters are case-insensitive,
  and a wrong model I was talked out of before it cost anything.

## Superseded, kept for the reasoning only

- `onset_glitch_investigation_reverted.md` — the session-9 dead end.
  Its lattice-filter analysis is sound but was not the cause; carries a
  pointer to the real fix.
- `pacing_investigation_restarts.md`,
  `pacing_mame_bytestream_comparison.md`,
  `pacing_isolated_to_restart_idle.md`,
  `pacing_device_sample_counter.md`,
  `pacing_arrival_phase_prediction.md` — the pacing hunt in sequence,
  including a falsified prediction and a correction that was itself
  wrong. Useful mainly as a record of which measurements mislead.
- `true_timing_implemented_not_the_cause.md` — true timing implemented
  faithfully; explains why it is opt-in.
- `fake6502_bcd_decimal_mode.md` — BCD enabled; measured to change
  nothing.
- `v13_*.md`, `session2_findings.md`, `disasm_fd53.txt`,
  `dispatch_table_d817.txt` — early reverse-engineering. Largely
  absorbed into HANDOFF.md.
