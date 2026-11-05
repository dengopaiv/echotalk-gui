/*
 * dsk.c -- a DOS 3.3 filesystem reader. See dsk.h for what and why.
 *
 * The format, since it is all magic numbers otherwise:
 *
 *   VTOC          track 17 sector 0. Byte $01/$02 is the first catalogue
 *                 sector; $27 is 122 (TS pairs per list sector); $34/$35
 *                 are 35 tracks and 16 sectors; $36/$37 are 256.
 *   catalogue     a chain of sectors. $01/$02 chain to the next, and
 *                 seven 35-byte entries start at $0B.
 *   entry         +0/+1 the track/sector of its TS list ($00 never used,
 *                 $FF deleted), +2 type and lock bit, +3..+32 a 30-char
 *                 name with the high bit set, +33/+34 the sector count.
 *   TS list       $01/$02 chain to the next, then 122 (track, sector)
 *                 pairs from $0C. A (0,0) pair is a sparse hole.
 *   binary file   the first four bytes of the data are the load address
 *                 and the length, little-endian, and are not part of it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsk.h"

/* DOS logical sector -> position in a ProDOS-ordered image. The map is
 * its own inverse, so the same table converts either way. */
static const int PO_ORDER[16] = {
    0, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 15
};

static const uint8_t *sector(const dsk *d, int track, int sec)
{
    if (track < 0 || track >= DSK_TRACKS || sec < 0 || sec >= DSK_SECTORS)
        return NULL;
    return d->data + ((size_t)track * DSK_SECTORS + (size_t)sec) * DSK_SECSZ;
}

const char *dsk_type_name(uint8_t type)
{
    switch (type & 0x7F) {
    case DSK_T_TEXT:      return "T";
    case DSK_T_INTEGER:   return "I";
    case DSK_T_APPLESOFT: return "A";
    case DSK_T_BINARY:    return "B";
    case DSK_T_S:         return "S";
    case DSK_T_RELOC:     return "R";
    case DSK_T_A:         return "A2";
    case DSK_T_B:         return "B2";
    default:              return "?";
    }
}

/* Does a track/sector list sector look like one? Byte 0 is unused, $01
 * and $02 chain to the next, and the first data pair is a real sector.
 * This is what makes the ordering detectable on a nearly empty disk. */
static int ts_looks_ok(const dsk *d, int t, int s)
{
    const uint8_t *ts = sector(d, t, s);
    if (!ts) return 0;
    if (ts[0] != 0) return 0;
    if (ts[1] != 0 && (ts[1] >= DSK_TRACKS || ts[2] >= DSK_SECTORS)) return 0;
    if (ts[0x0C] >= DSK_TRACKS || ts[0x0D] >= DSK_SECTORS) return 0;
    return 1;
}

/* How much like a DOS 3.3 disk does this ordering look? -1 for "not at
 * all"; otherwise the higher the better.
 *
 * TWO THINGS ARE DELIBERATE HERE, and both were learned from real
 * disks rather than from the format documentation.
 *
 * **The VTOC's geometry fields are evidence, not a gate.** PIZZA.DSK, a
 * perfectly ordinary DOS 3.3 disk full of Echo software, records 496
 * bytes per sector at $36/$37. Nothing reads that field, so nothing
 * ever noticed; a reader that insists on 256 rejects the whole disk.
 * The fields still score, so a standard VTOC wins a tie, but the
 * catalogue is what decides.
 *
 * **A plain VTOC check cannot choose an ordering at all.** The
 * DOS/ProDOS interleave map is its own inverse and FIXES sectors 0 and
 * 15, so track 17 sector 0 and the first catalogue sector (which DOS
 * puts at sector 15) are at the same offset in both orderings. So the
 * score follows the catalogue CHAIN, which leaves the fixed points at
 * the first hop (15 -> 14, and 14 is at position 1 under ProDOS
 * order), and checks each entry's track/sector list -- which
 * discriminates even on a disk whose catalogue is one sector long. */
