/*
 * tk_synth.c -- phoneme codes to TMS5220 bytes, $DBD6-$EA43.
 *
 * A translation of tools/allophone.py, which carries the reasoning.
 * Like stage 1 this is a transcription of the 6502, and every table is
 * read out of the user's own image:
 *
 *   $EA44  phoneme-mode character map, entry(c) = $EA24 + ord(c)
 *   $EA7F  pitch offset per group    $EA89  vowel variant per group
 *   $EA93  58 allophone pointers     $E906  the ten K bit widths
 *
 * Routines, following notes/disasm_stage2.txt:
 *
 *   $DBD6  the driver: walk the phoneme buffer, split each code into an
 *          allophone index and a pitch group, call $E5C7
 *   $E5AA  the Ctrl-V phoneme-mode entry, via $E5E3 and $E623
 *   $E5C7  speak one allophone
 *   $E6B0  the codes that are not allophones at all
 *   $E674  a stop consonant before a non-vowel takes the +8 variant
 *   $E713  this frame's pitch offset and vowel variant
 *   $E7A3  read the frames and emit the selected ones
 *   $E7ED  unpack one packed frame     $E893  emit one, interpolating
 *   $E945  the running pitch           $E9AF  the bit packer
 *
 * An allophone's data is a bitmask followed by packed frames. A frame
 * is six bytes when bit 7 of its first byte is set (voiced: energy,
 * pitch and K1-K10) and three when it is clear (unvoiced: energy and
 * K1-K4 only, which is exactly what the chip wants). The mask says
 * which of the frames to actually emit, so one allophone's data serves
 * several lengths, and the 26 low allophones carry three masks, one per
 * pitch group -- which is where the intonation lives.
 *
 * Five things had to be read rather than guessed, and each was worth a
 * failing family; they are marked in place below.
 */
#include <string.h>

#include "tk.h"

#define SPEAK_EXTERNAL 0x60

/* The whole of $FFAD-$FFE1, under readable names. Built fresh for each
 * utterance and then handed back to the voice, because most of it
 * carries across a flush. */
typedef struct {
    const tk_image *img;
    tk_chip *chip;
    tk_voice *v;

    uint8_t base_pitch;        /* $FFAF */
    uint8_t flags;             /* $FFAD */
    uint8_t ffb0;              /* $FFB0 monotone: F sets it, P clears it */
    uint8_t ffb1;              /* $FFB1 compress: C sets it, E clears it */
    uint8_t ffbb;              /* $FFBB the D inter-word delay */
    uint8_t group;             /* $FFBC */
    uint8_t index;             /* $FFBD the allophone index */
    uint8_t pitch;             /* $FFBF the running pitch */
    uint8_t p_off;             /* $FFC0 */
    uint8_t glide;             /* $FFC1 */
    uint8_t variant;           /* $FFC2 */
    uint8_t p_step;            /* $FFC3, the V command and + and - */
    uint8_t p_set;             /* $FFC4 */
    uint8_t mask;              /* $FFC8 */
    int     nframe;            /* $FFC9 */
    uint8_t prev_flags;        /* $FFCC */
    uint8_t prev[10];          /* $FFCD-$FFD6 */
    uint8_t ef;                /* $FFD7, flags | energy */
    uint8_t k[10];             /* $FFD8-$FFE1, K1-K10 */
    uint8_t ch;                /* $FD90, the current character */
    /* The 6502 X register, which $E756 reads $EA89 through WITHOUT
     * having set it on every path: when $FFC4 is non-zero, $E713 takes
     * the $E733 branch and never does LDX $FFBC, so the vowel variant
     * comes from $EA89 plus whatever X was left holding. Normally that
     * is the previous pitch group, but after "1N" it is the character
     * index, and $EA89+$2E is allophone data. */
    uint8_t x;
} synth;

/* ------------------------------------------------------------- the chip */

void tk_chip_init(tk_chip *c, uint8_t *out, int cap)
{
    c->out = out;
    c->n = 0;
    c->cap = cap;
    c->acc = 0x00;
    c->bitcnt = 0x01;
    c->started = 0xFF;
    c->sink = NULL;
    c->fc_count = 8;           /* $FD88 */
    c->abandoned = 0;
}

void tk_chip_attach(tk_chip *c, const tk_sink *sink)
{
    c->sink = sink;
}

