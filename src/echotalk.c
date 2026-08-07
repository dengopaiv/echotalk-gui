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

    /* settings */
    unsigned out_rate;
    double clock_mult;
    int frame_rate;
    int compressed;
    int pitch, volume;
    int word_delay, repeat_filter;
    size_t chunk_size;          /* 0 = do not chunk */
    int raw;                    /* 1 = skip text preparation */
    unsigned cmd_errors;        /* malformed Ctrl-D commands seen */
    int settings_dirty;

    /* one mark per utterance, for dead-air trimming */
    size_t marks[4096];
    size_t nmarks;

    /* audio queue */
    int16_t *audio;
    uint8_t *speaking;
    size_t count, cap, read_pos;

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
    run_to_halt(et, et->entry, 5000000, 0x0201);
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
    et->pitch = 24;
    et->volume = 12;
    et->word_delay = 0;
    et->repeat_filter = 99;  /* high enough never to trigger */
    et->chunk_size = DEFAULT_CHUNK;
    et->settings_dirty = 1;  /* push defaults before the first utterance */

    g_active = et;
    if (boot(et, loader_path, obj_path, errbuf, errbuf_len) != 0) {
        g_active = NULL;
        free(et->audio); free(et->speaking); free(et);
        return NULL;
    }
    return et;
}

