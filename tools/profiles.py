#!/usr/bin/env python3
"""Where everything lives in each Textalker image, and how the two differ.

The tools were written against 3.1.3 and hardcoded its addresses. v1.3
is the same architecture -- the same phoneme alphabet ("CAT" is
61 24 63 in both), rules encoded the same way (pattern bytes >= $80,
payload bytes < $80, delimited by the transition), the same dispatch
table indexed by 2*ord(c), the same 128-byte attribute table with one
bit per class -- but it loads at a different address, everything sits
somewhere else, and two things are genuinely absent:

**v1.3 has no syllabifier.** Watching $F59F while it speaks "SORTED CAT"
shows the text going in and nothing else happening: no $E6 sentinels, no
shifting, no prefix or suffix stripping. So $9B and $E6 have no meaning
there, and its letter-to-sound rules are written to compensate -- which
is why the two versions disagree about words like COPY and BEGIN, and
why ANGRY loses its R in 1.3.

**v1.3's metacharacter set stops at $FA.** Its context matcher is a
chain of comparisons ($DB2B) rather than 3.1.3's RTS-trick table, and
everything above $FA falls into one handler. There is no $FC, $FD, $FE
or $FF, and no $9C wildcard.

    meaning                       v3.1.3   v1.3
    word boundary                 $F3      $F3
    one consonant                 $F4      $F4
    one front vowel (E, I, Y)     $F5      $F5
    one voiced consonant          $F6      $F6
    one or more vowels, greedy    $F7      $F7
    zero or more consonants       $F8      $F8 and $F9
    bit $01, or C/S/T of a digraph $FA     $FA
    bit $10, or the H of one      $FB      $FB and above
    an inflectional ending        $F9      --
    one stop consonant            $FD      --
    skip sentinels                $FC      --
    syllable counter tests        $FE $FF  --
    the syllable sentinel         $9B $E6  --
    wildcard in context           $9C      --

**The synthesiser is the same in both.** The allophone data at $EB07
(3.1.3) and $F06D (1.3) is byte-identical, and so are the character map,
the pitch-offset table and the vowel-variant table. Street Electronics
changed the RULES between versions, not the voice. So mixing is real but
lopsided: `--rules` changes the pronunciation, `--voice` changes nothing
unless someone supplies a modified image.

What each version's rules sound like, from the phoneme strings:

    ANGRY     3.1.3  24 53 60 4F 1C     1.3  24 53 60 1C
    COPY      3.1.3  61 3B 62 1C        1.3  61 1B 62 1C
    BEGIN     3.1.3  5E 26 60 27 52     1.3  5E 0D 65 27 52
    NICKNAME  3.1.3  52 27 7F 7F 52...  1.3  52 27 61 52 1F 51 7F

1.3 drops the R of ANGRY entirely -- its NGR rule consumes three letters
and emits only NG and G. 3.1.3 loses the K of NICKNAME to two silencing
rules that meet ($7F $7F where 1.3 keeps $61). 1.3 says BEGIN with a J
($65) where 3.1.3 uses a G ($60), and gives COPY the right vowel where
3.1.3 uses the one from "boat". All four are Jayson's observations, and
all four fall out of the tables.
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PROFILES = {
    "3.1.3": {
        "name": "3.1.3",
        "obj": os.path.join(ROOT, "roms", "textalker.obj.bin"),
        "ram": os.path.join(ROOT, "roms", "textalker.ram.bin"),
        "base": 0xD000,
        # stage 1
        "dispatch": 0xEFDF,          # entry(c) = dispatch + 2*ord(c)
        "rule_lo": 0xF0AB,
        "rule_hi": 0xFCC4,
        "attrs": 0xEF24,
        "punct_names": 0xEF8B,       # {ptr_lo, ptr_hi, char} triples
        "punct_all": 0x53,           # $E27C: where Ctrl-E A starts the search
        "punct_most": 0x4A,          # ...and where Ctrl-E M does
        "inbuf": 0xFDAC,
        "phbuf": 0xFEAD,
        "phlen": 0xFFAC,
        "syllabifier": True,
        "metas": "full",
        "payload_pc": 0xE238,        # for tools/rule_probe.py
        # stage 2
        "charmap": 0xEA44,           # Ctrl-V map, base + ord(c) - $20
        "pitch_off": 0xEA7F,
        "vowel_var": 0xEA89,
        "allo_ptr": 0xEA93,
        "n_allophones": 58,
        "widths": [5, 5, 4, 4, 4, 4, 4, 3, 3, 3],   # the table at $E906
        "vars": {"flags": 0xFFAD, "base_pitch": 0xFFAF, "monotone": 0xFFB0,
                 "compress": 0xFFB1, "delay": 0xFFBB, "group": 0xFFBC,
                 "variant": 0xFFC2, "p_step": 0xFFC3, "utt_flag": 0xFDA2},
        "commands": "ABCDEFKLMNOPRSTVW;,I?",
        "capital_pitch": True,       # $DB7F lifts a capital by 5
        # what tools/frame_diff.py watches: the allophone index, the
        # frame's flags|energy byte, K1, and the PC of the FIRST of the
        # two writes to the energy byte ($E818, before the flag bits are
        # ORed in), which is the one to ignore.
        "frame": {"idx": 0xFFBD, "ef": 0xFFD7, "k": 0xFFD8, "first_pc": "E818"},
    },
    "1.3": {
        "name": "1.3",
        "obj": os.path.join(ROOT, "roms", "textalker_v13.obj.bin"),
        "ram": os.path.join(ROOT, "roms", "textalker_v13.ram.bin"),
        "base": 0xD400,
        "dispatch": 0xDECD,
        "rule_lo": 0xDF99,
        "rule_hi": 0xEA80,
        "attrs": 0xDE13,
        "punct_names": 0xDE79,
        "punct_all": 0x53,
        "punct_most": 0x4A,
        "inbuf": 0xF59F,
        "phbuf": 0xF492,
        "phlen": 0xF48A,
        "syllabifier": False,
        "metas": "v13",
        "payload_pc": 0xDD46,
        # Stage 2 is the SAME code at different addresses, down to the
        # pitch complement's ADC #$41 -- except that v1.3 unrolls the
        # parameter emitter, so the ten K widths are inline constants
        # ($EE96 onward) rather than a table.
        "charmap": 0xEFAA,
        "pitch_off": 0xEFE5,
        "vowel_var": 0xEFEF,
        "allo_ptr": 0xEFF9,
        "n_allophones": 58,
        "widths": [5, 5, 4, 4, 4, 4, 4, 3, 3, 3],
        "vars": {"flags": 0xF6A1, "base_pitch": 0xF6C8, "monotone": 0xF6C9,
                 "compress": 0xF6C7, "delay": None, "group": 0xF6A2,
                 "variant": 0xF6A8, "p_step": 0xF6A9, "utt_flag": 0xF592},
        # 13 commands, in a comparison chain at $D891-$D957 rather than a
        # jump table: no D (inter-word delay), no K or N (spell), no R
        # (repeat filter), and none of the ";,I?" index marks. There is
        # also no ADC #$05 anywhere in the image, so no capital lift.
        "commands": "ABCEFLMOPSTVW",
        "capital_pitch": False,
        "frame": {"idx": 0xF6A3, "ef": 0xF6BC, "k": 0xF6BD, "first_pc": "ED27"},
        # $ECEC decrements the frame counter BEFORE unpacking and loops
        # on BNE, so an allophone yields eight frames, not the ten
        # $E7E0's BPL gives. Eight is the honest number -- the mask is
        # one byte -- and 3.1.3's last two passes emit nothing because
        # the mask has shifted itself empty by then.
        #
        # It does move the counter under Ctrl-E C, which tests its
        # bottom bit: 8,7,..,1 here against 9,8,..,0 there. The TEST is
        # inverted to match -- $EDB9's BEQ emits, $E8AB's BEQ skips --
        # so the two inversions cancel and both versions drop the same
        # half of the frames. Model either one alone and compressed
        # speech comes out as the other half.
        "frames_pre_dec": True,
        "compress_emit_even": True,
        # The greedy runs stop at a FIXED index, not at the end of the
        # text: $DC90 and $DD19 both do CPX #$8F. 3.1.3 instead copies
        # $FD9E into $FD99 ($E3EF) and compares against that, so its
        # runs stop where the text does. The difference only shows once
        # a rule is reading past the end -- {F3}{F8}F{F8}{F3} matches
        # "F" against a bounded run and does not against the real one,
        # which is why "FR" flushed by a Ctrl-E says "eff" instead of
        # "f-r" if the bound is modelled.
        "fwd_run_limit": 0x8F,
    },
    # Textalker 3.1.2, off Jayson's PIZZA.DSK. Its engine is 326 bytes
    # shorter than 3.1.3's with EVERY table moved, which is what forced
    # src/tk_image.c to stop listing addresses and start finding them.
    #
    # The addresses below were not read off a disassembly: they are what
    # that discovery reports (build/native/dump_profile), recorded here
    # so the PYTHON oracles can run on this build too. The point of
    # having them written down is that the C and the Python then agree
    # about an image neither was written against -- and tk_diff.py can
    # check both against the emulator, which was written against none of
    # them.
    #
    # The workspace addresses are 3.1.3's. That they are also 3.1.2's is
    # the one thing here that discovery cannot establish, so it is
    # MEASURED instead: tk_diff.py --version 3.1.2 --emulator is the
    # test, and it is why this entry exists.
    "3.1.2": {
        "name": "3.1.2",
        "obj": os.path.join(ROOT, "roms", "textalker_312.obj.bin"),
        "ram": os.path.join(ROOT, "roms", "textalker_312.ram.bin"),
        "base": 0xD000,
        "attrs": 0xEDD5,
        "dispatch": 0xEE90,
        "rule_lo": 0xEF5C,
        "rule_hi": 0xFEB3,
        "punct_names": 0xEE3C,
        "punct_all": 0x53,
        "punct_most": 0x4A,
        "charmap": 0xE8F5,
        "pitch_off": 0xE930,
        "vowel_var": 0xE93A,
        "allo_ptr": 0xE944,
        "n_allophones": 58,
        "inbuf": 0xFDAC,
        "phbuf": 0xFEAD,
        "phlen": 0xFFAC,
        "payload_pc": 0,
        "syllabifier": True,
        "metas": "full",
        "capital_pitch": True,
        "commands": "ABCDEFKLMNOPRSTVW;,I?",
        "vars": {"flags": 0xFFAD, "base_pitch": 0xFFAF, "monotone": 0xFFB0,
                 "compress": 0xFFB1, "delay": 0xFFBB, "group": 0xFFBC,
                 "variant": 0xFFC2, "p_step": 0xFFC3, "utt_flag": 0xFDA2},
        "widths": [5, 5, 4, 4, 4, 4, 4, 3, 3, 3],
        "frame": {"idx": 0xFFBD, "ef": 0xFFD7, "k": 0xFFD8, "first_pc": None},
    },
}


def load_rom(prof):
    return open(prof["obj"], "rb").read()


def sections(prof, rom=None):
    """How many printable characters reach the rule area -- the sanity
    check that says whether a profile fits an image."""
    rom = rom or load_rom(prof)
    b, d = prof["base"], prof["dispatch"]
    n = 0
    for c in range(0x20, 0x66):
        i = d + 2 * c - b
        if not 0 <= i < len(rom) - 1:
            return 0
        p = rom[i] | (rom[i + 1] << 8)
        if prof["rule_lo"] <= p <= prof["rule_hi"]:
            n += 1
    return n


def attrs_ok(prof, rom=None):
    """Does the 128-byte attribute table look like one?

    A dispatch table can point into the rule area by coincidence -- both
    layouts score 70 of 70 on the other's image -- so the vowel bit is
    the discriminator: exactly A E I O U Y carry $80 among the letters,
    and every letter carries either that or the consonant bit."""
    rom = rom or load_rom(prof)
    a = prof["attrs"] - prof["base"]
    if not 0 <= a < len(rom) - 128:
        return False
    t = rom[a:a + 128]
    for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
        want = 0x80 if c in "AEIOUY" else 0x40
        if not t[ord(c)] & want:
            return False
        if c not in "AEIOUY" and t[ord(c)] & 0x80:
            return False
    return True


def loader_init(prof, addrs):
    """The values Textalker's loader writes into its variables at boot.

    3.1.3's OBJ image already carries its startup state, so reading a
    variable's initial value straight out of the image works. v1.3's
    does not: its loader sets eleven variables at $9410-$9445 before the
    first character arrives, and the image byte behind the flags is $00
    -- speech OFF -- where the running machine has $09. Taking the OBJ
    byte there makes the port silent for any text that does not begin
    with an explicit Ctrl-E T.

    The block is LDA #imm followed by a run of STA abs, which is
    distinctive enough to read out of the loader directly rather than
    recording measured constants. Only addresses in `addrs` are
    returned, so a chance A9/8D pair in data cannot invent a variable.
    """
    ram = open(prof["ram"], "rb").read()
    out = {}
    i = 0
    while i < len(ram) - 2:
        if ram[i] != 0xA9:
            i += 1
            continue
        imm = ram[i + 1]
        j = i + 2
        while j + 2 < len(ram) and ram[j] == 0x8D:
            a = ram[j + 1] | (ram[j + 2] << 8)
            if a in addrs:
                out[a] = imm
            j += 3
        i = j if j > i + 2 else i + 2
    return out


def detect(obj_path):
    """Which profile an image is."""
    for prof in PROFILES.values():
        p = dict(prof, obj=obj_path)
        try:
            if sections(p) >= 26 and attrs_ok(p):
                return p
        except (IOError, IndexError):
            pass
    raise SystemExit("%s matches no known Textalker layout" % obj_path)


def get(spec, obj_path=None):
    """A profile by name ("3.1.3", "1.3") or by detection from a path."""
    if spec in PROFILES:
        p = dict(PROFILES[spec])
        if obj_path:
            p["obj"] = obj_path
        return p
    return detect(spec if spec else obj_path)


def main():
    for k in sorted(PROFILES):
        p = PROFILES[k]
        ok = os.path.exists(p["obj"])
        print("%-6s base $%04X  dispatch $%04X  attrs $%04X  %s  %s"
              % (k, p["base"], p["dispatch"], p["attrs"],
                 "syllabifier" if p["syllabifier"] else "no syllabifier",
                 ("%d sections, attrs %s" % (sections(p), "ok" if attrs_ok(p) else "BAD"))
                 if ok else "(image not present)"))


if __name__ == "__main__":
    main()
