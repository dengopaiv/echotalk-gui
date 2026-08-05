/*
 * text_prep.c -- see text_prep.h for what this does and why.
 */
#include "text_prep.h"

#include <string.h>

/* Echo/Textalker control codes that must survive: Ctrl-E introduces a
 * command, Ctrl-V switches to phoneme mode. */
#define CTRL_E 0x05
#define CTRL_V 0x16

void echotalk_prep_defaults(echotalk_prep_opts *opts) {
    if (!opts) return;
    opts->unmapped = ' ';
}

/* --- Windows-1252 $80-$9F -> Unicode -------------------------------
 * The rest of $A0-$FF is identical to Latin-1, i.e. codepoint == byte,
 * so only this window needs a table. Entries that are unassigned in
 * CP1252 map to 0, which the caller treats as unmapped. */
static const uint16_t cp1252_high[32] = {
    0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
    0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178
};

/* --- Latin-1 supplement, U+00A0 - U+00FF --------------------------- */
static const char *const latin1_map[96] = {
    /* A0 */ " ",  "!",  " cents", " pounds", " currency", " yen", "|", " section",
    /* A8 */ "",   " copyright", "a", "\"", " not", "", " registered", "",
    /* B0 */ " degrees", " plus or minus", " squared", " cubed", "'", " micro", " paragraph", ".",
    /* B8 */ "",   "1",  "o",  "\"", " 1/4", " 1/2", " 3/4", "?",
    /* C0 */ "A",  "A",  "A",  "A",  "A",  "A",  "AE", "C",
    /* C8 */ "E",  "E",  "E",  "E",  "I",  "I",  "I",  "I",
    /* D0 */ "D",  "N",  "O",  "O",  "O",  "O",  "O",  " times ",
    /* D8 */ "O",  "U",  "U",  "U",  "U",  "Y",  "TH", "ss",
    /* E0 */ "a",  "a",  "a",  "a",  "a",  "a",  "ae", "c",
    /* E8 */ "e",  "e",  "e",  "e",  "i",  "i",  "i",  "i",
    /* F0 */ "d",  "n",  "o",  "o",  "o",  "o",  "o",  " divided by ",
    /* F8 */ "o",  "u",  "u",  "u",  "u",  "y",  "th", "y"
};

/* --- Latin Extended-A, U+0100 - U+017F -----------------------------
 * Accents dropped, digraphs spelled out. Covers most Central and
 * Eastern European Latin-script text. */
static const char *const latin_ext_a_map[128] = {
    /* 100 */ "A","a","A","a","A","a","C","c","C","c","C","c","C","c","D","d",
    /* 110 */ "D","d","E","e","E","e","E","e","E","e","E","e","G","g","G","g",
    /* 120 */ "G","g","G","g","H","h","H","h","I","i","I","i","I","i","I","i",
    /* 130 */ "I","i","IJ","ij","J","j","K","k","k","L","l","L","l","L","l","L",
    /* 140 */ "l","L","l","N","n","N","n","N","n","'n","NG","ng","O","o","O","o",
    /* 150 */ "O","o","OE","oe","R","r","R","r","R","r","S","s","S","s","S","s",
    /* 160 */ "S","s","T","t","T","t","T","t","U","u","U","u","U","u","U","u",
    /* 170 */ "U","u","U","u","W","w","Y","y","Y","Z","z","Z","z","Z","z","s"
};

/* Punctuation and symbols outside the two table ranges. */
static const char *map_misc(uint32_t cp) {
    switch (cp) {
    case 0x0192: return "f";      /* latin small letter f with hook */
    case 0x02C6: return "";       /* modifier circumflex */
    case 0x02DC: return "~";
    case 0x2010: case 0x2011: return "-";
    case 0x2012: case 0x2013: return "-";
    case 0x2014: case 0x2015: return "--";
    case 0x2018: case 0x2019: case 0x201B: return "'";
    case 0x201A: return ",";
    case 0x201C: case 0x201D: case 0x201E: return "\"";
    case 0x2020: return " dagger";
    case 0x2021: return " double dagger";
    case 0x2022: case 0x2023: case 0x25CF: case 0x25AA: return "*";
    case 0x2026: return "...";
    case 0x2030: return " per mille";
    case 0x2032: return "'";
    case 0x2033: return "\"";
    case 0x2039: return "<";
    case 0x203A: return ">";
    case 0x2044: return "/";
    case 0x20AC: return " euros";
    case 0x2122: return " trademark";
    case 0x2190: return " left arrow ";
    case 0x2192: return " to ";
    case 0x2194: return " to ";
    case 0x2212: return "-";
    case 0x2260: return " not equal to ";
    case 0x2264: return " less than or equal to ";
    case 0x2265: return " greater than or equal to ";
    case 0x00A0: return " ";
    case 0x2007: case 0x2008: case 0x2009: case 0x200A:
    case 0x2002: case 0x2003: case 0x2004: case 0x2005: case 0x2006:
        return " ";
    case 0x200B: case 0x200C: case 0x200D: case 0xFEFF:
        return "";                /* zero-width and BOM: silently gone */
    default: return NULL;
    }
}

