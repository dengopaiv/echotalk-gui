#!/usr/bin/env python3
"""Differential test of stage 2 against the emulator's chip byte stream.

The harness's third checkpoint: ECHOTALK_BYTE_DUMP records every byte
Textalker writes to the Echo II's data port, which is the whole of what
the TMS5220 ever sees. Getting this byte-exact means the native path is
indistinguishable from the emulation, since the chip emulation itself is
already validated against real hardware.

Usage:
    python tools/byte_diff.py --phonemes AEIOU HELO
    python tools/byte_diff.py --text "THE CAT." "HELLO, WORLD!"
    python tools/byte_diff.py --fuzz-phonemes 300 [--seed N]
"""
import argparse
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import allophone
import textalker
from rule_probe import HARNESS


# Every scratch name carries the process id. Two of these tools running
# at once otherwise share bd_in.bin / bd_bytes.txt and each reads the
# other's oracle, which does not fail -- it just reports disagreements
# that are not there. It cost two "failing" seeds that were fine.
def _scratch(tmpdir, name):
    stem, ext = os.path.splitext(name)
    return os.path.join(tmpdir, "%s_%d%s" % (stem, os.getpid(), ext))

ROOT = allophone.ROOT

# The 51 characters the Ctrl-V mode accepts, from Jayson's random-phoneme
# program in tmp/, plus the four the character map shows are also live:
# ',' is a pause, '/' an allophone, and '+' and '-' step the pitch.
PHONEME_CHARS = ("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
                 "!@#$%&*()" + ";':?<>" + ",/+-")


def oracle(payload, loader, obj, tmpdir):
    """The chip bytes the emulator writes for a raw input stream.

    Returns (bytes, truncated). `truncated` means the 6502 ran out of
    step budget part-way through a character, so the byte stream stops
    mid-utterance and says nothing about what Textalker would have done
    -- comparing against it measures the budget, not the port. It
    happens for the runaway cases this test deliberately produces: a
    rule that consumes past the end of the buffer can walk the whole 256
    bytes of leftovers and emit 240 phonemes for a two-letter word,
    which is half a minute of speech per character."""
    src = _scratch(tmpdir, "bd_in.bin")
    wav = _scratch(tmpdir, "bd_out.wav")
    dump = _scratch(tmpdir, "bd_bytes.txt")
    open(src, "wb").write(payload)
    env = dict(os.environ)
    env["ECHOTALK_BYTE_DUMP"] = dump
    r = subprocess.run([HARNESS, loader, obj, src, wav],
                       capture_output=True, env=env)
    return ([int(x, 16) for x in open(dump).read().split()],
            b"step budget" in r.stderr)


