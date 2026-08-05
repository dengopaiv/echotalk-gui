/*
 * render_text_loader.c -- THE CANONICAL TEXTALKER v3.1.3 HARNESS.
 *
 * Boots Textalker v3.1.3 by running its REAL loader
 * (roms/textalker.ram.bin at $9300), rather than calling $D003/$FCD6
 * directly the way tools/render_text_real_chip.c does.
 *
 * Use this one. The direct-init harness is kept only for comparison:
 * this approach was confirmed by ear (session 10) to fix the onset
 * glitch that had been open since session 9, in every case tested.
 *
 * Why this exists (session 10):
 *
 * Session 3 established that the loader is not *required* -- calling
 * $D003 (with $00 pushed) and then $FCD6 is enough to make Textalker
 * speak, so the loader was dropped from the pipeline. But "enough to
 * produce speech" is not the same as "does everything the loader did",
 * and there is a suggestive asymmetry: the v1.3 harness
 * (tools/render_v13.c) DOES run its real loader, and v1.3 has never
 * exhibited the onset glitch documented in
 * notes/onset_glitch_investigation_reverted.md. v3.1.3, booted the
 * short way, does.
 *
 * So this harness restores the real boot path for v3.1.3, to test
 * whether the loader performs initialization the direct-call path
 * misses. Everything downstream (per-character entry via $D006, the
 * chip model, the tick rate, WAV output) is deliberately identical to
 * render_text_real_chip.c so that the boot path is the only variable.
 *
 * One thing worth noting up front: the $BA83 "stub" the direct-call
 * harness writes by hand (PLA/RTS) is a stand-in for code the real
 * loader installs properly, as part of the $BA7C trampoline. Under this
 * harness the loader writes that region itself, so the hand-written
 * stubs are only applied if the loader left the area empty.
 *
 * ROM stubs below are taken from tools/boot_and_probe.c, which already
 * established what the v3.1.3 loader touches during boot.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tms5220_core.h"
#include "render_common.h"

extern uint16_t pc;
extern uint8_t sp, a, x, y, status;
extern uint32_t clockticks6502;
extern void reset6502(void);
extern void step6502(int printRegs);

static uint8_t mem[0x10000];
static tms5220_state tms;
static int readlatch_flag = 1;

/* --- Minimal Apple II language-card banking ---
 *
 * Textalker occupies $D000-$FFF9 in the language card, which shadows
 * the Apple's monitor ROM at those same addresses. So $FDED, $FDF0 and
 * $FD1B name Textalker code or monitor ROM code depending purely on
 * which bank is switched in -- and Textalker's own trampolines switch
 * banks deliberately: `LDA $C08B` selects the card (Textalker), `LDA
 * $C08A` selects ROM. The $BA7C entry trampoline reads $C08B before
 * jumping to $D006, and the exit path reads $C08A before handing off to
 * the monitor's screen-print continuation at $FD1B.
 *
 * A flat 64K model cannot express that, and pretending otherwise means
 * writing "ROM stubs" straight over Textalker's own bytes. The direct
 * harness gets away with it only because its hand-written PLA/RTS stub
 * at $BA83 returns before the ROM-side path is ever reached; with the
 * real loader installed, that path runs for real and lands in what is
 * actually Textalker code (this is how demo_bas hit the wild-jump trap
 * at $FDF3).
 *
 * So: keep Textalker in mem[] and give $D000-$FFFF a separate shadow
 * bank standing in for the monitor ROM. We emulate no actual Apple ROM
 * code -- the shadow is filled entirely with RTS, making every monitor
 * entry point a harmless no-op that returns to its caller, which is
 * exactly right for a headless renderer with no screen. */
static uint8_t rom_shadow[0x3000];
static int lc_ram_enabled = 1; /* Textalker visible by default */
static int bank_trace = 0;     /* set via ECHOTALK_BANK_TRACE=1 */

static inline int is_lc_space(uint16_t address) { return address >= 0xD000; }

static int16_t *audio = NULL;
static size_t audio_count = 0, audio_cap = 0;
static void audio_push(int16_t s) {
    if (audio_count >= audio_cap) {
        audio_cap = audio_cap ? audio_cap * 2 : 65536;
        audio = realloc(audio, audio_cap * sizeof(int16_t));
    }
    audio[audio_count++] = s;
}

#define CPU_HZ      1020484.0
#define CHIP_HZ     8000.0
/* Overridable for experiments only (ECHOTALK_CPU_HZ). The real values
 * are an Apple II 6502 at ~1.0205MHz and a TMS5220 clocked at 640kHz,
 * which divides to 8000 samples/sec -- 127.56 CPU cycles per sample. */
static double cycles_per_sample = CPU_HZ / CHIP_HZ;
#define CYCLES_PER_SAMPLE cycles_per_sample
static double tick_accumulator = 0.0;

