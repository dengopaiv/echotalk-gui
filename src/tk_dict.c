/*
 * tk_dict.c -- a pronunciation dictionary as a letter-to-sound source.
 *
 * The first thing plugged into the seam, and the one that answers the
 * licensing question. EchoTalk does not SHIP a dictionary; it reads one
 * the user supplies, at run time, exactly as it reads the Textalker
 * image. So whose dictionary it is, and what licence that dictionary
 * carries, is the user's business and not a term of EchoTalk's own
 * BSD-3-Clause licence. THIRD_PARTY_LICENSES.md records the reasoning.
 *
 * The format is CMUdict's, unchanged, so cmudict.dict works as it
 * stands:
 *
 *     ;;; anything after ;;; or # is a comment
 *     CAT  K AE1 T
 *     CAT(2)  K AA1 T          <- variants are ignored; the first wins
 *     TOMATO  T AH0 M EY1 T OW0
 *
 * Plus two extensions, because ARPABET is not what anybody who has
 * actually used one of these cards reaches for first:
 *
 *     BOOT      =booht           <- a RESPELLING
 *     NICKNAME  /NIKN@M          <- Ctrl-V phoneme characters
 *
 * **The respelling is the easy one and probably the one to use.** Say
 * the word the way you want it heard, spelled however makes Textalker
 * say it, and its own rules do the rest. Jayson's example is the whole
 * argument: Textalker pronounces BOOT with the vowel of BOOK, and
 * spelling it "booht" makes it say "boot". No phoneme chart needed, and
 * anyone who has lived with an Echo has a head full of these already.
 *
 * The "/" form is Street Electronics' own Ctrl-V notation, which the
 * Echo manual documents, for when you want an exact allophone. A digit
 * 1-3 before a vowel character sets its pitch group, 3 being the
 * stressed one.
 *
 * A DICTIONARY IS WRITTEN FOR ONE TEXTALKER VERSION, and the format has
 * no field that says which. A respelling repairs a rule bug, and the
 * rule bugs differ: v3.1.3 gives BOOT, ROOT and SHOOT the vowel of BOOK
 * where v1.3 gets all three right, so "=booht" fixes v3.1.3 and costs
 * v1.3 the silence the H inserts. Nothing here can detect that -- an
 * entry is applied to whichever image is loaded -- so it is the host's
 * job to offer one dictionary per voice, and echotalk.h says so.
 *
 * WHAT IT DOES NOT DO is take over the line. Only the words it knows
 * are its own; every space, every mark of punctuation and every word it
 * has never heard of goes back to Textalker's rules through
 * tk_run_range(), which is why the word separator, the spoken
 * punctuation names and the stress pass all still behave exactly as
 * they did. A dictionary that covered nothing would produce
 * byte-identical output to no dictionary at all, and there is a test
 * for that.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tk.h"

static int dict_run(void *ctx, const tk_image *img, const tk_buf *buf,
                    uint8_t flags, uint8_t *out, int outcap);

#define HASHSZ 8192            /* power of two; chained, so load is fine */

typedef struct entry {
    struct entry *next;
    uint32_t      codes_at;    /* offset into d->codes */
    uint8_t       ncodes;
    char          word[1];     /* over-allocated */
} entry;

struct tk_dict {
    tk_g2p    g2p;             /* first, so a tk_dict * is a tk_g2p * */
    entry    *bucket[HASHSZ];
    uint8_t  *codes;
    size_t    ncodes, codes_cap;
    long      entries, hits, misses;
    const tk_image *img;
};

