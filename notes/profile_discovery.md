# Finding Textalker's tables instead of listing them

`src/tk_image.c` used to hold two profiles: 3.1.3's addresses and 1.3's,
about twenty-five per version, read off a disassembly. Then Jayson's
PIZZA.DSK turned up carrying **Textalker 3.1.2** — the same architecture,
326 bytes shorter, every table moved — and the C rewrite rejected it
outright while the 6502 emulation ran it perfectly.

That is the wrong way round for a project whose whole habit is to
recognise things by what they are. So the profile is now **searched
for**, in the user's own image, at load time.

`tools/tk_profile` prints what the search decided, and is the first
thing to run against a build nobody here has seen.

## What is found, and by what shape

| | shape | unique? |
|---|---|---|
| `attrs` | 128 bytes where exactly A E I O U Y carry `$80` among the letters, and every other letter carries `$40` and not `$80` | **1 of 1** in all three images |
| `allo_ptr` | 58 consecutive 16-bit pointers, ascending, each within `$40` of the last, all inside the image | **1 of 1** in all three |
| `charmap`, `pitch_off`, `vowel_var` | fixed offsets `$4F`, `$14` and `$0A` below `allo_ptr` — then *verified*, not assumed | — |
| `punct_names` | 28 `{pointer, pointer, character}` triples whose first 25 characters ascend | — |
| `dispatch` | exactly `$54` past `punct_names`, confirmed by counting live entries | — |
| `rule_lo` | the lowest pointer in the dispatch table | — |
| the four syllabifier lists | consecutive null-terminated runs of letters with the high bit on each word's last character | — |

Measured against the two images whose addresses were already known, the
search reproduces **every one of them exactly**:

```
3.1.3  attrs $EF24  dispatch $EFDF  punct $EF8B  charmap $EA44
       pitch $EA7F  var $EA89  allo $EA93  lists $E127 $E141 $E192 $E1B1
1.3    attrs $DE13  dispatch $DECD  punct $DE79  charmap $EFAA
       pitch $EFE5  var $EFEF  allo $EFF9  no syllabifier
3.1.2  attrs $EDD5  dispatch $EE90  punct $EE3C  charmap $E8F5
       pitch $E930  var $E93A  allo $E944  lists $DFD8 $DFF2 $E043 $E062
```

### Three of these are worth the detail

**The punctuation table and the dispatch table find each other.**
`punct_names` is 28 three-byte records, so `$54` bytes, and the dispatch
table starts on the next byte: `$EF8B + $54 == $EFDF`, and
`$DE79 + $54 == $DECD`. Neither is easy to identify alone — but a
candidate punctuation table is *confirmed* by whether a dispatch table
with 70 live entries follows it, and the pair is then unambiguous. The
punctuation table also sits INSIDE the top of the attribute table in
every version, at offset `$67` in both 3.1.x builds: the attribute
entries above `e` are never read, so the two share the space.

**`rule_hi` is the end of the image, not the end of the rules.** The
dispatch pointers give every section's start but not the last one's end,
and `$E1FA` needs a bound. Walking the rules to find it does not work:
6502 code alternates high and low bytes in exactly the pattern-then-
payload shape a rule has, so the walk never stops. Using the image's end
is what `punct_section()` already does for its own sections, and it is
safe because it only ever limits the LAST section, whose final rule is a
catch-all — the walk stops long before it. The proof is that the C, with
`rule_hi` at `$FFF9`, is still byte-identical to the Python, which has
`$FCC4`.

**A page of zeros looks exactly like a word list** if all you require is
"letters, high-bit letters or nulls". The first version of the search
found `$FD78` in 3.1.3 — blank workspace — and then concluded that v1.3
*had* a syllabifier, which handed it 3.1.3's workspace and produced
garbage. The rule now is: no two nulls in a row, every null preceded by a
word-final high-bit letter, and at least `$80` real letters in the run.

## The workspace, and a claim that was wrong

The input and phoneme buffers and the dozen variables the Ctrl-E
commands set are **RAM addresses that only the code references**, so no
data pattern reaches them. The first version of this took them from a
per-family table — 3.1.3's for any 3.1.x — and said so, adding that
`tk_diff.py --version 3.1.2 --emulator` had measured the assumption.

**The assumption was wrong and the measurement did not catch it.**
3.1.2's phoneme buffer is at `$FD5D`, not `$FEAD`: the whole workspace
sits `$150` lower. Watching the emulator write `61 24 63` for "CAT"
shows it in one line. The text tests passed anyway because the port
keeps its own copy of the phoneme string and only ever *seeds* its
buffers from those addresses — so a wrong workspace shows up nowhere
until a rule reads past the end of the text into the leftovers, which
took the pathological command fuzz to reach (197 of 200, and the three
failures all needed the whole stream; every shrunk version passed).