static int order_score(const dsk *d)
{
    const uint8_t *v = sector(d, 17, 0);
    int t, s, guard, sectors = 0, good_ts = 0, vtoc = 0;

    if (!v) return -1;
    if (v[0x01] < 1 || v[0x01] >= DSK_TRACKS) return -1;
    if (v[0x02] >= DSK_SECTORS) return -1;

    if (v[0x27] == 0x7A) vtoc++;                /* 122 TS pairs per list */
    if (v[0x34] == DSK_TRACKS) vtoc++;
    if (v[0x35] == DSK_SECTORS) vtoc++;
    if (v[0x36] == 0x00 && v[0x37] == 0x01) vtoc++;     /* 256 bytes */
    if (v[0x03] == 3) vtoc++;                   /* DOS release */

    t = v[0x01];
    s = v[0x02];
    for (guard = 0; t && guard < 64; guard++) {
        const uint8_t *c = sector(d, t, s);
        int e;
        if (!c) break;
        if (c[0] != 0) break;                   /* unused in a catalogue */
        if (c[1] != 0 && (c[1] >= DSK_TRACKS || c[2] >= DSK_SECTORS)) break;
        sectors++;
        for (e = 0; e < 7; e++) {
            const uint8_t *p = c + 0x0B + e * 35;
            if (p[0] == 0x00 || p[0] == 0xFF) continue;
            if (p[0] < DSK_TRACKS && p[1] < DSK_SECTORS
                && ts_looks_ok(d, p[0], p[1]))
                good_ts++;
        }
        if (c[1] == t && c[2] == s) break;
        t = c[1];
        s = c[2];
    }
    /* One catalogue sector proves nothing -- sector 15 is a fixed point
     * of the interleave, so it reads the same either way. Real files
     * with real track/sector lists, or a chain that goes somewhere, are
     * what actually distinguishes the two. */
    if (!sectors || (sectors < 2 && !good_ts)) return -1;
    return vtoc + sectors + 2 * good_ts;
}

static void deinterleave(dsk *d)
{
    uint8_t tmp[DSK_IMAGE_SZ];
    int t, s;
    memcpy(tmp, d->data, sizeof tmp);
    for (t = 0; t < DSK_TRACKS; t++)
        for (s = 0; s < DSK_SECTORS; s++)
            memcpy(d->data + ((size_t)t * DSK_SECTORS + s) * DSK_SECSZ,
                   tmp + ((size_t)t * DSK_SECTORS + PO_ORDER[s]) * DSK_SECSZ,
                   DSK_SECSZ);
}

static void read_catalog(dsk *d)
{
    int track, sec, guard;
    const uint8_t *v = sector(d, 17, 0);

    d->nfiles = 0;
    track = v[0x01];
    sec = v[0x02];
    for (guard = 0; track && guard < 64; guard++) {
        const uint8_t *c = sector(d, track, sec);
        int e, nt, ns;
        if (!c) break;
        for (e = 0; e < 7 && d->nfiles < DSK_MAXFILES; e++) {
            const uint8_t *p = c + 0x0B + e * 35;
            dsk_file *f;
            int i, n;
            if (p[0] == 0x00) continue;          /* never used */
            if (p[0] == 0xFF) continue;          /* deleted */
            if (p[0] >= DSK_TRACKS || p[1] >= DSK_SECTORS) continue;
            f = &d->file[d->nfiles];
            f->ts_track = p[0];
            f->ts_sector = p[1];
            f->locked = (p[2] & 0x80) ? 1 : 0;
            f->type = (uint8_t)(p[2] & 0x7F);
            f->sectors = p[33] | (p[34] << 8);
            for (i = 0; i < DSK_NAMELEN; i++)
                f->name[i] = (char)(p[3 + i] & 0x7F);
            f->name[DSK_NAMELEN] = '\0';
            for (n = DSK_NAMELEN; n > 0 && f->name[n - 1] == ' '; n--)
                f->name[n - 1] = '\0';
            if (!f->name[0]) continue;
            d->nfiles++;
        }
        nt = c[1];
        ns = c[2];
        if (nt == track && ns == sec) break;     /* self-loop */
        track = nt;
        sec = ns;
    }
}

