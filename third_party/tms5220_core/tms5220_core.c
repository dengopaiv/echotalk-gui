/* Auto-extracted + hand-adapted from MAME tms5220.cpp (BSD-3-Clause).
 * Original: Frank Palazzolo, Aaron Giles, Jonathan Gevaryahu, Raphael Nabet,
 * Couriersud, Michael Zapf. See THIRD_PARTY_LICENSES.md. */
#include <string.h>
#include "tms5220_core.h"

static const uint8_t reload_table[4] = { 0, 2, 4, 6 };
#define INTERP_SHIFT >> tms->m_coeff->interp_coeff[tms->m_IP]

/* Sets TALK at the same moment SPEN goes active, when the FIFO passes
 * the buffer-low threshold, rather than waiting for the next RESETL4 to
 * set it. MAME defines this (tms5220.cpp line 355) and the two
 * `#ifdef FAST_START_HACK` sites below came across with the port, but
 * the define itself did not -- so both blocks compiled to nothing and
 * every restart cost an extra frame.
 *
 * That frame is the whole of the "sluggish compressed speech" bug:
 * TALK waits a RESETL4, TALKD follows TALK a further RESETL4 later, and
 * speech resumes 25ms late. MAME's own trace shows one idle frame per
 * restart against our two. See notes/pacing_fast_start_hack.md. */
#define FAST_START_HACK 1

/* Number of generated samples a pending READ BYTE / READ AND BRANCH
 * command takes to complete before its result becomes visible to a
 * status read.
 *
 * The minimum needed to fix card detection (Textalker's FCD6 routine,
 * which does a status read only ~500 CPU cycles / ~4 samples after
 * issuing a READ BYTE) is small -- but short common words like "HI"
 * turn out to run their own multi-attempt VSM dictionary lookup
 * (issuing several READ BYTE / LOAD ADDRESS / READ AND BRANCH commands
 * in a row before falling back to rule-based synthesis), and that
 * lookup's own timing assumptions are sensitive to this same delay in
 * a non-monotonic way -- short delays (5-100 samples) still resolve
 * "HI" audibly wrong (truncated, clipped) even though card detection
 * itself already succeeds. Swept empirically against known-good
 * output (matching both a real hardware capture and this port's
 * own earlier -- accidentally correct -- results): the delay needs to
 * be large enough to land past several such internal thresholds. 600
 * sits in a wide, stable plateau (confirmed stable from ~500-2000)
 * that reproduces the correct sample count and, critically, the
 * correct amplitude profile (no clipping, matches the reference
 * capture's dynamic range) for every case tested so far. This is not
 * derived from a datasheet timing figure -- if real per-command VSM
 * timing specs turn up, prefer those over this empirical value. */
#define TMS5220_READ_COMMAND_DELAY_SAMPLES 600

void tms5220_vsm_write(tms5220_state *tms, uint8_t rc, uint8_t m0, uint8_t m1, uint8_t addr)
{
			(void)rc; (void)m0; (void)m1; (void)addr; /* no external VSM in this port */
}

void tms5220_vsm_write_addr(tms5220_state *tms, uint8_t addr)
{
	tms5220_vsm_write(tms, 1, 0, 1, addr); // romclk 1, m0 0, m1 1, addr bus nybble = xxxx
	tms5220_vsm_write(tms, 0, 0, 1, addr); // romclk 0, m0 0, m1 1, addr bus nybble = xxxx
	tms5220_vsm_write(tms, 1, 0, 0, addr); // romclk 1, m0 0, m1 0, addr bus nybble = xxxx
	tms5220_vsm_write(tms, 0, 0, 0, addr); // romclk 0, m0 0, m1 0, addr bus nybble = xxxx
}

uint8_t tms5220_vsm_read(tms5220_state *tms)
{
	tms5220_vsm_write(tms, 1, 1, 0, 0); // romclk 1, m0 1, m1 0, addr bus nybble = 0/open bus
	tms5220_vsm_write(tms, 0, 1, 0, 0); // romclk 0, m0 1, m1 0, addr bus nybble = 0/open bus
	tms5220_vsm_write(tms, 1, 0, 0, 0); // romclk 1, m0 0, m1 0, addr bus nybble = 0/open bus
	tms5220_vsm_write(tms, 0, 0, 0, 0); // romclk 0, m0 0, m1 0, addr bus nybble = 0/open bus

	uint8_t val = 0;

	val = 0;
		

	return val;
}

void tms5220_vsm_read_and_branch(tms5220_state *tms)
{
	tms5220_vsm_write(tms, 0, 1, 1, 0); // see tms5110.cpp
	tms5220_vsm_write(tms, 1, 1, 1, 0); // romclk 1, m0 1, m1 1, addr bus nybble = 0/open bus
	tms5220_vsm_write(tms, 0, 1, 1, 0); // romclk 0, m0 1, m1 1, addr bus nybble = 0/open bus
	tms5220_vsm_write(tms, 0, 0, 0, 0); // see tms5110.cpp
	tms5220_vsm_write(tms, 1, 0, 0, 0); // romclk 1, m0 0, m1 0, addr bus nybble = 0/open bus
	tms5220_vsm_write(tms, 0, 0, 0, 0); // romclk 0, m0 0, m1 0, addr bus nybble = 0/open bus
}

void tms5220_data_write(tms5220_state *tms, int data)
{
	bool old_buffer_low = tms->m_buffer_low;
	

	if (tms->m_DDIS) // If we're in speak external mode
	{
		// add this byte to the FIFO
		if (tms->m_fifo_count < FIFO_SIZE)
		{
			tms->m_fifo[tms->m_fifo_tail] = data;
			tms->m_fifo_tail = (tms->m_fifo_tail + 1) % FIFO_SIZE;
			tms->m_fifo_count++;
			
			tms5220_update_fifo_status_and_ints(tms);

			// if we just unset buffer low with that last write, and SPEN *was* zero (see circuit 251, sheet 12)
			if ((!tms->m_SPEN) && (old_buffer_low && (!tms->m_buffer_low))) // MUST HAVE EDGE DETECT
			{
				
				// ...then we now have enough bytes to start talking; set zpar and clear out the new frame parameters (it will become old frame just before the first call to tms5220_parse_frame(tms) )
				tms->m_zpar = true;
				tms->m_uv_zpar = true; // zero k4-k10 as well
				tms->m_OLDE = true; // 'silence/zpar' frames are zero energy
				tms->m_OLDP = true; // 'silence/zpar' frames are zero pitch
#ifdef TMS5220_PERFECT_INTERPOLATION_HACK
				tms->m_old_zpar = true; // zero all the old parameters
				tms->m_old_uv_zpar = true; // zero old k4-k10 as well
#endif
				tms->m_SPEN = true;
#ifdef FAST_START_HACK
				tms->m_TALK = true;
#endif
				tms->m_new_frame_energy_idx = 0;
				tms->m_new_frame_pitch_idx = 0;
				for (int i = 0; i < 4; i++)
					tms->m_new_frame_k_idx[i] = 0;
				for (int i = 4; i < 7; i++)
					tms->m_new_frame_k_idx[i] = 0xF;
				for (int i = 7; i < tms->m_coeff->num_k; i++)
					tms->m_new_frame_k_idx[i] = 0x7;

			}
		}
		else
		{
			
			// at this point, /READY should remain HIGH/inactive until the FIFO has at least one byte open in it.
		}
		tms->m_data_latched = false;
	}
	else //(! tms->m_DDIS)
		// R Nabet : we parse commands at once.  It is necessary for such commands as read.
		tms5220_process_command(tms, data);

	if (!tms->m_data_latched)
		tms->m_io_ready = true;
}

