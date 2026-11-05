#!/usr/bin/env python3
"""A native implementation of Textalker's letter-to-sound rule walker,
and a differential test of it against the real thing.

This is the first half of stage 1 of the C rewrite, written in Python
first so the semantics can be settled cheaply before committing them to
C. It reads the rule table out of a user-supplied Textalker image and
walks it; it does not reimplement any of the rules themselves.

**This is a transcription of the 6502, not a model of it.** The earlier
version treated syllable boundaries as zero-width assertions over the
input string, and needed a hand-written "magic E" special case to
explain why HOPE silences its E but HOPESX does not. Both were wrong.
Textalker matches against a *buffer* in which a syllable boundary is a
real $E6 byte, and $E6 masks to $66, whose entry in the attribute table
at $EF24 is $00 -- neither vowel nor consonant nor separator, so it
blocks every class. HOPE is |HOPE| but HOPESX is |HO|PESX|, so in
HOPESX the {F7} of {F7}{F8}{F4}E runs into the sentinel and the rule
fails. That is the whole of the "magic E": no special case, just a byte
in the way. Matching the real buffer removes the hack and a family of
failures with it.

The matcher below follows notes/disasm_rule_engine.txt instruction for
instruction:

    $E1FA  main loop: step over sentinels, counting them into $FD93,
           pick the section from the dispatch table, try rules in order
    $E2F0  find the anchor -- the first pattern byte in $A0-$EF -- and
           match the literal run forward from the cursor
    $E354  left context, backward       $E3A9  right context, forward
    $E40D  class dispatch for $F3-$FF, through an RTS-trick table
    $E596  attribute lookup: A AND $EF24[buf[X] AND $7F]

Metacharacters, every one read from the code rather than fitted:

  $C1-$DA   focus letter    matched AND consumed
  $81-$9A   context letter  $80 + index; $E383/$E3D9 do ADC #$40
  $C0       wildcard inside the literal run          ($E309)
  $9C       wildcard in either context               ($E378, $E3CE)
  $9B       the syllable sentinel: $A6 + $40 = $E6   ($E380, $E3D6)
  $E6       the sentinel again, as a plain literal in the $A0-$EF range
  $F3       word boundary: skip one sentinel, then end-of-buffer or a
            character with attribute bit $02                ($E439)
  $F4       attribute bit $40 -- consonant                  ($E450)
  $F5       attribute bit $04 -- E, I, Y                    ($E458)
  $F6       attribute bit $20 -- B D G J L M N R V W Z      ($E460)
  $F7       one or more of bit $80 -- vowel, GREEDY         ($E468)
  $F8       zero or more of bit $40 -- consonant, GREEDY    ($E47F)
  $F9       an inflectional ending running to the word end  ($E49E)
  $FA       attribute bit $01, or the C/S/T of CH, SH, TH   ($E510)
  $FB       attribute bit $10, or the H of CH or SH         ($E53F)
  $FC       skip zero or more sentinels; always succeeds    ($E48F)
  $FD       one of P T K B D G C, from the table at $E57E   ($E569)
  $FE       zero width: the syllable counter $FD93 is not 1 ($E585)
  $FF       zero width: the syllable counter is 3 or more   ($E590)

$F7 and $F8 are greedy with **no backtracking** -- the loop at $E47F
runs to the end of the class and steps back exactly one. $FE and $FF are
zero width because $E3A2 and $E3F9 undo the caller's step before
dispatching to them.

Digits and punctuation go through the same rule engine as letters. The
dispatch table is indexed by raw ASCII (see rule_probe.DISPATCH), so
"5" has a section whose single rule spells FIVE -- which is why both
Textalker versions read 69105 as "six nine one zero five".

Usage:
    python tools/rule_walk.py WORD [WORD ...]      # walk, and show rules
    python tools/rule_walk.py --diff WORDFILE      # differential test
    python tools/rule_walk.py --diff-refs          # corpus from reference_text/
    python tools/rule_walk.py --diff-story         # the nonsense-word story
"""
import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rule_probe import load_rules, fmt_pattern, fmt_phonemes, HARNESS
import profiles
import syllabify


# Every scratch name carries the process id. Two of these tools running
# at once otherwise share bd_in.bin / bd_bytes.txt and each reads the
# other's oracle, which does not fail -- it just reports disagreements
# that are not there. It cost two "failing" seeds that were fine.
def _scratch(tmpdir, name):
    stem, ext = os.path.splitext(name)
    return os.path.join(tmpdir, "%s_%d%s" % (stem, os.getpid(), ext))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORD_SEP = 0x19
