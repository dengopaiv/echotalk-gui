/*
 * tk_syllable.c -- Textalker's syllabifier, $DD10-$E126. v3.1.3 only.
 *
 * A translation of tools/syllabify.py, which carries the reasoning and
 * the worked examples. The rule engine cannot be reproduced without
 * this: Textalker splits a word into syllables BEFORE matching any
 * letter-to-sound rule, and records the boundaries as real $E6 bytes in
 * the input buffer. They are not bookkeeping. $E6 masks to $66, whose
 * attribute is $00 -- neither vowel nor consonant nor separator -- so a
 * sentinel blocks every class, which is the whole of the "magic E":
 * HOPE is |HOPE| and its {F7} reaches the O, while HOPESX is |HO|PESX|
 * and the same {F7} runs into the sentinel and fails.
 *
 * The variables keep the 6502's names:
 *
 *   $FD9E end    one past the last character
 *   $FDA9 last   the last character the vowel loop may consider
 *   $FDA8 bound  the most recent boundary; scanning resumes after it
 *   $FDAA v1     first vowel        $FDAB v2  second vowel
 *   $FDA6 start  $FDA5 limit  $FDA7 hit  $FDA4 hitlen
 *
 * Every table is read out of the user's own image. Entries are
 * concatenated with the high bit set on each entry's last character,
 * and a $00 ends the list.
 */
#include <string.h>

#include "tk.h"

#define SENT       0xE6
#define VOWEL_BIT  0x80

#define LET(c)     ((uint8_t)(0x80 | (unsigned char)(c)))

/* The syllabifier's working state: the 6502 variables above, plus the
 * buffer it is editing in place. */
typedef struct {
    const tk_image *img;
    tk_buf  *buf;
    int end, last, bound, start, limit, hit, hitlen, v1, v2;
} syl;

static uint8_t g(const syl *s, int x)
{
    if (x < 0 || x >= s->buf->len) return 0x00;
    return s->buf->b[x];
}

static uint8_t attr_at(const syl *s, int x)
{
    return s->img->attrs[g(s, x) & 0x7F];
}

/* $E0F8: shift the tail right and drop a sentinel in at x.
 *
 * $E0FB walks down from $FD9E, so only the region up to the current end
 * moves. Anything beyond it is left where it is, which is exactly how a
 * previous utterance's tail stays readable. */
static void insert_sent(syl *s, int x)
{
    int i, top = s->end;
    if (top > s->buf->len - 2) top = s->buf->len - 2;
    for (i = top; i >= x; i--) s->buf->b[i + 1] = s->buf->b[i];
    if (x >= 0 && x < s->buf->len) s->buf->b[x] = SENT;
    s->end++;
    s->last++;
}

/* $E06A: find the first list entry occurring in buf[start..limit].
 * Sets hit to the entry's start index, 0 for no match, and hitlen to
 * its length. */
/* $E06A's inner loop is the syllabifier's real cost: it walks every
 * entry of a list against every position in the window. About 30
 * cycles a comparison -- LDA abs,X 4, AND 2, EOR ($00),Y 5, BEQ 3,
 * CMP 2, BEQ 3, CPX 2, BCS 3, INX 2, and the loop branch. */
#define TK_SYL_STEP 30

static void list_match(syl *s, uint16_t table_addr)
{
    const tk_image *img = s->img;
    uint16_t t = table_addr;
    s->hit = 0;
    while (tk_peek(img, t)) {
        int x = s->start, y = 0;
        for (;;) {
            uint8_t a = (uint8_t)((g(s, x) & 0x7F) ^ tk_peek(img, (uint16_t)(t + y)));
            tk_stage1_add(TK_SYL_STEP);
            if (a == 0x00) {                      /* $E08E: a non-final char */
                y++;
                if (x >= s->limit) break;
                x++;
                continue;
            }
            if (a == 0x80) {                      /* $E098: the last char */
                y++;
                s->hitlen = y;
                s->hit = x - y + 1;
                return;
            }
            if (x >= s->limit) break;             /* $E081 */
            if (y) { y = 0; continue; }           /* $E086: restart here */
            x++;
        }
        while (tk_peek(img, t) < 0x80) t++;       /* $E0AA: next entry */
        t++;
    }
}