static void chip_byte(tk_chip *c, uint8_t v)     /* JSR $FC8B */
{
    if (c->n < c->cap) c->out[c->n] = v;
    c->n++;
    if (c->sink && c->sink->byte) c->sink->byte(c->sink->ctx, v);
}

/* What one turn of $E9AF's bit loop costs the 6502, so a caller with a
 * real chip can let it run for the same time.
 *
 *   $E9B2 ASL 6, $E9B5 ROR 6, $E9B8 ASL 6, $E9BB BCC 3 taken,
 *   $E9DF DEX 2, $E9E0 BNE 3                                   = 26
 *
 * and when that bit completes a byte the branch falls through instead:
 *
 *   ...BCC 2 not taken, $E9BD ROL 6, $E9C0 DEC 6, $E9C3 BNE 3,
 *   $E9D9 LDA 4, $E9DC JSR $FC8B 6+27, $E9DF DEX 2, $E9E0 BNE 3 = 77
 *
 * with $FC8B being STA 4, LDA 4, CMP 2, BEQ 3, LDA 4, STA 4, RTS 6. */
#define TK_BIT_CYCLES  26
#define TK_BYTE_CYCLES 77

/* And what the CALLER spends around each of those calls. $E8DC's loop,
 * which emits the ten K coefficients and so accounts for most calls:
 *
 *   LDX 4, LDA abs,X 4, STA abs,X 5, ASL 2, PHA 3, LDA abs,X 4, TAX 2,
 *   PLA 4, JSR 6, $E9AF's STA 4, its RTS 6, INC 6, LDA 4, CMP 2,
 *   BNE 3, LDA 4, CMP 2, BCC 3                                   = 68
 *
 * A byte spans about two K coefficients, so 259 + 2*68 = 395 cycles a
 * byte -- and 395, 373 and 400 are the three commonest gaps in the
 * emulator's own cycle dump, against a floor of 291. The energy and
 * pitch fields have their own call sites with slightly different
 * overheads; this uses the K loop's for all of them, because it is
 * ten calls of the twelve. */
/* Each CALL SITE has its own cost, and one flat number over-charges
 * some and under-charges others -- which accumulates into a phase
 * error over a long utterance.
 *
 *   $E8DC  the K loop: LDX 4, LDA abs,X 4, STA abs,X 5, ASL 2, PHA 3,
 *          LDA abs,X 4, TAX 2, PLA 4, JSR 6, $E9AF's STA 4 and RTS 6,
 *          INC 6, LDA 4, CMP 2, BNE 3, LDA 4, CMP 2, BCC 3      = 68
 *   $E8C4  the energy write: LDA 4, ASL 2, ASL 2, LDX 2, JSR 6,
 *          STA 4, RTS 6                                          = 26
 *   $E945  the pitch: 9 to the branch, 14 at $E95F, 10 at $E969,
 *          the clamps 11, $E982's complement 10, LDX 2, the calls
 *          in and out 24                                         = 84
 */
#define TK_CALL_K      68
#define TK_CALL_ENERGY 26
#define TK_CALL_PITCH  84

/* $E5AA's loop, which is what phoneme mode spends per character before
 * any allophone is reached: the range checks at $E5AF-$E5B8, the call
 * into $E5E3 and out, $E623's character-map lookup, and $E5C7's chain
 * down to $E713. Text mode has no equivalent -- $DBD6 walks a buffer of
 * codes in a tight loop -- which is why every case that disagreed with
 * the emulator had a Ctrl-V in it and none of the others did. */
#define TK_PHONEME_CYCLES 110

/* $E7ED, which runs for all ten frames of an allophone whether or not
 * the mask emits them -- so it is the largest single block of 6502 time
 * the port was not charging for. Counted from the listing: the INY, the
 * variant clamp, the energy arithmetic at $E7FE-$E815 and the three
 * byte extractions up to $E858's BPL come to about 168 cycles, and the
 * three further extractions on the voiced path add about 62.
 *
 * $E7D7's loop around it -- JSR unpack, JSR $E893, DEC, BPL -- plus
 * what $E893 spends deciding, is about another 55. */
#define TK_UNPACK_CYCLES 168
#define TK_VOICED_CYCLES 62
#define TK_FRAME_CYCLES  55

