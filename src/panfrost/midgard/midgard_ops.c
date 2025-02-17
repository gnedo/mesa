/* Copyright (c) 2018-2019 Alyssa Rosenzweig (alyssa@rosenzweig.io)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "midgard.h"

/* Include the definitions of the macros and such */

#define MIDGARD_OPS_TABLE
#include "helpers.h"
#undef MIDGARD_OPS_TABLE

/* Table of mapping opcodes to accompanying properties. This is used for both
 * the disassembler and the compiler. It is placed in a .c file like this to
 * avoid duplications in the binary */

struct mir_op_props alu_opcode_props[256] = {
        [midgard_alu_op_fadd]		 = {"fadd", UNITS_ADD | OP_COMMUTES},
        [midgard_alu_op_fmul]		 = {"fmul", UNITS_MUL | UNIT_VLUT | OP_COMMUTES},
        [midgard_alu_op_fmin]		 = {"fmin", UNITS_MOST | OP_COMMUTES},
        [midgard_alu_op_fmax]		 = {"fmax", UNITS_MOST | OP_COMMUTES},
        [midgard_alu_op_imin]		 = {"imin", UNITS_MOST | OP_COMMUTES},
        [midgard_alu_op_imax]		 = {"imax", UNITS_MOST | OP_COMMUTES},
        [midgard_alu_op_umin]		 = {"umin", UNITS_MOST | OP_COMMUTES},
        [midgard_alu_op_umax]		 = {"umax", UNITS_MOST | OP_COMMUTES},
        [midgard_alu_op_ihadd]		 = {"ihadd", UNITS_ADD | OP_COMMUTES},
        [midgard_alu_op_uhadd]		 = {"uhadd", UNITS_ADD | OP_COMMUTES},
        [midgard_alu_op_irhadd]		 = {"irhadd", UNITS_ADD | OP_COMMUTES},
        [midgard_alu_op_urhadd]		 = {"urhadd", UNITS_ADD | OP_COMMUTES},

        [midgard_alu_op_fmov]		 = {"fmov", UNITS_ALL | QUIRK_FLIPPED_R24},
        [midgard_alu_op_fmov_rtz]	 = {"fmov_rtz", UNITS_ALL | QUIRK_FLIPPED_R24},
        [midgard_alu_op_fmov_rtn]	 = {"fmov_rtn", UNITS_ALL | QUIRK_FLIPPED_R24},
        [midgard_alu_op_fmov_rtp]	 = {"fmov_rtp", UNITS_ALL | QUIRK_FLIPPED_R24},
        [midgard_alu_op_fround]          = {"fround", UNITS_ADD},
        [midgard_alu_op_froundeven]      = {"froundeven", UNITS_ADD},
        [midgard_alu_op_ftrunc]          = {"ftrunc", UNITS_ADD},
        [midgard_alu_op_ffloor]		 = {"ffloor", UNITS_ADD},
        [midgard_alu_op_fceil]		 = {"fceil", UNITS_ADD},
        [midgard_alu_op_ffma]		 = {"ffma", UNIT_VLUT},

        /* Though they output a scalar, they need to run on a vector unit
         * since they process vectors */
        [midgard_alu_op_fdot3]		 = {"fdot3", UNIT_VMUL | OP_CHANNEL_COUNT(3) | OP_COMMUTES},
        [midgard_alu_op_fdot3r]		 = {"fdot3r", UNIT_VMUL | OP_CHANNEL_COUNT(3) | OP_COMMUTES},
        [midgard_alu_op_fdot4]		 = {"fdot4", UNIT_VMUL | OP_CHANNEL_COUNT(4) | OP_COMMUTES},

