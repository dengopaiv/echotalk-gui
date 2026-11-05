#!/usr/bin/env python3
"""Textalker's syllabifier, ported from the 6502, plus its oracle.

The rule engine cannot be reproduced without this. Textalker splits a
word into syllables *before* matching any letter-to-sound rule, and
records the boundaries as $E6 sentinel bytes in the input buffer at
$FDAC. Those sentinels are not bookkeeping: they are real characters
that the rule matcher walks over, and $E6 masks to $66, whose entry in
the attribute table at $EF24 is $00 -- neither vowel nor consonant nor
separator -- so a sentinel blocks every class. Nineteen rules test for
one directly (GHO{E6}TI, {F3}GE{E6}ORGE{F3}, {F3}DR{E6}{AE}), and far
more depend on where they fall.

**This is a transcription of $DD10-$E126, not a model of it.** It builds
the same buffer the 6502 builds, using the same variables:

    $FD9E  end       one past the last character
    $FDA9  last      the last character the vowel loop may consider
    $FDA8  bound     the most recent boundary; scanning resumes after it
    $FDAA  v1        first vowel      $FDAB  v2  second vowel
    $FDA6  start     $FDA5  limit     $FDA7  hit    $FDA4  hitlen

    $DD10  set up, leading sentinel, the leading-Y rule, prefix, suffix
    $DD30  loop: find two vowels, the E cases, then split between them
    $DD65  prefix test ($E127), only for words of 7 characters or more
    $DD98  suffix test ($E141), which LOOPS -- CARE|FUL|LY strips two
    $DE4C  find the next two vowels        $E0C9  scan forward for one
    $DE7A  the E cases: -LE/-LED/-LES, -ES, -ED
    $DF35  adjacent vowels: QU, the digraph list at $E192, -ING, -IE
    $DFFD  consonants between: one splits V|CV (R and X split VC|V),
           several are cut by the cluster list at $E1B1
    $E06A  the list matcher     $E0F8  insert a sentinel at X

Every table is read out of the user's own image rather than hardcoded,
as everything else in this project is. Entries are concatenated with the
high bit set on each entry's last character; a $00 ends a list.

Usage:
    python tools/syllabify.py WORD [WORD ...]
    python tools/syllabify.py --tables
    python tools/syllabify.py --diff-story [--limit N]
"""
import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rule_probe import HARNESS


# Every scratch name carries the process id. Two of these tools running
# at once otherwise share bd_in.bin / bd_bytes.txt and each reads the
# other's oracle, which does not fail -- it just reports disagreements
# that are not there. It cost two "failing" seeds that were fine.
def _scratch(tmpdir, name):
    stem, ext = os.path.splitext(name)
    return os.path.join(tmpdir, "%s_%d%s" % (stem, os.getpid(), ext))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0xD000
ATTR = 0xEF24          # 128-byte character attribute table
PREFIXES = 0xE127      # $DD70
SUFFIXES = 0xE141      # $DDA3
DIGRAPHS = 0xE192      # $DF7F
CLUSTERS = 0xE1B1      # $E031
VOWEL_BIT = 0x80
SENT = 0xE6
STORY = os.path.join(ROOT, "tmp", "A Story with Lots of Nonsense Words.txt")

