/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <time.h>
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/ralloc.h"
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "util/u_upload_mgr.h"
#include "drm-uapi/i915_drm.h"
#include "iris_context.h"
#include "iris_resource.h"
#include "iris_screen.h"
#include "common/gen_defines.h"
#include "common/gen_sample_positions.h"

/**
 * For debugging purposes, this returns a time in seconds.
 */
double
get_time(void)
{
   struct timespec tp;

   clock_gettime(CLOCK_MONOTONIC, &tp);

   return tp.tv_sec + tp.tv_nsec / 1000000000.0;
}

/**
 * The pipe->set_debug_callback() driver hook.
 */
static void
iris_set_debug_callback(struct pipe_context *ctx,
                        const struct pipe_debug_callback *cb)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   if (cb)
      ice->dbg = *cb;
   else
      memset(&ice->dbg, 0, sizeof(ice->dbg));
}

/**
 * Called from the batch module when it detects a GPU hang.
 *
 * In this case, we've lost our GEM context, and can't rely on any existing
 * state on the GPU.  We must mark everything dirty and wipe away any saved
 * assumptions about the last known state of the GPU.
 */
void
iris_lost_context_state(struct iris_batch *batch)
{
   /* The batch module doesn't have an iris_context, because we want to
    * avoid introducing lots of layering violations.  Unfortunately, here
    * we do need to inform the context of batch catastrophe.  We know the
    * batch is one of our context's, so hackily claw our way back.
    */
   struct iris_context *ice = NULL;
   struct iris_screen *screen;

   if (batch->name == IRIS_BATCH_RENDER) {
      ice = container_of(batch, ice, batches[IRIS_BATCH_RENDER]);
      assert(&ice->batches[IRIS_BATCH_RENDER] == batch);
      screen = (void *) ice->ctx.screen;

      ice->vtbl.init_render_context(screen, batch, &ice->vtbl, &ice->dbg);
   } else if (batch->name == IRIS_BATCH_COMPUTE) {
      ice = container_of(batch, ice, batches[IRIS_BATCH_COMPUTE]);
      assert(&ice->batches[IRIS_BATCH_COMPUTE] == batch);
      screen = (void *) ice->ctx.screen;

      ice->vtbl.init_compute_context(screen, batch, &ice->vtbl, &ice->dbg);
   } else {
      unreachable("unhandled batch reset");
   }

   ice->state.dirty = ~0ull;
   memset(ice->state.last_grid, 0, sizeof(ice->state.last_grid));
   batch->last_surface_base_address = ~0ull;
   ice->vtbl.lost_genx_state(ice, batch);
}

static enum pipe_reset_status
iris_get_device_reset_status(struct pipe_context *ctx)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   enum pipe_reset_status worst_reset = PIPE_NO_RESET;

   /* Check the reset status of each batch's hardware context, and take the
    * worst status (if one was guilty, proclaim guilt).
    */
   for (int i = 0; i < IRIS_BATCH_COUNT; i++) {
      /* This will also recreate the hardware contexts as necessary, so any
       * future queries will show no resets.  We only want to report once.
       */
      enum pipe_reset_status batch_reset =
         iris_batch_check_for_reset(&ice->batches[i]);

      if (batch_reset == PIPE_NO_RESET)
         continue;

      if (worst_reset == PIPE_NO_RESET) {
         worst_reset = batch_reset;
      } else {
         /* GUILTY < INNOCENT < UNKNOWN */
         worst_reset = MIN2(worst_reset, batch_reset);
      }
   }

   if (worst_reset != PIPE_NO_RESET && ice->reset.reset)
      ice->reset.reset(ice->reset.data, worst_reset);

   return worst_reset;
}

static void
iris_set_device_reset_callback(struct pipe_context *ctx,
                               const struct pipe_device_reset_callback *cb)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   if (cb)
      ice->reset = *cb;
   else
      memset(&ice->reset, 0, sizeof(ice->reset));
}

static void
iris_get_sample_position(struct pipe_context *ctx,
                         unsigned sample_count,
                         unsigned sample_index,
                         float *out_value)
{
   union {
      struct {
         float x[16];
         float y[16];
      } a;
      struct {
         float  _0XOffset,  _1XOffset,  _2XOffset,  _3XOffset,
                _4XOffset,  _5XOffset,  _6XOffset,  _7XOffset,
                _8XOffset,  _9XOffset, _10XOffset, _11XOffset,
               _12XOffset, _13XOffset, _14XOffset, _15XOffset;
         float  _0YOffset,  _1YOffset,  _2YOffset,  _3YOffset,
                _4YOffset,  _5YOffset,  _6YOffset,  _7YOffset,
                _8YOffset,  _9YOffset, _10YOffset, _11YOffset,
               _12YOffset, _13YOffset, _14YOffset, _15YOffset;
      } v;
   } u;
   switch (sample_count) {
   case 1:  GEN_SAMPLE_POS_1X(u.v._);  break;
   case 2:  GEN_SAMPLE_POS_2X(u.v._);  break;
   case 4:  GEN_SAMPLE_POS_4X(u.v._);  break;
   case 8:  GEN_SAMPLE_POS_8X(u.v._);  break;
   case 16: GEN_SAMPLE_POS_16X(u.v._); break;
   default: unreachable("invalid sample count");
   }

   out_value[0] = u.a.x[sample_index];
   out_value[1] = u.a.y[sample_index];
}

