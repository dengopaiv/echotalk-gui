#!/usr/bin/env python3
"""Smoke checks for the EchoTalk GUI that need the live window or the disk.

    python tools/verify_gui_smoke.py [ROMDIR]

1. Preview, Stop part-way, then Preview on the other voice: each reaches
   the Last result box, the window stays alive, and no error dialog opens.
   Buttons are driven with WM_COMMAND, since BM_CLICK is documented to fail
   on an inactive window, and the result box is read with WM_GETTEXT, since
   GetWindowText on another process's edit returns its creation-time text.
2. A Textalker pair in a folder named outside the ANSI code page renders --
   the manifest's UTF-8 code page at work. The images are copied into a
   temporary folder for this and deleted; nothing touches the repository.
3. say's --flat, --letter-mode and --punctuation produce the same WAV as the
   GUI for the same settings.

ROMDIR defaults to the unpacked add-on beside this repository.
"""
import ctypes, ctypes.wintypes as w, shutil, subprocess, sys, tempfile, time, wave
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ADDON = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO.parent / "echotalk" / "synthDrivers" / "echotalk"
EXE = REPO / "gui-native/build/echotalk_gui-x64.exe"
SAY = REPO / "gui-native/build/say-x64.exe"
SP = Path(tempfile.mkdtemp(prefix="echotalk_smoke_"))
u32 = ctypes.WinDLL("user32")
u32.GetDlgItem.restype = w.HWND
u32.GetDlgItem.argtypes = [w.HWND, ctypes.c_int]
WM_CLOSE = 0x0010
fails = 0

def dialogs(pid):
    found = []
    EnumProc = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    def cb(h, _):
        p = w.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        buf = ctypes.create_unicode_buffer(64)
        u32.GetClassNameW(h, buf, 64)
        if p.value == pid and buf.value == "#32770" and u32.IsWindowVisible(h):
            found.append(h)
        return True
    u32.EnumWindows(EnumProc(cb), 0)
    return found

# 1. Preview, Stop mid-utterance, Preview on the other voice, window alive, no dialog.
proc = subprocess.Popen([str(EXE), "--no-settings", "--rom-dir", str(ADDON)])
try:
    hwnd = None
    for _ in range(100):
        hwnd = u32.FindWindowW("EchoTalkGuiMainWindow", None)
        if hwnd: break
        time.sleep(0.05)
    time.sleep(0.5)
    text = u32.GetDlgItem(hwnd, 1001)
    u32.SendMessageW(text, 0x000C, 0, ctypes.c_wchar_p("This is a long enough sentence that stopping it part of the way through is a real test of Stop."))
    u32.PostMessageW(hwnd, 0x0111, 1047, u32.GetDlgItem(hwnd, 1047))
    time.sleep(1.0)
    buf = ctypes.create_unicode_buffer(512)
    u32.SendMessageW(u32.GetDlgItem(hwnd, 1054), 0x000D, 512, buf)
    print("after Preview:", buf.value)
    ok1 = buf.value.startswith("Playing:")
    u32.PostMessageW(hwnd, 0x0111, 1048, u32.GetDlgItem(hwnd, 1048))
    time.sleep(0.4)
    u32.SendMessageW(u32.GetDlgItem(hwnd, 1054), 0x000D, 512, buf)
    print("after Stop:", buf.value)
    ok2 = buf.value == "Stopped."
    voice = u32.GetDlgItem(hwnd, 1009)
    u32.SendMessageW(voice, 0x014E, 1, 0)   # CB_SETCURSEL to the second voice
    u32.PostMessageW(hwnd, 0x0111, (1 << 16) | 1009, voice)  # CBN_SELCHANGE
    u32.PostMessageW(hwnd, 0x0111, 1047, u32.GetDlgItem(hwnd, 1047))
    time.sleep(1.0)
    u32.SendMessageW(u32.GetDlgItem(hwnd, 1054), 0x000D, 512, buf)
    print("after Preview on voice 2:", buf.value)
    ok3 = buf.value.startswith("Playing:")
    alive = proc.poll() is None and u32.IsWindow(hwnd)
    d = dialogs(proc.pid)
    print("alive:", alive, "dialogs:", len(d))
    if not (ok1 and ok2 and ok3 and alive and not d):
        fails += 1
        print("FAIL preview/stop smoke")
    else:
        print("ok   preview/stop smoke")
    u32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    proc.wait(timeout=5)
finally:
    if proc.poll() is None:
        proc.kill()

# 2. A folder whose name is outside the ANSI code page.
uni = SP / "rõõm ÄÖÜ ☃"
if uni.exists():
    shutil.rmtree(uni)
uni.mkdir()
for f in ("textalker.ram.bin", "textalker.obj.bin"):
    shutil.copy(ADDON / f, uni / f)
out = SP / "uni.wav"
rc = subprocess.run([str(EXE), "--selftest", str(uni), "textalker", "50", "24", "12", "0", "99",
                     "50", "8000", "0", "0", "0", "0", "0", "80", "0", "0", str(out), "Hi."]).returncode
if rc != 0:
    fails += 1
    print("FAIL unicode folder: exit", rc)
else:
    with wave.open(str(out)) as wf:
        n = wf.getnframes()
    print("ok   unicode folder %r: %d samples" % (uni.name, n))
shutil.rmtree(uni)

# 3. say's new flags against the GUI's selftest for the same settings.
txt = SP / "t.txt"
txt.write_text("Hello, world. Testing letters and punctuation!", encoding="utf-8")
sw, gw = SP / "say.wav", SP / "gui.wav"
r1 = subprocess.run([str(SAY), "--flat", "--letter-mode", "1", "--punctuation", "2", "--pitch", "30",
                     "--file", str(txt), str(ADDON / "textalker.ram.bin"), str(ADDON / "textalker.obj.bin"),
                     str(sw)], capture_output=True).returncode
r2 = subprocess.run([str(EXE), "--selftest", str(ADDON), "textalker", "50", "30", "12", "0", "99",
                     "50", "8000", "1", "0", "0", "2", "3", "80", "0", "0", str(gw), "@" + str(txt)]).returncode
def pcm(p):
    with wave.open(str(p)) as wf:
        return wf.getframerate(), wf.readframes(wf.getnframes())
if r1 or r2 or pcm(sw) != pcm(gw):
    fails += 1
    print("FAIL say flags vs GUI", r1, r2, len(pcm(sw)[1]), len(pcm(gw)[1]))
else:
    print("ok   say --flat --letter-mode 1 --punctuation 2 identical to the GUI: %d samples" % (len(pcm(sw)[1]) // 2))
shutil.rmtree(SP, ignore_errors=True)
print("\nOK: smoke checks passed." if not fails else "\n%d smoke check(s) FAILED." % fails)
sys.exit(1 if fails else 0)
