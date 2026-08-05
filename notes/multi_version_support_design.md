# Supporting both Textalker versions from one binary (design)

Requirement: Textalker 3.1.3 and 1.3 selectable as NVDA voice variants,
without shipping four DLLs, and without identifying ROM images by hash --
users may hold 3.1.2 or 3.1.4 or other near-identical builds that should
just work.

## The DLL count

**Two, not four.** 32-bit versus 64-bit is a property of the *host
process* (older NVDA builds are 32-bit), not of the voice. Both Textalker
versions belong in the same binary, chosen at runtime.

## What actually differs between the versions

Measured from the two working harnesses:

| | v3.1.3 | v1.3 |
|---|---|---|
| loader load address | `$9300` | `$9300` |
| OBJ load address | `$D000` | `$D400` |
| per-character entry | `$BA7C` trampoline, char in A | `$D400`, char pushed |
| language-card banking | modelled | not modelled |
| ROM stubs | shadow + `$FBB3`; `$9EBD`, `$C300` | `$FC58`, `$FBFD`, `$9EBD` |
| `$AA59` seed | `0xFD - 2` | `0xFD` |
| extra init | `$FCD6` if detection failed | none |

Most of this collapses once the two structural facts below are used.

## Structural detection, no hashing

Both loaders install a trampoline of the identical shape, and searching
the loader *files* for the byte pattern `48 AD 8B C0 4C lo hi` --
`PHA; LDA $C08B; JMP $xxxx` -- finds every public entry:

```
v3.1.3 loader:  JMP $D003, JMP $D006, JMP $D009
v1.3   loader:  JMP $D400, JMP $D403
```

Two things fall out:

**1. The OBJ load address is the page of those targets.** `$D0xx` means
load at `$D000`; `$D4xx` means `$D400`. Determined before executing
anything, which is what we need in order to place the image. Any 3.1.x
build shares 3.1.3's layout and lands on the same answer without being
recognised individually.

**2. The calling convention is the same for both, if we call the
trampoline rather than the OBJ.** Our v3.1.3 harness already calls
`$BA7C` with the character in A and lets the trampoline do the `PHA`.
v1.3's trampoline (installed at `$BA82`) begins with `PHA` as well, so
calling it the same way works identically. The "char pushed versus char
in A" difference is an artifact of how the two harnesses grew, not a
real difference between the versions. **Verified**: v1.3 now runs this
way, with identical output.

### Correction: scanning finds the trampolines but cannot choose between them

The first draft of this design proposed scanning low memory for the
installed `48 AD 8B C0 4C lo hi` and using the match as the character
entry. **That is wrong, and testing caught it.** The loader installs one
trampoline per public entry:

```
v3.1.3:  $BA72 -> JMP $D003 (init)
         $BA7C -> JMP $D006 (character)   <- the one wanted
         $BCF0 -> JMP $D009 (keyboard echo)
v1.3:    $BA82 -> JMP $D400 (character)
```

Taking the first match gives v1.3 the right answer and v3.1.3 the
*init* entry, which produces silence. Position does not generalise
either: the character entry is v3.1.3's second trampoline and v1.3's
first.

The scan is still worth keeping. It reliably yields the OBJ load address
(the page of the targets), it proves the loader ran, and it enumerates
the candidates. It just cannot pick among them by pattern alone.

Two ways to finish the job, neither yet implemented:

- **Probe the candidates.** Boot once, then feed each candidate a short
  test string and keep the one that produces Echo-card writes. This is
  exactly how `$D006` was originally identified in session 3, it needs
  no version knowledge at all, and boot is milliseconds. A wrong
  candidate is caught by the existing step budget and wild-jump trap.
- **Follow the DOS hook chain.** Both versions patch `$A22B` to
  `JMP $BA69` -- the same fixed address in both, per the session 2 and
  v1.3 compatibility notes. Whatever that chain reaches is by definition
  the character-output hook. Cheaper than probing if it holds up, and
  worth checking first.

## Language-card banking should be unified

Note that **v1.3's trampoline also reads `$C08B`** -- it bank-switches
exactly like v3.1.3. The v1.3 harness gets away without modelling the
language card only because it never exercises the ROM-side exit path,
the same way the old direct-init v3.1.3 harness did before session 10.

Modelling banking for both is therefore more correct, not less, and it
absorbs most of the per-version stub list: `$FC58`, `$FBFD` and `$FBB3`
all sit inside the `$D000-$FFFF` window and resolve against the all-RTS
ROM shadow automatically. Only `$9EBD` and `$C300`, which are below
`$D000`, still need explicit stubs, and those can be set for both
versions unconditionally.

**This needs testing before it is relied on** -- it is the "v1.3 has the
same latent language-card problem" follow-up flagged earlier. If v1.3
renders identically with banking modelled, the two code paths merge.

## Proposed shape

One implementation, one small profile resolved at load time:

```c
typedef struct {
    uint16_t loader_addr;    /* $9300 for both so far */
    uint16_t obj_addr;       /* from the trampoline target page */
    uint16_t entry;          /* installed trampoline, found after boot */
    uint8_t  aa59_seed;      /* stack-pointer seed for the loader */
    char     version[16];    /* parsed banner, for display only */
} textalker_profile;
```

Everything in it is either detected or shared. Nothing is keyed to a
specific build.

The version banner is worth parsing for the *label* NVDA shows, but not
for behaviour. Note the two store it differently: 3.1.3 holds
`VERSION 3.1.3` as plain text while 1.3 holds it reversed
(`)DRAC MAR( 3.1 NOISREV`). Try forward, then reversed, and fall back to
"unknown version" rather than refusing to run -- a build we cannot name
should still work if it boots.

## Failure handling

A profile is validated by booting it: the loader must run to completion,
a trampoline must appear in low memory, and card detection must set
`$FD87` to `$1F`. The wild-jump trap already catches a bad configuration
immediately rather than hanging. So an unknown image can simply be
tried, and rejected cleanly if it does not boot.

## What the user supplies

The ROM images stay the user's own, per THIRD_PARTY_LICENSES.md. For
NVDA the natural arrangement is a folder per voice variant, each holding
a loader and an OBJ; the add-on enumerates them, boots each once to
identify it, and offers whatever it found as variants. Adding a 3.1.2
image would then require no code change at all.
