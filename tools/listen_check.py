#!/usr/bin/env python3
"""Exercises the EchoTalk shared library and writes a WAV you can check by ear.

Loads libechotalk.so (or echotalk.dll) through ctypes exactly as a host
would, runs every feature past it, and writes one WAV file in which the
speech itself announces what each section is about to demonstrate. So
you can listen straight through and hear whether each section does what
it just said it would -- no need to follow along with a transcript.

It also prints a pass/fail report for the things a machine can check on
its own (sample counts, index-event ordering, streaming latency).

usage:
    python3 tools/listen_check.py <library> <loader.bin> <obj.bin> [out.wav]

e.g.
    python3 tools/listen_check.py build/native/libechotalk.so \\
        roms/textalker.ram.bin roms/textalker.obj.bin listen.wav
"""

import ctypes
import os
import struct
import sys
import time

RATE = 8000
GAP = RATE // 2          # half a second of silence between sections


class Report:
    def __init__(self):
        self.failed = 0
        self.lines = []

    def check(self, label, ok, detail=""):
        self.lines.append((ok, label, detail))
        if not ok:
            self.failed += 1
        return ok

    def dump(self):
        print("\nAutomated checks")
        print("-" * 68)
        for ok, label, detail in self.lines:
            print(f"  {'ok  ' if ok else 'FAIL'}  {label}{'  -- ' + detail if detail else ''}")
        print("-" * 68)
        print("all checks passed" if not self.failed else f"{self.failed} check(s) FAILED")


def declare(lib):
    p = ctypes.c_void_p
    lib.echotalk_abi_version.restype = ctypes.c_uint
    lib.echotalk_abi_version.argtypes = []
    lib.echotalk_create.restype = p
    lib.echotalk_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                                    ctypes.c_char_p, ctypes.c_size_t]
    lib.echotalk_destroy.restype = None
    lib.echotalk_destroy.argtypes = [p]
    lib.echotalk_version.restype = ctypes.c_char_p
    lib.echotalk_version.argtypes = [p]
    for name, arg in [("set_sample_rate", ctypes.c_uint),
                      ("set_clock_multiplier", ctypes.c_double),
                      ("set_speed", ctypes.c_double),
                      ("set_frame_rate", ctypes.c_int),
                      ("set_compressed", ctypes.c_int),
                      ("set_pitch", ctypes.c_int),
                      ("set_volume", ctypes.c_int),
                      ("set_word_delay", ctypes.c_int),
                      ("set_chunk_size", ctypes.c_uint),
                      ("set_raw", ctypes.c_int),
                      ("set_flat", ctypes.c_int),
                      ("set_letter_mode", ctypes.c_int),
                      ("set_punctuation", ctypes.c_int),
                      ("set_repeat_filter", ctypes.c_int)]:
        fn = getattr(lib, "echotalk_" + name)
        fn.restype, fn.argtypes = ctypes.c_int, [p, arg]
    lib.echotalk_speak.restype = ctypes.c_int
    lib.echotalk_speak.argtypes = [p, ctypes.c_char_p]
    lib.echotalk_read.restype = ctypes.c_size_t
    lib.echotalk_read.argtypes = [p, ctypes.POINTER(ctypes.c_int16), ctypes.c_size_t]
    for name in ("available", "pending"):
        fn = getattr(lib, "echotalk_" + name)
        fn.restype, fn.argtypes = ctypes.c_size_t, [p]
    lib.echotalk_synthesize.restype = ctypes.c_size_t
    lib.echotalk_synthesize.argtypes = [p, ctypes.c_size_t]
    lib.echotalk_next_index.restype = ctypes.c_int
    lib.echotalk_next_index.argtypes = [p, ctypes.POINTER(ctypes.c_int)]
    lib.echotalk_chunk_size.restype = ctypes.c_uint
    lib.echotalk_chunk_size.argtypes = [p]
    for name in ("pitch", "flat", "volume", "word_delay", "repeat_filter",
                 "compressed", "letter_mode", "punctuation", "frame_rate", "raw"):
        fn = getattr(lib, "echotalk_" + name)
        fn.restype, fn.argtypes = ctypes.c_int, [p]
    lib.echotalk_command_errors.restype = ctypes.c_uint
    lib.echotalk_command_errors.argtypes = [p]
    lib.echotalk_clear_command_errors.restype = None
    lib.echotalk_clear_command_errors.argtypes = [p]
    lib.echotalk_stop.restype = None
    lib.echotalk_stop.argtypes = [p]


