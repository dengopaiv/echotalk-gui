/*
 * test_dll_load.c -- loads echotalk.dll the way a host does, by name,
 * resolving every export through GetProcAddress.
 *
 * This exists because the 32-bit DLL cannot be checked from Python on
 * the machine this was developed on: the only interpreter here is
 * 64-bit, and a bitness mismatch is refused at load time. Building this
 * for both architectures verifies both DLLs through the same dynamic
 * path ctypes uses -- export table, undecorated names, calling
 * convention, and runtime dependencies all have to be right or it fails
 * here rather than in a screen reader.
 *
 * It deliberately does NOT link against the import library, because
 * that would prove something weaker.
 *
 * usage: test_dll_load <echotalk.dll> <loader.bin> <obj.bin>
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <windows.h>

static int failures = 0;

static int check(const char *label, int ok, const char *detail) {
    printf("  %s  %s%s%s\n", ok ? "ok  " : "FAIL", label,
           detail && *detail ? "  " : "", detail ? detail : "");
    if (!ok) failures++;
    return ok;
}

/* Every exported function, resolved by name. */
typedef void *(*fn_create)(const char *, const char *, char *, size_t);
typedef void  (*fn_destroy)(void *);
typedef const char *(*fn_version)(const void *);
typedef unsigned (*fn_abi)(void);
typedef int   (*fn_set_int)(void *, int);
typedef int   (*fn_set_uint)(void *, unsigned);
typedef int   (*fn_set_double)(void *, double);
typedef int   (*fn_speak)(void *, const char *);
typedef size_t (*fn_read)(void *, int16_t *, size_t);
typedef size_t (*fn_avail)(const void *);
typedef void  (*fn_stop)(void *);
typedef unsigned (*fn_get_uint)(const void *);
typedef size_t (*fn_get_size)(const void *);
typedef size_t (*fn_synth)(void *, size_t);
typedef int   (*fn_next_index)(void *, int *);
typedef int   (*fn_get_int)(const void *);
typedef double (*fn_get_dbl)(const void *);
typedef void  (*fn_clear)(void *);

static HMODULE lib;

static void *sym(const char *name) {
    void *p = (void *)GetProcAddress(lib, name);
    if (!p) { printf("  FAIL  missing export: %s\n", name); failures++; }
    return p;
}

/* Drains the queue and returns how many samples came out. Index events
 * are collected after every read INCLUDING the final one that returns 0,
 * since a mark at the very end of the text only becomes ready then. */
static int g_marks[32], g_nmarks;
static size_t g_mark_pos[32];

static size_t drain(void *et, fn_read rd, fn_next_index nextidx) {
    int16_t block[1024];
    size_t total = 0, n;
    g_nmarks = 0;
    for (;;) {
        n = rd(et, block, 1024);
        total += n;
        if (nextidx) {
            int idx;
            while (nextidx(et, &idx) && g_nmarks < 32) {
                g_mark_pos[g_nmarks] = total;
                g_marks[g_nmarks++] = idx;
            }
        }
        if (n == 0) break;
    }
    return total;
}

