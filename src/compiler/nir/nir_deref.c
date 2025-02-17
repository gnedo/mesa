/*
 * Copyright © 2018 Intel Corporation
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

#include "nir.h"
#include "nir_builder.h"
#include "nir_deref.h"
#include "util/hash_table.h"

void
nir_deref_path_init(nir_deref_path *path,
                    nir_deref_instr *deref, void *mem_ctx)
{
   assert(deref != NULL);

   /* The length of the short path is at most ARRAY_SIZE - 1 because we need
    * room for the NULL terminator.
    */
   static const int max_short_path_len = ARRAY_SIZE(path->_short_path) - 1;

   int count = 0;

   nir_deref_instr **tail = &path->_short_path[max_short_path_len];
   nir_deref_instr **head = tail;

   *tail = NULL;
   for (nir_deref_instr *d = deref; d; d = nir_deref_instr_parent(d)) {
      count++;
      if (count <= max_short_path_len)
         *(--head) = d;
   }

   if (count <= max_short_path_len) {
      /* If we're under max_short_path_len, just use the short path. */
      path->path = head;
      goto done;
   }

#ifndef NDEBUG
   /* Just in case someone uses short_path by accident */
   for (unsigned i = 0; i < ARRAY_SIZE(path->_short_path); i++)
      path->_short_path[i] = (void *)0xdeadbeef;
#endif

   path->path = ralloc_array(mem_ctx, nir_deref_instr *, count + 1);
   head = tail = path->path + count;
   *tail = NULL;
   for (nir_deref_instr *d = deref; d; d = nir_deref_instr_parent(d))
      *(--head) = d;

done:
   assert(head == path->path);
   assert(tail == head + count);
   assert(*tail == NULL);
}

void
nir_deref_path_finish(nir_deref_path *path)
{
   if (path->path < &path->_short_path[0] ||
       path->path > &path->_short_path[ARRAY_SIZE(path->_short_path) - 1])
      ralloc_free(path->path);
}

/**
 * Recursively removes unused deref instructions
 */
bool
nir_deref_instr_remove_if_unused(nir_deref_instr *instr)
{
   bool progress = false;

   for (nir_deref_instr *d = instr; d; d = nir_deref_instr_parent(d)) {
      /* If anyone is using this deref, leave it alone */
      assert(d->dest.is_ssa);
      if (!list_empty(&d->dest.ssa.uses))
         break;

      nir_instr_remove(&d->instr);
      progress = true;
   }

   return progress;
}

bool
nir_deref_instr_has_indirect(nir_deref_instr *instr)
{
   while (instr->deref_type != nir_deref_type_var) {
      /* Consider casts to be indirects */
      if (instr->deref_type == nir_deref_type_cast)
         return true;

      if ((instr->deref_type == nir_deref_type_array ||
           instr->deref_type == nir_deref_type_ptr_as_array) &&
          !nir_src_is_const(instr->arr.index))
         return true;

      instr = nir_deref_instr_parent(instr);
   }

   return false;
}

bool
nir_deref_instr_is_known_out_of_bounds(nir_deref_instr *instr)
{
   for (; instr; instr = nir_deref_instr_parent(instr)) {
      if (instr->deref_type == nir_deref_type_array &&
          nir_src_is_const(instr->arr.index) &&
           nir_src_as_uint(instr->arr.index) >=
           glsl_get_length(nir_deref_instr_parent(instr)->type))
         return true;
   }

   return false;
}

