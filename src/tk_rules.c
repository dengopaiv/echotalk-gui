/*
 * tk_rules.c -- Textalker's letter-to-sound rule engine, $E1FA-$E5A9.
 *
 * A translation of tools/rule_walk.py, which carries the reasoning.
 * This is a transcription of the 6502, not a model of it: it walks the
 * same buffer the machine walks, sentinels and all, and reads every
 * rule out of the user's own image.
 *
 *   $E1FA  main loop: step over sentinels, counting them into $FD93,
 *          pick the section from the dispatch table, try rules in order
 *   $E256  the spoken names of punctuation (Ctrl-E A and M)
 *   $E29B  the stress pass
 *   $E2F0  find the anchor -- the first pattern byte in $A0-$EF -- and
 *          match the literal run forward from the cursor
 *   $E354  left context, backward      $E3A9  right context, forward
 *   $E40D  class dispatch for $F3-$FF, through an RTS-trick table
 *   $E596  attribute lookup: A AND $EF24[buf[X] AND $7F]
 *
 * The metacharacters, every one read from the code rather than fitted:
 *
 *   $C1-$DA  focus letter, matched AND consumed
 *   $81-$9A  context letter, $80 + index; $E383/$E3D9 do ADC #$40
 *   $C0      wildcard inside the literal run              $E309
 *   $9C      wildcard in either context                   $E378 $E3CE
 *   $9B      the syllable sentinel: $A6 + $40 = $E6       $E380 $E3D6
 *   $F3      word boundary                                $E439
 *   $F4      consonant          $F5  E, I, Y              $E450 $E458
 *   $F6      B D G J L M N R V W Z                        $E460
 *   $F7      one or more vowels, GREEDY                   $E468
 *   $F8      zero or more consonants, GREEDY              $E47F
 *   $F9      an inflectional ending                       $E49E
 *   $FA      bit $01, or the C/S/T of a digraph           $E510
 *   $FB      bit $10, or the H of one                     $E53F
 *   $FC      skip sentinels      $FD  one stop consonant  $E48F $E569
 *   $FE $FF  zero width: syllable-counter tests           $E585 $E590
 *
 * $F7 and $F8 are greedy with NO backtracking: the loop runs to the end
 * of the class and steps back exactly one.
 */
#include <string.h>

#include "tk.h"

#define SENT   0xE6            /* the syllable sentinel, as stored */
#define TERM   0x00            /* what lies past either end of the buffer */

/* Attribute bits at $EF24, named by the handler that reads them. */
#define A_VOWEL 0x80
#define A_CONS  0x40
#define A_F6    0x20
#define A_FB    0x10
#define A_F5    0x04
#define A_SEP   0x02
#define A_FA    0x01

#define LET(c) ((uint8_t)(0x80 | (unsigned char)(c)))

/* $E57E: P T K B D G C */
static const uint8_t STOPS[7] = { 0xD0, 0xD4, 0xCB, 0xC2, 0xC4, 0xC7, 0xC3 };

/* What stage 1 costs the 6502, counted structurally rather than
 * guessed: the port already knows how many characters it dispatched,
 * how many rules it tried and how many pattern bytes it compared, and
 * those are exactly the loops the cycles go into.
 *
 *   $E1FA per character: LDX 4, LDA abs,X 4, AND 2, STA 4, CMP 2,
 *         BNE 3, JSR $E256 and its punctuation check, then the
 *         dispatch-table lookup at $E216                        ~ 80
 *   $E2F0 per rule tried: the anchor search, the literal run's
 *         set-up, and $E31F's walk to the next rule on failure   ~ 55
 *   $E354 per pattern byte examined: DEX 2, DEY 2, LDA ($00),Y 5,
 *         BMI 3, CPX 2, BNE 3, then the compare at $E380         ~ 28
 *   $E40D per class handler: AND 2, TAY 2, LDA 4, PHA 3, LDA 4,
 *         PHA 3, LDY 4, STX 4, RTS 6, plus the handler body      ~ 60
 *
 * The per-rule figure carries $E3E2's set-up as well as $E2F0's, since
 * a rule attempt patches the direction into five sites TWICE, once for
 * each context. These are CALIBRATED against ECHOTALK_CYCLE_PROFILE on
 * the harness rather than counted instruction by instruction: for ten
 * letters it reports 137,000 cycles in $E000-$E4FF, and the model has
 * to land there. Re-check it with that profile if the rules change.
 */
