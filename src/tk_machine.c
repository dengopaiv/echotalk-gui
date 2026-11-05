/*
 * tk_machine.c -- the driver: raw input bytes to TMS5220 bytes.
 *
 * A translation of tools/textalker.py. This is the top of the rewrite:
 * it owns the machine state -- the variables the Ctrl-E commands set --
 * parses the input stream the way $D8D4-$DBA3 does, and drives both
 * stages.
 *
 * The per-character chain, from $D8D4 down:
 *
 *   $D8D4  Ctrl-V ($16) enters phoneme mode; a carriage return there is
 *          turned into "." and leaves it again
 *   $D917  the command parser: the introducer (default Ctrl-E, held in
 *          $FFAE), then digits into $FFBE, then a letter dispatched
 *          through the table at $D9A2
 *   $DACE  otherwise text, buffered at $FDAC and spoken at a carriage
 *          return, at $80, or when the buffer reaches $F0
 *
 *   nP  pitch, 0-63 (default 24), with intonation     $FFAF, $FFB0 = 0
 *   nF  the same pitch but monotone                   $FFAF, $FFB0 = $FF
 *   nV  volume 0-15 (default 12)                      $FFC3 = (n & 15) - 12
 *   nD  inter-word delay 0-15 (v3.1.3 only)           $FFBB
 *   nR  repeat filter                                 $FFF2 = (n or $80) - 1
 *   C   compressed: drop the interpolated frames      $FFB1 = $FF
 *   E   expanded: keep them (the default)             $FFB1 = 0
 *   A   ALL punctuation, spoken by name               $FFAD |= $20
 *   M   MOST punctuation                              $FFAD: -$20 +$10
 *   S   SOME punctuation -- the default               $FFAD: -$30
 *   L   letter mode: every character alone, and an UPPER-CASE letter
 *       raises the pitch by 5 ($DB22 stores $FFAF into $FD7E only for
 *       $C1-$DA; lower case folds through $DB19 and gets $FF, which
 *       $DB7D reads as "no bump")
 *   W   word mode (the default)                       $FFAD &= ~$40
 *   K   spell UPPER-CASE letters: plant an $80 before each one
 *   N   stop spelling (the default)
 *   T   Talk only: speak, do not echo to the screen   $FFAD: +$01 -$08
 *   O   Off: echo to the screen, do not speak         $FFAD: -$01 +$08
 *   B   Both: speak AND echo to the screen            $FFAD |= $09
 *
 * $FFAD bit 0 is "speak" and bit 3 is "echo to the screen", which is
 * why those three commands are the four combinations minus the useless
 * one. They exist so a program can say one thing and print another.
 * Only bit 0 concerns a speech rewrite; there is no screen here, so O
 * simply produces nothing.
 *
 * Three things about the buffers had to be got right, and getting one
 * without the others is worse than none -- the persistent buffer alone
 * dropped the command fuzz from 98% to 75%:
 *
 * - NEITHER BUFFER IS EVER CLEARED. A flush resets only the LENGTH, so
 *   the tail of the previous utterance stays where it is and is still
 *   readable. Neither is initialised at boot either: before anything is
 *   spoken they hold whatever the image carries at those addresses.
 * - THE LITERAL RUN READS PAST THE END; THE CONTEXTS DO NOT.
 * - $E247 TESTS THE CURSOR FOR EQUALITY with $FD9E. Put the three
 *   together and a rule that consumes more characters than are left
 *   steps straight over the end, the test never fires, and stage 1
 *   walks on through the leftovers until it stops at 240 phonemes.
 *   "VWVOUQDPWO45DZAA QW" does exactly that. It is audible, and
 *   reproducing it is the point.
 */
#include <string.h>

#include "tk.h"

void tk_machine_init(tk_machine *m, const tk_image *img, uint8_t *out, int cap)
{
    int i;
    memset(m, 0, sizeof *m);
    m->img = img;
    tk_voice_init(img, &m->v);
    tk_chip_init(&m->chip, out, cap);
    m->g2p = &tk_g2p_textalker;          /* the seam, at its default */
    m->intro = 0x05;                     /* $FFAE */
    m->saved_pitch = 0xFF;               /* $FD7E */
    m->bufsz = TK_BUFSZ;
    m->buf.size = m->bufsz;
    m->buf.len = m->bufsz;
    m->buf.end = 0;
    for (i = 0; i < m->bufsz; i++)
        m->buf.b[i] = tk_peek(img, (uint16_t)(img->p.inbuf + i));
    tk_phbuf_init(img, m->phbuf, m->bufsz);
}

/* Put a different letter-to-sound source in. NULL restores Textalker's
 * own rules. Everything else about the machine is unchanged: the
 * command set, both buffers, the syllabifier and stage 2 all still run,
 * so a source is responsible only for turning the buffer into codes. */
/* Grow the line buffer. See TK_BUFSZ_MAX in tk.h for what it trades
 * away. Everything derived from the size is recomputed, including the
 * bytes the buffers are SEEDED with -- Textalker never clears either of
 * them, so what lies past the text is readable and audible, and a
 * buffer bigger than the image's own reads zero out there. */
