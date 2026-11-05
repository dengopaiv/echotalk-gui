# Getting Textalker off an Apple II disk, in C

Until session 17 the first step of using EchoTalk was: install
CiderPress, open a `.dsk`, find two files, extract them, rename them.
That is a Windows GUI, which is the wrong shape for the person this
project is for, and the wrong shape for an installer. It was the last
thing standing between the project and somebody else running it.

`src/dsk.c` reads DOS 3.3 disk images. `src/tk_disk.c` finds Textalker
on one. `tools/tk_extract.c` is the standalone program, and
`echotalk_create_from_disk()` skips the extraction step altogether.

**Everything is C.** Jayson's constraint, and it is the right one: a
pre-runtime step written in Python would need Python on the user's
machine, and there is no reason for anything here to need it. The
Python in `tools/` stays what it has always been — the development
oracles, not part of using the thing.

## What is supported

| | |
|---|---|
| `.do`, `.dsk` | 143,360-byte DOS-order images |
| `.po` | the same in ProDOS sector order |
| `.2mg` | a 64-byte header round either |
| `.nib`, `.woz` | **not** supported, and said so by name |

Nibble images need a bit-level decoder. Textalker disks are not
copy-protected, so nothing requires one; the error names the format and
says to convert rather than failing vaguely.

## The trap: sectors 0 and 15 are fixed points

The obvious way to detect sector ordering is to look for a valid VTOC at
track 17 sector 0 and, failing that, de-interleave and look again.

**That does not work, and it fails silently.** The DOS↔ProDOS map is

```
DOS logical:  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
ProDOS pos :  0 14 13 12 11 10  9  8  7  6  5  4  3  2  1 15
```

which is its own inverse, and **fixes 0 and 15**. So track 17 sector 0
— the VTOC — is at the same offset in both orderings, and so is the
first catalogue sector, which DOS puts at sector 15. A reader that
stops there calls every `.po` file DOS-ordered, then reads file data as
a catalogue and fails somewhere much later and much less clearly.

This was caught by the round-trip test, which built a ProDOS-ordered
image and got "DOS order" back with seven of eight files.

What works is to **score** both orderings rather than accept the first
plausible one:

- follow the catalogue **chain**, which leaves the fixed points at the
  first hop (15 → 14, and 14 sits at position 1 under ProDOS order);
- check each entry's **track/sector list** actually looks like one.

The second matters on a nearly empty disk, where the chain is short. The
higher score wins; a `.2mg` header's claim decides a tie, because a
wrong header is more common than a wrong disk but is still worth
believing when nothing contradicts it.

## Verified on real disks — and what they broke

Three disks of Jayson's, and **each found something the synthetic tests
could not**. That is the whole argument for testing against something
you did not write.

**`both.dsk`** — v3.1.3 as `TEXTALKER.RAM` + `TEXTALKER.OBJ` and v1.3 as
`TT.RAM` + `TEXTALKER.RAM.OBJ`, beside INTEGER, OREGON TRAIL and
ATOM 20. Its v3.1.3 extracts byte-identically to `roms/`. Its v1.3 is a
*different build* — a 602-byte loader against 611, engines differing
from byte 11 — and is still identified as 1.3 and still speaks.

> **Defect**: `tk_disk_find()` took the first pair and stopped, so one
> of the two Textalkers on that disk vanished silently. `tk_disk_list()`
> now returns all of them.

**`PIZZA.DSK`** — 38 files of Echo software, carrying **Textalker
3.1.2** as `TEXTALKER` + `TT.OBJ`.

