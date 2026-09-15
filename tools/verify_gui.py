#!/usr/bin/env python3
"""Check the EchoTalk GUI against the EchoTalk NVDA add-on itself.

    python tools/verify_gui.py [ROMDIR [ADDON_DLL]]

ROMDIR holds Textalker image pairs (<stem>.ram.bin + <stem>.obj.bin).
ADDON_DLL is the add-on's shipped echotalk64.dll. Both default to the
unpacked add-on beside this repository, ..\\echotalk\\synthDrivers\\echotalk,
and are read there -- nothing is copied, committed or shipped.

The reference is the add-on's driver, nvda-addon/synthDrivers/echotalk/
__init__.py. Its speaking path is reproduced below with NVDA removed:
_openVoice, _applyAll with its conversions, speak()'s sanitising, and
_speakOne's read loop in blocks of 1024. `AddonDriver` is that copy, and
the comments name the method each piece comes from. It runs on the DLL
the add-on ships, not on anything built here.

1. For a matrix over both voices and every setting, the GUI's --selftest
   WAV holds exactly the samples the driver would hand to NVDA's player on
   a freshly opened voice, at the same sample rate.
2. The settings the add-on does not expose (frame rate, reading and
   punctuation modes, chunk size, raw text, caret-notation commands) are
   checked the same way, through the same DLL, with the extra library
   calls the GUI makes.
3. A preset written by the GUI and rendered from the file matches the same
   settings rendered directly.
4. Batch render writes one WAV per non-blank line, each identical to the
   add-on speaking that line; the pitch sweep writes pitch 00-99, each
   identical to the add-on speaking the text after a raw Ctrl-E nP (nF when
   monotone), including the undocumented pitches above 63.

Exit status 0 when every case matches.
"""

from __future__ import annotations

import ctypes
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "gui-native" / "build" / "echotalk_gui-x64.exe"
DEFAULT_ADDON = ROOT.parent / "echotalk" / "synthDrivers" / "echotalk"

SAMPLES_PER_READ = 1024
CHIP_HZ = 8000
PITCH_MAX, VOLUME_MAX, DELAY_MAX, REPEAT_MAX = 63, 15, 15, 99
_UNSAFE = re.compile(r"[\x04\x05\x16]")


# --- the add-on's helpers, verbatim -----------------------------------------

def _toCard(pct, maxVal):
    return max(0, min(maxVal, int(round(pct * maxVal / 100.0))))


def _toPct(card, maxVal):
    return max(0, min(100, int(round(card * 100.0 / maxVal))))


def _toMult(pct):
    return 2.0 ** ((pct - 50) / 25.0)