void tms5220_update_fifo_status_and_ints(tms5220_state *tms)
{
	/* update 52xx FIFO flags and set ints if needed */
	if (!TMS5220_IS_52xx) return; // bail out if not a 52xx chip
	tms5220_update_ready_state(tms);

	/* BL is set if neither byte 9 nor 8 of the FIFO are in use; this
	translates to having fifo_count (which ranges from 0 bytes in use to 16
	bytes used) being less than or equal to 8. Victory/Victorba depends on this. */
	if (tms->m_fifo_count <= 8)
	{
		// generate an interrupt if necessary; if /BL was inactive and is now active, set int.
		if (!tms->m_buffer_low)
		{
			tms->m_buffer_low = true;
			tms5220_set_interrupt_state(tms, 1);
		}
	}
	else
		tms->m_buffer_low = false;

	/* BE is set if neither byte 15 nor 14 of the FIFO are in use; this
	translates to having fifo_count equal to exactly 0
	*/
	if (tms->m_fifo_count == 0)
	{
		// generate an interrupt if necessary; if /BE was inactive and is now active, set int.
		if (!tms->m_buffer_empty)
		{
			tms->m_buffer_empty = true;
			tms5220_set_interrupt_state(tms, 1);
		}
		if (tms->m_DDIS)
			tms->m_TALK = tms->m_SPEN = false; // /BE being active clears the TALK status via TCON, which in turn clears SPEN, but ONLY if tms->m_DDIS is set! See patent page 16, gate 232b
	}
	else
		tms->m_buffer_empty = false;

	// generate an interrupt if /TS was active, and is now inactive.
	// also, in this case, regardless if DDIS was set, unset it.
	if (tms->m_previous_talk_status && !tms5220_talk_status(tms))
	{
		
		tms5220_set_interrupt_state(tms, 1);
		tms->m_DDIS = false;

		tms->m_previous_talk_status = false;

		// Is there a command stuck in the command register due to speech output?
		// Then resume it.
		if (tms->m_command_register != NOCOMMAND)
		{
			
			tms5220_process_command(tms, tms->m_command_register);

			// Is there another data transfer pending? Resume it as well.
			if (tms->m_data_latched)
			{
				
				tms->m_io_ready = true; /* true-timing path unused in this port */
			}
			else
				tms->m_io_ready = true;

			tms5220_update_ready_state(tms);
		}
	}
	tms->m_previous_talk_status = tms5220_talk_status(tms);

	// update continuously when RSQ is held active low
	if (tms->m_rs_ws == 0x01 && !tms->m_RDB_flag)
		tms->m_read_latch = tms5220_status_read(tms, false);
}

int tms5220_read_bits(tms5220_state *tms, int count)
{
	int val = 0;

	if (tms->m_DDIS)
	{
		// extract from FIFO
		while (count--)
		{
			val = (val << 1) | ((tms->m_fifo[tms->m_fifo_head] >> tms->m_fifo_bits_taken) & 1);
			tms->m_fifo_bits_taken++;
			if (tms->m_fifo_bits_taken >= 8)
			{
				tms->m_fifo_count--;
				tms->m_fifo[tms->m_fifo_head] = 0; // zero the newly depleted FIFO head byte
				tms->m_fifo_head = (tms->m_fifo_head + 1) % FIFO_SIZE;
				tms->m_fifo_bits_taken = 0;
				tms5220_update_fifo_status_and_ints(tms);
			}
		}
	}
	else
	{
		if (0)
		{
			while (count--)
			{
				val = (val << 1) | tms5220_vsm_read(tms);
				
			}
		}
		else
		{
			// assume the input floats high if nothing is connected, so a
			// spurious speak vsm command will eventually return a 0xF (STOP)
			// frame which will halt speech
			val = (1<<count)-1;
		}
	}
	return val;
}

void tms5220_perform_dummy_read(tms5220_state *tms)
{
	tms->m_schedule_dummy_read = false;
	if (0)
	{
		
		tms5220_vsm_write_addr(tms, 0);
		tms5220_vsm_read(tms);
	}
}

uint8_t tms5220_status_read(tms5220_state *tms, bool clear_int)
{
	if (tms->m_RDB_flag)
	{   /* if last command was read, return data register */
		tms->m_RDB_flag = false;
		return(tms->m_read_byte_register);
	}
	else
	{   /* read status */
		/* clear the interrupt pin on status read */
		if (clear_int)
			tms5220_set_interrupt_state(tms, 0);
		
		return (tms5220_talk_status(tms) << 7) | (tms->m_buffer_low << 6) | (tms->m_buffer_empty << 5);// | (tms->m_write_latch & 0x1f); // low 5 bits are open bus, so use the tms->m_write_latch value.
	}
}

bool tms5220_ready_read(tms5220_state *tms)
{
	
	/* if tms->m_true_timing is NOT set (we're in 'hacky instant write mode'), the
	   tms->m_timer_io_ready timer doesn't run and will never de-assert tms->m_io_ready
	   if the FIFO is full, so we need to explicitly check for FIFO full here
	   and return the proper value.

	   SEVERE CAVEAT: This makes the assumption that the ready_read was after
	   wsq was 'virtually asserted', so if the FIFO has no room in it ready
	   will always return inactive, even if no write happened! i.e., after a
	   read command when the FIFO was exactly filled, but no write attempted
	   to overfill it. This behavior is inaccurate to hardware and may cause
	   issues! You have been warned!
	*/
	if (!tms->m_true_timing)
		return ((tms->m_fifo_count < FIFO_SIZE)||(!tms->m_DDIS)) && tms->m_io_ready;
	else
		return tms->m_io_ready;
}

bool tms5220_int_read(tms5220_state *tms)
{
	

	return tms->m_irq_pin;
}