        /* Incredibly, iadd can run on vmul, etc */
        [midgard_alu_op_iadd]		 = {"iadd", UNITS_MOST | OP_COMMUTES},
        [midgard_alu_op_ishladd]         = {"ishladd", UNITS_MUL},
        [midgard_alu_op_iaddsat]	 = {"iaddsat", UNITS_ADD | OP_COMMUTES},
        [midgard_alu_op_uaddsat]	 = {"uaddsat", UNITS_ADD | OP_COMMUTES},
        [midgard_alu_op_iabsdiff]	 = {"iabsdiff", UNITS_ADD},
        [midgard_alu_op_uabsdiff]	 = {"uabsdiff", UNITS_ADD},
        [midgard_alu_op_ichoose]	 = {"ichoose", UNITS_ADD},
        [midgard_alu_op_isub]		 = {"isub", UNITS_MOST},
        [midgard_alu_op_isubsat]	 = {"isubsat", UNITS_MOST},
        [midgard_alu_op_usubsat]	 = {"usubsat", UNITS_MOST},
        [midgard_alu_op_imul]		 = {"imul", UNITS_MUL | OP_COMMUTES},
        [midgard_alu_op_imov]		 = {"imov", UNITS_ALL | QUIRK_FLIPPED_R24},

        /* For vector comparisons, use ball etc */
        [midgard_alu_op_feq]		 = {"feq", UNITS_MOST | OP_TYPE_CONVERT | OP_COMMUTES},
        [midgard_alu_op_fne]		 = {"fne", UNITS_MOST | OP_TYPE_CONVERT | OP_COMMUTES},
        [midgard_alu_op_fle]		 = {"fle", UNITS_MOST | OP_TYPE_CONVERT},
        [midgard_alu_op_flt]		 = {"flt", UNITS_MOST | OP_TYPE_CONVERT},
        [midgard_alu_op_ieq]		 = {"ieq", UNITS_MOST | OP_COMMUTES},
        [midgard_alu_op_ine]		 = {"ine", UNITS_MOST | OP_COMMUTES},
        [midgard_alu_op_ilt]		 = {"ilt", UNITS_MOST},
        [midgard_alu_op_ile]		 = {"ile", UNITS_MOST},
        [midgard_alu_op_ult]		 = {"ult", UNITS_MOST},
        [midgard_alu_op_ule]		 = {"ule", UNITS_MOST},

        /* csel must run in the second pipeline stage (r31 written in first) */
        [midgard_alu_op_icsel]		 = {"icsel", UNIT_VADD | UNIT_SMUL},
        [midgard_alu_op_icsel_v]         = {"icsel_v", UNIT_VADD | UNIT_SMUL}, /* Acts as bitselect() */
        [midgard_alu_op_fcsel_v]	 = {"fcsel_v", UNIT_VADD | UNIT_SMUL},
        [midgard_alu_op_fcsel]		 = {"fcsel", UNIT_VADD | UNIT_SMUL},

        [midgard_alu_op_frcp]		 = {"frcp", UNIT_VLUT},
        [midgard_alu_op_frsqrt]		 = {"frsqrt", UNIT_VLUT},
        [midgard_alu_op_fsqrt]		 = {"fsqrt", UNIT_VLUT},
        [midgard_alu_op_fpow_pt1]	 = {"fpow_pt1", UNIT_VLUT},
        [midgard_alu_op_fpown_pt1]	 = {"fpown_pt1", UNIT_VLUT},
        [midgard_alu_op_fpowr_pt1]	 = {"fpowr_pt1", UNIT_VLUT},
        [midgard_alu_op_fexp2]		 = {"fexp2", UNIT_VLUT},
        [midgard_alu_op_flog2]		 = {"flog2", UNIT_VLUT},