/* Chip-state trace (ECHOTALK_CHIP_TRACE=1). Reads the chip struct
 * directly rather than instrumenting the core, so the ported code stays
 * untouched. Logs the transitions that matter for diagnosing pacing:
 * whether the chip is speaking (TALKD), whether it has run out of data
 * (buffer_empty -- the `goto ranout` path in parse_frame), how full the
 * FIFO is, and the energy index of the current frame (0 = silent frame,
 * 15 = stop frame). */
static int chip_trace = 0;
static void trace_chip_state(void) {
    static int first = 1;
    static int p_talkd, p_empty, p_energy, p_spen, p_ddis, p_talk;
    static int p_fifo_zero;
    int talkd = tms.m_TALKD, empty = tms.m_buffer_empty;
    int energy = tms.m_new_frame_energy_idx;
    int fifo_zero = (tms.m_fifo_count == 0);
    int spen = tms.m_SPEN, ddis = tms.m_DDIS, talk = tms.m_TALK;
    if (first || talkd != p_talkd || empty != p_empty ||
        energy != p_energy || fifo_zero != p_fifo_zero ||
        spen != p_spen || ddis != p_ddis || talk != p_talk) {
        fprintf(stderr, "[chip] %7.1fms  TALK=%d TALKD=%d SPEN=%d DDIS=%d BE=%d fifo=%2d energy=%2d%s\n",
                audio_count / 8.0, talk, talkd, spen, ddis, empty,
                tms.m_fifo_count, energy,
                energy == 0 ? "  (silent)" : energy == 15 ? "  (STOP)" : "");
        first = 0;
        p_talkd = talkd; p_empty = empty; p_energy = energy; p_fifo_zero = fifo_zero;
        p_spen = spen; p_ddis = ddis; p_talk = talk;
    }
}

static void tick_chip(uint32_t elapsed_cpu_cycles) {
    tick_accumulator += elapsed_cpu_cycles;
    while (tick_accumulator >= CYCLES_PER_SAMPLE) {
        int16_t sample;
        tms5220_process(&tms, &sample, 1);
        audio_push(sample);
        if (chip_trace) trace_chip_state();
        tick_accumulator -= CYCLES_PER_SAMPLE;
    }
}

static int echo_write_count = 0;

uint8_t read6502(uint16_t address) {
    if (address >= 0xC0A0 && address <= 0xC0AF) {
        uint8_t retval;
        if (!readlatch_flag) retval = 0x1f | tms5220_status_r(&tms);
        else retval = 0xff;
        readlatch_flag = !readlatch_flag;
        return retval;
    }
    /* Language-card softswitches. Only the two Textalker actually uses
     * are modelled; the rest of $C08x is left alone. */
    if (address == 0xC08B) { if (bank_trace && !lc_ram_enabled) fprintf(stderr, "[bank] PC=$%04X -> LC RAM\n", pc); lc_ram_enabled = 1; return 0; }
    if (address == 0xC08A) { if (bank_trace && lc_ram_enabled) fprintf(stderr, "[bank] PC=$%04X -> ROM\n", pc); lc_ram_enabled = 0; return 0; }
    if (is_lc_space(address) && !lc_ram_enabled)
        return rom_shadow[address - 0xD000];
    return mem[address];
}

void write6502(uint16_t address, uint8_t value) {
    if (address >= 0xC0A0 && address <= 0xC0AF) {
        tms5220_data_w(&tms, value);
        echo_write_count++;
        return;
    }
    if (address == 0xC08B) { lc_ram_enabled = 1; return; }
    if (address == 0xC08A) { lc_ram_enabled = 0; return; }
    /* Writes always land in RAM, as on real hardware where the ROM
     * bank is read-only -- so Textalker's image is never corrupted by
     * a write issued while the ROM bank happens to be selected. */
    mem[address] = value;
}

#define TRAP_ADDR 0x0300
static void install_wild_jump_trap(void) {
    mem[0xFFFE] = TRAP_ADDR & 0xFF;
    mem[0xFFFF] = (TRAP_ADDR >> 8) & 0xFF;
}
static void check_wild_jump_trap(void) {
    if (pc != TRAP_ADDR) return;
    uint8_t pcl = mem[0x0100 + ((sp + 2) & 0xFF)];
    uint8_t pch = mem[0x0100 + ((sp + 3) & 0xFF)];
    uint16_t brk_origin = (uint16_t)((pch << 8) | pcl) - 2;
    fprintf(stderr,
            "\n*** WILD JUMP TRAP: execution hit unmapped memory (BRK) originating "
            "from $%04X -- likely a missing ROM stub or corrupted jump/return address. "
            "Aborting instead of spinning. ***\n", brk_origin);
    exit(1);
}