void echotalk_destroy(echotalk *et) {
    if (!et) return;
    if (g_active == et) g_active = NULL;
    free(et->audio); free(et->speaking); free(et);
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

int echotalk_set_compressed(echotalk *et, int c) {
    et->compressed = c ? 1 : 0; et->settings_dirty = 1; return 0;
}
int echotalk_set_pitch(echotalk *et, int p) {
    if (p < 0 || p > 63) return -1;
    et->pitch = p; et->settings_dirty = 1; return 0;
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

int echotalk_set_raw(echotalk *et, int raw) {
    if (raw != 0 && raw != 1) return -1;
    et->raw = raw;
    return 0;
}

unsigned echotalk_command_errors(const echotalk *et) { return et->cmd_errors; }
void echotalk_clear_command_errors(echotalk *et) { et->cmd_errors = 0; }

static void apply_settings(echotalk *et) {
    char cmd[16];
    if (!et->settings_dirty) return;
    snprintf(cmd, sizeof cmd, "\x05%dP", et->pitch);         send_string(et, cmd);
    snprintf(cmd, sizeof cmd, "\x05%dV", et->volume);        send_string(et, cmd);
    snprintf(cmd, sizeof cmd, "\x05%dD", et->word_delay);    send_string(et, cmd);
    snprintf(cmd, sizeof cmd, "\x05%dR", et->repeat_filter); send_string(et, cmd);
    send_string(et, et->compressed ? "\x05" "C" : "\x05" "E");
    et->settings_dirty = 0;
}

/* Sends one utterance and the CR that makes Textalker speak it.
 *
 * A single character on its own is almost always meant as a character
 * rather than a word -- a letter being reviewed, a punctuation mark
 * being announced -- so it is wrapped in letter mode and
 * all-punctuation, then set back to word mode and some-punctuation.
 * Without this a lone "," is silent and a lone letter can be read as a
 * word or swallowed by the command dispatcher.
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
    /* Record where this utterance's audio starts. Textalker processes a
     * whole line before sending anything to the chip, and that work
     * scales with the amount of text, so every utterance is preceded by
     * dead air proportional to its length. Trimming needs one mark per
     * utterance, not one per speak() call -- otherwise the pause at
     * every chunk boundary survives. */
    if (et->nmarks < sizeof(et->marks) / sizeof(et->marks[0]))
        et->marks[et->nmarks++] = et->count;
    if (single) send_string(et, "\x05L\x05" "A");
    for (size_t i = 0; i < len; i++) send_char(et, (uint8_t)s[i]);
    if (single) send_string(et, "\x05S\x05W");
    send_char(et, '\r');
}

/* Sends one line, split if it is longer than the current chunk size.
 *
 * The chunker is called in batches rather than once. It stops when the
 * array it was given fills up, and a single call with a fixed array
 * silently drops everything past that point -- 20,480 characters at the
 * default chunk size, which is why this went unnoticed, but only 2,048
 * at a chunk size of 8, which Ctrl-D B now makes reachable. Looping
 * until the line is consumed removes the ceiling. */
static void send_line(echotalk *et, const char *line, size_t len) {
    if (!len) return;
    if (!et->chunk_size || len <= et->chunk_size) {
        send_utterance(et, line, len);
        return;
    }
    size_t off = 0;
    while (off < len) {
        echotalk_chunk chunks[64];
        size_t n = echotalk_chunk_text(line + off, len - off, et->chunk_size,
                                       chunks, sizeof chunks / sizeof chunks[0]);
        if (!n) break;                  /* only whitespace left */
        for (size_t c = 0; c < n; c++)
            send_utterance(et, line + off + chunks[c].offset, chunks[c].length);
        size_t consumed = chunks[n - 1].offset + chunks[n - 1].length;
        if (!consumed) break;           /* no progress; refuse to spin */
        off += consumed;
    }
}

/* One stretch of text spoken at a single set of driver settings.
 *
 * This is everything echotalk_speak() used to be. It became a helper
 * when Ctrl-D commands made one speak() call able to change settings
 * partway through: trimming and resampling both depend on settings a
 * later command may change, so each run is finished off on its own. */
static int speak_run(echotalk *et, const char *text, size_t len) {
    char *prepped = NULL;
    const char *body = text;
    size_t body_len = len;

    if (!et->raw) {
        size_t need = echotalk_prep_text((const uint8_t *)text, len,
                                         NULL, 0, NULL);
        prepped = malloc(need + 1);
        if (!prepped) return -1;
        echotalk_prep_text((const uint8_t *)text, len, prepped, need + 1, NULL);
        body = prepped;
        body_len = need;
    }

    /* Mark where this utterance's audio begins so the think-time dead
     * air before it can be trimmed; CR is what makes Textalker speak. */
    size_t utt_start = et->count;
    et->nmarks = 0;

    const char *line = body;
    size_t remaining = body_len;
    while (remaining) {
        size_t line_len = 0;
        while (line_len < remaining && line[line_len] != '\r') line_len++;
        send_line(et, line, line_len);
        if (line_len >= remaining) break;
        line += line_len + 1;
        remaining -= line_len + 1;
    }
    free(prepped);

    /* Drain any speech still in flight. */
    int guard = 0;
    while (tms5220_talk_status(&et->tms) && guard++ < 500000)
        tick_chip(et, (uint32_t)CYCLES_PER_SAMPLE);

    /* Trim the dead air at the head of every utterance, closing the gap
     * up as we go.
     *
     * The scan stops at the first sample that is both played by the chip
     * (TALKD set) and audible. Both conditions matter: TALKD alone stops
     * on the obligatory silent frame emitted when speech restarts, which
     * is an artifact rather than content, and amplitude alone can stop
     * on a stray non-zero sample while the chip is idle. Because it
     * halts at the first audible output, it can only ever consume
     * silence at the head of an utterance -- a pause after a comma or
     * period follows audible speech and is unreachable. */
    size_t w = utt_start, pos = utt_start;
    for (size_t m = 0; m < et->nmarks; m++) {
        size_t start = (m == 0) ? utt_start : et->marks[m];
        size_t end   = (m + 1 < et->nmarks) ? et->marks[m + 1] : et->count;
        if (start < pos) start = pos;
        if (end < start) end = start;

        size_t i = start, skip = 0;
        while (i < end) {
            int mag = et->audio[i] < 0 ? -et->audio[i] : et->audio[i];
            if (et->speaking[i] && mag > TRIM_THRESHOLD) break;
            i++;
        }
        if (i < end) {              /* nothing audible -> keep it all */
            size_t run = i - start;
            if (run > TRIM_MARGIN) skip = run - TRIM_MARGIN;
        }
        for (size_t k = start + skip; k < end; k++) {
            et->audio[w] = et->audio[k];
            et->speaking[w] = et->speaking[k];
            w++;
        }
        pos = end;
    }
    if (et->nmarks) et->count = w;

    /* Convert this utterance to the output format once, here, rather
     * than in echotalk_read -- doing it there would resample the
     * already-resampled tail on every call and shrink the audio each
     * time. The clock multiplier is applied by declaring a different
     * source rate for the same samples: the chip expresses everything
     * in sample counts, so speed and pitch move together exactly as
     * over/underclocking the real chip would. */
    unsigned native = (unsigned)(CHIP_HZ * et->clock_mult + 0.5);
    if (native != et->out_rate && et->count > utt_start) {
        size_t in_n = et->count - utt_start, out_n = 0;
        int16_t *rs = echotalk_resample(et->audio + utt_start, in_n,
                                        native, et->out_rate, &out_n);
        if (rs) {
            size_t need_cap = utt_start + out_n;
            if (need_cap > et->cap) {
                int16_t *na = realloc(et->audio, need_cap * sizeof(int16_t));
                uint8_t *ns = realloc(et->speaking, need_cap);
                if (na) et->audio = na;
                if (ns) et->speaking = ns;
                if (na && ns) et->cap = need_cap;
            }
            if (need_cap <= et->cap) {
                memcpy(et->audio + utt_start, rs, out_n * sizeof(int16_t));
                memset(et->speaking + utt_start, 1, out_n);
                et->count = need_cap;
            }
            free(rs);
        }
    }
    return et->aborted ? -1 : 0;
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
    case 'B': case 'b':                       /* line buffer / chunking */
        if (!has_value) return echotalk_set_chunk_size(et, DEFAULT_CHUNK);
        if (drv_int(value, &n)) return -1;
        return echotalk_set_chunk_size(et, (unsigned)n);
    case 'R': case 'r':                       /* raw text passthrough  */
        if (!has_value) return echotalk_set_raw(et, 0);
        if (drv_int(value, &n)) return -1;
        return echotalk_set_raw(et, n);
    default:
        return -1;
    }
}

