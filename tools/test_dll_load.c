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
typedef void  (*fn_clear)(void *);

static HMODULE lib;

static void *sym(const char *name) {
    void *p = (void *)GetProcAddress(lib, name);
    if (!p) { printf("  FAIL  missing export: %s\n", name); failures++; }
    return p;
}

/* Drains the queue and returns how many samples came out. */
static size_t drain(void *et, fn_read rd) {
    int16_t block[1024];
    size_t total = 0, n;
    while ((n = rd(et, block, 1024)) > 0) total += n;
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
    fn_clear      clrerrs = (fn_clear)sym("echotalk_clear_command_errors");

    if (failures) { printf("\n%d export(s) missing\n", failures); return 1; }
    printf("  ok    all 21 exports resolved\n");

    sprintf(detail, "got %u", abi());
    check("abi version", abi() == 1, detail);

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
    size_t plain = drain(et, rd);
    sprintf(detail, "%zu samples", plain);
    check("speak and read", plain > 4000, detail);
    check("queue drained", avail(et) == 0, "");

    /* The double is the argument most likely to be got wrong across an
     * ABI boundary, so check it changes the output rather than merely
     * being accepted. */
    check("set_clock_multiplier(2.0)", setclk(et, 2.0) == 0, "");
    say(et, "Hello there.");
    size_t fast = drain(et, rd);
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
    drain(et, rd);
    sprintf(detail, "got %u", errs(et));
    check("bad Ctrl-D counted", errs(et) == 1, detail);
    clrerrs(et);
    check("error counter clears", errs(et) == 0, "");

    say(et, "\x04" "2FHello there.");
    size_t ctrl_d = drain(et, rd);
    sprintf(detail, "%zu vs %zu samples", ctrl_d, plain);
    check("Ctrl-D frame rate through the ABI", ctrl_d < plain, detail);
    check("no errors from a good command", errs(et) == 0, "");
    setrate(et, 0);   /* Ctrl-D settings persist; undo before measuring below */

    say(et, "Discard this please.");
    check("audio queued before stop", avail(et) > 0, "");
    stop(et);
    check("stop drains the queue", avail(et) == 0, "");

    /* The session-11 single-character fix, through the ABI. */
    say(et, ",");
    size_t comma = drain(et, rd);
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
