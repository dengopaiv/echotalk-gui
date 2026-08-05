/*
 * render_common.h -- shared option handling, leading-silence trimming,
 * WAV output and end-of-run reporting for the EchoTalk render tools.
 *
 * Header-only (everything is static inline) so the tools stay
 * single-translation-unit programs, as they always have been. The point
 * is that all three harnesses behave identically from the command line
 * rather than drifting apart.
 *
 * Output policy: quiet by default. A run prints one short stats block
 * when it finishes, and nothing else -- except warnings, which always
 * print because they mean the audio may be wrong. Pass -v for the boot
 * diagnostics and per-character progress that used to be unconditional.
 */
#ifndef RENDER_COMMON_H
#define RENDER_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "text_prep.h"
#include "chunker.h"

/* Samples of lead-in kept before speech resumes, so trimming can never
 * bite into the attack of the first phoneme. 40 is 5ms at 8kHz. */
#define TRIM_KEEP_MARGIN 40

/* --- Dead air between utterances ---
 *
 * Textalker processes a whole line before it sends anything to the
 * chip, and that work scales with how much text there is. While it is
 * thinking, the chip has nothing to play, so the renderer emits silence
 * -- a pause before each chunk whose length tracks the chunk's length.
 * That is a batch-rendering artifact, not speech.
 *
 * It must not be confused with the pauses Textalker puts after a
 * period or comma, which are real content. The two are easy to tell
 * apart at the chip level rather than by looking at the waveform: an
 * intentional pause is the chip *playing* silent frames, with TALKD
 * asserted, whereas dead air is the chip idle with TALKD clear. So we
 * record, per sample, whether the chip was speaking, and only ever trim
 * runs where it was not. Punctuation pauses survive by construction --
 * there is no threshold to tune and no way for the trimmer to reach
 * them. */

/* Textalker's repeat-character filter collapses runs of the same
 * character, so a decorative line of asterisks is not read out one
 * "star" at a time. It does not distinguish decoration from content,
 * though: "EEEEEEEEE" is spoken as if it were "EE". Sending Ctrl-E, a
 * repeat count of 99, then R sets the threshold high enough that the
 * filter never triggers in practice.
 *
 * Send this once, after init and before any real text. */
#define REPEAT_FILTER_DISABLE "\x05" "99R"

/* Longest run of text handed to Textalker between CRs.
 *
 * Textalker auto-flushes -- i.e. speaks -- when its line buffer fills,
 * and that boundary lands wherever it lands, including the middle of a
 * word. Worse, in this harness the bound is never even initialised:
 * Textalker computes it at $D781 from the Apple II text-window width,
 * and $D781 is never reached, because nothing here plays the part of
 * DOS/Applesoft setting up a screen. $FD80-$FD82 stay zero and the
 * effective flush point is emergent (observed around 165 characters).
 *
 * So we must not let Textalker reach its own boundary. Chunking here
 * splits at clause boundaries first, then word boundaries, so speech
 * breaks where a reader would breathe. 80 matches the largest buffer
 * Textalker natively supports (80-column mode) while staying well under
 * the observed flush point. */
#define DEFAULT_CHUNK_SIZE 80

typedef struct {
    int verbose;
    int trim;               /* trim dead air at the start of the file */
    int gap_trim;           /* ...and at the start of every utterance */
    int repeat_filter_fix;  /* send REPEAT_FILTER_DISABLE during init */
    int raw_input;          /* skip text preparation */
    int chunk_size;         /* 0 = do not chunk */
    const char *pos[8];
    int npos;
} render_opts;

static render_opts g_ropts = { 0, 1, 1, 1, 0, DEFAULT_CHUNK_SIZE, { 0 }, 0 };

/* --- Shared capture buffer ---
 * Holds the samples plus, in parallel, whether the chip was speaking
 * when each was produced. Lives here rather than in each harness so all
 * three trim identically. */
static int16_t *r_audio = NULL;
static uint8_t *r_speaking = NULL;
static size_t r_count = 0, r_cap = 0;