/* $E0C9: the first vowel in buf[start..limit], or 0. */
static void scan_vowel(syl *s)
{
    int x = s->start;
    s->hit = 0;
    s->limit++;
    for (;;) {
        if (attr_at(s, x) & VOWEL_BIT) { s->hit = x; break; }
        x++;
        if (x == s->end || x >= s->limit) { s->hit = 0; break; }
    }
    s->limit--;
}

/* $DE4C. */
static void two_vowels(syl *s)
{
    s->v1 = s->v2 = 0;
    s->start = s->bound + 1;
    s->limit = s->last;
    scan_vowel(s);
    if (!s->hit) return;
    s->v1 = s->hit;
    s->start = s->hit + 1;
    s->limit = s->last;
    scan_vowel(s);
    s->v2 = s->hit;
}

/* $DD65: strip a prefix, but only from a word of 7 or more. */
static void do_prefix(syl *s)
{
    if (s->end - s->start < 7) return;
    s->limit = s->start + 4;
    list_match(s, s->img->p.prefixes);
    if (s->hit != s->start) return;               /* must match at the start */
    s->bound = s->hit + s->hitlen;
    insert_sent(s, s->bound);
}

/* $DD98: strip suffixes, one after another.
 *
 * Each match pulls "last" back in front of the suffix, so the next
 * search runs on what is left -- which is how CAREFULLY becomes
 * CARE|FUL|LY in two passes. Three shapes end the search early: an E
 * just before the suffix is kept with the stem ($DDFE); -LY after
 * anything but S, L or G takes no boundary at all ($DE10), which is why
 * PROBABLY comes out PRO|BA|BLY; and a suffix starting immediately
 * after the word start or a prefix boundary is refused ($DE3A). */
static void do_suffix(syl *s)
{
    if (s->end - s->start < 6) return;
    for (;;) {
        int a = s->last - 5, p, n, x;
        s->start = a >= s->bound ? a : s->bound + 1;
        s->limit = s->last;
        list_match(s, s->img->p.suffixes);
        if (s->hit < 3) return;
        p = s->hit;
        n = s->hitlen;
        x = s->last - n;
        if (x == p) {                             /* $DDEB: one char over */
            if (g(s, s->last) != LET('S')) return;    /* a plural S */
        } else if (x + 1 != p) {                  /* $DDE2: must end at last */
            return;
        }
        if (g(s, p - 1) == LET('E')) {            /* $DDFE */
            s->last = p - 3;
            insert_sent(s, p);                    /* which puts it at p - 2 */
            return;
        }
        if (g(s, p) == LET('L') && g(s, p + 1) == LET('Y')) {      /* $DE10 */
            uint8_t b = g(s, p - 1);
            if (b != LET('S') && b != LET('L') && b != LET('G')) return;
        }
        if (p - 1 == s->bound) return;            /* $DE3A */
        s->last = p - 2;
        insert_sent(s, p);                        /* which puts last at p-1 */
    }
}

/* $DE7A: the three inflections, when the second vowel is an E standing
 * last or second-to-last in the word. Each asks a different question
 * about the consonant before the E, and they are the /Iz/ plural and
 * the /Id/ past tense -- the same allomorphy the rule engine draws
 * again in its three -ED rules. Whenever one applies it ends the vowel
 * loop, by leaving "last" below v2 ($DF2D, tested at $DD3B). */
