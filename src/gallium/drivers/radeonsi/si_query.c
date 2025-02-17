/*
 * Copyright 2010 Jerome Glisse <glisse@freedesktop.org>
 * Copyright 2014 Marek Olšák <marek.olsak@amd.com>
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
#include "si_query.h"
#include "util/u_memory.h"
#include "util/u_upload_mgr.h"
#include "util/os_time.h"
#include "util/u_suballoc.h"
#include "amd/common/sid.h"

static const struct si_query_ops query_hw_ops;

struct si_hw_query_params {
	unsigned start_offset;
	unsigned end_offset;
	unsigned fence_offset;
	unsigned pair_stride;
	unsigned pair_count;
};

/* Queries without buffer handling or suspend/resume. */
struct si_query_sw {
	struct si_query b;

	uint64_t begin_result;
	uint64_t end_result;

	uint64_t begin_time;
	uint64_t end_time;

	/* Fence for GPU_FINISHED. */
	struct pipe_fence_handle *fence;
};

static void si_query_sw_destroy(struct si_context *sctx,
				struct si_query *squery)
{
	struct si_query_sw *query = (struct si_query_sw *)squery;

	sctx->b.screen->fence_reference(sctx->b.screen, &query->fence, NULL);
	FREE(query);
}

static enum radeon_value_id winsys_id_from_type(unsigned type)
{
	switch (type) {
	case SI_QUERY_REQUESTED_VRAM: return RADEON_REQUESTED_VRAM_MEMORY;
	case SI_QUERY_REQUESTED_GTT: return RADEON_REQUESTED_GTT_MEMORY;
	case SI_QUERY_MAPPED_VRAM: return RADEON_MAPPED_VRAM;
	case SI_QUERY_MAPPED_GTT: return RADEON_MAPPED_GTT;
	case SI_QUERY_BUFFER_WAIT_TIME: return RADEON_BUFFER_WAIT_TIME_NS;
	case SI_QUERY_NUM_MAPPED_BUFFERS: return RADEON_NUM_MAPPED_BUFFERS;
	case SI_QUERY_NUM_GFX_IBS: return RADEON_NUM_GFX_IBS;
	case SI_QUERY_NUM_SDMA_IBS: return RADEON_NUM_SDMA_IBS;
	case SI_QUERY_GFX_BO_LIST_SIZE: return RADEON_GFX_BO_LIST_COUNTER;
	case SI_QUERY_GFX_IB_SIZE: return RADEON_GFX_IB_SIZE_COUNTER;
	case SI_QUERY_NUM_BYTES_MOVED: return RADEON_NUM_BYTES_MOVED;
	case SI_QUERY_NUM_EVICTIONS: return RADEON_NUM_EVICTIONS;
	case SI_QUERY_NUM_VRAM_CPU_PAGE_FAULTS: return RADEON_NUM_VRAM_CPU_PAGE_FAULTS;
	case SI_QUERY_VRAM_USAGE: return RADEON_VRAM_USAGE;
	case SI_QUERY_VRAM_VIS_USAGE: return RADEON_VRAM_VIS_USAGE;
	case SI_QUERY_GTT_USAGE: return RADEON_GTT_USAGE;
	case SI_QUERY_GPU_TEMPERATURE: return RADEON_GPU_TEMPERATURE;
	case SI_QUERY_CURRENT_GPU_SCLK: return RADEON_CURRENT_SCLK;
	case SI_QUERY_CURRENT_GPU_MCLK: return RADEON_CURRENT_MCLK;
	case SI_QUERY_CS_THREAD_BUSY: return RADEON_CS_THREAD_TIME;
	default: unreachable("query type does not correspond to winsys id");
	}
}

static int64_t si_finish_dma_get_cpu_time(struct si_context *sctx)
{
	struct pipe_fence_handle *fence = NULL;

	si_flush_dma_cs(sctx, 0, &fence);
	if (fence) {
		sctx->ws->fence_wait(sctx->ws, fence, PIPE_TIMEOUT_INFINITE);
		sctx->ws->fence_reference(&fence, NULL);
	}

	return os_time_get_nano();
}

static bool si_query_sw_begin(struct si_context *sctx,
			      struct si_query *squery)
{
	struct si_query_sw *query = (struct si_query_sw *)squery;
	enum radeon_value_id ws_id;

	switch(query->b.type) {
	case PIPE_QUERY_TIMESTAMP_DISJOINT:
	case PIPE_QUERY_GPU_FINISHED:
		break;
	case SI_QUERY_TIME_ELAPSED_SDMA_SI:
		query->begin_result = si_finish_dma_get_cpu_time(sctx);
		break;
	case SI_QUERY_DRAW_CALLS:
		query->begin_result = sctx->num_draw_calls;
		break;
	case SI_QUERY_DECOMPRESS_CALLS:
		query->begin_result = sctx->num_decompress_calls;
		break;
	case SI_QUERY_MRT_DRAW_CALLS:
		query->begin_result = sctx->num_mrt_draw_calls;
		break;
	case SI_QUERY_PRIM_RESTART_CALLS:
		query->begin_result = sctx->num_prim_restart_calls;
		break;
	case SI_QUERY_SPILL_DRAW_CALLS:
		query->begin_result = sctx->num_spill_draw_calls;
		break;
	case SI_QUERY_COMPUTE_CALLS:
		query->begin_result = sctx->num_compute_calls;
		break;
	case SI_QUERY_SPILL_COMPUTE_CALLS:
		query->begin_result = sctx->num_spill_compute_calls;
		break;
	case SI_QUERY_DMA_CALLS:
		query->begin_result = sctx->num_dma_calls;
		break;
	case SI_QUERY_CP_DMA_CALLS:
		query->begin_result = sctx->num_cp_dma_calls;
		break;
	case SI_QUERY_NUM_VS_FLUSHES:
		query->begin_result = sctx->num_vs_flushes;
		break;
	case SI_QUERY_NUM_PS_FLUSHES:
		query->begin_result = sctx->num_ps_flushes;
		break;
	case SI_QUERY_NUM_CS_FLUSHES:
		query->begin_result = sctx->num_cs_flushes;
		break;
	case SI_QUERY_NUM_CB_CACHE_FLUSHES:
		query->begin_result = sctx->num_cb_cache_flushes;
		break;
	case SI_QUERY_NUM_DB_CACHE_FLUSHES:
		query->begin_result = sctx->num_db_cache_flushes;
		break;
	case SI_QUERY_NUM_L2_INVALIDATES:
		query->begin_result = sctx->num_L2_invalidates;
		break;
	case SI_QUERY_NUM_L2_WRITEBACKS:
		query->begin_result = sctx->num_L2_writebacks;
		break;
	case SI_QUERY_NUM_RESIDENT_HANDLES:
		query->begin_result = sctx->num_resident_handles;
		break;
	case SI_QUERY_TC_OFFLOADED_SLOTS:
		query->begin_result = sctx->tc ? sctx->tc->num_offloaded_slots : 0;
		break;
	case SI_QUERY_TC_DIRECT_SLOTS:
		query->begin_result = sctx->tc ? sctx->tc->num_direct_slots : 0;
		break;
	case SI_QUERY_TC_NUM_SYNCS:
		query->begin_result = sctx->tc ? sctx->tc->num_syncs : 0;
		break;
	case SI_QUERY_REQUESTED_VRAM:
	case SI_QUERY_REQUESTED_GTT:
	case SI_QUERY_MAPPED_VRAM:
	case SI_QUERY_MAPPED_GTT:
	case SI_QUERY_VRAM_USAGE:
	case SI_QUERY_VRAM_VIS_USAGE:
	case SI_QUERY_GTT_USAGE:
	case SI_QUERY_GPU_TEMPERATURE:
	case SI_QUERY_CURRENT_GPU_SCLK:
	case SI_QUERY_CURRENT_GPU_MCLK:
	case SI_QUERY_BACK_BUFFER_PS_DRAW_RATIO:
	case SI_QUERY_NUM_MAPPED_BUFFERS:
		query->begin_result = 0;
		break;
	case SI_QUERY_BUFFER_WAIT_TIME:
	case SI_QUERY_GFX_IB_SIZE:
	case SI_QUERY_NUM_GFX_IBS:
	case SI_QUERY_NUM_SDMA_IBS:
	case SI_QUERY_NUM_BYTES_MOVED:
	case SI_QUERY_NUM_EVICTIONS:
	case SI_QUERY_NUM_VRAM_CPU_PAGE_FAULTS: {
		enum radeon_value_id ws_id = winsys_id_from_type(query->b.type);
		query->begin_result = sctx->ws->query_value(sctx->ws, ws_id);
		break;
	}
	case SI_QUERY_GFX_BO_LIST_SIZE:
		ws_id = winsys_id_from_type(query->b.type);
		query->begin_result = sctx->ws->query_value(sctx->ws, ws_id);
		query->begin_time = sctx->ws->query_value(sctx->ws,
							  RADEON_NUM_GFX_IBS);
		break;
	case SI_QUERY_CS_THREAD_BUSY:
		ws_id = winsys_id_from_type(query->b.type);
		query->begin_result = sctx->ws->query_value(sctx->ws, ws_id);
		query->begin_time = os_time_get_nano();
		break;
	case SI_QUERY_GALLIUM_THREAD_BUSY:
		query->begin_result =
			sctx->tc ? util_queue_get_thread_time_nano(&sctx->tc->queue, 0) : 0;
		query->begin_time = os_time_get_nano();
		break;
	case SI_QUERY_GPU_LOAD:
	case SI_QUERY_GPU_SHADERS_BUSY:
	case SI_QUERY_GPU_TA_BUSY:
	case SI_QUERY_GPU_GDS_BUSY:
	case SI_QUERY_GPU_VGT_BUSY:
	case SI_QUERY_GPU_IA_BUSY:
	case SI_QUERY_GPU_SX_BUSY:
	case SI_QUERY_GPU_WD_BUSY:
	case SI_QUERY_GPU_BCI_BUSY:
	case SI_QUERY_GPU_SC_BUSY:
	case SI_QUERY_GPU_PA_BUSY:
	case SI_QUERY_GPU_DB_BUSY:
	case SI_QUERY_GPU_CP_BUSY:
	case SI_QUERY_GPU_CB_BUSY:
	case SI_QUERY_GPU_SDMA_BUSY:
	case SI_QUERY_GPU_PFP_BUSY:
	case SI_QUERY_GPU_MEQ_BUSY:
	case SI_QUERY_GPU_ME_BUSY:
	case SI_QUERY_GPU_SURF_SYNC_BUSY:
	case SI_QUERY_GPU_CP_DMA_BUSY:
	case SI_QUERY_GPU_SCRATCH_RAM_BUSY:
		query->begin_result = si_begin_counter(sctx->screen,
							 query->b.type);
		break;
	case SI_QUERY_NUM_COMPILATIONS:
		query->begin_result = p_atomic_read(&sctx->screen->num_compilations);
		break;
	case SI_QUERY_NUM_SHADERS_CREATED:
		query->begin_result = p_atomic_read(&sctx->screen->num_shaders_created);
		break;
	case SI_QUERY_NUM_SHADER_CACHE_HITS:
		query->begin_result =
			p_atomic_read(&sctx->screen->num_shader_cache_hits);
		break;
	case SI_QUERY_PD_NUM_PRIMS_ACCEPTED:
		query->begin_result = sctx->compute_num_verts_accepted;
		break;
	case SI_QUERY_PD_NUM_PRIMS_REJECTED:
		query->begin_result = sctx->compute_num_verts_rejected;
		break;
	case SI_QUERY_PD_NUM_PRIMS_INELIGIBLE:
		query->begin_result = sctx->compute_num_verts_ineligible;
		break;
	case SI_QUERY_GPIN_ASIC_ID:
	case SI_QUERY_GPIN_NUM_SIMD:
	case SI_QUERY_GPIN_NUM_RB:
	case SI_QUERY_GPIN_NUM_SPI:
	case SI_QUERY_GPIN_NUM_SE:
		break;
	default:
		unreachable("si_query_sw_begin: bad query type");
	}

