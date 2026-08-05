/*
 * resample_wav.c
 *
 * Reads a 16-bit mono PCM WAV file and writes a resampled copy using
 * the same naive (unfiltered, intentionally aliasing) linear
 * interpolation as render_wav.c's resample_naive() -- kept separate so
 * it can be applied to any already-rendered WAV, not just the
 * hardcoded demo stream render_wav.c works from.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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
    if (argc < 4) {
        fprintf(stderr, "usage: %s <input.wav> <output.wav> <target_rate>\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open input"); return 1; }

    uint8_t riff_hdr[12];
    fread(riff_hdr, 1, 12, f);
    uint32_t in_rate = 0;
    uint16_t bits = 0, channels = 0;
    int16_t *pcm = NULL;
    size_t pcm_count = 0;

    while (1) {
        char chunk_id[4];
        uint32_t chunk_size;
        if (fread(chunk_id, 1, 4, f) != 4) break;
        if (fread(&chunk_size, 4, 1, f) != 1) break;
        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            uint16_t fmt; fread(&fmt, 2, 1, f);
            fread(&channels, 2, 1, f);
            fread(&in_rate, 4, 1, f);
            uint32_t byte_rate; fread(&byte_rate, 4, 1, f);
            uint16_t block_align; fread(&block_align, 2, 1, f);
            fread(&bits, 2, 1, f);
            fseek(f, chunk_size - 16, SEEK_CUR);
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            pcm_count = chunk_size / 2;
            pcm = malloc(chunk_size);
            fread(pcm, 1, chunk_size, f);
        } else {
            fseek(f, chunk_size, SEEK_CUR);
        }
    }
    fclose(f);

    if (!pcm || in_rate == 0) {
        fprintf(stderr, "failed to parse input WAV (need fmt + data chunks)\n");
        return 1;
    }
    fprintf(stderr, "Input: %u Hz, %u channels, %u bits, %zu samples (%.3fs)\n",
            in_rate, channels, bits, pcm_count, (double)pcm_count / in_rate);

    uint32_t target_rate = (uint32_t)atoi(argv[3]);
    size_t out_count;
    int16_t *resampled = resample_naive(pcm, pcm_count, in_rate, target_rate, &out_count);
    fprintf(stderr, "Resampled (naive, unfiltered -- aliasing expected) to %u Hz: %zu samples (%.3fs)\n",
            target_rate, out_count, (double)out_count / target_rate);

    FILE *out = fopen(argv[2], "wb");
    uint32_t data_bytes = (uint32_t)(out_count * 2);
    uint32_t byte_rate = target_rate * 2;
    uint16_t block_align = 2, out_bits = 16, fmt_tag = 1, out_ch = 1;
    uint32_t fmt_size = 16, riff_size = 36 + data_bytes;
    fwrite("RIFF", 1, 4, out); fwrite(&riff_size, 4, 1, out); fwrite("WAVE", 1, 4, out);
    fwrite("fmt ", 1, 4, out); fwrite(&fmt_size, 4, 1, out);
    fwrite(&fmt_tag, 2, 1, out); fwrite(&out_ch, 2, 1, out);
    fwrite(&target_rate, 4, 1, out); fwrite(&byte_rate, 4, 1, out);
    fwrite(&block_align, 2, 1, out); fwrite(&out_bits, 2, 1, out);
    fwrite("data", 1, 4, out); fwrite(&data_bytes, 4, 1, out);
    fwrite(resampled, 2, out_count, out);
    fclose(out);
    fprintf(stderr, "Wrote %s\n", argv[2]);
    return 0;
}