static int run_to_halt(uint16_t entry, int max_steps, uint16_t halt_addr,
                        int has_pushed_char, uint8_t pushed_char) {
    mem[halt_addr] = 0x00;
    uint16_t retaddr = halt_addr - 1;
    mem[0x0100 + sp] = (retaddr >> 8) & 0xFF;
    mem[0x0100 + ((sp - 1) & 0xFF)] = retaddr & 0xFF;
    sp -= 2;
    if (has_pushed_char) {
        mem[0x0100 + sp] = pushed_char;
        sp -= 1;
    }
    pc = entry;
    int steps = 0;
    while (pc != halt_addr && steps < max_steps) {
        uint32_t before = clockticks6502;
        step6502(0);
        uint32_t elapsed = clockticks6502 - before;
        tick_chip(elapsed);
        check_wild_jump_trap();
        steps++;
    }
    return steps;
}

int main(int argc, char **argv) {
    if (render_parse_args(argc, argv, 4,
                          "<textalker.ram.bin> <textalker.obj.bin> <input bytes file> <output.wav>",
                          &g_ropts))
        return 1;
    const char *ram_path = g_ropts.pos[0], *obj_path = g_ropts.pos[1];
    const char *in_path  = g_ropts.pos[2], *out_path = g_ropts.pos[3];

    FILE *fo = fopen(obj_path, "rb");
    if (!fo) { perror(obj_path); return 1; }
    size_t no = fread(mem + 0xD000, 1, 0x3000, fo);
    fclose(fo);
    VLOG("Loaded %zu bytes of TEXTALKER.OBJ at $D000\n", no);

    FILE *fr = fopen(ram_path, "rb");
    if (!fr) { perror(ram_path); return 1; }
    size_t nr = fread(mem + 0x9300, 1, 0x300, fr);
    fclose(fr);
    VLOG("Loaded %zu bytes of TEXTALKER.RAM (loader) at $9300\n", nr);

    long tlen = 0;
    uint8_t *text = render_load_input(&g_ropts, in_path, &tlen);

    bank_trace = getenv("ECHOTALK_BANK_TRACE") != NULL;
    chip_trace = getenv("ECHOTALK_CHIP_TRACE") != NULL;
    { const char *hz = getenv("ECHOTALK_CPU_HZ");
      if (hz) { cycles_per_sample = atof(hz) / CHIP_HZ;
                fprintf(stderr, "[experiment] CPU %s Hz -> %.3f cycles/sample\n",
                        hz, cycles_per_sample); } }

    tms5220_reset(&tms, TMS5220_IS_5220);
    install_wild_jump_trap();

    /* The stand-in monitor ROM: every address returns immediately. We
     * reproduce no Apple ROM code at all -- screen output, cursor
     * handling and the like are simply nothing in a headless renderer,
     * and RTS is the correct nothing because callers expect to get
     * control back. */
    memset(rom_shadow, 0x60, sizeof(rom_shadow));
    /* Real Apple II ROM hardware-signature byte. Without it the
     * loader's machine-detection misreads "unusual hardware" and sets
     * an internal flag that routes character processing down a
     * different path entirely (session 2 finding). */
    rom_shadow[0xFBB3 - 0xD000] = 0xEA;

    /* Stubs below $D000 are outside the language-card window, so they
     * live in main memory and cannot collide with Textalker. */
    mem[0x9EBD] = 0x60; /* LC-bank-2 patch loop */
    mem[0xC300] = 0x60; /* slot-3 firmware (Ctrl-C path) */

    /* Start on the ROM bank, which is how DOS enters this loader: the
     * OBJ has already been BLOADed into the card, but the card is not
     * switched in. The loader manages banking itself -- traced as
     * $9338 -> ROM, $93CA -> LC RAM, $93DD -> ROM -- and notably
     * returns to its caller with ROM selected, which is why characters
     * must be fed through the $BA7C trampoline rather than jumped
     * straight into $D006. */
    lc_ram_enabled = 0;

    /* No monitor stubs are written into mem[] here, deliberately. With
     * banking modelled, every monitor address the loader touches
     * ($FC58, $FBFD, $FE95, $FE1F, $FDED/$FDF0, the $FBB3 signature)
     * resolves through rom_shadow while the loader has ROM selected --
     * so Textalker's own image is left completely intact, unlike in the
     * flat-memory harness where those stubs overwrote real Textalker
     * bytes. */

    reset6502();
    sp = 0xFD;

    /* Plausible zero-page state, as a real machine would have after
     * DOS boot: output vector pointing at the COUT1 stub, cursor at
     * column 0, screen line base pointing at text page 1 rather than
     * zero page. */
    mem[0x0036] = 0xF0; mem[0x0037] = 0xFD;
    mem[0x0024] = 0x00;
    mem[0x0028] = 0x00; mem[0x0029] = 0x04;

    /* The loader's prologue reads DOS's register-save slots so it can
     * restore them and RTS cleanly at the end. We jam PC in directly
     * rather than arriving via a real JSR from DOS, so seed $AA59 with
     * the SP that will be in effect after run_to_halt pushes its fake
     * return address (0xFD - 2). */
    mem[0xAA59] = 0xFD - 2;
    mem[0xAA5C] = 0;
    mem[0xAA5B] = 0;

    uint8_t ba83_before = mem[0xBA83];
    int steps = run_to_halt(0x9300, 2000000, 0x0200, 0, 0);
    VLOG("Loader ($9300) ran: %d steps, echo writes during load: %d\n",
         steps, echo_write_count);
    VLOG("  $36/$37 (CSWL output vector) = $%02X%02X\n", mem[0x0037], mem[0x0036]);
    if (g_ropts.verbose) {
        fprintf(stderr, "  $BA7C region (was $BA83=%02X):", ba83_before);
        for (uint16_t addr = 0xBA7C; addr <= 0xBA92; addr++)
            fprintf(stderr, " %02X", mem[addr]);
        fprintf(stderr, "\n");
    }
    VLOG("  $FD87 (card detection) = $%02X %s\n",
         mem[0xFD87], mem[0xFD87] == 0x1F ? "(SUCCEEDED)" : "(not set by loader)");
    /* Always worth knowing: if detection failed the audio is garbage. */
    if (mem[0xFD87] != 0x1F)
        fprintf(stderr, "WARNING: card detection did not succeed (FD87=$%02X)\n", mem[0xFD87]);

    /* The loader normally installs a trampoline at $BA7C ending in
     * JMP $D006. If it left $BA83/$BA88 untouched, fall back to the
     * hand-written PLA/RTS stubs the direct-call harness uses -- see
     * notes/single_letter_word_bug_fixed.md for why $BA88 matters. */
    if (mem[0xBA83] == 0x00) { mem[0xBA83] = 0x68; mem[0xBA84] = 0x60; }
    if (mem[0xBA88] == 0x00) { mem[0xBA88] = 0x68; mem[0xBA89] = 0x60; }

    /* $FCD6 is Textalker's own TMS5220 presence/signature check, which
     * pokes low-level jump targets the byte-I/O path depends on. Only
     * run it if the loader did not already achieve card detection, so
     * that a loader-driven boot stays as close to the real thing as
     * possible. */
    if (mem[0xFD87] != 0x1F) {
        int s2 = run_to_halt(0xFCD6, 20000, 0x0202, 0, 0);
        VLOG("Ran $FCD6 explicitly: %d steps (card detection %s -- FD87=%02X)\n",
             s2, mem[0xFD87] == 0x1F ? "SUCCEEDED" : "FAILED", mem[0xFD87]);
    }

    /* Feed characters through the trampoline the loader actually
     * installed at $BA7C (PHA / LDA $C08B / JMP $D006) rather than
     * jumping to $D006 ourselves. This matters: the loader returns with
     * the ROM bank selected, so $D006 does not even name Textalker at
     * that moment -- the trampoline's own $C08B read is what switches
     * the card in. It also means the character is passed the way real
     * hardware passes it, in A, with the trampoline doing the push. */
    #define SEND_CHAR(c) do {                                     \
        sp = 0xFD;                                                \
        a = (uint8_t)((c) | 0x80);                                \
        run_to_halt(0xBA7C, 5000000, 0x0201, 0, 0);               \
    } while (0)

    /* Disable the repeat-character filter now that init is complete and
     * before any real text -- see REPEAT_FILTER_DISABLE. */
    if (g_ropts.repeat_filter_fix) {
        for (const char *p = REPEAT_FILTER_DISABLE; *p; p++) SEND_CHAR(*p);
        VLOG("Sent repeat-filter disable (Ctrl-E 99 R)\n");
    }

    for (long i = 0; i < tlen; i++) {
        uint8_t ch = text[i] | 0x80;
        sp = 0xFD;
        a = ch;
        int s = run_to_halt(0xBA7C, 5000000, 0x0201, 0, 0);
        if (s >= 5000000) {
            fprintf(stderr, "WARNING: char #%ld ($%02X) hit step budget -- may be incomplete\n", i, ch);
        }
        if (g_ropts.verbose) {
            fprintf(stderr, "\rchar %ld/%ld ($%02X), audio=%.3fs   ",
                    i + 1, tlen, ch, audio_count / CHIP_HZ);
            fflush(stderr);
        }
    }
    VLOG("\n");

    int idle_guard = 0;
    while (tms5220_talk_status(&tms) && idle_guard < 500000) {
        tick_chip((uint32_t)CYCLES_PER_SAMPLE);
        idle_guard++;
    }

    render_finish(&g_ropts, in_path, out_path, tlen, (uint32_t)CHIP_HZ, audio, audio_count);
    return 0;
}