/* Sample offsets at which a new utterance begins, i.e. where dead air
 * may be trimmed. Index 0 is implicit (start of file). */
static size_t r_marks[4096];
static size_t r_nmarks = 0;

static inline void render_audio_push(int16_t s, int speaking) {
    if (r_count >= r_cap) {
        r_cap = r_cap ? r_cap * 2 : 65536;
        r_audio = (int16_t *)realloc(r_audio, r_cap * sizeof(int16_t));
        r_speaking = (uint8_t *)realloc(r_speaking, r_cap);
    }
    r_speaking[r_count] = (uint8_t)(speaking ? 1 : 0);
    r_audio[r_count++] = s;
}

static inline size_t render_audio_count(void) { return r_count; }

/* Call immediately before sending the first character of an utterance. */
static inline void render_mark_utterance(void) {
    if (r_nmarks < sizeof(r_marks) / sizeof(r_marks[0]))
        r_marks[r_nmarks++] = r_count;
}

#define VLOG(...) do { if (g_ropts.verbose) fprintf(stderr, __VA_ARGS__); } while (0)

static inline void render_usage(const char *prog, const char *argspec) {
    fprintf(stderr,
        "usage: %s [options] %s\n"
        "\n"
        "options:\n"
        "  -v, --verbose       boot diagnostics and per-character progress\n"
        "      --no-trim       keep leading silence (needed to reproduce the\n"
        "                      documented reference sample counts exactly)\n"
        "      --no-repeat-fix leave Textalker's repeat-character filter at its\n"
        "                      default, where \"EEEEEEEEE\" is spoken as \"EE\"\n"
        "      --raw           send input bytes as-is, with no conversion to\n"
        "                      7-bit ASCII and no LF stripping\n"
        "      --chunk N       split long lines at clause/word boundaries every\n"
        "                      N characters (default %d)\n"
        "      --no-chunk      never split; lets Textalker's own buffer decide,\n"
        "                      which can break speech mid-word\n"
        "  -h, --help          this message\n",
        prog, argspec, DEFAULT_CHUNK_SIZE);
}

/* Returns 0 on success. Collects non-flag arguments in o->pos. */
static inline int render_parse_args(int argc, char **argv, int need,
                                    const char *argspec, render_opts *o) {
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "-v") || !strcmp(arg, "--verbose")) o->verbose = 1;
        else if (!strcmp(arg, "--no-trim")) o->trim = 0;
        else if (!strcmp(arg, "--no-repeat-fix")) o->repeat_filter_fix = 0;
        else if (!strcmp(arg, "--raw")) o->raw_input = 1;
        else if (!strcmp(arg, "--no-chunk")) o->chunk_size = 0;
        else if (!strcmp(arg, "--keep-chunk-gaps")) o->gap_trim = 0;
        else if (!strcmp(arg, "--chunk") && i + 1 < argc) o->chunk_size = atoi(argv[++i]);
        else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            render_usage(argv[0], argspec);
            exit(0);
        } else if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "%s: unknown option '%s'\n", argv[0], arg);
            render_usage(argv[0], argspec);
            return 1;
        } else if (o->npos < (int)(sizeof(o->pos) / sizeof(o->pos[0]))) {
            o->pos[o->npos++] = arg;
        }
    }
    if (o->npos != need) {
        render_usage(argv[0], argspec);
        return 1;
    }
    return 0;
}

static inline uint8_t *render_chunk_lines(const render_opts *o,
                                          uint8_t *text, long len, long *out_len);

/* Loads a file and converts it to the 7-bit ASCII Textalker expects
 * (unless --raw). Returns a malloc'd buffer and sets *out_len; the
 * caller owns it. Exits on failure, since every caller would anyway. */
