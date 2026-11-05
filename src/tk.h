/*
 * tk.h -- Textalker's speech path, natively.
 *
 * A C translation of the Python in tools/, which is itself a
 * transcription of the 6502 rather than a model of it. Text goes in and
 * the exact byte stream Textalker writes to the TMS5220 comes out, with
 * no 6502 anywhere in it. Every 6502 address the original reads is in
 * the comments, and the Python of the same name is the reference:
 *
 *   tk_image.c     tools/profiles.py     where everything lives, and
 *                                        which version an image is
 *   tk_syllable.c  tools/syllabify.py    $DD10-$E126, v3.1.3 only
 *   tk_rules.c     tools/rule_walk.py    $E1FA-$E5A9, text -> phonemes
 *   tk_synth.c     tools/allophone.py    $DBD6-$EA43, phonemes -> chip
 *   tk_machine.c   tools/textalker.py    $D8D4-$DBA3, the driver
 *
 * And three that have no 6502 counterpart, because they are the point
 * of having a rewrite at all:
 *
 *   tk_arpabet.c   ARPABET <-> Textalker's phoneme codes
 *   tk_dict.c      a CMUdict-format dictionary as a tk_g2p source
 *   tk_disk.c      finding Textalker on an Apple II disk image
 *
 * NO TABLE IS COMPILED IN. Everything -- the rules, the attribute
 * table, the allophone frames, the punctuation names -- is read out of
 * the user's own Textalker image at run time, exactly as the emulation
 * path does. The engine ships; the data is Street Electronics' and does
 * not. That is a deliberate constraint on this whole project, not an
 * implementation detail: see HANDOFF.md.
 */
#ifndef TK_H
#define TK_H

#include <stddef.h>
#include <stdint.h>

#define TK_BUFSZ      256      /* $FDAC and $FEAD are both 256 bytes */
#define TK_PHONEME_CAP 0xF0    /* $E243 CPX #$F0 stops stage 1 at 240 */

/* EXTENDED BUFFER MODE.
 *
 * Textalker's line buffer is 256 bytes and it speaks when 240 of them
 * are used ($DB6A's CPX #$F0); stage 1 stops at 240 phonemes for the
 * same reason ($E243). At TK_BUFSZ the port reproduces all of that
 * exactly, which is what every differential test measures.
 *
 * A caller may ask for MORE. Nothing about the rewrite needs 256 -- it
 * is a 1981 constraint, not a property of the speech -- and a bigger
 * buffer means longer utterances and far fewer places where a sentence
 * has to be broken. What it costs is byte-exactness: the audible
 * behaviours that depend on the buffer's size stop matching the real
 * machine. Three of them, all deliberate features of the port:
 *
 *   - a rule that consumes more characters than are left walks on
 *     through the previous utterance's leftovers ($E247 tests the
 *     cursor for EQUALITY with $FD9E) until the phoneme cap stops it;
 *   - that cap, 240 phonemes;
 *   - $DC71's one-code lookahead, which for the last code reads one
 *     past the end and wraps within the buffer.
 *
 * So it is opt-in and named as a divergence, never a default.
 * TK_BUFSZ_MAX only sizes the arrays; the size in force is
 * tk_buf::size, and the flush point and phoneme cap are TK_CAP_OF() of
 * it -- sixteen bytes of headroom, which is 240 at 256. */
#define TK_BUFSZ_MAX  4096
#define TK_CAP_OF(sz) ((sz) - 16)

/* Where everything lives in one image, and the handful of places the
 * two versions genuinely differ. tools/profiles.py holds the same
 * fields with the same names and explains each one. An address of 0
 * means "this version does not have it". */