#define TK_S1_CHAR    80
#define TK_S1_RULE   175      /* incl. $E3E2's direction patching, twice */
#define TK_S1_PATBYTE 28
#define TK_S1_CLASS   60
#define TK_S1_ANCHOR  17      /* $E2F6's scan for the focus letter */

static unsigned long g_s1_cycles;

unsigned long tk_stage1_cycles(void) { return g_s1_cycles; }

void tk_stage1_add(unsigned long c) { g_s1_cycles += c; }

/* Reset at the END of an utterance, not the start of the rule
 * walk: the syllabifier runs during buffering, one word at a
 * time, long before stage 1 is called. */
void tk_stage1_reset(void) { g_s1_cycles = 0; }

static uint8_t bat(const tk_buf *buf, int x)
{
    if (x < 0 || x >= buf->len) return TERM;
    return buf->b[x];
}

static uint8_t attr(const tk_image *img, const tk_buf *buf, int x)
{
    return img->attrs[bat(buf, x) & 0x7F];
}

/* ------------------------------------------------------- rule iteration */

/* A rule is a run of bytes >= $80 (the pattern) followed by a run of
 * bytes < $80 (the phoneme payload), so a rule ends at the high-to-low
 * transition. Do NOT split on $F3 -- that is the word-boundary
 * metacharacter, not a delimiter. */
typedef struct {
    const tk_image *img;
    /* 32-bit because the end of the image is base + rom_len, which for
     * v1.3 is exactly $10000: as a uint16_t that is 0, and every junk
     * section comes back empty. 3.1.3's image ends at $FFFA and hid it. */
    uint32_t pos, stop;
    int      left;             /* rules remaining, or -1 for unlimited */
    int      stop_on_empty;    /* $DADB just tries the next rule forever */
} rule_iter;

static void rules_begin(rule_iter *it, const tk_image *img,
                        uint32_t start, uint32_t stop, int limit,
                        int skip_payload)
{
    it->img = img;
    it->pos = start;
    it->stop = stop;
    it->left = limit;
    it->stop_on_empty = limit >= 0;
    if (skip_payload) {
        /* A real section pointer lands on the last payload byte of what
         * precedes it, so start is already a pattern byte. A junk
         * pointer can land anywhere, and $DA96's anchor search then
         * walks forward over the payload bytes without caring. */
        while (it->pos < it->stop
               && tk_peek(img, (uint16_t)it->pos) < 0x80) it->pos++;
    }
}

static int rules_next(rule_iter *it, uint16_t *pat, int *patlen,
                      uint16_t *pho, int *pholen)
{
    for (;;) {
        uint32_t ps, hs;
        if (it->left == 0) return 0;
        if (it->pos >= it->stop) return 0;
        ps = it->pos;
        while (it->pos < it->stop
               && tk_peek(it->img, (uint16_t)it->pos) >= 0x80)
            it->pos++;
        *pat = (uint16_t)ps;
        *patlen = (int)(it->pos - ps);
        hs = it->pos;
        while (it->pos < it->stop
               && tk_peek(it->img, (uint16_t)it->pos) < 0x80)
            it->pos++;
        *pho = (uint16_t)hs;
        *pholen = (int)(it->pos - hs);
        if (it->left > 0) it->left--;
        if (*patlen) return 1;
        if (it->stop_on_empty) return 0;
        if (it->pos == ps) return 0;       /* no progress; cannot happen */
    }
}

/* $E256-$E298: the spoken names of punctuation. Ctrl-E A and M turn
 * this on. It is a table of three-byte {ptr_lo, ptr_hi, char} entries
 * searched DOWNWARD from $53 (A, all) or $4A (M, most); the three
 * entries between are line feed, carriage return and space, which only
 * A speaks. A hit replaces the dispatch-table pointer, so "." reaches a
 * rule that spells PERIOD instead of the one that makes a pause. */
static int punct_section(const tk_image *img, uint8_t ch, int top,
                         uint32_t *start, uint32_t *stop)
{
    uint16_t table = img->p.punct_names;
    int x = top;
    for (; x >= 2; x -= 3) {
        if (tk_peek(img, (uint16_t)(table + x)) == ch) {
            uint16_t p = (uint16_t)(tk_peek(img, (uint16_t)(table + x - 2))
                       | (tk_peek(img, (uint16_t)(table + x - 1)) << 8));
            *start = (uint16_t)(p + 1);
            *stop = (uint32_t)img->p.base + img->rom_len;
            return 1;
        }
    }
    return 0;
}

