/*
 * local-propagation.c: Local constant, copy and tree propagation.
 *
 * To make some sense of the tree mover, read mono/docs/tree-mover.txt
 *
 * Author:
 *   Paolo Molaro (lupus@ximian.com)
 *   Dietmar Maurer (dietmar@ximian.com)
 *   Massimiliano Mantione (massi@ximian.com)
 *
 * (C) 2006 Novell, Inc.  http://www.novell.com
 * Copyright 2011 Xamarin, Inc (http://www.xamarin.com)
 */

#include <config.h>
#ifndef DISABLE_JIT

#include <string.h>
#include <stdio.h>
#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif

#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/mempool.h>
#include <mono/metadata/opcodes.h>
#include "mini.h"
#include "ir-emit.h"

#ifndef MONO_ARCH_IS_OP_MEMBASE
#define MONO_ARCH_IS_OP_MEMBASE(opcode) FALSE
#endif

static inline MonoBitSet* 
mono_bitset_mp_new_noinit (MonoMemPool *mp,  guint32 max_size)
{
	int size = mono_bitset_alloc_size (max_size, 0);
	gpointer mem;

	mem = mono_mempool_alloc (mp, size);
	return mono_bitset_mem_new (mem, max_size, MONO_BITSET_DONT_FREE);
}

/*
 * Replaces ins with optimized opcodes.
 *
 * We can emit to cbb the equivalent instructions which will be used as
 * replacement for ins, or simply change the fields of ins. Spec needs to
 * be updated if we silently change the opcode of ins.
 *
 * Returns TRUE if additional vregs were allocated.
 */
static gboolean
mono_strength_reduction_ins (MonoCompile *cfg, MonoInst *ins, const char **spec)
{
	gboolean allocated_vregs = FALSE;

	/* FIXME: Add long/float */
	switch (ins->opcode) {
	case OP_MOVE:
	case OP_XMOVE:
		if (ins->dreg == ins->sreg1) {
			NULLIFY_INS (ins);
		}
		break;
	case OP_ADD_IMM:
	case OP_IADD_IMM:
	case OP_SUB_IMM:
	case OP_ISUB_IMM:
#if SIZEOF_REGISTER == 8
	case OP_LADD_IMM:
	case OP_LSUB_IMM:
#endif
		if (ins->inst_imm == 0) {
			ins->opcode = OP_MOVE;
		}
		break;
	case OP_MUL_IMM:
	case OP_IMUL_IMM:
#if SIZEOF_REGISTER == 8
	case OP_LMUL_IMM:
#endif
		if (ins->inst_imm == 0) {
			ins->opcode = (ins->opcode == OP_LMUL_IMM) ? OP_I8CONST : OP_ICONST;
			ins->inst_c0 = 0;
			ins->sreg1 = -1;
		} else if (ins->inst_imm == 1) {
			ins->opcode = OP_MOVE;
		} else if ((ins->opcode == OP_IMUL_IMM) && (ins->inst_imm == -1)) {
			ins->opcode = OP_INEG;
		} else if ((ins->opcode == OP_LMUL_IMM) && (ins->inst_imm == -1)) {
			ins->opcode = OP_LNEG;
		} else {
			int power2 = mono_is_power_of_two (ins->inst_imm);
			if (power2 >= 0) {
				ins->opcode = (ins->opcode == OP_MUL_IMM) ? OP_SHL_IMM : ((ins->opcode == OP_LMUL_IMM) ? OP_LSHL_IMM : OP_ISHL_IMM);
				ins->inst_imm = power2;
			}
		}
		break;
	case OP_IREM_UN_IMM:
	case OP_IDIV_UN_IMM: {
		int c = ins->inst_imm;
		int power2 = mono_is_power_of_two (c);

		if (power2 >= 0) {
			if (ins->opcode == OP_IREM_UN_IMM) {
				ins->opcode = OP_IAND_IMM;
				ins->sreg2 = -1;
				ins->inst_imm = (1 << power2) - 1;
			} else if (ins->opcode == OP_IDIV_UN_IMM) {
				ins->opcode = OP_ISHR_UN_IMM;
				ins->sreg2 = -1;
				ins->inst_imm = power2;
			}
		}
		break;
	}
	case OP_IDIV_IMM: {
		int c = ins->inst_imm;
		int power2 = mono_is_power_of_two (c);

		if (power2 == 1) {
			int r1 = mono_alloc_ireg (cfg);

			MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ISHR_UN_IMM, r1, ins->sreg1, 31);
			MONO_EMIT_NEW_BIALU (cfg, OP_IADD, r1, r1, ins->sreg1);
			MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ISHR_IMM, ins->dreg, r1, 1);

			allocated_vregs = TRUE;
		} else if (power2 > 0 && power2 < 31) {
			int r1 = mono_alloc_ireg (cfg);

			MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ISHR_IMM, r1, ins->sreg1, 31);
			MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ISHR_UN_IMM, r1, r1, (32 - power2));
			MONO_EMIT_NEW_BIALU (cfg, OP_IADD, r1, r1, ins->sreg1);
			MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ISHR_IMM, ins->dreg, r1, power2);

			allocated_vregs = TRUE;
		}
		break;
	}
