/*
 * render_common.h -- shared option handling, leading-silence trimming,
 * WAV output and end-of-run reporting for the EchoTalk render tools.
 *
 * Header-only (everything is static inline) so the tools stay
 * single-translation-unit programs, as they always have been. The point
 * is that all three harnesses behave identically from the command line
 * rather than drifting apart.
 *
 * Output policy: quiet by default. A run prints one short stats block
 * when it finishes, and nothing else -- except warnings, which always
 * print because they mean the audio may be wrong. Pass -v for the boot
 * diagnostics and per-character progress that used to be unconditional.
 */
#ifndef RENDER_COMMON_H
#define RENDER_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* A sample counts as silence below this. The TMS5220 does not idle at
 * exactly zero, so a zero test would find "audio" immediately. */
#define TRIM_SILENCE_THRESHOLD 150
/* Samples of lead-in kept before the first real audio, so trimming can
 * never bite into the attack of the first phoneme. 40 is 5ms at 8kHz. */
#define TRIM_KEEP_MARGIN 40

typedef struct {
    int verbose;
    int trim;
    const char *pos[8];
    int npos;
} render_opts;

static render_opts g_ropts = { 0, 1, { 0 }, 0 };

#define VLOG(...) do { if (g_ropts.verbose) fprintf(stderr, __VA_ARGS__); } while (0)

static inline void render_usage(const char *prog, const char *argspec) {
    fprintf(stderr,
        "usage: %s [options] %s\n"
        "\n"
        "options:\n"
        "  -v, --verbose   boot diagnostics and per-character progress\n"
        "      --no-trim   keep leading silence (needed to reproduce the\n"
        "                  documented reference sample counts exactly)\n"
        "  -h, --help      this message\n",
        prog, argspec);
}

/* Returns 0 on success. Collects non-flag arguments in o->pos. */
static inline int render_parse_args(int argc, char **argv, int need,
                                    const char *argspec, render_opts *o) {
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "-v") || !strcmp(arg, "--verbose")) o->verbose = 1;
        else if (!strcmp(arg, "--no-trim")) o->trim = 0;
        else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            render_usage(argv[0], argspec);
            exit(0);
        } else if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "%s: unknown option '%s'\n", argv[0], arg);
            render_usage(argv[0], argspec);
            return 1;
        } else if (o->npos < (int)(sizeof(o->pos) / sizeof(o->pos[0]))) {
            o->pos[o->npos++] = arg;
        }
    }
    if (o->npos != need) {
        render_usage(argv[0], argspec);
        return 1;
    }
    return 0;
}

/* Index of the first sample that is not leading silence, backed off by
 * TRIM_KEEP_MARGIN. Returns 0 if the whole buffer is silent, so an
 * all-silent result is never turned into an empty file. */
static inline size_t render_trim_offset(const int16_t *pcm, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int mag = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (mag > TRIM_SILENCE_THRESHOLD)
            return i > TRIM_KEEP_MARGIN ? i - TRIM_KEEP_MARGIN : 0;
    }
    return 0;
}

static inline void render_wav_write(const char *path, uint32_t rate,
                                    const int16_t *pcm, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    uint32_t data_bytes = (uint32_t)(n * 2);
    uint32_t byte_rate = rate * 2;
    uint16_t block_align = 2, bits = 16, fmt = 1, ch = 1;
    uint32_t fmt_size = 16, riff_size = 36 + data_bytes;
    fwrite("RIFF", 1, 4, f); fwrite(&riff_size, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmt_size, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    fwrite(pcm, 2, n, f);
    fclose(f);
}

/* Writes the WAV (trimmed unless --no-trim) and prints the end-of-run
 * summary. Reports the untrimmed total as well, so results stay
 * comparable against the documented reference sample counts. */
static inline void render_finish(const render_opts *o, const char *in_path,
                                 const char *out_path, long nchars,
                                 uint32_t rate, const int16_t *pcm, size_t n) {
    size_t offset = o->trim ? render_trim_offset(pcm, n) : 0;
    size_t kept = n - offset;

    int lo = 0, hi = 0, clipped = 0;
    for (size_t i = offset; i < n; i++) {
        if (pcm[i] < lo) lo = pcm[i];
        if (pcm[i] > hi) hi = pcm[i];
        if (pcm[i] >= 32767 || pcm[i] <= -32768) clipped++;
    }
    int peak = hi > -lo ? hi : -lo;

    render_wav_write(out_path, rate, pcm + offset, kept);

    fprintf(stderr, "%s -> %s\n", in_path, out_path);
    fprintf(stderr, "  %ld chars, %zu samples (%.3f s) at %u Hz\n",
            nchars, n, (double)n / rate, rate);
    if (offset)
        fprintf(stderr, "  trimmed %zu leading silent samples (%.3f s), wrote %zu (%.3f s)\n",
                offset, (double)offset / rate, kept, (double)kept / rate);
    else if (!o->trim)
        fprintf(stderr, "  leading silence kept (--no-trim)\n");
    fprintf(stderr, "  peak %d, range %d/+%d, clipped %d\n", peak, lo, hi, clipped);
}

#endif /* RENDER_COMMON_H */