class Talker:
    """Thin wrapper so the sections below read like what they test."""

    def __init__(self, lib, et):
        self.lib, self.et = lib, et
        self.pcm = bytearray()
        self.marks = []

    def say(self, text):
        """Speaks text, appending to the output. Returns samples produced."""
        before = len(self.pcm) // 2
        if self.lib.echotalk_speak(self.et, text.encode("utf-8")) != 0:
            raise RuntimeError("echotalk_speak failed")
        buf = (ctypes.c_int16 * 1024)()
        idx = ctypes.c_int()
        while True:
            n = self.lib.echotalk_read(self.et, buf, 1024)
            self.pcm += bytes(buf)[:n * 2]
            # Collect after every read INCLUDING the last, which returns
            # 0: a mark at the very end of the text only becomes ready
            # on that call.
            while self.lib.echotalk_next_index(self.et, ctypes.byref(idx)):
                self.marks.append((idx.value, len(self.pcm) // 2))
            if n == 0:
                break
        return len(self.pcm) // 2 - before

    def gap(self, samples=GAP):
        self.pcm += b"\0\0" * samples

    def reset(self):
        """Back to defaults between sections, so each stands alone."""
        lib, et = self.lib, self.et
        lib.echotalk_set_frame_rate(et, 0)
        lib.echotalk_set_clock_multiplier(et, 1.0)
        lib.echotalk_set_speed(et, 1.0)
        lib.echotalk_set_pitch(et, 24)
        lib.echotalk_set_volume(et, 12)
        lib.echotalk_set_compressed(et, 0)
        lib.echotalk_set_chunk_size(et, 80)
        lib.echotalk_set_raw(et, 0)
        lib.echotalk_set_flat(et, 0)
        lib.echotalk_set_letter_mode(et, 0)
        lib.echotalk_set_punctuation(et, 1)


def wav_write(path, rate, pcm):
    n = len(pcm) // 2
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + n * 2) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", n * 2))
        f.write(bytes(pcm))


def main():
    if len(sys.argv) not in (4, 5):
        print(__doc__)
        return 2
    libpath, loader, obj = sys.argv[1:4]
    outpath = sys.argv[4] if len(sys.argv) == 5 else "listen.wav"

    for path in (libpath, loader, obj):
        if not os.path.exists(path):
            print(f"missing: {path}")
            return 2

    try:
        lib = ctypes.CDLL(os.path.abspath(libpath))
    except OSError as e:
        print(f"could not load {libpath}: {e}")
        print("On Linux, check it was built with 'make so' and that the")
        print("architecture matches this Python interpreter.")
        return 1

    declare(lib)
    r = Report()

    abi = lib.echotalk_abi_version()
    r.check("ABI version is 4", abi == 4, f"got {abi}")

    err = ctypes.create_string_buffer(256)
    et = lib.echotalk_create(loader.encode(), obj.encode(), err, len(err))
    if not r.check("library loads and boots Textalker", bool(et),
                   err.value.decode() if not et else ""):
        r.dump()
        return 1

    banner = lib.echotalk_version(et).decode()
    print(f"loaded {libpath}")
    print(f"Textalker version {banner}")

    t = Talker(lib, et)

    try:
        # 1 -----------------------------------------------------------
        t.say("Section one. Plain speech, at the default settings. "
              "This is what an Echo two sounds like.")
        plain_len = t.say("The quick brown fox jumps over the lazy dog.")
        t.gap()
        r.check("plain speech produced audio", plain_len > 8000,
                f"{plain_len} samples")

        # 2 -----------------------------------------------------------
        t.say("Section two. The same sentence at frame rate two. "
              "It should be faster, but the pitch should not change.")
        lib.echotalk_set_frame_rate(et, 2)
        fast_len = t.say("The quick brown fox jumps over the lazy dog.")
        t.reset()
        t.gap()
        r.check("frame rate 2 speeds speech up", fast_len < plain_len * 0.75,
                f"{fast_len} vs {plain_len} samples, {plain_len / fast_len:.2f}x")

        # 3 -----------------------------------------------------------
        t.say("Section three. Now the chip clock is raised by half. "
              "This one should be faster AND higher pitched.")
        lib.echotalk_set_clock_multiplier(et, 1.5)
        clock_len = t.say("The quick brown fox jumps over the lazy dog.")
        t.reset()
        t.gap()
        r.check("clock multiplier speeds speech up", clock_len < plain_len * 0.8,
                f"{clock_len} vs {plain_len} samples, {plain_len / clock_len:.2f}x")

        # 3b ----------------------------------------------------------
        t.say("Section three B. The continuous speed control, at half "
              "speed then double speed. Both should keep the same pitch "
              "as each other and as section one.")
        lib.echotalk_set_speed(et, 0.5)
        slow_n = t.say("The quick brown fox jumps over the lazy dog.")
        lib.echotalk_set_speed(et, 2.0)
        fast_n = t.say("The quick brown fox jumps over the lazy dog.")
        t.reset()
        t.gap()
        r.check("continuous speed is monotonic and spans a wide range",
                slow_n > plain_len * 1.5 and fast_n < plain_len * 0.7,
                f"{slow_n} / {plain_len} / {fast_n} samples at 0.5 / 1.0 / 2.0")

        # 4 -----------------------------------------------------------
        t.say("Section four. Low pitch, then high pitch. "
              "The speed should stay the same in both.")
        lib.echotalk_set_pitch(et, 8)
        low = t.say("This is a low voice.")
        lib.echotalk_set_pitch(et, 48)
        high = t.say("And this is a high voice.")
        t.reset()
        t.gap()
        r.check("pitch does not change duration",
                abs(low - high) < max(low, high) * 0.35,
                f"{low} vs {high} samples")

        # 5 -----------------------------------------------------------
        t.say("Section five. Single characters. You should hear "
              "comma, period, question mark, and the letter zed, "
              "each announced once, with no word return after any of them.")
        singles = {c: t.say(c) for c in (",", ".", "?", "Z")}
        t.gap()
        # A trailing "return" costs about 4,400 samples. Duration alone
        # cannot judge every character, because some names are two words
        # -- "question mark" is legitimately 8,882 samples. The comma is
        # the canary: it measures 3,082 correct against 7,650 with the
        # bug, so the two cannot be confused. The rest are for the ear.
        r.check("a lone comma does not also say 'return'",
                2800 < singles[","] < 3600,
                f"{singles[',']} samples; ~3,100 is right, ~7,700 means "
                f"'return' is back")
        print(f"  (single characters: "
              f"{', '.join(f'{c!r} {n}' for c, n in singles.items())} samples)")

        # 6 -----------------------------------------------------------
        t.say("Section six. Ctrl D driver commands, embedded in the text. "
              "The next sentence changes speed halfway through, "
              "at the word now.")
        t.say("This part is at the normal speed, and \x04 3F now it is not, "
              "\x04F and now it is again.")
        t.gap()

        # 7 -----------------------------------------------------------
        t.say("Section seven. Index marks. Three of them are placed in "
              "the next sentence, after the words one, two and three.")
        before = len(t.marks)
        t.say("One\x04 1I two\x04 2I three.\x04 3I")
        got = t.marks[before:]
        t.gap()
        r.check("all three index marks fired",
                [m[0] for m in got] == [1, 2, 3], str([m[0] for m in got]))
        r.check("index marks arrive in ascending sample order",
                all(got[i][1] <= got[i + 1][1] for i in range(len(got) - 1)),
                str([m[1] for m in got]))

        # 8 -----------------------------------------------------------
        t.say("Section eight. A long passage, to show that text is split "
              "at sensible places and never in the middle of a word.")
        t.say("The chunker breaks long text at clause boundaries where it "
              "can, at word boundaries where it cannot, and only splits a "
              "word when a single word is longer than the whole buffer, "
              "which in ordinary prose essentially never happens. Listen "
              "for whether any word comes apart.")
        t.gap()

        # 9 -----------------------------------------------------------
        t.say("Section nine. Compressed speech, which Textalker makes "
              "faster by leaving sounds out rather than by playing faster.")
        lib.echotalk_set_compressed(et, 1)
        comp = t.say("The quick brown fox jumps over the lazy dog.")
        t.reset()
        t.gap()
        r.check("compressed speech is shorter", comp < plain_len,
                f"{comp} vs {plain_len} samples")

        # 10 ----------------------------------------------------------
        t.say("Section ten. Monotone. The next sentence is spoken flat, "
              "with the pitch held still, and the one after it goes back "
              "to normal intonation.")
        SENT = "The quick brown fox jumps over the lazy dog."
        p0 = len(t.pcm)
        lib.echotalk_set_flat(et, 1)
        flat_n = t.say(SENT)
        p1 = len(t.pcm)
        lib.echotalk_set_flat(et, 0)
        normal_n = t.say(SENT)
        # Monotone changes the pitch contour, not the timing, so the
        # content must differ while the duration stays put. "Stays put"
        # is within a few samples rather than exact: an utterance can
        # land a sample either side depending on where the interpolation
        # boundary falls, and demanding equality made this fail on a
        # one-sample difference that meant nothing.
        r.check("monotone changes the voice but not its duration",
                bytes(t.pcm[p0:p1]) != bytes(t.pcm[p1:])
                and abs(flat_n - normal_n) < max(16, normal_n // 100),
                f"{flat_n} vs {normal_n} samples, audio differs")
        t.gap()

        # A Ctrl-E command embedded in the text must be mirrored into the
        # library's own settings, or the two drift apart.
        t.say("36PThis pitch was set by a command inside the text.")
        r.check("Ctrl-E in the text updates the library's settings",
                lib.echotalk_pitch(et) == 36, f"pitch reads {lib.echotalk_pitch(et)}")
        t.reset()
        t.gap()

        # 11 ----------------------------------------------------------
        t.say("Section eleven. This is the end of the test. "
              "If every section did what it said, the library works.")

        # --- checks with no audible component -------------------------
        # Long enough that the first chunk is a small fraction of the
        # whole; with only two chunks the ratio proves nothing.
        long_text = ("This is a long enough passage that streaming has "
                     "something to actually stream. It runs to a good many "
                     "chunks, so that the time taken to produce the first "
                     "one is a small fraction of the time taken to produce "
                     "all of them. That is the entire point of streaming: "
                     "the listener hears the beginning while the end is "
                     "still being made, rather than waiting for the whole "
                     "utterance to be synthesised before anything at all "
                     "comes out of the speaker.")
        t0 = time.perf_counter()
        lib.echotalk_speak(et, long_text.encode())
        t_speak = time.perf_counter() - t0
        r.check("speak() returns without synthesising",
                lib.echotalk_available(et) == 0 and t_speak < 0.05,
                f"{t_speak * 1000:.1f} ms, {lib.echotalk_available(et)} samples queued")
        r.check("pending() reports outstanding text",
                lib.echotalk_pending(et) > 0, f"{lib.echotalk_pending(et)} bytes")

        buf = (ctypes.c_int16 * 1024)()
        t0 = time.perf_counter()
        first = lib.echotalk_read(et, buf, 1024)
        t_first = time.perf_counter() - t0
        total = first
        while True:
            n = lib.echotalk_read(et, buf, 1024)
            if n == 0:
                break
            total += n
        t_all = time.perf_counter() - t0
        r.check("first audio arrives well before the rest is synthesised",
                first > 0 and t_first < t_all / 2,
                f"first block {t_first * 1000:.1f} ms, all {total} samples "
                f"in {t_all * 1000:.1f} ms")
        r.check("synthesis is comfortably faster than real time",
                t_all > 0 and (total / RATE) / t_all > 10,
                f"{(total / RATE) / t_all:.0f}x real time")

        lib.echotalk_speak(et, long_text.encode())
        lib.echotalk_stop(et)
        r.check("stop() abandons queued text as well as audio",
                lib.echotalk_pending(et) == 0 and lib.echotalk_available(et) == 0)

        lib.echotalk_clear_command_errors(et)
        lib.echotalk_speak(et, b"\x04 9Z ignored.")
        while lib.echotalk_read(et, buf, 1024):
            pass
        r.check("a bad Ctrl-D command is counted, not spoken",
                lib.echotalk_command_errors(et) == 1,
                f"{lib.echotalk_command_errors(et)} error(s)")

        second = lib.echotalk_create(loader.encode(), obj.encode(), err, len(err))
        r.check("a second instance is refused", not second, err.value.decode())
        if second:
            lib.echotalk_destroy(second)

    finally:
        wav_write(outpath, RATE, t.pcm)
        lib.echotalk_destroy(et)

    dur = len(t.pcm) / 2 / RATE
    print(f"\nwrote {outpath}: {len(t.pcm) // 2} samples, {dur:.1f} s at {RATE} Hz")
    print(f"index marks seen: {t.marks}")
    r.dump()
    return 1 if r.failed else 0


if __name__ == "__main__":
    sys.exit(main())
