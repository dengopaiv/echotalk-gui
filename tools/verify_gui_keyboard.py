#!/usr/bin/env python3
"""Keyboard and screen-reader checks for the EchoTalk GUI's real window.

    python tools/verify_gui_keyboard.py [ROMDIR]

Adapted from Votraxxion's tools/verify_gui_keyboard.py and the Votrax SC-01
ROM GUI's verify_rom_gui_keyboard.py (same author, BSD-3-Clause), whose
reasoning is kept below.

verify_gui.py proves the executable renders the right audio. It says nothing
about whether anyone can reach the button that renders it. This launches the
real GUI (with --no-settings, so a user's remembered settings never decide a
result, and --rom-dir so every voice control is populated) and:

  * walks the tab order by posting actual VK_TAB messages into its queue --
    the same messages a keypress delivers, processed by the same
    IsDialogMessage call -- and reads back where focus went. Tab must leave
    every control, the multiline boxes included; every WS_TABSTOP must be
    reached; the ring must close, forwards and with Shift+Tab.
  * checks no two controls claim the same Alt accelerator. Windows does not
    complain about a duplicate; the key just cycles between the claimants.
  * reads every tab stop's MSAA name, role and value through oleacc, the way
    NVDA reads a Win32 control. A slider with no name is announced as
    "slider 50" and nothing else. Each slider's value must be the setting's
    own number.
  * presses arrow keys on the sliders: Rate moves by the add-on's minStep of
    2, Pitch by one Textalker step; the readout follows; the Voice preset
    combo is derived from the values, so it says Custom off the defaults and
    Echo II defaults back on them; a raised chip clock shows the raised
    output rate.

Exit status 0 when everything passes.
"""
import ctypes
import ctypes.wintypes as w
import os
import subprocess
import sys
import time

import comtypes.client
from comtypes.automation import VARIANT

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'gui-native', 'build', 'echotalk_gui-x64.exe')
DEFAULT_ROMS = os.path.join(os.path.dirname(ROOT), 'echotalk', 'synthDrivers', 'echotalk')
WINDOW_CLASS = 'EchoTalkGuiMainWindow'

WM_KEYDOWN, WM_KEYUP, WM_CLOSE, WM_GETDLGCODE = 0x0100, 0x0101, 0x0010, 0x0087
VK_TAB, VK_SHIFT, VK_LEFT, VK_RIGHT, VK_HOME, VK_END = 0x09, 0x10, 0x25, 0x27, 0x24, 0x23
GWL_STYLE, GWL_ID = -16, -12
WS_TABSTOP, WS_DISABLED = 0x00010000, 0x08000000
TBM_GETPOS, CB_GETCURSEL, OBJID_CLIENT = 0x0400, 0x0147, -4

DLGC = [(0x0001, 'WANTARROWS'), (0x0002, 'WANTTAB'), (0x0004, 'WANTALLKEYS'),
        (0x0008, 'HASSETSEL'), (0x0010, 'DEFPUSHBUTTON'), (0x0080, 'WANTCHARS')]