static void chip_bits(tk_chip *c, uint8_t a, int n)     /* $E9AF */
{
    int i;
    for (i = 0; i < n; i++) {
        uint8_t carry = (uint8_t)((a >> 7) & 1);        /* ASL $FFC6 */
        int full;
        a = (uint8_t)(a << 1);
        c->acc = (uint8_t)((c->acc >> 1) | (carry << 7));   /* ROR $FFC7 */
        full = c->bitcnt & 0x80;                        /* ASL $FFC5 */
        c->bitcnt = (uint8_t)(c->bitcnt << 1);
        if (c->sink && c->sink->spend)
            c->sink->spend(c->sink->ctx,
                           full ? TK_BYTE_CYCLES : TK_BIT_CYCLES);
        if (full) {
            c->bitcnt = 0x01;                           /* ROL brings it back */
            /* $E9C0: DEC the counter, and on zero wait for the chip.
             * Purely a timing matter -- the byte stream is the same
             * either way -- so with no sink attached none of it runs. */
            if (--c->fc_count == 0) {
                c->fc_count = 8;
                if (c->sink && c->sink->wait_bl
                    && !c->sink->wait_bl(c->sink->ctx)) {
                    c->abandoned++;                     /* $E9D8 */
                    return;
                }
            }
            chip_byte(c, c->acc);
        }
    }
}

/* $E9AF counts down through X and so always returns with X at zero.
 * That matters because $E756 indexes $EA89 with X. */
static void spend(synth *s, unsigned cycles)
{
    if (s->chip->sink && s->chip->sink->spend)
        s->chip->sink->spend(s->chip->sink->ctx, cycles);
}

static void bits(synth *s, uint8_t a, int n)
{
    /* v1.3's $EC61 polls before EVERY byte; 3.1.3's $E9C0 keeps a
     * counter and polls before every eighth. Setting the counter to 1
     * here is exactly that. */
    if (s->img->p.flow_every_n == 1) s->chip->fc_count = 1;
    chip_bits(s->chip, a, n);
    s->x = 0;
}

/* ------------------------------------------------------------- the voice */

void tk_voice_init(const tk_image *img, tk_voice *v)
{
    memset(v, 0, sizeof *v);
    v->flags = img->init_flags;
    v->pitch = img->init_base_pitch;
    v->monotone = img->init_monotone;
    v->compress = img->init_compress;
    v->delay = img->init_delay;
    v->volume = img->init_p_step;
    v->carry.valid = 0;
}

void tk_phbuf_init(const tk_image *img, uint8_t *phbuf, int phbufsz)
{
    int i;
    for (i = 0; i < phbufsz; i++)
        phbuf[i] = tk_peek(img, (uint16_t)(img->p.phbuf + i));
}

static void synth_begin(synth *s, const tk_image *img, tk_voice *v,
                        tk_chip *chip)
{
    memset(s, 0, sizeof *s);
    s->img = img;
    s->chip = chip;
    s->v = v;
    s->base_pitch = v->pitch;
    s->flags = v->flags;
    s->ffb0 = v->monotone;
    s->ffb1 = v->compress;
    s->ffbb = v->delay;
    s->group = img->init_group;
    s->index = 0;
    s->pitch = s->base_pitch;
    s->variant = img->init_variant;
    s->p_step = v->volume;
    s->x = s->group;
    /* The restore has to come LAST, after every field above has been
     * given its default: putting it earlier lets those defaults
     * overwrite the carried state, which is a one-bit difference in
     * letter mode and 30 points of agreement. */
    if (v->carry.valid) {
        const tk_carry *c = &v->carry;
        s->group = c->group;
        s->index = c->index;
        s->pitch = c->pitch;
        s->p_off = c->p_off;
        s->glide = c->glide;
        s->variant = c->variant;
        s->p_set = c->p_set;
        s->prev_flags = c->prev_flags;
        s->ef = c->ef;
        memcpy(s->prev, c->prev, sizeof s->prev);
        memcpy(s->k, c->k, sizeof s->k);
    }
}

static void synth_save(synth *s)
{
    tk_carry *c = &s->v->carry;
    c->valid = 1;
    c->group = s->group;
    c->index = s->index;
    c->pitch = s->pitch;
    c->p_off = s->p_off;
    c->glide = s->glide;
    c->variant = s->variant;
    c->p_set = s->p_set;
    c->prev_flags = s->prev_flags;
    c->ef = s->ef;
    memcpy(c->prev, s->prev, sizeof c->prev);
    memcpy(c->k, s->k, sizeof c->k);
    s->v->flags = s->flags;
}

/* ------------------------------------------------------------- plumbing */