**`bex21my.dsk`** — 41 files, carrying a *second* Textalker 3.1.2 (four
engine bytes different from PIZZA's) as `TEXTALKER.RAM` +
`TEXTALKER.OBJ`, and a v1.3 as **`TEXTALKER.PLUS` + `TEXTALKER.RAM.OBJ`**
— a loader named nothing like its engine, which is the clearest case yet
for pairing structurally. Both pairs were found and both speak.

> **Defect**: its VTOC records **496 bytes per sector** at `$36/$37`
> instead of 256. Nothing reads that field, so nothing ever noticed —
> but `dsk.c` rejected the whole disk over it. The VTOC's geometry
> fields are now evidence that *scores* rather than a gate; the
> catalogue decides.

The lesson is one this project keeps relearning in new places: **a
field nothing reads will contain anything**. Validate on the structures
that are actually used.

## Finding Textalker: structurally, never by name

The two versions do not even agree on the filenames — v3.1.3 ships
`Textalker.ram` + `Textalker.obj`, v1.3 ships `Textalker.ram` +
`Textalker.ram.obj` — and a 3.1.x build nobody has catalogued would not
be in a list anyway. This project's habit is to recognise things by what
they are, and that is what happens here:

- **A loader is a file carrying the entry trampoline**, the six bytes
  `48 AD 8B C0 4C xx yy`: PHA, LDA `$C08B` (select the language card),
  JMP into Textalker's jump table. The JMP target's high byte is the
  page the engine loads at — so **a loader announces its partner**.
- **An engine is a file the loader's trampolines fit.** The targets are
  not just a page — they are the engine's jump table, and every one
  lands within the first few bytes of it, on a `4C` (a JMP) or on the
  `AD 8B C0` that enters the engine proper:

  ```
  v3.1.3          $D003 $D006 $D009  ->  4C 4C AD
  v1.3            $D400 $D403        ->  4C AD
  3.1.2 on PIZZA  $D003 $D006 $D009  ->  4C 4C AD
  ```

  At least two distinct targets must fit. `both.dsk`'s 5.6K `INTEGER`
  is rejected on the first one.

`echotalk.c`'s `boot()` uses the same `tk_disk_obj_page()`, so there is
one copy of the signature rather than two that can drift.

### A profile match is a label, not a gate

The first version of this required `tk_image_load_mem()` to recognise
the engine. **PIZZA showed that to be wrong.** `src/tk_image.c` was then
keyed to one build's table addresses; 3.1.2's engine is 326 bytes
shorter with everything moved, so the C rewrite rejected it — while the
**emulated** engine, which just runs the 6502, spoke it perfectly.
Requiring a profile would have refused a disk that works.

`tk_image.c` now finds its tables rather than listing them
(`notes/profile_discovery.md`) and reads all four known builds, so
nothing to hand exercises this any more. It stays because the next
unrecognised build will.

So the profile is consulted only to NAME the version, and `native_ok`
says whether the rewrite is available too. `say --native` on PIZZA
prints "the native engine does not recognise this image" and stops,
rather than producing anything wrong.

### Making the C engine version-independent -- done

`notes/profile_discovery.md`. `src/tk_image.c` now searches each image
for the shape of every table instead of holding a list of addresses, so
3.1.2 runs on both engines and `tk_profile --disk <disk>` reports what
was found. `native_ok` still exists, because a build neither engine's
tables fit is still possible -- it just is not 3.1.2 any more.

## The binary header

A DOS type-B file starts with a load address and a length, both
little-endian, and neither is part of the file. **The length is the
authority, not the sector count** — Textalker's engine is 12,282 bytes
inside 49 sectors — and getting that wrong yields a file that is
byte-identical for 12,282 bytes and then has 262 bytes of slack, which
loads and runs and is nonetheless not what CiderPress produces.

The check that catches it: `tk_extract` on a disk built round the real
files must produce output **byte-identical to `roms/*.bin`**. It does.

## What the test proves, and what it does not

`tools/test_dsk.c` builds DOS 3.3 images from the format description in
`dsk.c` and reads them back. It catches every coding error — interleave,
the catalogue chain, multi-sector track/sector lists, the binary header,
name padding, the lock bit. Given a loader and an engine it also builds
a disk round them and checks `tk_disk_find()` picks the pair out and
that what comes back is byte-identical.

**It cannot catch a misreading of the format itself**, because the
builder and the reader share one. That reservation is why the two real
disks above were asked for, and why both of them found something. On a
disk nobody here has seen,

```
tk_extract <a real disk> --list
```

is still the first thing to run.

## Using it

```
tk_extract disk.dsk --list             what is on it
tk_extract disk.dsk --out roms/        write EVERY pair on it
tk_extract disk.dsk --file HELLO       one named file, verbatim
say --disk disk.dsk "Hello." out.wav   no extraction step at all
say --disk d.dsk --disk-version 1.3    when it carries more than one
```

The NVDA add-on discovers a disk image beside the driver as a voice,
the same as it discovers a `.ram.bin`/`.obj.bin` pair, so a user can
drop the floppy image in and be done.

## A bug this turned up

The add-on's `REQUIRED_ABI` was **6** while the library was on 8. It had
been refusing to load against a current DLL for two sessions and nothing
noticed, because the DLL's own tests check the ABI number and
`tools/test_nvda_driver.py` was not being run. It is 9 now, and the
constant carries a note saying to bump it with the library.

**The lesson is the familiar one in a new place**: a check that is never
executed is not a check. Run `test_nvda_driver.py` after any ABI change.
