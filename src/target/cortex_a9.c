/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2006 by Magnus Lundin                                   *
 *   lundin@mlu.mine.nu                                                    *
 *                                                                         *
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2009 by Dirk Behme                                      *
 *   dirk.behme@gmail.com - copy from cortex_m3                            *
 *                                                                         *
 *   Copyright (C) 2010 Øyvind Harboe                                      *
 *   oyvind.harboe@zylin.com                                               *
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
 *                                                                         *
 *   Cortex-A9(tm) TRM, ARM DDI 0407F                                      *
 *                                                                         *
 ***************************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "breakpoints.h"
#include "cortex_a9.h"
#include "register.h"
#include "target_request.h"
#include "target_type.h"
#include "arm_opcodes.h"
#include <helper/time_support.h>

static int cortex_a9_poll(struct target *target);
static int cortex_a9_debug_entry(struct target *target);
static int cortex_a9_restore_context(struct target *target, bool bpwp);
static int cortex_a9_set_breakpoint(struct target *target,
		struct breakpoint *breakpoint, uint8_t matchmode);
static int cortex_a9_unset_breakpoint(struct target *target,
		struct breakpoint *breakpoint);
static int cortex_a9_dap_read_coreregister_u32(struct target *target,
		uint32_t *value, int regnum);
static int cortex_a9_dap_write_coreregister_u32(struct target *target,
		uint32_t value, int regnum);
static int cortex_a9_mmu(struct target *target, int *enabled);
static int cortex_a9_virt2phys(struct target *target,
                uint32_t virt, uint32_t *phys);
static int cortex_a9_disable_mmu_caches(struct target *target, int mmu,
                int d_u_cache, int i_cache);
static int cortex_a9_enable_mmu_caches(struct target *target, int mmu,
                int d_u_cache, int i_cache);
static int cortex_a9_get_ttb(struct target *target, uint32_t *result);


/*
 * FIXME do topology discovery using the ROM; don't
 * assume this is an OMAP3.   Also, allow for multiple ARMv7-A
 * cores, with different AP numbering ... don't use a #define
 * for these numbers, use per-core armv7a state.
 */
#define swjdp_memoryap 0
#define swjdp_debugap 1

/*
 * Cortex-A9 Basic debug access, very low level assumes state is saved
 */
static int cortex_a9_init_debug_access(struct target *target)
{
	struct armv7a_common *armv7a = target_to_armv7a(target);
	struct adiv5_dap *swjdp = &armv7a->dap;

	int retval;
	uint32_t dummy;

	LOG_DEBUG(" ");

	/* Unlocking the debug registers for modification */
	/* The debugport might be uninitialised so try twice */
	retval = mem_ap_write_atomic_u32(swjdp, armv7a->debug_base + CPUDBG_LOCKACCESS, 0xC5ACCE55);
	if (retval != ERROR_OK)
	{
		/* try again */
		retval = mem_ap_write_atomic_u32(swjdp, armv7a->debug_base + CPUDBG_LOCKACCESS, 0xC5ACCE55);
		if (retval == ERROR_OK)
		{
			LOG_USER("Locking debug access failed on first, but succeeded on second try.");
		}
	}
	if (retval != ERROR_OK)
		return retval;
	/* Clear Sticky Power Down status Bit in PRSR to enable access to
	   the registers in the Core Power Domain */
	retval = mem_ap_read_atomic_u32(swjdp, armv7a->debug_base + CPUDBG_PRSR, &dummy);
	if (retval != ERROR_OK)
		return retval;

	/* Enabling of instruction execution in debug mode is done in debug_entry code */

	/* Resync breakpoint registers */

	/* Since this is likely called from init or reset, update target state information*/
	retval = cortex_a9_poll(target);

	return retval;
}

/* To reduce needless round-trips, pass in a pointer to the current
 * DSCR value.  Initialize it to zero if you just need to know the
 * value on return from this function; or DSCR_INSTR_COMP if you
 * happen to know that no instruction is pending.
 */
static int cortex_a9_exec_opcode(struct target *target,
		uint32_t opcode, uint32_t *dscr_p)
{
	uint32_t dscr;
	int retval;
	struct armv7a_common *armv7a = target_to_armv7a(target);
	struct adiv5_dap *swjdp = &armv7a->dap;

	dscr = dscr_p ? *dscr_p : 0;

	LOG_DEBUG("exec opcode 0x%08" PRIx32, opcode);

	/* Wait for InstrCompl bit to be set */
	long long then = timeval_ms();
	while ((dscr & DSCR_INSTR_COMP) == 0)
	{
		retval = mem_ap_read_atomic_u32(swjdp,
				armv7a->debug_base + CPUDBG_DSCR, &dscr);
		if (retval != ERROR_OK)
		{
			LOG_ERROR("Could not read DSCR register, opcode = 0x%08" PRIx32, opcode);
			return retval;
		}
		if (timeval_ms() > then + 1000)
		{
			LOG_ERROR("Timeout waiting for cortex_a9_exec_opcode");
			return ERROR_FAIL;
		}
	}

	retval = mem_ap_write_u32(swjdp, armv7a->debug_base + CPUDBG_ITR, opcode);
	if (retval != ERROR_OK)
		return retval;

	then = timeval_ms();
	do
	{
		retval = mem_ap_read_atomic_u32(swjdp,
				armv7a->debug_base + CPUDBG_DSCR, &dscr);
		if (retval != ERROR_OK)
		{
			LOG_ERROR("Could not read DSCR register");
			return retval;
		}
		if (timeval_ms() > then + 1000)
		{
			LOG_ERROR("Timeout waiting for cortex_a9_exec_opcode");
			return ERROR_FAIL;
		}
	}
	while ((dscr & DSCR_INSTR_COMP) == 0); /* Wait for InstrCompl bit to be set */

	if (dscr_p)
		*dscr_p = dscr;

	return retval;
}

/**************************************************************************
Read core register with very few exec_opcode, fast but needs work_area.
This can cause problems with MMU active.
**************************************************************************/
static int cortex_a9_read_regs_through_mem(struct target *target, uint32_t address,
		uint32_t * regfile)
{
	int retval = ERROR_OK;
	struct armv7a_common *armv7a = target_to_armv7a(target);
	struct adiv5_dap *swjdp = &armv7a->dap;

	retval = cortex_a9_dap_read_coreregister_u32(target, regfile, 0);
	if (retval != ERROR_OK)
		return retval;
	retval = cortex_a9_dap_write_coreregister_u32(target, address, 0);
	if (retval != ERROR_OK)
		return retval;
	retval = cortex_a9_exec_opcode(target, ARMV4_5_STMIA(0, 0xFFFE, 0, 0), NULL);
	if (retval != ERROR_OK)
		return retval;

	dap_ap_select(swjdp, swjdp_memoryap);
	retval = mem_ap_read_buf_u32(swjdp, (uint8_t *)(&regfile[1]), 4*15, address);
	if (retval != ERROR_OK)
		return retval;
	dap_ap_select(swjdp, swjdp_debugap);

	return retval;
}

static int cortex_a9_dap_read_coreregister_u32(struct target *target,
		uint32_t *value, int regnum)
{
	int retval = ERROR_OK;
	uint8_t reg = regnum&0xFF;
	uint32_t dscr = 0;
	struct armv7a_common *armv7a = target_to_armv7a(target);
	struct adiv5_dap *swjdp = &armv7a->dap;

	if (reg > 17)
		return retval;

	if (reg < 15)
	{
		/* Rn to DCCTX, "MCR p14, 0, Rn, c0, c5, 0"  0xEE00nE15 */
		retval = cortex_a9_exec_opcode(target,
				ARMV4_5_MCR(14, 0, reg, 0, 5, 0),
				&dscr);
		if (retval != ERROR_OK)
			return retval;
	}
	else if (reg == 15)
	{
		/* "MOV r0, r15"; then move r0 to DCCTX */
		retval = cortex_a9_exec_opcode(target, 0xE1A0000F, &dscr);
		if (retval != ERROR_OK)
			return retval;
		retval = cortex_a9_exec_opcode(target,
				ARMV4_5_MCR(14, 0, 0, 0, 5, 0),
				&dscr);
		if (retval != ERROR_OK)
			return retval;
	}
	else
	{
		/* "MRS r0, CPSR" or "MRS r0, SPSR"
		 * then move r0 to DCCTX
		 */
		retval = cortex_a9_exec_opcode(target, ARMV4_5_MRS(0, reg & 1), &dscr);
		if (retval != ERROR_OK)
			return retval;
		retval = cortex_a9_exec_opcode(target,
				ARMV4_5_MCR(14, 0, 0, 0, 5, 0),
				&dscr);
		if (retval != ERROR_OK)
			return retval;
	}

	/* Wait for DTRRXfull then read DTRRTX */
	long long then = timeval_ms();
	while ((dscr & DSCR_DTR_TX_FULL) == 0)
	{
		retval = mem_ap_read_atomic_u32(swjdp,
				armv7a->debug_base + CPUDBG_DSCR, &dscr);
		if (retval != ERROR_OK)
			return retval;
		if (timeval_ms() > then + 1000)
		{
			LOG_ERROR("Timeout waiting for cortex_a9_exec_opcode");
			return ERROR_FAIL;
		}
	}

	retval = mem_ap_read_atomic_u32(swjdp,
			armv7a->debug_base + CPUDBG_DTRTX, value);
	LOG_DEBUG("read DCC 0x%08" PRIx32, *value);

	return retval;
}

