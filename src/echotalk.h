/*
 * echotalk.h -- speech synthesis by emulating an Apple II Echo II card
 * running Street Electronics' Textalker.
 *
 * Give it a Textalker loader and OBJ image and it speaks. Which
 * Textalker version you get is decided by which images you pass;
 * everything else about the two versions is worked out from the files
 * themselves, so 3.1.3, 1.3 and near-identical builds like 3.1.2 all
 * work without being named. See notes/multi_version_support_design.md.
 *
 * SINGLE INSTANCE. The 6502 core (Fake6502) keeps its registers in
 * globals, so exactly one echotalk may exist at a time.
 * echotalk_create() fails if one is already live. This is not a problem
 * for a screen reader, which wants one voice at a time, but it is worth
 * knowing before designing around it.
 *
 * Threading: not thread-safe. Serialise calls, or confine an instance
 * to one thread.
 */
#ifndef ECHOTALK_H
#define ECHOTALK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct echotalk echotalk;

/* --- lifecycle --- */

/* Loads the images and boots Textalker. On failure returns NULL and,
 * if errbuf is non-NULL, writes a human-readable reason into it. */
echotalk *echotalk_create(const char *loader_path, const char *obj_path,
                          char *errbuf, size_t errbuf_len);
void echotalk_destroy(echotalk *et);

/* Version banner read out of the loader, e.g. "3.1.3" or "1.3", or
 * "unknown" if it could not be parsed. Display only -- nothing in this
 * library keys behaviour off it. */
const char *echotalk_version(const echotalk *et);

/* --- settings ---
 *
 * All return 0 on success, -1 if the value is out of range. Settings
 * take effect on the next echotalk_speak(); they do not disturb speech
 * already queued.
 *
 * The four rate-ish controls are deliberately independent:
 *
 *   pitch            pitch only     Textalker's own nP command
 *   frame rate       speed only     TMS5220 interpolation periods
 *   clock multiplier speed + pitch  the "sped-up tape" character
 *   sample rate      neither        output format only
 */

/* Output sample rate in Hz. The chip is native 8000; anything else is
 * resampled by linear interpolation with no anti-aliasing, which keeps
 * the original grit rather than smoothing it. 0 selects native. */
int echotalk_set_sample_rate(echotalk *et, unsigned hz);

/* Pretends the TMS5220 runs at a different clock. Changes speed and
 * pitch together, exactly as over/underclocking the real chip would.
 * 1.0 is the real Echo II. Range 0.25 to 4.0. */
int echotalk_set_clock_multiplier(echotalk *et, double multiplier);

/* TMS5220 frame rate, 0-3: 8, 6, 4 or 2 interpolation periods per
 * frame, measuring roughly 1.00x, 1.31x, 1.89x and 3.40x speed with
 * pitch unchanged. 0 is what real Echo II hardware does; the rest are
 * an emulator capability, since the card carries a plain TMS5220 whose
 * SET RATE command is a no-op. Higher rates skip the gentler early
 * interpolation steps, so timbre changes as well as speed. */
int echotalk_set_frame_rate(echotalk *et, int rate);

/* Textalker's own two speech rates: 0 expanded (default), 1 compressed.
 * It implements these by skipping phoneme segments rather than by
 * changing playback rate, so this is a different effect from either
 * control above and composes with both. */
int echotalk_set_compressed(echotalk *et, int compressed);

/* Textalker pitch 0-63 (default 24) and volume 0-15 (default 12). */
int echotalk_set_pitch(echotalk *et, int pitch);
int echotalk_set_volume(echotalk *et, int volume);

/* --- speaking --- */

/* Synthesises `text` in full and queues the audio for reading.
 * Accepts UTF-8 or legacy single-byte text, detected per character, and
 * reduces it to the 7-bit ASCII Textalker understands. Long lines are
 * split at clause then word boundaries so Textalker's own buffer never
 * decides where to break, which it would otherwise sometimes do
 * mid-word. Returns 0 on success. */
int echotalk_speak(echotalk *et, const char *text);

/* Copies up to `frames` samples of 16-bit mono PCM into `out` and
 * returns how many were written; 0 means the queue is drained. */
size_t echotalk_read(echotalk *et, int16_t *out, size_t frames);

/* Samples still waiting to be read. */
size_t echotalk_available(const echotalk *et);

/* Discards queued audio. Textalker's own state is untouched, so
 * subsequent speech sounds the same as if this had not been called. */
void echotalk_stop(echotalk *et);

#ifdef __cplusplus
}
#endif

#endif /* ECHOTALK_H */
