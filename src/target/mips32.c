/***************************************************************************
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2008 by David T.L. Wong                                 *
 *                                                                         *
 *   Copyright (C) 2007,2008 Øyvind Harboe                                 *
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
 ***************************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mips32.h"
#include "breakpoints.h"
#include "algorithm.h"
#include "register.h"

static char* mips32_core_reg_list[] =
{
	"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
	"t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
	"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
	"t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
	"status", "lo", "hi", "badvaddr", "cause", "pc"
};

static const char *mips_isa_strings[] =
{
	"MIPS32", "MIPS16e"
};

static struct mips32_core_reg mips32_core_reg_list_arch_info[MIPS32NUMCOREREGS] =
{
	{0, NULL, NULL},
	{1, NULL, NULL},
	{2, NULL, NULL},
	{3, NULL, NULL},
	{4, NULL, NULL},
	{5, NULL, NULL},
	{6, NULL, NULL},
	{7, NULL, NULL},
	{8, NULL, NULL},
	{9, NULL, NULL},
	{10, NULL, NULL},
	{11, NULL, NULL},
	{12, NULL, NULL},
	{13, NULL, NULL},
	{14, NULL, NULL},
	{15, NULL, NULL},
	{16, NULL, NULL},
	{17, NULL, NULL},
	{18, NULL, NULL},
	{19, NULL, NULL},
	{20, NULL, NULL},
	{21, NULL, NULL},
	{22, NULL, NULL},
	{23, NULL, NULL},
	{24, NULL, NULL},
	{25, NULL, NULL},
	{26, NULL, NULL},
	{27, NULL, NULL},
	{28, NULL, NULL},
	{29, NULL, NULL},
	{30, NULL, NULL},
	{31, NULL, NULL},

	{32, NULL, NULL},
	{33, NULL, NULL},
	{34, NULL, NULL},
	{35, NULL, NULL},
	{36, NULL, NULL},
	{37, NULL, NULL},
};

/* number of mips dummy fp regs fp0 - fp31 + fsr and fir
 * we also add 18 unknown registers to handle gdb requests */

#define MIPS32NUMFPREGS 34 + 18

static uint8_t mips32_gdb_dummy_fp_value[] = {0, 0, 0, 0};

static struct reg mips32_gdb_dummy_fp_reg =
{
	.name = "GDB dummy floating-point register",
	.value = mips32_gdb_dummy_fp_value,
	.dirty = 0,
	.valid = 1,
	.size = 32,
	.arch_info = NULL,
};

