/*
 * © Copyright 2018 Alyssa Rosenzweig
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
 */

#ifndef __BUILDER_H__
#define __BUILDER_H__

#define _LARGEFILE64_SOURCE 1
#define CACHE_LINE_SIZE 1024 /* TODO */
#include <sys/mman.h>
#include <assert.h>
#include "pan_resource.h"
#include "pan_job.h"
#include "pan_blend.h"

#include "pipe/p_compiler.h"
#include "pipe/p_config.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_format.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/u_blitter.h"
#include "util/hash_table.h"

#include "midgard/midgard_compile.h"
#include "compiler/shader_enums.h"

/* Forward declare to avoid extra header dep */
struct prim_convert_context;

#define MAX_VARYINGS   4096

//#define PAN_DIRTY_CLEAR	     (1 << 0)
#define PAN_DIRTY_RASTERIZER (1 << 2)
#define PAN_DIRTY_FS	     (1 << 3)
#define PAN_DIRTY_FRAG_CORE  (PAN_DIRTY_FS) /* Dirty writes are tied */
#define PAN_DIRTY_VS	     (1 << 4)
#define PAN_DIRTY_VERTEX     (1 << 5)
#define PAN_DIRTY_VERT_BUF   (1 << 6)
//#define PAN_DIRTY_VIEWPORT   (1 << 7)
#define PAN_DIRTY_SAMPLERS   (1 << 8)
#define PAN_DIRTY_TEXTURES   (1 << 9)

#define SET_BIT(lval, bit, cond) \
	if (cond) \
		lval |= (bit); \
	else \
		lval &= ~(bit);

struct panfrost_constant_buffer {
        struct pipe_constant_buffer cb[PIPE_MAX_CONSTANT_BUFFERS];
        uint32_t enabled_mask;
        uint32_t dirty_mask;
};

struct panfrost_query {
        /* Passthrough from Gallium */
        unsigned type;
        unsigned index;

        /* Memory for the GPU to writeback the value of the query */
        struct panfrost_transfer transfer;
};

struct panfrost_fence {
        struct pipe_reference reference;
        int fd;
};

struct panfrost_context {
        /* Gallium context */
        struct pipe_context base;

        /* Compiler context */
        struct midgard_screen compiler;

        /* Bound job and map of panfrost_job_key to jobs */
        struct panfrost_job *job;
        struct hash_table *jobs;

        /* panfrost_resource -> panfrost_job */
        struct hash_table *write_jobs;

        /* Bit mask for supported PIPE_DRAW for this hardware */
        unsigned draw_modes;

        struct pipe_framebuffer_state pipe_framebuffer;

        struct panfrost_memory cmdstream_persistent;
        struct panfrost_memory shaders;
        struct panfrost_memory scratchpad;
        struct panfrost_memory tiler_heap;
        struct panfrost_memory tiler_dummy;
        struct panfrost_memory depth_stencil_buffer;

        struct panfrost_query *occlusion_query;

        /* Each draw has corresponding vertex and tiler payloads */
        struct midgard_payload_vertex_tiler payloads[PIPE_SHADER_TYPES];

        /* The fragment shader binary itself is pointed here (for the tripipe) but
         * also everything else in the shader core, including blending, the
         * stencil/depth tests, etc. Refer to the presentations. */

        struct mali_shader_meta fragment_shader_core;

        /* Per-draw Dirty flags are setup like any other driver */
        int dirty;

        unsigned vertex_count;
        unsigned instance_count;

        /* If instancing is enabled, vertex count padded for instance; if
         * it is disabled, just equal to plain vertex count */
        unsigned padded_count;

        union mali_attr attributes[PIPE_MAX_ATTRIBS];

        /* TODO: Multiple uniform buffers (index =/= 0), finer updates? */

        struct panfrost_constant_buffer constant_buffer[PIPE_SHADER_TYPES];

        struct panfrost_rasterizer *rasterizer;
        struct panfrost_shader_variants *shader[PIPE_SHADER_TYPES];
        struct panfrost_vertex_state *vertex;

        struct pipe_vertex_buffer vertex_buffers[PIPE_MAX_ATTRIBS];
        uint32_t vb_mask;

        struct pipe_shader_buffer ssbo[PIPE_SHADER_TYPES][PIPE_MAX_SHADER_BUFFERS];
        uint32_t ssbo_mask[PIPE_SHADER_TYPES];

        struct panfrost_sampler_state *samplers[PIPE_SHADER_TYPES][PIPE_MAX_SAMPLERS];
        unsigned sampler_count[PIPE_SHADER_TYPES];

        struct panfrost_sampler_view *sampler_views[PIPE_SHADER_TYPES][PIPE_MAX_SHADER_SAMPLER_VIEWS];
        unsigned sampler_view_count[PIPE_SHADER_TYPES];

        struct primconvert_context *primconvert;
        struct blitter_context *blitter;

        /* Blitting the wallpaper (the old contents of the framebuffer back to
         * itself) uses a dedicated u_blitter instance versus general blit()
         * callbacks from Gallium, as the blit() callback can trigger
         * wallpapering without Gallium realising, which in turns u_blitter
         * errors due to unsupported reucrsion */

        struct blitter_context *blitter_wallpaper;
        struct panfrost_job *wallpaper_batch;

        struct panfrost_blend_state *blend;

