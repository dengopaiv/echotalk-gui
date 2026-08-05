/*
 * probe_writebyte.c
 *
 * Proof-of-concept harness: loads TEXTALKER.OBJ at $D000, jams the 6502
 * PC directly into the byte-output routine we identified by disassembly
 * at $FD53 ("wait for chip ready, then latch a byte to the Echo II card"),
 * and checks whether it behaves the way MAME's a2echoii.cpp says real
 * Echo II hardware behaves (alternating status/pullup reads, latch writes
 * pulse WSQ on the TMS5220).
 *
 * This does NOT yet involve the TMS5220 DSP core -- it's purely testing
 * the 6502-side bus protocol in isolation before we wire the real chip in.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern uint16_t pc;
extern uint8_t sp, a, x, y, status;
extern void reset6502(void);
extern void step6502(int printRegs);

static uint8_t mem[0x10000];

/* Echo II card emulation, mirroring a2bus_echoii_device::read_c0nx /
 * write_c0nx from MAME (src/devices/bus/a2bus/a2echoii.cpp), minus the
 * actual TMS5220 (stubbed as "always idle, buffer empty" for this probe). */
static int readlatch_flag = 1;   /* true after reset, per hardware */
static int writelatch_flag = 1;  /* true = ready for a new byte */
static uint8_t writelatch_data = 0xff;
static int got_write = 0;
static uint8_t last_write_value = 0;
static int write_count = 0;

static uint8_t fake_tms_status(void) {
    /* Stub: chip idle, buffer empty, no talk in progress -> BE=1, others 0 */
    return 0x20; /* bit5 = BE */
}

uint8_t read6502(uint16_t address) {
    if (address >= 0xC0A0 && address <= 0xC0AF) {
        uint8_t retval;
        if (!readlatch_flag) {
            retval = 0x1f | fake_tms_status();
        } else {
            retval = 0xff; /* pullups, "dummy" half of the cycle */
        }
        readlatch_flag = !readlatch_flag;
        return retval;
    }
    return mem[address];
}

void write6502(uint16_t address, uint8_t value) {
    if (address >= 0xC0A0 && address <= 0xC0AF) {
        writelatch_data = value;
        writelatch_flag = 0; /* WSQ asserted -- chip "consumes" it */
        got_write = 1;
        last_write_value = value;
        write_count++;
        /* Simulate the chip immediately accepting the byte and the
         * ready callback firing (real hardware: TMS5220 pulses /READY
         * a short time later). For this probe we just set it back to
         * ready on the *next* status read, which is enough to unstick
         * the polling loop at $FD54. */
        writelatch_flag = 1;
        return;
    }
    mem[address] = value;
}

int main(void) {
    FILE *f = fopen("roms/textalker.obj.bin", "rb");
    if (!f) { perror("open obj"); return 1; }
    size_t n = fread(mem + 0xD000, 1, 0x3000, f);
    fclose(f);
    printf("Loaded %zu bytes of TEXTALKER.OBJ at $D000 (expected 0x2FFA = 12282)\n", n);

    reset6502();

    /* Set up a fake return address: push address of a RTS-trap byte.
     * We stop stepping when PC reaches this sentinel instead of really
     * executing an RTS into nowhere. */
    const uint16_t HALT_ADDR = 0x0200;
    mem[HALT_ADDR] = 0x00; /* BRK, used as our halt sentinel opcode */

    /* JSR-equivalent: push (HALT_ADDR - 1) so RTS lands exactly on HALT_ADDR */
    uint16_t retaddr = HALT_ADDR - 1;
    mem[0x0100 + sp]     = (retaddr >> 8) & 0xFF;
    mem[0x0100 + (sp-1)] = retaddr & 0xFF;
    sp -= 2;

    a = 0x41; /* test byte: 'A' with Apple II high bit NOT set, doesn't matter for this probe */
    pc = 0xFD53;

    int steps = 0;
    const int MAX_STEPS = 2000;
    while (pc != HALT_ADDR && steps < MAX_STEPS) {
        step6502(0);
        steps++;
    }

    printf("Stopped after %d 6502 instructions at PC=$%04X (halt target was $%04X)\n",
           steps, pc, HALT_ADDR);
    printf("Echo-card write observed: %s\n", got_write ? "YES" : "no");
    if (got_write) {
        printf("  byte latched to $C0A8-$C0AF: $%02X ('%c'), write_count=%d\n",
               last_write_value,
               (last_write_value >= 0x20 && last_write_value < 0x7f) ? last_write_value : '.',
               write_count);
    }
    if (steps >= MAX_STEPS) {
        printf("WARNING: hit step limit without returning -- polling loop or "
               "our stub status logic likely needs adjustment.\n");
        return 2;
    }
    return got_write ? 0 : 3;
}