static void e_cases(syl *s)
{
    int v2 = s->v2;
    uint8_t b, a;
    if (g(s, v2) != LET('E') || v2 + 2 < s->end) return;
    b = g(s, v2 - 1);

    if (b == LET('L')) {                                        /* $DE8F */
        uint8_t nx = g(s, v2 + 1);
        if (v2 + 1 == s->end || nx == LET('D') || nx == LET('S')) {
            if (g(s, v2 - 2) != LET('L') && v2 - 2 != s->v1) {   /* $DEA8 */
                s->bound = v2 - 2;
                insert_sent(s, v2 - 2);
            }
            s->last = s->v2 - 1;
        }
        return;
    }
    if (v2 + 1 == s->end) {                                     /* $DEBD */
        s->last = v2 - 1;                                       /* silent E */
        return;
    }
    a = g(s, v2 + 1);
    if (a == LET('S')) {                                        /* $DECA */
        if (b == LET('G') || b == LET('S') || b == LET('C')) {
            s->bound = v2 - 1;
            insert_sent(s, v2 - 1);
        } else if (b == LET('H')
                   && (g(s, v2 - 2) == LET('S') || g(s, v2 - 2) == LET('C'))) {
            s->bound = v2 - 2;
            insert_sent(s, v2 - 2);
        }
        s->last = s->v2 - 1;
        return;
    }
    if (a != LET('D')) return;                                  /* $DF34 */
    if (b == LET('D')) {                                        /* $DEF1 */
        s->bound = v2 - 1;
        insert_sent(s, v2 - 1);
    } else if (b == LET('R')) {
        if (g(s, v2 - 2) == LET('D')) {                         /* $DF0E */
            s->bound = v2 - 2;
            insert_sent(s, v2 - 2);
        }
    } else if (b == LET('T')) {                                 /* $DF17 */
        int at = g(s, v2 - 2) == LET('S') ? v2 - 2 : v2 - 1;
        s->bound = at;
        insert_sent(s, at);
    }
    s->last = s->v2 - 1;
}

/* $DF35: the vowel-pair cases. Reached for every pair, but does nothing
 * unless the two vowels are adjacent -- except for the one shape it
 * checks first, a W between them followed by I or E, which always
 * splits: VOW|ELS, SHOW|ER.
 *
 * $DF3A tests CPX #$01, an ABSOLUTE buffer index, so that first shape
 * is disabled only for the word standing FIRST on the line. OWETEO
 * alone is |O|WE|TE|O| but "AAA OWETEO" gives |OW|E|TE|O|. */
static void adjacent(syl *s)
{
    int v1 = s->v1, v2 = s->v2;
    uint8_t a;

    if (v1 != 1 && g(s, v1 + 1) == LET('W') && v1 + 2 == v2
        && (g(s, v2) == LET('I') || g(s, v2) == LET('E'))) {
        s->bound = v2;                                       /* $DF51/$DF55 */
        insert_sent(s, v2);
        return;
    }
    if (v1 + 1 != v2) return;                                /* $DF5E */
    if (g(s, v1) == LET('U') && g(s, v1 - 1) == LET('Q')) {   /* $DF66 */
        s->bound = v1;                                       /* QU is one */
        return;
    }
    s->start = v1;
    s->limit = v2;
    list_match(s, s->img->p.digraphs);
    if (!s->hit) {                                           /* $DF95 */
        s->bound = v2;
        insert_sent(s, v2);
        return;
    }
    /* A digraph. It normally stays whole, but two shapes still split. */
    if (g(s, v2) == LET('I') && g(s, v2 + 1) == LET('N')
        && g(s, v2 + 2) == LET('G') && v2 + 2 < s->end) {
        s->bound = v2;                                       /* GO|ING */
        insert_sent(s, v2);
        return;
    }
    if (g(s, v1) == LET('I') && g(s, v1 + 1) == LET('E')) {   /* $DFB6 */
        a = g(s, v2 + 1);
        if (a == LET('R') && g(s, v2 + 2) != LET('C')) {      /* EAS|I|ER */
            s->bound = v2;
            insert_sent(s, v2);
            return;
        }
        if (a == LET('S') && g(s, v2 + 2) == LET('T')) {      /* EAS|I|EST */
            s->bound = v2;
            insert_sent(s, v2);
            return;
        }
    }
    s->bound = v1;                                           /* $DFEA */
}

