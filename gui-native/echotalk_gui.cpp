/*
 * echotalk_gui.cpp - EchoTalk desktop GUI: an emulated Apple II Echo II,
 * Street Electronics' real Textalker running on an emulated 6502 driving
 * a TMS5220 emulation, with every setting exposed, Preview and Render to
 * WAV.
 *
 * The engine is this repository's library (src/echotalk.h), compiled
 * straight in. The settings, and the way the engine is driven, are those of
 * Jayson Smith's EchoTalk NVDA add-on (nvda-addon/synthDrivers/echotalk/
 * __init__.py), followed step by step in Render():
 *
 *      Voice                one per Textalker image pair found
 *      Rate                 0-100; speed 2^((rate-50)/25), pitch held
 *      Pitch                Textalker 0-63
 *      Volume               Textalker 0-15
 *      Delay between words  Textalker 0-15 (no effect on 1.3)
 *      Repeat filter        Textalker 0-99 (99 never triggers)
 *      Chip clock           0-100; clock 2^((clock-50)/25), pitch moves
 *      Output sample rate   a floor, raised to 8000 x clock
 *      Monotone, Compressed speech
 *
 * plus what the library has and the add-on does not show: the TMS5220's
 * own frame rate, Textalker's letter and punctuation modes, chunk size,
 * raw text, and an opt-in to obey commands embedded in the text.
 *
 * The Textalker images are proprietary and are not compiled in or shipped;
 * the program finds the user's own. See gui-native/README.md.
 *
 * The layout follows the Votrax SC-01 ROM GUI and the SAM, STSPEECH and
 * Votrax Native GUIs: plain Win32 controls, each with a static label
 * immediately before it in z-order and an & accelerator, IsDialogMessage in
 * the message loop, and the multiline boxes subclassed so Tab leaves them.
 * A front end for a screen reader voice that a screen reader cannot drive
 * would be worthless, so none of that is decoration.
 *
 * Sliders are sliders because they are sliders in NVDA's own voice
 * settings. Where the add-on's value is a Textalker number, the slider's
 * position IS that number, so NVDA announces "Pitch slider 24" rather than
 * a percentage; the readout beside it gives the NVDA percentage that
 * corresponds, so a voice can be carried between the two.
 *
 * BSD-3-Clause, as the rest of this repository. See gui-native/NOTICE.md.
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX

#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <commdlg.h>
#include <initguid.h>   /* so oleacc.h defines PROPID_ACC_VALUE rather than just declaring it */
#include <mmsystem.h>
#include <oleacc.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "echotalk.h"   /* declares its own extern "C" */

#include "resource.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "oleaut32.lib")

/* --------------------------------------------------------------------- */
/* The add-on's parameter set                                            */
/* --------------------------------------------------------------------- */

/* Library ranges, as the add-on names them. */
static const int PITCH_MAX = 63;
static const int VOLUME_MAX = 15;
static const int DELAY_MAX = 15;
static const int REPEAT_MAX = 99;
static const int CHUNK_MAX = 255;

/* What the TMS5220 produces at a 1.0 chip clock. */
static const int CHIP_HZ = 8000;

/* The add-on reads in blocks of this many samples. */
static const size_t SAMPLES_PER_READ = 1024;

/* The add-on's seven output rates, in its order. */
static const int SAMPLE_RATES[] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000 };
static const wchar_t *SAMPLE_RATE_NAMES[] = {
    L"8 kHz (native)", L"11 kHz", L"16 kHz", L"22 kHz", L"32 kHz", L"44 kHz", L"48 kHz",
};
static const int SAMPLE_RATE_COUNT = 7;

static const wchar_t *FRAME_RATE_NAMES[] = {
    L"8 periods per frame (normal)", L"6 periods (about 1.31x)",
    L"4 periods (about 1.89x)", L"2 periods (about 3.40x)",
};
static const wchar_t *READING_NAMES[] = {
    L"Textalker's startup mode", L"Words", L"Spell out letters",
};
static const wchar_t *PUNCT_NAMES[] = {
    L"Textalker's startup mode", L"None", L"Some", L"All",
};

static const wchar_t *DEFAULT_TEXT =
    L"Hello. This is Textalker, running on an emulated Echo two.";
static const wchar_t *WINDOW_TITLE = L"EchoTalk GUI (emulated Echo II)";
static const wchar_t *WINDOW_CLASS = L"EchoTalkGuiMainWindow";

/* Every value in the units the controls hold. Rate and clock are the
 * add-on's 0-100 slider positions; pitch, volume, delay, repeat and chunk
 * are Textalker's own numbers; srate is Hz. */
struct Settings {
    int  rate = 50;
    int  pitch = 24;
    int  volume = 12;
    int  delay = 0;
    int  repeat = 99;
    int  clock = 50;
    int  srate = 8000;
    bool monotone = false;
    bool compressed = false;
    int  frameRate = 0;       /* 0-3 */
    int  reading = 0;         /* 0 startup (not sent), 1 words, 2 spell */
    int  punct = 0;           /* 0 startup (not sent), 1 none, 2 some, 3 all */
    int  chunk = 80;          /* 0 off */
    bool raw = false;
    bool obey = false;        /* not a Textalker setting; not part of a preset match */
};

/* Echo II defaults: every row except Obey, which is about the text box. */
static bool IsEchoDefaults(const Settings &s)
{
    const Settings d;
    return s.rate == d.rate && s.pitch == d.pitch && s.volume == d.volume
        && s.delay == d.delay && s.repeat == d.repeat && s.clock == d.clock
        && s.srate == d.srate && s.monotone == d.monotone
        && s.compressed == d.compressed && s.frameRate == d.frameRate
        && s.reading == d.reading && s.punct == d.punct && s.chunk == d.chunk
        && s.raw == d.raw;
}

static int Clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Python's round() rounds halves to even, and the add-on's mappings use
 * it. nearbyint under the default rounding mode does the same. */
static int PyRound(double x)
{
    return (int)std::nearbyint(x);
}

/* _toCard: NVDA's 0-100 slider -> a library value. */
static int ToCard(int pct, int maxVal)
{
    return Clamp(PyRound(pct * (double)maxVal / 100.0), 0, maxVal);
}

/* _toPct: the inverse. */
static int ToPct(int card, int maxVal)
{
    return Clamp(PyRound(card * 100.0 / maxVal), 0, 100);
}

/* _toMult: two octaves either side of normal; 50 is 1.0, every 25 doubles. */
static double ToMult(int pct)
{
    return std::pow(2.0, (pct - 50) / 25.0);
}

static bool IsSampleRate(int hz)
{
    for (int i = 0; i < SAMPLE_RATE_COUNT; i++) {
        if (SAMPLE_RATES[i] == hz) {
            return true;
        }
    }
    return false;
}

/* _effectiveSamplerate: the chosen rate is a floor, raised to meet what the
 * chip really produces whenever the clock outruns it. Rounded as the
 * library rounds it. */
static int EffectiveRate(const Settings &s)
{
    int native = (int)(CHIP_HZ * ToMult(s.clock) + 0.5);
    return std::max(s.srate, native);
}

static void Sanitize(Settings &s)
{
    const Settings d;
    s.rate = Clamp(s.rate, 0, 100);
    s.pitch = Clamp(s.pitch, 0, PITCH_MAX);
    s.volume = Clamp(s.volume, 0, VOLUME_MAX);
    s.delay = Clamp(s.delay, 0, DELAY_MAX);
    s.repeat = Clamp(s.repeat, 0, REPEAT_MAX);
    s.clock = Clamp(s.clock, 0, 100);
    if (!IsSampleRate(s.srate)) {
        s.srate = d.srate;
    }
    s.frameRate = Clamp(s.frameRate, 0, 3);
    s.reading = Clamp(s.reading, 0, 2);
    s.punct = Clamp(s.punct, 0, 3);
    s.chunk = Clamp(s.chunk, 0, CHUNK_MAX);
}

/* --------------------------------------------------------------------- */
/* Small helpers                                                         */
/* --------------------------------------------------------------------- */

static HINSTANCE g_inst;
static HWND g_main;
static HFONT g_font;
static int g_dpi = 96;

/* Pixel sizes are written for 96 DPI and scaled here. */
static int Px(int v)
{
    return MulDiv(v, g_dpi, 96);
}

static std::string ToUtf8(const std::wstring &w)
{
    if (w.empty()) {
        return std::string();
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s((size_t)std::max(n, 0), '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    }
    return s;
}

static std::wstring FromUtf8(const std::string &s)
{
    if (s.empty()) {
        return std::wstring();
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w((size_t)std::max(n, 0), L'\0');
    if (n > 0) {
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    }
    return w;
}

static std::wstring Format(const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = L'\0';
    return buf;
}

static std::wstring GetText(HWND hwnd)
{
    int n = GetWindowTextLengthW(hwnd);
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(hwnd, &s[0], n + 1);
    s.resize((size_t)n);
    return s;
}

static std::wstring Trim(const std::wstring &s)
{
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) {
        return std::wstring();
    }
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::wstring JoinPath(const std::wstring &dir, const std::wstring &name)
{
    if (dir.empty()) {
        return name;
    }
    wchar_t last = dir[dir.size() - 1];
    if (last == L'\\' || last == L'/') {
        return dir + name;
    }
    return dir + L"\\" + name;
}

static bool FileExists(const std::wstring &path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool DirExists(const std::wstring &path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool ReadWholeFile(const std::wstring &path, std::vector<unsigned char> &out)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < (1LL << 30);
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out.empty() || (ReadFile(f, &out[0], (DWORD)out.size(), &got, NULL)
                             && got == out.size());
    }
    CloseHandle(f);
    return ok;
}

/* Written beside the target and renamed over it, so a failure part-way
 * never leaves half a WAV under the name the user chose. */
static bool WriteWholeFile(const std::wstring &path, const std::vector<unsigned char> &data)
{
    std::wstring temp = path + L".partial";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    bool ok = data.empty() || (WriteFile(f, &data[0], (DWORD)data.size(), &written, NULL)
                               && written == data.size());
    CloseHandle(f);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

static std::wstring Md5Hex(const std::vector<unsigned char> &data)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    unsigned char digest[16];
    std::wstring hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, NULL, 0) < 0) {
        return hex;
    }
    unsigned char empty = 0;
    if (BCryptHash(alg, NULL, 0, data.empty() ? &empty : (PUCHAR)&data[0],
                   (ULONG)data.size(), digest, sizeof(digest)) >= 0) {
        for (unsigned char b : digest) {
            hex += Format(L"%02x", b);
        }
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

static std::wstring ExeDir()
{
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH * 2);
    std::wstring s(path, n);
    size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : s.substr(0, slash);
}

static std::wstring AppDataDir()
{
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, path))) {
        return path;
    }
    return std::wstring();
}

/* --------------------------------------------------------------------- */
/* Textalker image pairs                                                 */
/* --------------------------------------------------------------------- */

