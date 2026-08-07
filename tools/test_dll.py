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
import sys

EXPECTED_ABI = 1


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
                      ("set_frame_rate", ctypes.c_int),
                      ("set_compressed", ctypes.c_int),
                      ("set_pitch", ctypes.c_int),
                      ("set_volume", ctypes.c_int),
                      ("set_word_delay", ctypes.c_int),
                      ("set_chunk_size", ctypes.c_uint),
                      ("set_raw", ctypes.c_int),
                      ("set_repeat_filter", ctypes.c_int)]:
        fn = getattr(lib, "echotalk_" + name)
        fn.restype = ctypes.c_int
        fn.argtypes = [p, arg]

    lib.echotalk_chunk_size.restype = ctypes.c_uint
    lib.echotalk_chunk_size.argtypes = [p]
    lib.echotalk_speak.restype = ctypes.c_int
    lib.echotalk_speak.argtypes = [p, ctypes.c_char_p]
    lib.echotalk_read.restype = ctypes.c_size_t
    lib.echotalk_read.argtypes = [p, ctypes.POINTER(ctypes.c_int16),
                                  ctypes.c_size_t]
    lib.echotalk_available.restype = ctypes.c_size_t
    lib.echotalk_available.argtypes = [p]
    lib.echotalk_stop.restype = None
    lib.echotalk_stop.argtypes = [p]
    lib.echotalk_command_errors.restype = ctypes.c_uint
    lib.echotalk_command_errors.argtypes = [p]
    lib.echotalk_clear_command_errors.restype = None
    lib.echotalk_clear_command_errors.argtypes = [p]


def drain(lib, et, block=1024):
    """Pull PCM the way an audio callback would, in small blocks."""
    buf = (ctypes.c_int16 * block)()
    out = bytearray()
    while True:
        n = lib.echotalk_read(et, buf, block)
        if n == 0:
            break
        out += bytes(buf)[:n * 2]
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

        # stop() must discard queued audio without disturbing Textalker.
        lib.echotalk_speak(et, b"Discard this please.")
        f.check("audio queued before stop", lib.echotalk_available(et) > 0)
        lib.echotalk_stop(et)
        f.check("stop drains the queue", lib.echotalk_available(et) == 0)
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
