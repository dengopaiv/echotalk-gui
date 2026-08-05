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
    double clock_mult = 1.0;
    int frame_rate = 0, compressed = 0, pitch = -1, volume = -1;
    const char *pos[4]; int npos = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--clock") && i + 1 < argc) clock_mult = atof(argv[++i]);
        else if (!strcmp(argv[i], "--frame-rate") && i + 1 < argc) frame_rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--compressed")) compressed = 1;
        else if (!strcmp(argv[i], "--pitch") && i + 1 < argc) pitch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--volume") && i + 1 < argc) volume = atoi(argv[++i]);
        else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "unknown option %s\n", argv[i]); return 1;
        } else if (npos < 4) pos[npos++] = argv[i];
    }
    if (npos != 4) {
        fprintf(stderr,
            "usage: %s [options] <loader.bin> <obj.bin> <text> <out.wav>\n"
            "  --rate HZ         output sample rate (default 8000, native)\n"
            "  --clock MULT      TMS5220 clock multiplier, speed and pitch\n"
            "  --frame-rate N    0-3, speed only (1.00x 1.31x 1.89x 3.40x)\n"
            "  --compressed      Textalker compressed speech\n"
            "  --pitch N         0-63 (default 24)\n"
            "  --volume N        0-15 (default 12)\n", argv[0]);
        return 1;
    }

    char err[256];
    echotalk *et = echotalk_create(pos[0], pos[1], err, sizeof err);
    if (!et) { fprintf(stderr, "echotalk_create: %s\n", err); return 1; }

    fprintf(stderr, "Textalker version %s\n", echotalk_version(et));

    if (rate && echotalk_set_sample_rate(et, rate)) fprintf(stderr, "bad --rate\n");
    if (echotalk_set_clock_multiplier(et, clock_mult)) fprintf(stderr, "bad --clock\n");
    if (echotalk_set_frame_rate(et, frame_rate)) fprintf(stderr, "bad --frame-rate\n");
    echotalk_set_compressed(et, compressed);
    if (pitch >= 0 && echotalk_set_pitch(et, pitch)) fprintf(stderr, "bad --pitch\n");
    if (volume >= 0 && echotalk_set_volume(et, volume)) fprintf(stderr, "bad --volume\n");

    if (echotalk_speak(et, pos[2]) != 0) {
        fprintf(stderr, "echotalk_speak failed\n");
        echotalk_destroy(et);
        return 1;
    }

    /* Pull in small blocks, the way an audio callback would. */
    size_t cap = 65536, n = 0;
    int16_t *pcm = malloc(cap * sizeof(int16_t)), block[1024];
    size_t got;
    while ((got = echotalk_read(et, block, 1024)) > 0) {
        if (n + got > cap) { cap = (n + got) * 2; pcm = realloc(pcm, cap * sizeof(int16_t)); }
        memcpy(pcm + n, block, got * sizeof(int16_t));
        n += got;
    }

    /* The library delivers at the rate it was asked for -- the clock
     * multiplier is already baked into the samples by then, so applying
     * it again here would double it. */
    unsigned out_rate = rate ? rate : 8000;
    wav_write(pos[3], out_rate, pcm, n);
    fprintf(stderr, "%zu samples, %.3f s at %u Hz -> %s\n",
            n, (double)n / out_rate, out_rate, pos[3]);

    free(pcm);
    echotalk_destroy(et);
    return 0;
}