	return true;
}

static bool si_query_sw_end(struct si_context *sctx,
			    struct si_query *squery)
{
	struct si_query_sw *query = (struct si_query_sw *)squery;
	enum radeon_value_id ws_id;

	switch(query->b.type) {
	case PIPE_QUERY_TIMESTAMP_DISJOINT:
		break;
	case PIPE_QUERY_GPU_FINISHED:
		sctx->b.flush(&sctx->b, &query->fence, PIPE_FLUSH_DEFERRED);
		break;
	case SI_QUERY_TIME_ELAPSED_SDMA_SI:
		query->end_result = si_finish_dma_get_cpu_time(sctx);
		break;
	case SI_QUERY_DRAW_CALLS:
		query->end_result = sctx->num_draw_calls;
		break;
	case SI_QUERY_DECOMPRESS_CALLS:
		query->end_result = sctx->num_decompress_calls;
		break;
	case SI_QUERY_MRT_DRAW_CALLS:
		query->end_result = sctx->num_mrt_draw_calls;
		break;
	case SI_QUERY_PRIM_RESTART_CALLS:
		query->end_result = sctx->num_prim_restart_calls;
		break;
	case SI_QUERY_SPILL_DRAW_CALLS:
		query->end_result = sctx->num_spill_draw_calls;
		break;
	case SI_QUERY_COMPUTE_CALLS:
		query->end_result = sctx->num_compute_calls;
		break;
	case SI_QUERY_SPILL_COMPUTE_CALLS:
		query->end_result = sctx->num_spill_compute_calls;
		break;
	case SI_QUERY_DMA_CALLS:
		query->end_result = sctx->num_dma_calls;
		break;
	case SI_QUERY_CP_DMA_CALLS:
		query->end_result = sctx->num_cp_dma_calls;
		break;
	case SI_QUERY_NUM_VS_FLUSHES:
		query->end_result = sctx->num_vs_flushes;
		break;
	case SI_QUERY_NUM_PS_FLUSHES:
		query->end_result = sctx->num_ps_flushes;
		break;
	case SI_QUERY_NUM_CS_FLUSHES:
		query->end_result = sctx->num_cs_flushes;
		break;
	case SI_QUERY_NUM_CB_CACHE_FLUSHES:
		query->end_result = sctx->num_cb_cache_flushes;
		break;
	case SI_QUERY_NUM_DB_CACHE_FLUSHES:
		query->end_result = sctx->num_db_cache_flushes;
		break;
	case SI_QUERY_NUM_L2_INVALIDATES:
		query->end_result = sctx->num_L2_invalidates;
		break;
	case SI_QUERY_NUM_L2_WRITEBACKS:
		query->end_result = sctx->num_L2_writebacks;
		break;
	case SI_QUERY_NUM_RESIDENT_HANDLES:
		query->end_result = sctx->num_resident_handles;
		break;
	case SI_QUERY_TC_OFFLOADED_SLOTS:
		query->end_result = sctx->tc ? sctx->tc->num_offloaded_slots : 0;
		break;
	case SI_QUERY_TC_DIRECT_SLOTS:
		query->end_result = sctx->tc ? sctx->tc->num_direct_slots : 0;
		break;
	case SI_QUERY_TC_NUM_SYNCS:
		query->end_result = sctx->tc ? sctx->tc->num_syncs : 0;
		break;
	case SI_QUERY_REQUESTED_VRAM:
	case SI_QUERY_REQUESTED_GTT:
	case SI_QUERY_MAPPED_VRAM:
	case SI_QUERY_MAPPED_GTT:
	case SI_QUERY_VRAM_USAGE:
	case SI_QUERY_VRAM_VIS_USAGE:
	case SI_QUERY_GTT_USAGE:
	case SI_QUERY_GPU_TEMPERATURE:
	case SI_QUERY_CURRENT_GPU_SCLK:
	case SI_QUERY_CURRENT_GPU_MCLK:
	case SI_QUERY_BUFFER_WAIT_TIME:
	case SI_QUERY_GFX_IB_SIZE:
	case SI_QUERY_NUM_MAPPED_BUFFERS:
	case SI_QUERY_NUM_GFX_IBS:
	case SI_QUERY_NUM_SDMA_IBS:
	case SI_QUERY_NUM_BYTES_MOVED:
	case SI_QUERY_NUM_EVICTIONS:
	case SI_QUERY_NUM_VRAM_CPU_PAGE_FAULTS: {
		enum radeon_value_id ws_id = winsys_id_from_type(query->b.type);
		query->end_result = sctx->ws->query_value(sctx->ws, ws_id);
		break;
	}
	case SI_QUERY_GFX_BO_LIST_SIZE:
		ws_id = winsys_id_from_type(query->b.type);
		query->end_result = sctx->ws->query_value(sctx->ws, ws_id);
		query->end_time = sctx->ws->query_value(sctx->ws,
							RADEON_NUM_GFX_IBS);
		break;
	case SI_QUERY_CS_THREAD_BUSY:
		ws_id = winsys_id_from_type(query->b.type);
		query->end_result = sctx->ws->query_value(sctx->ws, ws_id);
		query->end_time = os_time_get_nano();
		break;
	case SI_QUERY_GALLIUM_THREAD_BUSY:
		query->end_result =
			sctx->tc ? util_queue_get_thread_time_nano(&sctx->tc->queue, 0) : 0;
		query->end_time = os_time_get_nano();
		break;
	case SI_QUERY_GPU_LOAD:
	case SI_QUERY_GPU_SHADERS_BUSY:
	case SI_QUERY_GPU_TA_BUSY:
	case SI_QUERY_GPU_GDS_BUSY:
	case SI_QUERY_GPU_VGT_BUSY:
	case SI_QUERY_GPU_IA_BUSY:
	case SI_QUERY_GPU_SX_BUSY:
	case SI_QUERY_GPU_WD_BUSY:
	case SI_QUERY_GPU_BCI_BUSY:
	case SI_QUERY_GPU_SC_BUSY:
	case SI_QUERY_GPU_PA_BUSY:
	case SI_QUERY_GPU_DB_BUSY:
	case SI_QUERY_GPU_CP_BUSY:
	case SI_QUERY_GPU_CB_BUSY:
	case SI_QUERY_GPU_SDMA_BUSY:
	case SI_QUERY_GPU_PFP_BUSY:
	case SI_QUERY_GPU_MEQ_BUSY:
	case SI_QUERY_GPU_ME_BUSY:
	case SI_QUERY_GPU_SURF_SYNC_BUSY:
	case SI_QUERY_GPU_CP_DMA_BUSY:
	case SI_QUERY_GPU_SCRATCH_RAM_BUSY:
		query->end_result = si_end_counter(sctx->screen,
						     query->b.type,
						     query->begin_result);
		query->begin_result = 0;
		break;
	case SI_QUERY_NUM_COMPILATIONS:
		query->end_result = p_atomic_read(&sctx->screen->num_compilations);
		break;
	case SI_QUERY_NUM_SHADERS_CREATED:
		query->end_result = p_atomic_read(&sctx->screen->num_shaders_created);
		break;
	case SI_QUERY_BACK_BUFFER_PS_DRAW_RATIO:
		query->end_result = sctx->last_tex_ps_draw_ratio;
		break;
	case SI_QUERY_NUM_SHADER_CACHE_HITS:
		query->end_result =
			p_atomic_read(&sctx->screen->num_shader_cache_hits);
		break;
	case SI_QUERY_PD_NUM_PRIMS_ACCEPTED:
		query->end_result = sctx->compute_num_verts_accepted;
		break;
	case SI_QUERY_PD_NUM_PRIMS_REJECTED:
		query->end_result = sctx->compute_num_verts_rejected;
		break;
	case SI_QUERY_PD_NUM_PRIMS_INELIGIBLE:
		query->end_result = sctx->compute_num_verts_ineligible;
		break;
	case SI_QUERY_GPIN_ASIC_ID:
	case SI_QUERY_GPIN_NUM_SIMD:
	case SI_QUERY_GPIN_NUM_RB:
	case SI_QUERY_GPIN_NUM_SPI:
	case SI_QUERY_GPIN_NUM_SE:
		break;
	default:
		unreachable("si_query_sw_end: bad query type");
	}