static int mips32_get_core_reg(struct reg *reg)
{
	int retval;
	struct mips32_core_reg *mips32_reg = reg->arch_info;
	struct target *target = mips32_reg->target;
	struct mips32_common *mips32_target = target_to_mips32(target);

	if (target->state != TARGET_HALTED)
	{
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = mips32_target->read_core_reg(target, mips32_reg->num);

	return retval;
}

static int mips32_set_core_reg(struct reg *reg, uint8_t *buf)
{
	struct mips32_core_reg *mips32_reg = reg->arch_info;
	struct target *target = mips32_reg->target;
	uint32_t value = buf_get_u32(buf, 0, 32);

	if (target->state != TARGET_HALTED)
	{
		return ERROR_TARGET_NOT_HALTED;
	}

	buf_set_u32(reg->value, 0, 32, value);
	reg->dirty = 1;
	reg->valid = 1;

	return ERROR_OK;
}

static int mips32_read_core_reg(struct target *target, int num)
{
	uint32_t reg_value;
	struct mips32_core_reg *mips_core_reg;

	/* get pointers to arch-specific information */
	struct mips32_common *mips32 = target_to_mips32(target);

	if ((num < 0) || (num >= MIPS32NUMCOREREGS))
		return ERROR_INVALID_ARGUMENTS;

	mips_core_reg = mips32->core_cache->reg_list[num].arch_info;
	reg_value = mips32->core_regs[num];
	buf_set_u32(mips32->core_cache->reg_list[num].value, 0, 32, reg_value);
	mips32->core_cache->reg_list[num].valid = 1;
	mips32->core_cache->reg_list[num].dirty = 0;

	return ERROR_OK;
}

static int mips32_write_core_reg(struct target *target, int num)
{
	uint32_t reg_value;
	struct mips32_core_reg *mips_core_reg;

	/* get pointers to arch-specific information */
	struct mips32_common *mips32 = target_to_mips32(target);

	if ((num < 0) || (num >= MIPS32NUMCOREREGS))
		return ERROR_INVALID_ARGUMENTS;

	reg_value = buf_get_u32(mips32->core_cache->reg_list[num].value, 0, 32);
	mips_core_reg = mips32->core_cache->reg_list[num].arch_info;
	mips32->core_regs[num] = reg_value;
	LOG_DEBUG("write core reg %i value 0x%" PRIx32 "", num , reg_value);
	mips32->core_cache->reg_list[num].valid = 1;
	mips32->core_cache->reg_list[num].dirty = 0;

	return ERROR_OK;
}

int mips32_get_gdb_reg_list(struct target *target, struct reg **reg_list[], int *reg_list_size)
{
	/* get pointers to arch-specific information */
	struct mips32_common *mips32 = target_to_mips32(target);
	int i;

	/* include floating point registers */
	*reg_list_size = MIPS32NUMCOREREGS + MIPS32NUMFPREGS;
	*reg_list = malloc(sizeof(struct reg*) * (*reg_list_size));

	for (i = 0; i < MIPS32NUMCOREREGS; i++)
	{
		(*reg_list)[i] = &mips32->core_cache->reg_list[i];
	}

	/* add dummy floating points regs */
	for (i = MIPS32NUMCOREREGS; i < (MIPS32NUMCOREREGS + MIPS32NUMFPREGS); i++)
	{
		(*reg_list)[i] = &mips32_gdb_dummy_fp_reg;
	}

	return ERROR_OK;
}

int mips32_save_context(struct target *target)
{
	int i;

	/* get pointers to arch-specific information */
	struct mips32_common *mips32 = target_to_mips32(target);
	struct mips_ejtag *ejtag_info = &mips32->ejtag_info;

	/* read core registers */
	mips32_pracc_read_regs(ejtag_info, mips32->core_regs);

	for (i = 0; i < MIPS32NUMCOREREGS; i++)
	{
		if (!mips32->core_cache->reg_list[i].valid)
		{
			mips32->read_core_reg(target, i);
		}
	}

	return ERROR_OK;
}

int mips32_restore_context(struct target *target)
{
	int i;

	/* get pointers to arch-specific information */
	struct mips32_common *mips32 = target_to_mips32(target);
	struct mips_ejtag *ejtag_info = &mips32->ejtag_info;

	for (i = 0; i < MIPS32NUMCOREREGS; i++)
	{
		if (mips32->core_cache->reg_list[i].dirty)
		{
			mips32->write_core_reg(target, i);
		}
	}

	/* write core regs */
	mips32_pracc_write_regs(ejtag_info, mips32->core_regs);

	return ERROR_OK;
}

int mips32_arch_state(struct target *target)
{
	struct mips32_common *mips32 = target_to_mips32(target);

	LOG_USER("target halted in %s mode due to %s, pc: 0x%8.8" PRIx32 "",
		mips_isa_strings[mips32->isa_mode],
		debug_reason_name(target),
		buf_get_u32(mips32->core_cache->reg_list[MIPS32_PC].value, 0, 32));

	return ERROR_OK;
}

static const struct reg_arch_type mips32_reg_type = {
	.get = mips32_get_core_reg,
	.set = mips32_set_core_reg,
};

struct reg_cache *mips32_build_reg_cache(struct target *target)
{
	/* get pointers to arch-specific information */
	struct mips32_common *mips32 = target_to_mips32(target);

	int num_regs = MIPS32NUMCOREREGS;
	struct reg_cache **cache_p = register_get_last_cache_p(&target->reg_cache);
	struct reg_cache *cache = malloc(sizeof(struct reg_cache));
	struct reg *reg_list = malloc(sizeof(struct reg) * num_regs);
	struct mips32_core_reg *arch_info = malloc(sizeof(struct mips32_core_reg) * num_regs);
	int i;

	register_init_dummy(&mips32_gdb_dummy_fp_reg);

	/* Build the process context cache */
	cache->name = "mips32 registers";
	cache->next = NULL;
	cache->reg_list = reg_list;
	cache->num_regs = num_regs;
	(*cache_p) = cache;
	mips32->core_cache = cache;

	for (i = 0; i < num_regs; i++)
	{
		arch_info[i] = mips32_core_reg_list_arch_info[i];
		arch_info[i].target = target;
		arch_info[i].mips32_common = mips32;
		reg_list[i].name = mips32_core_reg_list[i];
		reg_list[i].size = 32;
		reg_list[i].value = calloc(1, 4);
		reg_list[i].dirty = 0;
		reg_list[i].valid = 0;
		reg_list[i].type = &mips32_reg_type;
		reg_list[i].arch_info = &arch_info[i];
	}

	return cache;
}

int mips32_init_arch_info(struct target *target, struct mips32_common *mips32, struct jtag_tap *tap)
{
	target->arch_info = mips32;
	mips32->common_magic = MIPS32_COMMON_MAGIC;
	mips32->fast_data_area = NULL;

	/* has breakpoint/watchpint unit been scanned */
	mips32->bp_scanned = 0;
	mips32->data_break_list = NULL;

	mips32->ejtag_info.tap = tap;
	mips32->read_core_reg = mips32_read_core_reg;
	mips32->write_core_reg = mips32_write_core_reg;

	return ERROR_OK;
}

/* run to exit point. return error if exit point was not reached. */
static int mips32_run_and_wait(struct target *target, uint32_t entry_point,
		int timeout_ms, uint32_t exit_point, struct mips32_common *mips32)
{
	uint32_t pc;
	int retval;
	/* This code relies on the target specific  resume() and  poll()->debug_entry()
	 * sequence to write register values to the processor and the read them back */
	if ((retval = target_resume(target, 0, entry_point, 0, 1)) != ERROR_OK)
	{
		return retval;
	}

	retval = target_wait_state(target, TARGET_HALTED, timeout_ms);
	/* If the target fails to halt due to the breakpoint, force a halt */
	if (retval != ERROR_OK || target->state != TARGET_HALTED)
	{
		if ((retval = target_halt(target)) != ERROR_OK)
			return retval;
		if ((retval = target_wait_state(target, TARGET_HALTED, 500)) != ERROR_OK)
		{
			return retval;
		}
		return ERROR_TARGET_TIMEOUT;
	}

	pc = buf_get_u32(mips32->core_cache->reg_list[MIPS32_PC].value, 0, 32);
	if (exit_point && (pc != exit_point))
	{
		LOG_DEBUG("failed algoritm halted at 0x%" PRIx32 " ", pc);
		return ERROR_TARGET_TIMEOUT;
	}

	return ERROR_OK;
}

int mips32_run_algorithm(struct target *target, int num_mem_params,
		struct mem_param *mem_params, int num_reg_params,
		struct reg_param *reg_params, uint32_t entry_point,
		uint32_t exit_point, int timeout_ms, void *arch_info)
{
	struct mips32_common *mips32 = target_to_mips32(target);
	struct mips32_algorithm *mips32_algorithm_info = arch_info;
	enum mips32_isa_mode isa_mode = mips32->isa_mode;

	uint32_t context[MIPS32NUMCOREREGS];
	int i;
	int retval = ERROR_OK;

	LOG_DEBUG("Running algorithm");

	/* NOTE: mips32_run_algorithm requires that each algorithm uses a software breakpoint
	 * at the exit point */

	if (mips32->common_magic != MIPS32_COMMON_MAGIC)
	{
		LOG_ERROR("current target isn't a MIPS32 target");
		return ERROR_TARGET_INVALID;
	}

	if (target->state != TARGET_HALTED)
	{
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* refresh core register cache */
	for (i = 0; i < MIPS32NUMCOREREGS; i++)
	{
		if (!mips32->core_cache->reg_list[i].valid)
			mips32->read_core_reg(target, i);
		context[i] = buf_get_u32(mips32->core_cache->reg_list[i].value, 0, 32);
	}

	for (i = 0; i < num_mem_params; i++)
	{
		if ((retval = target_write_buffer(target, mem_params[i].address,
				mem_params[i].size, mem_params[i].value)) != ERROR_OK)
		{
			return retval;
		}
	}

	for (i = 0; i < num_reg_params; i++)
	{
		struct reg *reg = register_get_by_name(mips32->core_cache, reg_params[i].reg_name, 0);

		if (!reg)
		{
			LOG_ERROR("BUG: register '%s' not found", reg_params[i].reg_name);
			return ERROR_INVALID_ARGUMENTS;
		}

		if (reg->size != reg_params[i].size)
		{
			LOG_ERROR("BUG: register '%s' size doesn't match reg_params[i].size",
					reg_params[i].reg_name);
			return ERROR_INVALID_ARGUMENTS;
		}

		mips32_set_core_reg(reg, reg_params[i].value);
	}

	mips32->isa_mode = mips32_algorithm_info->isa_mode;

	retval = mips32_run_and_wait(target, entry_point, timeout_ms, exit_point, mips32);

	if (retval != ERROR_OK)
		return retval;

	for (i = 0; i < num_mem_params; i++)
	{
		if (mem_params[i].direction != PARAM_OUT)
		{
			if ((retval = target_read_buffer(target, mem_params[i].address, mem_params[i].size,
					mem_params[i].value)) != ERROR_OK)
			{
				return retval;
			}
		}
	}

	for (i = 0; i < num_reg_params; i++)
	{
		if (reg_params[i].direction != PARAM_OUT)
		{
			struct reg *reg = register_get_by_name(mips32->core_cache, reg_params[i].reg_name, 0);
			if (!reg)
			{
				LOG_ERROR("BUG: register '%s' not found", reg_params[i].reg_name);
				return ERROR_INVALID_ARGUMENTS;
			}

			if (reg->size != reg_params[i].size)
			{
				LOG_ERROR("BUG: register '%s' size doesn't match reg_params[i].size",
						reg_params[i].reg_name);
				return ERROR_INVALID_ARGUMENTS;
			}

			buf_set_u32(reg_params[i].value, 0, 32, buf_get_u32(reg->value, 0, 32));
		}
	}

	/* restore everything we saved before */
	for (i = 0; i < MIPS32NUMCOREREGS; i++)
	{
		uint32_t regvalue;
		regvalue = buf_get_u32(mips32->core_cache->reg_list[i].value, 0, 32);
		if (regvalue != context[i])
		{
			LOG_DEBUG("restoring register %s with value 0x%8.8" PRIx32,
				mips32->core_cache->reg_list[i].name, context[i]);
			buf_set_u32(mips32->core_cache->reg_list[i].value,
					0, 32, context[i]);
			mips32->core_cache->reg_list[i].valid = 1;
			mips32->core_cache->reg_list[i].dirty = 1;
		}
	}

	mips32->isa_mode = isa_mode;

	return ERROR_OK;
}

int mips32_examine(struct target *target)
{
	struct mips32_common *mips32 = target_to_mips32(target);

	if (!target_was_examined(target))
	{
		target_set_examined(target);

		/* we will configure later */
		mips32->bp_scanned = 0;
		mips32->num_inst_bpoints = 0;
		mips32->num_data_bpoints = 0;
		mips32->num_inst_bpoints_avail = 0;
		mips32->num_data_bpoints_avail = 0;
	}

	return ERROR_OK;
}

int mips32_configure_break_unit(struct target *target)
{
	/* get pointers to arch-specific information */
	struct mips32_common *mips32 = target_to_mips32(target);
	int retval;
	uint32_t dcr, bpinfo;
	int i;

	if (mips32->bp_scanned)
		return ERROR_OK;

	/* get info about breakpoint support */
	if ((retval = target_read_u32(target, EJTAG_DCR, &dcr)) != ERROR_OK)
		return retval;

	if (dcr & EJTAG_DCR_IB)
	{
		/* get number of inst breakpoints */
		if ((retval = target_read_u32(target, EJTAG_IBS, &bpinfo)) != ERROR_OK)
			return retval;

		mips32->num_inst_bpoints = (bpinfo >> 24) & 0x0F;
		mips32->num_inst_bpoints_avail = mips32->num_inst_bpoints;
		mips32->inst_break_list = calloc(mips32->num_inst_bpoints, sizeof(struct mips32_comparator));
		for (i = 0; i < mips32->num_inst_bpoints; i++)
		{
			mips32->inst_break_list[i].reg_address = EJTAG_IBA1 + (0x100 * i);
		}

		/* clear IBIS reg */
		if ((retval = target_write_u32(target, EJTAG_IBS, 0)) != ERROR_OK)
			return retval;
	}

	if (dcr & EJTAG_DCR_DB)
	{
		/* get number of data breakpoints */
		if ((retval = target_read_u32(target, EJTAG_DBS, &bpinfo)) != ERROR_OK)
			return retval;

		mips32->num_data_bpoints = (bpinfo >> 24) & 0x0F;
		mips32->num_data_bpoints_avail = mips32->num_data_bpoints;
		mips32->data_break_list = calloc(mips32->num_data_bpoints, sizeof(struct mips32_comparator));
		for (i = 0; i < mips32->num_data_bpoints; i++)
		{
			mips32->data_break_list[i].reg_address = EJTAG_DBA1 + (0x100 * i);
		}

		/* clear DBIS reg */
		if ((retval = target_write_u32(target, EJTAG_DBS, 0)) != ERROR_OK)
			return retval;
	}

	LOG_DEBUG("DCR 0x%" PRIx32 " numinst %i numdata %i", dcr, mips32->num_inst_bpoints,
			mips32->num_data_bpoints);

	mips32->bp_scanned = 1;

	return ERROR_OK;
}

int mips32_enable_interrupts(struct target *target, int enable)
{
	int retval;
	int update = 0;
	uint32_t dcr;

	/* read debug control register */
	if ((retval = target_read_u32(target, EJTAG_DCR, &dcr)) != ERROR_OK)
		return retval;

	if (enable)
	{
		if (!(dcr & EJTAG_DCR_INTE))
		{
			/* enable interrupts */
			dcr |= EJTAG_DCR_INTE;
			update = 1;
		}
	}
	else
	{
		if (dcr & EJTAG_DCR_INTE)
		{
			/* disable interrupts */
			dcr &= ~EJTAG_DCR_INTE;
			update = 1;
		}
	}

	if (update)
	{
		if ((retval = target_write_u32(target, EJTAG_DCR, dcr)) != ERROR_OK)
			return retval;
	}

	return ERROR_OK;
}

int mips32_checksum_memory(struct target *target, uint32_t address,
		uint32_t count, uint32_t* checksum)
{
	struct working_area *crc_algorithm;
	struct reg_param reg_params[2];
	struct mips32_algorithm mips32_info;
	int retval;
	uint32_t i;

	/* see contib/loaders/checksum/mips32.s for src */

	static const uint32_t mips_crc_code[] =
	{
		0x248C0000,		/* addiu 	$t4, $a0, 0 */
		0x24AA0000,		/* addiu	$t2, $a1, 0 */
		0x2404FFFF,		/* addiu	$a0, $zero, 0xffffffff */
		0x10000010,		/* beq		$zero, $zero, ncomp */
		0x240B0000,		/* addiu	$t3, $zero, 0 */
						/* nbyte: */
		0x81850000,		/* lb		$a1, ($t4) */
		0x218C0001,		/* addi		$t4, $t4, 1 */
		0x00052E00,		/* sll		$a1, $a1, 24 */
		0x3C0204C1,		/* lui		$v0, 0x04c1 */
		0x00852026,		/* xor		$a0, $a0, $a1 */
		0x34471DB7,		/* ori		$a3, $v0, 0x1db7 */
		0x00003021,		/* addu		$a2, $zero, $zero */
						/* loop: */
		0x00044040,		/* sll		$t0, $a0, 1 */
		0x24C60001,		/* addiu	$a2, $a2, 1 */
		0x28840000,		/* slti		$a0, $a0, 0 */
		0x01074826,		/* xor		$t1, $t0, $a3 */
		0x0124400B,		/* movn		$t0, $t1, $a0 */
		0x28C30008,		/* slti		$v1, $a2, 8 */
		0x1460FFF9,		/* bne		$v1, $zero, loop */
		0x01002021,		/* addu		$a0, $t0, $zero */
						/* ncomp: */
		0x154BFFF0,		/* bne		$t2, $t3, nbyte */
		0x256B0001,		/* addiu	$t3, $t3, 1 */
		0x7000003F,		/* sdbbp */
	};

	/* make sure we have a working area */
	if (target_alloc_working_area(target, sizeof(mips_crc_code), &crc_algorithm) != ERROR_OK)
	{
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	/* convert flash writing code into a buffer in target endianness */
	for (i = 0; i < ARRAY_SIZE(mips_crc_code); i++)
		target_write_u32(target, crc_algorithm->address + i*sizeof(uint32_t), mips_crc_code[i]);

	mips32_info.common_magic = MIPS32_COMMON_MAGIC;
	mips32_info.isa_mode = MIPS32_ISA_MIPS32;

	init_reg_param(&reg_params[0], "a0", 32, PARAM_IN_OUT);
	buf_set_u32(reg_params[0].value, 0, 32, address);

	init_reg_param(&reg_params[1], "a1", 32, PARAM_OUT);
	buf_set_u32(reg_params[1].value, 0, 32, count);

	int timeout = 20000 * (1 + (count / (1024 * 1024)));

	if ((retval = target_run_algorithm(target, 0, NULL, 2, reg_params,
			crc_algorithm->address, crc_algorithm->address + (sizeof(mips_crc_code)-4), timeout,
			&mips32_info)) != ERROR_OK)
	{
		destroy_reg_param(&reg_params[0]);
		destroy_reg_param(&reg_params[1]);
		target_free_working_area(target, crc_algorithm);
		return 0;
	}

	*checksum = buf_get_u32(reg_params[0].value, 0, 32);

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);

	target_free_working_area(target, crc_algorithm);

	return ERROR_OK;
}

/** Checks whether a memory region is zeroed. */
int mips32_blank_check_memory(struct target *target,
		uint32_t address, uint32_t count, uint32_t* blank)
{
	struct working_area *erase_check_algorithm;
	struct reg_param reg_params[3];
	struct mips32_algorithm mips32_info;
	int retval;
	uint32_t i;

	static const uint32_t erase_check_code[] =
	{
						/* nbyte: */
		0x80880000,		/* lb		$t0, ($a0) */
		0x00C83024,		/* and		$a2, $a2, $t0 */
		0x24A5FFFF,		/* addiu	$a1, $a1, -1 */
		0x14A0FFFC,		/* bne		$a1, $zero, nbyte */
		0x24840001,		/* addiu	$a0, $a0, 1 */
		0x7000003F		/* sdbbp */
	};

	/* make sure we have a working area */
	if (target_alloc_working_area(target, sizeof(erase_check_code), &erase_check_algorithm) != ERROR_OK)
	{
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	/* convert flash writing code into a buffer in target endianness */
	for (i = 0; i < ARRAY_SIZE(erase_check_code); i++)
	{
		target_write_u32(target, erase_check_algorithm->address + i*sizeof(uint32_t),
				erase_check_code[i]);
	}

	mips32_info.common_magic = MIPS32_COMMON_MAGIC;
	mips32_info.isa_mode = MIPS32_ISA_MIPS32;

	init_reg_param(&reg_params[0], "a0", 32, PARAM_OUT);
	buf_set_u32(reg_params[0].value, 0, 32, address);

	init_reg_param(&reg_params[1], "a1", 32, PARAM_OUT);
	buf_set_u32(reg_params[1].value, 0, 32, count);

	init_reg_param(&reg_params[2], "a2", 32, PARAM_IN_OUT);
	buf_set_u32(reg_params[2].value, 0, 32, 0xff);

	if ((retval = target_run_algorithm(target, 0, NULL, 3, reg_params,
			erase_check_algorithm->address,
			erase_check_algorithm->address + (sizeof(erase_check_code)-2),
			10000, &mips32_info)) != ERROR_OK)
	{
		destroy_reg_param(&reg_params[0]);
		destroy_reg_param(&reg_params[1]);
		destroy_reg_param(&reg_params[2]);
		target_free_working_area(target, erase_check_algorithm);
		return 0;
	}

	*blank = buf_get_u32(reg_params[2].value, 0, 32);

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);

	target_free_working_area(target, erase_check_algorithm);

	return ERROR_OK;
}
