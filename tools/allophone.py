#!/usr/bin/env python3
"""Stage 2 of the C rewrite: phoneme codes -> TMS5220 chip bytes.

Stage 1 (tools/rule_walk.py) turns text into the phoneme string Textalker
builds at $FEAD. This turns that string into the exact byte stream
Textalker writes to the Echo II's chip port, which is the harness's
third differential checkpoint (ECHOTALK_BYTE_DUMP).

Like stage 1 this is a **transcription of the 6502**, not a model of it,
and every table is read out of the user's own image:

    $EA44  phoneme-mode character map, entry(c) = $EA24 + ord(c)
    $EA7F  pitch offset per group      $EA89  vowel variant per group
    $EA93  58 allophone pointers, 2 bytes each, into $EAEA-$EF20
    $E906  the ten K-coefficient bit widths: 5 5 4 4 4 4 4 3 3 3

Routines, following notes/disasm_stage2.txt:

    $DBD6  the driver: walk the phoneme buffer, split each code into an
           allophone index and a pitch group, call $E5C7
    $E5AA  the Ctrl-V phoneme-mode entry, via $E5E3 and $E623
    $E5C7  speak one allophone
    $E6B0  the codes that are not allophones at all: $7F skip, $00 and
           $19 pause, $FF end, >=$4F pitch glide, $37/$3F pitch step
    $E674  a stop consonant before a non-vowel takes the +8 variant,
           and is always preceded by a stop frame and a delay
    $E713  work out this frame's pitch offset and vowel variant
    $E7A3  read the allophone's frames and emit the selected ones
    $E7ED  unpack one packed frame into energy, pitch and K1-K10
    $E893  emit a frame, interpolating a half-step frame when the
           voicing changes ($E910)
    $E945  the pitch, which is a running value, not a per-frame one
    $E9AF  the bit packer: shift n bits of A into a byte and write it

An allophone's data is a bitmask followed by packed frames. A frame is
six bytes when bit 7 of its first byte is set (voiced: energy, pitch and
K1-K10) and three when it is clear (unvoiced: energy and K1-K4 only,
which is exactly what the TMS5220 wants). The mask says which of the ten
frames to actually emit, so one allophone's data serves several lengths.
The 26 low allophones carry three masks, one per pitch group, which is
how the same vowel gets three intonations.

Usage:
    python tools/allophone.py --phonemes "HELO"      # Ctrl-V phoneme mode
    python tools/allophone.py --codes 61 24 63 1A    # a phoneme string
    python tools/allophone.py --text "THE CAT."      # through stage 1
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0xD000

CHARMAP = 0xEA44        # indexed by (char | $80) - $A0, i.e. ord(c) - $20
PITCH_OFF = 0xEA7F      # $EA7F,X with X = the pitch group
VOWEL_VAR = 0xEA89
ALLO_PTR = 0xEA93       # 58 two-byte pointers
WIDTHS = 0xE906         # ten K-coefficient bit widths

N_ALLOPHONES = 58
SPEAK_EXTERNAL = 0x60


def load_tables(obj_path, prof=None):
    """The synthesiser's tables, from whichever image this is."""
    import profiles
    prof = prof or profiles.detect(obj_path)
    d = open(obj_path, "rb").read()
    b = prof["base"]
    g = lambda a: d[a - b]
    t = {"rom": d, "prof": prof, "base": b,
         "charmap": [g(prof["charmap"] + i) for i in range(0x3B)],
         "pitch_off": [g(prof["pitch_off"] + i) for i in range(10)],
         "vowel_var": [g(prof["vowel_var"] + i) for i in range(10)],
         "widths": list(prof["widths"]),
         "ptr": [g(prof["allo_ptr"] + 2 * i) | (g(prof["allo_ptr"] + 2 * i + 1) << 8)
                 for i in range(prof["n_allophones"])]}
    # The pointer table ends exactly where the allophone data begins in
    # both versions; if it does not, the addresses are wrong.
    if t["ptr"][0] != prof["allo_ptr"] + 2 * prof["n_allophones"]:
        raise SystemExit(
            "the allophone pointer table at $%04X does not end where its "
            "data begins -- wrong image, or wrong profile" % prof["allo_ptr"])
    return t


