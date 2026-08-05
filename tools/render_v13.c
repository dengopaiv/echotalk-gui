/*
 * render_v13.c
 *
 * Textalker v1.3 through the same real 6502 + real TMS5220 pipeline
 * validated for v3.1.3, to compare boot/detection behavior directly.
 * v1.3 loads its 611-byte RAM loader at $9300 and its 11264-byte OBJ
 * at $D400 (spanning $D400-$FFFF). The loader references a handful of
 * real Apple ROM monitor routines ($FC58, $FBFD, $9EBD) early on, for
 * what appears to be on-screen calibration text -- not detection logic
 * -- so those are stubbed as harmless RTS. Everything else the loader
 * calls ($EC0A, $EC17, etc.) falls within the already-loaded OBJ's own
 * address range, so no further stubbing is needed.
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

#define audio_count render_audio_count()

#define CPU_HZ      1020484.0
#define CHIP_HZ     8000.0
#define CYCLES_PER_SAMPLE (CPU_HZ / CHIP_HZ)
static double tick_accumulator = 0.0;

static void tick_chip(uint32_t elapsed_cpu_cycles) {
    tick_accumulator += elapsed_cpu_cycles;
    while (tick_accumulator >= CYCLES_PER_SAMPLE) {
        int16_t sample;
        tms5220_process(&tms, &sample, 1);
        render_audio_push(sample, tms.m_TALKD);
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
    return mem[address];
}

void write6502(uint16_t address, uint8_t value) {
    if (address >= 0xC0A0 && address <= 0xC0AF) {
        tms5220_data_w(&tms, value);
        echo_write_count++;
        return;
    }
    mem[address] = value;
}

/* Wild-jump trap: point the 6502's IRQ/BRK vector ($FFFE/$FFFF) at an
 * address we control (in an unused RAM page) rather than leaving it
 * as zeroed memory. If execution ever drifts into unmapped/zeroed
 * memory (e.g. an unstubbed real-ROM entry point -- the same failure
 * mode found and fixed for v3.1.3's single-letter-word bug), it will
 * hit a BRK (opcode 0x00) and land HERE instead of silently looping
 * through a zero vector back to $0000 forever. See
 * tools/render_text_real_chip.c for the original version of this and
 * notes/single_letter_word_bug_fixed.md for why it exists. */
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
                          "<ram_loader.bin> <obj.bin> <input bytes file> <output.wav>",
                          &g_ropts))
        return 1;
    const char *ram_path = g_ropts.pos[0], *obj_path = g_ropts.pos[1];
    const char *in_path  = g_ropts.pos[2], *out_path = g_ropts.pos[3];

    FILE *fr = fopen(ram_path, "rb");
    if (!fr) { perror(ram_path); return 1; }
    size_t nr = fread(mem + 0x9300, 1, 0x2000, fr);
    fclose(fr);
    VLOG("Loaded %zu bytes of RAM loader at $9300\n", nr);

    FILE *fo = fopen(obj_path, "rb");
    if (!fo) { perror(obj_path); return 1; }
    size_t no = fread(mem + 0xD400, 1, 0x2C00, fo);
    fclose(fo);
    VLOG("Loaded %zu bytes of OBJ at $D400\n", no);

    long tlen = 0;
    uint8_t *text = render_load_input(&g_ropts, in_path, &tlen);

    /* Stub the handful of real Apple ROM monitor routines the loader
     * calls early on (apparent screen/calibration text output, not
     * detection logic) as harmless RTS. */
    mem[0xFC58] = 0x60; /* RTS */
    mem[0xFBFD] = 0x60; /* RTS */
    mem[0x9EBD] = 0x60; /* RTS */
    install_wild_jump_trap();

    tms5220_reset(&tms, TMS5220_IS_5220);
    reset6502();
    sp = 0xFD;

    /* The loader does its own "LDA $AA59 / STA $95F0 ... LDX $95F0 / TXS"
     * partway through (restoring what it assumes was a caller-saved SP),
     * which clobbers our injected fake return address entirely. Rather
     * than fight that, cooperate with it: make $AA59 read back as a
     * known SP value, and place our desired return address at exactly
     * the stack slot that SP will then point to, so its own RTS lands
     * where we want. */
    mem[0xAA59] = 0xFD; /* SP value the loader will TXS to */
    uint16_t halt_addr_loader = 0x0200;
    mem[0x01FE] = (uint8_t)((halt_addr_loader - 1) & 0xFF);
    mem[0x01FF] = (uint8_t)(((halt_addr_loader - 1) >> 8) & 0xFF);

    int steps = run_to_halt(0x9300, 2000000, halt_addr_loader, 0, 0);
    VLOG("Loader ($9300) ran: %d steps, echo writes during load: %d\n", steps, echo_write_count);
    VLOG("  $EC0B (last successful candidate low byte, 0 if none): $%02X\n", mem[0xEC0B]);
    VLOG("  $F48F (install-success flag): $%02X\n", mem[0xF48F]);

    /* v1.3 predates several v3.1.3 commands and silently discards any it
     * does not recognise (see HANDOFF.md on the $D857 dispatch chain),
     * so sending this is safe whether or not v1.3 implements it --
     * verified to leave every reference render byte-identical. */
    if (g_ropts.repeat_filter_fix) {
        for (const char *p = REPEAT_FILTER_DISABLE; *p; p++) {
            sp = 0xFD;
            run_to_halt(0xD400, 5000000, 0x0201, 1, (uint8_t)(*p | 0x80));
        }
        VLOG("Sent repeat-filter disable (Ctrl-E 99 R)\n");
    }

    for (long i = 0; i < tlen; i++) {
        uint8_t ch = text[i] | 0x80;
        /* Mark where each utterance begins (start of text, and after
         * every CR) so its think-time dead air can be trimmed. */
        if (i == 0 || text[i - 1] == '\r') render_mark_utterance();
        sp = 0xFD;
        int s = run_to_halt(0xD400, 5000000, 0x0201, 1, ch);
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

    render_finish(&g_ropts, in_path, out_path, tlen, (uint32_t)CHIP_HZ);
    return 0;
}
