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

/**
 * @file iris_program.c
 *
 * This file contains the driver interface for compiling shaders.
 *
 * See iris_program_cache.c for the in-memory program cache where the
 * compiled shaders are stored.
 */

#include <stdio.h>
#include <errno.h>
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_atomic.h"
#include "util/u_upload_mgr.h"
#include "util/debug.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_serialize.h"
#include "intel/compiler/brw_compiler.h"
#include "intel/compiler/brw_nir.h"
#include "iris_context.h"
#include "nir/tgsi_to_nir.h"

#define KEY_INIT_NO_ID(gen)                              \
   .base.subgroup_size_type = BRW_SUBGROUP_SIZE_UNIFORM, \
   .base.tex.swizzles[0 ... MAX_SAMPLERS - 1] = 0x688,   \
   .base.tex.compressed_multisample_layout_mask = ~0,    \
   .base.tex.msaa_16 = (gen >= 9 ? ~0 : 0)
#define KEY_INIT(gen) .base.program_string_id = ish->program_id, KEY_INIT_NO_ID(gen)

static unsigned
get_new_program_id(struct iris_screen *screen)
{
   return p_atomic_inc_return(&screen->program_id);
}

static void *
upload_state(struct u_upload_mgr *uploader,
             struct iris_state_ref *ref,
             unsigned size,
             unsigned alignment)
{
   void *p = NULL;
   u_upload_alloc(uploader, 0, size, alignment, &ref->offset, &ref->res, &p);
   return p;
}

void
iris_upload_ubo_ssbo_surf_state(struct iris_context *ice,
                                struct pipe_shader_buffer *buf,
                                struct iris_state_ref *surf_state,
                                bool ssbo)
{
   struct pipe_context *ctx = &ice->ctx;
   struct iris_screen *screen = (struct iris_screen *) ctx->screen;

   void *map =
      upload_state(ice->state.surface_uploader, surf_state,
                   screen->isl_dev.ss.size, 64);
   if (!unlikely(map)) {
      surf_state->res = NULL;
      return;
   }

   struct iris_resource *res = (void *) buf->buffer;
   struct iris_bo *surf_bo = iris_resource_bo(surf_state->res);
   surf_state->offset += iris_bo_offset_from_base_address(surf_bo);

   isl_buffer_fill_state(&screen->isl_dev, map,
                         .address = res->bo->gtt_offset + res->offset +
                                    buf->buffer_offset,
                         .size_B = buf->buffer_size - res->offset,
                         .format = ssbo ? ISL_FORMAT_RAW
                                        : ISL_FORMAT_R32G32B32A32_FLOAT,
                         .swizzle = ISL_SWIZZLE_IDENTITY,
                         .stride_B = 1,
                         .mocs = ice->vtbl.mocs(res->bo));
}

static nir_ssa_def *
get_aoa_deref_offset(nir_builder *b,
                     nir_deref_instr *deref,
                     unsigned elem_size)
{
   unsigned array_size = elem_size;
   nir_ssa_def *offset = nir_imm_int(b, 0);

   while (deref->deref_type != nir_deref_type_var) {
      assert(deref->deref_type == nir_deref_type_array);

      /* This level's element size is the previous level's array size */
      nir_ssa_def *index = nir_ssa_for_src(b, deref->arr.index, 1);
      assert(deref->arr.index.ssa);
      offset = nir_iadd(b, offset,
                           nir_imul(b, index, nir_imm_int(b, array_size)));

      deref = nir_deref_instr_parent(deref);
      assert(glsl_type_is_array(deref->type));
      array_size *= glsl_get_length(deref->type);
   }

   /* Accessing an invalid surface index with the dataport can result in a
    * hang.  According to the spec "if the index used to select an individual
    * element is negative or greater than or equal to the size of the array,
    * the results of the operation are undefined but may not lead to
    * termination" -- which is one of the possible outcomes of the hang.
    * Clamp the index to prevent access outside of the array bounds.
    */
   return nir_umin(b, offset, nir_imm_int(b, array_size - elem_size));
}

static void
iris_lower_storage_image_derefs(nir_shader *nir)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   nir_builder b;
   nir_builder_init(&b, impl);

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         switch (intrin->intrinsic) {
         case nir_intrinsic_image_deref_load:
         case nir_intrinsic_image_deref_store:
         case nir_intrinsic_image_deref_atomic_add:
         case nir_intrinsic_image_deref_atomic_min:
         case nir_intrinsic_image_deref_atomic_max:
         case nir_intrinsic_image_deref_atomic_and:
         case nir_intrinsic_image_deref_atomic_or:
         case nir_intrinsic_image_deref_atomic_xor:
         case nir_intrinsic_image_deref_atomic_exchange:
         case nir_intrinsic_image_deref_atomic_comp_swap:
         case nir_intrinsic_image_deref_size:
         case nir_intrinsic_image_deref_samples:
         case nir_intrinsic_image_deref_load_raw_intel:
         case nir_intrinsic_image_deref_store_raw_intel: {
            nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
            nir_variable *var = nir_deref_instr_get_variable(deref);

            b.cursor = nir_before_instr(&intrin->instr);
            nir_ssa_def *index =
               nir_iadd(&b, nir_imm_int(&b, var->data.driver_location),
                            get_aoa_deref_offset(&b, deref, 1));
            nir_rewrite_image_intrinsic(intrin, index, false);
            break;
         }

         default:
            break;
         }
      }
   }
}

// XXX: need unify_interfaces() at link time...

/**
 * Fix an uncompiled shader's stream output info.
 *
 * Core Gallium stores output->register_index as a "slot" number, where
 * slots are assigned consecutively to all outputs in info->outputs_written.
 * This naive packing of outputs doesn't work for us - we too have slots,
 * but the layout is defined by the VUE map, which we won't have until we
 * compile a specific shader variant.  So, we remap these and simply store
 * VARYING_SLOT_* in our copy's output->register_index fields.
 *
 * We also fix up VARYING_SLOT_{LAYER,VIEWPORT,PSIZ} to select the Y/Z/W
 * components of our VUE header.  See brw_vue_map.c for the layout.
 */
static void
update_so_info(struct pipe_stream_output_info *so_info,
               uint64_t outputs_written)
{
   uint8_t reverse_map[64] = {};
   unsigned slot = 0;
   while (outputs_written) {
      reverse_map[slot++] = u_bit_scan64(&outputs_written);
   }

   for (unsigned i = 0; i < so_info->num_outputs; i++) {
      struct pipe_stream_output *output = &so_info->output[i];

      /* Map Gallium's condensed "slots" back to real VARYING_SLOT_* enums */
      output->register_index = reverse_map[output->register_index];

      /* The VUE header contains three scalar fields packed together:
       * - gl_PointSize is stored in VARYING_SLOT_PSIZ.w
       * - gl_Layer is stored in VARYING_SLOT_PSIZ.y
       * - gl_ViewportIndex is stored in VARYING_SLOT_PSIZ.z
       */
      switch (output->register_index) {
      case VARYING_SLOT_LAYER:
         assert(output->num_components == 1);
         output->register_index = VARYING_SLOT_PSIZ;
         output->start_component = 1;
         break;
      case VARYING_SLOT_VIEWPORT:
         assert(output->num_components == 1);
         output->register_index = VARYING_SLOT_PSIZ;
         output->start_component = 2;
         break;
      case VARYING_SLOT_PSIZ:
         assert(output->num_components == 1);
         output->start_component = 3;
         break;
      }

      //info->outputs_written |= 1ull << output->register_index;
   }
}

static void
setup_vec4_image_sysval(uint32_t *sysvals, uint32_t idx,
                        unsigned offset, unsigned n)
{
   assert(offset % sizeof(uint32_t) == 0);

   for (unsigned i = 0; i < n; ++i)
      sysvals[i] = BRW_PARAM_IMAGE(idx, offset / sizeof(uint32_t) + i);

   for (unsigned i = n; i < 4; ++i)
      sysvals[i] = BRW_PARAM_BUILTIN_ZERO;
}

/**
 * Associate NIR uniform variables with the prog_data->param[] mechanism
 * used by the backend.  Also, decide which UBOs we'd like to push in an
 * ideal situation (though the backend can reduce this).
 */
