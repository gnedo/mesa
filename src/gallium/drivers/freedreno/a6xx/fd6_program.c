/*
 * Copyright (C) 2016 Rob Clark <robclark@freedesktop.org>
 * Copyright © 2018 Google, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "pipe/p_state.h"
#include "util/u_string.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "util/bitset.h"

#include "freedreno_program.h"

#include "fd6_program.h"
#include "fd6_emit.h"
#include "fd6_texture.h"
#include "fd6_format.h"

static struct ir3_shader *
create_shader_stateobj(struct pipe_context *pctx, const struct pipe_shader_state *cso,
		gl_shader_stage type)
{
	struct fd_context *ctx = fd_context(pctx);
	struct ir3_compiler *compiler = ctx->screen->compiler;
	return ir3_shader_create(compiler, cso, type, &ctx->debug, pctx->screen);
}

static void *
fd6_fp_state_create(struct pipe_context *pctx,
		const struct pipe_shader_state *cso)
{
	return create_shader_stateobj(pctx, cso, MESA_SHADER_FRAGMENT);
}

static void
fd6_fp_state_delete(struct pipe_context *pctx, void *hwcso)
{
	struct ir3_shader *so = hwcso;
	struct fd_context *ctx = fd_context(pctx);
	ir3_cache_invalidate(fd6_context(ctx)->shader_cache, hwcso);
	ir3_shader_destroy(so);
}

static void *
fd6_vp_state_create(struct pipe_context *pctx,
		const struct pipe_shader_state *cso)
{
	return create_shader_stateobj(pctx, cso, MESA_SHADER_VERTEX);
}

static void
fd6_vp_state_delete(struct pipe_context *pctx, void *hwcso)
{
	struct ir3_shader *so = hwcso;
	struct fd_context *ctx = fd_context(pctx);
	ir3_cache_invalidate(fd6_context(ctx)->shader_cache, hwcso);
	ir3_shader_destroy(so);
}

void
fd6_emit_shader(struct fd_ringbuffer *ring, const struct ir3_shader_variant *so)
{
	enum a6xx_state_block sb = fd6_stage2shadersb(so->type);

	OUT_PKT7(ring, fd6_stage2opcode(so->type), 3);
	OUT_RING(ring, CP_LOAD_STATE6_0_DST_OFF(0) |
			CP_LOAD_STATE6_0_STATE_TYPE(ST6_SHADER) |
			CP_LOAD_STATE6_0_STATE_SRC(SS6_INDIRECT) |
			CP_LOAD_STATE6_0_STATE_BLOCK(sb) |
			CP_LOAD_STATE6_0_NUM_UNIT(so->instrlen));
	OUT_RELOCD(ring, so->bo, 0, 0, 0);
}

/* Add any missing varyings needed for stream-out.  Otherwise varyings not
 * used by fragment shader will be stripped out.
 */
static void
link_stream_out(struct ir3_shader_linkage *l, const struct ir3_shader_variant *v)
{
	const struct ir3_stream_output_info *strmout = &v->shader->stream_output;

	/*
	 * First, any stream-out varyings not already in linkage map (ie. also
	 * consumed by frag shader) need to be added:
	 */
	for (unsigned i = 0; i < strmout->num_outputs; i++) {
		const struct ir3_stream_output *out = &strmout->output[i];
		unsigned k = out->register_index;
		unsigned compmask =
			(1 << (out->num_components + out->start_component)) - 1;
		unsigned idx, nextloc = 0;

		/* psize/pos need to be the last entries in linkage map, and will
		 * get added link_stream_out, so skip over them:
		 */
		if ((v->outputs[k].slot == VARYING_SLOT_PSIZ) ||
				(v->outputs[k].slot == VARYING_SLOT_POS))
			continue;

		for (idx = 0; idx < l->cnt; idx++) {
			if (l->var[idx].regid == v->outputs[k].regid)
				break;
			nextloc = MAX2(nextloc, l->var[idx].loc + 4);
		}

		/* add if not already in linkage map: */
		if (idx == l->cnt)
			ir3_link_add(l, v->outputs[k].regid, compmask, nextloc);

		/* expand component-mask if needed, ie streaming out all components
		 * but frag shader doesn't consume all components:
		 */
		if (compmask & ~l->var[idx].compmask) {
			l->var[idx].compmask |= compmask;
			l->max_loc = MAX2(l->max_loc,
				l->var[idx].loc + util_last_bit(l->var[idx].compmask));
		}
	}
}

static void
setup_stream_out(struct fd6_program_state *state, const struct ir3_shader_variant *v,
		struct ir3_shader_linkage *l)
{
	const struct ir3_stream_output_info *strmout = &v->shader->stream_output;
	struct fd6_streamout_state *tf = &state->tf;