	return true;
}

static bool si_query_sw_get_result(struct si_context *sctx,
				   struct si_query *squery,
				   bool wait,
				   union pipe_query_result *result)
{
	struct si_query_sw *query = (struct si_query_sw *)squery;

	switch (query->b.type) {
	case PIPE_QUERY_TIMESTAMP_DISJOINT:
		/* Convert from cycles per millisecond to cycles per second (Hz). */
		result->timestamp_disjoint.frequency =
			(uint64_t)sctx->screen->info.clock_crystal_freq * 1000;
		result->timestamp_disjoint.disjoint = false;
		return true;
	case PIPE_QUERY_GPU_FINISHED: {
		struct pipe_screen *screen = sctx->b.screen;
		struct pipe_context *ctx = squery->b.flushed ? NULL : &sctx->b;

		result->b = screen->fence_finish(screen, ctx, query->fence,
						 wait ? PIPE_TIMEOUT_INFINITE : 0);
		return result->b;
	}

	case SI_QUERY_GFX_BO_LIST_SIZE:
		result->u64 = (query->end_result - query->begin_result) /
			      (query->end_time - query->begin_time);
		return true;
	case SI_QUERY_CS_THREAD_BUSY:
	case SI_QUERY_GALLIUM_THREAD_BUSY:
		result->u64 = (query->end_result - query->begin_result) * 100 /
			      (query->end_time - query->begin_time);
		return true;
	case SI_QUERY_PD_NUM_PRIMS_ACCEPTED:
	case SI_QUERY_PD_NUM_PRIMS_REJECTED:
	case SI_QUERY_PD_NUM_PRIMS_INELIGIBLE:
		result->u64 = ((unsigned)query->end_result -
			       (unsigned)query->begin_result) / 3;
		return true;
	case SI_QUERY_GPIN_ASIC_ID:
		result->u32 = 0;
		return true;
	case SI_QUERY_GPIN_NUM_SIMD:
		result->u32 = sctx->screen->info.num_good_compute_units;
		return true;
	case SI_QUERY_GPIN_NUM_RB:
		result->u32 = sctx->screen->info.num_render_backends;
		return true;
	case SI_QUERY_GPIN_NUM_SPI:
		result->u32 = 1; /* all supported chips have one SPI per SE */
		return true;
	case SI_QUERY_GPIN_NUM_SE:
		result->u32 = sctx->screen->info.max_se;
		return true;
	}

	result->u64 = query->end_result - query->begin_result;

	switch (query->b.type) {
	case SI_QUERY_BUFFER_WAIT_TIME:
	case SI_QUERY_GPU_TEMPERATURE:
		result->u64 /= 1000;
		break;
	case SI_QUERY_CURRENT_GPU_SCLK:
	case SI_QUERY_CURRENT_GPU_MCLK:
		result->u64 *= 1000000;
		break;
	}

	return true;
}


static const struct si_query_ops sw_query_ops = {
	.destroy = si_query_sw_destroy,
	.begin = si_query_sw_begin,
	.end = si_query_sw_end,
	.get_result = si_query_sw_get_result,
	.get_result_resource = NULL
};

static struct pipe_query *si_query_sw_create(unsigned query_type)
{
	struct si_query_sw *query;

	query = CALLOC_STRUCT(si_query_sw);
	if (!query)
		return NULL;

	query->b.type = query_type;
	query->b.ops = &sw_query_ops;

	return (struct pipe_query *)query;
}

void si_query_buffer_destroy(struct si_screen *sscreen, struct si_query_buffer *buffer)
{
	struct si_query_buffer *prev = buffer->previous;

	/* Release all query buffers. */
	while (prev) {
		struct si_query_buffer *qbuf = prev;
		prev = prev->previous;
		si_resource_reference(&qbuf->buf, NULL);
		FREE(qbuf);
	}

	si_resource_reference(&buffer->buf, NULL);
}

void si_query_buffer_reset(struct si_context *sctx, struct si_query_buffer *buffer)
{
	/* Discard all query buffers except for the oldest. */
	while (buffer->previous) {
		struct si_query_buffer *qbuf = buffer->previous;
		buffer->previous = qbuf->previous;

		si_resource_reference(&buffer->buf, NULL);
		buffer->buf = qbuf->buf; /* move ownership */
		FREE(qbuf);
	}
	buffer->results_end = 0;

	if (!buffer->buf)
		return;

	/* Discard even the oldest buffer if it can't be mapped without a stall. */
	if (si_rings_is_buffer_referenced(sctx, buffer->buf->buf, RADEON_USAGE_READWRITE) ||
	    !sctx->ws->buffer_wait(buffer->buf->buf, 0, RADEON_USAGE_READWRITE)) {
		si_resource_reference(&buffer->buf, NULL);
	} else {
		buffer->unprepared = true;
	}
}

bool si_query_buffer_alloc(struct si_context *sctx, struct si_query_buffer *buffer,
			   bool (*prepare_buffer)(struct si_context *, struct si_query_buffer*),
			   unsigned size)
{
	bool unprepared = buffer->unprepared;
	buffer->unprepared = false;

	if (!buffer->buf || buffer->results_end + size > buffer->buf->b.b.width0) {
		if (buffer->buf) {
			struct si_query_buffer *qbuf = MALLOC_STRUCT(si_query_buffer);
			memcpy(qbuf, buffer, sizeof(*qbuf));
			buffer->previous = qbuf;
		}
		buffer->results_end = 0;

		/* Queries are normally read by the CPU after
		 * being written by the gpu, hence staging is probably a good
		 * usage pattern.
		 */
		struct si_screen *screen = sctx->screen;
		unsigned buf_size = MAX2(size, screen->info.min_alloc_size);
		buffer->buf = si_resource(
			pipe_buffer_create(&screen->b, 0, PIPE_USAGE_STAGING, buf_size));
		if (unlikely(!buffer->buf))
			return false;
		unprepared = true;
	}

	if (unprepared && prepare_buffer) {
		if (unlikely(!prepare_buffer(sctx, buffer))) {
			si_resource_reference(&buffer->buf, NULL);
			return false;
		}
	}

	return true;
}


void si_query_hw_destroy(struct si_context *sctx, struct si_query *squery)
{
	struct si_query_hw *query = (struct si_query_hw *)squery;

	si_query_buffer_destroy(sctx->screen, &query->buffer);
	si_resource_reference(&query->workaround_buf, NULL);
	FREE(squery);
}

static bool si_query_hw_prepare_buffer(struct si_context *sctx,
				       struct si_query_buffer *qbuf)
{
	static const struct si_query_hw si_query_hw_s;
	struct si_query_hw *query = container_of(qbuf, &si_query_hw_s, buffer);
	struct si_screen *screen = sctx->screen;

	/* The caller ensures that the buffer is currently unused by the GPU. */
	uint32_t *results = screen->ws->buffer_map(qbuf->buf->buf, NULL,
						   PIPE_TRANSFER_WRITE |
						   PIPE_TRANSFER_UNSYNCHRONIZED);
	if (!results)
		return false;

	memset(results, 0, qbuf->buf->b.b.width0);

	if (query->b.type == PIPE_QUERY_OCCLUSION_COUNTER ||
	    query->b.type == PIPE_QUERY_OCCLUSION_PREDICATE ||
	    query->b.type == PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE) {
		unsigned max_rbs = screen->info.num_render_backends;
		unsigned enabled_rb_mask = screen->info.enabled_rb_mask;
		unsigned num_results;
		unsigned i, j;

		/* Set top bits for unused backends. */
		num_results = qbuf->buf->b.b.width0 / query->result_size;
		for (j = 0; j < num_results; j++) {
			for (i = 0; i < max_rbs; i++) {
				if (!(enabled_rb_mask & (1<<i))) {
					results[(i * 4)+1] = 0x80000000;
					results[(i * 4)+3] = 0x80000000;
				}
			}
			results += 4 * max_rbs;
		}
	}

	return true;
}

static void si_query_hw_get_result_resource(struct si_context *sctx,
					    struct si_query *squery,
					    bool wait,
					    enum pipe_query_value_type result_type,
					    int index,
					    struct pipe_resource *resource,
					    unsigned offset);

static void si_query_hw_do_emit_start(struct si_context *sctx,
				      struct si_query_hw *query,
				      struct si_resource *buffer,
				      uint64_t va);
static void si_query_hw_do_emit_stop(struct si_context *sctx,
				     struct si_query_hw *query,
				     struct si_resource *buffer,
				     uint64_t va);
static void si_query_hw_add_result(struct si_screen *sscreen,
				   struct si_query_hw *, void *buffer,
				   union pipe_query_result *result);
static void si_query_hw_clear_result(struct si_query_hw *,
				     union pipe_query_result *);

static struct si_query_hw_ops query_hw_default_hw_ops = {
	.prepare_buffer = si_query_hw_prepare_buffer,
	.emit_start = si_query_hw_do_emit_start,
	.emit_stop = si_query_hw_do_emit_stop,
	.clear_result = si_query_hw_clear_result,
	.add_result = si_query_hw_add_result,
};