        [midgard_alu_op_f2i_rte]	 = {"f2i_rte", UNITS_ADD | OP_TYPE_CONVERT},
        [midgard_alu_op_f2i_rtz]	 = {"f2i_rtz", UNITS_ADD | OP_TYPE_CONVERT},
        [midgard_alu_op_f2i_rtn]	 = {"f2i_rtn", UNITS_ADD | OP_TYPE_CONVERT},
        [midgard_alu_op_f2i_rtp]	 = {"f2i_rtp", UNITS_ADD | OP_TYPE_CONVERT},
        [midgard_alu_op_f2u_rte]	 = {"f2i_rte", UNITS_ADD | OP_TYPE_CONVERT},
        [midgard_alu_op_f2u_rtz]	 = {"f2i_rtz", UNITS_ADD | OP_TYPE_CONVERT},
        [midgard_alu_op_f2u_rtn]	 = {"f2i_rtn", UNITS_ADD | OP_TYPE_CONVERT},
        [midgard_alu_op_f2u_rtp]	 = {"f2i_rtp", UNITS_ADD | OP_TYPE_CONVERT},
        [midgard_alu_op_i2f_rte]	 = {"i2f", UNITS_ADD | OP_TYPE_CONVERT},
        [midgard_alu_op_i2f_rtz]	 = {"i2f_rtz", UNITS_ADD | OP_TYPE_CONVERT},
        [midgard_alu_op_i2f_rtn]	 = {"i2f_rtn", UNITS_ADD | OP_TYPE_CONVERT},
        [midgard_alu_op_i2f_rtp]	 = {"i2f_rtp", UNITS_ADD | OP_TYPE_CONVERT},
        [midgard_alu_op_u2f_rte]	 = {"u2f", UNITS_ADD | OP_TYPE_CONVERT},
        [midgard_alu_op_u2f_rtz]	 = {"u2f_rtz", UNITS_ADD | OP_TYPE_CONVERT},
        [midgard_alu_op_u2f_rtn]	 = {"u2f_rtn", UNITS_ADD | OP_TYPE_CONVERT},
        [midgard_alu_op_u2f_rtp]	 = {"u2f_rtp", UNITS_ADD | OP_TYPE_CONVERT},

        [midgard_alu_op_fsin]		 = {"fsin", UNIT_VLUT},
        [midgard_alu_op_fcos]		 = {"fcos", UNIT_VLUT},

        [midgard_alu_op_iand]		 = {"iand", UNITS_MOST | OP_COMMUTES},
        [midgard_alu_op_iandnot]         = {"iandnot", UNITS_MOST},

        [midgard_alu_op_ior]		 = {"ior", UNITS_MOST | OP_COMMUTES},
        [midgard_alu_op_iornot]		 = {"iornot", UNITS_MOST | OP_COMMUTES},
        [midgard_alu_op_inor]		 = {"inor", UNITS_MOST | OP_COMMUTES},
        [midgard_alu_op_ixor]		 = {"ixor", UNITS_MOST | OP_COMMUTES},
        [midgard_alu_op_inxor]		 = {"inxor", UNITS_MOST | OP_COMMUTES},
        [midgard_alu_op_iclz]		 = {"iclz", UNITS_ADD},
        [midgard_alu_op_ibitcount8]	 = {"ibitcount8", UNITS_ADD},
        [midgard_alu_op_inand]		 = {"inand", UNITS_MOST},
        [midgard_alu_op_ishl]		 = {"ishl", UNITS_ADD},
        [midgard_alu_op_iasr]		 = {"iasr", UNITS_ADD},
        [midgard_alu_op_ilsr]		 = {"ilsr", UNITS_ADD},

        [midgard_alu_op_fball_eq]	 = {"fball_eq",  UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES | OP_TYPE_CONVERT},
        [midgard_alu_op_fball_neq]	 = {"fball_neq", UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES | OP_TYPE_CONVERT},
        [midgard_alu_op_fball_lt]	 = {"fball_lt",  UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES | OP_TYPE_CONVERT},
        [midgard_alu_op_fball_lte]	 = {"fball_lte", UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES | OP_TYPE_CONVERT},

        [midgard_alu_op_fbany_eq]	 = {"fbany_eq",  UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES | OP_TYPE_CONVERT},
        [midgard_alu_op_fbany_neq]	 = {"fbany_neq", UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES | OP_TYPE_CONVERT},
        [midgard_alu_op_fbany_lt]	 = {"fbany_lt",  UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES | OP_TYPE_CONVERT},
        [midgard_alu_op_fbany_lte]	 = {"fbany_lte", UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES | OP_TYPE_CONVERT},

        [midgard_alu_op_iball_eq]	 = {"iball_eq",  UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES},
        [midgard_alu_op_iball_neq]	 = {"iball_neq", UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES},
        [midgard_alu_op_iball_lt]	 = {"iball_lt",  UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES},
        [midgard_alu_op_iball_lte]	 = {"iball_lte", UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES},
        [midgard_alu_op_uball_lt]	 = {"uball_lt",  UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES},
        [midgard_alu_op_uball_lte]	 = {"uball_lte", UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES},