class Chip(object):
    """$E9AF and its byte accumulator, plus the bytes that come out.

    Bits go into $FFC7 from the top through ROR, so the first bit
    written ends up in bit 0 of the byte. $FFC5 is a walking sentinel
    that starts at 1 and signals the eighth bit when it falls out."""

    def __init__(self):
        self.out = []
        self.acc = 0x00                 # $FFC7
        self.bitcnt = 0x01              # $FFC5
        self.started = 0xFF             # $FFCA: $FF means "not speaking"

    def bits(self, a, n):               # $E9AF
        a &= 0xFF
        for _ in range(n):
            c = (a >> 7) & 1            # ASL $FFC6
            a = (a << 1) & 0xFF
            self.acc = ((self.acc >> 1) | (c << 7)) & 0xFF   # ROR $FFC7
            full = self.bitcnt & 0x80   # ASL $FFC5
            self.bitcnt = (self.bitcnt << 1) & 0xFF
            if full:
                self.bitcnt = 0x01      # ROL $FFC5 brings the 1 back
                self.out.append(self.acc)

    def byte(self, v):                  # JSR $FC8B, outside the bit packer
        self.out.append(v & 0xFF)


class Synth(object):
    """The zero-page-ish state of $FFAD-$FFE1, under readable names."""

    def __init__(self, t, chip=None, state=None):
        self.t = t
        self.chip = chip or Chip()
        rom = t["rom"]
        self.base = t["base"]
        self.prof = t["prof"]
        v = self.prof["vars"]
        # Without a Machine these come from the image, which is where
        # Textalker's own defaults live. With one, they are whatever the
        # Ctrl-E commands have set them to. v1.3 has no C/E and no D, so
        # those addresses are None and the defaults stand.
        def rd(k, default=0):
            a = v.get(k)
            return rom[a - self.base] if a is not None else default
        self.base_pitch = rd("base_pitch", 0x18)
        self.flags = rd("flags", 0x09)
        self.ffb0 = rd("monotone")               # F sets it, P clears it
        self.ffb1 = rd("compress")               # C sets it, E clears it
        self.ffbb = rd("delay")                  # the D inter-word delay
        self.group = rd("group", 2)
        self._rd = rd
        self.state = state
        if state is not None:
            self.base_pitch = state.pitch
            self.flags = state.flags
            self.ffb0 = state.monotone
            self.ffb1 = state.compress
            self.ffbb = state.delay
        self.index = 0                           # $FFBD: allophone index
        self.pitch = self.base_pitch             # $FFBF: the running pitch
        self.p_off = 0                           # $FFC0
        self.glide = 0                           # $FFC1
        self.variant = rd("variant", 1)          # $FFC2, 1 in the image
        self.p_step = (state.volume if state is not None
                       else rd("p_step"))        # the V command, and + and -
        self.p_set = 0                           # $FFC4
        self.mask = 0                            # $FFC8
        self.nframe = 0                          # $FFC9
        self.prev_flags = 0                      # $FFCC
        self.prev = [0] * 10                     # $FFCD-$FFD6
        self.ef = 0                              # $FFD7: flags | energy
        self.k = [0] * 10                        # $FFD8-$FFE1: K1-K10
        self.ch = 0                              # $FD90: the current character
        # The 6502 X register, which $E756 reads $EA89 through WITHOUT
        # having set it on every path: when $FFC4 is non-zero, $E713
        # takes the $E733 branch and never does LDX $FFBC, so the vowel
        # variant comes from $EA89 plus whatever X was left holding.
        # Normally that is the previous pitch group, but after "1N" it is
        # the character index, and $EA89+$2E is allophone data.
        self.x = self.group
        # The 6502 has ONE set of these, not one per utterance: the
        # pitch group, the pitch offset, the vowel variant, the running
        # pitch, the previous frame and even the X register all carry
        # across a flush. Ctrl-E L and K flush every character, so
        # rebuilding them from the image each time is wrong everywhere
        # but the first utterance -- and this restore has to come LAST,
        # after every field above has been given its default.
        if state is not None:
            for k, v in getattr(state, "carry", {}).items():
                setattr(self, k, v)

    CARRY = ("group", "index", "pitch", "p_off", "glide", "variant",
             "p_set", "prev_flags", "ef")

    def save(self, state):
        c = dict((k, getattr(self, k)) for k in self.CARRY)
        c["prev"] = list(self.prev)
        c["k"] = list(self.k)
        state.carry = c

    # ---------------------------------------------------------- plumbing

    def bits(self, a, n):
        """$E9AF, which counts down through X and so always returns with
        X at zero. That matters because $E756 indexes $EA89 with X."""
        self.chip.bits(a, n)
        self.x = 0


    def start(self):                             # $EA1A
        if self.chip.started == 0xFF:
            self.chip.byte(SPEAK_EXTERNAL)
            self.pitch = self.base_pitch
            self.prev_flags = 1
            self.chip.started = 1
            self.chip.acc = 0
            self.chip.bitcnt = 0x01

    def stop_frames(self):                       # $E98F
        for _ in range(6):
            self.chip.started = 0xFF
            self.bits(0xFF, 8)
        self.glide = 0
        self.variant = 0
        self.p_set = 0

    def set_pitch(self):                         # $E611
        self.p_set = self.group
        self.pitch = self.base_pitch
        self.glide = 0

    # ------------------------------------------------------- the frames

    def unpack(self, data, y):                   # $E7ED
        """One packed frame from data[y+1:]. Returns the new y.

        `data` is the image from the allophone's pointer onward, not a
        fixed window: Ctrl-E D leaves the delay in $FFBC ($E6CA), so
        $EA89,X can be read far past its table and hand back a vowel
        variant of almost anything, and ($04),Y then walks wherever that
        points. Clamping it changes the sound."""
        y += 1
        if self.variant:
            self.variant = 1
        b0 = data[y]
        # $E7FE-$E804 is CLC, ADC $FFC3, ADC $FFC2 -- and the second ADC
        # takes the carry out of the first, so a p_step of $FF (one '-')
        # borrows and then gives the borrow straight back. Dropping that
        # carry makes every frame after a '-' one energy step too quiet.
        e = ((b0 & 0x3C) >> 2) + self.p_step
        e, c = e & 0xFF, e > 0xFF
        e = e + self.variant + (1 if c else 0)
        e &= 0xFF
        if e == 0 or e > 0x7F:                   # BEQ / BMI
            e = 1
        elif e >= 0x0F:
            e = 0x0E
        self.ef = ((e << 2) & 0xFF) | (b0 & 0xC0)
        k1 = b0 & 0x03
        y += 1
        b1 = data[y]
        k1 = (k1 << 3) | (b1 >> 5)               # 5 bits
        self.k[0] = (k1 << 2) & 0xFF
        self.k[1] = ((b1 & 0x1F) << 2) & 0xFF
        y += 1
        b2 = data[y]
        self.k[2] = (b2 & 0xF0) >> 1
        self.k[3] = (b2 & 0x0F) << 3
        if not (self.ef & 0x80):                 # $E858: an unvoiced frame
            return y                             # stops after three bytes
        y += 1
        b3 = data[y]
        self.k[4] = (b3 & 0xF0) >> 1
        self.k[5] = (b3 & 0x0F) << 3
        y += 1
        b4 = data[y]
        self.k[6] = (b4 & 0xF0) >> 1
        self.k[7] = ((b4 & 0x0E) << 3) & 0xFF
        y += 1
        b5 = data[y]
        self.k[8] = (b5 & 0xE0) >> 1
        self.k[9] = ((b5 & 0x1C) << 2) & 0xFF
        return y

    def emit_pitch(self):                        # $E945
        """The pitch is a running value, not a per-frame one.

        An unvoiced frame resets the run and writes a pitch of zero,
        which is what the TMS5220 reads as "unvoiced" -- note $E95D
        branches to the conversion at $E982 with A still 0, skipping the
        clamp entirely."""
        if not (self.ef & 0x80):
            self.pitch = self.base_pitch
            self.glide = 0
            self.variant = 0
            self.p_set = 0
            self._pitch_out(0)
            return
        self.pitch = (self.pitch + self.glide) & 0xFF     # $E95F
        self.emit_pitch_run()

    def emit_pitch_run(self):                    # $E969
        a = (self.pitch + self.p_off) & 0xFF
        if a == 0 or a & 0x80:                   # $E970 / $E976
            a = 1
        elif a >= 0x40:                          # $E97C
            a = 0x3F
        self._pitch_out(a)

    def _pitch_out(self, a):                     # $E982
        # EOR #$FF then ADC #$41 with the carry clear is 320 - a, which
        # in eight bits is $40 - a: Textalker holds a period and the chip
        # wants its complement.
        self.bits(((0x40 - a) << 2) & 0xFF, 6)

    def emit_frame(self):                        # $E8C4
        self.bits((self.ef << 2) & 0xFF, 5)     # energy, then repeat=0
        self.emit_pitch()
        self.prev_flags = self.ef
        i = 0
        while True:
            self.prev[i] = self.k[i]
            self.bits((self.k[i] << 1) & 0xFF, self.t["widths"][i])
            i += 1
            if i == 4 and not (self.ef & 0x80):      # $E8F2: unvoiced stops
                return
            if i >= 10:
                return

    def emit_interpolated(self):                 # $E910
        a = (self.ef + self.prev_flags) & 0xFF
        self.bits(((a << 1) & 0xFF) & 0xF0, 5)
        self.emit_pitch_run()
        for i in range(10):
            self.bits((self.k[i] + self.prev[i]) & 0xFF, self.t["widths"][i])

    def maybe_emit(self):                        # $E893
        take = (self.mask & 0x80) != 0
        self.mask = (self.mask << 1) & 0xFF
        if not take:
            return
        if (self.ef & 0xC0) == 0x80:             # voiced, not a repeat
            if (self.prev_flags & 0xC0) == 0x80 and not self.ffb1:
                self.emit_interpolated()         # $E8BD
        else:
            if self.ffb1:
                # $E8A6's BEQ skips; $EDB9's BEQ emits. See the profile.
                odd = self.nframe & 1
                if bool(odd) == bool(self.prof.get("compress_emit_even")):
                    return
        self.emit_frame()

    # ----------------------------------------------------- one allophone

    def frames(self):                            # $E7A3
        idx = self.index
        p = self.t["ptr"][idx]
        data = self.t["rom"][p - self.base:]
        self.mask = data[0]
        y = 0
        if 2 * idx + 1 < 0x34:                   # $E7C4: the 26 low allophones
            self.mask = data[self.variant]       # one mask per pitch group
            y = 2
        self.nframe = 9
        if self.prof.get("frames_pre_dec"):      # $ECEC: DEC, then BNE
            while True:
                self.nframe -= 1
                if not self.nframe:
                    break
                y = self.unpack(data, y)
                self.maybe_emit()
        else:
            while self.nframe >= 0:              # $E7E0: DEC, then BPL
                y = self.unpack(data, y)
                self.maybe_emit()
                self.nframe -= 1

    def pitch_for_frame(self):                   # $E713
        self.start()
        if self.index >= 0x24:
            return
        if self.index < 0x1A:
            self.x = self.group                  # $E721
            if self.group == 0:
                self.index = 4                   # $E726
        self.p_off = self.p_set
        if self.p_off:
            self.p_off = (self.p_off - 5) & 0xFF     # $E733: 5 is neutral
        else:
            self.x = self.group                  # $E73C
            # $EA7F,X, read from the image rather than a ten-entry list:
            # a stress marker can leave $FFBC as high as 15 ($DC92 masks
            # with $0F), and the 6502 happily indexes past the table.
            self.p_off = (self.t["rom"][(self.prof["pitch_off"] + self.x)
                                        - self.base] + self.p_off) & 0xFF
        if self.ffb0:
            self.p_off = 0
            self.glide = 0
        self.variant = self.t["rom"][(self.prof["vowel_var"] + self.x)
                                     - self.base]

    def specials(self):                          # $E6B0. True = handled.
        i = self.index
        if i == 0x7F:
            return True
        if i == 0x00:
            self.pause()
            return True
        if i == 0x19:
            if self.ffbb == 0:
                return True
            self.group = self.ffbb
            self.pause()
            return True
        if i == 0xFF:                            # end of utterance
            self.stop_frames()
            self.flags &= 0x7D
            return True
        if i >= 0x4F:                            # < and > : the pitch glide
            self.glide = ((i >> 4) - 5) & 0xFF
            return True
        if i == 0x37:                            # '+'
            self.p_step = (self.p_step + 1) & 0xFF
            return True
        if i == 0x3F:                            # '-'
            self.p_step = (self.p_step - 1) & 0xFF
            return True
        return False

    def pause(self):                             # $E9FC
        if self.group == 0:
            self.bits(0x00, 4)
        else:
            self.stop_frames()

    def stops(self):                             # $E674
        if not 0x2A <= self.index < 0x32:
            return
        c = (self.ch - 0xA0) & 0xFF
        bump = False
        if c == 0x0B:
            bump = False
        elif c >= 0x1F or c < 0x10:
            self.x = c                           # $E691 TAX
            v = self.t["rom"][(self.prof["charmap"] + c) - self.base]
            bump = (v == 0) or (v >= 0x1D)
        if bump:
            self.index = (self.index + 8) & 0xFF
        self.stop_frames()

    def speak_allophone(self):                   # $E5C7
        """True if an allophone was actually spoken.

        $E5CA branches straight to the RTS when $E6B0 handled the code
        itself, skipping the $E5D5 test -- so a pause, a glide or an end
        marker does NOT re-enter $E5AA, and anything the pipeline was
        holding stays held. That is why a lone "," speaks one stop frame
        and then swallows the end of the line."""
        if self.specials():
            # 3.1.3's $E5CA branches to the RTS, skipping the $E5D5
            # re-entry, so a pause or an end marker leaves the pipeline's
            # held sound held. v1.3 merges the two routines at $EAD6 and
            # has no such branch, so it re-enters either way.
            return self.prof["metas"] == "v13"
        self.stops()
        self.pitch_for_frame()
        self.frames()
        return True


