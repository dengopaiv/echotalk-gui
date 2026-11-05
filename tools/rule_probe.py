#!/usr/bin/env python3
"""Identify which Textalker letter-to-sound rule fires for each letter of
a word, by running the real thing under the emulator and watching it read
its own rule table.

This exists to decode the context metacharacters in the rule patterns.
Inferring them from phoneme output does not work: rules are tried in
order and a more specific earlier rule wins, hiding the class you are
trying to measure. Watching which rule the engine actually consults
answers the question directly.

How it works. The winning rule's payload is fetched at PC=$E238, one
read per phoneme, immediately before $E23D writes it into the phoneme
buffer. render_text_loader's ECHOTALK_READ_WATCH logs reads in an
address range with the issuing PC, so filtering that log to $E238 gives
the address of every rule that fired, in order.

Rules are parsed straight out of the image. Pattern bytes are all >=$80
(high-bit ASCII letters plus metacharacters); phoneme codes are all
<$80. A rule therefore ends at the high-to-low transition. Do NOT try to
split on $F3 -- that is the word-boundary metacharacter, not a
delimiter. See notes/textalker_rewrite_table_discovery.md.

Usage:
    python tools/rule_probe.py WORD [WORD ...]
    python tools/rule_probe.py --family "V?"      # ? expands over A-Z
    python tools/rule_probe.py --family "A?B" --pos 0
Options:
    --obj PATH --loader PATH   images (default: 3.1.3 in roms/)
    --pos N                    report only the rule that fired Nth
"""
import argparse
import os
import re
import subprocess
import sys
import collections


# Every scratch name carries the process id. Two of these tools running
# at once otherwise share bd_in.bin / bd_bytes.txt and each reads the
# other's oracle, which does not fail -- it just reports disagreements
# that are not there. It cost two "failing" seeds that were fine.
def _scratch(tmpdir, name):
    stem, ext = os.path.splitext(name)
    return os.path.join(tmpdir, "%s_%d%s" % (stem, os.getpid(), ext))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_H = os.path.join(ROOT, "build", "native", "render_text_loader")
HARNESS = _H + ".exe" if os.path.exists(_H + ".exe") else _H
BASE = 0xD000
# The dispatch table is one contiguous array of LE pointers indexed by
# raw ASCII: entry(c) = DISPATCH + 2*ord(c). Found by watching the
# lookup reads -- 'A' reads $F061/$F062 and '5' reads $F049/$F04A at the
# same PCs ($E21E/$E223), and $F061-$F049 is 24, twice 'A'-'5'.
# What was called "the letter table at $F061" is this same table sliced
# from 'A'; digits and punctuation have entries too and reach the rule
# sections in the tail, which is why 69105 reads as six digit names.
DISPATCH = 0xEFDF
DISPATCH_CHARS = range(0x20, 0x66)   # space (0x20) through 0x65
# The table itself occupies $F01F-$F0AA and the rules begin at $F0AB, so
# it must stop at 0x65. Reading further treats rule bytes as pointers:
# 'f' (0x66) computes to $F0AB, the start of A's rules, and several
# lowercase codes then land inside E's section and truncate it from 60
# rules to 10 -- which cost 33 points of agreement before it was caught.
RULE_LO = 0xF0AB
RULE_HI = 0xFCC4               # rules run past $FBF1; code begins at $FCC5
PAYLOAD_FETCH_PC = 0xE238      # where the winning rule's payload is read


