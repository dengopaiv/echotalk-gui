/*
 * echotalk.c -- see echotalk.h.
 *
 * This is tools/render_text_loader.c's proven pipeline turned into a
 * library: same boot sequence, same language-card model, same entry
 * discovery, same text preparation and chunking, same dead-air
 * trimming. The harness stays as the diagnostic front end and as the
 * thing the reference baselines are measured with.
 */
#include "echotalk.h"
#include "text_prep.h"
#include "chunker.h"
#include "resample.h"
#include "tms5220_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint16_t pc;
extern uint8_t sp, a, x, y, status;
extern uint32_t clockticks6502;
extern void reset6502(void);
extern void step6502(int printRegs);

#define CPU_HZ      1020484.0
#define CHIP_HZ     8000
#define CYCLES_PER_SAMPLE (CPU_HZ / (double)CHIP_HZ)

#define TRAP_ADDR        0x0300
#define LOADER_ADDR      0x9300
#define DOS_HOOK         0xBA69
#define TRIM_THRESHOLD   150
#define TRIM_MARGIN      40
#define DEFAULT_CHUNK    80

/* --- runaway guards ---
 *
 * Both of these exist to stop a wild jump or a stuck poll from hanging
 * the host forever. Neither is a tuning knob, and hitting one is always
 * a fault: they are sized well above anything Textalker legitimately
 * needs, and any overrun is counted and reported through
 * echotalk_overruns().
 *
 * STEP_BUDGET was 5,000,000 and was FAR too low, which cost a real bug.
 * Textalker's per-character work grows with the inter-word delay and
 * with slower speech -- both make it sit waiting on the chip -- and at
 * word delay 15 with speed 0.25 a single character needs up to about
 * 20,000,000 instructions. The old budget cut the 6502 off mid-routine,
 * leaving a half-finished call stack that the next character was then
 * entered on top of. Nothing said so; the speech simply came out wrong.
 * See notes/step_budget_truncation.md.
 *
 * 64M is roughly three times the worst case measured across both
 * Textalker versions at the slowest exposed settings, and costs nothing
 * in normal use because normal characters finish thousands of times
 * sooner. It only sets how long a genuine hang takes to give up, which
 * at roughly 30M emulated instructions per second is about two seconds.
 */
#define STEP_BUDGET      64000000
#define DRAIN_BUDGET     4000000

struct echotalk {
    uint8_t mem[0x10000];
    uint8_t rom_shadow[0x3000];
    int lc_ram_enabled;
    int readlatch_flag;

    tms5220_state tms;
    double tick_accumulator;

    uint16_t obj_addr;
    uint16_t entry;
    char version[16];

    /* --- settings ---
     *
     * Everything Textalker itself holds is mirrored here, and kept in
     * step whether it was set through the API or by a Ctrl-E command
     * embedded in the text -- see sniff_ctrl_e(). Without that the two
     * drift apart, and the next settings push would quietly undo
     * whatever the text had asked for.
     *
     * Pitch and flatness are one Textalker setting, not two: "nP" sets
     * the pitch and normal intonation, "nF" sets the same pitch and
     * monotone. They are stored apart because that is how a caller
     * thinks about them, and recombined when the command is sent. */
    unsigned out_rate;
    double clock_mult;
    int frame_rate;
    double speed;               /* continuous, pitch-preserving  */
    int compressed;
    int pitch, flat;
    int volume;
    int word_delay, repeat_filter;
    int letter_mode;            /* 0 word, 1 letter                    */
    int punctuation;            /* 0 none, 1 some, 2 all               */
    int modes_chosen;           /* caller picked the two above         */
    size_t chunk_size;          /* 0 = do not chunk */
    int raw;                    /* 1 = skip text preparation */
    unsigned cmd_errors;        /* malformed Ctrl-D commands seen */
    unsigned overruns;          /* runaway guards tripped -- always a fault */
    int settings_dirty;

    /* --- streaming ---
     *
     * echotalk_speak() no longer synthesises; it appends to `pending`
     * and returns. echotalk_read() synthesises one utterance at a time,
     * on demand, so a caller hears the first chunk without waiting for
     * the last. Synthesis measures ~136x real time, so doing it inline
     * from an audio callback has ample headroom; a host that would
     * rather not can call echotalk_synthesize() from its own thread.
     *
     * `pending` is raw text with its Ctrl-D parse cursor. `seg` is the
     * prepared body of the segment currently being consumed, chunk by
     * chunk. Commands are applied when `seg` runs out, which is what
     * puts them after everything that preceded them in the text. */
    char *pending; size_t pend_len, pend_pos, pend_cap;
    char *seg;     size_t seg_len, seg_pos, seg_cap;

    /* Index events, in text order. `offset` is an absolute position in
     * the output stream; an event is ready once read_pos reaches it. */
    struct { int index; size_t offset; } idx[256];
    size_t n_idx, idx_head;

    /* Index marks waiting to be placed, carrying their CHARACTER offset
     * into `seg`. They are resolved to sample positions as each utterance
     * is emitted -- see resolve_marks(). */
    struct { int index; size_t off; } mark[256];
    size_t n_mark, mark_head;
    int index_break;            /* 1 = old behaviour, marks split speech */

    /* audio queue */
    int16_t *audio;
    uint8_t *speaking;
    size_t count, cap, read_pos;

    /* Carried across resampler calls so that per-utterance resampling
     * does not reset the interpolation phase at every boundary. */
    double resamp_phase;

    int aborted;
};

/* Fake6502 keeps the CPU in globals, so the bus callbacks need to know
 * which instance is live. Only one may exist; echotalk_create()
 * enforces that. */
static echotalk *g_active;

/* --- bus ------------------------------------------------------------ */

static void audio_push(echotalk *et, int16_t s, int spk) {
    if (et->count >= et->cap) {
        size_t ncap = et->cap ? et->cap * 2 : 65536;
        int16_t *na = realloc(et->audio, ncap * sizeof(int16_t));
        uint8_t *ns = realloc(et->speaking, ncap);
        if (!na || !ns) { et->aborted = 1; free(na); free(ns); return; }
        et->audio = na; et->speaking = ns; et->cap = ncap;
    }
    et->speaking[et->count] = (uint8_t)(spk ? 1 : 0);
    et->audio[et->count++] = s;
}