int tk_machine_set_buffer_size(tk_machine *m, int bytes)
{
    int i;
    if (bytes < TK_BUFSZ || bytes > TK_BUFSZ_MAX) return -1;
    m->bufsz = bytes;
    m->buf.size = bytes;
    m->buf.len = bytes;
    m->buf.end = 0;
    m->word = 0;
    m->wstart = 0;
    for (i = 0; i < bytes; i++)
        m->buf.b[i] = tk_peek(m->img, (uint16_t)(m->img->p.inbuf + i));
    tk_phbuf_init(m->img, m->phbuf, bytes);
    return 0;
}

void tk_machine_set_g2p(tk_machine *m, const tk_g2p *g)
{
    m->g2p = g ? g : &tk_g2p_textalker;
}

static int has_command(const tk_image *img, char c)
{
    return strchr(img->p.commands, c) != NULL;
}

static void command(tk_machine *m, char letter)
{
    uint8_t n = m->arg, f = m->v.flags;
    switch (letter) {
    case 'A': m->v.flags = (uint8_t)(f | 0x20); break;
    case 'B': m->v.flags = (uint8_t)(f | 0x09); break;
    case 'C': m->v.compress = 0xFF; break;
    case 'D': m->v.delay = n; break;
    case 'E': m->v.compress = 0x00; break;
    case 'F': m->v.pitch = n; m->v.monotone = 0xFF; break;
    case 'K': m->spell = 0xFF; break;
    case 'L': m->v.flags = (uint8_t)(f | 0x40); break;
    case 'M': m->v.flags = (uint8_t)((f & 0xDF) | 0x10); break;
    case 'N': m->spell = 0x00; break;
    case 'O':
        tk_machine_flush(m);
        m->v.flags = (uint8_t)((m->v.flags & 0xFE) | 0x08);
        break;
    case 'P': m->v.pitch = n; m->v.monotone = 0x00; break;
    case 'R': m->repeat = (uint8_t)((n ? n : 0x80) - 1); break;
    case 'S': m->v.flags = (uint8_t)(f & 0xCF); break;
    case 'T': m->v.flags = (uint8_t)((f & 0xF7) | 0x01); break;
    case 'V': m->v.volume = (uint8_t)((n & 0x0F) - 0x0C); break;
    case 'W': m->v.flags = (uint8_t)(f & 0xBF); break;
    case ';': m->marks[0] = n; m->arg = 0; break;
    case ',': m->marks[1] = n; m->arg = 0; break;
    case 'I': m->marks[2] = n; break;
    default: break;
    }
}

void tk_machine_flush(tk_machine *m)
{
    static uint8_t codes[TK_BUFSZ_MAX + 16];
    int n;

    if (!m->buf.end) return;
    m->buf.len = m->bufsz;
    /* The seam. m->g2p is Textalker's own $E1FA rules unless a host has
     * put something else there; everything downstream of this line --
     * the synthesiser, the chip, the library -- neither knows nor
     * cares which. */
    n = m->g2p->run(m->g2p->ctx, m->img, &m->buf, m->v.flags,
                    codes, (int)sizeof codes);
    if (n < 0) {
        /* No rule matched at all, which should be impossible: the last
         * rule of every real section is a catch-all. Counted rather
         * than swallowed, because speech that vanishes in silence is
         * the one failure this project has been bitten by twice. */
        m->rule_failures++;
        n = 0;
    }
    /* Stage 1 has just run to completion, and on real hardware the
     * chip played on throughout. Hand that time over before stage 2
     * sends its first byte, or the chip starts the utterance in the
     * wrong part of its interpolation period. */
    if (m->chip.sink && m->chip.sink->spend)
        m->chip.sink->spend(m->chip.sink->ctx,
                            (unsigned)tk_stage1_cycles());
    tk_stage1_reset();
    tk_speak_codes(m->img, &m->v, &m->chip, codes, n, m->phbuf, m->bufsz);
    m->buf.end = 0;
    m->word = 0;
    m->wstart = 0;
    if (!(m->saved_pitch & 0x80))        /* $DBCD */
        m->v.pitch = m->saved_pitch;
}

/* $DACE-$DBA2: buffer one character, syllabifying each word as it
 * closes, and speak when something says to. */
