/*
 * render_wav.c
 *
 * Feeds a captured TMS5220 command/data byte stream (as produced by
 * Textalker and captured by our Echo-II-card probes) through the real
 * ported chip core and writes a 16-bit mono WAV file.
 *
 * Usage: render_wav <output.wav> [hex bytes on stdin, space separated]
 * If no bytes are piped in, uses the built-in "HI" stream captured
 * from the working v3.1.3 harness as a demo/self-test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tms5220_core.h"

/* Captured from tools/probe_no_loader.c: Textalker v3.1.3 speaking "HI"
 * in Talk-only mode, with our best current understanding of the byte
 * stream (real hardware-accurate, minus proper FIFO backpressure --
 * the trailing $FF run is an artifact of that, see README). */
static const uint8_t DEMO_HI[] = {
    0x60, 0x0C, 0x48, 0x5A, 0x94, 0x00, 0x45, 0x9B, 0xD9, 0xA8, 0xAE, 0xAC,
    0xD8, 0x49, 0x33, 0x59, 0x59, 0x75, 0xE9, 0x6A, 0xBE, 0x64, 0xCD, 0x9B,
    0x89, 0xC7, 0x93, 0x95, 0x75, 0xC2, 0x11, 0x4B, 0x56, 0x36, 0xBD, 0x32,
    0x4B, 0x46, 0x36, 0xBD, 0x32, 0x55, 0x2E, 0xD9, 0xF4, 0xCA, 0x54, 0xB9,
    0xE4
    /* trailing $FF run intentionally dropped -- see note below */
};