static void tick_chip(echotalk *et, uint32_t cycles) {
    et->tick_accumulator += cycles;
    while (et->tick_accumulator >= CYCLES_PER_SAMPLE) {
        int16_t s;
        tms5220_process(&et->tms, &s, 1);
        audio_push(et, s, et->tms.m_TALKD);
        et->tick_accumulator -= CYCLES_PER_SAMPLE;
    }
}

uint8_t read6502(uint16_t address) {
    echotalk *et = g_active;
    if (address >= 0xC0A0 && address <= 0xC0AF) {
        uint8_t r = et->readlatch_flag ? 0xff
                                       : (uint8_t)(0x1f | tms5220_status_r(&et->tms));
        et->readlatch_flag = !et->readlatch_flag;
        return r;
    }
    if (address == 0xC08B) { et->lc_ram_enabled = 1; return 0; }
    if (address == 0xC08A) { et->lc_ram_enabled = 0; return 0; }
    if (address >= 0xD000 && !et->lc_ram_enabled)
        return et->rom_shadow[address - 0xD000];
    return et->mem[address];
}

void write6502(uint16_t address, uint8_t value) {
    echotalk *et = g_active;
    if (address >= 0xC0A0 && address <= 0xC0AF) {
        tms5220_data_w(&et->tms, value);
        return;
    }
    if (address == 0xC08B) { et->lc_ram_enabled = 1; return; }
    if (address == 0xC08A) { et->lc_ram_enabled = 0; return; }
    et->mem[address] = value; /* ROM bank is read-only */
}

/* --- 6502 driving --------------------------------------------------- */

static int run_to_halt(echotalk *et, uint16_t entry, int max_steps,
                       uint16_t halt_addr) {
    et->mem[halt_addr] = 0x00;
    uint16_t ret = halt_addr - 1;
    et->mem[0x0100 + sp] = (ret >> 8) & 0xFF;
    et->mem[0x0100 + ((sp - 1) & 0xFF)] = ret & 0xFF;
    sp -= 2;
    pc = entry;
    int steps = 0;
    while (pc != halt_addr && steps < max_steps && !et->aborted) {
        uint32_t before = clockticks6502;
        step6502(0);
        tick_chip(et, clockticks6502 - before);
        if (pc == TRAP_ADDR) { et->aborted = 1; break; } /* wild jump */
        steps++;
    }
    return steps;
}

static void send_char(echotalk *et, uint8_t ch) {
    sp = 0xFD;
    a = (uint8_t)(ch | 0x80);
    /* Reaching the budget means the 6502 was cut off part-way through
     * Textalker's routine, so its stack and internal state are left
     * inconsistent and every character after this one is entered on top
     * of the wreckage. Count it: this used to happen silently, and the
     * only symptom was speech that came out wrong. */
    if (run_to_halt(et, et->entry, STEP_BUDGET, 0x0201) >= STEP_BUDGET)
        et->overruns++;
}

static void send_string(echotalk *et, const char *s) {
    for (; *s; s++) send_char(et, (uint8_t)*s);
}

/* --- setup ---------------------------------------------------------- */

static void parse_version(echotalk *et, size_t loader_len) {
    /* v3.1.3 stores its banner as plain text, v1.3 stores it reversed.
     * Try forward then reversed; the label is cosmetic either way. */
    /* Text in these images is stored the Apple II way, with the high
     * bit set, so everything here masks it off. */
#define CH(k) ((char)(p[k] & 0x7F))
    const uint8_t *p = et->mem + LOADER_ADDR;
    strcpy(et->version, "unknown");
    for (size_t i = 0; i + 16 < loader_len; i++) {
        int fwd = 1, rev = 1;
        for (int k = 0; k < 7; k++) {
            if (CH(i + k) != "VERSION"[k]) fwd = 0;
            if (CH(i + k) != "NOISREV"[k]) rev = 0;
        }
        if (fwd) {
            size_t n = 0, j = i + 8; /* skip "VERSION " */
            while (n < sizeof(et->version) - 1 && j < loader_len && CH(j) > ' ')
                { et->version[n++] = CH(j); j++; }
            et->version[n] = 0;
            return;
        }
        if (rev) { /* stored backwards, e.g. ")DRAC MAR( 3.1 NOISREV" */
            size_t n = 0;
            long j = (long)i - 2; /* step back over the space */
            while (n < sizeof(et->version) - 1 && j >= 0 && CH(j) > ' ')
                { et->version[n++] = CH(j); j--; }
            et->version[n] = 0;
            return;
        }
    }
#undef CH
}

