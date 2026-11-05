#!/usr/bin/env python3
"""Differential test of the C port, at the phonemes or the chip bytes.

Two oracles, and both matter:

    --python   (default) against tools/rule_walk.py, which is already
               byte-exact against the emulation. Fast -- no 6502 runs at
               all -- so this is the one to iterate on.
    --emulator against the real thing through ECHOTALK_PHONEME_DUMP.
               Slow, and the acceptance test: "byte-exact against the
               emulation" is what this project means by correct, and a
               Python bug shared with the C would pass --python happily.

--chip runs both stages and compares checkpoint 3, the end of the
pipeline; without it the comparison is at checkpoint 1, the phoneme
codes. --vfuzz feeds random Ctrl-V phoneme strings instead of text.

The C side runs ONCE for the whole corpus, in the tool's batch mode. A process per line would make the port look slower than the
emulator it replaces.

    python tools/tk_diff.py --refs --story --fuzz 2000 [--version 1.3]
    python tools/tk_diff.py --lines --chip --emulator
    python tools/tk_diff.py --vfuzz 400 --chip
"""
import argparse
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import profiles
import rule_walk as RW
from rule_probe import load_rules, HARNESS

ROOT = profiles.ROOT
def _exe(name):
    p = os.path.join(ROOT, "build", "native", name)
    return p + ".exe" if os.path.exists(p + ".exe") else p


NATIVE = _exe("tk_phonemes")
NATIVE_CHIP = _exe("tk_chip")


def _scratch(tmpdir, name):
    stem, ext = os.path.splitext(name)
    return os.path.join(tmpdir, "%s_%d%s" % (stem, os.getpid(), ext))


def native(lines, prof, tmpdir, flags=0x09, chip=False, mode="--lines"):
    """The C port's answer for every line, in one run."""
    src = _scratch(tmpdir, "tk_in.txt")
    exe = NATIVE_CHIP if chip else NATIVE
    with open(src, "w", encoding="ascii", errors="replace", newline="\n") as f:
        for ln in lines:
            f.write(ln + "\n")
    r = subprocess.run([exe, prof["ram"], prof["obj"],
                        "--version", prof["name"], "--flags", str(flags),
                        mode, src],
                       capture_output=True)
    if r.returncode:
        sys.exit("%s failed: %s" % (os.path.basename(exe),
                                    r.stderr.decode("utf-8", "replace")))
    out = []
    for ln in r.stdout.decode("ascii").splitlines():
        out.append(None if ln.strip() == "NORULE"
                   else [int(x, 16) for x in ln.split()])
    if len(out) != len(lines):
        sys.exit("%s returned %d lines for %d inputs"
                 % (os.path.basename(exe), len(out), len(lines)))
    return out


def emulator(text, prof, tmpdir):
    """The phoneme string the emulator builds, or None if it chunked."""
    src = _scratch(tmpdir, "tk_or.bin")
    wav = _scratch(tmpdir, "tk_or.wav")
    dump = _scratch(tmpdir, "tk_or.txt")
    open(src, "wb").write(b"\x05T" + text.encode("ascii") + b"\r")
    env = dict(os.environ)
    env["ECHOTALK_PHONEME_DUMP"] = dump
    subprocess.run([HARNESS, prof["ram"], prof["obj"], src, wav],
                   capture_output=True, env=env)
    out = []
    for ln in open(dump).read().splitlines():
        b = {}
        for t in ln.split():
            if ":" in t:
                o, v = t.split(":")
                b[int(o, 16)] = int(v, 16)
        if b:
            out.append([b[k] for k in sorted(b)])
    return out[0] if len(out) == 1 else None


def escape(s):
    """A python-escaped byte string, for tk_chip --raw."""
    out = []
    for ch in s:
        c = ord(ch)
        if c == 92:
            out.append(chr(92) * 2)
        elif 32 <= c < 127:
            out.append(ch)
        else:
            out.append("%sx%02x" % (chr(92), c))
    return "".join(out)


