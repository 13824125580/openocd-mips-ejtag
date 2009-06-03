/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2007,2008 Øyvind Harboe                                 *
 *   oyvind.harboe@zylin.com                                               *
 *                                                                         *
 *   Copyright (C) 2009 Zachary T Welch                                    *
 *   zw@superlucidity.net                                                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#ifndef OPENOCD_JTAG_INTERFACE_H
#define OPENOCD_JTAG_INTERFACE_H

/* @file
 * The "Cable Helper API" is what the cable drivers can use to help
 * implement their "Cable API".  So a Cable Helper API is a set of
 * helper functions used by cable drivers, and this is different from a
 * Cable API.  A "Cable API" is what higher level code used to talk to a
 * cable.
 */


/** implementation of wrapper function tap_set_state() */
void tap_set_state_impl(tap_state_t new_state);

/**
 * This function sets the state of a "state follower" which tracks the
 * state of the TAPs connected to the cable.  The state follower is
 * hopefully always in the same state as the actual TAPs in the jtag
 * chain, and will be so if there are no bugs in the tracking logic
 * within that cable driver.
 *
 * All the cable drivers call this function to indicate the state they
 * think the TAPs attached to their cables are in.  Because this
 * function can also log transitions, it will be helpful to call this
 * function with every transition that the TAPs being manipulated are
 * expected to traverse, not just end points of a multi-step state path.
 *
 * @param new_state The state we think the TAPs are currently in (or
 * 	are about to enter).
 */
#if defined(_DEBUG_JTAG_IO_)
#define tap_set_state(new_state) \
	do { \
		LOG_DEBUG( "tap_set_state(%s)", tap_state_name(new_state) ); \
		tap_set_state_impl(new_state); \
	} while (0)
#else
static inline void tap_set_state(tap_state_t new_state)
{
	tap_set_state_impl(new_state);
}
#endif

/**
 * This function gets the state of the "state follower" which tracks the
 * state of the TAPs connected to the cable. @see tap_set_state @return
 * tap_state_t The state the TAPs are in now.
 */
tap_state_t tap_get_state(void);

/**
 * This function sets the state of an "end state follower" which tracks
 * the state that any cable driver thinks will be the end (resultant)
 * state of the current TAP SIR or SDR operation.
 *
 * At completion of that TAP operation this value is copied into the
 * state follower via tap_set_state().
 *
 * @param new_end_state The state the TAPs should enter at completion of
 * 	a pending TAP operation.
 */
void tap_set_end_state(tap_state_t new_end_state);

/**
 * For more information, @see tap_set_end_state
 * @return tap_state_t - The state the TAPs should be in at completion of the current TAP operation.
 */
tap_state_t tap_get_end_state(void);

/**
 * This function provides a "bit sequence" indicating what has to be
 * done with TMS during a sequence of seven TAP clock cycles in order to
 * get from state \a "from" to state \a "to".
 *
 * The length of the sequence must be determined with a parallel call to
 * tap_get_tms_path_len().
 *
 * @param from The starting state.
 * @param to The desired final state.
 * @return int The required TMS bit sequence, with the first bit in the
 * 	sequence at bit 0.
 */
int tap_get_tms_path(tap_state_t from, tap_state_t to);


/**
 * Function int tap_get_tms_path_len
 * returns the total number of bits that represents a TMS path
 * transition as given by the function tap_get_tms_path().
 *
 * For at least one interface (JLink) it's not OK to simply "pad" TMS
 * sequences to fit a whole byte.  (I suspect this is a general TAP
 * problem within OOCD.) Padding TMS causes all manner of instability
 * that's not easily discovered.  Using this routine we can apply
 * EXACTLY the state transitions required to make something work - no
 * more - no less.
 *
 * @param from is the starting state
 * @param to is the resultant or final state
 * @return int - the total number of bits in a transition.
 */
int tap_get_tms_path_len(tap_state_t from, tap_state_t to);