static struct pipe_query *si_query_hw_create(struct si_screen *sscreen,
					     unsigned query_type,
					     unsigned index)
{
	struct si_query_hw *query = CALLOC_STRUCT(si_query_hw);
	if (!query)
		return NULL;

	query->b.type = query_type;
	query->b.ops = &query_hw_ops;
	query->ops = &query_hw_default_hw_ops;

	switch (query_type) {
	case PIPE_QUERY_OCCLUSION_COUNTER:
	case PIPE_QUERY_OCCLUSION_PREDICATE:
	case PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE:
		query->result_size = 16 * sscreen->info.num_render_backends;
		query->result_size += 16; /* for the fence + alignment */
		query->b.num_cs_dw_suspend = 6 + si_cp_write_fence_dwords(sscreen);
		break;
	case SI_QUERY_TIME_ELAPSED_SDMA:
		/* GET_GLOBAL_TIMESTAMP only works if the offset is a multiple of 32. */
		query->result_size = 64;
		break;
	case PIPE_QUERY_TIME_ELAPSED:
		query->result_size = 24;
		query->b.num_cs_dw_suspend = 8 + si_cp_write_fence_dwords(sscreen);
		break;
	case PIPE_QUERY_TIMESTAMP:
		query->result_size = 16;
		query->b.num_cs_dw_suspend = 8 + si_cp_write_fence_dwords(sscreen);
		query->flags = SI_QUERY_HW_FLAG_NO_START;
		break;
	case PIPE_QUERY_PRIMITIVES_EMITTED:
	case PIPE_QUERY_PRIMITIVES_GENERATED:
	case PIPE_QUERY_SO_STATISTICS:
	case PIPE_QUERY_SO_OVERFLOW_PREDICATE:
		/* NumPrimitivesWritten, PrimitiveStorageNeeded. */
		query->result_size = 32;
		query->b.num_cs_dw_suspend = 6;
		query->stream = index;
		break;
	case PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE:
		/* NumPrimitivesWritten, PrimitiveStorageNeeded. */
		query->result_size = 32 * SI_MAX_STREAMS;
		query->b.num_cs_dw_suspend = 6 * SI_MAX_STREAMS;
		break;
	case PIPE_QUERY_PIPELINE_STATISTICS:
		/* 11 values on GCN. */
		query->result_size = 11 * 16;
		query->result_size += 8; /* for the fence + alignment */
		query->b.num_cs_dw_suspend = 6 + si_cp_write_fence_dwords(sscreen);
		break;
	default:
		assert(0);
		FREE(query);
		return NULL;
	}

	return (struct pipe_query *)query;
}

static void si_update_occlusion_query_state(struct si_context *sctx,
					    unsigned type, int diff)
{
	if (type == PIPE_QUERY_OCCLUSION_COUNTER ||
	    type == PIPE_QUERY_OCCLUSION_PREDICATE ||
	    type == PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE) {
		bool old_enable = sctx->num_occlusion_queries != 0;
		bool old_perfect_enable =
			sctx->num_perfect_occlusion_queries != 0;
		bool enable, perfect_enable;

		sctx->num_occlusion_queries += diff;
		assert(sctx->num_occlusion_queries >= 0);

		if (type != PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE) {
			sctx->num_perfect_occlusion_queries += diff;
			assert(sctx->num_perfect_occlusion_queries >= 0);
		}

		enable = sctx->num_occlusion_queries != 0;
		perfect_enable = sctx->num_perfect_occlusion_queries != 0;

		if (enable != old_enable || perfect_enable != old_perfect_enable) {
			si_set_occlusion_query_state(sctx, old_perfect_enable);
		}
	}
}

static unsigned event_type_for_stream(unsigned stream)
{
	switch (stream) {
	default:
	case 0: return V_028A90_SAMPLE_STREAMOUTSTATS;
	case 1: return V_028A90_SAMPLE_STREAMOUTSTATS1;
	case 2: return V_028A90_SAMPLE_STREAMOUTSTATS2;
	case 3: return V_028A90_SAMPLE_STREAMOUTSTATS3;
	}
}

static void emit_sample_streamout(struct radeon_cmdbuf *cs, uint64_t va,
				  unsigned stream)
{
	radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 2, 0));
	radeon_emit(cs, EVENT_TYPE(event_type_for_stream(stream)) | EVENT_INDEX(3));
	radeon_emit(cs, va);
	radeon_emit(cs, va >> 32);
}

static void si_query_hw_do_emit_start(struct si_context *sctx,
					struct si_query_hw *query,
					struct si_resource *buffer,
					uint64_t va)
{
	struct radeon_cmdbuf *cs = sctx->gfx_cs;

	switch (query->b.type) {
	case SI_QUERY_TIME_ELAPSED_SDMA:
		si_dma_emit_timestamp(sctx, buffer, va - buffer->gpu_address);
		return;
	case PIPE_QUERY_OCCLUSION_COUNTER:
	case PIPE_QUERY_OCCLUSION_PREDICATE:
	case PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE:
		radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 2, 0));
		radeon_emit(cs, EVENT_TYPE(V_028A90_ZPASS_DONE) | EVENT_INDEX(1));
		radeon_emit(cs, va);
		radeon_emit(cs, va >> 32);
		break;
	case PIPE_QUERY_PRIMITIVES_EMITTED:
	case PIPE_QUERY_PRIMITIVES_GENERATED:
	case PIPE_QUERY_SO_STATISTICS:
	case PIPE_QUERY_SO_OVERFLOW_PREDICATE:
		emit_sample_streamout(cs, va, query->stream);
		break;
	case PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE:
		for (unsigned stream = 0; stream < SI_MAX_STREAMS; ++stream)
			emit_sample_streamout(cs, va + 32 * stream, stream);
		break;
	case PIPE_QUERY_TIME_ELAPSED:
		si_cp_release_mem(sctx, cs, V_028A90_BOTTOM_OF_PIPE_TS, 0,
				  EOP_DST_SEL_MEM, EOP_INT_SEL_NONE,
				  EOP_DATA_SEL_TIMESTAMP, NULL, va,
				  0, query->b.type);
		break;
	case PIPE_QUERY_PIPELINE_STATISTICS:
		radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 2, 0));
		radeon_emit(cs, EVENT_TYPE(V_028A90_SAMPLE_PIPELINESTAT) | EVENT_INDEX(2));
		radeon_emit(cs, va);
		radeon_emit(cs, va >> 32);
		break;
	default:
		assert(0);
	}
	radeon_add_to_buffer_list(sctx, sctx->gfx_cs, query->buffer.buf, RADEON_USAGE_WRITE,
				  RADEON_PRIO_QUERY);
}

static void si_query_hw_emit_start(struct si_context *sctx,
				   struct si_query_hw *query)
{
	uint64_t va;

	if (!si_query_buffer_alloc(sctx, &query->buffer, query->ops->prepare_buffer,
				   query->result_size))
		return;

	si_update_occlusion_query_state(sctx, query->b.type, 1);
	si_update_prims_generated_query_state(sctx, query->b.type, 1);

	if (query->b.type == PIPE_QUERY_PIPELINE_STATISTICS)
		sctx->num_pipeline_stat_queries++;

	if (query->b.type != SI_QUERY_TIME_ELAPSED_SDMA)
		si_need_gfx_cs_space(sctx);

	va = query->buffer.buf->gpu_address + query->buffer.results_end;
	query->ops->emit_start(sctx, query, query->buffer.buf, va);
}

static void si_query_hw_do_emit_stop(struct si_context *sctx,
				       struct si_query_hw *query,
				       struct si_resource *buffer,
				       uint64_t va)
{
	struct radeon_cmdbuf *cs = sctx->gfx_cs;
	uint64_t fence_va = 0;

	switch (query->b.type) {
	case SI_QUERY_TIME_ELAPSED_SDMA:
		si_dma_emit_timestamp(sctx, buffer, va + 32 - buffer->gpu_address);
		return;
	case PIPE_QUERY_OCCLUSION_COUNTER:
	case PIPE_QUERY_OCCLUSION_PREDICATE:
	case PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE:
		va += 8;
		radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 2, 0));
		radeon_emit(cs, EVENT_TYPE(V_028A90_ZPASS_DONE) | EVENT_INDEX(1));
		radeon_emit(cs, va);
		radeon_emit(cs, va >> 32);

		fence_va = va + sctx->screen->info.num_render_backends * 16 - 8;
		break;
	case PIPE_QUERY_PRIMITIVES_EMITTED:
	case PIPE_QUERY_PRIMITIVES_GENERATED:
	case PIPE_QUERY_SO_STATISTICS:
	case PIPE_QUERY_SO_OVERFLOW_PREDICATE:
		va += 16;
		emit_sample_streamout(cs, va, query->stream);
		break;
	case PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE:
		va += 16;
		for (unsigned stream = 0; stream < SI_MAX_STREAMS; ++stream)
			emit_sample_streamout(cs, va + 32 * stream, stream);
		break;
	case PIPE_QUERY_TIME_ELAPSED:
		va += 8;
		/* fall through */
	case PIPE_QUERY_TIMESTAMP:
		si_cp_release_mem(sctx, cs, V_028A90_BOTTOM_OF_PIPE_TS, 0,
				  EOP_DST_SEL_MEM, EOP_INT_SEL_NONE,
				  EOP_DATA_SEL_TIMESTAMP, NULL, va,
				  0, query->b.type);
		fence_va = va + 8;
		break;
	case PIPE_QUERY_PIPELINE_STATISTICS: {
		unsigned sample_size = (query->result_size - 8) / 2;

		va += sample_size;
		radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 2, 0));
		radeon_emit(cs, EVENT_TYPE(V_028A90_SAMPLE_PIPELINESTAT) | EVENT_INDEX(2));
		radeon_emit(cs, va);
		radeon_emit(cs, va >> 32);

		fence_va = va + sample_size;
		break;
	}
	default:
		assert(0);
	}
	radeon_add_to_buffer_list(sctx, sctx->gfx_cs, query->buffer.buf, RADEON_USAGE_WRITE,
				  RADEON_PRIO_QUERY);

	if (fence_va) {
		si_cp_release_mem(sctx, cs, V_028A90_BOTTOM_OF_PIPE_TS, 0,
				  EOP_DST_SEL_MEM, EOP_INT_SEL_NONE,
				  EOP_DATA_SEL_VALUE_32BIT,
				  query->buffer.buf, fence_va, 0x80000000,
				  query->b.type);
	}
}

static void si_query_hw_emit_stop(struct si_context *sctx,
				  struct si_query_hw *query)
{
	uint64_t va;