/* One voice: a loader and an OBJ image, labelled with the banner read out
 * of the loader itself. */
struct VoicePair {
    std::wstring stem, loader, obj;
    std::wstring banner;               /* "3.1.3", "1.3", ... */
    std::wstring loaderMd5, objMd5;
    size_t       loaderSize = 0, objSize = 0;
};

/* The 6502 core keeps its registers in globals, so exactly one echotalk
 * instance may exist at a time. Probing a voice and rendering both create
 * one, and they can happen on different threads, so every instance's
 * whole life is spent inside this lock. */
static CRITICAL_SECTION g_engine;

static bool EndsWithNoCase(const std::wstring &s, const wchar_t *suffix)
{
    size_t n = wcslen(suffix);
    return s.size() > n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

/* The add-on's _imagePairs: <stem>.ram.bin (or <stem>.loader.bin) beside
 * <stem>.obj.bin, in sorted file-name order. Each is then booted briefly for
 * its banner, as _probeBanner does; `status` gets one line per pair. */
static std::vector<VoicePair> FindVoices(const std::wstring &dir, std::wstring &status)
{
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*.bin").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                names.push_back(fd.cFileName);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(names.begin(), names.end());

    std::vector<VoicePair> voices;
    for (const std::wstring &name : names) {
        const wchar_t *suffix = EndsWithNoCase(name, L".ram.bin") ? L".ram.bin"
                              : EndsWithNoCase(name, L".loader.bin") ? L".loader.bin"
                              : NULL;
        if (suffix == NULL) {
            continue;
        }
        VoicePair v;
        v.stem = name.substr(0, name.size() - wcslen(suffix));
        v.loader = JoinPath(dir, name);
        v.obj = JoinPath(dir, v.stem + L".obj.bin");
        if (!FileExists(v.obj)) {
            status += Format(L"%s: no %s.obj.bin beside it, skipped.\r\n",
                             name.c_str(), v.stem.c_str());
            continue;
        }

        char err[256] = "";
        EnterCriticalSection(&g_engine);
        echotalk *et = echotalk_create(ToUtf8(v.loader).c_str(), ToUtf8(v.obj).c_str(),
                                       err, sizeof(err));
        if (et != NULL) {
            const char *b = echotalk_version(et);
            v.banner = FromUtf8(b != NULL && b[0] ? b : "unknown");
            echotalk_destroy(et);
        }
        LeaveCriticalSection(&g_engine);

        if (et == NULL) {
            status += Format(L"%s + %s.obj.bin: refused - %hs\r\n", name.c_str(),
                             v.stem.c_str(), err[0] ? err : "the engine could not start");
            continue;
        }
        std::vector<unsigned char> bytes;
        if (ReadWholeFile(v.loader, bytes)) {
            v.loaderSize = bytes.size();
            v.loaderMd5 = Md5Hex(bytes);
        }
        if (ReadWholeFile(v.obj, bytes)) {
            v.objSize = bytes.size();
            v.objMd5 = Md5Hex(bytes);
        }
        status += Format(L"%s + %s.obj.bin: loaded, Textalker %s (%zu + %zu bytes).\r\n",
                         name.c_str(), v.stem.c_str(), v.banner.c_str(),
                         v.loaderSize, v.objSize);
        voices.push_back(v);
    }
    return voices;
}

/* The add-on's _defaultVoice: Textalker 3.x if it is there, otherwise the
 * first pair found. */
static int DefaultVoice(const std::vector<VoicePair> &voices)
{
    for (size_t i = 0; i < voices.size(); i++) {
        if (voices[i].banner.compare(0, 2, L"3.") == 0 || voices[i].banner == L"3") {
            return (int)i;
        }
    }
    return voices.empty() ? -1 : 0;
}

static bool HasVoices(const std::wstring &dir)
{
    if (!DirExists(dir)) {
        return false;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*.obj.bin").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    FindClose(h);
    return true;
}

/* Where to look when nobody has said: beside the program, roms\ beside it,
 * this repository's roms\ when run from gui-native\build\, and the add-on's
 * own folder, so a machine with the add-on installed needs no setup. */
static std::vector<std::wstring> DefaultRomDirs()
{
    std::wstring exe = ExeDir();
    std::vector<std::wstring> dirs = {
        exe,
        JoinPath(exe, L"roms"),
        JoinPath(exe, L"..\\..\\roms"),
    };
    std::wstring appData = AppDataDir();
    if (!appData.empty()) {
        dirs.push_back(JoinPath(appData, L"nvda\\addons\\echotalk\\synthDrivers\\echotalk"));
    }
    return dirs;
}

/* --------------------------------------------------------------------- */
/* Images carried inside the executable                                  */
/* --------------------------------------------------------------------- */

/*
 * A build of this program may carry Textalker image pairs as resources of
 * type TEXTALKER, named by file name ("TEXTALKER.RAM.BIN"). The published
 * build never does -- the images are proprietary and are not distributed
 * -- but a private build for one's own machine can, so that nobody has to
 * find, choose or even see the image files.
 *
 * echotalk_create() takes paths, so the resources are written to a folder
 * of this process's own under %TEMP% at startup and deleted at exit. The
 * window then hides the ROM folder, Browse and ROM status controls
 * entirely; everything else behaves as usual.
 */
static const wchar_t *IMAGE_RESOURCE_TYPE = L"TEXTALKER";
static const wchar_t *IMAGE_DIR_PREFIX = L"EchoTalkGUI-";
static std::wstring g_embeddedDir;   /* non-empty when the images came from resources */

static BOOL CALLBACK CollectImageName(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR param)
{
    if (!IS_INTRESOURCE(name)) {
        ((std::vector<std::wstring> *)param)->push_back(name);
    }
    return TRUE;
}

static std::vector<std::wstring> EmbeddedImageNames()
{
    std::vector<std::wstring> names;
    EnumResourceNamesW(NULL, IMAGE_RESOURCE_TYPE, CollectImageName, (LONG_PTR)&names);
    return names;
}

static void DeleteImageDir(const std::wstring &dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                DeleteFileW(JoinPath(dir, fd.cFileName).c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

/* Folders left by an earlier run that crashed before it could clean up.
 * A folder whose process is still running belongs to another open copy of
 * this program and is left alone -- deleting it would break that copy's
 * next render. */
static void DeleteStaleImageDirs(const std::wstring &temp)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(temp, std::wstring(IMAGE_DIR_PREFIX) + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            continue;
        }
        DWORD pid = (DWORD)wcstoul(fd.cFileName + wcslen(IMAGE_DIR_PREFIX), NULL, 10);
        if (pid == GetCurrentProcessId()) {
            continue;
        }
        HANDLE proc = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
        bool alive = proc != NULL && WaitForSingleObject(proc, 0) == WAIT_TIMEOUT;
        if (proc != NULL) {
            CloseHandle(proc);
        }
        if (!alive) {
            DeleteImageDir(JoinPath(temp, fd.cFileName));
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* Writes the carried images out and returns the folder, or an empty string
 * when this build carries none. */
static std::wstring ExtractEmbeddedImages()
{
    std::vector<std::wstring> names = EmbeddedImageNames();
    if (names.empty()) {
        return std::wstring();
    }
    wchar_t tempBuf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, tempBuf);
    if (n == 0 || n > MAX_PATH) {
        return std::wstring();
    }
    std::wstring temp(tempBuf, n);
    DeleteStaleImageDirs(temp);
    std::wstring dir = JoinPath(temp, Format(L"%s%lu", IMAGE_DIR_PREFIX, GetCurrentProcessId()));
    CreateDirectoryW(dir.c_str(), NULL);
    size_t written = 0;
    for (std::wstring name : names) {
        HRSRC res = FindResourceW(NULL, name.c_str(), IMAGE_RESOURCE_TYPE);
        HGLOBAL data = res != NULL ? LoadResource(NULL, res) : NULL;
        const unsigned char *bytes = data != NULL ? (const unsigned char *)LockResource(data) : NULL;
        DWORD size = res != NULL ? SizeofResource(NULL, res) : 0;
        if (bytes == NULL || size == 0) {
            continue;
        }
        /* rc keeps the quotation marks of a quoted name as part of it
         * ("\"TEXTALKER.RAM.BIN\"", measured) and upper-cases it. The pair
         * rule does not care about case, but lower case reads like the
         * original file names; the quotes would make the name invalid. */
        if (name.size() >= 2 && name.front() == L'"' && name.back() == L'"') {
            name = name.substr(1, name.size() - 2);
        }
        if (name.empty() || name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
            continue;
        }
        std::transform(name.begin(), name.end(), name.begin(), towlower);
        if (WriteWholeFile(JoinPath(dir, name), std::vector<unsigned char>(bytes, bytes + size))) {
            written++;
        }
    }
    if (written == 0) {
        DeleteImageDir(dir);
        return std::wstring();
    }
    return dir;
}

static void RemoveEmbeddedImages()
{
    if (!g_embeddedDir.empty()) {
        DeleteImageDir(g_embeddedDir);
        g_embeddedDir.clear();
    }
}

/* --------------------------------------------------------------------- */
/* Synthesis: the add-on's driver, step for step                         */
/* --------------------------------------------------------------------- */

struct RenderResult {
    std::vector<int16_t> pcm;
    int          rate = 0;
    unsigned     overruns = 0;
    unsigned     commandErrors = 0;
    bool         cancelled = false;
    std::wstring error;               /* empty on success */
};

/*
 * The text the library is given.
 *
 * By default, the add-on's _UNSAFE rule: Ctrl-D (driver command), Ctrl-E
 * (Textalker command) and Ctrl-V (phoneme mode) are each replaced with a
 * space, so a pasted document cannot silently change the voice.
 *
 * With Obey embedded commands on, those bytes cannot be typed into an edit
 * box, so caret notation stands in for them: ^E, ^D, ^V, and ^^ for one
 * caret. Anything else after a caret is left as it is.
 */
static std::wstring PrepareText(const std::wstring &text, bool obey)
{
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        wchar_t c = text[i];
        if (!obey) {
            out += (c == 0x04 || c == 0x05 || c == 0x16) ? L' ' : c;
            continue;
        }
        if (c == L'^' && i + 1 < text.size()) {
            wchar_t n = (wchar_t)towupper(text[i + 1]);
            wchar_t code = n == L'E' ? 0x05 : n == L'D' ? 0x04 : n == L'V' ? 0x16
                         : n == L'^' ? L'^' : 0;
            if (code != 0) {
                out += code;
                i++;
                continue;
            }
        }
        out += c;
    }
    return out;
}

/*
 * One utterance, from a fresh machine to the last sample.
 *
 * The add-on keeps one instance for a session. This boots a new one for
 * every render, so every render is the add-on's first utterance after a
 * voice opens: the WAV depends on the images, the settings and the text and
 * on nothing previewed before it, and a Ctrl-E command obeyed in one render
 * cannot leak into the next. Booting costs well under 10 ms.
 *
 * `generation`, when given, is checked between reads: a newer Preview or a
 * Stop abandons this one.
 *
 * `prefix` is bytes put in front of the prepared text, untouched by the
 * sanitising -- how the pitch sweep sends a Ctrl-E pitch command the
 * library's own setter would clamp to 63.
 */
static RenderResult Render(const VoicePair &voice, const Settings &settings,
                           const std::wstring &text, const volatile LONG *generation, LONG mine,
                           const std::string &prefix = std::string())
{
    RenderResult r;
    Settings s = settings;
    Sanitize(s);
    std::string utf8 = prefix + ToUtf8(PrepareText(text, s.obey));

    char err[256] = "";
    EnterCriticalSection(&g_engine);
    echotalk *et = echotalk_create(ToUtf8(voice.loader).c_str(), ToUtf8(voice.obj).c_str(),
                                   err, sizeof(err));
    if (et == NULL) {
        LeaveCriticalSection(&g_engine);
        r.error = Format(L"Textalker %s could not start: %hs", voice.banner.c_str(),
                         err[0] ? err : "unknown error");
        return r;
    }

    /* _openVoice -> _applyAll, in its order and with its conversions. */
    echotalk_set_pitch(et, s.pitch);
    echotalk_set_flat(et, s.monotone ? 1 : 0);
    echotalk_set_volume(et, s.volume);
    echotalk_set_word_delay(et, s.delay);
    echotalk_set_repeat_filter(et, s.repeat);
    echotalk_set_compressed(et, s.compressed ? 1 : 0);
    echotalk_set_speed(et, ToMult(s.rate));
    echotalk_set_clock_multiplier(et, ToMult(s.clock));
    echotalk_set_sample_rate(et, (unsigned)EffectiveRate(s));

    /* Beyond the add-on. Each is applied only when moved off the library's
     * own default, so a GUI left at defaults drives the engine exactly as
     * the add-on does -- and letter and punctuation modes in particular are
     * never sent unless chosen, because the library assumes nothing about
     * Textalker's startup modes until told. */
    if (s.frameRate != 0) {
        echotalk_set_frame_rate(et, s.frameRate);
    }
    if (s.reading != 0) {
        echotalk_set_letter_mode(et, s.reading - 1);
    }
    if (s.punct != 0) {
        echotalk_set_punctuation(et, s.punct - 1);
    }
    if (s.chunk != 80) {
        echotalk_set_chunk_size(et, (unsigned)s.chunk);
    }
    if (s.raw) {
        echotalk_set_raw(et, 1);
    }

    /* speak -> _speakOne: queue, then read in the add-on's block size until
     * a read returns 0, which is the end of speech. */
    echotalk_speak(et, utf8.c_str());
    int16_t block[SAMPLES_PER_READ];
    for (;;) {
        if (generation != NULL && *generation != mine) {
            r.cancelled = true;
            break;
        }
        size_t n = echotalk_read(et, block, SAMPLES_PER_READ);
        if (n == 0) {
            break;
        }
        r.pcm.insert(r.pcm.end(), block, block + n);
    }
    r.rate = (int)echotalk_sample_rate(et);
    r.overruns = echotalk_overruns(et);
    r.commandErrors = echotalk_command_errors(et);
    echotalk_destroy(et);
    LeaveCriticalSection(&g_engine);
    return r;
}

/* 16-bit mono PCM at the rate the library delivered, which is the
 * effective rate: at chip clock 1.5 the samples are the 1.0 samples and
 * only this header's rate differs, so a hard-coded 8000 here would export
 * every clock setting as the normal voice. */
static std::vector<unsigned char> MakeWav(const std::vector<int16_t> &pcm, int rate)
{
    const DWORD dataSize = (DWORD)(pcm.size() * 2);
    std::vector<unsigned char> w(44 + (size_t)dataSize);
    auto put32 = [&](size_t at, DWORD v) { memcpy(&w[at], &v, 4); };
    auto put16 = [&](size_t at, WORD v) { memcpy(&w[at], &v, 2); };
    memcpy(&w[0], "RIFF", 4);
    put32(4, 36 + dataSize);
    memcpy(&w[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 1);
    put32(24, (DWORD)rate);
    put32(28, (DWORD)rate * 2);
    put16(32, 2);
    put16(34, 16);
    memcpy(&w[36], "data", 4);
    put32(40, dataSize);
    if (dataSize) {
        memcpy(&w[44], &pcm[0], dataSize);
    }
    return w;
}

/* --------------------------------------------------------------------- */
/* Presets and remembered settings                                       */
/* --------------------------------------------------------------------- */

static const wchar_t *PRESET_SECTION = L"echotalk-preset";

static void WriteInt(const std::wstring &path, const wchar_t *section, const wchar_t *key, int v)
{
    WritePrivateProfileStringW(section, key, Format(L"%d", v).c_str(), path.c_str());
}

static void WriteSettings(const std::wstring &path, const wchar_t *section, const Settings &s)
{
    WriteInt(path, section, L"rate", s.rate);
    WriteInt(path, section, L"pitch", s.pitch);
    WriteInt(path, section, L"volume", s.volume);
    WriteInt(path, section, L"word_delay", s.delay);
    WriteInt(path, section, L"repeat_filter", s.repeat);
    WriteInt(path, section, L"chip_clock", s.clock);
    WriteInt(path, section, L"output_sample_rate", s.srate);
    WriteInt(path, section, L"monotone", s.monotone ? 1 : 0);
    WriteInt(path, section, L"compressed", s.compressed ? 1 : 0);
    WriteInt(path, section, L"frame_rate", s.frameRate);
    WriteInt(path, section, L"reading_mode", s.reading);
    WriteInt(path, section, L"punctuation", s.punct);
    WriteInt(path, section, L"chunk_size", s.chunk);
    WriteInt(path, section, L"raw_text", s.raw ? 1 : 0);
    WriteInt(path, section, L"obey_embedded_commands", s.obey ? 1 : 0);
}

static Settings ReadSettings(const std::wstring &path, const wchar_t *section)
{
    const Settings d;
    Settings s;
    auto get = [&](const wchar_t *key, int def) {
        return (int)GetPrivateProfileIntW(section, key, def, path.c_str());
    };
    s.rate = get(L"rate", d.rate);
    s.pitch = get(L"pitch", d.pitch);
    s.volume = get(L"volume", d.volume);
    s.delay = get(L"word_delay", d.delay);
    s.repeat = get(L"repeat_filter", d.repeat);
    s.clock = get(L"chip_clock", d.clock);
    s.srate = get(L"output_sample_rate", d.srate);
    s.monotone = get(L"monotone", 0) != 0;
    s.compressed = get(L"compressed", 0) != 0;
    s.frameRate = get(L"frame_rate", d.frameRate);
    s.reading = get(L"reading_mode", d.reading);
    s.punct = get(L"punctuation", d.punct);
    s.chunk = get(L"chunk_size", d.chunk);
    s.raw = get(L"raw_text", 0) != 0;
    s.obey = get(L"obey_embedded_commands", 0) != 0;
    Sanitize(s);
    return s;
}

static std::wstring ReadString(const std::wstring &path, const wchar_t *section, const wchar_t *key)
{
    wchar_t buf[MAX_PATH * 2];
    GetPrivateProfileStringW(section, key, L"", buf, MAX_PATH * 2, path.c_str());
    return buf;
}

/* A preset records which Textalker it was tuned on, so loading it against
 * different images can say so. */
static bool SavePreset(const std::wstring &path, const Settings &s, const VoicePair *v)
{
    DeleteFileW(path.c_str());
    WriteSettings(path, PRESET_SECTION, s);
    if (v != NULL) {
        WritePrivateProfileStringW(PRESET_SECTION, L"voice", v->stem.c_str(), path.c_str());
        WritePrivateProfileStringW(PRESET_SECTION, L"textalker", v->banner.c_str(), path.c_str());
        WritePrivateProfileStringW(PRESET_SECTION, L"loader_md5", v->loaderMd5.c_str(), path.c_str());
        WritePrivateProfileStringW(PRESET_SECTION, L"obj_md5", v->objMd5.c_str(), path.c_str());
    }
    return FileExists(path);
}

static std::wstring SettingsFilePath()
{
    std::wstring appData = AppDataDir();
    if (appData.empty()) {
        return std::wstring();
    }
    std::wstring dir = JoinPath(appData, L"EchoTalk GUI");
    CreateDirectoryW(dir.c_str(), NULL);
    return JoinPath(dir, L"settings.ini");
}

/* --------------------------------------------------------------------- */
/* Window state                                                          */
/* --------------------------------------------------------------------- */

static HWND g_text, g_romDir, g_browse, g_romStatus;
static HWND g_voice, g_rate, g_rateValue, g_pitch, g_pitchValue, g_volume, g_volumeValue;
static HWND g_delay, g_delayValue, g_repeat, g_repeatValue, g_clock, g_clockValue;
static HWND g_srate, g_srateValue, g_monotone, g_compressed;
static HWND g_frameRate, g_reading, g_punct, g_chunk, g_chunkValue, g_raw, g_obey, g_preset;
static HWND g_preview, g_stop, g_render, g_savePreset, g_loadPreset, g_copySay, g_result;

static std::vector<VoicePair> g_voices;
static std::wstring g_loadedRomDir;
static bool g_useSettingsFile = true;
static bool g_syncingPreset = false;

/* Bumped by Stop and by every new Preview. A preview render still running
 * for an older generation gives up, and its audio is thrown away. */
static volatile LONG g_generation;

/* The WAV image PlaySound is playing asynchronously. It must outlive the
 * playback, so it is freed only after PlaySound(NULL) has stopped it. */
static std::vector<unsigned char> *g_playingWav;

static void ShowError(const std::wstring &msg)
{
    MessageBoxW(g_main, msg.c_str(), L"EchoTalk GUI", MB_OK | MB_ICONERROR);
}

static void ShowWarn(const std::wstring &msg)
{
    MessageBoxW(g_main, msg.c_str(), L"EchoTalk GUI", MB_OK | MB_ICONWARNING);
}

static void SetResult(const std::wstring &msg)
{
    SetWindowTextW(g_result, msg.c_str());
}

static int GetCombo(HWND combo)
{
    return (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
}

static void SetCombo(HWND combo, int index)
{
    SendMessageW(combo, CB_SETCURSEL, (WPARAM)index, 0);
}

static int GetSlider(HWND tb)
{
    return (int)SendMessageW(tb, TBM_GETPOS, 0, 0);
}

static void SetSlider(HWND tb, int v)
{
    SendMessageW(tb, TBM_SETPOS, TRUE, v);
}

static bool IsChecked(HWND cb)
{
    return SendMessageW(cb, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void SetChecked(HWND cb, bool on)
{
    SendMessageW(cb, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

static const VoicePair *SelectedVoice()
{
    int sel = GetCombo(g_voice);
    if (sel < 0 || sel >= (int)g_voices.size()) {
        return NULL;
    }
    return &g_voices[(size_t)sel];
}

static Settings SettingsFromUI()
{
    Settings s;
    s.rate = GetSlider(g_rate);
    s.pitch = GetSlider(g_pitch);
    s.volume = GetSlider(g_volume);
    s.delay = GetSlider(g_delay);
    s.repeat = GetSlider(g_repeat);
    s.clock = GetSlider(g_clock);
    int sr = GetCombo(g_srate);
    s.srate = SAMPLE_RATES[Clamp(sr, 0, SAMPLE_RATE_COUNT - 1)];
    s.monotone = IsChecked(g_monotone);
    s.compressed = IsChecked(g_compressed);
    s.frameRate = std::max(GetCombo(g_frameRate), 0);
    s.reading = std::max(GetCombo(g_reading), 0);
    s.punct = std::max(GetCombo(g_punct), 0);
    s.chunk = GetSlider(g_chunk);
    s.raw = IsChecked(g_raw);
    s.obey = IsChecked(g_obey);
    return s;
}

static void UpdateReadouts();

static void SettingsToUI(const Settings &in)
{
    Settings s = in;
    Sanitize(s);
    SetSlider(g_rate, s.rate);
    SetSlider(g_pitch, s.pitch);
    SetSlider(g_volume, s.volume);
    SetSlider(g_delay, s.delay);
    SetSlider(g_repeat, s.repeat);
    SetSlider(g_clock, s.clock);
    for (int i = 0; i < SAMPLE_RATE_COUNT; i++) {
        if (SAMPLE_RATES[i] == s.srate) {
            SetCombo(g_srate, i);
        }
    }
    SetChecked(g_monotone, s.monotone);
    SetChecked(g_compressed, s.compressed);
    SetCombo(g_frameRate, s.frameRate);
    SetCombo(g_reading, s.reading);
    SetCombo(g_punct, s.punct);
    SetSlider(g_chunk, s.chunk);
    SetChecked(g_raw, s.raw);
    SetChecked(g_obey, s.obey);
    UpdateReadouts();
}

/* "NVDA 38%", and a note when NVDA's slider cannot land on this exact
 * Textalker value at all -- 64 pitches share 101 positions, so most can,
 * but it is worth saying when one cannot. */
static std::wstring NvdaPercent(int card, int maxVal)
{
    int pct = ToPct(card, maxVal);
    if (ToCard(pct, maxVal) != card) {
        return Format(L"NVDA about %d%%", pct);
    }
    return Format(L"NVDA %d%%", pct);
}

/* The readouts after each control say what the number means, and the preset
 * combo is re-derived from the values: it names the preset they describe,
 * or says Custom. It is never remembered, so it can never claim one voice
 * while the controls describe another. */
static void UpdateReadouts()
{
    Settings s = SettingsFromUI();
    const VoicePair *v = SelectedVoice();
    bool v13 = v != NULL && v->banner.compare(0, 2, L"1.") == 0;

    SetWindowTextW(g_rateValue, Format(L"%.2fx speed, pitch held", ToMult(s.rate)).c_str());
    SetWindowTextW(g_pitchValue, NvdaPercent(s.pitch, PITCH_MAX).c_str());
    SetWindowTextW(g_volumeValue, NvdaPercent(s.volume, VOLUME_MAX).c_str());
    SetWindowTextW(g_delayValue, (NvdaPercent(s.delay, DELAY_MAX)
                                  + (v13 ? L"; none on 1.3" : L"")).c_str());
    SetWindowTextW(g_repeatValue, (NvdaPercent(s.repeat, REPEAT_MAX)
                                   + (s.repeat == REPEAT_MAX ? L"; off" : L"")).c_str());
    SetWindowTextW(g_clockValue, Format(L"%.2fx clock, pitch moves", ToMult(s.clock)).c_str());
    int eff = EffectiveRate(s);
    SetWindowTextW(g_srateValue, (eff != s.srate
                                  ? Format(L"raised to %d Hz by the clock", eff)
                                  : Format(L"output %d Hz", eff)).c_str());
    SetWindowTextW(g_chunkValue, (s.chunk == 0 ? std::wstring(L"off (unsafe)")
                                  : Format(L"%d characters", s.chunk)).c_str());

    g_syncingPreset = true;
    SetCombo(g_preset, IsEchoDefaults(s) ? 0 : 1);
    g_syncingPreset = false;
}

/* --------------------------------------------------------------------- */
/* Images                                                                */
/* --------------------------------------------------------------------- */

static void ReloadVoices(const std::wstring *keepStem)
{
    std::wstring dir = g_romDir != NULL ? Trim(GetText(g_romDir)) : g_embeddedDir;
    std::wstring keep = keepStem != NULL ? *keepStem
                      : (SelectedVoice() != NULL ? SelectedVoice()->stem : std::wstring());
    std::wstring status;

    g_voices = FindVoices(dir, status);
    g_loadedRomDir = dir;
    SendMessageW(g_voice, CB_RESETCONTENT, 0, 0);
    int sel = DefaultVoice(g_voices);
    for (size_t i = 0; i < g_voices.size(); i++) {
        SendMessageW(g_voice, CB_ADDSTRING, 0,
                     (LPARAM)Format(L"Textalker %s (%s)", g_voices[i].banner.c_str(),
                                    g_voices[i].stem.c_str()).c_str());
        if (!keep.empty() && _wcsicmp(g_voices[i].stem.c_str(), keep.c_str()) == 0) {
            sel = (int)i;
        }
    }
    if (g_voices.empty()) {
        status += Format(L"No usable Textalker images in \"%s\". A voice is a "
                         L"<name>.ram.bin and <name>.obj.bin pair; see \"Obtaining "
                         L"the Textalker images\" in the README. They are not "
                         L"included with this program.", dir.c_str());
    }
    if (g_romStatus != NULL) {
        SetWindowTextW(g_romStatus, status.c_str());
    } else if (g_voices.empty()) {
        SetResult(L"The Textalker images built into this program could not be started.");
    }
    if (sel >= 0) {
        SetCombo(g_voice, sel);
    }
    UpdateReadouts();
}

/* The Vista folder picker: a real file dialog, which screen readers handle
 * far better than the old SHBrowseForFolder tree. */
static void OnBrowse()
{
    IFileOpenDialog *dlg = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                IID_IFileOpenDialog, (void **)&dlg))) {
        ShowError(L"The folder picker is not available.");
        return;
    }
    FILEOPENDIALOGOPTIONS opts = 0;
    IShellItem *item = NULL;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Folder holding the Textalker .ram.bin and .obj.bin files");
    if (SUCCEEDED(dlg->Show(g_main)) && SUCCEEDED(dlg->GetResult(&item))) {
        wchar_t *path = NULL;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            SetWindowTextW(g_romDir, path);
            CoTaskMemFree(path);
            ReloadVoices(NULL);
        }
        item->Release();
    }
    dlg->Release();
}

/* --------------------------------------------------------------------- */
/* Preview, stop, render                                                 */
/* --------------------------------------------------------------------- */

struct Job {
    VoicePair    voice;
    Settings     settings;
    std::wstring text;
    LONG         generation = 0;
    bool         save = false;       /* Render to WAV rather than Preview */
    std::wstring path;
};

struct JobDone {
    RenderResult                result;
    std::vector<unsigned char> *wav = NULL;
    bool                        save = false;
    bool                        written = false;
    std::wstring                path;
    LONG                        generation = 0;
    bool                        obey = false;
};

static DWORD WINAPI RenderThread(LPVOID param)
{
    Job *job = (Job *)param;
    JobDone *done = new JobDone;
    /* A save is never abandoned by Stop: the user asked for that file. */
    done->result = Render(job->voice, job->settings, job->text,
                          job->save ? NULL : &g_generation, job->generation);
    done->save = job->save;
    done->path = job->path;
    done->generation = job->generation;
    done->obey = job->settings.obey;
    if (done->result.error.empty() && !done->result.cancelled) {
        done->wav = new std::vector<unsigned char>(MakeWav(done->result.pcm, done->result.rate));
        if (job->save) {
            done->written = WriteWholeFile(job->path, *done->wav);
        }
    }
    delete job;
    if (!PostMessageW(g_main, WM_APP_RENDERED, 0, (LPARAM)done)) {
        delete done->wav;
        delete done;
    }
    return 0;
}

/* --------------------------------------------------------------------- */
/* Batch render and pitch sweep                                          */
/* --------------------------------------------------------------------- */

struct BatchItem {
    std::wstring text;
    std::string  prefix;
    std::wstring path;
};

struct BatchResult {
    size_t       written = 0;
    size_t       failed = 0;
    unsigned     overruns = 0;
    bool         cancelled = false;
    std::wstring firstError;
};

/* Set by Stop; checked between files. */
static volatile LONG g_batchCancel;
static bool g_batchRunning;

/* A name that says what is in the file and is legal on every Windows
 * filesystem: letters, digits, spaces and hyphens from the first few words. */
static std::wstring FileNameWords(const std::wstring &text)
{
    std::wstring out;
    int words = 0;
    bool inWord = false;
    for (wchar_t c : text) {
        if (iswalnum(c) || c == L'-' || c == L'\'') {
            if (!inWord) {
                if (words == 6) {
                    break;
                }
                if (!out.empty()) {
                    out += L' ';
                }
                words++;
                inWord = true;
            }
            if (c != L'\'') {
                out += c;
            }
        } else {
            inWord = false;
        }
        if (out.size() >= 40) {
            break;
        }
    }
    return out;
}

/* One item per non-blank line of the text box, numbered in order. */
static std::vector<BatchItem> BatchItems(const std::wstring &text, const std::wstring &folder)
{
    std::vector<BatchItem> items;
    size_t start = 0;
    int n = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        std::wstring line = text.substr(start, end == std::wstring::npos ? std::wstring::npos
                                                                          : end - start);
        if (!line.empty() && line[line.size() - 1] == L'\r') {
            line.erase(line.size() - 1);
        }
        if (!Trim(line).empty()) {
            BatchItem it;
            it.text = line;
            std::wstring words = FileNameWords(line);
            it.path = JoinPath(folder, Format(L"%03d%s%s.wav", ++n, words.empty() ? L"" : L" ",
                                              words.c_str()));
            items.push_back(it);
        }
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }
    return items;
}

/* The whole text at every raw Textalker pitch from 0 to 99. The library's
 * setter clamps to the documented 0-63, but Textalker does something of its
 * own above that -- "\x05 99P" is audibly not "\x05 63P" -- so the command
 * goes out as bytes ahead of the text instead. F rather than P on a
 * monotone voice, or the command would quietly restore intonation. */
static std::vector<BatchItem> SweepItems(const std::wstring &text, const std::wstring &folder,
                                         bool monotone)
{
    std::vector<BatchItem> items;
    for (int p = 0; p <= 99; p++) {
        BatchItem it;
        it.text = text;
        char cmd[16];
        _snprintf(cmd, sizeof(cmd), "\x05%d%c", p, monotone ? 'F' : 'P');
        cmd[15] = '\0';
        it.prefix = cmd;
        it.path = JoinPath(folder, Format(L"pitch %02d.wav", p));
        items.push_back(it);
    }
    return items;
}

static BatchResult RunBatch(const VoicePair &voice, const Settings &s,
                            const std::vector<BatchItem> &items, HWND progress)
{
    BatchResult b;
    for (size_t i = 0; i < items.size(); i++) {
        if (g_batchCancel) {
            b.cancelled = true;
            break;
        }
        if (progress != NULL) {
            PostMessageW(progress, WM_APP_PROGRESS, (WPARAM)(i + 1), (LPARAM)items.size());
        }
        RenderResult r = Render(voice, s, items[i].text, NULL, 0, items[i].prefix);
        b.overruns += r.overruns;
        if (!r.error.empty()) {
            b.failed++;
            if (b.firstError.empty()) {
                b.firstError = r.error;
            }
        } else if (WriteWholeFile(items[i].path, MakeWav(r.pcm, r.rate))) {
            b.written++;
        } else {
            b.failed++;
            if (b.firstError.empty()) {
                b.firstError = L"Could not write " + items[i].path;
            }
        }
    }
    return b;
}

struct BatchJob {
    VoicePair              voice;
    Settings               settings;
    std::vector<BatchItem> items;
    std::wstring           folder;
    bool                   sweep = false;
};

struct BatchDone {
    BatchResult  result;
    std::wstring folder;
    size_t       total = 0;
    bool         sweep = false;
};

static DWORD WINAPI BatchThread(LPVOID param)
{
    BatchJob *job = (BatchJob *)param;
    BatchDone *done = new BatchDone;
    done->result = RunBatch(job->voice, job->settings, job->items, g_main);
    done->folder = job->folder;
    done->total = job->items.size();
    done->sweep = job->sweep;
    delete job;
    if (!PostMessageW(g_main, WM_APP_BATCHDONE, 0, (LPARAM)done)) {
        delete done;
    }
    return 0;
}

static void OnBatchDone(BatchDone *d)
{
    const BatchResult &b = d->result;
    g_batchRunning = false;
    std::wstring what = d->sweep ? L"Pitch sweep" : L"Batch render";
    std::wstring msg = Format(L"%s: %zu of %zu file(s) written to %s.", what.c_str(), b.written,
                              d->total, d->folder.c_str());
    if (b.cancelled) {
        msg += L" Stopped before the end.";
    }
    if (b.failed) {
        msg += Format(L" %zu failed: %s", b.failed, b.firstError.c_str());
    }
    if (b.overruns) {
        msg += Format(L" The emulation overran %u time(s); those files are wrong.", b.overruns);
    }
    SetResult(msg);
    MessageBoxW(g_main, msg.c_str(), what.c_str(),
                MB_OK | ((b.failed || b.overruns) ? MB_ICONWARNING : MB_ICONINFORMATION));
    delete d;
}

/* The Vista folder picker, as for the images. Empty if cancelled. */
static std::wstring PickFolder(const wchar_t *title)
{
    std::wstring result;
    IFileOpenDialog *dlg = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                IID_IFileOpenDialog, (void **)&dlg))) {
        return result;
    }
    FILEOPENDIALOGOPTIONS opts = 0;
    IShellItem *item = NULL;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(title);
    if (SUCCEEDED(dlg->Show(g_main)) && SUCCEEDED(dlg->GetResult(&item))) {
        wchar_t *path = NULL;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            result = path;
            CoTaskMemFree(path);
        }
        item->Release();
    }
    dlg->Release();
    return result;
}

