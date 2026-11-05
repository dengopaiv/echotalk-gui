/*
 * tk_arpabet.c -- ARPABET <-> Textalker phoneme codes.
 *
 * Stage 2 consumes one-byte phoneme codes and nothing else, so the only
 * thing standing between Textalker's synthesiser and somebody else's
 * letter-to-sound work is a translation of alphabets. This is it.
 *
 * NOTHING HERE IS STREET ELECTRONICS' DATA. The allophone frames, the
 * rules and the character map all still come out of the user's own
 * image at run time, exactly as before; what is written down here is
 * the CORRESPONDENCE -- which of Textalker's allophones is the vowel of
 * "beet" -- and that is our own measurement, not their table.
 *
 * IT WAS MEASURED WITH FOUR FRAME WORDS PER VOWEL, and the reason is a
 * mistake worth keeping in view. The first pass used ONE word each, and
 * picked COT for $0B and BOUGHT for $01 -- the cot/caught minimal pair,
 * where a 1980s speech card is exactly as likely to merge as a speaker
 * is, and where BOUGHT happens to be a word Textalker gets wrong (it
 * really does say "baht"). That put $01 and $0B the wrong way round,
 * and Jayson caught it by ear. Four frames settle it in one run:
 *
 *   LAW SAW TALK AWFUL CAUGHT LAWN FAWN CHALK -> all $0B, so AO
 *   FATHER -> $01, and ARM CAR FAR HARD -> $11, which is $01 + R.
 *
 * The rest held up unchanged under the same treatment:
 *
 *   BEET SEE MEAT FIELD    IY      BOOK PUT FOOT GOOD     UH
 *   BAIT DAY MADE RAIN     EY      BITE MY TIME HIGH      AY
 *   BOAT GO SHOW ROAD      OW      BOUT NOW HOUSE LOUD    AW
 *   CUTE FEW MUSIC HUMAN   YU      BAT CAT HAND MAP       AE
 *   BET RED HEAD SAID      EH      BIT SIT MILK GIVE      IH
 *   TOO BLUE SOUP          UW      BUT CUP LOVE           AH
 *   BOY TOY NOISE JOIN     OY      BIRD HERD WORD TURN    ER
 *
 * (BOOT is not in that UW list on purpose: Textalker says it with the
 * vowel of BOOK. That is the rule bug the "=booht" respelling in
 * tk_dict.c exists to work around.)
 *
 * notes/phoneme_alphabet.md has the whole derivation.
 *
 * THE ENCODING, from $DC22-$DC92 as ported in tk_synth.c:
 *
 *   allophone index < $1A   three codes, one per pitch group:
 *                           idx (group 1), idx+$1A (2), idx+$34 (3)
 *   allophone index >= $1A  one code, idx+$34
 *   $7F                     silence; tk_word_separator() gives the
 *                           between-words code ($19 or $7F by version)
 *   $B3 / $B7               the sentence-final and comma intonation
 *   $1A                     end of utterance
 *
 * The pitch group IS the stress: $EA7F offsets the running pitch by
 * -2, 0 and +2 for groups 1, 2 and 3. So an ARPABET stress digit maps
 * straight onto it -- 1 and 2 take group 3, 0 takes group 2 -- and
 * group 1 is the extra-reduced one Textalker itself uses for the
 * syllabic vowel of BOTTLE.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "tk.h"

/* The vowels, allophone index by ARPABET symbol. */
static const struct { char sym[4]; uint8_t idx; } VOWELS[] = {
    { "AA", 0x01 },   /* father, and + R it is the vowel of CAR */
    { "IY", 0x02 },   /* beet   */
    { "UH", 0x03 },   /* book   */
    { "AX", 0x04 },   /* the off-glide of POOL; with R it is ER */
    { "EY", 0x05 },   /* bait   */
    { "AY", 0x06 },   /* bite   */
    { "OW", 0x07 },   /* boat   */
    { "AW", 0x08 },   /* bout   */
    { "YU", 0x09 },   /* cute, few -- one allophone, not Y + UW */
    { "AE", 0x0A },   /* bat    */
    { "AO", 0x0B },   /* law, talk, caught */
    { "EH", 0x0C },   /* bet    */
    { "IH", 0x0D },   /* bit    */
    { "UW", 0x0E },   /* boot   */
    { "AH", 0x0F },   /* but -- and the schwa: Textalker uses it for both */
    { "OY", 0x10 },   /* boy    */
    { "ER", 0x14 },   /* bird -- AX + R */
    { "AAR", 0x11 },  /* car   -- the rest of the R-coloured set, named  */
    { "IYR", 0x12 },  /* here     so a caller can reach one on purpose.  */
    { "UHR", 0x13 },  /* poor     R_FORM below reaches them from a plain */
    { "EYR", 0x15 },  /* hair     vowel followed by R, which is how a    */
    { "AYR", 0x16 },  /* fire     dictionary entry will spell them.      */
    { "OWR", 0x17 },  /* more   */
    { "AWR", 0x18 }   /* hour   */
};
#define N_VOWELS ((int)(sizeof VOWELS / sizeof VOWELS[0]))