# gui-native/resource.h
NAMES = {
    1001: 'text box', 1003: 'ROM folder', 1004: 'Browse', 1006: 'ROM status',
    1009: 'voice', 1011: 'rate', 1012: 'rate readout', 1014: 'pitch',
    1015: 'pitch readout', 1017: 'volume', 1020: 'word delay', 1023: 'repeat filter',
    1026: 'chip clock', 1029: 'sample rate', 1030: 'sample rate readout',
    1031: 'monotone', 1032: 'compressed', 1035: 'frame rate', 1037: 'reading mode',
    1039: 'punctuation', 1041: 'chunk size', 1043: 'raw text', 1044: 'obey commands',
    1046: 'voice preset', 1047: 'Preview', 1048: 'Stop', 1049: 'Render',
    1050: 'Save preset', 1051: 'Load preset', 1052: 'Copy say', 1053: 'Batch',
    1054: 'Sweep', 1056: 'last result',
}
TEXT, SLIDER, COMBO, BUTTON, CHECK = 42, 51, 46, 43, 44
EXPECTED = {   # role, and a word the accessible name must contain
    'text box': (TEXT, 'Text'), 'ROM folder': (TEXT, 'ROM folder'),
    'Browse': (BUTTON, 'Browse'), 'ROM status': (TEXT, 'ROM status'),
    'voice': (COMBO, 'Voice'), 'rate': (SLIDER, 'Rate'), 'pitch': (SLIDER, 'Pitch'),
    'volume': (SLIDER, 'Volume'), 'word delay': (SLIDER, 'Delay between words'),
    'repeat filter': (SLIDER, 'Repeat-character filter'),
    'chip clock': (SLIDER, 'Chip clock'), 'sample rate': (COMBO, 'Output sample rate'),
    'monotone': (CHECK, 'Monotone'), 'compressed': (CHECK, 'Compressed speech'),
    'frame rate': (COMBO, 'Frame rate'), 'reading mode': (COMBO, 'Reading mode'),
    'punctuation': (COMBO, 'Punctuation'), 'chunk size': (SLIDER, 'Chunk size'),
    'raw text': (CHECK, 'Raw text'), 'obey commands': (CHECK, 'Obey embedded commands'),
    'voice preset': (COMBO, 'Voice preset'), 'Preview': (BUTTON, 'Preview'),
    'Stop': (BUTTON, 'Stop'), 'Render': (BUTTON, 'Render to WAV'),
    'Save preset': (BUTTON, 'Save preset'), 'Load preset': (BUTTON, 'Load preset'),
    'Copy say': (BUTTON, 'say command line'), 'Batch': (BUTTON, 'Batch render'),
    'Sweep': (BUTTON, 'Pitch sweep'), 'last result': (TEXT, 'Last result'),
}
SLIDER_VALUES = {'rate': '50', 'pitch': '24', 'volume': '12', 'word delay': '0',
                 'repeat filter': '99', 'chip clock': '50', 'chunk size': '80'}

u32 = ctypes.WinDLL('user32', use_last_error=True)
u32.GetDlgItem.restype = w.HWND
u32.GetDlgItem.argtypes = [w.HWND, ctypes.c_int]
u32.SendMessageW.restype = ctypes.c_ssize_t
u32.SendMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]


class GUITHREADINFO(ctypes.Structure):
    _fields_ = [('cbSize', w.DWORD), ('flags', w.DWORD), ('hwndActive', w.HWND),
                ('hwndFocus', w.HWND), ('hwndCapture', w.HWND), ('hwndMenuOwner', w.HWND),
                ('hwndMoveSize', w.HWND), ('hwndCaret', w.HWND), ('rcCaret', w.RECT)]


ENUMPROC = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)


