/***************************************************************************
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2008 by David T.L. Wong                                 *
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

/*
This version has optimized assembly routines for 32 bit operations:
- read word
- write word
- write array of words

One thing to be aware of is that the MIPS32 cpu will execute the
instruction after a branch instruction (one delay slot).

For example:


    LW $2, ($5 +10)
    B foo
    LW $1, ($2 +100)

The LW $1, ($2 +100) instruction is also executed. If this is
not wanted a NOP can be inserted:

    LW $2, ($5 +10)
    B foo
    NOP
    LW $1, ($2 +100)

or the code can be changed to:

    B foo
    LW $2, ($5 +10)
    LW $1, ($2 +100)

The original code contained NOPs. I have removed these and moved
the branches.

I also moved the PRACC_STACK to 0xFF204000. This allows
the use of 16 bits offsets to get pointers to the input
and output area relative to the stack. Note that the stack
isn't really a stack (the stack pointer is not 'moving')
but a FIFO simulated in software.

These changes result in a 35% speed increase when programming an
external flash.

More improvement could be gained if the registers do no need
to be preserved but in that case the routines should be aware
OpenOCD is used as a flash programmer or as a debug tool.

Nico Coesel
*/


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mips32.h"
#include "mips32_pracc.h"

struct mips32_pracc_context
{
	uint32_t *local_iparam;
	int num_iparam;
	uint32_t *local_oparam;
	int num_oparam;
	uint32_t *code;
	int code_len;
	uint32_t stack[32];
	int stack_offset;
	struct mips_ejtag *ejtag_info;
};

static int wait_for_pracc_rw(struct mips_ejtag *ejtag_info, uint32_t *ctrl)
{
	uint32_t ejtag_ctrl;

	while (1)
	{
		mips_ejtag_set_instr(ejtag_info, EJTAG_INST_CONTROL, NULL);
		ejtag_ctrl = ejtag_info->ejtag_ctrl;
		mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);
		if (ejtag_ctrl & EJTAG_CTRL_PRACC)
			break;
		LOG_DEBUG("DEBUGMODULE: No memory access in progress!\n");
		return ERROR_JTAG_DEVICE_ERROR;
	}

	*ctrl = ejtag_ctrl;
	return ERROR_OK;
}