#if SIZEOF_REGISTER == 8
	case OP_LREM_IMM:
#endif
	case OP_IREM_IMM: {
		int power = mono_is_power_of_two (ins->inst_imm);
		if (ins->inst_imm == 1) {
			ins->opcode = OP_ICONST;
			MONO_INST_NULLIFY_SREGS (ins);
			ins->inst_c0 = 0;
#if __s390__
		}
#else
		} else if ((ins->inst_imm > 0) && (ins->inst_imm < (1LL << 32)) && (power != -1)) {
			gboolean is_long = ins->opcode == OP_LREM_IMM;
			int compensator_reg = alloc_ireg (cfg);
			int intermediate_reg;

			/* Based on gcc code */

			/* Add compensation for negative numerators */

			if (power > 1) {
				intermediate_reg = compensator_reg;
				MONO_EMIT_NEW_BIALU_IMM (cfg, is_long ? OP_LSHR_IMM : OP_ISHR_IMM, intermediate_reg, ins->sreg1, is_long ? 63 : 31);
			} else {
				intermediate_reg = ins->sreg1;
			}

			MONO_EMIT_NEW_BIALU_IMM (cfg, is_long ? OP_LSHR_UN_IMM : OP_ISHR_UN_IMM, compensator_reg, intermediate_reg, (is_long ? 64 : 32) - power);
			MONO_EMIT_NEW_BIALU (cfg, is_long ? OP_LADD : OP_IADD, ins->dreg, ins->sreg1, compensator_reg);
			/* Compute remainder */
			MONO_EMIT_NEW_BIALU_IMM (cfg, is_long ? OP_LAND_IMM : OP_AND_IMM, ins->dreg, ins->dreg, (1 << power) - 1);
			/* Remove compensation */
			MONO_EMIT_NEW_BIALU (cfg, is_long ? OP_LSUB : OP_ISUB, ins->dreg, ins->dreg, compensator_reg);

			allocated_vregs = TRUE;
		}
#endif
		break;
	}

	default:
		break;
	}

	*spec = INS_INFO (ins->opcode);
	return allocated_vregs;
}

/*
 * mono_local_cprop:
 *
 *  A combined local copy and constant propagation pass.
 */