def find_window(timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        hwnd = u32.FindWindowW(WINDOW_CLASS, None)
        if hwnd:
            return hwnd
        time.sleep(0.05)
    return None


def children(parent):
    out = []
    u32.EnumChildWindows(parent, ENUMPROC(lambda h, _: out.append(h) or True), 0)
    return out


def describe(hwnd):
    if not hwnd:
        return 'nothing'
    cid = u32.GetWindowLongW(hwnd, GWL_ID)
    return NAMES.get(cid, 'control %d' % cid)


def tabstops(parent):
    out = []
    for h in children(parent):
        style = u32.GetWindowLongW(h, GWL_STYLE)
        if (style & WS_TABSTOP) and not (style & WS_DISABLED) and u32.IsWindowVisible(h):
            out.append(h)
    return out


def focus_of(thread_id):
    info = GUITHREADINFO()
    info.cbSize = ctypes.sizeof(info)
    if not u32.GetGUIThreadInfo(thread_id, ctypes.byref(info)):
        return None
    return info.hwndFocus


def settle(hwnd, thread_id, timeout=10.0):
    """FindWindow succeeds while WM_CREATE is still running; wait for the
    child count to stop changing, then for focus to reach a control."""
    deadline = time.time() + timeout
    count = -1
    while time.time() < deadline:
        now = len(children(hwnd))
        if now == count and now > 0:
            break
        count = now
        time.sleep(0.1)
    while time.time() < deadline:
        focus = focus_of(thread_id)
        if focus and focus != hwnd and u32.IsChild(hwnd, focus):
            return focus
        time.sleep(0.05)
    return None


def press_tab(hwnd, thread_id, shift=False, timeout=2.0):
    """Post a real Tab and wait for focus to move; a trap costs the timeout."""
    before = focus_of(thread_id)
    if shift:
        u32.PostMessageW(hwnd, WM_KEYDOWN, VK_SHIFT, 1)
    u32.PostMessageW(hwnd, WM_KEYDOWN, VK_TAB, 1)
    u32.PostMessageW(hwnd, WM_KEYUP, VK_TAB, 0xC0000001)
    if shift:
        u32.PostMessageW(hwnd, WM_KEYUP, VK_SHIFT, 0xC0000001)
    deadline = time.time() + timeout
    while time.time() < deadline:
        now = focus_of(thread_id)
        if now and now != before:
            return now
        time.sleep(0.02)
    return focus_of(thread_id)


def walk(thread_id, start, steps, shift=False):
    seen, cur = [], start
    for _ in range(steps):
        nxt = press_tab(cur, thread_id, shift)
        seen.append(nxt)
        if not nxt or nxt == cur:
            break
        cur = nxt
    return seen


def check_tab_order(hwnd, thread_id, start):
    failures = 0
    stops = tabstops(hwnd)
    print('  start: focus on the %s; %d tab stops' % (describe(start), len(stops)))
    seen = walk(thread_id, start, len(stops) + 1)
    cur = start
    for hit in seen:
        if hit == cur:
            code = u32.SendMessageW(cur, WM_GETDLGCODE, 0, 0)
            print('  FAIL Tab does not leave the %s (WM_GETDLGCODE 0x%04x: %s)'
                  % (describe(cur), code, ', '.join(n for b, n in DLGC if code & b)))
            failures += 1
            break
        cur = hit
    missed = [h for h in stops if h not in set(seen) | {start}]
    for h in missed:
        print('  FAIL Tab never reaches the %s' % describe(h))
    failures += 1 if missed else 0
    if not missed:
        print('  ok   forwards: %s' % ' -> '.join(describe(h) for h in [start] + seen[:len(stops)]))
    if len(seen) >= len(stops) and seen[len(stops) - 1] != start:
        print('  FAIL the ring does not close: %d tabs from the %s landed on the %s'
              % (len(stops), describe(start), describe(seen[len(stops) - 1])))
        failures += 1
    back = walk(thread_id, focus_of(thread_id), len(stops), shift=True)
    missed_back = [h for h in stops if h not in set(back)]
    for h in missed_back:
        print('  FAIL Shift+Tab never reaches the %s' % describe(h))
    failures += 1 if missed_back else 0
    if not missed_back:
        print('  ok   backwards: Shift+Tab walks the same ring')
    return failures


def check_accelerators(hwnd):
    seen, clashes = {}, []
    for h in children(hwnd):
        buf = ctypes.create_unicode_buffer(512)
        u32.GetWindowTextW(h, buf, 512)
        text = buf.value
        i = text.find('&')
        while i >= 0 and i + 1 < len(text) and text[i + 1] == '&':
            i = text.find('&', i + 2)
        if i < 0 or i + 1 >= len(text):
            continue
        k = text[i + 1].upper()
        if k in seen:
            clashes.append((k, seen[k], text))
        else:
            seen[k] = text
    for k, first, second in clashes:
        print('  FAIL Alt+%s is claimed by both "%s" and "%s"' % (k, first, second))
    if not clashes:
        print('  ok   %d accelerators, all distinct: %s' % (len(seen), ' '.join(sorted(seen))))
    return len(clashes)


def msaa(hwnd):
    from comtypes.gen.Accessibility import IAccessible
    ptr = ctypes.POINTER(IAccessible)()
    ctypes.oledll.oleacc.AccessibleObjectFromWindow(
        hwnd, OBJID_CLIENT, ctypes.byref(IAccessible._iid_), ctypes.byref(ptr))
    child = VARIANT(0)
    name, role = ptr.accName(child), ptr.accRole(child)
    try:
        value = ptr.accValue(child)
    except Exception:
        value = None
    return name, role, value


def check_names(hwnd):
    failures = 0
    for h in tabstops(hwnd):
        what = describe(h)
        name, role, value = msaa(h)
        want_role, want_word = EXPECTED.get(what, (None, None))
        if not name or not name.strip():
            print('  FAIL the %s has no accessible name' % what)
            failures += 1
        elif want_role is not None and role != want_role:
            print('  FAIL the %s has role %s, want %s' % (what, role, want_role))
            failures += 1
        elif want_word and want_word.lower() not in name.replace('&', '').lower():
            print('  FAIL the %s is named "%s", expected it to say "%s"' % (what, name, want_word))
            failures += 1
        elif what in SLIDER_VALUES and value != SLIDER_VALUES[what]:
            print('  FAIL the %s slider reads "%s", want its default "%s"'
                  % (what, value, SLIDER_VALUES[what]))
            failures += 1
        else:
            shown = name if role != SLIDER and role != COMBO else '%s = %s' % (name, value)
            print('  ok   %-14s role %-2d "%s"' % (what, role, shown))
    return failures


def text_of(hwnd):
    buf = ctypes.create_unicode_buffer(512)
    u32.GetWindowTextW(hwnd, buf, 512)
    return buf.value


def key(hwnd, vk):
    u32.PostMessageW(hwnd, WM_KEYDOWN, vk, 1)
    u32.PostMessageW(hwnd, WM_KEYUP, vk, 0xC0000001)
    time.sleep(0.2)
    return u32.SendMessageW(hwnd, TBM_GETPOS, 0, 0)


def check_sliders(hwnd):
    failures = 0

    def expect(label, got, want):
        nonlocal failures
        if got != want:
            print('  FAIL %s: got %r, want %r' % (label, got, want))
            failures += 1
        else:
            print('  ok   %s: %r' % (label, got))

    rate = u32.GetDlgItem(hwnd, 1011)
    pitch = u32.GetDlgItem(hwnd, 1014)
    clock = u32.GetDlgItem(hwnd, 1026)
    preset = u32.GetDlgItem(hwnd, 1046)
    u32.SetFocus(rate)

    expect('rate arrows step by 2', [key(rate, VK_RIGHT), key(rate, VK_LEFT)], [52, 50])
    expect('pitch right arrow', key(pitch, VK_RIGHT), 25)
    expect('pitch readout at 25', text_of(u32.GetDlgItem(hwnd, 1015)), 'NVDA 40%')
    # The system proxy would say "40" here (a percentage of 0-63); the
    # annotation must make NVDA read the Textalker number, and follow it.
    expect('pitch accessible value follows the slider', msaa(pitch)[2], '25')
    expect('preset off the defaults', u32.SendMessageW(preset, CB_GETCURSEL, 0, 0), 1)
    expect('pitch left arrow', key(pitch, VK_LEFT), 24)
    expect('preset back on the defaults', u32.SendMessageW(preset, CB_GETCURSEL, 0, 0), 0)
    expect('chip clock End', key(clock, VK_END), 100)
    expect('clock raises the output rate', text_of(u32.GetDlgItem(hwnd, 1030)),
           'raised to 32000 Hz by the clock')
    key(clock, VK_HOME)
    for _ in range(25):
        key(clock, VK_RIGHT)
    expect('chip clock back to 50', u32.SendMessageW(clock, TBM_GETPOS, 0, 0), 50)
    expect('output rate back to the choice', text_of(u32.GetDlgItem(hwnd, 1030)),
           'output 8000 Hz')
    return failures


def main(argv):
    rom_dir = os.path.abspath(argv[0]) if argv else DEFAULT_ROMS
    if not os.path.isfile(EXE):
        print('not built: %s' % EXE)
        return 1
    comtypes.client.GetModule('oleacc.dll')
    proc = subprocess.Popen([EXE, '--no-settings', '--rom-dir', rom_dir])
    failures = 0
    try:
        hwnd = find_window()
        if not hwnd:
            print('FAIL the window never appeared')
            return 1
        thread_id = u32.GetWindowThreadProcessId(hwnd, None)
        start = settle(hwnd, thread_id)
        if not start:
            print('FAIL focus never reached a control')
            return 1
        print('tab order:')
        failures += check_tab_order(hwnd, thread_id, start)
        print('accelerators:')
        failures += check_accelerators(hwnd)
        print('names, roles and values:')
        failures += check_names(hwnd)
        print('sliders, readouts and the preset combo:')
        failures += check_sliders(hwnd)
        u32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    finally:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    if failures:
        print('\n%d problem(s).' % failures)
        return 1
    print('\nOK: reachable, named, and the sliders behave as the add-on\'s do.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