	memset(tf, 0, sizeof(*tf));

	tf->prog_count = align(l->max_loc, 2) / 2;

	debug_assert(tf->prog_count < ARRAY_SIZE(tf->prog));

	for (unsigned i = 0; i < strmout->num_outputs; i++) {
		const struct ir3_stream_output *out = &strmout->output[i];
		unsigned k = out->register_index;
		unsigned idx;

		tf->ncomp[out->output_buffer] += out->num_components;

		/* linkage map sorted by order frag shader wants things, so
		 * a bit less ideal here..
		 */
		for (idx = 0; idx < l->cnt; idx++)
			if (l->var[idx].regid == v->outputs[k].regid)
				break;

		debug_assert(idx < l->cnt);

		for (unsigned j = 0; j < out->num_components; j++) {
			unsigned c   = j + out->start_component;
			unsigned loc = l->var[idx].loc + c;
			unsigned off = j + out->dst_offset;  /* in dwords */

			if (loc & 1) {
				tf->prog[loc/2] |= A6XX_VPC_SO_PROG_B_EN |
						A6XX_VPC_SO_PROG_B_BUF(out->output_buffer) |
						A6XX_VPC_SO_PROG_B_OFF(off * 4);
			} else {
				tf->prog[loc/2] |= A6XX_VPC_SO_PROG_A_EN |
						A6XX_VPC_SO_PROG_A_BUF(out->output_buffer) |
						A6XX_VPC_SO_PROG_A_OFF(off * 4);
			}
		}
	}

	tf->vpc_so_buf_cntl = A6XX_VPC_SO_BUF_CNTL_ENABLE |
			COND(tf->ncomp[0] > 0, A6XX_VPC_SO_BUF_CNTL_BUF0) |
			COND(tf->ncomp[1] > 0, A6XX_VPC_SO_BUF_CNTL_BUF1) |
			COND(tf->ncomp[2] > 0, A6XX_VPC_SO_BUF_CNTL_BUF2) |
			COND(tf->ncomp[3] > 0, A6XX_VPC_SO_BUF_CNTL_BUF3);
}

static void
setup_config_stateobj(struct fd_ringbuffer *ring, struct fd6_program_state *state)
{
	OUT_PKT4(ring, REG_A6XX_HLSQ_UPDATE_CNTL, 1);
	OUT_RING(ring, 0xff);        /* XXX */

	debug_assert(state->vs->constlen >= state->bs->constlen);

	OUT_PKT4(ring, REG_A6XX_HLSQ_VS_CNTL, 4);
	OUT_RING(ring, A6XX_HLSQ_VS_CNTL_CONSTLEN(align(state->vs->constlen, 4)) |
			A6XX_HLSQ_VS_CNTL_ENABLED);
	OUT_RING(ring, A6XX_HLSQ_HS_CNTL_CONSTLEN(0));
	OUT_RING(ring, A6XX_HLSQ_DS_CNTL_CONSTLEN(0));
	OUT_RING(ring, A6XX_HLSQ_GS_CNTL_CONSTLEN(0));

	OUT_PKT4(ring, REG_A6XX_HLSQ_FS_CNTL, 1);
	OUT_RING(ring, A6XX_HLSQ_FS_CNTL_CONSTLEN(align(state->fs->constlen, 4)) |
			A6XX_HLSQ_FS_CNTL_ENABLED);

	OUT_PKT4(ring, REG_A6XX_SP_VS_CONFIG, 1);
	OUT_RING(ring, COND(state->vs, A6XX_SP_VS_CONFIG_ENABLED) |
			A6XX_SP_VS_CONFIG_NIBO(state->vs->image_mapping.num_ibo) |
			A6XX_SP_VS_CONFIG_NTEX(state->vs->num_samp) |
			A6XX_SP_VS_CONFIG_NSAMP(state->vs->num_samp));

	OUT_PKT4(ring, REG_A6XX_SP_FS_CONFIG, 1);
	OUT_RING(ring, COND(state->fs, A6XX_SP_FS_CONFIG_ENABLED) |
			A6XX_SP_FS_CONFIG_NIBO(state->fs->image_mapping.num_ibo) |
			A6XX_SP_FS_CONFIG_NTEX(state->fs->num_samp) |
			A6XX_SP_FS_CONFIG_NSAMP(state->fs->num_samp));

	OUT_PKT4(ring, REG_A6XX_SP_HS_CONFIG, 1);
	OUT_RING(ring, COND(false, A6XX_SP_HS_CONFIG_ENABLED));

	OUT_PKT4(ring, REG_A6XX_SP_DS_CONFIG, 1);
	OUT_RING(ring, COND(false, A6XX_SP_DS_CONFIG_ENABLED));

	OUT_PKT4(ring, REG_A6XX_SP_GS_CONFIG, 1);
	OUT_RING(ring, COND(false, A6XX_SP_GS_CONFIG_ENABLED));

	OUT_PKT4(ring, REG_A6XX_SP_IBO_COUNT, 1);
	OUT_RING(ring, state->fs->image_mapping.num_ibo);
}