/* Decodes one character at in[*i], advancing *i past it. Returns the
 * Unicode codepoint. Invalid UTF-8 is not an error: the lead byte is
 * reinterpreted as Windows-1252, which is what real-world mixed or
 * legacy input actually needs. */
static uint32_t decode_one(const uint8_t *in, size_t len, size_t *i) {
    uint8_t b = in[*i];
    if (b < 0x80) { (*i)++; return b; }

    int need = 0;
    uint32_t cp = 0;
    if ((b & 0xE0) == 0xC0) { need = 1; cp = b & 0x1F; }
    else if ((b & 0xF0) == 0xE0) { need = 2; cp = b & 0x0F; }
    else if ((b & 0xF8) == 0xF0) { need = 3; cp = b & 0x07; }

    if (need && *i + need < len) {
        int ok = 1;
        uint32_t acc = cp;
        for (int k = 1; k <= need; k++) {
            uint8_t c = in[*i + k];
            if ((c & 0xC0) != 0x80) { ok = 0; break; }
            acc = (acc << 6) | (c & 0x3F);
        }
        /* Reject overlong forms and out-of-range results, which would
         * otherwise let the same character have several encodings. */
        if (ok) {
            uint32_t min_cp = need == 1 ? 0x80 : need == 2 ? 0x800 : 0x10000;
            if (acc >= min_cp && acc <= 0x10FFFF) {
                *i += need + 1;
                return acc;
            }
        }
    }

    /* Not valid UTF-8: treat as Windows-1252. */
    (*i)++;
    if (b >= 0x80 && b <= 0x9F) {
        uint16_t mapped = cp1252_high[b - 0x80];
        return mapped ? mapped : 0xFFFD;
    }
    return b; /* $A0-$FF: Latin-1, codepoint == byte */
}

/* Renders one codepoint as ASCII. Returns NULL if there is no sensible
 * rendering (caller applies the unmapped policy). `scratch` holds
 * single-character results. */
static const char *render(uint32_t cp, char *scratch) {
    if (cp == '\r' || cp == CTRL_E || cp == CTRL_V) {
        scratch[0] = (char)cp; scratch[1] = '\0';
        return scratch;
    }
    if (cp == '\n') return "";           /* Apple II lines end with CR */
    if (cp == '\t') return " ";
    if (cp < 0x20 || cp == 0x7F) return "";   /* other C0 controls, DEL */
    if (cp < 0x7F) {                     /* printable ASCII, as-is */
        scratch[0] = (char)cp; scratch[1] = '\0';
        return scratch;
    }
    if (cp >= 0xA0 && cp <= 0xFF) return latin1_map[cp - 0xA0];
    if (cp >= 0x100 && cp <= 0x17F) return latin_ext_a_map[cp - 0x100];
    return map_misc(cp);
}

size_t echotalk_prep_text(const uint8_t *in, size_t in_len,
                          char *out, size_t out_cap,
                          const echotalk_prep_opts *opts) {
    echotalk_prep_opts defaults;
    if (!opts) { echotalk_prep_defaults(&defaults); opts = &defaults; }

    size_t written = 0, needed = 0;
    char scratch[2];

    for (size_t i = 0; i < in_len; ) {
        uint32_t cp = decode_one(in, in_len, &i);
        const char *rep = render(cp, scratch);
        char fallback[2];
        if (!rep) {
            if (opts->unmapped == 0) continue;
            fallback[0] = opts->unmapped; fallback[1] = '\0';
            rep = fallback;
        }
        for (const char *p = rep; *p; p++) {
            if (out && written + 1 < out_cap) out[written++] = *p;
            needed++;
        }
    }

    if (out && out_cap > 0) out[written < out_cap ? written : out_cap - 1] = '\0';
    return needed;
}