static void wav_write_header(FILE *f, uint32_t sample_rate, uint32_t num_samples) {
    uint32_t data_bytes = num_samples * 2;
    uint32_t byte_rate = sample_rate * 2;
    uint16_t block_align = 2;
    uint16_t bits_per_sample = 16;
    uint32_t riff_size = 36 + data_bytes;

    fwrite("RIFF", 1, 4, f);
    fwrite(&riff_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    uint16_t audio_format = 1; /* PCM */
    uint16_t num_channels = 1;
    fwrite(&audio_format, 2, 1, f);
    fwrite(&num_channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
}

/* Naive linear-interpolation resampler -- deliberately NOT anti-aliased.
 * No low-pass filtering is applied before downsampling, so content
 * above the new Nyquist frequency folds back down as aliasing rather
 * than being removed. This is intentional: it preserves the raw,
 * slightly gritty character of the original chip's output rather than
 * smoothing it into something that sounds like a different (nicer)
 * DAC than the real hardware had. Callers who want a clean resample
 * for other purposes should filter externally before calling this. */
static int16_t *resample_naive(const int16_t *in, size_t in_count,
                                uint32_t in_rate, uint32_t out_rate,
                                size_t *out_count) {
    if (in_rate == out_rate) {
        int16_t *out = malloc(in_count * sizeof(int16_t));
        memcpy(out, in, in_count * sizeof(int16_t));
        *out_count = in_count;
        return out;
    }
    size_t n_out = (size_t)((double)in_count * out_rate / in_rate);
    int16_t *out = malloc(n_out * sizeof(int16_t));
    double step = (double)in_rate / (double)out_rate;
    for (size_t i = 0; i < n_out; i++) {
        double src_pos = i * step;
        size_t i0 = (size_t)src_pos;
        double frac = src_pos - i0;
        int16_t s0 = (i0 < in_count) ? in[i0] : 0;
        int16_t s1 = (i0 + 1 < in_count) ? in[i0 + 1] : s0;
        out[i] = (int16_t)(s0 + (s1 - s0) * frac);
    }
    *out_count = n_out;
    return out;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <output.wav> [clock_multiplier] [target_rate]\n", argv[0]);
        fprintf(stderr, "  clock_multiplier: 0.5 to 2.0 (default 1.0) -- pitch+speed together\n");
        fprintf(stderr, "  target_rate: output sample rate in Hz, e.g. 44100 (default: native, no resampling)\n");
        return 1;
    }
    const char *outpath = argv[1];
    double clock_mult = (argc >= 3) ? atof(argv[2]) : 1.0;
    if (clock_mult < 0.5) clock_mult = 0.5;
    if (clock_mult > 2.0) clock_mult = 2.0;
    uint32_t target_rate = (argc >= 4) ? (uint32_t)atoi(argv[3]) : 0;

    /* Standard Echo II clock is 640kHz -> 8000 Hz native sample rate.
     * See a2echoii.cpp's device_add_mconfig(). Adjusting the declared
     * output rate by clock_mult produces the classic over/underclocked
     * "chipmunk" speed+pitch effect (see notes/tms5220_clock_and_resampling.md). */
    const uint32_t BASE_SAMPLE_RATE = 8000;
    uint32_t declared_rate = (uint32_t)(BASE_SAMPLE_RATE * clock_mult + 0.5);

    tms5220_state tms;
    tms5220_reset(&tms, TMS5220_IS_5220);

    /* Feed the byte stream, generating audio whenever the FIFO can't
     * accept the next byte yet (mirrors how a real host CPU would
     * poll /READY between writes). */
    int16_t *pcm = NULL;
    size_t pcm_count = 0, pcm_cap = 0;
    #define PUSH_SAMPLES(n) do { \
        int16_t chunk[256]; \
        unsigned remaining = (n); \
        while (remaining > 0) { \
            unsigned take = remaining > 256 ? 256 : remaining; \
            tms5220_process(&tms, chunk, take); \
            if (pcm_count + take > pcm_cap) { \
                pcm_cap = (pcm_cap == 0) ? 65536 : pcm_cap * 2; \
                if (pcm_cap < pcm_count + take) pcm_cap = pcm_count + take; \
                pcm = realloc(pcm, pcm_cap * sizeof(int16_t)); \
            } \
            memcpy(pcm + pcm_count, chunk, take * sizeof(int16_t)); \
            pcm_count += take; \
            remaining -= take; \
        } \
    } while (0)

    for (size_t i = 0; i < sizeof(DEMO_HI); i++) {
        int guard = 0;
        while (!tms5220_ready_read(&tms) && guard < 100000) {
            PUSH_SAMPLES(1);
            guard++;
        }
        tms5220_data_w(&tms, DEMO_HI[i]);
    }

    /* Drain remaining speech until the chip stops talking (bounded
     * safety limit in case something's still off in the port). */
    int silence_guard = 0;
    while (tms5220_talk_status(&tms) && silence_guard < BASE_SAMPLE_RATE * 10) {
        PUSH_SAMPLES(1);
        silence_guard++;
    }

    printf("Generated %zu samples (%.2f seconds at %u Hz declared rate)\n",
           pcm_count, (double)pcm_count / declared_rate, declared_rate);

    int16_t *final_pcm = pcm;
    size_t final_count = pcm_count;
    uint32_t final_rate = declared_rate;

    if (target_rate != 0 && target_rate != declared_rate) {
        size_t resampled_count;
        int16_t *resampled = resample_naive(pcm, pcm_count, declared_rate, target_rate, &resampled_count);
        printf("Resampled (naive, unfiltered -- aliasing expected) to %u Hz: %zu samples\n",
               target_rate, resampled_count);
        final_pcm = resampled;
        final_count = resampled_count;
        final_rate = target_rate;
    }

    FILE *f = fopen(outpath, "wb");
    if (!f) { perror("fopen"); return 1; }
    wav_write_header(f, final_rate, (uint32_t)final_count);
    fwrite(final_pcm, sizeof(int16_t), final_count, f);
    fclose(f);
    if (final_pcm != pcm) free(final_pcm);
    free(pcm);

    printf("Wrote %s\n", outpath);
    return 0;
}