int16_t tms5220_clip_analog(tms5220_state *tms, int16_t cliptemp)
{
	/* clipping, just like the patent shows:
	 * the top 10 bits of this result are visible on the digital output IO pin.
	 * next, if the top 3 bits of the 14 bit result are all the same, the
	 * lowest of those 3 bits plus the next 7 bits are the signed analog
	 * output, otherwise the low bits are all forced to match the inverse of
	 * the topmost bit, i.e.:
	 * 1x xxxx xxxx xxxx -> 0b10000000
	 * 11 1bcd efgh xxxx -> 0b1bcdefgh
	 * 00 0bcd efgh xxxx -> 0b0bcdefgh
	 * 0x xxxx xxxx xxxx -> 0b01111111
	 */
	if ((cliptemp > 2047) || (cliptemp < -2048))
	{
		if (cliptemp > 2047) cliptemp = 2047;
		else if (cliptemp < -2048) cliptemp = -2048;
	}
	/* at this point the analog output is tapped */
#ifdef ALLOW_4_LSB
	// input:  ssss snnn nnnn nnnn
	// N taps:       ^^^ ^         = 0x0780
	// output: snnn nnnn nnnn NNNN
	return (cliptemp << 4)|((cliptemp&0x780)>>7); // upshift and range adjust
#else
	cliptemp &= ~0xF;
	// input:  ssss snnn nnnn 0000
	// N taps:       ^^^ ^^^^      = 0x07F0
	// P taps:       ^             = 0x0400
	// output: snnn nnnn NNNN NNNP
	return (cliptemp << 4)|((cliptemp&0x7F0)>>3)|((cliptemp&0x400)>>10); // upshift and range adjust
#endif
}

int32_t tms5220_matrix_multiply(tms5220_state *tms, int32_t a, int32_t b)
{
	int32_t result;
	while (a>511) { a-=1024; }
	while (a<-512) { a+=1024; }
	while (b>16383) { b-=32768; }
	while (b<-16384) { b+=32768; }
	result = ((a*b)>>9); /** TODO: this isn't technically right to the chip, which truncates the lowest result bit, but it causes glitches otherwise. **/
	return result;
}

int32_t tms5220_lattice_filter(tms5220_state *tms)
{
	// Lattice filter here
	// Aug/05/07: redone as unrolled loop, for clarity - LN
	/* Originally Copied verbatim from table I in US patent 4,209,804, now
	  updated to be in same order as the actual chip does it, not that it matters.

	  notation equivalencies from table:
	  Yn(i) == tms->m_u[n-1]
	  Kn = tms->m_current_k[n-1]
	  bn = tms->m_x[n-1]
	 */
	/*
	    int ep = tms5220_matrix_multiply(tms, tms->m_previous_energy, (tms->m_excitation_data<<6));  //Y(11)
	     tms->m_u[10] = ep;
	    for (int i = 0; i < 10; i++)
	    {
	        int ii = 10-i; // for m = 10, this would be 11 - i, and since i is from 1 to 10, then ii ranges from 10 to 1
	        // int jj = ii+1; // this variable, even on the fortran version, is
	        // never used. It probably was intended to be used on the two lines
	        // below the next one to save some redundant additions on each.
	        ep = ep - (((tms->m_current_k[ii-1] * tms->m_x[ii-1])>>9)|1); // subtract reflection from lower stage 'top of lattice'
	         tms->m_u[ii-1] = ep;
	        tms->m_x[ii] = tms->m_x[ii-1] + (((tms->m_current_k[ii-1] * ep)>>9)|1); // add reflection from upper stage 'bottom of lattice'
	    }
	tms->m_x[0] = ep; // feed the last section of the top of the lattice directly to the bottom of the lattice
	*/
		tms->m_u[10] = tms5220_matrix_multiply(tms, tms->m_previous_energy, (tms->m_excitation_data<<6));  //Y(11)
		tms->m_u[9] = tms->m_u[10] - tms5220_matrix_multiply(tms, tms->m_current_k[9], tms->m_x[9]);
		tms->m_u[8] = tms->m_u[9] - tms5220_matrix_multiply(tms, tms->m_current_k[8], tms->m_x[8]);
		tms->m_u[7] = tms->m_u[8] - tms5220_matrix_multiply(tms, tms->m_current_k[7], tms->m_x[7]);
		tms->m_u[6] = tms->m_u[7] - tms5220_matrix_multiply(tms, tms->m_current_k[6], tms->m_x[6]);
		tms->m_u[5] = tms->m_u[6] - tms5220_matrix_multiply(tms, tms->m_current_k[5], tms->m_x[5]);
		tms->m_u[4] = tms->m_u[5] - tms5220_matrix_multiply(tms, tms->m_current_k[4], tms->m_x[4]);
		tms->m_u[3] = tms->m_u[4] - tms5220_matrix_multiply(tms, tms->m_current_k[3], tms->m_x[3]);
		tms->m_u[2] = tms->m_u[3] - tms5220_matrix_multiply(tms, tms->m_current_k[2], tms->m_x[2]);
		tms->m_u[1] = tms->m_u[2] - tms5220_matrix_multiply(tms, tms->m_current_k[1], tms->m_x[1]);
		tms->m_u[0] = tms->m_u[1] - tms5220_matrix_multiply(tms, tms->m_current_k[0], tms->m_x[0]);
		int32_t err = tms->m_x[9] + tms5220_matrix_multiply(tms, tms->m_current_k[9], tms->m_u[9]); //x_10, real chip doesn't use or calculate this
		tms->m_x[9] = tms->m_x[8] + tms5220_matrix_multiply(tms, tms->m_current_k[8], tms->m_u[8]);
		tms->m_x[8] = tms->m_x[7] + tms5220_matrix_multiply(tms, tms->m_current_k[7], tms->m_u[7]);
		tms->m_x[7] = tms->m_x[6] + tms5220_matrix_multiply(tms, tms->m_current_k[6], tms->m_u[6]);
		tms->m_x[6] = tms->m_x[5] + tms5220_matrix_multiply(tms, tms->m_current_k[5], tms->m_u[5]);
		tms->m_x[5] = tms->m_x[4] + tms5220_matrix_multiply(tms, tms->m_current_k[4], tms->m_u[4]);
		tms->m_x[4] = tms->m_x[3] + tms5220_matrix_multiply(tms, tms->m_current_k[3], tms->m_u[3]);
		tms->m_x[3] = tms->m_x[2] + tms5220_matrix_multiply(tms, tms->m_current_k[2], tms->m_u[2]);
		tms->m_x[2] = tms->m_x[1] + tms5220_matrix_multiply(tms, tms->m_current_k[1], tms->m_u[1]);
		tms->m_x[1] = tms->m_x[0] + tms5220_matrix_multiply(tms, tms->m_current_k[0], tms->m_u[0]);
		tms->m_x[0] = tms->m_u[0];
		tms->m_previous_energy = tms->m_current_energy;

		
		for (int i = 9; i >= 0; i--)
		{
			
		}
		
		
		for (int i = 9; i >= 0; i--)
		{
			
		}
		

		return tms->m_u[0];
}

