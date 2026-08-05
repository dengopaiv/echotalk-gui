/*
 * render_text_loader.c -- THE CANONICAL TEXTALKER HARNESS.
 *
 * Handles BOTH Textalker versions from one binary. Give it any
 * loader/OBJ pair -- v3.1.3 or v1.3 -- and it works out the rest:
 *
 *   - where the OBJ loads, from the trampoline templates in the loader
 *     ($D0xx targets mean $D000, $D4xx mean $D400)
 *   - where the character entry is, by running the loader and then
 *     asking through DOS's own hook at $BA69, which leaves the answer
 *     in the Apple II output vector CSWL ($36/$37)
 *
 * Nothing is keyed to a particular build, so a 3.1.2 or 3.1.4 image
 * should work without being recognised individually. Only two things
 * remain version-specific, both scoped to the $D000 family and both
 * only diagnostics: the $FD87 card-detection flag and the $FCD6
 * fallback, neither of which means anything in a v1.3 image.
 * See notes/multi_version_support_design.md.
 *
 * It boots by running Textalker's REAL loader rather than calling
 * $D003/$FCD6 directly the way tools/render_text_real_chip.c does. Use
 * this one; the direct-init harness is kept only for comparison, since
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
static int true_timing = 1;
static int arrival_trace = 0;

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
static uint16_t pc_watch = 0;  /* set via ECHOTALK_PC_WATCH=D781 */

static inline int is_lc_space(uint16_t address) { return address >= 0xD000; }

#define audio_count render_audio_count()

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
static unsigned long frames_played = 0;
static int resetl4_trace = 0;
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
    /* Resolve any pending /READY cycle first. Its delays are 13us for a
     * read and 25us for a write, both far shorter than the 125us
     * between generated samples, so this is driven off elapsed CPU
     * cycles (~1us each) rather than the sample clock -- otherwise
     * every delay would round up to a whole sample. */
    if (true_timing)
        tms5220_tick_ready_timer(&tms, elapsed_cpu_cycles * (1000000.0 / CPU_HZ));

    tick_accumulator += elapsed_cpu_cycles;
    while (tick_accumulator >= CYCLES_PER_SAMPLE) {
        int16_t sample;
        static int prev_ip = -1, prev_talkd = -1;
        /* Capture pre-call state so a RESETL4 can be reported in exactly
         * MAME's format: its "about to update status" line is the state
         * on entry, "status updated" the state after the TALKD latch and
         * the conditional TALK set. Detecting it from outside the core
         * (an IP 7->0 wrap from PC=12) keeps the ported code untouched. */
        int b_ip = tms.m_IP, b_pc = tms.m_PC, b_sub = tms.m_subcycle;
        int b_spen = tms.m_SPEN, b_talk = tms.m_TALK, b_talkd = tms.m_TALKD;

        tms5220_process(&tms, &sample, 1);

        if (b_ip == 7 && b_pc == 12 && tms.m_IP == 0) {
            if (b_talkd) frames_played++;
            if (resetl4_trace) {
                fprintf(stderr,
                    "RESETL4, about to update status: IP=%d, PC=%d, subcycle=%d, m_SPEN=%d, m_TALK=%d, m_TALKD=%d\n",
                    b_ip, b_pc, b_sub, b_spen, b_talk, b_talkd);
                fprintf(stderr,
                    "RESETL4, status updated: t=%.2fms IP=%d, PC=%d, subcycle=%d, m_SPEN=%d, m_TALK=%d, m_TALKD=%d\n",
                    audio_count/8.0, b_ip, b_pc, b_sub, tms.m_SPEN, tms.m_TALK, tms.m_TALKD);
            }
        }
        prev_ip = tms.m_IP; prev_talkd = tms.m_TALKD;
        (void)prev_ip; (void)prev_talkd;
        /* TALKD distinguishes silence the chip is playing (a real pause)
         * from the chip sitting idle while the 6502 thinks (dead air). */
        render_audio_push(sample, tms.m_TALKD);
        if (chip_trace) trace_chip_state();
        tick_accumulator -= CYCLES_PER_SAMPLE;
    }
}

static int echo_write_count = 0;