L = dict((c, 0x80 | ord(c)) for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ")


def read_list(d, addr):
    """The raw bytes of one $00-terminated list, and its entries."""
    i = addr - BASE
    raw, words, w = [], [], ""
    while d[i]:
        raw.append(d[i])
        w += chr(d[i] & 0x7F)
        if d[i] & 0x80:
            words.append(w)
            w = ""
        i += 1
    raw.append(0)
    return bytes(raw), words


def load_tables(obj_path):
    d = open(obj_path, "rb").read()
    t = {"attrs": d[ATTR - BASE:ATTR - BASE + 128], "rom": d}
    for name, addr in (("prefixes", PREFIXES), ("suffixes", SUFFIXES),
                       ("digraphs", DIGRAPHS), ("clusters", CLUSTERS)):
        t[name + "_raw"], t[name] = read_list(d, addr)
    return t


# ------------------------------------------------------------------ buffer

def _g(buf, x):
    return buf[x] if 0 <= x < len(buf) else 0x00


def _attr(t, buf, x):
    return t["attrs"][_g(buf, x) & 0x7F]


def _insert(buf, v, x):
    """$E0F8: shift the tail right and drop a sentinel in at x.

    On a fixed-size buffer -- the real $FDAC, which is 256 bytes that
    nothing ever clears -- $E0FB walks down from $FD9E, so only the
    region up to the current end moves. Anything beyond it is left
    where it is, which is exactly how a previous utterance's tail stays
    readable."""
    if v["fixed"]:
        for i in range(min(v["end"], len(buf) - 2), x - 1, -1):
            buf[i + 1] = buf[i]
        buf[x] = SENT
    else:
        buf.insert(x, SENT)
    v["end"] += 1
    v["last"] += 1


def _list_match(buf, v, table):
    """$E06A: find the first list entry occurring in buf[start..limit].

    Sets v["hit"] to the entry's start index, 0 for no match, and
    v["hitlen"] to its length."""
    v["hit"] = 0
    i = 0
    while table[i]:
        x, y = v["start"], 0
        while True:
            a = (_g(buf, x) & 0x7F) ^ table[i + y]
            if a == 0:                          # $E08E: a non-final char
                y += 1
                if x >= v["limit"]:
                    break
                x += 1
                continue
            if a == 0x80:                       # $E098: the entry's last char
                y += 1
                v["hitlen"] = y
                v["hit"] = x - y + 1
                return
            if x >= v["limit"]:                 # $E081
                break
            if y:                               # $E086: restart here
                y = 0
                continue
            x += 1
        while table[i] < 0x80:                  # $E0AA: skip to the next entry
            i += 1
        i += 1


def _scan_vowel(t, buf, v):
    """$E0C9: the first vowel in buf[start..limit], or 0."""
    v["hit"] = 0
    x = v["start"]
    v["limit"] += 1
    while True:
        if _attr(t, buf, x) & VOWEL_BIT:
            v["hit"] = x
            break
        x += 1
        if x == v["end"] or x >= v["limit"]:
            v["hit"] = 0
            break
    v["limit"] -= 1


def _two_vowels(t, buf, v):
    """$DE4C."""
    v["v1"] = v["v2"] = 0
    v["start"] = v["bound"] + 1
    v["limit"] = v["last"]
    _scan_vowel(t, buf, v)
    if not v["hit"]:
        return
    v["v1"] = v["hit"]
    v["start"] = v["hit"] + 1
    v["limit"] = v["last"]
    _scan_vowel(t, buf, v)
    v["v2"] = v["hit"]


# ------------------------------------------------------------------ stages

def _prefix(t, buf, v):
    """$DD65: strip a prefix, but only from a word of 7 or more."""
    if v["end"] - v["start"] < 7:
        return
    v["limit"] = v["start"] + 4
    _list_match(buf, v, t["prefixes_raw"])
    if v["hit"] != v["start"]:                  # must match at the very start
        return
    v["bound"] = v["hit"] + v["hitlen"]
    _insert(buf, v, v["bound"])


def _suffix(t, buf, v):
    """$DD98: strip suffixes, one after another.

    Each match pulls "last" back in front of the suffix, so the next
    search runs on what is left -- which is how CAREFULLY becomes
    CARE|FUL|LY in two passes. Three shapes end the search early:

      - an E just before the suffix is kept with the stem and the loop
        stops ($DDFE): ABSOLUTE|LY, PURE|LY, PEACE|FUL
      - -LY after anything but S, L or G takes no boundary at all
        ($DE10), which is why PROBABLY is left to the cluster rule and
        comes out PRO|BA|BLY rather than PRO|BAB|LY
      - a suffix starting immediately after the word start or a prefix
        boundary is refused ($DE3A)
    """
    if v["end"] - v["start"] < 6:
        return
    while True:
        a = v["last"] - 5
        v["start"] = a if a >= v["bound"] else v["bound"] + 1
        v["limit"] = v["last"]
        _list_match(buf, v, t["suffixes_raw"])
        if v["hit"] < 3:
            return
        p, n = v["hit"], v["hitlen"]
        x = v["last"] - n
        if x == p:                              # $DDEB: one character over
            if _g(buf, v["last"]) != L["S"]:    # which must be a plural S
                return
        elif x + 1 != p:                        # $DDE2: must end at "last"
            return
        if _g(buf, p - 1) == L["E"]:            # $DDFE
            v["last"] = p - 3
            _insert(buf, v, p)                  # ...which puts it at p - 2
            return
        if _g(buf, p) == L["L"] and _g(buf, p + 1) == L["Y"]:       # $DE10
            if _g(buf, p - 1) not in (L["S"], L["L"], L["G"]):
                return
        if p - 1 == v["bound"]:                 # $DE3A
            return
        v["last"] = p - 2
        _insert(buf, v, p)                      # ...which puts last at p - 1


def _e_cases(buf, v):
    """$DE7A: the three inflections, when the second vowel is an E
    standing last or second-to-last in the word.

    Each asks a different question about the consonant before the E, and
    they are the /Iz/ plural and the /Id/ past tense -- the same
    allomorphy the rule engine draws again in its three -ED rules:

      -LE -LED -LES  break off the consonant BEFORE the L, unless that
                     is another L or is the previous vowel: PUZ|ZLE and
                     HAN|DLES, against ROCHELLE and A|BLE
      -ES            a nucleus after G, S, C or the SH/CH digraph:
                     PLA|CES, NUR|SES, STA|GES, WI|SHES; silent
                     otherwise, as in ROOM|MATES and CON|CLUDES
      -ED            a nucleus after D, T, or the RD and ST clusters:
                     NOD|DED, SOR|TED, RE|STED; silent otherwise, which
                     is why HOPED and SMELLED stay whole

    Whenever one applies it ends the vowel loop, by leaving "last"
    below v2 ($DF2D, tested at $DD3B)."""
    v2 = v["v2"]
    if _g(buf, v2) != L["E"] or v2 + 2 < v["end"]:
        return
    b = _g(buf, v2 - 1)

    def cut(x):
        v["bound"] = x
        _insert(buf, v, x)

    if b == L["L"]:                                             # $DE8F
        if v2 + 1 == v["end"] or _g(buf, v2 + 1) in (L["D"], L["S"]):
            if _g(buf, v2 - 2) != L["L"] and v2 - 2 != v["v1"]:  # $DEA8
                cut(v2 - 2)
            v["last"] = v["v2"] - 1
        return
    if v2 + 1 == v["end"]:                                      # $DEBD
        v["last"] = v2 - 1                                      # a silent E
        return
    a = _g(buf, v2 + 1)
    if a == L["S"]:                                             # $DECA
        if b in (L["G"], L["S"], L["C"]):
            cut(v2 - 1)
        elif b == L["H"] and _g(buf, v2 - 2) in (L["S"], L["C"]):
            cut(v2 - 2)
        v["last"] = v["v2"] - 1
        return
    if a != L["D"]:                                             # $DF34
        return
    if b == L["D"]:                                             # $DEF1
        cut(v2 - 1)
    elif b == L["R"]:
        if _g(buf, v2 - 2) == L["D"]:                           # $DF0E
            cut(v2 - 2)
    elif b == L["T"]:                                           # $DF17
        cut(v2 - 2 if _g(buf, v2 - 2) == L["S"] else v2 - 1)
    v["last"] = v["v2"] - 1


def _adjacent(t, buf, v):
    """$DF35: the vowel-pair cases.

    Reached for every pair, but does nothing unless the two vowels are
    adjacent -- except for the one shape it checks first, a W between
    them followed by I or E, which always splits: VOW|ELS, SHOW|ER."""
    v1, v2 = v["v1"], v["v2"]

    def cut_at_v2():
        v["bound"] = v2
        _insert(buf, v, v2)

    if v1 != 1 and _g(buf, v1 + 1) == L["W"] and v1 + 2 == v2 \
            and _g(buf, v2) in (L["I"], L["E"]):
        cut_at_v2()                                             # $DF51/$DF55
        return
    if v1 + 1 != v2:                                            # $DF5E
        return
    if _g(buf, v1) == L["U"] and _g(buf, v1 - 1) == L["Q"]:      # $DF66
        v["bound"] = v1                                         # QU is one
        return
    v["start"], v["limit"] = v1, v2
    _list_match(buf, v, t["digraphs_raw"])
    if not v["hit"]:                                            # $DF95
        cut_at_v2()
        return
    # A digraph. It normally stays whole, but two shapes still split.
    if (_g(buf, v2) == L["I"] and _g(buf, v2 + 1) == L["N"]
            and _g(buf, v2 + 2) == L["G"] and v2 + 2 < v["end"]):
        cut_at_v2()                                             # GO|ING
        return
    if _g(buf, v1) == L["I"] and _g(buf, v1 + 1) == L["E"]:      # $DFB6
        a = _g(buf, v2 + 1)
        if a == L["R"] and _g(buf, v2 + 2) != L["C"]:           # EAS|I|ER
            cut_at_v2()
            return
        if a == L["S"] and _g(buf, v2 + 2) == L["T"]:           # EAS|I|EST
            cut_at_v2()
            return
    v["bound"] = v1                                             # $DFEA


def _between(t, buf, v):
    """$DFFD: divide the consonants between two vowels.

    One consonant splits V|CV, except R and X which split VC|V -- that
    is SE|VEN against VER|Y and MAR|GAR|ET. For a longer run, walk
    forward looking for a cluster from the list at $E1B1 that fills the
    window exactly; the cluster goes with the following vowel, so
    DEE|PLY and SHAR|PLY, and with no match the last consonant goes with
    it, so DOC|TOR."""
    v1, v2 = v["v1"], v["v2"]

    def cut(x):
        v["bound"] = x
        _insert(buf, v, x)

    if v1 + 2 == v2:                                            # $E002
        c = _g(buf, v1 + 1)
        cut(v2 if c in (L["R"], L["X"]) else v1 + 1)
        return
    v["start"] = v1 + 1                                         # $E019
    while True:
        v["limit"] = v["start"] + 2
        if v["limit"] == v2:                                    # $E025
            v["limit"] -= 1
        _list_match(buf, v, t["clusters_raw"])
        if (v["hit"] == v["start"]
                and v["limit"] - v["start"] + 1 == v["hitlen"]):
            cut(v["start"])                                     # $E04E
            return
        x = v["start"] + 1
        if x == v["limit"]:                                     # $E058
            cut(x)
            return
        v["start"] = x


def build_at(buf, t, start, end=None):
    """$DD10, syllabifying buf[start:] in place inside the line buffer.

    The offset matters: $DF3A tests CPX #$01, an ABSOLUTE buffer index,
    so the "vowel W vowel" rule at $DF35 is disabled only for a word
    standing first on the line. OWETEO alone is |O|WE|TE|O| but
    "AAA OWETEO" gives |OW|E|TE|O|, and the two syllabifications lead to
    different rules and different phonemes."""
    n = len(buf) if end is None else end
    v = {"end": n, "last": n - 1, "bound": start, "fixed": end is not None,
         "start": start, "limit": 0, "hit": 0, "hitlen": 0, "v1": 0, "v2": 0}
    _insert(buf, v, start)                      # $DD17
    v["start"] = start + 1
    if _g(buf, start + 1) == L["Y"]:            # $DD20: a leading Y is a
        v["bound"] = start + 1                  # consonant, not a nucleus
    _prefix(t, buf, v)
    _suffix(t, buf, v)
    while True:                                 # $DD30
        _two_vowels(t, buf, v)
        if not v["v2"]:
            break
        _e_cases(buf, v)
        if v["last"] < v["v2"]:                 # $DD3B
            break
        _adjacent(t, buf, v)
        if v["bound"] >= v["v1"]:               # $DD46
            continue
        _between(t, buf, v)
    _insert(buf, v, v["end"])                   # $DD54: the trailing sentinel
    return v["end"]


def build(word, t):
    """One word, as if it stood alone on the line."""
    buf = [0x80 | ord(c) for c in word.upper()]
    build_at(buf, t, 0)
    return buf


def build_line(text, t):
    """The whole line, the way $DB5E and $DD10 build it together.

    The buffer is not one word: the scanner copies a word in, runs the
    syllabifier over it in place, then copies the next separator and
    word after it. So "THE CAT." becomes |THE| |CAT|. -- and the "."
    sits in the same buffer as CAT, which is what lets {F3}DR{E6}{AE}
    match "DR." across the trailing sentinel.

    A word is a run of letters, digits and apostrophes; measured against
    the emulator's own buffer, every other character stands alone:
    AB1CD and AB'CD are one word each, while AB-CD, AB@CD, AB/CD and
    AB$CD are three."""
    out, i, n = [], 0, len(text)
    while i < n:
        if text[i].isalnum() or text[i] == "'":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "'"):
                j += 1
            start = len(out)
            out.extend(0x80 | ord(c) for c in text[i:j])
            build_at(out, t, start)             # in place, at its real offset
            i = j
        else:
            out.append(0x80 | ord(text[i]))
            i += 1
    return out