/**
 * Function tap_move_ndx
 * when given a stable state, returns an index from 0-5.  The index corresponds to a
 * sequence of stable states which are given in this order: <p>
 * { TAP_RESET, TAP_IDLE, TAP_DRSHIFT, TAP_DRPAUSE, TAP_IRSHIFT, TAP_IRPAUSE }
 * <p>
 * This sequence corresponds to look up tables which are used in some of the
 * cable drivers.
 * @param astate is the stable state to find in the sequence.  If a non stable
 *  state is passed, this may cause the program to output an error message
 *  and terminate.
 * @return int - the array (or sequence) index as described above
 */
int tap_move_ndx(tap_state_t astate);

/**
 * Function tap_is_state_stable
 * returns true if the \a astate is stable.
 */
bool tap_is_state_stable(tap_state_t astate);

/**
 * Function tap_state_transition
 * takes a current TAP state and returns the next state according to the tms value.
 * @param current_state is the state of a TAP currently.
 * @param tms is either zero or non-zero, just like a real TMS line in a jtag interface.
 * @return tap_state_t - the next state a TAP would enter.
 */
tap_state_t tap_state_transition(tap_state_t current_state, bool tms);

/**
 * Function tap_state_name
 * Returns a string suitable for display representing the JTAG tap_state
 */
const char* tap_state_name(tap_state_t state);

/// Provides user-friendly name lookup of TAP states.
tap_state_t tap_state_by_name(const char *name);

#ifdef _DEBUG_JTAG_IO_
/**
 * @brief Prints verbose TAP state transitions for the given TMS/TDI buffers.
 * @param tms_buf must points to a buffer containing the TMS bitstream.
 * @param tdi_buf must points to a buffer containing the TDI bitstream.
 * @param tap_len must specify the length of the TMS/TDI bitstreams.
 * @param start_tap_state must specify the current TAP state.
 * @returns the final TAP state; pass as @a start_tap_state in following call.
 */
tap_state_t jtag_debug_state_machine(const void *tms_buf, const void *tdi_buf,
		unsigned tap_len, tap_state_t start_tap_state);
#else
static inline tap_state_t jtag_debug_state_machine(const void *tms_buf,
		const void *tdi_buf, unsigned tap_len, tap_state_t start_tap_state)
{
	return start_tap_state;
}
#endif // _DEBUG_JTAG_IO_

typedef struct jtag_interface_s
{
	char* name;

	/* queued command execution
	 */
	int (*execute_queue)(void);

	/* interface initalization
	 */
	int (*speed)(int speed);
	int (*register_commands)(struct command_context_s* cmd_ctx);
	int (*init)(void);
	int (*quit)(void);

	/* returns JTAG maxium speed for KHz. 0=RTCK. The function returns
	 *  a failure if it can't support the KHz/RTCK.
	 *
	 *  WARNING!!!! if RTCK is *slow* then think carefully about
	 *  whether you actually want to support this in the driver.
	 *  Many target scripts are written to handle the absence of RTCK
	 *  and use a fallback kHz TCK.
	 */
	int (*khz)(int khz, int* jtag_speed);

	/* returns the KHz for the provided JTAG speed. 0=RTCK. The function returns
	 *  a failure if it can't support the KHz/RTCK. */
	int (*speed_div)(int speed, int* khz);

	/* Read and clear the power dropout flag. Note that a power dropout
	 *  can be transitionary, easily much less than a ms.
	 *
	 *  So to find out if the power is *currently* on, you must invoke
	 *  this method twice. Once to clear the power dropout flag and a
	 *  second time to read the current state.
	 *
	 *  Currently the default implementation is never to detect power dropout.
	 */
	int (*power_dropout)(int* power_dropout);

	/* Read and clear the srst asserted detection flag.
	 *
	 * NB!!!! like power_dropout this does *not* read the current
	 * state. srst assertion is transitionary and *can* be much
	 * less than 1ms.
	 */
	int (*srst_asserted)(int* srst_asserted);
} jtag_interface_t;


#endif // OPENOCD_JTAG_INTERFACE_H