/* Which PC reads the status port, and how often (ECHOTALK_POLL_TRACE).
 * A polling loop shows up as one address with an enormous count; the
 * instruction there says which status bit Textalker is waiting on. */
static unsigned long poll_pc_count[0x10000];
static int poll_trace = 0;

/* --- Echo II card, modelled on a2echoii.cpp ---
 *
 * Every address in $C0A0-$C0AF hits the same pair of latches. Reads
 * alternate between a real status byte and the bus pull-up value, with
 * the 74C74 inverting itself on each access and driving /RS. Writes
 * latch a byte into the 74LS373 and pull /WS low; the chip takes it
 * when it is ready, and the /READY edge is what releases the latch for
 * the next write. If the host writes again before that, the latched
 * byte is lost -- which is worth warning about, since it means data
 * never reached the chip.
 *
 * The alternating read is not a quirk to work around: Textalker's own
 * status helper at $FCC9 reads the port twice with six ROR A of delay
 * between, which is exactly long enough for /READY to come back after
 * /RS falls. The first read arms the latch, the second collects it. */
static uint8_t writelatch_data = 0xff;
static int writelatch_flag = 1;

static void echoii_readyq(void *ctx, int state) {
    (void)ctx;
    if (state) return;          /* rising edge of /READY does nothing */
    writelatch_flag = 1;        /* chip is ready: release the write latch */
    tms5220_wsq_w(&tms, 1, 0);
}