static const struct { char sym[4]; uint8_t idx; } CONSONANTS[] = {
    { "L",  0x1A }, { "R",  0x1B }, { "W",  0x1C }, { "M",  0x1D },
    { "N",  0x1E }, { "NG", 0x1F }, { "DH", 0x20 }, { "V",  0x21 },
    { "Y",  0x22 }, { "Z",  0x23 }, { "ZH", 0x24 }, { "F",  0x25 },
    { "HH", 0x26 }, { "S",  0x27 }, { "SH", 0x28 }, { "TH", 0x29 },
    { "B",  0x2A }, { "D",  0x2B }, { "G",  0x2C }, { "K",  0x2D },
    { "P",  0x2E }, { "T",  0x2F }, { "CH", 0x30 }, { "JH", 0x31 }
};
#define N_CONSONANTS ((int)(sizeof CONSONANTS / sizeof CONSONANTS[0]))

/* $23 is never spoken alone by Textalker's own rules. Every /z/ they
 * emit is a PAIR with $27, so $23 is the voiced part and wants the
 * frication beside it; a bare $23 is untested.
 *
 * WHICH WAY ROUND is not phonetic and there is no rule to copy: it
 * follows the SPELLING. The letter Z gives "S Z" (ZERO, ZONE, ZEBRA,
 * LAZY) and an S pronounced /z/ gives "Z S" (IS, WAS, DOGS, PLEASE,
 * NOSE) -- but RESEARCH and SYNTHESIZER break even that. So this emits
 * "Z S" always, which is the commoner of the two; the round trip in
 * tools/seam_diff.py counts the words where Textalker chose the other
 * order, and it is a handful. */
#define IDX_Z  0x23
#define IDX_S  0x27
#define ZCODE  (IDX_Z + 0x34)
#define SCODE  (IDX_S + 0x34)

/* An R after a vowel is not a separate sound here. $E62F adds $10 to
 * allophone indices 1-8 when an R follows, and Textalker's own rules
 * emit the R-COLOURED CODE rather than a vowel and an R: CAR is one
 * code, not two. A dictionary that spells it "K AA1 R" therefore has to
 * be folded the same way, or a word like STARTING comes out with an
 * audible R of its own -- which is what it sounded like before this
 * existed.
 *
 * Two of these are not the index arithmetic. Textalker has no
 * R-coloured form of AO at all ($0B is outside $E62F's 1-8 range), so
 * MORE and FOUR use OW + R; and it uses EY + R for HAIR and CARE where
 * CMUdict writes EH. Both were read off its own output. */
static uint8_t r_form(const char *sym, uint8_t idx)
{
    if (idx >= 0x01 && idx <= 0x08) return (uint8_t)(idx + 0x10);
    if (!strcmp(sym, "EH")) return 0x15;        /* HAIR, CARE  */
    if (!strcmp(sym, "AO")) return 0x17;        /* MORE, FOUR  */
    return 0;
}

uint8_t tk_arpa_code(uint8_t idx, int group)
{
    if (idx >= 0x1A) return (uint8_t)(idx + 0x34);
    if (group < 1) group = 1;
    if (group > 3) group = 3;
    return (uint8_t)(idx + 0x1A * (group - 1));
}

int tk_arpa_lookup(const char *sym, uint8_t *idx, int *is_vowel)
{
    int i;
    for (i = 0; i < N_VOWELS; i++)
        if (!strcmp(sym, VOWELS[i].sym)) {
            *idx = VOWELS[i].idx;
            *is_vowel = 1;
            return 1;
        }
    for (i = 0; i < N_CONSONANTS; i++)
        if (!strcmp(sym, CONSONANTS[i].sym)) {
            *idx = CONSONANTS[i].idx;
            *is_vowel = 0;
            return 1;
        }
    return 0;
}

#define MAX_SYMS 96

typedef struct { char sym[8]; int stress; } asym;

/* Split a pronunciation into symbols, upper-cased, with any CMUdict
 * stress digit taken off. Returns how many. */
static int split(const char *p, asym *out, int cap)
{
    int n = 0;
    while (*p && n < cap) {
        size_t k = 0;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        out[n].stress = -1;
        while (*p && !isspace((unsigned char)*p)) {
            if (isdigit((unsigned char)*p)) out[n].stress = *p - '0';
            else if (k + 1 < sizeof out[n].sym)
                out[n].sym[k++] = (char)toupper((unsigned char)*p);
            p++;
        }
        out[n].sym[k] = '\0';
        if (k) n++;
    }
    return n;
}

