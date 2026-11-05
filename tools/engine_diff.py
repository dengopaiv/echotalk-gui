#!/usr/bin/env python3
"""Compare the two engines' AUDIO, the way audio has to be compared.

The chip byte stream is checked elsewhere (tools/tk_diff.py). This asks
the different question: does the native engine, driving the same chip
through the same library, produce the same SOUND as the 6502 does?

Not by asserting sample-wise equality of whole buffers -- HANDOFF has a
lesson about that, learned four times in one session. Two renderings of
the same speech can differ by a small alignment shift and be identical
as content. So this slides a window along, finds the best local shift
for each, and reports mismatches within it.

What that measures, and why the shift is allowed to move: the native
engine reproduces Textalker's waits but not the 6502's incidental
compute time, so its gaps between utterances are marginally shorter.
The speech itself should be bit-identical, and the shift should drift
slowly and monotonically -- a few tens of samples over several seconds.
Mismatches inside a window mean the speech itself differs, which is the
thing that must be zero.

    python tools/engine_diff.py [--version 1.3] [--text "..."]
"""
import argparse
import os
import struct
import subprocess
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import profiles

ROOT = profiles.ROOT
SAY = os.path.join(ROOT, "build", "native", "say")
if os.path.exists(SAY + ".exe"):
    SAY += ".exe"


def _scratch(tmpdir, name):
    stem, ext = os.path.splitext(name)
    return os.path.join(tmpdir, "%s_%d%s" % (stem, os.getpid(), ext))