static const VoicePair *SelectedVoice();
static Settings SettingsFromUI();

static void StartBatch(bool sweep)
{
    if (g_batchRunning) {
        ShowWarn(L"A batch is already running. Press Stop to end it first.");
        return;
    }
    const VoicePair *v = SelectedVoice();
    if (v == NULL) {
        ShowWarn(L"No Textalker images are loaded. Choose the folder holding "
                 L"your .ram.bin and .obj.bin files first.");
        return;
    }
    std::wstring text = GetText(g_text);
    if (Trim(text).empty()) {
        ShowWarn(L"Please enter some text to speak.");
        return;
    }
    std::wstring folder = PickFolder(sweep ? L"Folder for the pitch sweep's 100 WAV files"
                                           : L"Folder for one WAV file per line");
    if (folder.empty()) {
        return;
    }
    Settings s = SettingsFromUI();
    BatchJob *job = new BatchJob;
    job->voice = *v;
    job->settings = s;
    job->folder = folder;
    job->sweep = sweep;
    job->items = sweep ? SweepItems(text, folder, s.monotone) : BatchItems(text, folder);
    std::wstring confirm = Format(L"This writes %zu WAV file(s) into %s, replacing any "
                                  L"with the same names. Continue?", job->items.size(),
                                  folder.c_str());
    if (MessageBoxW(g_main, confirm.c_str(), sweep ? L"Pitch sweep" : L"Batch render",
                    MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
        delete job;
        return;
    }
    InterlockedExchange(&g_batchCancel, 0);
    HANDLE th = CreateThread(NULL, 0, BatchThread, job, 0, NULL);
    if (th == NULL) {
        delete job;
        ShowError(L"Could not start the render thread.");
        return;
    }
    CloseHandle(th);
    g_batchRunning = true;
    SetResult(sweep ? L"Pitch sweep started." : L"Batch render started.");
}

static void StopPlayback()
{
    InterlockedIncrement(&g_generation);
    InterlockedExchange(&g_batchCancel, 1);
    PlaySoundW(NULL, NULL, 0);
    delete g_playingWav;
    g_playingWav = NULL;
}

static bool StartJob(bool save, const std::wstring &path)
{
    const VoicePair *v = SelectedVoice();
    if (v == NULL) {
        ShowWarn(L"No Textalker images are loaded. Choose the folder holding "
                 L"your .ram.bin and .obj.bin files first.");
        return false;
    }
    std::wstring text = GetText(g_text);
    if (Trim(text).empty()) {
        ShowWarn(L"Please enter some text to speak.");
        return false;
    }
    Job *job = new Job;
    job->voice = *v;
    job->settings = SettingsFromUI();
    job->text = text;
    job->save = save;
    job->path = path;
    if (!save) {
        StopPlayback();
    }
    job->generation = g_generation;
    HANDLE th = CreateThread(NULL, 0, RenderThread, job, 0, NULL);
    if (th == NULL) {
        delete job;
        ShowError(L"Could not start the render thread.");
        return false;
    }
    CloseHandle(th);
    return true;
}

static void OnPreview()
{
    if (StartJob(false, std::wstring())) {
        SetResult(L"Rendering the preview...");
    }
}

static void OnRender()
{
    wchar_t path[MAX_PATH] = L"echotalk.wav";
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"WAV files (*.wav)\0*.wav\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Render to WAV";
    ofn.lpstrDefExt = L"wav";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) {
        return;
    }
    if (StartJob(true, path)) {
        EnableWindow(g_render, FALSE);
        SetResult(L"Rendering to WAV...");
    }
}