int tk_arpabet_to_codes(const char *arpa, uint8_t *out, int cap,
                        char *bad, size_t badsz)
{
    asym s[MAX_SYMS];
    int ns = split(arpa, s, MAX_SYMS);
    int i = 0, n = 0;

    if (bad && badsz) bad[0] = '\0';

    while (i < ns) {
        const char *sym = s[i].sym;
        int stress = s[i].stress, vowel, group;
        uint8_t idx, rf;

        /* Y + UW is Textalker's own $09, and a better sound than the two
         * spoken separately -- it is what its rules emit for CUTE, FEW,
         * USE and UNIT. */
        if (i + 1 < ns && !strcmp(sym, "Y") && !strcmp(s[i + 1].sym, "UW")) {
            sym = "YU";
            if (s[i + 1].stress >= 0) stress = s[i + 1].stress;
            i += 2;
        } else {
            i++;
        }

        if (!tk_arpa_lookup(sym, &idx, &vowel)) {
            if (bad && badsz) snprintf(bad, badsz, "%s", sym);
            return -1;
        }

        if (vowel && (rf = r_form(sym, idx)) != 0 && i < ns) {
            /* A plain R always folds. An ER folds only after AY and AW,
             * which is how CMUdict spells FIRE and HOUR -- one nucleus
             * in Textalker's alphabet. Folding it after EY as well would
             * turn PLAYER into "pair". */
            int fold = !strcmp(s[i].sym, "R")
                    || (!strcmp(s[i].sym, "ER")
                        && (idx == 0x06 || idx == 0x08));
            if (fold) {
                idx = rf;
                i++;
            }
        }

        group = vowel ? (stress > 0 ? 3 : 2) : 2;

        if (idx == IDX_Z) {
            if (n < cap) out[n++] = tk_arpa_code(IDX_Z, 2);
            if (n < cap) out[n++] = tk_arpa_code(IDX_S, 2);
            continue;
        }
        if (n < cap) out[n++] = tk_arpa_code(idx, group);
    }
    return n;
}

/* The other direction, for looking at what any source produced. Codes
 * with no ARPABET name -- the separator, the intonation markers, the
 * terminator -- come out in angle brackets. */
int tk_codes_to_arpabet(const uint8_t *codes, int n, char *out, size_t cap)
{
    size_t used = 0;
    int i;

    if (!cap) return 0;
    out[0] = '\0';
    for (i = 0; i < n; i++) {
        char piece[24];
        uint8_t c = codes[i], idx;
        int group = 0, j, found = 0;

        /* Textalker spells /z/ as a PAIR -- see IDX_Z above -- so read
         * one back as one, or a round trip through here grows an extra
         * S every time. A genuine /z s/ sequence would be misread; no
         * English word has one. */
        if (i + 1 < n
            && ((c == ZCODE && codes[i + 1] == SCODE)
                || (c == SCODE && codes[i + 1] == ZCODE))) {
            snprintf(piece, sizeof piece, "Z");
            i++;
        }
        else if (c == 0x1A) snprintf(piece, sizeof piece, "<end>");
        else if (c == 0x7F) snprintf(piece, sizeof piece, "<sil>");
        /* $19 is the word separator in v3.1.3 and $34 the sentence-final
         * fall; both are allophone $00, the pause, at different pitch
         * groups. ($1A, which would be that pause at group 2, is the
         * terminator instead -- the one collision in the encoding.) */
        else if (c == 0x19) snprintf(piece, sizeof piece, "<sep>");
        else if (c == 0x00) snprintf(piece, sizeof piece, "<pau1>");
        else if (c == 0x34) snprintf(piece, sizeof piece, "<pau3>");
        else if (c >= 0xB3 && c <= 0xBF)
            snprintf(piece, sizeof piece, "<p%d>", c & 0x0F);
        else {
            if (c < 0x1A)      { idx = c;                    group = 1; }
            else if (c < 0x34) { idx = (uint8_t)(c - 0x1A);  group = 2; }
            else if (c < 0x4E) { idx = (uint8_t)(c - 0x34);  group = 3; }
            else               { idx = (uint8_t)(c - 0x34);  group = 0; }
            for (j = 0; j < N_VOWELS && !found; j++)
                if (VOWELS[j].idx == idx) {
                    snprintf(piece, sizeof piece, "%s%d", VOWELS[j].sym, group);
                    found = 1;
                }
            for (j = 0; j < N_CONSONANTS && !found; j++)
                if (CONSONANTS[j].idx == idx) {
                    snprintf(piece, sizeof piece, "%s", CONSONANTS[j].sym);
                    found = 1;
                }
            if (!found) snprintf(piece, sizeof piece, "<%02X>", c);
        }
        if (used + strlen(piece) + 2 >= cap) break;
        if (used) out[used++] = ' ';
        strcpy(out + used, piece);
        used += strlen(piece);
    }
    return (int)used;
}
