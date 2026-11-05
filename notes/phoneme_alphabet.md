# Textalker's phoneme alphabet, and how it was read

This is the table the letter-to-sound seam stands on. Stage 2 consumes a
string of one-byte phoneme codes and nothing else, so anything that can
emit that alphabet can drive Textalker's voice — but only once someone
can say **which code is which sound**. That is what this note settles.

`src/tk_arpabet.c` implements it. `tools/seam_diff.py` tests it.

## What a code is

From `$DC22-$DC92` (ported in `src/tk_synth.c`), the driver over the
phoneme buffer decodes a code into an **allophone index** and a **pitch
group**:

| code | index | group |
|---|---|---|
| `$00-$19` | the code itself | 1 |
| `$1A-$33` | code − `$1A` | 2 |
| `$34-$4D` | code − `$34` | 3 |
| `$4E-$5D` | code − `$34` (`$1A-$29`) | unchanged |
| `$5E-$B2` | code − `$34` (`$2A-$7E`) | unchanged, with the `$DC71` lookahead |
| `$7F` | — | silence |
| `$B3-$BF` | — | set the group to `code & $0F` |
| `$1A` | — | end of utterance |

So the **26 low allophones carry three codes each**, one per pitch
group, and everything from index `$1A` up carries exactly one, at
`index + $34`. That is the whole encoding.

`$1A` is the one collision: it would be allophone `$00` at group 2, and
is the terminator instead.

**The pitch group is the stress.** `$EA7F+1` holds `FE 00 02` — the
offset added to the running pitch for groups 1, 2 and 3, i.e. −2, 0 and
+2. Group 3 is the raised one.

## What an index is

The **Ctrl-V character map** turns a typed character into an index:
`index = charmap[ord(c) - $20]`, at `$EA44` in v3.1.3 and `$EFAA` in
v1.3. **The two maps are byte-identical**, as is the allophone data
behind them, which is the earlier finding that Street Electronics
changed the rules between versions and not the voice.

55 characters are live. Inverted, they give the inventory:

```
$00  , ]   pause            $1A  L      $2A  B
$01  ;     AA  father       $1B  R      $2B  D
$02  & ^ _ IY  beet         $1C  W      $2C  G
$03  Q     UH  book         $1D  M      $2D  K
$04  '     AX  (see below)  $1E  N      $2E  P
$05  @     EY  bait         $1F  /  NG  $2F  T
$06  !     AY  bite         $20  (  DH  $30  C  CH
$07  O     OW  boat         $21  V      $31  J  JH
$08  #     AW  bout         $22  Y
$09  %     YU  cute         $23  Z      $32-$39  the same eight stops
$0A  A     AE  bat          $24  X  ZH           again, with the $E674
$0B  *     AO  law          $25  F               release variant
$0C  E     EH  bet          $26  H  HH
$0D  I     IH  bit          $27  S
$0E  :     UW  boot         $28  $  SH
$0F  U     AH  but          $29  )  TH
$10  ?     OY  boy
$11-$18    the R-coloured forms of $01-$08 ($E62F adds $10 when an
           R follows): AAR car, IYR here, UHR poor, AXR = ER bird,
           EYR hair, AYR fire, OWR more, AWR hour
$19        the pause again, at group 1 -- the word separator in v3.1.3
$37 $3F    the energy steppers, + and -      $4F $5F $6F  the glides
$FE $FF    end markers
```

**`$0B` has no R-coloured form**, because `$E62F` only colours indices
1-8. That is why MORE and FOUR come out as OW + R rather than AO + R,
and it is a fact about the machine rather than a choice.

`$11-$18` are not reachable from the character map at all; only `$E62F`
produces them, which is why they had to be found through words rather
than through the table.

## How the vowels were identified — and how one pair was got wrong

Not by guessing at the characters: by asking stage 1. Every vowel of
English goes into a frame whose pronunciation is not in doubt, through
`tk_phonemes`, and the code is read off.

**Do it with four frames per vowel, not one.** The first pass used one
each, and got `$01` and `$0B` exactly backwards. The two frames chosen
were COT for `$0B` and BOUGHT for `$01` — which is the cot/caught
minimal pair, precisely where a 1980s speech card is as likely to merge
as a speaker is, and BOUGHT is a word Textalker gets *wrong* (it really
does say "baht"). Jayson caught it by ear from a rendering. Four frames
settle it in one run and leave nothing to argue with:

```
LAW SAW TALK AWFUL CAUGHT LAWN FAWN CHALK   ->  all $0B, so AO
FATHER                                       ->      $01
ARM CAR FAR HARD                             ->      $11 = $01 + R
```

`$01` is AA, the vowel of FATHER, and R-coloured it is the vowel of
CAR. `$0B` is AO. The cot/caught merger is real in Textalker — COT,
COMMA, BOTTLE, DOG, SONG and LONG all take `$0B` — which is why those
words were no help at all.

The rest held under the same treatment:

