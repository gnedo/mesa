/*
 * Copyright © 2014-2015 Broadcom
 * Copyright (C) 2014 Rob Clark <robclark@freedesktop.org>
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "util/ralloc.h"
#include "pipe/p_screen.h"

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_control_flow.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/glsl/gl_nir.h"
#include "compiler/glsl/list.h"
#include "compiler/shader_enums.h"

#include "tgsi_to_nir.h"
#include "tgsi/tgsi_parse.h"
#include "tgsi/tgsi_dump.h"
#include "tgsi/tgsi_info.h"
#include "tgsi/tgsi_scan.h"
#include "tgsi/tgsi_from_mesa.h"

#define SWIZ(X, Y, Z, W) (unsigned[4]){      \
      TGSI_SWIZZLE_##X,                      \
      TGSI_SWIZZLE_##Y,                      \
      TGSI_SWIZZLE_##Z,                      \
      TGSI_SWIZZLE_##W,                      \
   }

struct ttn_reg_info {
   /** nir register containing this TGSI index. */
   nir_register *reg;
   nir_variable *var;
   /** Offset (in vec4s) from the start of var for this TGSI index. */
   int offset;
};

struct ttn_compile {
   union tgsi_full_token *token;
   nir_builder build;
   struct tgsi_shader_info *scan;

   struct ttn_reg_info *output_regs;
   struct ttn_reg_info *temp_regs;
   nir_ssa_def **imm_defs;

   unsigned num_samp_types;
   nir_alu_type *samp_types;

   nir_register *addr_reg;

   nir_variable **inputs;
   nir_variable **outputs;
   nir_variable *samplers[PIPE_MAX_SAMPLERS];

   nir_variable *input_var_face;
   nir_variable *input_var_position;
   nir_variable *input_var_point;

   /**
    * Stack of nir_cursors where instructions should be pushed as we pop
    * back out of the control flow stack.
    *
    * For each IF/ELSE/ENDIF block, if_stack[if_stack_pos] has where the else
    * instructions should be placed, and if_stack[if_stack_pos - 1] has where
    * the next instructions outside of the if/then/else block go.
    */
   nir_cursor *if_stack;
   unsigned if_stack_pos;

   /**
    * Stack of nir_cursors where instructions should be pushed as we pop
    * back out of the control flow stack.
    *
    * loop_stack[loop_stack_pos - 1] contains the cf_node_list for the outside
    * of the loop.
    */
   nir_cursor *loop_stack;
   unsigned loop_stack_pos;

   /* How many TGSI_FILE_IMMEDIATE vec4s have been parsed so far. */
   unsigned next_imm;

   bool cap_scalar;
   bool cap_face_is_sysval;
   bool cap_position_is_sysval;
   bool cap_point_is_sysval;
   bool cap_packed_uniforms;
   bool cap_samplers_as_deref;
};

#define ttn_swizzle(b, src, x, y, z, w) \
   nir_swizzle(b, src, SWIZ(x, y, z, w), 4)
