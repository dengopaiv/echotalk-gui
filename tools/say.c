/*
 * say.c -- drives the library the way a host application would, and is
 * the end-to-end test that the API works.
 *
 * usage: say [options] <loader.bin> <obj.bin> <text> <out.wav>
 */
#include "echotalk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void wav_write(const char *path, unsigned rate, const int16_t *pcm, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    uint32_t data_bytes = (uint32_t)(n * 2), byte_rate = rate * 2;
    uint16_t block_align = 2, bits = 16, fmt = 1, ch = 1;
    uint32_t fmt_size = 16, riff_size = 36 + data_bytes, r = rate;
    fwrite("RIFF", 1, 4, f); fwrite(&riff_size, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmt_size, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&r, 4, 1, f); fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    fwrite(pcm, 2, n, f);
    fclose(f);
}

int main(int argc, char **argv) {
    unsigned rate = 0;
    double clock_mult = 1.0, speed = 1.0;
    int frame_rate = 0, compressed = 0, pitch = -1, volume = -1;
    int word_delay = -1, repeat_filter = -1;
    int chunk = -1, raw = 0;
    const char *text_file = NULL;
    const char *pos[4]; int npos = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--file") && i + 1 < argc) text_file = argv[++i];
        else if (!strcmp(argv[i], "--word-delay") && i + 1 < argc) word_delay = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--repeat-filter") && i + 1 < argc) repeat_filter = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--clock") && i + 1 < argc) clock_mult = atof(argv[++i]);
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc) speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "--frame-rate") && i + 1 < argc) frame_rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--compressed")) compressed = 1;
        else if (!strcmp(argv[i], "--chunk") && i + 1 < argc) chunk = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-chunk")) chunk = 0;
        else if (!strcmp(argv[i], "--raw")) raw = 1;
        else if (!strcmp(argv[i], "--pitch") && i + 1 < argc) pitch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--volume") && i + 1 < argc) volume = atoi(argv[++i]);
        else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "unknown option %s\n", argv[i]); return 1;
        } else if (npos < 4) pos[npos++] = argv[i];
    }
    /* With --file the text positional is dropped. */
    int want = text_file ? 3 : 4;
    if (npos != want) {
        fprintf(stderr,
            "usage: %s [options] <loader.bin> <obj.bin> [text] <out.wav>\n"
            "  --file PATH        read the text from a file instead of the\n"
            "                     command line (omit the text argument)\n"
            "  --rate HZ          output sample rate (default 8000, native)\n"
            "  --clock MULT       TMS5220 clock multiplier, speed and pitch\n"
            "  --speed MULT       0.25-4.0, speed only, pitch unchanged. This\n"
            "                     is the one you probably want.\n"
            "  --frame-rate N     0-3, the chip's own four fixed steps, speed\n"
            "                     only (1.00x 1.31x 1.89x 3.40x)\n"
            "  --compressed       Textalker compressed speech\n"
            "  --pitch N          0-63 (default 24)\n"
            "  --volume N         0-15 (default 12)\n"
            "  --word-delay N     0-15 pause between words (default 0)\n"
            "  --repeat-filter N  0-99 repeat-character threshold\n"
            "                     (default 99, i.e. effectively off)\n"
            "  --chunk N          split long lines every N characters (default 80)\n"
            "  --no-chunk         never split; see the warning it prints\n"
            "  --raw              send bytes to Textalker untouched\n"
            "\n"
            "The text may also carry Ctrl-D driver commands: 0x04, an optional\n"
            "number, then a letter. 2F frame rate, 0.75C clock, 0B chunking off,\n"
            "1R raw on, 7I an index mark; a bare letter restores that setting's\n"
            "default, and a doubled 0x04 is one literal 0x04. Each command ends\n"
            "the current utterance, so what precedes it speaks at the old\n"
            "settings. Index marks are reported on stderr as they are reached.\n",
            argv[0]);
        return 1;
    }

    char *filetext = NULL;
    if (text_file) {
        FILE *tf = fopen(text_file, "rb");
        if (!tf) { perror(text_file); return 1; }
        fseek(tf, 0, SEEK_END); long len = ftell(tf); fseek(tf, 0, SEEK_SET);
        filetext = malloc((size_t)len + 1);
        if (!filetext || (len && fread(filetext, 1, (size_t)len, tf) != (size_t)len)) {
            fprintf(stderr, "%s: read failed\n", text_file); fclose(tf); return 1;
        }
        filetext[len] = 0;
        fclose(tf);
    }
    const char *text = text_file ? filetext : pos[2];
    const char *outpath = text_file ? pos[2] : pos[3];

    char err[256];
    echotalk *et = echotalk_create(pos[0], pos[1], err, sizeof err);
    if (!et) { fprintf(stderr, "echotalk_create: %s\n", err); return 1; }

    fprintf(stderr, "Textalker version %s\n", echotalk_version(et));

    if (rate && echotalk_set_sample_rate(et, rate)) fprintf(stderr, "bad --rate\n");
    if (echotalk_set_clock_multiplier(et, clock_mult)) fprintf(stderr, "bad --clock\n");
    if (echotalk_set_frame_rate(et, frame_rate)) fprintf(stderr, "bad --frame-rate\n");
    if (echotalk_set_speed(et, speed)) fprintf(stderr, "bad --speed\n");
    echotalk_set_compressed(et, compressed);
    if (pitch >= 0 && echotalk_set_pitch(et, pitch)) fprintf(stderr, "bad --pitch\n");
    if (volume >= 0 && echotalk_set_volume(et, volume)) fprintf(stderr, "bad --volume\n");
    if (word_delay >= 0 && echotalk_set_word_delay(et, word_delay)) fprintf(stderr, "bad --word-delay\n");
    if (repeat_filter >= 0 && echotalk_set_repeat_filter(et, repeat_filter)) fprintf(stderr, "bad --repeat-filter\n");
    if (chunk >= 0 && echotalk_set_chunk_size(et, (unsigned)chunk)) fprintf(stderr, "bad --chunk\n");
    if (raw) echotalk_set_raw(et, 1);

    if (echotalk_speak(et, text) != 0) {
        fprintf(stderr, "echotalk_speak failed\n");
        echotalk_destroy(et);
        return 1;
    }

    /* Bad Ctrl-D commands are swallowed rather than spoken, which is
     * right for a screen reader but leaves a typo invisible. This is
     * the only place it surfaces. */
    unsigned bad = echotalk_command_errors(et);
    if (bad)
        fprintf(stderr, "warning: %u malformed or unknown Ctrl-D command%s "
                        "ignored\n", bad, bad == 1 ? "" : "s");

    /* A runaway guard tripping means the 6502 was cut off part-way
     * through a routine and the speech after it is wrong. This used to
     * happen in silence, at high word delays and slow speeds, and the
     * only symptom was speech that came out mangled. */
    unsigned over = echotalk_overruns(et);
    if (over)
        fprintf(stderr, "WARNING: %u emulation overrun%s -- the audio is "
                        "wrong. Please report this.\n", over, over == 1 ? "" : "s");

    /* Chunking off is not just "longer lines" -- Textalker's own line
     * buffer bound is never initialised under this emulation, so its
     * auto-flush point is undefined. Say so, however it got turned off. */
    if (echotalk_chunk_size(et) == 0)
        fprintf(stderr, "warning: chunking is off; text past Textalker's own "
                        "line buffer reaches an undefined flush point, and "
                        "how it breaks is uncharacterised\n");

    /* Pull in small blocks, the way an audio callback would. Since the
     * library streams, this is also where synthesis happens -- speak()
     * above only queued the text. Index events are collected after each
     * block, which is how a host turns them into progress reports. */
    size_t cap = 65536, n = 0;
    int16_t *pcm = malloc(cap * sizeof(int16_t)), block[1024];
    size_t got;
    while ((got = echotalk_read(et, block, 1024)) > 0) {
        if (n + got > cap) { cap = (n + got) * 2; pcm = realloc(pcm, cap * sizeof(int16_t)); }
        memcpy(pcm + n, block, got * sizeof(int16_t));
        n += got;
        int idx;
        while (echotalk_next_index(et, &idx))
            fprintf(stderr, "index %d reached at sample %zu (%.3f s)\n",
                    idx, n, (double)n / (rate ? rate : 8000));
    }
    /* Once more after the final read: a mark at the very end of the text
     * only becomes ready on the read that returns 0, and an end-of-speech
     * marker is exactly what a host is most likely to put there. */
    {
        int idx;
        while (echotalk_next_index(et, &idx))
            fprintf(stderr, "index %d reached at sample %zu (%.3f s)\n",
                    idx, n, (double)n / (rate ? rate : 8000));
    }

    /* The library delivers at the rate it was asked for -- the clock
     * multiplier is already baked into the samples by then, so applying
     * it again here would double it. */
    unsigned out_rate = rate ? rate : 8000;
    wav_write(outpath, out_rate, pcm, n);
    fprintf(stderr, "%zu samples, %.3f s at %u Hz -> %s\n",
            n, (double)n / out_rate, out_rate, outpath);

    free(pcm); free(filetext);
    echotalk_destroy(et);
    return 0;
}