static void OnRendered(JobDone *d)
{
    const RenderResult &r = d->result;
    if (d->save) {
        EnableWindow(g_render, TRUE);
    }
    if (!r.error.empty()) {
        SetResult(L"Failed: " + r.error);
        ShowError(r.error);
    } else if (r.cancelled || (!d->save && d->generation != g_generation)) {
        /* Stopped, or superseded by a newer Preview. */
    } else {
        std::wstring summary = Format(L"%zu samples, %.2f seconds at %d Hz. Overruns %u.",
                                      r.pcm.size(), r.rate ? (double)r.pcm.size() / r.rate : 0.0,
                                      r.rate, r.overruns);
        if (d->save) {
            if (d->written) {
                SetResult(L"Saved " + d->path + L": " + summary);
                MessageBoxW(g_main, (L"Saved to " + d->path + L"\r\n\r\n" + summary).c_str(),
                            L"Render to WAV", MB_OK | MB_ICONINFORMATION);
            } else {
                SetResult(L"Could not write " + d->path);
                ShowError(L"Could not write " + d->path);
            }
        } else {
            SetResult(L"Playing: " + summary);
            PlaySoundW(NULL, NULL, 0);
            delete g_playingWav;
            g_playingWav = d->wav;
            d->wav = NULL;
            PlaySoundW((LPCWSTR)&(*g_playingWav)[0], NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
        }
        /* A runaway guard tripping means the 6502 was cut off part-way
         * through a routine: that audio is wrong, and it must not pass in
         * silence -- which is how it once went unnoticed for sessions. */
        if (r.overruns) {
            ShowWarn(Format(L"The emulation overran %u time(s). The audio just "
                            L"produced is wrong. Please report the settings in use.",
                            r.overruns));
        }
        if (d->obey && r.commandErrors) {
            ShowWarn(Format(L"%u malformed or unknown Ctrl-D command(s) in the text "
                            L"were ignored.", r.commandErrors));
        }
    }
    delete d->wav;
    delete d;
}

/* --------------------------------------------------------------------- */
/* Presets and the say command line                                      */
/* --------------------------------------------------------------------- */

static void OnSavePreset()
{
    wchar_t path[MAX_PATH] = L"echotalk-preset.ini";
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"EchoTalk presets (*.ini)\0*.ini\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Save preset";
    ofn.lpstrDefExt = L"ini";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) {
        return;
    }
    if (SavePreset(path, SettingsFromUI(), SelectedVoice())) {
        SetResult(std::wstring(L"Preset saved to ") + path);
        MessageBoxW(g_main, (std::wstring(L"Preset saved to ") + path).c_str(),
                    L"Save preset", MB_OK | MB_ICONINFORMATION);
    } else {
        ShowError(std::wstring(L"Could not write ") + path);
    }
}