void
mono_local_cprop (MonoCompile *cfg)
{
	MonoBasicBlock *bb, *bb_opt;
	MonoInst **defs;
	gint32 *def_index;
	int max;
	int filter = FILTER_IL_SEQ_POINT;
	int initial_max_vregs = cfg->next_vreg;

	max = cfg->next_vreg;
	defs = (MonoInst **)mono_mempool_alloc (cfg->mempool, sizeof (MonoInst*) * cfg->next_vreg);
	def_index = (gint32 *)mono_mempool_alloc (cfg->mempool, sizeof (guint32) * cfg->next_vreg);
	cfg->cbb = bb_opt = mono_mempool_alloc0 ((cfg)->mempool, sizeof (MonoBasicBlock));

	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		MonoInst *ins;
		int ins_index;
		int last_call_index;

		/* Manually init the defs entries used by the bblock */
		MONO_BB_FOR_EACH_INS (bb, ins) {
			int sregs [MONO_MAX_SRC_REGS];
			int num_sregs, i;

			if (ins->dreg != -1) {
#if SIZEOF_REGISTER == 4
				const char *spec = INS_INFO (ins->opcode);
				if (spec [MONO_INST_DEST] == 'l') {
					defs [ins->dreg + 1] = NULL;
					defs [ins->dreg + 2] = NULL;
				}
#endif
				defs [ins->dreg] = NULL;
			}

			num_sregs = mono_inst_get_src_registers (ins, sregs);
			for (i = 0; i < num_sregs; ++i) {
				int sreg = sregs [i];
#if SIZEOF_REGISTER == 4
				const char *spec = INS_INFO (ins->opcode);
				if (spec [MONO_INST_SRC1 + i] == 'l') {
					defs [sreg + 1] = NULL;
					defs [sreg + 2] = NULL;
				}
#endif
				defs [sreg] = NULL;
			}
		}

		ins_index = 0;
		last_call_index = -1;
		MONO_BB_FOR_EACH_INS (bb, ins) {
			const char *spec = INS_INFO (ins->opcode);
			int regtype, srcindex, sreg;
			int num_sregs;
			int sregs [MONO_MAX_SRC_REGS];

			if (ins->opcode == OP_NOP) {
				MONO_DELETE_INS (bb, ins);
				continue;
			}

			g_assert (ins->opcode > MONO_CEE_LAST);

			/* FIXME: Optimize this */
			if (ins->opcode == OP_LDADDR) {
				MonoInst *var = (MonoInst *)ins->inst_p0;

				defs [var->dreg] = NULL;
				/*
				if (!MONO_TYPE_ISSTRUCT (var->inst_vtype))
					break;
				*/
			}

			if (MONO_IS_STORE_MEMBASE (ins)) {
				sreg = ins->dreg;
				regtype = 'i';

				if ((regtype == 'i') && (sreg != -1) && defs [sreg]) {
					MonoInst *def = defs [sreg];

					if ((def->opcode == OP_MOVE) && (!defs [def->sreg1] || (def_index [def->sreg1] < def_index [sreg])) && !vreg_is_volatile (cfg, def->sreg1)) {
						int vreg = def->sreg1;
						if (cfg->verbose_level > 2) printf ("CCOPY: R%d -> R%d\n", sreg, vreg);
						ins->dreg = vreg;
					}
				}
			}

			num_sregs = mono_inst_get_src_registers (ins, sregs);
			for (srcindex = 0; srcindex < num_sregs; ++srcindex) {
				MonoInst *def;

				mono_inst_get_src_registers (ins, sregs);

				regtype = spec [MONO_INST_SRC1 + srcindex];
				sreg = sregs [srcindex];

				if ((regtype == ' ') || (sreg == -1) || (!defs [sreg]))
					continue;

				def = defs [sreg];

				/* Copy propagation */
				/* 
				 * The first check makes sure the source of the copy did not change since 
				 * the copy was made.
				 * The second check avoids volatile variables.
				 * The third check avoids copy propagating local vregs through a call, 
				 * since the lvreg will be spilled 
				 * The fourth check avoids copy propagating a vreg in cases where
				 * it would be eliminated anyway by reverse copy propagation later,
				 * because propagating it would create another use for it, thus making 
				 * it impossible to use reverse copy propagation.
				 */
				/* Enabling this for floats trips up the fp stack */
				/* 
				 * Enabling this for floats on amd64 seems to cause a failure in 
				 * basic-math.cs, most likely because it gets rid of some r8->r4 
				 * conversions.
				 */
				if (MONO_IS_MOVE (def) &&
					(!defs [def->sreg1] || (def_index [def->sreg1] < def_index [sreg])) &&
					!vreg_is_volatile (cfg, def->sreg1) &&
					/* This avoids propagating local vregs across calls */
					((get_vreg_to_inst (cfg, def->sreg1) || !defs [def->sreg1] || (def_index [def->sreg1] >= last_call_index) || (def->opcode == OP_VMOVE))) &&
					!(defs [def->sreg1] && mono_inst_next (defs [def->sreg1], filter) == def) &&
					(!MONO_ARCH_USE_FPSTACK || (def->opcode != OP_FMOVE)) &&
					(def->opcode != OP_FMOVE)) {
					int vreg = def->sreg1;

					if (cfg->verbose_level > 2) printf ("CCOPY/2: R%d -> R%d\n", sreg, vreg);
					sregs [srcindex] = vreg;
					mono_inst_set_src_registers (ins, sregs);

					/* Allow further iterations */
					srcindex = -1;
					continue;
				}

				/* Constant propagation */
				/* FIXME: Make is_inst_imm a macro */
				/* FIXME: Make is_inst_imm take an opcode argument */
				/* is_inst_imm is only needed for binops */
				if ((((def->opcode == OP_ICONST) || ((sizeof (gpointer) == 8) && (def->opcode == OP_I8CONST))) &&
					 (((srcindex == 0) && (ins->sreg2 == -1)) || mono_arch_is_inst_imm (def->inst_c0))) || 
					(!MONO_ARCH_USE_FPSTACK && (def->opcode == OP_R8CONST))) {
					guint32 opcode2;

					/* srcindex == 1 -> binop, ins->sreg2 == -1 -> unop */
					if ((srcindex == 1) && (ins->sreg1 != -1) && defs [ins->sreg1] && (defs [ins->sreg1]->opcode == OP_ICONST) && defs [ins->sreg2]) {
						/* Both arguments are constants, perform cfold */
						mono_constant_fold_ins (cfg, ins, defs [ins->sreg1], defs [ins->sreg2], TRUE);
					} else if ((srcindex == 0) && (ins->sreg2 != -1) && defs [ins->sreg2]) {
						/* Arg 1 is constant, swap arguments if possible */
						int opcode = ins->opcode;
						mono_constant_fold_ins (cfg, ins, defs [ins->sreg1], defs [ins->sreg2], TRUE);
						if (ins->opcode != opcode) {
							/* Allow further iterations */
							srcindex = -1;
							continue;
						}
					} else if ((srcindex == 0) && (ins->sreg2 == -1)) {
						/* Constant unop, perform cfold */
						mono_constant_fold_ins (cfg, ins, defs [ins->sreg1], NULL, TRUE);
					}

					opcode2 = mono_op_to_op_imm (ins->opcode);
					if ((opcode2 != -1) && mono_arch_is_inst_imm (def->inst_c0) && ((srcindex == 1) || (ins->sreg2 == -1))) {
						ins->opcode = opcode2;
						if ((def->opcode == OP_I8CONST) && (sizeof (gpointer) == 4)) {
							ins->inst_ls_word = def->inst_ls_word;
							ins->inst_ms_word = def->inst_ms_word;
						} else {
							ins->inst_imm = def->inst_c0;
						}
						sregs [srcindex] = -1;
						mono_inst_set_src_registers (ins, sregs);

						if ((opcode2 == OP_VOIDCALL) || (opcode2 == OP_CALL) || (opcode2 == OP_LCALL) || (opcode2 == OP_FCALL))
							((MonoCallInst*)ins)->fptr = (gpointer)ins->inst_imm;

						/* Allow further iterations */
						srcindex = -1;
						continue;
					}
					else {
						/* Special cases */
#if defined(TARGET_X86) || defined(TARGET_AMD64)
						if ((ins->opcode == OP_X86_LEA) && (srcindex == 1)) {
#if SIZEOF_REGISTER == 8
							/* FIXME: Use OP_PADD_IMM when the new JIT is done */
							ins->opcode = OP_LADD_IMM;
#else
							ins->opcode = OP_ADD_IMM;
#endif
							ins->inst_imm += def->inst_c0 << ins->backend.shift_amount;
							ins->sreg2 = -1;
						}
#endif
						opcode2 = mono_load_membase_to_load_mem (ins->opcode);
						if ((srcindex == 0) && (opcode2 != -1) && mono_arch_is_inst_imm (def->inst_c0)) {
							ins->opcode = opcode2;
							ins->inst_imm = def->inst_c0 + ins->inst_offset;
							ins->sreg1 = -1;
						}
					}
				}
				else if (((def->opcode == OP_ADD_IMM) || (def->opcode == OP_LADD_IMM)) && (MONO_IS_LOAD_MEMBASE (ins) || MONO_ARCH_IS_OP_MEMBASE (ins->opcode))) {
					/* ADD_IMM is created by spill_global_vars */
					/* 
					 * We have to guarantee that def->sreg1 haven't changed since def->dreg
					 * was defined. cfg->frame_reg is assumed to remain constant.
					 */
					if ((def->sreg1 == cfg->frame_reg) || ((mono_inst_next (def, filter) == ins) && (def->dreg != def->sreg1))) {
						ins->inst_basereg = def->sreg1;
						ins->inst_offset += def->inst_imm;
					}
				} else if ((ins->opcode == OP_ISUB_IMM) && (def->opcode == OP_IADD_IMM) && (mono_inst_next (def, filter) == ins) && (def->dreg != def->sreg1)) {
					ins->sreg1 = def->sreg1;
					ins->inst_imm -= def->inst_imm;
				} else if ((ins->opcode == OP_IADD_IMM) && (def->opcode == OP_ISUB_IMM) && (mono_inst_next (def, filter) == ins) && (def->dreg != def->sreg1)) {
					ins->sreg1 = def->sreg1;
					ins->inst_imm -= def->inst_imm;
				} else if (ins->opcode == OP_STOREI1_MEMBASE_REG &&
						   (def->opcode == OP_ICONV_TO_U1 || def->opcode == OP_ICONV_TO_I1 || def->opcode == OP_SEXT_I4 || (SIZEOF_REGISTER == 8 && def->opcode == OP_LCONV_TO_U1)) &&
						   (!defs [def->sreg1] || (def_index [def->sreg1] < def_index [sreg]))) {
					/* Avoid needless sign extension */
					ins->sreg1 = def->sreg1;
				} else if (ins->opcode == OP_STOREI2_MEMBASE_REG &&
						   (def->opcode == OP_ICONV_TO_U2 || def->opcode == OP_ICONV_TO_I2 || def->opcode == OP_SEXT_I4 || (SIZEOF_REGISTER == 8 && def->opcode == OP_LCONV_TO_I2)) &&
						   (!defs [def->sreg1] || (def_index [def->sreg1] < def_index [sreg]))) {
					/* Avoid needless sign extension */
					ins->sreg1 = def->sreg1;
				} else if (ins->opcode == OP_COMPARE_IMM && def->opcode == OP_LDADDR && ins->inst_imm == 0) {
					MonoInst dummy_arg1;

					memset (&dummy_arg1, 0, sizeof (MonoInst));
					dummy_arg1.opcode = OP_ICONST;
					dummy_arg1.inst_c0 = 1;

					mono_constant_fold_ins (cfg, ins, &dummy_arg1, NULL, TRUE);
				}
			}

			g_assert (cfg->cbb == bb_opt);
			g_assert (!bb_opt->code);
			/* Do strength reduction here */
			if (mono_strength_reduction_ins (cfg, ins, &spec) && max < cfg->next_vreg) {
				MonoInst **defs_prev = defs;
				gint32 *def_index_prev = def_index;
				guint32 prev_max = max;
				guint32 additional_vregs = cfg->next_vreg - initial_max_vregs;

				/* We have more vregs so we need to reallocate defs and def_index arrays */
				max  = initial_max_vregs + additional_vregs * 2;
				defs = (MonoInst **)mono_mempool_alloc (cfg->mempool, sizeof (MonoInst*) * max);
				def_index = (gint32 *)mono_mempool_alloc (cfg->mempool, sizeof (guint32) * max);

				/* Keep the entries for the previous vregs, zero the rest */
				memcpy (defs, defs_prev, sizeof (MonoInst*) * prev_max);
				memset (defs + prev_max, 0, sizeof (MonoInst*) * (max - prev_max));
				memcpy (def_index, def_index_prev, sizeof (guint32) * prev_max);
				memset (def_index + prev_max, 0, sizeof (guint32) * (max - prev_max));
			}

			if (cfg->cbb->code || (cfg->cbb != bb_opt)) {
				MonoInst *saved_prev = ins->prev;

				/* If we have code in cbb, we need to replace ins with the decomposition */
				mono_replace_ins (cfg, bb, ins, &ins->prev, bb_opt, cfg->cbb);
				bb_opt->code = bb_opt->last_ins = NULL;
				bb_opt->in_count = bb_opt->out_count = 0;
				cfg->cbb = bb_opt;

				/* ins is hanging, continue scanning the emitted code */
				ins = saved_prev;
				continue;
			}

			if (spec [MONO_INST_DEST] != ' ') {
				MonoInst *def = defs [ins->dreg];

				if (def && (def->opcode == OP_ADD_IMM) && (def->sreg1 == cfg->frame_reg) && (MONO_IS_STORE_MEMBASE (ins))) {
					/* ADD_IMM is created by spill_global_vars */
					/* cfg->frame_reg is assumed to remain constant */
					ins->inst_destbasereg = def->sreg1;
					ins->inst_offset += def->inst_imm;
				}
			}
			
			if ((spec [MONO_INST_DEST] != ' ') && !MONO_IS_STORE_MEMBASE (ins) && !vreg_is_volatile (cfg, ins->dreg)) {
				defs [ins->dreg] = ins;
				def_index [ins->dreg] = ins_index;
			}

			if (MONO_IS_CALL (ins))
				last_call_index = ins_index;

			ins_index ++;
		}
	}
}