/* $E216 (and v1.3's $DA09) index the dispatch table with the raw
 * character and follow whatever pointer comes back. The table only has
 * entries for $20-$65, so a character above that reads its "pointer"
 * out of the RULES -- and the engine then parses rule bytes at wherever
 * that lands. It is nonsense, but it is deterministic nonsense that
 * reaches the phoneme buffer, so it has to be followed rather than
 * rejected. v1.3's $7F points at $F3C5, in the allophone data, and
 * yields 12 0C. Only a pointer below the image is refused: there the
 * 6502 is reading main RAM, which the OBJ says nothing about. */
static int dispatch_at(const tk_image *img, uint8_t c,
                       uint32_t *start, uint32_t *stop)
{
    uint16_t e = (uint16_t)(img->p.dispatch + 2 * c);
    uint16_t p;
    if (e < img->p.base) return 0;
    p = (uint16_t)(tk_peek(img, e) | (tk_peek(img, (uint16_t)(e + 1)) << 8));
    if (p < img->p.base || (size_t)(p - img->p.base) >= img->rom_len) return 0;
    *start = (uint16_t)(p + 1);
    *stop = (uint32_t)img->p.base + img->rom_len;
    return 1;
}

/* ------------------------------------------------------------- classes */
/* Each handler reports whether it matched and where it left the cursor.
 * The caller then steps by one in the direction of travel, so a handler
 * that leaves x alone consumes exactly one character, and a zero-width
 * one steps back by one so the next step lands on the same place. */

/* $E439: word boundary. v1.3's $DC81 is only the attribute test --
 * there are no sentinels to skip. */
static int h_f3(const tk_image *img, const tk_buf *buf, int *x,
                int step, int limit)
{
    if (img->p.v13_metas) return (attr(img, buf, *x) & A_SEP) != 0;
    if (bat(buf, *x) == SENT) *x += step;
    if (*x == limit) return 1;
    return (attr(img, buf, *x) & A_SEP) != 0;
}

/* $E468 ($F7) and $E47F ($F8): a greedy run, no backtracking.
 *
 * 3.1.3 steps back one at the end of both ($E48C). v1.3's $F8 does the
 * same ($DBB6, $DCA2) but its $F7 does NOT -- $DC0F and $DD2E return
 * with X wherever the loop stopped -- so a vowel run there swallows one
 * extra position. That is what makes {F3}{82}{F7}G fail for BEGIN and
 * gives "bih-jin" rather than "beg-in". */
static int h_run(const tk_image *img, const tk_buf *buf, int *x,
                 int step, int limit, uint8_t bit, int need_one,
                 int step_back)
{
    int start = *x;
    while (attr(img, buf, *x) & bit) {
        *x += step;
        if (*x == limit) break;
    }
    if (need_one && *x == start) { *x = start; return 0; }
    if (step_back) *x -= step;
    return 1;
}

/* $E48F: skip a run of sentinels; always succeeds. */
static int h_fc(const tk_buf *buf, int *x, int step, int limit)
{
    while (bat(buf, *x) == SENT) {
        *x += step;
        if (*x == limit) break;
    }
    *x -= step;
    return 1;
}

/* $E510: attribute bit $01, or the C/S/T of a CH/SH/TH digraph. */
static int h_fa(const tk_image *img, const tk_buf *buf, int *x, int end)
{
    uint8_t c;
    if (attr(img, buf, *x) & A_FA) return 1;
    if (*x + 1 == end || bat(buf, *x + 1) != LET('H')) return 0;
    c = bat(buf, *x);
    return c == LET('C') || c == LET('S') || c == LET('T');
}

/* $DB6F: v1.3's $FA -- attribute bit $01, or the H of CH, SH or TH.
 *
 * 3.1.3's looks FORWARD for the H and matches the C, S or T; v1.3's
 * looks at the H itself and steps back onto the C/S/T, which it then
 * leaves X sitting on. So it consumes two characters, the way 3.1.3's
 * $FB does, and it is why {FA}U fires for the U of ENTHUSIASM. */
