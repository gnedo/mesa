/*
 * Copyright (c) 2012-2015 Etnaviv Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Wladimir J. van der Laan <laanwj@gmail.com>
 */

#include "etnaviv_resource.h"

#include "hw/common.xml.h"

#include "etnaviv_context.h"
#include "etnaviv_debug.h"
#include "etnaviv_screen.h"
#include "etnaviv_translate.h"

#include "util/hash_table.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

#include "drm-uapi/drm_fourcc.h"

static enum etna_surface_layout modifier_to_layout(uint64_t modifier)
{
   switch (modifier) {
   case DRM_FORMAT_MOD_VIVANTE_TILED:
      return ETNA_LAYOUT_TILED;
   case DRM_FORMAT_MOD_VIVANTE_SUPER_TILED:
      return ETNA_LAYOUT_SUPER_TILED;
   case DRM_FORMAT_MOD_VIVANTE_SPLIT_TILED:
      return ETNA_LAYOUT_MULTI_TILED;
   case DRM_FORMAT_MOD_VIVANTE_SPLIT_SUPER_TILED:
      return ETNA_LAYOUT_MULTI_SUPERTILED;
   case DRM_FORMAT_MOD_LINEAR:
   default:
      return ETNA_LAYOUT_LINEAR;
   }
}

static uint64_t layout_to_modifier(enum etna_surface_layout layout)
{
   switch (layout) {
   case ETNA_LAYOUT_TILED:
      return DRM_FORMAT_MOD_VIVANTE_TILED;
   case ETNA_LAYOUT_SUPER_TILED:
      return DRM_FORMAT_MOD_VIVANTE_SUPER_TILED;
   case ETNA_LAYOUT_MULTI_TILED:
      return DRM_FORMAT_MOD_VIVANTE_SPLIT_TILED;
   case ETNA_LAYOUT_MULTI_SUPERTILED:
      return DRM_FORMAT_MOD_VIVANTE_SPLIT_SUPER_TILED;
   case ETNA_LAYOUT_LINEAR:
      return DRM_FORMAT_MOD_LINEAR;
   default:
      return DRM_FORMAT_MOD_INVALID;
   }
}

/* A tile is 4x4 pixels, having 'screen->specs.bits_per_tile' of tile status.
 * So, in a buffer of N pixels, there are N / (4 * 4) tiles.
 * We need N * screen->specs.bits_per_tile / (4 * 4) bits of tile status, or
 * N * screen->specs.bits_per_tile / (4 * 4 * 8) bytes.
 */
bool
etna_screen_resource_alloc_ts(struct pipe_screen *pscreen,
                              struct etna_resource *rsc)
{
   struct etna_screen *screen = etna_screen(pscreen);
   size_t rt_ts_size, ts_layer_stride;
   size_t ts_bits_per_tile, bytes_per_tile;
   uint8_t ts_mode = TS_MODE_128B; /* only used by halti5 */
   int8_t ts_compress_fmt;

   assert(!rsc->ts_bo);

   /* pre-v4 compression is largely useless, so disable it when not wanted for MSAA
    * v4 compression can be enabled everywhere without any known drawback,
    * except that in-place resolve must go through a slower path
    */
   ts_compress_fmt = (screen->specs.v4_compression || rsc->base.nr_samples > 1) ?
                      translate_ts_format(rsc->base.format) : -1;

   if (screen->specs.halti >= 5) {
      /* enable 256B ts mode with compression, as it improves performance
       * the size of the resource might also determine if we want to use it or not
       */
      if (ts_compress_fmt >= 0)
         ts_mode = TS_MODE_256B;

      ts_bits_per_tile = 4;
      bytes_per_tile = ts_mode == TS_MODE_256B ? 256 : 128;
   } else {
      ts_bits_per_tile = screen->specs.bits_per_tile;
      bytes_per_tile = 64;
   }

