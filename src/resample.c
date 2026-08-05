/*
 * resample.c -- linear resampling, deliberately without anti-aliasing.
 *
 * Lifted unchanged in behaviour from tools/resample_wav.c. Content above
 * the new Nyquist frequency aliases rather than being filtered out, and
 * that is the point: it keeps the raw, slightly gritty character of the
 * original chip instead of smoothing it into something that sounds like
 * a nicer DAC than the Echo II ever had.
 */
#include "resample.h"

#include <stdlib.h>
#include <string.h>

int16_t *echotalk_resample(const int16_t *in, size_t in_count,
                           unsigned in_rate, unsigned out_rate,
                           size_t *out_count) {
    if (!in || !out_count || !in_rate || !out_rate) return NULL;

    if (in_rate == out_rate) {
        int16_t *out = malloc(in_count ? in_count * sizeof(int16_t) : 1);
        if (!out) return NULL;
        memcpy(out, in, in_count * sizeof(int16_t));
        *out_count = in_count;
        return out;
    }

    size_t n_out = (size_t)((double)in_count * out_rate / in_rate);
    int16_t *out = malloc(n_out ? n_out * sizeof(int16_t) : 1);
    if (!out) return NULL;

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