static inline uint8_t *render_load_input(const render_opts *o,
                                         const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long raw_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *raw = (uint8_t *)malloc(raw_len ? raw_len : 1);
    if (raw_len && fread(raw, 1, raw_len, f) != (size_t)raw_len) {
        perror(path); fclose(f); exit(1);
    }
    fclose(f);

    if (o->raw_input) { *out_len = raw_len; return raw; }

    size_t need = echotalk_prep_text(raw, (size_t)raw_len, NULL, 0, NULL);
    char *prepped = (char *)malloc(need + 1);
    echotalk_prep_text(raw, (size_t)raw_len, prepped, need + 1, NULL);
    if ((long)need != raw_len && o->verbose)
        fprintf(stderr, "Text preparation: %ld bytes in, %zu out\n", raw_len, need);
    free(raw);
    return render_chunk_lines(o, (uint8_t *)prepped, (long)need, out_len);
}

/* Splits text so Textalker never reaches its own buffer boundary.
 *
 * CR is an explicit utterance boundary in the input, so lines are
 * honoured as-is and only over-long ones are chunked; each resulting
 * chunk is terminated with its own CR, which is what makes Textalker
 * speak it. Text short enough to fit is passed through untouched, so
 * this is a no-op for anything that already fits.
 *
 * Returns a malloc'd buffer; caller owns it. */
static inline uint8_t *render_chunk_lines(const render_opts *o,
                                          uint8_t *text, long len, long *out_len) {
    if (o->chunk_size <= 0) { *out_len = len; return text; }

    /* Worst case adds one CR per chunk, plus a terminator. */
    size_t cap = (size_t)len + (size_t)(len / o->chunk_size + 2) + 1;
    uint8_t *out = (uint8_t *)malloc(cap);
    size_t w = 0;
    int split_count = 0;

    long line_start = 0;
    for (long i = 0; i <= len; i++) {
        int at_end = (i == len);
        if (!at_end && text[i] != '\r') continue;
        long line_len = i - line_start;

        if (line_len > 0) {
            if (line_len <= o->chunk_size) {
                memcpy(out + w, text + line_start, (size_t)line_len);
                w += (size_t)line_len;
                out[w++] = '\r';
            } else {
                echotalk_chunk chunks[256];
                size_t n = echotalk_chunk_text((const char *)text + line_start,
                                               (size_t)line_len,
                                               (size_t)o->chunk_size,
                                               chunks, 256);
                for (size_t c = 0; c < n; c++) {
                    memcpy(out + w, text + line_start + chunks[c].offset, chunks[c].length);
                    w += chunks[c].length;
                    out[w++] = '\r';
                }
                if (n > 1) split_count += (int)n - 1;
            }
        } else if (!at_end) {
            out[w++] = '\r'; /* preserve blank lines / bare CRs */
        }
        line_start = i + 1;
    }

    if (split_count && o->verbose)
        fprintf(stderr, "Chunking: %d extra split(s) at clause/word boundaries "
                        "(max %d chars)\n", split_count, o->chunk_size);
    free(text);
    *out_len = (long)w;
    return out;
}

/* A sample counts as audible above this. The TMS5220 does not idle at
 * exactly zero, so a zero test would find "audio" immediately. */
#define TRIM_SILENCE_THRESHOLD 150

/* From `start`, how many samples of dead air precede the point real
 * output begins, less the keep-margin.
 *
 * The scan stops at the first sample that is both *played by the chip*
 * (TALKD set) and *audible*. Both conditions matter: TALKD alone would
 * stop on the obligatory silent frame the chip emits when speech
 * restarts, which is a restart artifact rather than content; amplitude
 * alone could stop on a stray non-zero sample while the chip is idle.
 *
 * Because the scan stops at the first audible output, it can only ever
 * consume silence at the very head of an utterance -- a pause after a
 * comma or period, which by definition follows audible speech, is
 * unreachable. Returns 0 if nothing audible follows, so trailing
 * silence is never swallowed either. */