# -------------------------------------------------------- the two drivers

def speak_codes(codes, t, chip=None, state=None):
    """$DBD6: the phoneme string at $FEAD, as stage 1 leaves it.

    The buffer is NOT cleared between utterances -- stage 1 overwrites
    only as many bytes as it produces, and $FFAC says how many. That
    matters because $DC71's one-code lookahead reads $FEAE,Y, which for
    the LAST code is one past the end, so it sees a leftover from the
    previous utterance. In "XH" spoken letter by letter, H's final $64
    looks past its own string and finds the $5B that X left there, which
    is in $51-$B2, so $DC7C fakes a following "T" and $E674 gives the
    stop its +8 release variant -- allophone $38 rather than $30."""
    s = Synth(t, chip, state)
    n0 = len(s.chip.out)
    if state is not None:
        buf = state.phbuf
    else:
        buf = [0] * 256
    for i, c in enumerate(codes[:256]):
        buf[i] = c
    # $E240 leaves the phoneme count in X ($FFAC), and $DBD6 does not
    # touch X before the first $E5C7 -- so that, not whatever $E9AF left
    # behind in the previous utterance, is what $E756 reads $EA89
    # through when $E713 takes its short path.
    s.x = len(codes) & 0xFF
    started = t["rom"][t["prof"]["vars"]["utt_flag"] - t["base"]]
    y = 0
    while y < len(codes):
        prev = codes[y - 1] if y else 0
        # $DBF4 branches PAST this when the previous code is a word
        # separator, so the pitch-7 reset arms only when speech is not
        # already running -- at the start of the line, and again after
        # anything that emitted stop frames and put $FFCA back to $FF.
        if prev != 0x19 and s.chip.started == 0xFF:
            started = 0xFF                       # $DBFF
        if started == 0xFF and codes[y] < 0x4E:  # $DC09
            s.group = 7
            s.set_pitch()
            started = 0
        c = codes[y]
        if c < 0x1A:                             # $DC22
            s.index, s.group = c, 1
        elif c < 0x34:
            s.index, s.group = c - 0x1A, 2
        elif c < 0x4E:
            s.index, s.group = c - 0x34, 3
        elif c < 0x5E:                           # $DC48: group unchanged
            s.index = c - 0x34
        elif c == 0x7F:                          # $DC60
            y += 1
            continue
        elif c < 0xB3:                           # $DC6B, with a lookahead
            s.index = c - 0x34
            nxt = buf[y + 1] if y + 1 < len(buf) else 0
            s.ch = 0xD4 if 0x51 <= nxt < 0xB3 else 0xC1
            s.flags &= 0x7F
            s.speak_allophone()
            y += 1
            continue
        else:                                    # $DC92: the stress markers
            s.group = c & 0x0F
            s.set_pitch()
            y += 1
            continue
        s.flags &= 0x7F
        s.speak_allophone()
        y += 1
    s.index = 0xFF                               # $DC9D: end of utterance
    s.speak_allophone()
    if state is not None:
        state.flags = s.flags
        s.save(state)
    return s.chip.out[n0:]


