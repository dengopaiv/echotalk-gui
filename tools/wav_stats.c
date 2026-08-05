/*
 * wav_stats.c
 *
 * Measures the things that matter when judging the onset glitch
 * (notes/onset_glitch_investigation_reverted.md) without having to
 * listen: where audio starts, how hard it hits in the first few
 * milliseconds, and -- critically -- the total duration, so that a
 * change which fixes the onset but silently truncates speech cannot
 * pass unnoticed. That is the exact mistake attempt 3 made.
 *
 * usage: wav_stats <file.wav> [more.wav ...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* A sample is "silence" below this; the chip idles at small nonzero
 * values, so a flat zero threshold would find onset immediately. */
#define SILENCE_THRESHOLD 150
/* Window after speech onset that counts as "the onset", in samples.
 * 300 at 8kHz is ~37ms -- long enough to contain the lattice filter's
 * ring-up transient, short enough not to be dominated by the steady
 * state that follows it. */
#define ONSET_WINDOW 300

static int stats(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    fseek(f, 44, SEEK_SET); /* skip canonical 44-byte header */
    long n = (bytes - 44) / 2;
    if (n <= 0) { fprintf(stderr, "%s: no samples\n", path); fclose(f); return 1; }
    int16_t *s = malloc(n * sizeof(int16_t));
    if (fread(s, sizeof(int16_t), n, f) != (size_t)n) { perror("read"); fclose(f); return 1; }
    fclose(f);

    long onset = -1;
    int gmin = 32767, gmax = -32768, clipped = 0;
    for (long i = 0; i < n; i++) {
        if (s[i] < gmin) gmin = s[i];
        if (s[i] > gmax) gmax = s[i];
        if (s[i] >= 32767 || s[i] <= -32768) clipped++;
        if (onset < 0) {
            int mag = s[i] < 0 ? -s[i] : s[i];
            if (mag > SILENCE_THRESHOLD) onset = i;
        }
    }

    int onset_peak = 0;
    if (onset >= 0) {
        long end = onset + ONSET_WINDOW;
        if (end > n) end = n;
        for (long i = onset; i < end; i++) {
            int mag = s[i] < 0 ? -s[i] : s[i];
            if (mag > onset_peak) onset_peak = mag;
        }
    }

    int overall_peak = gmax > -gmin ? gmax : -gmin;
    printf("%-40s %8ld samples  %7.3fs  onset@%-7ld onset_peak=%-7d peak=%-7d range=%d/%d  clipped=%d\n",
           path, n, (double)n / 8000.0,
           onset, onset_peak, overall_peak, gmin, gmax, clipped);
    free(s);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.wav> [...]\n", argv[0]); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) rc |= stats(argv[i]);
    return rc;
}