int main(int argc, char **argv) {
    char detail[256];

    if (argc != 4) {
        fprintf(stderr, "usage: %s <echotalk.dll> <loader.bin> <obj.bin>\n", argv[0]);
        return 2;
    }

    printf("loading %s (%d-bit build)\n", argv[1], (int)(sizeof(void *) * 8));
    lib = LoadLibraryA(argv[1]);
    if (!lib) {
        printf("  FAIL  LoadLibrary failed, error %lu\n", (unsigned long)GetLastError());
        return 1;
    }
    printf("  ok    loaded\n");

    fn_abi        abi     = (fn_abi)sym("echotalk_abi_version");
    fn_create     create  = (fn_create)sym("echotalk_create");
    fn_destroy    destroy = (fn_destroy)sym("echotalk_destroy");
    fn_version    version = (fn_version)sym("echotalk_version");
    fn_speak      say     = (fn_speak)sym("echotalk_speak");
    fn_read       rd      = (fn_read)sym("echotalk_read");
    fn_avail      avail   = (fn_avail)sym("echotalk_available");
    fn_stop       stop    = (fn_stop)sym("echotalk_stop");
    fn_set_double setclk  = (fn_set_double)sym("echotalk_set_clock_multiplier");
    fn_set_int    setrate = (fn_set_int)sym("echotalk_set_frame_rate");
    fn_set_int    setpit  = (fn_set_int)sym("echotalk_set_pitch");
    fn_set_int    setvol  = (fn_set_int)sym("echotalk_set_volume");
    fn_set_int    setdel  = (fn_set_int)sym("echotalk_set_word_delay");
    fn_set_int    setrep  = (fn_set_int)sym("echotalk_set_repeat_filter");
    fn_set_int    setcmp  = (fn_set_int)sym("echotalk_set_compressed");
    fn_set_int    setraw  = (fn_set_int)sym("echotalk_set_raw");
    fn_set_uint   sethz   = (fn_set_uint)sym("echotalk_set_sample_rate");
    fn_set_uint   setchk  = (fn_set_uint)sym("echotalk_set_chunk_size");
    fn_get_uint   getchk  = (fn_get_uint)sym("echotalk_chunk_size");
    fn_get_uint   errs    = (fn_get_uint)sym("echotalk_command_errors");
    fn_get_uint   overrun = (fn_get_uint)sym("echotalk_overruns");
    fn_set_int    setibrk = (fn_set_int)sym("echotalk_set_index_break");
    fn_get_int    getibrk = (fn_get_int)sym("echotalk_index_break");
    fn_clear      clrerrs = (fn_clear)sym("echotalk_clear_command_errors");
    fn_get_size   pending = (fn_get_size)sym("echotalk_pending");
    fn_synth      synth   = (fn_synth)sym("echotalk_synthesize");
    fn_next_index nextidx = (fn_next_index)sym("echotalk_next_index");
    fn_set_int    setflat = (fn_set_int)sym("echotalk_set_flat");
    fn_set_int    setlet  = (fn_set_int)sym("echotalk_set_letter_mode");
    fn_set_int    setpunc = (fn_set_int)sym("echotalk_set_punctuation");
    fn_get_int    getpit  = (fn_get_int)sym("echotalk_pitch");
    fn_get_int    getflat = (fn_get_int)sym("echotalk_flat");
    fn_get_int    getvol  = (fn_get_int)sym("echotalk_volume");
    fn_get_int    getdel  = (fn_get_int)sym("echotalk_word_delay");
    fn_get_int    getrep  = (fn_get_int)sym("echotalk_repeat_filter");
    fn_get_int    getcmp  = (fn_get_int)sym("echotalk_compressed");
    fn_get_int    getlet  = (fn_get_int)sym("echotalk_letter_mode");
    fn_get_int    getpunc = (fn_get_int)sym("echotalk_punctuation");
    fn_get_int    getfr   = (fn_get_int)sym("echotalk_frame_rate");
    fn_get_int    getraw  = (fn_get_int)sym("echotalk_raw");
    fn_get_dbl    getclk  = (fn_get_dbl)sym("echotalk_clock_multiplier");
    fn_set_double setspd  = (fn_set_double)sym("echotalk_set_speed");
    fn_get_dbl    getspd  = (fn_get_dbl)sym("echotalk_speed");
    fn_get_uint   gethz   = (fn_get_uint)sym("echotalk_sample_rate");

    if (failures) { printf("\n%d export(s) missing\n", failures); return 1; }
    printf("  ok    all 44 exports resolved\n");

    sprintf(detail, "got %u", abi());
    check("abi version", abi() == 6, detail);

    char err[256] = {0};
    void *et = create(argv[2], argv[3], err, sizeof err);
    if (!check("create", et != NULL, err)) return 1;
    if (!et) return 1;

    sprintf(detail, "\"%s\"", version(et));
    check("version banner", version(et)[0] != 0, detail);

    char err2[256] = {0};
    void *second = create(argv[2], argv[3], err2, sizeof err2);
    check("second instance refused", second == NULL, err2);
    if (second) destroy(second);

    if (say(et, "Hello there.") != 0) { check("speak", 0, "returned nonzero"); }
    size_t plain = drain(et, rd, nextidx);
    sprintf(detail, "%zu samples", plain);
    check("speak and read", plain > 4000, detail);
    check("queue drained", avail(et) == 0, "");

    /* The double is the argument most likely to be got wrong across an
     * ABI boundary, so check it changes the output rather than merely
     * being accepted. */
    check("set_clock_multiplier(2.0)", setclk(et, 2.0) == 0, "");
    say(et, "Hello there.");
    size_t fast = drain(et, rd, nextidx);
    double ratio = fast ? (double)plain / (double)fast : 0.0;
    sprintf(detail, "%zu vs %zu samples, ratio %.2f", plain, fast, ratio);
    check("clock multiplier crosses the ABI", ratio > 1.8 && ratio < 2.2, detail);
    check("set_clock_multiplier(1.0)", setclk(et, 1.0) == 0, "");

    check("range checks reject", setrate(et, 9) == -1 && setpit(et, 99) == -1 &&
                                 setclk(et, 99.0) == -1 && sethz(et, 1) == -1 &&
                                 setraw(et, 7) == -1 && setchk(et, 999) == -1, "");
    check("setters accept", setrate(et, 0) == 0 && setpit(et, 24) == 0 &&
                            setvol(et, 12) == 0 && setdel(et, 0) == 0 &&
                            setrep(et, 99) == 0 && setcmp(et, 0) == 0 &&
                            setraw(et, 0) == 0 && sethz(et, 0) == 0, "");
    check("chunk size round-trips", setchk(et, 0) == 0 && getchk(et) == 0 &&
                                    setchk(et, 80) == 0 && getchk(et) == 80, "");

    /* Ctrl-D across the boundary, and the error counter that is a
     * host's only way to notice a typo. */
    clrerrs(et);
    say(et, "\x04" "9ZOne.");
    drain(et, rd, nextidx);
    sprintf(detail, "got %u", errs(et));
    check("bad Ctrl-D counted", errs(et) == 1, detail);
    clrerrs(et);
    check("error counter clears", errs(et) == 0, "");

    say(et, "\x04" "2FHello there.");
    size_t ctrl_d = drain(et, rd, nextidx);
    sprintf(detail, "%zu vs %zu samples", ctrl_d, plain);
    check("Ctrl-D frame rate through the ABI", ctrl_d < plain, detail);
    check("no errors from a good command", errs(et) == 0, "");
    setrate(et, 0);   /* Ctrl-D settings persist; undo before measuring below */

    say(et, "Discard this please.");
    check("text queued before stop", pending(et) > 0, "");
    stop(et);
    check("stop clears the queue", avail(et) == 0 && pending(et) == 0, "");

    /* --- streaming --- speak() must not synthesise. */
    const char *longtext =
        "This is a fairly long passage, long enough to be split into several "
        "chunks, so that streaming has something to actually stream. It keeps "
        "going for a while yet.";
    say(et, longtext);
    sprintf(detail, "%zu samples queued, %zu bytes pending",
            avail(et), pending(et));
    check("speak() returns without synthesising",
          avail(et) == 0 && pending(et) > 0, detail);
    size_t streamed = drain(et, rd, nextidx);
    sprintf(detail, "%zu samples", streamed);
    check("streamed text is fully spoken",
          streamed > 4000 && pending(et) == 0, detail);

    say(et, longtext);
    size_t prefilled = synth(et, 8000);
    sprintf(detail, "%zu samples ready before any read", prefilled);
    check("synthesize() pre-fills the queue", prefilled >= 8000, detail);
    drain(et, rd, nextidx);

    /* --- index events ---
     *
     * Shaped like what NVDA actually sends, from a real log: a Say All
     * arrives as ONE sequence with a mark between every line, and the
     * marks must not split it into separate utterances. */
#define SAYALL "" "75IThis is  " "" "76Ia test  " "" "77Iof multiple  "                "" "78Iline breaks  " "" "79Ibetween words of  "                "" "80Ia sentence.  " "" "81I"
    say(et, SAYALL);
    size_t idx_total = drain(et, rd, nextidx);
    check("every index mark fired, in order",
          g_nmarks == 7 && g_marks[0] == 75 && g_marks[6] == 81, "");
    sprintf(detail, "last mark at %zu, audio %zu",
            g_nmarks ? g_mark_pos[g_nmarks - 1] : (size_t)0, idx_total);
    check("last mark lands at the end of the audio",
          g_nmarks == 7 && g_mark_pos[6] == idx_total, detail);
    sprintf(detail, "%zu samples", idx_total);
    check("marks do not split the sentence into separate utterances",
          idx_total < 45100, detail);

    setibrk(et, 1);
    say(et, SAYALL);
    size_t split = drain(et, rd, nextidx);
    sprintf(detail, "%zu split vs %zu continuous", split, idx_total);
    check("index_break restores the splitting behaviour",
          split > idx_total && getibrk(et) == 1, detail);
    setibrk(et, 0);

    /* The settings that make the emulation work hardest: a long
     * inter-word delay and slow speech both leave Textalker waiting on
     * the chip, which used to push a character past the 6502 step budget
     * and truncate the utterance in silence. */
    check("no overruns at default settings", overrun(et) == 0, "");
    setdel(et, 15); setspd(et, 0.25);
    say(et, "The quick brown fox jumps over the lazy dog while the cat "
            "watches from a wall.");
    size_t hard = drain(et, rd, nextidx);
    sprintf(detail, "%u overrun(s), %zu samples", overrun(et), hard);
    check("no overruns at the slowest speed and longest word delay",
          overrun(et) == 0 && hard > 300000, detail);
    setdel(et, 0); setspd(et, 1.0);

    /* --- continuous speed: monotonic, and it crosses the ABI as a
     * double like the clock multiplier does. */
    setspd(et, 0.5); say(et, "Aaaaah."); size_t slow = drain(et, rd, nextidx);
    setspd(et, 1.0); say(et, "Aaaaah."); size_t norm = drain(et, rd, nextidx);
    setspd(et, 2.0); say(et, "Aaaaah."); size_t fastr = drain(et, rd, nextidx);
    sprintf(detail, "%zu / %zu / %zu samples at 0.5 / 1.0 / 2.0", slow, norm, fastr);
    check("speed is monotonic across the ABI", slow > norm && norm > fastr, detail);
    check("set_speed(9) rejected", setspd(et, 9.0) == -1, "");
    check("speed reads back", setspd(et, 1.5) == 0 && getspd(et) > 1.49 &&
                              getspd(et) < 1.51, "");
    setspd(et, 1.0);

    /* --- Ctrl-E commands in the text must update the settings ---
     *
     * Without this the library's idea of the voice drifts from
     * Textalker's, and a host cannot carry a voice to a fresh instance
     * because it cannot read the current values back. */
    setrate(et, 0); setpit(et, 24); setflat(et, 0); setvol(et, 12);
    setdel(et, 0); setrep(et, 99); setcmp(et, 0);
    setlet(et, 0); setpunc(et, 1);
    say(et, "" "0R" "" "48PHello.");
    drain(et, rd, nextidx);
    sprintf(detail, "pitch %d", getpit(et));
    check("Ctrl-E nP is mirrored", getpit(et) == 48, detail);

    say(et, "" "40FHello.");
    drain(et, rd, nextidx);
    sprintf(detail, "pitch %d flat %d", getpit(et), getflat(et));
    check("Ctrl-E nF sets pitch AND monotone",
          getpit(et) == 40 && getflat(et) == 1, detail);

    say(et, "" "12pHello.");         /* lowercase */
    drain(et, rd, nextidx);
    sprintf(detail, "pitch %d flat %d", getpit(et), getflat(et));
    check("command letters are case-insensitive",
          getpit(et) == 12 && getflat(et) == 0, detail);

    say(et, "" "3V" "" "10D" "" "2R" "" "CHi.");
    drain(et, rd, nextidx);
    sprintf(detail, "V%d D%d R%d C%d",
            getvol(et), getdel(et), getrep(et), getcmp(et));
    check("Ctrl-E V/D/R/C are mirrored",
          getvol(et) == 3 && getdel(et) == 10 && getrep(et) == 2 &&
          getcmp(et) == 1, detail);

    say(et, "" "99PHi.");
    drain(et, rd, nextidx);
    sprintf(detail, "pitch %d", getpit(et));
    check("out-of-range Ctrl-E value is clamped", getpit(et) == 63, detail);

    check("every getter value is accepted by its setter",
          setpit(et, getpit(et)) == 0 && setflat(et, getflat(et)) == 0 &&
          setvol(et, getvol(et)) == 0 && setdel(et, getdel(et)) == 0 &&
          setrep(et, getrep(et)) == 0 && setcmp(et, getcmp(et)) == 0 &&
          setlet(et, getlet(et)) == 0 && setpunc(et, getpunc(et)) == 0 &&
          setrate(et, getfr(et)) == 0 && setraw(et, getraw(et)) == 0 &&
          setclk(et, getclk(et)) == 0 && sethz(et, gethz(et)) == 0, "");

    /* A one-character utterance must not clobber the caller's modes. */
    setpunc(et, 2);
    say(et, "x");
    drain(et, rd, nextidx);
    sprintf(detail, "punctuation %d", getpunc(et));
    check("single character preserves punctuation mode", getpunc(et) == 2, detail);
    setpunc(et, 1); setlet(et, 0); setcmp(et, 0); setrep(et, 99);
    setdel(et, 0); setvol(et, 12); setpit(et, 24); setflat(et, 0);

    /* The session-11 single-character fix, through the ABI. */
    say(et, ",");
    size_t comma = drain(et, rd, nextidx);
    sprintf(detail, "%zu samples; over ~7000 means \"return\" is back", comma);
    check("single character is short", comma < 4000, detail);

    check("empty string is harmless", say(et, "") == 0, "");

    destroy(et);
    printf("  ok    destroy\n");

    char err3[256] = {0};
    void *et2 = create(argv[2], argv[3], err3, sizeof err3);
    check("create again after destroy", et2 != NULL, err3);
    if (et2) destroy(et2);

    FreeLibrary(lib);

    printf("\n%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
