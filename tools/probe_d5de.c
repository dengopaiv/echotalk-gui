/*
 * probe_d5de.c
 *
 * Calls directly into Textalker's real per-character entry point
 * ($D006, which is just JMP $D5DE) instead of going through $FDED and
 * the DOS CSWL vector chain -- no Apple ROM/DOS code needed at all,
 * just the loader + OBJ image.
 *
 * Calling convention (confirmed by disassembling $BA7C, the real
 * trampoline Textalker's own $D682 init routine wires $36/$37 to on
 * real hardware): PHA the character, then JMP/JSR into $D006.
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
static uint8_t writes[4096];

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
        writelatch_flag = 1;
        if (write_count < (int)sizeof(writes)) writes[write_count] = value;
        write_count++;
        return;
    }
    mem[address] = value;
}

static void stub(uint16_t addr, uint8_t opcode) { mem[addr] = opcode; }

static int run_to_halt(uint16_t entry, int max_steps, uint16_t halt_addr, int trace) {
    mem[halt_addr] = 0x00;
    uint16_t retaddr = halt_addr - 1;
    mem[0x0100 + sp]     = (retaddr >> 8) & 0xFF;
    mem[0x0100 + ((sp - 1) & 0xFF)] = retaddr & 0xFF;
    sp -= 2;
    mem[0xAA59] = sp; /* loader's caller-SP save slot */
    mem[0xAA5C] = 0;
    mem[0xAA5B] = 0;
    pc = entry;
    int steps = 0;
    while (pc != halt_addr && steps < max_steps) {
        if (trace && steps < 4000) {
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

    stub(0xFDED, 0x60); /* loader's banner-print loop calls this directly during boot */
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

    int steps = run_to_halt(0x9300, 20000, 0x0200, 0);
    printf("Loader ran %d instructions, stopped at PC=$%04X\n\n", steps, pc);

    const uint16_t TRAMP = 0x0300;
    mem[TRAMP + 0] = 0x48;
    mem[TRAMP + 1] = 0x4C; mem[TRAMP + 2] = 0x06; mem[TRAMP + 3] = 0xD0;

    const char *text = "\x05T" "HI" "\x8d";
    for (const char *p = text; *p; p++) {
        uint8_t ch = (uint8_t)(*p) | 0x80;
        write_count = 0;
        a = ch;
        sp = 0xFD;
        int s = run_to_halt(TRAMP, 500000, 0x0201, 0);
        printf("char $%02X: %d instructions, writes this call=%d",
               ch, s, write_count);
        if (write_count) {
            printf("  bytes: ");
            for (int i = 0; i < write_count && i < 256; i++) printf("%02X ", writes[i]);
        }
        printf("\n");
    }
    return 0;
}