        struct pipe_viewport_state pipe_viewport;
        struct pipe_scissor_state scissor;
        struct pipe_blend_color blend_color;
        struct pipe_depth_stencil_alpha_state *depth_stencil;
        struct pipe_stencil_ref stencil_ref;

        /* True for t6XX, false for t8xx. */
        bool is_t6xx;

        uint32_t out_sync;
};

/* Corresponds to the CSO */

struct panfrost_rasterizer {
        struct pipe_rasterizer_state base;

        /* Bitmask of front face, etc */
        unsigned tiler_gl_enables;
};

/* Variants bundle together to form the backing CSO, bundling multiple
 * shaders with varying emulated features baked in (alpha test
 * parameters, etc) */
#define MAX_SHADER_VARIANTS 8

/* A shader state corresponds to the actual, current variant of the shader */
struct panfrost_shader_state {
        /* Compiled, mapped descriptor, ready for the hardware */
        bool compiled;
        struct mali_shader_meta *tripipe;

        /* Non-descript information */
        int uniform_count;
        bool can_discard;
        bool writes_point_size;
        bool reads_point_coord;
        bool reads_face;

        struct mali_attr_meta varyings[PIPE_MAX_ATTRIBS];
        gl_varying_slot varyings_loc[PIPE_MAX_ATTRIBS];

        unsigned sysval_count;
        unsigned sysval[MAX_SYSVAL_COUNT];

        /* Information on this particular shader variant */
        struct pipe_alpha_state alpha_state;

        uint16_t point_sprite_mask;
        unsigned point_sprite_upper_left : 1;

        /* Should we enable helper invocations */
        bool helper_invocations;
};

/* A collection of varyings (the CSO) */
struct panfrost_shader_variants {
        /* A panfrost_shader_variants can represent a shader for
         * either graphics or compute */

        bool is_compute;

        union {
                struct pipe_shader_state base;
                struct pipe_compute_state cbase;
        };

        struct panfrost_shader_state variants[MAX_SHADER_VARIANTS];
        unsigned variant_count;

        /* The current active variant */
        unsigned active_variant;
};

struct panfrost_vertex_state {
        unsigned num_elements;

        struct pipe_vertex_element pipe[PIPE_MAX_ATTRIBS];
        struct mali_attr_meta hw[PIPE_MAX_ATTRIBS];
};

struct panfrost_sampler_state {
        struct pipe_sampler_state base;
        struct mali_sampler_descriptor hw;
};

/* Misnomer: Sampler view corresponds to textures, not samplers */

struct panfrost_sampler_view {
        struct pipe_sampler_view base;
        struct mali_texture_descriptor hw;
        bool manual_stride;
};

static inline struct panfrost_context *
pan_context(struct pipe_context *pcontext)
{
        return (struct panfrost_context *) pcontext;
}

struct pipe_context *
panfrost_create_context(struct pipe_screen *screen, void *priv, unsigned flags);

void
panfrost_emit_for_draw(struct panfrost_context *ctx, bool with_vertex_data);

struct panfrost_transfer
panfrost_vertex_tiler_job(struct panfrost_context *ctx, bool is_tiler);

unsigned
panfrost_get_default_swizzle(unsigned components);

void
panfrost_flush(
        struct pipe_context *pipe,
        struct pipe_fence_handle **fence,
        unsigned flags);

bool
panfrost_is_scanout(struct panfrost_context *ctx);

mali_ptr panfrost_sfbd_fragment(struct panfrost_context *ctx, bool has_draws);
mali_ptr panfrost_mfbd_fragment(struct panfrost_context *ctx, bool has_draws);

struct bifrost_framebuffer
panfrost_emit_mfbd(struct panfrost_context *ctx, unsigned vertex_count);

struct mali_single_framebuffer
panfrost_emit_sfbd(struct panfrost_context *ctx, unsigned vertex_count);

mali_ptr
panfrost_fragment_job(struct panfrost_context *ctx, bool has_draws);

void
panfrost_shader_compile(
                struct panfrost_context *ctx,
                struct mali_shader_meta *meta,
                enum pipe_shader_ir ir_type,
                const void *ir,
                gl_shader_stage stage,
                struct panfrost_shader_state *state);

void
panfrost_pack_work_groups_compute(
        struct mali_vertex_tiler_prefix *out,
        unsigned num_x,
        unsigned num_y,
        unsigned num_z,
        unsigned size_x,
        unsigned size_y,
        unsigned size_z);

void
panfrost_pack_work_groups_fused(
        struct mali_vertex_tiler_prefix *vertex,
        struct mali_vertex_tiler_prefix *tiler,
        unsigned num_x,
        unsigned num_y,
        unsigned num_z,
        unsigned size_x,
        unsigned size_y,
        unsigned size_z);

/* Instancing */

mali_ptr
panfrost_vertex_buffer_address(struct panfrost_context *ctx, unsigned i);

void
panfrost_emit_vertex_data(struct panfrost_job *batch);

struct pan_shift_odd {
        unsigned shift;
        unsigned odd;
};

struct pan_shift_odd
panfrost_padded_vertex_count(
        unsigned vertex_count,
        bool primitive_pot);


unsigned
pan_expand_shift_odd(struct pan_shift_odd o);

/* Compute */

void
panfrost_compute_context_init(struct pipe_context *pctx);

#endif
