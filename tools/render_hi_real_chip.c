/*
 * render_hi_real_chip.c
 *
 * The real thing: boots Textalker (v3.1.3, via the no-loader init
 * sequence proven in probe_no_loader.c), and instead of a fake
 * "always ready" Echo-card status stub, wires the Echo I/O port
 * directly to the real ported TMS5220 core (third_party/tms5220_core).
 * The chip is advanced in real time, paced against actual elapsed
 * 6502 cycles (Apple II NTSC ~1.02MHz vs the TMS5220's 8000Hz sample
 * rate at the Echo II's standard 640kHz clock), so Textalker's status
 * polling gets genuine feedback instead of a canned value. Captures
 * whatever audio comes out, in real time, exactly as it would on
 * real hardware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tms5220_core.h"

extern uint16_t pc;
extern uint8_t sp, a, x, y, status;
extern uint32_t clockticks6502;
extern void reset6502(void);
extern void step6502(int printRegs);

static uint8_t mem[0x10000];

static tms5220_state tms;
static int readlatch_flag = 1;

/* Audio capture */
static int16_t *audio = NULL;
static size_t audio_count = 0, audio_cap = 0;
static void audio_push(int16_t s) {
    if (audio_count >= audio_cap) {
        audio_cap = audio_cap ? audio_cap * 2 : 65536;
        audio = realloc(audio, audio_cap * sizeof(int16_t));
    }
    audio[audio_count++] = s;
}

/* Timing: Apple II NTSC CPU ~1,020,484 Hz; Echo II TMS5220 clock
 * 640,000 Hz -> 8,000 samples/sec (clock/80, see a2echoii.cpp). */
#define CPU_HZ      1020484.0
#define CHIP_HZ     8000.0
#define CYCLES_PER_SAMPLE (CPU_HZ / CHIP_HZ)
static double tick_accumulator = 0.0;

static void tick_chip(uint32_t elapsed_cpu_cycles) {
    tick_accumulator += elapsed_cpu_cycles;
    while (tick_accumulator >= CYCLES_PER_SAMPLE) {
        int16_t sample;
        tms5220_process(&tms, &sample, 1);
        audio_push(sample);
        tick_accumulator -= CYCLES_PER_SAMPLE;
    }
}

static int echo_log = 0;

static void dump_tms_state(const char *label) {
    printf("--- TMS state [%s]: SPEN=%d TALK=%d TALKD=%d DDIS=%d cmd_reg=%02X RDB=%d io_ready=%d fifo_count=%d ---\n",
           label, tms.m_SPEN, tms.m_TALK, tms.m_TALKD, tms.m_DDIS,
           tms.m_command_register, tms.m_RDB_flag, tms.m_io_ready, tms.m_fifo_count);
}
static int write_count = 0;

uint8_t read6502(uint16_t address) {
    if (address >= 0xC0A0 && address <= 0xC0AF) {
        int flag_at_entry = readlatch_flag;
        uint8_t raw_status = tms5220_status_r(&tms);
        uint8_t retval;
        if (!readlatch_flag) {
            retval = 0x1f | raw_status;
        } else {
            retval = 0xff;
        }
        readlatch_flag = !readlatch_flag;
        if (echo_log)
            printf("      [ECHO READ] addr=$%04X flag_at_entry=%d raw_status=%02X retval=%02X at PC=$%04X\n",
                   address, flag_at_entry, raw_status, retval, pc);
        return retval;
    }
    return mem[address];
}

void write6502(uint16_t address, uint8_t value) {
    if (address >= 0xC0A0 && address <= 0xC0AF) {
        tms5220_data_w(&tms, value);
        write_count++;
        if (echo_log) printf("    >> write $%02X (#%d)\n", value, write_count);
        return;
    }
    if (address == 0xFD87) printf("      [FD87 WRITE] old=%02X new=%02X at PC=$%04X\n", mem[address], value, pc);
    mem[address] = value;
}

