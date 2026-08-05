/*
 * boot_and_probe.c
 *
 * Runs the REAL TEXTALKER.RAM loader ($9300) against a minimal Apple II
 * "monitor ROM" stub, so we can observe what it actually sets up (the
 * $36/$37 CSWL output-hook vector in particular), then tries calling
 * the documented entry point ($FDED, "GOUT") with test characters and
 * watches for bytes reaching the Echo II card.
 *
 * Stubbed ROM/DOS routines (all become RTS, i.e. harmless no-ops,
 * except $FDED itself which does the real hardware's JMP ($36)):
 *   $FDED  JMP ($0036)      -- the real monitor ROM COUT behavior
 *   $FDF0  RTS               -- COUT1 (real screen print) fallback
 *   $FC58  RTS               -- HOME
 *   $FBFD  RTS               -- used by the loader's banner-print loop
 *   $9EBD  RTS               -- LC-bank-2 patch loop (not needed for TTS)
 *   $FE95  RTS               -- DOS break-vector chain-through
 *   $C300  RTS               -- slot-3 firmware (Ctrl-C path, unused)
 *   $FE1F  RTS               -- monitor utility (only reached if $FBB3==6)
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern uint16_t pc;
extern uint8_t sp, a, x, y, status;
extern void reset6502(void);
extern void step6502(int printRegs);

static uint8_t mem[0x10000];

static int readlatch_flag = 1;
static int writelatch_flag = 1;
static uint8_t writelatch_data = 0xff;
static int write_count = 0;
static uint8_t last_writes[64];

static uint8_t fake_tms_status(void) { return 0x20; /* BE=1, idle */ }

uint8_t read6502(uint16_t address) {
    if (address >= 0xC0A0 && address <= 0xC0AF) {
        uint8_t retval = readlatch_flag ? 0xff : (0x1f | fake_tms_status());
        readlatch_flag = !readlatch_flag;
        return retval;
    }
    return mem[address];
}

void write6502(uint16_t address, uint8_t value) {
    if (address >= 0xC0A0 && address <= 0xC0AF) {
        writelatch_data = value;
        writelatch_flag = 1; /* accept immediately for this probe */
        if (write_count < (int)sizeof(last_writes))
            last_writes[write_count] = value;
        write_count++;
        return;
    }
    mem[address] = value;
}

static void stub(uint16_t addr, uint8_t opcode) { mem[addr] = opcode; }

/* Run the 6502 from `entry`, with a fake JSR (pushes halt-1 so RTS lands
 * exactly on HALT_ADDR), for up to max_steps instructions. */
static int run_to_halt(uint16_t entry, int max_steps, uint16_t halt_addr, int trace) {
    mem[halt_addr] = 0x00; /* BRK sentinel, never actually reached-into */
    uint16_t retaddr = halt_addr - 1;
    mem[0x0100 + sp]     = (retaddr >> 8) & 0xFF;
    mem[0x0100 + ((sp - 1) & 0xFF)] = retaddr & 0xFF;
    sp -= 2;
    /* TEXTALKER.RAM's prologue saves the caller's SP/A/Y from DOS's
     * register-save slots ($AA59/$AA5C/$AA5B) so it can restore them
     * and RTS cleanly at the end. We jammed PC in directly rather than
     * via a real JSR from DOS, so that slot would otherwise be zero --
     * seed it with our *current* SP so the loader's final TXS+RTS lands
     * back on the fake return address we just pushed. */
    mem[0xAA59] = sp;
    mem[0xAA5C] = 0;
    mem[0xAA5B] = 0;
    pc = entry;
    int steps = 0;
    while (pc != halt_addr && steps < max_steps) {
        if (trace && steps < 800) {
            printf("  [%5d] PC=$%04X A=%02X X=%02X Y=%02X SP=%02X op=%02X\n",
                   steps, pc, a, x, y, sp, mem[pc]);
        }
        step6502(0);
        steps++;
    }
    return steps;
}