def render(prof, engine, tmpdir, text=None, path=None, raw=False):
    wav = _scratch(tmpdir, "eng_%s.wav" % engine)
    cmd = [SAY, "--" + engine]
    if raw:
        cmd += ["--raw"]
    cmd += [prof["ram"], prof["obj"]]
    if path:
        cmd += ["--file", path]
    else:
        cmd += [text]
    cmd += [wav]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode:
        sys.exit("say failed: %s" % r.stderr.decode("utf-8", "replace"))
    w = wave.open(wav, "rb")
    d = w.readframes(w.getnframes())
    w.close()
    return list(struct.unpack("<%dh" % (len(d) // 2), d))


def segments(x, thresh=64, min_gap=160):
    """Runs of audible samples, split at silences of at least min_gap.

    Fixed windows cannot answer the question. The two engines differ in
    the LENGTH OF THE GAPS, so any window containing a gap boundary
    mismatches at every alignment however identical the speech is --
    which reports a defect that is not there. Comparing the speech runs
    themselves asks the question that matters: is the speech the same,
    with only the silence between it differing?"""
    runs, start, quiet = [], None, 0
    for i, v in enumerate(x):
        if (v if v >= 0 else -v) > thresh:
            if start is None:
                start = i
            quiet = 0
        elif start is not None:
            quiet += 1
            if quiet >= min_gap:
                runs.append((start, i - quiet))
                start, quiet = None, 0
    if start is not None:
        runs.append((start, len(x)))
    return runs


def compare(a, b, span=4000, block=400, slack=24):
    """(worst mismatches in any block, first shift, last shift, samples
    compared per block, number of runs).

    The comparison walks the speech in blocks, carrying ONE alignment
    across the whole file and letting it move a few samples at a time.
    Both halves of that matter, and each was learned by getting it
    wrong:

    - It has to be allowed to move. The engines differ in how long the
      gaps are, and a gap can change length by a single sample in the
      middle of what looks like one run. Insisting on one shift per run
      reports that as corrupted speech.
    - It must NOT be re-searched from scratch per block. A 400-sample
      block searched over thousands of samples will happily find a
      false match; that reported two whole runs as garbage when they
      were identical. The alignment drifts slowly and monotonically, so
      the previous block's value is the right place to look.

    What this cannot excuse is a difference in the waveform itself: a
    block that matches at no nearby alignment is a real one."""
    ra, rb = segments(a), segments(b)
    nruns = min(len(ra), len(rb))
    worst, first, last, per = 0, None, None, 0
    shift = None
    for k in range(nruns):
        s0, e0 = ra[k]
        s1 = rb[k][0]
        ln = min(e0 - s0, rb[k][1] - s1)
        if ln < 200:
            continue
        off = 0
        while off + 200 <= ln:
            n = min(block, ln - off)
            if shift is None:
                # The first alignment is found once, over a long enough
                # block that it cannot be a coincidence.
                wide = min(2000, ln)
                lo_sh, hi_sh, m = -span, span, wide
            else:
                lo_sh, hi_sh, m = shift - slack, shift + slack, n
            best = None
            for sh in range(lo_sh, hi_sh + 1):
                if s1 + sh + off < 0 or s1 + sh + off + m > len(b):
                    continue
                d = sum(1 for i in range(0, m, 2)
                        if a[s0 + off + i] != b[s1 + sh + off + i])
                if best is None or d < best[1]:
                    best = (sh, d)
                if d == 0:
                    break
            if best is None:
                break
            shift = best[0]
            if first is None:
                first = shift
            last = shift
            worst = max(worst, best[1])
            per = max(per, m // 2)
            off += n
    return worst, first, last, per, nruns


PHONEME_CHARS = ("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
                 "!@#$%&*()" + ";':?<>" + ",/+-")
LETTERS = "EEEETTTAAAOOOIIINNSSHHRRDLCUMWFGYPBVKJXQZ   ,.;:!?"


def fuzz_payload(rnd, prof):
    """One random input stream: commands, phoneme strings and text,
    over several lines.

    Several LINES on purpose. Every defect the two engines have
    disagreed about lived in the state carried from one utterance into
    the next -- the chip left mid-frame, a SPEAK EXTERNAL skipped, a
    stop frame still queued. A corpus of one-line cases would have
    missed all of them."""
    E, CR = chr(5), chr(13)
    out = [E + "T"]
    for _ in range(rnd.randint(1, 4)):
        kind = rnd.random()
        if kind < 0.35:                                  # a command run
            cmds = [c for c in "AMSCELWKNTBPFVDR" if c in prof["commands"]]
            for _ in range(rnd.randint(1, 3)):
                n = rnd.choice(["", "", str(rnd.randint(0, 63))])
                out.append(E + n + rnd.choice(cmds))
                out.append("".join(rnd.choice(LETTERS)
                                   for _ in range(rnd.randint(1, 10))))
        elif kind < 0.6:                                 # Ctrl-V phonemes
            out.append(chr(0x16) + "".join(rnd.choice(PHONEME_CHARS)
                                           for _ in range(rnd.randint(1, 8))))
        else:                                            # plain text
            out.append(" ".join(
                "".join(rnd.choice(LETTERS) for _ in range(rnd.randint(1, 8)))
                for _ in range(rnd.randint(1, 4))))
        out.append(CR)
    return "".join(out).encode("latin1")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="3.1.3", choices=tuple(profiles.PROFILES))
    ap.add_argument("--text")
    ap.add_argument("--refs", action="store_true",
                    help="every file in reference_text/")
    ap.add_argument("--fuzz", type=int, default=0,
                    help="random multi-line command/phoneme/text streams")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--show", type=int, default=6)
    ap.add_argument("--span", type=int, default=4000,
                    help="how far the alignment may drift")
    ap.add_argument("--tmp", default=os.environ.get("TEMP", "."))
    args = ap.parse_args()
    prof = profiles.PROFILES[args.version]

    cases = []
    if args.text:
        cases.append(("(text)", args.text, None))
    if args.fuzz:
        import random
        rnd = random.Random(args.seed)
        for i in range(args.fuzz):
            pay = fuzz_payload(rnd, prof)
            path = _scratch(args.tmp, "fz_%d.bin" % i)
            open(path, "wb").write(pay)
            cases.append((repr(pay.decode("latin1"))[1:-1][:46], None, path))
    if args.refs or (not cases):
        d = os.path.join(ROOT, "reference_text")
        for name in sorted(os.listdir(d)):
            if name.endswith(".bin"):
                cases.append((name[:-4], None, os.path.join(d, name)))

    bad = 0
    shown = 0
    for name, text, path in cases:
        a = render(prof, "emulated", args.tmp, text, path, raw=bool(args.fuzz))
        b = render(prof, "native", args.tmp, text, path, raw=bool(args.fuzz))
        worst, first, last, per, nseg = compare(a, b, span=args.span)
        drift = "n/a" if first is None else "%+d..%+d" % (first, last)
        flag = "" if worst == 0 else "   <-- SPEECH DIFFERS"
        if worst:
            bad += 1
        if not args.fuzz or worst:
            if not args.fuzz or shown < args.show:
                print("  %-48s %7d/%-7d  %2d runs  worst %d/%d%s"
                      % (name, len(a), len(b), nseg, worst, per, flag))
                shown += 1
    print("%s: %d of %d identical as content"
          % (prof["name"], len(cases) - bad, len(cases)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