typedef struct {
    const char *name;
    uint16_t base;             /* where the image loads */
    uint16_t dispatch;         /* entry(c) = dispatch + 2*c */
    uint16_t rule_lo, rule_hi;
    uint16_t attrs;            /* the 128-byte attribute table */
    uint16_t punct_names;      /* {ptr_lo, ptr_hi, char} triples */
    uint8_t  punct_all;        /* where Ctrl-E A starts the search */
    uint8_t  punct_most;       /* ...and where Ctrl-E M does */
    uint16_t inbuf, phbuf, phlen;
    int      syllabifier;      /* v1.3 has none at all */
    int      v13_metas;        /* the reduced metacharacter set */
    uint16_t payload_pc;       /* for tools/rule_probe.py */

    uint16_t charmap;          /* Ctrl-V map, base + ord(c) - $20 */
    uint16_t pitch_off, vowel_var, allo_ptr;
    int      n_allophones;

    /* the machine variables the Ctrl-E commands set */
    uint16_t v_flags, v_base_pitch, v_monotone, v_compress, v_delay,
             v_group, v_variant, v_p_step, v_utt_flag;

    const char *commands;      /* which Ctrl-E letters this version has */
    int      capital_pitch;    /* $DB7F lifts a capital by 5 */
    int      frames_pre_dec;   /* $ECEC: eight frames, not ten */
    int      compress_emit_even;
    int      fwd_run_limit;    /* v1.3's fixed $8F; -1 = the buffer end */
    int      flow_every_n;     /* $E9CA every 8th byte; v1.3 every one */
    /* $E5CA: after $E6B0 has handled a code that is not an allophone,
     * does the engine RE-ENTER phoneme mode for the next character, or
     * return? 3.1.3 branches past the re-entry, which is why a lone ","
     * in a Ctrl-V string swallows the rest of the line. One Textalker
     * 3.1.2 build branches only as far as the re-entry and carries on.
     * Read from the branch displacement itself -- tk_image.c. */
    int      reenter_after_special;

    /* the syllabifier's four lists, v3.1.3 only */
    uint16_t prefixes, suffixes, digraphs, clusters;
} tk_profile;

/* One loaded image: the bytes, the profile that fits them, and the
 * startup values of the machine variables. */
typedef struct {
    uint8_t   rom[0x10000];    /* rom[a - base] for a >= base */
    size_t    rom_len;
    tk_profile p;

    const uint8_t *attrs;      /* rom + attrs - base, 128 bytes */

    /* $E1FA picks a section by sorted pointer bounds, so each one's
     * extent has to be worked out once at load time. */
    uint16_t  sec_start[0x100];   /* offset of the first pattern byte */
    uint16_t  sec_end[0x100];     /* one past the last byte to consider */
    uint8_t   sec_ok[0x100];      /* this character has a real section */

    /* The release, from the loader's banner -- "3.1.3", "1.3", "3.1.2"
     * -- or the family ("3.1.x") when there was no loader to read one
     * from. p.name points at this once the image is loaded. */
    char version[16];

    /* $9410-$9445 in v1.3's loader; v3.1.3's image carries its own. */
    uint8_t init_flags, init_base_pitch, init_monotone, init_compress,
            init_delay, init_group, init_variant, init_p_step,
            init_utt_flag;
} tk_image;

/* The engine's input buffer: $FDAC, which is 256 bytes that nothing
 * ever clears, plus $FD9E which says how much of it is this
 * utterance's. `len` is how much of the array is meaningful at all --
 * for the machine that is the whole 256, because a rule can read past
 * `end` into the previous utterance's tail and consume it. */
typedef struct {
    uint8_t b[TK_BUFSZ_MAX];
    int     len;
    int     end;               /* $FD9E */
    /* The machine's buffer size: TK_BUFSZ, or more in extended mode.
     * `len` is how much of the array is readable as leftovers and is
     * not the same thing -- the standalone walkers set it to the text
     * length, having no previous utterance to read into. */
    int     size;
} tk_buf;

/* ------------------------------------------------------------ tk_image.c */

/* Load an image. `ram_path` may be NULL, in which case v1.3's startup
 * values fall back to the OBJ bytes -- which are wrong, so pass it.
 * `want` names a version ("3.1.3", "1.3") or is NULL to detect.
 * Returns 0 on success; on failure fills `err` and returns -1. */
int tk_image_load(tk_image *img, const char *obj_path, const char *ram_path,
                  const char *want, char *err, size_t errsz);

/* The same from memory, for an image that came out of a disk image
 * rather than off the filesystem. `ram` may be NULL. */
int tk_image_load_mem(tk_image *img, const uint8_t *obj, size_t objn,
                      const uint8_t *ram, size_t ramn,
                      const char *want, char *err, size_t errsz);

/* rom byte at a 6502 address, 0 outside the image. */
uint8_t tk_peek(const tk_image *img, uint16_t addr);

/* ------------------------------------------------------- tk_syllable.c */

/* $DD10 over buf->b[start..buf->end), in place. Returns the new end.
 * Does nothing and returns buf->end unless the profile has a
 * syllabifier. */
int tk_syllabify_at(const tk_image *img, tk_buf *buf, int start);