static int h_fa_v13(const tk_image *img, const tk_buf *buf, int *x)
{
    uint8_t c;
    if (attr(img, buf, *x) & A_FA) return 1;
    if (bat(buf, *x) != LET('H')) return 0;
    (*x)--;
    if (*x < 0) return 0;
    c = bat(buf, *x);
    return c == LET('C') || c == LET('S') || c == LET('T');
}

/* $E53F: attribute bit $10, or the H of CH or SH. The DEX at $E552 is
 * not patched for direction and is not undone on the success path, so
 * when $FB matches through the digraph it leaves x one further back
 * than when it matches through the bit. */
static int h_fb(const tk_image *img, const tk_buf *buf, int *x)
{
    uint8_t c;
    if (attr(img, buf, *x) & A_FB) return 1;
    if (bat(buf, *x) != LET('H')) return 0;
    (*x)--;
    if (*x < 0) return 0;
    c = bat(buf, *x);
    return c == LET('C') || c == LET('S');
}

/* $E49E: an inflectional ending that runs to the word end.
 *
 * E+R, E+S, E+ST, E+D, E+LY, I+NG or a bare final E, optionally with a
 * trailing S, and then the word must end or a boundary follow. This is
 * what makes SILENCE come out right: $F9 lands on the E of -LENCE,
 * which is followed by N, so the rule fails and the engine falls
 * through to I{F8}{9B} => 27 instead of 20. */
typedef struct {
    const tk_image *img;
    const tk_buf *buf;
    int end, step, limit, start;
} f9ctx;

static int f9_e4fe(f9ctx *c, int *x)
{
    (*x)++;
    if (*x == c->end) return 1;
    if (h_f3(c->img, c->buf, x, c->step, c->limit)) return 1;
    *x = c->start;
    return 0;
}

static int f9_e4f6(f9ctx *c, int *x)
{
    if (bat(c->buf, *x) == LET('S')) return f9_e4fe(c, x);
    (*x)--;
    return f9_e4fe(c, x);
}

static int f9_e4eb(f9ctx *c, int *x)
{
    if (*x >= c->end) { *x = c->start; return 0; }
    (*x)++;
    if (*x == c->end) return 1;
    return f9_e4f6(c, x);
}

static int h_f9(const tk_image *img, const tk_buf *buf, int *x, int end,
                int step, int limit)
{
    f9ctx c;
    uint8_t a;
    c.img = img; c.buf = buf; c.end = end; c.step = step; c.limit = limit;
    c.start = *x;

    a = bat(buf, *x);
    if (a == LET('E')) {
        (*x)++;
        a = bat(buf, *x);
        if (a == LET('R')) return f9_e4eb(&c, x);
        if (a == LET('S')) {
            (*x)++;
            if (*x == end) return 1;
            if (bat(buf, *x) == LET('T')) return f9_e4eb(&c, x);
            (*x)--;
            return f9_e4fe(&c, x);
        }
        if (a == LET('D')) return f9_e4fe(&c, x);
        if (a == LET('L')) {
            (*x)++;
            if (bat(buf, *x) == LET('Y')) return f9_e4eb(&c, x);
            *x = c.start;
            return 0;
        }
        return f9_e4f6(&c, x);
    }
    if (a == LET('I')) {
        (*x)++;
        if (bat(buf, *x) != LET('N')) { *x = c.start; return 0; }
        (*x)++;
        if (bat(buf, *x) != LET('G')) { *x = c.start; return 0; }
        return f9_e4eb(&c, x);
    }
    *x = c.start;
    return 0;
}

/* $DCC4: v1.3's inflectional ending.
 *
 * E followed by R, S or D; E, L, Y; or I, N, G -- and then a word
 * boundary. The branch at $DCE2 is the one that matters: an E followed
 * by anything that is not R, S, D or L falls THROUGH to the boundary
 * test at $DD0F rather than failing, which is how a bare final E
 * matches. Reading it as a failure costs A{F4}{F9}, and with it the
 * second A of INANIMATE.
 *
 * Every step is capped at a FIXED $8F, not at the end of the text:
 * $DD0D reaching $8F is the only way to succeed without a boundary.
 * Standing the end of the buffer there instead makes E{F4}{F9} match
 * "EZES", because the machine goes on to test the leftover byte past
 * the S and finds it is not a separator.
 *
 * The I path is A BUG IN v1.3 and has to be reproduced: $DCF8 does INX
 * but never reloads the character, so $DCFD's CMP #$CE still sees the
 * I. {F9} can therefore never match "ING" in 1.3, which is why DARLING
 * takes the plain L rule where the pattern says it should not. */
