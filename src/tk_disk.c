/*
 * tk_disk.c -- finding Textalker on a DOS 3.3 disk, structurally.
 *
 * The disk holds a LOADER and an ENGINE, and the whole project's habit
 * is to recognise things by what they are rather than by what they are
 * called. That is not a preference here, it is forced: the two known
 * versions do not agree on the spelling (v3.1.3 ships "Textalker.ram" +
 * "Textalker.obj", v1.3 ships "Textalker.ram" + "Textalker.ram.obj"),
 * and one of Jayson's own disks carries an older 3.1.x as "TEXTALKER" +
 * "TT.OBJ". No list of names would have covered it.
 *
 * WHAT IDENTIFIES A LOADER: it carries the entry trampoline, the six
 * bytes `48 AD 8B C0 4C lo hi` -- PHA, LDA $C08B (select the language
 * card), JMP into the engine's jump table.
 *
 * WHAT IDENTIFIES ITS ENGINE: the trampolines do not merely name a
 * PAGE, they name exact addresses, and those addresses are the jump
 * table itself. Every one of them lands within the first few bytes of
 * the engine, on a JMP ($4C) or on the LDA $C08B ($AD) that enters it
 * proper. Measured on all three builds to hand:
 *
 *   v3.1.3            $D003 $D006 $D009 -> 4C 4C AD
 *   v1.3              $D400 $D403       -> 4C AD
 *   3.1.2, two builds $D003 $D006 $D009 -> 4C 4C AD
 *
 * **This deliberately does NOT require the C engine to recognise the
 * image**, and the reason is worth keeping even though it no longer
 * bites. tk_image.c USED to hold a table of addresses per release, so
 * PIZZA.DSK's Textalker 3.1.2 -- 326 bytes shorter with every table
 * moved -- was rejected outright while the 6502 emulation ran it
 * perfectly. Refusing a disk that works would have been the wrong
 * answer, so pairing was made independent of it.
 *
 * tk_image.c now finds its tables rather than listing them
 * (notes/profile_discovery.md) and reads all four known builds, so
 * nothing to hand exercises this any more. It stays because the next
 * unrecognised build will, and because the emulated engine -- which
 * needs no profile at all -- can always run one. A profile match NAMES
 * the version; `native_ok` says whether the C rewrite is available too.
 */
#include <stdio.h>
#include <string.h>

#include "tk_disk.h"

/* Read once into here rather than onto the stack: these are 64K. */
static uint8_t g_buf[0x10000];
static uint8_t g_obj[0x10000];
static tk_image g_img;

int tk_disk_obj_page(const uint8_t *loader, size_t n)
{
    size_t i;
    if (!loader) return 0;
    for (i = 0; i + 6 < n; i++) {
        const uint8_t *p = loader + i;
        if (p[0] == 0x48 && p[1] == 0xAD && p[2] == 0x8B &&
            p[3] == 0xC0 && p[4] == 0x4C)
            return (int)p[6] << 8;
    }
    return 0;
}

/* Does `obj` look like the engine `loader`'s trampolines point at?
 *
 * Every distinct JMP target must land in the engine's first few bytes,
 * on a JMP or on the language-card select, and there must be at least
 * two of them. A large binary that merely happens to sit on the same
 * disk does not survive that -- both.dsk carries a 5.6K file called
 * INTEGER, and it is rejected on the first target. */
#define JUMP_TABLE_SPAN 24

static int obj_matches_loader(const uint8_t *loader, size_t ln,
                              const uint8_t *obj, size_t on, uint16_t base)
{
    size_t i;
    int targets = 0;
    uint16_t seen[16];

    if (on < 0x1000) return 0;
    for (i = 0; i + 6 < ln; i++) {
        const uint8_t *p = loader + i;
        uint16_t tgt;
        int j, dup = 0;
        long off;
        if (p[0] != 0x48 || p[1] != 0xAD || p[2] != 0x8B ||
            p[3] != 0xC0 || p[4] != 0x4C)
            continue;
        tgt = (uint16_t)(p[5] | (p[6] << 8));
        for (j = 0; j < targets; j++)
            if (seen[j] == tgt) { dup = 1; break; }
        if (dup) continue;

        off = (long)tgt - (long)base;
        if (off < 0 || off >= JUMP_TABLE_SPAN || (size_t)off >= on) return 0;
        if (obj[off] != 0x4C && obj[off] != 0xAD) return 0;
        if (targets < (int)(sizeof seen / sizeof seen[0]))
            seen[targets] = tgt;
        targets++;
    }
    return targets >= 2;
}