#define VALIDREG(r)      ((r) != regid(63,0))
#define CONDREG(r, val)  COND(VALIDREG(r), (val))

static inline uint32_t
next_regid(uint32_t reg, uint32_t increment)
{
	if (VALIDREG(reg))
		return reg + increment;
	else
		return regid(63,0);
}

static void
setup_stateobj(struct fd_ringbuffer *ring, struct fd6_program_state *state,
		const struct ir3_shader_key *key, bool binning_pass)
{
	uint32_t pos_regid, psize_regid, color_regid[8], posz_regid;
	uint32_t face_regid, coord_regid, zwcoord_regid, samp_id_regid;
	uint32_t smask_in_regid, smask_regid;
	uint32_t vertex_regid, instance_regid;
	uint32_t ij_pix_regid, ij_samp_regid, ij_cent_regid, ij_size_regid;
	enum a3xx_threadsize fssz;
	uint8_t psize_loc = ~0;
	int i, j;

	static const struct ir3_shader_variant dummy_fs = {0};
	const struct ir3_shader_variant *vs = binning_pass ? state->bs : state->vs;
	const struct ir3_shader_variant *fs = binning_pass ? &dummy_fs : state->fs;

	bool sample_shading = fs->per_samp | key->sample_shading;

	fssz = FOUR_QUADS;

	pos_regid = ir3_find_output_regid(vs, VARYING_SLOT_POS);
	psize_regid = ir3_find_output_regid(vs, VARYING_SLOT_PSIZ);
	vertex_regid = ir3_find_sysval_regid(vs, SYSTEM_VALUE_VERTEX_ID);
	instance_regid = ir3_find_sysval_regid(vs, SYSTEM_VALUE_INSTANCE_ID);

	if (fs->color0_mrt) {
		color_regid[0] = color_regid[1] = color_regid[2] = color_regid[3] =
		color_regid[4] = color_regid[5] = color_regid[6] = color_regid[7] =
			ir3_find_output_regid(fs, FRAG_RESULT_COLOR);
	} else {
		color_regid[0] = ir3_find_output_regid(fs, FRAG_RESULT_DATA0);
		color_regid[1] = ir3_find_output_regid(fs, FRAG_RESULT_DATA1);
		color_regid[2] = ir3_find_output_regid(fs, FRAG_RESULT_DATA2);
		color_regid[3] = ir3_find_output_regid(fs, FRAG_RESULT_DATA3);
		color_regid[4] = ir3_find_output_regid(fs, FRAG_RESULT_DATA4);
		color_regid[5] = ir3_find_output_regid(fs, FRAG_RESULT_DATA5);
		color_regid[6] = ir3_find_output_regid(fs, FRAG_RESULT_DATA6);
		color_regid[7] = ir3_find_output_regid(fs, FRAG_RESULT_DATA7);
	}

	samp_id_regid   = ir3_find_sysval_regid(fs, SYSTEM_VALUE_SAMPLE_ID);
	smask_in_regid  = ir3_find_sysval_regid(fs, SYSTEM_VALUE_SAMPLE_MASK_IN);
	face_regid      = ir3_find_sysval_regid(fs, SYSTEM_VALUE_FRONT_FACE);
	coord_regid     = ir3_find_sysval_regid(fs, SYSTEM_VALUE_FRAG_COORD);
	zwcoord_regid   = next_regid(coord_regid, 2);
	ij_pix_regid    = ir3_find_sysval_regid(fs, SYSTEM_VALUE_BARYCENTRIC_PIXEL);
	ij_samp_regid   = ir3_find_sysval_regid(fs, SYSTEM_VALUE_BARYCENTRIC_SAMPLE);
	ij_cent_regid   = ir3_find_sysval_regid(fs, SYSTEM_VALUE_BARYCENTRIC_CENTROID);
	ij_size_regid   = ir3_find_sysval_regid(fs, SYSTEM_VALUE_BARYCENTRIC_SIZE);
	posz_regid      = ir3_find_output_regid(fs, FRAG_RESULT_DEPTH);
	smask_regid     = ir3_find_output_regid(fs, FRAG_RESULT_SAMPLE_MASK);

	/* we can't write gl_SampleMask for !msaa..  if b0 is zero then we
	 * end up masking the single sample!!
	 */
	if (!key->msaa)
		smask_regid = regid(63, 0);

	/* we could probably divide this up into things that need to be
	 * emitted if frag-prog is dirty vs if vert-prog is dirty..
	 */

	OUT_PKT4(ring, REG_A6XX_SP_VS_INSTRLEN, 1);
	OUT_RING(ring, vs->instrlen);							/* SP_VS_INSTRLEN */

	OUT_PKT4(ring, REG_A6XX_SP_HS_UNKNOWN_A831, 1);
	OUT_RING(ring, 0);

	OUT_PKT4(ring, REG_A6XX_SP_HS_INSTRLEN, 1);
	OUT_RING(ring, 0);										/* SP_HS_INSTRLEN */

	OUT_PKT4(ring, REG_A6XX_SP_DS_INSTRLEN, 1);
	OUT_RING(ring, 0);										/* SP_DS_INSTRLEN */

	OUT_PKT4(ring, REG_A6XX_SP_GS_UNKNOWN_A871, 1);
	OUT_RING(ring, 0);

	OUT_PKT4(ring, REG_A6XX_SP_GS_INSTRLEN, 1);
	OUT_RING(ring, 0);										/* SP_GS_INSTRLEN */

	/* I believe this is related to pre-dispatch texture fetch.. we probably
	 * should't turn it on by accident:
	 */
	OUT_PKT4(ring, REG_A6XX_SP_UNKNOWN_A99E, 1);
	OUT_RING(ring, 0x0);

	OUT_PKT4(ring, REG_A6XX_SP_UNKNOWN_A9A8, 1);
	OUT_RING(ring, 0);

	OUT_PKT4(ring, REG_A6XX_SP_UNKNOWN_AB00, 1);
	OUT_RING(ring, 0x5);

	OUT_PKT4(ring, REG_A6XX_SP_FS_INSTRLEN, 1);
	OUT_RING(ring, fs->instrlen);							  /* SP_FS_INSTRLEN */

	OUT_PKT4(ring, REG_A6XX_SP_FS_OUTPUT_CNTL0, 1);
	OUT_RING(ring, A6XX_SP_FS_OUTPUT_CNTL0_DEPTH_REGID(posz_regid) |
			 A6XX_SP_FS_OUTPUT_CNTL0_SAMPMASK_REGID(smask_regid) |
			 0xfc000000);

	OUT_PKT4(ring, REG_A6XX_SP_VS_CTRL_REG0, 1);
	OUT_RING(ring, A6XX_SP_VS_CTRL_REG0_THREADSIZE(fssz) |
			A6XX_SP_VS_CTRL_REG0_FULLREGFOOTPRINT(vs->info.max_reg + 1) |
			A6XX_SP_VS_CTRL_REG0_MERGEDREGS |
			A6XX_SP_VS_CTRL_REG0_BRANCHSTACK(vs->branchstack) |
			COND(vs->need_pixlod, A6XX_SP_VS_CTRL_REG0_PIXLODENABLE));

	struct ir3_shader_linkage l = {0};
	ir3_link_shaders(&l, vs, fs);

	if ((vs->shader->stream_output.num_outputs > 0) && !binning_pass)
		link_stream_out(&l, vs);

	BITSET_DECLARE(varbs, 128) = {0};
	uint32_t *varmask = (uint32_t *)varbs;

	for (i = 0; i < l.cnt; i++)
		for (j = 0; j < util_last_bit(l.var[i].compmask); j++)
			BITSET_SET(varbs, l.var[i].loc + j);

	OUT_PKT4(ring, REG_A6XX_VPC_VAR_DISABLE(0), 4);
	OUT_RING(ring, ~varmask[0]);  /* VPC_VAR[0].DISABLE */
	OUT_RING(ring, ~varmask[1]);  /* VPC_VAR[1].DISABLE */
	OUT_RING(ring, ~varmask[2]);  /* VPC_VAR[2].DISABLE */
	OUT_RING(ring, ~varmask[3]);  /* VPC_VAR[3].DISABLE */

	/* a6xx appends pos/psize to end of the linkage map: */
	if (VALIDREG(pos_regid))
		ir3_link_add(&l, pos_regid, 0xf, l.max_loc);

	if (VALIDREG(psize_regid)) {
		psize_loc = l.max_loc;
		ir3_link_add(&l, psize_regid, 0x1, l.max_loc);
	}

	if ((vs->shader->stream_output.num_outputs > 0) && !binning_pass) {
		setup_stream_out(state, vs, &l);
	}

	for (i = 0, j = 0; (i < 16) && (j < l.cnt); i++) {
		uint32_t reg = 0;

		OUT_PKT4(ring, REG_A6XX_SP_VS_OUT_REG(i), 1);

		reg |= A6XX_SP_VS_OUT_REG_A_REGID(l.var[j].regid);
		reg |= A6XX_SP_VS_OUT_REG_A_COMPMASK(l.var[j].compmask);
		j++;

		reg |= A6XX_SP_VS_OUT_REG_B_REGID(l.var[j].regid);
		reg |= A6XX_SP_VS_OUT_REG_B_COMPMASK(l.var[j].compmask);
		j++;

		OUT_RING(ring, reg);
	}

	for (i = 0, j = 0; (i < 8) && (j < l.cnt); i++) {
		uint32_t reg = 0;

		OUT_PKT4(ring, REG_A6XX_SP_VS_VPC_DST_REG(i), 1);

		reg |= A6XX_SP_VS_VPC_DST_REG_OUTLOC0(l.var[j++].loc);
		reg |= A6XX_SP_VS_VPC_DST_REG_OUTLOC1(l.var[j++].loc);
		reg |= A6XX_SP_VS_VPC_DST_REG_OUTLOC2(l.var[j++].loc);
		reg |= A6XX_SP_VS_VPC_DST_REG_OUTLOC3(l.var[j++].loc);

		OUT_RING(ring, reg);
	}

	OUT_PKT4(ring, REG_A6XX_SP_VS_OBJ_START_LO, 2);
	OUT_RELOC(ring, vs->bo, 0, 0, 0);  /* SP_VS_OBJ_START_LO/HI */

	if (vs->instrlen)
		fd6_emit_shader(ring, vs);


	OUT_PKT4(ring, REG_A6XX_SP_PRIMITIVE_CNTL, 1);
	OUT_RING(ring, A6XX_SP_PRIMITIVE_CNTL_VSOUT(l.cnt));

	bool enable_varyings = fs->total_in > 0;

	OUT_PKT4(ring, REG_A6XX_VPC_CNTL_0, 1);
	OUT_RING(ring, A6XX_VPC_CNTL_0_NUMNONPOSVAR(fs->total_in) |
			 COND(enable_varyings, A6XX_VPC_CNTL_0_VARYING) |
			 0xff00ff00);

	OUT_PKT4(ring, REG_A6XX_PC_PRIMITIVE_CNTL_1, 1);
	OUT_RING(ring, A6XX_PC_PRIMITIVE_CNTL_1_STRIDE_IN_VPC(l.max_loc) |
			 CONDREG(psize_regid, 0x100));

	if (binning_pass) {
		OUT_PKT4(ring, REG_A6XX_SP_FS_OBJ_START_LO, 2);
		OUT_RING(ring, 0x00000000);    /* SP_FS_OBJ_START_LO */
		OUT_RING(ring, 0x00000000);    /* SP_FS_OBJ_START_HI */
	} else {
		OUT_PKT4(ring, REG_A6XX_SP_FS_OBJ_START_LO, 2);
		OUT_RELOC(ring, fs->bo, 0, 0, 0);  /* SP_FS_OBJ_START_LO/HI */
	}

	OUT_PKT4(ring, REG_A6XX_HLSQ_CONTROL_1_REG, 5);
	OUT_RING(ring, 0x7);                /* XXX */
	OUT_RING(ring, A6XX_HLSQ_CONTROL_2_REG_FACEREGID(face_regid) |
			 A6XX_HLSQ_CONTROL_2_REG_SAMPLEID(samp_id_regid) |
			 A6XX_HLSQ_CONTROL_2_REG_SAMPLEMASK(smask_in_regid) |
			 A6XX_HLSQ_CONTROL_2_REG_SIZE(ij_size_regid));
	OUT_RING(ring, A6XX_HLSQ_CONTROL_3_REG_BARY_IJ_PIXEL(ij_pix_regid) |
			 A6XX_HLSQ_CONTROL_3_REG_BARY_IJ_CENTROID(ij_cent_regid) |
			 0xfc00fc00);               /* XXX */
	OUT_RING(ring, A6XX_HLSQ_CONTROL_4_REG_XYCOORDREGID(coord_regid) |
			 A6XX_HLSQ_CONTROL_4_REG_ZWCOORDREGID(zwcoord_regid) |
			 A6XX_HLSQ_CONTROL_4_REG_BARY_IJ_PIXEL_PERSAMP(ij_samp_regid) |
			 0x0000fc00);               /* XXX */
	OUT_RING(ring, 0xfc);              /* XXX */

	OUT_PKT4(ring, REG_A6XX_HLSQ_UNKNOWN_B980, 1);
	OUT_RING(ring, enable_varyings ? 3 : 1);

	OUT_PKT4(ring, REG_A6XX_SP_FS_CTRL_REG0, 1);
	OUT_RING(ring, A6XX_SP_FS_CTRL_REG0_THREADSIZE(fssz) |
			COND(enable_varyings, A6XX_SP_FS_CTRL_REG0_VARYING) |
			COND(fs->frag_coord, A6XX_SP_FS_CTRL_REG0_VARYING) |
			0x1000000 |
			A6XX_SP_FS_CTRL_REG0_FULLREGFOOTPRINT(fs->info.max_reg + 1) |
			A6XX_SP_FS_CTRL_REG0_MERGEDREGS |
			A6XX_SP_FS_CTRL_REG0_BRANCHSTACK(fs->branchstack) |
			COND(fs->need_pixlod, A6XX_SP_FS_CTRL_REG0_PIXLODENABLE));

	OUT_PKT4(ring, REG_A6XX_SP_UNKNOWN_A982, 1);
	OUT_RING(ring, 0);        /* XXX */

	OUT_PKT4(ring, REG_A6XX_VPC_GS_SIV_CNTL, 1);
	OUT_RING(ring, 0x0000ffff);        /* XXX */

	OUT_PKT4(ring, REG_A6XX_GRAS_CNTL, 1);
	OUT_RING(ring,
			CONDREG(ij_pix_regid, A6XX_GRAS_CNTL_VARYING) |
			CONDREG(ij_cent_regid, A6XX_GRAS_CNTL_CENTROID) |
			CONDREG(ij_samp_regid, A6XX_GRAS_CNTL_PERSAMP_VARYING) |
			COND(VALIDREG(ij_size_regid) && !sample_shading, A6XX_GRAS_CNTL_SIZE) |
			COND(VALIDREG(ij_size_regid) &&  sample_shading, A6XX_GRAS_CNTL_SIZE_PERSAMP) |
			COND(fs->frag_coord,
					A6XX_GRAS_CNTL_SIZE |
					A6XX_GRAS_CNTL_XCOORD |
					A6XX_GRAS_CNTL_YCOORD |
					A6XX_GRAS_CNTL_ZCOORD |
					A6XX_GRAS_CNTL_WCOORD) |
			COND(fs->frag_face, A6XX_GRAS_CNTL_SIZE));

	OUT_PKT4(ring, REG_A6XX_RB_RENDER_CONTROL0, 2);
	OUT_RING(ring,
			CONDREG(ij_pix_regid, A6XX_RB_RENDER_CONTROL0_VARYING) |
			CONDREG(ij_cent_regid, A6XX_RB_RENDER_CONTROL0_CENTROID) |
			CONDREG(ij_samp_regid, A6XX_RB_RENDER_CONTROL0_PERSAMP_VARYING) |
			COND(enable_varyings, A6XX_RB_RENDER_CONTROL0_UNK10) |
			COND(VALIDREG(ij_size_regid) && !sample_shading, A6XX_RB_RENDER_CONTROL0_SIZE) |
			COND(VALIDREG(ij_size_regid) &&  sample_shading, A6XX_RB_RENDER_CONTROL0_SIZE_PERSAMP) |
			COND(fs->frag_coord,
					A6XX_RB_RENDER_CONTROL0_SIZE |
					A6XX_RB_RENDER_CONTROL0_XCOORD |
					A6XX_RB_RENDER_CONTROL0_YCOORD |
					A6XX_RB_RENDER_CONTROL0_ZCOORD |
					A6XX_RB_RENDER_CONTROL0_WCOORD) |
			COND(fs->frag_face, A6XX_RB_RENDER_CONTROL0_SIZE));

	OUT_RING(ring,
			CONDREG(smask_in_regid, A6XX_RB_RENDER_CONTROL1_SAMPLEMASK) |
			CONDREG(samp_id_regid, A6XX_RB_RENDER_CONTROL1_SAMPLEID) |
			CONDREG(ij_size_regid, A6XX_RB_RENDER_CONTROL1_SIZE) |
			COND(fs->frag_face, A6XX_RB_RENDER_CONTROL1_FACENESS));

	OUT_PKT4(ring, REG_A6XX_RB_SAMPLE_CNTL, 1);
	OUT_RING(ring, COND(sample_shading, A6XX_RB_SAMPLE_CNTL_PER_SAMP_MODE));

	OUT_PKT4(ring, REG_A6XX_GRAS_UNKNOWN_8101, 1);
	OUT_RING(ring, COND(sample_shading, 0x6));  // XXX

	OUT_PKT4(ring, REG_A6XX_GRAS_SAMPLE_CNTL, 1);
	OUT_RING(ring, COND(sample_shading, A6XX_GRAS_SAMPLE_CNTL_PER_SAMP_MODE));

	OUT_PKT4(ring, REG_A6XX_SP_FS_OUTPUT_REG(0), 8);
	for (i = 0; i < 8; i++) {
		OUT_RING(ring, A6XX_SP_FS_OUTPUT_REG_REGID(color_regid[i]) |
				COND(color_regid[i] & HALF_REG_ID, A6XX_SP_FS_OUTPUT_REG_HALF_PRECISION));
	}

	OUT_PKT4(ring, REG_A6XX_VPC_PACK, 1);
	OUT_RING(ring, A6XX_VPC_PACK_NUMNONPOSVAR(fs->total_in) |
			 A6XX_VPC_PACK_PSIZELOC(psize_loc) |
			 A6XX_VPC_PACK_STRIDE_IN_VPC(l.max_loc));

	if (!binning_pass) {
		/* figure out VARYING_INTERP / VARYING_PS_REPL register values: */
		for (j = -1; (j = ir3_next_varying(fs, j)) < (int)fs->inputs_count; ) {
			/* NOTE: varyings are packed, so if compmask is 0xb
			 * then first, third, and fourth component occupy
			 * three consecutive varying slots:
			 */
			unsigned compmask = fs->inputs[j].compmask;

			uint32_t inloc = fs->inputs[j].inloc;

			if (fs->inputs[j].interpolate == INTERP_MODE_FLAT) {
				uint32_t loc = inloc;

				for (i = 0; i < 4; i++) {
					if (compmask & (1 << i)) {
						state->vinterp[loc / 16] |= 1 << ((loc % 16) * 2);
						loc++;
					}
				}
			}
		}
	}

	if (!binning_pass)
		if (fs->instrlen)
			fd6_emit_shader(ring, fs);

	OUT_PKT4(ring, REG_A6XX_VFD_CONTROL_1, 6);
	OUT_RING(ring, A6XX_VFD_CONTROL_1_REGID4VTX(vertex_regid) |
			A6XX_VFD_CONTROL_1_REGID4INST(instance_regid) |
			0xfcfc0000);
	OUT_RING(ring, 0x0000fcfc);   /* VFD_CONTROL_2 */
	OUT_RING(ring, 0xfcfcfcfc);   /* VFD_CONTROL_3 */
	OUT_RING(ring, 0x000000fc);   /* VFD_CONTROL_4 */
	OUT_RING(ring, 0x0000fcfc);   /* VFD_CONTROL_5 */
	OUT_RING(ring, 0x00000000);   /* VFD_CONTROL_6 */

	bool fragz = fs->no_earlyz | fs->writes_pos;

	OUT_PKT4(ring, REG_A6XX_RB_DEPTH_PLANE_CNTL, 1);
	OUT_RING(ring, COND(fragz, A6XX_RB_DEPTH_PLANE_CNTL_FRAG_WRITES_Z));

	OUT_PKT4(ring, REG_A6XX_GRAS_SU_DEPTH_PLANE_CNTL, 1);
	OUT_RING(ring, COND(fragz, A6XX_GRAS_SU_DEPTH_PLANE_CNTL_FRAG_WRITES_Z));
}