#define F9_CAP 0x8F

static int f9v_dd0f(const tk_image *img, const tk_buf *buf, int *x,
                    int step, int limit, int start)
{
    if (h_f3(img, buf, x, step, limit)) return 1;
    *x = start;
    return 0;
}

static int f9v_dd0a(const tk_image *img, const tk_buf *buf, int *x,
                    int step, int limit, int start)
{
    (*x)++;
    if (*x == F9_CAP) return 1;
    return f9v_dd0f(img, buf, x, step, limit, start);
}

static int h_f9_v13(const tk_image *img, const tk_buf *buf, int *x,
                    int step, int limit)
{
    int start = *x;
    uint8_t a = bat(buf, *x);

    if (a == LET('E')) {
        (*x)++;
        if (*x == F9_CAP) { *x = start; return 0; }        /* $DCD1 */
        a = bat(buf, *x);
        if (a == LET('R') || a == LET('S') || a == LET('D'))
            return f9v_dd0a(img, buf, x, step, limit, start);
        if (a == LET('L')) {                               /* $DCE2 */
            (*x)++;
            if (*x == F9_CAP) { *x = start; return 0; }    /* $DCE9 */
            if (bat(buf, *x) == LET('Y'))
                return f9v_dd0a(img, buf, x, step, limit, start);
            *x = start;
            return 0;
        }
        return f9v_dd0f(img, buf, x, step, limit, start);  /* bare final E */
    }
    *x = start;
    return 0;
}

/* v1.3's context matcher is a chain of comparisons ($DB2B) rather than
 * 3.1.3's RTS-trick table, and everything above $FA falls into one
 * handler. The two directions map the codes DIFFERENTLY, which 3.1.3
 * never does. Backward ($DB2B): $F6 is the voiced test, $F9 a second
 * $F8. Forward ($DC4D): $F6 is just "a consonant", and $F9 AND
 * EVERYTHING ABOVE IT is the inflectional-ending matcher. */
static uint8_t v13_remap(uint8_t a, int back)
{
    if (back) {
        switch (a) {
        case 0xF9: return 0xF8;
        case 0xFC: case 0xFD: case 0xFE: case 0xFF: return 0xFB;
        default: return a;
        }
    }
    switch (a) {
    case 0xF6: return 0xF4;
    case 0xFA: case 0xFB: case 0xFC: case 0xFD: case 0xFE: case 0xFF:
        return 0xF9;
    default: return a;
    }
}

/* $E40D: the class handlers for pattern bytes $F3-$FF. */
static int class_dispatch(const tk_image *img, const tk_buf *buf, uint8_t code,
                          int *x, int syl, int end, int step, int limit,
                          int run_limit)
{
    int i;
    switch (code) {
    case 0xF3: return h_f3(img, buf, x, step, limit);
    case 0xF4: return (attr(img, buf, *x) & A_CONS) != 0;
    case 0xF5: return (attr(img, buf, *x) & A_F5) != 0;
    case 0xF6: return (attr(img, buf, *x) & A_F6) != 0;
    case 0xF7: return h_run(img, buf, x, step, run_limit, A_VOWEL, 1,
                            !img->p.v13_metas);
    case 0xF8: return h_run(img, buf, x, step, run_limit, A_CONS, 0, 1);
    case 0xF9:
        if (img->p.v13_metas) return h_f9_v13(img, buf, x, step, limit);
        return h_f9(img, buf, x, end, step, limit);
    case 0xFA:
        if (img->p.v13_metas) return h_fa_v13(img, buf, x);
        return h_fa(img, buf, x, end);
    case 0xFB: return h_fb(img, buf, x);
    case 0xFC: return h_fc(buf, x, step, limit);
    case 0xFD:
        for (i = 0; i < 7; i++) if (bat(buf, *x) == STOPS[i]) return 1;
        return 0;
    case 0xFE: *x -= step; return syl != 1;    /* zero width: $E3A2/$E3F9 */
    case 0xFF: *x -= step; return syl >= 3;
    default: return 0;
    }
}

/* ------------------------------------------------------------- matcher */

