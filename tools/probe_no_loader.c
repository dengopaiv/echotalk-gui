/*
 * probe_no_loader.c
 *
 * Tests whether TEXTALKER.RAM (the 543-byte loader) is needed at all
 * once we call directly into Textalker. Loads ONLY TEXTALKER.OBJ,
 * calls its init entry ($D003, "fresh install") once, then feeds text
 * through $D006 per character -- no loader, no DOS, no Apple ROM code.
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
static int write_count = 0;
static uint8_t writes[4096];

static uint8_t fake_tms_status(void) { return 0x20; }

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
        if (write_count < (int)sizeof(writes)) writes[write_count] = value;
        write_count++;
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
    while (pc != halt_addr && steps < max_steps) {
        if (trace)
            printf("  [%3d] PC=$%04X A=%02X X=%02X Y=%02X SP=%02X op=%02X\n",
                   steps, pc, a, x, y, sp, mem[pc]);
        step6502(0);
        steps++;
    }
    return steps;
}

int main(void) {
    FILE *f = fopen("roms/textalker.obj.bin", "rb");
    if (!f) { perror("open obj"); return 1; }
    size_t n = fread(mem + 0xD000, 1, 0x3000, f);
    fclose(f);
    printf("Loaded %zu bytes of TEXTALKER.OBJ at $D000 -- NO loader used\n", n);

    /* $D682/$D5DE end their processing by jumping to a fixed low-memory
     * address (normally populated by TEXTALKER.RAM) that pops the
     * pushed character and returns. Since we're not running the
     * loader, provide that ourselves -- this is just PLA+RTS, not
     * Apple ROM/DOS code. */
    mem[0xBA83] = 0x68; /* PLA */
    mem[0xBA84] = 0x60; /* RTS */

    reset6502();
    sp = 0xFD;

    /* Call $D003 once: $D682 peeks (PLA/PHA) a value on the stack to
     * decide fresh-install (0) vs chain-existing-hook (nonzero). Push
     * $00 for a fresh install (matching a real PHA before the jump). */
    sp = 0xFD;
    int steps = run_to_halt(0xD003, 20000, 0x0200, 1, 0x00, 0);
    printf("Init via $D003 ran %d instructions, stopped at PC=$%04X\n", steps, pc);

    /* $FCD6: TMS5220 presence/signature check. Pokes low-level jump
     * targets ($FCB0 etc, part of the $FD53 byte-write routine's own
     * patch points) that the speech pipeline depends on -- this is
     * what the loader called during boot that we'd otherwise miss. */
    sp = 0xFD;
    steps = run_to_halt(0xFCD6, 20000, 0x0202, 0, 0, 0);
    printf("Init via $FCD6 ran %d instructions, stopped at PC=$%04X\n\n", steps, pc);

    const char *text = "\x05T" "HI" "\x8d";
    for (const char *p = text; *p; p++) {
        uint8_t ch = (uint8_t)(*p) | 0x80;
        write_count = 0;
        sp = 0xFD;
        int s = run_to_halt(0xD006, 500000, 0x0201, 1, ch, 0);
        printf("char $%02X: %d instructions, writes=%d", ch, s, write_count);
        if (write_count) {
            printf("  bytes: ");
            for (int i = 0; i < write_count && i < 256; i++) printf("%02X ", writes[i]);
        }
        printf("\n");
    }
    return 0;
}