def _digit(s):                                   # $E5E3
    """'none' if not a digit, 'done' if it only set the pitch, 'speak'."""
    if not 0xB0 <= s.ch < 0xBA:
        return "none"                            # $E60D
    s.group = s.ch & 0x0F
    if not (s.flags & 0x80):
        s.set_pitch()                            # $E611, and A comes back 0
        return "done"
    if s.index < 0x2A:                           # a vowel is pending: speak it
        s.flags &= 0x7F                          # again at the new pitch
    return "speak"


def _lookup(s):                                  # $E623
    """'speak' or 'defer'. Phoneme mode runs one character behind.

    A vowel or a stop consonant is HELD until the next character
    arrives, because $E674 needs to know what follows before it can
    choose a stop's release variant. The held sound is spoken on the
    next character, and $E5D5-$E5DF then re-enters $E5AA so that
    character is processed in its turn."""
    if s.flags & 0x80:                           # something is pending
        s.group = 2                              # $E62A
        if s.ch == 0xD2 and 0 < s.index < 9:     # $E62F: R colours a vowel
            s.index = (s.index + 0x10) & 0xFF
            s.flags |= 0x80
            return "defer"
        return "speak"
    s.x = s.ch - 0xA0                            # $E647 TAX
    s.index = s.t["rom"][(s.prof["charmap"] + s.x) - s.base]
    if s.index >= 0x32 or 0x1A <= s.index < 0x2A:
        s.flags &= 0x7F                          # $E660
        return "speak"
    s.flags |= 0x80                              # $E66A
    return "defer"