static inline gboolean
reg_is_softreg_no_fpstack (int reg, const char spec)
{
	return (spec == 'i' && reg >= MONO_MAX_IREGS)
		|| ((spec == 'f' && reg >= MONO_MAX_FREGS) && !MONO_ARCH_USE_FPSTACK)
#ifdef MONO_ARCH_SIMD_INTRINSICS
		|| (spec == 'x' && reg >= MONO_MAX_XREGS)
#endif
		|| (spec == 'v');
}
		
static inline gboolean
reg_is_softreg (int reg, const char spec)
{
	return (spec == 'i' && reg >= MONO_MAX_IREGS)
		|| (spec == 'f' && reg >= MONO_MAX_FREGS)
#ifdef MONO_ARCH_SIMD_INTRINSICS
		|| (spec == 'x' && reg >= MONO_MAX_XREGS)
#endif
		|| (spec == 'v');
}

static inline gboolean
mono_is_simd_accessor (MonoInst *ins)
{
	switch (ins->opcode) {
#ifdef MONO_ARCH_SIMD_INTRINSICS
	case OP_INSERT_I1:
	case OP_INSERT_I2:
	case OP_INSERT_I4:
	case OP_INSERT_I8:
	case OP_INSERT_R4:
	case OP_INSERT_R8:

	case OP_INSERTX_U1_SLOW:
	case OP_INSERTX_I4_SLOW:
	case OP_INSERTX_R4_SLOW:
	case OP_INSERTX_R8_SLOW:
	case OP_INSERTX_I8_SLOW:
		return TRUE;
#endif
	default:
		return FALSE;
	}
}