   ts_layer_stride = align(DIV_ROUND_UP(rsc->levels[0].layer_stride,
                                        bytes_per_tile * 8 / ts_bits_per_tile),
                           0x100 * screen->specs.pixel_pipes);
   rt_ts_size = ts_layer_stride * rsc->base.array_size;
   if (rt_ts_size == 0)
      return true;

   DBG_F(ETNA_DBG_RESOURCE_MSGS, "%p: Allocating tile status of size %zu",
         rsc, rt_ts_size);

   struct etna_bo *rt_ts;
   rt_ts = etna_bo_new(screen->dev, rt_ts_size, DRM_ETNA_GEM_CACHE_WC);

   if (unlikely(!rt_ts)) {
      BUG("Problem allocating tile status for resource");
      return false;
   }

   rsc->ts_bo = rt_ts;
   rsc->levels[0].ts_offset = 0;
   rsc->levels[0].ts_layer_stride = ts_layer_stride;
   rsc->levels[0].ts_size = rt_ts_size;
   rsc->levels[0].ts_mode = ts_mode;
   rsc->levels[0].ts_compress_fmt = ts_compress_fmt;

   return true;
}

static bool
etna_screen_can_create_resource(struct pipe_screen *pscreen,
                                const struct pipe_resource *templat)
{
   struct etna_screen *screen = etna_screen(pscreen);
   if (!translate_samples_to_xyscale(templat->nr_samples, NULL, NULL, NULL))
      return false;

   /* templat->bind is not set here, so we must use the minimum sizes */
   uint max_size =
      MIN2(screen->specs.max_rendertarget_size, screen->specs.max_texture_size);

   if (templat->width0 > max_size || templat->height0 > max_size)
      return false;

   return true;
}

static unsigned
setup_miptree(struct etna_resource *rsc, unsigned paddingX, unsigned paddingY,
              unsigned msaa_xscale, unsigned msaa_yscale)
{
   struct pipe_resource *prsc = &rsc->base;
   unsigned level, size = 0;
   unsigned width = prsc->width0;
   unsigned height = prsc->height0;
   unsigned depth = prsc->depth0;

   for (level = 0; level <= prsc->last_level; level++) {
      struct etna_resource_level *mip = &rsc->levels[level];

      mip->width = width;
      mip->height = height;
      mip->padded_width = align(width * msaa_xscale, paddingX);
      mip->padded_height = align(height * msaa_yscale, paddingY);
      mip->stride = util_format_get_stride(prsc->format, mip->padded_width);
      mip->offset = size;
      mip->layer_stride = mip->stride * util_format_get_nblocksy(prsc->format, mip->padded_height);
      mip->size = prsc->array_size * mip->layer_stride;

      /* align levels to 64 bytes to be able to render to them */
      size += align(mip->size, ETNA_PE_ALIGNMENT) * depth;

      width = u_minify(width, 1);
      height = u_minify(height, 1);
      depth = u_minify(depth, 1);
   }

   return size;
}

/* Is rs alignment needed? */
static bool is_rs_align(struct etna_screen *screen,
                        const struct pipe_resource *tmpl)
{
   return screen->specs.use_blt ? false : (
      VIV_FEATURE(screen, chipMinorFeatures1, TEXTURE_HALIGN) ||
      !etna_resource_sampler_only(tmpl));
}