TERMINATOR = 0x1A
OBJ = os.path.join(ROOT, "roms", "textalker.obj.bin")

BASE = 0xD000            # where the image loads
SENT = 0xE6              # the syllable-boundary sentinel, as stored
TERM = 0x00              # what lies past either end of the buffer

# Attribute-table bits at $EF24, named by the handler that reads them.
A_VOWEL, A_CONS, A_F6, A_FB, A_F5, A_SEP, A_FA = (
    0x80, 0x40, 0x20, 0x10, 0x04, 0x02, 0x01)

STOPS = (0xD0, 0xD4, 0xCB, 0xC2, 0xC4, 0xC7, 0xC3)      # $E57E: P T K B D G C
LET = dict((c, 0x80 | ord(c)) for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ")

USE_SYLLABLES = True
_TABLES = None
_ATTR = None
_PROF = None

# v1.3's context matcher is a chain of comparisons ($DB2B) rather than
# 3.1.3's RTS-trick table, and everything above $FA falls into the one
# handler. So it has $F9 as a second $F8, no $FC/$FD/$FE/$FF, and no
# $9C or $9B -- it has no syllabifier for those to refer to.
# The two directions map the codes DIFFERENTLY, which 3.1.3 does not do.
# Backward ($DB2B): $F6 is the voiced-consonant test, $F9 is a second
# $F8, $FA is the digraph class and everything above it is the sibilant
# one. Forward ($DC4D): $F6 is just "a consonant", and $F9 AND EVERYTHING
# ABOVE IT is the inflectional-ending matcher at $DCC4 -- so $FA and $FB
# have no forward meaning at all.
V13_BACK = {0xF9: 0xF8, 0xFC: 0xFB, 0xFD: 0xFB, 0xFE: 0xFB, 0xFF: 0xFB}
V13_FWD = {0xF6: 0xF4, 0xFA: 0xF9, 0xFB: 0xF9, 0xFC: 0xF9,
           0xFD: 0xF9, 0xFE: 0xF9, 0xFF: 0xF9}


def _tables(prof=None):
    """The attribute table and, for 3.1.3, the syllabifier's tables."""
    global _TABLES, _ATTR, _PROF
    prof = prof or _PROF or profiles.PROFILES["3.1.3"]
    if _TABLES is None or _PROF is not prof:
        _PROF = prof
        rom = open(prof["obj"], "rb").read()
        a = prof["attrs"] - prof["base"]
        _TABLES = {"rom": rom, "attrs": rom[a:a + 128], "prof": prof}
        if prof["syllabifier"]:
            _TABLES = syllabify.load_tables(prof["obj"])
            _TABLES["prof"] = prof
        _ATTR = _TABLES["attrs"]
    return _TABLES


def build_buffer(word):
    """The engine's input buffer at $FDAC, straight from the syllabifier
    -- which is itself a transcription of $DD10, so this is the buffer
    the 6502 hands to the matcher, sentinels and all."""
    t = _tables()
    if not USE_SYLLABLES or not t["prof"]["syllabifier"]:
        return [0x80 | ord(c) for c in word]
    return syllabify.build(word, t)


def _b(buf, x):
    """buf[x], with the terminator past either end -- the 6502 reads
    whatever is there and every handler then fails on it."""
    return buf[x] if 0 <= x < len(buf) else TERM


def _attr(buf, x):
    """$E596: the attribute byte for the character at x."""
    return _ATTR[_b(buf, x) & 0x7F]


# ---------------------------------------------------------------- classes
# Each handler returns (matched, x), where x is where it left the cursor.
# The caller then steps by one in the direction of travel, so a handler
# that leaves x alone consumes exactly one character, and a zero-width
# one steps back by one so the next step lands on the same place.

def h_f3(buf, x, step, limit):
    """$E439: word boundary; $DC81 in v1.3, which is only the attribute
    test -- there are no sentinels to skip."""
    if _PROF is not None and _PROF["metas"] == "v13":
        return bool(_attr(buf, x) & A_SEP), x
    if _b(buf, x) == SENT:
        x += step
    if x == limit:
        return True, x
    return bool(_attr(buf, x) & A_SEP), x


def h_run(buf, x, step, limit, bit, need_one, step_back=True):
    """$E468 ($F7) and $E47F ($F8): a greedy run, no backtracking.

    3.1.3 steps back one at the end of both ($E48C). v1.3's $F8 does the
    same ($DBB6, $DCA2) but its $F7 does NOT -- $DC0F and $DD2E return
    with X wherever the loop stopped -- so a vowel run there swallows one
    extra position. That is what makes {F3}{82}{F7}G fail for BEGIN: the
    run eats the E, the caller's step takes X off the front of the
    buffer, and the boundary never gets a chance to match. Hence "bih-jin"
    rather than "beg-in"."""
    start = x
    while _attr(buf, x) & bit:
        x += step
        if x == limit:
            break
    if need_one and x == start:
        return False, start
    return True, (x - step) if step_back else x


def h_fc(buf, x, step, limit):
    """$E48F: skip a run of sentinels; always succeeds."""
    while _b(buf, x) == SENT:
        x += step
        if x == limit:
            break
    return True, x - step


def h_fa(buf, x, end):
    """$E510: attribute bit $01, or the C/S/T of a CH/SH/TH digraph."""
    if _attr(buf, x) & A_FA:
        return True, x
    if x + 1 == end or _b(buf, x + 1) != LET["H"]:
        return False, x
    return _b(buf, x) in (LET["C"], LET["S"], LET["T"]), x


def h_fa_v13(buf, x):
    """$DB6F: v1.3's $FA -- attribute bit $01, or the H of CH, SH or TH.

    3.1.3's $FA ($E510) looks FORWARD for the H and matches the C, S or
    T; v1.3's looks at the H itself and steps back onto the C/S/T, which
    it then leaves X sitting on. So it consumes two characters, the way
    3.1.3's $FB does, and it is the reason {FA}U fires for the U of
    ENTHUSIASM."""
    if _attr(buf, x) & A_FA:
        return True, x
    if _b(buf, x) != LET["H"]:
        return False, x
    x -= 1
    if x < 0:
        return False, x
    return _b(buf, x) in (LET["C"], LET["S"], LET["T"]), x


def h_fb(buf, x):
    """$E53F: attribute bit $10, or the H of CH or SH.

    The DEX at $E552 is not patched for direction and is not undone on
    the success path, so when $FB matches through the digraph it leaves
    x one further back than when it matches through the bit."""
    if _attr(buf, x) & A_FB:
        return True, x
    if _b(buf, x) != LET["H"]:
        return False, x
    x -= 1
    if x < 0:
        return False, x
    return _b(buf, x) in (LET["C"], LET["S"]), x


def h_f9_v13(buf, x, end, step, limit):
    """$DCC4: v1.3's inflectional ending.

    E followed by R, S or D; E, L, Y; or I, N, G -- and then a word
    boundary. The branch at $DCE2 is the one that matters: an E followed
    by anything that is not R, S, D or L falls THROUGH to the boundary
    test at $DD0F rather than failing, which is how a bare final E
    matches. Reading it as a failure costs A{F4}{F9}, and with it the
    second A of INANIMATE.

    Every step here is capped at a FIXED $8F, not at the end of the
    text: $DD0D reaching $8F is the only way to succeed without a
    boundary. Standing in the end of the buffer instead makes E{F4}{F9}
    match "EZES" and say "ay-zes", because the machine goes on to test
    the leftover byte past the S and finds it is not a separator."""
    CAP = 0x8F                                   # $DCCF, $DCE7, $DD0B...
    start = x

    def dd0f(x):                                 # $DD0F
        ok, x2 = h_f3(buf, x, step, limit)
        return (True, x2) if ok else (False, start)

    def dd0a(x):                                 # $DD0A
        x += 1
        return (True, x) if x == CAP else dd0f(x)

    a = _b(buf, x)
    if a == LET["E"]:
        x += 1
        if x == CAP:                             # $DCD1
            return False, start
        a = _b(buf, x)
        if a in (LET["R"], LET["S"], LET["D"]):
            return dd0a(x)
        if a == LET["L"]:                        # $DCE2
            x += 1
            if x == CAP:                         # $DCE9
                return False, start
            return dd0a(x) if _b(buf, x) == LET["Y"] else (False, start)
        return dd0f(x)                           # the bare final E
    if a == LET["I"]:                            # $DCF4
        # A BUG IN v1.3, and it has to be reproduced. $DCF8 does INX but
        # never reloads the character, so $DCFD's CMP #$CE still sees the
        # I -- and $C9 is not $CE, so the branch fails immediately. The
        # E path reloads properly at $DCD3 and $DCEB; this one does not.
        # So {F9} can never match "ING" in 1.3, which is why DARLING
        # takes the plain L rule where the pattern says it should not.
        return False, start
    return False, start


def h_f9(buf, x, end, step, limit):
    """$E49E: an inflectional ending that runs to the word end.

    E+R, E+S, E+ST, E+D, E+LY, I+NG or a bare final E, optionally with a
    trailing S, and then the word must end or a boundary follow. This is
    what makes SILENCE come out right: $F9 lands on the E of -LENCE,
    which is followed by N, so the rule fails and the engine falls
    through to I{F8}{9B} => 27 instead of 20."""
    start = x

    def e4fe(x):
        x += 1
        if x == end:
            return True, x
        ok, x2 = h_f3(buf, x, step, limit)
        return (True, x2) if ok else (False, start)

    def e4f6(x):
        if _b(buf, x) == LET["S"]:
            return e4fe(x)
        return e4fe(x - 1)

    def e4eb(x):
        if x >= end:
            return False, start
        x += 1
        if x == end:
            return True, x
        return e4f6(x)

    a = _b(buf, x)
    if a == LET["E"]:
        x += 1
        a = _b(buf, x)
        if a == LET["R"]:
            return e4eb(x)
        if a == LET["S"]:
            x += 1
            if x == end:
                return True, x
            if _b(buf, x) == LET["T"]:
                return e4eb(x)
            return e4fe(x - 1)
        if a == LET["D"]:
            return e4fe(x)
        if a == LET["L"]:
            x += 1
            if _b(buf, x) == LET["Y"]:
                return e4eb(x)
            return False, start
        return e4f6(x)
    if a == LET["I"]:
        x += 1
        if _b(buf, x) != LET["N"]:
            return False, start
        x += 1
        if _b(buf, x) != LET["G"]:
            return False, start
        return e4eb(x)
    return False, start


def dispatch(code, buf, x, syl, end, step, limit, run_limit=None):
    """$E40D: the class handlers for pattern bytes $F3-$FF."""
    if run_limit is None:
        run_limit = limit
    if code == 0xF3:
        return h_f3(buf, x, step, limit)
    if code == 0xF4:
        return bool(_attr(buf, x) & A_CONS), x
    if code == 0xF5:
        return bool(_attr(buf, x) & A_F5), x
    if code == 0xF6:
        return bool(_attr(buf, x) & A_F6), x
    if code == 0xF7:
        v13 = _PROF is not None and _PROF["metas"] == "v13"
        return h_run(buf, x, step, run_limit, A_VOWEL, True,
                     step_back=not v13)
    if code == 0xF8:
        return h_run(buf, x, step, run_limit, A_CONS, False)
    if code == 0xF9:
        if _PROF is not None and _PROF["metas"] == "v13":
            return h_f9_v13(buf, x, end, step, limit)
        return h_f9(buf, x, end, step, limit)
    if code == 0xFA:
        if _PROF is not None and _PROF["metas"] == "v13":
            return h_fa_v13(buf, x)
        return h_fa(buf, x, end)
    if code == 0xFB:
        return h_fb(buf, x)
    if code == 0xFC:
        return h_fc(buf, x, step, limit)
    if code == 0xFD:
        return _b(buf, x) in STOPS, x
    if code == 0xFE:                        # zero width: $E3A2 / $E3F9
        return syl != 1, x - step
    if code == 0xFF:
        return syl >= 3, x - step
    return False, x


# ---------------------------------------------------------------- matcher

def match_ctx(pat, y, buf, x, syl, end, back):
    """$E354 (backward) and $E3A9 (forward), which are one routine with
    the direction patched into it by $E400."""
    step = -1 if back else 1
    limit = -1 if back else end
    # Where a greedy run stops. 3.1.3 uses the end of the text; v1.3
    # uses a fixed $8F, so its runs walk on into the leftovers.
    fwd = _PROF.get("fwd_run_limit") if _PROF else None
    run_limit = limit if (back or fwd is None) else fwd
    while True:
        x += step
        y += step
        a = pat[y] if 0 <= y < len(pat) else TERM
        if a < 0x80:                        # ran into the payload: matched
            return True
        if x == limit:
            # $E35C / $E3B1: off the end. Only $F3 and $F8 may remain.
            while 0 <= y < len(pat) and pat[y] in (0xF3, 0xF8):
                y += step
            return (pat[y] if 0 <= y < len(pat) else TERM) < 0x80
        if a >= 0xF3:
            if _PROF is not None and _PROF["metas"] == "v13":
                a = (V13_BACK if back else V13_FWD).get(a, a)
            ok, x = dispatch(a, buf, x, syl, end, step, limit, run_limit)
            if not ok:
                return False
            continue
        v13 = _PROF is not None and _PROF["metas"] == "v13"
        if a == 0x9C and not v13:           # $E378 / $E3CE: wildcard
            continue
        b = 0xA6 if (a == 0x9B and not v13) else a      # $E380 / $E3D6
        if (b + 0x40) & 0xFF != _b(buf, x):
            return False


def match_rule(pat, buf, cursor, syl, end=None):
    """$E2F0. Returns the new cursor, or None.

    `end` is $FD9E. The CONTEXT matchers stop there ($E35C and $E3B1
    both test it), but the literal run at $E305-$E310 does not -- it
    compares $FDAC,X with no limit at all. That asymmetry is what lets a
    rule reach past the end of the text into whatever the previous
    utterance left in the buffer, consume it, and step the cursor over
    $FD9E so that $E247's equality test never fires."""
    end = len(buf) if end is None else end
    n = len(pat)
    anchor = 0
    while anchor < n and not 0xA0 <= pat[anchor] <= 0xEF:
        anchor += 1
    if anchor == n:
        return None

    x, y = cursor, anchor                   # $E304-$E310: the literal run
    while y < n:
        a = pat[y]
        if a != 0xC0 and not (0xA0 <= a <= 0xEF and a == _b(buf, x)):
            break
        x += 1
        y += 1
    if y < n and 0xA0 <= pat[y] <= 0xEF:    # $E31C: a literal mismatched
        return None

    if not match_ctx(pat, anchor, buf, cursor, syl, end, True):
        return None
    if not match_ctx(pat, y - 1, buf, x - 1, syl, end, False):
        return None
    return x


def walk(word, by_letter, trace=None, prof=None):
    """$E1FA. Returns the phoneme list, or None if some character had no
    rule at all."""
    _tables(prof)
    buf = build_buffer(word)
    stop = ctx_end = len(buf)
    out, cursor, syl = [], 0, 0
    while cursor != stop and len(out) < PHONEME_CAP:
        if cursor >= len(buf):
            break
        c = buf[cursor] & 0x7F
        if c == (SENT & 0x7F):              # $E205: count it, step over it
            cursor += 1
            syl += 1
            continue
        ch = chr(c)
        if not ("0" <= ch <= "9" or "A" <= ch <= "Z"):
            syl = 0                         # $E266
        if ch not in by_letter:
            cursor += 1
            continue
        fired = None
        for pat, pho in by_letter[ch]:
            nxt = match_rule(pat, buf, cursor, syl, ctx_end)
            if nxt is not None:
                fired = (pat, pho, nxt)
                break
        if fired is None:
            if trace is not None:
                trace.append((ch, None, None))
            return None
        pat, pho, nxt = fired
        if trace is not None:
            trace.append((ch, pat, pho))
        out.extend(pho)
        cursor = nxt if nxt > cursor else cursor + 1
    return out


PUNCT = ",;?.!:"


def stress_pass(phonemes, punct):
    """The intonation marker, transcribed from $E29B-$E2EF.

    Textalker maps the punctuation to a marker -- $B7 for , ; ? and $B3
    for . ! : -- then walks BACKWARD from the end of the phoneme string,
    shifting each code one place right, until it meets one below $4E.
    That threshold is the vowel range: $4E is L and everything above it
    is a consonant. The marker goes in front of that last vowel; if none
    is found, a $7F lands at index 1, not 0 ($E2E2 stores through
    $FEAD,Y with Y already down to 1).

    This is what turns MAT. from 51 24 63 into 51 B3 24 63, which the
    write-watch first showed as a mysterious back-patch from $E2D8 and
    $E2EF. It runs BEFORE the punctuation mark is matched, which is why
    DR. carries no marker at all: the abbreviation rule swallows the
    stop, so the cursor never reaches it."""
    marker = (0xB7 if punct in ",;?" else
              0xB3 if punct in ".!:" else None)
    if marker is None or not phonemes:
        return list(phonemes)
    out = list(phonemes) + [0]
    y = len(phonemes) + 1                   # $E2CE INY
    while True:
        a = out[y - 2]                      # $E2D2: $FEAB,Y is base + y - 2
        out[y - 1] = a
        if a < 0x4E:                        # $E2D8
            out[y - 2] = marker             # $E2E9
            return out
        y -= 1
        if y < 2:                           # $E2DE CMP #$02
            break
    out[y] = 0x7F                           # $E2E2
    return out


PUNCT_TABLE = 0xEF8B     # {ptr_lo, ptr_hi, char} triples, searched backward


def punct_section(rom, ch, top, table=None, base=None):
    """$E256-$E298: the spoken names of punctuation.

    Ctrl-E A and M turn this on. It is a table of three-byte entries at
    $EF8B searched DOWNWARD from $53 (A, all) or $4A (M, most); the three
    entries between are line feed, carriage return and space, which only
    A speaks. A hit replaces the dispatch-table pointer, so "." reaches a
    rule that spells PERIOD instead of the one that makes a pause."""
    table = PUNCT_TABLE if table is None else table
    base = BASE if base is None else base
    x = top
    while x >= 2:
        if rom[(table + x) - base] == ch:
            p = rom[(table + x - 2) - base] | (rom[(table + x - 1) - base] << 8)
            return rules_at(rom, p, base)
        x -= 3
    return None


DISPATCH = 0xEFDF


def dispatch_at(rom, c, prof=None):
    """The rule section for a character load_rules did not index.

    $E216 (and v1.3's $DA09) index the table with the raw character and
    follow whatever pointer comes back. The table only has entries for
    $20-$65, so a character above that reads its "pointer" out of the
    RULES -- and the engine then parses rule bytes at wherever that
    lands. It is nonsense, but it is deterministic nonsense that reaches
    the phoneme buffer, so it has to be followed rather than rejected.
    v1.3's $7F points at $F3C5, in the allophone data, and yields
    `12 0C`; refusing to follow it drops two phonemes from the very
    first word that reads past the end of the buffer.

    Only a pointer below the image is refused: there the 6502 is reading
    main RAM, which the OBJ image says nothing about."""
    p_ = prof or (_PROF or profiles.PROFILES["3.1.3"])
    i = (p_["dispatch"] + 2 * c) - p_["base"]
    if not 0 <= i < len(rom) - 1:
        return None
    p = rom[i] | (rom[i + 1] << 8)
    if not p_["base"] <= p < p_["base"] + len(rom):
        return None
    key = (id(rom), p_["base"], p)
    sec = _DISPATCH_CACHE.get(key)
    if sec is None:
        sec = _DISPATCH_CACHE[key] = rules_at(rom, p, p_["base"], limit=256)
    return sec


_DISPATCH_CACHE = {}


def rules_at(rom, ptr, base=None, limit=6):
    """Parse rules forward from a pointer, which lands on the last
    payload byte of whatever precedes it.

    There is no section end: $DADB simply steps over the pattern and the
    payload and tries the next rule, forever, so a section runs until
    something matches. Six is enough for a real section, whose last rule
    is a catch-all; a junk pointer needs more before it finds one."""
    out, k = [], (ptr + 1) - (BASE if base is None else base)
    # A real section pointer lands on the last payload byte of whatever
    # precedes it, so ptr+1 is already a pattern byte. A junk pointer
    # can land anywhere, and $DA96's anchor search then walks forward
    # over the payload bytes it lands in without caring -- so skip them,
    # or the parse produces an empty pattern and gives up where the
    # machine happily carries on.
    while k < len(rom) and rom[k] < 0x80:
        k += 1
    for _ in range(limit):
        if k >= len(rom):
            break
        ps = k
        while k < len(rom) and rom[k] >= 0x80:
            k += 1
        pat = bytes(rom[ps:k])
        hs = k
        while k < len(rom) and rom[k] < 0x80:
            k += 1
        if not pat:
            break
        out.append((pat, bytes(rom[hs:k])))
    return out


def walk_line(text, by_letter, flags=0, prof=None):
    """$E1FA over a whole line rather than one word.

    Same loop; the only additions are that the buffer is the whole line
    ($DB5E interleaves words and separators, see syllabify.build_line)
    and that reaching a punctuation mark runs the stress pass first."""
    t = _tables(prof)
    if not t["prof"]["syllabifier"]:
        # v1.3 does no syllabification at all: $F59F just holds the text.
        buf = [0x80 | ord(c) for c in text.upper()]
        return run_buffer(buf, by_letter, flags, prof=prof) + [TERMINATOR]
    buf = syllabify.build_line(text.upper(), t)
    # walk_line is given text without its carriage return, so nothing in
    # the buffer emits the $1A terminator and it has to be supplied.
    # run_buffer is given the real buffer, where the CR's own rule
    # ({C0} => $1A, through the dispatch entry for $0D) already does.
    return run_buffer(buf, by_letter, flags, prof=prof) + [TERMINATOR]


PHONEME_CAP = 0xF0       # $E243 CPX #$F0


def run_buffer(buf, by_letter, flags=0, end=None, prof=None):
    """$E1FA over a buffer the caller already built and syllabified.

    `flags` is $FFAD. Bits 4 and 5 choose how punctuation is handled
    (Ctrl-E A, M, S), and bits 5 and 6 together SUPPRESS the stress pass
    -- $E29B opens with AND #$60 and returns if either is set. So Ctrl-E
    A and Ctrl-E L both drop the intonation markers, which is the only
    thing that separated them from the emulator once the punctuation
    names were right."""
    _tables(prof)
    rom = _TABLES["rom"]
    # $E247 compares the cursor with $FD9E for EQUALITY, so a rule that
    # consumes more characters than are left steps straight over the end
    # and the walk carries on through whatever the previous utterance
    # left in the buffer -- until $E243 stops it at 240 phonemes. That is
    # not a fault to be guarded against; it is audible, and reproducing
    # it is the point.
    stop = len(buf) if end is None else end
    ctx_end = stop
    out, cursor, syl = [], 0, 0
    while cursor != stop and len(out) < PHONEME_CAP:
        if cursor >= len(buf):
            break
        c = buf[cursor] & 0x7F
        if c == (SENT & 0x7F):
            cursor += 1
            syl += 1
            continue
        ch = chr(c)
        if not ("0" <= ch <= "9" or "A" <= ch <= "Z"):
            syl = 0
        if ch in PUNCT and not (flags & 0x60):   # $E29B
            out = stress_pass(out, ch)
        section = by_letter.get(ch)
        if section is None:
            # $E216 dispatches EVERY character, not just the printable
            # ones load_rules parses. The $80 that Ctrl-E K plants before
            # each letter masks to 0, whose entry reaches {C0} => $1A --
            # the terminator -- so each letter is spoken on its own.
            section = dispatch_at(rom, c, _PROF)
        if (flags & 0x30) and not ("0" <= ch <= "9" or "A" <= ch <= "Z"):
            alt = punct_section(
                rom, c, _PROF["punct_all"] if flags & 0x20 else _PROF["punct_most"],
                _PROF["punct_names"], _PROF["base"])
            if alt:
                section = alt
        if section is None:
            cursor += 1
            continue
        fired = None
        for pat, pho in section:
            nxt = match_rule(pat, buf, cursor, syl, ctx_end)
            if nxt is not None:
                fired = (pat, pho, nxt)
                break
        if fired is None:
            return None
        pat, pho, nxt = fired
        out.extend(pho)
        cursor = nxt if nxt > cursor else cursor + 1
    return out


def build_sections(rules):
    by = {}
    for a in sorted(rules):
        chars, pat, pho = rules[a]
        for ch in chars:                 # a section may serve several
            by.setdefault(ch, []).append((pat, pho))
    return by


def oracle_line(text, loader, obj, tmpdir):
    """The phoneme buffer the emulator ends up with, for one line.

    ECHOTALK_PHONEME_DUMP is "offset:value" in WRITE order, so a
    stress-pass shift appears as several writes to one offset and must
    be replayed, not just read off. It starts a new record at every $1A,
    and Textalker breaks long input into chunks of its own, so this
    returns a LIST of chunks; anything that wants a single answer should
    insist on getting exactly one."""
    src = _scratch(tmpdir, "walk_in.bin")
    wav = _scratch(tmpdir, "walk_out.wav")
    dump = _scratch(tmpdir, "walk_ph.txt")
    open(src, "wb").write(b"\x05T" + text.encode("ascii") + b"\r")
    env = dict(os.environ)
    env["ECHOTALK_PHONEME_DUMP"] = dump
    subprocess.run([HARNESS, loader, obj, src, wav], capture_output=True, env=env)
    out = []
    for ln in open(dump).read().splitlines():
        b = {}
        for tok in ln.split():
            if ":" in tok:
                o, v = tok.split(":")
                b[int(o, 16)] = int(v, 16)
        if b:
            out.append([b[k] for k in sorted(b)])
    return out


def split_words(codes):
    """A phoneme string cut at the $19 word separators, with $1A dropped."""
    parts, cur = [], []
    for v in codes:
        if v == TERMINATOR:
            continue
        if v == WORD_SEP:
            parts.append(cur)
            cur = []
        else:
            cur.append(v)
    parts.append(cur)
    return parts


def diff(words, by_letter, loader, obj, tmpdir, batch=6, progress=False, prof=None):
    """Compare the walker with the emulator, a batch of words per run.

    Both sides are given the SAME line, because a word's position in the
    line is not neutral: $DF3A tests an absolute buffer index, so the
    "vowel W vowel" syllable rule is disabled only for the word standing
    first. OWETEO alone is |O|WE|TE|O| and yields 21 50 26 63 36 21,
    while in "AAA OWETEO" it is |OW|E|TE|O| and yields 3C 26 63 36 21.
    Walking each word in isolation against an oracle that ran them in
    batches compares two different questions.

    Returns (ok, bad_list, unresolved)."""
    ok, bad_list, unresolved = 0, [], []
    for s in range(0, len(words), batch):
        chunk = words[s:s + batch]
        line = " ".join(chunk)
        chunks = oracle_line(line, loader, obj, tmpdir)
        if len(chunks) != 1:              # Textalker split it; retry singly
            if batch > 1:
                o, b, u = diff(chunk, by_letter, loader, obj, tmpdir, 1, prof=prof)
                ok += o
                bad_list += b
                unresolved += u
            else:
                unresolved += chunk
            continue
        want = chunks[0]
        got = walk_line(line, by_letter, prof=prof)
        if got == want:
            ok += len(chunk)
        else:
            wp = split_words(want)
            gp = split_words(got or [])
            if len(wp) == len(chunk) and len(gp) == len(chunk):
                for w, r, m in zip(chunk, wp, gp):
                    if r == m:
                        ok += 1
                    else:
                        bad_list.append((w, r, m))
            else:
                bad_list.append((line, want, got))
        if progress and (s // batch) % 20 == 0:
            sys.stderr.write("  oracle %d/%d\r" % (s, len(words)))
            sys.stderr.flush()
    if progress:
        sys.stderr.write("                          \r")
    return ok, bad_list, unresolved


def corpus_from(path, limit=None):
    seen, words = set(), []
    files = ([path] if os.path.isfile(path)
             else [os.path.join(path, f) for f in sorted(os.listdir(path))])
    for fn in files:
        txt = open(fn, "rb").read().decode("latin1").upper()
        for w in re.findall(r"[A-Z]+", txt):
            if w not in seen:
                seen.add(w)
                words.append(w)
    return words[:limit] if limit else words


STORY = os.path.join(ROOT, "tmp", "A Story with Lots of Nonsense Words.txt")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("words", nargs="*")
    ap.add_argument("--diff")
    ap.add_argument("--diff-refs", action="store_true")
    ap.add_argument("--diff-story", action="store_true")
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--version", default="3.1.3", choices=("3.1.3", "1.3"),
                    help="which Textalker image to use")
    ap.add_argument("--loader")
    ap.add_argument("--obj")
    ap.add_argument("--tmp", default=os.environ.get("TEMP", "."))
    args = ap.parse_args()

    prof = profiles.PROFILES[args.version]
    args.loader = args.loader or prof["ram"]
    args.obj = args.obj or prof["obj"]
    by_letter = build_sections(load_rules(args.obj, prof))

    words = [w.upper() for w in args.words]
    if args.diff:
        words = corpus_from(args.diff, args.limit)
    if args.diff_refs:
        words = corpus_from(os.path.join(ROOT, "reference_text"), args.limit or 300)
    if args.diff_story:
        words = corpus_from(STORY, args.limit)
    if not words:
        sys.exit("give words, or --diff FILE / --diff-refs / --diff-story")

    if not (args.diff or args.diff_refs or args.diff_story):
        for w in words:
            tr = []
            got = walk(w, by_letter, tr, prof)
            print("%s -> %s" % (w, " ".join("%02X" % x for x in got) if got else "(no match)"))
            for c, pat, pho in tr:
                print("    [%s] %-30s => %s" % (
                    c, fmt_pattern(pat) if pat else "(none)",
                    fmt_phonemes(pho) if pho else ""))
        return

    ok, bad_list, unresolved = diff(words, by_letter, args.loader, args.obj,
                                    args.tmp, progress=True, prof=prof)
    total = ok + len(bad_list)
    print("compared %d words (%d not resolved by the oracle)" % (total, len(unresolved)))
    if total:
        print("agree: %d  (%.1f%%)   disagree: %d"
              % (ok, 100.0 * ok / total, len(bad_list)))
    print()
    for w, r, m in bad_list[:25]:
        print("  %-18s oracle %-30s walker %s" % (
            w, " ".join("%02X" % x for x in r),
            " ".join("%02X" % x for x in m) if m is not None else "(no match)"))


if __name__ == "__main__":
    main()