static int boot(echotalk *et, const char *loader_path, const char *obj_path,
                char *err, size_t errlen) {
#define FAIL(msg) do { if (err) snprintf(err, errlen, "%s", msg); return -1; } while (0)
    FILE *f = fopen(loader_path, "rb");
    if (!f) FAIL("cannot open loader image");
    size_t nr = fread(et->mem + LOADER_ADDR, 1, 0x2000, f);
    fclose(f);
    if (nr < 16) FAIL("loader image is too small to be valid");

    /* The loader's trampoline templates say where the OBJ goes: their
     * JMP targets are its jump table, so the page is the load address. */
    for (size_t i = 0; i + 6 < nr && !et->obj_addr; i++) {
        const uint8_t *p = et->mem + LOADER_ADDR + i;
        if (p[0] == 0x48 && p[1] == 0xAD && p[2] == 0x8B &&
            p[3] == 0xC0 && p[4] == 0x4C)
            et->obj_addr = (uint16_t)(p[6] << 8);
    }
    if (!et->obj_addr) FAIL("loader contains no Textalker entry trampoline");

    f = fopen(obj_path, "rb");
    if (!f) FAIL("cannot open Textalker image");
    size_t no = fread(et->mem + et->obj_addr, 1, 0x10000 - et->obj_addr, f);
    fclose(f);
    if (no < 0x1000) FAIL("Textalker image is too small to be valid");

    parse_version(et, nr);

    memset(et->rom_shadow, 0x60, sizeof(et->rom_shadow)); /* RTS everywhere */
    et->rom_shadow[0xFBB3 - 0xD000] = 0xEA;               /* hardware signature */
    et->mem[0x9EBD] = 0x60;
    et->mem[0xC300] = 0x60;
    et->mem[0xFFFE] = TRAP_ADDR & 0xFF;
    et->mem[0xFFFF] = TRAP_ADDR >> 8;
    et->lc_ram_enabled = 0;   /* entered from DOS with ROM selected */
    et->readlatch_flag = 1;

    tms5220_reset(&et->tms, TMS5220_IS_5220);
    reset6502();
    sp = 0xFD;

    et->mem[0x0036] = 0xF0; et->mem[0x0037] = 0xFD;
    et->mem[0x0028] = 0x00; et->mem[0x0029] = 0x04;
    et->mem[0xAA59] = 0xFD - 2; /* SP the loader restores via TXS */

    run_to_halt(et, LOADER_ADDR, 2000000, 0x0200);
    if (et->aborted) FAIL("Textalker's loader crashed (wild jump)");

    /* Ask through DOS's hook where the character entry is: with Z set
     * this installs it into CSWL, in both Textalker versions. */
    sp = 0xFD;
    status |= 0x02;
    run_to_halt(et, DOS_HOOK, 200000, 0x0203);
    et->entry = (uint16_t)(et->mem[0x0036] | (et->mem[0x0037] << 8));
    if (!(et->mem[et->entry] == 0x48 && et->mem[et->entry + 1] == 0xAD &&
          et->mem[et->entry + 2] == 0x8B && et->mem[et->entry + 3] == 0xC0 &&
          et->mem[et->entry + 4] == 0x4C))
        FAIL("Textalker did not install itself (no entry trampoline at CSWL)");

    /* Talk-only mode, and disable the repeat-character filter, which
     * otherwise speaks "EEEEEEEEE" as "EE". */
    send_string(et, "\x05T");   /* talk-only: no screen echo assumed */

    /* Everything generated so far is boot noise, and for v1.3 that
     * includes its ~1.37s calibration delay. Discard it: a caller
     * expects the queue to hold speech it asked for, nothing else. */
    et->count = et->read_pos = 0;
    return 0;
#undef FAIL
}

/* --- public --------------------------------------------------------- */

echotalk *echotalk_create(const char *loader_path, const char *obj_path,
                          char *errbuf, size_t errbuf_len) {
    if (g_active) {
        if (errbuf) snprintf(errbuf, errbuf_len,
                             "an echotalk instance already exists "
                             "(the 6502 core is a singleton)");
        return NULL;
    }
    echotalk *et = calloc(1, sizeof(*et));
    if (!et) {
        if (errbuf) snprintf(errbuf, errbuf_len, "out of memory");
        return NULL;
    }
    et->out_rate = CHIP_HZ;
    et->clock_mult = 1.0;
    et->speed = 1.0;
    et->pitch = 24;
    et->volume = 12;
    et->word_delay = 0;
    et->repeat_filter = 99;  /* high enough never to trigger */
    et->chunk_size = DEFAULT_CHUNK;
    /* Textalker's own startup modes. Word mode is measured -- a bare
     * letter is spoken as a word. Some-punctuation is an assumption
     * rather than a measurement: a bare comma is silent, so it is
     * certainly not all-punctuation, but "some" versus "none" has not
     * been told apart. It is what this code has always restored to, so
     * recording it here changes nothing. */
    et->letter_mode = 0;
    et->punctuation = 1;
    et->settings_dirty = 1;  /* push defaults before the first utterance */

    g_active = et;
    if (boot(et, loader_path, obj_path, errbuf, errbuf_len) != 0) {
        g_active = NULL;
        free(et->audio); free(et->speaking);
    free(et->pending); free(et->seg); free(et);
        return NULL;
    }
    return et;
}

void echotalk_destroy(echotalk *et) {
    if (!et) return;
    if (g_active == et) g_active = NULL;
    free(et->audio); free(et->speaking);
    free(et->pending); free(et->seg); free(et);
}

unsigned echotalk_abi_version(void) { return ECHOTALK_ABI_VERSION; }

const char *echotalk_version(const echotalk *et) { return et->version; }

int echotalk_set_sample_rate(echotalk *et, unsigned hz) {
    if (hz == 0) hz = CHIP_HZ;
    if (hz < 4000 || hz > 192000) return -1;
    et->out_rate = hz;
    return 0;
}

int echotalk_set_clock_multiplier(echotalk *et, double m) {
    if (m < 0.25 || m > 4.0) return -1;
    et->clock_mult = m;
    return 0;
}

int echotalk_set_frame_rate(echotalk *et, int rate) {
    if (rate < 0 || rate > 3) return -1;
    et->frame_rate = rate;
    /* m_configured_rate survives the RESET commands Textalker issues
     * between segments; m_c_variant_rate is the live value. */
    et->tms.m_configured_rate = (uint8_t)rate;
    et->tms.m_c_variant_rate = (uint8_t)rate;
    return 0;
}

int echotalk_set_speed(echotalk *et, double speed) {
    if (speed < 0.25 || speed > 4.0) return -1;
    et->speed = speed;
    tms5220_set_speech_rate(&et->tms, speed);
    return 0;
}
double echotalk_speed(const echotalk *et) { return et->speed; }

int echotalk_set_compressed(echotalk *et, int c) {
    et->compressed = c ? 1 : 0; et->settings_dirty = 1; return 0;
}
int echotalk_set_pitch(echotalk *et, int p) {
    if (p < 0 || p > 63) return -1;
    et->pitch = p; et->settings_dirty = 1; return 0;
}
int echotalk_set_flat(echotalk *et, int flat) {
    if (flat != 0 && flat != 1) return -1;
    et->flat = flat; et->settings_dirty = 1; return 0;
}
/* These two are pushed only once a caller has actually chosen them --
 * see modes_chosen. Sending them unconditionally would assert this
 * code's guess at Textalker's startup punctuation mode as though it
 * were measured, and would change the byte stream every instance emits
 * before its first word. */
