/*
 * tk_chip.c -- both stages of the native Textalker, on the command line.
 *
 * Text (or a Ctrl-V phoneme string, or a raw phoneme code list) in, and
 * out comes the exact byte stream Textalker writes to the Echo II's
 * chip port. That is checkpoint 3, the end of the pipeline, and what
 * the emulator exposes through ECHOTALK_BYTE_DUMP.
 *
 *   tk_chip <loader.bin> <obj.bin> [options]
 *     --version V     3.1.3 or 1.3; omit to detect from the image
 *     --flags N       $FFAD, decimal or 0x hex (default 0x09)
 *     --lines FILE    one line of TEXT per line of FILE
 *     --phonemes FILE one Ctrl-V phoneme string per line
 *     --codes FILE    one line of hex phoneme codes per line
 *     --raw FILE      one python-escaped raw input stream per line,
 *                     driven through the whole machine (tk_machine.c)
 *     --dict FILE     a CMUdict-format pronunciation dictionary, put in
 *                     at the seam; applies to --lines and --raw
 *
 * In every mode but --raw each input line is spoken by a FRESH voice
 * with a ZEROED phoneme buffer, which is what tools/allophone.py does
 * when it is called without a machine. --raw instead builds a whole
 * machine per line, with both buffers initialised from the image the
 * way the real one is, and sends it Ctrl-E 99 R first -- exactly what
 * the emulator harness does during init, so the two start level.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tk.h"

#define OUTCAP (1 << 20)

static uint8_t g_out[OUTCAP];
static uint8_t g_phbuf[TK_BUFSZ];

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The python-escaped byte strings the differential tests pass around:
 * \xNN, \r, \n, \t, \0 and \\. Anything else after a backslash is
 * taken literally. */
static int unescape(const char *in, uint8_t *out, int cap)
{
    int n = 0;
    while (*in && n < cap) {
        if (*in != 92) { out[n++] = (uint8_t)*in++; continue; }
        in++;
        switch (*in) {
        case 'x': {
            int hi = hexval(in[1]), lo = hi < 0 ? -1 : hexval(in[2]);
            if (lo < 0) { out[n++] = 'x'; in++; break; }
            out[n++] = (uint8_t)(hi * 16 + lo);
            in += 3;
            break;
        }
        case 'r': out[n++] = 13; in++; break;
        case 'n': out[n++] = 10; in++; break;
        case 't': out[n++] = 9;  in++; break;
        case '0': out[n++] = 0;  in++; break;
        case 0:   out[n++] = 92; break;
        default:  out[n++] = (uint8_t)*in++; break;
        }
    }
    return n;
}

static void show(const tk_chip *chip)
{
    int i, n = chip->n < chip->cap ? chip->n : chip->cap;
    for (i = 0; i < n; i++) printf("%s%02x", i ? " " : "", chip->out[i]);
    putchar('\n');
}

int main(int argc, char **argv)
{
    static tk_image img;
    const char *pos[2] = { NULL, NULL };
    const char *version = NULL, *path = NULL, *dictpath = NULL;
    tk_dict *dict = NULL;
    const tk_g2p *g = &tk_g2p_textalker;
    enum { M_LINES, M_PHONEMES, M_CODES, M_RAW } mode = M_LINES;
    char err[256], line[4096];
    uint8_t flags = 0x09;
    int i, npos = 0;
    FILE *f;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version") && i + 1 < argc) version = argv[++i];
        else if (!strcmp(argv[i], "--flags") && i + 1 < argc)
            flags = (uint8_t)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--lines") && i + 1 < argc)
            { mode = M_LINES; path = argv[++i]; }
        else if (!strcmp(argv[i], "--phonemes") && i + 1 < argc)
            { mode = M_PHONEMES; path = argv[++i]; }
        else if (!strcmp(argv[i], "--codes") && i + 1 < argc)
            { mode = M_CODES; path = argv[++i]; }
        else if (!strcmp(argv[i], "--raw") && i + 1 < argc)
            { mode = M_RAW; path = argv[++i]; }
        else if (!strcmp(argv[i], "--dict") && i + 1 < argc)
            dictpath = argv[++i];
        else if (npos < 2) pos[npos++] = argv[i];
    }
    if (npos < 2 || !path) {
        fprintf(stderr, "usage: tk_chip <loader.bin> <obj.bin> "
                "(--lines|--phonemes|--codes|--raw) FILE [--version V]\n");
        return 2;
    }
    if (tk_image_load(&img, pos[1], pos[0], version, err, sizeof err) < 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (dictpath) {
        dict = tk_dict_open(&img, dictpath, err, sizeof err);
        if (!dict) { fprintf(stderr, "%s\n", err); return 1; }
        g = tk_dict_g2p(dict);
        fprintf(stderr, "%s: %ld entries\n", dictpath,
                tk_dict_entries(dict));
    }

    f = !strcmp(path, "-") ? stdin : fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    while (fgets(line, sizeof line, f)) {
        tk_voice v;
        tk_chip chip;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;

        tk_voice_init(&img, &v);
        v.flags = flags;
        tk_chip_init(&chip, g_out, OUTCAP);
        memset(g_phbuf, 0, sizeof g_phbuf);

        if (mode == M_RAW) {
            /* The whole machine, one per line, sent the same
             * repeat-filter disable the harness sends during init. */
            static tk_machine m;
            uint8_t raw[8192];
            int nr = unescape(line, raw, (int)sizeof raw);
            tk_machine_init(&m, &img, g_out, OUTCAP);
            tk_machine_set_g2p(&m, g);
            tk_machine_feed(&m, (const uint8_t *)"\x05" "99R", 4);
            tk_machine_feed(&m, raw, nr);
            if (m.rule_failures)
                fprintf(stderr, "warning: %ld character(s) found no rule\n",
                        m.rule_failures);
            if (m.chip.n > m.chip.cap) { printf("OVERFLOW\n"); continue; }
            show(&m.chip);
            continue;
        }
        if (mode == M_PHONEMES) {
            tk_speak_phonemes(&img, &v, &chip, line);
        } else if (mode == M_CODES) {
            uint8_t codes[TK_BUFSZ];
            int nc = 0;
            char *p = line;
            while (nc < TK_BUFSZ) {
                char *e;
                long val = strtol(p, &e, 16);
                if (e == p) break;
                codes[nc++] = (uint8_t)val;
                p = e;
            }
            tk_speak_codes(&img, &v, &chip, codes, nc, g_phbuf, TK_BUFSZ);
        } else {
            uint8_t codes[TK_BUFSZ + 16];
            int nc = tk_walk_line_g2p(&img, g, line, flags, codes,
                                      (int)sizeof codes);
            if (nc < 0) { printf("NORULE\n"); continue; }
            tk_speak_codes(&img, &v, &chip, codes, nc, g_phbuf, TK_BUFSZ);
        }
        if (chip.n > chip.cap) { printf("OVERFLOW\n"); continue; }
        show(&chip);
    }
    if (f != stdin) fclose(f);
    tk_dict_free(dict);
    return 0;
}