int dsk_open(dsk *d, const char *path, char *err, size_t errsz)
{
    FILE *f;
    uint8_t *raw;
    long size;
    size_t got, off = 0;
    int fmt = -1;

    memset(d, 0, sizeof *d);
    f = fopen(path, "rb");
    if (!f) { snprintf(err, errsz, "cannot open %s", path); return -1; }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
        fclose(f);
        snprintf(err, errsz, "cannot measure %s", path);
        return -1;
    }
    rewind(f);
    if (size > 4 * DSK_IMAGE_SZ + 65536) size = 4 * DSK_IMAGE_SZ + 65536;
    raw = (uint8_t *)malloc((size_t)size);
    if (!raw) { fclose(f); snprintf(err, errsz, "out of memory"); return -1; }
    got = fread(raw, 1, (size_t)size, f);
    fclose(f);

    /* .2mg: a 64-byte header saying where the data is and what order it
     * is in, which saves guessing. */
    if (got >= 64 && !memcmp(raw, "2IMG", 4)) {
        uint32_t hoff = (uint32_t)(raw[0x20] | (raw[0x21] << 8)
                                 | (raw[0x22] << 16) | ((uint32_t)raw[0x23] << 24));
        fmt = raw[0x0C] | (raw[0x0D] << 8);
        d->two_img = 1;
        if (fmt == 2) {
            free(raw);
            snprintf(err, errsz,
                     "%s is a nibble image inside a .2mg, which is not "
                     "supported; save it as a plain .do or .po", path);
            return -1;
        }
        if (hoff >= got || got - hoff < DSK_IMAGE_SZ) {
            free(raw);
            snprintf(err, errsz, "%s has a .2mg header but no 140K of data",
                     path);
            return -1;
        }
        off = hoff;
    } else if (got == DSK_NIB_SZ) {
        free(raw);
        snprintf(err, errsz,
                 "%s is a nibble image (.nib), which is not supported; "
                 "Textalker disks are not copy-protected, so convert it "
                 "to .do or .dsk", path);
        return -1;
    } else if (got < DSK_IMAGE_SZ) {
        free(raw);
        snprintf(err, errsz,
                 "%s is %lu bytes; a 35-track DOS 3.3 image is %d",
                 path, (unsigned long)got, DSK_IMAGE_SZ);
        return -1;
    }

    memcpy(d->data, raw + off, DSK_IMAGE_SZ);
    free(raw);

    /* Score both orderings and take the better. The .2mg header's claim
     * decides a tie, because a wrong header is more common than a wrong
     * disk but is still worth believing when nothing contradicts it. */
    {
        int dos_score, po_score;
        dos_score = order_score(d);
        deinterleave(d);
        po_score = order_score(d);
        if (po_score > dos_score
            || (po_score == dos_score && po_score >= 0 && fmt == 1)) {
            d->prodos_order = 1;
        } else {
            deinterleave(d);                    /* the map is its own inverse */
            d->prodos_order = 0;
        }
        if ((d->prodos_order ? po_score : dos_score) < 0) {
            snprintf(err, errsz,
                     "%s has no DOS 3.3 catalogue in either sector order "
                     "(is it a ProDOS, Pascal or CP/M disk?)", path);
            return -1;
        }
    }

    read_catalog(d);
    if (!d->nfiles) {
        snprintf(err, errsz, "%s has a DOS 3.3 catalogue but no files", path);
        return -1;
    }
    return 0;
}

long dsk_read(const dsk *d, const dsk_file *f, uint8_t *out, size_t cap,
              uint16_t *addr, char *err, size_t errsz)
{
    int track = f->ts_track, sec = f->ts_sector, guard;
    size_t n = 0;

    if (addr) *addr = 0;
    for (guard = 0; track && guard < DSK_TRACKS * DSK_SECTORS; guard++) {
        const uint8_t *ts = sector(d, track, sec);
        int i, nt, ns;
        if (!ts) {
            snprintf(err, errsz, "%s: track/sector list runs off the disk",
                     f->name);
            return -1;
        }
        for (i = 0; i < 122; i++) {
            int tt = ts[0x0C + i * 2], ss = ts[0x0C + i * 2 + 1];
            const uint8_t *dat;
            if (n + DSK_SECSZ > cap) {
                snprintf(err, errsz, "%s is larger than the %lu-byte buffer",
                         f->name, (unsigned long)cap);
                return -1;
            }
            if (!tt && !ss) {
                /* A sparse hole. DOS writes these for text files with
                 * gaps; keeping the offsets right costs 256 zeros. */
                memset(out + n, 0, DSK_SECSZ);
                n += DSK_SECSZ;
                continue;
            }
            dat = sector(d, tt, ss);
            if (!dat) {
                snprintf(err, errsz, "%s: data sector %d/%d is off the disk",
                         f->name, tt, ss);
                return -1;
            }
            memcpy(out + n, dat, DSK_SECSZ);
            n += DSK_SECSZ;
        }
        nt = ts[1];
        ns = ts[2];
        if (nt == track && ns == sec) break;
        track = nt;
        sec = ns;
    }

    /* Trailing sparse sectors are an artefact of the 122-pair granularity,
     * not part of the file. The type-B length below is the real bound;
     * for other types, trim them. */
    if (f->type == DSK_T_BINARY) {
        size_t len;
        if (n < 4) {
            snprintf(err, errsz, "%s is a binary file with no header", f->name);
            return -1;
        }
        if (addr) *addr = (uint16_t)(out[0] | (out[1] << 8));
        len = (size_t)(out[2] | (out[3] << 8));
        if (len > n - 4) len = n - 4;
        memmove(out, out + 4, len);
        return (long)len;
    }
    while (n >= DSK_SECSZ) {
        size_t i, blank = 1;
        for (i = n - DSK_SECSZ; i < n; i++)
            if (out[i]) { blank = 0; break; }
        if (!blank) break;
        n -= DSK_SECSZ;
    }
    return (long)n;
}
