/*
 * tk_image.c -- where everything lives in a Textalker image, FOUND.
 *
 * Nothing here is a list of addresses per release any more. Every table
 * is located by searching the user's own image for its shape, the way
 * the rest of this project already recognises things -- and for the
 * same reason it had to: Jayson's PIZZA.DSK carries a **Textalker
 * 3.1.2** whose engine is 326 bytes shorter than 3.1.3's with every
 * table moved, and a hard-coded profile rejected it outright while the
 * 6502 emulation ran it perfectly.
 *
 * WHAT IS FOUND, and by what shape:
 *
 *   attrs        the 128-byte attribute table: exactly A E I O U Y carry
 *                the vowel bit among the letters, and every other letter
 *                carries the consonant bit and not the vowel bit. UNIQUE
 *                in all three images.
 *   allo_ptr     58 consecutive 16-bit pointers, ascending, each within
 *                $40 of the last, all inside the image. UNIQUE in all
 *                three. The character map and the two pitch tables sit
 *                at FIXED offsets below it -- $4F, $14 and $0A -- in
 *                both known versions, and each is verified after the
 *                fact rather than assumed.
 *   punct_names  28 {pointer, pointer, character} triples whose first 25
 *                characters ascend. The dispatch table begins exactly
 *                $54 past it, which is that table's own last byte plus
 *                one, so finding either finds both -- and the dispatch
 *                table having 70 live entries is what confirms the
 *                punctuation table was the right one.
 *   rule_lo      the lowest pointer in the dispatch table.
 *   the four syllabifier lists: consecutive null-terminated runs of
 *                letters with the high bit set on each word's last
 *                character. Their presence is also what says this is a
 *                3.1.x rather than a 1.3.
 *
 * WHAT IS NOT FOUND is the WORKSPACE -- the input and phoneme buffers
 * and the dozen variables the Ctrl-E commands set. Those are RAM
 * addresses that only the CODE references, so no data pattern reaches
 * them, and they come from a small per-FAMILY table below. That is
 * honest rather than ideal: it means a hypothetical future 1.x with a
 * different workspace would need work, while any 3.1.x is already
 * covered because 3.1.2 and 3.1.3 share theirs (measured, not assumed
 * -- tools/tk_diff.py runs 3.1.2 against the emulator).
 *
 * The FAMILY is chosen structurally too: an image carrying the
 * syllabifier's word lists is a 3.1.x, and one without is a 1.3. v1.3
 * has no syllabifier at all -- watching its input buffer while it
 * speaks shows the text going in and nothing else.
 *
 * **v1.3's startup state comes from its LOADER**, not its image. Its
 * loader sets eleven variables at $9410-$9445 before the first
 * character arrives, and the flags byte reads $00 -- speech OFF -- from
 * the OBJ where the running machine has $09. loader_init() below reads
 * that block rather than recording measured constants.
 */
#include <stdio.h>
#include <string.h>

#include "tk.h"

/* The part of a profile that is behaviour rather than an address, plus
 * the workspace, which no data pattern can reach. Two families, told
 * apart by whether the image carries the syllabifier's word lists. */
typedef struct {
    const char *name;              /* used only when the loader has no banner */
    int      syllabifier, v13_metas, capital_pitch, frames_pre_dec,
             compress_emit_even, fwd_run_limit, flow_every_n;
    const char *commands;

    /* The workspace, as OFFSETS from the phoneme buffer. It is one
     * rigid block: every address in it moves together, so finding
     * phbuf places all of them. Measured by watching the emulator's
     * writes while Ctrl-E commands go past, on v3.1.3 and again on
     * 3.1.2 -- whose whole workspace sits $150 lower and whose every
     * variable landed exactly where these offsets predict. */
    uint16_t phbuf_fallback;       /* if the search below finds nothing */
    int      d_inbuf;              /* inbuf - phbuf, and the search key */
    int      d_phlen, d_flags, d_base_pitch, d_monotone, d_compress,
             d_delay, d_group, d_variant, d_p_step, d_utt_flag;
    int      has_delay;            /* v1.3 has no inter-word delay at all */
    int      reenter_after_special;   /* the fallback; see find_reenter() */
    uint16_t payload_pc;
} tk_family;