#define ttn_channel(b, src, swiz) \
   nir_channel(b, src, TGSI_SWIZZLE_##swiz)

static gl_varying_slot
tgsi_varying_semantic_to_slot(unsigned semantic, unsigned index)
{
   switch (semantic) {
   case TGSI_SEMANTIC_POSITION:
      return VARYING_SLOT_POS;
   case TGSI_SEMANTIC_COLOR:
      if (index == 0)
         return VARYING_SLOT_COL0;
      else
         return VARYING_SLOT_COL1;
   case TGSI_SEMANTIC_BCOLOR:
      if (index == 0)
         return VARYING_SLOT_BFC0;
      else
         return VARYING_SLOT_BFC1;
   case TGSI_SEMANTIC_FOG:
      return VARYING_SLOT_FOGC;
   case TGSI_SEMANTIC_PSIZE:
      return VARYING_SLOT_PSIZ;
   case TGSI_SEMANTIC_GENERIC:
      return VARYING_SLOT_VAR0 + index;
   case TGSI_SEMANTIC_FACE:
      return VARYING_SLOT_FACE;
   case TGSI_SEMANTIC_EDGEFLAG:
      return VARYING_SLOT_EDGE;
   case TGSI_SEMANTIC_PRIMID:
      return VARYING_SLOT_PRIMITIVE_ID;
   case TGSI_SEMANTIC_CLIPDIST:
      if (index == 0)
         return VARYING_SLOT_CLIP_DIST0;
      else
         return VARYING_SLOT_CLIP_DIST1;
   case TGSI_SEMANTIC_CLIPVERTEX:
      return VARYING_SLOT_CLIP_VERTEX;
   case TGSI_SEMANTIC_TEXCOORD:
      return VARYING_SLOT_TEX0 + index;
   case TGSI_SEMANTIC_PCOORD:
      return VARYING_SLOT_PNTC;
   case TGSI_SEMANTIC_VIEWPORT_INDEX:
      return VARYING_SLOT_VIEWPORT;
   case TGSI_SEMANTIC_LAYER:
      return VARYING_SLOT_LAYER;
   default:
      fprintf(stderr, "Bad TGSI semantic: %d/%d\n", semantic, index);
      abort();
   }
}

static nir_ssa_def *
ttn_src_for_dest(nir_builder *b, nir_alu_dest *dest)
{
   nir_alu_src src;
   memset(&src, 0, sizeof(src));

   if (dest->dest.is_ssa)
      src.src = nir_src_for_ssa(&dest->dest.ssa);
   else {
      assert(!dest->dest.reg.indirect);
      src.src = nir_src_for_reg(dest->dest.reg.reg);
      src.src.reg.base_offset = dest->dest.reg.base_offset;
   }

   for (int i = 0; i < 4; i++)
      src.swizzle[i] = i;

   return nir_mov_alu(b, src, 4);
}

static enum glsl_interp_mode
ttn_translate_interp_mode(unsigned tgsi_interp)
{
   switch (tgsi_interp) {
   case TGSI_INTERPOLATE_CONSTANT:
      return INTERP_MODE_FLAT;
   case TGSI_INTERPOLATE_LINEAR:
      return INTERP_MODE_NOPERSPECTIVE;
   case TGSI_INTERPOLATE_PERSPECTIVE:
      return INTERP_MODE_SMOOTH;
   case TGSI_INTERPOLATE_COLOR:
      return INTERP_MODE_SMOOTH;
   default:
      unreachable("bad TGSI interpolation mode");
   }
}

static void
ttn_emit_declaration(struct ttn_compile *c)
{
   nir_builder *b = &c->build;
   struct tgsi_full_declaration *decl = &c->token->FullDeclaration;
   unsigned array_size = decl->Range.Last - decl->Range.First + 1;
   unsigned file = decl->Declaration.File;
   unsigned i;

   if (file == TGSI_FILE_TEMPORARY) {
      if (decl->Declaration.Array) {
         /* for arrays, we create variables instead of registers: */
         nir_variable *var = rzalloc(b->shader, nir_variable);

         var->type = glsl_array_type(glsl_vec4_type(), array_size, 0);
         var->data.mode = nir_var_shader_temp;
         var->name = ralloc_asprintf(var, "arr_%d", decl->Array.ArrayID);

         exec_list_push_tail(&b->shader->globals, &var->node);

         for (i = 0; i < array_size; i++) {
            /* point all the matching slots to the same var,
             * with appropriate offset set, mostly just so
             * we know what to do when tgsi does a non-indirect
             * access
             */
            c->temp_regs[decl->Range.First + i].reg = NULL;
            c->temp_regs[decl->Range.First + i].var = var;
            c->temp_regs[decl->Range.First + i].offset = i;
         }
      } else {
         for (i = 0; i < array_size; i++) {
            nir_register *reg = nir_local_reg_create(b->impl);
            reg->num_components = 4;
            c->temp_regs[decl->Range.First + i].reg = reg;
            c->temp_regs[decl->Range.First + i].var = NULL;
            c->temp_regs[decl->Range.First + i].offset = 0;
         }
      }
   } else if (file == TGSI_FILE_ADDRESS) {
      c->addr_reg = nir_local_reg_create(b->impl);
      c->addr_reg->num_components = 4;
   } else if (file == TGSI_FILE_SYSTEM_VALUE) {
      /* Nothing to record for system values. */
   } else if (file == TGSI_FILE_SAMPLER) {
      /* Nothing to record for samplers. */
   } else if (file == TGSI_FILE_SAMPLER_VIEW) {
      struct tgsi_declaration_sampler_view *sview = &decl->SamplerView;
      nir_alu_type type;

      assert((sview->ReturnTypeX == sview->ReturnTypeY) &&
             (sview->ReturnTypeX == sview->ReturnTypeZ) &&
             (sview->ReturnTypeX == sview->ReturnTypeW));

      switch (sview->ReturnTypeX) {
      case TGSI_RETURN_TYPE_SINT:
         type = nir_type_int;
         break;
      case TGSI_RETURN_TYPE_UINT:
         type = nir_type_uint;
         break;
      case TGSI_RETURN_TYPE_FLOAT:
      default:
         type = nir_type_float;
         break;
      }

      for (i = 0; i < array_size; i++) {
         c->samp_types[decl->Range.First + i] = type;
      }
   } else {
      bool is_array = (array_size > 1);

      assert(file == TGSI_FILE_INPUT ||
             file == TGSI_FILE_OUTPUT ||
             file == TGSI_FILE_CONSTANT);

      /* nothing to do for UBOs: */
      if ((file == TGSI_FILE_CONSTANT) && decl->Declaration.Dimension &&
          decl->Dim.Index2D != 0) {
         b->shader->info.num_ubos =
            MAX2(b->shader->info.num_ubos, decl->Dim.Index2D);
         return;
      }

      if ((file == TGSI_FILE_INPUT) || (file == TGSI_FILE_OUTPUT)) {
         is_array = (is_array && decl->Declaration.Array &&
                     (decl->Array.ArrayID != 0));
      }

      for (i = 0; i < array_size; i++) {
         unsigned idx = decl->Range.First + i;
         nir_variable *var = rzalloc(b->shader, nir_variable);

         var->data.driver_location = idx;

         var->type = glsl_vec4_type();
         if (is_array)
            var->type = glsl_array_type(var->type, array_size, 0);

         switch (file) {
         case TGSI_FILE_INPUT:
            var->data.read_only = true;
            var->data.mode = nir_var_shader_in;
            var->name = ralloc_asprintf(var, "in_%d", idx);

            if (c->scan->processor == PIPE_SHADER_FRAGMENT) {
               if (decl->Semantic.Name == TGSI_SEMANTIC_FACE) {
                  var->type = glsl_bool_type();
                  if (c->cap_face_is_sysval) {
                     var->data.mode = nir_var_system_value;
                     var->data.location = SYSTEM_VALUE_FRONT_FACE;
                  } else {
                     var->data.location = VARYING_SLOT_FACE;
                  }
                  c->input_var_face = var;
               } else if (decl->Semantic.Name == TGSI_SEMANTIC_POSITION) {
                  if (c->cap_position_is_sysval) {
                     var->data.mode = nir_var_system_value;
                     var->data.location = SYSTEM_VALUE_FRAG_COORD;
                  } else {
                     var->data.location = VARYING_SLOT_POS;
                  }
                  c->input_var_position = var;
               } else if (decl->Semantic.Name == TGSI_SEMANTIC_PCOORD) {
                  if (c->cap_point_is_sysval) {
                     var->data.mode = nir_var_system_value;
                     var->data.location = SYSTEM_VALUE_POINT_COORD;
                  } else {
                     var->data.location = VARYING_SLOT_PNTC;
                  }
                  c->input_var_point = var;
               } else {
                  var->data.location =
                     tgsi_varying_semantic_to_slot(decl->Semantic.Name,
                                                   decl->Semantic.Index);
               }
            } else {
               assert(!decl->Declaration.Semantic);
               var->data.location = VERT_ATTRIB_GENERIC0 + idx;
            }
            var->data.index = 0;
            var->data.interpolation =
               ttn_translate_interp_mode(decl->Interp.Interpolate);

            exec_list_push_tail(&b->shader->inputs, &var->node);
            c->inputs[idx] = var;

            for (int i = 0; i < array_size; i++)
               b->shader->info.inputs_read |= 1 << (var->data.location + i);

            break;
         case TGSI_FILE_OUTPUT: {
            int semantic_name = decl->Semantic.Name;
            int semantic_index = decl->Semantic.Index;
            /* Since we can't load from outputs in the IR, we make temporaries
             * for the outputs and emit stores to the real outputs at the end of
             * the shader.
             */
            nir_register *reg = nir_local_reg_create(b->impl);
            reg->num_components = 4;
            if (is_array)
               reg->num_array_elems = array_size;

            var->data.mode = nir_var_shader_out;
            var->name = ralloc_asprintf(var, "out_%d", idx);
            var->data.index = 0;
            var->data.interpolation =
               ttn_translate_interp_mode(decl->Interp.Interpolate);

            if (c->scan->processor == PIPE_SHADER_FRAGMENT) {
               switch (semantic_name) {
               case TGSI_SEMANTIC_COLOR: {
                  /* TODO tgsi loses some information, so we cannot
                   * actually differentiate here between DSB and MRT
                   * at this point.  But so far no drivers using tgsi-
                   * to-nir support dual source blend:
                   */
                  bool dual_src_blend = false;
                  if (dual_src_blend && (semantic_index == 1)) {
                     var->data.location = FRAG_RESULT_DATA0;
                     var->data.index = 1;
                  } else {
                     if (c->scan->properties[TGSI_PROPERTY_FS_COLOR0_WRITES_ALL_CBUFS])
                        var->data.location = FRAG_RESULT_COLOR;
                     else
                        var->data.location = FRAG_RESULT_DATA0 + semantic_index;
                  }
                  break;
               }
               case TGSI_SEMANTIC_POSITION:
                  var->data.location = FRAG_RESULT_DEPTH;
                  var->type = glsl_float_type();
                  break;
               default:
                  fprintf(stderr, "Bad TGSI semantic: %d/%d\n",
                          decl->Semantic.Name, decl->Semantic.Index);
                  abort();
               }
            } else {
               var->data.location =
                  tgsi_varying_semantic_to_slot(semantic_name, semantic_index);
            }

            if (is_array) {
               unsigned j;
               for (j = 0; j < array_size; j++) {
                  c->output_regs[idx + j].offset = i + j;
                  c->output_regs[idx + j].reg = reg;
               }
            } else {
               c->output_regs[idx].offset = i;
               c->output_regs[idx].reg = reg;
            }

            exec_list_push_tail(&b->shader->outputs, &var->node);
            c->outputs[idx] = var;

            for (int i = 0; i < array_size; i++)
               b->shader->info.outputs_written |= 1ull << (var->data.location + i);
         }
            break;
         case TGSI_FILE_CONSTANT:
            var->data.mode = nir_var_uniform;
            var->name = ralloc_asprintf(var, "uniform_%d", idx);
            var->data.location = idx;

            exec_list_push_tail(&b->shader->uniforms, &var->node);
            break;
         default:
            unreachable("bad declaration file");
            return;
         }

         if (is_array)
            break;
      }

   }
}

static void
ttn_emit_immediate(struct ttn_compile *c)
{
   nir_builder *b = &c->build;
   struct tgsi_full_immediate *tgsi_imm = &c->token->FullImmediate;
   nir_load_const_instr *load_const;
   int i;

   load_const = nir_load_const_instr_create(b->shader, 4, 32);
   c->imm_defs[c->next_imm] = &load_const->def;
   c->next_imm++;

   for (i = 0; i < load_const->def.num_components; i++)
      load_const->value[i].u32 = tgsi_imm->u[i].Uint;

   nir_builder_instr_insert(b, &load_const->instr);
}

static nir_ssa_def *
ttn_src_for_indirect(struct ttn_compile *c, struct tgsi_ind_register *indirect);

/* generate either a constant or indirect deref chain for accessing an
 * array variable.
 */
static nir_deref_instr *
ttn_array_deref(struct ttn_compile *c, nir_variable *var, unsigned offset,
                struct tgsi_ind_register *indirect)
{
   nir_deref_instr *deref = nir_build_deref_var(&c->build, var);
   nir_ssa_def *index = nir_imm_int(&c->build, offset);
   if (indirect)
      index = nir_iadd(&c->build, index, ttn_src_for_indirect(c, indirect));
   return nir_build_deref_array(&c->build, deref, index);
}

/* Special case: Turn the frontface varying into a load of the
 * frontface variable, and create the vector as required by TGSI.
 */
static nir_ssa_def *
ttn_emulate_tgsi_front_face(struct ttn_compile *c)
{
   nir_ssa_def *tgsi_frontface[4];

   if (c->cap_face_is_sysval) {
      /* When it's a system value, it should be an integer vector: (F, 0, 0, 1)
       * F is 0xffffffff if front-facing, 0 if not.
       */

      nir_ssa_def *frontface = nir_load_front_face(&c->build, 1);

      tgsi_frontface[0] = nir_bcsel(&c->build,
                             frontface,
                             nir_imm_int(&c->build, 0xffffffff),
                             nir_imm_int(&c->build, 0));
      tgsi_frontface[1] = nir_imm_int(&c->build, 0);
      tgsi_frontface[2] = nir_imm_int(&c->build, 0);
      tgsi_frontface[3] = nir_imm_int(&c->build, 1);
   } else {
      /* When it's an input, it should be a float vector: (F, 0.0, 0.0, 1.0)
       * F is positive if front-facing, negative if not.
       */

      assert(c->input_var_face);
      nir_ssa_def *frontface = nir_load_var(&c->build, c->input_var_face);

      tgsi_frontface[0] = nir_bcsel(&c->build,
                             frontface,
                             nir_imm_float(&c->build, 1.0),
                             nir_imm_float(&c->build, -1.0));
      tgsi_frontface[1] = nir_imm_float(&c->build, 0.0);
      tgsi_frontface[2] = nir_imm_float(&c->build, 0.0);
      tgsi_frontface[3] = nir_imm_float(&c->build, 1.0);
   }

   return nir_vec(&c->build, tgsi_frontface, 4);
}

static nir_src
ttn_src_for_file_and_index(struct ttn_compile *c, unsigned file, unsigned index,
                           struct tgsi_ind_register *indirect,
                           struct tgsi_dimension *dim,
                           struct tgsi_ind_register *dimind)
{
   nir_builder *b = &c->build;
   nir_src src;

   memset(&src, 0, sizeof(src));

   switch (file) {
   case TGSI_FILE_TEMPORARY:
      if (c->temp_regs[index].var) {
         unsigned offset = c->temp_regs[index].offset;
         nir_variable *var = c->temp_regs[index].var;
         nir_ssa_def *load = nir_load_deref(&c->build,
               ttn_array_deref(c, var, offset, indirect));

         src = nir_src_for_ssa(load);
      } else {
         assert(!indirect);
         src.reg.reg = c->temp_regs[index].reg;
      }
      assert(!dim);
      break;

   case TGSI_FILE_ADDRESS:
      src.reg.reg = c->addr_reg;
      assert(!dim);
      break;

   case TGSI_FILE_IMMEDIATE:
      src = nir_src_for_ssa(c->imm_defs[index]);
      assert(!indirect);
      assert(!dim);
      break;

   case TGSI_FILE_SYSTEM_VALUE: {
      nir_intrinsic_op op;
      nir_ssa_def *load;

      assert(!indirect);
      assert(!dim);

      switch (c->scan->system_value_semantic_name[index]) {
      case TGSI_SEMANTIC_VERTEXID_NOBASE:
         op = nir_intrinsic_load_vertex_id_zero_base;
         load = nir_load_vertex_id_zero_base(b);
         break;
      case TGSI_SEMANTIC_VERTEXID:
         op = nir_intrinsic_load_vertex_id;
         load = nir_load_vertex_id(b);
         break;
      case TGSI_SEMANTIC_BASEVERTEX:
         op = nir_intrinsic_load_base_vertex;
         load = nir_load_base_vertex(b);
         break;
      case TGSI_SEMANTIC_INSTANCEID:
         op = nir_intrinsic_load_instance_id;
         load = nir_load_instance_id(b);
         break;
      case TGSI_SEMANTIC_FACE:
         assert(c->cap_face_is_sysval);
         op = nir_intrinsic_load_front_face;
         load = ttn_emulate_tgsi_front_face(c);
         break;
      case TGSI_SEMANTIC_POSITION:
         assert(c->cap_position_is_sysval);
         op = nir_intrinsic_load_frag_coord;
         load = nir_load_frag_coord(b);
         break;
      case TGSI_SEMANTIC_PCOORD:
         assert(c->cap_point_is_sysval);
         op = nir_intrinsic_load_point_coord;
         load = nir_load_point_coord(b);
         break;
      default:
         unreachable("bad system value");
      }

      src = nir_src_for_ssa(load);
      b->shader->info.system_values_read |=
         (1 << nir_system_value_from_intrinsic(op));

      break;
   }

   case TGSI_FILE_INPUT:
      if (c->scan->processor == PIPE_SHADER_FRAGMENT &&
          c->scan->input_semantic_name[index] == TGSI_SEMANTIC_FACE) {
         assert(!c->cap_face_is_sysval && c->input_var_face);
         return nir_src_for_ssa(ttn_emulate_tgsi_front_face(c));
      } else if (c->scan->processor == PIPE_SHADER_FRAGMENT &&
          c->scan->input_semantic_name[index] == TGSI_SEMANTIC_POSITION) {
         assert(!c->cap_position_is_sysval && c->input_var_position);
         return nir_src_for_ssa(nir_load_var(&c->build, c->input_var_position));
      } else if (c->scan->processor == PIPE_SHADER_FRAGMENT &&
          c->scan->input_semantic_name[index] == TGSI_SEMANTIC_PCOORD) {
         assert(!c->cap_point_is_sysval && c->input_var_point);
         return nir_src_for_ssa(nir_load_var(&c->build, c->input_var_point));
      } else {
         /* Indirection on input arrays isn't supported by TTN. */
         assert(!dim);
         nir_deref_instr *deref = nir_build_deref_var(&c->build,
                                                      c->inputs[index]);
         return nir_src_for_ssa(nir_load_deref(&c->build, deref));
      }
      break;

   case TGSI_FILE_CONSTANT: {
      nir_intrinsic_instr *load;
      nir_intrinsic_op op;
      unsigned srcn = 0;

      if (dim && (dim->Index > 0 || dim->Indirect)) {
         op = nir_intrinsic_load_ubo;
      } else {
         op = nir_intrinsic_load_uniform;
      }

      load = nir_intrinsic_instr_create(b->shader, op);

      load->num_components = 4;
      if (dim && (dim->Index > 0 || dim->Indirect)) {
         if (dimind) {
            load->src[srcn] =
               ttn_src_for_file_and_index(c, dimind->File, dimind->Index,
                                          NULL, NULL, NULL);
         } else {
            /* UBOs start at index 1 in TGSI: */
            load->src[srcn] =
               nir_src_for_ssa(nir_imm_int(b, dim->Index - 1));
         }
         srcn++;
      }

      nir_ssa_def *offset;
      if (op == nir_intrinsic_load_ubo) {
         /* UBO loads don't have a base offset. */
         offset = nir_imm_int(b, index);
         if (indirect) {
            offset = nir_iadd(b, offset, ttn_src_for_indirect(c, indirect));
         }
         /* UBO offsets are in bytes, but TGSI gives them to us in vec4's */
         offset = nir_ishl(b, offset, nir_imm_int(b, 4));
      } else {
         nir_intrinsic_set_base(load, index);
         if (indirect) {
            offset = ttn_src_for_indirect(c, indirect);
         } else {
            offset = nir_imm_int(b, 0);
         }
      }
      load->src[srcn++] = nir_src_for_ssa(offset);

      nir_ssa_dest_init(&load->instr, &load->dest, 4, 32, NULL);
      nir_builder_instr_insert(b, &load->instr);

      src = nir_src_for_ssa(&load->dest.ssa);
      break;
   }

   default:
      unreachable("bad src file");
   }


   return src;
}

static nir_ssa_def *
ttn_src_for_indirect(struct ttn_compile *c, struct tgsi_ind_register *indirect)
{
   nir_builder *b = &c->build;
   nir_alu_src src;
   memset(&src, 0, sizeof(src));
   for (int i = 0; i < 4; i++)
      src.swizzle[i] = indirect->Swizzle;
   src.src = ttn_src_for_file_and_index(c,
                                        indirect->File,
                                        indirect->Index,
                                        NULL, NULL, NULL);
   return nir_mov_alu(b, src, 1);
}

static nir_alu_dest
ttn_get_dest(struct ttn_compile *c, struct tgsi_full_dst_register *tgsi_fdst)
{
   struct tgsi_dst_register *tgsi_dst = &tgsi_fdst->Register;
   nir_alu_dest dest;
   unsigned index = tgsi_dst->Index;

   memset(&dest, 0, sizeof(dest));

   if (tgsi_dst->File == TGSI_FILE_TEMPORARY) {
      if (c->temp_regs[index].var) {
          nir_register *reg;

         /* this works, because TGSI will give us a base offset
          * (in case of indirect index) that points back into
          * the array.  Access can be direct or indirect, we
          * don't really care.  Just create a one-shot dst reg
          * that will get store_var'd back into the array var
          * at the end of ttn_emit_instruction()
          */
         reg = nir_local_reg_create(c->build.impl);
         reg->num_components = 4;
         dest.dest.reg.reg = reg;
         dest.dest.reg.base_offset = 0;
      } else {
         assert(!tgsi_dst->Indirect);
         dest.dest.reg.reg = c->temp_regs[index].reg;
         dest.dest.reg.base_offset = c->temp_regs[index].offset;
      }
   } else if (tgsi_dst->File == TGSI_FILE_OUTPUT) {
      dest.dest.reg.reg = c->output_regs[index].reg;
      dest.dest.reg.base_offset = c->output_regs[index].offset;
   } else if (tgsi_dst->File == TGSI_FILE_ADDRESS) {
      assert(index == 0);
      dest.dest.reg.reg = c->addr_reg;
   }

   dest.write_mask = tgsi_dst->WriteMask;
   dest.saturate = false;

   if (tgsi_dst->Indirect && (tgsi_dst->File != TGSI_FILE_TEMPORARY)) {
      nir_src *indirect = ralloc(c->build.shader, nir_src);
      *indirect = nir_src_for_ssa(ttn_src_for_indirect(c, &tgsi_fdst->Indirect));
      dest.dest.reg.indirect = indirect;
   }

   return dest;
}

static nir_variable *
ttn_get_var(struct ttn_compile *c, struct tgsi_full_dst_register *tgsi_fdst)
{
   struct tgsi_dst_register *tgsi_dst = &tgsi_fdst->Register;
   unsigned index = tgsi_dst->Index;

   if (tgsi_dst->File == TGSI_FILE_TEMPORARY) {
      /* we should not have an indirect when there is no var! */
      if (!c->temp_regs[index].var)
         assert(!tgsi_dst->Indirect);
      return c->temp_regs[index].var;
   }

   return NULL;
}

static nir_ssa_def *
ttn_get_src(struct ttn_compile *c, struct tgsi_full_src_register *tgsi_fsrc,
            int src_idx)
{
   nir_builder *b = &c->build;
   struct tgsi_src_register *tgsi_src = &tgsi_fsrc->Register;
   enum tgsi_opcode opcode = c->token->FullInstruction.Instruction.Opcode;
   unsigned tgsi_src_type = tgsi_opcode_infer_src_type(opcode, src_idx);
   bool src_is_float = !(tgsi_src_type == TGSI_TYPE_SIGNED ||
                         tgsi_src_type == TGSI_TYPE_UNSIGNED);
   nir_alu_src src;

   memset(&src, 0, sizeof(src));

   if (tgsi_src->File == TGSI_FILE_NULL) {
      return nir_imm_float(b, 0.0);
   } else if (tgsi_src->File == TGSI_FILE_SAMPLER) {
      /* Only the index of the sampler gets used in texturing, and it will
       * handle looking that up on its own instead of using the nir_alu_src.
       */
      assert(!tgsi_src->Indirect);
      return NULL;
   } else {
      struct tgsi_ind_register *ind = NULL;
      struct tgsi_dimension *dim = NULL;
      struct tgsi_ind_register *dimind = NULL;
      if (tgsi_src->Indirect)
         ind = &tgsi_fsrc->Indirect;
      if (tgsi_src->Dimension) {
         dim = &tgsi_fsrc->Dimension;
         if (dim->Indirect)
            dimind = &tgsi_fsrc->DimIndirect;
      }
      src.src = ttn_src_for_file_and_index(c,
                                           tgsi_src->File,
                                           tgsi_src->Index,
                                           ind, dim, dimind);
   }

   src.swizzle[0] = tgsi_src->SwizzleX;
   src.swizzle[1] = tgsi_src->SwizzleY;
   src.swizzle[2] = tgsi_src->SwizzleZ;
   src.swizzle[3] = tgsi_src->SwizzleW;

   nir_ssa_def *def = nir_mov_alu(b, src, 4);

   if (tgsi_src->Absolute) {
      if (src_is_float)
         def = nir_fabs(b, def);
      else
         def = nir_iabs(b, def);
   }

   if (tgsi_src->Negate) {
      if (src_is_float)
         def = nir_fneg(b, def);
      else
         def = nir_ineg(b, def);
   }

   return def;
}

static void
ttn_alu(nir_builder *b, nir_op op, nir_alu_dest dest, nir_ssa_def **src)
{
   unsigned num_srcs = nir_op_infos[op].num_inputs;
   nir_alu_instr *instr = nir_alu_instr_create(b->shader, op);
   unsigned i;

   for (i = 0; i < num_srcs; i++)
      instr->src[i].src = nir_src_for_ssa(src[i]);

   instr->dest = dest;
   nir_builder_instr_insert(b, &instr->instr);
}

static void
ttn_move_dest_masked(nir_builder *b, nir_alu_dest dest,
                     nir_ssa_def *def, unsigned write_mask)
{
   if (!(dest.write_mask & write_mask))
      return;

   nir_alu_instr *mov = nir_alu_instr_create(b->shader, nir_op_mov);
   mov->dest = dest;
   mov->dest.write_mask &= write_mask;
   mov->src[0].src = nir_src_for_ssa(def);
   for (unsigned i = def->num_components; i < 4; i++)
      mov->src[0].swizzle[i] = def->num_components - 1;
   nir_builder_instr_insert(b, &mov->instr);
}

static void
ttn_move_dest(nir_builder *b, nir_alu_dest dest, nir_ssa_def *def)
{
   ttn_move_dest_masked(b, dest, def, TGSI_WRITEMASK_XYZW);
}

static void
ttn_arl(nir_builder *b, nir_op op, nir_alu_dest dest, nir_ssa_def **src)
{
   ttn_move_dest(b, dest, nir_f2i32(b, nir_ffloor(b, src[0])));
}

/* EXP - Approximate Exponential Base 2
 *  dst.x = 2^{\lfloor src.x\rfloor}
 *  dst.y = src.x - \lfloor src.x\rfloor
 *  dst.z = 2^{src.x}
 *  dst.w = 1.0
 */
static void
ttn_exp(nir_builder *b, nir_op op, nir_alu_dest dest, nir_ssa_def **src)
{
   nir_ssa_def *srcx = ttn_channel(b, src[0], X);

   ttn_move_dest_masked(b, dest, nir_fexp2(b, nir_ffloor(b, srcx)),
                        TGSI_WRITEMASK_X);
   ttn_move_dest_masked(b, dest, nir_fsub(b, srcx, nir_ffloor(b, srcx)),
                        TGSI_WRITEMASK_Y);
   ttn_move_dest_masked(b, dest, nir_fexp2(b, srcx), TGSI_WRITEMASK_Z);
   ttn_move_dest_masked(b, dest, nir_imm_float(b, 1.0), TGSI_WRITEMASK_W);
}

/* LOG - Approximate Logarithm Base 2
 *  dst.x = \lfloor\log_2{|src.x|}\rfloor
 *  dst.y = \frac{|src.x|}{2^{\lfloor\log_2{|src.x|}\rfloor}}
 *  dst.z = \log_2{|src.x|}
 *  dst.w = 1.0
 */
static void
ttn_log(nir_builder *b, nir_op op, nir_alu_dest dest, nir_ssa_def **src)
{
   nir_ssa_def *abs_srcx = nir_fabs(b, ttn_channel(b, src[0], X));
   nir_ssa_def *log2 = nir_flog2(b, abs_srcx);

   ttn_move_dest_masked(b, dest, nir_ffloor(b, log2), TGSI_WRITEMASK_X);
   ttn_move_dest_masked(b, dest,
                        nir_fdiv(b, abs_srcx, nir_fexp2(b, nir_ffloor(b, log2))),
                        TGSI_WRITEMASK_Y);
   ttn_move_dest_masked(b, dest, nir_flog2(b, abs_srcx), TGSI_WRITEMASK_Z);
   ttn_move_dest_masked(b, dest, nir_imm_float(b, 1.0), TGSI_WRITEMASK_W);
}

/* DST - Distance Vector
 *   dst.x = 1.0
 *   dst.y = src0.y \times src1.y
 *   dst.z = src0.z
 *   dst.w = src1.w
 */
static void
ttn_dst(nir_builder *b, nir_op op, nir_alu_dest dest, nir_ssa_def **src)
{
   ttn_move_dest_masked(b, dest, nir_imm_float(b, 1.0), TGSI_WRITEMASK_X);
   ttn_move_dest_masked(b, dest, nir_fmul(b, src[0], src[1]), TGSI_WRITEMASK_Y);
   ttn_move_dest_masked(b, dest, nir_mov(b, src[0]), TGSI_WRITEMASK_Z);
   ttn_move_dest_masked(b, dest, nir_mov(b, src[1]), TGSI_WRITEMASK_W);
}

/* LIT - Light Coefficients
 *  dst.x = 1.0
 *  dst.y = max(src.x, 0.0)
 *  dst.z = (src.x > 0.0) ? max(src.y, 0.0)^{clamp(src.w, -128.0, 128.0))} : 0
 *  dst.w = 1.0
 */
static void
ttn_lit(nir_builder *b, nir_op op, nir_alu_dest dest, nir_ssa_def **src)
{
   ttn_move_dest_masked(b, dest, nir_imm_float(b, 1.0), TGSI_WRITEMASK_XW);

   ttn_move_dest_masked(b, dest, nir_fmax(b, ttn_channel(b, src[0], X),
                                          nir_imm_float(b, 0.0)), TGSI_WRITEMASK_Y);

   if (dest.write_mask & TGSI_WRITEMASK_Z) {
      nir_ssa_def *src0_y = ttn_channel(b, src[0], Y);
      nir_ssa_def *wclamp = nir_fmax(b, nir_fmin(b, ttn_channel(b, src[0], W),
                                                 nir_imm_float(b, 128.0)),
                                     nir_imm_float(b, -128.0));
      nir_ssa_def *pow = nir_fpow(b, nir_fmax(b, src0_y, nir_imm_float(b, 0.0)),
                                  wclamp);

      ttn_move_dest_masked(b, dest,
                           nir_bcsel(b,
                                     nir_flt(b,
                                             ttn_channel(b, src[0], X),
                                             nir_imm_float(b, 0.0)),
                                     nir_imm_float(b, 0.0),
                                     pow),
                           TGSI_WRITEMASK_Z);
   }
}

static void
ttn_sle(nir_builder *b, nir_op op, nir_alu_dest dest, nir_ssa_def **src)
{
   ttn_move_dest(b, dest, nir_sge(b, src[1], src[0]));
}

static void
ttn_sgt(nir_builder *b, nir_op op, nir_alu_dest dest, nir_ssa_def **src)
{
   ttn_move_dest(b, dest, nir_slt(b, src[1], src[0]));
}

static void
ttn_dp2(nir_builder *b, nir_op op, nir_alu_dest dest, nir_ssa_def **src)
{
   ttn_move_dest(b, dest, nir_fdot2(b, src[0], src[1]));
}

static void
ttn_dp3(nir_builder *b, nir_op op, nir_alu_dest dest, nir_ssa_def **src)
{
   ttn_move_dest(b, dest, nir_fdot3(b, src[0], src[1]));
}

static void
ttn_dp4(nir_builder *b, nir_op op, nir_alu_dest dest, nir_ssa_def **src)
{
   ttn_move_dest(b, dest, nir_fdot4(b, src[0], src[1]));
}

static void
ttn_umad(nir_builder *b, nir_op op, nir_alu_dest dest, nir_ssa_def **src)
{
   ttn_move_dest(b, dest, nir_iadd(b, nir_imul(b, src[0], src[1]), src[2]));
}

static void
ttn_arr(nir_builder *b, nir_op op, nir_alu_dest dest, nir_ssa_def **src)
{
   ttn_move_dest(b, dest, nir_f2i32(b, nir_fround_even(b, src[0])));
}

static void
ttn_cmp(nir_builder *b, nir_op op, nir_alu_dest dest, nir_ssa_def **src)
{
   ttn_move_dest(b, dest, nir_bcsel(b,
                                    nir_flt(b, src[0], nir_imm_float(b, 0.0)),
                                    src[1], src[2]));
}

static void
ttn_ucmp(nir_builder *b, nir_op op, nir_alu_dest dest, nir_ssa_def **src)
{
   ttn_move_dest(b, dest, nir_bcsel(b,
                                    nir_ine(b, src[0], nir_imm_int(b, 0)),
                                    src[1], src[2]));
}

static void
ttn_kill(nir_builder *b, nir_op op, nir_alu_dest dest, nir_ssa_def **src)
{
   nir_intrinsic_instr *discard =
      nir_intrinsic_instr_create(b->shader, nir_intrinsic_discard);
   nir_builder_instr_insert(b, &discard->instr);
   b->shader->info.fs.uses_discard = true;
}

static void
ttn_kill_if(nir_builder *b, nir_op op, nir_alu_dest dest, nir_ssa_def **src)
{
   nir_ssa_def *cmp = nir_bany(b, nir_flt(b, src[0], nir_imm_float(b, 0.0)));
   nir_intrinsic_instr *discard =
      nir_intrinsic_instr_create(b->shader, nir_intrinsic_discard_if);
   discard->src[0] = nir_src_for_ssa(cmp);
   nir_builder_instr_insert(b, &discard->instr);
   b->shader->info.fs.uses_discard = true;
}

static void
ttn_if(struct ttn_compile *c, nir_ssa_def *src, bool is_uint)
{
   nir_builder *b = &c->build;
   nir_ssa_def *src_x = ttn_channel(b, src, X);

   nir_if *if_stmt = nir_if_create(b->shader);
   if (is_uint) {
      /* equivalent to TGSI UIF, src is interpreted as integer */
      if_stmt->condition = nir_src_for_ssa(nir_ine(b, src_x, nir_imm_int(b, 0)));
   } else {
      /* equivalent to TGSI IF, src is interpreted as float */
      if_stmt->condition = nir_src_for_ssa(nir_fne(b, src_x, nir_imm_float(b, 0.0)));
   }
   nir_builder_cf_insert(b, &if_stmt->cf_node);

   c->if_stack[c->if_stack_pos] = nir_after_cf_node(&if_stmt->cf_node);
   c->if_stack_pos++;

   b->cursor = nir_after_cf_list(&if_stmt->then_list);

   c->if_stack[c->if_stack_pos] = nir_after_cf_list(&if_stmt->else_list);
   c->if_stack_pos++;
}

static void
ttn_else(struct ttn_compile *c)
{
   nir_builder *b = &c->build;

   b->cursor = c->if_stack[c->if_stack_pos - 1];
}

static void
ttn_endif(struct ttn_compile *c)
{
   nir_builder *b = &c->build;

   c->if_stack_pos -= 2;
   b->cursor = c->if_stack[c->if_stack_pos];
}

static void
ttn_bgnloop(struct ttn_compile *c)
{
   nir_builder *b = &c->build;

   nir_loop *loop = nir_loop_create(b->shader);
   nir_builder_cf_insert(b, &loop->cf_node);

   c->loop_stack[c->loop_stack_pos] = nir_after_cf_node(&loop->cf_node);
   c->loop_stack_pos++;

   b->cursor = nir_after_cf_list(&loop->body);
}

static void
ttn_cont(nir_builder *b)
{
   nir_jump_instr *instr = nir_jump_instr_create(b->shader, nir_jump_continue);
   nir_builder_instr_insert(b, &instr->instr);
}

static void
ttn_brk(nir_builder *b)
{
   nir_jump_instr *instr = nir_jump_instr_create(b->shader, nir_jump_break);
   nir_builder_instr_insert(b, &instr->instr);
}

static void
ttn_endloop(struct ttn_compile *c)
{
   nir_builder *b = &c->build;

   c->loop_stack_pos--;
   b->cursor = c->loop_stack[c->loop_stack_pos];
}

static void
setup_texture_info(nir_tex_instr *instr, unsigned texture)
{
   switch (texture) {
   case TGSI_TEXTURE_BUFFER:
      instr->sampler_dim = GLSL_SAMPLER_DIM_BUF;
      break;
   case TGSI_TEXTURE_1D:
      instr->sampler_dim = GLSL_SAMPLER_DIM_1D;
      break;
   case TGSI_TEXTURE_1D_ARRAY:
      instr->sampler_dim = GLSL_SAMPLER_DIM_1D;
      instr->is_array = true;
      break;
   case TGSI_TEXTURE_SHADOW1D:
      instr->sampler_dim = GLSL_SAMPLER_DIM_1D;
      instr->is_shadow = true;
      break;
   case TGSI_TEXTURE_SHADOW1D_ARRAY:
      instr->sampler_dim = GLSL_SAMPLER_DIM_1D;
      instr->is_shadow = true;
      instr->is_array = true;
      break;
   case TGSI_TEXTURE_2D:
      instr->sampler_dim = GLSL_SAMPLER_DIM_2D;
      break;
   case TGSI_TEXTURE_2D_ARRAY:
      instr->sampler_dim = GLSL_SAMPLER_DIM_2D;
      instr->is_array = true;
      break;
   case TGSI_TEXTURE_2D_MSAA:
      instr->sampler_dim = GLSL_SAMPLER_DIM_MS;
      break;
   case TGSI_TEXTURE_2D_ARRAY_MSAA:
      instr->sampler_dim = GLSL_SAMPLER_DIM_MS;
      instr->is_array = true;
      break;
   case TGSI_TEXTURE_SHADOW2D:
      instr->sampler_dim = GLSL_SAMPLER_DIM_2D;
      instr->is_shadow = true;
      break;
   case TGSI_TEXTURE_SHADOW2D_ARRAY:
      instr->sampler_dim = GLSL_SAMPLER_DIM_2D;
      instr->is_shadow = true;
      instr->is_array = true;
      break;
   case TGSI_TEXTURE_3D:
      instr->sampler_dim = GLSL_SAMPLER_DIM_3D;
      break;
   case TGSI_TEXTURE_CUBE:
      instr->sampler_dim = GLSL_SAMPLER_DIM_CUBE;
      break;
   case TGSI_TEXTURE_CUBE_ARRAY:
      instr->sampler_dim = GLSL_SAMPLER_DIM_CUBE;
      instr->is_array = true;
      break;
   case TGSI_TEXTURE_SHADOWCUBE:
      instr->sampler_dim = GLSL_SAMPLER_DIM_CUBE;
      instr->is_shadow = true;
      break;
   case TGSI_TEXTURE_SHADOWCUBE_ARRAY:
      instr->sampler_dim = GLSL_SAMPLER_DIM_CUBE;
      instr->is_shadow = true;
      instr->is_array = true;
      break;
   case TGSI_TEXTURE_RECT:
      instr->sampler_dim = GLSL_SAMPLER_DIM_RECT;
      break;
   case TGSI_TEXTURE_SHADOWRECT:
      instr->sampler_dim = GLSL_SAMPLER_DIM_RECT;
      instr->is_shadow = true;
      break;
   default:
      fprintf(stderr, "Unknown TGSI texture target %d\n", texture);
      abort();
   }
}

static enum glsl_base_type
base_type_for_alu_type(nir_alu_type type)
{
   type = nir_alu_type_get_base_type(type);

   switch (type) {
   case nir_type_float:
      return GLSL_TYPE_FLOAT;
   case nir_type_int:
      return GLSL_TYPE_INT;
   case nir_type_uint:
      return GLSL_TYPE_UINT;
   default:
      unreachable("invalid type");
   }
}

static nir_variable *
get_sampler_var(struct ttn_compile *c, int binding,
                enum glsl_sampler_dim dim,
                bool is_shadow,
                bool is_array,
                enum glsl_base_type base_type)
{
   nir_variable *var = c->samplers[binding];
   if (!var) {
      const struct glsl_type *type =
         glsl_sampler_type(dim, is_shadow, is_array, base_type);
      var = nir_variable_create(c->build.shader, nir_var_uniform, type,
                                "sampler");
      var->data.binding = binding;
      var->data.explicit_binding = true;
      c->samplers[binding] = var;
   }

   return var;
}

static void
ttn_tex(struct ttn_compile *c, nir_alu_dest dest, nir_ssa_def **src)
{
   nir_builder *b = &c->build;
   struct tgsi_full_instruction *tgsi_inst = &c->token->FullInstruction;
   nir_tex_instr *instr;
   nir_texop op;
   unsigned num_srcs, samp = 1, sview, i;

   switch (tgsi_inst->Instruction.Opcode) {
   case TGSI_OPCODE_TEX:
      op = nir_texop_tex;
      num_srcs = 1;
      break;
   case TGSI_OPCODE_TEX2:
      op = nir_texop_tex;
      num_srcs = 1;
      samp = 2;
      break;
   case TGSI_OPCODE_TXP:
      op = nir_texop_tex;
      num_srcs = 2;
      break;
   case TGSI_OPCODE_TXB:
      op = nir_texop_txb;
      num_srcs = 2;
      break;
   case TGSI_OPCODE_TXB2:
      op = nir_texop_txb;
      num_srcs = 2;
      samp = 2;
      break;
   case TGSI_OPCODE_TXL:
      op = nir_texop_txl;
      num_srcs = 2;
      break;
   case TGSI_OPCODE_TXL2:
      op = nir_texop_txl;
      num_srcs = 2;
      samp = 2;
      break;
   case TGSI_OPCODE_TXF:
      if (tgsi_inst->Texture.Texture == TGSI_TEXTURE_2D_MSAA ||
          tgsi_inst->Texture.Texture == TGSI_TEXTURE_2D_ARRAY_MSAA) {
         op = nir_texop_txf_ms;
      } else {
         op = nir_texop_txf;
      }
      num_srcs = 2;
      break;
   case TGSI_OPCODE_TXD:
      op = nir_texop_txd;
      num_srcs = 3;
      samp = 3;
      break;
   case TGSI_OPCODE_LODQ:
      op = nir_texop_lod;
      num_srcs = 1;
      break;

   default:
      fprintf(stderr, "unknown TGSI tex op %d\n", tgsi_inst->Instruction.Opcode);
      abort();
   }

   if (tgsi_inst->Texture.Texture == TGSI_TEXTURE_SHADOW1D ||
       tgsi_inst->Texture.Texture == TGSI_TEXTURE_SHADOW1D_ARRAY ||
       tgsi_inst->Texture.Texture == TGSI_TEXTURE_SHADOW2D ||
       tgsi_inst->Texture.Texture == TGSI_TEXTURE_SHADOW2D_ARRAY ||
       tgsi_inst->Texture.Texture == TGSI_TEXTURE_SHADOWRECT ||
       tgsi_inst->Texture.Texture == TGSI_TEXTURE_SHADOWCUBE ||
       tgsi_inst->Texture.Texture == TGSI_TEXTURE_SHADOWCUBE_ARRAY) {
      num_srcs++;
   }

   /* Deref sources */
   num_srcs += 2;

   num_srcs += tgsi_inst->Texture.NumOffsets;

   instr = nir_tex_instr_create(b->shader, num_srcs);
   instr->op = op;

   setup_texture_info(instr, tgsi_inst->Texture.Texture);

   switch (instr->sampler_dim) {
   case GLSL_SAMPLER_DIM_1D:
   case GLSL_SAMPLER_DIM_BUF:
      instr->coord_components = 1;
      break;
   case GLSL_SAMPLER_DIM_2D:
   case GLSL_SAMPLER_DIM_RECT:
   case GLSL_SAMPLER_DIM_EXTERNAL:
   case GLSL_SAMPLER_DIM_MS:
      instr->coord_components = 2;
      break;
   case GLSL_SAMPLER_DIM_3D:
   case GLSL_SAMPLER_DIM_CUBE:
      instr->coord_components = 3;
      break;
   case GLSL_SAMPLER_DIM_SUBPASS:
   case GLSL_SAMPLER_DIM_SUBPASS_MS:
      unreachable("invalid sampler_dim");
   }

   if (instr->is_array)
      instr->coord_components++;

   assert(tgsi_inst->Src[samp].Register.File == TGSI_FILE_SAMPLER);

   /* TODO if we supported any opc's which take an explicit SVIEW
    * src, we would use that here instead.  But for the "legacy"
    * texture opc's the SVIEW index is same as SAMP index:
    */
   sview = tgsi_inst->Src[samp].Register.Index;

   if (op == nir_texop_lod) {
      instr->dest_type = nir_type_float;
   } else if (sview < c->num_samp_types) {
      instr->dest_type = c->samp_types[sview];
   } else {
      instr->dest_type = nir_type_float;
   }

   nir_variable *var =
      get_sampler_var(c, sview, instr->sampler_dim,
                      instr->is_shadow,
                      instr->is_array,
                      base_type_for_alu_type(instr->dest_type));

   nir_deref_instr *deref = nir_build_deref_var(b, var);

   unsigned src_number = 0;

   instr->src[src_number].src = nir_src_for_ssa(&deref->dest.ssa);
   instr->src[src_number].src_type = nir_tex_src_texture_deref;
   src_number++;
   instr->src[src_number].src = nir_src_for_ssa(&deref->dest.ssa);
   instr->src[src_number].src_type = nir_tex_src_sampler_deref;
   src_number++;

   instr->src[src_number].src =
      nir_src_for_ssa(nir_swizzle(b, src[0], SWIZ(X, Y, Z, W),
                                  instr->coord_components));
   instr->src[src_number].src_type = nir_tex_src_coord;
   src_number++;

   if (tgsi_inst->Instruction.Opcode == TGSI_OPCODE_TXP) {
      instr->src[src_number].src = nir_src_for_ssa(ttn_channel(b, src[0], W));
      instr->src[src_number].src_type = nir_tex_src_projector;
      src_number++;
   }

   if (tgsi_inst->Instruction.Opcode == TGSI_OPCODE_TXB) {
      instr->src[src_number].src = nir_src_for_ssa(ttn_channel(b, src[0], W));
      instr->src[src_number].src_type = nir_tex_src_bias;
      src_number++;
   }

   if (tgsi_inst->Instruction.Opcode == TGSI_OPCODE_TXB2) {
      instr->src[src_number].src = nir_src_for_ssa(ttn_channel(b, src[1], X));
      instr->src[src_number].src_type = nir_tex_src_bias;
      src_number++;
   }

   if (tgsi_inst->Instruction.Opcode == TGSI_OPCODE_TXL) {
      instr->src[src_number].src = nir_src_for_ssa(ttn_channel(b, src[0], W));
      instr->src[src_number].src_type = nir_tex_src_lod;
      src_number++;
   }

   if (tgsi_inst->Instruction.Opcode == TGSI_OPCODE_TXL2) {
      instr->src[src_number].src = nir_src_for_ssa(ttn_channel(b, src[1], X));
      instr->src[src_number].src_type = nir_tex_src_lod;
      src_number++;
   }

   if (tgsi_inst->Instruction.Opcode == TGSI_OPCODE_TXF) {
      instr->src[src_number].src = nir_src_for_ssa(ttn_channel(b, src[0], W));
      if (op == nir_texop_txf_ms)
         instr->src[src_number].src_type = nir_tex_src_ms_index;
      else
         instr->src[src_number].src_type = nir_tex_src_lod;
      src_number++;
   }

   if (tgsi_inst->Instruction.Opcode == TGSI_OPCODE_TXD) {
      instr->src[src_number].src_type = nir_tex_src_ddx;
      instr->src[src_number].src =
         nir_src_for_ssa(nir_swizzle(b, src[1], SWIZ(X, Y, Z, W),
				     nir_tex_instr_src_size(instr, src_number)));
      src_number++;
      instr->src[src_number].src_type = nir_tex_src_ddy;
      instr->src[src_number].src =
         nir_src_for_ssa(nir_swizzle(b, src[2], SWIZ(X, Y, Z, W),
				     nir_tex_instr_src_size(instr, src_number)));
      src_number++;
   }

   if (instr->is_shadow) {
      if (instr->coord_components == 4)
         instr->src[src_number].src = nir_src_for_ssa(ttn_channel(b, src[1], X));
      else if (instr->coord_components == 3)
         instr->src[src_number].src = nir_src_for_ssa(ttn_channel(b, src[0], W));
      else
         instr->src[src_number].src = nir_src_for_ssa(ttn_channel(b, src[0], Z));

      instr->src[src_number].src_type = nir_tex_src_comparator;
      src_number++;
   }

   for (i = 0; i < tgsi_inst->Texture.NumOffsets; i++) {
      struct tgsi_texture_offset *tex_offset = &tgsi_inst->TexOffsets[i];
      /* since TexOffset ins't using tgsi_full_src_register we get to
       * do some extra gymnastics:
       */
      nir_alu_src src;

      memset(&src, 0, sizeof(src));

      src.src = ttn_src_for_file_and_index(c,
                                           tex_offset->File,
                                           tex_offset->Index,
                                           NULL, NULL, NULL);

      src.swizzle[0] = tex_offset->SwizzleX;
      src.swizzle[1] = tex_offset->SwizzleY;
      src.swizzle[2] = tex_offset->SwizzleZ;
      src.swizzle[3] = TGSI_SWIZZLE_W;

      instr->src[src_number].src_type = nir_tex_src_offset;
      instr->src[src_number].src = nir_src_for_ssa(
         nir_mov_alu(b, src, nir_tex_instr_src_size(instr, src_number)));
      src_number++;
   }

   assert(src_number == num_srcs);
   assert(src_number == instr->num_srcs);

   nir_ssa_dest_init(&instr->instr, &instr->dest,
		     nir_tex_instr_dest_size(instr),
		     32, NULL);
   nir_builder_instr_insert(b, &instr->instr);

   /* Resolve the writemask on the texture op. */
   ttn_move_dest(b, dest, &instr->dest.ssa);
}

/* TGSI_OPCODE_TXQ is actually two distinct operations:
 *
 *     dst.x = texture\_width(unit, lod)
 *     dst.y = texture\_height(unit, lod)
 *     dst.z = texture\_depth(unit, lod)
 *     dst.w = texture\_levels(unit)
 *
 * dst.xyz map to NIR txs opcode, and dst.w maps to query_levels
 */
static void
ttn_txq(struct ttn_compile *c, nir_alu_dest dest, nir_ssa_def **src)
{
   nir_builder *b = &c->build;
   struct tgsi_full_instruction *tgsi_inst = &c->token->FullInstruction;
   nir_tex_instr *txs, *qlv;

   txs = nir_tex_instr_create(b->shader, 2);
   txs->op = nir_texop_txs;
   setup_texture_info(txs, tgsi_inst->Texture.Texture);

   qlv = nir_tex_instr_create(b->shader, 1);
   qlv->op = nir_texop_query_levels;
   setup_texture_info(qlv, tgsi_inst->Texture.Texture);

   assert(tgsi_inst->Src[1].Register.File == TGSI_FILE_SAMPLER);
   int tex_index = tgsi_inst->Src[1].Register.Index;

   nir_variable *var =
      get_sampler_var(c, tex_index, txs->sampler_dim,
                      txs->is_shadow,
                      txs->is_array,
                      base_type_for_alu_type(txs->dest_type));

   nir_deref_instr *deref = nir_build_deref_var(b, var);

   txs->src[0].src = nir_src_for_ssa(&deref->dest.ssa);
   txs->src[0].src_type = nir_tex_src_texture_deref;

   qlv->src[0].src = nir_src_for_ssa(&deref->dest.ssa);
   qlv->src[0].src_type = nir_tex_src_texture_deref;

   /* lod: */
   txs->src[1].src = nir_src_for_ssa(ttn_channel(b, src[0], X));
   txs->src[1].src_type = nir_tex_src_lod;

   nir_ssa_dest_init(&txs->instr, &txs->dest,
		     nir_tex_instr_dest_size(txs), 32, NULL);
   nir_builder_instr_insert(b, &txs->instr);

   nir_ssa_dest_init(&qlv->instr, &qlv->dest, 1, 32, NULL);
   nir_builder_instr_insert(b, &qlv->instr);

   ttn_move_dest_masked(b, dest, &txs->dest.ssa, TGSI_WRITEMASK_XYZ);
   ttn_move_dest_masked(b, dest, &qlv->dest.ssa, TGSI_WRITEMASK_W);
}

static const nir_op op_trans[TGSI_OPCODE_LAST] = {
   [TGSI_OPCODE_ARL] = 0,
   [TGSI_OPCODE_MOV] = nir_op_mov,
   [TGSI_OPCODE_LIT] = 0,
   [TGSI_OPCODE_RCP] = nir_op_frcp,
   [TGSI_OPCODE_RSQ] = nir_op_frsq,
   [TGSI_OPCODE_EXP] = 0,
   [TGSI_OPCODE_LOG] = 0,
   [TGSI_OPCODE_MUL] = nir_op_fmul,
   [TGSI_OPCODE_ADD] = nir_op_fadd,
   [TGSI_OPCODE_DP3] = 0,
   [TGSI_OPCODE_DP4] = 0,
   [TGSI_OPCODE_DST] = 0,
   [TGSI_OPCODE_MIN] = nir_op_fmin,
   [TGSI_OPCODE_MAX] = nir_op_fmax,
   [TGSI_OPCODE_SLT] = nir_op_slt,
   [TGSI_OPCODE_SGE] = nir_op_sge,
   [TGSI_OPCODE_MAD] = nir_op_ffma,
   [TGSI_OPCODE_LRP] = 0,
   [TGSI_OPCODE_SQRT] = nir_op_fsqrt,
   [TGSI_OPCODE_FRC] = nir_op_ffract,
   [TGSI_OPCODE_FLR] = nir_op_ffloor,
   [TGSI_OPCODE_ROUND] = nir_op_fround_even,
   [TGSI_OPCODE_EX2] = nir_op_fexp2,
   [TGSI_OPCODE_LG2] = nir_op_flog2,
   [TGSI_OPCODE_POW] = nir_op_fpow,
   [TGSI_OPCODE_COS] = nir_op_fcos,
   [TGSI_OPCODE_DDX] = nir_op_fddx,
   [TGSI_OPCODE_DDY] = nir_op_fddy,
   [TGSI_OPCODE_KILL] = 0,
   [TGSI_OPCODE_PK2H] = 0, /* XXX */
   [TGSI_OPCODE_PK2US] = 0, /* XXX */
   [TGSI_OPCODE_PK4B] = 0, /* XXX */
   [TGSI_OPCODE_PK4UB] = 0, /* XXX */
   [TGSI_OPCODE_SEQ] = nir_op_seq,
   [TGSI_OPCODE_SGT] = 0,
   [TGSI_OPCODE_SIN] = nir_op_fsin,
   [TGSI_OPCODE_SNE] = nir_op_sne,
   [TGSI_OPCODE_SLE] = 0,
   [TGSI_OPCODE_TEX] = 0,
   [TGSI_OPCODE_TXD] = 0,
   [TGSI_OPCODE_TXP] = 0,
   [TGSI_OPCODE_UP2H] = 0, /* XXX */
   [TGSI_OPCODE_UP2US] = 0, /* XXX */
   [TGSI_OPCODE_UP4B] = 0, /* XXX */
   [TGSI_OPCODE_UP4UB] = 0, /* XXX */
   [TGSI_OPCODE_ARR] = 0,

   /* No function calls, yet. */
   [TGSI_OPCODE_CAL] = 0, /* XXX */
   [TGSI_OPCODE_RET] = 0, /* XXX */

   [TGSI_OPCODE_SSG] = nir_op_fsign,
   [TGSI_OPCODE_CMP] = 0,
   [TGSI_OPCODE_TXB] = 0,
   [TGSI_OPCODE_DIV] = nir_op_fdiv,
   [TGSI_OPCODE_DP2] = 0,
   [TGSI_OPCODE_TXL] = 0,

   [TGSI_OPCODE_BRK] = 0,
   [TGSI_OPCODE_IF] = 0,
   [TGSI_OPCODE_UIF] = 0,
   [TGSI_OPCODE_ELSE] = 0,
   [TGSI_OPCODE_ENDIF] = 0,

   [TGSI_OPCODE_DDX_FINE] = nir_op_fddx_fine,
   [TGSI_OPCODE_DDY_FINE] = nir_op_fddy_fine,

   [TGSI_OPCODE_CEIL] = nir_op_fceil,
   [TGSI_OPCODE_I2F] = nir_op_i2f32,
   [TGSI_OPCODE_NOT] = nir_op_inot,
   [TGSI_OPCODE_TRUNC] = nir_op_ftrunc,
   [TGSI_OPCODE_SHL] = nir_op_ishl,
   [TGSI_OPCODE_AND] = nir_op_iand,
   [TGSI_OPCODE_OR] = nir_op_ior,
   [TGSI_OPCODE_MOD] = nir_op_umod,
   [TGSI_OPCODE_XOR] = nir_op_ixor,
   [TGSI_OPCODE_TXF] = 0,
   [TGSI_OPCODE_TXQ] = 0,

   [TGSI_OPCODE_CONT] = 0,

   [TGSI_OPCODE_EMIT] = 0, /* XXX */
   [TGSI_OPCODE_ENDPRIM] = 0, /* XXX */

   [TGSI_OPCODE_BGNLOOP] = 0,
   [TGSI_OPCODE_BGNSUB] = 0, /* XXX: no function calls */
   [TGSI_OPCODE_ENDLOOP] = 0,
   [TGSI_OPCODE_ENDSUB] = 0, /* XXX: no function calls */

   [TGSI_OPCODE_NOP] = 0,
   [TGSI_OPCODE_FSEQ] = nir_op_feq32,
   [TGSI_OPCODE_FSGE] = nir_op_fge32,
   [TGSI_OPCODE_FSLT] = nir_op_flt32,
   [TGSI_OPCODE_FSNE] = nir_op_fne32,

   [TGSI_OPCODE_KILL_IF] = 0,

   [TGSI_OPCODE_END] = 0,

   [TGSI_OPCODE_F2I] = nir_op_f2i32,
   [TGSI_OPCODE_IDIV] = nir_op_idiv,
   [TGSI_OPCODE_IMAX] = nir_op_imax,
   [TGSI_OPCODE_IMIN] = nir_op_imin,
   [TGSI_OPCODE_INEG] = nir_op_ineg,
   [TGSI_OPCODE_ISGE] = nir_op_ige32,
   [TGSI_OPCODE_ISHR] = nir_op_ishr,
   [TGSI_OPCODE_ISLT] = nir_op_ilt32,
   [TGSI_OPCODE_F2U] = nir_op_f2u32,
   [TGSI_OPCODE_U2F] = nir_op_u2f32,
   [TGSI_OPCODE_UADD] = nir_op_iadd,
   [TGSI_OPCODE_UDIV] = nir_op_udiv,
   [TGSI_OPCODE_UMAD] = 0,
   [TGSI_OPCODE_UMAX] = nir_op_umax,
   [TGSI_OPCODE_UMIN] = nir_op_umin,
   [TGSI_OPCODE_UMOD] = nir_op_umod,
   [TGSI_OPCODE_UMUL] = nir_op_imul,
   [TGSI_OPCODE_USEQ] = nir_op_ieq32,
   [TGSI_OPCODE_USGE] = nir_op_uge32,
   [TGSI_OPCODE_USHR] = nir_op_ushr,
   [TGSI_OPCODE_USLT] = nir_op_ult32,
   [TGSI_OPCODE_USNE] = nir_op_ine32,

   [TGSI_OPCODE_SWITCH] = 0, /* not emitted by glsl_to_tgsi.cpp */
   [TGSI_OPCODE_CASE] = 0, /* not emitted by glsl_to_tgsi.cpp */
   [TGSI_OPCODE_DEFAULT] = 0, /* not emitted by glsl_to_tgsi.cpp */
   [TGSI_OPCODE_ENDSWITCH] = 0, /* not emitted by glsl_to_tgsi.cpp */

   /* XXX: SAMPLE opcodes */

   [TGSI_OPCODE_UARL] = nir_op_mov,
   [TGSI_OPCODE_UCMP] = 0,
   [TGSI_OPCODE_IABS] = nir_op_iabs,
   [TGSI_OPCODE_ISSG] = nir_op_isign,

   /* XXX: atomics */

   [TGSI_OPCODE_TEX2] = 0,
   [TGSI_OPCODE_TXB2] = 0,
   [TGSI_OPCODE_TXL2] = 0,

   [TGSI_OPCODE_IMUL_HI] = nir_op_imul_high,
   [TGSI_OPCODE_UMUL_HI] = nir_op_umul_high,

   [TGSI_OPCODE_TG4] = 0,
   [TGSI_OPCODE_LODQ] = 0,

   [TGSI_OPCODE_IBFE] = nir_op_ibitfield_extract,
   [TGSI_OPCODE_UBFE] = nir_op_ubitfield_extract,
   [TGSI_OPCODE_BFI] = nir_op_bitfield_insert,
   [TGSI_OPCODE_BREV] = nir_op_bitfield_reverse,
   [TGSI_OPCODE_POPC] = nir_op_bit_count,
   [TGSI_OPCODE_LSB] = nir_op_find_lsb,
   [TGSI_OPCODE_IMSB] = nir_op_ifind_msb,
   [TGSI_OPCODE_UMSB] = nir_op_ufind_msb,

   [TGSI_OPCODE_INTERP_CENTROID] = 0, /* XXX */
   [TGSI_OPCODE_INTERP_SAMPLE] = 0, /* XXX */
   [TGSI_OPCODE_INTERP_OFFSET] = 0, /* XXX */
};

static void
ttn_emit_instruction(struct ttn_compile *c)
{
   nir_builder *b = &c->build;
   struct tgsi_full_instruction *tgsi_inst = &c->token->FullInstruction;
   unsigned i;
   unsigned tgsi_op = tgsi_inst->Instruction.Opcode;
   struct tgsi_full_dst_register *tgsi_dst = &tgsi_inst->Dst[0];

   if (tgsi_op == TGSI_OPCODE_END)
      return;

   nir_ssa_def *src[TGSI_FULL_MAX_SRC_REGISTERS];
   for (i = 0; i < tgsi_inst->Instruction.NumSrcRegs; i++) {
      src[i] = ttn_get_src(c, &tgsi_inst->Src[i], i);
   }
   nir_alu_dest dest = ttn_get_dest(c, tgsi_dst);

   switch (tgsi_op) {
   case TGSI_OPCODE_RSQ:
      ttn_move_dest(b, dest, nir_frsq(b, ttn_channel(b, src[0], X)));
      break;

   case TGSI_OPCODE_SQRT:
      ttn_move_dest(b, dest, nir_fsqrt(b, ttn_channel(b, src[0], X)));
      break;

   case TGSI_OPCODE_RCP:
      ttn_move_dest(b, dest, nir_frcp(b, ttn_channel(b, src[0], X)));
      break;

   case TGSI_OPCODE_EX2:
      ttn_move_dest(b, dest, nir_fexp2(b, ttn_channel(b, src[0], X)));
      break;

   case TGSI_OPCODE_LG2:
      ttn_move_dest(b, dest, nir_flog2(b, ttn_channel(b, src[0], X)));
      break;

   case TGSI_OPCODE_POW:
      ttn_move_dest(b, dest, nir_fpow(b,
                                      ttn_channel(b, src[0], X),
                                      ttn_channel(b, src[1], X)));
      break;

   case TGSI_OPCODE_COS:
      ttn_move_dest(b, dest, nir_fcos(b, ttn_channel(b, src[0], X)));
      break;

   case TGSI_OPCODE_SIN:
      ttn_move_dest(b, dest, nir_fsin(b, ttn_channel(b, src[0], X)));
      break;

   case TGSI_OPCODE_ARL:
      ttn_arl(b, op_trans[tgsi_op], dest, src);
      break;

   case TGSI_OPCODE_EXP:
      ttn_exp(b, op_trans[tgsi_op], dest, src);
      break;

   case TGSI_OPCODE_LOG:
      ttn_log(b, op_trans[tgsi_op], dest, src);
      break;

   case TGSI_OPCODE_DST:
      ttn_dst(b, op_trans[tgsi_op], dest, src);
      break;

   case TGSI_OPCODE_LIT:
      ttn_lit(b, op_trans[tgsi_op], dest, src);
      break;

   case TGSI_OPCODE_DP2:
      ttn_dp2(b, op_trans[tgsi_op], dest, src);
      break;

   case TGSI_OPCODE_DP3:
      ttn_dp3(b, op_trans[tgsi_op], dest, src);
      break;

   case TGSI_OPCODE_DP4:
      ttn_dp4(b, op_trans[tgsi_op], dest, src);
      break;

   case TGSI_OPCODE_UMAD:
      ttn_umad(b, op_trans[tgsi_op], dest, src);
      break;

   case TGSI_OPCODE_LRP:
      ttn_move_dest(b, dest, nir_flrp(b, src[2], src[1], src[0]));
      break;

   case TGSI_OPCODE_KILL:
      ttn_kill(b, op_trans[tgsi_op], dest, src);
      break;

   case TGSI_OPCODE_ARR:
      ttn_arr(b, op_trans[tgsi_op], dest, src);
      break;

   case TGSI_OPCODE_CMP:
      ttn_cmp(b, op_trans[tgsi_op], dest, src);
      break;

   case TGSI_OPCODE_UCMP:
      ttn_ucmp(b, op_trans[tgsi_op], dest, src);
      break;

   case TGSI_OPCODE_SGT:
      ttn_sgt(b, op_trans[tgsi_op], dest, src);
      break;

   case TGSI_OPCODE_SLE:
      ttn_sle(b, op_trans[tgsi_op], dest, src);
      break;

   case TGSI_OPCODE_KILL_IF:
      ttn_kill_if(b, op_trans[tgsi_op], dest, src);
      break;

   case TGSI_OPCODE_TEX:
   case TGSI_OPCODE_TXP:
   case TGSI_OPCODE_TXL:
   case TGSI_OPCODE_TXB:
   case TGSI_OPCODE_TXD:
   case TGSI_OPCODE_TEX2:
   case TGSI_OPCODE_TXL2:
   case TGSI_OPCODE_TXB2:
   case TGSI_OPCODE_TXF:
   case TGSI_OPCODE_TG4:
   case TGSI_OPCODE_LODQ:
      ttn_tex(c, dest, src);
      break;

   case TGSI_OPCODE_TXQ:
      ttn_txq(c, dest, src);
      break;

   case TGSI_OPCODE_NOP:
      break;

   case TGSI_OPCODE_IF:
      ttn_if(c, src[0], false);
      break;

   case TGSI_OPCODE_UIF:
      ttn_if(c, src[0], true);
      break;

   case TGSI_OPCODE_ELSE:
      ttn_else(c);
      break;

   case TGSI_OPCODE_ENDIF:
      ttn_endif(c);
      break;

   case TGSI_OPCODE_BGNLOOP:
      ttn_bgnloop(c);
      break;

   case TGSI_OPCODE_BRK:
      ttn_brk(b);
      break;

   case TGSI_OPCODE_CONT:
      ttn_cont(b);
      break;

   case TGSI_OPCODE_ENDLOOP:
      ttn_endloop(c);
      break;

   default:
      if (op_trans[tgsi_op] != 0 || tgsi_op == TGSI_OPCODE_MOV) {
         ttn_alu(b, op_trans[tgsi_op], dest, src);
      } else {
         fprintf(stderr, "unknown TGSI opcode: %s\n",
                 tgsi_get_opcode_name(tgsi_op));
         abort();
      }
      break;
   }

   if (tgsi_inst->Instruction.Saturate) {
      assert(!dest.dest.is_ssa);
      ttn_move_dest(b, dest, nir_fsat(b, ttn_src_for_dest(b, &dest)));
   }

   /* if the dst has a matching var, append store_var to move
    * output from reg to var
    */
   nir_variable *var = ttn_get_var(c, tgsi_dst);
   if (var) {
      unsigned index = tgsi_dst->Register.Index;
      unsigned offset = c->temp_regs[index].offset;
      struct tgsi_ind_register *indirect = tgsi_dst->Register.Indirect ?
                                           &tgsi_dst->Indirect : NULL;
      nir_src val = nir_src_for_reg(dest.dest.reg.reg);
      nir_store_deref(b, ttn_array_deref(c, var, offset, indirect),
                      nir_ssa_for_src(b, val, 4), dest.write_mask);
   }
}

/**
 * Puts a NIR intrinsic to store of each TGSI_FILE_OUTPUT value to the output
 * variables at the end of the shader.
 *
 * We don't generate these incrementally as the TGSI_FILE_OUTPUT values are
 * written, because there's no output load intrinsic, which means we couldn't
 * handle writemasks.
 */
static void
ttn_add_output_stores(struct ttn_compile *c)
{
   nir_builder *b = &c->build;

   for (int i = 0; i < c->build.shader->num_outputs; i++) {
      nir_variable *var = c->outputs[i];
      if (!var)
         continue;

      nir_src src = nir_src_for_reg(c->output_regs[i].reg);
      src.reg.base_offset = c->output_regs[i].offset;

      nir_ssa_def *store_value = nir_ssa_for_src(b, src, 4);
      if (c->build.shader->info.stage == MESA_SHADER_FRAGMENT &&
          var->data.location == FRAG_RESULT_DEPTH) {
         /* TGSI uses TGSI_SEMANTIC_POSITION.z for the depth output, while
          * NIR uses a single float FRAG_RESULT_DEPTH.
          */
         store_value = nir_channel(b, store_value, 2);
      }

      nir_store_deref(b, nir_build_deref_var(b, var), store_value,
                      (1 << store_value->num_components) - 1);
   }
}

/**
 * Parses the given TGSI tokens.
 */
static void
ttn_parse_tgsi(struct ttn_compile *c, const void *tgsi_tokens)
{
   struct tgsi_parse_context parser;
   int ret;

   ret = tgsi_parse_init(&parser, tgsi_tokens);
   assert(ret == TGSI_PARSE_OK);

   while (!tgsi_parse_end_of_tokens(&parser)) {
      tgsi_parse_token(&parser);
      c->token = &parser.FullToken;

      switch (parser.FullToken.Token.Type) {
      case TGSI_TOKEN_TYPE_DECLARATION:
         ttn_emit_declaration(c);
         break;

      case TGSI_TOKEN_TYPE_INSTRUCTION:
         ttn_emit_instruction(c);
         break;

      case TGSI_TOKEN_TYPE_IMMEDIATE:
         ttn_emit_immediate(c);
         break;
      }
   }

   tgsi_parse_free(&parser);
}

static void
ttn_read_pipe_caps(struct ttn_compile *c,
                   struct pipe_screen *screen)
{
   c->cap_scalar = screen->get_shader_param(screen, c->scan->processor, PIPE_SHADER_CAP_SCALAR_ISA);
   c->cap_packed_uniforms = screen->get_param(screen, PIPE_CAP_PACKED_UNIFORMS);
   c->cap_samplers_as_deref = screen->get_param(screen, PIPE_CAP_NIR_SAMPLERS_AS_DEREF);
   c->cap_face_is_sysval = screen->get_param(screen, PIPE_CAP_TGSI_FS_FACE_IS_INTEGER_SYSVAL);
   c->cap_position_is_sysval = screen->get_param(screen, PIPE_CAP_TGSI_FS_POSITION_IS_SYSVAL);
   c->cap_point_is_sysval = screen->get_param(screen, PIPE_CAP_TGSI_FS_POINT_IS_SYSVAL);
}

/**
 * Initializes a TGSI-to-NIR compiler.
 */
static struct ttn_compile *
ttn_compile_init(const void *tgsi_tokens,
                 const nir_shader_compiler_options *options,
                 struct pipe_screen *screen)
{
   struct ttn_compile *c;
   struct nir_shader *s;
   struct tgsi_shader_info scan;

   assert(options || screen);
   c = rzalloc(NULL, struct ttn_compile);

   tgsi_scan_shader(tgsi_tokens, &scan);
   c->scan = &scan;

   if (!options) {
      options =
         screen->get_compiler_options(screen, PIPE_SHADER_IR_NIR, scan.processor);
   }

   nir_builder_init_simple_shader(&c->build, NULL,
                                  tgsi_processor_to_shader_stage(scan.processor),
                                  options);

   s = c->build.shader;

   if (screen) {
      ttn_read_pipe_caps(c, screen);
   } else {
      /* TTN used to be hard coded to always make FACE a sysval,
       * so it makes sense to preserve that behavior so users don't break. */
      c->cap_face_is_sysval = true;
   }

   if (s->info.stage == MESA_SHADER_FRAGMENT)
      s->info.fs.untyped_color_outputs = true;

   s->num_inputs = scan.file_max[TGSI_FILE_INPUT] + 1;
   s->num_uniforms = scan.const_file_max[0] + 1;
   s->num_outputs = scan.file_max[TGSI_FILE_OUTPUT] + 1;

   s->info.vs.window_space_position = scan.properties[TGSI_PROPERTY_VS_WINDOW_SPACE_POSITION];

   c->inputs = rzalloc_array(c, struct nir_variable *, s->num_inputs);
   c->outputs = rzalloc_array(c, struct nir_variable *, s->num_outputs);

   c->output_regs = rzalloc_array(c, struct ttn_reg_info,
                                  scan.file_max[TGSI_FILE_OUTPUT] + 1);
   c->temp_regs = rzalloc_array(c, struct ttn_reg_info,
                                scan.file_max[TGSI_FILE_TEMPORARY] + 1);
   c->imm_defs = rzalloc_array(c, nir_ssa_def *,
                               scan.file_max[TGSI_FILE_IMMEDIATE] + 1);

   c->num_samp_types = scan.file_max[TGSI_FILE_SAMPLER_VIEW] + 1;
   c->samp_types = rzalloc_array(c, nir_alu_type, c->num_samp_types);

   c->if_stack = rzalloc_array(c, nir_cursor,
                               (scan.opcode_count[TGSI_OPCODE_IF] +
                                scan.opcode_count[TGSI_OPCODE_UIF]) * 2);
   c->loop_stack = rzalloc_array(c, nir_cursor,
                                 scan.opcode_count[TGSI_OPCODE_BGNLOOP]);


   ttn_parse_tgsi(c, tgsi_tokens);
   ttn_add_output_stores(c);

   nir_validate_shader(c->build.shader, "TTN: after parsing TGSI and creating the NIR shader");

   return c;
}

static void
ttn_optimize_nir(nir_shader *nir, bool scalar)
{
   bool progress;
   do {
      progress = false;

      NIR_PASS_V(nir, nir_lower_vars_to_ssa);

      if (scalar) {
         NIR_PASS_V(nir, nir_lower_alu_to_scalar, NULL);
         NIR_PASS_V(nir, nir_lower_phis_to_scalar);
      }

      NIR_PASS_V(nir, nir_lower_alu);
      NIR_PASS_V(nir, nir_lower_pack);
      NIR_PASS(progress, nir, nir_copy_prop);
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_opt_dce);

      if (nir_opt_trivial_continues(nir)) {
         progress = true;
         NIR_PASS(progress, nir, nir_copy_prop);
         NIR_PASS(progress, nir, nir_opt_dce);
      }

      NIR_PASS(progress, nir, nir_opt_if, false);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_peephole_select, 8, true, true);

      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_constant_folding);

      NIR_PASS(progress, nir, nir_opt_undef);
      NIR_PASS(progress, nir, nir_opt_conditional_discard);

      if (nir->options->max_unroll_iterations) {
         NIR_PASS(progress, nir, nir_opt_loop_unroll, (nir_variable_mode)0);
      }

   } while (progress);

}