static uint32_t hash_word(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

static int is_word_char(uint8_t masked)
{
    return (masked >= 'A' && masked <= 'Z')
        || (masked >= '0' && masked <= '9')
        || masked == '\'';
}

/* ------------------------------------------------------------- loading */

static int reserve(tk_dict *d, size_t n)
{
    if (d->ncodes + n <= d->codes_cap) return 1;
    {
        size_t want = d->codes_cap ? d->codes_cap * 2 : 65536;
        uint8_t *p;
        while (want < d->ncodes + n) want *= 2;
        p = (uint8_t *)realloc(d->codes, want);
        if (!p) return 0;
        d->codes = p;
        d->codes_cap = want;
    }
    return 1;
}

/* The "=..." form: a respelling, run through Textalker's OWN rules.
 *
 * Resolved here, at load time, over the respelling by itself -- so it
 * gets the syllabifier and the whole rule engine but not the
 * surrounding line's context, which is exactly what a respelling is
 * for. $FFAD is left at its $09 default because nothing in a bare word
 * reads the punctuation or letter-mode bits. */
static int respell_to_codes(const tk_image *img, const char *s,
                            uint8_t *out, int cap)
{
    tk_buf buf;
    tk_build_line(img, s, &buf);
    return tk_run_range(img, &buf, 0x09, 0, buf.end, out, 0, cap, NULL);
}

/* The "/..." form: Textalker's own Ctrl-V characters, resolved through
 * the character map in the user's image rather than any table here. */
static int ctrlv_to_codes(const tk_image *img, const char *s,
                          uint8_t *out, int cap)
{
    int n = 0, group = 2;
    for (; *s; s++) {
        unsigned char c = (unsigned char)toupper((unsigned char)*s);
        uint8_t idx;
        if (c >= '1' && c <= '3') { group = c - '0'; continue; }
        if (c < 0x20 || c >= 0x60) continue;
        idx = tk_peek(img, (uint16_t)(img->p.charmap + c - 0x20));
        if (idx >= 0x32) return -1;             /* not an allophone */
        if (n < cap) out[n++] = tk_arpa_code(idx, group);
        if (idx < 0x1A) group = 2;              /* the group is per-vowel */
    }
    return n;
}

static int add(tk_dict *d, const char *word, const uint8_t *codes, int n)
{
    uint32_t h = hash_word(word) & (HASHSZ - 1);
    entry *e;
    for (e = d->bucket[h]; e; e = e->next)
        if (!strcmp(e->word, word)) return 1;   /* first spelling wins */
    e = (entry *)malloc(sizeof(entry) + strlen(word));
    if (!e) return 0;
    if (!reserve(d, (size_t)n)) { free(e); return 0; }
    strcpy(e->word, word);
    e->codes_at = (uint32_t)d->ncodes;
    e->ncodes = (uint8_t)n;
    memcpy(d->codes + d->ncodes, codes, (size_t)n);
    d->ncodes += (size_t)n;
    e->next = d->bucket[h];
    d->bucket[h] = e;
    d->entries++;
    return 1;
}

tk_dict *tk_dict_open(const tk_image *img, const char *path,
                      char *err, size_t errsz)
{
    tk_dict *d;
    FILE *f;
    char line[1024];
    long lineno = 0, skipped = 0;

    f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errsz, "cannot open dictionary %s", path);
        return NULL;
    }
    d = (tk_dict *)calloc(1, sizeof *d);
    if (!d) { fclose(f); snprintf(err, errsz, "out of memory"); return NULL; }
    d->img = img;
    d->g2p.name = "dictionary";
    d->g2p.ctx = d;
    d->g2p.run = dict_run;

    while (fgets(line, sizeof line, f)) {
        char *p = line, *w, *pron;
        uint8_t codes[TK_BUFSZ];
        int n;
        lineno++;
        if (!strncmp(p, ";;;", 3) || *p == '#') continue;
        while (*p && isspace((unsigned char)*p)) p++;
        w = p;
        while (*p && !isspace((unsigned char)*p)) {
            *p = (char)toupper((unsigned char)*p);
            p++;
        }
        if (p == w) continue;
        if (*p) *p++ = '\0';
        pron = p;
        while (*pron && isspace((unsigned char)*pron)) pron++;
        if (!*pron) continue;
        {   /* strip the trailing newline and any comment */
            char *q = strpbrk(pron, "\r\n");
            if (q) *q = '\0';
            q = strchr(pron, '#');
            if (q) *q = '\0';
        }
        if (strchr(w, '(')) continue;           /* CMUdict variant */

        if (*pron == '=') {
            n = respell_to_codes(img, pron + 1, codes, (int)sizeof codes);
        } else if (*pron == '/') {
            n = ctrlv_to_codes(img, pron + 1, codes, (int)sizeof codes);
        } else {
            char bad[32];
            n = tk_arpabet_to_codes(pron, codes, (int)sizeof codes,
                                    bad, sizeof bad);
        }
        if (n <= 0) { skipped++; continue; }
        if (!add(d, w, codes, n)) {
            snprintf(err, errsz, "out of memory reading %s", path);
            fclose(f);
            tk_dict_free(d);
            return NULL;
        }
    }
    fclose(f);
    if (!d->entries) {
        snprintf(err, errsz, "%s has no usable entries in %ld lines",
                 path, lineno);
        tk_dict_free(d);
        return NULL;
    }
    /* Not an error -- CMUdict has punctuation names and foreign symbols
     * that translate to nothing -- but a caller that wants to know can
     * compare tk_dict_entries() with the file's line count. */
    (void)skipped;
    return d;
}

