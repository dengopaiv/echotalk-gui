# Upstream's `Textalker-RW` branch: what it holds

Written 2026-11-05, to decide what to do about the branch. This note is a summary of Jayson
Smith's work, and the branch's own `HANDOFF.md` and `notes/README.md` are the authority on it.
Below "What was taken", which was added once the decision was made, the claims about what the
engine does are the branch's account.

## What was taken

**The owner's decision, 2026-11-05: take the code, and leave out the disassembly listings.** The
branch came in at `9041634` as **one squashed commit**, not a merge. A merge would bring its 83
commits, and the listings are in their trees, so pushing it would publish them even with the files
deleted afterwards. The cost is that Jayson Smith's commit-by-commit history is not in this fork.
It is on his branch, and the commit here credits him and names `9041634`.

Left out, from this fork:

- `notes/disasm_rule_engine.txt`, `notes/disasm_v13.txt`, `notes/disasm_stage2.txt`.
- The syllabifier's four word lists, from `notes/textalker3_rule_changes.md` and
  `notes/textalker_rewrite_table_discovery.md`. They are Street Electronics' data as well, and were
  named alongside the listings when the owner was asked.
- The machine-code byte columns of the one short excerpt in `notes/profile_discovery.md`, leaving
  its addresses and mnemonics.

`tools/disasm.py` and `tools/syllabify.py` stay, and make all of it from your own image.

**Still in the notes, on purpose, for the owner to know:** individual Textalker rules quoted in the
course of explaining them, chiefly in `textalker_rewrite_table_discovery.md` and
`textalker3_rule_changes.md`, and `phoneme_alphabet.md`'s measured correspondence. **Already on this
fork's `main` before this work, from upstream's `main`, and not touched:** `notes/disasm_fd53.txt`,
and a short excerpt in `notes/onset_glitch_fixed_by_real_loader.md`.

Changed to make it build here: `.gitignore` and `tools/say.c`, the two conflicts, each resolved by
keeping both sides; and `gui-native/build.cmd`, which compiles the nine new source files.

Checked, on 2026-11-05, x64:

| Check | Result |
|---|---|
| `tools/verify_gui.py` against the **old add-on's `echotalk64.dll`**, before this work (`main`) | 114 of 114 |
| The same, after | **102 of 114.** The 12 are reading mode and punctuation |
| `tools/verify_gui.py` against a **separate DLL built with MinGW GCC from pristine `9041634`** | 114 of 114 |
| `tools/verify_gui_keyboard.py`, `tools/verify_gui_smoke.py` | Passed |
| `tools/test_nvda_driver.py` on that DLL | Passed: stand-in modules, not NVDA |

**The 12 are upstream's fix showing, not a fault.** The old library stored those two settings and
never sent them to Textalker: upstream's `0a40367` says so ("MODES_CHOSEN HAD NO READER"), and the old
DLL gives the same sample count for every one of them. So **the shipped add-on's DLL can no longer
serve as `verify_gui.py`'s reference;** use a DLL built from this tree, as `gui-native/README.md` now
says.

**Not done:** nothing from this has been heard in NVDA. The GUI does not yet offer what the branch
adds (the native engine, dictionaries, disk images), and was not asked to. The branch's `Makefile`
and `nvda-addon/build_addon.sh` still have `win32` targets, as `main` already did; nothing 32-bit
was built.

## The study, as first written

## How it was looked at

A fetch-only remote was added on 2026-11-05: `upstream` =
`https://github.com/jaybird110127/echotalk.git`, with its push URL set to `DISABLED`. The branch
is at `upstream/Textalker-RW`, `9041634` (2026-09-12). Everything below comes from
`git diff main...upstream/Textalker-RW`, `git show`, and one dry-run merge:

    git merge-tree --write-tree --name-only main upstream/Textalker-RW

That command changes no branch and no working file.

## What is on it

83 commits ahead of upstream's `main` (`25ba18f`), none behind. 59 files, about 20,900 lines added
and 800 removed. The add-on's manifest still says 0.1.0, so the version alone does not reveal
the change.

