#!/usr/bin/env python3
"""The whole of Textalker, natively: raw input bytes -> TMS5220 bytes.

This is the top of the rewrite. It owns the machine state -- the
$FF.. variables the Ctrl-E commands set -- parses the input stream the
way $D8D4-$DBA3 does, and drives the two stages:

    tools/rule_walk.py   text -> phoneme codes   ($DD10-$E5A9)
    tools/allophone.py   phoneme codes -> chip   ($DBD6-$EA43)

The per-character chain, from $D8D4 down:

    $D8D4  Ctrl-V ($16) enters phoneme mode; a carriage return there is
           turned into "." and leaves it again
    $D917  the command parser: the introducer (default Ctrl-E, $05, held
           in $FFAE), then digits into $FFBE, then a letter dispatched
           through the table at $D9A2
    $DACE  otherwise text, buffered at $FDAC and spoken at a carriage
           return, at $80, or when the buffer reaches $F0

The command table at $D9A2 has **21 entries**, not the 13 that were
documented: A B C D E F K L M N O P R S T V W and then ; , I ?. The
letters they are looked up in are case-folded, and any character that is
neither a digit nor a match ends the command.

    nP  pitch, 0-63 (default 24), with intonation      $FFAF, $FFB0 = 0
    nF  the same pitch but monotone                    $FFAF, $FFB0 = $FF
    nV  volume 0-15 (default 12)                       $FFC3 = (n & 15) - 12
    nD  inter-word delay 0-15 (v3.1.3 only)            $FFBB
    nR  repeat filter                                  $FFF2 = (n or $80) - 1
    C   compressed: drop the interpolated frames       $FFB1 = $FF
    E   expanded: keep them (the default)              $FFB1 = 0
    A   ALL punctuation, spoken by name                $FFAD |= $20
    M   MOST punctuation                               $FFAD: -$20 +$10
    S   SOME punctuation -- the default: no names, but $FFAD: -$30
        commas and stops still pause and shape the intonation
    L   letter mode: every character is spoken alone,  $FFAD |= $40
        and an UPPER-CASE letter raises the pitch by
        5. $DB22 stores $FFAF into $FD7E only for
        $C1-$DA; lower case folds through $DB19 and
        gets $FF, which $DB7D reads as "no bump".
    W   word mode (the default)                        $FFAD &= ~$40
    K   spell UPPER-CASE letters: plant an $80 before   $FFB2 = $FF
        each one so it is spoken alone. $DAEA-$DAF2
        checks $C1-$DA, so lower case is untouched.
    N   stop spelling (the default)                    $FFB2 = 0
    T   Talk only: speak, do not echo to the screen    $FFAD: +$01 -$08
    O   Off: echo to the screen, do not speak          $FFAD: -$01 +$08
    B   Both: speak AND echo to the screen             $FFAD |= $09

$FFAD bit 0 is "speak" and bit 3 is "echo to the screen", which is why
those three commands are the four combinations minus the useless one.
They exist so a program can say one thing and print another -- a
deliberate misspelling that the Echo pronounces correctly while the
screen shows the real word, or a progress indicator that would be
tedious to hear. Only bit 0 concerns a speech-synthesis rewrite; there
is no screen here, so O simply produces nothing.
    ;   $FFF5 = n     ,  $FFF6 = n     I  $FFF7 = n, and $DAAC files
        the three into $FFE6/$FFEA -- the Echo's index-mark machinery
    ?   reports state through $FFBB

A control character (below $20, other than a carriage return) that
follows the introducer becomes the NEW introducer ($D949), so the
command character itself is reprogrammable.

Usage:
    python tools/textalker.py --text "THE CAT."
    python tools/textalker.py --raw '\\x05A\\x05THI, THERE.'
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import allophone
import rule_walk as RW
import syllabify
from rule_probe import load_rules

ROOT = allophone.ROOT
BASE = allophone.BASE

CMD_LETTERS = 0xD9A2        # 21 entries, then the low and high jump bytes
N_COMMANDS = 21


class Machine(object):
    """Textalker's settable state, and the input parser over it."""

    # render_text_loader sends this during init (render_common.h's
    # REPEAT_FILTER_DISABLE), so anything compared against the emulator
    # has to start from the same place.
    REPEAT_FILTER_DISABLE = b"" + b"99R"

    def __init__(self, obj_path=None, rules=None, voice=None):
        """`rules` and `voice` are profile names, and they need not
        agree: the phoneme alphabet is shared between the versions, so
        1.3's letter-to-sound rules can drive 3.1.3's synthesiser, or the
        other way round. `rules` also decides the driver -- the command
        set, letter mode and the capital lift all belong to that half."""
        import profiles
        self.rp = profiles.get(rules or "3.1.3", obj_path)
        self.vp = profiles.get(voice or self.rp["name"])
        self.rom = open(self.rp["obj"], "rb").read()
        base = self.rp["base"]
        self.t2 = allophone.load_tables(self.vp["obj"], self.vp)
        self.tsyl = (syllabify.load_tables(self.rp["obj"])
                     if self.rp["syllabifier"] else None)
        self.by = RW.build_sections(load_rules(self.rp["obj"], self.rp))
        v = self.rp["vars"]
        # $9410-$9445 in v1.3's loader; empty for 3.1.3, whose image
        # already carries its startup state.
        init = profiles.loader_init(self.rp,
                                    set(a for a in v.values() if a))
        g = lambda a: (init.get(a, self.rom[a - base])
                       if a is not None else 0)
        self.letters = [0x80 | ord(c) for c in self.rp["commands"]]

        self.flags = g(v["flags"])
        self.intro = 0x05               # $FFAE, the command character
        self.pitch = g(v["base_pitch"])
        self.monotone = g(v["monotone"])
        self.compress = g(v.get("compress"))
        self.spell = 0                  # $FFB2; v1.3 has no K or N
        self.delay = g(v.get("delay"))
        self.arg = 0                    # $FFBE
        self.volume = g(v["p_step"])
        self.repeat = 0                 # $FFF2; v1.3 has no R
        self.marks = [0, 0, 0]

        # $FDAC is 256 bytes that nothing ever clears: a flush resets
        # only the LENGTH ($DBB8 STX $FD9E), so the tail of the previous
        # utterance stays where it is and is still readable.
        #
        # "Never cleared" reaches back further than the previous
        # utterance: neither buffer is initialised at boot either, so
        # before anything is spoken they hold whatever the OBJ image
        # carries at those addresses. 3.1.3's are zeros and starting
        # from zero works by luck; v1.3's are not -- $F494 is $A0, in
        # the $51-$B2 window $DC7C tests, so the very first utterance's
        # last code sees a fake following "T" and takes the $EAxx +8
        # stop release. "ED" flushed by a Ctrl-E is the shortest case
        # that shows it.
        def window(a):
            i = a - base
            return list(self.rom[i:i + 256])
        self.buf = window(self.rp["inbuf"])         # $FDAC
        self.blen = 0                   # $FD9E
        self.word = 0                   # $FD9C: a word is open
        self.wstart = 0                 # $FDA6
        self.saved_pitch = 0xFF         # $FD7E
        self.out = []
        self.chip = allophone.Chip()
        self.carry = {}                 # synthesis state across utterances
        self.phbuf = window(self.rp["phbuf"])   # $FEAD, likewise

    # ------------------------------------------------------------ commands

    def command(self, letter):
        n, f = self.arg, self.flags
        if letter == "A":
            self.flags = f | 0x20
        elif letter == "B":
            self.flags = f | 0x09
        elif letter == "C":
            self.compress = 0xFF
        elif letter == "D":
            self.delay = n
        elif letter == "E":
            self.compress = 0x00
        elif letter == "F":
            self.pitch, self.monotone = n, 0xFF
        elif letter == "K":
            self.spell = 0xFF
        elif letter == "L":
            self.flags = f | 0x40
        elif letter == "M":
            self.flags = (f & 0xDF) | 0x10
        elif letter == "N":
            self.spell = 0x00
        elif letter == "O":
            self.flush()
            self.flags = (f & 0xFE) | 0x08
        elif letter == "P":
            self.pitch, self.monotone = n, 0x00
        elif letter == "R":
            self.repeat = ((n or 0x80) - 1) & 0xFF
        elif letter == "S":
            self.flags = f & 0xCF
        elif letter == "T":
            self.flags = (f & 0xF7) | 0x01
        elif letter == "V":
            self.volume = ((n & 0x0F) - 0x0C) & 0xFF
        elif letter == "W":
            self.flags = f & 0xBF
        elif letter == ";":
            self.marks[0], self.arg = n, 0
        elif letter == ",":
            self.marks[1], self.arg = n, 0
        elif letter == "I":
            self.marks[2] = n

    # -------------------------------------------------------------- speaking

    def flush(self):
        """$DBA3: run both stages over the buffer, then reset it."""
        if not self.blen:
            return
        codes = RW.run_buffer(self.buf, self.by, self.flags, end=self.blen,
                              prof=self.rp)
        self.out += allophone.speak_codes(
            codes, self.t2, chip=self.chip, state=self)
        self.blen, self.word, self.wstart = 0, 0, 0
        if not self.saved_pitch & 0x80:         # $DBCD
            self.pitch = self.saved_pitch

    def _text(self, c):
        """$DACE-$DBA2: buffer one character, syllabifying each word as it
        closes, and speak when something says to."""
        if not (self.flags & 0x01):             # $DACE: not accepting text
            return
        if self.spell and 0xC1 <= c <= 0xDA and self.blen != 0xFE:
            # $DAF9 only PLACES the $80 in the buffer; it does not speak.
            # The letter is stored after it and the mark travels with the
            # text into stage 1.
            self.buf[self.blen] = 0x80
            self.blen += 1
        if 0xC1 <= c <= 0xDA:                   # $DB03: case folding
            self.saved_pitch = self.pitch       # $DB22
        else:
            if 0xE1 <= c <= 0xFA:
                c -= 0x20
                if c >= 0xDB:
                    c = (c + 6) & 0xFF
            self.saved_pitch = 0xFF             # $DB1A
        word_char = (c == 0xA7 or 0xB0 <= c < 0xBA or 0xC1 <= c < 0xDB)
        if word_char:
            self.word = 0xFF                    # $DB41
        elif self.word:                         # $DB4D: the word just closed
            if self.tsyl is not None:       # v1.3 has no syllabifier
                self.blen = syllabify.build_at(self.buf, self.tsyl,
                                               self.wstart, self.blen)
            self.word = 0
        self.buf[self.blen] = c                 # $DB55
        self.blen += 1
        if not self.word:
            self.wstart = self.blen             # $DB67
        if self.blen >= 0xF0:                   # $DB6A
            return self.flush()
        if self.flags & 0x40:                   # $DB6E: letter mode
            # $DB77 writes the carriage return at $FDAC,X without
            # bumping $FD9E, so it is past the end and never spoken; and
            # because the word logic at $DB2D never ran for a word
            # character, letter mode speaks each character UNsyllabified.
            if self.rp["capital_pitch"] and not self.saved_pitch & 0x80:
                self.pitch = (self.saved_pitch + 5) & 0xFF   # $DB7F
            return self.flush()
        if c in (0xDD, 0xBE):                   # $DB8A: "]" or ">" alone
            if self.blen == 1:
                self.flush()
            return
        if c in (0x8D, 0x80):                   # $DB9A: CR, or the spell mark
            self.flush()

    def feed(self, payload):
        """$D8D4 down. Returns the chip byte stream."""
        phon = []
        for c in payload:
            # The Apple II's input routines hand Textalker characters
            # with the high bit set; $FD90 always holds them that way.
            c |= 0x80
            m = c & 0x7F
            if m == 0x16:                       # $D8D4: Ctrl-V
                self.flush()
                self.flags |= 0x02
                self.arg = 0
                continue
            if self.flags & 0x02:               # $D8EF: phoneme mode
                if c == 0x8D:
                    self.out += allophone.speak_phonemes(
                        "".join(phon), self.t2, chip=self.chip, state=self)
                    phon = []
                    self.flags &= 0xFD
                else:
                    phon.append(chr(m))
                continue
            if self.flags & 0x04:               # $D93C: inside a command
                if m < 0x20 and m != 0x0D:
                    self.intro = m              # $D949: a new introducer
                    self.flags &= 0xFB
                    continue
                if 0xB0 <= c < 0xBA:            # $D959: another digit
                    # $D972's BEQ tests the Z flag from the multiply
                    # loop's DEX, not from the STA before it -- STA sets
                    # no flags -- so it is ALWAYS taken and a digit
                    # always leaves the command open. Reading it as
                    # "branch when the accumulator is zero" makes every
                    # multi-digit command end at its first digit.
                    self.arg = ((self.arg & 0x0F) * 10 + (c & 0x0F)) & 0xFF
                    continue
                else:
                    a = c
                    if 0xE1 <= a < 0xFB:
                        a &= 0xDF               # $D97C: fold the letter
                    if a in self.letters:
                        self.command(chr(a & 0x7F))
                self.flags &= 0xFB              # $D996: the command ends
                continue
            if m == self.intro:                 # $D923
                self.flush()
                self.flags |= 0x04
                self.arg = 0
                continue
            self._text(c)
        return self.out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--text")
    ap.add_argument("--raw", help="a python-escaped byte string")
    ap.add_argument("--rules", default="3.1.3", choices=("3.1.3", "1.3"),
                    help="whose letter-to-sound rules and command set")
    ap.add_argument("--voice", choices=("3.1.3", "1.3"),
                    help="whose synthesiser; defaults to --rules")
    ap.add_argument("--obj")
    args = ap.parse_args()
    if args.text:
        payload = b"\x05T" + args.text.encode("ascii") + b"\r"
    elif args.raw:
        payload = args.raw.encode("ascii").decode("unicode_escape").encode("latin1")
    else:
        sys.exit("give --text or --raw")
    m = Machine(args.obj, rules=args.rules, voice=args.voice)
    print(" ".join("%02x" % b for b in m.feed(payload)))


if __name__ == "__main__":
    main()
