/*
 * text_prep.h
 *
 * Reduces modern text to something Textalker can actually consume.
 *
 * Textalker predates every 8-bit extended encoding -- Latin-1, MS-ANSI,
 * Windows-1252, UTF-8, all of it. It understands the original 7-bit
 * ASCII set and nothing else, and the Apple II convention of setting the
 * high bit is a *transport* detail applied by the emulation layer, not
 * an encoding. So by the time text reaches Textalker it must already be
 * plain 7-bit ASCII.
 *
 * This module also handles two Apple II line-ending quirks:
 *
 *   - The Apple II terminates lines with CR. A LF byte is not a line
 *     ending to Textalker; in "all punctuation" mode it announces it as
 *     "linefeed". LF is therefore stripped, and a CRLF pair collapses to
 *     the CR that Textalker expects.
 *   - Other C0 control characters would likewise be spoken or
 *     misinterpreted, so they are dropped -- except the two that carry
 *     meaning: Ctrl-E ($05), which introduces an Echo command, and
 *     Ctrl-V ($16), which switches to phoneme mode. Tab becomes a space.
 *
 * Like the chunker, this is pure logic with no dependency on the
 * 6502/TMS5220 emulation, so it is independently unit-testable.
 *
 * Ordering note: run this BEFORE chunking. It can change the length of
 * the text (one character may become several, e.g. "..." for an
 * ellipsis), and the chunker's whole job is fitting text to Textalker's
 * buffer, which only means anything once the text is in Textalker's own
 * character set.
 */
#ifndef ECHOTALK_TEXT_PREP_H
#define ECHOTALK_TEXT_PREP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Worst-case output growth per input byte. One input character can
 * expand to a short word (e.g. U+00B1 -> " plus or minus "), so a
 * caller sizing its own buffer should allow this much per input byte.
 * Deliberately generous. */
#define ECHOTALK_PREP_MAX_EXPANSION 16

typedef struct {
    /* Byte substituted for a character with no ASCII rendering. Use ' '
     * (the default) to keep words from running together, or 0 to drop
     * such characters entirely. */
    char unmapped;
} echotalk_prep_opts;

/* Fills `opts` with defaults: unmapped = ' '. */
void echotalk_prep_defaults(echotalk_prep_opts *opts);

/* Converts `in` (`in_len` bytes, need not be NUL-terminated) to 7-bit
 * ASCII in `out`, writing at most `out_cap` bytes and NUL-terminating
 * if there is room.
 *
 * Input encoding is detected per character rather than declared: a
 * well-formed UTF-8 sequence is decoded as UTF-8, and any other byte
 * >= $80 is interpreted as Windows-1252 (which is Latin-1 except for
 * $80-$9F, where it holds the curly quotes and dashes that real-world
 * text is full of). This handles UTF-8 input, legacy single-byte input,
 * and even the two mixed together, without the caller having to know
 * which it has.
 *
 * `opts` may be NULL for defaults.
 *
 * Returns the number of bytes the full result would occupy, excluding
 * the terminator -- so a return >= out_cap means output was truncated,
 * in the manner of snprintf. Passing out_cap 0 (and out NULL) is a
 * legitimate way to measure the required size first. */
size_t echotalk_prep_text(const uint8_t *in, size_t in_len,
                          char *out, size_t out_cap,
                          const echotalk_prep_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* ECHOTALK_TEXT_PREP_H */