static void OnLoadPreset()
{
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"EchoTalk presets (*.ini)\0*.ini\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Load preset";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) {
        return;
    }
    std::wstring stem = ReadString(path, PRESET_SECTION, L"voice");
    SettingsToUI(ReadSettings(path, PRESET_SECTION));
    std::wstring note;
    if (!stem.empty()) {
        int found = -1;
        for (size_t i = 0; i < g_voices.size(); i++) {
            if (_wcsicmp(g_voices[i].stem.c_str(), stem.c_str()) == 0) {
                found = (int)i;
            }
        }
        if (found < 0) {
            note = Format(L"The preset was made on voice \"%s\", which is not in the "
                          L"current images folder. The voice was left as it is.", stem.c_str());
        } else {
            SetCombo(g_voice, found);
            const VoicePair &v = g_voices[(size_t)found];
            std::wstring lmd5 = ReadString(path, PRESET_SECTION, L"loader_md5");
            std::wstring omd5 = ReadString(path, PRESET_SECTION, L"obj_md5");
            if ((!lmd5.empty() && _wcsicmp(lmd5.c_str(), v.loaderMd5.c_str()) != 0)
                || (!omd5.empty() && _wcsicmp(omd5.c_str(), v.objMd5.c_str()) != 0)) {
                note = Format(L"The preset was made on different \"%s\" images "
                              L"(Textalker %s then, %s now). It may not sound the same.",
                              stem.c_str(), ReadString(path, PRESET_SECTION, L"textalker").c_str(),
                              v.banner.c_str());
            }
        }
    }
    UpdateReadouts();
    SetResult(std::wstring(L"Preset loaded from ") + path);
    if (!note.empty()) {
        ShowWarn(note);
    }
}