The lesson is the project's own, in a new place: **a test that exercises
a thing only incidentally is not a measurement of it.**

### Finding it from the code

The workspace is not addressable by any data pattern, but two of its
addresses appear as instruction operands in a distinctive **pair**: the
rule engine reads the input buffer with `LDA abs,X` and stores phonemes
with `STA abs,X`, and the two buffers sit a fixed distance apart. So the
search is: the one `STA abs,X` operand `W` for which `W + d` is also an
`LDA abs,X` operand.

```
3.1.3   $FEAD / $FDAC     d = -$101
3.1.2   $FD5D / $FC5C     d = -$101
1.3     $F492 / $F59F     d = +$10D   (its input buffer is ABOVE)
```

**Unique in every image**, and it has to be — a whole Textalker has only
nine to fifteen `STA abs,X` operands. The distance `d` is a family
constant, and the family is already known by then from the syllabifier
lists.

Everything else in the workspace is a fixed offset from the phoneme
buffer, because **it is one rigid block**. That is measured rather than
assumed: sending `^E37P ^E11F ^EC ^E9D` and watching the writes gives

```
             3.1.3    3.1.2
flags        $FFAD    $FE5D      base pitch   $FFAF    $FE5F
delay        $FFBB    $FE6B      and every other address observed,
                                 $FFBE $FFBF $FFC7 $FFC9 $FFCB $FFD8,
                                 lands exactly $150 lower
```

A failed search falls back to the family's own address rather than
refusing the image, because the consequence is mild and local: the
buffers are only seeded from there.

## Two 3.1.2s, and the one byte between them

PIZZA.DSK and bex21my.dsk both carry a Textalker 3.1.2, the same length,
**differing in four engine bytes**:

```
$D8A2  STA $FE5E  ->  three NOPs     (a store to the command character)
$E47C  BCS +$09   ->  BCS +$16
```

Their loaders differ only in the filename they BLOAD. The second of those
two patches is the whole of the audible difference, and it is one branch
displacement at the site HANDOFF has flagged since session 13:

```
$E5C7: JSR $E6B0     the codes that are not allophones
$E5CA: BCS $E5E2     ...handled: skip to the RTS
$E5CC: JSR $E674     stop-consonant release
$E5CF: JSR $E713     pitch offset and vowel variant
$E5D2: JSR $E75D     read and emit the frames
$E5D5: LDA $FFAD     <- the re-entry begins here
$E5D8: BPL $E5E2
$E5DF: JMP $E5AA     re-enter phoneme mode
$E5E2: RTS
```

Three JSRs is nine bytes, so **a displacement of exactly 9 lands ON the
re-entry and anything larger skips it**. 3.1.3 and bex's 3.1.2 use `$16`
and skip; PIZZA's uses `$09` and carries on. That is why a lone `,` in a
Ctrl-V string swallows the rest of the line on one build and does not on
the other — and why PIZZA emitted 12 chip bytes for `^V,` where the
others emit 6.

**It is not a property of the release**, which is why guessing it from
the version number was never going to work: v1.3 merges the two routines
at `$EAD6` and re-enters either way, so it behaves like PIZZA's 3.1.2
and unlike 3.1.3. The port used to infer it from `v13_metas` — "is this
a 1.3?" — which happened to be right for the two builds then known.

So it is read out of the image, by shape rather than address: JSR, BCS,
three more JSRs, LDA abs, BPL. Unique in all three 3.1.x images, absent
from v1.3, and a miss keeps the family's own value.

### All four builds are now exact

| vs the emulator, at the chip bytes | 3.1.3 | 1.3 | 3.1.2 PIZZA | 3.1.2 bex |
|---|---|---|---|---|
| whole lines | 679/679 | 679/679 | 679/679 | 679/679 |
| the nonsense-story corpus | — | — | 1053/1053 | — |
| random Ctrl-V phoneme strings | 300/300 | 300/300 | **300/300** | **300/300** |
| random Ctrl-E command streams | 200/200 | 200/200 | **200/200** | **200/200** |

Nothing about a Textalker build is guessed from its version number any
more. Every table, the workspace and this one branch all come out of the
image itself.

## The check to run on a new build

```
tk_profile --disk <disk.dsk>
```

If it names every table, the C engine will run that image. If it cannot,
it says which table it failed to find — and the 6502 emulation, which is
the default engine and needs no profile at all, runs the image anyway.
