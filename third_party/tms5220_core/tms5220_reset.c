/* Hand-adapted from MAME's tms5220_device::device_start() and
 * device_reset() (BSD-3-Clause; see THIRD_PARTY_LICENSES.md). Combines
 * both into a single reset function since this port has no separate
 * "construct once, reset many times" device lifecycle -- every field
 * device_start() set once (m_coeff selection) gets redone here too,
 * which is harmless (idempotent) and keeps this the single entry point
 * a caller needs. */
#include <string.h>
#include "tms5220_core.h"

bool tms5220_talk_status(tms5220_state *tms)
{
    return tms->m_SPEN || tms->m_TALKD;
}

void tms5220_reset(tms5220_state *tms, int variant)
{
    /* Host configuration has to survive a chip reset.
     *
     * This function is reached two ways: once from the host at startup,
     * and again every time the RESET command ($Fx) is processed --
     * which Textalker issues between every segment. The memset below
     * therefore wipes anything the host configured, once per utterance.
     * That is what made the frame-rate control appear not to work: it
     * was correct for the first segment and reset to normal at every
     * chunk boundary afterwards, which is audible as speech that speeds
     * up briefly and then falls back.
     *
     * The /READY handler has the same problem, and worse consequences:
     * losing it means the Echo II card never learns the chip is ready
     * and so never releases its write latch, which is a strong
     * candidate for the write clobbering seen under true timing.
     *
     * Save and restore rather than reordering the memset, so that any
     * field added to the struct later is still zeroed by default and
     * only deliberately-preserved ones survive. */
    uint8_t saved_rate = tms->m_configured_rate;
    void (*saved_readyq)(void *, int) = tms->m_readyq_handler;
    void *saved_readyq_ctx = tms->m_readyq_ctx;

    memset(tms, 0, sizeof(*tms));

    tms->m_configured_rate = saved_rate;
    tms->m_readyq_handler = saved_readyq;
    tms->m_readyq_ctx = saved_readyq_ctx;

    tms->m_variant = variant;

    switch (tms->m_variant)
    {
    case TMS5220_IS_TMC0281:
        tms->m_coeff = &T0280B_0281A_coeff;
        break;
    case TMS5220_IS_TMC0281D:
        tms->m_coeff = &T0280D_0281D_coeff;
        break;
    case TMS5220_IS_CD2801:
        tms->m_coeff = &T0280F_2801A_coeff;
        break;
    case TMS5220_IS_M58817:
        tms->m_coeff = &M58817_coeff;
        break;
    case TMS5220_IS_CD2802:
        tms->m_coeff = &T0280F_2802_coeff;
        break;
    case TMS5220_IS_TMS5110A:
        tms->m_coeff = &tms5110a_coeff;
        break;
    case TMS5220_IS_5200:
    case TMS5220_IS_CD2501ECD:
        tms->m_coeff = &T0285_2501E_coeff;
        break;
    case TMS5220_IS_5220C:
    case TMS5220_IS_5220:
        tms->m_coeff = &tms5220_coeff;
        break;
    default:
        tms->m_coeff = &tms5220_coeff; /* fall back rather than abort */
        break;
    }

    tms->m_io_ready = true;
    tms->m_true_timing = false;
    tms->m_rs_ws = 0x03;
    tms->m_write_latch = 0;

    /* ---- device_reset() below ---- */
    tms->m_digital_select = 0; /* FORCE_DIGITAL = 0: analog output, matches Echo II */

    memset(tms->m_fifo, 0, sizeof(tms->m_fifo));
    tms->m_fifo_head = tms->m_fifo_tail = tms->m_fifo_count = tms->m_fifo_bits_taken = 0;

    tms->m_SPEN = tms->m_DDIS = tms->m_TALK = tms->m_TALKD = tms->m_previous_talk_status =
        tms->m_irq_pin = tms->m_ready_pin = false;
    tms5220_set_interrupt_state(tms, 0);
    tms5220_update_ready_state(tms);
    tms->m_buffer_empty = tms->m_buffer_low = true;

    tms->m_command_register = NOCOMMAND;
    tms->m_data_latched = false;
    tms->m_RDB_flag = false;

    tms->m_new_frame_energy_idx = tms->m_current_energy = tms->m_previous_energy = 0;
    tms->m_new_frame_pitch_idx = tms->m_current_pitch = 0;
    tms->m_zpar = tms->m_uv_zpar = false;
    memset(tms->m_new_frame_k_idx, 0, sizeof(tms->m_new_frame_k_idx));
    memset(tms->m_current_k, 0, sizeof(tms->m_current_k));

    tms->m_inhibit = true;
    tms->m_subcycle = tms->m_pitch_count = tms->m_PC = 0;
    /* MAME clears m_c_variant_rate here, because on real silicon the
     * only way to set it is the SET RATE command, which the host would
     * reissue. EchoTalk uses it as a speech-rate control set from
     * outside, and Textalker sends a RESET between every segment -- so
     * clearing it would wipe the setting after the first utterance,
     * which is exactly what happened when this was first tried.
     * m_configured_rate is the host's value and survives reset; it is
     * zero unless something sets it, so default behaviour is unchanged
     * and identical to MAME. */
    tms->m_c_variant_rate = tms->m_configured_rate;
    tms->m_subc_reload = 1; /* FORCE_SUBC_RELOAD = 1: normal (not SPKSLOW) speech rate */
    tms->m_OLDE = tms->m_OLDP = true;
    {
        static const uint8_t reload_table[4] = { 0, 2, 4, 6 };
        tms->m_IP = reload_table[tms->m_c_variant_rate & 0x3];
    }
    tms->m_RNG = 0x1FFF;
    memset(tms->m_u, 0, sizeof(tms->m_u));
    memset(tms->m_x, 0, sizeof(tms->m_x));

    tms5220_perform_dummy_read(tms);

    tms->m_PDC = 0;
    tms->m_CTL_pins = 0;
    tms->m_state = 0;
    tms->m_address = 0;
    tms->m_next_is_address = false;
    tms->m_addr_bit = 0;
    tms->m_CTL_buffer = 0;
}