/* $EA1A, in full. The status read at the top is not decoration:
 *
 *   $EA1A JSR $FCB3 / AND #$80 / BNE $EA43   still talking -> do nothing
 *   $EA21 LDA $FFCA / CMP #$FF / BNE $EA43   not stopped -> do nothing
 *   $EA28 LDA #$60 / JSR $FC8B               SPEAK EXTERNAL
 *   $EA2D pitch = base pitch, prev_flags = 1, started = 1, bitcnt = 1
 *   $EA3E $FD88 = 8                          the flow-control counter
 *
 * SPEAK EXTERNAL RESETS THE CHIP'S FIFO, so sending one while frames
 * are still queued discards them and the chip decodes what is left as
 * a partial frame. Textalker never does: it waits until the chip has
 * genuinely stopped. Reading only $FFCA and ignoring the status is what
 * made the port's longer speech break up into malformed-frame noise. */
static void start(synth *s)
{
    const tk_sink *snk = s->chip->sink;
    if (snk && snk->talking && snk->talking(snk->ctx)) return;   /* $EA1F */
    if (s->chip->started == 0xFF) {
        chip_byte(s->chip, SPEAK_EXTERNAL);
        s->pitch = s->base_pitch;
        s->prev_flags = 1;
        s->chip->started = 1;
        s->chip->acc = 0;
        s->chip->bitcnt = 0x01;
        s->chip->fc_count = 8;                                   /* $EA3E */
    }
}

static void stop_frames(synth *s)                /* $E98F */
{
    int i;
    for (i = 0; i < 6; i++) {
        s->chip->started = 0xFF;
        spend(s, TK_CALL_K);          /* $E98F's loop, near enough the same */
        bits(s, 0xFF, 8);
    }
    s->glide = 0;
    s->variant = 0;
    s->p_set = 0;
}

static void set_pitch(synth *s)                  /* $E611 */
{
    s->p_set = s->group;
    s->pitch = s->base_pitch;
    s->glide = 0;
}

/* --------------------------------------------------------- the frames */

/* $E7ED: one packed frame from data[y+1:]. Returns the new y.
 *
 * `data` is the image from the allophone's pointer onward, not a fixed
 * window: Ctrl-E D leaves the delay in $FFBC ($E6CA), so $EA89,X can be
 * read far past its table and hand back a vowel variant of almost
 * anything, and ($04),Y then walks wherever that points. */
static int unpack(synth *s, uint16_t data, int y)
{
    const tk_image *img = s->img;
    uint8_t b0, b1, b2, b3, b4, b5;
    unsigned e;
    int carry;

    y++;
    if (s->chip->sink && s->chip->sink->spend)
        s->chip->sink->spend(s->chip->sink->ctx, TK_UNPACK_CYCLES);
    if (s->variant) s->variant = 1;
    b0 = tk_peek(img, (uint16_t)(data + y));
    /* $E7FE-$E804 is CLC, ADC $FFC3, ADC $FFC2 -- and the second ADC
     * takes the carry out of the first, so a p_step of $FF (one '-')
     * borrows and then gives the borrow straight back. Dropping that
     * carry makes every frame after a '-' one energy step too quiet. */
    e = (unsigned)((b0 & 0x3C) >> 2) + s->p_step;
    carry = e > 0xFF;
    e &= 0xFF;
    e = (e + s->variant + (carry ? 1 : 0)) & 0xFF;
    if (e == 0 || e > 0x7F) e = 1;               /* BEQ / BMI */
    else if (e >= 0x0F) e = 0x0E;
    s->ef = (uint8_t)(((e << 2) & 0xFF) | (b0 & 0xC0));

    y++;
    b1 = tk_peek(img, (uint16_t)(data + y));
    s->k[0] = (uint8_t)(((((b0 & 0x03) << 3) | (b1 >> 5)) << 2) & 0xFF);
    s->k[1] = (uint8_t)(((b1 & 0x1F) << 2) & 0xFF);
    y++;
    b2 = tk_peek(img, (uint16_t)(data + y));
    s->k[2] = (uint8_t)((b2 & 0xF0) >> 1);
    s->k[3] = (uint8_t)((b2 & 0x0F) << 3);
    if (!(s->ef & 0x80)) return y;               /* $E858: unvoiced, 3 bytes */
    if (s->chip->sink && s->chip->sink->spend)
        s->chip->sink->spend(s->chip->sink->ctx, TK_VOICED_CYCLES);
    y++;
    b3 = tk_peek(img, (uint16_t)(data + y));
    s->k[4] = (uint8_t)((b3 & 0xF0) >> 1);
    s->k[5] = (uint8_t)((b3 & 0x0F) << 3);
    y++;
    b4 = tk_peek(img, (uint16_t)(data + y));
    s->k[6] = (uint8_t)((b4 & 0xF0) >> 1);
    s->k[7] = (uint8_t)(((b4 & 0x0E) << 3) & 0xFF);
    y++;
    b5 = tk_peek(img, (uint16_t)(data + y));
    s->k[8] = (uint8_t)((b5 & 0xE0) >> 1);
    s->k[9] = (uint8_t)(((b5 & 0x1C) << 2) & 0xFF);
    return y;
}