/* Read a catalogued binary file into `into`, or 0 if it will not. */
static long read_binary(const dsk *d, int i, uint8_t *into, size_t cap)
{
    char why[192];
    long n;
    if (d->file[i].type != DSK_T_BINARY) return 0;
    n = dsk_read(d, &d->file[i], into, cap, NULL, why, sizeof why);
    return n > 0 ? n : 0;
}

int tk_disk_list(const dsk *d, tk_disk_match *out, int cap)
{
    int i, j, n = 0;

    for (i = 0; i < d->nfiles && n < cap; i++) {
        long ln = read_binary(d, i, g_buf, sizeof g_buf);
        int page;
        if (!ln || ln > (long)sizeof ((tk_disk_pair *)0)->loader) continue;
        page = tk_disk_obj_page(g_buf, (size_t)ln);
        if (!page) continue;

        for (j = 0; j < d->nfiles && n < cap; j++) {
            long on;
            char why[192];
            if (j == i) continue;
            on = read_binary(d, j, g_obj, sizeof g_obj);
            if (!on) continue;
            if (!obj_matches_loader(g_buf, (size_t)ln, g_obj, (size_t)on,
                                    (uint16_t)page))
                continue;
            out[n].loader_index = i;
            out[n].obj_index = j;
            out[n].obj_addr = (uint16_t)page;
            /* A profile match names the version and says the C engine
             * will work too. Without one the emulated engine still runs
             * the image, so this is a label, not a gate. The loader
             * goes in as well because it carries the version banner --
             * that is what makes the label "3.1.2" rather than the
             * family name "3.1.x". */
            out[n].native_ok = tk_image_load_mem(&g_img, g_obj, (size_t)on,
                                                 g_buf, (size_t)ln, NULL,
                                                 why, sizeof why) == 0;
            snprintf(out[n].version, sizeof out[n].version, "%s",
                     out[n].native_ok ? g_img.version : "unknown");
            n++;
            /* One engine per loader: a loader names one jump table. */
            break;
        }
    }
    return n;
}

int tk_disk_load(const dsk *d, const tk_disk_match *m, tk_disk_pair *out,
                 char *err, size_t errsz)
{
    long ln, on;
    char why[192];

    memset(out, 0, sizeof *out);
    ln = dsk_read(d, &d->file[m->loader_index], g_buf, sizeof g_buf,
                  NULL, why, sizeof why);
    if (ln <= 0 || (size_t)ln > sizeof out->loader) {
        snprintf(err, errsz, "cannot read \"%s\": %s",
                 d->file[m->loader_index].name, ln <= 0 ? why : "too large");
        return -1;
    }
    on = dsk_read(d, &d->file[m->obj_index], out->obj, sizeof out->obj,
                  NULL, why, sizeof why);
    if (on <= 0) {
        snprintf(err, errsz, "cannot read \"%s\": %s",
                 d->file[m->obj_index].name, why);
        return -1;
    }
    memcpy(out->loader, g_buf, (size_t)ln);
    out->loader_len = (size_t)ln;
    out->obj_len = (size_t)on;
    out->obj_addr = m->obj_addr;
    out->loader_index = m->loader_index;
    out->obj_index = m->obj_index;
    out->native_ok = m->native_ok;
    snprintf(out->version, sizeof out->version, "%s", m->version);
    return 0;
}

int tk_disk_find(const dsk *d, const char *want, tk_disk_pair *out,
                 char *err, size_t errsz)
{
    tk_disk_match m[TK_DISK_MAXMATCH];
    int n = tk_disk_list(d, m, TK_DISK_MAXMATCH);
    int i, pick = -1;

    if (n <= 0) {
        snprintf(err, errsz,
                 "no Textalker on this disk: none of its %d files is a "
                 "loader carrying an entry trampoline with an engine to "
                 "match", d->nfiles);
        return -1;
    }
    if (want) {
        for (i = 0; i < n; i++)
            if (!strcmp(m[i].version, want)) pick = i;
        if (pick < 0) {
            snprintf(err, errsz, "no Textalker %s on this disk", want);
            return -1;
        }
    } else {
        /* Prefer a 3.x the C engine also recognises, then anything it
         * recognises, then whatever is there -- which is the NVDA
         * driver's own default-voice rule. */
        for (i = 0; i < n && pick < 0; i++)
            if (m[i].native_ok && m[i].version[0] == '3') pick = i;
        for (i = 0; i < n && pick < 0; i++)
            if (m[i].native_ok) pick = i;
        if (pick < 0) pick = 0;
    }
    return tk_disk_load(d, &m[pick], out, err, errsz);
}