int main(void) {
    FILE *f;
    size_t n;

    f = fopen("roms/textalker.obj.bin", "rb");
    if (!f) { perror("open obj"); return 1; }
    n = fread(mem + 0xD000, 1, 0x3000, f);
    fclose(f);
    printf("Loaded %zu bytes of TEXTALKER.OBJ at $D000\n", n);

    f = fopen("roms/textalker.ram.bin", "rb");
    if (!f) { perror("open ram"); return 1; }
    n = fread(mem + 0x9300, 1, 0x300, f);
    fclose(f);
    printf("Loaded %zu bytes of TEXTALKER.RAM (loader) at $9300\n", n);

    /* --- minimal monitor ROM stub ---
     * We traced the loader's own patch code (copied to $BCF0) and
     * confirmed it does: PHA / LDA $C08B / JMP $D009 / ... / JMP $FD1B
     * on real hardware, reached via a DOS-internal COUT hook we don't
     * have (DOS 3.3 itself owns $36/$37 at boot, before Textalker ever
     * touches it). Rather than reimplement DOS 3.3, we call the proven
     * real entry point ($D009) directly -- this *is* what "JSR $FDED"
     * ends up doing on a real machine once Textalker is installed. */
    mem[0xFDED] = 0x6C; mem[0xFDEE] = 0x36; mem[0xFDEF] = 0x00; /* JMP ($0036), real hardware behavior */
    stub(0xFDF0, 0x60); /* COUT1 fallback: harmless during boot's own banner-print loop */

    /* Our own tiny trampoline mirroring the real $BCF0 code the loader
     * itself installs (PHA / JMP $D009) -- placed in scratch RAM. We
     * point $36/$37 at this only *after* boot, once Textalker is fully
     * initialized, mirroring what DOS+Textalker's real hook chain does
     * on real hardware (which we don't have modeled). */
    const uint16_t TRAMPOLINE = 0x0300;
    mem[TRAMPOLINE + 0] = 0x48;                                       /* PHA */
    mem[TRAMPOLINE + 1] = 0x4C; mem[TRAMPOLINE+2] = 0x09; mem[TRAMPOLINE+3] = 0xD0; /* JMP $D009 */
    stub(0xFC58, 0x60);
    stub(0xFBFD, 0x60);
    stub(0x9EBD, 0x60);
    stub(0xFE95, 0x60);
    stub(0xC300, 0x60);
    stub(0xFE1F, 0x60);
    mem[0xFBB3] = 0xEA; /* real Apple II ROM signature byte the loader
                            checks for machine-type detection; without
                            this it misdetects "unusual hardware" and
                            takes a delay/keyboard-poll compatibility
                            path that never returns in a headless sim */

    reset6502();
    sp = 0xFD;
    mem[0x0036] = 0xF0; /* CSWL/CSWH -- reset-time default: points at COUT1 stub */
    mem[0x0037] = 0xFD;
    mem[0x0024] = 0x00; /* CH: cursor column */
    mem[0x0028] = 0x00; /* BASL/BASH: current screen line base address -- */
    mem[0x0029] = 0x04; /* point at real text page 1 ($0400), not zero page */

    int steps = run_to_halt(0x9300, 20000, 0x0200, 0);
    printf("Loader ran %d instructions, stopped at PC=$%04X\n", steps, pc);
    printf("After boot: zero-page $36/$37 (CSWL) = $%02X%02X\n", mem[0x0037], mem[0x0036]);
    printf("Memory at $A22B (DOS hook slot): %02X %02X %02X\n",
           mem[0xA22B], mem[0xA22C], mem[0xA22D]);
    printf("Echo-card writes seen during boot: %d\n", write_count);

    /* Now install the hook, mirroring what real DOS+Textalker would
     * have wired up by this point. */
    mem[0x0036] = (uint8_t)(TRAMPOLINE & 0xFF);
    mem[0x0037] = (uint8_t)(TRAMPOLINE >> 8);
    printf("Installed CSWL -> $%04X trampoline -> $D009\n\n", TRAMPOLINE);

    /* Put Textalker into "Talk only" mode first (Ctrl-E, T) -- this
     * should suppress screen echo entirely, meaning the cursor-blink /
     * keyboard-wait code we were hitting for plain output shouldn't be
     * reached at all. */
    write_count = 0;
    const char *prefix = "\x05T"; /* Ctrl-E, 'T' -- both need the high bit set */
    for (const char *p = prefix; *p; p++) {
        uint8_t ch = (uint8_t)(*p) | 0x80;
        a = ch;
        sp = 0xFD;
        int s = run_to_halt(0xFDED, 200000, 0x0201, 0);
        printf("Mode-set: JSR $FDED with A=$%02X: %d instructions, writes-so-far=%d\n",
               ch, s, write_count);
    }

    const char *word = "HI";
    for (const char *p = word; *p; p++) {
        uint8_t ch = (uint8_t)(*p) | 0x80;
        a = ch;
        sp = 0xFD;
        int s = run_to_halt(0xFDED, 200000, 0x0201, 0);
        printf("JSR $FDED with A=$%02X ('%c'): %d instructions, writes-so-far=%d\n",
               ch, *p, s, write_count);
    }
    a = 0x8D; /* CR, high-bit RETURN, flush the word */
    sp = 0xFD;
    int s = run_to_halt(0xFDED, 200000, 0x0201, 0);
    printf("JSR $FDED with A=$8D (RETURN): %d instructions, writes-so-far=%d\n",
           s, write_count);

    printf("\nTotal Echo-card bytes written: %d\n", write_count);
    if (write_count > 0) {
        printf("Bytes: ");
        for (int i = 0; i < write_count && i < 64; i++) printf("%02X ", last_writes[i]);
        printf("\n");
    }
    return 0;
}
