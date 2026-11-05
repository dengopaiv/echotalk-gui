/*
 * test_dsk.c -- the DOS 3.3 reader, against images built here.
 *
 * WHAT THIS DOES AND DOES NOT PROVE. It builds DOS 3.3 images from the
 * format description in src/dsk.c and reads them back, so it catches
 * every coding error in the reader -- interleave, the catalogue chain,
 * multi-sector track/sector lists, the binary header, sparse holes.
 * What it cannot catch is a misreading of the FORMAT ITSELF, because
 * the builder here and the reader there would share it. Only a real
 * Apple II disk settles that, and running
 *
 *     tk_extract <a real disk> --list
 *
 * is the check that matters. Run bare, this does the synthetic tests;
 * given a loader and an OBJ it also builds a disk containing them and
 * checks that tk_disk_find() picks the pair out and that what comes
 * back is byte-identical, which is the whole path end to end.
 *
 *     test_dsk [loader.bin obj.bin]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define GETPID _getpid
#else
#include <unistd.h>
#define GETPID getpid
#endif

#include "dsk.h"
#include "tk_disk.h"

static int failures;

static void check(const char *what, int ok, const char *detail)
{
    printf("  %-4s %s%s%s\n", ok ? "ok" : "FAIL", what,
           detail && *detail ? " -- " : "", detail ? detail : "");
    if (!ok) failures++;
}

/* ------------------------------------------------------ a disk builder */

typedef struct {
    uint8_t img[DSK_IMAGE_SZ];
    int     next_track, next_sector;    /* the free-space cursor */
    int     cat_track, cat_sector;
    int     cat_entry;
} builder;

static uint8_t *sec(builder *b, int t, int s)
{
    return b->img + ((size_t)t * DSK_SECTORS + (size_t)s) * DSK_SECSZ;
}

static void alloc_sector(builder *b, int *t, int *s)
{
    *t = b->next_track;
    *s = b->next_sector;
    if (++b->next_sector == DSK_SECTORS) {
        b->next_sector = 0;
        b->next_track++;
    }
}

static void builder_init(builder *b)
{
    uint8_t *v;
    memset(b, 0, sizeof *b);
    v = sec(b, 17, 0);
    v[0x01] = 17;                       /* first catalogue sector */
    v[0x02] = 15;
    v[0x03] = 3;                        /* DOS 3 */
    v[0x27] = 0x7A;                     /* 122 TS pairs per list */
    v[0x34] = DSK_TRACKS;
    v[0x35] = DSK_SECTORS;
    v[0x36] = 0x00;
    v[0x37] = 0x01;                     /* 256 bytes per sector */
    b->cat_track = 17;
    b->cat_sector = 15;
    b->cat_entry = 0;
    b->next_track = 18;                 /* data starts past the catalogue */
    b->next_sector = 0;
    /* DOS's INIT writes the WHOLE chain, 15 down to 1, whether or not
     * there are files in it. Building only as much as the files need
     * would make this disk easier to identify than a real one -- and it
     * is exactly the chain the sector-order detection walks. */
    {
        int s2;
        for (s2 = 15; s2 >= 1; s2--) {
            uint8_t *c = sec(b, 17, s2);
            c[0x01] = 17;
            c[0x02] = (uint8_t)(s2 - 1);
        }
        sec(b, 17, 1)[0x01] = 0;
        sec(b, 17, 1)[0x02] = 0;
    }
}

static void add_file(builder *b, const char *name, uint8_t type,
                     const uint8_t *data, size_t n)
{
    int ts_t, ts_s, first_ts_t, first_ts_s;
    size_t off = 0;
    int pair = 0, total = 0, i;
    uint8_t *ts, *entry;

    alloc_sector(b, &ts_t, &ts_s);
    first_ts_t = ts_t;
    first_ts_s = ts_s;
    ts = sec(b, ts_t, ts_s);
    total = 1;

    while (off < n) {
        int dt, ds;
        size_t take = n - off > DSK_SECSZ ? (size_t)DSK_SECSZ : n - off;
        if (pair == 122) {              /* chain to another TS list */
            int nt, ns;
            alloc_sector(b, &nt, &ns);
            ts[0x01] = (uint8_t)nt;
            ts[0x02] = (uint8_t)ns;
            ts = sec(b, nt, ns);
            ts[0x05] = (uint8_t)((total - 1) & 0xFF);
            pair = 0;
            total++;
        }
        alloc_sector(b, &dt, &ds);
        memcpy(sec(b, dt, ds), data + off, take);
        ts[0x0C + pair * 2] = (uint8_t)dt;
        ts[0x0C + pair * 2 + 1] = (uint8_t)ds;
        pair++;
        total++;
        off += take;
    }

    entry = sec(b, b->cat_track, b->cat_sector) + 0x0B + b->cat_entry * 35;
    entry[0] = (uint8_t)first_ts_t;
    entry[1] = (uint8_t)first_ts_s;
    entry[2] = type;
    for (i = 0; i < DSK_NAMELEN; i++) {
        char ch = (i < (int)strlen(name)) ? name[i] : ' ';
        entry[3 + i] = (uint8_t)(0x80 | (unsigned char)ch);
    }
    entry[33] = (uint8_t)(total & 0xFF);
    entry[34] = (uint8_t)(total >> 8);

    if (++b->cat_entry == 7) {          /* on to the next catalogue sector */
        b->cat_sector--;
        b->cat_entry = 0;
    }
}

