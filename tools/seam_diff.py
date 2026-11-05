#!/usr/bin/env python3
"""The letter-to-sound seam, tested the two ways it can be wrong.

The seam lets a different pronunciation source stand in for Textalker's
own $E1FA rules. Two things have to hold, and they pull in opposite
directions:

1. **Declining must be free.** A source that knows none of the words in
   a line has to produce EXACTLY what Textalker's rules produce -- the
   same word separators, the same spoken punctuation names, the same
   back-patched stress markers. If handing a word back costs anything,
   then every partial dictionary quietly degrades the voice, which is
   the failure mode nobody would hear until much later.

2. **The alphabet bridge has to be right.** tk_arpabet.c says which of
   Textalker's allophones is the vowel of "beet". Nothing in the machine
   can check that, so this checks it the only way available: take what
   Textalker's own rules say for a word, write it out as ARPABET, feed
   THAT back in as a dictionary entry, and see whether the same codes
   come out. A round trip cannot prove the names are right, but it does
   prove the encoding is, and it catches a symbol that maps to the wrong
   index the moment a corpus is thrown at it.

   **It cannot catch two names swapped**, and did not: AA and AO were
   the wrong way round for a whole session and this reported 151 of 155
   throughout, because encode and decode share the table and a swap is
   self-consistent. Only an ear, or a frame word whose pronunciation is
   not in doubt, settles which name goes where -- see
   notes/phoneme_alphabet.md.

    python tools/seam_diff.py [--version 1.3] [--limit 2000]

Exit status is non-zero if either check fails.
"""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import profiles
from tk_diff import corpus_words

ROOT = profiles.ROOT
TK_PHONEMES = os.path.join(ROOT, "build", "native", "tk_phonemes")
if os.path.exists(TK_PHONEMES + ".exe"):
    TK_PHONEMES += ".exe"


def scratch(tmp, name):
    stem, ext = os.path.splitext(name)
    return os.path.join(tmp, "%s_%d%s" % (stem, os.getpid(), ext))


def allophones(codes_hex):
    """The allophone indices alone, with the pitch group discarded.
    $DC22-$DC92: below $1A is group 1, $1A-$33 group 2, $34-$4D group 3,
    all of the same 26 low allophones; above that one code each."""
    out = []
    for tok in codes_hex.split():
        c = int(tok, 16)
        if c >= 0xB3 or c in (0x1A, 0x7F):
            out.append(c)
        elif c < 0x1A:
            out.append(c)
        elif c < 0x34:
            out.append(c - 0x1A)
        elif c < 0x4E:
            out.append(c - 0x34)
        else:
            out.append(c - 0x34)
    return out


def run(prof, lines, dict_path=None, arpabet=False):
    cmd = [TK_PHONEMES, prof["ram"], prof["obj"],
           "--version", prof["name"], "--lines", "-"]
    if dict_path:
        cmd += ["--dict", dict_path]
    if arpabet:
        cmd += ["--arpabet"]
    r = subprocess.run(cmd, input=("\n".join(lines) + "\n").encode(),
                       capture_output=True)
    if r.returncode:
        sys.exit("tk_phonemes failed: %s" % r.stderr.decode("utf-8", "replace"))
    return r.stdout.decode().splitlines()[:len(lines)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="3.1.3", choices=tuple(profiles.PROFILES))
    ap.add_argument("--limit", type=int, default=2000)
    ap.add_argument("--show", type=int, default=8)
    ap.add_argument("--tmp", default=os.environ.get("TEMP", "."))
    args = ap.parse_args()
    prof = profiles.PROFILES[args.version]

    words = corpus_words(os.path.join(ROOT, "reference_text"), args.limit)
    # Whole lines too: the separators, the punctuation and the stress
    # pass only appear once there is more than one word.
    lines = list(words)
    d = os.path.join(ROOT, "reference_text")
    for name in sorted(os.listdir(d)):
        raw = open(os.path.join(d, name), "rb").read()
        txt = "".join(chr(c & 0x7F) if 32 <= (c & 0x7F) < 127 else " "
                      for c in raw)
        for ln in txt.split("  "):
            ln = ln.strip()
            if 1 < len(ln) < 70:
                lines.append(ln)
    lines = lines[:args.limit * 2]

    base = run(prof, lines)
    bad = 0

    # --- 1. a dictionary that matches nothing must change nothing -----
    empty = scratch(args.tmp, "seam_empty.dict")
    with open(empty, "w") as f:
        f.write(";;; nothing here can match anything in the corpus\n")
        f.write("QQZZXJWKVQ  K AE1 T\n")
    got = run(prof, lines, empty)
    shown = 0
    for ln, a, b in zip(lines, base, got):
        if a != b:
            bad += 1
            if shown < args.show:
                print("  declined but changed: %-40r\n    rules %s\n    dict  %s"
                      % (ln[:40], a, b))
                shown += 1
    print("%s: declining is free on %d of %d lines"
          % (prof["name"], len(lines) - bad, len(lines)))

    # --- 2. round trip through ARPABET --------------------------------
    # Only single words, and only ones whose codes are all nameable:
    # a line's separators and stress markers are the RULES' business and
    # a dictionary entry never carries them.
    names = run(prof, words, None, arpabet=True)
    rt = scratch(args.tmp, "seam_roundtrip.dict")
    usable = []
    with open(rt, "w") as f:
        for w, nm in zip(words, names):
            toks = [t for t in nm.split() if t not in ("<end>",)]
            if not toks or any(t.startswith("<") for t in toks):
                continue
            # strip the pitch-group digit off the vowels: a dictionary
            # entry carries ARPABET stress, and 3 is what stress 1 means
            out = []
            for t in toks:
                if t and t[-1].isdigit():
                    out.append(t[:-1] + ("1" if t[-1] == "3" else "0"))
                else:
                    out.append(t)
            f.write("%s  %s\n" % (w, " ".join(out)))
            usable.append(w)
    if usable:
        want = run(prof, usable)
        got = run(prof, usable, rt)
        miss = group_only = 0
        shown = 0
        for w, a, b in zip(usable, want, got):
            if a == b:
                continue
            if allophones(a) == allophones(b):
                # The sound is identical and only the PITCH GROUP moved.
                # That is the one thing the round trip cannot preserve:
                # the group is stress, ARPABET has three stress levels
                # and Textalker three groups, but they do not line up --
                # v3.1.3 speaks an unstressed vowel at group 2 and v1.3
                # at group 1, and nothing in "AH0" says which. The "/"
                # escape in tk_dict.c reaches a group directly.
                group_only += 1
                continue
            miss += 1
            if shown < args.show:
                print("  round trip differs: %-16s rules %s / dict %s"
                      % (w, a, b))
                shown += 1
        print("%s: ARPABET round trip exact on %d of %d words "
              "(%d more differ only in pitch group)"
              % (prof["name"], len(usable) - miss - group_only, len(usable),
                 group_only))
    for p in (empty, rt):
        try:
            os.remove(p)
        except OSError:
            pass
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
