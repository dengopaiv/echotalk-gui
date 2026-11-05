#!/usr/bin/env python3
"""Recursive-descent 6502 disassembler for the Textalker image.

A linear disassembler is not good enough here. Textalker interleaves
code and data, and `notes/disasm_fd53.txt` records py65's linear pass
drifting into the phoneme tables after about $D9A0 and never
recovering. Recursive descent follows control flow instead, so it only
decodes bytes that are actually reached, and anything it never reaches
is reported as data rather than mis-rendered as instructions.

Seeded from the addresses the emulator itself showed us. The rule
matcher was located by watching which PCs read the rule table:

    $E238  fetches the winning rule's payload
    $E23D  writes a phoneme into the buffer
    $E2F9 $E309 $E314 $E358 $E374 $E3AD $E3CA   compare pattern bytes
    $E340  reads whole rules in one run -- the skip-to-next-rule scan
    $E21E $E223  read the dispatch table

Run with the DEFAULT Python, not the MSYS2 one: prepending
/s/msys/ucrt64/bin to PATH for a build shadows the interpreter that has
py65 installed, and the import fails.

Usage:
    python tools/disasm.py                    # the rule matcher
    python tools/disasm.py --from E200 E2D8   # explicit seeds (hex)
    python tools/disasm.py --range E200 E400  # only report in a window
"""
import argparse
import os
import sys

from py65.devices.mpu6502 import MPU
from py65.disassembler import Disassembler

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0xD000            # v3.1.3 loads here; v1.3 loads at $D400

# The PCs the read-watch identified, plus the stress pass.
MATCHER_SEEDS = [0xE21E, 0xE238, 0xE2F9, 0xE309, 0xE314, 0xE340,
                 0xE358, 0xE374, 0xE3AD, 0xE3CA, 0xE2D8, 0xE2EF]

BRANCHES = {0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0}
STOP = {0x60, 0x40, 0x00}          # RTS, RTI, BRK
JMP_ABS, JMP_IND, JSR = 0x4C, 0x6C, 0x20


def load(obj_path, base=BASE):
    mem = [0] * 65536
    d = open(obj_path, "rb").read()
    for i, b in enumerate(d):
        if base + i < 65536:
            mem[base + i] = b
    return mem, base + len(d)


def walk(mem, seeds, lo, hi):
    """Follow control flow from each seed. Returns {addr: (len, text)}."""
    mpu = MPU()
    mpu.memory = mem
    dis = Disassembler(mpu)
    seen, out = set(), {}
    queue = list(seeds)
    while queue:
        pc = queue.pop()
        while True:
            if pc in seen or not (lo <= pc < hi):
                break
            seen.add(pc)
            length, text = dis.instruction_at(pc)
            out[pc] = (length, text)
            op = mem[pc]
            if op in STOP:
                break
            if op == JMP_ABS:
                pc = mem[pc + 1] | (mem[pc + 2] << 8)
                continue
            if op == JMP_IND:
                break                       # target unknown without state
            if op == JSR:
                queue.append(mem[pc + 1] | (mem[pc + 2] << 8))
                pc += 3
                continue
            if op in BRANCHES:
                off = mem[pc + 1]
                if off > 127:
                    off -= 256
                queue.append(pc + 2 + off)
            pc += length
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--obj", default=os.path.join(ROOT, "roms", "textalker.obj.bin"))
    ap.add_argument("--from", dest="seeds", nargs="*", default=None,
                    help="hex seed addresses; default is the rule matcher")
    ap.add_argument("--range", nargs=2, default=None,
                    help="hex lo hi; only report instructions in this window")
    ap.add_argument("--base", default=None,
                    help="hex load address; default $D000, v1.3 loads at $D400")
    args = ap.parse_args()

    base = int(args.base, 16) if args.base else BASE
    mem, end = load(args.obj, base)
    seeds = [int(s, 16) for s in args.seeds] if args.seeds else MATCHER_SEEDS
    code = walk(mem, seeds, base, end)

    lo, hi = (int(args.range[0], 16), int(args.range[1], 16)) if args.range \
        else (min(code), max(code) + 4)

    print("; recursive descent from: %s"
          % " ".join("$%04X" % s for s in sorted(seeds)))
    print("; %d instructions reached in $%04X-$%04X\n" % (len(code), BASE, end - 1))
    addr = lo
    while addr < hi:
        if addr in code:
            length, text = code[addr]
            raw = " ".join("%02X" % mem[addr + k] for k in range(length))
            print("%04X:  %-9s %s" % (addr, raw, text))
            addr += length
        else:
            run = addr
            while addr < hi and addr not in code:
                addr += 1
            n = addr - run
            if n:
                sample = " ".join("%02X" % mem[run + k] for k in range(min(n, 8)))
                print("%04X:  --- %d byte%s not reached (data?)  %s%s"
                      % (run, n, "" if n == 1 else "s", sample,
                         " ..." if n > 8 else ""))


if __name__ == "__main__":
    main()
