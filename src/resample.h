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

#ifdef __cplusplus
}
#endif

#endif /* ECHOTALK_RESAMPLE_H */