def show(name, want, got):
    print("  %s" % name)
    n = min(len(want), len(got))
    at = next((i for i in range(n) if want[i] != got[i]), n)
    print("    first difference at byte %d of %d/%d" % (at, len(want), len(got)))
    lo = max(0, at - 6)
    print("    oracle %s" % " ".join("%02x" % b for b in want[lo:at + 10]))
    print("    native %s" % " ".join("%02x" % b for b in got[lo:at + 10]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--phonemes", nargs="*")
    ap.add_argument("--text", nargs="*")
    ap.add_argument("--fuzz-phonemes", type=int)
    ap.add_argument("--raw", nargs="*",
                    help="python-escaped byte strings fed to tools/textalker.py")
    ap.add_argument("--fuzz-cmd", type=int,
                    help="random Ctrl-E command streams through the driver")
    ap.add_argument("--fuzz-text", type=int,
                    help="random words and punctuation through both stages")
    ap.add_argument("--lines", nargs="?", const=os.path.join(
        ROOT, "tmp", "A Story with Lots of Nonsense Words.txt"),
        help="a text file, cut into pieces that fit one Textalker chunk")
    ap.add_argument("--width", type=int, default=38)
    ap.add_argument("--limit", type=int)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--maxlen", type=int, default=6)
    ap.add_argument("--show", type=int, default=6)
    ap.add_argument("--version", default="3.1.3", choices=("3.1.3", "1.3"))
    ap.add_argument("--loader")
    ap.add_argument("--obj")
    ap.add_argument("--tmp", default=os.environ.get("TEMP", "."))
    args = ap.parse_args()

    import profiles
    prof = profiles.PROFILES[args.version]
    args.loader = args.loader or prof["ram"]
    args.obj = args.obj or prof["obj"]
    t = allophone.load_tables(args.obj, prof)
    cases = []
    if args.phonemes:
        cases += [("phon", p) for p in args.phonemes]
    if args.text:
        cases += [("text", s) for s in args.text]
    if args.lines:
        import line_diff
        got = list(line_diff.pieces(args.lines, args.width))
        cases += [("text", p) for p in (got[:args.limit] if args.limit else got)]
    if args.raw:
        cases += [("raw", r.encode("ascii").decode("unicode_escape"))
                  for r in args.raw]
    if args.fuzz_cmd:
        random.seed(args.seed)
        E, CR = chr(5), chr(13)
        letters = "EEEETTTAAAOOOIIINNSSHHRRDLCUMWFGYPBVKJXQZ   ,."
        # Only the commands this version actually has: v1.3 is missing
        # D, K, N, R and the ";,I?" index marks.
        cmds = [c for c in "AMSCELWKNTBPFVDRO" if c in prof["commands"]]
        for _ in range(args.fuzz_cmd):
            out = [E + "T"]
            for _ in range(random.randint(1, 5)):
                n = random.choice(["", "", str(random.randint(0, 63))])
                out.append(E + n + random.choice(cmds))
                out.append("".join(random.choice(letters)
                                   for _ in range(random.randint(1, 12))))
            cases.append(("raw", "".join(out) + CR))
    if args.fuzz_text:
        random.seed(args.seed)
        letters = ("EEEEEEEETTTTTAAAAOOOOIIIINNNNSSSHHHRRRDDLLCUMWFGYPBVKJXQZ"
                   "0123456789")
        for _ in range(args.fuzz_text):
            words = [("".join(random.choice(letters)
                              for _ in range(random.randint(1, 9))))
                     for _ in range(random.randint(1, 4))]
            line = " ".join(words) + random.choice(["", "", ".", ",", "!", "?", ";", ":"])
            cases.append(("text", line))
    if args.fuzz_phonemes:
        random.seed(args.seed)
        for _ in range(args.fuzz_phonemes):
            n = random.randint(1, args.maxlen)
            cases.append(("phon", "".join(random.choice(PHONEME_CHARS)
                                          for _ in range(n))))
    if not cases:
        sys.exit("give --phonemes, --text, --lines, --raw or a --fuzz- mode")

    by = None
    ok = bad = skipped = 0
    for kind, s in cases:
        if kind == "raw":
            want, cut = oracle(s.encode("latin1"), args.loader, args.obj,
                               args.tmp)
            m = textalker.Machine(args.obj, rules=args.version)
            # The harness sends this during init whatever the version;
            # v1.3 has no R command and simply ends the command on it.
            m.feed(textalker.Machine.REPEAT_FILTER_DISABLE)
            got = m.feed(s.encode("latin1"))
        elif kind == "phon":
            want, cut = oracle(b"\x16" + s.encode("ascii") + b"\r",
                               args.loader, args.obj, args.tmp)
            got = allophone.speak_phonemes(s, t)
        else:
            if by is None:
                import rule_walk as RW
                from rule_probe import load_rules
                by = RW.build_sections(load_rules(args.obj, prof))
            want, cut = oracle(b"\x05T" + s.encode("ascii") + b"\r",
                               args.loader, args.obj, args.tmp)
            import rule_walk as RW
            got = allophone.speak_codes(RW.walk_line(s, by, prof=prof), t)
        if cut:
            skipped += 1
        elif want == got:
            ok += 1
        else:
            bad += 1
            if bad <= args.show:
                show("%s %r" % (kind, s), want, got)
    if skipped:
        print("%d case%s not compared: the emulator hit its step budget"
              % (skipped, "" if skipped == 1 else "s"))
    if not ok + bad:
        print("chip byte stream: nothing comparable")
        return
    print("chip byte stream: %d/%d  %.1f%%"
          % (ok, ok + bad, 100.0 * ok / (ok + bad)))


if __name__ == "__main__":
    main()