bool
nir_deref_instr_has_complex_use(nir_deref_instr *deref)
{
   nir_foreach_use(use_src, &deref->dest.ssa) {
      nir_instr *use_instr = use_src->parent_instr;

      switch (use_instr->type) {
      case nir_instr_type_deref: {
         nir_deref_instr *use_deref = nir_instr_as_deref(use_instr);

         /* A var deref has no sources */
         assert(use_deref->deref_type != nir_deref_type_var);

         /* If a deref shows up in an array index or something like that, it's
          * a complex use.
          */
         if (use_src != &use_deref->parent)
            return true;

         /* Anything that isn't a basic struct or array deref is considered to
          * be a "complex" use.  In particular, we don't allow ptr_as_array
          * because we assume that opt_deref will turn any non-complex
          * ptr_as_array derefs into regular array derefs eventually so passes
          * which only want to handle simple derefs will pick them up in a
          * later pass.
          */
         if (use_deref->deref_type != nir_deref_type_struct &&
             use_deref->deref_type != nir_deref_type_array_wildcard &&
             use_deref->deref_type != nir_deref_type_array)
            return true;

         if (nir_deref_instr_has_complex_use(use_deref))
            return true;

         continue;
      }

      case nir_instr_type_intrinsic: {
         nir_intrinsic_instr *use_intrin = nir_instr_as_intrinsic(use_instr);
         switch (use_intrin->intrinsic) {
         case nir_intrinsic_load_deref:
            assert(use_src == &use_intrin->src[0]);
            continue;

         case nir_intrinsic_copy_deref:
            assert(use_src == &use_intrin->src[0] ||
                   use_src == &use_intrin->src[1]);
            continue;

         case nir_intrinsic_store_deref:
            /* A use in src[1] of a store means we're taking that pointer and
             * writing it to a variable.  Because we have no idea who will
             * read that variable and what they will do with the pointer, it's
             * considered a "complex" use.  A use in src[0], on the other
             * hand, is a simple use because we're just going to dereference
             * it and write a value there.
             */
            if (use_src == &use_intrin->src[0])
               continue;
            return true;

         default:
            return true;
         }
         unreachable("Switch default failed");
      }

      default:
         return true;
      }
   }

   nir_foreach_if_use(use, &deref->dest.ssa)
      return true;

   return false;
}

unsigned
nir_deref_instr_ptr_as_array_stride(nir_deref_instr *deref)
{
   assert(deref->deref_type == nir_deref_type_ptr_as_array);
   nir_deref_instr *parent = nir_deref_instr_parent(deref);
   switch (parent->deref_type) {
   case nir_deref_type_array:
      return glsl_get_explicit_stride(nir_deref_instr_parent(parent)->type);
   case nir_deref_type_ptr_as_array:
      return nir_deref_instr_ptr_as_array_stride(parent);
   case nir_deref_type_cast:
      return parent->cast.ptr_stride;
   default:
      unreachable("Invalid parent for ptr_as_array deref");
   }
}

static unsigned
type_get_array_stride(const struct glsl_type *elem_type,
                      glsl_type_size_align_func size_align)
{
   unsigned elem_size, elem_align;
   size_align(elem_type, &elem_size, &elem_align);
   return ALIGN_POT(elem_size, elem_align);
}

static unsigned
struct_type_get_field_offset(const struct glsl_type *struct_type,
                             glsl_type_size_align_func size_align,
                             unsigned field_idx)
{
   assert(glsl_type_is_struct_or_ifc(struct_type));
   unsigned offset = 0;
   for (unsigned i = 0; i <= field_idx; i++) {
      unsigned elem_size, elem_align;
      size_align(glsl_get_struct_field(struct_type, i), &elem_size, &elem_align);
      offset = ALIGN_POT(offset, elem_align);
      if (i < field_idx)
         offset += elem_size;
   }
   return offset;
}

unsigned
nir_deref_instr_get_const_offset(nir_deref_instr *deref,
                                 glsl_type_size_align_func size_align)
{
   nir_deref_path path;
   nir_deref_path_init(&path, deref, NULL);

   assert(path.path[0]->deref_type == nir_deref_type_var);

   unsigned offset = 0;
   for (nir_deref_instr **p = &path.path[1]; *p; p++) {
      if ((*p)->deref_type == nir_deref_type_array) {
         offset += nir_src_as_uint((*p)->arr.index) *
                   type_get_array_stride((*p)->type, size_align);
      } else if ((*p)->deref_type == nir_deref_type_struct) {
         /* p starts at path[1], so this is safe */
         nir_deref_instr *parent = *(p - 1);
         offset += struct_type_get_field_offset(parent->type, size_align,
                                                (*p)->strct.index);
      } else {
         unreachable("Unsupported deref type");
      }
   }

   nir_deref_path_finish(&path);

   return offset;
}

