# What changed between Textalker 1.3 and 3, and why it broke some words

Jayson's question, session 20: *"If 1.3 was already saying boot, root and
shoot correctly, why did they mess it up in 3.x? My only thought is that
they made a rule change which, in a different context, was judged to do
more good than harm."*

That is exactly what happened, and the rule can be named. This note is
the evidence, because it is expensive to re-derive and it explains four
of the mispronunciations HANDOFF lists as "reproduce, not fix".

Everything here is measured out of the two images with
`tools/rule_walk.py`, `tools/syllabify.py` and `build/native/tk_phonemes`.
Nothing is inferred from documentation, of which there is none.

## The headline

**Textalker 3 is not a patched 1.3. It is the rule set rebuilt on top of
a syllabifier that 1.3 does not have**, and the rebuild traded
*enumeration* for *generalisation*: dozens of whole-word rules were
deleted and replaced by general rules that ask the syllabifier
questions. That bought a great deal. It cost exactly the places where
English's exceptions are the rule.

```
                          1.3     3.1.3
rules                     486      568
{FC} skip a boundary        0       70     none of these four
{FD} one of P T K B D G C   0       12     metacharacters exists
{FE} syllable count != 1    0       36     in 1.3 at all -- there
{FF} syllable count >= 3    0        7     is nothing to ask
```

Plus the whole syllabifier at `$DD10-$E126`, with four word lists, which
1.3 simply does not contain: watching `$F59F` while v1.3 speaks shows
the text going in and nothing else.

The clearest single illustration is the final Y. 1.3 asks *is this word
more than one syllable* the long way round --

```
1.3     {F7}{F8}{F4}Y{F3}    a vowel, then consonants, then a
                             consonant, then Y at the end of the word
3.1.3   {FE}Y{F3}            the syllable counter is not 1
```

-- and somebody went through the rule set replacing hand-rolled
approximations like that one with the direct question. The section
counts show where the work went. **Every vowel section grew and almost
every consonant section shrank:**

```
A +30   E +14   I +11   O +41   U +15
B  -7   D  -4   F  -4   H  -7   L  -3   N -2   T -14   W -8
```

## Exhibit A: OO before a stop consonant

```
1.3      OOR -> OWR      OOK -> UH      OOD -> UH      OO -> UW
3.1.3    OOR -> OWR      OO{FC}{FD} -> UH              OO -> UW
```

`{FD}` is a literal seven-byte table at `$E57E`: `P T K B D G C`,
terminated by `-`. So two literal rules became one class rule covering
every stop, and `{FC}` in front of it lets the match cross a syllable
boundary, so WOODEN and BOOKING come along too.

This is the phonics rule every child is taught: **a vowel is short in a
closed syllable**. It is right for the whole K family and the
good/wood/hood/stood family, and it gains FOOT and SOOT, which 1.3 gets
wrong. It is wrong for everything in OO+T and OO+P.

Scored over 64 OO+stop words (`foot football soot boot root shoot hoot
loot moot scoot toot uproot booth tooth smooth coop droop hoop loop
scoop snoop stoop swoop troop book look took cook hook shook brook good
wood hood stood food mood brood ...`):

```
1.3     53 of 64 right
3.1.3   32 of 64 right
```

| sub-class | 1.3 | 3.1.3 |
|---|---|---|
| OOK (book, look, took, cook, hook, shook, brook) | 18/20 | 18/20 |
| FOOT, SOOT and derived | 0/5 | **5/5** |
| OOT long (boot, root, shoot, tooth, smooth...) | **17/17** | 0/17 |
| OOP (loop, troop, scoop, stoop...) | **11/11** | 1/11 |
| OOD (good, wood, food, mood...) | 7/10 | **8/10** |

Two explanations that do NOT hold, both checked:

- **It was not space.** 1.3 is 2791 bytes of rule data for 486 rules,
  3.1.3 is 3097 for 568 -- 5.74 against 5.45 bytes a rule. The rule area
  GREW by 82 rules. Merging OOK and OOD saved about four bytes and they
  plainly had room. The motive was theory, not economy.
- **It was not that nobody tested words.** 3.1.3 carries a hand-written
  exception `{86}OO{84}`, which decodes as the literal letters `F OO D`,
  giving FOOD the long vowel. FOOD is *wrong* in 1.3, because its `OOD`
  rule catches it, so this is a real fix -- somebody sat down with the
  word FOOD and patched it by name. They did not go on to BOOT, ROOT,
  SHOOT or TOOTH.