def speak_phonemes(text, t, chip=None, state=None):
    """$E5AA: Ctrl-V phoneme mode.

    A trailing "." is appended because that is what the line ends as:
    the driver turns the carriage return into $AE, whose character-map
    entry is $FF, which is the end-of-utterance code. It is also what
    flushes the held sound."""
    s = Synth(t, chip, state)
    n0 = len(s.chip.out)
    for ch in text.upper() + ".":
        c = ord(ch) | 0x80
        if not 0xA0 <= c < 0xDB:                 # $E5AF/$E5B6
            continue
        s.ch = c
        while True:
            act = _digit(s)
            if act == "done":
                break
            if act == "none" and _lookup(s) == "defer":
                break
            if s.speak_allophone() and (s.flags & 0x80):
                s.flags &= 0x7F                  # $E5D5: the held sound went
                continue                         # out; now do this character
            break
    if state is not None:
        state.flags = s.flags
        s.save(state)
    return s.chip.out[n0:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--phonemes")
    ap.add_argument("--codes", nargs="*")
    ap.add_argument("--text")
    ap.add_argument("--version", default="3.1.3", choices=("3.1.3", "1.3"))
    ap.add_argument("--obj")
    args = ap.parse_args()

    import profiles
    prof = profiles.PROFILES[args.version]
    args.obj = args.obj or prof["obj"]
    t = load_tables(args.obj, prof)
    if args.phonemes:
        out = speak_phonemes(args.phonemes, t)
    elif args.codes:
        out = speak_codes([int(c, 16) for c in args.codes], t)
    elif args.text:
        import rule_walk as RW
        from rule_probe import load_rules
        by = RW.build_sections(load_rules(args.obj, prof))
        out = speak_codes(RW.walk_line(args.text, by, prof=prof), t)
    else:
        sys.exit("give --phonemes, --codes or --text")
    print(" ".join("%02x" % b for b in out))


if __name__ == "__main__":
    main()