static std::wstring Quote(const std::wstring &s)
{
    return L"\"" + s + L"\"";
}

/* The same render from a shell, with the say built beside this program. */
static std::wstring SayCommandLine(const Settings &s, const VoicePair *v)
{
    std::wstring c = L"say-x64.exe";
    c += Format(L" --speed %.17g --clock %.17g --rate %d", ToMult(s.rate), ToMult(s.clock), s.srate);
    c += Format(L" --pitch %d --volume %d --word-delay %d --repeat-filter %d",
                s.pitch, s.volume, s.delay, s.repeat);
    if (s.monotone) c += L" --flat";
    if (s.compressed) c += L" --compressed";
    if (s.frameRate) c += Format(L" --frame-rate %d", s.frameRate);
    if (s.reading) c += Format(L" --letter-mode %d", s.reading - 1);
    if (s.punct) c += Format(L" --punctuation %d", s.punct - 1);
    if (s.chunk == 0) c += L" --no-chunk";
    else if (s.chunk != 80) c += Format(L" --chunk %d", s.chunk);
    if (s.raw) c += L" --raw";
    c += L" --file text.txt ";
    if (v == NULL) {
        c += L"LOADER.bin OBJ.bin";
    } else if (!g_embeddedDir.empty()) {
        /* The extracted copies vanish when this program closes, so name
         * the files rather than point at a folder that will not exist. */
        c += Quote(v->stem + L".ram.bin") + L" " + Quote(v->stem + L".obj.bin");
    } else {
        c += Quote(v->loader) + L" " + Quote(v->obj);
    }
    c += L" out.wav";
    return c;
}

static void OnCopySay()
{
    std::wstring cmd = SayCommandLine(SettingsFromUI(), SelectedVoice());
    bool ok = false;
    if (OpenClipboard(g_main)) {
        EmptyClipboard();
        size_t bytes = (cmd.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (mem != NULL) {
            memcpy(GlobalLock(mem), cmd.c_str(), bytes);
            GlobalUnlock(mem);
            ok = SetClipboardData(CF_UNICODETEXT, mem) != NULL;
            if (!ok) {
                GlobalFree(mem);
            }
        }
        CloseClipboard();
    }
    if (!ok) {
        ShowError(L"Could not put the command line on the clipboard.");
        return;
    }
    SetResult(L"Copied: " + cmd);
    MessageBoxW(g_main, (L"Copied to the clipboard:\r\n\r\n" + cmd
                         + L"\r\n\r\nSave the text as UTF-8 in text.txt. say obeys "
                           L"control bytes in the text as they are; it does not "
                           L"apply this program's caret notation or sanitising.").c_str(),
                L"Copy as say command line", MB_OK | MB_ICONINFORMATION);
}

/* --------------------------------------------------------------------- */
/* Layout                                                                */
/* --------------------------------------------------------------------- */

static HWND Make(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD exStyle,
                 int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style,
                             Px(x), Px(y), Px(w), Px(h), g_main,
                             (HMENU)(INT_PTR)id, g_inst, NULL);
    if (c != NULL) {
        SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
    return c;
}

/* Tab out of the multiline boxes instead of typing a tab character: a
 * multiline EDIT answers WM_GETDLGCODE with DLGC_WANTALLKEYS, and without
 * this IsDialogMessage hands it the Tab and focus never leaves. That trap
 * shipped once in a sibling GUI with every label correct. */
static WNDPROC g_textProc, g_statusProc;

static LRESULT TabOut(WNDPROC base, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_GETDLGCODE) {
        LRESULT code = CallWindowProcW(base, hwnd, msg, wp, lp);
        const MSG *m = (const MSG *)lp;
        if (m != NULL && m->message == WM_KEYDOWN && m->wParam == VK_TAB) {
            code &= ~(LRESULT)(DLGC_WANTALLKEYS | DLGC_WANTTAB);
        }
        return code;
    }
    return CallWindowProcW(base, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK TextProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return TabOut(g_textProc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK StatusProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return TabOut(g_statusProc, hwnd, msg, wp, lp);
}

/*
 * A trackbar's accessible value, as the system's MSAA proxy reports it, is
 * its position as a PERCENTAGE of its range -- not its position. Measured
 * with tools/verify_gui_keyboard.py: Pitch at 24 of 0-63 read "38", Volume
 * at 12 of 0-15 read "80". NVDA would announce those, which is exactly the
 * percentage this GUI exists to get past.
 *
 * Dynamic Annotation fixes it without a custom control: this server answers
 * PROPID_ACC_VALUE with the live position, so the value NVDA reads is the
 * Textalker number itself. It is asked at the moment of reading, so it can
 * never lag behind the slider.
 */
class SliderValueServer : public IAccPropServer {
public:
    explicit SliderValueServer(HWND tb) : m_tb(tb) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (out == NULL) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IAccPropServer) {
            *out = static_cast<IAccPropServer *>(this);
            AddRef();
            return S_OK;
        }
        *out = NULL;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return (ULONG)InterlockedIncrement(&m_refs);
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG n = InterlockedDecrement(&m_refs);
        if (n == 0) {
            delete this;
        }
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE GetPropValue(const BYTE *, DWORD, MSAAPROPID idProp,
                                           VARIANT *value, BOOL *hasProp) override
    {
        VariantInit(value);
        *hasProp = FALSE;
        if (idProp != PROPID_ACC_VALUE || !IsWindow(m_tb)) {
            return S_OK;
        }
        wchar_t buf[16];
        _snwprintf(buf, 15, L"%d", (int)SendMessageW(m_tb, TBM_GETPOS, 0, 0));
        buf[15] = L'\0';
        value->vt = VT_BSTR;
        value->bstrVal = SysAllocString(buf);
        *hasProp = value->bstrVal != NULL;
        return S_OK;
    }

private:
    HWND m_tb;
    LONG m_refs = 1;
};

static IAccPropServices *g_accProps;
static std::vector<HWND> g_annotated;

static void AnnotateSliderValue(HWND tb)
{
    if (g_accProps == NULL
        && FAILED(CoCreateInstance(CLSID_AccPropServices, NULL, CLSCTX_INPROC_SERVER,
                                   IID_IAccPropServices, (void **)&g_accProps))) {
        g_accProps = NULL;
        return;
    }
    SliderValueServer *server = new SliderValueServer(tb);
    MSAAPROPID props[] = { PROPID_ACC_VALUE };
    if (SUCCEEDED(g_accProps->SetHwndPropServer(tb, (DWORD)OBJID_CLIENT, CHILDID_SELF, props, 1,
                                                server, ANNO_THIS))) {
        g_annotated.push_back(tb);
    }
    server->Release();   /* the annotation holds its own reference */
}

static void ClearAnnotations()
{
    if (g_accProps == NULL) {
        return;
    }
    MSAAPROPID props[] = { PROPID_ACC_VALUE };
    for (HWND tb : g_annotated) {
        g_accProps->ClearHwndProps(tb, (DWORD)OBJID_CLIENT, CHILDID_SELF, props, 1);
    }
    g_annotated.clear();
    g_accProps->Release();
    g_accProps = NULL;
}

static const int ROW = 30, LBL = 20;

/* A right-aligned label, then the control it names -- in that z-order, so
 * a screen reader takes the label as the control's name. */
static void Label(int col, int y, const wchar_t *text, int id)
{
    Make(L"STATIC", text, SS_RIGHT, 0, col, y + 4, 156, LBL, id);
}

static HWND Slider(int col, int y, int lo, int hi, int line, int page, int value, int id,
                   int labelId, const wchar_t *label, HWND *readout, int readoutId)
{
    Label(col, y, label, labelId);
    HWND tb = Make(TRACKBAR_CLASSW, NULL, WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, 0,
                   col + 162, y, 170, 26, id);
    SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    SendMessageW(tb, TBM_SETLINESIZE, 0, line);
    SendMessageW(tb, TBM_SETPAGESIZE, 0, page);
    SendMessageW(tb, TBM_SETPOS, TRUE, value);
    /* 0-100 sliders already read correctly; the rest need their own number. */
    if (lo != 0 || hi != 100) {
        AnnotateSliderValue(tb);
    }
    *readout = Make(L"STATIC", L"", SS_NOPREFIX, 0, col + 336, y + 4, 136, LBL, readoutId);
    return tb;
}

static HWND Combo(int col, int y, const wchar_t *label, int labelId, int id,
                  const wchar_t *const *items, int count)
{
    Label(col, y, label, labelId);
    HWND c = Make(L"COMBOBOX", NULL, WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0,
                  col + 162, y, 200, 240, id);
    for (int i = 0; i < count; i++) {
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)items[i]);
    }
    SetCombo(c, 0);
    return c;
}

static HWND Check(int col, int y, const wchar_t *text, int id)
{
    return Make(L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, 0, col + 162, y + 2, 300, LBL, id);
}

static const int CLIENT_W = 980, CLIENT_H = 690;
/* Height of the ROM folder and ROM status rows, which a build carrying its
 * own images leaves out. */
static const int ROM_ROWS_H = ROW + 52 + 10;

