/*
 * render_from_bytes.c
 *
 * Feeds an arbitrary pre-encoded byte stream (already high-bit-set,
 * exactly as Applesoft's PRINT would emit -- Ctrl-E/Ctrl-V commands
 * and literal text interleaved) through the real 6502 + TMS5220
 * pipeline, captures the resulting real-time audio, and writes a WAV
 * resampled (naive/unfiltered) to a target rate.
 *
 * Usage: render_from_bytes <input_bytes.bin> <output.wav> [target_rate]
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

uint8_t read6502(uint16_t address) {
    if (address >= 0xC0A0 && address <= 0xC0AF) {
        uint8_t raw_status = tms5220_status_r(&tms);
        uint8_t retval;
        if (!readlatch_flag) retval = 0x1f | raw_status;
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

static int trace_on = 0;
static int run_to_halt(uint16_t entry, long max_steps, uint16_t halt_addr,
                        int has_pushed_char, uint8_t pushed_char) {
    mem[halt_addr] = 0x00;
    uint16_t retaddr = halt_addr - 1;
    mem[0x0100 + sp]     = (retaddr >> 8) & 0xFF;
    mem[0x0100 + ((sp - 1) & 0xFF)] = retaddr & 0xFF;
    sp -= 2;
    if (has_pushed_char) {
        mem[0x0100 + sp] = pushed_char;
        sp -= 1;
    }
    pc = entry;
    long steps = 0;
    while (pc != halt_addr && steps < max_steps) {
        if (trace_on && steps < 400)
            printf("  [%5ld] PC=$%04X A=%02X X=%02X Y=%02X SP=%02X op=%02X\n",
                   steps, pc, a, x, y, sp, mem[pc]);
        uint32_t before = clockticks6502;
        step6502(0);
        uint32_t elapsed = clockticks6502 - before;
        tick_chip(elapsed);
        steps++;
    }
    return (int)steps;
}

static int16_t *resample_naive(const int16_t *in, size_t in_count,
                                uint32_t in_rate, uint32_t out_rate,
                                size_t *out_count) {
    if (in_rate == out_rate) {
        int16_t *out = malloc(in_count * sizeof(int16_t));
        memcpy(out, in, in_count * sizeof(int16_t));
        *out_count = in_count;
        return out;
    }
    size_t n_out = (size_t)((double)in_count * out_rate / in_rate);
    int16_t *out = malloc(n_out * sizeof(int16_t));
    double step = (double)in_rate / (double)out_rate;
    for (size_t i = 0; i < n_out; i++) {
        double src_pos = i * step;
        size_t i0 = (size_t)src_pos;
        double frac = src_pos - i0;
        int16_t s0 = (i0 < in_count) ? in[i0] : 0;
        int16_t s1 = (i0 + 1 < in_count) ? in[i0 + 1] : s0;
        out[i] = (int16_t)(s0 + (s1 - s0) * frac);
    }
    *out_count = n_out;
    return out;
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

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <input_bytes.bin> <output.wav> [target_rate]\n", argv[0]);
        return 1;
    }
    const char *inpath = argv[1];
    const char *outpath = argv[2];
    uint32_t target_rate = (argc >= 4) ? (uint32_t)atoi(argv[3]) : 0;

    FILE *f = fopen("roms/textalker.obj.bin", "rb");
    if (!f) { perror("open obj"); return 1; }
    size_t n = fread(mem + 0xD000, 1, 0x3000, f);
    fclose(f);
    fprintf(stderr, "Loaded %zu bytes of TEXTALKER.OBJ at $D000\n", n);

    f = fopen(inpath, "rb");
    if (!f) { perror("open input bytes"); return 1; }
    fseek(f, 0, SEEK_END);
    long inlen = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *input = malloc(inlen);
    fread(input, 1, inlen, f);
    fclose(f);
    fprintf(stderr, "Loaded %ld bytes of input text/commands\n", inlen);

    tms5220_reset(&tms, TMS5220_IS_5220);
    mem[0xBA83] = 0x68; /* PLA */
    mem[0xBA84] = 0x60; /* RTS */
    reset6502();
    sp = 0xFD;

    int steps = run_to_halt(0xD003, 20000, 0x0200, 1, 0x00);
    fprintf(stderr, "Init via $D003: %d steps\n", steps);
    steps = run_to_halt(0xFCD6, 20000, 0x0202, 0, 0);
    fprintf(stderr, "Init via $FCD6: %d steps\n", steps);

    long total_steps = 0;
    for (long i = 0; i < inlen; i++) {
        uint8_t ch = input[i];
        sp = 0xFD;
        size_t audio_before = audio_count;
        trace_on = (i == inlen - 1);
        int s = run_to_halt(0xD006, 2000000L, 0x0201, 1, ch);
        total_steps += s;
        fprintf(stderr, "byte %ld: $%02X ('%c') steps=%d audio_delta=%zu samples (%.3fs) total_audio=%.3fs%s\n",
                i, ch, (ch >= 0xA0 && ch < 0xFF) ? (ch & 0x7f) : '.', s,
                audio_count - audio_before, (audio_count - audio_before) / CHIP_HZ,
                audio_count / CHIP_HZ,
                (s >= 20000000L) ? "  <-- HIT STEP LIMIT" : "");
        if (s >= 20000000L) {
            fprintf(stderr, "WARNING: byte %ld ($%02X) hit step limit without completing!\n", i, ch);
        }
    }
    fprintf(stderr, "Total: %ld 6502 steps across %ld input bytes\n", total_steps, inlen);

    /* Drain any trailing speech. */
    int idle_guard = 0;
    while (tms5220_talk_status(&tms) && idle_guard < 4000000) {
        tick_chip((uint32_t)CYCLES_PER_SAMPLE);
        idle_guard++;
    }

    fprintf(stderr, "Total audio: %zu samples, %.3f seconds at %.0f Hz\n",
            audio_count, audio_count / CHIP_HZ, CHIP_HZ);

    int16_t *final_pcm = audio;
    size_t final_count = audio_count;
    uint32_t final_rate = (uint32_t)CHIP_HZ;

    if (target_rate != 0 && target_rate != (uint32_t)CHIP_HZ) {
        size_t resampled_count;
        int16_t *resampled = resample_naive(audio, audio_count, (uint32_t)CHIP_HZ, target_rate, &resampled_count);
        fprintf(stderr, "Resampled (naive, unfiltered) to %u Hz: %zu samples\n", target_rate, resampled_count);
        final_pcm = resampled;
        final_count = resampled_count;
        final_rate = target_rate;
    }

    wav_write(outpath, final_rate, final_pcm, final_count);
    fprintf(stderr, "Wrote %s\n", outpath);
    return 0;
}