/* Create a new resource object, using the given template info */
struct pipe_resource *
etna_resource_alloc(struct pipe_screen *pscreen, unsigned layout,
                    enum etna_resource_addressing_mode mode, uint64_t modifier,
                    const struct pipe_resource *templat)
{
   struct etna_screen *screen = etna_screen(pscreen);
   struct etna_resource *rsc;
   unsigned size;

   DBG_F(ETNA_DBG_RESOURCE_MSGS,
         "target=%d, format=%s, %ux%ux%u, array_size=%u, "
         "last_level=%u, nr_samples=%u, usage=%u, bind=%x, flags=%x",
         templat->target, util_format_name(templat->format), templat->width0,
         templat->height0, templat->depth0, templat->array_size,
         templat->last_level, templat->nr_samples, templat->usage,
         templat->bind, templat->flags);

   /* Determine scaling for antialiasing, allow override using debug flag */
   int nr_samples = templat->nr_samples;
   if ((templat->bind & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_DEPTH_STENCIL)) &&
       !(templat->bind & PIPE_BIND_SAMPLER_VIEW)) {
      if (DBG_ENABLED(ETNA_DBG_MSAA_2X))
         nr_samples = 2;
      if (DBG_ENABLED(ETNA_DBG_MSAA_4X))
         nr_samples = 4;
   }

   int msaa_xscale = 1, msaa_yscale = 1;
   if (!translate_samples_to_xyscale(nr_samples, &msaa_xscale, &msaa_yscale, NULL)) {
      /* Number of samples not supported */
      return NULL;
   }

   /* Determine needed padding (alignment of height/width) */
   unsigned paddingX = 0, paddingY = 0;
   unsigned halign = TEXTURE_HALIGN_FOUR;
   if (!util_format_is_compressed(templat->format)) {
      /* If we have the TEXTURE_HALIGN feature, we can always align to the
       * resolve engine's width.  If not, we must not align resources used
       * only for textures. If this GPU uses the BLT engine, never do RS align.
       */
      etna_layout_multiple(layout, screen->specs.pixel_pipes,
                           is_rs_align (screen, templat),
                           &paddingX, &paddingY, &halign);
      assert(paddingX && paddingY);
   } else {
      /* Compressed textures are padded to their block size, but we don't have
       * to do anything special for that. */
      paddingX = 1;
      paddingY = 1;
   }

   if (!screen->specs.use_blt && templat->target != PIPE_BUFFER && layout == ETNA_LAYOUT_LINEAR)
      paddingY = align(paddingY, ETNA_RS_HEIGHT_MASK + 1);

   if (templat->bind & PIPE_BIND_SCANOUT && screen->ro->kms_fd >= 0) {
      struct pipe_resource scanout_templat = *templat;
      struct renderonly_scanout *scanout;
      struct winsys_handle handle;

      /* pad scanout buffer size to be compatible with the RS */
      if (!screen->specs.use_blt && modifier == DRM_FORMAT_MOD_LINEAR) {
         paddingX = align(paddingX, ETNA_RS_WIDTH_MASK + 1);
         paddingY = align(paddingY, ETNA_RS_HEIGHT_MASK + 1);
      }

      scanout_templat.width0 = align(scanout_templat.width0, paddingX);
      scanout_templat.height0 = align(scanout_templat.height0, paddingY);

      scanout = renderonly_scanout_for_resource(&scanout_templat,
                                                screen->ro, &handle);
      if (!scanout)
         return NULL;

      assert(handle.type == WINSYS_HANDLE_TYPE_FD);
      handle.modifier = modifier;
      rsc = etna_resource(pscreen->resource_from_handle(pscreen, templat,
                                                        &handle,
                                                        PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE));
      close(handle.handle);
      if (!rsc)
         return NULL;

      rsc->scanout = scanout;

      return &rsc->base;
   }

   rsc = CALLOC_STRUCT(etna_resource);
   if (!rsc)
      return NULL;

   rsc->base = *templat;
   rsc->base.screen = pscreen;
   rsc->base.nr_samples = nr_samples;
   rsc->layout = layout;
   rsc->halign = halign;
   rsc->addressing_mode = mode;

   pipe_reference_init(&rsc->base.reference, 1);

   size = setup_miptree(rsc, paddingX, paddingY, msaa_xscale, msaa_yscale);

   uint32_t flags = DRM_ETNA_GEM_CACHE_WC;
   if (templat->bind & PIPE_BIND_VERTEX_BUFFER)
      flags |= DRM_ETNA_GEM_FORCE_MMU;
   struct etna_bo *bo = etna_bo_new(screen->dev, size, flags);
   if (unlikely(bo == NULL)) {
      BUG("Problem allocating video memory for resource");
      goto free_rsc;
   }

   rsc->bo = bo;
   rsc->ts_bo = 0; /* TS is only created when first bound to surface */

   if (DBG_ENABLED(ETNA_DBG_ZERO)) {
      void *map = etna_bo_map(bo);
      memset(map, 0, size);
   }

   rsc->pending_ctx = _mesa_set_create(NULL, _mesa_hash_pointer,
                                       _mesa_key_pointer_equal);
   if (!rsc->pending_ctx)
      goto free_rsc;

   return &rsc->base;