static int run_to_halt(uint16_t entry, int max_steps, uint16_t halt_addr,
                        int has_pushed_char, uint8_t pushed_char, int trace) {
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
#define RINGSZ 60
    uint16_t ring[RINGSZ]; uint8_t ring_a[RINGSZ], ring_x[RINGSZ], ring_y[RINGSZ];
    while (pc != halt_addr && steps < max_steps) {
        if (trace && (steps % 3000 == 0))
            printf("  [chipstate %6d] SPEN=%d TALK=%d TALKD=%d DDIS=%d fifo_count=%d fifo_head=%d fifo_tail=%d cmd_reg=%02X RDB=%d\n",
                   steps, tms.m_SPEN, tms.m_TALK, tms.m_TALKD, tms.m_DDIS,
                   tms.m_fifo_count, tms.m_fifo_head, tms.m_fifo_tail, tms.m_command_register, tms.m_RDB_flag);
            printf("  [%6d] PC=$%04X A=%02X X=%02X Y=%02X SP=%02X op=%02X\n",
                   steps, pc, a, x, y, sp, mem[pc]);
        ring[steps % RINGSZ] = pc;
        ring_a[steps % RINGSZ] = a; ring_x[steps % RINGSZ] = x; ring_y[steps % RINGSZ] = y;
        uint32_t before = clockticks6502;
        step6502(0);
        uint32_t elapsed = clockticks6502 - before;
        tick_chip(elapsed);
        steps++;
    }
    if (steps >= max_steps) {
        printf("  *** hit budget -- last %d PCs (oldest first): ***\n", RINGSZ);
        int start = steps < RINGSZ ? 0 : steps % RINGSZ;
        for (int k = 0; k < (steps < RINGSZ ? steps : RINGSZ); k++) {
            int idx = (start + k) % RINGSZ;
            printf("    PC=$%04X A=%02X X=%02X Y=%02X\n", ring[idx], ring_a[idx], ring_x[idx], ring_y[idx]);
        }
    }
    return steps;
}

static void wav_write(const char *path, uint32_t rate, const int16_t *pcm, size_t n) {
    FILE *f = fopen(path, "wb");
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

int main(void) {
    FILE *f = fopen("roms/textalker.obj.bin", "rb");
    if (!f) { perror("open obj"); return 1; }
    size_t n = fread(mem + 0xD000, 1, 0x3000, f);
    fclose(f);
    printf("Loaded %zu bytes of TEXTALKER.OBJ at $D000\n", n);

    tms5220_reset(&tms, TMS5220_IS_5220);
    mem[0xBA83] = 0x68; /* PLA */
    mem[0xBA84] = 0x60; /* RTS */
    reset6502();
    sp = 0xFD;

    dump_tms_state("before D003");
    int steps = run_to_halt(0xD003, 20000, 0x0200, 1, 0x00, 0);
    printf("Init via $D003: %d steps\n", steps);
    dump_tms_state("after D003");
    steps = run_to_halt(0xFCD6, 5000, 0x0202, 0, 0, 0);
    printf("Init via $FCD6: %d steps\n", steps);
    dump_tms_state("after FCD6");

    const char *text = "\x05" "B" "\x8d";
    for (const char *p = text; *p; p++) {
        uint8_t ch = (uint8_t)(*p) | 0x80;
        write_count = 0;
        echo_log = 0;
        sp = 0xFD;
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "before char $%02X", ch);
        dump_tms_state(lbl);
        int s = run_to_halt(0xD006, (ch == 0x8d) ? 300000 : 5000000, 0x0201, 1, ch, (ch == 0x8d));
        printf("char $%02X: %d 6502-steps, %d echo-writes, audio so far=%zu samples (%.3fs)\n",
               ch, s, write_count, audio_count, audio_count / CHIP_HZ);
        snprintf(lbl, sizeof(lbl), "after char $%02X", ch);
        dump_tms_state(lbl);
    }

    /* Keep ticking (with the CPU idle at the halt address, i.e. no
     * more 6502 work, just advance real time) until the chip finishes
     * talking, so we capture the speech tail. */
    int idle_guard = 0;
    while (tms5220_talk_status(&tms) && idle_guard < 200000) {
        tick_chip((uint32_t)CYCLES_PER_SAMPLE);
        idle_guard++;
    }

    printf("\nTotal audio: %zu samples, %.3f seconds at %.0f Hz\n",
           audio_count, audio_count / CHIP_HZ, CHIP_HZ);

    wav_write("/tmp/hi_real_chip.wav", (uint32_t)CHIP_HZ, audio, audio_count);
    printf("Wrote /tmp/hi_real_chip.wav\n");
    return 0;
}