/* $E982. EOR #$FF then ADC #$41 with the carry clear is 320 - a, which
 * in eight bits is $40 - a: Textalker holds a period and the chip wants
 * its complement. */
static void pitch_out(synth *s, uint8_t a)
{
    spend(s, TK_CALL_PITCH);
    bits(s, (uint8_t)(((0x40 - a) << 2) & 0xFF), 6);
}

static void emit_pitch_run(synth *s)             /* $E969 */
{
    uint8_t a = (uint8_t)(s->pitch + s->p_off);
    if (a == 0 || (a & 0x80)) a = 1;             /* $E970 / $E976 */
    else if (a >= 0x40) a = 0x3F;                /* $E97C */
    pitch_out(s, a);
}

/* $E945. The pitch is a running value, not a per-frame one. An unvoiced
 * frame resets the run and writes a pitch of zero, which is what the
 * chip reads as "unvoiced" -- note $E95D branches to the conversion at
 * $E982 with A still 0, skipping the clamp entirely. */
static void emit_pitch(synth *s)
{
    if (!(s->ef & 0x80)) {
        s->pitch = s->base_pitch;
        s->glide = 0;
        s->variant = 0;
        s->p_set = 0;
        pitch_out(s, 0);
        return;
    }
    s->pitch = (uint8_t)(s->pitch + s->glide);   /* $E95F */
    emit_pitch_run(s);
}

static uint8_t width(const tk_image *img, int i)
{
    static const uint8_t W[10] = { 5, 5, 4, 4, 4, 4, 4, 3, 3, 3 };
    (void)img;
    return W[i];                                 /* the table at $E906 */
}

static void emit_frame(synth *s)                 /* $E8C4 */
{
    int i = 0;
    spend(s, TK_CALL_ENERGY);
    bits(s, (uint8_t)((s->ef << 2) & 0xFF), 5);  /* energy, then repeat=0 */
    emit_pitch(s);
    s->prev_flags = s->ef;
    for (;;) {
        s->prev[i] = s->k[i];
        spend(s, TK_CALL_K);
        bits(s, (uint8_t)((s->k[i] << 1) & 0xFF), width(s->img, i));
        i++;
        if (i == 4 && !(s->ef & 0x80)) return;   /* $E8F2: unvoiced stops */
        if (i >= 10) return;
    }
}

static void emit_interpolated(synth *s)          /* $E910 */
{
    uint8_t a = (uint8_t)(s->ef + s->prev_flags);
    int i;
    spend(s, TK_CALL_ENERGY);
    bits(s, (uint8_t)(((a << 1) & 0xFF) & 0xF0), 5);
    emit_pitch_run(s);
    for (i = 0; i < 10; i++) {
        spend(s, TK_CALL_K);
        bits(s, (uint8_t)(s->k[i] + s->prev[i]), width(s->img, i));
    }
}

static void maybe_emit(synth *s)                 /* $E893 */
{
    int take = (s->mask & 0x80) != 0;
    if (s->chip->sink && s->chip->sink->spend)
        s->chip->sink->spend(s->chip->sink->ctx, TK_FRAME_CYCLES);
    s->mask = (uint8_t)(s->mask << 1);
    if (!take) return;
    if ((s->ef & 0xC0) == 0x80) {                /* voiced, not a repeat */
        if ((s->prev_flags & 0xC0) == 0x80 && !s->ffb1)
            emit_interpolated(s);                /* $E8BD */
    } else if (s->ffb1) {
        /* $E8A6's BEQ skips; $EDB9's BEQ emits. The counter runs
         * 9,8,..,0 in 3.1.3 and 8,7,..,1 in v1.3, and the test is
         * inverted to match, so the two cancel and both versions drop
         * the same half of the frames. Model one alone and compressed
         * speech comes out as the other half. */
        int odd = s->nframe & 1;
        if (!odd == !s->img->p.compress_emit_even) return;
    }
    emit_frame(s);
}