int echotalk_set_letter_mode(echotalk *et, int letter) {
    if (letter != 0 && letter != 1) return -1;
    et->letter_mode = letter;
    et->modes_chosen = 1;
    et->settings_dirty = 1;
    return 0;
}
int echotalk_set_punctuation(echotalk *et, int mode) {
    if (mode < 0 || mode > 2) return -1;
    et->punctuation = mode;
    et->modes_chosen = 1;
    et->settings_dirty = 1;
    return 0;
}
int echotalk_set_volume(echotalk *et, int v) {
    if (v < 0 || v > 15) return -1;
    et->volume = v; et->settings_dirty = 1; return 0;
}
int echotalk_set_word_delay(echotalk *et, int d) {
    if (d < 0 || d > 15) return -1;
    et->word_delay = d; et->settings_dirty = 1; return 0;
}
int echotalk_set_repeat_filter(echotalk *et, int t) {
    if (t < 0 || t > 99) return -1;
    et->repeat_filter = t; et->settings_dirty = 1; return 0;
}

/* Chunking is driver-side, so unlike the settings above this needs no
 * Ctrl-E traffic and does not dirty the settings block. 255 is an
 * arbitrary ceiling: anything past Textalker's ~80-character line is
 * already unverified territory, and 0 leaves it entirely. */
int echotalk_set_chunk_size(echotalk *et, unsigned chars) {
    if (chars > 255) return -1;
    et->chunk_size = chars;
    return 0;
}
unsigned echotalk_chunk_size(const echotalk *et) {
    return (unsigned)et->chunk_size;
}

/* 1 restores the old behaviour, where an index mark ends the utterance
 * so its position is exact. That is what made a wrapped sentence read
 * one line at a time under NVDA's Say All, so the default is 0. */
int echotalk_set_index_break(echotalk *et, int on) {
    if (on != 0 && on != 1) return -1;
    et->index_break = on;
    return 0;
}
int echotalk_index_break(const echotalk *et) { return et->index_break; }

int echotalk_set_raw(echotalk *et, int raw) {
    if (raw != 0 && raw != 1) return -1;
    et->raw = raw;
    return 0;
}

unsigned echotalk_command_errors(const echotalk *et) { return et->cmd_errors; }
unsigned echotalk_overruns(const echotalk *et) { return et->overruns; }
void echotalk_clear_command_errors(echotalk *et) { et->cmd_errors = 0; }

/* --- reading settings back ------------------------------------------
 *
 * Every one of these returns a value its matching setter accepts, so a
 * voice can be carried from one instance to another by reading here and
 * writing there. That round trip is the whole point: switching Textalker
 * version means a fresh 6502 and a fresh Textalker at ITS defaults, and
 * the only way to restore the voice is to know what it was. */
int      echotalk_pitch(const echotalk *et)        { return et->pitch; }
int      echotalk_flat(const echotalk *et)         { return et->flat; }
int      echotalk_volume(const echotalk *et)       { return et->volume; }
int      echotalk_word_delay(const echotalk *et)   { return et->word_delay; }
int      echotalk_repeat_filter(const echotalk *et){ return et->repeat_filter; }
int      echotalk_compressed(const echotalk *et)   { return et->compressed; }
int      echotalk_letter_mode(const echotalk *et)  { return et->letter_mode; }
int      echotalk_punctuation(const echotalk *et)  { return et->punctuation; }
int      echotalk_frame_rate(const echotalk *et)   { return et->frame_rate; }
double   echotalk_clock_multiplier(const echotalk *et) { return et->clock_mult; }
unsigned echotalk_sample_rate(const echotalk *et)  { return et->out_rate; }
int      echotalk_raw(const echotalk *et)          { return et->raw; }

static void apply_settings(echotalk *et) {
    char cmd[16];
    if (!et->settings_dirty) return;
    /* P or F is the same setting: pitch with intonation, or pitch
     * monotone. Sending "%dP" unconditionally, as this used to, threw
     * away any flatness the text had asked for. */
    snprintf(cmd, sizeof cmd, "\x05%d%c", et->pitch, et->flat ? 'F' : 'P');
    send_string(et, cmd);
    snprintf(cmd, sizeof cmd, "\x05%dV", et->volume);        send_string(et, cmd);
    snprintf(cmd, sizeof cmd, "\x05%dD", et->word_delay);    send_string(et, cmd);
    snprintf(cmd, sizeof cmd, "\x05%dR", et->repeat_filter); send_string(et, cmd);
    send_string(et, et->compressed ? "\x05" "C" : "\x05" "E");
    et->settings_dirty = 0;
}

/* Watches text on its way to Textalker for Ctrl-E commands, and mirrors
 * them into the settings above.
 *
 * The commands still go through untouched -- Textalker acts on them as
 * it always did. This only stops the library's idea of the current
 * voice from drifting away from the real one, which matters for two
 * reasons: a later settings push would otherwise overwrite whatever the
 * text had set, and a host cannot carry the voice across to a fresh
 * instance (switching Textalker version resets the 6502 and everything
 * in it) unless it can read the current values back.
 *
 * Command letters are case-insensitive -- verified: "\x05 10p" and
 * "\x05 10P" produce identical audio, and both differ from sending
 * nothing. Sniffing only uppercase would silently miss half of them.
 *
 * Values are clamped to the ranges the setters accept, so that anything
 * a getter reports can be handed straight back to its setter. Textalker
 * does something of its own with out-of-range values -- "\x05 99P" is
 * audibly not "\x05 63P" -- so a replay of out-of-spec input is not
 * bit-exact. That is the deliberate trade: a coherent API over exact
 * reproduction of input that was out of spec to begin with. */
static void sniff_ctrl_e(echotalk *et, const char *s, size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if ((unsigned char)s[i] != 0x05) continue;

        size_t j = i + 1;
        int val = 0, has_val = 0;
        while (j < len && s[j] >= '0' && s[j] <= '9') {
            if (val < 10000) val = val * 10 + (s[j] - '0');
            has_val = 1;
            j++;
        }
        if (j >= len) break;

        char c = s[j];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');

        switch (c) {
        case 'P':                       /* pitch, normal intonation    */
            if (has_val) et->pitch = val > 63 ? 63 : val;
            et->flat = 0;
            break;
        case 'F':                       /* the same pitch, monotone    */
            if (has_val) et->pitch = val > 63 ? 63 : val;
            et->flat = 1;
            break;
        case 'V': if (has_val) et->volume = val > 15 ? 15 : val; break;
        case 'D': if (has_val) et->word_delay = val > 15 ? 15 : val; break;
        case 'R': if (has_val) et->repeat_filter = val > 99 ? 99 : val; break;
        case 'C': et->compressed = 1; break;
        case 'E': et->compressed = 0; break;
        case 'L': et->letter_mode = 1; break;
        case 'W': et->letter_mode = 0; break;
        case 'A': et->punctuation = 2; break;
        case 'S': et->punctuation = 1; break;
        case 'N': et->punctuation = 0; break;
        default: break;                 /* T, and anything unknown     */
        }
        i = j;
    }
}