/* --- continuous speech rate -----------------------------------------
 *
 * The two functions below are the loop body of tms5220_process(), split
 * where the chip's own timing splits: parameter stepping and counter
 * advance are the PARAMETER state machine, and everything between them
 * (excitation, LFSR, lattice filter, pitch counter) is the AUDIO path
 * that must keep running once per output sample.
 *
 * Splitting them is what allows the machine to run at a rate other than
 * one cycle per sample, which changes speech rate while leaving pitch
 * alone. They are called in the original order at rate 1.0, so nothing
 * about the default path changes -- verified byte-exact against every
 * reference file.
 *
 * MAME has no equivalent; do not expect to find these when diffing
 * against third_party/tms5220/. The code inside them is unchanged.
 */
static void tms5220_step_parameters(tms5220_state *tms)
{
#ifdef TMS5220_PERFECT_INTERPOLATION_HACK
	int i;
#endif
	if ((tms->m_IP == 0) && (tms->m_PC == 12) && (tms->m_subcycle == 1))
	{
		// HACK for regression testing, be sure to comment out before release!
		//tms->m_RNG = 0x1234;
		// end HACK

		/* appropriately override the interp count if needed; this will be incremented after the frame parse! */
		tms->m_IP = reload_table[tms->m_c_variant_rate&0x3];

#ifdef TMS5220_PERFECT_INTERPOLATION_HACK
		/* remember previous frame energy, pitch, and coefficients */
		tms->m_old_frame_energy_idx = tms->m_new_frame_energy_idx;
		tms->m_old_frame_pitch_idx = tms->m_new_frame_pitch_idx;
		for (i = 0; i < tms->m_coeff->num_k; i++)
			tms->m_old_frame_k_idx[i] = tms->m_new_frame_k_idx[i];
#endif

		/* Parse a new frame into the new_target_energy, new_target_pitch and new_target_k[] */
		tms5220_parse_frame(tms);

		/* if the new frame is a stop frame, unset both TALK and SPEN (via TCON). TALKD remains active while the energy is ramping to 0. */
		if ((tms->m_new_frame_energy_idx == 0x0F))
		{
			tms->m_TALK = tms->m_SPEN = false;
			tms5220_update_fifo_status_and_ints(tms); // probably not necessary...
		}

		/* in all cases where interpolation would be inhibited, set the inhibit flag; otherwise clear it.
		 * Interpolation inhibit cases:
		 * Old frame was voiced, new is unvoiced
		 * Old frame was silence/zero energy, new has non-zero energy
		 * Old frame was unvoiced, new is voiced
		 * Old frame was unvoiced, new frame is silence/zero energy (non-existent on tms51xx rev D and F (present and working on tms52xx, present but buggy on tms51xx rev A and B))
		 */
		if ( (!tms->m_OLDP && (tms->m_new_frame_pitch_idx == 0))
			|| (tms->m_OLDP && !(tms->m_new_frame_pitch_idx == 0))
			|| (tms->m_OLDE && !(tms->m_new_frame_energy_idx == 0))
			//|| (tms->m_inhibit && tms->m_OLDP && (tms->m_new_frame_energy_idx == 0)) ) //TMS51xx INTERP BUG1
			|| (tms->m_OLDP && (tms->m_new_frame_energy_idx == 0)) )
			tms->m_inhibit = true;
		else // normal frame, normal interpolation
			tms->m_inhibit = false;

		/* Debug info for current parsed frame */
		
		
		
		
	}
	else // Not a new frame, just interpolate the existing frame.
	{
		bool inhibit_state = (tms->m_inhibit && (tms->m_IP != 0)); // disable inhibit when reaching the last interp period, but don't overwrite the tms->m_inhibit value
#ifdef TMS5220_PERFECT_INTERPOLATION_HACK
		int samples_per_frame = tms->m_subc_reload?175:266; // either (13 A cycles + 12 B cycles) * 7 interps for normal SPEAK/SPKEXT, or (13*2 A cycles + 12 B cycles) * 7 interps for SPKSLOW
		//int samples_per_frame = tms->m_subc_reload?200:304; // either (13 A cycles + 12 B cycles) * 8 interps for normal SPEAK/SPKEXT, or (13*2 A cycles + 12 B cycles) * 8 interps for SPKSLOW
		int current_sample = (tms->m_subcycle - tms->m_subc_reload)+(tms->m_PC*(3-tms->m_subc_reload))+((tms->m_subc_reload?25:38)*((tms->m_IP-1)&7));
		//
		// reset the current energy, pitch, etc to what it was at frame start
		tms->m_current_energy = (tms->m_coeff->energytable[tms->m_old_frame_energy_idx] * (1-tms->m_old_zpar));
		tms->m_current_pitch = (tms->m_coeff->pitchtable[tms->m_old_frame_pitch_idx] * (1-tms->m_old_zpar));
		for (i = 0; i < tms->m_coeff->num_k; i++)
			tms->m_current_k[i] = (tms->m_coeff->ktable[i][tms->m_old_frame_k_idx[i]] * (1-((i<4)?tms->m_old_zpar:tms->m_old_uv_zpar)));
		// now adjust each value to be exactly correct for each of the samples per frame
		if (tms->m_IP != 0) // if we're still interpolating...
		{
			tms->m_current_energy = (tms->m_current_energy + (((tms->m_coeff->energytable[tms->m_new_frame_energy_idx] - tms->m_current_energy)*(1-inhibit_state))*current_sample)/samples_per_frame)*(1-tms->m_zpar);
			tms->m_current_pitch = (tms->m_current_pitch + (((tms->m_coeff->pitchtable[tms->m_new_frame_pitch_idx] - tms->m_current_pitch)*(1-inhibit_state))*current_sample)/samples_per_frame)*(1-tms->m_zpar);
			for (i = 0; i < tms->m_coeff->num_k; i++)
				tms->m_current_k[i] = (tms->m_current_k[i] + (((tms->m_coeff->ktable[i][tms->m_new_frame_k_idx[i]] - tms->m_current_k[i])*(1-inhibit_state))*current_sample)/samples_per_frame)*(1-((i<4)?tms->m_zpar:tms->m_uv_zpar));
		}
		else // we're done, play this frame for 1/8 frame.
		{
			if (tms->m_subcycle == 2) tms->m_pitch_zero = false; // this reset happens around the second subcycle during IP=0
			tms->m_current_energy = (tms->m_coeff->energytable[tms->m_new_frame_energy_idx] * (1-tms->m_zpar));
			tms->m_current_pitch = (tms->m_coeff->pitchtable[tms->m_new_frame_pitch_idx] * (1-tms->m_zpar));
			for (i = 0; i < tms->m_coeff->num_k; i++)
				tms->m_current_k[i] = (tms->m_coeff->ktable[i][tms->m_new_frame_k_idx[i]] * (1-((i<4)?tms->m_zpar:tms->m_uv_zpar)));
		}
#else
		//Updates to parameters only happen on subcycle '2' (B cycle) of PCs.
		if (tms->m_subcycle == 2)
		{
			switch(tms->m_PC)
			{
				case 0: /* PC = 0, B cycle, write updated energy */
				if (tms->m_IP==0) tms->m_pitch_zero = 0; // this reset happens around the second subcycle during IP=0
				tms->m_current_energy = (tms->m_current_energy + (((tms->m_coeff->energytable[tms->m_new_frame_energy_idx] - tms->m_current_energy)*(1-inhibit_state)) INTERP_SHIFT))*(1-tms->m_zpar);
				break;
				case 1: /* PC = 1, B cycle, write updated pitch */
				tms->m_current_pitch = (tms->m_current_pitch + (((tms->m_coeff->pitchtable[tms->m_new_frame_pitch_idx] - tms->m_current_pitch)*(1-inhibit_state)) INTERP_SHIFT))*(1-tms->m_zpar);
				break;
				case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 11:
				/* PC = 2 through 11, B cycle, write updated K1 through K10 */
				tms->m_current_k[tms->m_PC-2] = (tms->m_current_k[tms->m_PC-2] + (((tms->m_coeff->ktable[tms->m_PC-2][tms->m_new_frame_k_idx[tms->m_PC-2]] - tms->m_current_k[tms->m_PC-2])*(1-inhibit_state)) INTERP_SHIFT))*(1-(((tms->m_PC-2)<4)?tms->m_zpar:tms->m_uv_zpar));
				break;
				case 12: /* PC = 12 */
				/* we should NEVER reach this point, PC=12 doesn't have a subcycle 2 */
				break;
			}
		}
#endif
	}
}