def corpus_words(path, limit):
    seen, out = set(), []
    for name in sorted(os.listdir(path)):
        raw = open(os.path.join(path, name), "rb").read()
        txt = "".join(chr(c & 0x7F) if 32 <= (c & 0x7F) < 127 else " "
                      for c in raw)
        for w in txt.split():
            w = "".join(ch for ch in w if ch.isalpha()).upper()
            if w and w not in seen:
                seen.add(w)
                out.append(w)
            if limit and len(out) >= limit:
                return out
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--refs", action="store_true")
    ap.add_argument("--story", action="store_true")
    ap.add_argument("--lines", action="store_true",
                    help="the story cut into single-chunk lines")
    ap.add_argument("--fuzz", type=int, default=0)
    ap.add_argument("--vfuzz", type=int, default=0,
                    help="random Ctrl-V phoneme strings; implies --chip")
    ap.add_argument("--cmdfuzz", type=int, default=0,
                    help="random Ctrl-E command streams through the whole "
                         "machine; implies --chip")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--uniform", action="store_true")
    ap.add_argument("--chip", action="store_true",
                    help="compare the chip bytes, not the phoneme codes")
    ap.add_argument("--emulator", action="store_true",
                    help="compare against the 6502, not against the Python")
    ap.add_argument("--version", default="3.1.3", choices=tuple(profiles.PROFILES))
    # Any loader/OBJ pair, without a profiles entry. The C port finds
    # its own tables and the emulator needs none, so --emulator --chip
    # works on a build nobody has catalogued; only the --python
    # comparisons need the addresses this overrides the paths of.
    ap.add_argument("--images", nargs=2, metavar=("LOADER", "OBJ"),
                    help="use these images instead of the version's own")
    ap.add_argument("--width", type=int, default=38)
    ap.add_argument("--limit", type=int)
    ap.add_argument("--show", type=int, default=8)
    ap.add_argument("--tmp", default=os.environ.get("TEMP", "."))
    args = ap.parse_args()

    prof = dict(profiles.PROFILES[args.version])
    if args.images:
        prof["ram"], prof["obj"] = args.images
    cases = []
    if args.refs:
        cases += corpus_words(os.path.join(ROOT, "reference_text"),
                              args.limit or 300)
    if args.story:
        import line_diff
        seen = set()
        for piece in line_diff.pieces(
                os.path.join(ROOT, "tmp",
                             "A Story with Lots of Nonsense Words.txt"), 200):
            for w in piece.split():
                w = "".join(c for c in w if c.isalpha()).upper()
                if w and w not in seen:
                    seen.add(w)
                    cases.append(w)
    if args.lines:
        import line_diff
        got = list(line_diff.pieces(
            os.path.join(ROOT, "tmp",
                         "A Story with Lots of Nonsense Words.txt"), args.width))
        cases += got[:args.limit] if args.limit else got
    if args.fuzz:
        random.seed(args.seed)
        FREQ = ("ABCDEFGHIJKLMNOPQRSTUVWXYZ" if args.uniform else
                "EEEEEEEEEEEETTTTTTTTTAAAAAAAAOOOOOOOIIIIIIINNNNNNNSSSSSSHHHHHH"
                "RRRRRRDDDDLLLLCCCUUMMWWFFGGYYPPBBVKJXQZ")
        words = set()
        while len(words) < args.fuzz:
            n = random.choice([1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18])
            words.add("".join(random.choice(FREQ) for _ in range(n)))
        cases += sorted(words)
    if args.vfuzz:
        random.seed(args.seed)
        import byte_diff
        for _ in range(args.vfuzz):
            n = random.randint(1, 6)
            cases.append("".join(random.choice(byte_diff.PHONEME_CHARS)
                                 for _ in range(n)))
        args.chip = True
    if args.cmdfuzz:
        random.seed(args.seed)
        E, CR = chr(5), chr(13)
        letters = "EEEETTTAAAOOOIIINNSSHHRRDLCUMWFGYPBVKJXQZ   ,."
        # Only the commands this version actually has: v1.3 is missing
        # D, K, N, R and the ";,I?" index marks.
        cmds = [c for c in "AMSCELWKNTBPFVDRO" if c in prof["commands"]]
        for _ in range(args.cmdfuzz):
            out = [E + "T"]
            for _ in range(random.randint(1, 5)):
                d = random.choice(["", "", str(random.randint(0, 63))])
                out.append(E + d + random.choice(cmds))
                out.append("".join(random.choice(letters)
                                   for _ in range(random.randint(1, 12))))
            cases.append("".join(out) + CR)
        args.chip = True
    if not cases:
        sys.exit("give --refs, --story, --lines, --fuzz, --vfuzz or --cmdfuzz")

    vmode = bool(args.vfuzz)
    rawmode = bool(args.cmdfuzz)
    if rawmode:
        mode = "--raw"
        feed = [escape(c) for c in cases]
    elif vmode:
        mode = "--phonemes"
        feed = cases
    else:
        mode = "--lines"
        feed = cases
    got = native(feed, prof, args.tmp, chip=args.chip, mode=mode)

    by = None
    if not args.emulator and not rawmode:
        import allophone
        t2 = allophone.load_tables(prof["obj"], prof)
        if not vmode:
            by = RW.build_sections(load_rules(prof["obj"], prof))

    def want_for(text):
        """The oracle's answer, or None when it cannot be compared."""
        if args.emulator:
            import byte_diff
            if not args.chip:
                return emulator(text, prof, args.tmp), 0
            if rawmode:
                w, cut = byte_diff.oracle(text.encode("latin1"), prof["ram"],
                                          prof["obj"], args.tmp)
                return (None, 1) if cut else (w, 0)
            pay = ((b"\x16" if vmode else b"\x05T")
                   + text.encode("ascii") + b"\r")
            w, cut = byte_diff.oracle(pay, prof["ram"], prof["obj"], args.tmp)
            return (None, 1) if cut else (w, 0)
        if rawmode:
            import textalker
            mach = textalker.Machine(prof["obj"], rules=prof["name"])
            mach.feed(textalker.Machine.REPEAT_FILTER_DISABLE)
            return list(mach.feed(text.encode("latin1"))), 0
        if vmode:
            return list(allophone.speak_phonemes(text, t2)), 0
        codes = RW.walk_line(text, by, prof=prof)
        if not args.chip:
            return codes, 0
        return list(allophone.speak_codes(codes, t2)), 0

    what = "chip bytes" if args.chip else "phonemes"
    oracle_name = "emulator" if args.emulator else "python"
    ok = bad = skipped = 0
    for text, g in zip(cases, got):
        want, cut = want_for(text)
        if cut or want is None:
            skipped += 1
            continue
        if g == want:
            ok += 1
        else:
            bad += 1
            if bad <= args.show:
                n = min(len(want), len(g or []))
                at = next((i for i in range(n) if want[i] != g[i]), n)
                lo = max(0, at - 5)
                print("  %r" % text)
                print("    first difference at %d of %d/%d"
                      % (at, len(want), len(g or [])))
                print("    %-8s %s" % (oracle_name,
                                       " ".join("%02X" % x
                                                for x in want[lo:at + 9])))
                print("    %-8s %s" % ("C",
                                       " ".join("%02X" % x
                                                for x in (g or [])[lo:at + 9])))
    if skipped:
        print("%d not compared: the emulator chunked them or ran out of steps"
              % skipped)
    print("%s vs %s: %d/%d  %.1f%%"
          % (what, oracle_name, ok, ok + bad, 100.0 * ok / max(1, ok + bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