/* $E7A3: read the allophone's frames and emit the selected ones. */
static void frames(synth *s)
{
    const tk_image *img = s->img;
    int idx = s->index;
    uint16_t p = (uint16_t)(tk_peek(img, (uint16_t)(img->p.allo_ptr + 2 * idx))
               | (tk_peek(img, (uint16_t)(img->p.allo_ptr + 2 * idx + 1)) << 8));
    int y = 0;

    s->mask = tk_peek(img, p);
    if (2 * idx + 1 < 0x34) {                    /* $E7C4: the 26 low ones */
        s->mask = tk_peek(img, (uint16_t)(p + s->variant));  /* one per group */
        y = 2;
    }
    s->nframe = 9;
    if (img->p.frames_pre_dec) {                 /* $ECEC: DEC, then BNE */
        for (;;) {
            s->nframe--;
            if (!s->nframe) break;
            y = unpack(s, p, y);
            maybe_emit(s);
        }
    } else {
        while (s->nframe >= 0) {                 /* $E7E0: DEC, then BPL */
            y = unpack(s, p, y);
            maybe_emit(s);
            s->nframe--;
        }
    }
}

/* $E713: this frame's pitch offset and vowel variant. */
static void pitch_for_frame(synth *s)
{
    const tk_image *img = s->img;
    start(s);
    if (s->index >= 0x24) return;
    if (s->index < 0x1A) {
        s->x = s->group;                         /* $E721 */
        if (s->group == 0) s->index = 4;         /* $E726 */
    }
    s->p_off = s->p_set;
    if (s->p_off) {
        s->p_off = (uint8_t)(s->p_off - 5);      /* $E733: 5 is neutral */
    } else {
        s->x = s->group;                         /* $E73C */
        /* $EA7F,X, read from the image rather than a ten-entry list: a
         * stress marker can leave $FFBC as high as 15 ($DC92 masks with
         * $0F), and the 6502 happily indexes past the table. */
        s->p_off = (uint8_t)(tk_peek(img, (uint16_t)(img->p.pitch_off + s->x))
                             + s->p_off);
    }
    if (s->ffb0) {
        s->p_off = 0;
        s->glide = 0;
    }
    s->variant = tk_peek(img, (uint16_t)(img->p.vowel_var + s->x));
}

static void pause(synth *s)                      /* $E9FC */
{
    if (s->group == 0) {
        bits(s, 0x00, 4);                            /* $EA01 */
        return;
    }
    stop_frames(s);                                  /* $EA09 */
    if (s->chip->sink) {
        /* $EA0C waits for the chip to stop talking, then $EA16 burns a
         * busy loop proportional to the pitch group. Both are pure
         * timing, so they only exist when a real chip is listening. */
        if (s->chip->sink->wait_ts) s->chip->sink->wait_ts(s->chip->sink->ctx);
        if (s->chip->sink->delay)
            s->chip->sink->delay(s->chip->sink->ctx,
                                 (uint8_t)((s->group << 4) & 0xFF));
    }
}

/* $E6B0: the codes that are not allophones at all. Returns 1 when it
 * handled the code itself. */
static int specials(synth *s)
{
    uint8_t i = s->index;
    if (i == 0x7F) return 1;
    if (i == 0x00) { pause(s); return 1; }
    if (i == 0x19) {
        if (s->ffbb == 0) return 1;
        s->group = s->ffbb;
        pause(s);
        return 1;
    }
    if (i == 0xFF) {                             /* end of utterance */
        stop_frames(s);                          /* $E6D7 */
        /* $E6DA waits for the chip to stop before the flags are
         * cleared. Timing only, so it needs a chip to be listening. */
        if (s->chip->sink && s->chip->sink->wait_ts)
            s->chip->sink->wait_ts(s->chip->sink->ctx);
        s->flags &= 0x7D;
        return 1;
    }
    if (i >= 0x4F) {                             /* < and > : the glide */
        s->glide = (uint8_t)((i >> 4) - 5);
        return 1;
    }
    if (i == 0x37) { s->p_step = (uint8_t)(s->p_step + 1); return 1; }  /* + */
    if (i == 0x3F) { s->p_step = (uint8_t)(s->p_step - 1); return 1; }  /* - */
    return 0;
}

/* $E674: a stop consonant before a non-vowel takes the +8 release
 * variant, and is always preceded by a stop frame and a delay. */