```
BEET SEE MEAT FIELD    IY      BOOK PUT FOOT GOOD     UH
BAIT DAY MADE RAIN     EY      BITE MY TIME HIGH      AY
BOAT GO SHOW ROAD      OW      BOUT NOW HOUSE LOUD    AW
CUTE FEW MUSIC HUMAN   YU      BAT CAT HAND MAP       AE
BET RED HEAD SAID      EH      BIT SIT MILK GIVE      IH
TOO BLUE SOUP          UW      BUT CUP LOVE           AH
BOY TOY NOISE JOIN     OY      BIRD HERD WORD TURN    ER
HERE BEER NEAR   IYR   HAIR CARE  EYR   FIRE HIRE  AYR
MORE FOUR SOUR   OWR   HOUR       AWR
```

**BOOT is deliberately not in the UW list**: Textalker says it with the
vowel of BOOK. So is TOUCH, which it gives the vowel of BOUT, and CALM
and PALM, which get AE. Those are rule bugs, not alphabet facts, and
using one as a frame word is how the mistake above happened. (BOOT is
also the example behind the `=booht` respelling syntax in
`src/tk_dict.c`: the quickest fix for a rule bug is to spell the word
the way that makes Textalker say it.)

**And a rule bug belongs to a VERSION, which is why a dictionary does
too.** BOOT, ROOT and SHOOT all take the BOOK vowel in v3.1.3 and all
three are right in v1.3 — the frame words above were chosen to be safe
in both, and these are exactly the sort that are not. ROUTE is wrong in
both (it rhymes with BOUT), so `=root` repairs it under v1.3 and does
nothing at all under v3.1.3, which needs `=rooht`.

The consonants that share a spelling were separated the same way:

```
THIN  vs THEN      $29 TH   vs $20 DH
SING               $1F NG
VISION, MEASURE    $24 ZH
WHO, LUKE, POOL    $0E UW
JUDGE, CHURCH      $31 JH, $30 CH
```

The two that are not a plain ARPABET symbol:

- **`$09` is a single allophone for /juː/**, not Y followed by UW. It is
  what the rules emit for CUTE, FEW, USE and UNIT, so `tk_arpabet.c`
  folds a `Y UW` pair into it.
- **`$04` is a glide, not a vowel of its own.** It appears as the
  off-glide of POOL (`UW AX L`) and, R-coloured at `$14`, as ER.

**Textalker has no separate schwa.** ABOUT, SOFA, BANANA and COMMA all
take `$0F`, the vowel of BUT, so `AH0` maps there and nothing is lost.

## An R after a vowel is not a separate sound

`$E62F` adds `$10` to allophone indices 1-8 when an R follows, so
Textalker's own rules emit **one R-coloured code** where a dictionary
would write two symbols: CAR is `$2B`, not AA followed by R.
`tk_arpabet.c` folds `<vowel> R` the same way, or a word like STARTING
comes out with an audible R of its own — which is what it sounded like
before the fold existed.

Two of the folds are not the index arithmetic, and both were read off
Textalker's own output rather than reasoned:

- **EH + R takes `$15`**, the EY-coloured one, which is what it gives
  HAIR and CARE where CMUdict writes `EH`.
- **AO + R takes `$17`**, the OW-coloured one, because `$0B` has no
  R-form at all. That is MORE and FOUR.

`ER` after a vowel folds only after AY and AW, which is how CMUdict
spells FIRE (`F AY1 ER0`) and HOUR (`AW1 ER0`) and how Textalker speaks
them — one nucleus. Folding it after EY as well would turn PLAYER into
"pair".

## Two things the round trip cannot preserve

`tools/seam_diff.py` writes Textalker's own output back out as ARPABET,
feeds it in as a dictionary and checks the codes come back. Over the
reference corpus that is exact for **151 of 155** words in v3.1.3 and
**156 of 181** in v1.3, with **1 genuine mismatch across both**. The
rest are these:

**The pitch group does not survive ARPABET.** ARPABET has three stress
levels and Textalker three pitch groups, and they do not line up:
v3.1.3 speaks an unstressed vowel at group 2 and **v1.3 at group 1**
(THE is `54 29` in one and `54 0F` in the other). Nothing in `AH0` says
which, so `tk_arpabet.c` maps stress 0 and 2 to group 2 and stress 1 to
group 3, and 25 of v1.3's words come back one group high. The sound is
identical; only the pitch offset moves, by 2 on the 1-9 scale. The
`/`-escape in `tk_dict.c` reaches a group directly for anyone who cares.

**/z/ is a pair and its order follows the spelling.** `$23` is never
spoken alone by Textalker's rules: every /z/ is `$23` beside `$27`,
which means `$23` is the voiced part and wants the frication next to it.
Which way round is not phonetic — the letter Z gives "S Z" (ZERO, ZONE,
ZEBRA, LAZY) and an S pronounced /z/ gives "Z S" (IS, WAS, DOGS, PLEASE,
NOSE) — and RESEARCH and SYNTHESIZER break even that. `tk_arpabet.c`
emits "Z S" always and reads either back as one /z/, which leaves
SYNTHESIZER as the single genuine round-trip failure in the corpus.

## The one thing to be careful about

**This note is a measurement, not Street Electronics' data.** No table
here is compiled into anything that ships: the allophone frames, the
character map, the rules and the attribute table are all still read out
of the user's own image at run time. What `tk_arpabet.c` carries is the
*correspondence* — that Textalker's allophone `$02` is the vowel of
"beet" — which is our own finding about how the thing sounds, arrived at
with the word lists above. It is the same kind of statement as a phoneme
chart in a manual, and it is 40 lines long.