static void tms5220_advance_counters(tms5220_state *tms)
{
	tms->m_subcycle++;
	if ((tms->m_subcycle == 2) && (tms->m_PC == 12)) // RESETF3
	{
		/* Circuit 412 in the patent acts a reset, resetting the pitch counter to 0
		 * if INHIBIT was true during the most recent frame transition.
		 * The exact time this occurs is betwen IP=7, PC=12 sub=0, T=t12
		 * and tms->m_IP = 0, PC=0 sub=0, T=t12, a period of exactly 20 cycles,
		 * which overlaps the time OLDE and OLDP are updated at IP=7 PC=12 T17
		 * (and hence INHIBIT itself 2 t-cycles later).
		 * According to testing the pitch zeroing lasts approximately 2 samples.
		 * We set the zeroing latch here, and unset it on PC=1 in the generator.
		 */
		if ((tms->m_IP == 7) && tms->m_inhibit) tms->m_pitch_zero = true;
		if (tms->m_IP == 7) // RESETL4
		{
			// Latch OLDE and OLDP
			//if (tms->m_OLDE) tms->m_uv_zpar = false; // TMS51xx INTERP BUG2
			tms->m_OLDE = (tms->m_new_frame_energy_idx == 0); // tms->m_OLDE
			tms->m_OLDP = (tms->m_new_frame_pitch_idx == 0); // tms->m_OLDP
			/* if TALK was clear last frame, halt speech now, since TALKD (latched from TALK on new frame) just went inactive. */

			/* NOTE: in MAME this latch is preceded by an
			 * `if ((!m_TALK) && (!m_SPEN))` guarding a LOGMASKED
			 * call only. Stripping the log statement during the
			 * port left the `if` with an empty body, so it
			 * captured the latch below and made it conditional --
			 * see notes/pacing_talkd_latch_bug.md. The latch is
			 * unconditional; do not reintroduce a guard here. */
			tms->m_TALKD = tms->m_TALK; // TALKD is latched from TALK
			tms5220_update_fifo_status_and_ints(tms); // to trigger an interrupt if talk_status has changed
			if ((!tms->m_TALK) && tms->m_SPEN) tms->m_TALK = true; // TALK is only activated if it wasn't already active, if tms->m_SPEN is active, and if we're in RESETL4 (which we are).

			
		}
		tms->m_subcycle = tms->m_subc_reload;
		tms->m_PC = 0;
		tms->m_IP++;
		tms->m_IP &= 0x7;
	}
	else if (tms->m_subcycle == 3)
	{
		tms->m_subcycle = tms->m_subc_reload;
		tms->m_PC++;
	}
}

void tms5220_set_speech_rate(tms5220_state *tms, double rate)
{
	if (rate < 0.1 || rate > 8.0) return;
	tms->m_speech_rate = rate;
}