/* Sends one utterance and the CR that makes Textalker speak it.
 *
 * A single character on its own is almost always meant as a character
 * rather than a word -- a letter being reviewed, a punctuation mark
 * being announced -- so it is wrapped in letter mode and
 * all-punctuation, then set back afterwards. Without this a lone "," is
 * silent and a lone letter can be read as a word or swallowed by the
 * command dispatcher.
 *
 * The restore goes back to the TRACKED modes, not to a fixed pair. This
 * used to send "\x05S\x05W" unconditionally, which meant a caller who
 * had chosen all-punctuation or letter mode lost it the first time a
 * one-character utterance went past.
 *
 * The restore goes BEFORE the CR, not after it. Textalker buffers the
 * whole line and processes it in order when the CR arrives, so a
 * command sitting in the buffer takes effect partway through that pass
 * -- which means the terminating CR is itself seen in whatever
 * punctuation mode is current by then. Left in all-punctuation, it is
 * announced as "return", so asking for one character got you two spoken
 * items. Restoring some-punctuation first silences the CR while still
 * leaving the character itself to be processed in all-punctuation
 * mode, since it was buffered ahead of the restore. */
static void send_utterance(echotalk *et, const char *s, size_t len) {
    int single = (len == 1);
    if (single) send_string(et, "\x05L\x05" "A");
    for (size_t i = 0; i < len; i++) send_char(et, (uint8_t)s[i]);
    if (single) {
        char restore[5];
        restore[0] = 0x05;
        restore[1] = et->punctuation == 2 ? 'A' : et->punctuation == 0 ? 'N' : 'S';
        restore[2] = 0x05;
        restore[3] = et->letter_mode ? 'L' : 'W';
        restore[4] = 0;
        send_string(et, restore);
    }
    send_char(et, '\r');
}

/* Speaks one utterance and finishes its audio off completely: drained,
 * trimmed and converted to the output format before returning.
 *
 * Doing all of that per utterance rather than once per speak() call is
 * what makes streaming possible -- audio is only safe to hand out once
 * nothing further will move it, and trimming moves it. It also keeps
 * the trimming rule intact: dead air is removed at the head of EVERY
 * utterance, which is what stops a pause appearing at every chunk
 * boundary. */
static void emit_utterance(echotalk *et, const char *s, size_t len) {
    /* Push any changed Ctrl-E settings first, and deliberately before
     * `start` is taken: the handful of samples that costs then sits
     * outside the trimmed range, which is where it has always sat. */
    apply_settings(et);

    /* Mirror any Ctrl-E commands the caller embedded. Only the caller's
     * own text is examined, never the wrapper send_utterance() puts
     * around a single character -- that restores the tracked modes, so
     * reading it back would be circular. */
    sniff_ctrl_e(et, s, len);

    size_t start = et->count;

    send_utterance(et, s, len);

    /* Drain what is still in flight before touching the samples. */
    int guard = 0;
    while (tms5220_talk_status(&et->tms) && guard++ < DRAIN_BUDGET)
        tick_chip(et, (uint32_t)CYCLES_PER_SAMPLE);
    if (guard >= DRAIN_BUDGET) et->overruns++;   /* chip never went idle */

    /* Trim the dead air at the head of this utterance.
     *
     * Textalker processes a whole line before it sends anything to the
     * chip, and that work scales with how much text there is, so every
     * utterance is preceded by silence proportional to its length. The
     * scan stops at the first sample that is both played by the chip
     * (TALKD set) and audible. Both conditions matter: TALKD alone stops
     * on the obligatory silent frame emitted when speech restarts, which
     * is an artifact rather than content, and amplitude alone can stop
     * on a stray non-zero sample while the chip is idle. Because it
     * halts at the first audible output, it can only ever consume
     * silence at the head -- a pause after a comma or period follows
     * audible speech and is unreachable. */
    size_t i = start;
    while (i < et->count) {
        int mag = et->audio[i] < 0 ? -et->audio[i] : et->audio[i];
        if (et->speaking[i] && mag > TRIM_THRESHOLD) break;
        i++;
    }
    if (i < et->count) {              /* nothing audible -> keep it all */
        size_t run = i - start;
        if (run > TRIM_MARGIN) {
            size_t skip = run - TRIM_MARGIN, w = start;
            for (size_t k = start + skip; k < et->count; k++) {
                et->audio[w] = et->audio[k];
                et->speaking[w] = et->speaking[k];
                w++;
            }
            et->count = w;
        }
    }

    /* Convert to the output format now, while this utterance is the
     * tail of the buffer. The clock multiplier is applied by declaring a
     * different source rate for the same samples: the chip expresses
     * everything in sample counts, so speed and pitch move together
     * exactly as over/underclocking the real chip would. The resampler
     * carries its phase across utterances so the boundaries do not
     * click. */
    unsigned native = (unsigned)(CHIP_HZ * et->clock_mult + 0.5);
    if (native != et->out_rate && et->count > start) {
        size_t in_n = et->count - start, out_n = 0;
        int16_t *rs = echotalk_resample_stream(et->audio + start, in_n,
                                               native, et->out_rate,
                                               &et->resamp_phase, &out_n);
        if (rs) {
            size_t need_cap = start + out_n;
            if (need_cap > et->cap) {
                int16_t *na = realloc(et->audio, need_cap * sizeof(int16_t));
                uint8_t *ns = realloc(et->speaking, need_cap);
                if (na) et->audio = na;
                if (ns) et->speaking = ns;
                if (na && ns) et->cap = need_cap;
            }
            if (need_cap <= et->cap) {
                memcpy(et->audio + start, rs, out_n * sizeof(int16_t));
                memset(et->speaking + start, 1, out_n);
                et->count = need_cap;
            }
            free(rs);
        }
    }
}