## Exhibit B: COPY, and English's doubling convention

```
{F3}{F8}O{9B}{F4}{99}  ->  long O
 ^word start  ^syllable boundary  ^Y
```

Word start, any consonants, O, **a syllable boundary**, a consonant, Y.
This rule cannot be written in 1.3 at all: `{9B}`, the boundary, does not
exist there. It is the open-syllable half of the same phonics rule, and
PONY, HOLY, ROSY, PHONY, STORY, GLORY and IVORY all fire this exact rule
and all come out right.

What makes it worth the detail is why JOLLY, FOLLY, HOBBY, SOGGY and
FOGGY do *not* break: the doubled consonant makes the syllabifier split
`JOL|LY`, so the O is not at a syllable end and the rule never fires.
**The rule encodes English's own convention -- double the consonant to
mark the short vowel.** COPY and BODY are words where English did not
follow its own convention, and they land on the wrong side of a rule
that is otherwise correct.

They did write the exceptions they noticed. MODEL and ROBIN take a much
longer rule of their own -- `{F3}{F8}O{9B}{F4}{F5}{F4}{F8}{F3}`, O at a
syllable end followed by consonant, E/I/Y, consonant, word end -- and
PROPER takes another. COPY and BODY simply were not on the list.

MOTION is the one to hold up beside COPY: `{F3}{F8}O{9B}` fires, the
same open-syllable reading, and 3.1.3 says *moh-shun* where 1.3 says
*mah-shun*. Same theory, right answer.

## Exhibit C: NICKNAME is the cluster list, not two unlucky rules

HANDOFF used to say the K vanishes because `C{FC}{8B}` silences the C
before K and `{9B}K{8E}` silences the K before N "with nothing guarding
the combination". That is the mechanism but not the cause, and both
rules are correct in themselves -- CK is a digraph, and KN- is silent in
KNIGHT and KNEE.

**The cause is that `KN` is in the syllabifier's consonant-cluster list
at `$E1B1`**, so it moves to the next syllable as a unit:

```
nickname     |NIC|KNAME|      the K starts a syllable, before N
acknowledge  |AC|KNOWLEDGE|   the same
unknown      |UN|KNOWN|       the same
darkness     |DARK|NESS|      NESS is in the SUFFIX list, which wins
sickness     |SICK|NESS|      so the K survives
backpack     |BACK|PACK|      PK is not a cluster, so the K survives
picnic       |PIC|NIC|        no K at all
```

Once KN begins a syllable, the silent-K rule fires exactly as it does
for KNIGHT. So the bug is not confined to NICKNAME:

```
              3.1.3                          1.3
NICKNAME      N IH . . N EY M                N IH K N EY M
ACKNOWLEDGE   AE . . N OW L EH D JH          AE K N OW L EH D JH
UNKNOWN       AH N . N OW N                  AH N K N OW N
```

*a-nowledge* and *un-nown*. 1.3 says all three correctly, because it has
no syllabifier to be misled by. Any word with a word-internal KN and no
suffix to intervene is affected.

## Exhibit D: the voiced TH was a LIST, and they made it a rule

This is the cleanest case of the whole pattern, because the thing 1.3
did was simply correct: the voiced-TH words in English *are* a closed
list of function words, and 1.3 enumerates them --

```
THAT THIS THEY THERE THEIR THAN THEM THESE THEN THOSE THOUGH THUS
```

-- each a whole-word rule in the T section, with unvoiced TH as the
default. 3.1.3 deleted the list (the T section went from 31 rules to
17) and generalised to *TH before A or E at a syllable start is voiced*,
keeping literal-ish rules only for THIS and THOSE.

The result, in both directions:

```
            3.1.3      1.3
THOUGH      TH OW      DH OW     lost -- and ALTHOUGH with it
THUS        TH AH S    DH AH S   lost
THAW        DH AO      TH AO     newly broken by the generalisation
THATCH      DH AE CH   TH AE CH  newly broken
THANK       TH AE NG K TH AE NG K  saved by its OWN exception rule,
                                   TH{81}{8E}{8B} -- literally THANK
```

A rule that needs an exception for THANK, breaks THAW and THATCH, and
drops THOUGH and THUS, in place of a list that was right.

## Exhibit E: what the deleted word rules cost, which was mostly nothing