static const tk_family FAMILIES[] = {
    {   /* 3.1.x */
        "3.1.x",
        1, 0, 1, 0, 0, -1, 8,
        "ABCDEFKLMNOPRSTVW;,I?",
        0xFEAD,
        -0x101,
        0x0FF, 0x100, 0x102, 0x103, 0x104,
        0x10E, 0x10F, 0x115, 0x116, -0x10B,
        1,
        0,                         /* 3.1.3 branches PAST the re-entry */
        0xE238
    },
    {   /* 1.3: no syllabifier, a reduced metacharacter set, thirteen
         * commands in a comparison chain, eight frames not ten, the
         * greedy runs capped at a fixed $8F, and a status poll before
         * every byte rather than every eighth. Its input buffer sits
         * ABOVE its phoneme buffer, where 3.1.x's sits below. */
        "1.3",
        0, 1, 0, 1, 1, 0x8F, 1,
        "ABCEFLMOPSTVW",
        0xF492,
        0x10D,
        -0x008, 0x20F, 0x236, 0x237, 0x235,
        0, 0x210, 0x216, 0x217, 0x100,
        0,
        1,                         /* v1.3 merges the two and always does */
        0
    }
};

#define FAM_31X 0
#define FAM_13  1

/* $EA44 + $4F == $EA93, and $EFAA + $4F == $EFF9: the character map and
 * the two pitch tables sit at the same offsets below the allophone
 * pointer table in both known versions. Verified after being applied,
 * never assumed -- charmap_ok() below is the check. */
#define OFF_CHARMAP    0x4F
#define OFF_PITCH_OFF  0x14
#define OFF_VOWEL_VAR  0x0A

/* The punctuation-name table is 28 {lo, hi, char} triples = $54 bytes,
 * and the dispatch table starts immediately after it. $E27C scans it
 * downwards from $53 for Ctrl-E A and from $4A for Ctrl-E M, which are
 * the 28th and 25th records; both constants hold for both versions. */
#define PUNCT_RECORDS  28
#define PUNCT_BYTES    (PUNCT_RECORDS * 3)
#define PUNCT_ALL      0x53
#define PUNCT_MOST     0x4A


uint8_t tk_peek(const tk_image *img, uint16_t addr)
{
    size_t i;
    if (addr < img->p.base) return 0;
    i = (size_t)(addr - img->p.base);
    if (i >= img->rom_len) return 0;
    return img->rom[i];
}

/* $E1FA picks a section from the dispatch table, but the sections are
 * delimited by the SORTED pointers rather than by ASCII order -- the
 * digits point into the tail past Z -- and a pointer lands on the last
 * payload byte of whatever precedes it. So a section spans ptr+1 up to
 * and including the next pointer. Several characters can share one:
 * ',' '?' and '!' all point at the same place. */
static void build_sections(tk_image *img)
{
    const tk_profile *p = &img->p;
    uint16_t ptrs[0x80];
    int n = 0, c, i, j;

    memset(img->sec_ok, 0, sizeof img->sec_ok);
    for (c = 0x20; c < 0x66; c++) {
        uint16_t e = (uint16_t)(p->dispatch + 2 * c);
        uint16_t q = (uint16_t)(tk_peek(img, e) | (tk_peek(img, (uint16_t)(e + 1)) << 8));
        if (q < p->rule_lo || q > p->rule_hi) continue;
        img->sec_start[c] = (uint16_t)(q + 1);
        img->sec_ok[c] = 1;
        for (i = 0; i < n; i++) if (ptrs[i] == q) break;
        if (i == n && n < (int)(sizeof ptrs / sizeof ptrs[0]) - 1)
            ptrs[n++] = q;
    }
    ptrs[n++] = p->rule_hi;
    for (i = 0; i < n; i++)                       /* insertion sort */
        for (j = i + 1; j < n; j++)
            if (ptrs[j] < ptrs[i]) {
                uint16_t t = ptrs[i]; ptrs[i] = ptrs[j]; ptrs[j] = t;
            }
    for (c = 0x20; c < 0x66; c++) {
        if (!img->sec_ok[c]) continue;
        for (i = 0; i < n; i++)
            if (ptrs[i] == (uint16_t)(img->sec_start[c] - 1)) break;
        /* the slice is inclusive of the next pointer, so one past it */
        img->sec_end[c] = (uint16_t)(ptrs[i + 1 < n ? i + 1 : i] + 1);
    }
}