/* The whole line, the way $DB5E and $DD10 build it together. */
void tk_build_line(const tk_image *img, const char *text, tk_buf *buf);

/* ---------------------------------------------------------- tk_rules.c */

/* $E1FA over a buffer the caller already built. Appends phoneme codes
 * to `out`, which holds at most `outcap`. Returns the number written,
 * or -1 if some character had no rule at all. `flags` is $FFAD. */
int tk_run_buffer(const tk_image *img, const tk_buf *buf, uint8_t flags,
                  uint8_t *out, int outcap);

/* ------------------------------------------------------------- the seam */

/* A letter-to-sound source: an input buffer in, phoneme codes out.
 *
 * This is what the whole rewrite was for. Stage 2 consumes a string of
 * one-byte phoneme codes and nothing else, so ANY source that can emit
 * that alphabet drops straight in -- a dictionary, a different rule
 * set, a G2P model -- without touching the synthesiser, the chip or the
 * library above them. Textalker's own $E1FA rules are simply the
 * default one.
 *
 * `run` appends nothing before out[0] and returns how many codes it
 * wrote, or -1 for "some character had no rule at all", which the
 * machine counts rather than swallows.
 *
 * The alphabet is documented in notes/phoneme_alphabet.md and
 * src/tk_arpabet.c implements it. In short: a code below $4E is a
 * vowel, $4E and above a consonant; the 26 low allophones carry three
 * pitch groups as code = index, index+$1A and index+$34, and everything
 * above index $19 has the single code index+$34. tk_word_separator()
 * gives the between-words code and $1A terminates. */
typedef struct {
    int (*run)(void *ctx, const tk_image *img, const tk_buf *buf,
               uint8_t flags, uint8_t *out, int outcap);
    void *ctx;
    const char *name;
} tk_g2p;

/* Textalker's rules, $E1FA. The default source; also what a partial
 * source hands unknown words back to. */
extern const tk_g2p tk_g2p_textalker;

/* $E1FA over buf[start..stop) only, appending at out[n], with the rest
 * of the buffer still visible as context. Returns the new n, or -1.
 * `end_cursor` may be NULL; otherwise it receives where the walk
 * actually stopped, which is past `stop` whenever a rule consumed more
 * characters than the range held. tk_rules.c has the detail. */
int tk_run_range(const tk_image *img, const tk_buf *buf, uint8_t flags,
                 int start, int stop, uint8_t *out, int n, int outcap,
                 int *end_cursor);

/* What the rules emit between words -- $19 in v3.1.3, $7F in v1.3 --
 * read out of the image rather than assumed. */
uint8_t tk_word_separator(const tk_image *img);

/* ------------------------------------------------------- tk_arpabet.c */

/* ARPABET <-> Textalker's codes. The correspondence was measured, not
 * guessed; tk_arpabet.c carries the word list it was read off and
 * notes/phoneme_alphabet.md the whole derivation. */

/* The code for an allophone index at a pitch group (1 low, 2 neutral,
 * 3 stressed). Groups only exist for indices below $1A. */
uint8_t tk_arpa_code(uint8_t idx, int group);

/* One ARPABET symbol, no stress digit, upper case. 0 if unknown. */
int tk_arpa_lookup(const char *sym, uint8_t *idx, int *is_vowel);

/* A whole pronunciation -- "K AE1 T", CMUdict's spelling, stress
 * digits and all. Returns codes written, or -1 with the offending
 * symbol in `bad`. */
int tk_arpabet_to_codes(const char *arpa, uint8_t *out, int cap,
                        char *bad, size_t badsz);

/* The other way, for reading back what any source produced. */
int tk_codes_to_arpabet(const uint8_t *codes, int n, char *out, size_t cap);

/* ---------------------------------------------------------- tk_dict.c */

/* A pronunciation dictionary in CMUdict format, read at RUN TIME from
 * a file the user supplies -- EchoTalk ships no dictionary, for the
 * same reason it ships no Textalker image. Words it does not know go
 * back to Textalker's own rules, so a dictionary is an override list
 * rather than a replacement. */
typedef struct tk_dict tk_dict;

tk_dict *tk_dict_open(const tk_image *img, const char *path,
                      char *err, size_t errsz);
void tk_dict_free(tk_dict *d);
const tk_g2p *tk_dict_g2p(tk_dict *d);
long tk_dict_entries(const tk_dict *d);
long tk_dict_hits(const tk_dict *d);
long tk_dict_misses(const tk_dict *d);