nir_ssa_def *
nir_build_deref_offset(nir_builder *b, nir_deref_instr *deref,
                       glsl_type_size_align_func size_align)
{
   nir_deref_path path;
   nir_deref_path_init(&path, deref, NULL);

   assert(path.path[0]->deref_type == nir_deref_type_var);

   nir_ssa_def *offset = nir_imm_int(b, 0);
   for (nir_deref_instr **p = &path.path[1]; *p; p++) {
      if ((*p)->deref_type == nir_deref_type_array) {
         nir_ssa_def *index = nir_ssa_for_src(b, (*p)->arr.index, 1);
         int stride = type_get_array_stride((*p)->type, size_align);
         offset = nir_iadd(b, offset, nir_imul_imm(b, index, stride));
      } else if ((*p)->deref_type == nir_deref_type_struct) {
         /* p starts at path[1], so this is safe */
         nir_deref_instr *parent = *(p - 1);
         unsigned field_offset =
            struct_type_get_field_offset(parent->type, size_align,
                                         (*p)->strct.index);
         offset = nir_iadd_imm(b, offset, field_offset);
      } else {
         unreachable("Unsupported deref type");
      }
   }

   nir_deref_path_finish(&path);

   return offset;
}

bool
nir_remove_dead_derefs_impl(nir_function_impl *impl)
{
   bool progress = false;

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type == nir_instr_type_deref &&
             nir_deref_instr_remove_if_unused(nir_instr_as_deref(instr)))
            progress = true;
      }
   }

   if (progress)
      nir_metadata_preserve(impl, nir_metadata_block_index |
                                  nir_metadata_dominance);

   return progress;
}

bool
nir_remove_dead_derefs(nir_shader *shader)
{
   bool progress = false;
   nir_foreach_function(function, shader) {
      if (function->impl && nir_remove_dead_derefs_impl(function->impl))
         progress = true;
   }

   return progress;
}

void
nir_fixup_deref_modes(nir_shader *shader)
{
   nir_foreach_function(function, shader) {
      if (!function->impl)
         continue;

      nir_foreach_block(block, function->impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_deref)
               continue;

            nir_deref_instr *deref = nir_instr_as_deref(instr);
            if (deref->deref_type == nir_deref_type_cast)
               continue;

            nir_variable_mode parent_mode;
            if (deref->deref_type == nir_deref_type_var) {
               parent_mode = deref->var->data.mode;
            } else {
               assert(deref->parent.is_ssa);
               nir_deref_instr *parent =
                  nir_instr_as_deref(deref->parent.ssa->parent_instr);
               parent_mode = parent->mode;
            }

            deref->mode = parent_mode;
         }
      }
   }
}

static bool
modes_may_alias(nir_variable_mode a, nir_variable_mode b)
{
   /* Generic pointers can alias with SSBOs */
   if ((a == nir_var_mem_ssbo || a == nir_var_mem_global) &&
       (b == nir_var_mem_ssbo || b == nir_var_mem_global))
      return true;

   /* In the general case, pointers can only alias if they have the same mode.
    *
    * NOTE: In future, with things like OpenCL generic pointers, this may not
    * be true and will have to be re-evaluated.  However, with graphics only,
    * it should be safe.
    */
   return a == b;
}

static bool
deref_path_contains_coherent_decoration(nir_deref_path *path)
{
   assert(path->path[0]->deref_type == nir_deref_type_var);

   if (path->path[0]->var->data.image.access & ACCESS_COHERENT)
      return true;

   for (nir_deref_instr **p = &path->path[1]; *p; p++) {
      if ((*p)->deref_type != nir_deref_type_struct)
         continue;

      const struct glsl_type *struct_type = (*(p - 1))->type;
      const struct glsl_struct_field *field =
         glsl_get_struct_field_data(struct_type, (*p)->strct.index);
      if (field->memory_coherent)
         return true;
   }

   return false;
}