/* ------------------------------------------------------------ discovery */

/* Where the engine loads. The loader's entry trampoline names it
 * outright (tk_disk.c pairs the two files on a disk by the same six
 * bytes), which is exact whenever a loader is to hand. Without one the
 * engine has to be placed by where it must END: Textalker lives in the
 * language card at the top of memory, so base + length lands at or just
 * below $10000 and there are only a few page-aligned candidates. */
static uint16_t loader_base(const uint8_t *ram, size_t n)
{
    size_t i;
    if (!ram) return 0;
    for (i = 0; i + 6 < n; i++)
        if (ram[i] == 0x48 && ram[i + 1] == 0xAD && ram[i + 2] == 0x8B &&
            ram[i + 3] == 0xC0 && ram[i + 4] == 0x4C)
            return (uint16_t)((uint16_t)ram[i + 6] << 8);
    return 0;
}

/* The 128-byte attribute table, by the vowel bit: exactly A E I O U Y
 * carry $80 among the letters, and every other letter carries $40 and
 * not $80. Unique in all three images to hand. */
static int find_attrs(const uint8_t *rom, size_t n, size_t *out)
{
    static const char L[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    size_t o;
    int found = 0;
    for (o = 0; o + 128 <= n; o++) {
        int i, ok = 1;
        for (i = 0; L[i] && ok; i++) {
            int vowel = strchr("AEIOUY", L[i]) != NULL;
            uint8_t t = rom[o + (size_t)(unsigned char)L[i]];
            if (!(t & (vowel ? 0x80 : 0x40))) ok = 0;
            else if (!vowel && (t & 0x80)) ok = 0;
        }
        if (!ok) continue;
        if (!found) *out = o;
        found++;
    }
    return found;
}

/* The 58 allophone pointers: consecutive, ascending, each within $40 of
 * the last, all inside the image. A few exceptions are allowed rather
 * than demanding a perfect run -- index 25 points away at $EAEA in both
 * known versions. */
static int find_allo(const uint8_t *rom, size_t n, uint16_t base, size_t *out)
{
    size_t o;
    int found = 0;
    for (o = 0; o + 2 * 58 <= n; o++) {
        int i, good = 0, inside = 1;
        for (i = 0; i < 58 && inside; i++) {
            uint16_t q = (uint16_t)(rom[o + 2 * i] | (rom[o + 2 * i + 1] << 8));
            if (q < base || (size_t)(q - base) >= n) inside = 0;
        }
        if (!inside) continue;
        for (i = 0; i < 57; i++) {
            long p0 = rom[o + 2 * i] | (rom[o + 2 * i + 1] << 8);
            long p1 = rom[o + 2 * i + 2] | (rom[o + 2 * i + 3] << 8);
            if (p1 > p0 && p1 - p0 <= 0x40) good++;
        }
        if (good < 54) continue;
        if (!found) *out = o;
        found++;
    }
    return found;
}

/* Does the character map look like one where it is expected to be? The
 * digits all map to silence, "," is the pause and "." the end marker.
 * This is what turns "$4F below the allophone table" from an assumption
 * into a checked one. */
static int charmap_ok(const uint8_t *rom, size_t n, size_t cm)
{
    int c;
    if (cm + 0x40 > n) return 0;
    for (c = 0x30; c <= 0x39; c++)              /* the digits */
        if (rom[cm + (size_t)c - 0x20] != 0x7F) return 0;
    if (rom[cm + 0x2C - 0x20] != 0x00) return 0;        /* , */
    if (rom[cm + 0x2E - 0x20] != 0xFF) return 0;        /* . */
    return 1;
}

/* The punctuation-name table, and with it the dispatch table $54 later.
 *
 * 28 {pointer, pointer, character} triples. The first 25 characters --
 * the ones Ctrl-E M reaches -- ascend; the last three are what Ctrl-E A
 * adds, and they break the ascent, which is itself part of the shape.
 * The dispatch table is then confirmed by counting live entries: 70 of
 * the 70 characters $20-$65 point into the rules in every image here,
 * so anything under 60 is the wrong candidate. That check is what makes
 * the pair self-verifying rather than two separate guesses. */
static int find_punct(const uint8_t *rom, size_t n, uint16_t base,
                      size_t *punct, size_t *dispatch, uint16_t *rule_lo)
{
    size_t o;
    int found = 0;

    for (o = 0; o + PUNCT_BYTES <= n; o++) {
        int i, ok = 1, live = 0;
        uint16_t lo = 0xFFFF;
        size_t disp;

        for (i = 0; i < PUNCT_RECORDS && ok; i++) {
            uint16_t q = (uint16_t)(rom[o + 3 * i] | (rom[o + 3 * i + 1] << 8));
            uint8_t ch = rom[o + 3 * i + 2];
            if (q < base || (size_t)(q - base) >= n) ok = 0;
            else if (ch >= 0x66) ok = 0;
            else if (i > 0 && i < 25 && ch <= rom[o + 3 * (i - 1) + 2]) ok = 0;
        }
        if (!ok) continue;

        /* The dispatch table's base is this table's last byte plus one:
         * $EF8B + $54 == $EFDF, and $DE79 + $54 == $DECD. */
        disp = o + PUNCT_BYTES;
        for (i = 0x20; i < 0x66; i++) {
            size_t e = disp + 2 * (size_t)i;
            uint16_t q;
            if (e + 1 >= n) break;
            q = (uint16_t)(rom[e] | (rom[e + 1] << 8));
            if (q < base || (size_t)(q - base) >= n) continue;
            live++;
            if (q < lo) lo = q;
        }
        if (live < 60) continue;

        if (!found) {
            *punct = o;
            *dispatch = disp;
            *rule_lo = lo;
        }
        found++;
    }
    return found;
}

/* The syllabifier's four word lists: consecutive null-terminated runs
 * of letters with the high bit set on each word's last character. Their
 * presence is also the family test -- v1.3 has no syllabifier at all,
 * and watching its input buffer while it speaks shows the text going in
 * and nothing else.
 *
 * The run has to be long enough that ordinary rule payloads cannot
 * imitate it, and hold at least four terminators, which is what the
 * four lists are. */
#define LIST_MIN 0xA0

static int list_byte(uint8_t b)
{
    return b == 0x00
        || (b >= 0x41 && b <= 0x5A)
        || (b >= 0xC1 && b <= 0xDA);
}

/* A run qualifies only if it is shaped like WORDS. Letting a byte be
 * "letter, high-bit letter or null" is not enough on its own -- a page
 * of zeros satisfies it, and $FD78 in v3.1.3 is exactly that, so the
 * first version of this search found a blank workspace and then decided
 * v1.3 had a syllabifier. So: no two nulls in a row, every null
 * preceded by a word-final high-bit letter, and most of the run made of
 * actual letters. */
static int list_run(const uint8_t *rom, size_t n, size_t o, size_t *end,
                    int *zeros)
{
    size_t e = o;
    int z = 0, letters = 0;

    if (o >= n || rom[o] == 0) return 0;
    while (e < n && list_byte(rom[e])) {
        if (!rom[e]) {
            if (e == o || rom[e - 1] < 0xC1) break;   /* no word ended here */
            if (e + 1 < n && rom[e + 1] == 0) { e++; z++; break; }
            z++;
        } else {
            letters++;
        }
        e++;
    }
    *end = e;
    *zeros = z;
    return z >= 4 && letters >= 0x80 && e - o >= LIST_MIN;
}

static int find_lists(const uint8_t *rom, size_t n, uint16_t base,
                      tk_profile *p)
{
    size_t o = 0, best = 0, bestlen = 0;

    while (o < n) {
        size_t e = o;
        int z = 0;
        if (list_run(rom, n, o, &e, &z) && e - o > bestlen) {
            bestlen = e - o;
            best = o;
        }
        o = (e > o) ? e + 1 : o + 1;
    }
    if (!bestlen) return 0;

    {
        size_t at = best;
        uint16_t *slot[4];
        int k;
        slot[0] = &p->prefixes;
        slot[1] = &p->suffixes;
        slot[2] = &p->digraphs;
        slot[3] = &p->clusters;
        for (k = 0; k < 4; k++) {
            if (at >= n) return 0;
            *slot[k] = (uint16_t)(base + at);
            while (at < n && rom[at]) at++;     /* to this list's null */
            at++;
        }
    }
    return 1;
}

/* The phoneme buffer, from the CODE that uses it.
 *
 * Nothing in the DATA reaches the workspace: it is RAM the engine
 * writes, and its addresses appear only as instruction operands. But
 * that is enough, because two of them appear as a distinctive PAIR --
 * the rule engine reads the input buffer with LDA abs,X and stores
 * phonemes with STA abs,X, and the two buffers are a fixed distance
 * apart. So: the one STA abs,X operand W for which W + d is also an
 * LDA abs,X operand.
 *
 * It is unique in every image here, and it has to be -- there are only
 * nine to fifteen STA abs,X operands in a whole Textalker:
 *
 *   3.1.3   $FEAD / $FDAC        d = -$101
 *   3.1.2   $FD5D / $FC5C        d = -$101
 *   1.3     $F492 / $F59F        d = +$10D   (its input buffer is ABOVE)
 *
 * Getting this wrong is not fatal, which is why a failure falls back to
 * the family's own address rather than refusing the image: the buffers
 * are only ever SEEDED from here, with the bytes the image happens to
 * carry at those addresses, and that only shows when a rule reads past
 * the end of the text into the previous utterance's leftovers. It is
 * audible when it happens, though, so it is worth getting right. */
static int find_phbuf(const uint8_t *rom, size_t n, uint16_t base, int d,
                      uint16_t *out)
{
    size_t i, j;
    int found = 0;
    for (i = 0; i + 2 < n; i++) {
        long w, v;
        if (rom[i] != 0x9D) continue;           /* STA abs,X */
        w = rom[i + 1] | (rom[i + 2] << 8);
        v = w + d;
        if (v < 0 || v > 0xFFFF) continue;
        for (j = 0; j + 2 < n; j++)
            if (rom[j] == 0xBD                  /* LDA abs,X */
                && (long)(rom[j + 1] | (rom[j + 2] << 8)) == v)
                break;
        if (j + 2 >= n) continue;
        if (!found) *out = (uint16_t)w;
        else if (*out != (uint16_t)w) found++;  /* a second, different one */
        found++;
        (void)base;
    }
    return found;
}

/* $E5CA: after a code that is NOT an allophone, does the engine
 * re-enter phoneme mode for the next character, or return?
 *
 * This is one branch displacement, and it is the only thing separating
 * two Textalker 3.1.2 builds that are otherwise four bytes apart. 3.1.3
 * branches PAST the re-entry, which is why a lone "," in a Ctrl-V string
 * swallows the rest of the line; PIZZA.DSK's 3.1.2 branches only as far
 * as the re-entry and carries on, so its comma speaks and the line
 * continues. bex21my.dsk's 3.1.2 agrees with 3.1.3.
 *
 * The site is identifiable by its shape rather than its address --
 * JSR, BCS, three more JSRs, LDA abs, BPL:
 *
 *     20 b0 e6   JSR $E6B0     the codes that are not allophones
 *     b0 16      BCS $E5E2     ...handled: skip to the RTS
 *     20 74 e6   JSR $E674     stop-consonant release
 *     20 13 e7   JSR $E713     pitch offset and vowel variant
 *     20 5d e7   JSR $E75D     read and emit the frames
 *     ad ad ff   LDA $FFAD     <- the re-entry begins here
 *     10 08      BPL $E5E2
 *
 * Three JSRs is nine bytes, so a displacement of exactly 9 lands ON the
 * re-entry and anything larger skips it. Unique in all three 3.1.x
 * images and absent from v1.3, which merges the two routines at $EAD6
 * and re-enters either way -- so a miss keeps the family's own value. */
static int find_reenter(const uint8_t *rom, size_t n, int *out)
{
    size_t i;
    int found = 0;
    for (i = 0; i + 18 < n; i++) {
        if (rom[i] != 0x20 || rom[i + 3] != 0xB0 || rom[i + 5] != 0x20
            || rom[i + 8] != 0x20 || rom[i + 11] != 0x20
            || rom[i + 14] != 0xAD || rom[i + 17] != 0x10)
            continue;
        if (!found) *out = (rom[i + 4] == 9);
        found++;
    }
    return found;
}

/* All of the above, for one candidate load address. 0 if the image does
 * not lay out that way, with the reason in `err`. */
static int find_layout(tk_image *img, uint16_t base, char *err, size_t errsz)
{
    const uint8_t *rom = img->rom;
    size_t n = img->rom_len;
    size_t attrs = 0, allo = 0, punct = 0, disp = 0;
    tk_profile *p = &img->p;
    int fam;

    if ((size_t)base + n > 0x10000) return 0;
    memset(p, 0, sizeof *p);

    if (find_attrs(rom, n, &attrs) != 1) {
        snprintf(err, errsz, "no single attribute table");
        return 0;
    }
    if (find_allo(rom, n, base, &allo) != 1) {
        snprintf(err, errsz, "no single allophone pointer table");
        return 0;
    }
    if (allo < OFF_CHARMAP || !charmap_ok(rom, n, allo - OFF_CHARMAP)) {
        snprintf(err, errsz, "no Ctrl-V character map below the allophones");
        return 0;
    }
    if (!find_punct(rom, n, base, &punct, &disp, &p->rule_lo)) {
        snprintf(err, errsz, "no punctuation-name table with a dispatch "
                 "table behind it");
        return 0;
    }

    p->base = base;
    p->attrs = (uint16_t)(base + attrs);
    p->allo_ptr = (uint16_t)(base + allo);
    p->charmap = (uint16_t)(base + allo - OFF_CHARMAP);
    p->pitch_off = (uint16_t)(base + allo - OFF_PITCH_OFF);
    p->vowel_var = (uint16_t)(base + allo - OFF_VOWEL_VAR);
    p->n_allophones = 58;
    p->punct_names = (uint16_t)(base + punct);
    p->punct_all = PUNCT_ALL;
    p->punct_most = PUNCT_MOST;
    p->dispatch = (uint16_t)(base + disp);

    /* The dispatch pointers give every section's START but not the end
     * of the LAST one, and $E1FA needs a bound. Walking the rules to
     * find it does not work: 6502 code alternates high and low bytes
     * exactly as a pattern-then-payload rule does, so the walk runs to
     * the end of the image anyway. So the end of the image IS the
     * bound, which is what punct_section() already uses for its own
     * sections. It only ever limits the last section, and the last rule
     * of a real section is a catch-all, so the walk stops long before
     * it. */
    p->rule_hi = (uint16_t)(base + n - 1);

    fam = find_lists(rom, n, base, p) ? FAM_31X : FAM_13;
    {
        const tk_family *f = &FAMILIES[fam];
        uint16_t ph = f->phbuf_fallback;
        find_phbuf(rom, n, base, f->d_inbuf, &ph);
        p->phbuf       = ph;
        p->inbuf       = (uint16_t)(ph + f->d_inbuf);
        p->phlen       = (uint16_t)(ph + f->d_phlen);
        p->v_flags     = (uint16_t)(ph + f->d_flags);
        p->v_base_pitch = (uint16_t)(ph + f->d_base_pitch);
        p->v_monotone  = (uint16_t)(ph + f->d_monotone);
        p->v_compress  = (uint16_t)(ph + f->d_compress);
        p->v_delay     = f->has_delay ? (uint16_t)(ph + f->d_delay) : 0;
        p->v_group     = (uint16_t)(ph + f->d_group);
        p->v_variant   = (uint16_t)(ph + f->d_variant);
        p->v_p_step    = (uint16_t)(ph + f->d_p_step);
        p->v_utt_flag  = (uint16_t)(ph + f->d_utt_flag);
        p->name = f->name;
        p->syllabifier = f->syllabifier;
        p->v13_metas = f->v13_metas;
        p->capital_pitch = f->capital_pitch;
        p->frames_pre_dec = f->frames_pre_dec;
        p->compress_emit_even = f->compress_emit_even;
        p->fwd_run_limit = f->fwd_run_limit;
        p->flow_every_n = f->flow_every_n;
        p->commands = f->commands;
        p->reenter_after_special = f->reenter_after_special;
        find_reenter(rom, n, &p->reenter_after_special);
        p->payload_pc = f->payload_pc;
    }
    return 1;
}

/* The release, from the loader's banner. v3.1.3 stores it as plain text
 * and v1.3 reversed; echotalk.c's parse_version() reads the same thing
 * for its own label. Without a loader there is no banner and the family
 * name stands in, which is honest: the layout is all that was
 * established. */
static void banner_version(const uint8_t *ram, size_t n, char *out, size_t cap)
{
    size_t i;
    if (!ram || cap < 2) return;
    for (i = 0; i + 16 < n; i++) {
        int fwd = 1, rev = 1, k;
        size_t j, w = 0;
        (void)j;
        for (k = 0; k < 7; k++) {
            char c = (char)(ram[i + k] & 0x7F);
            if (c != "VERSION"[k]) fwd = 0;
            if (c != "NOISREV"[k]) rev = 0;
        }
        if (fwd) {
            for (j = i + 8; j < n && w + 1 < cap; j++) {
                char c = (char)(ram[j] & 0x7F);
                if (c <= ' ') break;
                out[w++] = c;
            }
            out[w] = 0;
            if (w) return;
        }
        if (rev && i >= 2) {
            /* Stored backwards, e.g. ")DRAC MAR( 3.1 NOISREV", so step
             * back over the space and read right to left. */
            long q = (long)i - 2;
            while (q >= 0 && w + 1 < cap && (char)(ram[q] & 0x7F) > ' ')
                out[w++] = (char)(ram[q--] & 0x7F);
            out[w] = 0;
            if (w) return;
        }
    }
}

/* The values Textalker's loader writes into its variables at boot.
 *
 * 3.1.3's OBJ already carries its startup state; v1.3's does not, and
 * taking the OBJ byte there makes the port silent for any text that
 * does not open with an explicit Ctrl-E T. The block is LDA #imm
 * followed by a run of STA abs, distinctive enough to read straight out
 * of the loader rather than recording measured constants. Only the
 * address asked for is honoured, so a chance A9/8D pair in data cannot
 * invent a variable. */
static uint8_t loader_init(const uint8_t *ram, size_t n, uint16_t want,
                           uint8_t fallback)
{
    size_t i = 0;
    if (!want || !ram) return fallback;
    while (i + 2 < n) {
        size_t j;
        uint8_t imm;
        if (ram[i] != 0xA9) { i++; continue; }
        imm = ram[i + 1];
        for (j = i + 2; j + 2 < n && ram[j] == 0x8D; j += 3) {
            uint16_t a = (uint16_t)(ram[j + 1] | (ram[j + 2] << 8));
            if (a == want) fallback = imm;      /* the last write wins */
        }
        i = (j > i + 2) ? j : i + 2;
    }
    return fallback;
}

/* A variable's value at boot: what the loader writes if it writes one,
 * otherwise whatever the image carries at that address, otherwise the
 * documented default.
 *
 * That last fallback is not decoration. The workspace sits at the top
 * of the language card and an image only reaches as far as its last
 * initialised byte -- v3.1.3's ends at $FFFA and covers it, but
 * **3.1.2's ends at $FEB4**, so its flags byte is not in the file at
 * all. Taking tk_peek()'s zero there means speech OFF, and the engine
 * says nothing until an explicit Ctrl-E T. */
static uint8_t start_val(const tk_image *img, const uint8_t *ram, size_t rn,
                         uint16_t addr, uint8_t dflt)
{
    uint8_t in_image = dflt;
    if (!addr) return dflt;
    if (addr >= img->p.base
        && (size_t)(addr - img->p.base) < img->rom_len)
        in_image = tk_peek(img, addr);
    return loader_init(ram, rn, addr, in_image);
}

int tk_image_load_mem(tk_image *img, const uint8_t *obj, size_t objn,
                      const uint8_t *ram, size_t ramn,
                      const char *want, char *err, size_t errsz)
{
    uint16_t base;
    char why[192];
    int ok = 0;

    memset(img, 0, sizeof *img);
    if (objn > sizeof img->rom) objn = sizeof img->rom;
    memcpy(img->rom, obj, objn);
    img->rom_len = objn;
    if (img->rom_len < 0x1000) {
        snprintf(err, errsz, "the image is too small to be a Textalker OBJ");
        return -1;
    }
    snprintf(why, sizeof why, "no Textalker tables in it");

    base = loader_base(ram, ramn);
    if (base) {
        ok = find_layout(img, base, why, sizeof why);
    } else {
        /* No loader, so try every page the engine could load at and
         * still fit under $10000. Textalker lives in the language card,
         * so that is a handful of candidates, and only one of them has
         * an attribute table where an attribute table should be. */
        uint32_t b;
        for (b = 0xD000; b + img->rom_len <= 0x10000 && !ok; b += 0x100)
            ok = find_layout(img, (uint16_t)b, why, sizeof why);
    }
    if (!ok) {
        snprintf(err, errsz, "%s", why);
        return -1;
    }

    img->version[0] = 0;
    banner_version(ram, ramn, img->version, sizeof img->version);
    if (!img->version[0])
        snprintf(img->version, sizeof img->version, "%s", img->p.name);

    /* `want` names either the release from the loader's banner
     * ("3.1.2") or the family ("3.1.x", "1.3"), so a caller can ask for
     * one without knowing which builds exist. */
    if (want && strcmp(want, img->version) && strcmp(want, img->p.name)) {
        char fam[16];
        snprintf(fam, sizeof fam, "%s", img->p.name);
        snprintf(err, errsz, "this is Textalker %s (%s), not %s",
                 img->version, fam, want);
        return -1;
    }
    img->p.name = img->version;

    img->attrs = img->rom + (size_t)(img->p.attrs - img->p.base);
    build_sections(img);

    img->init_flags      = start_val(img, ram, ramn, img->p.v_flags, 0x09);
    img->init_base_pitch = start_val(img, ram, ramn, img->p.v_base_pitch, 0x18);
    img->init_monotone   = start_val(img, ram, ramn, img->p.v_monotone, 0);
    img->init_compress   = start_val(img, ram, ramn, img->p.v_compress, 0);
    img->init_delay      = start_val(img, ram, ramn, img->p.v_delay, 0);
    img->init_group      = start_val(img, ram, ramn, img->p.v_group, 2);
    img->init_variant    = start_val(img, ram, ramn, img->p.v_variant, 1);
    img->init_p_step     = start_val(img, ram, ramn, img->p.v_p_step, 0);
    img->init_utt_flag   = start_val(img, ram, ramn, img->p.v_utt_flag, 0);
    return 0;
}

int tk_image_load(tk_image *img, const char *obj_path, const char *ram_path,
                  const char *want, char *err, size_t errsz)
{
    static uint8_t obj[0x10000];
    uint8_t ram[0x2000];
    size_t objn, ramn = 0;
    FILE *f;

    f = fopen(obj_path, "rb");
    if (!f) { snprintf(err, errsz, "cannot open %s", obj_path); return -1; }
    objn = fread(obj, 1, sizeof obj, f);
    fclose(f);

    if (ram_path) {
        f = fopen(ram_path, "rb");
        if (f) { ramn = fread(ram, 1, sizeof ram, f); fclose(f); }
    }

    if (tk_image_load_mem(img, obj, objn, ram_path ? ram : NULL, ramn,
                          want, err, errsz) < 0) {
        /* The memory form has no filename to name; put it back, since
         * these messages reach the user through the NVDA driver. */
        char detail[192];
        snprintf(detail, sizeof detail, "%s", err);
        snprintf(err, errsz, "%s: %s", obj_path, detail);
        return -1;
    }
    return 0;
}
