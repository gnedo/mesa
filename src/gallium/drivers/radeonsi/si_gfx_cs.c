/*
 * Copyright 2010 Jerome Glisse <glisse@freedesktop.org>
 * Copyright 2018 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "si_pipe.h"
#include "si_build_pm4.h"
#include "sid.h"

#include "util/os_time.h"
#include "util/u_upload_mgr.h"

/* initialize */
void si_need_gfx_cs_space(struct si_context *ctx)
{
	struct radeon_cmdbuf *cs = ctx->gfx_cs;

	/* There is no need to flush the DMA IB here, because
	 * si_need_dma_space always flushes the GFX IB if there is
	 * a conflict, which means any unflushed DMA commands automatically
	 * precede the GFX IB (= they had no dependency on the GFX IB when
	 * they were submitted).
	 */

	/* There are two memory usage counters in the winsys for all buffers
	 * that have been added (cs_add_buffer) and two counters in the pipe
	 * driver for those that haven't been added yet.
	 */
	if (unlikely(!radeon_cs_memory_below_limit(ctx->screen, ctx->gfx_cs,
						   ctx->vram, ctx->gtt))) {
		ctx->gtt = 0;
		ctx->vram = 0;
		si_flush_gfx_cs(ctx, RADEON_FLUSH_ASYNC_START_NEXT_GFX_IB_NOW, NULL);
		return;
	}
	ctx->gtt = 0;
	ctx->vram = 0;

	unsigned need_dwords = si_get_minimum_num_gfx_cs_dwords(ctx);
	if (!ctx->ws->cs_check_space(cs, need_dwords, false))
		si_flush_gfx_cs(ctx, RADEON_FLUSH_ASYNC_START_NEXT_GFX_IB_NOW, NULL);
}

void si_unref_sdma_uploads(struct si_context *sctx)
{
	for (unsigned i = 0; i < sctx->num_sdma_uploads; i++) {
		si_resource_reference(&sctx->sdma_uploads[i].dst, NULL);
		si_resource_reference(&sctx->sdma_uploads[i].src, NULL);
	}
	sctx->num_sdma_uploads = 0;
}

void si_flush_gfx_cs(struct si_context *ctx, unsigned flags,
		     struct pipe_fence_handle **fence)
{
	struct radeon_cmdbuf *cs = ctx->gfx_cs;
	struct radeon_winsys *ws = ctx->ws;
	const unsigned wait_ps_cs = SI_CONTEXT_PS_PARTIAL_FLUSH |
				    SI_CONTEXT_CS_PARTIAL_FLUSH;
	unsigned wait_flags = 0;

	if (ctx->gfx_flush_in_progress)
		return;

	if (!ctx->screen->info.kernel_flushes_tc_l2_after_ib) {
		wait_flags |= wait_ps_cs |
			      SI_CONTEXT_INV_L2;
	} else if (ctx->chip_class == GFX6) {
		/* The kernel flushes L2 before shaders are finished. */
		wait_flags |= wait_ps_cs;
	} else if (!(flags & RADEON_FLUSH_START_NEXT_GFX_IB_NOW)) {
		wait_flags |= wait_ps_cs;
	}

	/* Drop this flush if it's a no-op. */
	if (!radeon_emitted(cs, ctx->initial_gfx_cs_size) &&
	    (!wait_flags || !ctx->gfx_last_ib_is_busy))
		return;

	if (si_check_device_reset(ctx))
		return;

	if (ctx->screen->debug_flags & DBG(CHECK_VM))
		flags &= ~PIPE_FLUSH_ASYNC;

	ctx->gfx_flush_in_progress = true;

	/* If the state tracker is flushing the GFX IB, si_flush_from_st is
	 * responsible for flushing the DMA IB and merging the fences from both.
	 * If the driver flushes the GFX IB internally, and it should never ask
	 * for a fence handle.
	 */
	assert(!radeon_emitted(ctx->dma_cs, 0) || fence == NULL);

	/* Update the sdma_uploads list by flushing the uploader. */
	u_upload_unmap(ctx->b.const_uploader);