nir_deref_compare_result
nir_compare_deref_paths(nir_deref_path *a_path,
                        nir_deref_path *b_path)
{
   if (!modes_may_alias(b_path->path[0]->mode, a_path->path[0]->mode))
      return nir_derefs_do_not_alias;

   if (a_path->path[0]->deref_type != b_path->path[0]->deref_type)
      return nir_derefs_may_alias_bit;

   if (a_path->path[0]->deref_type == nir_deref_type_var) {
      if (a_path->path[0]->var != b_path->path[0]->var) {
         /* Shader and function temporaries aren't backed by memory so two
          * distinct variables never alias.
          */
         static const nir_variable_mode temp_var_modes =
            nir_var_shader_temp | nir_var_function_temp;
         if ((a_path->path[0]->mode & temp_var_modes) ||
             (b_path->path[0]->mode & temp_var_modes))
            return nir_derefs_do_not_alias;

         /* If they are both declared coherent or have coherent somewhere in
          * their path (due to a member of an interface being declared
          * coherent), we have to assume we that we could have any kind of
          * aliasing.  Otherwise, they could still alias but the client didn't
          * tell us and that's their fault.
          */
         if (deref_path_contains_coherent_decoration(a_path) &&
             deref_path_contains_coherent_decoration(b_path))
            return nir_derefs_may_alias_bit;

         /* If we can chase the deref all the way back to the variable and
          * they're not the same variable and at least one is not declared
          * coherent, we know they can't possibly alias.
          */
         return nir_derefs_do_not_alias;
      }
   } else {
      assert(a_path->path[0]->deref_type == nir_deref_type_cast);
      /* If they're not exactly the same cast, it's hard to compare them so we
       * just assume they alias.  Comparing casts is tricky as there are lots
       * of things such as mode, type, etc. to make sure work out; for now, we
       * just assume nit_opt_deref will combine them and compare the deref
       * instructions.
       *
       * TODO: At some point in the future, we could be clever and understand
       * that a float[] and int[] have the same layout and aliasing structure
       * but double[] and vec3[] do not and we could potentially be a bit
       * smarter here.
       */
      if (a_path->path[0] != b_path->path[0])
         return nir_derefs_may_alias_bit;
   }

   /* Start off assuming they fully compare.  We ignore equality for now.  In
    * the end, we'll determine that by containment.
    */
   nir_deref_compare_result result = nir_derefs_may_alias_bit |
                                     nir_derefs_a_contains_b_bit |
                                     nir_derefs_b_contains_a_bit;

   nir_deref_instr **a_p = &a_path->path[1];
   nir_deref_instr **b_p = &b_path->path[1];
   while (*a_p != NULL && *a_p == *b_p) {
      a_p++;
      b_p++;
   }

   /* We're at either the tail or the divergence point between the two deref
    * paths.  Look to see if either contains a ptr_as_array deref.  It it
    * does we don't know how to safely make any inferences.  Hopefully,
    * nir_opt_deref will clean most of these up and we can start inferring
    * things again.
    *
    * In theory, we could do a bit better.  For instance, we could detect the
    * case where we have exactly one ptr_as_array deref in the chain after the
    * divergence point and it's matched in both chains and the two chains have
    * different constant indices.
    */
   for (nir_deref_instr **t_p = a_p; *t_p; t_p++) {
      if ((*t_p)->deref_type == nir_deref_type_ptr_as_array)
         return nir_derefs_may_alias_bit;
   }
   for (nir_deref_instr **t_p = b_p; *t_p; t_p++) {
      if ((*t_p)->deref_type == nir_deref_type_ptr_as_array)
         return nir_derefs_may_alias_bit;
   }

   while (*a_p != NULL && *b_p != NULL) {
      nir_deref_instr *a_tail = *(a_p++);
      nir_deref_instr *b_tail = *(b_p++);

      switch (a_tail->deref_type) {
      case nir_deref_type_array:
      case nir_deref_type_array_wildcard: {
         assert(b_tail->deref_type == nir_deref_type_array ||
                b_tail->deref_type == nir_deref_type_array_wildcard);

         if (a_tail->deref_type == nir_deref_type_array_wildcard) {
            if (b_tail->deref_type != nir_deref_type_array_wildcard)
               result &= ~nir_derefs_b_contains_a_bit;
         } else if (b_tail->deref_type == nir_deref_type_array_wildcard) {
            if (a_tail->deref_type != nir_deref_type_array_wildcard)
               result &= ~nir_derefs_a_contains_b_bit;
         } else {
            assert(a_tail->deref_type == nir_deref_type_array &&
                   b_tail->deref_type == nir_deref_type_array);
            assert(a_tail->arr.index.is_ssa && b_tail->arr.index.is_ssa);

            if (nir_src_is_const(a_tail->arr.index) &&
                nir_src_is_const(b_tail->arr.index)) {
               /* If they're both direct and have different offsets, they
                * don't even alias much less anything else.
                */
               if (nir_src_as_uint(a_tail->arr.index) !=
                   nir_src_as_uint(b_tail->arr.index))
                  return nir_derefs_do_not_alias;
            } else if (a_tail->arr.index.ssa == b_tail->arr.index.ssa) {
               /* They're the same indirect, continue on */
            } else {
               /* They're not the same index so we can't prove anything about
                * containment.
                */
               result &= ~(nir_derefs_a_contains_b_bit | nir_derefs_b_contains_a_bit);
            }
         }
         break;
      }

      case nir_deref_type_struct: {
         /* If they're different struct members, they don't even alias */
         if (a_tail->strct.index != b_tail->strct.index)
            return nir_derefs_do_not_alias;
         break;
      }

      default:
         unreachable("Invalid deref type");
      }
   }

   /* If a is longer than b, then it can't contain b */
   if (*a_p != NULL)
      result &= ~nir_derefs_a_contains_b_bit;
   if (*b_p != NULL)
      result &= ~nir_derefs_b_contains_a_bit;

   /* If a contains b and b contains a they must be equal. */
   if ((result & nir_derefs_a_contains_b_bit) && (result & nir_derefs_b_contains_a_bit))
      result |= nir_derefs_equal_bit;

   return result;
}