free_rsc:
   FREE(rsc);
   return NULL;
}

static struct pipe_resource *
etna_resource_create(struct pipe_screen *pscreen,
                     const struct pipe_resource *templat)
{
   struct etna_screen *screen = etna_screen(pscreen);

   /* Figure out what tiling and address mode to use -- for now, assume that
    * texture cannot be linear. there is a capability LINEAR_TEXTURE_SUPPORT
    * (supported on gc880 and gc2000 at least), but not sure how it works.
    * Buffers always have LINEAR layout.
    */
   unsigned layout = ETNA_LAYOUT_LINEAR;
   enum etna_resource_addressing_mode mode = ETNA_ADDRESSING_MODE_TILED;

   if (etna_resource_sampler_only(templat)) {
      /* The buffer is only used for texturing, so create something
       * directly compatible with the sampler.  Such a buffer can
       * never be rendered to. */
      layout = ETNA_LAYOUT_TILED;

      if (util_format_is_compressed(templat->format))
         layout = ETNA_LAYOUT_LINEAR;
   } else if (templat->target != PIPE_BUFFER) {
      bool want_multitiled = false;
      bool want_supertiled = screen->specs.can_supertile;

      /* When this GPU supports single-buffer rendering, don't ever enable
       * multi-tiling. This replicates the blob behavior on GC3000.
       */
      if (!screen->specs.single_buffer)
         want_multitiled = screen->specs.pixel_pipes > 1;

      /* Keep single byte blocksized resources as tiled, since we
       * are unable to use the RS blit to de-tile them. However,
       * if they're used as a render target or depth/stencil, they
       * must be multi-tiled for GPUs with multiple pixel pipes.
       * Ignore depth/stencil here, but it is an error for a render
       * target.
       */
      if (util_format_get_blocksize(templat->format) == 1 &&
          !(templat->bind & PIPE_BIND_DEPTH_STENCIL)) {
         assert(!(templat->bind & PIPE_BIND_RENDER_TARGET && want_multitiled));
         want_multitiled = want_supertiled = false;
      }

      layout = ETNA_LAYOUT_BIT_TILE;
      if (want_multitiled)
         layout |= ETNA_LAYOUT_BIT_MULTI;
      if (want_supertiled)
         layout |= ETNA_LAYOUT_BIT_SUPER;
   }

   if (templat->target == PIPE_TEXTURE_3D)
      layout = ETNA_LAYOUT_LINEAR;

   /* modifier is only used for scanout surfaces, so safe to use LINEAR here */
   return etna_resource_alloc(pscreen, layout, mode, DRM_FORMAT_MOD_LINEAR, templat);
}

enum modifier_priority {
   MODIFIER_PRIORITY_INVALID = 0,
   MODIFIER_PRIORITY_LINEAR,
   MODIFIER_PRIORITY_SPLIT_TILED,
   MODIFIER_PRIORITY_SPLIT_SUPER_TILED,
   MODIFIER_PRIORITY_TILED,
   MODIFIER_PRIORITY_SUPER_TILED,
};

const uint64_t priority_to_modifier[] = {
   [MODIFIER_PRIORITY_INVALID] = DRM_FORMAT_MOD_INVALID,
   [MODIFIER_PRIORITY_LINEAR] = DRM_FORMAT_MOD_LINEAR,
   [MODIFIER_PRIORITY_SPLIT_TILED] = DRM_FORMAT_MOD_VIVANTE_SPLIT_TILED,
   [MODIFIER_PRIORITY_SPLIT_SUPER_TILED] = DRM_FORMAT_MOD_VIVANTE_SPLIT_SUPER_TILED,
   [MODIFIER_PRIORITY_TILED] = DRM_FORMAT_MOD_VIVANTE_TILED,
   [MODIFIER_PRIORITY_SUPER_TILED] = DRM_FORMAT_MOD_VIVANTE_SUPER_TILED,
};