static void text_char(tk_machine *m, uint8_t c)
{
    int word_char;

    if (!(m->v.flags & 0x01)) return;             /* $DACE: not accepting */
    if (m->spell && c >= 0xC1 && c <= 0xDA && m->buf.end != m->bufsz - 2) {
        /* $DAF9 only PLACES the $80 in the buffer; it does not speak.
         * The letter is stored after it and the mark travels with the
         * text into stage 1. */
        m->buf.b[m->buf.end++] = 0x80;
    }
    if (c >= 0xC1 && c <= 0xDA) {                 /* $DB03: case folding */
        m->saved_pitch = m->v.pitch;              /* $DB22 */
    } else {
        if (c >= 0xE1 && c <= 0xFA) {
            c = (uint8_t)(c - 0x20);
            if (c >= 0xDB) c = (uint8_t)(c + 6);
        }
        m->saved_pitch = 0xFF;                    /* $DB1A */
    }
    word_char = (c == 0xA7) || (c >= 0xB0 && c < 0xBA) || (c >= 0xC1 && c < 0xDB);
    if (word_char) {
        m->word = 0xFF;                           /* $DB41 */
    } else if (m->word) {                         /* $DB4D: the word closed */
        if (m->img->p.syllabifier) {
            m->buf.len = m->bufsz;
            tk_syllabify_at(m->img, &m->buf, m->wstart);
        }
        m->word = 0;
    }
    m->buf.b[m->buf.end++] = c;                   /* $DB55 */
    if (!m->word) m->wstart = m->buf.end;         /* $DB67 */
    if (m->buf.end >= TK_CAP_OF(m->bufsz)) {                      /* $DB6A */
        tk_machine_flush(m);
        return;
    }
    if (m->v.flags & 0x40) {                      /* $DB6E: letter mode */
        /* $DB77 writes the carriage return at $FDAC,X without bumping
         * $FD9E, so it is past the end and never spoken; and because
         * the word logic at $DB2D never ran for a word character,
         * letter mode speaks each character UNsyllabified. */
        if (m->img->p.capital_pitch && !(m->saved_pitch & 0x80))
            m->v.pitch = (uint8_t)(m->saved_pitch + 5);            /* $DB7F */
        tk_machine_flush(m);
        return;
    }
    if (c == 0xDD || c == 0xBE) {                 /* $DB8A: "]" or ">" alone */
        if (m->buf.end == 1) tk_machine_flush(m);
        return;
    }
    if (c == 0x8D || c == 0x80) tk_machine_flush(m);   /* $DB9A: CR, or $80 */
}

/* What $D8D4's chain costs per input character before anything else
 * happens: the Ctrl-V test, the phoneme-mode and command-flag tests,
 * the introducer compare, and for text $DACE-$DBA2's case folding,
 * word tracking and buffering. Measured rather than counted -- with
 * ECHOTALK_CHIP_BYTES, an input that produces almost no stage-1 work
 * ("T" then one letter) puts its first chip byte 76 samples in,
 * and that is ~24 characters of settings block and text, so about 390
 * cycles each. Two independent inputs agree on it. */
#define TK_DRIVER_CYCLES 390

void tk_machine_feed(tk_machine *m, const uint8_t *payload, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (m->chip.sink && m->chip.sink->spend)
            m->chip.sink->spend(m->chip.sink->ctx, TK_DRIVER_CYCLES);
        /* The Apple II's input routines hand Textalker characters with
         * the high bit set; $FD90 always holds them that way. */
        uint8_t c = (uint8_t)(payload[i] | 0x80);
        uint8_t low = (uint8_t)(c & 0x7F);

        if (low == 0x16) {                        /* $D8D4: Ctrl-V */
            tk_machine_flush(m);
            m->v.flags |= 0x02;
            m->arg = 0;
            continue;
        }
        if (m->v.flags & 0x02) {                  /* $D8EF: phoneme mode */
            if (c == 0x8D) {
                m->phon[m->nphon] = 0;
                tk_speak_phonemes(m->img, &m->v, &m->chip, m->phon);
                m->nphon = 0;
                m->v.flags &= 0xFD;
            } else if (m->nphon < TK_BUFSZ - 1) {
                m->phon[m->nphon++] = (char)low;
            }
            continue;
        }
        if (m->v.flags & 0x04) {                  /* $D93C: inside a command */
            uint8_t a;
            if (low < 0x20 && low != 0x0D) {
                m->intro = low;                   /* $D949: a new introducer */
                m->v.flags &= 0xFB;
                continue;
            }
            if (c >= 0xB0 && c < 0xBA) {          /* $D959: another digit */
                /* $D972's BEQ tests the Z flag from the multiply loop's
                 * DEX, not from the STA before it -- STA sets no flags
                 * -- so it is ALWAYS taken and a digit always leaves
                 * the command open. Reading it as "branch when the
                 * accumulator is zero" makes every multi-digit command
                 * end at its first digit. */
                m->arg = (uint8_t)((m->arg & 0x0F) * 10 + (c & 0x0F));
                continue;
            }
            a = c;
            if (a >= 0xE1 && a < 0xFB) a &= 0xDF; /* $D97C: fold the letter */
            if (has_command(m->img, (char)(a & 0x7F)))
                command(m, (char)(a & 0x7F));
            m->v.flags &= 0xFB;                   /* $D996: the command ends */
            continue;
        }
        if (low == m->intro) {                    /* $D923 */
            tk_machine_flush(m);
            m->v.flags |= 0x04;
            m->arg = 0;
            continue;
        }
        text_char(m, c);
    }
}