	/* The queries which need begin already called this in begin_query. */
	if (query->flags & SI_QUERY_HW_FLAG_NO_START) {
		si_need_gfx_cs_space(sctx);
		if (!si_query_buffer_alloc(sctx, &query->buffer, query->ops->prepare_buffer,
					   query->result_size))
			return;
	}

	if (!query->buffer.buf)
		return; // previous buffer allocation failure

	/* emit end query */
	va = query->buffer.buf->gpu_address + query->buffer.results_end;

	query->ops->emit_stop(sctx, query, query->buffer.buf, va);

	query->buffer.results_end += query->result_size;

	si_update_occlusion_query_state(sctx, query->b.type, -1);
	si_update_prims_generated_query_state(sctx, query->b.type, -1);

	if (query->b.type == PIPE_QUERY_PIPELINE_STATISTICS)
		sctx->num_pipeline_stat_queries--;
}

static void emit_set_predicate(struct si_context *ctx,
			       struct si_resource *buf, uint64_t va,
			       uint32_t op)
{
	struct radeon_cmdbuf *cs = ctx->gfx_cs;

	if (ctx->chip_class >= GFX9) {
		radeon_emit(cs, PKT3(PKT3_SET_PREDICATION, 2, 0));
		radeon_emit(cs, op);
		radeon_emit(cs, va);
		radeon_emit(cs, va >> 32);
	} else {
		radeon_emit(cs, PKT3(PKT3_SET_PREDICATION, 1, 0));
		radeon_emit(cs, va);
		radeon_emit(cs, op | ((va >> 32) & 0xFF));
	}
	radeon_add_to_buffer_list(ctx, ctx->gfx_cs, buf, RADEON_USAGE_READ,
				  RADEON_PRIO_QUERY);
}

static void si_emit_query_predication(struct si_context *ctx)
{
	struct si_query_hw *query = (struct si_query_hw *)ctx->render_cond;
	struct si_query_buffer *qbuf;
	uint32_t op;
	bool flag_wait, invert;

	if (!query)
		return;

	if (ctx->chip_class == GFX10 &&
	    (query->b.type == PIPE_QUERY_SO_OVERFLOW_PREDICATE ||
	     query->b.type == PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE)) {
		assert(!"not implemented");
	}

	invert = ctx->render_cond_invert;
	flag_wait = ctx->render_cond_mode == PIPE_RENDER_COND_WAIT ||
		    ctx->render_cond_mode == PIPE_RENDER_COND_BY_REGION_WAIT;

	if (query->workaround_buf) {
		op = PRED_OP(PREDICATION_OP_BOOL64);
	} else {
		switch (query->b.type) {
		case PIPE_QUERY_OCCLUSION_COUNTER:
		case PIPE_QUERY_OCCLUSION_PREDICATE:
		case PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE:
			op = PRED_OP(PREDICATION_OP_ZPASS);
			break;
		case PIPE_QUERY_SO_OVERFLOW_PREDICATE:
		case PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE:
			op = PRED_OP(PREDICATION_OP_PRIMCOUNT);
			invert = !invert;
			break;
		default:
			assert(0);
			return;
		}
	}

	/* if true then invert, see GL_ARB_conditional_render_inverted */
	if (invert)
		op |= PREDICATION_DRAW_NOT_VISIBLE; /* Draw if not visible or overflow */
	else
		op |= PREDICATION_DRAW_VISIBLE; /* Draw if visible or no overflow */

	/* Use the value written by compute shader as a workaround. Note that
	 * the wait flag does not apply in this predication mode.
	 *
	 * The shader outputs the result value to L2. Workarounds only affect GFX8
	 * and later, where the CP reads data from L2, so we don't need an
	 * additional flush.
	 */
	if (query->workaround_buf) {
		uint64_t va = query->workaround_buf->gpu_address + query->workaround_offset;
		emit_set_predicate(ctx, query->workaround_buf, va, op);
		return;
	}

	op |= flag_wait ? PREDICATION_HINT_WAIT : PREDICATION_HINT_NOWAIT_DRAW;

	/* emit predicate packets for all data blocks */
	for (qbuf = &query->buffer; qbuf; qbuf = qbuf->previous) {
		unsigned results_base = 0;
		uint64_t va_base = qbuf->buf->gpu_address;

		while (results_base < qbuf->results_end) {
			uint64_t va = va_base + results_base;

			if (query->b.type == PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE) {
				for (unsigned stream = 0; stream < SI_MAX_STREAMS; ++stream) {
					emit_set_predicate(ctx, qbuf->buf, va + 32 * stream, op);

					/* set CONTINUE bit for all packets except the first */
					op |= PREDICATION_CONTINUE;
				}
			} else {
				emit_set_predicate(ctx, qbuf->buf, va, op);
				op |= PREDICATION_CONTINUE;
			}

			results_base += query->result_size;
		}
	}
}

static struct pipe_query *si_create_query(struct pipe_context *ctx, unsigned query_type, unsigned index)
{
	struct si_screen *sscreen =
		(struct si_screen *)ctx->screen;

	if (query_type == PIPE_QUERY_TIMESTAMP_DISJOINT ||
	    query_type == PIPE_QUERY_GPU_FINISHED ||
	    (query_type >= PIPE_QUERY_DRIVER_SPECIFIC &&
	     query_type != SI_QUERY_TIME_ELAPSED_SDMA))
		return si_query_sw_create(query_type);

	if (sscreen->info.chip_class >= GFX10 &&
	    (query_type == PIPE_QUERY_PRIMITIVES_EMITTED ||
	     query_type == PIPE_QUERY_PRIMITIVES_GENERATED ||
	     query_type == PIPE_QUERY_SO_STATISTICS ||
	     query_type == PIPE_QUERY_SO_OVERFLOW_PREDICATE ||
	     query_type == PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE))
		return gfx10_sh_query_create(sscreen, query_type, index);

	return si_query_hw_create(sscreen, query_type, index);
}

static void si_destroy_query(struct pipe_context *ctx, struct pipe_query *query)
{
	struct si_context *sctx = (struct si_context *)ctx;
	struct si_query *squery = (struct si_query *)query;

	squery->ops->destroy(sctx, squery);
}

static bool si_begin_query(struct pipe_context *ctx,
			   struct pipe_query *query)
{
	struct si_context *sctx = (struct si_context *)ctx;
	struct si_query *squery = (struct si_query *)query;

	return squery->ops->begin(sctx, squery);
}

bool si_query_hw_begin(struct si_context *sctx,
		       struct si_query *squery)
{
	struct si_query_hw *query = (struct si_query_hw *)squery;

	if (query->flags & SI_QUERY_HW_FLAG_NO_START) {
		assert(0);
		return false;
	}

	if (!(query->flags & SI_QUERY_HW_FLAG_BEGIN_RESUMES))
		si_query_buffer_reset(sctx, &query->buffer);

	si_resource_reference(&query->workaround_buf, NULL);

	si_query_hw_emit_start(sctx, query);
	if (!query->buffer.buf)
		return false;

	LIST_ADDTAIL(&query->b.active_list, &sctx->active_queries);
	sctx->num_cs_dw_queries_suspend += query->b.num_cs_dw_suspend;
	return true;
}

static bool si_end_query(struct pipe_context *ctx, struct pipe_query *query)
{
	struct si_context *sctx = (struct si_context *)ctx;
	struct si_query *squery = (struct si_query *)query;

	return squery->ops->end(sctx, squery);
}

bool si_query_hw_end(struct si_context *sctx,
		     struct si_query *squery)
{
	struct si_query_hw *query = (struct si_query_hw *)squery;

	if (query->flags & SI_QUERY_HW_FLAG_NO_START)
		si_query_buffer_reset(sctx, &query->buffer);

	si_query_hw_emit_stop(sctx, query);

	if (!(query->flags & SI_QUERY_HW_FLAG_NO_START)) {
		LIST_DELINIT(&query->b.active_list);
		sctx->num_cs_dw_queries_suspend -= query->b.num_cs_dw_suspend;
	}

	if (!query->buffer.buf)
		return false;

	return true;
}

static void si_get_hw_query_params(struct si_context *sctx,
				   struct si_query_hw *squery, int index,
				   struct si_hw_query_params *params)
{
	unsigned max_rbs = sctx->screen->info.num_render_backends;

	params->pair_stride = 0;
	params->pair_count = 1;

	switch (squery->b.type) {
	case PIPE_QUERY_OCCLUSION_COUNTER:
	case PIPE_QUERY_OCCLUSION_PREDICATE:
	case PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE:
		params->start_offset = 0;
		params->end_offset = 8;
		params->fence_offset = max_rbs * 16;
		params->pair_stride = 16;
		params->pair_count = max_rbs;
		break;
	case PIPE_QUERY_TIME_ELAPSED:
		params->start_offset = 0;
		params->end_offset = 8;
		params->fence_offset = 16;
		break;
	case PIPE_QUERY_TIMESTAMP:
		params->start_offset = 0;
		params->end_offset = 0;
		params->fence_offset = 8;
		break;
	case PIPE_QUERY_PRIMITIVES_EMITTED:
		params->start_offset = 8;
		params->end_offset = 24;
		params->fence_offset = params->end_offset + 4;
		break;
	case PIPE_QUERY_PRIMITIVES_GENERATED:
		params->start_offset = 0;
		params->end_offset = 16;
		params->fence_offset = params->end_offset + 4;
		break;
	case PIPE_QUERY_SO_STATISTICS:
		params->start_offset = 8 - index * 8;
		params->end_offset = 24 - index * 8;
		params->fence_offset = params->end_offset + 4;
		break;
	case PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE:
		params->pair_count = SI_MAX_STREAMS;
		params->pair_stride = 32;
	case PIPE_QUERY_SO_OVERFLOW_PREDICATE:
		params->start_offset = 0;
		params->end_offset = 16;

		/* We can re-use the high dword of the last 64-bit value as a
		 * fence: it is initialized as 0, and the high bit is set by
		 * the write of the streamout stats event.
		 */
		params->fence_offset = squery->result_size - 4;
		break;
	case PIPE_QUERY_PIPELINE_STATISTICS:
	{
		static const unsigned offsets[] = {56, 48, 24, 32, 40, 16, 8, 0, 64, 72, 80};
		params->start_offset = offsets[index];
		params->end_offset = 88 + offsets[index];
		params->fence_offset = 2 * 88;
		break;
	}
	default:
		unreachable("si_get_hw_query_params unsupported");
	}
}