/* The 6502 cycles the last tk_run_buffer() would have taken.
 *
 * Stage 1 runs to completion BEFORE stage 2 sends a single byte, so on
 * real hardware its cost is one lump of time during which the chip
 * plays on. It is only about 3% of the total -- ECHOTALK_CYCLE_PROFILE
 * on the harness says 65% goes on the status polls and chip writes and
 * 26% on the bit packer -- but 3% is enough to move the chip's
 * sub-frame phase at an utterance restart, which is audible. */
unsigned long tk_stage1_cycles(void);

/* The syllabifier adds its own cost into the same accumulator. */
void tk_stage1_add(unsigned long cycles);
void tk_stage1_reset(void);

/* One line of text, syllabified and walked, with the $1A terminator
 * appended the way the carriage return's own rule supplies it. */
int tk_walk_line(const tk_image *img, const char *text, uint8_t flags,
                 uint8_t *out, int outcap);

/* The same, through any letter-to-sound source. NULL means Textalker's. */
int tk_walk_line_g2p(const tk_image *img, const tk_g2p *g, const char *text,
                     uint8_t flags, uint8_t *out, int outcap);

/* --------------------------------------------------------- tk_synth.c */

/* $E9AF and its byte accumulator, plus the bytes that come out. Bits go
 * into $FFC7 from the top through ROR, so the first bit written ends up
 * in bit 0 of the byte; $FFC5 is a walking sentinel that starts at 1
 * and signals the eighth bit when it falls out. */
/* Textalker's flow control, for a caller that owns a real chip.
 *
 * The byte STREAM does not depend on any of this, so the pure
 * byte-stream tools leave the sink NULL and nothing below runs. The
 * AUDIO does: when each byte arrives decides how full the FIFO is, and
 * $E9CA is where Textalker waits for the chip rather than overrunning
 * it. Without these the bytes are right and their timing is not.
 *
 *   $E9CA  before every Nth byte, wait for BL (buffer low, <= 8 bytes
 *          in the FIFO). N is 8 in 3.1.3, which keeps a $FD88 counter,
 *          and 1 in v1.3, which polls before every byte ($EC61).
 *   $E9D8  if BL is clear and the chip has stopped talking, the packer
 *          RETURNS without writing, abandoning the rest of its bits.
 *   $E9E3  wait until the chip stops talking.
 *   $E9EB  a busy delay proportional to the pitch group, after a pause.
 */
typedef struct {
    void (*byte)(void *ctx, uint8_t b);    /* $FC8B */
    int  (*wait_bl)(void *ctx);            /* $E9CA; 0 = $E9D8, give up */
    void (*wait_ts)(void *ctx);            /* $E9E3 */
    void (*delay)(void *ctx, uint8_t a);   /* $E9EB */
    /* $FCB3 AND #$80, the talk-status bit. $EA1A refuses to send a
     * SPEAK EXTERNAL while the chip is still talking, and SPEAK
     * EXTERNAL resets the FIFO -- so getting this wrong throws away
     * frames that were still queued. With no chip attached there is
     * nothing to ask and the answer is "not talking", which is what the
     * byte-stream tools have always assumed. */
    int  (*talking)(void *ctx);            /* $EA1A */
    /* 6502 cycles the port has just spent. The chip free-runs against
     * them on real hardware, so a caller driving a real chip has to let
     * it advance by the same amount -- otherwise every byte of a burst
     * arrives at the same instant and the chip sees a frame's worth of
     * data with no time passing between. */
    void (*spend)(void *ctx, unsigned cycles);
    void *ctx;
} tk_sink;

typedef struct {
    uint8_t *out;
    int      n, cap;
    uint8_t  acc;              /* $FFC7 */
    uint8_t  bitcnt;           /* $FFC5 */
    uint8_t  started;          /* $FFCA: $FF means "not speaking" */
    const tk_sink *sink;       /* NULL for the byte stream alone */
    uint8_t  fc_count;         /* $FD88, the every-Nth-byte counter */
    int      abandoned;        /* $E9D8 fired; should never happen */
} tk_chip;

/* The 6502 has ONE set of these, not one per utterance: the pitch
 * group, the pitch offset, the vowel variant, the running pitch, the
 * previous frame and even the X register all carry across a flush.
 * Ctrl-E L and K flush every character, so rebuilding them from the
 * image each time is wrong everywhere but the first utterance. */