static void
iris_setup_uniforms(const struct brw_compiler *compiler,
                    void *mem_ctx,
                    nir_shader *nir,
                    struct brw_stage_prog_data *prog_data,
                    enum brw_param_builtin **out_system_values,
                    unsigned *out_num_system_values,
                    unsigned *out_num_cbufs)
{
   UNUSED const struct gen_device_info *devinfo = compiler->devinfo;

   /* The intel compiler assumes that num_uniforms is in bytes. For
    * scalar that means 4 bytes per uniform slot.
    *
    * Ref: brw_nir_lower_uniforms, type_size_scalar_bytes.
    */
   nir->num_uniforms *= 4;

   const unsigned IRIS_MAX_SYSTEM_VALUES =
      PIPE_MAX_SHADER_IMAGES * BRW_IMAGE_PARAM_SIZE;
   enum brw_param_builtin *system_values =
      rzalloc_array(mem_ctx, enum brw_param_builtin, IRIS_MAX_SYSTEM_VALUES);
   unsigned num_system_values = 0;

   unsigned patch_vert_idx = -1;
   unsigned ucp_idx[IRIS_MAX_CLIP_PLANES];
   unsigned img_idx[PIPE_MAX_SHADER_IMAGES];
   memset(ucp_idx, -1, sizeof(ucp_idx));
   memset(img_idx, -1, sizeof(img_idx));

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   nir_builder b;
   nir_builder_init(&b, impl);

   b.cursor = nir_before_block(nir_start_block(impl));
   nir_ssa_def *temp_ubo_name = nir_ssa_undef(&b, 1, 32);
   nir_ssa_def *temp_const_ubo_name = NULL;

   /* Turn system value intrinsics into uniforms */
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         nir_ssa_def *offset;

         switch (intrin->intrinsic) {
         case nir_intrinsic_load_constant: {
            /* This one is special because it reads from the shader constant
             * data and not cbuf0 which gallium uploads for us.
             */
            b.cursor = nir_before_instr(instr);
            nir_ssa_def *offset =
               nir_iadd_imm(&b, nir_ssa_for_src(&b, intrin->src[0], 1),
                                nir_intrinsic_base(intrin));

            if (temp_const_ubo_name == NULL)
               temp_const_ubo_name = nir_imm_int(&b, 0);

            nir_intrinsic_instr *load_ubo =
               nir_intrinsic_instr_create(b.shader, nir_intrinsic_load_ubo);
            load_ubo->num_components = intrin->num_components;
            load_ubo->src[0] = nir_src_for_ssa(temp_const_ubo_name);
            load_ubo->src[1] = nir_src_for_ssa(offset);
            nir_ssa_dest_init(&load_ubo->instr, &load_ubo->dest,
                              intrin->dest.ssa.num_components,
                              intrin->dest.ssa.bit_size,
                              intrin->dest.ssa.name);
            nir_builder_instr_insert(&b, &load_ubo->instr);

            nir_ssa_def_rewrite_uses(&intrin->dest.ssa,
                                     nir_src_for_ssa(&load_ubo->dest.ssa));
            nir_instr_remove(&intrin->instr);
            continue;
         }
         case nir_intrinsic_load_user_clip_plane: {
            unsigned ucp = nir_intrinsic_ucp_id(intrin);

            if (ucp_idx[ucp] == -1) {
               ucp_idx[ucp] = num_system_values;
               num_system_values += 4;
            }

            for (int i = 0; i < 4; i++) {
               system_values[ucp_idx[ucp] + i] =
                  BRW_PARAM_BUILTIN_CLIP_PLANE(ucp, i);
            }

            b.cursor = nir_before_instr(instr);
            offset = nir_imm_int(&b, ucp_idx[ucp] * sizeof(uint32_t));
            break;
         }
         case nir_intrinsic_load_patch_vertices_in:
            if (patch_vert_idx == -1)
               patch_vert_idx = num_system_values++;

            system_values[patch_vert_idx] =
               BRW_PARAM_BUILTIN_PATCH_VERTICES_IN;

            b.cursor = nir_before_instr(instr);
            offset = nir_imm_int(&b, patch_vert_idx * sizeof(uint32_t));
            break;
         case nir_intrinsic_image_deref_load_param_intel: {
            assert(devinfo->gen < 9);
            nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
            nir_variable *var = nir_deref_instr_get_variable(deref);

            if (img_idx[var->data.binding] == -1) {
               /* GL only allows arrays of arrays of images. */
               assert(glsl_type_is_image(glsl_without_array(var->type)));
               unsigned num_images = MAX2(1, glsl_get_aoa_size(var->type));

               for (int i = 0; i < num_images; i++) {
                  const unsigned img = var->data.binding + i;

                  img_idx[img] = num_system_values;
                  num_system_values += BRW_IMAGE_PARAM_SIZE;

                  uint32_t *img_sv = &system_values[img_idx[img]];

                  setup_vec4_image_sysval(
                     img_sv + BRW_IMAGE_PARAM_OFFSET_OFFSET, img,
                     offsetof(struct brw_image_param, offset), 2);
                  setup_vec4_image_sysval(
                     img_sv + BRW_IMAGE_PARAM_SIZE_OFFSET, img,
                     offsetof(struct brw_image_param, size), 3);
                  setup_vec4_image_sysval(
                     img_sv + BRW_IMAGE_PARAM_STRIDE_OFFSET, img,
                     offsetof(struct brw_image_param, stride), 4);
                  setup_vec4_image_sysval(
                     img_sv + BRW_IMAGE_PARAM_TILING_OFFSET, img,
                     offsetof(struct brw_image_param, tiling), 3);
                  setup_vec4_image_sysval(
                     img_sv + BRW_IMAGE_PARAM_SWIZZLING_OFFSET, img,
                     offsetof(struct brw_image_param, swizzling), 2);
               }
            }

            b.cursor = nir_before_instr(instr);
            offset = nir_iadd(&b,
               get_aoa_deref_offset(&b, deref, BRW_IMAGE_PARAM_SIZE * 4),
               nir_imm_int(&b, img_idx[var->data.binding] * 4 +
                               nir_intrinsic_base(intrin) * 16));
            break;
         }
         default:
            continue;
         }

         unsigned comps = nir_intrinsic_dest_components(intrin);

         nir_intrinsic_instr *load =
            nir_intrinsic_instr_create(nir, nir_intrinsic_load_ubo);
         load->num_components = comps;
         load->src[0] = nir_src_for_ssa(temp_ubo_name);
         load->src[1] = nir_src_for_ssa(offset);
         nir_ssa_dest_init(&load->instr, &load->dest, comps, 32, NULL);
         nir_builder_instr_insert(&b, &load->instr);
         nir_ssa_def_rewrite_uses(&intrin->dest.ssa,
                                  nir_src_for_ssa(&load->dest.ssa));
         nir_instr_remove(instr);
      }
   }

   nir_validate_shader(nir, "before remapping");

   /* Uniforms are stored in constant buffer 0, the
    * user-facing UBOs are indexed by one.  So if any constant buffer is
    * needed, the constant buffer 0 will be needed, so account for it.
    */
   unsigned num_cbufs = nir->info.num_ubos;
   if (num_cbufs || nir->num_uniforms)
      num_cbufs++;

   /* Place the new params in a new cbuf. */
   if (num_system_values > 0) {
      unsigned sysval_cbuf_index = num_cbufs;
      num_cbufs++;

      system_values = reralloc(mem_ctx, system_values, enum brw_param_builtin,
                               num_system_values);

      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            nir_intrinsic_instr *load = nir_instr_as_intrinsic(instr);

            if (load->intrinsic != nir_intrinsic_load_ubo)
               continue;

            b.cursor = nir_before_instr(instr);

            assert(load->src[0].is_ssa);

            if (load->src[0].ssa == temp_ubo_name) {
               nir_ssa_def *imm = nir_imm_int(&b, sysval_cbuf_index);
               nir_instr_rewrite_src(instr, &load->src[0],
                                     nir_src_for_ssa(imm));
            }
         }
      }

      /* We need to fold the new iadds for brw_nir_analyze_ubo_ranges */
      nir_opt_constant_folding(nir);
   } else {
      ralloc_free(system_values);
      system_values = NULL;
   }

   assert(num_cbufs < PIPE_MAX_CONSTANT_BUFFERS);
   nir_validate_shader(nir, "after remap");

   /* We don't use params[], but fs_visitor::nir_setup_uniforms() asserts
    * about it for compute shaders, so go ahead and make some fake ones
    * which the backend will dead code eliminate.
    */
   prog_data->nr_params = nir->num_uniforms / 4;
   prog_data->param = rzalloc_array(mem_ctx, uint32_t, prog_data->nr_params);

   /* Constant loads (if any) need to go at the end of the constant buffers so
    * we need to know num_cbufs before we can lower to them.
    */
   if (temp_const_ubo_name != NULL) {
      nir_load_const_instr *const_ubo_index =
         nir_instr_as_load_const(temp_const_ubo_name->parent_instr);
      assert(const_ubo_index->def.bit_size == 32);
      const_ubo_index->value[0].u32 = num_cbufs;
   }

   *out_system_values = system_values;
   *out_num_system_values = num_system_values;
   *out_num_cbufs = num_cbufs;
}

static const char *surface_group_names[] = {
   [IRIS_SURFACE_GROUP_RENDER_TARGET]      = "render target",
   [IRIS_SURFACE_GROUP_CS_WORK_GROUPS]     = "CS work groups",
   [IRIS_SURFACE_GROUP_TEXTURE]            = "texture",
   [IRIS_SURFACE_GROUP_UBO]                = "ubo",
   [IRIS_SURFACE_GROUP_SSBO]               = "ssbo",
   [IRIS_SURFACE_GROUP_IMAGE]              = "image",
};

static void
iris_print_binding_table(FILE *fp, const char *name,
                         const struct iris_binding_table *bt)
{
   STATIC_ASSERT(ARRAY_SIZE(surface_group_names) == IRIS_SURFACE_GROUP_COUNT);

   uint32_t total = 0;
   uint32_t compacted = 0;

   for (int i = 0; i < IRIS_SURFACE_GROUP_COUNT; i++) {
      uint32_t size = bt->sizes[i];
      total += size;
      if (size)
         compacted += util_bitcount64(bt->used_mask[i]);
   }

   if (total == 0) {
      fprintf(fp, "Binding table for %s is empty\n\n", name);
      return;
   }

   if (total != compacted) {
      fprintf(fp, "Binding table for %s "
              "(compacted to %u entries from %u entries)\n",
              name, compacted, total);
   } else {
      fprintf(fp, "Binding table for %s (%u entries)\n", name, total);
   }

   uint32_t entry = 0;
   for (int i = 0; i < IRIS_SURFACE_GROUP_COUNT; i++) {
      uint64_t mask = bt->used_mask[i];
      while (mask) {
         int index = u_bit_scan64(&mask);
         fprintf(fp, "  [%u] %s #%d\n", entry++, surface_group_names[i], index);
      }
   }
   fprintf(fp, "\n");
}

enum {
   /* Max elements in a surface group. */
   SURFACE_GROUP_MAX_ELEMENTS = 64,
};

/**
 * Map a <group, index> pair to a binding table index.
 *
 * For example: <UBO, 5> => binding table index 12
 */
uint32_t
iris_group_index_to_bti(const struct iris_binding_table *bt,
                        enum iris_surface_group group, uint32_t index)
{
   assert(index < bt->sizes[group]);
   uint64_t mask = bt->used_mask[group];
   uint64_t bit = 1ull << index;
   if (bit & mask) {
      return bt->offsets[group] + util_bitcount64((bit - 1) & mask);
   } else {
      return IRIS_SURFACE_NOT_USED;
   }
}

/**
 * Map a binding table index back to a <group, index> pair.
 *
 * For example: binding table index 12 => <UBO, 5>
 */
uint32_t
iris_bti_to_group_index(const struct iris_binding_table *bt,
                        enum iris_surface_group group, uint32_t bti)
{
   uint64_t used_mask = bt->used_mask[group];
   assert(bti >= bt->offsets[group]);

   uint32_t c = bti - bt->offsets[group];
   while (used_mask) {
      int i = u_bit_scan64(&used_mask);
      if (c == 0)
         return i;
      c--;
   }

   return IRIS_SURFACE_NOT_USED;
}

static void
rewrite_src_with_bti(nir_builder *b, struct iris_binding_table *bt,
                     nir_instr *instr, nir_src *src,
                     enum iris_surface_group group)
{
   assert(bt->sizes[group] > 0);

   b->cursor = nir_before_instr(instr);
   nir_ssa_def *bti;
   if (nir_src_is_const(*src)) {
      uint32_t index = nir_src_as_uint(*src);
      bti = nir_imm_intN_t(b, iris_group_index_to_bti(bt, group, index),
                           src->ssa->bit_size);
   } else {
      /* Indirect usage makes all the surfaces of the group to be available,
       * so we can just add the base.
       */
      assert(bt->used_mask[group] == BITFIELD64_MASK(bt->sizes[group]));
      bti = nir_iadd_imm(b, src->ssa, bt->offsets[group]);
   }
   nir_instr_rewrite_src(instr, src, nir_src_for_ssa(bti));
}

static void
mark_used_with_src(struct iris_binding_table *bt, nir_src *src,
                   enum iris_surface_group group)
{
   assert(bt->sizes[group] > 0);

   if (nir_src_is_const(*src)) {
      uint64_t index = nir_src_as_uint(*src);
      assert(index < bt->sizes[group]);
      bt->used_mask[group] |= 1ull << index;
   } else {
      /* There's an indirect usage, we need all the surfaces. */
      bt->used_mask[group] = BITFIELD64_MASK(bt->sizes[group]);
   }
}