It is worth being fair about the size of the win. 1.3's consonant
sections are full of whole-word literals -- the B section alone has
BOTH, BY, BUT, BEEN, BUS, BUILD and BEING; the H section has HAVE, HAS,
HAD, HERE, HOW and HI. 3.1.3 deletes essentially all of them (B 11 -> 4,
H 11 -> 4) and **the general machinery reproduces almost every one**:
THAT, THIS, THEY, THAN, THEM, THESE, THEN, THROUGH, THOSE, TWO, BOTH,
BUT, BY, BEEN, BUS, HAVE, HAS, HAD, HERE, HOW, HI and HOUR all still
come out right without a rule of their own. That is a substantial piece
of engineering, and the casualties above are the exceptions to it.

The syllabifier earns its keep elsewhere too:

```
              3.1.3                1.3
ANGRY         AE NG G R IY         AE NG G IY      1.3 drops the R
DARLING       D AAR L IH NG        D AAR L IY NG   "darl-eeng"
GARGLING      G AAR G AH L IH NG   G AAR G L IY NG loses the syllabic L
MOTION        M OW SH AH N         M AA SH AH N    "mah-shun"
LAMB THUMB    L AE M / TH AH M     L AE M B        3.1.3 silences the B
GIRL GIFT     G ER L / G IH F T    JH ER L         1.3 says "jirl"
```

`{8D}B -> silence` (B after M) and the narrowed soft-G are both new in
3.1.3. 1.3's `-ING` comes out *-eeng* throughout; 3.1.3 fixes it.

## Other systematic changes worth knowing