static void stops(synth *s)
{
    uint8_t c;
    int bump = 0;
    if (!(s->index >= 0x2A && s->index < 0x32)) return;
    c = (uint8_t)(s->ch - 0xA0);
    if (c == 0x0B) {
        bump = 0;
    } else if (c >= 0x1F || c < 0x10) {
        uint8_t val;
        s->x = c;                                /* $E691 TAX */
        val = tk_peek(s->img, (uint16_t)(s->img->p.charmap + c));
        bump = (val == 0) || (val >= 0x1D);
    }
    if (bump) s->index = (uint8_t)(s->index + 8);
    stop_frames(s);                                  /* $E6A4 */
    /* $E6A7 waits for the chip to stop, and $E6AC then burns a fixed
     * delay of A=8. Both are pure timing, so they only exist when a
     * real chip is listening -- but they are not optional: without the
     * wait the chip is still talking when $EA1A next looks, $EA1F
     * skips the SPEAK EXTERNAL, and everything after goes into a chip
     * that was never restarted. */
    if (s->chip->sink) {
        if (s->chip->sink->wait_ts) s->chip->sink->wait_ts(s->chip->sink->ctx);
        if (s->chip->sink->delay) s->chip->sink->delay(s->chip->sink->ctx, 8);
    }
}

/* $E5C7. Returns 1 if an allophone was actually spoken.
 *
 * $E5CA branches straight to the RTS when $E6B0 handled the code,
 * skipping the $E5D5 re-entry -- so a pause, a glide or an end marker
 * does NOT re-enter $E5AA, and anything the pipeline was holding stays
 * held. That is why a lone "," speaks one stop frame and then swallows
 * the end of the line.
 *
 * It is NOT a property of the version. v1.3 merges the two routines at
 * $EAD6 and re-enters either way, and one Textalker 3.1.2 build sets
 * that branch's displacement to 9 so that it lands ON the re-entry
 * rather than past it -- which is the whole of the difference between
 * the two 3.1.2s. tk_image.c reads the displacement out of the image
 * instead of inferring it from which release this is. */
static int speak_allophone(synth *s)
{
    if (specials(s)) return s->img->p.reenter_after_special;
    stops(s);
    pitch_for_frame(s);
    frames(s);
    return 1;
}

/* ------------------------------------------------------- the two drivers */

/* $DBD6: the phoneme string at $FEAD, as stage 1 leaves it.
 *
 * The buffer is NOT cleared between utterances -- stage 1 overwrites
 * only as many bytes as it produces, and $FFAC says how many. That
 * matters because $DC71's one-code lookahead reads $FEAE,Y, which for
 * the LAST code is one past the end, so it sees a leftover from the
 * previous utterance. In "XH" spoken letter by letter, H's final $64
 * looks past its own string and finds the $5B that X left there, which
 * is in $51-$B2, so $DC7C fakes a following "T" and $E674 gives the
 * stop its +8 release variant. */
void tk_speak_codes(const tk_image *img, tk_voice *v, tk_chip *chip,
                    const uint8_t *codes, int ncodes, uint8_t *phbuf,
                    int phbufsz)
{
    synth s;
    int i, y = 0;
    uint8_t started;

    synth_begin(&s, img, v, chip);
    for (i = 0; i < ncodes && i < phbufsz; i++) phbuf[i] = codes[i];
    /* $E240 leaves the phoneme count in X ($FFAC), and $DBD6 does not
     * touch X before the first $E5C7 -- so that, not whatever $E9AF
     * left behind in the previous utterance, is what $E756 reads $EA89
     * through when $E713 takes its short path. */
    s.x = (uint8_t)ncodes;
    started = img->init_utt_flag;

    while (y < ncodes) {
        uint8_t prev = y ? codes[y - 1] : 0;
        uint8_t c;
        /* $DBF4 branches PAST this when the previous code is a word
         * separator, so the pitch-7 reset arms only when speech is not
         * already running -- at the start of the line, and again after
         * anything that emitted stop frames and put $FFCA back to $FF. */
        if (prev != 0x19 && chip->started == 0xFF) started = 0xFF;   /* $DBFF */
        if (started == 0xFF && codes[y] < 0x4E) {                    /* $DC09 */
            s.group = 7;
            set_pitch(&s);
            started = 0;
        }
        c = codes[y];
        if (c < 0x1A) {                          /* $DC22 */
            s.index = c;
            s.group = 1;
        } else if (c < 0x34) {
            s.index = (uint8_t)(c - 0x1A);
            s.group = 2;
        } else if (c < 0x4E) {
            s.index = (uint8_t)(c - 0x34);
            s.group = 3;
        } else if (c < 0x5E) {                   /* $DC48: group unchanged */
            s.index = (uint8_t)(c - 0x34);
        } else if (c == 0x7F) {                  /* $DC60 */
            y++;
            continue;
        } else if (c < 0xB3) {                   /* $DC6B, with a lookahead */
            /* $DC71 reads $FEAE,Y, so for the LAST code it reads one
             * past the end and wraps within the buffer. */
            uint8_t nxt = phbuf[(y + 1) % phbufsz];
            s.index = (uint8_t)(c - 0x34);
            s.ch = (nxt >= 0x51 && nxt < 0xB3) ? 0xD4 : 0xC1;
            s.flags &= 0x7F;
            speak_allophone(&s);
            y++;
            continue;
        } else {                                 /* $DC92: stress markers */
            s.group = (uint8_t)(c & 0x0F);
            set_pitch(&s);
            y++;
            continue;
        }
        s.flags &= 0x7F;
        speak_allophone(&s);
        y++;
    }
    s.index = 0xFF;                              /* $DC9D: end of utterance */
    speak_allophone(&s);
    synth_save(&s);
}