static bool
skip_compacting_binding_tables(void)
{
   static int skip = -1;
   if (skip < 0)
      skip = env_var_as_boolean("INTEL_DISABLE_COMPACT_BINDING_TABLE", false);
   return skip;
}

/**
 * Set up the binding table indices and apply to the shader.
 */
static void
iris_setup_binding_table(struct nir_shader *nir,
                         struct iris_binding_table *bt,
                         unsigned num_render_targets,
                         unsigned num_system_values,
                         unsigned num_cbufs)
{
   const struct shader_info *info = &nir->info;

   memset(bt, 0, sizeof(*bt));

   /* Set the sizes for each surface group.  For some groups, we already know
    * upfront how many will be used, so mark them.
    */
   if (info->stage == MESA_SHADER_FRAGMENT) {
      bt->sizes[IRIS_SURFACE_GROUP_RENDER_TARGET] = num_render_targets;
      /* All render targets used. */
      bt->used_mask[IRIS_SURFACE_GROUP_RENDER_TARGET] =
         BITFIELD64_MASK(num_render_targets);
   } else if (info->stage == MESA_SHADER_COMPUTE) {
      bt->sizes[IRIS_SURFACE_GROUP_CS_WORK_GROUPS] = 1;
   }

   bt->sizes[IRIS_SURFACE_GROUP_TEXTURE] = util_last_bit(info->textures_used);
   bt->used_mask[IRIS_SURFACE_GROUP_TEXTURE] = info->textures_used;

   bt->sizes[IRIS_SURFACE_GROUP_IMAGE] = info->num_images;

   /* Allocate an extra slot in the UBO section for NIR constants.
    * Binding table compaction will remove it if unnecessary.
    *
    * We don't include them in iris_compiled_shader::num_cbufs because
    * they are uploaded separately from shs->constbuf[], but from a shader
    * point of view, they're another UBO (at the end of the section).
    */
   bt->sizes[IRIS_SURFACE_GROUP_UBO] = num_cbufs + 1;

   /* The first IRIS_MAX_ABOs indices in the SSBO group are for atomics, real
    * SSBOs start after that.  Compaction will remove unused ABOs.
    */
   bt->sizes[IRIS_SURFACE_GROUP_SSBO] = IRIS_MAX_ABOS + info->num_ssbos;

   for (int i = 0; i < IRIS_SURFACE_GROUP_COUNT; i++)
      assert(bt->sizes[i] <= SURFACE_GROUP_MAX_ELEMENTS);

   /* Mark surfaces used for the cases we don't have the information available
    * upfront.
    */
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_foreach_block (block, impl) {
      nir_foreach_instr (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         switch (intrin->intrinsic) {
         case nir_intrinsic_load_num_work_groups:
            bt->used_mask[IRIS_SURFACE_GROUP_CS_WORK_GROUPS] = 1;
            break;

         case nir_intrinsic_image_size:
         case nir_intrinsic_image_load:
         case nir_intrinsic_image_store:
         case nir_intrinsic_image_atomic_add:
         case nir_intrinsic_image_atomic_min:
         case nir_intrinsic_image_atomic_max:
         case nir_intrinsic_image_atomic_and:
         case nir_intrinsic_image_atomic_or:
         case nir_intrinsic_image_atomic_xor:
         case nir_intrinsic_image_atomic_exchange:
         case nir_intrinsic_image_atomic_comp_swap:
         case nir_intrinsic_image_load_raw_intel:
         case nir_intrinsic_image_store_raw_intel:
            mark_used_with_src(bt, &intrin->src[0], IRIS_SURFACE_GROUP_IMAGE);
            break;

         case nir_intrinsic_load_ubo:
            mark_used_with_src(bt, &intrin->src[0], IRIS_SURFACE_GROUP_UBO);
            break;

         case nir_intrinsic_store_ssbo:
            mark_used_with_src(bt, &intrin->src[1], IRIS_SURFACE_GROUP_SSBO);
            break;

         case nir_intrinsic_get_buffer_size:
         case nir_intrinsic_ssbo_atomic_add:
         case nir_intrinsic_ssbo_atomic_imin:
         case nir_intrinsic_ssbo_atomic_umin:
         case nir_intrinsic_ssbo_atomic_imax:
         case nir_intrinsic_ssbo_atomic_umax:
         case nir_intrinsic_ssbo_atomic_and:
         case nir_intrinsic_ssbo_atomic_or:
         case nir_intrinsic_ssbo_atomic_xor:
         case nir_intrinsic_ssbo_atomic_exchange:
         case nir_intrinsic_ssbo_atomic_comp_swap:
         case nir_intrinsic_ssbo_atomic_fmin:
         case nir_intrinsic_ssbo_atomic_fmax:
         case nir_intrinsic_ssbo_atomic_fcomp_swap:
         case nir_intrinsic_load_ssbo:
            mark_used_with_src(bt, &intrin->src[0], IRIS_SURFACE_GROUP_SSBO);
            break;

         default:
            break;
         }
      }
   }

   /* When disable we just mark everything as used. */
   if (unlikely(skip_compacting_binding_tables())) {
      for (int i = 0; i < IRIS_SURFACE_GROUP_COUNT; i++)
         bt->used_mask[i] = BITFIELD64_MASK(bt->sizes[i]);
   }

   /* Calculate the offsets and the binding table size based on the used
    * surfaces.  After this point, the functions to go between "group indices"
    * and binding table indices can be used.
    */
   uint32_t next = 0;
   for (int i = 0; i < IRIS_SURFACE_GROUP_COUNT; i++) {
      if (bt->used_mask[i] != 0) {
         bt->offsets[i] = next;
         next += util_bitcount64(bt->used_mask[i]);
      }
   }
   bt->size_bytes = next * 4;

   if (unlikely(INTEL_DEBUG & DEBUG_BT)) {
      iris_print_binding_table(stderr, gl_shader_stage_name(info->stage), bt);
   }

   /* Apply the binding table indices.  The backend compiler is not expected
    * to change those, as we haven't set any of the *_start entries in brw
    * binding_table.
    */
   nir_builder b;
   nir_builder_init(&b, impl);

   nir_foreach_block (block, impl) {
      nir_foreach_instr (instr, block) {
         if (instr->type == nir_instr_type_tex) {
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            tex->texture_index =
               iris_group_index_to_bti(bt, IRIS_SURFACE_GROUP_TEXTURE,
                                       tex->texture_index);
            continue;
         }

         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         switch (intrin->intrinsic) {
         case nir_intrinsic_image_size:
         case nir_intrinsic_image_load:
         case nir_intrinsic_image_store:
         case nir_intrinsic_image_atomic_add:
         case nir_intrinsic_image_atomic_min:
         case nir_intrinsic_image_atomic_max:
         case nir_intrinsic_image_atomic_and:
         case nir_intrinsic_image_atomic_or:
         case nir_intrinsic_image_atomic_xor:
         case nir_intrinsic_image_atomic_exchange:
         case nir_intrinsic_image_atomic_comp_swap:
         case nir_intrinsic_image_load_raw_intel:
         case nir_intrinsic_image_store_raw_intel:
            rewrite_src_with_bti(&b, bt, instr, &intrin->src[0],
                                 IRIS_SURFACE_GROUP_IMAGE);
            break;

         case nir_intrinsic_load_ubo:
            rewrite_src_with_bti(&b, bt, instr, &intrin->src[0],
                                 IRIS_SURFACE_GROUP_UBO);
            break;

         case nir_intrinsic_store_ssbo:
            rewrite_src_with_bti(&b, bt, instr, &intrin->src[1],
                                 IRIS_SURFACE_GROUP_SSBO);
            break;

         case nir_intrinsic_get_buffer_size:
         case nir_intrinsic_ssbo_atomic_add:
         case nir_intrinsic_ssbo_atomic_imin:
         case nir_intrinsic_ssbo_atomic_umin:
         case nir_intrinsic_ssbo_atomic_imax:
         case nir_intrinsic_ssbo_atomic_umax:
         case nir_intrinsic_ssbo_atomic_and:
         case nir_intrinsic_ssbo_atomic_or:
         case nir_intrinsic_ssbo_atomic_xor:
         case nir_intrinsic_ssbo_atomic_exchange:
         case nir_intrinsic_ssbo_atomic_comp_swap:
         case nir_intrinsic_ssbo_atomic_fmin:
         case nir_intrinsic_ssbo_atomic_fmax:
         case nir_intrinsic_ssbo_atomic_fcomp_swap:
         case nir_intrinsic_load_ssbo:
            rewrite_src_with_bti(&b, bt, instr, &intrin->src[0],
                                 IRIS_SURFACE_GROUP_SSBO);
            break;

         default:
            break;
         }
      }
   }
}

static void
iris_debug_recompile(struct iris_context *ice,
                     struct shader_info *info,
                     const struct brw_base_prog_key *key)
{
   struct iris_screen *screen = (struct iris_screen *) ice->ctx.screen;
   const struct brw_compiler *c = screen->compiler;

   if (!info)
      return;

   c->shader_perf_log(&ice->dbg, "Recompiling %s shader for program %s: %s\n",
                      _mesa_shader_stage_to_string(info->stage),
                      info->name ? info->name : "(no identifier)",
                      info->label ? info->label : "");

   const void *old_key =
      iris_find_previous_compile(ice, info->stage, key->program_string_id);

   brw_debug_key_recompile(c, &ice->dbg, info->stage, old_key, key);
}

/**
 * Get the shader for the last enabled geometry stage.
 *
 * This stage is the one which will feed stream output and the rasterizer.
 */
static gl_shader_stage
last_vue_stage(struct iris_context *ice)
{
   if (ice->shaders.uncompiled[MESA_SHADER_GEOMETRY])
      return MESA_SHADER_GEOMETRY;

   if (ice->shaders.uncompiled[MESA_SHADER_TESS_EVAL])
      return MESA_SHADER_TESS_EVAL;

   return MESA_SHADER_VERTEX;
}

/**
 * Compile a vertex shader, and upload the assembly.
 */