	/* Execute SDMA uploads. */
	ctx->sdma_uploads_in_progress = true;
	for (unsigned i = 0; i < ctx->num_sdma_uploads; i++) {
		struct si_sdma_upload *up = &ctx->sdma_uploads[i];
		struct pipe_box box;

		assert(up->src_offset % 4 == 0 && up->dst_offset % 4 == 0 &&
		       up->size % 4 == 0);

		u_box_1d(up->src_offset, up->size, &box);
		ctx->dma_copy(&ctx->b, &up->dst->b.b, 0, up->dst_offset, 0, 0,
			      &up->src->b.b, 0, &box);
	}
	ctx->sdma_uploads_in_progress = false;
	si_unref_sdma_uploads(ctx);

	/* Flush SDMA (preamble IB). */
	if (radeon_emitted(ctx->dma_cs, 0))
		si_flush_dma_cs(ctx, flags, NULL);

	if (radeon_emitted(ctx->prim_discard_compute_cs, 0)) {
		struct radeon_cmdbuf *compute_cs = ctx->prim_discard_compute_cs;
		si_compute_signal_gfx(ctx);

		/* Make sure compute shaders are idle before leaving the IB, so that
		 * the next IB doesn't overwrite GDS that might be in use. */
		radeon_emit(compute_cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
		radeon_emit(compute_cs, EVENT_TYPE(V_028A90_CS_PARTIAL_FLUSH) |
					EVENT_INDEX(4));

		/* Save the GDS prim restart counter if needed. */
		if (ctx->preserve_prim_restart_gds_at_flush) {
			si_cp_copy_data(ctx, compute_cs,
					COPY_DATA_DST_MEM, ctx->wait_mem_scratch, 4,
					COPY_DATA_GDS, NULL, 4);
		}
	}

	if (ctx->has_graphics) {
		if (!LIST_IS_EMPTY(&ctx->active_queries))
			si_suspend_queries(ctx);

		ctx->streamout.suspended = false;
		if (ctx->streamout.begin_emitted) {
			si_emit_streamout_end(ctx);
			ctx->streamout.suspended = true;

			/* Since streamout uses GDS on gfx10, we need to make
			 * GDS idle when we leave the IB, otherwise another
			 * process might overwrite it while our shaders are busy.
			 */
			if (ctx->chip_class >= GFX10)
				wait_flags |= SI_CONTEXT_PS_PARTIAL_FLUSH;
		}
	}

	/* Make sure CP DMA is idle at the end of IBs after L2 prefetches
	 * because the kernel doesn't wait for it. */
	if (ctx->chip_class >= GFX7)
		si_cp_dma_wait_for_idle(ctx);

	/* Wait for draw calls to finish if needed. */
	if (wait_flags) {
		ctx->flags |= wait_flags;
		ctx->emit_cache_flush(ctx);
	}
	ctx->gfx_last_ib_is_busy = (wait_flags & wait_ps_cs) != wait_ps_cs;

	if (ctx->current_saved_cs) {
		si_trace_emit(ctx);

		/* Save the IB for debug contexts. */
		si_save_cs(ws, cs, &ctx->current_saved_cs->gfx, true);
		ctx->current_saved_cs->flushed = true;
		ctx->current_saved_cs->time_flush = os_time_get_nano();

		si_log_hw_flush(ctx);
	}

	if (si_compute_prim_discard_enabled(ctx)) {
		/* The compute IB can start after the previous gfx IB starts. */
		if (radeon_emitted(ctx->prim_discard_compute_cs, 0) &&
		    ctx->last_gfx_fence) {
			ctx->ws->cs_add_fence_dependency(ctx->gfx_cs,
							 ctx->last_gfx_fence,
							 RADEON_DEPENDENCY_PARALLEL_COMPUTE_ONLY |
							 RADEON_DEPENDENCY_START_FENCE);
		}

		/* Remember the last execution barrier. It's in the IB.
		 * It will signal the start of the next compute IB.
		 */
		if (flags & RADEON_FLUSH_START_NEXT_GFX_IB_NOW &&
		    ctx->last_pkt3_write_data) {
			*ctx->last_pkt3_write_data = PKT3(PKT3_WRITE_DATA, 3, 0);
			ctx->last_pkt3_write_data = NULL;

			si_resource_reference(&ctx->last_ib_barrier_buf, ctx->barrier_buf);
			ctx->last_ib_barrier_buf_offset = ctx->barrier_buf_offset;
			si_resource_reference(&ctx->barrier_buf, NULL);

			ws->fence_reference(&ctx->last_ib_barrier_fence, NULL);
		}
	}

	/* Flush the CS. */
	ws->cs_flush(cs, flags, &ctx->last_gfx_fence);
	if (fence)
		ws->fence_reference(fence, ctx->last_gfx_fence);

	ctx->num_gfx_cs_flushes++;

	if (si_compute_prim_discard_enabled(ctx)) {
		/* Remember the last execution barrier, which is the last fence
		 * in this case.
		 */
		if (!(flags & RADEON_FLUSH_START_NEXT_GFX_IB_NOW)) {
			ctx->last_pkt3_write_data = NULL;
			si_resource_reference(&ctx->last_ib_barrier_buf, NULL);
			ws->fence_reference(&ctx->last_ib_barrier_fence, ctx->last_gfx_fence);
		}
	}

	/* Check VM faults if needed. */
	if (ctx->screen->debug_flags & DBG(CHECK_VM)) {
		/* Use conservative timeout 800ms, after which we won't wait any
		 * longer and assume the GPU is hung.
		 */
		ctx->ws->fence_wait(ctx->ws, ctx->last_gfx_fence, 800*1000*1000);

		si_check_vm_faults(ctx, &ctx->current_saved_cs->gfx, RING_GFX);
	}

	if (ctx->current_saved_cs)
		si_saved_cs_reference(&ctx->current_saved_cs, NULL);

	si_begin_new_gfx_cs(ctx);
	ctx->gfx_flush_in_progress = false;
}

static void si_begin_gfx_cs_debug(struct si_context *ctx)
{
	static const uint32_t zeros[1];
	assert(!ctx->current_saved_cs);

	ctx->current_saved_cs = calloc(1, sizeof(*ctx->current_saved_cs));
	if (!ctx->current_saved_cs)
		return;

	pipe_reference_init(&ctx->current_saved_cs->reference, 1);

	ctx->current_saved_cs->trace_buf = si_resource(
		pipe_buffer_create(ctx->b.screen, 0, PIPE_USAGE_STAGING, 8));
	if (!ctx->current_saved_cs->trace_buf) {
		free(ctx->current_saved_cs);
		ctx->current_saved_cs = NULL;
		return;
	}

	pipe_buffer_write_nooverlap(&ctx->b, &ctx->current_saved_cs->trace_buf->b.b,
				    0, sizeof(zeros), zeros);
	ctx->current_saved_cs->trace_id = 0;

	si_trace_emit(ctx);

	radeon_add_to_buffer_list(ctx, ctx->gfx_cs, ctx->current_saved_cs->trace_buf,
			      RADEON_USAGE_READWRITE, RADEON_PRIO_TRACE);
}

static void si_add_gds_to_buffer_list(struct si_context *sctx)
{
	if (sctx->gds) {
		sctx->ws->cs_add_buffer(sctx->gfx_cs, sctx->gds,
				       RADEON_USAGE_READWRITE, 0, 0);
		if (sctx->gds_oa) {
			sctx->ws->cs_add_buffer(sctx->gfx_cs, sctx->gds_oa,
					       RADEON_USAGE_READWRITE, 0, 0);
		}
	}
}

void si_allocate_gds(struct si_context *sctx)
{
	struct radeon_winsys *ws = sctx->ws;

	if (sctx->gds)
		return;

	assert(sctx->chip_class >= GFX10); /* for gfx10 streamout */

	/* 4 streamout GDS counters.
	 * We need 256B (64 dw) of GDS, otherwise streamout hangs.
	 */
	sctx->gds = ws->buffer_create(ws, 256, 4, RADEON_DOMAIN_GDS, 0);
	sctx->gds_oa = ws->buffer_create(ws, 4, 1, RADEON_DOMAIN_OA, 0);

	assert(sctx->gds && sctx->gds_oa);
	si_add_gds_to_buffer_list(sctx);
}

void si_begin_new_gfx_cs(struct si_context *ctx)
{
	if (ctx->is_debug)
		si_begin_gfx_cs_debug(ctx);

	si_add_gds_to_buffer_list(ctx);

	/* Always invalidate caches at the beginning of IBs, because external
	 * users (e.g. BO evictions and SDMA/UVD/VCE IBs) can modify our
	 * buffers.
	 *
	 * Note that the cache flush done by the kernel at the end of GFX IBs
	 * isn't useful here, because that flush can finish after the following
	 * IB starts drawing.
	 *
	 * TODO: Do we also need to invalidate CB & DB caches?
	 */
	ctx->flags |= SI_CONTEXT_INV_ICACHE |
		      SI_CONTEXT_INV_SCACHE |
		      SI_CONTEXT_INV_VCACHE |
		      SI_CONTEXT_INV_L2 |
		      SI_CONTEXT_START_PIPELINE_STATS;

	ctx->cs_shader_state.initialized = false;
	si_all_descriptors_begin_new_cs(ctx);

	if (!ctx->has_graphics) {
		ctx->initial_gfx_cs_size = ctx->gfx_cs->current.cdw;
		return;
	}

	/* set all valid group as dirty so they get reemited on
	 * next draw command
	 */
	si_pm4_reset_emitted(ctx);

	/* The CS initialization should be emitted before everything else. */
	si_pm4_emit(ctx, ctx->init_config);
	if (ctx->init_config_gs_rings)
		si_pm4_emit(ctx, ctx->init_config_gs_rings);

	if (ctx->queued.named.ls)
		ctx->prefetch_L2_mask |= SI_PREFETCH_LS;
	if (ctx->queued.named.hs)
		ctx->prefetch_L2_mask |= SI_PREFETCH_HS;
	if (ctx->queued.named.es)
		ctx->prefetch_L2_mask |= SI_PREFETCH_ES;
	if (ctx->queued.named.gs)
		ctx->prefetch_L2_mask |= SI_PREFETCH_GS;
	if (ctx->queued.named.vs)
		ctx->prefetch_L2_mask |= SI_PREFETCH_VS;
	if (ctx->queued.named.ps)
		ctx->prefetch_L2_mask |= SI_PREFETCH_PS;
	if (ctx->vb_descriptors_buffer && ctx->vertex_elements)
		ctx->prefetch_L2_mask |= SI_PREFETCH_VBO_DESCRIPTORS;

	/* CLEAR_STATE disables all colorbuffers, so only enable bound ones. */
	bool has_clear_state = ctx->screen->has_clear_state;
	if (has_clear_state) {
		ctx->framebuffer.dirty_cbufs =
			 u_bit_consecutive(0, ctx->framebuffer.state.nr_cbufs);
		/* CLEAR_STATE disables the zbuffer, so only enable it if it's bound. */
		ctx->framebuffer.dirty_zsbuf = ctx->framebuffer.state.zsbuf != NULL;
	} else {
		ctx->framebuffer.dirty_cbufs = u_bit_consecutive(0, 8);
		ctx->framebuffer.dirty_zsbuf = true;
	}
	/* This should always be marked as dirty to set the framebuffer scissor
	 * at least. */
	si_mark_atom_dirty(ctx, &ctx->atoms.s.framebuffer);

	si_mark_atom_dirty(ctx, &ctx->atoms.s.clip_regs);
	/* CLEAR_STATE sets zeros. */
	if (!has_clear_state || ctx->clip_state.any_nonzeros)
		si_mark_atom_dirty(ctx, &ctx->atoms.s.clip_state);
	ctx->sample_locs_num_samples = 0;
	si_mark_atom_dirty(ctx, &ctx->atoms.s.msaa_sample_locs);
	si_mark_atom_dirty(ctx, &ctx->atoms.s.msaa_config);
	/* CLEAR_STATE sets 0xffff. */
	if (!has_clear_state || ctx->sample_mask != 0xffff)
		si_mark_atom_dirty(ctx, &ctx->atoms.s.sample_mask);
	si_mark_atom_dirty(ctx, &ctx->atoms.s.cb_render_state);
	/* CLEAR_STATE sets zeros. */
	if (!has_clear_state || ctx->blend_color.any_nonzeros)
		si_mark_atom_dirty(ctx, &ctx->atoms.s.blend_color);
	si_mark_atom_dirty(ctx, &ctx->atoms.s.db_render_state);
	if (ctx->chip_class >= GFX9)
		si_mark_atom_dirty(ctx, &ctx->atoms.s.dpbb_state);
	si_mark_atom_dirty(ctx, &ctx->atoms.s.stencil_ref);
	si_mark_atom_dirty(ctx, &ctx->atoms.s.spi_map);
	if (ctx->chip_class < GFX10)
		si_mark_atom_dirty(ctx, &ctx->atoms.s.streamout_enable);
	si_mark_atom_dirty(ctx, &ctx->atoms.s.render_cond);
	/* CLEAR_STATE disables all window rectangles. */
	if (!has_clear_state || ctx->num_window_rectangles > 0)
		si_mark_atom_dirty(ctx, &ctx->atoms.s.window_rectangles);

	si_mark_atom_dirty(ctx, &ctx->atoms.s.guardband);
	si_mark_atom_dirty(ctx, &ctx->atoms.s.scissors);
	si_mark_atom_dirty(ctx, &ctx->atoms.s.viewports);

	si_mark_atom_dirty(ctx, &ctx->atoms.s.scratch_state);
	if (ctx->scratch_buffer) {
		si_context_add_resource_size(ctx, &ctx->scratch_buffer->b.b);
	}

	if (ctx->streamout.suspended) {
		ctx->streamout.append_bitmask = ctx->streamout.enabled_mask;
		si_streamout_buffers_dirty(ctx);
	}

	if (!LIST_IS_EMPTY(&ctx->active_queries))
		si_resume_queries(ctx);

	assert(!ctx->gfx_cs->prev_dw);
	ctx->initial_gfx_cs_size = ctx->gfx_cs->current.cdw;

	/* Invalidate various draw states so that they are emitted before
	 * the first draw call. */
	si_invalidate_draw_sh_constants(ctx);
	ctx->last_index_size = -1;
	ctx->last_primitive_restart_en = -1;
	ctx->last_restart_index = SI_RESTART_INDEX_UNKNOWN;
	ctx->last_prim = -1;
	ctx->last_multi_vgt_param = -1;
	ctx->last_rast_prim = -1;
	ctx->last_flatshade_first = -1;
	ctx->last_sc_line_stipple = ~0;
	ctx->last_vs_state = ~0;
	ctx->last_ls = NULL;
	ctx->last_tcs = NULL;
	ctx->last_tes_sh_base = -1;
	ctx->last_num_tcs_input_cp = -1;
	ctx->last_ls_hs_config = -1; /* impossible value */
	ctx->last_binning_enabled = -1;

	ctx->prim_discard_compute_ib_initialized = false;

        /* Compute-based primitive discard:
         *   The index ring is divided into 2 halves. Switch between the halves
         *   in the same fashion as doublebuffering.
         */
        if (ctx->index_ring_base)
                ctx->index_ring_base = 0;
        else
                ctx->index_ring_base = ctx->index_ring_size_per_ib;

        ctx->index_ring_offset = 0;

	if (has_clear_state) {
		ctx->tracked_regs.reg_value[SI_TRACKED_DB_RENDER_CONTROL] = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_DB_COUNT_CONTROL] = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_DB_RENDER_OVERRIDE2] = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_DB_SHADER_CONTROL] = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_CB_TARGET_MASK] = 0xffffffff;
		ctx->tracked_regs.reg_value[SI_TRACKED_CB_DCC_CONTROL] = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_SX_PS_DOWNCONVERT] = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_SX_BLEND_OPT_EPSILON] = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_SX_BLEND_OPT_CONTROL] = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_PA_SC_LINE_CNTL]	= 0x00001000;
		ctx->tracked_regs.reg_value[SI_TRACKED_PA_SC_AA_CONFIG]	= 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_DB_EQAA]	= 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_PA_SC_MODE_CNTL_1] = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_PA_SU_PRIM_FILTER_CNTL] = 0;
		ctx->tracked_regs.reg_value[SI_TRACKED_PA_SU_SMALL_PRIM_FILTER_CNTL] = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_PA_CL_VS_OUT_CNTL] = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_PA_CL_CLIP_CNTL]	= 0x00090000;
		ctx->tracked_regs.reg_value[SI_TRACKED_PA_SC_BINNER_CNTL_0] = 0x00000003;
		ctx->tracked_regs.reg_value[SI_TRACKED_DB_DFSM_CONTROL]	= 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_PA_CL_GB_VERT_CLIP_ADJ]	= 0x3f800000;
		ctx->tracked_regs.reg_value[SI_TRACKED_PA_CL_GB_VERT_DISC_ADJ]	= 0x3f800000;
		ctx->tracked_regs.reg_value[SI_TRACKED_PA_CL_GB_HORZ_CLIP_ADJ]	= 0x3f800000;
		ctx->tracked_regs.reg_value[SI_TRACKED_PA_CL_GB_HORZ_DISC_ADJ]	= 0x3f800000;
		ctx->tracked_regs.reg_value[SI_TRACKED_PA_SU_HARDWARE_SCREEN_OFFSET] = 0;
		ctx->tracked_regs.reg_value[SI_TRACKED_PA_SU_VTX_CNTL] = 0x00000005;
		ctx->tracked_regs.reg_value[SI_TRACKED_PA_SC_CLIPRECT_RULE]	= 0xffff;
		ctx->tracked_regs.reg_value[SI_TRACKED_VGT_ESGS_RING_ITEMSIZE]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GSVS_RING_OFFSET_1]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GSVS_RING_OFFSET_2]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GSVS_RING_OFFSET_3]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GSVS_RING_ITEMSIZE]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GS_MAX_VERT_OUT]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GS_VERT_ITEMSIZE]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GS_VERT_ITEMSIZE_1]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GS_VERT_ITEMSIZE_2]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GS_VERT_ITEMSIZE_3]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GS_INSTANCE_CNT]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GS_ONCHIP_CNTL]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GS_MAX_PRIMS_PER_SUBGROUP]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GS_MODE]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_VGT_PRIMITIVEID_EN]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_VGT_REUSE_OFF]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_SPI_VS_OUT_CONFIG]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_GE_MAX_OUTPUT_PER_SUBGROUP]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_GE_NGG_SUBGRP_CNTL]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_SPI_SHADER_IDX_FORMAT]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_SPI_SHADER_POS_FORMAT]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_PA_CL_VTE_CNTL]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_PA_CL_NGG_CNTL]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_SPI_PS_INPUT_ENA]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_SPI_PS_INPUT_ADDR]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_SPI_BARYC_CNTL]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_SPI_PS_IN_CONTROL]  = 0x00000002;
		ctx->tracked_regs.reg_value[SI_TRACKED_SPI_SHADER_Z_FORMAT]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_SPI_SHADER_COL_FORMAT]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_CB_SHADER_MASK]  = 0xffffffff;
		ctx->tracked_regs.reg_value[SI_TRACKED_VGT_TF_PARAM]  = 0x00000000;
		ctx->tracked_regs.reg_value[SI_TRACKED_VGT_VERTEX_REUSE_BLOCK_CNTL]  = 0x0000001e; /* From GFX8 */

		/* Set all saved registers state to saved. */
		ctx->tracked_regs.reg_saved = 0xffffffffffffffff;
	} else {
		/* Set all saved registers state to unknown. */
		ctx->tracked_regs.reg_saved = 0;
	}

	/* 0xffffffff is a impossible value to register SPI_PS_INPUT_CNTL_n */
	memset(ctx->tracked_regs.spi_ps_input_cntl, 0xff, sizeof(uint32_t) * 32);
}