class AddonDriver:
    """The add-on's library handling and speaking path, minus NVDA."""

    def __init__(self, dll: Path):
        # _EchoTalkDLL.__init__
        self.lib = lib = ctypes.cdll.LoadLibrary(str(dll))
        p = ctypes.c_void_p
        lib.echotalk_create.restype = p
        lib.echotalk_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                                        ctypes.c_char_p, ctypes.c_size_t]
        lib.echotalk_destroy.restype = None
        lib.echotalk_destroy.argtypes = [p]
        lib.echotalk_version.restype = ctypes.c_char_p
        lib.echotalk_version.argtypes = [p]
        for fname, arg in (
            ("set_pitch", ctypes.c_int), ("set_flat", ctypes.c_int),
            ("set_volume", ctypes.c_int), ("set_word_delay", ctypes.c_int),
            ("set_repeat_filter", ctypes.c_int), ("set_compressed", ctypes.c_int),
            ("set_frame_rate", ctypes.c_int), ("set_raw", ctypes.c_int),
            ("set_letter_mode", ctypes.c_int), ("set_punctuation", ctypes.c_int),
            ("set_speed", ctypes.c_double),
            ("set_clock_multiplier", ctypes.c_double),
            ("set_sample_rate", ctypes.c_uint),
            ("set_chunk_size", ctypes.c_uint),
        ):
            fn = getattr(lib, "echotalk_" + fname)
            fn.restype, fn.argtypes = ctypes.c_int, [p, arg]
        lib.echotalk_sample_rate.restype = ctypes.c_uint
        lib.echotalk_sample_rate.argtypes = [p]
        lib.echotalk_speak.restype = ctypes.c_int
        lib.echotalk_speak.argtypes = [p, ctypes.c_char_p]
        lib.echotalk_read.restype = ctypes.c_size_t
        lib.echotalk_read.argtypes = [p, ctypes.POINTER(ctypes.c_int16), ctypes.c_size_t]
        lib.echotalk_overruns.restype = ctypes.c_uint
        lib.echotalk_overruns.argtypes = [p]

    def render(self, loader: Path, obj: Path, g: dict, text: str, prefix: str = ""):
        """One utterance on a freshly opened voice. `g` is in the GUI's units."""
        lib = self.lib
        err = ctypes.create_string_buffer(256)
        # _openVoice
        h = lib.echotalk_create(str(loader).encode("utf-8"), str(obj).encode("utf-8"),
                                err, len(err))
        if not h:
            raise RuntimeError(err.value.decode())
        try:
            speed, clock = _toMult(g["rate"]), _toMult(g["clock"])
            # _effectiveSamplerate
            rate = max(int(g["srate"]), int(CHIP_HZ * clock + 0.5))
            # _applyAll
            lib.echotalk_set_pitch(h, g["pitch"])
            lib.echotalk_set_flat(h, 1 if g["mono"] else 0)
            lib.echotalk_set_volume(h, g["volume"])
            lib.echotalk_set_word_delay(h, g["delay"])
            lib.echotalk_set_repeat_filter(h, g["repeat"])
            lib.echotalk_set_compressed(h, 1 if g["comp"] else 0)
            lib.echotalk_set_speed(h, speed)
            lib.echotalk_set_clock_multiplier(h, clock)
            lib.echotalk_set_sample_rate(h, rate)
            # Beyond the add-on: the library calls the GUI adds, each only
            # when moved off the library default.
            if g["frame"]:
                lib.echotalk_set_frame_rate(h, g["frame"])
            if g["reading"]:
                lib.echotalk_set_letter_mode(h, g["reading"] - 1)
            if g["punct"]:
                lib.echotalk_set_punctuation(h, g["punct"] - 1)
            if g["chunk"] != 80:
                lib.echotalk_set_chunk_size(h, g["chunk"])
            if g["raw"]:
                lib.echotalk_set_raw(h, 1)
            # speak()
            if g["obey"]:
                text = decode_carets(text)
            else:
                text = _UNSAFE.sub(" ", text)
            # The GUI's pitch sweep puts a raw command ahead of the
            # prepared text; everything else passes an empty prefix.
            lib.echotalk_speak(h, prefix.encode("ascii") + text.encode("utf-8"))
            # _speakOne
            buf = (ctypes.c_int16 * SAMPLES_PER_READ)()
            out = bytearray()
            while True:
                n = lib.echotalk_read(h, buf, SAMPLES_PER_READ)
                if not n:
                    break
                out += ctypes.string_at(buf, n * 2)
            return bytes(out), lib.echotalk_sample_rate(h), lib.echotalk_overruns(h)
        finally:
            lib.echotalk_destroy(h)


def decode_carets(text: str) -> str:
    """The GUI's caret notation, written independently of its C++."""
    codes = {"E": "\x05", "D": "\x04", "V": "\x16", "^": "^"}
    out, i = [], 0
    while i < len(text):
        c = text[i]
        if c == "^" and i + 1 < len(text) and text[i + 1].upper() in codes:
            out.append(codes[text[i + 1].upper()])
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


# --- the GUI ------------------------------------------------------------------

DEFAULTS = dict(rate=50, pitch=24, volume=12, delay=0, repeat=99, clock=50, srate=8000,
                mono=0, comp=0, frame=0, reading=0, punct=0, chunk=80, raw=0, obey=0)
ORDER = ["rate", "pitch", "volume", "delay", "repeat", "clock", "srate", "mono", "comp",
         "frame", "reading", "punct", "chunk", "raw", "obey"]


def values(g):
    return [str(int(g[k])) for k in ORDER]


def read_wav(path: Path):
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:16] != b"WAVEfmt ":
        raise ValueError("not a WAV")
    rate = struct.unpack_from("<I", data, 24)[0]
    size = struct.unpack_from("<I", data, 40)[0]
    return data[44:44 + size], rate


