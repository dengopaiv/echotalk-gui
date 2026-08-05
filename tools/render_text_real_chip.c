/*
 * render_text_real_chip.c -- SUPERSEDED, kept for comparison only.
 *
 * Use tools/render_text_loader.c instead. This harness boots Textalker
 * v3.1.3 the short way -- calling $D003 and $FCD6 directly and skipping
 * the real loader -- which was how the project ran from session 3 to
 * session 10. It produces correct speech, but it is the configuration
 * that exhibits the onset glitch (notes/onset_glitch_investigation_
 * reverted.md), and booting via the real loader is now confirmed to
 * fix that. It also has to hand-write approximations of code the real
 * loader installs at $BA83/$BA88, and stubs "monitor ROM" addresses
 * straight over Textalker's own image because it does not model
 * language-card banking.
 *
 * Retained because a known-different second implementation is useful
 * for A/B comparison, and because the reference sample counts recorded
 * throughout notes/ were produced by it.
 *
 * Reads an arbitrary byte stream from a file (the literal bytes
 * Applesoft would have sent to COUT -- Echo/Textalker control codes and
 * text, high bit NOT set) and renders it through the real 6502 + real
 * ported TMS5220 pipeline, writing 8kHz mono PCM to a WAV file.
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
        return;
    }
    mem[address] = value;
}

/* Wild-jump trap: point the 6502's IRQ/BRK vector ($FFFE/$FFFF) at an
 * address we control (in an unused RAM page, not touched by the
 * loaded OBJ or any of Textalker's own variables) rather than leaving
 * it as zeroed memory. If execution ever drifts into unmapped/zeroed
 * memory again (e.g. an unstubbed real-ROM entry point, same failure
 * mode as the single-letter-word bug), it will hit a BRK (opcode
 * 0x00) and land HERE instead of silently looping through a zero
 * vector back to $0000 forever -- turning what was previously a
 * multi-hour trace-and-guess diagnosis into an immediate, precise
 * error pointing at exactly where the wild jump happened. */
#define TRAP_ADDR 0x0300
static void install_wild_jump_trap(void) {
    mem[0xFFFE] = TRAP_ADDR & 0xFF;
    mem[0xFFFF] = (TRAP_ADDR >> 8) & 0xFF;
}
static void check_wild_jump_trap(void) {
    if (pc != TRAP_ADDR) return;
    /* BRK pushes PCH, PCL, then status (with B set), in that order --
     * so relative to the current (post-push) sp, status is at sp+1,
     * PCL at sp+2, PCH at sp+3. The BRK opcode itself is 2 bytes
     * (opcode + padding byte), so the actual offending instruction
     * address is the pushed return address minus 2. */
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
    if (render_parse_args(argc, argv, 3,
                          "<textalker.obj> <input bytes file> <output.wav>",
                          &g_ropts))
        return 1;
    const char *obj_path = g_ropts.pos[0];
    const char *in_path  = g_ropts.pos[1], *out_path = g_ropts.pos[2];

    FILE *f = fopen(obj_path, "rb");
    if (!f) { perror(obj_path); return 1; }
    size_t n = fread(mem + 0xD000, 1, 0x3000, f);
    fclose(f);
    VLOG("Loaded %zu bytes of TEXTALKER.OBJ at $D000\n", n);

    long tlen = 0;
    uint8_t *text = render_load_input(&g_ropts, in_path, &tlen);

    tms5220_reset(&tms, TMS5220_IS_5220);
    install_wild_jump_trap();
    mem[0xBA83] = 0x68; /* PLA */
    mem[0xBA84] = 0x60; /* RTS */
    /* $D66D branches between two entry points into what's presumably a
     * single real Apple ROM routine based on carry: $BA83 (stubbed
     * above) and $BA88 (not previously stubbed). Since $BA88 falls
     * outside our loaded OBJ's address range, it was pure zeroed
     * memory -- executing BRK there, then vectoring through the
     * (also zero) $FFFE/$FFFF IRQ vector straight back to $0000,
     * forming a genuine infinite BRK loop. This is the root cause of
     * the single-letter-word ("B") hang. Stub it the same way as its
     * sibling entry point. */
    mem[0xBA88] = 0x68; /* PLA */
    mem[0xBA89] = 0x60; /* RTS */
    reset6502();
    sp = 0xFD;

    int steps = run_to_halt(0xD003, 20000, 0x0200, 1, 0x00);
    VLOG("Init via $D003: %d steps\n", steps);
    steps = run_to_halt(0xFCD6, 20000, 0x0202, 0, 0);
    VLOG("Init via $FCD6: %d steps (card detection %s -- FD87=%02X)\n",
         steps, mem[0xFD87] == 0x1F ? "SUCCEEDED" : "FAILED", mem[0xFD87]);
    if (mem[0xFD87] != 0x1F)
        fprintf(stderr, "WARNING: card detection did not succeed (FD87=$%02X)\n", mem[0xFD87]);

    if (g_ropts.repeat_filter_fix) {
        for (const char *p = REPEAT_FILTER_DISABLE; *p; p++) {
            sp = 0xFD;
            run_to_halt(0xD006, 5000000, 0x0201, 1, (uint8_t)(*p | 0x80));
        }
        VLOG("Sent repeat-filter disable (Ctrl-E 99 R)\n");
    }

    for (long i = 0; i < tlen; i++) {
        uint8_t ch = text[i] | 0x80;
        /* Mark where each utterance begins (start of text, and after
         * every CR) so its think-time dead air can be trimmed. */
        if (i == 0 || text[i - 1] == '\r') render_mark_utterance();
        sp = 0xFD;
        int s = run_to_halt(0xD006, 5000000, 0x0201, 1, ch);
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