static uint64_t
select_best_modifier(const struct etna_screen * screen,
                     const uint64_t *modifiers, const unsigned count)
{
   enum modifier_priority prio = MODIFIER_PRIORITY_INVALID;

   for (int i = 0; i < count; i++) {
      switch (modifiers[i]) {
      case DRM_FORMAT_MOD_VIVANTE_SUPER_TILED:
         if ((screen->specs.pixel_pipes > 1 && !screen->specs.single_buffer) ||
             !screen->specs.can_supertile)
            break;
         prio = MAX2(prio, MODIFIER_PRIORITY_SUPER_TILED);
         break;
      case DRM_FORMAT_MOD_VIVANTE_TILED:
         if (screen->specs.pixel_pipes > 1 && !screen->specs.single_buffer)
            break;
         prio = MAX2(prio, MODIFIER_PRIORITY_TILED);
         break;
      case DRM_FORMAT_MOD_VIVANTE_SPLIT_SUPER_TILED:
         if ((screen->specs.pixel_pipes < 2) || !screen->specs.can_supertile)
            break;
         prio = MAX2(prio, MODIFIER_PRIORITY_SPLIT_SUPER_TILED);
         break;
      case DRM_FORMAT_MOD_VIVANTE_SPLIT_TILED:
         if (screen->specs.pixel_pipes < 2)
            break;
         prio = MAX2(prio, MODIFIER_PRIORITY_SPLIT_TILED);
         break;
      case DRM_FORMAT_MOD_LINEAR:
         prio = MAX2(prio, MODIFIER_PRIORITY_LINEAR);
         break;
      case DRM_FORMAT_MOD_INVALID:
      default:
         break;
      }
   }

   return priority_to_modifier[prio];
}

static struct pipe_resource *
etna_resource_create_modifiers(struct pipe_screen *pscreen,
                               const struct pipe_resource *templat,
                               const uint64_t *modifiers, int count)
{
   struct etna_screen *screen = etna_screen(pscreen);
   struct pipe_resource tmpl = *templat;
   uint64_t modifier = select_best_modifier(screen, modifiers, count);

   if (modifier == DRM_FORMAT_MOD_INVALID)
      return NULL;

   /*
    * We currently assume that all buffers allocated through this interface
    * should be scanout enabled.
    */
   tmpl.bind |= PIPE_BIND_SCANOUT;

   return etna_resource_alloc(pscreen, modifier_to_layout(modifier),
                              ETNA_ADDRESSING_MODE_TILED, modifier, &tmpl);
}

static void
etna_resource_changed(struct pipe_screen *pscreen, struct pipe_resource *prsc)
{
   struct etna_resource *res = etna_resource(prsc);

   if (res->external)
      etna_resource(res->external)->seqno++;
   else
      res->seqno++;
}

static void
etna_resource_destroy(struct pipe_screen *pscreen, struct pipe_resource *prsc)
{
   struct etna_screen *screen = etna_screen(pscreen);
   struct etna_resource *rsc = etna_resource(prsc);

   mtx_lock(&screen->lock);
   _mesa_set_remove_key(screen->used_resources, rsc);
   _mesa_set_destroy(rsc->pending_ctx, NULL);
   mtx_unlock(&screen->lock);

   if (rsc->bo)
      etna_bo_del(rsc->bo);

   if (rsc->ts_bo)
      etna_bo_del(rsc->ts_bo);

   if (rsc->scanout)
      renderonly_scanout_destroy(rsc->scanout, etna_screen(pscreen)->ro);

   pipe_resource_reference(&rsc->texture, NULL);
   pipe_resource_reference(&rsc->external, NULL);

   for (unsigned i = 0; i < ETNA_NUM_LOD; i++)
      FREE(rsc->levels[i].patch_offsets);

   FREE(rsc);
}