By the branch's own account:

- **A second engine.** A native C rewrite of Textalker's whole speech path (`src/tk_*.c`), selected
  with `echotalk_set_engine()`, with no 6502. It is said to be byte-exact against the emulation at
  the chip bytes for four Textalker builds (3.1.3, 1.3 and two 3.1.2s). Its audio is *not*
  phase-identical to the emulation, and `HANDOFF.md`'s branch section says how far that got and why
  it stopped. The emulation stays the default and the reference.
- **The tables are found, not listed.** The C engine locates its tables in the image by their shape
  (`notes/profile_discovery.md`), so it can read builds nobody has catalogued.
- **Pronunciation dictionaries** in CMUdict format, read at run time, as ARPABET, as respellings
  (`BOOT =booht`), or in Street Electronics' Ctrl-V notation. EchoTalk ships none, and
  `THIRD_PARTY_LICENSES.md` explains why it keeps it that way.
- **Longer utterances.** The 80-character chunk limit belonged to the emulation. The native engine
  takes 130 or 200.
- **Apple II disk images read directly** (`src/dsk.c`, `src/tk_disk.c`, `tools/tk_extract.c`), with
  Textalker found on a disk by structure, not by filename.
- **ABI 12 with 56 exports,** against `main`'s ABI 6 with 44. The NVDA driver grows by about 270
  lines.

Of interest for study:

- `notes/textalker3_rule_changes.md`: what Street Electronics changed between Textalker 1.3 and 3,
  and why some words got worse. It records a design trade: enumerated whole-word rules replaced by
  general rules that ask a syllabifier.
- `notes/textalker_rewrite_table_discovery.md`: the full record of the rewrite. Parts of it are marked
  as superseded by the branch itself.
- `notes/phoneme_alphabet.md`: Textalker's 58 allophones, measured.

## What matters before taking anything from it

**This fork is public, and the branch carries Street Electronics' material in its notes.**

- **No table is compiled in.** The branch's README states that rule and says it is why the library is
  BSD-3-Clause and the add-on can be given away. A search for dense byte arrays across the main source
  and tool files found only a 7-byte `STOPS` constant (`src/tk_rules.c:59`, `tools/rule_walk.py:106`)
  and a bit-mask line. No `.bin` of Textalker's is tracked: the `.bin` files on the branch are input
  texts (`reference_text/`, `Rubber Baby Buggy Bumper Test/`), and `roms/*.bin` stays ignored. That
  search was by pattern, not a reading of every file.
- **The notes are another matter.** `notes/disasm_stage2.txt`, `disasm_v13.txt` and
  `disasm_rule_engine.txt` are annotated disassembly listings with the machine-code bytes in them, about
  3,600 listing lines (counted by matching address columns). `notes/textalker3_rule_changes.md` gives
  the syllabifier's word lists "in full": 70 entries, "read straight out of the image", in its own words.
  Textalker is proprietary (Street Electronics; v3.1.3 also carries a 1986 American Printing House
  for the Blind copyright).
- **So a plain merge and push would republish those listings from this fork.** They are already public
  on Jayson Smith's branch, and whether he may publish them is not for this note to say. Whether *we*
  carry them forward is a question for the owner, under the head directory's rule on checking what is in
  a push. Nothing here has been decided.
- **32-bit.** The branch's `Makefile` and `nvda-addon/build_addon.sh` still build and package 32-bit
  (`win32`, `echotalk32.dll`), like upstream's `main`. Nothing built from it here may be 32-bit.

## What a merge would take

The dry-run reports **two conflicts: `.gitignore` and `tools/say.c`**. `HANDOFF.md`, `README.md` and
`notes/README.md` merge automatically, and `gui-native/` is ours alone and is not touched by the branch.

That is the textual result only. The native GUI calls the library's API, and ABI 6 to 12 was not checked
against it. Nothing was built or run. Golden results and the check against the add-on's shipped DLL would
need rerunning.
