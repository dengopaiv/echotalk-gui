/*
 * chunker.h
 *
 * Splits arbitrary input text into chunks that fit within Textalker's
 * per-line buffer (see README/notes: tied to screen width, up to ~80
 * characters in 80-column mode -- see notes/buffer_and_indexing.md).
 *
 * Split preference, in order:
 *   1. Clause boundary (, . ? ! ; :) nearest the end of the window --
 *      keeps natural-sounding pauses/intonation intact.
 *   2. Word boundary (whitespace) nearest the end of the window, if no
 *      clause boundary was found.
 *   3. Hard split at exactly max_chunk_size, if the text has run past
 *      the window with no whitespace at all (e.g. a keysmash string,
 *      a URL, or other unbroken "word" longer than the buffer).
 *
 * This is pure logic with no dependency on the 6502/TMS5220 emulation,
 * so it's independently unit-testable.
 */
#ifndef ECHOTALK_CHUNKER_H
#define ECHOTALK_CHUNKER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t offset; /* byte offset into the original text */
    size_t length; /* length of this chunk, in bytes */
} echotalk_chunk;

/* Splits `text` (length `text_len`, need not be NUL-terminated) into
 * chunks of at most `max_chunk_size` bytes each, per the policy above.
 * Writes up to `max_chunks` entries into `chunks` and returns the
 * number actually written. Leading/trailing whitespace within each
 * chunk is trimmed from chunk boundaries (but not from the middle).
 * Returns 0 if text_len is 0, or if max_chunks is 0 and more chunks
 * would be needed (caller should check text_len vs. returned count *
 * max_chunk_size to detect truncation in that case). */
size_t echotalk_chunk_text(const char *text, size_t text_len,
                            size_t max_chunk_size,
                            echotalk_chunk *chunks, size_t max_chunks);

#ifdef __cplusplus
}
#endif

#endif /* ECHOTALK_CHUNKER_H */
