/*
 * tms5220_core.h
 *
 * Standalone C port of MAME's TMS5220/TMS5200/TMS5220C speech synthesis
 * chip emulation (src/devices/sound/tms5220.cpp + tms5110r.hxx),
 * originally by Frank Palazzolo, Aaron Giles, Jonathan Gevaryahu,
 * Raphael Nabet, Couriersud, and Michael Zapf. BSD-3-Clause license --
 * see THIRD_PARTY_LICENSES.md in the project root.
 *
 * This port strips MAME's device_t/sound_stream/devcb infrastructure
 * (save states, IRQ/ready line callbacks, the "true timing" rsq_w/wsq_w
 * cycle-accurate handshake, and the external VSM/TMS6100 speech-ROM
 * interface, none of which EchoTalk needs) and exposes a plain,
 * synchronous C API instead:
 *
 *   tms5220_state tms;
 *   tms5220_reset(&tms, TMS5220_VARIANT_TMS5220);
 *   tms5220_data_w(&tms, byte);              // write a command/data byte
 *   uint8_t status = tms5220_status_r(&tms); // read status
 *   tms5220_process(&tms, pcm_buffer, n);    // pull n 16-bit PCM samples
 *
 * The chip runs at 80x the output sample rate (so e.g. a 640kHz clock
 * gives 8kHz audio) -- tms5220_process() advances the chip state and
 * fills pcm_buffer with n samples at whatever rate the caller wants
 * (typically 8000 Hz for the classic Echo II sound).
 */
#ifndef TMS5220_CORE_H
#define TMS5220_CORE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FIFO_SIZE 16
#define NOCOMMAND 0xff

/* Variant identifiers -- which real chip variant to emulate. EchoTalk
 * (Echo II card) uses a TMS5220 or TMS5220C; the others are included
 * because they're part of the same coefficient-table family and cost
 * nothing extra to keep. */
#define TMS5220_IS_TMC0281   (1)
#define TMS5220_IS_TMC0281D  (2)
#define TMS5220_IS_CD2801    (3)
#define TMS5220_IS_CD2802    (4)
#define TMS5220_IS_TMS5110A  (5)
#define TMS5220_IS_M58817    (6)
#define TMS5220_IS_5220C     (7)
#define TMS5220_IS_5200      (8)
#define TMS5220_IS_5220      (9)
#define TMS5220_IS_CD2501ECD (10)
#define TMS5220_IS_CD2501E   TMS5220_IS_5200

/* These were file-scope macros in the original that referenced the bare
 * m_variant field; every function in this port takes a `tms` parameter,
 * so they still work unmodified as long as that name is in scope. */
#define TMS5220_HAS_RATE_CONTROL ((tms->m_variant == TMS5220_IS_5220C) || (tms->m_variant == TMS5220_IS_CD2501ECD))
#define TMS5220_IS_52xx ((tms->m_variant == TMS5220_IS_5220C) || (tms->m_variant == TMS5220_IS_5200) || (tms->m_variant == TMS5220_IS_5220) || (tms->m_variant == TMS5220_IS_CD2501ECD))

/* Coefficient table type + the actual tables (chirp table, k-tables,
 * energy/pitch tables, interpolation shift amounts per variant). */
#include "tms5110r.hxx"

typedef struct tms5220_state {
    int variant_requested; /* set by tms5220_reset(), mirrored into m_variant below */

    int m_variant;
    const struct tms5100_coeffs *m_coeff;

    uint8_t m_PDC;
    uint8_t m_CTL_pins;
    uint8_t m_state;

    uint32_t m_address;
    bool  m_next_is_address;
    bool  m_schedule_dummy_read;
    uint8_t  m_addr_bit;
    uint8_t  m_CTL_buffer;

    uint8_t m_read_byte_register;
    bool m_RDB_flag;
    /* Real hardware takes measurable chip time to complete a READ BYTE /
     * READ AND BRANCH command (serial VSM bit reads over ROMCLK), during
     * which a status read should see normal TS/BL/BE status, not a
     * not-yet-ready result. Our port previously completed these commands
     * synchronously within the same write, which let stale RDB data leak
     * into the very next status read -- this models that latency instead.
     * Ticks down once per generated sample (tms5220_process call),
     * matching the same per-sample cadence used elsewhere in this port. */
    int m_pending_command;      /* 0 = none pending; otherwise the masked command (0x10 or 0x30) */
    int m_pending_ticks;        /* samples remaining until the pending command completes */
    uint8_t m_pending_read_byte_register; /* value to apply once the delay elapses */

    uint8_t m_fifo[FIFO_SIZE];
    uint8_t m_fifo_head;
    uint8_t m_fifo_tail;
    uint8_t m_fifo_count;
    uint8_t m_fifo_bits_taken;

    bool m_previous_talk_status;
    bool m_SPEN;
    bool m_DDIS;
    bool m_TALK;
    bool m_TALKD;
    bool m_buffer_low;
    bool m_buffer_empty;
    bool m_irq_pin;
    bool m_ready_pin;

    uint8_t m_command_register;
    bool m_data_latched;

    bool m_OLDE;
    bool m_OLDP;

    uint8_t m_new_frame_energy_idx;
    uint8_t m_new_frame_pitch_idx;
    uint8_t m_new_frame_k_idx[10];

    int16_t m_current_energy;
    int16_t m_current_pitch;
    int16_t m_current_k[10];

    uint16_t m_previous_energy;

    uint8_t m_subcycle;
    uint8_t m_subc_reload;
    uint8_t m_PC;
    uint8_t m_IP;
    bool m_inhibit;
    bool m_uv_zpar;
    bool m_zpar;
    bool m_pitch_zero;
    uint8_t m_c_variant_rate;
    uint16_t m_pitch_count;

    int32_t m_u[11];
    int32_t m_x[10];

    uint16_t m_RNG;
    int16_t m_excitation_data;

    bool m_digital_select;
    bool m_io_ready;
    bool m_true_timing; /* always false in this port -- see file header */

    uint8_t m_rs_ws;
    uint8_t m_read_latch;
    uint8_t m_write_latch;
    /* True-timing support: real Echo II hardware gates every read/write
     * through the chip's actual /READY line (see a2echoii.cpp's
     * read_c0nx/write_c0nx calling rsq_w/wsq_w) -- a write issued before
     * the chip is ready gets silently clobbered, and a status read
     * reflects whatever was last latched, not a freshly computed value.
     * MAME schedules the deferred read/write completion via a timer;
     * we don't have a timer/scheduler, so track it as a countdown in
     * microseconds instead, decremented by the harness's tick function
     * using real elapsed CPU time. 0 = no pending action.
     * action: 0 = none, 1 = pending read (compute m_read_latch), 2 = pending write (apply m_write_latch). */
    int m_pending_ready_action;
    double m_pending_ready_us_remaining;
    /* DDIS itself only becomes true once a SPEAK EXTERNAL command
     * actually resolves (500us later, once the command's own pending
     * write completes) -- checking live m_DDIS to decide a *new*
     * write's delay lags behind by exactly that resolution time, so
     * several FIFO-data bytes sent right after SPEAK EXTERNAL would
     * still see DDIS=false and incorrectly get the long command-mode
     * delay, in turn getting clobbered by the next byte. This sticky
     * flag is set the instant DDIS first becomes true and (unlike
     * m_DDIS) never clears again for the rest of the utterance, so
     * wsq_w can use it instead to avoid that lag. */
    bool m_seen_ddis;

    /* /READY handler, the equivalent of MAME's m_readyq_handler devcb.
     * Called on every change of the ready pin with the ACTIVE-LOW value
     * (0 = ready, 1 = not ready), matching what a2echoii.cpp's
     * tms_readyq_callback receives. The Echo II card uses the ready
     * edge to release its write latch, so a host driving the chip
     * through rsq_w/wsq_w must install this. NULL is fine for the
     * instant-write path, which never gates on /READY. */
    void (*m_readyq_handler)(void *ctx, int state);
    void *m_readyq_ctx;
} tms5220_state;