nir_deref_compare_result
nir_compare_derefs(nir_deref_instr *a, nir_deref_instr *b)
{
   if (a == b) {
      return nir_derefs_equal_bit | nir_derefs_may_alias_bit |
             nir_derefs_a_contains_b_bit | nir_derefs_b_contains_a_bit;
   }

   nir_deref_path a_path, b_path;
   nir_deref_path_init(&a_path, a, NULL);
   nir_deref_path_init(&b_path, b, NULL);
   assert(a_path.path[0]->deref_type == nir_deref_type_var ||
          a_path.path[0]->deref_type == nir_deref_type_cast);
   assert(b_path.path[0]->deref_type == nir_deref_type_var ||
          b_path.path[0]->deref_type == nir_deref_type_cast);

   nir_deref_compare_result result = nir_compare_deref_paths(&a_path, &b_path);

   nir_deref_path_finish(&a_path);
   nir_deref_path_finish(&b_path);

   return result;
}

struct rematerialize_deref_state {
   bool progress;
   nir_builder builder;
   nir_block *block;
   struct hash_table *cache;
};

static nir_deref_instr *
rematerialize_deref_in_block(nir_deref_instr *deref,
                             struct rematerialize_deref_state *state)
{
   if (deref->instr.block == state->block)
      return deref;

   if (!state->cache) {
      state->cache = _mesa_pointer_hash_table_create(NULL);
   }

   struct hash_entry *cached = _mesa_hash_table_search(state->cache, deref);
   if (cached)
      return cached->data;

   nir_builder *b = &state->builder;
   nir_deref_instr *new_deref =
      nir_deref_instr_create(b->shader, deref->deref_type);
   new_deref->mode = deref->mode;
   new_deref->type = deref->type;

   if (deref->deref_type == nir_deref_type_var) {
      new_deref->var = deref->var;
   } else {
      nir_deref_instr *parent = nir_src_as_deref(deref->parent);
      if (parent) {
         parent = rematerialize_deref_in_block(parent, state);
         new_deref->parent = nir_src_for_ssa(&parent->dest.ssa);
      } else {
         nir_src_copy(&new_deref->parent, &deref->parent, new_deref);
      }
   }

   switch (deref->deref_type) {
   case nir_deref_type_var:
   case nir_deref_type_array_wildcard:
   case nir_deref_type_cast:
      /* Nothing more to do */
      break;

   case nir_deref_type_array:
      assert(!nir_src_as_deref(deref->arr.index));
      nir_src_copy(&new_deref->arr.index, &deref->arr.index, new_deref);
      break;

   case nir_deref_type_struct:
      new_deref->strct.index = deref->strct.index;
      break;

   default:
      unreachable("Invalid deref instruction type");
   }

   nir_ssa_dest_init(&new_deref->instr, &new_deref->dest,
                     deref->dest.ssa.num_components,
                     deref->dest.ssa.bit_size,
                     deref->dest.ssa.name);
   nir_builder_instr_insert(b, &new_deref->instr);

   return new_deref;
}

static bool
rematerialize_deref_src(nir_src *src, void *_state)
{
   struct rematerialize_deref_state *state = _state;

   nir_deref_instr *deref = nir_src_as_deref(*src);
   if (!deref)
      return true;

   nir_deref_instr *block_deref = rematerialize_deref_in_block(deref, state);
   if (block_deref != deref) {
      nir_instr_rewrite_src(src->parent_instr, src,
                            nir_src_for_ssa(&block_deref->dest.ssa));
      nir_deref_instr_remove_if_unused(deref);
      state->progress = true;
   }

   return true;
}

