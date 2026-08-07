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

/* --- linkage ---
 *
 * Building the shared library defines ECHOTALK_BUILD_DLL; a C or C++
 * consumer that links against the import library defines ECHOTALK_DLL.
 * Neither is needed to compile the sources straight into a program,
 * which is what tools/say.c does, and it is the case that has to keep
 * working with no defines at all.
 *
 * Everything is plain cdecl. On x86-64 there is only one convention; on
 * 32-bit the exported names are undecorated, so ctypes.CDLL finds them
 * by their plain C names on both. */
#if defined(_WIN32)
#  if defined(ECHOTALK_BUILD_DLL)
#    define ECHOTALK_API __declspec(dllexport)
#  elif defined(ECHOTALK_DLL)
#    define ECHOTALK_API __declspec(dllimport)
#  else
#    define ECHOTALK_API
#  endif
#elif defined(ECHOTALK_BUILD_DLL)
#  define ECHOTALK_API __attribute__((visibility("default")))
#else
#  define ECHOTALK_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct echotalk echotalk;

/* ABI version of this header, bumped when the exported surface changes
 * in a way a caller could notice. A host that loads the library at
 * runtime -- which is what a screen reader does -- has no compile-time
 * check available, so it should call echotalk_abi_version() and compare
 * against this before anything else. */
#define ECHOTALK_ABI_VERSION 1
ECHOTALK_API unsigned echotalk_abi_version(void);

/* --- lifecycle --- */

/* Loads the images and boots Textalker. On failure returns NULL and,
 * if errbuf is non-NULL, writes a human-readable reason into it. */
ECHOTALK_API echotalk *echotalk_create(const char *loader_path,
                                       const char *obj_path,
                                       char *errbuf, size_t errbuf_len);
ECHOTALK_API void echotalk_destroy(echotalk *et);

/* Version banner read out of the loader, e.g. "3.1.3" or "1.3", or
 * "unknown" if it could not be parsed. Display only -- nothing in this
 * library keys behaviour off it. */
ECHOTALK_API const char *echotalk_version(const echotalk *et);

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
ECHOTALK_API int echotalk_set_sample_rate(echotalk *et, unsigned hz);

/* Pretends the TMS5220 runs at a different clock. Changes speed and
 * pitch together, exactly as over/underclocking the real chip would.
 * 1.0 is the real Echo II. Range 0.25 to 4.0. */
ECHOTALK_API int echotalk_set_clock_multiplier(echotalk *et, double multiplier);

/* TMS5220 frame rate, 0-3: 8, 6, 4 or 2 interpolation periods per
 * frame, measuring roughly 1.00x, 1.31x, 1.89x and 3.40x speed with
 * pitch unchanged. 0 is what real Echo II hardware does; the rest are
 * an emulator capability, since the card carries a plain TMS5220 whose
 * SET RATE command is a no-op. Higher rates skip the gentler early
 * interpolation steps, so timbre changes as well as speed. */
ECHOTALK_API int echotalk_set_frame_rate(echotalk *et, int rate);

/* Textalker's own two speech rates: 0 expanded (default), 1 compressed.
 * It implements these by skipping phoneme segments rather than by
 * changing playback rate, so this is a different effect from either
 * control above and composes with both. */
ECHOTALK_API int echotalk_set_compressed(echotalk *et, int compressed);

/* Textalker pitch 0-63 (default 24) and volume 0-15 (default 12). */
ECHOTALK_API int echotalk_set_pitch(echotalk *et, int pitch);
ECHOTALK_API int echotalk_set_volume(echotalk *et, int volume);

/* Pause Textalker inserts between words, 0-15 (default 0).
 * Textalker 1.3 does not implement this command at all and silently
 * discards it, so it has no effect there. */
ECHOTALK_API int echotalk_set_word_delay(echotalk *et, int delay);

/* Longest run of text handed to Textalker between CRs, in characters.
 *
 * Textalker speaks when its line buffer fills, and that boundary lands
 * wherever it lands -- including mid-word. Worse, in this emulation its
 * own buffer bound is never initialised (nothing plays the part of DOS
 * setting up a screen), so its auto-flush point is undefined. Chunking
 * ahead of it is what keeps text splitting at clause and word
 * boundaries instead. Default 80.
 *
 * 0 disables chunking entirely. That is not merely "longer lines": it
 * removes the only thing standing between long text and Textalker's
 * uncharacterised flush behaviour. Supported for experimentation, but
 * do not use it for anything that has to be right. */