def syllabify(word, t):
    """The boundary offsets: k means "before word[k]"."""
    out, k = set(), 0
    for b in build(word, t):
        if b == SENT:
            out.add(k)
        else:
            k += 1
    return out


# ------------------------------------------------------------------- oracle

def oracle(word, loader, obj, tmpdir):
    """Boundary offsets, read out of the emulator's own buffer."""
    src = _scratch(tmpdir, "syl.bin")
    open(src, "wb").write(b"\x05T" + word.encode() + b"\r")
    env = dict(os.environ)
    env["ECHOTALK_RAM_WATCH"] = "FDAC-FDD0"
    r = subprocess.run([HARNESS, loader, obj, src, _scratch(tmpdir, "syl.wav")],
                       capture_output=True, env=env)
    buf = {}
    for m in re.finditer(rb"\[ram\] \$([0-9A-F]{4}) <- ([0-9A-F]{2})", r.stderr):
        buf[int(m.group(1), 16)] = int(m.group(2), 16)
    out, letters = set(), 0
    a = 0xFDAC
    while a in buf and buf[a] not in (0x00, 0x8D):
        if buf[a] == SENT:
            out.add(letters)
        else:
            letters += 1
        a += 1
    return out


def show(word, bounds):
    w = word.upper()
    return "".join(("|" if k in bounds else "") + w[k] for k in range(len(w))) + \
           ("|" if len(w) in bounds else "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("words", nargs="*")
    ap.add_argument("--tables", action="store_true")
    ap.add_argument("--diff-story", action="store_true")
    ap.add_argument("--limit", type=int, default=200)
    ap.add_argument("--loader", default=os.path.join(ROOT, "roms", "textalker.ram.bin"))
    ap.add_argument("--obj", default=os.path.join(ROOT, "roms", "textalker.obj.bin"))
    ap.add_argument("--tmp", default=os.environ.get("TEMP", "."))
    args = ap.parse_args()

    t = load_tables(args.obj)
    if args.tables:
        for k in ("prefixes", "suffixes", "digraphs", "clusters"):
            print("%-10s %s" % (k, " ".join(t[k])))
        return

    words = [w.upper() for w in args.words]
    if args.diff_story:
        seen = set()
        txt = open(STORY, "rb").read().decode("latin1").upper()
        for w in re.findall(r"[A-Z]+", txt):
            if w not in seen:
                seen.add(w)
                words.append(w)
        words = words[:args.limit]

    if not words:
        sys.exit("give words, --tables, or --diff-story")

    ok = bad = 0
    wrong = []
    for w in words:
        want = oracle(w, args.loader, args.obj, args.tmp)
        got = syllabify(w, t)
        if want == got:
            ok += 1
        else:
            bad += 1
            wrong.append((w, want, got))
        if not args.diff_story:
            print("  %-16s oracle %-22s port %s" % (w, show(w, want), show(w, got)))
    if args.diff_story:
        print("compared %d words" % (ok + bad))
        print("agree: %d (%.1f%%)   disagree: %d" % (ok, 100.0 * ok / max(1, ok + bad), bad))
        print()
        for w, want, got in wrong[:25]:
            print("  %-16s oracle %-24s port %s" % (w, show(w, want), show(w, got)))


if __name__ == "__main__":
    main()
