"""Loads echotalk.dll through ctypes and exercises the exported surface.

This is the end-to-end check that the DLL is usable by a host that has
no C compiler involved -- which is the situation NVDA is in. It is a
separate thing from say.c: say compiles the library's sources into
itself and so proves nothing about the export table, the calling
convention, or the runtime dependencies.

usage: python tools/test_dll.py <echotalk.dll> <loader.bin> <obj.bin>

The Python interpreter's bitness must match the DLL's.
"""

import ctypes
import os
import struct
import time
import sys

EXPECTED_ABI = 5


class Failures:
    def __init__(self):
        self.count = 0

    def check(self, label, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'}  {label}{'  ' + detail if detail else ''}")
        if not ok:
            self.count += 1
        return ok


def declare(lib):
    """Give ctypes the real signatures.

    Without this every argument is guessed from the Python value, which
    happens to work on 64-bit for ints and breaks silently for doubles
    and for pointers above 2GB. Declaring them is not optional detail.
    """
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
        fn.restype = ctypes.c_int
        fn.argtypes = [p, arg]

    lib.echotalk_chunk_size.restype = ctypes.c_uint
    lib.echotalk_chunk_size.argtypes = [p]
    for name in ("pitch", "flat", "volume", "word_delay", "repeat_filter",
                 "compressed", "letter_mode", "punctuation", "frame_rate", "raw"):
        fn = getattr(lib, "echotalk_" + name)
        fn.restype, fn.argtypes = ctypes.c_int, [p]
    lib.echotalk_clock_multiplier.restype = ctypes.c_double
    lib.echotalk_clock_multiplier.argtypes = [p]
    lib.echotalk_speed.restype = ctypes.c_double
    lib.echotalk_speed.argtypes = [p]
    lib.echotalk_sample_rate.restype = ctypes.c_uint
    lib.echotalk_sample_rate.argtypes = [p]
    lib.echotalk_speak.restype = ctypes.c_int
    lib.echotalk_speak.argtypes = [p, ctypes.c_char_p]
    lib.echotalk_read.restype = ctypes.c_size_t
    lib.echotalk_read.argtypes = [p, ctypes.POINTER(ctypes.c_int16),
                                  ctypes.c_size_t]
    lib.echotalk_available.restype = ctypes.c_size_t
    lib.echotalk_available.argtypes = [p]
    lib.echotalk_pending.restype = ctypes.c_size_t
    lib.echotalk_pending.argtypes = [p]
    lib.echotalk_synthesize.restype = ctypes.c_size_t
    lib.echotalk_synthesize.argtypes = [p, ctypes.c_size_t]
    lib.echotalk_next_index.restype = ctypes.c_int
    lib.echotalk_next_index.argtypes = [p, ctypes.POINTER(ctypes.c_int)]
    lib.echotalk_stop.restype = None
    lib.echotalk_stop.argtypes = [p]
    lib.echotalk_command_errors.restype = ctypes.c_uint
    lib.echotalk_command_errors.argtypes = [p]
    lib.echotalk_overruns.restype = ctypes.c_uint
    lib.echotalk_overruns.argtypes = [p]
    lib.echotalk_clear_command_errors.restype = None
    lib.echotalk_clear_command_errors.argtypes = [p]


def drain(lib, et, block=1024, indices=None):
    """Pull PCM the way an audio callback would, in small blocks.

    Collects index events after every read INCLUDING the final one that
    returns 0, since a mark at the very end of the text only becomes
    ready then. If `indices` is a list, (index, sample_position) pairs
    are appended to it.
    """
    buf = (ctypes.c_int16 * block)()
    out = bytearray()
    idx = ctypes.c_int()
    while True:
        n = lib.echotalk_read(et, buf, block)
        out += bytes(buf)[:n * 2]
        while lib.echotalk_next_index(et, ctypes.byref(idx)):
            if indices is not None:
                indices.append((idx.value, len(out) // 2))
        if n == 0:
            break
    return out


def speak(lib, et, text):
    rc = lib.echotalk_speak(et, text.encode("utf-8"))
    if rc != 0:
        raise RuntimeError("echotalk_speak failed")
    return drain(lib, et)


def peak(pcm):
    if not pcm:
        return 0, 0
    vals = struct.unpack(f"<{len(pcm) // 2}h", pcm)
    return min(vals), max(vals)


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    dll_path, loader, obj = sys.argv[1:4]
    for path in (dll_path, loader, obj):
        if not os.path.exists(path):
            print(f"missing: {path}")
            return 2

    f = Failures()

    print(f"loading {dll_path} ({8 * struct.calcsize('P')}-bit Python)")
    try:
        lib = ctypes.CDLL(os.path.abspath(dll_path))
    except OSError as e:
        # The usual cause is a bitness mismatch, which Windows reports
        # as "not a valid Win32 application" and which reads as a
        # corrupt file if you do not know to look for it.
        print(f"  FAIL  could not load: {e}")
        return 1
    print("  ok    loaded")

    declare(lib)

    abi = lib.echotalk_abi_version()
    f.check("abi version", abi == EXPECTED_ABI, f"got {abi}, expected {EXPECTED_ABI}")

    err = ctypes.create_string_buffer(256)
    et = lib.echotalk_create(loader.encode(), obj.encode(), err, len(err))
    if not f.check("create", bool(et), err.value.decode() if not et else ""):
        return 1

    try:
        ver = lib.echotalk_version(et).decode()
        f.check("version banner", bool(ver) and ver != "unknown", repr(ver))

        # Single instance: the 6502 core keeps its state in globals, so a
        # second create must be refused rather than quietly corrupting
        # the first. Worth testing from a host that might well try.
        err2 = ctypes.create_string_buffer(256)
        second = lib.echotalk_create(loader.encode(), obj.encode(), err2, len(err2))
        f.check("second instance refused", not second, err2.value.decode())
        if second:
            lib.echotalk_destroy(second)

        pcm = speak(lib, et, "Hello there.")
        lo, hi = peak(pcm)
        f.check("speak and read", len(pcm) > 8000,
                f"{len(pcm) // 2} samples, range {lo}/{hi}")

        f.check("queue drained", lib.echotalk_available(et) == 0)

        # Every setter, including the double, whose ABI is the one most
        # likely to be got wrong by a host that skipped argtypes.
        f.check("set_clock_multiplier(1.5)", lib.echotalk_set_clock_multiplier(et, 1.5) == 0)
        f.check("set_clock_multiplier(99) rejected", lib.echotalk_set_clock_multiplier(et, 99.0) == -1)
        f.check("set_clock_multiplier(1.0)", lib.echotalk_set_clock_multiplier(et, 1.0) == 0)
        f.check("set_frame_rate(2)", lib.echotalk_set_frame_rate(et, 2) == 0)
        f.check("set_frame_rate(9) rejected", lib.echotalk_set_frame_rate(et, 9) == -1)
        f.check("set_frame_rate(0)", lib.echotalk_set_frame_rate(et, 0) == 0)
        f.check("set_pitch(8)", lib.echotalk_set_pitch(et, 8) == 0)
        f.check("set_pitch(99) rejected", lib.echotalk_set_pitch(et, 99) == -1)
        f.check("set_pitch(24)", lib.echotalk_set_pitch(et, 24) == 0)
        f.check("set_volume(12)", lib.echotalk_set_volume(et, 12) == 0)
        f.check("set_word_delay(0)", lib.echotalk_set_word_delay(et, 0) == 0)
        f.check("set_repeat_filter(99)", lib.echotalk_set_repeat_filter(et, 99) == 0)
        f.check("set_compressed(0)", lib.echotalk_set_compressed(et, 0) == 0)
        f.check("set_sample_rate(22050)", lib.echotalk_set_sample_rate(et, 22050) == 0)
        f.check("set_sample_rate(1) rejected", lib.echotalk_set_sample_rate(et, 1) == -1)
        f.check("set_sample_rate(0) = native", lib.echotalk_set_sample_rate(et, 0) == 0)
        f.check("set_raw(1)", lib.echotalk_set_raw(et, 1) == 0)
        f.check("set_raw(7) rejected", lib.echotalk_set_raw(et, 7) == -1)
        f.check("set_raw(0)", lib.echotalk_set_raw(et, 0) == 0)
        f.check("set_chunk_size(0)", lib.echotalk_set_chunk_size(et, 0) == 0)
        f.check("chunk_size reads back 0", lib.echotalk_chunk_size(et) == 0)
        f.check("set_chunk_size(999) rejected", lib.echotalk_set_chunk_size(et, 999) == -1)
        f.check("set_chunk_size(80)", lib.echotalk_set_chunk_size(et, 80) == 0)

        # The double actually taking effect, not merely being accepted:
        # a clock multiplier passed as garbage would still return 0.
        plain = speak(lib, et, "Testing one two three.")
        lib.echotalk_set_clock_multiplier(et, 2.0)
        fast = speak(lib, et, "Testing one two three.")
        lib.echotalk_set_clock_multiplier(et, 1.0)
        ratio = len(plain) / len(fast) if fast else 0
        f.check("clock multiplier crosses the ABI", 1.8 < ratio < 2.2,
                f"{len(plain) // 2} vs {len(fast) // 2} samples, ratio {ratio:.2f}")

        # Ctrl-D commands through the DLL boundary, including the error
        # counter that is a host's only way to see a typo.
        lib.echotalk_clear_command_errors(et)
        speak(lib, et, "\x04\x39\x5aOne.")   # \x04 9Z -- unknown letter
        f.check("bad Ctrl-D counted", lib.echotalk_command_errors(et) == 1,
                f"got {lib.echotalk_command_errors(et)}")
        lib.echotalk_clear_command_errors(et)
        f.check("error counter clears", lib.echotalk_command_errors(et) == 0)

        ctrl_d = speak(lib, et, "\x042FTesting one two three.")
        f.check("Ctrl-D frame rate through the ABI", len(ctrl_d) < len(plain),
                f"{len(ctrl_d) // 2} vs {len(plain) // 2} samples")
        f.check("no errors from a good command", lib.echotalk_command_errors(et) == 0)

        # stop() must discard everything outstanding without disturbing
        # Textalker. Note speak() queues text, not audio, so it is
        # pending() rather than available() that is non-zero here.
        lib.echotalk_speak(et, b"Discard this please.")
        f.check("text queued before stop", lib.echotalk_pending(et) > 0,
                f"{lib.echotalk_pending(et)} bytes")
        lib.echotalk_stop(et)
        f.check("stop clears the queue",
                lib.echotalk_available(et) == 0 and lib.echotalk_pending(et) == 0)
        after = speak(lib, et, "Hello there.")
        f.check("speech survives stop", len(after) > 8000,
                f"{len(after) // 2} samples")

        # A single character must speak the character and nothing else --
        # the session-11 fix, verified through the ABI a screen reader
        # will actually use it through. Reset the frame rate first: the
        # Ctrl-D command above persists by design, and left in place it
        # would shrink these counts and make the thresholds meaningless.
        lib.echotalk_set_frame_rate(et, 0)
        comma = speak(lib, et, ",")
        f.check("single character is short", len(comma) // 2 < 4000,
                f"{len(comma) // 2} samples; over ~7000 means 'return' is back")

        f.check("empty string is harmless", lib.echotalk_speak(et, b"") == 0)

        # --- streaming ---
        #
        # speak() must return without synthesising, so a host is not
        # blocked for the length of the utterance before the first
        # sample exists.
        long_text = ("This is a fairly long passage, long enough to be split "
                     "into several chunks, so that streaming has something to "
                     "actually stream. It keeps going for a while yet.")
        t0 = time.perf_counter()
        lib.echotalk_speak(et, long_text.encode())
        t_speak = time.perf_counter() - t0
        f.check("speak() returns without synthesising",
                lib.echotalk_available(et) == 0 and t_speak < 0.01,
                f"{t_speak * 1000:.1f} ms, {lib.echotalk_available(et)} samples queued")
        f.check("pending reports outstanding text", lib.echotalk_pending(et) > 0,
                f"{lib.echotalk_pending(et)} bytes")

        buf = (ctypes.c_int16 * 1024)()
        t0 = time.perf_counter()
        first = lib.echotalk_read(et, buf, 1024)
        t_first = time.perf_counter() - t0
        rest = first
        while True:
            n = lib.echotalk_read(et, buf, 1024)
            if n == 0:
                break
            rest += n
        t_total = time.perf_counter() - t0
        f.check("first audio arrives before the rest is made",
                first > 0 and t_first < t_total / 2,
                f"first block {t_first * 1000:.1f} ms, all {rest} samples "
                f"{t_total * 1000:.1f} ms")
        f.check("everything was spoken", lib.echotalk_pending(et) == 0 and
                lib.echotalk_available(et) == 0)

        # synthesize() is the alternative for a host that would rather
        # not run synthesis inside its audio callback.
        lib.echotalk_speak(et, long_text.encode())
        got = lib.echotalk_synthesize(et, 8000)
        f.check("synthesize() pre-fills the queue", got >= 8000,
                f"{got} samples ready before any read")
        drain(lib, et)

        # --- index events ---
        marks = []
        lib.echotalk_speak(et, b"First part.\x04 1ISecond part.\x04 2IThird.\x04 3I")
        audio = drain(lib, et, indices=marks)
        f.check("all three index marks fired", [m[0] for m in marks] == [1, 2, 3],
                str(marks))
        f.check("index marks are in ascending order",
                all(marks[i][1] <= marks[i + 1][1] for i in range(len(marks) - 1)),
                str([m[1] for m in marks]))
        f.check("last mark lands at the end of the audio",
                marks and marks[-1][1] == len(audio) // 2,
                f"mark at {marks[-1][1] if marks else '-'}, audio {len(audio) // 2}")
        f.check("index mark with no number is rejected",
                (lambda: (lib.echotalk_clear_command_errors(et),
                          lib.echotalk_speak(et, b"Hi.\x04I"),
                          drain(lib, et),
                          lib.echotalk_command_errors(et))[-1])() == 1)

        # --- the settings that make the emulation work hardest ---
        #
        # A long inter-word delay and slow speech both leave Textalker
        # waiting on the chip, which is what pushes a single character
        # past the 6502 step budget. That used to truncate the utterance
        # in silence and leave the CPU mid-routine, so every character
        # after it ran on a corrupted stack. Nothing reported it.
        f.check("no overruns at default settings", lib.echotalk_overruns(et) == 0,
                str(lib.echotalk_overruns(et)))
        lib.echotalk_set_word_delay(et, 15)
        lib.echotalk_set_speed(et, 0.25)
        hard = speak(lib, et, "The quick brown fox jumps over the lazy dog "
                              "while the cat watches from a wall.")
        f.check("no overruns at the slowest speed and longest word delay",
                lib.echotalk_overruns(et) == 0,
                f"{lib.echotalk_overruns(et)} overrun(s), {len(hard) // 2} samples")
        # 367302 is what the emulation produces when it is allowed to
        # finish; the old budget cut it to 115487.
        f.check("the whole utterance is synthesised, not truncated",
                len(hard) // 2 > 300000, f"{len(hard) // 2} samples")
        lib.echotalk_set_word_delay(et, 0)
        lib.echotalk_set_speed(et, 1.0)
        drain(lib, et)

        # --- continuous speed: faster, monotonic, pitch untouched ---
        lib.echotalk_set_pitch(et, 24)
        lib.echotalk_set_flat(et, 0)
        SPEEDS = (0.5, 1.0, 2.0)
        lens = []
        for sp in SPEEDS:
            f.check(f"set_speed({sp})", lib.echotalk_set_speed(et, sp) == 0)
            lens.append(len(speak(lib, et, "Aaaaah.")) // 2)
        f.check("speed is monotonic", lens[0] > lens[1] > lens[2], str(lens))
        f.check("set_speed(9) rejected", lib.echotalk_set_speed(et, 9.0) == -1)
        f.check("speed reads back", lib.echotalk_set_speed(et, 1.5) == 0
                and abs(lib.echotalk_speed(et) - 1.5) < 1e-9,
                str(lib.echotalk_speed(et)))
        lib.echotalk_set_speed(et, 1.0)
        drain(lib, et)

        # --- Ctrl-E commands in the text must update the settings ---
        #
        # Otherwise the library's idea of the voice drifts away from
        # Textalker's, the next settings push silently undoes whatever
        # the text asked for, and a host cannot carry the voice to a
        # fresh instance because it cannot read the current values.
        def state():
            return {n: getattr(lib, "echotalk_" + n)(et) for n in
                    ("pitch", "flat", "volume", "word_delay", "repeat_filter",
                     "compressed", "letter_mode", "punctuation")}

        lib.echotalk_set_pitch(et, 24)
        lib.echotalk_set_volume(et, 12)
        lib.echotalk_set_flat(et, 0)
        drain(lib, et)
        speak(lib, et, "48PHello.")
        f.check("Ctrl-E nP is mirrored", state()["pitch"] == 48,
                str(state()["pitch"]))
        speak(lib, et, "40FHello.")
        f.check("Ctrl-E nF sets pitch AND monotone",
                state()["pitch"] == 40 and state()["flat"] == 1, str(state()))
        speak(lib, et, "12pHello.")
        f.check("command letters are case-insensitive",
                state()["pitch"] == 12 and state()["flat"] == 0, str(state()))
        speak(lib, et, "3V10D2RCHi.")
        st = state()
        f.check("Ctrl-E V/D/R/C are mirrored",
                (st["volume"], st["word_delay"], st["repeat_filter"],
                 st["compressed"]) == (3, 10, 2, 1), str(st))
        speak(lib, et, "99PHi.")
        f.check("out-of-range Ctrl-E value is clamped to the setter's range",
                state()["pitch"] == 63, str(state()["pitch"]))
        f.check("every getter value is accepted by its setter",
                all(getattr(lib, "echotalk_set_" + n)(et, v) == 0
                    for n, v in state().items()))

        # Flatness set from the text must survive a settings push. This
        # is the regression the mirroring exists to prevent: apply_settings
        # used to send "%dP" unconditionally and un-flatten the voice.
        #
        # Both renderings below are made with the settings dirty on entry
        # and no Ctrl-E in the text, so each carries an identical settings
        # block. Comparing a call that has one against a call that does
        # not would compare the block's untrimmed lead-in rather than the
        # speech, and they would differ whatever the voice did.
        SENT = "The quick brown fox."
        # Put every setting that reaches Textalker into a known state
        # first. Without this the comparison depends on whatever the
        # preceding checks happened to leave behind, and inserting a new
        # check earlier in the file breaks it -- which is exactly what
        # happened when the overrun checks above were added.
        lib.echotalk_set_volume(et, 12)
        lib.echotalk_set_word_delay(et, 0)
        lib.echotalk_set_repeat_filter(et, 99)
        lib.echotalk_set_compressed(et, 0)
        lib.echotalk_set_speed(et, 1.0)
        lib.echotalk_set_clock_multiplier(et, 1.0)
        speak(lib, et, "Settling.")

        lib.echotalk_set_pitch(et, 40)
        lib.echotalk_set_flat(et, 1)                 # flat, via the API
        ref_flat = speak(lib, et, SENT)

        lib.echotalk_set_pitch(et, 24)
        lib.echotalk_set_flat(et, 0)                 # back to normal
        ref_normal = speak(lib, et, SENT)

        speak(lib, et, "40FSetting flatness from the text.")
        lib.echotalk_set_volume(et, lib.echotalk_volume(et))   # marks dirty
        after_push = speak(lib, et, SENT)

        # Compare CONTENT, not whole buffers. An utterance can land a
        # sample either side depending on where the interpolation boundary
        # falls, and requiring byte equality made this fail on a
        # one-sample difference while every state getter said flatness had
        # survived. Asking "is it closer to the flat rendering than to the
        # normal one" is what the check actually means.
        def meanDiff(a, b):
            # Tolerant of a small alignment shift. Sample-wise comparison
            # of speech is worthless if one buffer starts a sample early:
            # identical audio then reads as a large difference, which is
            # what made the first attempt at this check report 2271 for
            # two renderings that were the same voice.
            best = float("inf")
            for shift in range(-3, 4):
                sa = a[max(0, shift) * 2:]
                sb = b[max(0, -shift) * 2:]
                n = min(len(sa), len(sb)) // 2
                if not n:
                    continue
                va = struct.unpack(f"<{n}h", sa[:n * 2])
                vb = struct.unpack(f"<{n}h", sb[:n * 2])
                best = min(best, sum(abs(x - y) for x, y in zip(va, vb)) / n)
            return best

        dFlat = meanDiff(after_push, ref_flat)
        dNormal = meanDiff(after_push, ref_normal)
        f.check("flatness set in the text survives a settings push",
                dFlat * 10 < dNormal and lib.echotalk_flat(et) == 1,
                f"mean sample difference {dFlat:.1f} from the flat rendering "
                f"vs {dNormal:.1f} from the normal one")
        f.check("...and the flat and normal renderings really do differ",
                meanDiff(ref_flat, ref_normal) > 100,
                f"{meanDiff(ref_flat, ref_normal):.1f}")
        lib.echotalk_set_pitch(et, 24)
        lib.echotalk_set_flat(et, 0)

        # A one-character utterance must not clobber the caller's modes.
        speak(lib, et, "A")
        before = lib.echotalk_punctuation(et)
        speak(lib, et, "x")
        f.check("single character preserves punctuation mode",
                before == 2 and lib.echotalk_punctuation(et) == 2,
                f"{before} -> {lib.echotalk_punctuation(et)}")
        lib.echotalk_set_punctuation(et, 1)
        lib.echotalk_set_letter_mode(et, 0)
        lib.echotalk_set_compressed(et, 0)
        drain(lib, et)

        # --- stop() abandons pending text, not just queued audio ---
        lib.echotalk_speak(et, long_text.encode())
        lib.echotalk_stop(et)
        f.check("stop() discards unsynthesised text",
                lib.echotalk_pending(et) == 0 and lib.echotalk_available(et) == 0)
        f.check("read() after stop returns nothing",
                lib.echotalk_read(et, buf, 1024) == 0)
        after = speak(lib, et, "Hello there.")
        f.check("speech works again after stop", len(after) > 4000,
                f"{len(after) // 2} samples")
    finally:
        lib.echotalk_destroy(et)
        print("  ok    destroy")

    # Create must work again after destroy, or a host cannot change voice.
    err3 = ctypes.create_string_buffer(256)
    et2 = lib.echotalk_create(loader.encode(), obj.encode(), err3, len(err3))
    f.check("create again after destroy", bool(et2), err3.value.decode())
    if et2:
        lib.echotalk_destroy(et2)

    print()
    if f.count:
        print(f"{f.count} check(s) FAILED")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