static struct iris_compiled_shader *
iris_compile_vs(struct iris_context *ice,
                struct iris_uncompiled_shader *ish,
                const struct brw_vs_prog_key *key)
{
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   const struct brw_compiler *compiler = screen->compiler;
   const struct gen_device_info *devinfo = &screen->devinfo;
   void *mem_ctx = ralloc_context(NULL);
   struct brw_vs_prog_data *vs_prog_data =
      rzalloc(mem_ctx, struct brw_vs_prog_data);
   struct brw_vue_prog_data *vue_prog_data = &vs_prog_data->base;
   struct brw_stage_prog_data *prog_data = &vue_prog_data->base;
   enum brw_param_builtin *system_values;
   unsigned num_system_values;
   unsigned num_cbufs;

   nir_shader *nir = nir_shader_clone(mem_ctx, ish->nir);

   if (key->nr_userclip_plane_consts) {
      nir_function_impl *impl = nir_shader_get_entrypoint(nir);
      nir_lower_clip_vs(nir, (1 << key->nr_userclip_plane_consts) - 1, true);
      nir_lower_io_to_temporaries(nir, impl, true, false);
      nir_lower_global_vars_to_local(nir);
      nir_lower_vars_to_ssa(nir);
      nir_shader_gather_info(nir, impl);
   }

   prog_data->use_alt_mode = ish->use_alt_mode;

   iris_setup_uniforms(compiler, mem_ctx, nir, prog_data, &system_values,
                       &num_system_values, &num_cbufs);

   struct iris_binding_table bt;
   iris_setup_binding_table(nir, &bt, /* num_render_targets */ 0,
                            num_system_values, num_cbufs);

   brw_nir_analyze_ubo_ranges(compiler, nir, NULL, prog_data->ubo_ranges);

   brw_compute_vue_map(devinfo,
                       &vue_prog_data->vue_map, nir->info.outputs_written,
                       nir->info.separate_shader);

   /* Don't tell the backend about our clip plane constants, we've already
    * lowered them in NIR and we don't want it doing it again.
    */
   struct brw_vs_prog_key key_no_ucp = *key;
   key_no_ucp.nr_userclip_plane_consts = 0;

   char *error_str = NULL;
   const unsigned *program =
      brw_compile_vs(compiler, &ice->dbg, mem_ctx, &key_no_ucp, vs_prog_data,
                     nir, -1, &error_str);
   if (program == NULL) {
      dbg_printf("Failed to compile vertex shader: %s\n", error_str);
      ralloc_free(mem_ctx);
      return false;
   }

   if (ish->compiled_once) {
      iris_debug_recompile(ice, &nir->info, &key->base);
   } else {
      ish->compiled_once = true;
   }

   uint32_t *so_decls =
      ice->vtbl.create_so_decl_list(&ish->stream_output,
                                    &vue_prog_data->vue_map);

   struct iris_compiled_shader *shader =
      iris_upload_shader(ice, IRIS_CACHE_VS, sizeof(*key), key, program,
                         prog_data, so_decls, system_values, num_system_values,
                         num_cbufs, &bt);

   iris_disk_cache_store(screen->disk_cache, ish, shader, key, sizeof(*key));

   ralloc_free(mem_ctx);
   return shader;
}

/**
 * Update the current vertex shader variant.
 *
 * Fill out the key, look in the cache, compile and bind if needed.
 */
static void
iris_update_compiled_vs(struct iris_context *ice)
{
   struct iris_shader_state *shs = &ice->state.shaders[MESA_SHADER_VERTEX];
   struct iris_uncompiled_shader *ish =
      ice->shaders.uncompiled[MESA_SHADER_VERTEX];
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   const struct gen_device_info *devinfo = &screen->devinfo;

   struct brw_vs_prog_key key = { KEY_INIT(devinfo->gen) };
   ice->vtbl.populate_vs_key(ice, &ish->nir->info, last_vue_stage(ice), &key);

   struct iris_compiled_shader *old = ice->shaders.prog[IRIS_CACHE_VS];
   struct iris_compiled_shader *shader =
      iris_find_cached_shader(ice, IRIS_CACHE_VS, sizeof(key), &key);

   if (!shader)
      shader = iris_disk_cache_retrieve(ice, ish, &key, sizeof(key));

   if (!shader)
      shader = iris_compile_vs(ice, ish, &key);

   if (old != shader) {
      ice->shaders.prog[IRIS_CACHE_VS] = shader;
      ice->state.dirty |= IRIS_DIRTY_VS |
                          IRIS_DIRTY_BINDINGS_VS |
                          IRIS_DIRTY_CONSTANTS_VS |
                          IRIS_DIRTY_VF_SGVS;
      shs->sysvals_need_upload = true;

      const struct brw_vs_prog_data *vs_prog_data =
            (void *) shader->prog_data;
      const bool uses_draw_params = vs_prog_data->uses_firstvertex ||
                                    vs_prog_data->uses_baseinstance;
      const bool uses_derived_draw_params = vs_prog_data->uses_drawid ||
                                            vs_prog_data->uses_is_indexed_draw;
      const bool needs_sgvs_element = uses_draw_params ||
                                      vs_prog_data->uses_instanceid ||
                                      vs_prog_data->uses_vertexid;
      bool needs_edge_flag = false;
      nir_foreach_variable(var, &ish->nir->inputs) {
         if (var->data.location == VERT_ATTRIB_EDGEFLAG)
            needs_edge_flag = true;
      }

      if (ice->state.vs_uses_draw_params != uses_draw_params ||
          ice->state.vs_uses_derived_draw_params != uses_derived_draw_params ||
          ice->state.vs_needs_edge_flag != needs_edge_flag) {
         ice->state.dirty |= IRIS_DIRTY_VERTEX_BUFFERS |
                             IRIS_DIRTY_VERTEX_ELEMENTS;
      }
      ice->state.vs_uses_draw_params = uses_draw_params;
      ice->state.vs_uses_derived_draw_params = uses_derived_draw_params;
      ice->state.vs_needs_sgvs_element = needs_sgvs_element;
      ice->state.vs_needs_edge_flag = needs_edge_flag;
   }
}

/**
 * Get the shader_info for a given stage, or NULL if the stage is disabled.
 */
const struct shader_info *
iris_get_shader_info(const struct iris_context *ice, gl_shader_stage stage)
{
   const struct iris_uncompiled_shader *ish = ice->shaders.uncompiled[stage];

   if (!ish)
      return NULL;

   const nir_shader *nir = ish->nir;
   return &nir->info;
}

/**
 * Get the union of TCS output and TES input slots.
 *
 * TCS and TES need to agree on a common URB entry layout.  In particular,
 * the data for all patch vertices is stored in a single URB entry (unlike
 * GS which has one entry per input vertex).  This means that per-vertex
 * array indexing needs a stride.
 *
 * SSO requires locations to match, but doesn't require the number of
 * outputs/inputs to match (in fact, the TCS often has extra outputs).
 * So, we need to take the extra step of unifying these on the fly.
 */
static void
get_unified_tess_slots(const struct iris_context *ice,
                       uint64_t *per_vertex_slots,
                       uint32_t *per_patch_slots)
{
   const struct shader_info *tcs =
      iris_get_shader_info(ice, MESA_SHADER_TESS_CTRL);
   const struct shader_info *tes =
      iris_get_shader_info(ice, MESA_SHADER_TESS_EVAL);

   *per_vertex_slots = tes->inputs_read;
   *per_patch_slots = tes->patch_inputs_read;

   if (tcs) {
      *per_vertex_slots |= tcs->outputs_written;
      *per_patch_slots |= tcs->patch_outputs_written;
   }
}

/**
 * Compile a tessellation control shader, and upload the assembly.
 */
static struct iris_compiled_shader *
iris_compile_tcs(struct iris_context *ice,
                 struct iris_uncompiled_shader *ish,
                 const struct brw_tcs_prog_key *key)
{
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   const struct brw_compiler *compiler = screen->compiler;
   const struct nir_shader_compiler_options *options =
      compiler->glsl_compiler_options[MESA_SHADER_TESS_CTRL].NirOptions;
   void *mem_ctx = ralloc_context(NULL);
   struct brw_tcs_prog_data *tcs_prog_data =
      rzalloc(mem_ctx, struct brw_tcs_prog_data);
   struct brw_vue_prog_data *vue_prog_data = &tcs_prog_data->base;
   struct brw_stage_prog_data *prog_data = &vue_prog_data->base;
   enum brw_param_builtin *system_values = NULL;
   unsigned num_system_values = 0;
   unsigned num_cbufs = 0;

   nir_shader *nir;

   struct iris_binding_table bt;

   if (ish) {
      nir = nir_shader_clone(mem_ctx, ish->nir);

      iris_setup_uniforms(compiler, mem_ctx, nir, prog_data, &system_values,
                          &num_system_values, &num_cbufs);
      iris_setup_binding_table(nir, &bt, /* num_render_targets */ 0,
                               num_system_values, num_cbufs);
      brw_nir_analyze_ubo_ranges(compiler, nir, NULL, prog_data->ubo_ranges);
   } else {
      nir = brw_nir_create_passthrough_tcs(mem_ctx, compiler, options, key);

      /* Reserve space for passing the default tess levels as constants. */
      num_cbufs = 1;
      num_system_values = 8;
      system_values =
         rzalloc_array(mem_ctx, enum brw_param_builtin, num_system_values);
      prog_data->param = rzalloc_array(mem_ctx, uint32_t, num_system_values);
      prog_data->nr_params = num_system_values;

      if (key->tes_primitive_mode == GL_QUADS) {
         for (int i = 0; i < 4; i++)
            system_values[7 - i] = BRW_PARAM_BUILTIN_TESS_LEVEL_OUTER_X + i;

         system_values[3] = BRW_PARAM_BUILTIN_TESS_LEVEL_INNER_X;
         system_values[2] = BRW_PARAM_BUILTIN_TESS_LEVEL_INNER_Y;
      } else if (key->tes_primitive_mode == GL_TRIANGLES) {
         for (int i = 0; i < 3; i++)
            system_values[7 - i] = BRW_PARAM_BUILTIN_TESS_LEVEL_OUTER_X + i;

         system_values[4] = BRW_PARAM_BUILTIN_TESS_LEVEL_INNER_X;
      } else {
         assert(key->tes_primitive_mode == GL_ISOLINES);
         system_values[7] = BRW_PARAM_BUILTIN_TESS_LEVEL_OUTER_Y;
         system_values[6] = BRW_PARAM_BUILTIN_TESS_LEVEL_OUTER_X;
      }

      /* Manually setup the TCS binding table. */
      memset(&bt, 0, sizeof(bt));
      bt.sizes[IRIS_SURFACE_GROUP_UBO] = 1;
      bt.used_mask[IRIS_SURFACE_GROUP_UBO] = 1;
      bt.size_bytes = 4;

      prog_data->ubo_ranges[0].length = 1;
   }

   char *error_str = NULL;
   const unsigned *program =
      brw_compile_tcs(compiler, &ice->dbg, mem_ctx, key, tcs_prog_data, nir,
                      -1, &error_str);
   if (program == NULL) {
      dbg_printf("Failed to compile control shader: %s\n", error_str);
      ralloc_free(mem_ctx);
      return false;
   }

   if (ish) {
      if (ish->compiled_once) {
         iris_debug_recompile(ice, &nir->info, &key->base);
      } else {
         ish->compiled_once = true;
      }
   }

   struct iris_compiled_shader *shader =
      iris_upload_shader(ice, IRIS_CACHE_TCS, sizeof(*key), key, program,
                         prog_data, NULL, system_values, num_system_values,
                         num_cbufs, &bt);

   if (ish)
      iris_disk_cache_store(screen->disk_cache, ish, shader, key, sizeof(*key));

   ralloc_free(mem_ctx);
   return shader;
}

/**
 * Update the current tessellation control shader variant.
 *
 * Fill out the key, look in the cache, compile and bind if needed.
 */