/* $E354 (backward) and $E3A9 (forward), which are one routine with the
 * direction patched into it by $E400. */
static int match_ctx(const tk_image *img, uint16_t pat, int patlen, int y,
                     const tk_buf *buf, int x, int syl, int end, int back)
{
    int step = back ? -1 : 1;
    int limit = back ? -1 : end;
    /* Where a greedy run stops. 3.1.3 copies $FD9E into $FD99 ($E3EF)
     * so its runs stop where the text does; v1.3's $DC90 and $DD19 both
     * do CPX #$8F, so its runs walk on into the leftovers. */
    int run_limit = (back || img->p.fwd_run_limit < 0)
                  ? limit : img->p.fwd_run_limit;

    for (;;) {
        uint8_t a, b;
        x += step;
        y += step;
        a = (y >= 0 && y < patlen) ? tk_peek(img, (uint16_t)(pat + y)) : TERM;
        g_s1_cycles += TK_S1_PATBYTE;
        if (a < 0x80) return 1;                 /* ran into the payload */
        if (x == limit) {
            /* $E35C / $E3B1: off the end. Only $F3 and $F8 may remain. */
            while (y >= 0 && y < patlen) {
                uint8_t p = tk_peek(img, (uint16_t)(pat + y));
                if (p != 0xF3 && p != 0xF8) break;
                y += step;
            }
            b = (y >= 0 && y < patlen) ? tk_peek(img, (uint16_t)(pat + y)) : TERM;
            return b < 0x80;
        }
        if (a >= 0xF3) {
            if (img->p.v13_metas) a = v13_remap(a, back);
            g_s1_cycles += TK_S1_CLASS;
            if (!class_dispatch(img, buf, a, &x, syl, end, step, limit,
                                run_limit))
                return 0;
            continue;
        }
        if (a == 0x9C && !img->p.v13_metas) continue;   /* $E378 / $E3CE */
        b = (a == 0x9B && !img->p.v13_metas) ? 0xA6 : a;/* $E380 / $E3D6 */
        if ((uint8_t)(b + 0x40) != bat(buf, x)) return 0;
    }
}

/* $E2F0. Returns the new cursor, or -1.
 *
 * `end` is $FD9E. The CONTEXT matchers stop there ($E35C and $E3B1 both
 * test it), but the literal run at $E305-$E310 does not -- it compares
 * $FDAC,X with no limit at all. That asymmetry is what lets a rule
 * reach past the end of the text into whatever the previous utterance
 * left in the buffer, consume it, and step the cursor over $FD9E so
 * that $E247's equality test never fires. */
static int match_rule(const tk_image *img, uint16_t pat, int patlen,
                      const tk_buf *buf, int cursor, int syl, int end)
{
    int anchor = 0, x, y;

    while (anchor < patlen) {
        uint8_t a = tk_peek(img, (uint16_t)(pat + anchor));
        g_s1_cycles += TK_S1_ANCHOR;
        if (a >= 0xA0 && a <= 0xEF) break;
        anchor++;
    }
    if (anchor == patlen) return -1;

    x = cursor;
    y = anchor;
    while (y < patlen) {                        /* $E304-$E310 */
        uint8_t a = tk_peek(img, (uint16_t)(pat + y));
        if (a != 0xC0 && !(a >= 0xA0 && a <= 0xEF && a == bat(buf, x))) break;
        x++;
        y++;
    }
    if (y < patlen) {                           /* $E31C: a literal mismatch */
        uint8_t a = tk_peek(img, (uint16_t)(pat + y));
        if (a >= 0xA0 && a <= 0xEF) return -1;
    }
    if (!match_ctx(img, pat, patlen, anchor, buf, cursor, syl, end, 1))
        return -1;
    if (!match_ctx(img, pat, patlen, y - 1, buf, x - 1, syl, end, 0))
        return -1;
    return x;
}

/* ---------------------------------------------------------- stress pass */

/* $E29B-$E2EF. Textalker maps the punctuation to a marker -- $B7 for
 * , ; ? and $B3 for . ! : -- then walks BACKWARD from the end of the
 * phoneme string, shifting each code one place right, until it meets
 * one below $4E. That threshold is the vowel range: $4E is L and
 * everything above it is a consonant. The marker goes in front of that
 * last vowel; if none is found, a $7F lands at index 1, not 0 ($E2E2
 * stores through $FEAD,Y with Y already down to 1).
 *
 * This is what turns MAT. from 51 24 63 into 51 B3 24 63. It runs
 * BEFORE the punctuation mark is matched, which is why DR. carries no
 * marker at all: the abbreviation rule swallows the stop, so the cursor
 * never reaches it. */