/** Re-materialize derefs in every block
 *
 * This pass re-materializes deref instructions in every block in which it is
 * used.  After this pass has been run, every use of a deref will be of a
 * deref in the same block as the use.  Also, all unused derefs will be
 * deleted as a side-effect.
 *
 * Derefs used as sources of phi instructions are not rematerialized.
 */
bool
nir_rematerialize_derefs_in_use_blocks_impl(nir_function_impl *impl)
{
   struct rematerialize_deref_state state = { 0 };
   nir_builder_init(&state.builder, impl);

   nir_foreach_block(block, impl) {
      state.block = block;

      /* Start each block with a fresh cache */
      if (state.cache)
         _mesa_hash_table_clear(state.cache, NULL);

      nir_foreach_instr_safe(instr, block) {
         if (instr->type == nir_instr_type_deref &&
             nir_deref_instr_remove_if_unused(nir_instr_as_deref(instr)))
            continue;

         /* If a deref is used in a phi, we can't rematerialize it, as the new
          * derefs would appear before the phi, which is not valid.
          */
         if (instr->type == nir_instr_type_phi)
            continue;

         state.builder.cursor = nir_before_instr(instr);
         nir_foreach_src(instr, rematerialize_deref_src, &state);
      }

#ifndef NDEBUG
      nir_if *following_if = nir_block_get_following_if(block);
      if (following_if)
         assert(!nir_src_as_deref(following_if->condition));
#endif
   }

   _mesa_hash_table_destroy(state.cache, NULL);

   return state.progress;
}

static bool
is_trivial_deref_cast(nir_deref_instr *cast)
{
   nir_deref_instr *parent = nir_src_as_deref(cast->parent);
   if (!parent)
      return false;

   return cast->mode == parent->mode &&
          cast->type == parent->type &&
          cast->dest.ssa.num_components == parent->dest.ssa.num_components &&
          cast->dest.ssa.bit_size == parent->dest.ssa.bit_size;
}

static bool
is_trivial_array_deref_cast(nir_deref_instr *cast)
{
   assert(is_trivial_deref_cast(cast));

   nir_deref_instr *parent = nir_src_as_deref(cast->parent);

   if (parent->deref_type == nir_deref_type_array) {
      return cast->cast.ptr_stride ==
             glsl_get_explicit_stride(nir_deref_instr_parent(parent)->type);
   } else if (parent->deref_type == nir_deref_type_ptr_as_array) {
      return cast->cast.ptr_stride ==
             nir_deref_instr_ptr_as_array_stride(parent);
   } else {
      return false;
   }
}

static bool
is_deref_ptr_as_array(nir_instr *instr)
{
   return instr->type == nir_instr_type_deref &&
          nir_instr_as_deref(instr)->deref_type == nir_deref_type_ptr_as_array;
}

/**
 * Remove casts that just wrap other casts.
 */
static bool
opt_remove_cast_cast(nir_deref_instr *cast)
{
   nir_deref_instr *first_cast = cast;

   while (true) {
      nir_deref_instr *parent = nir_deref_instr_parent(first_cast);
      if (parent == NULL || parent->deref_type != nir_deref_type_cast)
         break;
      first_cast = parent;
   }
   if (cast == first_cast)
      return false;

   nir_instr_rewrite_src(&cast->instr, &cast->parent,
                         nir_src_for_ssa(first_cast->parent.ssa));
   return true;
}

/**
 * Is this casting a struct to a contained struct.
 * struct a { struct b field0 };
 * ssa_5 is structa;
 * deref_cast (structb *)ssa_5 (function_temp structb);
 * converts to
 * deref_struct &ssa_5->field0 (function_temp structb);
 * This allows subsequent copy propagation to work.
 */
static bool
opt_replace_struct_wrapper_cast(nir_builder *b, nir_deref_instr *cast)
{
   nir_deref_instr *parent = nir_src_as_deref(cast->parent);
   if (!parent)
      return false;

   if (!glsl_type_is_struct(parent->type))
      return false;

   if (glsl_get_struct_field_offset(parent->type, 0) != 0)
      return false;

   if (cast->type != glsl_get_struct_field(parent->type, 0))
      return false;

   nir_deref_instr *replace = nir_build_deref_struct(b, parent, 0);
   nir_ssa_def_rewrite_uses(&cast->dest.ssa, nir_src_for_ssa(&replace->dest.ssa));
   nir_deref_instr_remove_if_unused(cast);
   return true;
}