static int cortex_a9_dap_write_coreregister_u32(struct target *target,
		uint32_t value, int regnum)
{
	int retval = ERROR_OK;
	uint8_t Rd = regnum&0xFF;
	uint32_t dscr;
	struct armv7a_common *armv7a = target_to_armv7a(target);
	struct adiv5_dap *swjdp = &armv7a->dap;

	LOG_DEBUG("register %i, value 0x%08" PRIx32, regnum, value);

	/* Check that DCCRX is not full */
	retval = mem_ap_read_atomic_u32(swjdp,
				armv7a->debug_base + CPUDBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		return retval;
	if (dscr & DSCR_DTR_RX_FULL)
	{
		LOG_ERROR("DSCR_DTR_RX_FULL, dscr 0x%08" PRIx32, dscr);
		/* Clear DCCRX with MCR(p14, 0, Rd, c0, c5, 0), opcode  0xEE000E15 */
		retval = cortex_a9_exec_opcode(target, ARMV4_5_MRC(14, 0, 0, 0, 5, 0),
				&dscr);
		if (retval != ERROR_OK)
			return retval;
	}

	if (Rd > 17)
		return retval;

	/* Write DTRRX ... sets DSCR.DTRRXfull but exec_opcode() won't care */
	LOG_DEBUG("write DCC 0x%08" PRIx32, value);
	retval = mem_ap_write_u32(swjdp,
			armv7a->debug_base + CPUDBG_DTRRX, value);
	if (retval != ERROR_OK)
		return retval;

	if (Rd < 15)
	{
		/* DCCRX to Rn, "MCR p14, 0, Rn, c0, c5, 0", 0xEE00nE15 */
		retval = cortex_a9_exec_opcode(target, ARMV4_5_MRC(14, 0, Rd, 0, 5, 0),
				&dscr);
		if (retval != ERROR_OK)
			return retval;
	}
	else if (Rd == 15)
	{
		/* DCCRX to R0, "MCR p14, 0, R0, c0, c5, 0", 0xEE000E15
		 * then "mov r15, r0"
		 */
		retval = cortex_a9_exec_opcode(target, ARMV4_5_MRC(14, 0, 0, 0, 5, 0),
				&dscr);
		if (retval != ERROR_OK)
			return retval;
		retval = cortex_a9_exec_opcode(target, 0xE1A0F000, &dscr);
		if (retval != ERROR_OK)
			return retval;
	}
	else
	{
		/* DCCRX to R0, "MCR p14, 0, R0, c0, c5, 0", 0xEE000E15
		 * then "MSR CPSR_cxsf, r0" or "MSR SPSR_cxsf, r0" (all fields)
		 */
		retval = cortex_a9_exec_opcode(target, ARMV4_5_MRC(14, 0, 0, 0, 5, 0),
				&dscr);
		if (retval != ERROR_OK)
			return retval;
		retval = cortex_a9_exec_opcode(target, ARMV4_5_MSR_GP(0, 0xF, Rd & 1),
				&dscr);
		if (retval != ERROR_OK)
			return retval;

		/* "Prefetch flush" after modifying execution status in CPSR */
		if (Rd == 16)
		{
			retval = cortex_a9_exec_opcode(target,
					ARMV4_5_MCR(15, 0, 0, 7, 5, 4),
					&dscr);
			if (retval != ERROR_OK)
				return retval;
		}
	}

	return retval;
}

/* Write to memory mapped registers directly with no cache or mmu handling */
static int cortex_a9_dap_write_memap_register_u32(struct target *target, uint32_t address, uint32_t value)
{
	int retval;
	struct armv7a_common *armv7a = target_to_armv7a(target);
	struct adiv5_dap *swjdp = &armv7a->dap;

	retval = mem_ap_write_atomic_u32(swjdp, address, value);

	return retval;
}

/*
 * Cortex-A9 implementation of Debug Programmer's Model
 *
 * NOTE the invariant:  these routines return with DSCR_INSTR_COMP set,
 * so there's no need to poll for it before executing an instruction.
 *
 * NOTE that in several of these cases the "stall" mode might be useful.
 * It'd let us queue a few operations together... prepare/finish might
 * be the places to enable/disable that mode.
 */

static inline struct cortex_a9_common *dpm_to_a9(struct arm_dpm *dpm)
{
	return container_of(dpm, struct cortex_a9_common, armv7a_common.dpm);
}

static int cortex_a9_write_dcc(struct cortex_a9_common *a9, uint32_t data)
{
	LOG_DEBUG("write DCC 0x%08" PRIx32, data);
	return mem_ap_write_u32(&a9->armv7a_common.dap,
			a9->armv7a_common.debug_base + CPUDBG_DTRRX, data);
}

static int cortex_a9_read_dcc(struct cortex_a9_common *a9, uint32_t *data,
		uint32_t *dscr_p)
{
	struct adiv5_dap *swjdp = &a9->armv7a_common.dap;
	uint32_t dscr = DSCR_INSTR_COMP;
	int retval;

	if (dscr_p)
		dscr = *dscr_p;

	/* Wait for DTRRXfull */
	long long then = timeval_ms();
	while ((dscr & DSCR_DTR_TX_FULL) == 0) {
		retval = mem_ap_read_atomic_u32(swjdp,
				a9->armv7a_common.debug_base + CPUDBG_DSCR,
				&dscr);
		if (retval != ERROR_OK)
			return retval;
		if (timeval_ms() > then + 1000)
		{
			LOG_ERROR("Timeout waiting for read dcc");
			return ERROR_FAIL;
		}
	}

	retval = mem_ap_read_atomic_u32(swjdp,
			a9->armv7a_common.debug_base + CPUDBG_DTRTX, data);
	if (retval != ERROR_OK)
		return retval;
	//LOG_DEBUG("read DCC 0x%08" PRIx32, *data);

	if (dscr_p)
		*dscr_p = dscr;

	return retval;
}

static int cortex_a9_dpm_prepare(struct arm_dpm *dpm)
{
	struct cortex_a9_common *a9 = dpm_to_a9(dpm);
	struct adiv5_dap *swjdp = &a9->armv7a_common.dap;
	uint32_t dscr;
	int retval;

	/* set up invariant:  INSTR_COMP is set after ever DPM operation */
	long long then = timeval_ms();
	for (;;)
	{
		retval = mem_ap_read_atomic_u32(swjdp,
				a9->armv7a_common.debug_base + CPUDBG_DSCR,
				&dscr);
		if (retval != ERROR_OK)
			return retval;
		if ((dscr & DSCR_INSTR_COMP) != 0)
			break;
		if (timeval_ms() > then + 1000)
		{
			LOG_ERROR("Timeout waiting for dpm prepare");
			return ERROR_FAIL;
		}
	}

	/* this "should never happen" ... */
	if (dscr & DSCR_DTR_RX_FULL) {
		LOG_ERROR("DSCR_DTR_RX_FULL, dscr 0x%08" PRIx32, dscr);
		/* Clear DCCRX */
		retval = cortex_a9_exec_opcode(
				a9->armv7a_common.armv4_5_common.target,
				ARMV4_5_MRC(14, 0, 0, 0, 5, 0),
				&dscr);
		if (retval != ERROR_OK)
			return retval;
	}

	return retval;
}

static int cortex_a9_dpm_finish(struct arm_dpm *dpm)
{
	/* REVISIT what could be done here? */
	return ERROR_OK;
}

static int cortex_a9_instr_write_data_dcc(struct arm_dpm *dpm,
		uint32_t opcode, uint32_t data)
{
	struct cortex_a9_common *a9 = dpm_to_a9(dpm);
	int retval;
	uint32_t dscr = DSCR_INSTR_COMP;

	retval = cortex_a9_write_dcc(a9, data);
	if (retval != ERROR_OK)
		return retval;

	return cortex_a9_exec_opcode(
			a9->armv7a_common.armv4_5_common.target,
			opcode,
			&dscr);
}

static int cortex_a9_instr_write_data_r0(struct arm_dpm *dpm,
		uint32_t opcode, uint32_t data)
{
	struct cortex_a9_common *a9 = dpm_to_a9(dpm);
	uint32_t dscr = DSCR_INSTR_COMP;
	int retval;

	retval = cortex_a9_write_dcc(a9, data);
	if (retval != ERROR_OK)
		return retval;

	/* DCCRX to R0, "MCR p14, 0, R0, c0, c5, 0", 0xEE000E15 */
	retval = cortex_a9_exec_opcode(
			a9->armv7a_common.armv4_5_common.target,
			ARMV4_5_MRC(14, 0, 0, 0, 5, 0),
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	/* then the opcode, taking data from R0 */
	retval = cortex_a9_exec_opcode(
			a9->armv7a_common.armv4_5_common.target,
			opcode,
			&dscr);

	return retval;
}

static int cortex_a9_instr_cpsr_sync(struct arm_dpm *dpm)
{
	struct target *target = dpm->arm->target;
	uint32_t dscr = DSCR_INSTR_COMP;

	/* "Prefetch flush" after modifying execution status in CPSR */
	return cortex_a9_exec_opcode(target,
			ARMV4_5_MCR(15, 0, 0, 7, 5, 4),
			&dscr);
}

static int cortex_a9_instr_read_data_dcc(struct arm_dpm *dpm,
		uint32_t opcode, uint32_t *data)
{
	struct cortex_a9_common *a9 = dpm_to_a9(dpm);
	int retval;
	uint32_t dscr = DSCR_INSTR_COMP;

	/* the opcode, writing data to DCC */
	retval = cortex_a9_exec_opcode(
			a9->armv7a_common.armv4_5_common.target,
			opcode,
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	return cortex_a9_read_dcc(a9, data, &dscr);
}


static int cortex_a9_instr_read_data_r0(struct arm_dpm *dpm,
		uint32_t opcode, uint32_t *data)
{
	struct cortex_a9_common *a9 = dpm_to_a9(dpm);
	uint32_t dscr = DSCR_INSTR_COMP;
	int retval;

	/* the opcode, writing data to R0 */
	retval = cortex_a9_exec_opcode(
			a9->armv7a_common.armv4_5_common.target,
			opcode,
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	/* write R0 to DCC */
	retval = cortex_a9_exec_opcode(
			a9->armv7a_common.armv4_5_common.target,
			ARMV4_5_MCR(14, 0, 0, 0, 5, 0),
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	return cortex_a9_read_dcc(a9, data, &dscr);
}

static int cortex_a9_bpwp_enable(struct arm_dpm *dpm, unsigned index_t,
		uint32_t addr, uint32_t control)
{
	struct cortex_a9_common *a9 = dpm_to_a9(dpm);
	uint32_t vr = a9->armv7a_common.debug_base;
	uint32_t cr = a9->armv7a_common.debug_base;
	int retval;

	switch (index_t) {
	case 0 ... 15:		/* breakpoints */
		vr += CPUDBG_BVR_BASE;
		cr += CPUDBG_BCR_BASE;
		break;
	case 16 ... 31:		/* watchpoints */
		vr += CPUDBG_WVR_BASE;
		cr += CPUDBG_WCR_BASE;
		index_t -= 16;
		break;
	default:
		return ERROR_FAIL;
	}
	vr += 4 * index_t;
	cr += 4 * index_t;

	LOG_DEBUG("A9: bpwp enable, vr %08x cr %08x",
			(unsigned) vr, (unsigned) cr);

	retval = cortex_a9_dap_write_memap_register_u32(dpm->arm->target,
			vr, addr);
	if (retval != ERROR_OK)
		return retval;
	retval = cortex_a9_dap_write_memap_register_u32(dpm->arm->target,
			cr, control);
	return retval;
}

static int cortex_a9_bpwp_disable(struct arm_dpm *dpm, unsigned index_t)
{
	struct cortex_a9_common *a9 = dpm_to_a9(dpm);
	uint32_t cr;

	switch (index_t) {
	case 0 ... 15:
		cr = a9->armv7a_common.debug_base + CPUDBG_BCR_BASE;
		break;
	case 16 ... 31:
		cr = a9->armv7a_common.debug_base + CPUDBG_WCR_BASE;
		index_t -= 16;
		break;
	default:
		return ERROR_FAIL;
	}
	cr += 4 * index_t;

	LOG_DEBUG("A9: bpwp disable, cr %08x", (unsigned) cr);

	/* clear control register */
	return cortex_a9_dap_write_memap_register_u32(dpm->arm->target, cr, 0);
}

static int cortex_a9_dpm_setup(struct cortex_a9_common *a9, uint32_t didr)
{
	struct arm_dpm *dpm = &a9->armv7a_common.dpm;
	int retval;

	dpm->arm = &a9->armv7a_common.armv4_5_common;
	dpm->didr = didr;

	dpm->prepare = cortex_a9_dpm_prepare;
	dpm->finish = cortex_a9_dpm_finish;

	dpm->instr_write_data_dcc = cortex_a9_instr_write_data_dcc;
	dpm->instr_write_data_r0 = cortex_a9_instr_write_data_r0;
	dpm->instr_cpsr_sync = cortex_a9_instr_cpsr_sync;

	dpm->instr_read_data_dcc = cortex_a9_instr_read_data_dcc;
	dpm->instr_read_data_r0 = cortex_a9_instr_read_data_r0;

	dpm->bpwp_enable = cortex_a9_bpwp_enable;
	dpm->bpwp_disable = cortex_a9_bpwp_disable;

	retval = arm_dpm_setup(dpm);
	if (retval == ERROR_OK)
		retval = arm_dpm_initialize(dpm);

	return retval;
}


/*
 * Cortex-A9 Run control
 */

static int cortex_a9_poll(struct target *target)
{
	int retval = ERROR_OK;
	uint32_t dscr;
	struct cortex_a9_common *cortex_a9 = target_to_cortex_a9(target);
	struct armv7a_common *armv7a = &cortex_a9->armv7a_common;
	struct adiv5_dap *swjdp = &armv7a->dap;
	enum target_state prev_target_state = target->state;
	uint8_t saved_apsel = dap_ap_get_select(swjdp);

	dap_ap_select(swjdp, swjdp_debugap);
	retval = mem_ap_read_atomic_u32(swjdp,
			armv7a->debug_base + CPUDBG_DSCR, &dscr);
	if (retval != ERROR_OK)
	{
		dap_ap_select(swjdp, saved_apsel);
		return retval;
	}
	cortex_a9->cpudbg_dscr = dscr;

	if (DSCR_RUN_MODE(dscr) == (DSCR_CORE_HALTED | DSCR_CORE_RESTARTED))
	{
		if (prev_target_state != TARGET_HALTED)
		{
			/* We have a halting debug event */
			LOG_DEBUG("Target halted");
			target->state = TARGET_HALTED;
			if ((prev_target_state == TARGET_RUNNING)
					|| (prev_target_state == TARGET_RESET))
			{
				retval = cortex_a9_debug_entry(target);
				if (retval != ERROR_OK)
					return retval;

				target_call_event_callbacks(target,
						TARGET_EVENT_HALTED);
			}
			if (prev_target_state == TARGET_DEBUG_RUNNING)
			{
				LOG_DEBUG(" ");

				retval = cortex_a9_debug_entry(target);
				if (retval != ERROR_OK)
					return retval;

				target_call_event_callbacks(target,
						TARGET_EVENT_DEBUG_HALTED);
			}
		}
	}
	else if (DSCR_RUN_MODE(dscr) == DSCR_CORE_RESTARTED)
	{
		target->state = TARGET_RUNNING;
	}
	else
	{
		LOG_DEBUG("Unknown target state dscr = 0x%08" PRIx32, dscr);
		target->state = TARGET_UNKNOWN;
	}

	dap_ap_select(swjdp, saved_apsel);

	return retval;
}

static int cortex_a9_halt(struct target *target)
{
	int retval = ERROR_OK;
	uint32_t dscr;
	struct armv7a_common *armv7a = target_to_armv7a(target);
	struct adiv5_dap *swjdp = &armv7a->dap;
	uint8_t saved_apsel = dap_ap_get_select(swjdp);
	dap_ap_select(swjdp, swjdp_debugap);

	/*
	 * Tell the core to be halted by writing DRCR with 0x1
	 * and then wait for the core to be halted.
	 */
	retval = mem_ap_write_atomic_u32(swjdp,
			armv7a->debug_base + CPUDBG_DRCR, DRCR_HALT);
	if (retval != ERROR_OK)
		goto out;

	/*
	 * enter halting debug mode
	 */
	retval = mem_ap_read_atomic_u32(swjdp, armv7a->debug_base + CPUDBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		goto out;

	retval = mem_ap_write_atomic_u32(swjdp,
		armv7a->debug_base + CPUDBG_DSCR, dscr | DSCR_HALT_DBG_MODE);
	if (retval != ERROR_OK)
		goto out;

	long long then = timeval_ms();
	for (;;)
	{
		retval = mem_ap_read_atomic_u32(swjdp,
			armv7a->debug_base + CPUDBG_DSCR, &dscr);
		if (retval != ERROR_OK)
			goto out;
		if ((dscr & DSCR_CORE_HALTED) != 0)
		{
			break;
		}
		if (timeval_ms() > then + 1000)
		{
			LOG_ERROR("Timeout waiting for halt");
			return ERROR_FAIL;
		}
	}

	target->debug_reason = DBG_REASON_DBGRQ;

out:
	dap_ap_select(swjdp, saved_apsel);
	return retval;
}

static int cortex_a9_resume(struct target *target, int current,
		uint32_t address, int handle_breakpoints, int debug_execution)
{
	struct armv7a_common *armv7a = target_to_armv7a(target);
	struct arm *armv4_5 = &armv7a->armv4_5_common;
	struct adiv5_dap *swjdp = &armv7a->dap;
	int retval;

//	struct breakpoint *breakpoint = NULL;
	uint32_t resume_pc, dscr;

	uint8_t saved_apsel = dap_ap_get_select(swjdp);
	dap_ap_select(swjdp, swjdp_debugap);

	if (!debug_execution)
		target_free_all_working_areas(target);

#if 0
	if (debug_execution)
	{
		/* Disable interrupts */
		/* We disable interrupts in the PRIMASK register instead of
		 * masking with C_MASKINTS,
		 * This is probably the same issue as Cortex-M3 Errata 377493:
		 * C_MASKINTS in parallel with disabled interrupts can cause
		 * local faults to not be taken. */
		buf_set_u32(armv7m->core_cache->reg_list[ARMV7M_PRIMASK].value, 0, 32, 1);
		armv7m->core_cache->reg_list[ARMV7M_PRIMASK].dirty = 1;
		armv7m->core_cache->reg_list[ARMV7M_PRIMASK].valid = 1;

		/* Make sure we are in Thumb mode */
		buf_set_u32(armv7m->core_cache->reg_list[ARMV7M_xPSR].value, 0, 32,
			buf_get_u32(armv7m->core_cache->reg_list[ARMV7M_xPSR].value, 0, 32) | (1 << 24));
		armv7m->core_cache->reg_list[ARMV7M_xPSR].dirty = 1;
		armv7m->core_cache->reg_list[ARMV7M_xPSR].valid = 1;
	}
#endif

	/* current = 1: continue on current pc, otherwise continue at <address> */
	resume_pc = buf_get_u32(armv4_5->pc->value, 0, 32);
	if (!current)
		resume_pc = address;

	/* Make sure that the Armv7 gdb thumb fixups does not
	 * kill the return address
	 */
	switch (armv4_5->core_state)
	{
	case ARM_STATE_ARM:
		resume_pc &= 0xFFFFFFFC;
		break;
	case ARM_STATE_THUMB:
	case ARM_STATE_THUMB_EE:
		/* When the return address is loaded into PC
		 * bit 0 must be 1 to stay in Thumb state
		 */
		resume_pc |= 0x1;
		break;
	case ARM_STATE_JAZELLE:
		LOG_ERROR("How do I resume into Jazelle state??");
		return ERROR_FAIL;
	}
	LOG_DEBUG("resume pc = 0x%08" PRIx32, resume_pc);
	buf_set_u32(armv4_5->pc->value, 0, 32, resume_pc);
	armv4_5->pc->dirty = 1;
	armv4_5->pc->valid = 1;

	retval = cortex_a9_restore_context(target, handle_breakpoints);
	if (retval != ERROR_OK)
		return retval;

#if 0
	/* the front-end may request us not to handle breakpoints */
	if (handle_breakpoints)
	{
		/* Single step past breakpoint at current address */
		if ((breakpoint = breakpoint_find(target, resume_pc)))
		{
			LOG_DEBUG("unset breakpoint at 0x%8.8x", breakpoint->address);
			cortex_m3_unset_breakpoint(target, breakpoint);
			cortex_m3_single_step_core(target);
			cortex_m3_set_breakpoint(target, breakpoint);
		}
	}

#endif

	/*
	 * Restart core and wait for it to be started.  Clear ITRen and sticky
	 * exception flags: see ARMv7 ARM, C5.9.
	 *
	 * REVISIT: for single stepping, we probably want to
	 * disable IRQs by default, with optional override...
	 */

	retval = mem_ap_read_atomic_u32(swjdp,
			armv7a->debug_base + CPUDBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		return retval;

	if ((dscr & DSCR_INSTR_COMP) == 0)
		LOG_ERROR("DSCR InstrCompl must be set before leaving debug!");

	retval = mem_ap_write_atomic_u32(swjdp,
		armv7a->debug_base + CPUDBG_DSCR, dscr & ~DSCR_ITR_EN);
	if (retval != ERROR_OK)
		return retval;

	retval = mem_ap_write_atomic_u32(swjdp, armv7a->debug_base + CPUDBG_DRCR,
			DRCR_RESTART | DRCR_CLEAR_EXCEPTIONS);
	if (retval != ERROR_OK)
		return retval;

	long long then = timeval_ms();
	for (;;)
	{
		retval = mem_ap_read_atomic_u32(swjdp,
			armv7a->debug_base + CPUDBG_DSCR, &dscr);
		if (retval != ERROR_OK)
			return retval;
		if ((dscr & DSCR_CORE_RESTARTED) != 0)
			break;
		if (timeval_ms() > then + 1000)
		{
			LOG_ERROR("Timeout waiting for resume");
			return ERROR_FAIL;
		}
	}

	target->debug_reason = DBG_REASON_NOTHALTED;
	target->state = TARGET_RUNNING;

	/* registers are now invalid */
	register_cache_invalidate(armv4_5->core_cache);

	if (!debug_execution)
	{
		target->state = TARGET_RUNNING;
		target_call_event_callbacks(target, TARGET_EVENT_RESUMED);
		LOG_DEBUG("target resumed at 0x%" PRIx32, resume_pc);
	}
	else
	{
		target->state = TARGET_DEBUG_RUNNING;
		target_call_event_callbacks(target, TARGET_EVENT_DEBUG_RESUMED);
		LOG_DEBUG("target debug resumed at 0x%" PRIx32, resume_pc);
	}

	dap_ap_select(swjdp, saved_apsel);

	return ERROR_OK;
}

static int cortex_a9_debug_entry(struct target *target)
{
	int i;
	uint32_t regfile[16], cpsr, dscr;
	int retval = ERROR_OK;
	struct working_area *regfile_working_area = NULL;
	struct cortex_a9_common *cortex_a9 = target_to_cortex_a9(target);
	struct armv7a_common *armv7a = target_to_armv7a(target);
	struct arm *armv4_5 = &armv7a->armv4_5_common;
	struct adiv5_dap *swjdp = &armv7a->dap;
	struct reg *reg;

	LOG_DEBUG("dscr = 0x%08" PRIx32, cortex_a9->cpudbg_dscr);

	/* REVISIT surely we should not re-read DSCR !! */
	retval = mem_ap_read_atomic_u32(swjdp,
				armv7a->debug_base + CPUDBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		return retval;

	/* REVISIT see A9 TRM 12.11.4 steps 2..3 -- make sure that any
	 * imprecise data aborts get discarded by issuing a Data
	 * Synchronization Barrier:  ARMV4_5_MCR(15, 0, 0, 7, 10, 4).
	 */

	/* Enable the ITR execution once we are in debug mode */
	dscr |= DSCR_ITR_EN;
	retval = mem_ap_write_atomic_u32(swjdp,
			armv7a->debug_base + CPUDBG_DSCR, dscr);
	if (retval != ERROR_OK)
		return retval;

	/* Examine debug reason */
	arm_dpm_report_dscr(&armv7a->dpm, cortex_a9->cpudbg_dscr);

	/* save address of instruction that triggered the watchpoint? */
	if (target->debug_reason == DBG_REASON_WATCHPOINT) {
		uint32_t wfar;

		retval = mem_ap_read_atomic_u32(swjdp,
				armv7a->debug_base + CPUDBG_WFAR,
				&wfar);
		if (retval != ERROR_OK)
			return retval;
		arm_dpm_report_wfar(&armv7a->dpm, wfar);
	}

	/* REVISIT fast_reg_read is never set ... */

	/* Examine target state and mode */
	if (cortex_a9->fast_reg_read)
		target_alloc_working_area(target, 64, &regfile_working_area);

	/* First load register acessible through core debug port*/
	if (!regfile_working_area)
	{
		retval = arm_dpm_read_current_registers(&armv7a->dpm);
	}
	else
	{
		dap_ap_select(swjdp, swjdp_memoryap);
		retval = cortex_a9_read_regs_through_mem(target,
				regfile_working_area->address, regfile);
		dap_ap_select(swjdp, swjdp_memoryap);
		target_free_working_area(target, regfile_working_area);
		if (retval != ERROR_OK)
		{
			return retval;
		}

		/* read Current PSR */
		retval = cortex_a9_dap_read_coreregister_u32(target, &cpsr, 16);
		if (retval != ERROR_OK)
			return retval;
		dap_ap_select(swjdp, swjdp_debugap);
		LOG_DEBUG("cpsr: %8.8" PRIx32, cpsr);

		arm_set_cpsr(armv4_5, cpsr);

		/* update cache */
		for (i = 0; i <= ARM_PC; i++)
		{
			reg = arm_reg_current(armv4_5, i);

			buf_set_u32(reg->value, 0, 32, regfile[i]);
			reg->valid = 1;
			reg->dirty = 0;
		}

		/* Fixup PC Resume Address */
		if (cpsr & (1 << 5))
		{
			// T bit set for Thumb or ThumbEE state
			regfile[ARM_PC] -= 4;
		}
		else
		{
			// ARM state
			regfile[ARM_PC] -= 8;
		}

		reg = armv4_5->pc;
		buf_set_u32(reg->value, 0, 32, regfile[ARM_PC]);
		reg->dirty = reg->valid;
	}

#if 0
/* TODO, Move this */
	uint32_t cp15_control_register, cp15_cacr, cp15_nacr;
	cortex_a9_read_cp(target, &cp15_control_register, 15, 0, 1, 0, 0);
	LOG_DEBUG("cp15_control_register = 0x%08x", cp15_control_register);

	cortex_a9_read_cp(target, &cp15_cacr, 15, 0, 1, 0, 2);
	LOG_DEBUG("cp15 Coprocessor Access Control Register = 0x%08x", cp15_cacr);

	cortex_a9_read_cp(target, &cp15_nacr, 15, 0, 1, 1, 2);
	LOG_DEBUG("cp15 Nonsecure Access Control Register = 0x%08x", cp15_nacr);
#endif

	/* Are we in an exception handler */
//	armv4_5->exception_number = 0;
	if (armv7a->post_debug_entry)
	{
		retval = armv7a->post_debug_entry(target);
		if (retval != ERROR_OK)
			return retval;
	}

	return retval;
}

static int cortex_a9_post_debug_entry(struct target *target)
{
	struct cortex_a9_common *cortex_a9 = target_to_cortex_a9(target);
	struct armv7a_common *armv7a = &cortex_a9->armv7a_common;
	int retval;

	/* MRC p15,0,<Rt>,c1,c0,0 ; Read CP15 System Control Register */
	retval = armv7a->armv4_5_common.mrc(target, 15,
			0, 0,	/* op1, op2 */
			1, 0,	/* CRn, CRm */
			&cortex_a9->cp15_control_reg);
	if (retval != ERROR_OK)
		return retval;
	LOG_DEBUG("cp15_control_reg: %8.8" PRIx32, cortex_a9->cp15_control_reg);

	if (armv7a->armv4_5_mmu.armv4_5_cache.ctype == -1)
	{
		uint32_t cache_type_reg;

		/* MRC p15,0,<Rt>,c0,c0,1 ; Read CP15 Cache Type Register */
		retval = armv7a->armv4_5_common.mrc(target, 15,
				0, 1,	/* op1, op2 */
				0, 0,	/* CRn, CRm */
				&cache_type_reg);
		if (retval != ERROR_OK)
			return retval;
		LOG_DEBUG("cp15 cache type: %8.8x", (unsigned) cache_type_reg);

		/* FIXME the armv4_4 cache info DOES NOT APPLY to Cortex-A9 */
		armv4_5_identify_cache(cache_type_reg,
				&armv7a->armv4_5_mmu.armv4_5_cache);
	}

	armv7a->armv4_5_mmu.mmu_enabled =
			(cortex_a9->cp15_control_reg & 0x1U) ? 1 : 0;
	armv7a->armv4_5_mmu.armv4_5_cache.d_u_cache_enabled =
			(cortex_a9->cp15_control_reg & 0x4U) ? 1 : 0;
	armv7a->armv4_5_mmu.armv4_5_cache.i_cache_enabled =
			(cortex_a9->cp15_control_reg & 0x1000U) ? 1 : 0;

	return ERROR_OK;
}

static int cortex_a9_step(struct target *target, int current, uint32_t address,
		int handle_breakpoints)
{
	struct armv7a_common *armv7a = target_to_armv7a(target);
	struct arm *armv4_5 = &armv7a->armv4_5_common;
	struct breakpoint *breakpoint = NULL;
	struct breakpoint stepbreakpoint;
	struct reg *r;
	int retval;

	if (target->state != TARGET_HALTED)
	{
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* current = 1: continue on current pc, otherwise continue at <address> */
	r = armv4_5->pc;
	if (!current)
	{
		buf_set_u32(r->value, 0, 32, address);
	}
	else
	{
		address = buf_get_u32(r->value, 0, 32);
	}

	/* The front-end may request us not to handle breakpoints.
	 * But since Cortex-A9 uses breakpoint for single step,
	 * we MUST handle breakpoints.
	 */
	handle_breakpoints = 1;
	if (handle_breakpoints) {
		breakpoint = breakpoint_find(target, address);
		if (breakpoint)
			cortex_a9_unset_breakpoint(target, breakpoint);
	}

	/* Setup single step breakpoint */
	stepbreakpoint.address = address;
	stepbreakpoint.length = (armv4_5->core_state == ARM_STATE_THUMB)
			? 2 : 4;
	stepbreakpoint.type = BKPT_HARD;
	stepbreakpoint.set = 0;

	/* Break on IVA mismatch */
	cortex_a9_set_breakpoint(target, &stepbreakpoint, 0x04);

	target->debug_reason = DBG_REASON_SINGLESTEP;

	retval = cortex_a9_resume(target, 1, address, 0, 0);
	if (retval != ERROR_OK)
		return retval;

	long long then = timeval_ms();
	while (target->state != TARGET_HALTED)
	{
		retval = cortex_a9_poll(target);
		if (retval != ERROR_OK)
			return retval;
		if (timeval_ms() > then + 1000)
		{
			LOG_ERROR("timeout waiting for target halt");
			return ERROR_FAIL;
		}
	}

	cortex_a9_unset_breakpoint(target, &stepbreakpoint);

	target->debug_reason = DBG_REASON_BREAKPOINT;

	if (breakpoint)
		cortex_a9_set_breakpoint(target, breakpoint, 0);

	if (target->state != TARGET_HALTED)
		LOG_DEBUG("target stepped");

	return ERROR_OK;
}

static int cortex_a9_restore_context(struct target *target, bool bpwp)
{
	struct armv7a_common *armv7a = target_to_armv7a(target);

	LOG_DEBUG(" ");

	if (armv7a->pre_restore_context)
		armv7a->pre_restore_context(target);

	return arm_dpm_write_dirty_registers(&armv7a->dpm, bpwp);
}


/*
 * Cortex-A9 Breakpoint and watchpoint functions
 */

/* Setup hardware Breakpoint Register Pair */
static int cortex_a9_set_breakpoint(struct target *target,
		struct breakpoint *breakpoint, uint8_t matchmode)
{
	int retval;
	int brp_i=0;
	uint32_t control;
	uint8_t byte_addr_select = 0x0F;
	struct cortex_a9_common *cortex_a9 = target_to_cortex_a9(target);
	struct armv7a_common *armv7a = &cortex_a9->armv7a_common;
	struct cortex_a9_brp * brp_list = cortex_a9->brp_list;

	if (breakpoint->set)
	{
		LOG_WARNING("breakpoint already set");
		return ERROR_OK;
	}

	if (breakpoint->type == BKPT_HARD)
	{
		while (brp_list[brp_i].used && (brp_i < cortex_a9->brp_num))
			brp_i++ ;
		if (brp_i >= cortex_a9->brp_num)
		{
			LOG_ERROR("ERROR Can not find free Breakpoint Register Pair");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
		breakpoint->set = brp_i + 1;
		if (breakpoint->length == 2)
		{
			byte_addr_select = (3 << (breakpoint->address & 0x02));
		}
		control = ((matchmode & 0x7) << 20)
				| (byte_addr_select << 5)
				| (3 << 1) | 1;
		brp_list[brp_i].used = 1;
		brp_list[brp_i].value = (breakpoint->address & 0xFFFFFFFC);
		brp_list[brp_i].control = control;
		retval = cortex_a9_dap_write_memap_register_u32(target, armv7a->debug_base
				+ CPUDBG_BVR_BASE + 4 * brp_list[brp_i].BRPn,
				brp_list[brp_i].value);
		if (retval != ERROR_OK)
			return retval;
		retval = cortex_a9_dap_write_memap_register_u32(target, armv7a->debug_base
				+ CPUDBG_BCR_BASE + 4 * brp_list[brp_i].BRPn,
				brp_list[brp_i].control);
		if (retval != ERROR_OK)
			return retval;
		LOG_DEBUG("brp %i control 0x%0" PRIx32 " value 0x%0" PRIx32, brp_i,
				brp_list[brp_i].control,
				brp_list[brp_i].value);
	}
	else if (breakpoint->type == BKPT_SOFT)
	{
		uint8_t code[4];
		if (breakpoint->length == 2)
		{
			buf_set_u32(code, 0, 32, ARMV5_T_BKPT(0x11));
		}
		else
		{
			buf_set_u32(code, 0, 32, ARMV5_BKPT(0x11));
		}
		retval = target->type->read_memory(target,
				breakpoint->address & 0xFFFFFFFE,
				breakpoint->length, 1,
				breakpoint->orig_instr);
		if (retval != ERROR_OK)
			return retval;
		retval = target->type->write_memory(target,
				breakpoint->address & 0xFFFFFFFE,
				breakpoint->length, 1, code);
		if (retval != ERROR_OK)
			return retval;
		breakpoint->set = 0x11; /* Any nice value but 0 */
	}

	return ERROR_OK;
}

static int cortex_a9_unset_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	int retval;
	struct cortex_a9_common *cortex_a9 = target_to_cortex_a9(target);
	struct armv7a_common *armv7a = &cortex_a9->armv7a_common;
	struct cortex_a9_brp * brp_list = cortex_a9->brp_list;

	if (!breakpoint->set)
	{
		LOG_WARNING("breakpoint not set");
		return ERROR_OK;
	}

	if (breakpoint->type == BKPT_HARD)
	{
		int brp_i = breakpoint->set - 1;
		if ((brp_i < 0) || (brp_i >= cortex_a9->brp_num))
		{
			LOG_DEBUG("Invalid BRP number in breakpoint");
			return ERROR_OK;
		}
		LOG_DEBUG("rbp %i control 0x%0" PRIx32 " value 0x%0" PRIx32, brp_i,
				brp_list[brp_i].control, brp_list[brp_i].value);
		brp_list[brp_i].used = 0;
		brp_list[brp_i].value = 0;
		brp_list[brp_i].control = 0;
		retval = cortex_a9_dap_write_memap_register_u32(target, armv7a->debug_base
				+ CPUDBG_BCR_BASE + 4 * brp_list[brp_i].BRPn,
				brp_list[brp_i].control);
		if (retval != ERROR_OK)
			return retval;
		retval = cortex_a9_dap_write_memap_register_u32(target, armv7a->debug_base
				+ CPUDBG_BVR_BASE + 4 * brp_list[brp_i].BRPn,
				brp_list[brp_i].value);
		if (retval != ERROR_OK)
			return retval;
	}
	else
	{
		/* restore original instruction (kept in target endianness) */
		if (breakpoint->length == 4)
		{
			retval = target->type->write_memory(target,
					breakpoint->address & 0xFFFFFFFE,
					4, 1, breakpoint->orig_instr);
			if (retval != ERROR_OK)
				return retval;
		}
		else
		{
			retval = target->type->write_memory(target,
					breakpoint->address & 0xFFFFFFFE,
					2, 1, breakpoint->orig_instr);
			if (retval != ERROR_OK)
				return retval;
		}
	}
	breakpoint->set = 0;

	return ERROR_OK;
}

static int cortex_a9_add_breakpoint(struct target *target,
		struct breakpoint *breakpoint)
{
	struct cortex_a9_common *cortex_a9 = target_to_cortex_a9(target);

	if ((breakpoint->type == BKPT_HARD) && (cortex_a9->brp_num_available < 1))
	{
		LOG_INFO("no hardware breakpoint available");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	if (breakpoint->type == BKPT_HARD)
		cortex_a9->brp_num_available--;

	return cortex_a9_set_breakpoint(target, breakpoint, 0x00); /* Exact match */
}

static int cortex_a9_remove_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	struct cortex_a9_common *cortex_a9 = target_to_cortex_a9(target);

#if 0
/* It is perfectly possible to remove breakpoints while the target is running */
	if (target->state != TARGET_HALTED)
	{
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}
#endif

	if (breakpoint->set)
	{
		cortex_a9_unset_breakpoint(target, breakpoint);
		if (breakpoint->type == BKPT_HARD)
			cortex_a9->brp_num_available++ ;
	}


	return ERROR_OK;
}



/*
 * Cortex-A9 Reset functions
 */

static int cortex_a9_assert_reset(struct target *target)
{
	struct armv7a_common *armv7a = target_to_armv7a(target);

	LOG_DEBUG(" ");

	/* FIXME when halt is requested, make it work somehow... */

	/* Issue some kind of warm reset. */
	if (target_has_event_action(target, TARGET_EVENT_RESET_ASSERT)) {
		target_handle_event(target, TARGET_EVENT_RESET_ASSERT);
	} else if (jtag_get_reset_config() & RESET_HAS_SRST) {
		/* REVISIT handle "pulls" cases, if there's
		 * hardware that needs them to work.
		 */
		jtag_add_reset(0, 1);
	} else {
		LOG_ERROR("%s: how to reset?", target_name(target));
		return ERROR_FAIL;
	}

	/* registers are now invalid */
	register_cache_invalidate(armv7a->armv4_5_common.core_cache);

	target->state = TARGET_RESET;

	return ERROR_OK;
}

static int cortex_a9_deassert_reset(struct target *target)
{
	int retval;

	LOG_DEBUG(" ");

	/* be certain SRST is off */
	jtag_add_reset(0, 0);

	retval = cortex_a9_poll(target);
	if (retval != ERROR_OK)
		return retval;

	if (target->reset_halt) {
		if (target->state != TARGET_HALTED) {
			LOG_WARNING("%s: ran after reset and before halt ...",
					target_name(target));
			if ((retval = target_halt(target)) != ERROR_OK)
				return retval;
		}
	}

	return ERROR_OK;
}

/*
 * Cortex-A9 Memory access
 *
 * This is same Cortex M3 but we must also use the correct
 * ap number for every access.
 */

static int cortex_a9_read_phys_memory(struct target *target,
		uint32_t address, uint32_t size,
		uint32_t count, uint8_t *buffer)
{
	struct armv7a_common *armv7a = target_to_armv7a(target);
	struct adiv5_dap *swjdp = &armv7a->dap;
	int retval = ERROR_INVALID_ARGUMENTS;
	uint8_t saved_apsel = dap_ap_get_select(swjdp);

	/* cortex_a9 handles unaligned memory access */

	dap_ap_select(swjdp, swjdp_memoryap);

	LOG_DEBUG("Reading memory at real address 0x%x; size %d; count %d", address, size, count);
	if (count && buffer) {
		switch (size) {
			case 4:
				retval = mem_ap_read_buf_u32(swjdp, buffer, 4 * count, address);
				break;
			case 2:
				retval = mem_ap_read_buf_u16(swjdp, buffer, 2 * count, address);
				break;
			case 1:
				retval = mem_ap_read_buf_u8(swjdp, buffer, count, address);
				break;
		}
	}

	dap_ap_select(swjdp, saved_apsel);

	return retval;
}

static int cortex_a9_read_memory(struct target *target, uint32_t address,
		uint32_t size, uint32_t count, uint8_t *buffer)
{
	int enabled = 0;
	uint32_t virt, phys;
	int retval;

	/* cortex_a9 handles unaligned memory access */

	LOG_DEBUG("Reading memory at address 0x%x; size %d; count %d", address, size, count);
	retval = cortex_a9_mmu(target, &enabled);
	if (retval != ERROR_OK)
		return retval;

	if (enabled)
	{
		virt = address;
		retval = cortex_a9_virt2phys(target, virt, &phys);
		if (retval != ERROR_OK)
			return retval;

		LOG_DEBUG("Reading at virtual address. Translating v:0x%x to r:0x%x", virt, phys);
		address = phys;
	}

	return cortex_a9_read_phys_memory(target, address, size, count, buffer);
}

static int cortex_a9_write_phys_memory(struct target *target,
		uint32_t address, uint32_t size,
		uint32_t count, uint8_t *buffer)
{
	struct armv7a_common *armv7a = target_to_armv7a(target);
	struct adiv5_dap *swjdp = &armv7a->dap;
	int retval = ERROR_INVALID_ARGUMENTS;

	LOG_DEBUG("Writing memory to real address 0x%x; size %d; count %d", address, size, count);

	if (count && buffer) {
		uint8_t saved_apsel = dap_ap_get_select(swjdp);
		dap_ap_select(swjdp, swjdp_memoryap);

		switch (size) {
			case 4:
				retval = mem_ap_write_buf_u32(swjdp, buffer, 4 * count, address);
				break;
			case 2:
				retval = mem_ap_write_buf_u16(swjdp, buffer, 2 * count, address);
				break;
			case 1:
				retval = mem_ap_write_buf_u8(swjdp, buffer, count, address);
				break;
		}

		dap_ap_select(swjdp, saved_apsel);
	}


	/* REVISIT this op is generic ARMv7-A/R stuff */
	if (retval == ERROR_OK && target->state == TARGET_HALTED)
	{
		struct arm_dpm *dpm = armv7a->armv4_5_common.dpm;

		retval = dpm->prepare(dpm);
		if (retval != ERROR_OK)
			return retval;

		/* The Cache handling will NOT work with MMU active, the
		 * wrong addresses will be invalidated!
		 *
		 * For both ICache and DCache, walk all cache lines in the
		 * address range. Cortex-A9 has fixed 64 byte line length.
		 *
		 * REVISIT per ARMv7, these may trigger watchpoints ...
		 */

		/* invalidate I-Cache */
		if (armv7a->armv4_5_mmu.armv4_5_cache.i_cache_enabled)
		{
			/* ICIMVAU - Invalidate Cache single entry
			 * with MVA to PoU
			 *      MCR p15, 0, r0, c7, c5, 1
			 */
			for (uint32_t cacheline = address;
					cacheline < address + size * count;
					cacheline += 64) {
				retval = dpm->instr_write_data_r0(dpm,
						ARMV4_5_MCR(15, 0, 0, 7, 5, 1),
						cacheline);
				if (retval != ERROR_OK)
					return retval;
			}
		}

		/* invalidate D-Cache */
		if (armv7a->armv4_5_mmu.armv4_5_cache.d_u_cache_enabled)
		{
			/* DCIMVAC - Invalidate data Cache line
			 * with MVA to PoC
			 *      MCR p15, 0, r0, c7, c6, 1
			 */
			for (uint32_t cacheline = address;
					cacheline < address + size * count;
					cacheline += 64) {
				retval = dpm->instr_write_data_r0(dpm,
						ARMV4_5_MCR(15, 0, 0, 7, 6, 1),
						cacheline);
				if (retval != ERROR_OK)
					return retval;
			}
		}

		/* (void) */ dpm->finish(dpm);
	}

	return retval;
}

static int cortex_a9_write_memory(struct target *target, uint32_t address,
                uint32_t size, uint32_t count, uint8_t *buffer)
{
	int enabled = 0;
	uint32_t virt, phys;
	int retval;

	LOG_DEBUG("Writing memory to address 0x%x; size %d; count %d", address, size, count);
	retval = cortex_a9_mmu(target, &enabled);
	if (retval != ERROR_OK)
		return retval;

	if (enabled)
	{
		virt = address;
		retval = cortex_a9_virt2phys(target, virt, &phys);
		if (retval != ERROR_OK)
			return retval;
		LOG_DEBUG("Writing to virtual address. Translating v:0x%x to r:0x%x", virt, phys);
		address = phys;
	}

	return cortex_a9_write_phys_memory(target, address, size,
			count, buffer);
}

static int cortex_a9_bulk_write_memory(struct target *target, uint32_t address,
		uint32_t count, uint8_t *buffer)
{
	return cortex_a9_write_memory(target, address, 4, count, buffer);
}

static int cortex_a9_dcc_read(struct adiv5_dap *swjdp, uint8_t *value, uint8_t *ctrl)
{
#if 0
	u16 dcrdr;

	mem_ap_read_buf_u16(swjdp, (uint8_t*)&dcrdr, 1, DCB_DCRDR);
	*ctrl = (uint8_t)dcrdr;
	*value = (uint8_t)(dcrdr >> 8);

	LOG_DEBUG("data 0x%x ctrl 0x%x", *value, *ctrl);

	/* write ack back to software dcc register
	 * signify we have read data */
	if (dcrdr & (1 << 0))
	{
		dcrdr = 0;
		mem_ap_write_buf_u16(swjdp, (uint8_t*)&dcrdr, 1, DCB_DCRDR);
	}
#endif
	return ERROR_OK;
}


static int cortex_a9_handle_target_request(void *priv)
{
	struct target *target = priv;
	struct armv7a_common *armv7a = target_to_armv7a(target);
	struct adiv5_dap *swjdp = &armv7a->dap;
	int retval;

	if (!target_was_examined(target))
		return ERROR_OK;
	if (!target->dbg_msg_enabled)
		return ERROR_OK;

	if (target->state == TARGET_RUNNING)
	{
		uint8_t data = 0;
		uint8_t ctrl = 0;

		retval = cortex_a9_dcc_read(swjdp, &data, &ctrl);
		if (retval != ERROR_OK)
			return retval;

		/* check if we have data */
		if (ctrl & (1 << 0))
		{
			uint32_t request;

			/* we assume target is quick enough */
			request = data;
			retval = cortex_a9_dcc_read(swjdp, &data, &ctrl);
			if (retval != ERROR_OK)
				return retval;
			request |= (data << 8);
			retval = cortex_a9_dcc_read(swjdp, &data, &ctrl);
			if (retval != ERROR_OK)
				return retval;
			request |= (data << 16);
			retval = cortex_a9_dcc_read(swjdp, &data, &ctrl);
			if (retval != ERROR_OK)
				return retval;
			request |= (data << 24);
			target_request(target, request);
		}
	}

	return ERROR_OK;
}

/*
 * Cortex-A9 target information and configuration
 */

static int cortex_a9_examine_first(struct target *target)
{
	struct cortex_a9_common *cortex_a9 = target_to_cortex_a9(target);
	struct armv7a_common *armv7a = &cortex_a9->armv7a_common;
	struct adiv5_dap *swjdp = &armv7a->dap;
	int i;
	int retval = ERROR_OK;
	uint32_t didr, ctypr, ttypr, cpuid;

	/* We do one extra read to ensure DAP is configured,
	 * we call ahbap_debugport_init(swjdp) instead
	 */
	retval = ahbap_debugport_init(swjdp);
	if (retval != ERROR_OK)
		return retval;

	dap_ap_select(swjdp, swjdp_debugap);

	/*
	 * FIXME: assuming omap4430
	 *
	 * APB DBGBASE reads 0x80040000, but this points to an empty ROM table.
	 * 0x80000000 is cpu0 coresight region
	 */
	if (target->coreid > 3) {
		LOG_ERROR("cortex_a9 supports up to 4 cores");
		return ERROR_INVALID_ARGUMENTS;
	}
	armv7a->debug_base = 0x80000000 |
			((target->coreid & 0x3) << CORTEX_A9_PADDRDBG_CPU_SHIFT);

	retval = mem_ap_read_atomic_u32(swjdp,
			armv7a->debug_base + CPUDBG_CPUID, &cpuid);
	if (retval != ERROR_OK)
		return retval;

	if ((retval = mem_ap_read_atomic_u32(swjdp,
			armv7a->debug_base + CPUDBG_CPUID, &cpuid)) != ERROR_OK)
	{
		LOG_DEBUG("Examine %s failed", "CPUID");
		return retval;
	}

	if ((retval = mem_ap_read_atomic_u32(swjdp,
			armv7a->debug_base + CPUDBG_CTYPR, &ctypr)) != ERROR_OK)
	{
		LOG_DEBUG("Examine %s failed", "CTYPR");
		return retval;
	}

	if ((retval = mem_ap_read_atomic_u32(swjdp,
			armv7a->debug_base + CPUDBG_TTYPR, &ttypr)) != ERROR_OK)
	{
		LOG_DEBUG("Examine %s failed", "TTYPR");
		return retval;
	}

	if ((retval = mem_ap_read_atomic_u32(swjdp,
			armv7a->debug_base + CPUDBG_DIDR, &didr)) != ERROR_OK)
	{
		LOG_DEBUG("Examine %s failed", "DIDR");
		return retval;
	}

	LOG_DEBUG("cpuid = 0x%08" PRIx32, cpuid);
	LOG_DEBUG("ctypr = 0x%08" PRIx32, ctypr);
	LOG_DEBUG("ttypr = 0x%08" PRIx32, ttypr);
	LOG_DEBUG("didr = 0x%08" PRIx32, didr);

	armv7a->armv4_5_common.core_type = ARM_MODE_MON;
	retval = cortex_a9_dpm_setup(cortex_a9, didr);
	if (retval != ERROR_OK)
		return retval;

	/* Setup Breakpoint Register Pairs */
	cortex_a9->brp_num = ((didr >> 24) & 0x0F) + 1;
	cortex_a9->brp_num_context = ((didr >> 20) & 0x0F) + 1;
	cortex_a9->brp_num_available = cortex_a9->brp_num;
	cortex_a9->brp_list = calloc(cortex_a9->brp_num, sizeof(struct cortex_a9_brp));
//	cortex_a9->brb_enabled = ????;
	for (i = 0; i < cortex_a9->brp_num; i++)
	{
		cortex_a9->brp_list[i].used = 0;
		if (i < (cortex_a9->brp_num-cortex_a9->brp_num_context))
			cortex_a9->brp_list[i].type = BRP_NORMAL;
		else
			cortex_a9->brp_list[i].type = BRP_CONTEXT;
		cortex_a9->brp_list[i].value = 0;
		cortex_a9->brp_list[i].control = 0;
		cortex_a9->brp_list[i].BRPn = i;
	}

	LOG_DEBUG("Configured %i hw breakpoints", cortex_a9->brp_num);

	target_set_examined(target);
	return ERROR_OK;
}

static int cortex_a9_examine(struct target *target)
{
	int retval = ERROR_OK;

	/* don't re-probe hardware after each reset */
	if (!target_was_examined(target))
		retval = cortex_a9_examine_first(target);

	/* Configure core debug access */
	if (retval == ERROR_OK)
		retval = cortex_a9_init_debug_access(target);

	return retval;
}

/*
 *	Cortex-A9 target creation and initialization
 */

static int cortex_a9_init_target(struct command_context *cmd_ctx,
		struct target *target)
{
	/* examine_first() does a bunch of this */
	return ERROR_OK;
}

static int cortex_a9_init_arch_info(struct target *target,
		struct cortex_a9_common *cortex_a9, struct jtag_tap *tap)
{
	struct armv7a_common *armv7a = &cortex_a9->armv7a_common;
	struct arm *armv4_5 = &armv7a->armv4_5_common;
	struct adiv5_dap *dap = &armv7a->dap;

	armv7a->armv4_5_common.dap = dap;

	/* Setup struct cortex_a9_common */
	cortex_a9->common_magic = CORTEX_A9_COMMON_MAGIC;
	armv4_5->arch_info = armv7a;

	/* prepare JTAG information for the new target */
	cortex_a9->jtag_info.tap = tap;
	cortex_a9->jtag_info.scann_size = 4;

	/* Leave (only) generic DAP stuff for debugport_init() */
	dap->jtag_info = &cortex_a9->jtag_info;
	dap->memaccess_tck = 80;

	/* Number of bits for tar autoincrement, impl. dep. at least 10 */
	dap->tar_autoincr_block = (1 << 10);

	cortex_a9->fast_reg_read = 0;

	/* Set default value */
	cortex_a9->current_address_mode = ARM_MODE_ANY;

	/* register arch-specific functions */
	armv7a->examine_debug_reason = NULL;

	armv7a->post_debug_entry = cortex_a9_post_debug_entry;

	armv7a->pre_restore_context = NULL;
	armv7a->armv4_5_mmu.armv4_5_cache.ctype = -1;
	armv7a->armv4_5_mmu.get_ttb = cortex_a9_get_ttb;
	armv7a->armv4_5_mmu.read_memory = cortex_a9_read_phys_memory;
	armv7a->armv4_5_mmu.write_memory = cortex_a9_write_phys_memory;
	armv7a->armv4_5_mmu.disable_mmu_caches = cortex_a9_disable_mmu_caches;
	armv7a->armv4_5_mmu.enable_mmu_caches = cortex_a9_enable_mmu_caches;
	armv7a->armv4_5_mmu.has_tiny_pages = 1;
	armv7a->armv4_5_mmu.mmu_enabled = 0;


//	arm7_9->handle_target_request = cortex_a9_handle_target_request;

	/* REVISIT v7a setup should be in a v7a-specific routine */
	arm_init_arch_info(target, armv4_5);
	armv7a->common_magic = ARMV7_COMMON_MAGIC;

	target_register_timer_callback(cortex_a9_handle_target_request, 1, 1, target);

	return ERROR_OK;
}

static int cortex_a9_target_create(struct target *target, Jim_Interp *interp)
{
	struct cortex_a9_common *cortex_a9 = calloc(1, sizeof(struct cortex_a9_common));

	return cortex_a9_init_arch_info(target, cortex_a9, target->tap);
}

static int cortex_a9_get_ttb(struct target *target, uint32_t *result)
{
	struct cortex_a9_common *cortex_a9 = target_to_cortex_a9(target);
    struct armv7a_common *armv7a = &cortex_a9->armv7a_common;
    uint32_t ttb = 0, retval = ERROR_OK;

    /* current_address_mode is set inside cortex_a9_virt2phys()
       where we can determine if address belongs to user or kernel */
    if(cortex_a9->current_address_mode == ARM_MODE_SVC)
    {
        /* MRC p15,0,<Rt>,c1,c0,0 ; Read CP15 System Control Register */
        retval = armv7a->armv4_5_common.mrc(target, 15,
                    0, 1,   /* op1, op2 */
                    2, 0,   /* CRn, CRm */
                    &ttb);
		if (retval != ERROR_OK)
			return retval;
    }
    else if(cortex_a9->current_address_mode == ARM_MODE_USR)
    {
        /* MRC p15,0,<Rt>,c1,c0,0 ; Read CP15 System Control Register */
        retval = armv7a->armv4_5_common.mrc(target, 15,
                    0, 0,   /* op1, op2 */
                    2, 0,   /* CRn, CRm */
                    &ttb);
		if (retval != ERROR_OK)
			return retval;
    }
    /* we don't know whose address is: user or kernel
       we assume that if we are in kernel mode then
       address belongs to kernel else if in user mode
       - to user */
    else if(armv7a->armv4_5_common.core_mode == ARM_MODE_SVC)
    {
        /* MRC p15,0,<Rt>,c1,c0,0 ; Read CP15 System Control Register */
        retval = armv7a->armv4_5_common.mrc(target, 15,
                    0, 1,   /* op1, op2 */
                    2, 0,   /* CRn, CRm */
                    &ttb);
		if (retval != ERROR_OK)
			return retval;
    }
    else if(armv7a->armv4_5_common.core_mode == ARM_MODE_USR)
    {
        /* MRC p15,0,<Rt>,c1,c0,0 ; Read CP15 System Control Register */
        retval = armv7a->armv4_5_common.mrc(target, 15,
                    0, 0,   /* op1, op2 */
                    2, 0,   /* CRn, CRm */
                    &ttb);
		if (retval != ERROR_OK)
			return retval;
    }
    /* finally we don't know whose ttb to use: user or kernel */
    else
        LOG_ERROR("Don't know how to get ttb for current mode!!!");

    ttb &= 0xffffc000;

    *result = ttb;

    return ERROR_OK;
}

static int cortex_a9_disable_mmu_caches(struct target *target, int mmu,
                int d_u_cache, int i_cache)
{
	struct cortex_a9_common *cortex_a9 = target_to_cortex_a9(target);
	struct armv7a_common *armv7a = &cortex_a9->armv7a_common;
	uint32_t cp15_control;
	int retval;

	/* read cp15 control register */
	retval = armv7a->armv4_5_common.mrc(target, 15,
			0, 0,   /* op1, op2 */
			1, 0,   /* CRn, CRm */
			&cp15_control);
	if (retval != ERROR_OK)
		return retval;


	if (mmu)
		cp15_control &= ~0x1U;

	if (d_u_cache)
		cp15_control &= ~0x4U;

	if (i_cache)
		cp15_control &= ~0x1000U;

	retval = armv7a->armv4_5_common.mcr(target, 15,
			0, 0,   /* op1, op2 */
			1, 0,   /* CRn, CRm */
			cp15_control);
	return retval;
}

static int cortex_a9_enable_mmu_caches(struct target *target, int mmu,
		int d_u_cache, int i_cache)
{
	struct cortex_a9_common *cortex_a9 = target_to_cortex_a9(target);
	struct armv7a_common *armv7a = &cortex_a9->armv7a_common;
	uint32_t cp15_control;
	int retval;

	/* read cp15 control register */
	retval = armv7a->armv4_5_common.mrc(target, 15,
			0, 0,   /* op1, op2 */
			1, 0,   /* CRn, CRm */
			&cp15_control);
	if (retval != ERROR_OK)
		return retval;

	if (mmu)
		cp15_control |= 0x1U;

	if (d_u_cache)
		cp15_control |= 0x4U;

	if (i_cache)
		cp15_control |= 0x1000U;

	retval = armv7a->armv4_5_common.mcr(target, 15,
			0, 0,   /* op1, op2 */
			1, 0,   /* CRn, CRm */
			cp15_control);
	return retval;
}


static int cortex_a9_mmu(struct target *target, int *enabled)
{
	if (target->state != TARGET_HALTED) {
		LOG_ERROR("%s: target not halted", __func__);
		return ERROR_TARGET_INVALID;
	}

	*enabled = target_to_cortex_a9(target)->armv7a_common.armv4_5_mmu.mmu_enabled;
	return ERROR_OK;
}

static int cortex_a9_virt2phys(struct target *target,
		uint32_t virt, uint32_t *phys)
{
	uint32_t cb;
	struct cortex_a9_common *cortex_a9 = target_to_cortex_a9(target);
	// struct armv7a_common *armv7a = &cortex_a9->armv7a_common;
	struct armv7a_common *armv7a = target_to_armv7a(target);

    /* We assume that virtual address is separated
       between user and kernel in Linux style:
       0x00000000-0xbfffffff - User space
       0xc0000000-0xffffffff - Kernel space */
    if( virt < 0xc0000000 ) /* Linux user space */
        cortex_a9->current_address_mode = ARM_MODE_USR;
    else /* Linux kernel */
        cortex_a9->current_address_mode = ARM_MODE_SVC;
	uint32_t ret;
	int retval = armv4_5_mmu_translate_va(target,
			&armv7a->armv4_5_mmu, virt, &cb, &ret);
	if (retval != ERROR_OK)
		return retval;
    /* Reset the flag. We don't want someone else to use it by error */
    cortex_a9->current_address_mode = ARM_MODE_ANY;

	*phys = ret;
	return ERROR_OK;
}

COMMAND_HANDLER(cortex_a9_handle_cache_info_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct armv7a_common *armv7a = target_to_armv7a(target);

	return armv4_5_handle_cache_info_command(CMD_CTX,
			&armv7a->armv4_5_mmu.armv4_5_cache);
}


COMMAND_HANDLER(cortex_a9_handle_dbginit_command)
{
	struct target *target = get_current_target(CMD_CTX);
	if (!target_was_examined(target))
	{
		LOG_ERROR("target not examined yet");
		return ERROR_FAIL;
	}

	return cortex_a9_init_debug_access(target);
}

static const struct command_registration cortex_a9_exec_command_handlers[] = {
	{
		.name = "cache_info",
		.handler = cortex_a9_handle_cache_info_command,
		.mode = COMMAND_EXEC,
		.help = "display information about target caches",
	},
	{
		.name = "dbginit",
		.handler = cortex_a9_handle_dbginit_command,
		.mode = COMMAND_EXEC,
		.help = "Initialize core debug",
	},
	COMMAND_REGISTRATION_DONE
};
static const struct command_registration cortex_a9_command_handlers[] = {
	{
		.chain = arm_command_handlers,
	},
	{
		.chain = armv7a_command_handlers,
	},
	{
		.name = "cortex_a9",
		.mode = COMMAND_ANY,
		.help = "Cortex-A9 command group",
		.chain = cortex_a9_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct target_type cortexa9_target = {
	.name = "cortex_a9",

	.poll = cortex_a9_poll,
	.arch_state = armv7a_arch_state,

	.target_request_data = NULL,

	.halt = cortex_a9_halt,
	.resume = cortex_a9_resume,
	.step = cortex_a9_step,

	.assert_reset = cortex_a9_assert_reset,
	.deassert_reset = cortex_a9_deassert_reset,
	.soft_reset_halt = NULL,

	/* REVISIT allow exporting VFP3 registers ... */
	.get_gdb_reg_list = arm_get_gdb_reg_list,

	.read_memory = cortex_a9_read_memory,
	.write_memory = cortex_a9_write_memory,
	.bulk_write_memory = cortex_a9_bulk_write_memory,

	.checksum_memory = arm_checksum_memory,
	.blank_check_memory = arm_blank_check_memory,

	.run_algorithm = armv4_5_run_algorithm,

	.add_breakpoint = cortex_a9_add_breakpoint,
	.remove_breakpoint = cortex_a9_remove_breakpoint,
	.add_watchpoint = NULL,
	.remove_watchpoint = NULL,

	.commands = cortex_a9_command_handlers,
	.target_create = cortex_a9_target_create,
	.init_target = cortex_a9_init_target,
	.examine = cortex_a9_examine,

	.read_phys_memory = cortex_a9_read_phys_memory,
	.write_phys_memory = cortex_a9_write_phys_memory,
	.mmu = cortex_a9_mmu,
	.virt2phys = cortex_a9_virt2phys,
};