uint8_t read6502(uint16_t address) {
    if (address >= 0xC0A0 && address <= 0xC0AF) {
        uint8_t retval;
        if (poll_trace) poll_pc_count[pc]++;
        if (!readlatch_flag) retval = 0x1f | tms5220_status_r(&tms);
        else retval = 0xff;
        /* on the rising edge of /DEVREAD, i.e. after the read: the
         * latch inverts itself and drives /RS */
        readlatch_flag = !readlatch_flag;
        if (true_timing) tms5220_rsq_w(&tms, readlatch_flag);
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

/* Byte-stream dump (ECHOTALK_BYTE_DUMP=<file>): every byte Textalker
 * writes to the Echo II latch, in order. Directly comparable with
 * MAME's own "Data written to latch of %02x" log lines, which is how
 * the two implementations' Textalker behaviour gets compared. */
static FILE *byte_dump = NULL;

void write6502(uint16_t address, uint8_t value) {
    if (address >= 0xC0A0 && address <= 0xC0AF) {
        if (byte_dump) fprintf(byte_dump, "%02x ", value);
        /* Arrival phase (ECHOTALK_ARRIVAL=1), in the same shape as the
         * instrumentation added to MAME's tms5220.cpp data_w, so the
         * two runs can be compared byte for byte. What matters is the
         * IP: whether SPEAK EXTERNAL lands early or late within the
         * frame is what decides one idle frame versus two. */
        if (arrival_trace)
            fprintf(stderr, "tms5220_write_data: data %02X at IP=%d PC=%d subcycle=%d "
                            "(TALKD=%d TALK=%d SPEN=%d DDIS=%d fifo=%d)\n",
                    value, tms.m_IP, tms.m_PC, tms.m_subcycle,
                    tms.m_TALKD, tms.m_TALK, tms.m_SPEN, tms.m_DDIS, tms.m_fifo_count);
        if (true_timing) {
            if (!writelatch_flag)
                fprintf(stderr, "WARNING: echo II latch (%02X) clobbered by %02X "
                                "before the chip read it\n", writelatch_data, value);
            writelatch_data = value;
            writelatch_flag = 0;             /* /DEVWRITE clears it on the falling edge */
            tms5220_wsq_w(&tms, 0, value);
            tms5220_data_w(&tms, writelatch_data);
        } else {
            tms5220_data_w(&tms, value);
        }
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
        if (pc_watch && pc == pc_watch) {
            fprintf(stderr, "[pc] reached $%04X (A=%02X X=%02X Y=%02X)\n", pc, a, x, y);
        }
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

    /* The loader is read first, because it says where the OBJ goes. */
    FILE *fr = fopen(ram_path, "rb");
    if (!fr) { perror(ram_path); return 1; }
    size_t nr = fread(mem + 0x9300, 1, 0x2000, fr);
    fclose(fr);
    VLOG("Loaded %zu bytes of TEXTALKER.RAM (loader) at $9300\n", nr);

    /* Derive the OBJ load address rather than hardcoding it.
     *
     * Every loader carries templates of the trampoline it installs,
     * PHA / LDA $C08B / JMP <entry>, and those entries are the OBJ's own
     * jump table -- $D003/$D006/$D009 for v3.1.3, $D400/$D403 for v1.3.
     * The page of any of them is therefore where the image belongs:
     * $D0xx means $D000, $D4xx means $D400.
     *
     * This is what lets one binary handle both versions, and it
     * generalises to builds we have never seen: a 3.1.2 or 3.1.4 image
     * shares 3.1.3's layout and lands on the same answer without being
     * recognised individually. */
    uint16_t obj_addr = 0;
    for (size_t i = 0; i + 6 < nr; i++) {
        const uint8_t *p = mem + 0x9300 + i;
        if (p[0] == 0x48 && p[1] == 0xAD && p[2] == 0x8B &&
            p[3] == 0xC0 && p[4] == 0x4C) {
            obj_addr = (uint16_t)(p[6] << 8);
            break;
        }
    }
    if (!obj_addr) {
        fprintf(stderr, "ERROR: no entry trampoline (PHA/LDA $C08B/JMP) in the "
                        "loader -- cannot tell where the OBJ loads\n");
        return 1;
    }
    VLOG("Loader says the OBJ loads at $%04X\n", obj_addr);

    FILE *fo = fopen(obj_path, "rb");
    if (!fo) { perror(obj_path); return 1; }
    size_t no = fread(mem + obj_addr, 1, 0x10000 - obj_addr, fo);
    fclose(fo);
    VLOG("Loaded %zu bytes of TEXTALKER.OBJ at $%04X\n", no, obj_addr);

    long tlen = 0;
    uint8_t *text = render_load_input(&g_ropts, in_path, &tlen);

    bank_trace = getenv("ECHOTALK_BANK_TRACE") != NULL;
    chip_trace = getenv("ECHOTALK_CHIP_TRACE") != NULL;
    resetl4_trace = getenv("ECHOTALK_RESETL4") != NULL;
    poll_trace = getenv("ECHOTALK_POLL_TRACE") != NULL;
    arrival_trace = getenv("ECHOTALK_ARRIVAL") != NULL;
    { const char *bd = getenv("ECHOTALK_BYTE_DUMP");
      if (bd) byte_dump = fopen(bd, "w"); }
    { const char *w = getenv("ECHOTALK_PC_WATCH"); if (w) pc_watch = (uint16_t)strtol(w, NULL, 16); }
    { const char *hz = getenv("ECHOTALK_CPU_HZ");
      if (hz) { cycles_per_sample = atof(hz) / CHIP_HZ;
                fprintf(stderr, "[experiment] CPU %s Hz -> %.3f cycles/sample\n",
                        hz, cycles_per_sample); } }

    /* True timing (ECHOTALK_TRUE_TIMING=1) drives the chip through the
     * real /RS, /WS and /READY handshake the way the Echo II card does,
     * instead of MAME's "hacky instant write mode".
     *
     * NOT the default, deliberately. It produces byte-identical output
     * -- unsurprising once the magnitudes are compared, since /READY
     * delays are 13-25us against a 25ms frame -- while also losing 17
     * bytes to write-latch clobbering that MAME never suffers. Until
     * that is understood it is strictly worse, so it stays opt-in. */
    true_timing = getenv("ECHOTALK_TRUE_TIMING") != NULL;

    tms5220_reset(&tms, TMS5220_IS_5220);

    /* Frame rate. Written after reset because reset clears it. This is
     * the field the SET RATE command would write on a 5220C; on a plain
     * 5220 that command is a NOP, so the field stays 0 and every frame
     * gets all 8 interpolation periods. Setting it directly is an
     * emulator capability -- real Echo II hardware could not do this. */
    tms.m_configured_rate = tms.m_c_variant_rate = (uint8_t)g_ropts.frame_rate;
    if (g_ropts.frame_rate)
        VLOG("Frame rate %d: %d interpolation periods per frame\n",
             g_ropts.frame_rate, 8 - 2 * g_ropts.frame_rate);
    if (true_timing) {
        /* Installed after reset so the reset's own update_ready_state
         * does not fire into it. Then mirror a2echoii's reset_from_bus:
         * /RESET presets the read latch, which drives /RS high. */
        tms.m_readyq_handler = echoii_readyq;
        tms.m_readyq_ctx = NULL;
        readlatch_flag = 1;
        tms5220_rsq_w(&tms, readlatch_flag);
    }

    /* Experiment (ECHOTALK_PHASE=N): advance the chip's internal frame
     * counters by N samples before anything else runs, shifting the
     * phase of its frame clock relative to the 6502 without changing
     * either clock's rate. A restart costs one idle frame or two purely
     * according to whether SPEN is set before or after the RESETL4 that
     * clears TALKD, so if we are landing on the wrong side of that
     * boundary, some phase offset should flip it. */
    { const char *ph = getenv("ECHOTALK_PHASE");
      if (ph) { int n = atoi(ph); int16_t junk;
                for (int i = 0; i < n; i++) tms5220_process(&tms, &junk, 1); } }

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

    /* Text window width (WNDWDTH, zero page $21) and the 80-column
     * hardware flag. Textalker derives its line-buffer size from these
     * at $D781 -- without them the buffer bounds at $FD80-$FD82 stay
     * zero and the auto-flush boundary is undefined, which is what
     * caused speech to break mid-word at arbitrary offsets. */
    { const char *w = getenv("ECHOTALK_WIDTH");
      int width = w ? atoi(w) : 40;
      mem[0x0021] = (uint8_t)width;
      mem[0x0020] = 0x00;                    /* WNDLFT */
      mem[0xC01F] = width > 40 ? 0x80 : 0x00; /* RD80COL: bit 7 = 80-col active */
    }

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
    /* Textalker's line-buffer bounds, set from the screen width at
     * $D781 (see notes/buffer_chunking_and_indexing.md): $FD80 is
     * width+1, $FD81 width-1, $FD82 width. The per-character dispatch
     * loop auto-flushes -- i.e. speaks -- when the buffer reaches this
     * boundary, wherever in the text that happens to fall. */
    VLOG("  line-buffer bounds: $FD80=%d $FD81=%d $FD82=%d, $C01F(80col)=$%02X\n",
         mem[0xFD80], mem[0xFD81], mem[0xFD82], mem[0xC01F]);
    /* $FD87 is v3.1.3's card-detection flag. In a v1.3 image that
     * address is ordinary code, so the check only means anything for
     * the $D000 family -- checking it regardless produced a false
     * alarm on v1.3, whose own indicators are $EC0B and $F48F. */
    if (obj_addr == 0xD000 && mem[0xFD87] != 0x1F)
        fprintf(stderr, "WARNING: card detection did not succeed (FD87=$%02X)\n", mem[0xFD87]);

    /* Locate the per-character entry rather than hardcoding $BA7C: the
     * loader installs a PHA / LDA $C08B / JMP <entry> trampoline, and
     * finding it works identically for v1.3 (which lands at $BA82), so
     * one code path can serve both versions. The loader's own image is
     * skipped because it holds the templates it copies from, and the
     * first of those is JMP $D003 -- the init entry, not this one. */
    /* Ask Textalker where its character entry is, rather than guessing.
     *
     * Both versions have DOS's hook slot at $A22B patched to JMP $BA69,
     * and both branch there on the Z flag. With Z set, v3.1.3 goes to
     * its init trampoline ($BA72 -> $D003 -> $D682), which installs the
     * Apple II character-output vector CSWL at $36/$37; v1.3 instead
     * falls through to code that stores $BA82 into $36/$37 directly.
     * Either way the vector ends up holding the character entry, which
     * is exactly what a caller needs and is how the machine itself
     * finds it. No pattern matching, no version knowledge. */
    uint16_t entry;
    {
        uint8_t prev_lo = mem[0x0036], prev_hi = mem[0x0037];
        sp = 0xFD;
        status |= 0x02; /* Z set: take the install path in both versions */
        run_to_halt(0xBA69, 200000, 0x0203, 0, 0);
        entry = (uint16_t)(mem[0x0036] | (mem[0x0037] << 8));
        VLOG("  CSWL after $BA69 install: $%04X (was $%02X%02X)\n",
             entry, prev_hi, prev_lo);
        /* Validate: the vector must point at a PHA / LDA $C08B / JMP
         * trampoline. If it does not, the install path did not run and
         * speaking through it would produce silence or worse. */
        if (!(mem[entry] == 0x48 && mem[entry + 1] == 0xAD &&
              mem[entry + 2] == 0x8B && mem[entry + 3] == 0xC0 &&
              mem[entry + 4] == 0x4C)) {
            fprintf(stderr, "ERROR: CSWL ($%04X) does not point at an entry "
                            "trampoline -- Textalker did not install itself\n", entry);
            return 1;
        }
    }

    if (g_ropts.verbose) {
        fprintf(stderr, "  $A22B (DOS hook slot): %02X %02X %02X\n",
                mem[0xA22B], mem[0xA22C], mem[0xA22D]);
        fprintf(stderr, "  $BA69:");
        for (uint16_t addr = 0xBA69; addr <= 0xBA90; addr++)
            fprintf(stderr, " %02X", mem[addr]);
        fprintf(stderr, "\n");
    }

    /* List the trampolines the loader installed. v3.1.3 installs
     * several -- one per public entry -- so this is a diagnostic, not a
     * way to choose. Taking the first match lands on JMP $D003, the
     * init entry, and produces silence. See
     * notes/multi_version_support_design.md: identifying which
     * trampoline is the character hook needs probing, not pattern
     * matching. The character entry for v3.1.3 is the JMP $D006 one. */
    if (g_ropts.verbose) {
        for (uint32_t addr = 0x0200; addr <= 0xBFF9; addr++) {
            if (addr >= 0x9300 && addr < 0x9300 + nr) continue;
            if (mem[addr] == 0x48 && mem[addr + 1] == 0xAD &&
                mem[addr + 2] == 0x8B && mem[addr + 3] == 0xC0 &&
                mem[addr + 4] == 0x4C)
                fprintf(stderr, "  trampoline at $%04X -> JMP $%02X%02X%s\n",
                        (unsigned)addr, mem[addr + 6], mem[addr + 5],
                        addr == entry ? "   <- character entry" : "");
        }
    }

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
    if (obj_addr == 0xD000 && mem[0xFD87] != 0x1F) {
        /* $FCD6 is a v3.1.3 address too; in a v1.3 image it lands in
         * the middle of unrelated code, so this fallback is scoped to
         * the same family as the flag that triggers it. */
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
        run_to_halt(entry, 5000000, 0x0201, 0, 0);               \
    } while (0)

    /* Disable the repeat-character filter now that init is complete and
     * before any real text -- see REPEAT_FILTER_DISABLE. */
    if (g_ropts.repeat_filter_fix) {
        for (const char *p = REPEAT_FILTER_DISABLE; *p; p++) SEND_CHAR(*p);
        VLOG("Sent repeat-filter disable (Ctrl-E 99 R)\n");
    }

    for (long i = 0; i < tlen; i++) {
        uint8_t ch = text[i] | 0x80;
        /* A new utterance begins at the start of the text and after
         * every CR; that is where Textalker's think-time dead air
         * appears, so mark it before any of it is generated. */
        if (i == 0 || text[i - 1] == '\r') render_mark_utterance();
        sp = 0xFD;
        a = ch;
        int s = run_to_halt(entry, 5000000, 0x0201, 0, 0);
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

    fprintf(stderr, "  frames played: %lu\n", frames_played);
    if (poll_trace) {
        for (int i = 0; i < 0x10000; i++)
            if (poll_pc_count[i] > 0)
                fprintf(stderr, "  status-port reads from PC=$%04X: %lu\n",
                        i, poll_pc_count[i]);
    }
    render_finish(&g_ropts, in_path, out_path, tlen, (uint32_t)CHIP_HZ);
    return 0;
}
