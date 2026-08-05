/*
 * probe_v13.c
 *
 * Same approach as probe_d5de.c / probe_no_loader.c, applied to the
 * older Textalker v1.3 (RAM CARD) version: OBJ loads at $D400 (not
 * $D000), and the real per-character entry is $D400 itself (PHA char,
 * JMP $D400), confirmed by decoding the loader's own patch code
 * (source $9533 -> destination $BA82: PHA / LDA $C08B / JMP $D400).
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
static int echo_activity_log = 0;
static int tms_talking = 0; /* TS (talk status) -- real chip sets this
                                immediately after a SPEAK/SPEAK EXTERNAL
                                command ($50/$60); our previous fully
                                static status stub never set it, so
                                Textalker's "wait for TS" poll spun
                                forever. */

static uint8_t fake_tms_status(void) {
    return tms_talking ? 0xE0 : 0x20; /* TS|BL|BE once talking (FIFO empty, needs data), else just BE */
}



uint8_t read6502(uint16_t address) {
    if (address >= 0xC0A0 && address <= 0xC0AF) {
        uint8_t retval = readlatch_flag ? 0xff : (0x1f | fake_tms_status());
        readlatch_flag = !readlatch_flag;
        if (echo_activity_log)
            printf("    << echo read: $%02X from PC=$%04X\n", retval, pc);
        return retval;
    }
    return mem[address];
}

void write6502(uint16_t address, uint8_t value) {
    if (address >= 0xC0A0 && address <= 0xC0AF) {
        if (write_count < (int)sizeof(writes)) writes[write_count] = value;
        write_count++;
        if ((value & 0x70) == 0x50 || (value & 0x70) == 0x60) tms_talking = 1;
        if (echo_activity_log)
            printf("    >> echo write #%d: $%02X to addr=$%04X at PC=$%04X X=%02X Y=%02X  instr_bytes=%02X %02X %02X\n",
                   write_count, value, address, pc, x, y,
                   mem[(pc - 3) & 0xFFFF], mem[(pc - 2) & 0xFFFF], mem[(pc - 1) & 0xFFFF]);
        return;
    }
    mem[address] = value;
}

static void stub(uint16_t addr, uint8_t opcode) { mem[addr] = opcode; }

static int run_to_halt(uint16_t entry, int max_steps, uint16_t halt_addr,
                        int has_pushed_char, uint8_t pushed_char, int trace) {
    mem[halt_addr] = 0x00;
    uint16_t retaddr = halt_addr - 1;
    mem[0x0100 + sp] = (retaddr >> 8) & 0xFF;
    mem[0x0100 + ((sp - 1) & 0xFF)] = retaddr & 0xFF;
    sp -= 2;
    mem[0xAA59] = sp; /* loader's caller-SP save slot, same as before */
    mem[0xAA5C] = 0;
    mem[0xAA5B] = 0;
    if (has_pushed_char) {
        mem[0x0100 + sp] = pushed_char;
        sp -= 1;
    }
    pc = entry;
    int steps = 0;
    while (pc != halt_addr && steps < max_steps) {
        if (trace && 0)
            printf("  [%5d] PC=$%04X A=%02X X=%02X Y=%02X SP=%02X op=%02X\n",
                   steps, pc, a, x, y, sp, mem[pc]);
        step6502(0);
        steps++;
    }
    return steps;
}

int main(void) {
    FILE *f;
    size_t n;

    f = fopen("roms/textalker_v13.obj.bin", "rb");
    if (!f) { perror("open obj"); return 1; }
    n = fread(mem + 0xD400, 1, 0x2C00, f);
    fclose(f);
    printf("Loaded %zu bytes of v1.3 OBJ at $D400\n", n);

    f = fopen("roms/textalker_v13.ram.bin", "rb");
    if (!f) { perror("open ram"); return 1; }
    n = fread(mem + 0x9300, 1, 0x300, f);
    fclose(f);
    printf("Loaded %zu bytes of v1.3 loader at $9300\n", n);

    stub(0xFDED, 0x60);
    stub(0xFDF0, 0x60);
    stub(0xFC58, 0x60);
    stub(0xFBFD, 0x60);
    stub(0x9EBD, 0x60);
    stub(0xFE95, 0x60);
    stub(0xC300, 0x60);
    stub(0xFE1F, 0x60);
    mem[0xFBB3] = 0xEA;

    reset6502();
    sp = 0xFD;
    mem[0x0024] = 0x00;
    mem[0x0028] = 0x00; mem[0x0029] = 0x04;

    int steps = run_to_halt(0x9300, 2000000, 0x0200, 0, 0, 0);
    printf("Loader ran %d instructions, stopped at PC=$%04X\n\n", steps, pc);

    const char *text = "\x05T" "HI" "\x8d";
    for (const char *p = text; *p; p++) {
        uint8_t ch = (uint8_t)(*p) | 0x80;
        write_count = 0;
        echo_activity_log = (ch == 0x8d);
        sp = 0xFD;
        int s = run_to_halt(0xD400, 5000000, 0x0201, 1, ch, (ch==0x8d));
        printf("char $%02X: %d instructions (%s), writes=%d", ch, s,
               (pc == 0x0201) ? "completed" : "HIT STEP LIMIT", write_count);
        if (write_count) {
            printf("  bytes: ");
            for (int i = 0; i < write_count && i < 256; i++) printf("%02X ", writes[i]);
        }
        printf("\n");
    }
    return 0;
}
