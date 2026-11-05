#!/usr/bin/env python3
"""Compare unpacked LPC frames, emulator against port, frame by frame.

The synthesiser unpacks a packed allophone frame into a block of RAM --
$FFD7 (flags|energy) and $FFD8-$FFE1 (K1-K10) in 3.1.3, $F6BC and
$F6BD-$F6C6 in 1.3 -- before the bit packer turns it back into the
chip's own field widths. Watching that block gives the frames the
emulator built, with none of the packing in the way, and tagging each
one with the allophone index says which sound it came from.

A frame is complete at the second write to the energy byte (the first
happens before the flag bits are ORed in) followed by the last K. An
unvoiced frame stops after K4, leaving K5-K10 holding whatever the
previous frame left there -- so those are compared as they stand, not
zeroed.

This is the right first move whenever the chip bytes diverge and the
machine state looks identical: it found $DC71's one-code lookahead in a
single run.

    python tools/frame_diff.py [--version 1.3] '\x05L\x05TXH\r'
"""
import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import allophone as A
import profiles
import textalker
from rule_probe import HARNESS


# Every scratch name carries the process id. Two of these tools running
# at once otherwise share bd_in.bin / bd_bytes.txt and each reads the
# other's oracle, which does not fail -- it just reports disagreements
# that are not there. It cost two "failing" seeds that were fine.
def _scratch(tmpdir, name):
    stem, ext = os.path.splitext(name)
    return os.path.join(tmpdir, "%s_%d%s" % (stem, os.getpid(), ext))


def emulator(payload, prof, tmp):
    f = prof["frame"]
    lo, hi = min(f["idx"], f["ef"]), f["k"] + 9
    src = _scratch(tmp, "fr.bin")
    open(src, "wb").write(payload)
    env = dict(os.environ)
    env["ECHOTALK_RAM_WATCH"] = "%04X-%04X" % (lo, hi)
    r = subprocess.run([HARNESS, prof["ram"], prof["obj"], src,
                        _scratch(tmp, "fr.wav")],
                       capture_output=True, env=env)
    reg, out, idx = {}, [], 0
    pat = rb"\[ram\] \$([0-9A-F]{4}) <- ([0-9A-F]{2})  PC=\$([0-9A-F]{4})"
    for m in re.finditer(pat, r.stderr):
        a, v, pc = int(m.group(1), 16), int(m.group(2), 16), m.group(3).decode()
        if a == f["idx"]:
            idx = v
            continue
        if a != f["ef"] and not f["k"] <= a <= f["k"] + 9:
            continue
        if a == f["ef"] and pc == f["first_pc"]:
            continue
        reg[a] = v
        last_k = a == f["k"] + 9
        unvoiced_end = a == f["k"] + 3 and reg.get(f["ef"], 0) < 0x80
        if last_k or unvoiced_end:
            out.append((idx, reg.get(f["ef"], 0),
                        [reg.get(f["k"] + i, 0) for i in range(10)]))
    return out


def port(payload, version):
    out = []
    orig = A.Synth.unpack

    def traced(self, data, y):
        y = orig(self, data, y)
        out.append((self.index, self.ef, list(self.k)))
        return y
    A.Synth.unpack = traced
    try:
        m = textalker.Machine(rules=version)
        m.feed(textalker.Machine.REPEAT_FILTER_DISABLE)
        m.feed(payload)
    finally:
        A.Synth.unpack = orig
    return out


def show(f):
    return "allo %02X  ef %02X | %s" % (f[0], f[1],
                                        " ".join("%02X" % x for x in f[2]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("payload", nargs="?", default="\x05L\x05TXH\r",
                    help="a python-escaped byte string")
    ap.add_argument("--version", default="3.1.3", choices=("3.1.3", "1.3"))
    ap.add_argument("--show", type=int, default=8)
    ap.add_argument("--tmp", default=os.environ.get("TEMP", "."))
    args = ap.parse_args()
    prof = profiles.PROFILES[args.version]
    pay = args.payload.encode("ascii").decode("unicode_escape").encode("latin1")

    e = emulator(pay, prof, args.tmp)
    p = port(pay, args.version)
    print("emulator frames: %d    port frames: %d" % (len(e), len(p)))
    bad = 0
    for i in range(max(len(e), len(p))):
        a = e[i] if i < len(e) else None
        b = p[i] if i < len(p) else None
        if a == b:
            continue
        bad += 1
        if bad <= args.show:
            print(" frame %d" % i)
            print("   emulator %s" % (show(a) if a else "(none)"))
            print("   port     %s" % (show(b) if b else "(none)"))
    print("differing frames: %d" % bad)


if __name__ == "__main__":
    main()