def load_rules(obj_path, prof=None):
    """Parse the rule table. Returns {payload_address: (char, pattern, phonemes)}.

    `prof` is a tools/profiles.py entry; without one the image is
    identified from its own layout, so this works for either version."""
    import profiles
    prof = prof or profiles.detect(obj_path)
    base, dispatch = prof["base"], prof["dispatch"]
    rule_lo, rule_hi = prof["rule_lo"], prof["rule_hi"]
    d = open(obj_path, "rb").read()
    by = lambda a: d[a - base]
    ptr = lambda c: by(dispatch + 2 * c) | (by(dispatch + 2 * c + 1) << 8)

    entries = {}
    for c in DISPATCH_CHARS:
        p = ptr(c)
        if rule_lo <= p <= rule_hi:
            entries[chr(c)] = p
    if len(entries) < 26:
        raise SystemExit(
            "dispatch table at $%04X does not look right: only %d of the "
            "printable characters point into the rule area."
            % (dispatch, len(entries)))
    BASE, RULE_HI = base, rule_hi

    # Sections are delimited by the SORTED pointers, not by ASCII order:
    # the digits point into the tail past Z. As established earlier, a
    # pointer lands on the last payload byte of the preceding section, so
    # a section spans ptr+1 .. next_ptr inclusive.
    bounds = sorted(set(entries.values())) + [RULE_HI]
    nxt = {bounds[i]: bounds[i + 1] for i in range(len(bounds) - 1)}

    rules = {}
    for ch, p in entries.items():
        start = p + 1
        sec = d[start - BASE:nxt[p] - BASE + 1]
        k = 0
        while k < len(sec):
            ps = k
            while k < len(sec) and sec[k] >= 0x80:
                k += 1
            pat = sec[ps:k]
            hs = k
            while k < len(sec) and sec[k] < 0x80:
                k += 1
            if pat:
                # Several characters can share a section pointer -- ','
                # '?' and '!' all point at $FBF3 -- so accumulate the
                # characters rather than letting them overwrite each
                # other, which silently loses every section but the last.
                a = start + hs
                if a in rules:
                    prev = rules[a]
                    rules[a] = (prev[0] + ch, prev[1], prev[2])
                else:
                    rules[a] = (ch, bytes(pat), bytes(sec[hs:k]))
    return rules


def fmt_pattern(pat):
    return "".join(chr(x & 0x7F) if 0xC1 <= x <= 0xDA else "{%02X}" % x for x in pat)


def fmt_phonemes(pho):
    return " ".join("%02X" % x for x in pho)


def fire(word, rules, loader, obj, tmpdir, prof=None):
    """Return the rules that fired for `word`, in order."""
    import profiles
    prof = prof or profiles.detect(obj)
    src = _scratch(tmpdir, "probe_in.bin")
    wav = _scratch(tmpdir, "probe_out.wav")
    open(src, "wb").write(b"\x05T" + word.encode("ascii") + b"\r")
    env = dict(os.environ)
    env["ECHOTALK_READ_WATCH"] = "%04X-%04X" % (prof["rule_lo"], prof["rule_hi"])
    r = subprocess.run([HARNESS, loader, obj, src, wav],
                       capture_output=True, env=env)
    pat = (r"\[rd\] \$([0-9A-F]{4}) = [0-9A-F]{2}  PC=\$%04X"
           % prof["payload_pc"]).encode()
    out = []
    for m in re.finditer(pat, r.stderr):
        a = int(m.group(1), 16)
        if a in rules:
            out.append(rules[a])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("words", nargs="*")
    ap.add_argument("--family")
    ap.add_argument("--pos", type=int, default=None)
    ap.add_argument("--version", default="3.1.3", choices=("3.1.3", "1.3"))
    ap.add_argument("--loader")
    ap.add_argument("--obj")
    ap.add_argument("--tmp", default=os.environ.get("TEMP", "."))
    args = ap.parse_args()

    import profiles
    prof = profiles.PROFILES[args.version]
    args.loader = args.loader or prof["ram"]
    args.obj = args.obj or prof["obj"]
    if not os.path.exists(HARNESS):
        sys.exit("build build/native/render_text_loader first (make native)")
    rules = load_rules(args.obj, prof)

    words = list(args.words)
    if args.family:
        i = args.family.index("?")
        words += [args.family[:i] + c + args.family[i + 1:]
                  for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"]

    if args.family and args.pos is not None:
        groups = collections.defaultdict(list)
        for w in words:
            f = fire(w, rules, args.loader, args.obj, args.tmp, prof)
            key = fmt_pattern(f[args.pos][1]) + "  => " + fmt_phonemes(f[args.pos][2]) \
                if args.pos < len(f) else "(none)"
            groups[key].append(w[args.family.index("?")])
        for k, v in sorted(groups.items(), key=lambda kv: -len(kv[1])):
            print("  %-34s <- %s" % (k, "".join(v)))
        return

    for w in words:
        print("%s:" % w)
        for letter, pat, pho in fire(w, rules, args.loader, args.obj, args.tmp, prof):
            print("    [%s] %-30s => %s" % (letter, fmt_pattern(pat), fmt_phonemes(pho)))


if __name__ == "__main__":
    main()