static unsigned si_query_read_result(void *map, unsigned start_index, unsigned end_index,
				     bool test_status_bit)
{
	uint32_t *current_result = (uint32_t*)map;
	uint64_t start, end;

	start = (uint64_t)current_result[start_index] |
		(uint64_t)current_result[start_index+1] << 32;
	end = (uint64_t)current_result[end_index] |
	      (uint64_t)current_result[end_index+1] << 32;

	if (!test_status_bit ||
	    ((start & 0x8000000000000000UL) && (end & 0x8000000000000000UL))) {
		return end - start;
	}
	return 0;
}

static void si_query_hw_add_result(struct si_screen *sscreen,
				     struct si_query_hw *query,
				     void *buffer,
				     union pipe_query_result *result)
{
	unsigned max_rbs = sscreen->info.num_render_backends;

	switch (query->b.type) {
	case PIPE_QUERY_OCCLUSION_COUNTER: {
		for (unsigned i = 0; i < max_rbs; ++i) {
			unsigned results_base = i * 16;
			result->u64 +=
				si_query_read_result(buffer + results_base, 0, 2, true);
		}
		break;
	}
	case PIPE_QUERY_OCCLUSION_PREDICATE:
	case PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE: {
		for (unsigned i = 0; i < max_rbs; ++i) {
			unsigned results_base = i * 16;
			result->b = result->b ||
				si_query_read_result(buffer + results_base, 0, 2, true) != 0;
		}
		break;
	}
	case PIPE_QUERY_TIME_ELAPSED:
		result->u64 += si_query_read_result(buffer, 0, 2, false);
		break;
	case SI_QUERY_TIME_ELAPSED_SDMA:
		result->u64 += si_query_read_result(buffer, 0, 32/4, false);
		break;
	case PIPE_QUERY_TIMESTAMP:
		result->u64 = *(uint64_t*)buffer;
		break;
	case PIPE_QUERY_PRIMITIVES_EMITTED:
		/* SAMPLE_STREAMOUTSTATS stores this structure:
		 * {
		 *    u64 NumPrimitivesWritten;
		 *    u64 PrimitiveStorageNeeded;
		 * }
		 * We only need NumPrimitivesWritten here. */
		result->u64 += si_query_read_result(buffer, 2, 6, true);
		break;
	case PIPE_QUERY_PRIMITIVES_GENERATED:
		/* Here we read PrimitiveStorageNeeded. */
		result->u64 += si_query_read_result(buffer, 0, 4, true);
		break;
	case PIPE_QUERY_SO_STATISTICS:
		result->so_statistics.num_primitives_written +=
			si_query_read_result(buffer, 2, 6, true);
		result->so_statistics.primitives_storage_needed +=
			si_query_read_result(buffer, 0, 4, true);
		break;
	case PIPE_QUERY_SO_OVERFLOW_PREDICATE:
		result->b = result->b ||
			si_query_read_result(buffer, 2, 6, true) !=
			si_query_read_result(buffer, 0, 4, true);
		break;
	case PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE:
		for (unsigned stream = 0; stream < SI_MAX_STREAMS; ++stream) {
			result->b = result->b ||
				si_query_read_result(buffer, 2, 6, true) !=
				si_query_read_result(buffer, 0, 4, true);
			buffer = (char *)buffer + 32;
		}
		break;
	case PIPE_QUERY_PIPELINE_STATISTICS:
		result->pipeline_statistics.ps_invocations +=
			si_query_read_result(buffer, 0, 22, false);
		result->pipeline_statistics.c_primitives +=
			si_query_read_result(buffer, 2, 24, false);
		result->pipeline_statistics.c_invocations +=
			si_query_read_result(buffer, 4, 26, false);
		result->pipeline_statistics.vs_invocations +=
			si_query_read_result(buffer, 6, 28, false);
		result->pipeline_statistics.gs_invocations +=
			si_query_read_result(buffer, 8, 30, false);
		result->pipeline_statistics.gs_primitives +=
			si_query_read_result(buffer, 10, 32, false);
		result->pipeline_statistics.ia_primitives +=
			si_query_read_result(buffer, 12, 34, false);
		result->pipeline_statistics.ia_vertices +=
			si_query_read_result(buffer, 14, 36, false);
		result->pipeline_statistics.hs_invocations +=
			si_query_read_result(buffer, 16, 38, false);
		result->pipeline_statistics.ds_invocations +=
			si_query_read_result(buffer, 18, 40, false);
		result->pipeline_statistics.cs_invocations +=
			si_query_read_result(buffer, 20, 42, false);
#if 0 /* for testing */
		printf("Pipeline stats: IA verts=%llu, IA prims=%llu, VS=%llu, HS=%llu, "
		       "DS=%llu, GS=%llu, GS prims=%llu, Clipper=%llu, "
		       "Clipper prims=%llu, PS=%llu, CS=%llu\n",
		       result->pipeline_statistics.ia_vertices,
		       result->pipeline_statistics.ia_primitives,
		       result->pipeline_statistics.vs_invocations,
		       result->pipeline_statistics.hs_invocations,
		       result->pipeline_statistics.ds_invocations,
		       result->pipeline_statistics.gs_invocations,
		       result->pipeline_statistics.gs_primitives,
		       result->pipeline_statistics.c_invocations,
		       result->pipeline_statistics.c_primitives,
		       result->pipeline_statistics.ps_invocations,
		       result->pipeline_statistics.cs_invocations);
#endif
		break;
	default:
		assert(0);
	}
}

void si_query_hw_suspend(struct si_context *sctx, struct si_query *query)
{
	si_query_hw_emit_stop(sctx, (struct si_query_hw *)query);
}

void si_query_hw_resume(struct si_context *sctx, struct si_query *query)
{
	si_query_hw_emit_start(sctx, (struct si_query_hw *)query);
}

static const struct si_query_ops query_hw_ops = {
	.destroy = si_query_hw_destroy,
	.begin = si_query_hw_begin,
	.end = si_query_hw_end,
	.get_result = si_query_hw_get_result,
	.get_result_resource = si_query_hw_get_result_resource,

	.suspend = si_query_hw_suspend,
	.resume = si_query_hw_resume,
};

static bool si_get_query_result(struct pipe_context *ctx,
				struct pipe_query *query, bool wait,
				union pipe_query_result *result)
{
	struct si_context *sctx = (struct si_context *)ctx;
	struct si_query *squery = (struct si_query *)query;

	return squery->ops->get_result(sctx, squery, wait, result);
}

static void si_get_query_result_resource(struct pipe_context *ctx,
					 struct pipe_query *query,
					 bool wait,
					 enum pipe_query_value_type result_type,
					 int index,
					 struct pipe_resource *resource,
					 unsigned offset)
{
	struct si_context *sctx = (struct si_context *)ctx;
	struct si_query *squery = (struct si_query *)query;

	squery->ops->get_result_resource(sctx, squery, wait, result_type, index,
	                                 resource, offset);
}

static void si_query_hw_clear_result(struct si_query_hw *query,
				       union pipe_query_result *result)
{
	util_query_clear_result(result, query->b.type);
}

bool si_query_hw_get_result(struct si_context *sctx,
			    struct si_query *squery,
			    bool wait, union pipe_query_result *result)
{
	struct si_screen *sscreen = sctx->screen;
	struct si_query_hw *query = (struct si_query_hw *)squery;
	struct si_query_buffer *qbuf;

	query->ops->clear_result(query, result);

	for (qbuf = &query->buffer; qbuf; qbuf = qbuf->previous) {
		unsigned usage = PIPE_TRANSFER_READ |
				 (wait ? 0 : PIPE_TRANSFER_DONTBLOCK);
		unsigned results_base = 0;
		void *map;

		if (squery->b.flushed)
			map = sctx->ws->buffer_map(qbuf->buf->buf, NULL, usage);
		else
			map = si_buffer_map_sync_with_rings(sctx, qbuf->buf, usage);

		if (!map)
			return false;

		while (results_base != qbuf->results_end) {
			query->ops->add_result(sscreen, query, map + results_base,
					       result);
			results_base += query->result_size;
		}
	}

	/* Convert the time to expected units. */
	if (squery->type == PIPE_QUERY_TIME_ELAPSED ||
	    squery->type == SI_QUERY_TIME_ELAPSED_SDMA ||
	    squery->type == PIPE_QUERY_TIMESTAMP) {
		result->u64 = (1000000 * result->u64) / sscreen->info.clock_crystal_freq;
	}
	return true;
}