        [midgard_alu_op_ibany_eq]	 = {"ibany_eq",  UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES},
        [midgard_alu_op_ibany_neq]	 = {"ibany_neq", UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES},
        [midgard_alu_op_ibany_lt]	 = {"ibany_lt",  UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES},
        [midgard_alu_op_ibany_lte]	 = {"ibany_lte", UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES},
        [midgard_alu_op_ubany_lt]	 = {"ubany_lt",  UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES},
        [midgard_alu_op_ubany_lte]	 = {"ubany_lte", UNITS_VECTOR | OP_CHANNEL_COUNT(4) | OP_COMMUTES},

        [midgard_alu_op_fatan2_pt1]     = {"fatan2_pt1", UNIT_VLUT},
        [midgard_alu_op_fatan_pt2]      = {"fatan_pt2", UNIT_VLUT},

        /* Haven't seen in a while */
        [midgard_alu_op_freduce]        = {"freduce", 0},
};

const char *load_store_opcode_names[256] = {
        [midgard_op_st_cubemap_coords] = "st_cubemap_coords",
        [midgard_op_ld_global_id] = "ld_global_id",
        [midgard_op_ldst_perspective_division_z] = "ldst_perspective_division_z",
        [midgard_op_ldst_perspective_division_w] = "ldst_perspective_division_w",

        [midgard_op_atomic_add] = "atomic_add",
        [midgard_op_atomic_and] = "atomic_and",
        [midgard_op_atomic_or] = "atomic_or",
        [midgard_op_atomic_xor] = "atomic_xor",
        [midgard_op_atomic_imin] = "atomic_imin",
        [midgard_op_atomic_umin] = "atomic_umin",
        [midgard_op_atomic_imax] = "atomic_imax",
        [midgard_op_atomic_umax] = "atomic_umax",
        [midgard_op_atomic_xchg] = "atomic_xchg",

        [midgard_op_ld_char] = "ld_char",
        [midgard_op_ld_char2] = "ld_char2",
        [midgard_op_ld_short] = "ld_short",
        [midgard_op_ld_char4] = "ld_char4",
        [midgard_op_ld_short4] = "ld_short4",
        [midgard_op_ld_int4] = "ld_int4",

        [midgard_op_ld_attr_32] = "ld_attr_32",
        [midgard_op_ld_attr_16] = "ld_attr_16",
        [midgard_op_ld_attr_32i] = "ld_attr_32i",
        [midgard_op_ld_attr_32u] = "ld_attr_32u",

        [midgard_op_ld_vary_32] = "ld_vary_32",
        [midgard_op_ld_vary_16] = "ld_vary_16",
        [midgard_op_ld_vary_32i] = "ld_vary_32i",
        [midgard_op_ld_vary_32u] = "ld_vary_32u",

        [midgard_op_ld_color_buffer_16] = "ld_color_buffer_16",

        [midgard_op_ld_uniform_16] = "ld_uniform_16",
        [midgard_op_ld_uniform_32] = "ld_uniform_32",
        [midgard_op_ld_uniform_32i] = "ld_uniform_32i",
        [midgard_op_ld_color_buffer_8] = "ld_color_buffer_8",

        [midgard_op_st_char] = "st_char",
        [midgard_op_st_char2] = "st_char2",
        [midgard_op_st_char4] = "st_char4",
        [midgard_op_st_short4] = "st_short4",
        [midgard_op_st_int4] = "st_int4",

        [midgard_op_st_vary_32] = "st_vary_32",
        [midgard_op_st_vary_16] = "st_vary_16",
        [midgard_op_st_vary_32i] = "st_vary_32i",
        [midgard_op_st_vary_32u] = "st_vary_32u",

        [midgard_op_st_image_f] = "st_image_f",
        [midgard_op_st_image_ui] = "st_image_ui",
        [midgard_op_st_image_i] = "st_image_i",
};