ECHOTALK_API int echotalk_set_chunk_size(echotalk *et, unsigned chars);
ECHOTALK_API unsigned echotalk_chunk_size(const echotalk *et);

/* Raw mode: 0 (default) prepares text as echotalk_speak() describes,
 * 1 passes bytes to Textalker untouched. Raw mode is how you send
 * Ctrl-E command sequences that contain bytes text preparation would
 * otherwise fold away; note that ordinary Ctrl-E commands survive
 * preparation already and do not need it. */
ECHOTALK_API int echotalk_set_raw(echotalk *et, int raw);

/* Threshold for Textalker's repeat-character filter, 0-99.
 *
 * The filter exists so a decorative run like "*****" is not read out
 * one "star" at a time, but it does not distinguish decoration from
 * content: at its default setting "EEEEEEEEE" is spoken as "EE". The
 * default here is 99, high enough that it never triggers, which is
 * almost certainly what a screen reader wants. Lower it to get
 * Textalker's original behaviour back. */
ECHOTALK_API int echotalk_set_repeat_filter(echotalk *et, int threshold);

/* --- speaking --- */

/* Synthesises `text` in full and queues the audio for reading.
 * Accepts UTF-8 or legacy single-byte text, detected per character, and
 * reduces it to the 7-bit ASCII Textalker understands. Long lines are
 * split at clause then word boundaries so Textalker's own buffer never
 * decides where to break, which it would otherwise sometimes do
 * mid-word. Returns 0 on success.
 *
 * --- Ctrl-D driver commands ---
 *
 * The text may contain driver commands introduced by Ctrl-D (0x04),
 * deliberately echoing the shape of Textalker's own Ctrl-E commands:
 * an optional number, then a letter.
 *
 *   \x04 2F     frame rate 2          \x04F   frame rate back to default
 *   \x04 0.75C  clock multiplier      \x04C   clock back to 1.0
 *   \x04 0B     chunking off          \x04 80B  chunk at 80 characters
 *   \x04 1R     raw text on           \x04 0R   raw text off
 *   \x04\x04    one literal 0x04 byte, spoken rather than obeyed
 *
 * The namespaces are disjoint on purpose: Ctrl-E addresses the 1985
 * synthesiser, Ctrl-D addresses the driver around it. Ctrl-E sequences
 * survive text preparation untouched, so there is no need to duplicate
 * them here. Note that Ctrl-E's own letters differ -- Ctrl-E F is
 * flatness and Ctrl-E C is compressed, where Ctrl-D F is frame rate and
 * Ctrl-D C is clock.
 *
 * A command ENDS THE CURRENT UTTERANCE. Textalker buffers a whole line
 * and does not synthesise anything until the terminating CR arrives, so
 * a command's position in the text would otherwise bear no relation to
 * its position in the audio: applying it the moment it is seen would
 * apply it to everything already buffered. Flushing first means what
 * precedes the command speaks at the old settings and what follows it
 * at the new. The cost is an utterance boundary, and hence a small
 * pause, wherever a command appears.
 *
 * Settings changed this way PERSIST past the end of this call, exactly
 * as Ctrl-E commands persist inside Textalker.
 *
 * A malformed or unknown command is swallowed, never spoken -- a screen
 * reader reading its own control codes aloud would be worse than the
 * command being ignored -- and counted, see echotalk_command_errors(). */
ECHOTALK_API int echotalk_speak(echotalk *et, const char *text);

/* Number of malformed or unknown Ctrl-D commands seen since the last
 * call to echotalk_clear_command_errors(). Since bad commands are
 * silently dropped, this is the only way a host can notice a typo. */
ECHOTALK_API unsigned echotalk_command_errors(const echotalk *et);
ECHOTALK_API void echotalk_clear_command_errors(echotalk *et);

/* Copies up to `frames` samples of 16-bit mono PCM into `out` and
 * returns how many were written; 0 means the queue is drained. */
ECHOTALK_API size_t echotalk_read(echotalk *et, int16_t *out, size_t frames);

/* Samples still waiting to be read. */
ECHOTALK_API size_t echotalk_available(const echotalk *et);

/* Discards queued audio. Textalker's own state is untouched, so
 * subsequent speech sounds the same as if this had not been called. */
ECHOTALK_API void echotalk_stop(echotalk *et);

#ifdef __cplusplus
}
#endif

#endif /* ECHOTALK_H */