static void
iris_update_compiled_tcs(struct iris_context *ice)
{
   struct iris_shader_state *shs = &ice->state.shaders[MESA_SHADER_TESS_CTRL];
   struct iris_uncompiled_shader *tcs =
      ice->shaders.uncompiled[MESA_SHADER_TESS_CTRL];
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   const struct brw_compiler *compiler = screen->compiler;
   const struct gen_device_info *devinfo = &screen->devinfo;

   const struct shader_info *tes_info =
      iris_get_shader_info(ice, MESA_SHADER_TESS_EVAL);
   struct brw_tcs_prog_key key = {
      KEY_INIT_NO_ID(devinfo->gen),
      .base.program_string_id = tcs ? tcs->program_id : 0,
      .tes_primitive_mode = tes_info->tess.primitive_mode,
      .input_vertices =
         !tcs || compiler->use_tcs_8_patch ? ice->state.vertices_per_patch : 0,
   };
   get_unified_tess_slots(ice, &key.outputs_written,
                          &key.patch_outputs_written);
   ice->vtbl.populate_tcs_key(ice, &key);

   struct iris_compiled_shader *old = ice->shaders.prog[IRIS_CACHE_TCS];
   struct iris_compiled_shader *shader =
      iris_find_cached_shader(ice, IRIS_CACHE_TCS, sizeof(key), &key);

   if (tcs && !shader)
      shader = iris_disk_cache_retrieve(ice, tcs, &key, sizeof(key));

   if (!shader)
      shader = iris_compile_tcs(ice, tcs, &key);

   if (old != shader) {
      ice->shaders.prog[IRIS_CACHE_TCS] = shader;
      ice->state.dirty |= IRIS_DIRTY_TCS |
                          IRIS_DIRTY_BINDINGS_TCS |
                          IRIS_DIRTY_CONSTANTS_TCS;
      shs->sysvals_need_upload = true;
   }
}

/**
 * Compile a tessellation evaluation shader, and upload the assembly.
 */
static struct iris_compiled_shader *
iris_compile_tes(struct iris_context *ice,
                 struct iris_uncompiled_shader *ish,
                 const struct brw_tes_prog_key *key)
{
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   const struct brw_compiler *compiler = screen->compiler;
   void *mem_ctx = ralloc_context(NULL);
   struct brw_tes_prog_data *tes_prog_data =
      rzalloc(mem_ctx, struct brw_tes_prog_data);
   struct brw_vue_prog_data *vue_prog_data = &tes_prog_data->base;
   struct brw_stage_prog_data *prog_data = &vue_prog_data->base;
   enum brw_param_builtin *system_values;
   unsigned num_system_values;
   unsigned num_cbufs;

   nir_shader *nir = nir_shader_clone(mem_ctx, ish->nir);

   if (key->nr_userclip_plane_consts) {
      nir_function_impl *impl = nir_shader_get_entrypoint(nir);
      nir_lower_clip_vs(nir, (1 << key->nr_userclip_plane_consts) - 1, true);
      nir_lower_io_to_temporaries(nir, impl, true, false);
      nir_lower_global_vars_to_local(nir);
      nir_lower_vars_to_ssa(nir);
      nir_shader_gather_info(nir, impl);
   }

   iris_setup_uniforms(compiler, mem_ctx, nir, prog_data, &system_values,
                       &num_system_values, &num_cbufs);

   struct iris_binding_table bt;
   iris_setup_binding_table(nir, &bt, /* num_render_targets */ 0,
                            num_system_values, num_cbufs);

   brw_nir_analyze_ubo_ranges(compiler, nir, NULL, prog_data->ubo_ranges);

   struct brw_vue_map input_vue_map;
   brw_compute_tess_vue_map(&input_vue_map, key->inputs_read,
                            key->patch_inputs_read);

   char *error_str = NULL;
   const unsigned *program =
      brw_compile_tes(compiler, &ice->dbg, mem_ctx, key, &input_vue_map,
                      tes_prog_data, nir, NULL, -1, &error_str);
   if (program == NULL) {
      dbg_printf("Failed to compile evaluation shader: %s\n", error_str);
      ralloc_free(mem_ctx);
      return false;
   }

   if (ish->compiled_once) {
      iris_debug_recompile(ice, &nir->info, &key->base);
   } else {
      ish->compiled_once = true;
   }

   uint32_t *so_decls =
      ice->vtbl.create_so_decl_list(&ish->stream_output,
                                    &vue_prog_data->vue_map);


   struct iris_compiled_shader *shader =
      iris_upload_shader(ice, IRIS_CACHE_TES, sizeof(*key), key, program,
                         prog_data, so_decls, system_values, num_system_values,
                         num_cbufs, &bt);

   iris_disk_cache_store(screen->disk_cache, ish, shader, key, sizeof(*key));

   ralloc_free(mem_ctx);
   return shader;
}

/**
 * Update the current tessellation evaluation shader variant.
 *
 * Fill out the key, look in the cache, compile and bind if needed.
 */
static void
iris_update_compiled_tes(struct iris_context *ice)
{
   struct iris_shader_state *shs = &ice->state.shaders[MESA_SHADER_TESS_EVAL];
   struct iris_uncompiled_shader *ish =
      ice->shaders.uncompiled[MESA_SHADER_TESS_EVAL];
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   const struct gen_device_info *devinfo = &screen->devinfo;

   struct brw_tes_prog_key key = { KEY_INIT(devinfo->gen) };
   get_unified_tess_slots(ice, &key.inputs_read, &key.patch_inputs_read);
   ice->vtbl.populate_tes_key(ice, &ish->nir->info, last_vue_stage(ice), &key);

   struct iris_compiled_shader *old = ice->shaders.prog[IRIS_CACHE_TES];
   struct iris_compiled_shader *shader =
      iris_find_cached_shader(ice, IRIS_CACHE_TES, sizeof(key), &key);

   if (!shader)
      shader = iris_disk_cache_retrieve(ice, ish, &key, sizeof(key));

   if (!shader)
      shader = iris_compile_tes(ice, ish, &key);

   if (old != shader) {
      ice->shaders.prog[IRIS_CACHE_TES] = shader;
      ice->state.dirty |= IRIS_DIRTY_TES |
                          IRIS_DIRTY_BINDINGS_TES |
                          IRIS_DIRTY_CONSTANTS_TES;
      shs->sysvals_need_upload = true;
   }

   /* TODO: Could compare and avoid flagging this. */
   const struct shader_info *tes_info = &ish->nir->info;
   if (tes_info->system_values_read & (1ull << SYSTEM_VALUE_VERTICES_IN)) {
      ice->state.dirty |= IRIS_DIRTY_CONSTANTS_TES;
      ice->state.shaders[MESA_SHADER_TESS_EVAL].sysvals_need_upload = true;
   }
}

/**
 * Compile a geometry shader, and upload the assembly.
 */
static struct iris_compiled_shader *
iris_compile_gs(struct iris_context *ice,
                struct iris_uncompiled_shader *ish,
                const struct brw_gs_prog_key *key)
{
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   const struct brw_compiler *compiler = screen->compiler;
   const struct gen_device_info *devinfo = &screen->devinfo;
   void *mem_ctx = ralloc_context(NULL);
   struct brw_gs_prog_data *gs_prog_data =
      rzalloc(mem_ctx, struct brw_gs_prog_data);
   struct brw_vue_prog_data *vue_prog_data = &gs_prog_data->base;
   struct brw_stage_prog_data *prog_data = &vue_prog_data->base;
   enum brw_param_builtin *system_values;
   unsigned num_system_values;
   unsigned num_cbufs;

   nir_shader *nir = nir_shader_clone(mem_ctx, ish->nir);

   if (key->nr_userclip_plane_consts) {
      nir_function_impl *impl = nir_shader_get_entrypoint(nir);
      nir_lower_clip_gs(nir, (1 << key->nr_userclip_plane_consts) - 1);
      nir_lower_io_to_temporaries(nir, impl, true, false);
      nir_lower_global_vars_to_local(nir);
      nir_lower_vars_to_ssa(nir);
      nir_shader_gather_info(nir, impl);
   }

   iris_setup_uniforms(compiler, mem_ctx, nir, prog_data, &system_values,
                       &num_system_values, &num_cbufs);

   struct iris_binding_table bt;
   iris_setup_binding_table(nir, &bt, /* num_render_targets */ 0,
                            num_system_values, num_cbufs);

   brw_nir_analyze_ubo_ranges(compiler, nir, NULL, prog_data->ubo_ranges);

   brw_compute_vue_map(devinfo,
                       &vue_prog_data->vue_map, nir->info.outputs_written,
                       nir->info.separate_shader);

   char *error_str = NULL;
   const unsigned *program =
      brw_compile_gs(compiler, &ice->dbg, mem_ctx, key, gs_prog_data, nir,
                     NULL, -1, &error_str);
   if (program == NULL) {
      dbg_printf("Failed to compile geometry shader: %s\n", error_str);
      ralloc_free(mem_ctx);
      return false;
   }

   if (ish->compiled_once) {
      iris_debug_recompile(ice, &nir->info, &key->base);
   } else {
      ish->compiled_once = true;
   }

   uint32_t *so_decls =
      ice->vtbl.create_so_decl_list(&ish->stream_output,
                                    &vue_prog_data->vue_map);

   struct iris_compiled_shader *shader =
      iris_upload_shader(ice, IRIS_CACHE_GS, sizeof(*key), key, program,
                         prog_data, so_decls, system_values, num_system_values,
                         num_cbufs, &bt);

   iris_disk_cache_store(screen->disk_cache, ish, shader, key, sizeof(*key));

   ralloc_free(mem_ctx);
   return shader;
}

/**
 * Update the current geometry shader variant.
 *
 * Fill out the key, look in the cache, compile and bind if needed.
 */
static void
iris_update_compiled_gs(struct iris_context *ice)
{
   struct iris_shader_state *shs = &ice->state.shaders[MESA_SHADER_GEOMETRY];
   struct iris_uncompiled_shader *ish =
      ice->shaders.uncompiled[MESA_SHADER_GEOMETRY];
   struct iris_compiled_shader *old = ice->shaders.prog[IRIS_CACHE_GS];
   struct iris_compiled_shader *shader = NULL;

   if (ish) {
      struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
      const struct gen_device_info *devinfo = &screen->devinfo;
      struct brw_gs_prog_key key = { KEY_INIT(devinfo->gen) };
      ice->vtbl.populate_gs_key(ice, &ish->nir->info, last_vue_stage(ice), &key);

      shader =
         iris_find_cached_shader(ice, IRIS_CACHE_GS, sizeof(key), &key);

      if (!shader)
         shader = iris_disk_cache_retrieve(ice, ish, &key, sizeof(key));

      if (!shader)
         shader = iris_compile_gs(ice, ish, &key);
   }

   if (old != shader) {
      ice->shaders.prog[IRIS_CACHE_GS] = shader;
      ice->state.dirty |= IRIS_DIRTY_GS |
                          IRIS_DIRTY_BINDINGS_GS |
                          IRIS_DIRTY_CONSTANTS_GS;
      shs->sysvals_need_upload = true;
   }
}

/**
 * Compile a fragment (pixel) shader, and upload the assembly.
 */