static struct pipe_resource *
etna_resource_from_handle(struct pipe_screen *pscreen,
                          const struct pipe_resource *tmpl,
                          struct winsys_handle *handle, unsigned usage)
{
   struct etna_screen *screen = etna_screen(pscreen);
   struct etna_resource *rsc;
   struct etna_resource_level *level;
   struct pipe_resource *prsc;
   struct pipe_resource *ptiled = NULL;

   DBG("target=%d, format=%s, %ux%ux%u, array_size=%u, last_level=%u, "
       "nr_samples=%u, usage=%u, bind=%x, flags=%x",
       tmpl->target, util_format_name(tmpl->format), tmpl->width0,
       tmpl->height0, tmpl->depth0, tmpl->array_size, tmpl->last_level,
       tmpl->nr_samples, tmpl->usage, tmpl->bind, tmpl->flags);

   rsc = CALLOC_STRUCT(etna_resource);
   if (!rsc)
      return NULL;

   level = &rsc->levels[0];
   prsc = &rsc->base;

   *prsc = *tmpl;

   pipe_reference_init(&prsc->reference, 1);
   prsc->screen = pscreen;

   rsc->bo = etna_screen_bo_from_handle(pscreen, handle, &level->stride);
   if (!rsc->bo)
      goto fail;

   rsc->seqno = 1;
   rsc->layout = modifier_to_layout(handle->modifier);
   rsc->halign = TEXTURE_HALIGN_FOUR;
   rsc->addressing_mode = ETNA_ADDRESSING_MODE_TILED;


   level->width = tmpl->width0;
   level->height = tmpl->height0;

   /* Determine padding of the imported resource. */
   unsigned paddingX = 0, paddingY = 0;
   etna_layout_multiple(rsc->layout, screen->specs.pixel_pipes,
                        is_rs_align(screen, tmpl),
                        &paddingX, &paddingY, &rsc->halign);

   if (!screen->specs.use_blt && rsc->layout == ETNA_LAYOUT_LINEAR)
      paddingY = align(paddingY, ETNA_RS_HEIGHT_MASK + 1);
   level->padded_width = align(level->width, paddingX);
   level->padded_height = align(level->height, paddingY);

   level->layer_stride = level->stride * util_format_get_nblocksy(prsc->format,
                                                                  level->padded_height);
   level->size = level->layer_stride;

   /* The DDX must give us a BO which conforms to our padding size.
    * The stride of the BO must be greater or equal to our padded
    * stride. The size of the BO must accomodate the padded height. */
   if (level->stride < util_format_get_stride(tmpl->format, level->padded_width)) {
      BUG("BO stride %u is too small for RS engine width padding (%zu, format %s)",
          level->stride, util_format_get_stride(tmpl->format, level->padded_width),
          util_format_name(tmpl->format));
      goto fail;
   }
   if (etna_bo_size(rsc->bo) < level->stride * level->padded_height) {
      BUG("BO size %u is too small for RS engine height padding (%u, format %s)",
          etna_bo_size(rsc->bo), level->stride * level->padded_height,
          util_format_name(tmpl->format));
      goto fail;
   }

   rsc->pending_ctx = _mesa_set_create(NULL, _mesa_hash_pointer,
                                       _mesa_key_pointer_equal);
   if (!rsc->pending_ctx)
      goto fail;

   if (rsc->layout == ETNA_LAYOUT_LINEAR) {
      /*
       * Both sampler and pixel pipes can't handle linear, create a compatible
       * base resource, where we can attach the imported buffer as an external
       * resource.
       */
      struct pipe_resource tiled_templat = *tmpl;

      /*
       * Remove BIND_SCANOUT to avoid recursion, as etna_resource_create uses
       * this function to import the scanout buffer and get a tiled resource.
       */
      tiled_templat.bind &= ~PIPE_BIND_SCANOUT;

      ptiled = etna_resource_create(pscreen, &tiled_templat);
      if (!ptiled)
         goto fail;

      etna_resource(ptiled)->external = prsc;

      return ptiled;
   }