static int mips32_pracc_exec_read(struct mips32_pracc_context *ctx, uint32_t address)
{
	struct mips_ejtag *ejtag_info = ctx->ejtag_info;
	int offset;
	uint32_t ejtag_ctrl, data;

	if ((address >= MIPS32_PRACC_PARAM_IN)
		&& (address <= MIPS32_PRACC_PARAM_IN + ctx->num_iparam * 4))
	{
		offset = (address - MIPS32_PRACC_PARAM_IN) / 4;
		data = ctx->local_iparam[offset];
	}
	else if ((address >= MIPS32_PRACC_PARAM_OUT)
		&& (address <= MIPS32_PRACC_PARAM_OUT + ctx->num_oparam * 4))
	{
		offset = (address - MIPS32_PRACC_PARAM_OUT) / 4;
		data = ctx->local_oparam[offset];
	}
	else if ((address >= MIPS32_PRACC_TEXT)
		&& (address <= MIPS32_PRACC_TEXT + ctx->code_len*4))
	{
		offset = (address - MIPS32_PRACC_TEXT) / 4;
		data = ctx->code[offset];
	}
	else if (address == MIPS32_PRACC_STACK)
	{
		/* save to our debug stack */
		data = ctx->stack[--ctx->stack_offset];
	}
	else
	{
		/* TODO: send JMP 0xFF200000 instruction. Hopefully processor jump back
		 * to start of debug vector */

		data = 0;
		LOG_ERROR("Error reading unexpected address %8.8" PRIx32 "", address);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	/* Send the data out */
	mips_ejtag_set_instr(ctx->ejtag_info, EJTAG_INST_DATA, NULL);
	mips_ejtag_drscan_32(ctx->ejtag_info, &data);

	/* Clear the access pending bit (let the processor eat!) */

	ejtag_ctrl = ejtag_info->ejtag_ctrl & ~EJTAG_CTRL_PRACC;
	mips_ejtag_set_instr(ctx->ejtag_info, EJTAG_INST_CONTROL, NULL);
	mips_ejtag_drscan_32(ctx->ejtag_info, &ejtag_ctrl);

	jtag_add_clocks(5);
	jtag_execute_queue();


	return ERROR_OK;
}

static int mips32_pracc_exec_write(struct mips32_pracc_context *ctx, uint32_t address)
{
	uint32_t ejtag_ctrl,data;
	int offset;
	struct mips_ejtag *ejtag_info = ctx->ejtag_info;

	mips_ejtag_set_instr(ctx->ejtag_info, EJTAG_INST_DATA, NULL);
	mips_ejtag_drscan_32(ctx->ejtag_info, &data);

	/* Clear access pending bit */
	ejtag_ctrl = ejtag_info->ejtag_ctrl & ~EJTAG_CTRL_PRACC;
	mips_ejtag_set_instr(ctx->ejtag_info, EJTAG_INST_CONTROL, NULL);
	mips_ejtag_drscan_32(ctx->ejtag_info, &ejtag_ctrl);

	jtag_add_clocks(5);
	jtag_execute_queue();

	if ((address >= MIPS32_PRACC_PARAM_IN)
		&& (address <= MIPS32_PRACC_PARAM_IN + ctx->num_iparam * 4))
	{
		offset = (address - MIPS32_PRACC_PARAM_IN) / 4;
		ctx->local_iparam[offset] = data;
	}
	else if ((address >= MIPS32_PRACC_PARAM_OUT)
		&& (address <= MIPS32_PRACC_PARAM_OUT + ctx->num_oparam * 4))
	{
		offset = (address - MIPS32_PRACC_PARAM_OUT) / 4;
		ctx->local_oparam[offset] = data;
	}
	else if (address == MIPS32_PRACC_STACK)
	{
		/* save data onto our stack */
		ctx->stack[ctx->stack_offset++] = data;
	}
	else
	{
		LOG_ERROR("Error writing unexpected address %8.8" PRIx32 "", address);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	return ERROR_OK;
}

int mips32_pracc_exec(struct mips_ejtag *ejtag_info, int code_len, uint32_t *code, int num_param_in, uint32_t *param_in, int num_param_out, uint32_t *param_out, int cycle)
{
	uint32_t ejtag_ctrl;
	uint32_t address, data;
	struct mips32_pracc_context ctx;
	int retval;
	int pass = 0;

	ctx.local_iparam = param_in;
	ctx.local_oparam = param_out;
	ctx.num_iparam = num_param_in;
	ctx.num_oparam = num_param_out;
	ctx.code = code;
	ctx.code_len = code_len;
	ctx.ejtag_info = ejtag_info;
	ctx.stack_offset = 0;

	while (1)
	{
		if ((retval = wait_for_pracc_rw(ejtag_info, &ejtag_ctrl)) != ERROR_OK)
			return retval;

		address = data = 0;
		mips_ejtag_set_instr(ejtag_info, EJTAG_INST_ADDRESS, NULL);
		mips_ejtag_drscan_32(ejtag_info, &address);

//		printf("Adres: %.8x\n", address);

		/* Check for read or write */
		if (ejtag_ctrl & EJTAG_CTRL_PRNW)
		{
			if ((retval = mips32_pracc_exec_write(&ctx, address)) != ERROR_OK)
				return retval;
		}
		else
		{
			/* Check to see if its reading at the debug vector. The first pass through
			 * the module is always read at the vector, so the first one we allow.  When
			 * the second read from the vector occurs we are done and just exit. */
			if ((address == MIPS32_PRACC_TEXT) && (pass++))
			{
				break;
			}

			if ((retval = mips32_pracc_exec_read(&ctx, address)) != ERROR_OK)
				return retval;

		}

		if (cycle == 0)
			break;
	}

	/* stack sanity check */
	if (ctx.stack_offset != 0)
	{
		LOG_DEBUG("Pracc Stack not zero");
	}

	return ERROR_OK;
}

int mips32_pracc_read_mem(struct mips_ejtag *ejtag_info, uint32_t addr, int size, int count, void *buf)
{
	switch (size)
	{
		case 1:
			return mips32_pracc_read_mem8(ejtag_info, addr, count, (uint8_t*)buf);
		case 2:
			return mips32_pracc_read_mem16(ejtag_info, addr, count, (uint16_t*)buf);
		case 4:
			if (count == 1)
				return mips32_pracc_read_u32(ejtag_info, addr, (uint32_t*)buf);
			else
				return mips32_pracc_read_mem32(ejtag_info, addr, count, (uint32_t*)buf);
	}

	return ERROR_OK;
}

int mips32_pracc_read_mem32(struct mips_ejtag *ejtag_info, uint32_t addr, int count, uint32_t *buf)
{
	uint32_t code[] = {
															/* start: */
		MIPS32_MTC0(15,31,0),								/* move $15 to COP0 DeSave */
		MIPS32_LUI(15,UPPER16(MIPS32_PRACC_STACK)), 		/* $15 = MIPS32_PRACC_STACK */
		MIPS32_ORI(15,15,LOWER16(MIPS32_PRACC_STACK)),
		MIPS32_SW(8,0,15), 									/* sw $8,($15) */
		MIPS32_SW(9,0,15), 									/* sw $9,($15) */
		MIPS32_SW(10,0,15), 								/* sw $10,($15) */
		MIPS32_SW(11,0,15), 								/* sw $11,($15) */

		MIPS32_LUI(8,UPPER16(MIPS32_PRACC_PARAM_IN)),		/* $8 = MIPS32_PRACC_PARAM_IN */
		MIPS32_ORI(8,8,LOWER16(MIPS32_PRACC_PARAM_IN)),
		MIPS32_LW(9,0,8),									/* $9 = mem[$8]; read addr */
		MIPS32_LW(10,4,8),									/* $10 = mem[$8 + 4]; read count */
		MIPS32_LUI(11,UPPER16(MIPS32_PRACC_PARAM_OUT)), 	/* $11 = MIPS32_PRACC_PARAM_OUT */
		MIPS32_ORI(11,11,LOWER16(MIPS32_PRACC_PARAM_OUT)),
		MIPS32_NOP,
															/* loop: */
		MIPS32_BEQ(0,10,9),									/* beq 0, $10, end */
		MIPS32_NOP,

		MIPS32_LW(8,0,9), 									/* lw $8,0($9), Load $8 with the word @mem[$9] */
		MIPS32_SW(8,0,11), 									/* sw $8,0($11) */

		MIPS32_ADDI(10,10,NEG16(1)), 						/* $10-- */
		MIPS32_ADDI(9,9,4), 								/* $1 += 4 */
		MIPS32_ADDI(11,11,4), 								/* $11 += 4 */

		MIPS32_NOP,
		MIPS32_B(NEG16(9)),									/* b loop */
		MIPS32_NOP,
															/* end: */
		MIPS32_LW(11,0,15), 								/* lw $11,($15) */
		MIPS32_LW(10,0,15), 								/* lw $10,($15) */
		MIPS32_LW(9,0,15), 									/* lw $9,($15) */
		MIPS32_LW(8,0,15), 									/* lw $8,($15) */
		MIPS32_MFC0(15,31,0),								/* move COP0 DeSave to $15 */
		MIPS32_NOP,
		MIPS32_B(NEG16(31)),								/* b start */
		MIPS32_NOP,
	};

	int retval = ERROR_OK;
	int blocksize;
	int bytesread;
	uint32_t param_in[2];

	bytesread = 0;

	while (count > 0)
	{
		blocksize = count;
		if (count > 0x400)
			blocksize = 0x400;

		param_in[0] = addr;
		param_in[1] = blocksize;

		if ((retval = mips32_pracc_exec(ejtag_info, sizeof(code)/sizeof(code[0]), code,
			sizeof(param_in)/sizeof(param_in[0]), param_in, blocksize, &buf[bytesread], 1)) != ERROR_OK)
		{
			return retval;
		}

		count -= blocksize;
		addr += blocksize;
		bytesread += blocksize;
	}

	return retval;
}

int mips32_pracc_read_u32(struct mips_ejtag *ejtag_info, uint32_t addr, uint32_t *buf)
{
	uint32_t code[] = {
															/* start: */
		MIPS32_MTC0(15,31,0),								/* move $15 to COP0 DeSave */
		MIPS32_LUI(15,UPPER16(MIPS32_PRACC_STACK)), 		/* $15 = MIPS32_PRACC_STACK */
		MIPS32_ORI(15,15,LOWER16(MIPS32_PRACC_STACK)),
		MIPS32_SW(8,0,15), 									/* sw $8,($15) */

		MIPS32_LW(8,NEG16(MIPS32_PRACC_STACK-MIPS32_PRACC_PARAM_IN), 15),  //load R8 @ param_in[0] = address

		MIPS32_LW(8,0,8), 									/* lw $8,0($8), Load $8 with the word @mem[$8] */
		MIPS32_SW(8,NEG16(MIPS32_PRACC_STACK-MIPS32_PRACC_PARAM_OUT),15), 									/* sw $8,0($9) */

		MIPS32_LW(8,0,15), 									/* lw $8,($15) */
		MIPS32_B(NEG16(9)),	//was 17							/* b start */
		MIPS32_MFC0(15,31,0),   //this instruction will be executed (MIPS executes instruction after jump)							/* move COP0 DeSave to $15 */
		MIPS32_NOP,
	};

	int retval = ERROR_OK;
	uint32_t param_in[1];

	param_in[0] = addr;

	if ((retval = mips32_pracc_exec(ejtag_info, sizeof(code)/sizeof(code[0]), code,
		sizeof(param_in)/sizeof(param_in[0]), param_in, sizeof(uint32_t), buf, 1)) != ERROR_OK)
	{
		return retval;
	}

	return retval;
}

int mips32_pracc_read_mem16(struct mips_ejtag *ejtag_info, uint32_t addr, int count, uint16_t *buf)
{
	uint32_t code[] = {
															/* start: */
		MIPS32_MTC0(15,31,0),								/* move $15 to COP0 DeSave */
		MIPS32_LUI(15,UPPER16(MIPS32_PRACC_STACK)), 		/* $15 = MIPS32_PRACC_STACK */
		MIPS32_ORI(15,15,LOWER16(MIPS32_PRACC_STACK)),
		MIPS32_SW(8,0,15), 									/* sw $8,($15) */
		MIPS32_SW(9,0,15), 									/* sw $9,($15) */
		MIPS32_SW(10,0,15), 								/* sw $10,($15) */
		MIPS32_SW(11,0,15), 								/* sw $11,($15) */

		MIPS32_LUI(8,UPPER16(MIPS32_PRACC_PARAM_IN)),		/* $8 = MIPS32_PRACC_PARAM_IN */
		MIPS32_ORI(8,8,LOWER16(MIPS32_PRACC_PARAM_IN)),
		MIPS32_LW(9,0,8),									/* $9 = mem[$8]; read addr */
		MIPS32_LW(10,4,8),									/* $10 = mem[$8 + 4]; read count */
		MIPS32_LUI(11,UPPER16(MIPS32_PRACC_PARAM_OUT)),		/* $11 = MIPS32_PRACC_PARAM_OUT */
		MIPS32_ORI(11,11,LOWER16(MIPS32_PRACC_PARAM_OUT)),
		MIPS32_NOP,
															/* loop: */
		MIPS32_BEQ(0,10,9), 								/* beq 0, $10, end */
		MIPS32_NOP,

		MIPS32_LHU(8,0,9), 									/* lw $8,0($9), Load $8 with the halfword @mem[$9] */
		MIPS32_SW(8,0,11), 									/* sw $8,0($11) */

		MIPS32_ADDI(10,10,NEG16(1)), 						/* $10-- */
		MIPS32_ADDI(9,9,2), 								/* $9 += 2 */
		MIPS32_ADDI(11,11,4), 								/* $11 += 4 */
		MIPS32_NOP,
		MIPS32_B(NEG16(9)),									/* b loop */
		MIPS32_NOP,

		MIPS32_LW(11,0,15), 								/* lw $11,($15) */
		MIPS32_LW(10,0,15), 								/* lw $10,($15) */
		MIPS32_LW(9,0,15), 									/* lw $9,($15) */
		MIPS32_LW(8,0,15), 									/* lw $8,($15) */
		MIPS32_MFC0(15,31,0),								/* move COP0 DeSave to $15 */
		MIPS32_NOP,
		MIPS32_B(NEG16(31)),								/* b start */
		MIPS32_NOP,
	};

//	/* TODO remove array */
	uint32_t param_out[count];
	int i;

//	int retval;
	int blocksize;
	int bytesread;
	uint32_t param_in[2];

	bytesread = 0;

	//while (count > 0)
	{
		blocksize = count;
		if (count > 0x400)
			blocksize = 0x400;

		param_in[0] = addr;
		param_in[1] = blocksize;

		mips32_pracc_exec(ejtag_info, sizeof(code)/sizeof(code[0]), code, \
			sizeof(param_in)/sizeof(param_in[0]), param_in, count, param_out, 1);

//		count -= blocksize;
//		addr += blocksize;
//		bytesread += blocksize;
	}

	for (i = 0; i < count; i++)
	{
		buf[i] = param_out[i];
	}

	return ERROR_OK;
}

int mips32_pracc_read_mem8(struct mips_ejtag *ejtag_info, uint32_t addr, int count, uint8_t *buf)
{
	uint32_t code[] = {
															/* start: */
		MIPS32_MTC0(15,31,0),								/* move $15 to COP0 DeSave */
		MIPS32_LUI(15,UPPER16(MIPS32_PRACC_STACK)), 		/* $15 = MIPS32_PRACC_STACK */
		MIPS32_ORI(15,15,LOWER16(MIPS32_PRACC_STACK)),
		MIPS32_SW(8,0,15), 									/* sw $8,($15) */
		MIPS32_SW(9,0,15), 									/* sw $9,($15) */
		MIPS32_SW(10,0,15), 								/* sw $10,($15) */
		MIPS32_SW(11,0,15), 								/* sw $11,($15) */

		MIPS32_LUI(8,UPPER16(MIPS32_PRACC_PARAM_IN)), 		/* $8 = MIPS32_PRACC_PARAM_IN */
		MIPS32_ORI(8,8,LOWER16(MIPS32_PRACC_PARAM_IN)),
		MIPS32_LW(9,0,8), 									/* $9 = mem[$8]; read addr */
		MIPS32_LW(10,4,8), 									/* $10 = mem[$8 + 4]; read count */
		MIPS32_LUI(11,UPPER16(MIPS32_PRACC_PARAM_OUT)), 	/* $11 = MIPS32_PRACC_PARAM_OUT */
		MIPS32_ORI(11,11,LOWER16(MIPS32_PRACC_PARAM_OUT)),
		MIPS32_NOP,
															/* loop: */
		MIPS32_BEQ(0,10,9), 								/* beq 0, $10, end */
		MIPS32_NOP,

		MIPS32_LBU(8,0,9), 									/* lw $8,0($9), Load t4 with the byte @mem[t1] */
		MIPS32_SW(8,0,11), 									/* sw $8,0($11) */

		MIPS32_ADDI(10,10,NEG16(1)), 						/* $10-- */
		MIPS32_ADDI(9,9,1), 								/* $9 += 1 */
		MIPS32_ADDI(11,11,4), 								/* $11 += 4 */
		MIPS32_NOP,
		MIPS32_B(NEG16(9)),									/* b loop */
		MIPS32_NOP,
															/* end: */
		MIPS32_LW(11,0,15), 								/* lw $11,($15) */
		MIPS32_LW(10,0,15), 								/* lw $10,($15) */
		MIPS32_LW(9,0,15), 									/* lw $9,($15) */
		MIPS32_LW(8,0,15), 									/* lw $8,($15) */
		MIPS32_MFC0(15,31,0),								/* move COP0 DeSave to $15 */
		MIPS32_NOP,
		MIPS32_B(NEG16(31)),								/* b start */
		MIPS32_NOP,
	};

//	/* TODO remove array */
	uint32_t param_out[count];
	int i;

//	int retval;
	int blocksize;
	int bytesread;
	uint32_t param_in[2];

	bytesread = 0;

//	while (count > 0)
	{
		blocksize = count;
		if (count > 0x400)
			blocksize = 0x400;

		param_in[0] = addr;
		param_in[1] = blocksize;

		mips32_pracc_exec(ejtag_info, sizeof(code)/sizeof(code[0]), code, \
			sizeof(param_in)/sizeof(param_in[0]), param_in, count, param_out, 1);

//		count -= blocksize;
//		addr += blocksize;
//		bytesread += blocksize;
	}

	for (i = 0; i < count; i++)
	{
		buf[i] = param_out[i];
	}

	return ERROR_OK;
}

int mips32_pracc_write_mem(struct mips_ejtag *ejtag_info, uint32_t addr, int size, int count, void *buf)
{
	switch (size)
	{
		case 1:
			return mips32_pracc_write_mem8(ejtag_info, addr, count, (uint8_t*)buf);
		case 2:
			return mips32_pracc_write_mem16(ejtag_info, addr, count,(uint16_t*)buf);
		case 4:
			if (count == 1)
				return mips32_pracc_write_u32(ejtag_info, addr, (uint32_t*)buf);
			else
				return mips32_pracc_write_mem32(ejtag_info, addr, count, (uint32_t*)buf);
	}

	return ERROR_OK;
}

int mips32_pracc_write_mem32(struct mips_ejtag *ejtag_info, uint32_t addr, int count, uint32_t *buf)
{

//NC: use destination pointer as loop counter (last address is in $10)
	uint32_t code[] = {
															/* start: */
		MIPS32_MTC0(15,31,0),								/* move $15 to COP0 DeSave */
		MIPS32_LUI(15,UPPER16(MIPS32_PRACC_STACK)), 		/* $15 = MIPS32_PRACC_STACK */
		MIPS32_ORI(15,15,LOWER16(MIPS32_PRACC_STACK)),
		MIPS32_SW(8,0,15), 									/* sw $8,($15) */
		MIPS32_SW(9,0,15), 									/* sw $9,($15) */
		MIPS32_SW(10,0,15), 								/* sw $10,($15) */
		MIPS32_SW(11,0,15), 								/* sw $11,($15) */

		MIPS32_ADDI(8,15,NEG16(MIPS32_PRACC_STACK-MIPS32_PRACC_PARAM_IN)),  //$8= MIPS32_PRACC_PARAM_IN
		MIPS32_LW(9,0,8), 									/* Load write addr to $9 */
		MIPS32_LW(10,4,8),	//last address 									/* Load write count to $10 */
		MIPS32_ADDI(8,8,8), 	// $8 += 8 beginning of data

//loop:
		MIPS32_LW(11,0,8), 									/* lw $11,0($8), Load $11 with the word @mem[$8] */
		MIPS32_SW(11,0,9), 									/* sw $11,0($9) */

		MIPS32_ADDI(9,9,4), 								/* $9 += 4 */
		MIPS32_BNE(10,9,NEG16(4)),  //was 9 BNE $10, 9, loop									/* b loop */
		MIPS32_ADDI(8,8,4),  //this instruction is part of the loop (one delay slot)!	/* $8 += 4 */
															/* end: */
		MIPS32_LW(11,0,15), 								/* lw $11,($15) */
		MIPS32_LW(10,0,15), 								/* lw $10,($15) */
		MIPS32_LW(9,0,15), 									/* lw $9,($15) */
		MIPS32_LW(8,0,15), 									/* lw $8,($15) */
		MIPS32_B(NEG16(21)),	 //was 30							/* b start */
		MIPS32_MFC0(15,31,0),								/* move COP0 DeSave to $15 */
		MIPS32_NOP, //this one will not be executed
	};

	/* TODO remove array */
	uint32_t param_in[count + 2];
	param_in[0] = addr;
	param_in[1] = addr + count * sizeof(uint32_t);	//last address

	memcpy(&param_in[2], buf, count * sizeof(uint32_t));

	mips32_pracc_exec(ejtag_info, sizeof(code)/sizeof(code[0]), code, \
		sizeof(param_in)/sizeof(param_in[0]),param_in, 0, NULL, 1);

	return ERROR_OK;
}

int mips32_pracc_write_u32(struct mips_ejtag *ejtag_info, uint32_t addr, uint32_t *buf)
{
	uint32_t code[] = {
															/* start: */
		MIPS32_MTC0(15,31,0),								/* move $15 to COP0 DeSave */
		MIPS32_LUI(15,UPPER16(MIPS32_PRACC_STACK)), 		/* $15 = MIPS32_PRACC_STACK */
		MIPS32_ORI(15,15,LOWER16(MIPS32_PRACC_STACK)),
		MIPS32_SW(8,0,15), 									/* sw $8,($15) */
		MIPS32_SW(9,0,15), 									/* sw $9,($15) */

		MIPS32_LW(8,NEG16((MIPS32_PRACC_STACK-MIPS32_PRACC_PARAM_IN)-4), 15),  //load R8 @ param_in[1] = data
		MIPS32_LW(9,NEG16(MIPS32_PRACC_STACK-MIPS32_PRACC_PARAM_IN), 15),  //load R9 @ param_in[0] = address

		MIPS32_SW(8,0,9), 									/* sw $8,0($9) */

		MIPS32_LW(9,0,15), 									/* lw $9,($15) */
		MIPS32_LW(8,0,15), 									/* lw $8,($15) */
		MIPS32_B(NEG16(11)),								/* b start */
		MIPS32_MFC0(15,31,0),							/* move COP0 DeSave to $15 */
		MIPS32_NOP,
	};

	/* TODO remove array */
	uint32_t param_in[1 + 1];
	param_in[0] = addr;
	param_in[1] = *buf;

	mips32_pracc_exec(ejtag_info, sizeof(code)/sizeof(code[0]), code, \
		sizeof(param_in)/sizeof(param_in[0]),param_in, 0, NULL, 1);

	return ERROR_OK;
}

int mips32_pracc_write_mem16(struct mips_ejtag *ejtag_info, uint32_t addr, int count, uint16_t *buf)
{
	uint32_t code[] = {
															/* start: */
		MIPS32_MTC0(15,31,0),								/* move $15 to COP0 DeSave */
		MIPS32_LUI(15,UPPER16(MIPS32_PRACC_STACK)), 		/* $15 = MIPS32_PRACC_STACK */
		MIPS32_ORI(15,15,LOWER16(MIPS32_PRACC_STACK)),
		MIPS32_SW(8,0,15), 									/* sw $8,($15) */
		MIPS32_SW(9,0,15), 									/* sw $9,($15) */
		MIPS32_SW(10,0,15), 								/* sw $10,($15) */
		MIPS32_SW(11,0,15), 								/* sw $11,($15) */

		MIPS32_LUI(8,UPPER16(MIPS32_PRACC_PARAM_IN)), 		/* $8 = MIPS32_PRACC_PARAM_IN */
		MIPS32_ORI(8,8,LOWER16(MIPS32_PRACC_PARAM_IN)),
		MIPS32_LW(9,0,8), 									/* Load write addr to $9 */
		MIPS32_LW(10,4,8), 									/* Load write count to $10 */
		MIPS32_ADDI(8,8,8), 								/* $8 += 8 */
		MIPS32_NOP,
															/* loop: */
		MIPS32_BEQ(0,10,9),									/* beq $0, $10, end */
		MIPS32_NOP,

		MIPS32_LW(11,0,8), 									/* lw $11,0($8), Load $11 with the word @mem[$8] */
		MIPS32_SH(11,0,9), 									/* sh $11,0($9) */

		MIPS32_ADDI(10,10,NEG16(1)), 						/* $10-- */
		MIPS32_ADDI(9,9,2), 								/* $9 += 2 */
		MIPS32_ADDI(8,8,4), 								/* $8 += 4 */

		MIPS32_NOP,
		MIPS32_B(NEG16(9)),									/* b loop */
		MIPS32_NOP,
															/* end: */
		MIPS32_LW(11,0,15), 								/* lw $11,($15) */
		MIPS32_LW(10,0,15), 								/* lw $10,($15) */
		MIPS32_LW(9,0,15), 									/* lw $9,($15) */
		MIPS32_LW(8,0,15), 									/* lw $8,($15) */
		MIPS32_MFC0(15,31,0),								/* move COP0 DeSave to $15 */
		MIPS32_NOP,
		MIPS32_B(NEG16(30)),								/* b start */
		MIPS32_NOP,
	};

	/* TODO remove array */
	uint32_t param_in[count + 2];
	int i;
	param_in[0] = addr;
	param_in[1] = count;

	for (i = 0; i < count; i++)
	{
		param_in[i + 2] = buf[i];
	}

	mips32_pracc_exec(ejtag_info, sizeof(code)/sizeof(code[0]), code, \
		sizeof(param_in)/sizeof(param_in[0]), param_in, 0, NULL, 1);

	return ERROR_OK;
}

int mips32_pracc_write_mem8(struct mips_ejtag *ejtag_info, uint32_t addr, int count, uint8_t *buf)
{
	uint32_t code[] = {
															/* start: */
		MIPS32_MTC0(15,31,0),								/* move $15 to COP0 DeSave */
		MIPS32_LUI(15,UPPER16(MIPS32_PRACC_STACK)), 		/* $15 = MIPS32_PRACC_STACK */
		MIPS32_ORI(15,15,LOWER16(MIPS32_PRACC_STACK)),
		MIPS32_SW(8,0,15), 									/* sw $8,($15) */
		MIPS32_SW(9,0,15), 									/* sw $9,($15) */
		MIPS32_SW(10,0,15), 								/* sw $10,($15) */
		MIPS32_SW(11,0,15), 								/* sw $11,($15) */

		MIPS32_LUI(8,UPPER16(MIPS32_PRACC_PARAM_IN)), 		/* $8 = MIPS32_PRACC_PARAM_IN */
		MIPS32_ORI(8,8,LOWER16(MIPS32_PRACC_PARAM_IN)),
		MIPS32_LW(9,0,8), 									/* Load write addr to $9 */
		MIPS32_LW(10,4,8), 									/* Load write count to $10 */
		MIPS32_ADDI(8,8,8), 								/* $8 += 8 */
		MIPS32_NOP,
															/* loop: */
		MIPS32_BEQ(0,10,9),									/* beq $0, $10, end */
		MIPS32_NOP,

		MIPS32_LW(11,0,8), 									/* lw $11,0($8), Load $11 with the word @mem[$8] */
		MIPS32_SB(11,0,9), 									/* sb $11,0($9) */

		MIPS32_ADDI(10,10,NEG16(1)), 						/* $10-- */
		MIPS32_ADDI(9,9,1), 								/* $9 += 1 */
		MIPS32_ADDI(8,8,4), 								/* $8 += 4 */

		MIPS32_NOP,
		MIPS32_B(NEG16(9)),									/* b loop */
		MIPS32_NOP,
															/* end: */
		MIPS32_LW(11,0,15), 								/* lw $11,($15) */
		MIPS32_LW(10,0,15), 								/* lw $10,($15) */
		MIPS32_LW(9,0,15), 									/* lw $9,($15) */
		MIPS32_LW(8,0,15), 									/* lw $8,($15) */
		MIPS32_MFC0(15,31,0),								/* move COP0 DeSave to $15 */
		MIPS32_NOP,
		MIPS32_B(NEG16(30)),								/* b start */
		MIPS32_NOP,
	};

	/* TODO remove array */
	uint32_t param_in[count + 2];
	int retval;
	int i;
	param_in[0] = addr;
	param_in[1] = count;

	for (i = 0; i < count; i++)
	{
		param_in[i + 2] = buf[i];
	}

	retval = mips32_pracc_exec(ejtag_info, sizeof(code)/sizeof(code[0]), code, \
		sizeof(param_in)/sizeof(param_in[0]), param_in, 0, NULL, 1);

	return retval;
}

int mips32_pracc_write_regs(struct mips_ejtag *ejtag_info, uint32_t *regs)
{
	uint32_t code[] = {
														/* start: */
		MIPS32_LUI(2,UPPER16(MIPS32_PRACC_PARAM_IN)), 	/* $2 = MIPS32_PRACC_PARAM_IN */
		MIPS32_ORI(2,2,LOWER16(MIPS32_PRACC_PARAM_IN)),
		MIPS32_LW(1,1*4,2), 							/* lw $1,1*4($2) */
		MIPS32_LW(15,15*4,2), 							/* lw $15,15*4($2) */
		MIPS32_MTC0(15,31,0),							/* move $15 to COP0 DeSave */
		MIPS32_LUI(15,UPPER16(MIPS32_PRACC_STACK)), 	/* $15 = MIPS32_PRACC_STACK */
		MIPS32_ORI(15,15,LOWER16(MIPS32_PRACC_STACK)),
		MIPS32_SW(1,0,15), 								/* sw $1,($15) */
		MIPS32_LUI(1,UPPER16(MIPS32_PRACC_PARAM_IN)), 	/* $1 = MIPS32_PRACC_PARAM_IN */
		MIPS32_ORI(1,1,LOWER16(MIPS32_PRACC_PARAM_IN)),
		MIPS32_LW(3,3*4,1), 							/* lw $3,3*4($1) */
		MIPS32_LW(4,4*4,1), 							/* lw $4,4*4($1) */
		MIPS32_LW(5,5*4,1), 							/* lw $5,5*4($1) */
		MIPS32_LW(6,6*4,1), 							/* lw $6,6*4($1) */
		MIPS32_LW(7,7*4,1), 							/* lw $7,7*4($1) */
		MIPS32_LW(8,8*4,1), 							/* lw $8,8*4($1) */
		MIPS32_LW(9,9*4,1), 							/* lw $9,9*4($1) */
		MIPS32_LW(10,10*4,1), 							/* lw $10,10*4($1) */
		MIPS32_LW(11,11*4,1), 							/* lw $11,11*4($1) */
		MIPS32_LW(12,12*4,1), 							/* lw $12,12*4($1) */
		MIPS32_LW(13,13*4,1), 							/* lw $13,13*4($1) */
		MIPS32_LW(14,14*4,1), 							/* lw $14,14*4($1) */
		MIPS32_LW(16,16*4,1), 							/* lw $16,16*4($1) */
		MIPS32_LW(17,17*4,1), 							/* lw $17,17*4($1) */
		MIPS32_LW(18,18*4,1), 							/* lw $18,18*4($1) */
		MIPS32_LW(19,19*4,1), 							/* lw $19,19*4($1) */
		MIPS32_LW(20,20*4,1), 							/* lw $20,20*4($1) */
		MIPS32_LW(21,21*4,1), 							/* lw $21,21*4($1) */
		MIPS32_LW(22,22*4,1), 							/* lw $22,22*4($1) */
		MIPS32_LW(23,23*4,1), 							/* lw $23,23*4($1) */
		MIPS32_LW(24,24*4,1), 							/* lw $24,24*4($1) */
		MIPS32_LW(25,25*4,1), 							/* lw $25,25*4($1) */
		MIPS32_LW(26,26*4,1), 							/* lw $26,26*4($1) */
		MIPS32_LW(27,27*4,1), 							/* lw $27,27*4($1) */
		MIPS32_LW(28,28*4,1), 							/* lw $28,28*4($1) */
		MIPS32_LW(29,29*4,1), 							/* lw $29,29*4($1) */
		MIPS32_LW(30,30*4,1), 							/* lw $30,30*4($1) */
		MIPS32_LW(31,31*4,1), 							/* lw $31,31*4($1) */

		MIPS32_LW(2,32*4,1), 							/* lw $2,32*4($1) */
		MIPS32_MTC0(2,12,0),							/* move $2 to status */
		MIPS32_LW(2,33*4,1), 							/* lw $2,33*4($1) */
		MIPS32_MTLO(2),									/* move $2 to lo */
		MIPS32_LW(2,34*4,1), 							/* lw $2,34*4($1) */
		MIPS32_MTHI(2),									/* move $2 to hi */
		MIPS32_LW(2,35*4,1), 							/* lw $2,35*4($1) */
		MIPS32_MTC0(2,8,0),								/* move $2 to badvaddr */
		MIPS32_LW(2,36*4,1), 							/* lw $2,36*4($1) */
		MIPS32_MTC0(2,13,0),							/* move $2 to cause*/
		MIPS32_LW(2,37*4,1), 							/* lw $2,37*4($1) */
		MIPS32_MTC0(2,24,0),							/* move $2 to pc */

		MIPS32_LW(2,2*4,1), 							/* lw $2,2*4($1) */
		MIPS32_LW(1,0,15), 								/* lw $1,($15) */
		MIPS32_MFC0(15,31,0),							/* move COP0 DeSave to $15 */
		MIPS32_NOP,
		MIPS32_B(NEG16(55)),							/* b start */
		MIPS32_NOP,
	};

	int retval;

	retval = mips32_pracc_exec(ejtag_info, sizeof(code)/sizeof(code[0]), code, \
		38, regs, 0, NULL, 1);

	return retval;
}

int mips32_pracc_read_regs(struct mips_ejtag *ejtag_info, uint32_t *regs)
{
	uint32_t code[] = {
														/* start: */
		MIPS32_MTC0(2,31,0),							/* move $2 to COP0 DeSave */
		MIPS32_LUI(2,UPPER16(MIPS32_PRACC_PARAM_OUT)), 	/* $2 = MIPS32_PRACC_PARAM_OUT */
		MIPS32_ORI(2,2,LOWER16(MIPS32_PRACC_PARAM_OUT)),
		MIPS32_SW(0,0*4,2),								/* sw $0,0*4($2) */
		MIPS32_SW(1,1*4,2), 							/* sw $1,1*4($2) */
		MIPS32_SW(15,15*4,2), 							/* sw $15,15*4($2) */
		MIPS32_MFC0(2,31,0),							/* move COP0 DeSave to $2 */
		MIPS32_MTC0(15,31,0),							/* move $15 to COP0 DeSave */
		MIPS32_LUI(15,UPPER16(MIPS32_PRACC_STACK)), 	/* $15 = MIPS32_PRACC_STACK */
		MIPS32_ORI(15,15,LOWER16(MIPS32_PRACC_STACK)),
		MIPS32_SW(1,0,15), 								/* sw $1,($15) */
		MIPS32_SW(2,0,15), 								/* sw $2,($15) */
		MIPS32_LUI(1,UPPER16(MIPS32_PRACC_PARAM_OUT)), 	/* $1 = MIPS32_PRACC_PARAM_OUT */
		MIPS32_ORI(1,1,LOWER16(MIPS32_PRACC_PARAM_OUT)),
		MIPS32_SW(2,2*4,1), 							/* sw $2,2*4($1) */
		MIPS32_SW(3,3*4,1), 							/* sw $3,3*4($1) */
		MIPS32_SW(4,4*4,1), 							/* sw $4,4*4($1) */
		MIPS32_SW(5,5*4,1), 							/* sw $5,5*4($1) */
		MIPS32_SW(6,6*4,1), 							/* sw $6,6*4($1) */
		MIPS32_SW(7,7*4,1), 							/* sw $7,7*4($1) */
		MIPS32_SW(8,8*4,1), 							/* sw $8,8*4($1) */
		MIPS32_SW(9,9*4,1), 							/* sw $9,9*4($1) */
		MIPS32_SW(10,10*4,1), 							/* sw $10,10*4($1) */
		MIPS32_SW(11,11*4,1), 							/* sw $11,11*4($1) */
		MIPS32_SW(12,12*4,1), 							/* sw $12,12*4($1) */
		MIPS32_SW(13,13*4,1), 							/* sw $13,13*4($1) */
		MIPS32_SW(14,14*4,1), 							/* sw $14,14*4($1) */
		MIPS32_SW(16,16*4,1), 							/* sw $16,16*4($1) */
		MIPS32_SW(17,17*4,1), 							/* sw $17,17*4($1) */
		MIPS32_SW(18,18*4,1), 							/* sw $18,18*4($1) */
		MIPS32_SW(19,19*4,1), 							/* sw $19,19*4($1) */
		MIPS32_SW(20,20*4,1), 							/* sw $20,20*4($1) */
		MIPS32_SW(21,21*4,1), 							/* sw $21,21*4($1) */
		MIPS32_SW(22,22*4,1), 							/* sw $22,22*4($1) */
		MIPS32_SW(23,23*4,1), 							/* sw $23,23*4($1) */
		MIPS32_SW(24,24*4,1), 							/* sw $24,24*4($1) */
		MIPS32_SW(25,25*4,1), 							/* sw $25,25*4($1) */
		MIPS32_SW(26,26*4,1), 							/* sw $26,26*4($1) */
		MIPS32_SW(27,27*4,1), 							/* sw $27,27*4($1) */
		MIPS32_SW(28,28*4,1), 							/* sw $28,28*4($1) */
		MIPS32_SW(29,29*4,1), 							/* sw $29,29*4($1) */
		MIPS32_SW(30,30*4,1), 							/* sw $30,30*4($1) */
		MIPS32_SW(31,31*4,1), 							/* sw $31,31*4($1) */

		MIPS32_MFC0(2,12,0),							/* move status to $2 */
		MIPS32_SW(2,32*4,1), 							/* sw $2,32*4($1) */
		MIPS32_MFLO(2),									/* move lo to $2 */
		MIPS32_SW(2,33*4,1), 							/* sw $2,33*4($1) */
		MIPS32_MFHI(2),									/* move hi to $2 */
		MIPS32_SW(2,34*4,1), 							/* sw $2,34*4($1) */
		MIPS32_MFC0(2,8,0),								/* move badvaddr to $2 */
		MIPS32_SW(2,35*4,1), 							/* sw $2,35*4($1) */
		MIPS32_MFC0(2,13,0),							/* move cause to $2 */
		MIPS32_SW(2,36*4,1), 							/* sw $2,36*4($1) */
		MIPS32_MFC0(2,24,0),							/* move pc to $2 */
		MIPS32_SW(2,37*4,1), 							/* sw $2,37*4($1) */

		MIPS32_LW(2,0,15), 								/* lw $2,($15) */
		MIPS32_LW(1,0,15), 								/* lw $1,($15) */
		MIPS32_MFC0(15,31,0),							/* move COP0 DeSave to $15 */
		MIPS32_NOP,
		MIPS32_B(NEG16(60)),							/* b start */
		MIPS32_NOP,
	};

	int retval;

	retval = mips32_pracc_exec(ejtag_info, sizeof(code)/sizeof(code[0]), code, \
		0, NULL, 38, regs, 1);

	return retval;
}