/* A binary file as DOS stores one: load address and length in front. */
static size_t bin_wrap(uint8_t *out, uint16_t addr, const uint8_t *d, size_t n)
{
    out[0] = (uint8_t)(addr & 0xFF);
    out[1] = (uint8_t)(addr >> 8);
    out[2] = (uint8_t)(n & 0xFF);
    out[3] = (uint8_t)(n >> 8);
    memcpy(out + 4, d, n);
    return n + 4;
}

static void to_prodos_order(uint8_t *img)
{
    static const int PO[16] = { 0, 14, 13, 12, 11, 10, 9, 8,
                                7, 6, 5, 4, 3, 2, 1, 15 };
    uint8_t *tmp = (uint8_t *)malloc(DSK_IMAGE_SZ);
    int t, s;
    memcpy(tmp, img, DSK_IMAGE_SZ);
    for (t = 0; t < DSK_TRACKS; t++)
        for (s = 0; s < DSK_SECTORS; s++)
            memcpy(img + ((size_t)t * DSK_SECTORS + PO[s]) * DSK_SECSZ,
                   tmp + ((size_t)t * DSK_SECTORS + s) * DSK_SECSZ,
                   DSK_SECSZ);
    free(tmp);
}

static char g_path[512];

static const char *spill(const uint8_t *img, size_t n, const char *tag)
{
    FILE *f;
    /* The scratch name carries the pid: two test runs at once quietly
     * reading each other's files has cost this project a day already. */
    snprintf(g_path, sizeof g_path, "test_dsk_%s_%d.tmp", tag, (int)GETPID());
    f = fopen(g_path, "wb");
    if (!f) return NULL;
    fwrite(img, 1, n, f);
    fclose(f);
    return g_path;
}

/* ------------------------------------------------------------- the tests */

static builder B;
static dsk D;
static uint8_t payload[40000];
static uint8_t wrapped[40096];
static uint8_t back[0x10000];
static tk_disk_pair PAIR;

static void build_synthetic(void)
{
    size_t i, n;
    builder_init(&B);
    for (i = 0; i < sizeof payload; i++)
        payload[i] = (uint8_t)(i * 7 + (i >> 8));
    /* A binary file whose length is NOT a sector multiple, and which is
     * long enough to need a second track/sector list (122 sectors is
     * 31,232 bytes). Both are places a reader goes wrong quietly. */
    n = bin_wrap(wrapped, 0xD000, payload, sizeof payload);
    add_file(&B, "BIGBIN", DSK_T_BINARY | 0x80, wrapped, n);
    /* A short one, to check the header length is believed rather than
     * the sector count: 300 bytes lives in two sectors. */
    n = bin_wrap(wrapped, 0x9300, payload, 300);
    add_file(&B, "SHORTBIN", DSK_T_BINARY, wrapped, n);
    add_file(&B, "SOME TEXT FILE", DSK_T_TEXT, payload, 500);
    /* Enough files to chain a second catalogue sector. */
    add_file(&B, "F4", DSK_T_TEXT, payload, 10);
    add_file(&B, "F5", DSK_T_TEXT, payload, 10);
    add_file(&B, "F6", DSK_T_TEXT, payload, 10);
    add_file(&B, "F7", DSK_T_TEXT, payload, 10);
    add_file(&B, "F8 PAST THE FIRST CATALOG", DSK_T_TEXT, payload, 10);
}