TEXTS = {
    "sentence": "Hello. The quick brown fox jumps over the lazy dog, 1985.",
    "utf8": "Café – “quoted”, naïve résumé: 42%.",
    "controls": "Pitch \x0540P should not change, nor \x04 2F this, nor \x16 that.",
    "letter": "a",
    "run": "EEEEEEEEE, and ***** stars.",
    "paragraph": ("Textalker speaks when its line buffer fills, and that boundary lands "
                  "wherever it lands, including mid-word. Chunking ahead of it keeps "
                  "text splitting at clause and word boundaries instead, which is why "
                  "this sentence is long enough to need more than one chunk."),
    "carets": "Normal, ^E40P higher, ^E10F low and flat, ^^ a caret.",
}


def cases():
    out = []

    def add(label, text="sentence", **kw):
        g = dict(DEFAULTS)
        g.update(kw)
        out.append((label, g, text))

    add("defaults")
    for t in ("utf8", "controls", "letter", "paragraph"):
        add("text " + t, t)
    for v in (0, 25, 38, 76, 100):
        add("rate %d" % v, rate=v)
    for v in (0, 10, 40, 63):
        add("pitch %d" % v, pitch=v)
    for v in (0, 7, 15):
        add("volume %d" % v, volume=v)
    for v in (5, 15):
        add("word delay %d" % v, delay=v)
    for v in (0, 50):
        add("repeat filter %d" % v, repeat=v)
    add("repeat filter 3 on a run", "run", repeat=3)
    for v in (0, 30, 69, 100):
        add("chip clock %d" % v, clock=v)
    for hz in (11025, 16000, 22050, 32000, 44100, 48000):
        add("output %d Hz" % hz, srate=hz)
    add("clock 69 over 11025 Hz", clock=69, srate=11025)
    add("clock 60 under 48000 Hz", clock=60, srate=48000)
    add("monotone", mono=1)
    add("compressed", comp=1)
    add("monotone + compressed + pitch 50", mono=1, comp=1, pitch=50)
    for v in (1, 2, 3):
        add("frame rate %d" % v, frame=v)
    for v in (1, 2):
        add("reading mode %d" % v, reading=v)
    for v in (1, 2, 3):
        add("punctuation %d" % v, punct=v)
    for v in (0, 40, 255):
        add("chunk %d" % v, "paragraph", chunk=v)
    add("raw text", raw=1)
    add("obey carets", "carets", obey=1)
    add("carets sanitised", "carets")
    add("obey with control bytes", "controls", obey=1)
    add("everything at once", "paragraph", rate=62, pitch=33, volume=9, delay=3, repeat=20,
        clock=58, srate=22050, mono=1, comp=1, frame=1, reading=0, punct=3, chunk=60)
    return out