/* $DFFD: divide the consonants between two vowels.
 *
 * One consonant splits V|CV, except R and X which split VC|V -- that is
 * SE|VEN against VER|Y. For a longer run, walk forward looking for a
 * cluster from the list that fills the window exactly; the cluster goes
 * with the following vowel, so DEE|PLY, and with no match the last
 * consonant goes with it, so DOC|TOR. */
static void between(syl *s)
{
    int v1 = s->v1, v2 = s->v2;

    if (v1 + 2 == v2) {                                      /* $E002 */
        uint8_t c = g(s, v1 + 1);
        int at = (c == LET('R') || c == LET('X')) ? v2 : v1 + 1;
        s->bound = at;
        insert_sent(s, at);
        return;
    }
    s->start = v1 + 1;                                       /* $E019 */
    for (;;) {
        int x;
        s->limit = s->start + 2;
        if (s->limit == v2) s->limit--;                      /* $E025 */
        list_match(s, s->img->p.clusters);
        if (s->hit == s->start && s->limit - s->start + 1 == s->hitlen) {
            s->bound = s->start;                             /* $E04E */
            insert_sent(s, s->start);
            return;
        }
        x = s->start + 1;
        if (x == s->limit) {                                 /* $E058 */
            s->bound = x;
            insert_sent(s, x);
            return;
        }
        s->start = x;
    }
}

int tk_syllabify_at(const tk_image *img, tk_buf *buf, int start)
{
    syl s;
    if (!img->p.syllabifier) return buf->end;

    memset(&s, 0, sizeof s);
    s.img = img;
    s.buf = buf;
    s.end = buf->end;
    s.last = buf->end - 1;
    s.bound = start;
    s.start = start;

    insert_sent(&s, start);                       /* $DD17 */
    s.start = start + 1;
    if (g(&s, start + 1) == LET('Y'))             /* $DD20: a leading Y is a */
        s.bound = start + 1;                      /* consonant, not a nucleus */
    do_prefix(&s);
    do_suffix(&s);
    for (;;) {                                    /* $DD30 */
        two_vowels(&s);
        if (!s.v2) break;
        e_cases(&s);
        if (s.last < s.v2) break;                 /* $DD3B */
        adjacent(&s);
        if (s.bound >= s.v1) continue;            /* $DD46 */
        between(&s);
    }
    insert_sent(&s, s.end);                       /* $DD54: trailing sentinel */
    buf->end = s.end;
    return s.end;
}

/* $DB5E and $DD10 together. The buffer is not one word: the scanner
 * copies a word in, runs the syllabifier over it in place, then copies
 * the next separator and word after it. So "THE CAT." becomes
 * |THE| |CAT|. -- and the "." sits in the same buffer as CAT, which is
 * what lets {F3}DR{E6}{AE} match "DR." across the trailing sentinel.
 *
 * A word is a run of letters, digits and apostrophes; every other
 * character stands alone. Measured against the emulator's own buffer,
 * AB1CD and AB'CD are one word each while AB-CD and AB$CD are three. */
static int is_word_char(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z') || c == '\'';
}

static unsigned char up(unsigned char c)
{
    return (c >= 'a' && c <= 'z') ? (unsigned char)(c - 32) : c;
}

void tk_build_line(const tk_image *img, const char *text, tk_buf *buf)
{
    size_t i = 0, n = strlen(text);

    memset(buf, 0, sizeof *buf);
    if (!buf->size) buf->size = TK_BUFSZ;
    buf->len = buf->size;
    buf->end = 0;
    while (i < n && buf->end < buf->size - 4) {
        if (is_word_char((unsigned char)text[i])) {
            int start = buf->end;
            size_t j = i;
            while (j < n && is_word_char((unsigned char)text[j])
                   && buf->end < buf->size - 4) {
                buf->b[buf->end++] = LET(up((unsigned char)text[j]));
                j++;
            }
            tk_syllabify_at(img, buf, start);
            i = j;
        } else {
            buf->b[buf->end++] = LET(up((unsigned char)text[i]));
            i++;
        }
    }
    /* Without a syllabifier the buffer is just the text, so `len` and
     * `end` coincide; with one, the sentinels are already in place. */
    buf->len = buf->end;
}