   return prsc;

fail:
   etna_resource_destroy(pscreen, prsc);
   if (ptiled)
      etna_resource_destroy(pscreen, ptiled);

   return NULL;
}

static bool
etna_resource_get_handle(struct pipe_screen *pscreen,
                         struct pipe_context *pctx,
                         struct pipe_resource *prsc,
                         struct winsys_handle *handle, unsigned usage)
{
   struct etna_resource *rsc = etna_resource(prsc);
   /* Scanout is always attached to the base resource */
   struct renderonly_scanout *scanout = rsc->scanout;

   /*
    * External resources are preferred, so a import->export chain of
    * render/sampler incompatible buffers yield the same handle.
    */
   if (rsc->external)
      rsc = etna_resource(rsc->external);

   handle->stride = rsc->levels[0].stride;
   handle->offset = rsc->levels[0].offset;
   handle->modifier = layout_to_modifier(rsc->layout);

   if (handle->type == WINSYS_HANDLE_TYPE_SHARED) {
      return etna_bo_get_name(rsc->bo, &handle->handle) == 0;
   } else if (handle->type == WINSYS_HANDLE_TYPE_KMS) {
      if (renderonly_get_handle(scanout, handle)) {
         return true;
      } else {
         handle->handle = etna_bo_handle(rsc->bo);
         return true;
      }
   } else if (handle->type == WINSYS_HANDLE_TYPE_FD) {
      handle->handle = etna_bo_dmabuf(rsc->bo);
      return true;
   } else {
      return false;
   }
}

void
etna_resource_used(struct etna_context *ctx, struct pipe_resource *prsc,
                   enum etna_resource_status status)
{
   struct etna_screen *screen = ctx->screen;
   struct etna_resource *rsc;

   if (!prsc)
      return;

   rsc = etna_resource(prsc);

   mtx_lock(&screen->lock);

   /*
    * if we are pending read or write by any other context or
    * if reading a resource pending a write, then
    * flush all the contexts to maintain coherency
    */
   if (((status & ETNA_PENDING_WRITE) && rsc->status) ||
       ((status & ETNA_PENDING_READ) && (rsc->status & ETNA_PENDING_WRITE))) {
      set_foreach(rsc->pending_ctx, entry) {
         struct etna_context *extctx = (struct etna_context *)entry->key;
         struct pipe_context *pctx = &extctx->base;

         if (extctx == ctx)
            continue;

         pctx->flush(pctx, NULL, 0);
         /* It's safe to clear the status here. If we need to flush it means
          * either another context had the resource in exclusive (write) use,
          * or we transition the resource to exclusive use in our context.
          * In both cases the new status accurately reflects the resource use
          * after the flush.
          */
         rsc->status = 0;
      }
   }

   rsc->status |= status;

   _mesa_set_add(screen->used_resources, rsc);
   _mesa_set_add(rsc->pending_ctx, ctx);

   mtx_unlock(&screen->lock);
}

bool
etna_resource_has_valid_ts(struct etna_resource *rsc)
{
   if (!rsc->ts_bo)
      return false;

   for (int level = 0; level <= rsc->base.last_level; level++)
      if (rsc->levels[level].ts_valid)
         return true;

   return false;
}

void
etna_resource_screen_init(struct pipe_screen *pscreen)
{
   pscreen->can_create_resource = etna_screen_can_create_resource;
   pscreen->resource_create = etna_resource_create;
   pscreen->resource_create_with_modifiers = etna_resource_create_modifiers;
   pscreen->resource_from_handle = etna_resource_from_handle;
   pscreen->resource_get_handle = etna_resource_get_handle;
   pscreen->resource_changed = etna_resource_changed;
   pscreen->resource_destroy = etna_resource_destroy;
}
