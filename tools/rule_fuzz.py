"""Random words against the emulator, to look for divergences the
corpora do not contain.

Words go through in batches of six, and the walker is given the same
line the emulator gets: a word's position in the line is not neutral,
because $DF3A tests an absolute buffer index. A batch the emulator
splits into chunks of its own is retried one word at a time, so a parse
failure cannot quietly hide a real disagreement -- it hid 78 of 1200 the
first time this ran.

Usage:
    python tools/rule_fuzz.py [SEED] [COUNT] [--uniform]
"""
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rule_walk as RW
from rule_probe import load_rules
ROOT = RW.ROOT
tmp = os.environ.get("TEMP", ".")
import profiles
args = [a for a in sys.argv[1:] if not a.startswith("--")]
random.seed(int(args[0]) if args else 1)
N = int(args[1]) if len(args) > 1 else 1200
VERSION = next((a.split("=", 1)[1] for a in sys.argv[1:]
                if a.startswith("--version=")), "3.1.3")
PROF = profiles.PROFILES[VERSION]

UNIFORM = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
# English-ish letter weights, so the fuzz hits real rules rather than
# bouncing off the fallback rule for every letter. --uniform instead
# reaches letter combinations English never makes.
ENGLISH = ("EEEEEEEEEEEETTTTTTTTTAAAAAAAAOOOOOOOIIIIIIINNNNNNNSSSSSSHHHHHH"
           "RRRRRRDDDDLLLLCCCUUMMWWFFGGYYPPBBVKJXQZ")
FREQ = UNIFORM if "--uniform" in sys.argv else ENGLISH
words = set()
while len(words) < N:
    n = random.choice([1,2,3,4,5,6,7,8,10,12,14,16,18])
    words.add("".join(random.choice(FREQ) for _ in range(n)))
words = sorted(words)
BY = RW.build_sections(load_rules(PROF["obj"], PROF))
ok, bad, unresolved = RW.diff(words, BY, PROF["ram"], PROF["obj"], tmp,
                              progress=True, prof=PROF)
total = ok + len(bad)
print("fuzz: %d/%d  %.2f%%   (%d unresolved)"
      % (ok, total, 100.0 * ok / max(1, total), len(unresolved)))
for w, r, m in bad[:25]:
    print("  %-20s oracle %-34s walker %s"
          % (w, " ".join("%02X" % x for x in r),
             " ".join("%02X" % x for x in m) if m is not None else "(no match)"))