/**
 * Finalizes the NIR in a similar way as st_glsl_to_nir does.
 *
 * Drivers expect that these passes are already performed,
 * so we have to do it here too.
 */
static void
ttn_finalize_nir(struct ttn_compile *c)
{
   struct nir_shader *nir = c->build.shader;

   NIR_PASS_V(nir, nir_lower_vars_to_ssa);
   NIR_PASS_V(nir, nir_lower_regs_to_ssa);

   NIR_PASS_V(nir, nir_lower_global_vars_to_local);
   NIR_PASS_V(nir, nir_split_var_copies);
   NIR_PASS_V(nir, nir_lower_var_copies);
   NIR_PASS_V(nir, nir_lower_system_values);

   if (c->cap_packed_uniforms)
      NIR_PASS_V(nir, nir_lower_uniforms_to_ubo, 16);

   if (c->cap_samplers_as_deref)
      NIR_PASS_V(nir, gl_nir_lower_samplers_as_deref, NULL);
   else
      NIR_PASS_V(nir, gl_nir_lower_samplers, NULL);

   ttn_optimize_nir(nir, c->cap_scalar);
   nir_shader_gather_info(nir, c->build.impl);
   nir_validate_shader(nir, "TTN: after all optimizations");
}

struct nir_shader *
tgsi_to_nir(const void *tgsi_tokens,
            struct pipe_screen *screen)
{
   struct ttn_compile *c;
   struct nir_shader *s;

   c = ttn_compile_init(tgsi_tokens, NULL, screen);
   s = c->build.shader;
   ttn_finalize_nir(c);
   ralloc_free(c);

   return s;
}

struct nir_shader *
tgsi_to_nir_noscreen(const void *tgsi_tokens,
                     const nir_shader_compiler_options *options)
{
   struct ttn_compile *c;
   struct nir_shader *s;

   c = ttn_compile_init(tgsi_tokens, options, NULL);
   s = c->build.shader;
   ralloc_free(c);

   return s;
}