static struct iris_compiled_shader *
iris_compile_fs(struct iris_context *ice,
                struct iris_uncompiled_shader *ish,
                const struct brw_wm_prog_key *key,
                struct brw_vue_map *vue_map)
{
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   const struct brw_compiler *compiler = screen->compiler;
   void *mem_ctx = ralloc_context(NULL);
   struct brw_wm_prog_data *fs_prog_data =
      rzalloc(mem_ctx, struct brw_wm_prog_data);
   struct brw_stage_prog_data *prog_data = &fs_prog_data->base;
   enum brw_param_builtin *system_values;
   unsigned num_system_values;
   unsigned num_cbufs;

   nir_shader *nir = nir_shader_clone(mem_ctx, ish->nir);

   prog_data->use_alt_mode = ish->use_alt_mode;

   iris_setup_uniforms(compiler, mem_ctx, nir, prog_data, &system_values,
                       &num_system_values, &num_cbufs);

   struct iris_binding_table bt;
   iris_setup_binding_table(nir, &bt, MAX2(key->nr_color_regions, 1),
                            num_system_values, num_cbufs);

   brw_nir_analyze_ubo_ranges(compiler, nir, NULL, prog_data->ubo_ranges);

   char *error_str = NULL;
   const unsigned *program =
      brw_compile_fs(compiler, &ice->dbg, mem_ctx, key, fs_prog_data,
                     nir, NULL, -1, -1, -1, true, false, vue_map, &error_str);
   if (program == NULL) {
      dbg_printf("Failed to compile fragment shader: %s\n", error_str);
      ralloc_free(mem_ctx);
      return false;
   }

   if (ish->compiled_once) {
      iris_debug_recompile(ice, &nir->info, &key->base);
   } else {
      ish->compiled_once = true;
   }

   struct iris_compiled_shader *shader =
      iris_upload_shader(ice, IRIS_CACHE_FS, sizeof(*key), key, program,
                         prog_data, NULL, system_values, num_system_values,
                         num_cbufs, &bt);

   iris_disk_cache_store(screen->disk_cache, ish, shader, key, sizeof(*key));

   ralloc_free(mem_ctx);
   return shader;
}

/**
 * Update the current fragment shader variant.
 *
 * Fill out the key, look in the cache, compile and bind if needed.
 */
static void
iris_update_compiled_fs(struct iris_context *ice)
{
   struct iris_shader_state *shs = &ice->state.shaders[MESA_SHADER_FRAGMENT];
   struct iris_uncompiled_shader *ish =
      ice->shaders.uncompiled[MESA_SHADER_FRAGMENT];
      struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
      const struct gen_device_info *devinfo = &screen->devinfo;
   struct brw_wm_prog_key key = { KEY_INIT(devinfo->gen) };
   ice->vtbl.populate_fs_key(ice, &ish->nir->info, &key);

   if (ish->nos & (1ull << IRIS_NOS_LAST_VUE_MAP))
      key.input_slots_valid = ice->shaders.last_vue_map->slots_valid;

   struct iris_compiled_shader *old = ice->shaders.prog[IRIS_CACHE_FS];
   struct iris_compiled_shader *shader =
      iris_find_cached_shader(ice, IRIS_CACHE_FS, sizeof(key), &key);

   if (!shader)
      shader = iris_disk_cache_retrieve(ice, ish, &key, sizeof(key));

   if (!shader)
      shader = iris_compile_fs(ice, ish, &key, ice->shaders.last_vue_map);

   if (old != shader) {
      // XXX: only need to flag CLIP if barycentric has NONPERSPECTIVE
      // toggles.  might be able to avoid flagging SBE too.
      ice->shaders.prog[IRIS_CACHE_FS] = shader;
      ice->state.dirty |= IRIS_DIRTY_FS |
                          IRIS_DIRTY_BINDINGS_FS |
                          IRIS_DIRTY_CONSTANTS_FS |
                          IRIS_DIRTY_WM |
                          IRIS_DIRTY_CLIP |
                          IRIS_DIRTY_SBE;
      shs->sysvals_need_upload = true;
   }
}

/**
 * Update the last enabled stage's VUE map.
 *
 * When the shader feeding the rasterizer's output interface changes, we
 * need to re-emit various packets.
 */
static void
update_last_vue_map(struct iris_context *ice,
                    struct brw_stage_prog_data *prog_data)
{
   struct brw_vue_prog_data *vue_prog_data = (void *) prog_data;
   struct brw_vue_map *vue_map = &vue_prog_data->vue_map;
   struct brw_vue_map *old_map = ice->shaders.last_vue_map;
   const uint64_t changed_slots =
      (old_map ? old_map->slots_valid : 0ull) ^ vue_map->slots_valid;

   if (changed_slots & VARYING_BIT_VIEWPORT) {
      ice->state.num_viewports =
         (vue_map->slots_valid & VARYING_BIT_VIEWPORT) ? IRIS_MAX_VIEWPORTS : 1;
      ice->state.dirty |= IRIS_DIRTY_CLIP |
                          IRIS_DIRTY_SF_CL_VIEWPORT |
                          IRIS_DIRTY_CC_VIEWPORT |
                          IRIS_DIRTY_SCISSOR_RECT |
                          IRIS_DIRTY_UNCOMPILED_FS |
                          ice->state.dirty_for_nos[IRIS_NOS_LAST_VUE_MAP];
   }

   if (changed_slots || (old_map && old_map->separate != vue_map->separate)) {
      ice->state.dirty |= IRIS_DIRTY_SBE;
   }

   ice->shaders.last_vue_map = &vue_prog_data->vue_map;
}

/**
 * Get the prog_data for a given stage, or NULL if the stage is disabled.
 */
static struct brw_vue_prog_data *
get_vue_prog_data(struct iris_context *ice, gl_shader_stage stage)
{
   if (!ice->shaders.prog[stage])
      return NULL;

   return (void *) ice->shaders.prog[stage]->prog_data;
}

// XXX: iris_compiled_shaders are space-leaking :(
// XXX: do remember to unbind them if deleting them.

/**
 * Update the current shader variants for the given state.
 *
 * This should be called on every draw call to ensure that the correct
 * shaders are bound.  It will also flag any dirty state triggered by
 * swapping out those shaders.
 */
void
iris_update_compiled_shaders(struct iris_context *ice)
{
   const uint64_t dirty = ice->state.dirty;

   struct brw_vue_prog_data *old_prog_datas[4];
   if (!(dirty & IRIS_DIRTY_URB)) {
      for (int i = MESA_SHADER_VERTEX; i <= MESA_SHADER_GEOMETRY; i++)
         old_prog_datas[i] = get_vue_prog_data(ice, i);
   }

   if (dirty & (IRIS_DIRTY_UNCOMPILED_TCS | IRIS_DIRTY_UNCOMPILED_TES)) {
       struct iris_uncompiled_shader *tes =
          ice->shaders.uncompiled[MESA_SHADER_TESS_EVAL];
       if (tes) {
          iris_update_compiled_tcs(ice);
          iris_update_compiled_tes(ice);
       } else {
          ice->shaders.prog[IRIS_CACHE_TCS] = NULL;
          ice->shaders.prog[IRIS_CACHE_TES] = NULL;
          ice->state.dirty |=
             IRIS_DIRTY_TCS | IRIS_DIRTY_TES |
             IRIS_DIRTY_BINDINGS_TCS | IRIS_DIRTY_BINDINGS_TES |
             IRIS_DIRTY_CONSTANTS_TCS | IRIS_DIRTY_CONSTANTS_TES;
       }
   }

   if (dirty & IRIS_DIRTY_UNCOMPILED_VS)
      iris_update_compiled_vs(ice);
   if (dirty & IRIS_DIRTY_UNCOMPILED_GS)
      iris_update_compiled_gs(ice);

   if (dirty & (IRIS_DIRTY_UNCOMPILED_GS | IRIS_DIRTY_UNCOMPILED_TES)) {
      const struct iris_compiled_shader *gs =
         ice->shaders.prog[MESA_SHADER_GEOMETRY];
      const struct iris_compiled_shader *tes =
         ice->shaders.prog[MESA_SHADER_TESS_EVAL];

      bool points_or_lines = false;

      if (gs) {
         const struct brw_gs_prog_data *gs_prog_data = (void *) gs->prog_data;
         points_or_lines =
            gs_prog_data->output_topology == _3DPRIM_POINTLIST ||
            gs_prog_data->output_topology == _3DPRIM_LINESTRIP;
      } else if (tes) {
         const struct brw_tes_prog_data *tes_data = (void *) tes->prog_data;
         points_or_lines =
            tes_data->output_topology == BRW_TESS_OUTPUT_TOPOLOGY_LINE ||
            tes_data->output_topology == BRW_TESS_OUTPUT_TOPOLOGY_POINT;
      }

      if (ice->shaders.output_topology_is_points_or_lines != points_or_lines) {
         /* Outbound to XY Clip enables */
         ice->shaders.output_topology_is_points_or_lines = points_or_lines;
         ice->state.dirty |= IRIS_DIRTY_CLIP;
      }
   }

   gl_shader_stage last_stage = last_vue_stage(ice);
   struct iris_compiled_shader *shader = ice->shaders.prog[last_stage];
   struct iris_uncompiled_shader *ish = ice->shaders.uncompiled[last_stage];
   update_last_vue_map(ice, shader->prog_data);
   if (ice->state.streamout != shader->streamout) {
      ice->state.streamout = shader->streamout;
      ice->state.dirty |= IRIS_DIRTY_SO_DECL_LIST | IRIS_DIRTY_STREAMOUT;
   }

   if (ice->state.streamout_active) {
      for (int i = 0; i < PIPE_MAX_SO_BUFFERS; i++) {
         struct iris_stream_output_target *so =
            (void *) ice->state.so_target[i];
         if (so)
            so->stride = ish->stream_output.stride[i] * sizeof(uint32_t);
      }
   }

   if (dirty & IRIS_DIRTY_UNCOMPILED_FS)
      iris_update_compiled_fs(ice);

   /* Changing shader interfaces may require a URB configuration. */
   if (!(dirty & IRIS_DIRTY_URB)) {
      for (int i = MESA_SHADER_VERTEX; i <= MESA_SHADER_GEOMETRY; i++) {
         struct brw_vue_prog_data *old = old_prog_datas[i];
         struct brw_vue_prog_data *new = get_vue_prog_data(ice, i);
         if (!!old != !!new ||
             (new && new->urb_entry_size != old->urb_entry_size)) {
            ice->state.dirty |= IRIS_DIRTY_URB;
            break;
         }
      }
   }
}