/* emits the program state which is not part of the stateobj because of
 * dependency on other gl state (rasterflat or sprite-coord-replacement)
 */
void
fd6_program_emit(struct fd_ringbuffer *ring, struct fd6_emit *emit)
{
	const struct fd6_program_state *state = fd6_emit_get_prog(emit);

	if (!unlikely(emit->rasterflat || emit->sprite_coord_enable)) {
		/* fastpath: */
		OUT_PKT4(ring, REG_A6XX_VPC_VARYING_INTERP_MODE(0), 8);
		for (int i = 0; i < 8; i++)
			OUT_RING(ring, state->vinterp[i]);   /* VPC_VARYING_INTERP[i].MODE */

		OUT_PKT4(ring, REG_A6XX_VPC_VARYING_PS_REPL_MODE(0), 8);
		for (int i = 0; i < 8; i++)
			OUT_RING(ring, 0x00000000);          /* VPC_VARYING_PS_REPL[i] */
	} else {
		/* slow-path: */
		struct ir3_shader_variant *fs = state->fs;
		uint32_t vinterp[8], vpsrepl[8];

		memset(vinterp, 0, sizeof(vinterp));
		memset(vpsrepl, 0, sizeof(vpsrepl));

		for (int j = -1; (j = ir3_next_varying(fs, j)) < (int)fs->inputs_count; ) {

			/* NOTE: varyings are packed, so if compmask is 0xb
			 * then first, third, and fourth component occupy
			 * three consecutive varying slots:
			 */
			unsigned compmask = fs->inputs[j].compmask;

			uint32_t inloc = fs->inputs[j].inloc;

			if ((fs->inputs[j].interpolate == INTERP_MODE_FLAT) ||
					(fs->inputs[j].rasterflat && emit->rasterflat)) {
				uint32_t loc = inloc;

				for (int i = 0; i < 4; i++) {
					if (compmask & (1 << i)) {
						vinterp[loc / 16] |= 1 << ((loc % 16) * 2);
						loc++;
					}
				}
			}

			gl_varying_slot slot = fs->inputs[j].slot;

			/* since we don't enable PIPE_CAP_TGSI_TEXCOORD: */
			if (slot >= VARYING_SLOT_VAR0) {
				unsigned texmask = 1 << (slot - VARYING_SLOT_VAR0);
				/* Replace the .xy coordinates with S/T from the point sprite. Set
				 * interpolation bits for .zw such that they become .01
				 */
				if (emit->sprite_coord_enable & texmask) {
					/* mask is two 2-bit fields, where:
					 *   '01' -> S
					 *   '10' -> T
					 *   '11' -> 1 - T  (flip mode)
					 */
					unsigned mask = emit->sprite_coord_mode ? 0b1101 : 0b1001;
					uint32_t loc = inloc;
					if (compmask & 0x1) {
						vpsrepl[loc / 16] |= ((mask >> 0) & 0x3) << ((loc % 16) * 2);
						loc++;
					}
					if (compmask & 0x2) {
						vpsrepl[loc / 16] |= ((mask >> 2) & 0x3) << ((loc % 16) * 2);
						loc++;
					}
					if (compmask & 0x4) {
						/* .z <- 0.0f */
						vinterp[loc / 16] |= 0b10 << ((loc % 16) * 2);
						loc++;
					}
					if (compmask & 0x8) {
						/* .w <- 1.0f */
						vinterp[loc / 16] |= 0b11 << ((loc % 16) * 2);
						loc++;
					}
				}
			}
		}

		OUT_PKT4(ring, REG_A6XX_VPC_VARYING_INTERP_MODE(0), 8);
		for (int i = 0; i < 8; i++)
			OUT_RING(ring, vinterp[i]);     /* VPC_VARYING_INTERP[i].MODE */

		OUT_PKT4(ring, REG_A6XX_VPC_VARYING_PS_REPL_MODE(0), 8);
		for (int i = 0; i < 8; i++)
			OUT_RING(ring, vpsrepl[i]);     /* VPC_VARYING_PS_REPL[i] */
	}
}

