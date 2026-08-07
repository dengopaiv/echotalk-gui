/*
 * resample.h -- see resample.c. Linear, no anti-aliasing, by design.
 */
#ifndef ECHOTALK_RESAMPLE_H
#define ECHOTALK_RESAMPLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns a malloc'd buffer of *out_count samples, or NULL on failure.
 * Caller owns it. */
int16_t *echotalk_resample(const int16_t *in, size_t in_count,
                           unsigned in_rate, unsigned out_rate,
                           size_t *out_count);

/* The same, for a stream arriving in pieces.
 *
 * `*phase` is the source position, in input samples, at which this
 * piece's first output sample is taken; it is updated to where the next
 * piece should start. Pass a variable initialised to 0 and keep handing
 * back the same one. Without this, resampling a stream piecewise resets
 * the interpolation phase at every boundary, which puts a small
 * discontinuity wherever one piece meets the next -- and the library
 * resamples per utterance, so that would be every utterance.
 *
 * One approximation remains at each boundary: the final output sample
 * of a piece has no following input sample to interpolate towards, so
 * it holds the last value instead of reaching into the next piece. That
 * is a single sample per utterance, against a phase reset in every one. */
int16_t *echotalk_resample_stream(const int16_t *in, size_t in_count,
                                  unsigned in_rate, unsigned out_rate,
                                  double *phase, size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* ECHOTALK_RESAMPLE_H */