/**
 * mono_local_deadce:
 *
 *   Get rid of the dead assignments to local vregs like the ones created by the 
 * copyprop pass.
 */
void
mono_local_deadce (MonoCompile *cfg)
{
	MonoBasicBlock *bb;
	MonoInst *ins, *prev;
	MonoBitSet *used, *defined;

	//mono_print_code (cfg, "BEFORE LOCAL-DEADCE");

	/*
	 * Assignments to global vregs can't be eliminated so this pass must come
	 * after the handle_global_vregs () pass.
	 */

	used = mono_bitset_mp_new_noinit (cfg->mempool, cfg->next_vreg + 1);
	defined = mono_bitset_mp_new_noinit (cfg->mempool, cfg->next_vreg + 1);

	/* First pass: collect liveness info */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		/* Manually init the defs entries used by the bblock */
		MONO_BB_FOR_EACH_INS (bb, ins) {
			const char *spec = INS_INFO (ins->opcode);
			int sregs [MONO_MAX_SRC_REGS];
			int num_sregs, i;

			if (spec [MONO_INST_DEST] != ' ') {
				mono_bitset_clear_fast (used, ins->dreg);
				mono_bitset_clear_fast (defined, ins->dreg);
#if SIZEOF_REGISTER == 4
				/* Regpairs */
				mono_bitset_clear_fast (used, ins->dreg + 1);
				mono_bitset_clear_fast (defined, ins->dreg + 1);
#endif
			}
			num_sregs = mono_inst_get_src_registers (ins, sregs);
			for (i = 0; i < num_sregs; ++i) {
				mono_bitset_clear_fast (used, sregs [i]);
#if SIZEOF_REGISTER == 4
				mono_bitset_clear_fast (used, sregs [i] + 1);
#endif
			}
		}

		/*
		 * Make a reverse pass over the instruction list
		 */
		MONO_BB_FOR_EACH_INS_REVERSE_SAFE (bb, prev, ins) {
			const char *spec = INS_INFO (ins->opcode);
			int sregs [MONO_MAX_SRC_REGS];
			int num_sregs, i;
			MonoInst *prev_f = mono_inst_prev (ins, FILTER_NOP | FILTER_IL_SEQ_POINT);

			if (ins->opcode == OP_NOP) {
				MONO_DELETE_INS (bb, ins);
				continue;
			}

			g_assert (ins->opcode > MONO_CEE_LAST);

			if (MONO_IS_NON_FP_MOVE (ins) && prev_f) {
				MonoInst *def;
				const char *spec2;

				def = prev_f;
				spec2 = INS_INFO (def->opcode);

				/* 
				 * Perform a limited kind of reverse copy propagation, i.e.
				 * transform B <- FOO; A <- B into A <- FOO
				 * This isn't copyprop, not deadce, but it can only be performed
				 * after handle_global_vregs () has run.
				 */
				if (!get_vreg_to_inst (cfg, ins->sreg1) && (spec2 [MONO_INST_DEST] != ' ') && (def->dreg == ins->sreg1) && !mono_bitset_test_fast (used, ins->sreg1) && !MONO_IS_STORE_MEMBASE (def) && reg_is_softreg (ins->sreg1, spec [MONO_INST_DEST]) && !mono_is_simd_accessor (def)) {
					if (cfg->verbose_level > 2) {
						printf ("\tReverse copyprop in BB%d on ", bb->block_num);
						mono_print_ins (ins);
					}

					def->dreg = ins->dreg;
					MONO_DELETE_INS (bb, ins);
					spec = INS_INFO (ins->opcode);
				}
			}

			/* Enabling this on x86 could screw up the fp stack */
			if (reg_is_softreg_no_fpstack (ins->dreg, spec [MONO_INST_DEST])) {
				/* 
				 * Assignments to global vregs can only be eliminated if there is another
				 * assignment to the same vreg later in the same bblock.
				 */
				if (!mono_bitset_test_fast (used, ins->dreg) && 
					(!get_vreg_to_inst (cfg, ins->dreg) || (!bb->extended && !vreg_is_volatile (cfg, ins->dreg) && mono_bitset_test_fast (defined, ins->dreg))) &&
					MONO_INS_HAS_NO_SIDE_EFFECT (ins)) {
					/* Happens with CMOV instructions */
					if (prev_f && prev_f->opcode == OP_ICOMPARE_IMM) {
						MonoInst *prev = prev_f;
						/* 
						 * Can't use DELETE_INS since that would interfere with the
						 * FOR_EACH_INS loop.
						 */
						NULLIFY_INS (prev);
					}
					//printf ("DEADCE: "); mono_print_ins (ins);
					MONO_DELETE_INS (bb, ins);
					spec = INS_INFO (ins->opcode);
				}

				if (spec [MONO_INST_DEST] != ' ')
					mono_bitset_clear_fast (used, ins->dreg);
			}

			if (spec [MONO_INST_DEST] != ' ')
				mono_bitset_set_fast (defined, ins->dreg);
			num_sregs = mono_inst_get_src_registers (ins, sregs);
			for (i = 0; i < num_sregs; ++i)
				mono_bitset_set_fast (used, sregs [i]);
			if (MONO_IS_STORE_MEMBASE (ins))
				mono_bitset_set_fast (used, ins->dreg);

			if (MONO_IS_CALL (ins)) {
				MonoCallInst *call = (MonoCallInst*)ins;
				GSList *l;

				if (call->out_ireg_args) {
					for (l = call->out_ireg_args; l; l = l->next) {
						guint32 regpair, reg;

						regpair = (guint32)(gssize)(l->data);
						reg = regpair & 0xffffff;
					
						mono_bitset_set_fast (used, reg);
					}
				}

				if (call->out_freg_args) {
					for (l = call->out_freg_args; l; l = l->next) {
						guint32 regpair, reg;

						regpair = (guint32)(gssize)(l->data);
						reg = regpair & 0xffffff;
					
						mono_bitset_set_fast (used, reg);
					}
				}
			}
		}
	}

	//mono_print_code (cfg, "AFTER LOCAL-DEADCE");
}

#endif /* DISABLE_JIT */