/* Initialize/reset the chip to the given variant (use TMS5220_IS_5220
 * for a plain TMS5220, or TMS5220_IS_5220C for the C revision -- either
 * is a correct choice for an Echo II card; see a2echoii.cpp). */
void tms5220_reset(tms5220_state *tms, int variant);

/* Host interface -- what EchoTalk's Echo-II-card emulation calls. */
void    tms5220_data_w(tms5220_state *tms, uint8_t data);
uint8_t tms5220_status_r(tms5220_state *tms);

/* True-timing pin protocol (see notes on m_pending_ready_action above).
 * rsq_w/wsq_w mirror MAME's rsq_w()/wsq_w(): call on every falling
 * edge of /RS or /WS (state=0) to begin a pending read/write; the
 * harness must call tms5220_tick_ready_timer() with elapsed real time
 * to actually resolve it. tms5220_data_w_latch() just latches a byte
 * (matching data_w() under true timing -- does NOT process it
 * immediately); the actual command/FIFO processing happens when the
 * pending write resolves. */
void tms5220_rsq_w(tms5220_state *tms, int state);
void tms5220_wsq_w(tms5220_state *tms, int state, uint8_t pending_byte);
void tms5220_tick_ready_timer(tms5220_state *tms, double elapsed_us);
int     tms5220_readyq_r(tms5220_state *tms); /* 0 = ready for a new byte */
int     tms5220_intq_r(tms5220_state *tms);

/* Advance the chip by `size` samples, filling buffer with signed 16-bit
 * PCM. Call this regularly (e.g. once per output audio buffer) whether
 * or not the chip is currently talking -- it no-ops cheaply when idle. */
void tms5220_process(tms5220_state *tms, int16_t *buffer, unsigned int size);

/* Internal functions, exposed for the CLI/debug tooling; not part of
 * the stable API. */
bool tms5220_talk_status(tms5220_state *tms);
void tms5220_data_write(tms5220_state *tms, int data);
void tms5220_update_fifo_status_and_ints(tms5220_state *tms);
int tms5220_read_bits(tms5220_state *tms, int count);
void tms5220_perform_dummy_read(tms5220_state *tms);
uint8_t tms5220_status_read(tms5220_state *tms, bool clear_int);
bool tms5220_ready_read(tms5220_state *tms);
bool tms5220_int_read(tms5220_state *tms);
int16_t tms5220_clip_analog(tms5220_state *tms, int16_t cliptemp);
int32_t tms5220_matrix_multiply(tms5220_state *tms, int32_t a, int32_t b);
int32_t tms5220_lattice_filter(tms5220_state *tms);
void tms5220_process_command(tms5220_state *tms, uint8_t cmd);
void tms5220_parse_frame(tms5220_state *tms);
void tms5220_set_interrupt_state(tms5220_state *tms, int state);
void tms5220_update_ready_state(tms5220_state *tms);
void tms5220_vsm_write(tms5220_state *tms, uint8_t rc, uint8_t m0, uint8_t m1, uint8_t addr);
void tms5220_vsm_write_addr(tms5220_state *tms, uint8_t addr);
uint8_t tms5220_vsm_read(tms5220_state *tms);
void tms5220_vsm_read_and_branch(tms5220_state *tms);

#ifdef __cplusplus
}
#endif

#endif /* TMS5220_CORE_H */