void tms5220_process(tms5220_state *tms, int16_t *buffer, unsigned int size)
{
	int buf_count = 0;
	int i, bitout, steps;
	int32_t this_sample;

	

	/* loop until the buffer is full or we've stopped speaking */
	while (size > 0)
	{
		/* advance any pending READ BYTE / READ AND BRANCH command once
		 * per generated sample, applying its effect once the delay has
		 * elapsed -- see TMS5220_READ_COMMAND_DELAY_SAMPLES. */
		if (tms->m_pending_command != 0)
		{
			if (--tms->m_pending_ticks <= 0)
			{
				if (tms->m_pending_command == 0x10)
				{
					tms->m_read_byte_register = tms->m_pending_read_byte_register;
					tms->m_RDB_flag = true;
				}
				tms->m_pending_command = 0;
			}
		}

		if(tms->m_TALKD) // speaking
		{
			/* if we're ready for a new frame to be applied, i.e. when IP=0, PC=12, Sub=1
			* (In reality, the frame was really loaded incrementally during the entire IP=0
			* PC=x time period, but it doesn't affect anything until IP=0 PC=12 happens)
			*/
			/* Run the parameter state machine `steps` times for this one
			 * output sample. At the default rate that is exactly once,
			 * in the original order -- step, generate, advance -- so the
			 * output is bit-identical to MAME's. Faster rates take more
			 * machine cycles per sample and so walk the frame sooner;
			 * slower rates sometimes take none and hold the frame while
			 * the audio path below keeps running, which is what leaves
			 * the pitch untouched. */
			steps = 1;
			if (tms->m_speech_rate != 1.0)
			{
				tms->m_rate_acc += tms->m_speech_rate;
				steps = (int)tms->m_rate_acc;
				tms->m_rate_acc -= steps;
			}
			for (i = 1; i < steps; i++)
			{
				tms5220_step_parameters(tms);
				tms5220_advance_counters(tms);
			}
			if (steps > 0) tms5220_step_parameters(tms);

			// calculate the output
			if (tms->m_OLDP)
			{
				// generate unvoiced samples here
				if (tms->m_RNG & 1)
					tms->m_excitation_data = ~0x3F; /* according to the patent it is (either + or -) half of the maximum value in the chirp table, so either 01000000(0x40) or 11000000(0xC0)*/
				else
					tms->m_excitation_data = 0x40;
			}
			else /* (!tms->m_OLDP) */
			{
				// generate voiced samples here
				/* US patent 4331836 Figure 14B shows, and logic would hold, that a pitch based chirp
				 * function has a chirp/peak and then a long chain of zeroes.
				 * The last entry of the chirp rom is at address 0b110011 (51d), the 52nd sample,
				 * and if the address reaches that point the ADDRESS incrementer is
				 * disabled, forcing all samples beyond 51d to be == 51d
				 */
				if (tms->m_pitch_count >= 51)
					tms->m_excitation_data = (int8_t)tms->m_coeff->chirptable[51];
				else /*tms->m_pitch_count < 51*/
					tms->m_excitation_data = (int8_t)tms->m_coeff->chirptable[tms->m_pitch_count];
			}

			// Update LFSR *20* times every sample (once per T cycle), like patent shows
			for (i=0; i<20; i++)
			{
				bitout = ((tms->m_RNG >> 12) & 1) ^
						((tms->m_RNG >>  3) & 1) ^
						((tms->m_RNG >>  2) & 1) ^
						((tms->m_RNG >>  0) & 1);
				tms->m_RNG <<= 1;
				tms->m_RNG |= bitout;
			}
			this_sample = tms5220_lattice_filter(tms); /* execute lattice filter */

			/* (MAME logs the u[] lattice state here in a for loop; the
			 * loop body was log-only, so it is dropped rather than left
			 * behind as an empty-bodied for that captures the next
			 * statement.) */

			/* next, force result to 14 bits (since its possible that the addition at the final (k1) stage of the lattice overflowed) */
			while (this_sample > 16383) this_sample -= 32768;
			while (this_sample < -16384) this_sample += 32768;
			if (tms->m_digital_select == 0) // analog SPK pin output is only 8 bits, with clipping
				buffer[buf_count] = tms5220_clip_analog(tms, this_sample);
			else // digital I/O pin output is 12 bits
			{
#ifdef ALLOW_4_LSB
				// input:  ssss ssss ssss ssss ssnn nnnn nnnn nnnn
				// N taps:                       ^                 = 0x2000;
				// output: ssss ssss ssss ssss snnn nnnn nnnn nnnN
				buffer[buf_count] = (this_sample<<1)|((this_sample&0x2000)>>13);
#else
				this_sample &= ~0xF;
				// input:  ssss ssss ssss ssss ssnn nnnn nnnn 0000
				// N taps:                       ^^ ^^^            = 0x3E00;
				// output: ssss ssss ssss ssss snnn nnnn nnnN NNNN
				buffer[buf_count] = (this_sample<<1)|((this_sample&0x3E00)>>9);
#endif
			}
			// Update all counts

			if (steps > 0) tms5220_advance_counters(tms);
			tms->m_pitch_count++;
			if ((tms->m_pitch_count >= tms->m_current_pitch) || tms->m_pitch_zero) tms->m_pitch_count = 0;
			tms->m_pitch_count &= 0x1FF;
		}
		else // tms->m_TALKD == 0
		{
			tms->m_subcycle++;
			if ((tms->m_subcycle == 2) && (tms->m_PC == 12)) // RESETF3
			{
				if (tms->m_IP == 7) // RESETL4
				{
					tms->m_TALKD = tms->m_TALK; // TALKD is latched from TALK
					tms5220_update_fifo_status_and_ints(tms); // probably not necessary
					if ((!tms->m_TALK) && tms->m_SPEN) tms->m_TALK = true; // TALK is only activated if it wasn't already active, if tms->m_SPEN is active, and if we're in RESETL4 (which we are).
				}
				tms->m_subcycle = tms->m_subc_reload;
				tms->m_PC = 0;
				tms->m_IP++;
				tms->m_IP&=0x7;
			}
			else if (tms->m_subcycle == 3)
			{
				tms->m_subcycle = tms->m_subc_reload;
				tms->m_PC++;
			}
			buffer[buf_count] = -1; /* should be just -1; actual chip outputs -1 every idle sample; (cf note in data sheet, p 10, table 4) */
		}
	buf_count++;
	size--;
	}
}