static inline size_t render_dead_air(size_t start, size_t end) {
    size_t i = start;
    while (i < end) {
        int mag = r_audio[i] < 0 ? -r_audio[i] : r_audio[i];
        if (r_speaking[i] && mag > TRIM_SILENCE_THRESHOLD) break;
        i++;
    }
    if (i >= end) return 0;              /* nothing audible follows */
    size_t run = i - start;
    return run > TRIM_KEEP_MARGIN ? run - TRIM_KEEP_MARGIN : 0;
}

static inline void render_wav_write(const char *path, uint32_t rate,
                                    const int16_t *pcm, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    uint32_t data_bytes = (uint32_t)(n * 2);
    uint32_t byte_rate = rate * 2;
    uint16_t block_align = 2, bits = 16, fmt = 1, ch = 1;
    uint32_t fmt_size = 16, riff_size = 36 + data_bytes;
    fwrite("RIFF", 1, 4, f); fwrite(&riff_size, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmt_size, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    fwrite(pcm, 2, n, f);
    fclose(f);
}

/* Writes the WAV and prints the end-of-run summary.
 *
 * Dead air (chip idle, TALKD clear) is removed from the head of the
 * file and from the head of each utterance; silence the chip actually
 * played -- Textalker's pauses after punctuation -- is always kept.
 * Reports the untrimmed total too, so results stay comparable against
 * the documented reference sample counts. */
static inline void render_finish(const render_opts *o, const char *in_path,
                                 const char *out_path, long nchars,
                                 uint32_t rate) {
    size_t n = r_count;
    int16_t *out = (int16_t *)malloc((n ? n : 1) * sizeof(int16_t));
    size_t w = 0, trimmed_head = 0, trimmed_gaps = 0;
    int gaps_trimmed = 0;

    /* Walk the utterances in order, skipping each one's dead air.
     *
     * The first mark is the start of the first utterance; everything
     * before it is init (and the repeat-filter command), which is dead
     * air of exactly the same kind. So region 0 runs from sample 0
     * rather than from that mark -- the two are merged, and the head
     * gets trimmed as one piece. */
    size_t nregions = r_nmarks ? r_nmarks : 1;
    size_t pos = 0;
    for (size_t m = 0; m < nregions; m++) {
        size_t start = (m == 0) ? 0 : r_marks[m];
        size_t end = (m + 1 < nregions) ? r_marks[m + 1] : n;
        if (start < pos) start = pos;
        if (end < start) end = start;

        int is_head = (m == 0);
        int want = is_head ? o->trim : o->gap_trim;
        size_t skip = want ? render_dead_air(start, end) : 0;
        if (skip) {
            if (is_head) trimmed_head = skip;
            else { trimmed_gaps += skip; gaps_trimmed++; }
        }
        for (size_t i = start + skip; i < end; i++) out[w++] = r_audio[i];
        pos = end;
    }

    int lo = 0, hi = 0, clipped = 0;
    for (size_t i = 0; i < w; i++) {
        if (out[i] < lo) lo = out[i];
        if (out[i] > hi) hi = out[i];
        if (out[i] >= 32767 || out[i] <= -32768) clipped++;
    }
    int peak = hi > -lo ? hi : -lo;

    render_wav_write(out_path, rate, out, w);

    fprintf(stderr, "%s -> %s\n", in_path, out_path);
    fprintf(stderr, "  %ld chars, %zu samples (%.3f s) at %u Hz\n",
            nchars, n, (double)n / rate, rate);
    if (trimmed_head)
        fprintf(stderr, "  trimmed %zu samples (%.3f s) of leading dead air\n",
                trimmed_head, (double)trimmed_head / rate);
    if (gaps_trimmed)
        fprintf(stderr, "  trimmed %zu samples (%.3f s) of dead air before %d utterance(s)\n",
                trimmed_gaps, (double)trimmed_gaps / rate, gaps_trimmed);
    if (trimmed_head || trimmed_gaps)
        fprintf(stderr, "  wrote %zu samples (%.3f s)\n", w, (double)w / rate);
    fprintf(stderr, "  peak %d, range %d/+%d, clipped %d\n", peak, lo, hi, clipped);
    free(out);
}

#endif /* RENDER_COMMON_H */