static void si_query_hw_get_result_resource(struct si_context *sctx,
                                              struct si_query *squery,
                                              bool wait,
                                              enum pipe_query_value_type result_type,
                                              int index,
                                              struct pipe_resource *resource,
                                              unsigned offset)
{
	struct si_query_hw *query = (struct si_query_hw *)squery;
	struct si_query_buffer *qbuf;
	struct si_query_buffer *qbuf_prev;
	struct pipe_resource *tmp_buffer = NULL;
	unsigned tmp_buffer_offset = 0;
	struct si_qbo_state saved_state = {};
	struct pipe_grid_info grid = {};
	struct pipe_constant_buffer constant_buffer = {};
	struct pipe_shader_buffer ssbo[3];
	struct si_hw_query_params params;
	struct {
		uint32_t end_offset;
		uint32_t result_stride;
		uint32_t result_count;
		uint32_t config;
		uint32_t fence_offset;
		uint32_t pair_stride;
		uint32_t pair_count;
	} consts;

	if (!sctx->query_result_shader) {
		sctx->query_result_shader = si_create_query_result_cs(sctx);
		if (!sctx->query_result_shader)
			return;
	}

	if (query->buffer.previous) {
		u_suballocator_alloc(sctx->allocator_zeroed_memory, 16, 16,
				     &tmp_buffer_offset, &tmp_buffer);
		if (!tmp_buffer)
			return;
	}

	si_save_qbo_state(sctx, &saved_state);

	si_get_hw_query_params(sctx, query, index >= 0 ? index : 0, &params);
	consts.end_offset = params.end_offset - params.start_offset;
	consts.fence_offset = params.fence_offset - params.start_offset;
	consts.result_stride = query->result_size;
	consts.pair_stride = params.pair_stride;
	consts.pair_count = params.pair_count;

	constant_buffer.buffer_size = sizeof(consts);
	constant_buffer.user_buffer = &consts;

	ssbo[1].buffer = tmp_buffer;
	ssbo[1].buffer_offset = tmp_buffer_offset;
	ssbo[1].buffer_size = 16;

	ssbo[2] = ssbo[1];

	sctx->b.bind_compute_state(&sctx->b, sctx->query_result_shader);

	grid.block[0] = 1;
	grid.block[1] = 1;
	grid.block[2] = 1;
	grid.grid[0] = 1;
	grid.grid[1] = 1;
	grid.grid[2] = 1;

	consts.config = 0;
	if (index < 0)
		consts.config |= 4;
	if (query->b.type == PIPE_QUERY_OCCLUSION_PREDICATE ||
	    query->b.type == PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE)
		consts.config |= 8;
	else if (query->b.type == PIPE_QUERY_SO_OVERFLOW_PREDICATE ||
		 query->b.type == PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE)
		consts.config |= 8 | 256;
	else if (query->b.type == PIPE_QUERY_TIMESTAMP ||
		 query->b.type == PIPE_QUERY_TIME_ELAPSED)
		consts.config |= 32;

	switch (result_type) {
	case PIPE_QUERY_TYPE_U64:
	case PIPE_QUERY_TYPE_I64:
		consts.config |= 64;
		break;
	case PIPE_QUERY_TYPE_I32:
		consts.config |= 128;
		break;
	case PIPE_QUERY_TYPE_U32:
		break;
	}

	sctx->flags |= sctx->screen->barrier_flags.cp_to_L2;

	for (qbuf = &query->buffer; qbuf; qbuf = qbuf_prev) {
		if (query->b.type != PIPE_QUERY_TIMESTAMP) {
			qbuf_prev = qbuf->previous;
			consts.result_count = qbuf->results_end / query->result_size;
			consts.config &= ~3;
			if (qbuf != &query->buffer)
				consts.config |= 1;
			if (qbuf->previous)
				consts.config |= 2;
		} else {
			/* Only read the last timestamp. */
			qbuf_prev = NULL;
			consts.result_count = 0;
			consts.config |= 16;
			params.start_offset += qbuf->results_end - query->result_size;
		}

		sctx->b.set_constant_buffer(&sctx->b, PIPE_SHADER_COMPUTE, 0, &constant_buffer);

		ssbo[0].buffer = &qbuf->buf->b.b;
		ssbo[0].buffer_offset = params.start_offset;
		ssbo[0].buffer_size = qbuf->results_end - params.start_offset;

		if (!qbuf->previous) {
			ssbo[2].buffer = resource;
			ssbo[2].buffer_offset = offset;
			ssbo[2].buffer_size = 8;

			si_resource(resource)->TC_L2_dirty = true;
		}

		sctx->b.set_shader_buffers(&sctx->b, PIPE_SHADER_COMPUTE, 0, 3, ssbo,
					   1 << 2);

		if (wait && qbuf == &query->buffer) {
			uint64_t va;

			/* Wait for result availability. Wait only for readiness
			 * of the last entry, since the fence writes should be
			 * serialized in the CP.
			 */
			va = qbuf->buf->gpu_address + qbuf->results_end - query->result_size;
			va += params.fence_offset;

			si_cp_wait_mem(sctx, sctx->gfx_cs, va, 0x80000000,
				       0x80000000, WAIT_REG_MEM_EQUAL);
		}

		sctx->b.launch_grid(&sctx->b, &grid);
		sctx->flags |= SI_CONTEXT_CS_PARTIAL_FLUSH;
	}

	si_restore_qbo_state(sctx, &saved_state);
	pipe_resource_reference(&tmp_buffer, NULL);
}

static void si_render_condition(struct pipe_context *ctx,
				struct pipe_query *query,
				bool condition,
				enum pipe_render_cond_flag mode)
{
	struct si_context *sctx = (struct si_context *)ctx;
	struct si_query_hw *squery = (struct si_query_hw *)query;
	struct si_atom *atom = &sctx->atoms.s.render_cond;

	if (query) {
		bool needs_workaround = false;

		/* There was a firmware regression in GFX8 which causes successive
		 * SET_PREDICATION packets to give the wrong answer for
		 * non-inverted stream overflow predication.
		 */
		if (((sctx->chip_class == GFX8 && sctx->screen->info.pfp_fw_feature < 49) ||
		     (sctx->chip_class == GFX9 && sctx->screen->info.pfp_fw_feature < 38)) &&
		    !condition &&
		    (squery->b.type == PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE ||
		     (squery->b.type == PIPE_QUERY_SO_OVERFLOW_PREDICATE &&
		      (squery->buffer.previous ||
		       squery->buffer.results_end > squery->result_size)))) {
			needs_workaround = true;
		}

		if (needs_workaround && !squery->workaround_buf) {
			bool old_force_off = sctx->render_cond_force_off;
			sctx->render_cond_force_off = true;

			u_suballocator_alloc(
				sctx->allocator_zeroed_memory, 8, 8,
				&squery->workaround_offset,
				(struct pipe_resource **)&squery->workaround_buf);

			/* Reset to NULL to avoid a redundant SET_PREDICATION
			 * from launching the compute grid.
			 */
			sctx->render_cond = NULL;

			ctx->get_query_result_resource(
				ctx, query, true, PIPE_QUERY_TYPE_U64, 0,
				&squery->workaround_buf->b.b, squery->workaround_offset);

			/* Settings this in the render cond atom is too late,
			 * so set it here. */
			sctx->flags |= sctx->screen->barrier_flags.L2_to_cp |
				       SI_CONTEXT_FLUSH_FOR_RENDER_COND;

			sctx->render_cond_force_off = old_force_off;
		}
	}

	sctx->render_cond = query;
	sctx->render_cond_invert = condition;
	sctx->render_cond_mode = mode;

	si_set_atom_dirty(sctx, atom, query != NULL);
}

void si_suspend_queries(struct si_context *sctx)
{
	struct si_query *query;

	LIST_FOR_EACH_ENTRY(query, &sctx->active_queries, active_list)
		query->ops->suspend(sctx, query);
}

void si_resume_queries(struct si_context *sctx)
{
	struct si_query *query;

	/* Check CS space here. Resuming must not be interrupted by flushes. */
	si_need_gfx_cs_space(sctx);

	LIST_FOR_EACH_ENTRY(query, &sctx->active_queries, active_list)
		query->ops->resume(sctx, query);
}

#define XFULL(name_, query_type_, type_, result_type_, group_id_) \
	{ \
		.name = name_, \
		.query_type = SI_QUERY_##query_type_, \
		.type = PIPE_DRIVER_QUERY_TYPE_##type_, \
		.result_type = PIPE_DRIVER_QUERY_RESULT_TYPE_##result_type_, \
		.group_id = group_id_ \
	}

#define X(name_, query_type_, type_, result_type_) \
	XFULL(name_, query_type_, type_, result_type_, ~(unsigned)0)