static void CreateControls(const std::wstring &romDir)
{
    const int M = 12, W = CLIENT_W;
    const int C1 = M, C2 = 500;
    const Settings d;
    int y = M;

    Make(L"STATIC", L"&Text to speak:", 0, 0, M, y, 300, LBL, IDC_TEXTLABEL);
    y += LBL + 2;
    g_text = Make(L"EDIT", DEFAULT_TEXT,
                  WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                  WS_EX_CLIENTEDGE, M, y, W - 2 * M, 90, IDC_TEXT);
    g_textProc = (WNDPROC)(LONG_PTR)SetWindowLongPtrW(g_text, GWLP_WNDPROC, (LONG_PTR)TextProc);
    y += 90 + 8;

    /* With the images built in there is nothing to choose and nothing to
     * report, so these controls are not created at all -- a hidden
     * control can still confuse a screen reader; an absent one cannot. */
    if (g_embeddedDir.empty()) {
        Label(C1, y, L"ROM &folder:", IDC_ROMDIRLABEL);
        g_romDir = Make(L"EDIT", romDir.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
                        C1 + 162, y, W - C1 - 162 - M - 96, 24, IDC_ROMDIR);
        g_browse = Make(L"BUTTON", L"&Browse...", WS_TABSTOP | BS_PUSHBUTTON, 0,
                        W - M - 88, y - 1, 88, 26, IDC_BROWSE);
        y += ROW;

        Label(C1, y, L"R&OM status:", IDC_ROMSTATUSLABEL);
        g_romStatus = Make(L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY
                           | ES_AUTOVSCROLL, WS_EX_CLIENTEDGE, C1 + 162, y, W - C1 - 162 - M, 52,
                           IDC_ROMSTATUS);
        g_statusProc = (WNDPROC)(LONG_PTR)SetWindowLongPtrW(g_romStatus, GWLP_WNDPROC,
                                                           (LONG_PTR)StatusProc);
        y += ROM_ROWS_H - ROW;
    }

    int top = y;

    /* Column one: the add-on's settings, in the add-on's order. */
    Make(L"STATIC", L"The add-on's settings", SS_NOPREFIX, 0, C1 + 162, y, 300, LBL,
         IDC_ADDONHEADING);
    y += LBL + 4;
    Label(C1, y, L"Voi&ce:", IDC_VOICELABEL);
    g_voice = Make(L"COMBOBOX", NULL, WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0,
                   C1 + 162, y, 300, 200, IDC_VOICE);
    y += ROW + 2;
    /* Line 2: the add-on's RateSetting(minStep=2). */
    g_rate = Slider(C1, y, 0, 100, 2, 10, d.rate, IDC_RATE, IDC_RATELABEL, L"&Rate:",
                    &g_rateValue, IDC_RATEVALUE);
    y += ROW;
    g_pitch = Slider(C1, y, 0, PITCH_MAX, 1, 4, d.pitch, IDC_PITCH, IDC_PITCHLABEL, L"P&itch:",
                     &g_pitchValue, IDC_PITCHVALUE);
    y += ROW;
    g_volume = Slider(C1, y, 0, VOLUME_MAX, 1, 2, d.volume, IDC_VOLUME, IDC_VOLUMELABEL,
                      L"Vo&lume:", &g_volumeValue, IDC_VOLUMEVALUE);
    y += ROW;
    g_delay = Slider(C1, y, 0, DELAY_MAX, 1, 2, d.delay, IDC_DELAY, IDC_DELAYLABEL,
                     L"Delay between &words:", &g_delayValue, IDC_DELAYVALUE);
    y += ROW;
    g_repeat = Slider(C1, y, 0, REPEAT_MAX, 1, 10, d.repeat, IDC_REPEAT, IDC_REPEATLABEL,
                      L"R&epeat-character filter:", &g_repeatValue, IDC_REPEATVALUE);
    y += ROW;
    g_clock = Slider(C1, y, 0, 100, 2, 10, d.clock, IDC_CLOCK, IDC_CLOCKLABEL, L"Chip cloc&k:",
                     &g_clockValue, IDC_CLOCKVALUE);
    y += ROW;
    Label(C1, y, L"Output &sample rate:", IDC_SRATELABEL);
    g_srate = Make(L"COMBOBOX", NULL, WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0,
                   C1 + 162, y, 170, 240, IDC_SRATE);
    for (int i = 0; i < SAMPLE_RATE_COUNT; i++) {
        SendMessageW(g_srate, CB_ADDSTRING, 0, (LPARAM)SAMPLE_RATE_NAMES[i]);
    }
    SetCombo(g_srate, 0);
    g_srateValue = Make(L"STATIC", L"", SS_NOPREFIX, 0, C1 + 336, y + 4, 160, LBL, IDC_SRATEVALUE);
    y += ROW + 2;
    g_monotone = Check(C1, y, L"&Monotone", IDC_MONOTONE);
    y += LBL + 6;
    g_compressed = Check(C1, y, L"Compressed speec&h", IDC_COMPRESSED);
    y += LBL + 6;
    int bottom = y;

    /* Column two: what the library has and the add-on does not show. */
    y = top;
    Make(L"STATIC", L"Beyond the add-on", SS_NOPREFIX, 0, C2 + 162, y, 300, LBL,
         IDC_EXTRAHEADING);
    y += LBL + 4;
    g_frameRate = Combo(C2, y, L"Fr&ame rate:", IDC_FRAMERATELABEL, IDC_FRAMERATE,
                        FRAME_RATE_NAMES, 4);
    y += ROW + 2;
    g_reading = Combo(C2, y, L"Rea&ding mode:", IDC_READINGLABEL, IDC_READING, READING_NAMES, 3);
    y += ROW + 2;
    g_punct = Combo(C2, y, L"P&unctuation:", IDC_PUNCTLABEL, IDC_PUNCT, PUNCT_NAMES, 4);
    y += ROW + 2;
    g_chunk = Slider(C2, y, 0, CHUNK_MAX, 1, 10, d.chunk, IDC_CHUNK, IDC_CHUNKLABEL,
                     L"Chunk si&ze:", &g_chunkValue, IDC_CHUNKVALUE);
    y += ROW;
    g_raw = Check(C2, y, L"Raw te&xt", IDC_RAW);
    y += LBL + 6;
    g_obey = Check(C2, y, L"Obey embedded comma&nds (^E ^D ^V)", IDC_OBEY);
    y += LBL + 10;
    {
        static const wchar_t *const PRESETS[] = { L"Echo II defaults", L"Custom" };
        g_preset = Combo(C2, y, L"Voice &preset:", IDC_PRESETLABEL, IDC_PRESET, PRESETS, 2);
    }
    y += ROW;

    y = std::max(y, bottom) + 10;
    {
        int bx = M;
        const int bh = 28;
        g_preview = Make(L"BUTTON", L"Pre&view", BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
                         bx, y, 96, bh, IDC_PREVIEW);
        bx += 96 + 8;
        /* No letter: every letter of "Stop" is taken. Escape stops. */
        g_stop = Make(L"BUTTON", L"Stop", BS_PUSHBUTTON | WS_TABSTOP, 0, bx, y, 80, bh, IDC_STOP);
        bx += 80 + 8;
        g_render = Make(L"BUTTON", L"Render to WAV...", BS_PUSHBUTTON | WS_TABSTOP, 0,
                        bx, y, 140, bh, IDC_RENDER);
        bx += 140 + 8;
        g_savePreset = Make(L"BUTTON", L"Save preset...", BS_PUSHBUTTON | WS_TABSTOP, 0,
                            bx, y, 120, bh, IDC_SAVEPRESET);
        bx += 120 + 8;
        g_loadPreset = Make(L"BUTTON", L"Load preset...", BS_PUSHBUTTON | WS_TABSTOP, 0,
                            bx, y, 120, bh, IDC_LOADPRESET);
        bx += 120 + 8;
        g_copySay = Make(L"BUTTON", L"Cop&y as say command line", BS_PUSHBUTTON | WS_TABSTOP, 0,
                         bx, y, 200, bh, IDC_COPYSAY);
        y += bh + 8;
        bx = M;
        /* No letters left for these two; they are reached by Tab. */
        Make(L"BUTTON", L"Batch render lines to folder...", BS_PUSHBUTTON | WS_TABSTOP, 0,
             bx, y, 230, bh, IDC_BATCH);
        bx += 230 + 8;
        Make(L"BUTTON", L"Pitch sweep 0 to 99 to folder...", BS_PUSHBUTTON | WS_TABSTOP, 0,
             bx, y, 240, bh, IDC_SWEEP);
    }
    y += 28 + 10;

    Label(C1, y, L"Last result:", IDC_RESULTLABEL);
    g_result = Make(L"EDIT", L"Ready. F5 previews, Escape stops, Ctrl+S renders to WAV, "
                    L"Ctrl+O loads a preset, Ctrl+Shift+S saves one.",
                    WS_TABSTOP | ES_READONLY | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
                    C1 + 162, y, W - C1 - 162 - M, 24, IDC_RESULT);
}

/* --------------------------------------------------------------------- */
/* Window procedure                                                      */
/* --------------------------------------------------------------------- */

static std::wstring g_initialRomDir;
static Settings g_initialSettings;
static std::wstring g_initialVoice;

static void SaveRemembered()
{
    if (!g_useSettingsFile) {
        return;
    }
    std::wstring path = SettingsFilePath();
    if (path.empty()) {
        return;
    }
    WriteSettings(path, L"settings", SettingsFromUI());
    /* A build with its own images has no folder to remember, and must not
     * overwrite the one an ordinary build remembered. */
    if (g_romDir != NULL) {
        WritePrivateProfileStringW(L"settings", L"rom_folder", Trim(GetText(g_romDir)).c_str(),
                                   path.c_str());
    }
    const VoicePair *v = SelectedVoice();
    WritePrivateProfileStringW(L"settings", L"voice", v != NULL ? v->stem.c_str() : L"",
                               path.c_str());
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_main = hwnd;
        CreateControls(g_initialRomDir);
        SettingsToUI(g_initialSettings);
        ReloadVoices(&g_initialVoice);
        SetFocus(g_text);
        return 0;

    case WM_HSCROLL:
        if (lp != 0) {
            UpdateReadouts();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PREVIEW:
            OnPreview();
            return 0;
        case IDOK:
            /* Enter on anything but a button. This is not a dialog, so
             * IsDialogMessage sends IDOK rather than the default button's
             * own ID. In the ROM folder box Enter means "use this folder". */
            if (g_romDir != NULL && GetFocus() == g_romDir) {
                ReloadVoices(NULL);
            } else {
                OnPreview();
            }
            return 0;
        case IDC_STOP:
        case IDCANCEL:   /* Escape stops */
            StopPlayback();
            SetResult(L"Stopped.");
            return 0;
        case IDC_RENDER:
            OnRender();
            return 0;
        case IDC_SAVEPRESET:
            OnSavePreset();
            return 0;
        case IDC_LOADPRESET:
            OnLoadPreset();
            return 0;
        case IDC_COPYSAY:
            OnCopySay();
            return 0;
        case IDC_BATCH:
            StartBatch(false);
            return 0;
        case IDC_SWEEP:
            StartBatch(true);
            return 0;
        case IDC_BROWSE:
            OnBrowse();
            return 0;
        case IDC_ROMDIR:
            /* A typed path takes effect when focus leaves the box. */
            if (HIWORD(wp) == EN_KILLFOCUS && Trim(GetText(g_romDir)) != g_loadedRomDir) {
                ReloadVoices(NULL);
            }
            return 0;
        case IDC_PRESET:
            if (HIWORD(wp) == CBN_SELCHANGE && !g_syncingPreset) {
                if (GetCombo(g_preset) == 0) {
                    Settings s;
                    s.obey = IsChecked(g_obey);
                    SettingsToUI(s);
                }
                /* Choosing Custom does nothing: there is no custom set of
                 * numbers to apply. The combo is re-derived either way. */
                UpdateReadouts();
            }
            return 0;
        case IDC_VOICE:
        case IDC_SRATE:
        case IDC_FRAMERATE:
        case IDC_READING:
        case IDC_PUNCT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                UpdateReadouts();
            }
            return 0;
        case IDC_MONOTONE:
        case IDC_COMPRESSED:
        case IDC_RAW:
        case IDC_OBEY:
            if (HIWORD(wp) == BN_CLICKED) {
                UpdateReadouts();
            }
            return 0;
        }
        break;

    case WM_APP_RENDERED:
        OnRendered((JobDone *)lp);
        return 0;

    case WM_APP_BATCHDONE:
        OnBatchDone((BatchDone *)lp);
        return 0;

    case WM_APP_PROGRESS:
        SetResult(Format(L"Rendering file %u of %u...", (unsigned)wp, (unsigned)lp));
        return 0;

    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_romStatus || (HWND)lp == g_result) {
            break;   /* read-only edits keep their own background */
        }
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    case WM_CLOSE:
        SaveRemembered();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        StopPlayback();
        ClearAnnotations();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* --------------------------------------------------------------------- */