typedef struct {
    int     valid;
    uint8_t group, index, pitch, p_off, glide, variant, p_set,
            prev_flags, ef;
    uint8_t prev[10], k[10];
} tk_carry;

/* The part of the machine state stage 2 reads and writes. */
typedef struct {
    uint8_t flags;             /* $FFAD */
    uint8_t pitch;             /* $FFAF, the base pitch */
    uint8_t monotone;          /* $FFB0 */
    uint8_t compress;          /* $FFB1 */
    uint8_t delay;             /* $FFBB */
    uint8_t volume;            /* $FFC3, the running energy step */
    tk_carry carry;
} tk_voice;

void tk_chip_init(tk_chip *c, uint8_t *out, int cap);
/* Attach a real chip. Must be called before the first utterance. */
void tk_chip_attach(tk_chip *c, const tk_sink *sink);
void tk_voice_init(const tk_image *img, tk_voice *v);

/* The phoneme buffer at $FEAD. Nothing ever clears it, and it is not
 * initialised at boot either, so it starts as whatever the image
 * carries at that address -- which for v1.3 is not zeros. */
void tk_phbuf_init(const tk_image *img, uint8_t *phbuf, int phbufsz);

/* $DBD6: speak the phoneme string stage 1 produced. `phbuf` is the
 * persistent 256-byte buffer; the codes are copied into it. */
void tk_speak_codes(const tk_image *img, tk_voice *v, tk_chip *chip,
                    const uint8_t *codes, int ncodes, uint8_t *phbuf,
                    int phbufsz);

/* $E5AA: Ctrl-V phoneme mode, which runs one character behind. */
void tk_speak_phonemes(const tk_image *img, tk_voice *v, tk_chip *chip,
                       const char *text);

/* ------------------------------------------------------- tk_machine.c */

/* Textalker's settable state and the input parser over it, $D8D4-$DBA3.
 * Owns both buffers, because neither is ever cleared and the leftovers
 * are readable -- and audible. */
typedef struct {
    const tk_image *img;
    tk_voice v;                /* what stage 2 reads and writes */
    tk_chip  chip;
    const tk_g2p *g2p;         /* the seam; &tk_g2p_textalker by default */

    uint8_t  intro;            /* $FFAE, the command character */
    uint8_t  spell;            /* $FFB2; v1.3 has no K or N */
    uint8_t  arg;              /* $FFBE */
    uint8_t  repeat;           /* $FFF2; v1.3 has no R */
    uint8_t  marks[3];         /* $FFF5-$FFF7, the index marks */

    int      bufsz;            /* TK_BUFSZ, or more in extended mode */
    tk_buf   buf;              /* $FDAC, with $FD9E in buf.end */
    int      word;             /* $FD9C: a word is open */
    int      wstart;           /* $FDA6 */
    uint8_t  saved_pitch;      /* $FD7E */
    uint8_t  phbuf[TK_BUFSZ_MAX];   /* $FEAD */

    char     phon[TK_BUFSZ];   /* the Ctrl-V accumulator */
    int      nphon;

    /* Every character that reached stage 1 and found no rule at all.
     * It should stay zero: the last rule of every real section is a
     * catch-all. A limit that can swallow speech has to report it. */
    long     rule_failures;
} tk_machine;

void tk_machine_init(tk_machine *m, const tk_image *img, uint8_t *out, int cap);

/* Grow the line buffer past Textalker's own 256 bytes. See TK_BUFSZ_MAX
 * above for what that trades away. Must be called before any text; a
 * size outside [TK_BUFSZ, TK_BUFSZ_MAX] is refused and -1 returned. */
int tk_machine_set_buffer_size(tk_machine *m, int bytes);

/* Replace the letter-to-sound source. NULL restores Textalker's rules.
 * Must outlive the machine; the machine does not own it. */
void tk_machine_set_g2p(tk_machine *m, const tk_g2p *g);

/* $D8D4 down. Raw input bytes -- Echo control codes and text, high bit
 * NOT set; this sets it. Chip bytes accumulate in the buffer given to
 * tk_machine_init, and m->chip.n says how many there are. */
void tk_machine_feed(tk_machine *m, const uint8_t *payload, int n);

/* $DBA3: run both stages over the buffer, then reset it. */
void tk_machine_flush(tk_machine *m);

#endif /* TK_H */
