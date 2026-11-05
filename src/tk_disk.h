/*
 * tk_disk.h -- pairing a Textalker loader with its engine, off a disk.
 *
 * dsk.c is a plain DOS 3.3 reader and knows nothing about Textalker.
 * This is the layer that does: it identifies the loader/OBJ pair
 * STRUCTURALLY, by the loader's entry trampoline and by whether the
 * engine matches a known layout, rather than by filename -- because the
 * two Textalker versions do not agree on the filenames, and a 3.1.x
 * build nobody here has seen would not be in a list anyway.
 */
#ifndef TK_DISK_H
#define TK_DISK_H

#include "dsk.h"
#include "tk.h"

#define TK_DISK_MAXMATCH 8

/* One loader/engine pair on a disk, named but not yet loaded. Small on
 * purpose: a tk_disk_pair carries 72K of image and there can be several
 * pairs on a disk. */
typedef struct {
    int      loader_index;         /* into dsk::file */
    int      obj_index;
    uint16_t obj_addr;             /* where the loader says the engine goes */
    char     version[16];          /* "3.1.3", "1.3", or "unknown" */
    int      native_ok;            /* the C rewrite recognises this build;
                                    * when 0 only the emulated engine runs
                                    * it, which is still perfectly fine */
} tk_disk_match;

typedef struct {
    uint8_t  loader[0x2000];
    size_t   loader_len;
    uint8_t  obj[0x10000];
    size_t   obj_len;
    uint16_t obj_addr;
    char     version[16];
    int      native_ok;
    int      loader_index;
    int      obj_index;
} tk_disk_pair;

/* The page a loader's entry trampoline names, or 0 if it has none.
 * `48 AD 8B C0 4C lo hi` -- PHA, LDA $C08B, JMP into the jump table. */
int tk_disk_obj_page(const uint8_t *loader, size_t n);

/* Every Textalker pair on the disk. Returns how many, up to cap.
 * both.dsk carries two: v3.1.3 and v1.3. */
int tk_disk_list(const dsk *d, tk_disk_match *out, int cap);

/* Read one of them in. `out` is ~72K -- do not put one on the stack. */
int tk_disk_load(const dsk *d, const tk_disk_match *m, tk_disk_pair *out,
                 char *err, size_t errsz);

/* List and load in one step. `want` names a version or is NULL for the
 * best available: a 3.x the C engine recognises, then anything it
 * recognises, then whatever is there. 0 on success; -1 with a message
 * in err saying what was missing, which is the part a user can act on. */
int tk_disk_find(const dsk *d, const char *want, tk_disk_pair *out,
                 char *err, size_t errsz);

#endif /* TK_DISK_H */