static void check_image(const char *tag, const uint8_t *img, size_t n,
                        int want_prodos)
{
    const char *path = spill(img, n, tag);
    char err[512], detail[256];
    long got;
    uint16_t addr = 0;
    int i, found;

    printf("%s\n", tag);
    if (!path) { check("wrote a scratch image", 0, ""); return; }
    if (dsk_open(&D, path, err, sizeof err) < 0) {
        check("opens", 0, err);
        remove(path);
        return;
    }
    check("opens", 1, "");
    snprintf(detail, sizeof detail, "reader says %s order",
             D.prodos_order ? "ProDOS" : "DOS");
    check("detects the sector order", D.prodos_order == want_prodos, detail);
    snprintf(detail, sizeof detail, "%d files", D.nfiles);
    check("finds all eight catalogue entries", D.nfiles == 8, detail);

    found = -1;
    for (i = 0; i < D.nfiles; i++)
        if (!strcmp(D.file[i].name, "BIGBIN")) found = i;
    if (found < 0) {
        check("finds BIGBIN", 0, "");
        remove(path);
        return;
    }
    check("keeps the lock bit off the type", D.file[found].type == DSK_T_BINARY,
          "");
    check("reads the lock flag", D.file[found].locked == 1, "");

    got = dsk_read(&D, &D.file[found], back, sizeof back, &addr,
                   err, sizeof err);
    snprintf(detail, sizeof detail, "%ld bytes, loads at $%04X", got, addr);
    check("a chained TS list reads back whole",
          got == (long)sizeof payload && addr == 0xD000, detail);
    check("...and byte for byte",
          got > 0 && !memcmp(back, payload, sizeof payload), "");

    found = -1;
    for (i = 0; i < D.nfiles; i++)
        if (!strcmp(D.file[i].name, "SHORTBIN")) found = i;
    got = found < 0 ? -1
        : dsk_read(&D, &D.file[found], back, sizeof back, &addr,
                   err, sizeof err);
    snprintf(detail, sizeof detail, "%ld bytes, loads at $%04X", got, addr);
    check("the binary header's length wins over the sector count",
          got == 300 && addr == 0x9300, detail);

    found = -1;
    for (i = 0; i < D.nfiles; i++)
        if (!strcmp(D.file[i].name, "F8 PAST THE FIRST CATALOG")) found = i;
    check("follows the catalogue chain past seven entries", found >= 0, "");

    found = -1;
    for (i = 0; i < D.nfiles; i++)
        if (!strcmp(D.file[i].name, "SOME TEXT FILE")) found = i;
    check("keeps spaces in a name and trims the padding", found >= 0, "");

    remove(path);
}

int main(int argc, char **argv)
{
    static uint8_t img[DSK_IMAGE_SZ + 64];

    printf("=== the DOS 3.3 reader, on images built by this test ===\n");
    build_synthetic();

    check_image("DOS order", B.img, DSK_IMAGE_SZ, 0);

    memcpy(img, B.img, DSK_IMAGE_SZ);
    to_prodos_order(img);
    check_image("ProDOS order, detected rather than declared", img,
                DSK_IMAGE_SZ, 1);

    /* A .2mg wrapper round the DOS-order image. */
    memset(img, 0, 64);
    memcpy(img, "2IMG", 4);
    img[0x0C] = 0;                          /* DOS order */
    img[0x20] = 64;                         /* data offset */
    img[0x24] = (uint8_t)(DSK_IMAGE_SZ & 0xFF);
    img[0x25] = (uint8_t)((DSK_IMAGE_SZ >> 8) & 0xFF);
    img[0x26] = (uint8_t)((DSK_IMAGE_SZ >> 16) & 0xFF);
    memcpy(img + 64, B.img, DSK_IMAGE_SZ);
    check_image(".2mg wrapper", img, DSK_IMAGE_SZ + 64, 0);

    /* ------------------------------------------------------------ */
    if (argc >= 3) {
        static uint8_t loader[0x2000], obj[0x10000], wrap[0x10010];
        size_t ln, on, n;
        FILE *f;
        char err[512], detail[256];
        const char *path;

        printf("=== the real pair, on a disk built round it ===\n");
        f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 1; }
        ln = fread(loader, 1, sizeof loader, f);
        fclose(f);
        f = fopen(argv[2], "rb");
        if (!f) { perror(argv[2]); return 1; }
        on = fread(obj, 1, sizeof obj, f);
        fclose(f);

        builder_init(&B);
        add_file(&B, "HELLO", DSK_T_APPLESOFT, (const uint8_t *)"x", 1);
        n = bin_wrap(wrap, 0x9300, loader, ln);
        add_file(&B, "TEXTALKER.RAM", DSK_T_BINARY | 0x80, wrap, n);
        n = bin_wrap(wrap, 0xD000, obj, on);
        add_file(&B, "TEXTALKER.OBJ", DSK_T_BINARY | 0x80, wrap, n);

        path = spill(B.img, DSK_IMAGE_SZ, "real");
        if (dsk_open(&D, path, err, sizeof err) < 0) {
            check("the disk opens", 0, err);
        } else if (tk_disk_find(&D, NULL, &PAIR, err, sizeof err) < 0) {
            check("tk_disk_find picks the pair out", 0, err);
        } else {
            snprintf(detail, sizeof detail, "Textalker %s at $%04X",
                     PAIR.version, PAIR.obj_addr);
            check("tk_disk_find picks the pair out", 1, detail);
            check("the loader comes back byte for byte",
                  PAIR.loader_len == ln && !memcmp(PAIR.loader, loader, ln), "");
            check("the engine comes back byte for byte",
                  PAIR.obj_len == on && !memcmp(PAIR.obj, obj, on), "");
            check("it did not match on the filename",
                  PAIR.loader_index != PAIR.obj_index, "");
        }
        remove(path);
    } else {
        printf("(give a loader and an OBJ to also test tk_disk_find)\n");
    }

    printf("\n%s\n", failures ? "FAILURES" : "all disk tests passed");
    return failures ? 1 : 0;
}