#define XG(group_, name_, query_type_, type_, result_type_) \
	XFULL(name_, query_type_, type_, result_type_, SI_QUERY_GROUP_##group_)

static struct pipe_driver_query_info si_driver_query_list[] = {
	X("num-compilations",		NUM_COMPILATIONS,	UINT64, CUMULATIVE),
	X("num-shaders-created",	NUM_SHADERS_CREATED,	UINT64, CUMULATIVE),
	X("num-shader-cache-hits",	NUM_SHADER_CACHE_HITS,	UINT64, CUMULATIVE),
	X("draw-calls",			DRAW_CALLS,		UINT64, AVERAGE),
	X("decompress-calls",		DECOMPRESS_CALLS,	UINT64, AVERAGE),
	X("MRT-draw-calls",		MRT_DRAW_CALLS,		UINT64, AVERAGE),
	X("prim-restart-calls",		PRIM_RESTART_CALLS,	UINT64, AVERAGE),
	X("spill-draw-calls",		SPILL_DRAW_CALLS,	UINT64, AVERAGE),
	X("compute-calls",		COMPUTE_CALLS,		UINT64, AVERAGE),
	X("spill-compute-calls",	SPILL_COMPUTE_CALLS,	UINT64, AVERAGE),
	X("dma-calls",			DMA_CALLS,		UINT64, AVERAGE),
	X("cp-dma-calls",		CP_DMA_CALLS,		UINT64, AVERAGE),
	X("num-vs-flushes",		NUM_VS_FLUSHES,		UINT64, AVERAGE),
	X("num-ps-flushes",		NUM_PS_FLUSHES,		UINT64, AVERAGE),
	X("num-cs-flushes",		NUM_CS_FLUSHES,		UINT64, AVERAGE),
	X("num-CB-cache-flushes",	NUM_CB_CACHE_FLUSHES,	UINT64, AVERAGE),
	X("num-DB-cache-flushes",	NUM_DB_CACHE_FLUSHES,	UINT64, AVERAGE),
	X("num-L2-invalidates",		NUM_L2_INVALIDATES,	UINT64, AVERAGE),
	X("num-L2-writebacks",		NUM_L2_WRITEBACKS,	UINT64, AVERAGE),
	X("num-resident-handles",	NUM_RESIDENT_HANDLES,	UINT64, AVERAGE),
	X("tc-offloaded-slots",		TC_OFFLOADED_SLOTS,     UINT64, AVERAGE),
	X("tc-direct-slots",		TC_DIRECT_SLOTS,	UINT64, AVERAGE),
	X("tc-num-syncs",		TC_NUM_SYNCS,		UINT64, AVERAGE),
	X("CS-thread-busy",		CS_THREAD_BUSY,		UINT64, AVERAGE),
	X("gallium-thread-busy",	GALLIUM_THREAD_BUSY,	UINT64, AVERAGE),
	X("requested-VRAM",		REQUESTED_VRAM,		BYTES, AVERAGE),
	X("requested-GTT",		REQUESTED_GTT,		BYTES, AVERAGE),
	X("mapped-VRAM",		MAPPED_VRAM,		BYTES, AVERAGE),
	X("mapped-GTT",			MAPPED_GTT,		BYTES, AVERAGE),
	X("buffer-wait-time",		BUFFER_WAIT_TIME,	MICROSECONDS, CUMULATIVE),
	X("num-mapped-buffers",		NUM_MAPPED_BUFFERS,	UINT64, AVERAGE),
	X("num-GFX-IBs",		NUM_GFX_IBS,		UINT64, AVERAGE),
	X("num-SDMA-IBs",		NUM_SDMA_IBS,		UINT64, AVERAGE),
	X("GFX-BO-list-size",		GFX_BO_LIST_SIZE,	UINT64, AVERAGE),
	X("GFX-IB-size",		GFX_IB_SIZE,		UINT64, AVERAGE),
	X("num-bytes-moved",		NUM_BYTES_MOVED,	BYTES, CUMULATIVE),
	X("num-evictions",		NUM_EVICTIONS,		UINT64, CUMULATIVE),
	X("VRAM-CPU-page-faults",	NUM_VRAM_CPU_PAGE_FAULTS, UINT64, CUMULATIVE),
	X("VRAM-usage",			VRAM_USAGE,		BYTES, AVERAGE),
	X("VRAM-vis-usage",		VRAM_VIS_USAGE,		BYTES, AVERAGE),
	X("GTT-usage",			GTT_USAGE,		BYTES, AVERAGE),
	X("back-buffer-ps-draw-ratio",	BACK_BUFFER_PS_DRAW_RATIO, UINT64, AVERAGE),

	/* GPIN queries are for the benefit of old versions of GPUPerfStudio,
	 * which use it as a fallback path to detect the GPU type.
	 *
	 * Note: The names of these queries are significant for GPUPerfStudio
	 * (and possibly their order as well). */
	XG(GPIN, "GPIN_000",		GPIN_ASIC_ID,		UINT, AVERAGE),
	XG(GPIN, "GPIN_001",		GPIN_NUM_SIMD,		UINT, AVERAGE),
	XG(GPIN, "GPIN_002",		GPIN_NUM_RB,		UINT, AVERAGE),
	XG(GPIN, "GPIN_003",		GPIN_NUM_SPI,		UINT, AVERAGE),
	XG(GPIN, "GPIN_004",		GPIN_NUM_SE,		UINT, AVERAGE),

	X("temperature",		GPU_TEMPERATURE,	UINT64, AVERAGE),
	X("shader-clock",		CURRENT_GPU_SCLK,	HZ, AVERAGE),
	X("memory-clock",		CURRENT_GPU_MCLK,	HZ, AVERAGE),

	/* The following queries must be at the end of the list because their
	 * availability is adjusted dynamically based on the DRM version. */
	X("GPU-load",			GPU_LOAD,		UINT64, AVERAGE),
	X("GPU-shaders-busy",		GPU_SHADERS_BUSY,	UINT64, AVERAGE),
	X("GPU-ta-busy",		GPU_TA_BUSY,		UINT64, AVERAGE),
	X("GPU-gds-busy",		GPU_GDS_BUSY,		UINT64, AVERAGE),
	X("GPU-vgt-busy",		GPU_VGT_BUSY,		UINT64, AVERAGE),
	X("GPU-ia-busy",		GPU_IA_BUSY,		UINT64, AVERAGE),
	X("GPU-sx-busy",		GPU_SX_BUSY,		UINT64, AVERAGE),
	X("GPU-wd-busy",		GPU_WD_BUSY,		UINT64, AVERAGE),
	X("GPU-bci-busy",		GPU_BCI_BUSY,		UINT64, AVERAGE),
	X("GPU-sc-busy",		GPU_SC_BUSY,		UINT64, AVERAGE),
	X("GPU-pa-busy",		GPU_PA_BUSY,		UINT64, AVERAGE),
	X("GPU-db-busy",		GPU_DB_BUSY,		UINT64, AVERAGE),
	X("GPU-cp-busy",		GPU_CP_BUSY,		UINT64, AVERAGE),
	X("GPU-cb-busy",		GPU_CB_BUSY,		UINT64, AVERAGE),

	/* SRBM_STATUS2 */
	X("GPU-sdma-busy",		GPU_SDMA_BUSY,		UINT64, AVERAGE),

	/* CP_STAT */
	X("GPU-pfp-busy",		GPU_PFP_BUSY,		UINT64, AVERAGE),
	X("GPU-meq-busy",		GPU_MEQ_BUSY,		UINT64, AVERAGE),
	X("GPU-me-busy",		GPU_ME_BUSY,		UINT64, AVERAGE),
	X("GPU-surf-sync-busy",		GPU_SURF_SYNC_BUSY,	UINT64, AVERAGE),
	X("GPU-cp-dma-busy",		GPU_CP_DMA_BUSY,	UINT64, AVERAGE),
	X("GPU-scratch-ram-busy",	GPU_SCRATCH_RAM_BUSY,	UINT64, AVERAGE),

	X("pd-num-prims-accepted",	PD_NUM_PRIMS_ACCEPTED,	UINT64, AVERAGE),
	X("pd-num-prims-rejected",	PD_NUM_PRIMS_REJECTED,	UINT64, AVERAGE),
	X("pd-num-prims-ineligible",	PD_NUM_PRIMS_INELIGIBLE,UINT64, AVERAGE),
};

#undef X
#undef XG
#undef XFULL

static unsigned si_get_num_queries(struct si_screen *sscreen)
{
	/* amdgpu */
	if (sscreen->info.is_amdgpu) {
		if (sscreen->info.chip_class >= GFX8)
			return ARRAY_SIZE(si_driver_query_list);
		else
			return ARRAY_SIZE(si_driver_query_list) - 7;
	}

	/* radeon */
	if (sscreen->info.has_read_registers_query) {
		if (sscreen->info.chip_class == GFX7)
			return ARRAY_SIZE(si_driver_query_list) - 6;
		else
			return ARRAY_SIZE(si_driver_query_list) - 7;
	}

	return ARRAY_SIZE(si_driver_query_list) - 21;
}

static int si_get_driver_query_info(struct pipe_screen *screen,
				    unsigned index,
				    struct pipe_driver_query_info *info)
{
	struct si_screen *sscreen = (struct si_screen*)screen;
	unsigned num_queries = si_get_num_queries(sscreen);

	if (!info) {
		unsigned num_perfcounters =
			si_get_perfcounter_info(sscreen, 0, NULL);

		return num_queries + num_perfcounters;
	}

	if (index >= num_queries)
		return si_get_perfcounter_info(sscreen, index - num_queries, info);

	*info = si_driver_query_list[index];

	switch (info->query_type) {
	case SI_QUERY_REQUESTED_VRAM:
	case SI_QUERY_VRAM_USAGE:
	case SI_QUERY_MAPPED_VRAM:
		info->max_value.u64 = sscreen->info.vram_size;
		break;
	case SI_QUERY_REQUESTED_GTT:
	case SI_QUERY_GTT_USAGE:
	case SI_QUERY_MAPPED_GTT:
		info->max_value.u64 = sscreen->info.gart_size;
		break;
	case SI_QUERY_GPU_TEMPERATURE:
		info->max_value.u64 = 125;
		break;
	case SI_QUERY_VRAM_VIS_USAGE:
		info->max_value.u64 = sscreen->info.vram_vis_size;
		break;
	}

	if (info->group_id != ~(unsigned)0 && sscreen->perfcounters)
		info->group_id += sscreen->perfcounters->num_groups;

	return 1;
}

/* Note: Unfortunately, GPUPerfStudio hardcodes the order of hardware
 * performance counter groups, so be careful when changing this and related
 * functions.
 */
static int si_get_driver_query_group_info(struct pipe_screen *screen,
					  unsigned index,
					  struct pipe_driver_query_group_info *info)
{
	struct si_screen *sscreen = (struct si_screen *)screen;
	unsigned num_pc_groups = 0;

	if (sscreen->perfcounters)
		num_pc_groups = sscreen->perfcounters->num_groups;

	if (!info)
		return num_pc_groups + SI_NUM_SW_QUERY_GROUPS;

	if (index < num_pc_groups)
		return si_get_perfcounter_group_info(sscreen, index, info);

	index -= num_pc_groups;
	if (index >= SI_NUM_SW_QUERY_GROUPS)
		return 0;

	info->name = "GPIN";
	info->max_active_queries = 5;
	info->num_queries = 5;
	return 1;
}

void si_init_query_functions(struct si_context *sctx)
{
	sctx->b.create_query = si_create_query;
	sctx->b.create_batch_query = si_create_batch_query;
	sctx->b.destroy_query = si_destroy_query;
	sctx->b.begin_query = si_begin_query;
	sctx->b.end_query = si_end_query;
	sctx->b.get_query_result = si_get_query_result;
	sctx->b.get_query_result_resource = si_get_query_result_resource;

	if (sctx->has_graphics) {
		sctx->atoms.s.render_cond.emit = si_emit_query_predication;
		sctx->b.render_condition = si_render_condition;
	}

	LIST_INITHEAD(&sctx->active_queries);
}

void si_init_screen_query_functions(struct si_screen *sscreen)
{
	sscreen->b.get_driver_query_info = si_get_driver_query_info;
	sscreen->b.get_driver_query_group_info = si_get_driver_query_group_info;
}