def main(argv):
    rom_dir = Path(argv[0]) if argv else DEFAULT_ADDON
    dll = Path(argv[1]) if len(argv) > 1 else DEFAULT_ADDON / "echotalk64.dll"
    if not EXE.is_file():
        print("not built: %s -- run gui-native\\build.cmd" % EXE)
        return 1
    pairs = []
    for ram in sorted(rom_dir.glob("*.ram.bin")):
        stem = ram.name[:-len(".ram.bin")]
        if (rom_dir / (stem + ".obj.bin")).is_file():
            pairs.append((stem, ram, rom_dir / (stem + ".obj.bin")))
    if not pairs:
        print("no Textalker image pairs in %s" % rom_dir)
        return 1
    if not dll.is_file():
        print("no add-on DLL at %s" % dll)
        return 1

    driver = AddonDriver(dll)
    failures = total = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for stem, loader, obj in pairs:
            print("%s:" % stem)
            for label, g, text_key in cases():
                total += 1
                text = TEXTS[text_key]
                text_file = tmp / "text.txt"
                text_file.write_bytes(text.encode("utf-8"))
                out = tmp / "gui.wav"
                if out.exists():
                    out.unlink()
                rc = subprocess.run([str(EXE), "--selftest", str(rom_dir), stem]
                                    + values(g) + [str(out), "@" + str(text_file)]).returncode
                ref, ref_rate, overruns = driver.render(loader, obj, g, text)
                if rc not in (0, 8) or not out.exists():
                    print("  FAIL %-34s --selftest exit %d" % (label, rc))
                    failures += 1
                    continue
                pcm, rate = read_wav(out)
                problems = []
                if rate != ref_rate:
                    problems.append("rate %d, add-on %d" % (rate, ref_rate))
                if pcm != ref:
                    problems.append("%d samples differ from the add-on's %d"
                                    % (len(pcm) // 2, len(ref) // 2))
                if overruns or rc == 8:
                    problems.append("emulation overrun")
                if problems:
                    print("  FAIL %-34s %s" % (label, "; ".join(problems)))
                    failures += 1
                else:
                    print("  ok   %-34s %6d samples at %5d Hz" % (label, len(pcm) // 2, rate))

            # 3. A preset written by the GUI renders what its settings render.
            total += 1
            g = dict(DEFAULTS, rate=70, pitch=45, volume=10, mono=1, clock=60, srate=16000,
                     punct=2, chunk=50)
            preset = tmp / "preset.ini"
            direct, via = tmp / "direct.wav", tmp / "preset.wav"
            (tmp / "text.txt").write_bytes(TEXTS["sentence"].encode("utf-8"))
            text_arg = "@" + str(tmp / "text.txt")
            r1 = subprocess.run([str(EXE), "--write-preset", str(rom_dir), stem]
                                + values(g) + [str(preset)]).returncode
            r2 = subprocess.run([str(EXE), "--selftest", str(rom_dir), stem]
                                + values(g) + [str(direct), text_arg]).returncode
            r3 = subprocess.run([str(EXE), "--selftest-preset", str(rom_dir), str(preset),
                                 str(via), text_arg]).returncode
            if (r1, r2, r3) != (0, 0, 0) or read_wav(direct) != read_wav(via):
                print("  FAIL preset round trip (exits %d %d %d)" % (r1, r2, r3))
                failures += 1
            else:
                print("  ok   %-34s identical to the direct render" % "preset round trip")

            # 4. Batch render and pitch sweep.
            total += 1
            g = dict(DEFAULTS, rate=60, volume=10)
            lines = ["First line, spoken alone.", "", "Second: café & co.", "   ",
                     "Third line."]
            (tmp / "batch.txt").write_bytes("\r\n".join(lines).encode("utf-8"))
            bdir = tmp / ("batch-" + stem)
            bdir.mkdir()
            rc = subprocess.run([str(EXE), "--selftest-batch", str(rom_dir), stem] + values(g)
                                + [str(bdir), "@" + str(tmp / "batch.txt")]).returncode
            names = sorted(p.name for p in bdir.glob("*.wav"))
            want = ["001 First line spoken alone.wav", "002 Second café co.wav",
                    "003 Third line.wav"]
            bad = rc != 0 or names != want
            for name, line in zip(want, [lines[0], lines[2], lines[4]]):
                if not bad:
                    ref, ref_rate, _ = driver.render(loader, obj, g, line)
                    bad = read_wav(bdir / name) != (ref, ref_rate)
            if bad:
                print("  FAIL batch render (exit %d, files %s)" % (rc, names))
                failures += 1
            else:
                print("  ok   %-34s %s" % ("batch render", ", ".join(names)))

            for mono in (0, 1):
                total += 1
                g = dict(DEFAULTS, mono=mono)
                sdir = tmp / ("sweep%d-%s" % (mono, stem))
                sdir.mkdir()
                rc = subprocess.run([str(EXE), "--selftest-sweep", str(rom_dir), stem] + values(g)
                                    + [str(sdir), "Hi."]).returncode
                files = sorted(sdir.glob("*.wav"))
                bad = rc != 0 or len(files) != 100
                checked = []
                for p in (0, 24, 63, 64, 99):
                    if bad:
                        break
                    ref, ref_rate, _ = driver.render(
                        loader, obj, g, "Hi.", "\x05%d%s" % (p, "F" if mono else "P"))
                    bad = read_wav(sdir / ("pitch %02d.wav" % p)) != (ref, ref_rate)
                    checked.append(p)
                distinct = len({read_wav(f)[0] for f in files}) if not bad else 0
                label = "pitch sweep" + (" monotone" if mono else "")
                if bad:
                    print("  FAIL %s (exit %d, %d files)" % (label, rc, len(files)))
                    failures += 1
                else:
                    print("  ok   %-34s 100 files; %s match; %d distinct recordings"
                          % (label, "/".join(map(str, checked)), distinct))

    if failures:
        print("\n%d of %d cases FAILED." % (failures, total))
        return 1
    print("\nOK: all %d cases byte-identical to the add-on's own DLL." % total)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