static struct ir3_program_state *
fd6_program_create(void *data, struct ir3_shader_variant *bs,
		struct ir3_shader_variant *vs,
		struct ir3_shader_variant *fs,
		const struct ir3_shader_key *key)
{
	struct fd_context *ctx = data;
	struct fd6_program_state *state = CALLOC_STRUCT(fd6_program_state);

	state->bs = bs;
	state->vs = vs;
	state->fs = fs;
	state->config_stateobj = fd_ringbuffer_new_object(ctx->pipe, 0x1000);
	state->binning_stateobj = fd_ringbuffer_new_object(ctx->pipe, 0x1000);
	state->stateobj = fd_ringbuffer_new_object(ctx->pipe, 0x1000);

	setup_config_stateobj(state->config_stateobj, state);
	setup_stateobj(state->binning_stateobj, state, key, true);
	setup_stateobj(state->stateobj, state, key, false);

	return &state->base;
}

static void
fd6_program_destroy(void *data, struct ir3_program_state *state)
{
	struct fd6_program_state *so = fd6_program_state(state);
	fd_ringbuffer_del(so->stateobj);
	fd_ringbuffer_del(so->binning_stateobj);
	fd_ringbuffer_del(so->config_stateobj);
	free(so);
}

static const struct ir3_cache_funcs cache_funcs = {
	.create_state = fd6_program_create,
	.destroy_state = fd6_program_destroy,
};

void
fd6_prog_init(struct pipe_context *pctx)
{
	struct fd_context *ctx = fd_context(pctx);

	fd6_context(ctx)->shader_cache = ir3_cache_create(&cache_funcs, ctx);

	pctx->create_fs_state = fd6_fp_state_create;
	pctx->delete_fs_state = fd6_fp_state_delete;

	pctx->create_vs_state = fd6_vp_state_create;
	pctx->delete_vs_state = fd6_vp_state_delete;

	fd_prog_init(pctx);
}