static int stress_pass(uint8_t *out, int n, int cap, char punct)
{
    uint8_t marker;
    int y;
    if (strchr(",;?", punct)) marker = 0xB7;
    else if (strchr(".!:", punct)) marker = 0xB3;
    else return n;
    if (!n || n + 1 > cap) return n;

    out[n] = 0;
    y = n + 1;                                  /* $E2CE INY */
    for (;;) {
        uint8_t a = out[y - 2];                 /* $E2D2: $FEAB,Y */
        out[y - 1] = a;
        if (a < 0x4E) {                         /* $E2D8 */
            out[y - 2] = marker;                /* $E2E9 */
            return n + 1;
        }
        y--;
        if (y < 2) break;                       /* $E2DE CMP #$02 */
    }
    out[y] = 0x7F;                              /* $E2E2 */
    return n + 1;
}

/* --------------------------------------------------------- the main loop */

static int is_alnum_code(uint8_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
}

/* $E1FA over buf[start..stop) only, appending codes at out[n].
 *
 * This is the seam. Textalker's rules are now one letter-to-sound
 * source among several, and a source that knows only SOME words -- a
 * dictionary -- has to be able to hand the rest back. So the walk is
 * expressed as a range, with everything OUTSIDE the range still
 * visible as context (the machine's own contexts already read the
 * whole buffer, bounded by $FD9E), and the codes append rather than
 * start a fresh string.
 *
 * Appending is not a convenience. The stress pass at $E29B walks
 * BACKWARD over the whole phoneme string built so far to place its
 * marker in front of the last vowel, so it has to be able to see codes
 * another source produced.
 *
 * `*end_cursor`, when given, receives where the walk actually stopped,
 * which is NOT always `stop`: $E247 tests the cursor for EQUALITY, so
 * a rule consuming more characters than the range holds steps straight
 * over the end and carries on. Reproducing that is deliberate -- it is
 * audible, and HANDOFF records the case -- but a caller splitting the
 * buffer between two sources has to know where to resume.
 *
 * Returns the new `n`, or -1 if some character had no rule at all.
 */
int tk_run_range(const tk_image *img, const tk_buf *buf, uint8_t flags,
                 int start, int stop, uint8_t *out, int n, int outcap,
                 int *end_cursor)
{
    int cursor = start, syl = 0;
    int ctx_end = buf->end;
    int i;

    /* $FD93 belongs to the CURSOR, not to the position being matched,
     * and $E266 clears it at every non-alphanumeric. Starting part way
     * along the buffer therefore means reconstructing it. This replays
     * the same two rules over the characters already passed, which is
     * exact whenever the range begins just after a non-alphanumeric --
     * where a caller splitting on word boundaries always does -- and an
     * approximation only if one begins in the middle of a word. */
    for (i = 0; i < start && i < buf->len; i++) {
        uint8_t c = (uint8_t)(buf->b[i] & 0x7F);
        if (c == (SENT & 0x7F)) syl++;
        else if (!is_alnum_code(c)) syl = 0;
    }

    /* $E247 compares the cursor with $FD9E for EQUALITY, so a rule that
     * consumes more characters than are left steps straight over the
     * end and the walk carries on through whatever the previous
     * utterance left in the buffer -- until $E243 stops it at 240
     * phonemes. That is not a fault to be guarded against; it is
     * audible, and reproducing it is the point. */
    /* $E243's CPX #$F0 in terms of whatever buffer is in force. */
    int phoneme_cap = buf->size ? TK_CAP_OF(buf->size) : TK_PHONEME_CAP;

    while (cursor != stop && n < phoneme_cap) {
        uint8_t c, ch;
        uint32_t sec_start, sec_stop;
        int limit, skip, have = 0, fired = 0;
        rule_iter it;
        uint16_t pat, pho;
        int patlen, pholen;

        if (cursor >= buf->len || cursor < 0) break;
        c = (uint8_t)(buf->b[cursor] & 0x7F);
        if (c == (SENT & 0x7F)) {               /* $E205: count and step */
            cursor++;
            syl++;
            continue;
        }
        ch = c;
        if (!is_alnum_code(ch)) syl = 0;        /* $E266 */

        if (strchr(",;?.!:", (char)ch) && ch && !(flags & 0x60))
            n = stress_pass(out, n, outcap, (char)ch);   /* $E29B */

        limit = -1;
        skip = 0;
        if (img->sec_ok[ch]) {
            sec_start = img->sec_start[ch];
            sec_stop = img->sec_end[ch];
            have = 1;
        } else if (dispatch_at(img, c, &sec_start, &sec_stop)) {
            have = 1;
            limit = 256;
            skip = 1;
        }
        if ((flags & 0x30) && !is_alnum_code(ch)) {
            uint32_t as, ae;
            /* $E27C compares the table entry with the MASKED
             * character, not the high-bit-set one. */
            if (punct_section(img, ch,
                              (flags & 0x20) ? img->p.punct_all
                                             : img->p.punct_most, &as, &ae)) {
                sec_start = as;
                sec_stop = ae;
                have = 1;
                limit = 6;
                skip = 1;
            }
        }
        if (!have) { cursor++; continue; }

        g_s1_cycles += TK_S1_CHAR;
        rules_begin(&it, img, sec_start, sec_stop, limit, skip);
        while (rules_next(&it, &pat, &patlen, &pho, &pholen)) {
            g_s1_cycles += TK_S1_RULE;
            int nxt = match_rule(img, pat, patlen, buf, cursor, syl, ctx_end);
            int i2;
            if (nxt < 0) continue;
            for (i2 = 0; i2 < pholen && n < outcap; i2++)
                out[n++] = tk_peek(img, (uint16_t)(pho + i2));
            cursor = nxt > cursor ? nxt : cursor + 1;
            fired = 1;
            break;
        }
        if (!fired) {
            if (end_cursor) *end_cursor = cursor;
            return -1;
        }
    }
    if (end_cursor) *end_cursor = cursor;
    return n;
}