static struct iris_compiled_shader *
iris_compile_cs(struct iris_context *ice,
                struct iris_uncompiled_shader *ish,
                const struct brw_cs_prog_key *key)
{
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   const struct brw_compiler *compiler = screen->compiler;
   void *mem_ctx = ralloc_context(NULL);
   struct brw_cs_prog_data *cs_prog_data =
      rzalloc(mem_ctx, struct brw_cs_prog_data);
   struct brw_stage_prog_data *prog_data = &cs_prog_data->base;
   enum brw_param_builtin *system_values;
   unsigned num_system_values;
   unsigned num_cbufs;

   nir_shader *nir = nir_shader_clone(mem_ctx, ish->nir);

   prog_data->total_shared = nir->info.cs.shared_size;

   iris_setup_uniforms(compiler, mem_ctx, nir, prog_data, &system_values,
                       &num_system_values, &num_cbufs);

   struct iris_binding_table bt;
   iris_setup_binding_table(nir, &bt, /* num_render_targets */ 0,
                            num_system_values, num_cbufs);

   char *error_str = NULL;
   const unsigned *program =
      brw_compile_cs(compiler, &ice->dbg, mem_ctx, key, cs_prog_data,
                     nir, -1, &error_str);
   if (program == NULL) {
      dbg_printf("Failed to compile compute shader: %s\n", error_str);
      ralloc_free(mem_ctx);
      return false;
   }

   if (ish->compiled_once) {
      iris_debug_recompile(ice, &nir->info, &key->base);
   } else {
      ish->compiled_once = true;
   }

   struct iris_compiled_shader *shader =
      iris_upload_shader(ice, IRIS_CACHE_CS, sizeof(*key), key, program,
                         prog_data, NULL, system_values, num_system_values,
                         num_cbufs, &bt);

   iris_disk_cache_store(screen->disk_cache, ish, shader, key, sizeof(*key));

   ralloc_free(mem_ctx);
   return shader;
}

void
iris_update_compiled_compute_shader(struct iris_context *ice)
{
   struct iris_shader_state *shs = &ice->state.shaders[MESA_SHADER_COMPUTE];
   struct iris_uncompiled_shader *ish =
      ice->shaders.uncompiled[MESA_SHADER_COMPUTE];

   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   const struct gen_device_info *devinfo = &screen->devinfo;
   struct brw_cs_prog_key key = { KEY_INIT(devinfo->gen) };
   ice->vtbl.populate_cs_key(ice, &key);

   struct iris_compiled_shader *old = ice->shaders.prog[IRIS_CACHE_CS];
   struct iris_compiled_shader *shader =
      iris_find_cached_shader(ice, IRIS_CACHE_CS, sizeof(key), &key);

   if (!shader)
      shader = iris_disk_cache_retrieve(ice, ish, &key, sizeof(key));

   if (!shader)
      shader = iris_compile_cs(ice, ish, &key);

   if (old != shader) {
      ice->shaders.prog[IRIS_CACHE_CS] = shader;
      ice->state.dirty |= IRIS_DIRTY_CS |
                          IRIS_DIRTY_BINDINGS_CS |
                          IRIS_DIRTY_CONSTANTS_CS;
      shs->sysvals_need_upload = true;
   }
}

void
iris_fill_cs_push_const_buffer(struct brw_cs_prog_data *cs_prog_data,
                               uint32_t *dst)
{
   assert(cs_prog_data->push.total.size > 0);
   assert(cs_prog_data->push.cross_thread.size == 0);
   assert(cs_prog_data->push.per_thread.dwords == 1);
   assert(cs_prog_data->base.param[0] == BRW_PARAM_BUILTIN_SUBGROUP_ID);
   for (unsigned t = 0; t < cs_prog_data->threads; t++)
      dst[8 * t] = t;
}

/**
 * Allocate scratch BOs as needed for the given per-thread size and stage.
 */
struct iris_bo *
iris_get_scratch_space(struct iris_context *ice,
                       unsigned per_thread_scratch,
                       gl_shader_stage stage)
{
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   struct iris_bufmgr *bufmgr = screen->bufmgr;
   const struct gen_device_info *devinfo = &screen->devinfo;

   unsigned encoded_size = ffs(per_thread_scratch) - 11;
   assert(encoded_size < (1 << 16));

   struct iris_bo **bop = &ice->shaders.scratch_bos[encoded_size][stage];

   /* The documentation for 3DSTATE_PS "Scratch Space Base Pointer" says:
    *
    *    "Scratch Space per slice is computed based on 4 sub-slices.  SW
    *     must allocate scratch space enough so that each slice has 4
    *     slices allowed."
    *
    * According to the other driver team, this applies to compute shaders
    * as well.  This is not currently documented at all.
    *
    * This hack is no longer necessary on Gen11+.
    */
   unsigned subslice_total = screen->subslice_total;
   if (devinfo->gen < 11)
      subslice_total = 4 * devinfo->num_slices;
   assert(subslice_total >= screen->subslice_total);

   if (!*bop) {
      unsigned scratch_ids_per_subslice = devinfo->max_cs_threads;
      uint32_t max_threads[] = {
         [MESA_SHADER_VERTEX]    = devinfo->max_vs_threads,
         [MESA_SHADER_TESS_CTRL] = devinfo->max_tcs_threads,
         [MESA_SHADER_TESS_EVAL] = devinfo->max_tes_threads,
         [MESA_SHADER_GEOMETRY]  = devinfo->max_gs_threads,
         [MESA_SHADER_FRAGMENT]  = devinfo->max_wm_threads,
         [MESA_SHADER_COMPUTE]   = scratch_ids_per_subslice * subslice_total,
      };

      uint32_t size = per_thread_scratch * max_threads[stage];

      *bop = iris_bo_alloc(bufmgr, "scratch", size, IRIS_MEMZONE_SHADER);
   }

   return *bop;
}

/* ------------------------------------------------------------------- */

/**
 * The pipe->create_[stage]_state() driver hooks.
 *
 * Performs basic NIR preprocessing, records any state dependencies, and
 * returns an iris_uncompiled_shader as the Gallium CSO.
 *
 * Actual shader compilation to assembly happens later, at first use.
 */
static void *
iris_create_uncompiled_shader(struct pipe_context *ctx,
                              nir_shader *nir,
                              const struct pipe_stream_output_info *so_info)
{
   struct iris_context *ice = (void *)ctx;
   struct iris_screen *screen = (struct iris_screen *)ctx->screen;
   const struct gen_device_info *devinfo = &screen->devinfo;

   struct iris_uncompiled_shader *ish =
      calloc(1, sizeof(struct iris_uncompiled_shader));
   if (!ish)
      return NULL;

   brw_preprocess_nir(screen->compiler, nir, NULL);

   NIR_PASS_V(nir, brw_nir_lower_image_load_store, devinfo);
   NIR_PASS_V(nir, iris_lower_storage_image_derefs);

   nir_sweep(nir);

   if (nir->constant_data_size > 0) {
      unsigned data_offset;
      u_upload_data(ice->shaders.uploader, 0, nir->constant_data_size,
                    32, nir->constant_data, &data_offset, &ish->const_data);

      struct pipe_shader_buffer psb = {
         .buffer = ish->const_data,
         .buffer_offset = data_offset,
         .buffer_size = nir->constant_data_size,
      };
      iris_upload_ubo_ssbo_surf_state(ice, &psb, &ish->const_data_state, false);
   }

   ish->program_id = get_new_program_id(screen);
   ish->nir = nir;
   if (so_info) {
      memcpy(&ish->stream_output, so_info, sizeof(*so_info));
      update_so_info(&ish->stream_output, nir->info.outputs_written);
   }

   /* Save this now before potentially dropping nir->info.name */
   if (nir->info.name && strncmp(nir->info.name, "ARB", 3) == 0)
      ish->use_alt_mode = true;

   if (screen->disk_cache) {
      /* Serialize the NIR to a binary blob that we can hash for the disk
       * cache.  First, drop unnecessary information (like variable names)
       * so the serialized NIR is smaller, and also to let us detect more
       * isomorphic shaders when hashing, increasing cache hits.  We clone
       * the NIR before stripping away this info because it can be useful
       * when inspecting and debugging shaders.
       */
      nir_shader *clone = nir_shader_clone(NULL, nir);
      nir_strip(clone);

      struct blob blob;
      blob_init(&blob);
      nir_serialize(&blob, clone);
      _mesa_sha1_compute(blob.data, blob.size, ish->nir_sha1);
      blob_finish(&blob);

      ralloc_free(clone);
   }

   return ish;
}

static struct iris_uncompiled_shader *
iris_create_shader_state(struct pipe_context *ctx,
                         const struct pipe_shader_state *state)
{
   struct nir_shader *nir;

   if (state->type == PIPE_SHADER_IR_TGSI)
      nir = tgsi_to_nir(state->tokens, ctx->screen);
   else
      nir = state->ir.nir;

   return iris_create_uncompiled_shader(ctx, nir, &state->stream_output);
}

static void *
iris_create_vs_state(struct pipe_context *ctx,
                     const struct pipe_shader_state *state)
{
   struct iris_context *ice = (void *) ctx;
   struct iris_screen *screen = (void *) ctx->screen;
   struct iris_uncompiled_shader *ish = iris_create_shader_state(ctx, state);

   /* User clip planes */
   if (ish->nir->info.clip_distance_array_size == 0)
      ish->nos |= (1ull << IRIS_NOS_RASTERIZER);

   if (screen->precompile) {
      const struct gen_device_info *devinfo = &screen->devinfo;
      struct brw_vs_prog_key key = { KEY_INIT(devinfo->gen) };

      if (!iris_disk_cache_retrieve(ice, ish, &key, sizeof(key)))
         iris_compile_vs(ice, ish, &key);
   }

   return ish;
}

static void *
iris_create_tcs_state(struct pipe_context *ctx,
                      const struct pipe_shader_state *state)
{
   struct iris_context *ice = (void *) ctx;
   struct iris_screen *screen = (void *) ctx->screen;
   const struct brw_compiler *compiler = screen->compiler;
   struct iris_uncompiled_shader *ish = iris_create_shader_state(ctx, state);
   struct shader_info *info = &ish->nir->info;

   if (screen->precompile) {
      const unsigned _GL_TRIANGLES = 0x0004;
      const struct gen_device_info *devinfo = &screen->devinfo;
      struct brw_tcs_prog_key key = {
         KEY_INIT(devinfo->gen),
         // XXX: make sure the linker fills this out from the TES...
         .tes_primitive_mode =
            info->tess.primitive_mode ? info->tess.primitive_mode
                                      : _GL_TRIANGLES,
         .outputs_written = info->outputs_written,
         .patch_outputs_written = info->patch_outputs_written,
      };

      /* 8_PATCH mode needs the key to contain the input patch dimensionality.
       * We don't have that information, so we randomly guess that the input
       * and output patches are the same size.  This is a bad guess, but we
       * can't do much better.
       */
      if (compiler->use_tcs_8_patch)
         key.input_vertices = info->tess.tcs_vertices_out;

      if (!iris_disk_cache_retrieve(ice, ish, &key, sizeof(key)))
         iris_compile_tcs(ice, ish, &key);
   }

   return ish;
}

