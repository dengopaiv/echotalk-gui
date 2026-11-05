/*
 * dsk.h -- reading Apple II DOS 3.3 disk images.
 *
 * Textalker lives on DOS 3.3 floppies, and until now getting it off one
 * meant CiderPress: a Windows GUI, which is the wrong shape for a
 * screen-reader user and the wrong shape for an installer. This reads
 * the disk directly, in C, with no Python and no external tool -- so
 * `tk_extract` can produce the two .bin files, and so the library can
 * skip that step and open the .dsk itself.
 *
 * Supported: 143,360-byte 35-track images in DOS order (.do, .dsk) or
 * ProDOS order (.po), and .2mg wrappers around either. The ordering is
 * DETECTED by looking for a valid VTOC rather than trusting the
 * extension, because .dsk is used for both and gets renamed freely.
 *
 * Not supported: nibble images (.nib, .woz). They need a bit-level
 * decoder, and Textalker disks are not copy-protected, so nothing
 * requires one; dsk_open() says so by name rather than failing vaguely.
 *
 * Everything here is plain filesystem work and knows nothing about
 * Textalker. src/tk_disk.c is what pairs a loader with an OBJ.
 */
#ifndef DSK_H
#define DSK_H

#include <stddef.h>
#include <stdint.h>

#define DSK_TRACKS    35
#define DSK_SECTORS   16
#define DSK_SECSZ     256
#define DSK_IMAGE_SZ  (DSK_TRACKS * DSK_SECTORS * DSK_SECSZ)   /* 143360 */
#define DSK_NIB_SZ    (DSK_TRACKS * 6656)                      /* 232960 */

#define DSK_MAXFILES  256
#define DSK_NAMELEN   30

/* The DOS 3.3 file types, with the lock bit masked off. */
#define DSK_T_TEXT     0x00
#define DSK_T_INTEGER  0x01
#define DSK_T_APPLESOFT 0x02
#define DSK_T_BINARY   0x04
#define DSK_T_S        0x08
#define DSK_T_RELOC    0x10
#define DSK_T_A        0x20
#define DSK_T_B        0x40

typedef struct {
    char    name[DSK_NAMELEN + 1]; /* high bits cleared, trailing spaces cut */
    uint8_t type;                  /* lock bit already masked off */
    int     locked;
    int     sectors;               /* what the catalogue claims */
    uint8_t ts_track, ts_sector;
} dsk_file;

typedef struct {
    /* Always held in DOS LOGICAL order, whatever the file was in, so
     * nothing downstream has to think about interleave. */
    uint8_t  data[DSK_IMAGE_SZ];
    int      prodos_order;         /* what the file turned out to be */
    int      two_img;              /* it came wrapped in a .2mg */
    dsk_file file[DSK_MAXFILES];
    int      nfiles;
} dsk;

/* Open and validate an image. 0 on success; -1 with a message in err.
 * `d` is large (140K) -- do not put one on the stack. */
int dsk_open(dsk *d, const char *path, char *err, size_t errsz);

/* Read one catalogued file into `out`.
 *
 * For a BINARY file the four-byte DOS header is consumed: `*addr` gets
 * the load address and the return value is the payload length, which is
 * what the header says rather than what the sector count implies. (That
 * distinction matters here: Textalker's OBJ is 12,282 bytes inside 48
 * sectors of 256.) For any other type the whole sector chain comes
 * back and `*addr` is 0. `addr` may be NULL.
 *
 * Returns the number of bytes written, or -1 with a message in err. */
long dsk_read(const dsk *d, const dsk_file *f, uint8_t *out, size_t cap,
              uint16_t *addr, char *err, size_t errsz);

/* "B", "T", "A", "I"... the letter DOS 3.3's CATALOG prints. */
const char *dsk_type_name(uint8_t type);

#endif /* DSK_H */