void tk_dict_free(tk_dict *d)
{
    int i;
    if (!d) return;
    for (i = 0; i < HASHSZ; i++) {
        entry *e = d->bucket[i];
        while (e) { entry *nx = e->next; free(e); e = nx; }
    }
    free(d->codes);
    free(d);
}

long tk_dict_entries(const tk_dict *d) { return d ? d->entries : 0; }
long tk_dict_hits(const tk_dict *d)    { return d ? d->hits : 0; }
long tk_dict_misses(const tk_dict *d)  { return d ? d->misses : 0; }

static const entry *find(tk_dict *d, const char *word)
{
    uint32_t h = hash_word(word) & (HASHSZ - 1);
    const entry *e;
    for (e = d->bucket[h]; e; e = e->next)
        if (!strcmp(e->word, word)) return e;
    return NULL;
}

/* ------------------------------------------------------------- the source */

/* A word runs over word characters AND over any syllable sentinel that
 * has a word character after it: the syllabifier writes $E6 INSIDE a
 * word ($BAN$A$NA$), so stopping at the first one would look up three
 * fragments and find none of them. A sentinel followed by anything else
 * is the word's closing bracket and belongs to the gap. */
static int word_span(const tk_buf *buf, int i, int stop, char *key, int keysz)
{
    int n = 0;
    while (i < stop && i < buf->len) {
        uint8_t m = (uint8_t)(buf->b[i] & 0x7F);
        if (is_word_char(m)) {
            if (n + 1 < keysz) key[n] = (char)m;
            n++;
            i++;
            continue;
        }
        if (buf->b[i] == 0xE6 && i + 1 < stop && i + 1 < buf->len
            && is_word_char((uint8_t)(buf->b[i + 1] & 0x7F))) {
            i++;
            continue;
        }
        break;
    }
    if (n >= keysz) return -1;                  /* absurdly long; let the
                                                 * rules have it */
    key[n] = '\0';
    return i;
}

static int dict_run(void *ctx, const tk_image *img, const tk_buf *buf,
                    uint8_t flags, uint8_t *out, int outcap)
{
    tk_dict *d = (tk_dict *)ctx;
    int cursor = 0, n = 0;
    int stop = buf->end;

    int phoneme_cap = buf->size ? TK_CAP_OF(buf->size) : TK_PHONEME_CAP;

    while (cursor < stop && cursor < buf->len && n < phoneme_cap) {
        char key[64];
        int wend, gap;

        if (is_word_char((uint8_t)(buf->b[cursor] & 0x7F))) {
            const entry *e;
            wend = word_span(buf, cursor, stop, key, (int)sizeof key);
            e = (wend > cursor) ? find(d, key) : NULL;
            if (e) {
                int i;
                for (i = 0; i < e->ncodes && n < outcap; i++)
                    out[n++] = d->codes[e->codes_at + i];
                d->hits++;
                cursor = wend;
                continue;
            }
            if (wend > cursor) {
                d->misses++;
                n = tk_run_range(img, buf, flags, cursor, wend,
                                 out, n, outcap, &cursor);
                if (n < 0) return -1;
                continue;
            }
        }

        /* Everything that is not a word the dictionary knows: spaces,
         * punctuation, the closing sentinels, Ctrl-E K letter marks.
         * Handed back a run at a time so the rules keep their context
         * and the stress pass keeps its backward walk. */
        gap = cursor;
        while (gap < stop && gap < buf->len
               && !is_word_char((uint8_t)(buf->b[gap] & 0x7F)))
            gap++;
        if (gap == cursor) gap = cursor + 1;
        n = tk_run_range(img, buf, flags, cursor, gap, out, n, outcap,
                         &cursor);
        if (n < 0) return -1;
    }
    return n;
}

const tk_g2p *tk_dict_g2p(tk_dict *d)
{
    return d ? &d->g2p : NULL;
}