/**
 * Destroy a context, freeing any associated memory.
 */
static void
iris_destroy_context(struct pipe_context *ctx)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   if (ctx->stream_uploader)
      u_upload_destroy(ctx->stream_uploader);

   ice->vtbl.destroy_state(ice);
   iris_destroy_program_cache(ice);
   iris_destroy_border_color_pool(ice);
   u_upload_destroy(ice->state.surface_uploader);
   u_upload_destroy(ice->state.dynamic_uploader);
   u_upload_destroy(ice->query_buffer_uploader);

   slab_destroy_child(&ice->transfer_pool);

   iris_batch_free(&ice->batches[IRIS_BATCH_RENDER]);
   iris_batch_free(&ice->batches[IRIS_BATCH_COMPUTE]);
   iris_destroy_binder(&ice->state.binder);

   ralloc_free(ice);
}

#define genX_call(devinfo, func, ...)             \
   switch (devinfo->gen) {                        \
   case 11:                                       \
      gen11_##func(__VA_ARGS__);                  \
      break;                                      \
   case 10:                                       \
      gen10_##func(__VA_ARGS__);                  \
      break;                                      \
   case 9:                                        \
      gen9_##func(__VA_ARGS__);                   \
      break;                                      \
   case 8:                                        \
      gen8_##func(__VA_ARGS__);                   \
      break;                                      \
   default:                                       \
      unreachable("Unknown hardware generation"); \
   }

/**
 * Create a context.
 *
 * This is where each context begins.
 */
struct pipe_context *
iris_create_context(struct pipe_screen *pscreen, void *priv, unsigned flags)
{
   struct iris_screen *screen = (struct iris_screen*)pscreen;
   const struct gen_device_info *devinfo = &screen->devinfo;
   struct iris_context *ice = rzalloc(NULL, struct iris_context);

   if (!ice)
      return NULL;

   struct pipe_context *ctx = &ice->ctx;

   ctx->screen = pscreen;
   ctx->priv = priv;

   ctx->stream_uploader = u_upload_create_default(ctx);
   if (!ctx->stream_uploader) {
      free(ctx);
      return NULL;
   }
   ctx->const_uploader = ctx->stream_uploader;

   ctx->destroy = iris_destroy_context;
   ctx->set_debug_callback = iris_set_debug_callback;
   ctx->set_device_reset_callback = iris_set_device_reset_callback;
   ctx->get_device_reset_status = iris_get_device_reset_status;
   ctx->get_sample_position = iris_get_sample_position;

   ice->shaders.urb_size = devinfo->urb.size;

   iris_init_context_fence_functions(ctx);
   iris_init_blit_functions(ctx);
   iris_init_clear_functions(ctx);
   iris_init_program_functions(ctx);
   iris_init_resource_functions(ctx);
   iris_init_flush_functions(ctx);

   iris_init_program_cache(ice);
   iris_init_border_color_pool(ice);
   iris_init_binder(ice);

   slab_create_child(&ice->transfer_pool, &screen->transfer_pool);

   ice->state.surface_uploader =
      u_upload_create(ctx, 16384, PIPE_BIND_CUSTOM, PIPE_USAGE_IMMUTABLE,
                      IRIS_RESOURCE_FLAG_SURFACE_MEMZONE);
   ice->state.dynamic_uploader =
      u_upload_create(ctx, 16384, PIPE_BIND_CUSTOM, PIPE_USAGE_IMMUTABLE,
                      IRIS_RESOURCE_FLAG_DYNAMIC_MEMZONE);

   ice->query_buffer_uploader =
      u_upload_create(ctx, 4096, PIPE_BIND_CUSTOM, PIPE_USAGE_STAGING,
                      0);

   genX_call(devinfo, init_state, ice);
   genX_call(devinfo, init_blorp, ice);
   genX_call(devinfo, init_query, ice);

   int priority = 0;
   if (flags & PIPE_CONTEXT_HIGH_PRIORITY)
      priority = GEN_CONTEXT_HIGH_PRIORITY;
   if (flags & PIPE_CONTEXT_LOW_PRIORITY)
      priority = GEN_CONTEXT_LOW_PRIORITY;

   if (unlikely(INTEL_DEBUG & DEBUG_BATCH))
      ice->state.sizes = _mesa_hash_table_u64_create(ice);

   for (int i = 0; i < IRIS_BATCH_COUNT; i++) {
      iris_init_batch(&ice->batches[i], screen, &ice->vtbl, &ice->dbg,
                      &ice->reset, ice->state.sizes,
                      ice->batches, (enum iris_batch_name) i,
                      I915_EXEC_RENDER, priority);
   }

   ice->vtbl.init_render_context(screen, &ice->batches[IRIS_BATCH_RENDER],
                                 &ice->vtbl, &ice->dbg);
   ice->vtbl.init_compute_context(screen, &ice->batches[IRIS_BATCH_COMPUTE],
                                  &ice->vtbl, &ice->dbg);

   return ctx;
}