/* Self-test                                                             */
/* --------------------------------------------------------------------- */

/*
 * Headless modes of this executable, for tools/verify_gui.py. Both run what
 * Render to WAV runs.
 *
 *   --selftest ROMDIR STEM RATE PITCH VOLUME DELAY REPEAT CLOCK SRATE
 *              MONO COMP FRAMERATE READING PUNCT CHUNK RAW OBEY OUT.WAV TEXT
 *
 *   --selftest-preset ROMDIR PRESET.INI OUT.WAV TEXT
 *   --selftest-batch ROMDIR STEM <the 15 values> OUTDIR TEXT
 *   --selftest-sweep ROMDIR STEM <the 15 values> OUTDIR TEXT
 *   --write-preset ROMDIR STEM <the 15 values as above> PRESET.INI
 *
 * The values are in the controls' units (rate and clock 0-100, the rest
 * Textalker's numbers, SRATE in Hz). TEXT starting with @ names a UTF-8
 * file to read the text from, which is how control bytes get in.
 *
 * Exit codes: 0 written, 2 usage, 3 voice not found, 4 render failed,
 * 6 could not write, 8 written but the emulation overran.
 */
static bool ParseValues(wchar_t **argv, Settings &s)
{
    int v[15];
    for (int i = 0; i < 15; i++) {
        wchar_t *end = NULL;
        v[i] = (int)wcstol(argv[i], &end, 10);
        if (end == argv[i] || *end != L'\0') {
            return false;
        }
    }
    s.rate = v[0]; s.pitch = v[1]; s.volume = v[2]; s.delay = v[3]; s.repeat = v[4];
    s.clock = v[5]; s.srate = v[6]; s.monotone = v[7] != 0; s.compressed = v[8] != 0;
    s.frameRate = v[9]; s.reading = v[10]; s.punct = v[11]; s.chunk = v[12];
    s.raw = v[13] != 0; s.obey = v[14] != 0;
    return IsSampleRate(s.srate);
}

static const VoicePair *FindStem(const std::vector<VoicePair> &voices, const std::wstring &stem)
{
    for (const VoicePair &v : voices) {
        if (_wcsicmp(v.stem.c_str(), stem.c_str()) == 0) {
            return &v;
        }
    }
    return NULL;
}

static bool TextArg(const wchar_t *arg, std::wstring &text)
{
    if (arg[0] != L'@') {
        text = arg;
        return true;
    }
    std::vector<unsigned char> bytes;
    if (!ReadWholeFile(arg + 1, bytes)) {
        return false;
    }
    std::string s(bytes.begin(), bytes.end());
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB
        && (unsigned char)s[2] == 0xBF) {
        s.erase(0, 3);
    }
    text = FromUtf8(s);
    return true;
}

static int RenderToFile(const VoicePair &voice, const Settings &s, const wchar_t *out,
                        const wchar_t *textArg)
{
    std::wstring text;
    if (!TextArg(textArg, text)) {
        return 2;
    }
    RenderResult r = Render(voice, s, text, NULL, 0);
    if (!r.error.empty()) {
        return 4;
    }
    if (!WriteWholeFile(out, MakeWav(r.pcm, r.rate))) {
        return 6;
    }
    return r.overruns ? 8 : 0;
}

/* ROMDIR "embedded" means the images this build carries. */
static std::wstring RomDirArg(const wchar_t *arg)
{
    if (wcscmp(arg, L"embedded") == 0) {
        if (g_embeddedDir.empty()) {
            g_embeddedDir = ExtractEmbeddedImages();
        }
        return g_embeddedDir;
    }
    return arg;
}

static int RunSelfTest(int argc, wchar_t **argv)
{
    std::wstring status;
    const std::wstring mode = argv[1];
    if (mode == L"--selftest" && argc == 21) {
        Settings s;
        if (!ParseValues(argv + 4, s)) {
            return 2;
        }
        std::vector<VoicePair> voices = FindVoices(RomDirArg(argv[2]), status);
        const VoicePair *v = FindStem(voices, argv[3]);
        return v == NULL ? 3 : RenderToFile(*v, s, argv[19], argv[20]);
    }
    if (mode == L"--selftest-preset" && argc == 6) {
        std::vector<VoicePair> voices = FindVoices(RomDirArg(argv[2]), status);
        if (!FileExists(argv[3])) {
            return 2;
        }
        const VoicePair *v = FindStem(voices, ReadString(argv[3], PRESET_SECTION, L"voice"));
        return v == NULL ? 3 : RenderToFile(*v, ReadSettings(argv[3], PRESET_SECTION),
                                            argv[4], argv[5]);
    }
    if ((mode == L"--selftest-batch" || mode == L"--selftest-sweep") && argc == 21) {
        Settings s;
        std::wstring text;
        if (!ParseValues(argv + 4, s) || !DirExists(argv[19]) || !TextArg(argv[20], text)) {
            return 2;
        }
        std::vector<VoicePair> voices = FindVoices(RomDirArg(argv[2]), status);
        const VoicePair *v = FindStem(voices, argv[3]);
        if (v == NULL) {
            return 3;
        }
        std::vector<BatchItem> items = mode == L"--selftest-sweep"
                                     ? SweepItems(text, argv[19], s.monotone)
                                     : BatchItems(text, argv[19]);
        BatchResult b = RunBatch(*v, s, items, NULL);
        return b.failed ? 6 : (b.overruns ? 8 : 0);
    }
    if (mode == L"--write-preset" && argc == 20) {
        Settings s;
        if (!ParseValues(argv + 4, s)) {
            return 2;
        }
        std::vector<VoicePair> voices = FindVoices(RomDirArg(argv[2]), status);
        const VoicePair *v = FindStem(voices, argv[3]);
        if (v == NULL) {
            return 3;
        }
        return SavePreset(argv[19], s, v) ? 0 : 6;
    }
    return 2;
}

/* --------------------------------------------------------------------- */
/* Startup                                                               */
/* --------------------------------------------------------------------- */

static void MakeFont()
{
    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    if (g_font == NULL) {
        g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }
}

/* Where the images are: --rom-dir, else the folder remembered from last
 * time if it still has images, else the first default place that does. */
static std::wstring ChooseRomDir(const std::wstring &given, const std::wstring &remembered)
{
    if (!given.empty()) {
        return given;
    }
    if (!remembered.empty() && HasVoices(remembered)) {
        return remembered;
    }
    std::vector<std::wstring> dirs = DefaultRomDirs();
    for (const std::wstring &d : dirs) {
        if (HasVoices(d)) {
            wchar_t full[MAX_PATH * 2];
            DWORD n = GetFullPathNameW(d.c_str(), MAX_PATH * 2, full, NULL);
            return n > 0 && n < MAX_PATH * 2 ? std::wstring(full) : d;
        }
    }
    return JoinPath(ExeDir(), L"roms");
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show)
{
    g_inst = inst;
    InitializeCriticalSection(&g_engine);

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring givenRomDir;
    if (argv != NULL) {
        if (argc >= 2 && (wcscmp(argv[1], L"--selftest") == 0
                          || wcscmp(argv[1], L"--selftest-preset") == 0
                          || wcscmp(argv[1], L"--selftest-batch") == 0
                          || wcscmp(argv[1], L"--selftest-sweep") == 0
                          || wcscmp(argv[1], L"--write-preset") == 0)) {
            int rc = RunSelfTest(argc, argv);
            RemoveEmbeddedImages();
            LocalFree(argv);
            return rc;
        }
        for (int i = 1; i < argc; i++) {
            if (wcscmp(argv[i], L"--rom-dir") == 0 && i + 1 < argc) {
                givenRomDir = argv[++i];
            } else if (wcscmp(argv[i], L"--no-settings") == 0) {
                /* Start from the Echo II defaults and remember nothing:
                 * what the check scripts use, so a user's saved settings
                 * never decide whether a check passes. */
                g_useSettingsFile = false;
            }
        }
        LocalFree(argv);
    }

    std::wstring remembered;
    if (g_useSettingsFile) {
        std::wstring path = SettingsFilePath();
        if (!path.empty() && FileExists(path)) {
            g_initialSettings = ReadSettings(path, L"settings");
            remembered = ReadString(path, L"settings", L"rom_folder");
            g_initialVoice = ReadString(path, L"settings", L"voice");
        }
    }
    /* A build carrying its own images uses them, and nothing else. */
    g_embeddedDir = ExtractEmbeddedImages();
    g_initialRomDir = g_embeddedDir.empty() ? ChooseRomDir(givenRomDir, remembered)
                                            : g_embeddedDir;

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    MakeFont();
    {
        HDC dc = GetDC(NULL);
        g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(NULL, dc);
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = WINDOW_CLASS;
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = LoadIconW(NULL, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) {
        return 1;
    }

    const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    RECT r = { 0, 0, Px(CLIENT_W), Px(CLIENT_H - (g_embeddedDir.empty() ? 0 : ROM_ROWS_H)) };
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowExW(0, WINDOW_CLASS, WINDOW_TITLE, style, CW_USEDEFAULT,
                                CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                                NULL, NULL, inst, NULL);
    if (hwnd == NULL) {
        return 1;
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN) {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (msg.wParam == VK_F5) {
                OnPreview();
                continue;
            }
            if (ctrl && msg.wParam == 'S') {
                if (shift) {
                    OnSavePreset();
                } else if (IsWindowEnabled(g_render)) {
                    OnRender();
                }
                continue;
            }
            if (ctrl && msg.wParam == 'O') {
                OnLoadPreset();
                continue;
            }
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    RemoveEmbeddedImages();
    CoUninitialize();
    DeleteCriticalSection(&g_engine);
    return (int)msg.wParam;
}