static bool
opt_deref_cast(nir_builder *b, nir_deref_instr *cast)
{
   bool progress;

   if (opt_replace_struct_wrapper_cast(b, cast))
      return true;

   progress = opt_remove_cast_cast(cast);
   if (!is_trivial_deref_cast(cast))
      return progress;

   bool trivial_array_cast = is_trivial_array_deref_cast(cast);

   assert(cast->dest.is_ssa);
   assert(cast->parent.is_ssa);

   nir_foreach_use_safe(use_src, &cast->dest.ssa) {
      /* If this isn't a trivial array cast, we can't propagate into
       * ptr_as_array derefs.
       */
      if (is_deref_ptr_as_array(use_src->parent_instr) &&
          !trivial_array_cast)
         continue;

      nir_instr_rewrite_src(use_src->parent_instr, use_src, cast->parent);
      progress = true;
   }

   /* If uses would be a bit crazy */
   assert(list_empty(&cast->dest.ssa.if_uses));

   nir_deref_instr_remove_if_unused(cast);
   return progress;
}

static bool
opt_deref_ptr_as_array(nir_builder *b, nir_deref_instr *deref)
{
   assert(deref->deref_type == nir_deref_type_ptr_as_array);

   nir_deref_instr *parent = nir_deref_instr_parent(deref);

   if (nir_src_is_const(deref->arr.index) &&
       nir_src_as_int(deref->arr.index) == 0) {
      /* If it's a ptr_as_array deref with an index of 0, it does nothing
       * and we can just replace its uses with its parent.
       *
       * The source of a ptr_as_array deref always has a deref_type of
       * nir_deref_type_array or nir_deref_type_cast.  If it's a cast, it
       * may be trivial and we may be able to get rid of that too.  Any
       * trivial cast of trivial cast cases should be handled already by
       * opt_deref_cast() above.
       */
      if (parent->deref_type == nir_deref_type_cast &&
          is_trivial_deref_cast(parent))
         parent = nir_deref_instr_parent(parent);
      nir_ssa_def_rewrite_uses(&deref->dest.ssa,
                               nir_src_for_ssa(&parent->dest.ssa));
      nir_instr_remove(&deref->instr);
      return true;
   }

   if (parent->deref_type != nir_deref_type_array &&
       parent->deref_type != nir_deref_type_ptr_as_array)
      return false;

   assert(parent->parent.is_ssa);
   assert(parent->arr.index.is_ssa);
   assert(deref->arr.index.is_ssa);

   nir_ssa_def *new_idx = nir_iadd(b, parent->arr.index.ssa,
                                      deref->arr.index.ssa);

   deref->deref_type = parent->deref_type;
   nir_instr_rewrite_src(&deref->instr, &deref->parent, parent->parent);
   nir_instr_rewrite_src(&deref->instr, &deref->arr.index,
                         nir_src_for_ssa(new_idx));
   return true;
}

bool
nir_opt_deref_impl(nir_function_impl *impl)
{
   bool progress = false;

   nir_builder b;
   nir_builder_init(&b, impl);

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_deref)
            continue;

         b.cursor = nir_before_instr(instr);

         nir_deref_instr *deref = nir_instr_as_deref(instr);
         switch (deref->deref_type) {
         case nir_deref_type_ptr_as_array:
            if (opt_deref_ptr_as_array(&b, deref))
               progress = true;
            break;

         case nir_deref_type_cast:
            if (opt_deref_cast(&b, deref))
               progress = true;
            break;

         default:
            /* Do nothing */
            break;
         }
      }
   }

   if (progress) {
      nir_metadata_preserve(impl, nir_metadata_block_index |
                                  nir_metadata_dominance);
   } else {
#ifndef NDEBUG
      impl->valid_metadata &= ~nir_metadata_not_properly_reset;
#endif
   }

   return progress;
}

bool
nir_opt_deref(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_function(func, shader) {
      if (func->impl && nir_opt_deref_impl(func->impl))
         progress = true;
   }

   return progress;
}