**The unstressed prefixes went half-right.** 1.3 has `{F3}BE{F4}{F7}`,
an explicit BE- rule giving the reduced vowel. 3.1.3 deleted it and
`E{9B}` (E at a syllable end) gives EH instead, so BECAUSE, BEFORE,
BELOW, BELIEVE, BEGIN, DECIDE, DEFAULT, DELAY, DEMAND all became
*beh-*/*deh-*. But 3.1.3 ADDED `{F3}RE{9B}{F4}`, so RETURN, REPORT,
REMOVE and RESULT went from 1.3's *ree-turn* to the correct *rih-turn*;
and ABOVE went from *ay-bove* to *uh-bove*. Neither version has all
three prefixes. AGAIN is 1.3's: *a-gen* against 3.1.3's *a-gain*.

**A schwa before a final L, and this one is a DESIGN CHOICE.** 3.1.3
inserts one wherever a vowel is followed by a word-final L: CALL, BALL,
TALL, FALL, WALL, SMALL, COOL, POOL, TOOL, FOOL, SCHOOL, FULL, PULL,
BELL, TELL, WELL, WILL, STILL, GIRL. 1.3 does not, and it shows up in
the phoneme string as an extra `AX`. A dark L is a real feature of
English and the TMS5220 has no allophone for one, so approximating it
with a schwa is a reasonable thing to try -- and **Jayson listened to
both and judged it a deliberate improvement rather than a defect**. It
is the one change here that is unambiguously somebody making the voice
better. To hear it, say "call the cool pool, the girl will still tell"
through each version.

## Exhibit F: the soft G, where both versions fail the same way round

The G section is the one place where **1.3 makes the identical mistake
from the opposite direction**, which is the best evidence that the
difference between the versions is method rather than care.

1.3's rule is `G{F5}` -- G before E, I or Y is a J -- with the
exceptions enumerated ahead of it, one rule per word: `GIV`, `GE{94}`
(GET), `{93}{95}GESS` (GUESS), `GREAT`, `GEOR` (GEORGE) and the two
Applesoft keywords `GO{94}{8F}` and `GOS{95}{82}`, which are GOTO and
GOSUB. The list is incomplete, and everything that fell off it is
audibly wrong:

```
              1.3          3.1.3
GIRL          JH ER L      G ER L        "jirl"
GIFT          JH IH F T    G IH F T      "jift"
GILL          JH IH L      G IH L
GIGGLE        JH IH G AX L G IH G AH L
GILBERT       JH IH L ...  G IH L ...
```

3.1.3 fixed all five with one general rule, `{F3}G{89}` -- **a
word-initial G followed by I is hard** -- and that rule promptly broke
the other half of the class:

```
              1.3          3.1.3
GIANT         JH ...       G AY AH N T   "guy-ant"
GIRAFFE       JH ...       G ER EY F     "guh-rafe"
GINGER        JH IH ...    G IH NG G ER  "ging-ger"
GIN           JH IH N      G IH N
```

So 1.3 enumerates and its list runs out at GIRL; 3.1.3 generalises and
its rule runs over GIANT. Neither version has a working theory of the
soft G, and both of them are wrong about TIGER and ANGER (*tie-jer*,
*an-jer*).

BEGIN is the footnote that makes it. 1.3 *did* try to except it --
`{F3}{82}{F7}G`, word start, B, one or more vowels, G, hard -- and the
rule is defeated by a bug in 1.3's own engine: its `$F7` does not step
back after a greedy run (`$DC0F`, `$DD2E`), so the vowel run swallows
the G and the match fails. *bih-jin* is an exception that was written
and did not fire.

### Somebody maintained the easter egg

GHOTI is in both versions, and in 3.1.3 the rule is `GHO{E6}TI`, with
the syllable sentinel written into it -- the syllabifier splits it
`|GHO|TI|`. GEORGE is the same: `{F3}GE{E6}ORGE{F3}`, against 1.3's
plain `GEOR`. Those two are the only whole-word rules left in 3.1.3's G
section, and **both were deliberately rewritten to survive the new
syllabifier**. Whoever rebuilt the rule set for Textalker 3 went to the
trouble of porting the joke.

**The CON- prefix** moved from 1.3's AA to 3.1.3's AO: CONFIRM, CONVERT,
CONCLUDE, CONTINUOUS are *cawn-* rather than *cahn-*. Neither is the
schwa English actually uses. CON is in the prefix list, so the
syllabifier is involved.

Over the 200 distinct words in `reference_text/`, **74 of them -- 37% --
are pronounced differently by the two versions** once the stress marks
are ignored. This was not a tidy-up. It was a rewrite.

## The syllabifier's four word lists

Together they are the whole of Textalker 3's knowledge of English word
structure: 9 prefixes, 20 suffixes, 15 digraphs and 26 clusters, 70 entries,
read straight out of the image. **This fork does not reproduce them**, as
they are Street Electronics' data (see `README.md` in this folder).
`python tools/syllabify.py` prints them from your own image, and Jayson
Smith's branch `Textalker-RW` has them in this note.

Prefixes are only stripped from words of 7 characters or more. Digraphs are
vowel pairs that must never be split; OO is one, so the literal OO in the
rule above always matches, and {FC} is what carries it past a boundary AFTER
the pair, which is a real case: SHOOTING is |SHOO|TING|. Clusters are moved
whole to the next syllable, and KN being one of them is the NICKNAME bug.

`SOME` and `SUN` as prefixes are worth a second look: SOMETHING and
SUNSHINE are compounds rather than prefixed words, so the list is really
"things that start words and should not be merged into the next
syllable".

## How to repeat any of this

```
python tools/rule_walk.py boot copy nickname            which rule fired
python tools/rule_walk.py --version 1.3 boot            ...in 1.3
build/native/tk_phonemes <ram> <obj> --arpabet WORD      the phonemes
build/native/tk_phonemes <ram> <obj> --buffer WORD       the syllables
python tools/syllabify.py                                the four lists
```

The metacharacter table is in HANDOFF ("The metacharacters, all from the
code"); `{9B}` is the syllable sentinel, `{FC}` skips sentinels, `{FD}`
is the stop-consonant table at `$E57E`, `{FE}` and `{FF}` are the
syllable counter, and `{81}-{9A}` are context letters with `$40` added,
so `{86}` is F and `{99}` is Y.

## The one-line version

Textalker 3 replaced a list of words with a theory of English. The
theory is a good one -- open syllables long, closed syllables short,
clusters move right, suffixes bind -- and where English obeys it the
result is better than 1.3. BOOT, COPY, NICKNAME and THOUGH are four
places where English does not obey it, and nobody checked.

The G section is the control that proves it is method and not care:
there 1.3 enumerates until its list runs out at GIRL, 3.1.3 generalises
until its rule runs over GIANT, and both are wrong about TIGER.

### One practical consequence

`nvda-addon/doc/en/readme.md` used to offer "giraffe" as its example of
a soft G when explaining compressed speech. **Textalker 3.1.3, the
default voice, says GIRAFFE with a HARD G** -- `{F3}G{89}` above -- so
the example did not illustrate what it claimed on that voice. It now
says "gentle", which is soft in both versions, as are GENERAL, GEM and
GYM; "journey" beside it was always fine.
