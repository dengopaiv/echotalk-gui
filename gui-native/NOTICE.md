# NOTICE — EchoTalk GUI

Every borrowed piece inside `echotalk_gui-<arch>.exe` (and `say-<arch>.exe`,
built beside it), who made it, and under what terms. Anyone given the binary
must receive this file, `LICENSE` and `THIRD_PARTY_LICENSES.md` with it.

| Piece | Author | Terms | Full text |
|---|---|---|---|
| The EchoTalk library (`src/`), the `say` tool, and the NVDA add-on driver whose settings and speaking path the GUI reproduces (`nvda-addon/synthDrivers/echotalk/__init__.py`) | Jayson Smith, 2026 | BSD-3-Clause | `LICENSE` |
| Fake6502, the 6502 CPU core (`third_party/fake6502/`) | Mike Chambers, 2011 | Public domain; credit requested and given | `THIRD_PARTY_LICENSES.md` |
| The TMS5220 speech chip emulation, ported to C (`third_party/tms5220_core/`) from MAME's `tms5220.cpp` | Frank Palazzolo, Aaron Giles, Jonathan Gevaryahu, Raphael Nabet, Couriersud, Michael Zapf (MAME project) | BSD-3-Clause | `THIRD_PARTY_LICENSES.md` |
| The window (`gui-native/echotalk_gui.cpp`), its build script and the `tools/verify_gui*.py` checks | written for this repository, following the Votrax SC-01 ROM GUI, Votrax Native, SAM and STSPEECH GUIs; the keyboard check is adapted from Votraxxion's `verify_gui_keyboard.py` | BSD-3-Clause, as `LICENSE` | `LICENSE` |

**Not included and never to be included:** the Textalker images
(`*.ram.bin`, `*.obj.bin`). They are Street Electronics' original commercial
software — 3.1.3 also carries an American Printing House for the Blind
copyright — nobody here licenses them, and each user supplies their own. See
"Obtaining the Textalker images" in the top-level `README.md`.