void tms5220_process_command(tms5220_state *tms, uint8_t cmd)
{
	tms->m_command_register = cmd;

	/* parse the command */
	switch (cmd & 0x70)
	{
	case 0x10 : /* read byte */
		
		if (!tms5220_talk_status(tms)) /* TALKST must be clear for RDBY */
		{
			if (tms->m_schedule_dummy_read)
			{
				tms->m_schedule_dummy_read = false;
				tms5220_perform_dummy_read(tms);
			}

			/* Real hardware shifts this out serially over the VSM's
			 * ROMCLK protocol, which takes measurable chip time -- it
			 * does not complete within the same write that issued the
			 * command. Defer becoming visible to a status read until
			 * that time has passed (see tms5220_process), so an
			 * unrelated status read shortly after this command doesn't
			 * see a stale not-yet-ready result. (A proper /RS-/WS-
			 * /READY true-timing implementation was attempted and is
			 * available as tms5220_rsq_w/wsq_w/tick_ready_timer below,
			 * but proved too fragile -- razor-thin, sub-microsecond
			 * sensitive -- to rely on yet with this port's simplified
			 * single-pending-timer model. See notes/. This sample-based
			 * approach is the validated, working one for now.) */
			tms->m_pending_read_byte_register = (uint8_t)tms5220_read_bits(tms, 8);
			tms->m_pending_command = 0x10;
			tms->m_pending_ticks = TMS5220_READ_COMMAND_DELAY_SAMPLES;
			tms->m_command_register = NOCOMMAND;
		}
		/* (MAME logs here in the else; dropping the log statement must
		 * not leave the break as the else's body -- that would fall
		 * through into the next case.) */
		break;

	case 0x00:
	case 0x20: /* set rate (tms5220c and cd2501ecd only), otherwise NOP */
		if (TMS5220_HAS_RATE_CONTROL)
		{
			tms->m_c_variant_rate = cmd&0x0F;
		}
		tms->m_command_register = NOCOMMAND;
		break;

	case 0x30 : /* read and branch */
		if (!tms5220_talk_status(tms)) /* TALKST must be clear for RB */
		{
			
			tms->m_RDB_flag = false;

			if (tms->m_schedule_dummy_read)
			{
				tms->m_schedule_dummy_read = false;
				tms5220_perform_dummy_read(tms);
			}

			if (0)
				tms5220_vsm_read_and_branch(tms);

			tms->m_command_register = NOCOMMAND;
		}
		break;

	case 0x40 : /* load address */
		if (!tms5220_talk_status(tms)) /* TALKST must be clear for LA */
		{
			

			// tms5220 data sheet says that if we load only one 4-bit nibble,
			// it won't work. This code does not care about this.
			if (0)
				tms5220_vsm_write_addr(tms, cmd & 0x0f);

			tms->m_schedule_dummy_read = true;
			tms->m_command_register = NOCOMMAND;
		}
		else
		{
			// The Load Address command is not ignored during speech output,
			// as tests show. In fact, it is normally executed when the speech
			// terminates.
			
		}
		break;

	case 0x50 : /* speak */
		
		if (tms->m_schedule_dummy_read)
		{
			tms->m_schedule_dummy_read = false;
			tms5220_perform_dummy_read(tms);
		}

		tms->m_SPEN = 1;
#ifdef FAST_START_HACK
		tms->m_TALK = 1;
#endif
		tms->m_DDIS = false; // speak using VSM
		tms->m_zpar = true; // zero all the parameters
		tms->m_uv_zpar = true; // zero k4-k10 as well
		tms->m_OLDE = true; // 'silence/zpar' frames are zero energy
		tms->m_OLDP = true; // 'silence/zpar' frames are zero pitch
#ifdef TMS5220_PERFECT_INTERPOLATION_HACK
		tms->m_old_zpar = true; // zero all the old parameters
		tms->m_old_uv_zpar = true; // zero old k4-k10 as well
#endif
		// following is semi-hack but matches idle state observed on chip
		tms->m_new_frame_energy_idx = 0;
		tms->m_new_frame_pitch_idx = 0;
		for (int i = 0; i < 4; i++)
			tms->m_new_frame_k_idx[i] = 0;
		for (int i = 4; i < 7; i++)
			tms->m_new_frame_k_idx[i] = 0xF;
		for (int i = 7; i < tms->m_coeff->num_k; i++)
			tms->m_new_frame_k_idx[i] = 0x7;

		tms->m_command_register = NOCOMMAND;
		break;

	case 0x60 : /* speak external */
		

		// SPKEXT going active asserts /SPKEE for 2 clocks, which clears the FIFO and its counters
		memset(tms->m_fifo, 0, sizeof(tms->m_fifo));
		tms->m_fifo_head = tms->m_fifo_tail = tms->m_fifo_count = tms->m_fifo_bits_taken = 0;
		// SPEN is enabled when the FIFO passes half full (falling edge of BL signal)
		tms->m_DDIS = true; // speak using FIFO
		tms->m_seen_ddis = true;
		tms->m_zpar = true; // zero all the parameters
		tms->m_uv_zpar = true; // zero k4-k10 as well
		tms->m_OLDE = true; // 'silence/zpar' frames are zero energy
		tms->m_OLDP = true; // 'silence/zpar' frames are zero pitch
#ifdef TMS5220_PERFECT_INTERPOLATION_HACK
		tms->m_old_zpar = true; // zero all the old parameters
		tms->m_old_uv_zpar = true; // zero old k4-k10 as well
#endif
		// following is semi-hack but matches idle state observed on chip
		tms->m_new_frame_energy_idx = 0;
		tms->m_new_frame_pitch_idx = 0;
		for (int i = 0; i < 4; i++)
			tms->m_new_frame_k_idx[i] = 0;
		for (int i = 4; i < 7; i++)
			tms->m_new_frame_k_idx[i] = 0xF;
		for (int i = 7; i < tms->m_coeff->num_k; i++)
			tms->m_new_frame_k_idx[i] = 0x7;
		tms->m_RDB_flag = false;

		tms->m_command_register = NOCOMMAND;
		break;

	case 0x70 : /* reset */
		
		if (tms->m_schedule_dummy_read)
		{
			tms->m_schedule_dummy_read = false;
			tms5220_perform_dummy_read(tms);
		}

		tms5220_reset(tms, tms->m_variant);
		tms->m_command_register = NOCOMMAND;
		break;
	}

	/* update the buffer low state */
	tms5220_update_fifo_status_and_ints(tms);
}

void tms5220_parse_frame(tms5220_state *tms)
{
	int i, rep_flag;
#ifdef TMS5220_PERFECT_INTERPOLATION_HACK
	tms->m_old_uv_zpar = tms->m_uv_zpar;
	tms->m_old_zpar = tms->m_zpar;
#endif
	/* Since we're parsing a frame, we must be talking, so clear zpar here.
	Also, before we started parsing a frame, the P=0 and E=0 latches were both
	reset by RESETL4, so clear tms->m_uv_zpar here.
	*/
	tms->m_uv_zpar = tms->m_zpar = 0;

	/* We actually don't care how many bits are left in the FIFO here;
	the frame subpart will be processed normally, and any bits extracted
	'past the end' of the FIFO will be read as zeroes; the FIFO being emptied
	will set the /BE latch which will halt speech exactly as if a stop frame
	had been encountered (instead of whatever partial frame was read).
	The same exact circuitry is used for both functions on the real chip, see
	us patent 4335277 sheet 16, gates 232a (decode stop frame) and 232b
	(decode /BE plus DDIS (decode disable) which is active during speak external).
	*/

	/* if the chip is a tms5220C, and the rate mode is set to that each frame (0x04 bit set)
	has a 2 bit rate preceding it, grab two bits here and store them as the rate; */
	if ((TMS5220_HAS_RATE_CONTROL) && (tms->m_c_variant_rate & 0x04))
	{
		i = tms5220_read_bits(tms, 2);
		
		tms->m_IP = reload_table[i];
	}
	else // non-5220C and 5220C in fixed rate mode
	tms->m_IP = reload_table[tms->m_c_variant_rate&0x3];

	tms5220_update_fifo_status_and_ints(tms);
	if (tms->m_DDIS && tms->m_buffer_empty) goto ranout;

	// attempt to extract the energy index
	tms->m_new_frame_energy_idx = tms5220_read_bits(tms, tms->m_coeff->energy_bits);
	
	tms5220_update_fifo_status_and_ints(tms);
	if (tms->m_DDIS && tms->m_buffer_empty) goto ranout;
	// if the energy index is 0 or 15, we're done
	if ((tms->m_new_frame_energy_idx == 0) || (tms->m_new_frame_energy_idx == 15))
		return;


	// attempt to extract the repeat flag
	rep_flag = tms5220_read_bits(tms, 1);
	

	// attempt to extract the pitch
	tms->m_new_frame_pitch_idx = tms5220_read_bits(tms, tms->m_coeff->pitch_bits);
	
	// if the new frame is unvoiced, be sure to zero out the k5-k10 parameters
	tms->m_uv_zpar = (tms->m_new_frame_pitch_idx == 0);
	tms5220_update_fifo_status_and_ints(tms);
	if (tms->m_DDIS && tms->m_buffer_empty) goto ranout;
	// if this is a repeat frame, just do nothing, it will reuse the old coefficients
	if (rep_flag)
		return;

	// extract first 4 K coefficients
	for (i = 0; i < 4; i++)
	{
		tms->m_new_frame_k_idx[i] = tms5220_read_bits(tms, tms->m_coeff->kbits[i]);
		
		tms5220_update_fifo_status_and_ints(tms);
		if (tms->m_DDIS && tms->m_buffer_empty) goto ranout;
	}

	// if the pitch index was zero, we only need 4 K's...
	if (tms->m_new_frame_pitch_idx == 0)
	{
		/* and the rest of the coefficients are zeroed, but that's done in the generator code */
		return;
	}

	// If we got here, we need the remaining 6 K's
	for (i = 4; i < tms->m_coeff->num_k; i++)
	{
		tms->m_new_frame_k_idx[i] = tms5220_read_bits(tms, tms->m_coeff->kbits[i]);
		
		tms5220_update_fifo_status_and_ints(tms);
		if (tms->m_DDIS && tms->m_buffer_empty) goto ranout;
	}
	

	return;

	ranout:
	
	return;
}