/* --- Ctrl-D driver commands ----------------------------------------
 *
 * Shape deliberately copied from Textalker's own Ctrl-E commands: an
 * optional number, then a letter. See echotalk.h for the command list
 * and for why a command ends the current utterance. */

#define CTRL_D 0x04

/* Digits with at most one decimal point: "2", "0.75", ".5". Always a
 * '.' regardless of locale -- this is a wire format, not a display
 * format. Returns 1 if anything was consumed. */
static int parse_number(const char *s, size_t len, size_t *i, double *out) {
    size_t start = *i;
    int seen_dot = 0;
    double v = 0.0, scale = 0.1;
    while (*i < len) {
        char c = s[*i];
        if (c >= '0' && c <= '9') {
            if (seen_dot) { v += (c - '0') * scale; scale *= 0.1; }
            else            v = v * 10.0 + (c - '0');
        } else if (c == '.' && !seen_dot) {
            seen_dot = 1;
        } else break;
        (*i)++;
    }
    if (*i == start) return 0;
    *out = v;
    return 1;
}

/* Whole numbers only. "1.5F" is a mistake rather than something to
 * round, so it is rejected and counted like any other bad command. */
static int drv_int(double v, int *out) {
    if (v < 0.0 || v > 1000000.0) return -1;
    long n = (long)v;
    if ((double)n != v) return -1;
    *out = (int)n;
    return 0;
}

static int record_index(echotalk *et, int index);

/* Returns 0 if applied, -1 if the letter is unknown or the value is out
 * of range. A letter with no number restores that setting's default. */
static int apply_drv_cmd(echotalk *et, char letter, int has_value, double value) {
    int n;
    switch (letter) {
    case 'F': case 'f':                       /* TMS5220 frame rate    */
        if (!has_value) return echotalk_set_frame_rate(et, 0);
        if (drv_int(value, &n)) return -1;
        return echotalk_set_frame_rate(et, n);
    case 'C': case 'c':                       /* chip clock multiplier */
        return echotalk_set_clock_multiplier(et, has_value ? value : 1.0);
    case 'S': case 's':                       /* speed, pitch-preserving */
        return echotalk_set_speed(et, has_value ? value : 1.0);
    case 'B': case 'b':                       /* line buffer / chunking */
        if (!has_value) return echotalk_set_chunk_size(et, DEFAULT_CHUNK);
        if (drv_int(value, &n)) return -1;
        return echotalk_set_chunk_size(et, (unsigned)n);
    case 'R': case 'r':                       /* raw text passthrough  */
        if (!has_value) return echotalk_set_raw(et, 0);
        if (drv_int(value, &n)) return -1;
        return echotalk_set_raw(et, n);
    case 'I': case 'i':                       /* index mark            */
        /* The one command with no default: an index without a number
         * says nothing, so a bare I is a mistake rather than a request
         * for index zero. */
        if (!has_value || drv_int(value, &n)) return -1;
        return record_index(et, n);
    default:
        return -1;
    }
}

/* Records an index mark at the current end of the output stream.
 *
 * Commands are applied only once everything before them has been
 * synthesised, so et->count IS the position the mark belongs at -- no
 * bookkeeping through trimming or resampling is needed, because both
 * have already happened to every sample that precedes it. That is the
 * main reason index marks are Ctrl-D commands rather than something
 * that could sit in the middle of an utterance: a position inside one
 * is not knowable until the utterance has been spoken, and by then it
 * is too late to split it. */
/* Places a resolved event at a known position in the output stream. */
static int record_index_at(echotalk *et, int index, size_t offset) {
    if (et->n_idx >= sizeof(et->idx) / sizeof(et->idx[0])) return -1;
    et->idx[et->n_idx].index = index;
    et->idx[et->n_idx].offset = offset;
    et->n_idx++;
    return 0;
}

/* Turns the marks lying inside one utterance into events.
 *
 * [s0, s1) is the utterance's span in `seg`; [a0, a1) is the audio it
 * produced. A mark at the very start or end of the span lands exactly;
 * one in the middle is placed proportionally by character offset.
 *
 * That interpolation is an approximation, and deliberately so. Textalker
 * buffers a whole line and emits nothing until the terminating CR, so
 * there is no way to observe which character is being spoken -- the only
 * way to place a mark exactly is to END the utterance there, which is
 * what this used to do and what made NVDA's Say All read a wrapped
 * sentence one line at a time. Marks at an utterance boundary, which is
 * where all but a handful of NVDA's are, stay exact either way. */
static void resolve_marks(echotalk *et, size_t s0, size_t s1,
                          size_t a0, size_t a1) {
    while (et->mark_head < et->n_mark && et->mark[et->mark_head].off < s1) {
        size_t off = et->mark[et->mark_head].off;
        size_t pos = a0;
        if (off > s0 && s1 > s0 && a1 > a0)
            pos = a0 + (off - s0) * (a1 - a0) / (s1 - s0);
        record_index_at(et, et->mark[et->mark_head].index, pos);
        et->mark_head++;
    }
}

/* Anything left over once the segment is exhausted belongs at the end. */
static void resolve_trailing_marks(echotalk *et) {
    while (et->mark_head < et->n_mark) {
        record_index_at(et, et->mark[et->mark_head].index, et->count);
        et->mark_head++;
    }
}

static int record_index(echotalk *et, int index) {
    if (et->n_idx >= sizeof(et->idx) / sizeof(et->idx[0])) return -1;
    et->idx[et->n_idx].index = index;
    et->idx[et->n_idx].offset = et->count;
    et->n_idx++;
    return 0;
}

