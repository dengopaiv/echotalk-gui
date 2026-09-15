#!/usr/bin/env python3
"""Build, check and package the EchoTalk GUI for release.

    python tools/build_release.py [ROMDIR [ADDON_DLL]]

Modelled on STSPEECH's build_release.py. In order, stopping at the first
failure:

1. Wipe gui-native/build and build x64 from nothing, so the release is
   never made of stale objects.
2. Run tools/verify_gui.py (the exe against the add-on's own DLL),
   tools/verify_gui_keyboard.py and tools/verify_gui_smoke.py. ROMDIR and
   ADDON_DLL default to the unpacked add-on beside this repository and are
   only read.
3. Check the exe imports Windows system DLLs only.
4. Zip dist/echotalk-gui-<version>-windows-x64.zip: the GUI, say, README,
   NOTICE, LICENSE, THIRD_PARTY_LICENSES.md, and an empty roms/ holding only
   its README.
5. List the zip and FAIL if anything in it could be a Textalker image --
   any .bin, .dsk or .do file at all. The images are proprietary and are
   never shipped; this is the check that makes that true of the artefact
   rather than of our intentions.
6. Extract the zip into an empty temporary folder and run --selftest from
   there, which is the only check that catches the exe depending on
   something that merely happens to sit beside it in gui-native/build.
"""

import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GUI = ROOT / "gui-native"
BUILD = GUI / "build"
DIST = ROOT / "dist"
DEFAULT_ADDON = ROOT.parent / "echotalk" / "synthDrivers" / "echotalk"

SYSTEM_DLLS = {"user32.dll", "gdi32.dll", "comctl32.dll", "winmm.dll", "comdlg32.dll",
               "shell32.dll", "ole32.dll", "bcrypt.dll", "oleaut32.dll", "kernel32.dll"}
FORBIDDEN = re.compile(r"\.(bin|dsk|do|po|nib|2mg)$", re.IGNORECASE)


def step(title):
    print("\n=== %s ===" % title, flush=True)


def fail(msg):
    print("\nRELEASE FAILED: %s" % msg)
    sys.exit(1)


def run(args, **kw):
    print("> " + " ".join(str(a) for a in args), flush=True)
    return subprocess.run([str(a) for a in args], **kw).returncode


def version():
    rc = (GUI / "echotalk_gui.rc").read_text(encoding="utf-8")
    m = re.search(r'VALUE "ProductVersion",\s*"([0-9.]+)"', rc)
    if not m:
        fail("no ProductVersion in echotalk_gui.rc")
    parts = m.group(1).split(".")
    while len(parts) > 3 and parts[-1] == "0":
        parts.pop()
    return ".".join(parts)


def find_dumpbin():
    vs = Path(r"C:\Program Files\Microsoft Visual Studio")
    hits = sorted(vs.glob("*/*/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe"))
    return hits[-1] if hits else None


def main(argv):
    rom_dir = Path(argv[0]) if argv else DEFAULT_ADDON
    dll = Path(argv[1]) if len(argv) > 1 else DEFAULT_ADDON / "echotalk64.dll"
    ver = version()
    exe = BUILD / "echotalk_gui-x64.exe"
    say = BUILD / "say-x64.exe"

    step("1. clean x64 build")
    if BUILD.exists():
        shutil.rmtree(BUILD)
    if run(["cmd", "/c", GUI / "build.cmd", "x64"]) != 0 or not exe.is_file() or not say.is_file():
        fail("build.cmd x64")

    step("2. checks")
    for script, args in (("verify_gui.py", [rom_dir, dll]),
                         ("verify_gui_keyboard.py", [rom_dir]),
                         ("verify_gui_smoke.py", [rom_dir])):
        if run([sys.executable, ROOT / "tools" / script] + args) != 0:
            fail(script)

    step("3. imports")
    dumpbin = find_dumpbin()
    if dumpbin is None:
        fail("dumpbin.exe not found")
    out = subprocess.run([str(dumpbin), "/nologo", "/dependents", str(exe)],
                         capture_output=True, text=True).stdout
    dlls = {l.strip().lower() for l in out.splitlines() if l.strip().lower().endswith(".dll")}
    print("imports: " + ", ".join(sorted(dlls)))
    if not dlls or not dlls <= SYSTEM_DLLS:
        fail("unexpected imports: %s" % ", ".join(sorted(dlls - SYSTEM_DLLS)))

    step("4. package")
    DIST.mkdir(exist_ok=True)
    name = "echotalk-gui-%s-windows-x64" % ver
    zpath = DIST / (name + ".zip")
    if zpath.exists():
        zpath.unlink()
    roms_readme = (
        "Put your own Textalker images here: <name>.ram.bin and <name>.obj.bin,\r\n"
        "one pair per voice. They are proprietary and are not included.\r\n"
        "See \"Obtaining the Textalker images\" in README-library.md.\r\n")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(exe, name + "/echotalk_gui-x64.exe")
        z.write(say, name + "/say-x64.exe")
        z.write(GUI / "README.md", name + "/README.md")
        z.write(GUI / "NOTICE.md", name + "/NOTICE.md")
        z.write(ROOT / "README.md", name + "/README-library.md")
        z.write(ROOT / "LICENSE", name + "/LICENSE")
        z.write(ROOT / "THIRD_PARTY_LICENSES.md", name + "/THIRD_PARTY_LICENSES.md")
        z.writestr(name + "/roms/README.txt", roms_readme)
    print("wrote %s (%d bytes)" % (zpath, zpath.stat().st_size))

    step("5. no images in the archive")
    with zipfile.ZipFile(zpath) as z:
        names = z.namelist()
    for n in names:
        print("  " + n)
    bad = [n for n in names if FORBIDDEN.search(n)]
    if bad:
        zpath.unlink()
        fail("the archive contained possible Textalker images and was deleted: %s" % bad)
    print("ok: no .bin, .dsk or disk-image files")

    step("6. run from a clean extraction")
    with tempfile.TemporaryDirectory(prefix="echotalk_release_") as tmp:
        with zipfile.ZipFile(zpath) as z:
            z.extractall(tmp)
        ex = Path(tmp) / name / "echotalk_gui-x64.exe"
        wav = Path(tmp) / "out.wav"
        stems = sorted(p.name[:-len(".ram.bin")] for p in rom_dir.glob("*.ram.bin"))
        if not stems:
            fail("no images in %s to test the extracted exe with" % rom_dir)
        rc = run([ex, "--selftest", rom_dir, stems[0], "50", "24", "12", "0", "99", "50",
                  "8000", "0", "0", "0", "0", "0", "80", "0", "0", wav,
                  "Released and running from a clean folder."])
        if rc != 0 or not wav.is_file() or wav.stat().st_size < 1000:
            fail("the extracted exe could not render (exit %d)" % rc)
        print("ok: %d bytes of WAV from the extracted exe" % wav.stat().st_size)

    print("\nRELEASE OK: %s" % zpath)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