/* $E5E3. 0 = not a digit, 1 = it only set the pitch, 2 = speak. */
static int digit(synth *s)
{
    if (!(s->ch >= 0xB0 && s->ch < 0xBA)) return 0;      /* $E60D */
    s->group = (uint8_t)(s->ch & 0x0F);
    if (!(s->flags & 0x80)) {
        set_pitch(s);                            /* $E611, and A comes back 0 */
        return 1;
    }
    if (s->index < 0x2A) s->flags &= 0x7F;       /* a vowel is pending: speak */
    return 2;
}

/* $E623. 1 = speak, 0 = defer. Phoneme mode runs one character behind:
 * a vowel or a stop consonant is HELD until the next character arrives,
 * because $E674 needs to know what follows before it can choose a
 * stop's release variant. The held sound is spoken on the next
 * character, and $E5D5-$E5DF then re-enters $E5AA for that character. */
static int lookup(synth *s)
{
    if (s->flags & 0x80) {                       /* something is pending */
        s->group = 2;                            /* $E62A */
        if (s->ch == 0xD2 && s->index > 0 && s->index < 9) {   /* $E62F */
            s->index = (uint8_t)(s->index + 0x10);             /* R colours */
            s->flags |= 0x80;
            return 0;
        }
        return 1;
    }
    s->x = (uint8_t)(s->ch - 0xA0);              /* $E647 TAX */
    s->index = tk_peek(s->img, (uint16_t)(s->img->p.charmap + s->x));
    if (s->index >= 0x32 || (s->index >= 0x1A && s->index < 0x2A)) {
        s->flags &= 0x7F;                        /* $E660 */
        return 1;
    }
    s->flags |= 0x80;                            /* $E66A */
    return 0;
}

/* $E5AA: Ctrl-V phoneme mode.
 *
 * A trailing "." is appended because that is what the line ends as: the
 * driver turns the carriage return into $AE, whose character-map entry
 * is $FF, the end-of-utterance code. It is also what flushes the held
 * sound. */
void tk_speak_phonemes(const tk_image *img, tk_voice *v, tk_chip *chip,
                       const char *text)
{
    synth s;
    size_t i, n = strlen(text);

    synth_begin(&s, img, v, chip);
    for (i = 0; i <= n; i++) {
        unsigned char raw = (i < n) ? (unsigned char)text[i] : '.';
        uint8_t c;
        if (raw >= 'a' && raw <= 'z') raw = (unsigned char)(raw - 32);
        c = (uint8_t)(raw | 0x80);
        if (!(c >= 0xA0 && c < 0xDB)) continue;  /* $E5AF/$E5B6 */
        spend(&s, TK_PHONEME_CYCLES);
        s.ch = c;
        for (;;) {
            int act = digit(&s);
            if (act == 1) break;
            if (act == 0 && !lookup(&s)) break;
            if (speak_allophone(&s) && (s.flags & 0x80)) {
                s.flags &= 0x7F;                 /* $E5D5: the held sound */
                continue;                        /* went out; now this one */
            }
            break;
        }
    }
    synth_save(&s);
}