/* Applies the Ctrl-D command at pend_pos and steps over it. */
static void apply_pending_command(echotalk *et) {
    const char *text = et->pending;
    size_t len = et->pend_len;

    /* Whitespace may follow the introducer, because that is where a
     * person writing "Ctrl-D 2F" naturally puts it. The number and
     * letter are one token though, exactly as in Ctrl-E's "12P" --
     * allowing a gap there would make "\x04 2 Fox" ambiguous between a
     * command and text. */
    size_t j = et->pend_pos + 1;
    while (j < len && (text[j] == ' ' || text[j] == '\t')) j++;
    double value = 0.0;
    int has_value = parse_number(text, len, &j, &value);
    char c = (j < len) ? text[j] : 0;

    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        if (apply_drv_cmd(et, c, has_value, value) != 0) et->cmd_errors++;
        et->pend_pos = j + 1;
    } else {
        /* Swallow the introducer and any number that followed it.
         * Leaving the digits behind would have the host speak fragments
         * of its own control codes. */
        et->cmd_errors++;
        et->pend_pos = j;
    }
}

/* True if pend_pos is on a command rather than on an escaped literal. */
static int at_command(const echotalk *et) {
    if (et->pend_pos >= et->pend_len) return 0;
    if ((unsigned char)et->pending[et->pend_pos] != CTRL_D) return 0;
    return !(et->pend_pos + 1 < et->pend_len &&
             (unsigned char)et->pending[et->pend_pos + 1] == CTRL_D);
}

/* Applies any commands sitting at the cursor, then gathers the run of
 * literal text that follows into `seg`, prepared unless raw mode is on.
 * Returns 1 if there is a segment to speak, 0 if the pending text ran
 * out. Reading the raw-mode flag here rather than earlier is what makes
 * a mid-text Ctrl-D 1R apply to the text after it and not before. */
/* If the cursor is on a Ctrl-D index command, consumes it and returns 1
 * with the value in *out. Used to keep index marks inline rather than
 * letting them end the segment the way every other command does. */
static int take_index_command(echotalk *et, int *out) {
    if (!at_command(et)) return 0;
    size_t j = et->pend_pos + 1;
    while (j < et->pend_len && (et->pending[j] == ' ' || et->pending[j] == '\t')) j++;
    double value = 0.0;
    int has_value = parse_number(et->pending, et->pend_len, &j, &value);
    if (!has_value || j >= et->pend_len) return 0;
    char c = et->pending[j];
    if (c != 'I' && c != 'i') return 0;
    int n;
    if (drv_int(value, &n)) return 0;
    *out = n;
    et->pend_pos = j + 1;
    return 1;
}

/* Appends the run of literal text at the cursor to `seg`, stopping at
 * the next command. Preparation happens per run rather than once for the
 * whole segment, which is what lets an index mark sit between two runs
 * and still know its offset in the PREPARED text: preparation can change
 * the length of what it touches, so an offset taken before it would not
 * survive. It is safe to split here because preparation is per-character
 * and Ctrl-D is ASCII, so no multi-byte sequence is ever cut. */
static int append_literal_run(echotalk *et) {
    size_t start = et->pend_pos;
    size_t cap = et->pend_len - start + 1;
    char *raw = malloc(cap);
    if (!raw) return 0;

    size_t rl = 0;
    while (et->pend_pos < et->pend_len) {
        unsigned char c = (unsigned char)et->pending[et->pend_pos];
        if (c == CTRL_D) {
            /* Doubled Ctrl-D is an escaped literal, not a command. This
             * is what keeps raw mode reachable in both directions:
             * Ctrl-D has to be honoured even in raw mode, or 1R would be
             * a one-way door, so text that means a literal 0x04 needs a
             * way to say so. */
            if (et->pend_pos + 1 < et->pend_len &&
                (unsigned char)et->pending[et->pend_pos + 1] == CTRL_D) {
                raw[rl++] = (char)CTRL_D;
                et->pend_pos += 2;
                continue;
            }
            break;      /* a command */
        }
        raw[rl++] = (char)c;
        et->pend_pos++;
    }
    if (!rl) { free(raw); return 0; }

    size_t need = et->raw ? rl
                          : echotalk_prep_text((const uint8_t *)raw, rl,
                                               NULL, 0, NULL);
    if (et->seg_len + need + 1 > et->seg_cap) {
        size_t ncap = et->seg_len + need + 1;
        char *ns = realloc(et->seg, ncap);
        if (!ns) { free(raw); return 0; }
        et->seg = ns;
        et->seg_cap = ncap;
    }
    if (et->raw) {
        memcpy(et->seg + et->seg_len, raw, rl);
        et->seg_len += rl;
    } else {
        echotalk_prep_text((const uint8_t *)raw, rl,
                           et->seg + et->seg_len, need + 1, NULL);
        et->seg_len += need;
    }
    et->seg[et->seg_len] = 0;
    free(raw);
    return 1;
}

/* Applies any commands at the cursor, then gathers the run of literal
 * text that follows into `seg`, prepared unless raw mode is on.
 *
 * Index marks are the one command that does NOT end the segment. NVDA
 * puts one between every line of a Say All, and ending the utterance at
 * each of them made a sentence wrapped over several lines read as
 * several separate sentences. They are collected with their offset into
 * `seg` instead and placed once the audio exists.
 *
 * Returns 1 if there is anything to do -- text, or marks to place.
 * Reading the raw-mode flag here rather than earlier is what makes a
 * mid-text Ctrl-D 1R apply to the text after it and not before. */
static int gather_segment(echotalk *et) {
    et->seg_len = 0;
    et->seg_pos = 0;
    et->n_mark = et->mark_head = 0;

    for (;;) {
        int done = 0;
        while (at_command(et)) {
            int index;
            if (!et->index_break && take_index_command(et, &index)) {
                if (et->n_mark < sizeof(et->mark) / sizeof(et->mark[0])) {
                    et->mark[et->n_mark].index = index;
                    et->mark[et->n_mark].off = et->seg_len;
                    et->n_mark++;
                } else {
                    et->cmd_errors++;   /* more marks than can be tracked */
                }
                continue;
            }
            /* Any other command applies after whatever has been gathered,
             * so it ends the segment -- unless nothing has been gathered
             * yet, in which case it applies now. */
            if (et->seg_len) { done = 1; break; }
            apply_pending_command(et);
        }
        if (done || et->pend_pos >= et->pend_len) break;
        if (!append_literal_run(et)) break;
    }
    return (et->seg_len > 0 || et->n_mark > 0);
}

