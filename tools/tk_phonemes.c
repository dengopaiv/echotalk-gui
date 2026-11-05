/*
 * tk_phonemes.c -- stage 1 of the native Textalker, on the command line.
 *
 * Text in, the phoneme codes Textalker would build out, as hex. That is
 * checkpoint 1 of the three differential checkpoints, and the one the
 * emulator exposes through ECHOTALK_PHONEME_DUMP -- so this is directly
 * comparable with the real thing.
 *
 *   tk_phonemes <loader.bin> <obj.bin> [options] [TEXT ...]
 *     --version V   3.1.3 or 1.3; omit to detect from the image
 *     --flags N     $FFAD, decimal or 0x hex (default 0x09)
 *     --lines FILE  one line of text per line of FILE; - is stdin
 *     --buffer      print the syllabified input buffer instead
 *     --dict FILE   speak words from a CMUdict-format dictionary and
 *                   leave the rest to Textalker's rules -- the seam
 *     --arpabet     print ARPABET names instead of hex codes
 *
 * Reading a whole file in one process matters: the differential test
 * runs thousands of lines, and starting a process per line would make
 * the C port look slower than the emulator it replaces.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tk.h"

static void emit(const tk_image *img, const tk_g2p *g, const char *text,
                 uint8_t flags, int show_buffer, int arpabet)
{
    uint8_t out[TK_BUFSZ + 16];
    int i, n;

    if (show_buffer) {
        tk_buf buf;
        tk_build_line(img, text, &buf);
        for (i = 0; i < buf.end; i++)
            printf("%s%02x", i ? " " : "", buf.b[i]);
        putchar('\n');
        return;
    }
    n = tk_walk_line_g2p(img, g, text, flags, out, (int)sizeof out);
    if (n < 0) {
        printf("NORULE\n");
        return;
    }
    if (arpabet) {
        char names[1024];
        tk_codes_to_arpabet(out, n, names, sizeof names);
        printf("%s\n", names);
        return;
    }
    for (i = 0; i < n; i++) printf("%s%02x", i ? " " : "", out[i]);
    putchar('\n');
}

int main(int argc, char **argv)
{
    static tk_image img;                 /* 64K; not on the stack */
    const char *pos[2] = { NULL, NULL };
    const char *version = NULL, *lines = NULL, *dictpath = NULL;
    char err[256];
    uint8_t flags = 0x09;
    int i, npos = 0, show_buffer = 0, first_text = 0, arpabet = 0;
    tk_dict *dict = NULL;
    const tk_g2p *g = &tk_g2p_textalker;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version") && i + 1 < argc) version = argv[++i];
        else if (!strcmp(argv[i], "--flags") && i + 1 < argc)
            flags = (uint8_t)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--lines") && i + 1 < argc) lines = argv[++i];
        else if (!strcmp(argv[i], "--buffer")) show_buffer = 1;
        else if (!strcmp(argv[i], "--dict") && i + 1 < argc)
            dictpath = argv[++i];
        else if (!strcmp(argv[i], "--arpabet")) arpabet = 1;
        else if (npos < 2) pos[npos++] = argv[i];
        else { first_text = i; break; }
    }
    if (npos < 2) {
        fprintf(stderr,
                "usage: tk_phonemes <loader.bin> <obj.bin> [options] [TEXT ...]\n");
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

    if (lines) {
        char buf[4096];
        FILE *f = !strcmp(lines, "-") ? stdin : fopen(lines, "rb");
        if (!f) { perror(lines); return 1; }
        while (fgets(buf, sizeof buf, f)) {
            size_t n = strlen(buf);
            while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
            emit(&img, g, buf, flags, show_buffer, arpabet);
        }
        if (f != stdin) fclose(f);
        tk_dict_free(dict);
        return 0;
    }
    if (!first_text) {
        fprintf(stderr, "give --lines FILE or some text\n");
        return 2;
    }
    for (i = first_text; i < argc; i++)
        emit(&img, g, argv[i], flags, show_buffer, arpabet);
    tk_dict_free(dict);
    return 0;
}