void tms5220_set_interrupt_state(tms5220_state *tms, int state)
{
	if (!TMS5220_IS_52xx) return; // bail out if not a 52xx chip, since there's no int pin

	

	if (state != tms->m_irq_pin)
	{
		tms->m_irq_pin = state;
			}
}

void tms5220_update_ready_state(tms5220_state *tms)
{
	/* MAME uses ready_read() here, not m_io_ready directly: in instant
	 * mode ready_read() also accounts for a full FIFO. It then invokes
	 * the /READY handler, which the port had dropped along with the log
	 * statement beside it -- the Echo II card needs that edge to release
	 * its write latch (a2echoii.cpp's tms_readyq_callback). */
	bool state = tms5220_ready_read(tms);
	if (tms->m_ready_pin != state)
	{
		if (tms->m_readyq_handler)
			tms->m_readyq_handler(tms->m_readyq_ctx, !state); /* /READY is active low */
		tms->m_ready_pin = state;
	}
}

void tms5220_rsq_w(tms5220_state *tms, int state) {
    tms->m_true_timing = true;
    state &= 1;
    uint8_t new_val = (uint8_t)((tms->m_rs_ws & 0x01) | (state << 1));
    if (new_val != tms->m_rs_ws) {
        tms->m_rs_ws = new_val;
        if (new_val == 0) {
            return; /* illegal on plain TMS5220 -- ignore */
        } else if (new_val == 3) {
            tms->m_read_latch = 0xff; /* high impedance */
            return;
        }
        if (!state) {
            /* high to low -- schedule ready cycle for a read.
             * MAME's rsq_w uses a fixed 13 microseconds regardless of
             * what's being read (status vs. a pending READ BYTE result). */
            tms->m_io_ready = false;
            tms5220_update_ready_state(tms);
            tms->m_pending_ready_action = 1;
            tms->m_pending_ready_us_remaining = 13.0;
        }
    }
}

void tms5220_wsq_w(tms5220_state *tms, int state, uint8_t pending_byte) {
    tms->m_true_timing = true;
    state &= 1;
    uint8_t new_val = (uint8_t)((tms->m_rs_ws & 0x02) | (state << 0));
    if (new_val != tms->m_rs_ws) {
        tms->m_rs_ws = new_val;
        if (new_val == 0) {
            return; /* illegal on plain TMS5220 -- ignore */
        } else if (new_val == 3) {
            tms->m_read_latch = 0xff; /* high impedance */
            return;
        }
        if (!state) {
            /* high to low -- schedule ready cycle for a write. MAME uses
             * a flat 16 chip clocks for every write (with a TODO noting
             * real per-command timing varies), which at the Echo II's
             * 640kHz is 25us. An earlier version of this function peeked
             * at the command byte and used 520us for READ BYTE / READ
             * AND BRANCH, which was the sample-based delay hack wearing
             * a different hat; matching MAME exactly is the point of
             * this path, so the peek is gone. */
            (void)pending_byte;
            tms->m_io_ready = false;
            tms5220_update_ready_state(tms);
            tms->m_pending_ready_action = 2;
            tms->m_pending_ready_us_remaining = 25.0; /* 16 clocks @ 640kHz */
        }
    }
}

void tms5220_tick_ready_timer(tms5220_state *tms, double elapsed_us) {
    if (tms->m_pending_ready_action == 0) return;
    tms->m_pending_ready_us_remaining -= elapsed_us;
    if (tms->m_pending_ready_us_remaining > 0) return;

    int action = tms->m_pending_ready_action;
    tms->m_pending_ready_action = 0;

    if (action == 1) {
        /* Read: matches MAME's set_io_ready() case 0x01 */
        tms->m_read_latch = tms5220_status_read(tms, true);
        tms->m_io_ready = true;
    } else if (action == 2) {
        /* Write: matches MAME's set_io_ready() case 0x02 */
        if ((tms->m_fifo_count >= FIFO_SIZE) && tms->m_DDIS) {
            /* FIFO full -- retry in another 16 cycles (25us), same as MAME */
            tms->m_pending_ready_action = 2;
            tms->m_pending_ready_us_remaining = 25.0;
            return;
        }
        if (tms->m_command_register == NOCOMMAND) {
            tms->m_data_latched = false;
            tms5220_data_write(tms, tms->m_write_latch);
            /* data_write raises m_io_ready itself once the latch is
             * consumed. Deliberately NOT set here: when the command
             * register is still busy MAME leaves /READY inactive and
             * keeps the latch, which is what throttles the writer. */
        }
    }
    tms5220_update_ready_state(tms);
}

void tms5220_data_w(tms5220_state *tms, uint8_t data)
{
	
	/* bring up to date first */
		tms->m_write_latch = data;
	tms->m_data_latched = true;

	if (!tms->m_true_timing) // if we're in the default hacky mode where we don't bother with rsq_w and wsq_w...
		tms5220_data_write(tms, tms->m_write_latch); // ...force the write through instantly.
	else
	{
		/* actually in a write ? */
		/* (debug logging removed) */
	}
}

uint8_t tms5220_status_r(tms5220_state *tms)
{
	if (!tms->m_true_timing)
	{
		// prevent debugger from changing the internal state
		if (false)
			return tms5220_status_read(tms, false);
		return tms5220_status_read(tms, true);
	}
	else
	{
		/* actually in a read ? */
		if (tms->m_rs_ws == 0x01)
			return tms->m_read_latch;
		return 0xff; // tms->m_write_latch; // TODO: return open bus?
	}
}

int tms5220_readyq_r(tms5220_state *tms)
{
	// prevent debugger from changing the internal state
	if (!false)
		 /* bring up to date first */
	return !tms5220_ready_read(tms);
}

int tms5220_intq_r(tms5220_state *tms)
{
	// prevent debugger from changing the internal state
	if (!false)
		 /* bring up to date first */
	return !tms5220_int_read(tms);
}