/* Speaks the next utterance's worth of pending work, and no more.
 * Returns 1 if it produced audio, 0 if there is nothing left.
 *
 * Chunking happens one chunk at a time here rather than all at once,
 * which is both what streaming needs and what removes an old ceiling:
 * echotalk_chunk_text() stops when the caller's array fills and reports
 * nothing about the text it never reached, so asking it for a single
 * chunk per call and advancing past it cannot silently drop a tail. */
static int pump_one(echotalk *et) {
    for (;;) {
        while (et->seg_pos < et->seg_len) {
            const char *line = et->seg + et->seg_pos;
            size_t avail = et->seg_len - et->seg_pos;
            size_t line_len = 0;
            while (line_len < avail && line[line_len] != '\r') line_len++;
            size_t skip_cr = (line_len < avail) ? 1 : 0;

            if (line_len == 0) { et->seg_pos += skip_cr; continue; }

            size_t s0, s1, before, after;
            if (!et->chunk_size || line_len <= et->chunk_size) {
                s0 = et->seg_pos;
                s1 = s0 + line_len;
                before = et->count;
                emit_utterance(et, line, line_len);
                after = et->count;
                et->seg_pos += line_len + skip_cr;
                resolve_marks(et, s0, s1, before, after);
                return 1;
            }

            echotalk_chunk ch;
            if (echotalk_chunk_text(line, line_len, et->chunk_size, &ch, 1) == 0) {
                et->seg_pos += line_len + skip_cr;   /* only whitespace */
                continue;
            }
            s0 = et->seg_pos + ch.offset;
            s1 = s0 + ch.length;
            before = et->count;
            emit_utterance(et, line + ch.offset, ch.length);
            after = et->count;
            et->seg_pos += ch.offset + ch.length;
            resolve_marks(et, s0, s1, before, after);
            return 1;
        }

        /* The segment is spoken; any mark past its last utterance -- an
         * end-of-speech marker, typically -- belongs at the end. */
        resolve_trailing_marks(et);
        et->seg_len = et->seg_pos = 0;
        if (!gather_segment(et)) return 0;
        /* A segment can be nothing but marks (NVDA ends a Say All with a
         * bare index). Place them and move on rather than spinning. */
        if (!et->seg_len) {
            resolve_trailing_marks(et);
            if (et->pend_pos >= et->pend_len) return 0;
        }
    }
}

/* Once the queue is drained and nothing is outstanding, wind the buffer
 * back to the start. Without this the audio buffer grows for the life of
 * the instance, which matters for a screen reader that may run for days.
 * Index events not yet collected are rebased rather than dropped. */
static void recycle_buffer(echotalk *et) {
    if (et->read_pos < et->count || et->pend_pos < et->pend_len ||
        et->seg_pos < et->seg_len)
        return;
    et->count = et->read_pos = 0;
    et->pend_len = et->pend_pos = 0;
    et->seg_len = et->seg_pos = 0;
    if (et->idx_head >= et->n_idx) {
        et->n_idx = et->idx_head = 0;
    } else {
        size_t keep = et->n_idx - et->idx_head;
        memmove(et->idx, et->idx + et->idx_head, keep * sizeof(et->idx[0]));
        for (size_t i = 0; i < keep; i++) et->idx[i].offset = 0;
        et->n_idx = keep;
        et->idx_head = 0;
    }
    et->resamp_phase = 0.0;
}

int echotalk_speak(echotalk *et, const char *text) {
    if (!et || !text) return -1;
    size_t len = strlen(text);

    /* Drop what has already been read, so a long session does not grow
     * the buffer without bound. */
    recycle_buffer(et);

    if (et->pend_len + len + 1 > et->pend_cap) {
        size_t ncap = et->pend_len + len + 1;
        char *np = realloc(et->pending, ncap);
        if (!np) return -1;
        et->pending = np;
        et->pend_cap = ncap;
    }
    memcpy(et->pending + et->pend_len, text, len);
    et->pend_len += len;
    et->pending[et->pend_len] = 0;

    return et->aborted ? -1 : 0;
}

size_t echotalk_pending(const echotalk *et) {
    if (!et) return 0;
    return (et->pend_len - et->pend_pos) + (et->seg_len - et->seg_pos);
}

size_t echotalk_synthesize(echotalk *et, size_t min_samples) {
    if (!et) return 0;
    while (et->count - et->read_pos < min_samples && pump_one(et))
        ;
    return et->count - et->read_pos;
}

size_t echotalk_available(const echotalk *et) {
    return et->count - et->read_pos;
}

size_t echotalk_read(echotalk *et, int16_t *out, size_t frames) {
    if (!et || !out || !frames) return 0;

    /* Synthesise on demand. This is what makes the library stream: the
     * caller hears the first chunk without waiting for the last, and at
     * roughly 136x real time there is ample headroom to do it inline
     * from an audio callback. A host that would rather not can call
     * echotalk_synthesize() from a thread of its own. */
    while (et->read_pos >= et->count && pump_one(et))
        ;

    size_t have = et->count - et->read_pos;
    if (!have) { recycle_buffer(et); return 0; }

    size_t n = have < frames ? have : frames;
    memcpy(out, et->audio + et->read_pos, n * sizeof(int16_t));
    et->read_pos += n;
    return n;
}

int echotalk_next_index(echotalk *et, int *index) {
    if (!et || et->idx_head >= et->n_idx) return 0;
    if (et->idx[et->idx_head].offset > et->read_pos) return 0;
    if (index) *index = et->idx[et->idx_head].index;
    et->idx_head++;
    return 1;
}

void echotalk_stop(echotalk *et) {
    if (!et) return;
    /* Everything, not just the queued audio: text that has not been
     * synthesised yet would otherwise resume on the next read, and index
     * events left behind would fire against audio nobody is going to
     * hear. A screen reader calls this because the user moved on.
     *
     * Textalker's own state needs no attention. Synthesis runs to the
     * end of an utterance before returning, so there is never anything
     * in flight to interrupt. */
    et->count = et->read_pos = 0;
    et->pend_len = et->pend_pos = 0;
    et->seg_len = et->seg_pos = 0;
    et->n_idx = et->idx_head = 0;
    et->resamp_phase = 0.0;
}