int echotalk_speak(echotalk *et, const char *text) {
    if (!et || !text) return -1;
    size_t len = strlen(text);

    apply_settings(et);

    /* A segment can only ever be shorter than the input it came from. */
    char *seg = malloc(len + 1);
    if (!seg) return -1;
    size_t seglen = 0, i = 0;
    int rc = 0;

    while (i < len) {
        if ((unsigned char)text[i] != CTRL_D) { seg[seglen++] = text[i++]; continue; }

        /* Doubled Ctrl-D is an escaped literal, not a command. This is
         * what keeps raw mode reachable in both directions: Ctrl-D has
         * to be honoured even in raw mode, or 1R would be a one-way
         * door, so text that means a literal 0x04 needs a way to say
         * so. */
        if (i + 1 < len && (unsigned char)text[i + 1] == CTRL_D) {
            seg[seglen++] = (char)CTRL_D;
            i += 2;
            continue;
        }

        /* Flush what is buffered before the setting changes under it. */
        if (seglen && speak_run(et, seg, seglen) != 0) rc = -1;
        seglen = 0;

        /* Whitespace may follow the introducer, because that is where a
         * person writing "Ctrl-D 2F" naturally puts it. The number and
         * letter are one token though, exactly as in Ctrl-E's "12P" --
         * allowing a gap there would make "\x04 2 Fox" ambiguous
         * between a command and text. */
        size_t j = i + 1;
        while (j < len && (text[j] == ' ' || text[j] == '\t')) j++;
        double value = 0.0;
        int has_value = parse_number(text, len, &j, &value);
        char c = (j < len) ? text[j] : 0;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            if (apply_drv_cmd(et, c, has_value, value) != 0) et->cmd_errors++;
            i = j + 1;
        } else {
            /* Swallow the introducer and any number that followed it.
             * Leaving the digits behind would have the host speak
             * fragments of its own control codes. */
            et->cmd_errors++;
            i = j;
        }
    }
    if (seglen && speak_run(et, seg, seglen) != 0) rc = -1;
    free(seg);

    return (rc || et->aborted) ? -1 : 0;
}

size_t echotalk_available(const echotalk *et) {
    return et->count - et->read_pos;
}

size_t echotalk_read(echotalk *et, int16_t *out, size_t frames) {
    size_t have = et->count - et->read_pos;
    if (!have || !frames) return 0;
    {
        size_t n = have < frames ? have : frames;
        memcpy(out, et->audio + et->read_pos, n * sizeof(int16_t));
        et->read_pos += n;
        return n;
    }

    size_t n = have < frames ? have : frames;
    memcpy(out, et->audio + et->read_pos, n * sizeof(int16_t));
    et->read_pos += n;
    return n;
}

void echotalk_stop(echotalk *et) {
    et->count = et->read_pos = 0;
}
