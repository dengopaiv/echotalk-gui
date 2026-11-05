"""Differential test of the whole-line walk against the emulator.

Usage:
    python tools/line_diff.py [TEXTFILE] [--width N]


Only single-chunk inputs are compared. Textalker breaks long text into
chunks of its own and terminates each with $1A, dropping the space it
broke at -- that chunk-size limit is one of the things the rewrite
exists to remove, so reproducing it is not wanted, and modelling it here
would only measure the wrong thing. Feeding it text that fits in one
chunk tests the engine and nothing else.
"""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rule_walk as RW
from rule_probe import load_rules, HARNESS

ROOT = RW.ROOT
SP = os.environ.get("TEMP", ".")


def oracle(text):
    src, wav = os.path.join(SP, "ln.bin"), os.path.join(SP, "ln.wav")
    dump = os.path.join(SP, "ln.txt")
    open(src, "wb").write(b"\x05T" + text.encode("ascii") + b"\r")
    env = dict(os.environ)
    env["ECHOTALK_PHONEME_DUMP"] = dump
    subprocess.run([HARNESS, os.path.join(ROOT, "roms", "textalker.ram.bin"),
                    os.path.join(ROOT, "roms", "textalker.obj.bin"), src, wav],
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
    return out


def pieces(path, width):
    """Source text cut into pieces that fit in one Textalker chunk."""
    txt = open(path, "rb").read().decode("utf-8", "replace")
    txt = "".join(c if 32 <= ord(c) < 127 else " " for c in txt)
    for para in txt.split("\n"):
        cur = ""
        for word in para.split():
            if len(cur) + len(word) + 1 > width:
                if cur:
                    yield cur
                cur = word
            else:
                cur = (cur + " " + word).strip()
        if cur:
            yield cur


def main():
    global SP
    ap = argparse.ArgumentParser()
    ap.add_argument("text", nargs="?",
                    default=os.path.join(ROOT, "tmp",
                                         "A Story with Lots of Nonsense Words.txt"))
    ap.add_argument("--width", type=int, default=38,
                    help="characters per piece; keep it under one Textalker chunk")
    ap.add_argument("--obj", default=os.path.join(ROOT, "roms", "textalker.obj.bin"))
    ap.add_argument("--tmp", default=SP)
    args = ap.parse_args()
    SP = args.tmp

    by = RW.build_sections(load_rules(args.obj))
    ok = bad = skipped = 0
    for ln in pieces(args.text, args.width):
        want = oracle(ln)
        if len(want) != 1:
            skipped += 1
            continue
        got = RW.walk_line(ln, by)
        if got == want[0]:
            ok += 1
        else:
            bad += 1
            if bad <= 10:
                print("  %r\n    oracle %s\n    walker %s" % (
                    ln, " ".join("%02X" % x for x in want[0]),
                    " ".join("%02X" % x for x in got) if got else "(no match)"))
    print("single-chunk lines: %d/%d  %.1f%%   (%d skipped as multi-chunk)"
          % (ok, ok + bad, 100.0 * ok / max(1, ok + bad), skipped))


if __name__ == "__main__":
    main()