static void *
iris_create_tes_state(struct pipe_context *ctx,
                     const struct pipe_shader_state *state)
{
   struct iris_context *ice = (void *) ctx;
   struct iris_screen *screen = (void *) ctx->screen;
   struct iris_uncompiled_shader *ish = iris_create_shader_state(ctx, state);
   struct shader_info *info = &ish->nir->info;

   /* User clip planes */
   if (ish->nir->info.clip_distance_array_size == 0)
      ish->nos |= (1ull << IRIS_NOS_RASTERIZER);

   if (screen->precompile) {
      const struct gen_device_info *devinfo = &screen->devinfo;
      struct brw_tes_prog_key key = {
         KEY_INIT(devinfo->gen),
         // XXX: not ideal, need TCS output/TES input unification
         .inputs_read = info->inputs_read,
         .patch_inputs_read = info->patch_inputs_read,
      };

      if (!iris_disk_cache_retrieve(ice, ish, &key, sizeof(key)))
         iris_compile_tes(ice, ish, &key);
   }

   return ish;
}

static void *
iris_create_gs_state(struct pipe_context *ctx,
                     const struct pipe_shader_state *state)
{
   struct iris_context *ice = (void *) ctx;
   struct iris_screen *screen = (void *) ctx->screen;
   struct iris_uncompiled_shader *ish = iris_create_shader_state(ctx, state);

   /* User clip planes */
   if (ish->nir->info.clip_distance_array_size == 0)
      ish->nos |= (1ull << IRIS_NOS_RASTERIZER);

   if (screen->precompile) {
      const struct gen_device_info *devinfo = &screen->devinfo;
      struct brw_gs_prog_key key = { KEY_INIT(devinfo->gen) };

      if (!iris_disk_cache_retrieve(ice, ish, &key, sizeof(key)))
         iris_compile_gs(ice, ish, &key);
   }

   return ish;
}

static void *
iris_create_fs_state(struct pipe_context *ctx,
                     const struct pipe_shader_state *state)
{
   struct iris_context *ice = (void *) ctx;
   struct iris_screen *screen = (void *) ctx->screen;
   struct iris_uncompiled_shader *ish = iris_create_shader_state(ctx, state);
   struct shader_info *info = &ish->nir->info;

   ish->nos |= (1ull << IRIS_NOS_FRAMEBUFFER) |
               (1ull << IRIS_NOS_DEPTH_STENCIL_ALPHA) |
               (1ull << IRIS_NOS_RASTERIZER) |
               (1ull << IRIS_NOS_BLEND);

   /* The program key needs the VUE map if there are > 16 inputs */
   if (util_bitcount64(ish->nir->info.inputs_read &
                       BRW_FS_VARYING_INPUT_MASK) > 16) {
      ish->nos |= (1ull << IRIS_NOS_LAST_VUE_MAP);
   }

   if (screen->precompile) {
      const uint64_t color_outputs = info->outputs_written &
         ~(BITFIELD64_BIT(FRAG_RESULT_DEPTH) |
           BITFIELD64_BIT(FRAG_RESULT_STENCIL) |
           BITFIELD64_BIT(FRAG_RESULT_SAMPLE_MASK));

      bool can_rearrange_varyings =
         util_bitcount64(info->inputs_read & BRW_FS_VARYING_INPUT_MASK) <= 16;

      const struct gen_device_info *devinfo = &screen->devinfo;
      struct brw_wm_prog_key key = {
         KEY_INIT(devinfo->gen),
         .nr_color_regions = util_bitcount(color_outputs),
         .coherent_fb_fetch = true,
         .input_slots_valid =
            can_rearrange_varyings ? 0 : info->inputs_read | VARYING_BIT_POS,
      };

      if (!iris_disk_cache_retrieve(ice, ish, &key, sizeof(key)))
         iris_compile_fs(ice, ish, &key, NULL);
   }

   return ish;
}

static void *
iris_create_compute_state(struct pipe_context *ctx,
                          const struct pipe_compute_state *state)
{
   assert(state->ir_type == PIPE_SHADER_IR_NIR);

   struct iris_context *ice = (void *) ctx;
   struct iris_screen *screen = (void *) ctx->screen;
   struct iris_uncompiled_shader *ish =
      iris_create_uncompiled_shader(ctx, (void *) state->prog, NULL);

   // XXX: disallow more than 64KB of shared variables

   if (screen->precompile) {
      const struct gen_device_info *devinfo = &screen->devinfo;
      struct brw_cs_prog_key key = { KEY_INIT(devinfo->gen) };

      if (!iris_disk_cache_retrieve(ice, ish, &key, sizeof(key)))
         iris_compile_cs(ice, ish, &key);
   }

   return ish;
}

/**
 * The pipe->delete_[stage]_state() driver hooks.
 *
 * Frees the iris_uncompiled_shader.
 */
static void
iris_delete_shader_state(struct pipe_context *ctx, void *state, gl_shader_stage stage)
{
   struct iris_uncompiled_shader *ish = state;
   struct iris_context *ice = (void *) ctx;

   if (ice->shaders.uncompiled[stage] == ish) {
      ice->shaders.uncompiled[stage] = NULL;
      ice->state.dirty |= IRIS_DIRTY_UNCOMPILED_VS << stage;
   }

   if (ish->const_data) {
      pipe_resource_reference(&ish->const_data, NULL);
      pipe_resource_reference(&ish->const_data_state.res, NULL);
   }

   ralloc_free(ish->nir);
   free(ish);
}

static void
iris_delete_vs_state(struct pipe_context *ctx, void *state)
{
   iris_delete_shader_state(ctx, state, MESA_SHADER_VERTEX);
}

static void
iris_delete_tcs_state(struct pipe_context *ctx, void *state)
{
   iris_delete_shader_state(ctx, state, MESA_SHADER_TESS_CTRL);
}

static void
iris_delete_tes_state(struct pipe_context *ctx, void *state)
{
   iris_delete_shader_state(ctx, state, MESA_SHADER_TESS_EVAL);
}

static void
iris_delete_gs_state(struct pipe_context *ctx, void *state)
{
   iris_delete_shader_state(ctx, state, MESA_SHADER_GEOMETRY);
}

static void
iris_delete_fs_state(struct pipe_context *ctx, void *state)
{
   iris_delete_shader_state(ctx, state, MESA_SHADER_FRAGMENT);
}

static void
iris_delete_cs_state(struct pipe_context *ctx, void *state)
{
   iris_delete_shader_state(ctx, state, MESA_SHADER_COMPUTE);
}

/**
 * The pipe->bind_[stage]_state() driver hook.
 *
 * Binds an uncompiled shader as the current one for a particular stage.
 * Updates dirty tracking to account for the shader's NOS.
 */
static void
bind_shader_state(struct iris_context *ice,
                  struct iris_uncompiled_shader *ish,
                  gl_shader_stage stage)
{
   uint64_t dirty_bit = IRIS_DIRTY_UNCOMPILED_VS << stage;
   const uint64_t nos = ish ? ish->nos : 0;

   const struct shader_info *old_info = iris_get_shader_info(ice, stage);
   const struct shader_info *new_info = ish ? &ish->nir->info : NULL;

   if ((old_info ? util_last_bit(old_info->textures_used) : 0) !=
       (new_info ? util_last_bit(new_info->textures_used) : 0)) {
      ice->state.dirty |= IRIS_DIRTY_SAMPLER_STATES_VS << stage;
   }

   ice->shaders.uncompiled[stage] = ish;
   ice->state.dirty |= dirty_bit;

   /* Record that CSOs need to mark IRIS_DIRTY_UNCOMPILED_XS when they change
    * (or that they no longer need to do so).
    */
   for (int i = 0; i < IRIS_NOS_COUNT; i++) {
      if (nos & (1 << i))
         ice->state.dirty_for_nos[i] |= dirty_bit;
      else
         ice->state.dirty_for_nos[i] &= ~dirty_bit;
   }
}

static void
iris_bind_vs_state(struct pipe_context *ctx, void *state)
{
   bind_shader_state((void *) ctx, state, MESA_SHADER_VERTEX);
}

static void
iris_bind_tcs_state(struct pipe_context *ctx, void *state)
{
   bind_shader_state((void *) ctx, state, MESA_SHADER_TESS_CTRL);
}

static void
iris_bind_tes_state(struct pipe_context *ctx, void *state)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   /* Enabling/disabling optional stages requires a URB reconfiguration. */
   if (!!state != !!ice->shaders.uncompiled[MESA_SHADER_TESS_EVAL])
      ice->state.dirty |= IRIS_DIRTY_URB;

   bind_shader_state((void *) ctx, state, MESA_SHADER_TESS_EVAL);
}

static void
iris_bind_gs_state(struct pipe_context *ctx, void *state)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   /* Enabling/disabling optional stages requires a URB reconfiguration. */
   if (!!state != !!ice->shaders.uncompiled[MESA_SHADER_GEOMETRY])
      ice->state.dirty |= IRIS_DIRTY_URB;

   bind_shader_state((void *) ctx, state, MESA_SHADER_GEOMETRY);
}

static void
iris_bind_fs_state(struct pipe_context *ctx, void *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_uncompiled_shader *old_ish =
      ice->shaders.uncompiled[MESA_SHADER_FRAGMENT];
   struct iris_uncompiled_shader *new_ish = state;

   const unsigned color_bits =
      BITFIELD64_BIT(FRAG_RESULT_COLOR) |
      BITFIELD64_RANGE(FRAG_RESULT_DATA0, BRW_MAX_DRAW_BUFFERS);

   /* Fragment shader outputs influence HasWriteableRT */
   if (!old_ish || !new_ish ||
       (old_ish->nir->info.outputs_written & color_bits) !=
       (new_ish->nir->info.outputs_written & color_bits))
      ice->state.dirty |= IRIS_DIRTY_PS_BLEND;

   bind_shader_state((void *) ctx, state, MESA_SHADER_FRAGMENT);
}

static void
iris_bind_cs_state(struct pipe_context *ctx, void *state)
{
   bind_shader_state((void *) ctx, state, MESA_SHADER_COMPUTE);
}

void
iris_init_program_functions(struct pipe_context *ctx)
{
   ctx->create_vs_state  = iris_create_vs_state;
   ctx->create_tcs_state = iris_create_tcs_state;
   ctx->create_tes_state = iris_create_tes_state;
   ctx->create_gs_state  = iris_create_gs_state;
   ctx->create_fs_state  = iris_create_fs_state;
   ctx->create_compute_state = iris_create_compute_state;

   ctx->delete_vs_state  = iris_delete_vs_state;
   ctx->delete_tcs_state = iris_delete_tcs_state;
   ctx->delete_tes_state = iris_delete_tes_state;
   ctx->delete_gs_state  = iris_delete_gs_state;
   ctx->delete_fs_state  = iris_delete_fs_state;
   ctx->delete_compute_state = iris_delete_cs_state;

   ctx->bind_vs_state  = iris_bind_vs_state;
   ctx->bind_tcs_state = iris_bind_tcs_state;
   ctx->bind_tes_state = iris_bind_tes_state;
   ctx->bind_gs_state  = iris_bind_gs_state;
   ctx->bind_fs_state  = iris_bind_fs_state;
   ctx->bind_compute_state = iris_bind_cs_state;
}