int tk_run_buffer(const tk_image *img, const tk_buf *buf, uint8_t flags,
                  uint8_t *out, int outcap)
{
    return tk_run_range(img, buf, flags, 0, buf->end, out, 0, outcap, NULL);
}

/* The code the rules emit for a space, read from the image the same way
 * everything else is: $19 in v3.1.3, $7F in v1.3. A source that builds
 * its own phoneme strings needs it and must not assume either. */
uint8_t tk_word_separator(const tk_image *img)
{
    tk_buf b;
    uint8_t out[8];
    int n;
    memset(&b, 0, sizeof b);
    b.b[0] = 0xA0;                              /* a space, high bit set */
    b.len = TK_BUFSZ;
    b.size = TK_BUFSZ;
    b.end = 1;
    n = tk_run_range(img, &b, 0x09, 0, 1, out, 0, (int)sizeof out, NULL);
    return n > 0 ? out[0] : 0x7F;
}

/* Textalker's own rules as a tk_g2p source: the default, and what any
 * other source falls back to. */
static int textalker_run(void *ctx, const tk_image *img, const tk_buf *buf,
                         uint8_t flags, uint8_t *out, int outcap)
{
    (void)ctx;
    return tk_run_range(img, buf, flags, 0, buf->end, out, 0, outcap, NULL);
}

const tk_g2p tk_g2p_textalker = { textalker_run, NULL, "textalker" };

int tk_walk_line_g2p(const tk_image *img, const tk_g2p *g, const char *text,
                     uint8_t flags, uint8_t *out, int outcap)
{
    tk_buf buf;
    int n;
    if (!g) g = &tk_g2p_textalker;
    tk_build_line(img, text, &buf);
    n = g->run(g->ctx, img, &buf, flags, out, outcap);
    if (n < 0) return -1;
    /* tk_walk_line is given text without its carriage return, so nothing
     * in the buffer emits the $1A terminator and it has to be supplied.
     * tk_run_buffer is given the real buffer, where the CR's own rule
     * ({C0} => $1A, through the dispatch entry for $0D) already does. */
    if (n < outcap) out[n++] = 0x1A;
    return n;
}

int tk_walk_line(const tk_image *img, const char *text, uint8_t flags,
                 uint8_t *out, int outcap)
{
    return tk_walk_line_g2p(img, &tk_g2p_textalker, text, flags, out, outcap);
}
