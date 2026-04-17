/*
 * Copyright © 2015 Rob Clark <robclark@freedesktop.org>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "util/u_call_once.h"
#include "util/ralloc.h"

#include "freedreno_dev_info.h"

#include "ir3_compiler.h"
#include "ir3_nir.h"

static const struct debug_named_value shader_debug_options[] = {
   {"vs",         IR3_DBG_SHADER_VS,  "Print shader disasm for vertex shaders"},
   {"tcs",        IR3_DBG_SHADER_TCS, "Print shader disasm for tess ctrl shaders"},
   {"tes",        IR3_DBG_SHADER_TES, "Print shader disasm for tess eval shaders"},
   {"gs",         IR3_DBG_SHADER_GS,  "Print shader disasm for geometry shaders"},
   {"fs",         IR3_DBG_SHADER_FS,  "Print shader disasm for fragment shaders"},
   {"cs",         IR3_DBG_SHADER_CS,  "Print shader disasm for compute shaders"},
   {"internal",   IR3_DBG_SHADER_INTERNAL, "Print shader disasm for internal shaders"},
   {"disasm",     IR3_DBG_DISASM,     "Dump NIR and adreno shader disassembly"},
   {"optmsgs",    IR3_DBG_OPTMSGS,    "Enable optimizer debug messages"},
   {"forces2en",  IR3_DBG_FORCES2EN,  "Force s2en mode for tex sampler instructions"},
   {"nouboopt",   IR3_DBG_NOUBOOPT,   "Disable lowering UBO to uniform"},
   {"nofp16",     IR3_DBG_NOFP16,     "Don't lower mediump to fp16"},
   {"nocache",    IR3_DBG_NOCACHE,    "Disable shader cache"},
   {"spillall",   IR3_DBG_SPILLALL,   "Spill as much as possible to test the spiller"},
   {"nopreamble", IR3_DBG_NOPREAMBLE, "Disable the preamble pass"},
   {"fullsync",   IR3_DBG_FULLSYNC,   "Add (sy) + (ss) after each cat5/cat6"},
   {"fullnop",    IR3_DBG_FULLNOP,    "Add nops before each instruction"},
   {"noearlypreamble", IR3_DBG_NOEARLYPREAMBLE, "Disable early preambles"},
   {"nodescprefetch", IR3_DBG_NODESCPREFETCH, "Disable descriptor prefetch optimization"},
   {"expandrpt",  IR3_DBG_EXPANDRPT,  "Expand rptN instructions"},
   {"noaliastex", IR3_DBG_NOALIASTEX, "Don't use alias.tex"},
   {"noaliasrt",  IR3_DBG_NOALIASRT,  "Don't use alias.rt"},
   {"asmroundtrip", IR3_DBG_ASM_ROUNDTRIP, "Disassemble, reassemble and compare every shader"},
#if MESA_DEBUG
   {"schedmsgs",  IR3_DBG_SCHEDMSGS,  "Enable scheduler debug messages"},
   {"ramsgs",     IR3_DBG_RAMSGS,     "Enable register-allocation debug messages"},
#endif
   DEBUG_NAMED_VALUE_END
};

DEBUG_GET_ONCE_FLAGS_OPTION(ir3_shader_debug, "IR3_SHADER_DEBUG",
                            shader_debug_options, 0)
DEBUG_GET_ONCE_OPTION(ir3_shader_override_path, "IR3_SHADER_OVERRIDE_PATH",
                      NULL)

enum ir3_shader_debug ir3_shader_debug = 0;
const char *ir3_shader_override_path = NULL;

void
ir3_compiler_destroy(struct ir3_compiler *compiler)
{
   disk_cache_destroy(compiler->disk_cache);
   ralloc_free(compiler);
}

static bool
ir3_nir_lower_convert_alu_types(nir_intrinsic_instr *conv)
{
   assert(conv->intrinsic == nir_intrinsic_convert_alu_types);

   if (nir_src_is_const(conv->src[0]))
      return true;

   nir_alu_type src_type = nir_intrinsic_src_type(conv);
   nir_alu_type dest_type = nir_intrinsic_dest_type(conv);
   nir_rounding_mode rounding = nir_intrinsic_rounding_mode(conv);

   if (rounding == nir_rounding_mode_undef &&
       !nir_intrinsic_saturate(conv))
      return true;

   nir_alu_type src_base_type = nir_alu_type_get_base_type(src_type);
   nir_alu_type dest_base_type = nir_alu_type_get_base_type(dest_type);
   unsigned src_bit_size = nir_alu_type_get_type_size(src_type);
   unsigned dest_bit_size = nir_alu_type_get_type_size(dest_type);

   if ((src_base_type != nir_type_float) && (dest_base_type != nir_type_float))
      return true;

   if ((src_base_type == nir_type_float) && (dest_base_type == nir_type_float) &&
       (dest_bit_size > src_bit_size))
      return true;

   if ((dest_bit_size > 32) || (src_bit_size > 32))
      return true;

   if ((dest_bit_size < 16) || (src_bit_size < 16))
      return true;

   return false;
}

static const nir_shader_compiler_options ir3_base_options = {
   .compact_arrays = true,
   .lower_fpow = true,
   .lower_scmp = true,
   .lower_flrp16 = true,
   .lower_flrp32 = true,
   .lower_flrp64 = true,
   .lower_ffract = true,
   .lower_fmod = true,
   .lower_fdiv = true,
   .lower_isign = true,
   .lower_uadd_carry = true,
   .lower_usub_borrow = true,
   .lower_mul_high = true,
   .lower_mul_2x32_64 = true,
   .lower_ffma16 = true,
   .lower_ffma32 = true,
   .lower_ffma64 = true,
   .fuse_ffma16 = true,
   .fuse_ffma32 = true,
   .fuse_ffma64 = true,
   .vertex_id_zero_based = false,
   .lower_extract_byte = true,
   .lower_extract_word = true,
   .lower_insert_byte = true,
   .lower_insert_word = true,
   .lower_helper_invocation = true,
   .lower_bitfield_insert = true,
   .lower_bitfield_extract = true,
   .lower_bitfield_extract8 = true,
   .lower_bitfield_extract16 = true,
   .lower_pack_half_2x16 = true,
   .lower_pack_snorm_4x8 = true,
   .lower_pack_snorm_2x16 = true,
   .lower_pack_unorm_4x8 = true,
   .lower_pack_unorm_2x16 = true,
   .lower_unpack_half_2x16 = true,
   .lower_unpack_snorm_4x8 = true,
   .lower_unpack_snorm_2x16 = true,
   .lower_unpack_unorm_4x8 = true,
   .lower_unpack_unorm_2x16 = true,
   .lower_pack_split = true,
   .lower_pack_64_4x16 = true,
   .lower_to_scalar = true,
   .has_imul24 = true,
   .has_umul24 = true,
   .has_umul_16x16 = true,
   .has_icsel_eqz32 = true,
   .has_icsel_eqz16 = true,
   .has_fsub = true,
   .has_isub = true,
   .force_indirect_unrolling_sampler = true,
   .lower_uniforms_to_ubo = true,
   .max_unroll_iterations = 32,
   .max_samples = 4,
   .has_fmulz = true,
   .lower_fmulz_with_abs_min = true,
   .lower_cs_local_index_to_id = true,
   .lower_wpos_pntc = true,
   .lower_hadd = true,
   .lower_hadd64 = true,
   .lower_fisnormal = true,
   .lower_int64_options = (nir_lower_int64_options)~0,
   .lower_doubles_options = (nir_lower_doubles_options)~0,
   .divergence_analysis_options = nir_divergence_uniform_load_tears,
   .scalarize_ddx = true,
   .per_view_unique_driver_locations = true,
   .compact_view_index = true,
   .io_options = nir_io_has_intrinsics,
   .lower_convert_alu_types = ir3_nir_lower_convert_alu_types,
};

struct ir3_a8xx_codegen_profile {
   uint8_t max_unroll_iterations;
   uint8_t alu_to_alu_delay;
   uint8_t non_alu_delay;
   uint8_t cat3_src2_read_delay;
};

static struct ir3_a8xx_codegen_profile
ir3_a8xx_codegen_profile(uint64_t chip_id)
{
   switch (chip_id) {
   case 0x44010000:
   case 0xffff44010000: /* Adreno 810 */
      return (struct ir3_a8xx_codegen_profile){16, 2, 5, 1};
   case 0x44030000: /* Adreno 825 */
      return (struct ir3_a8xx_codegen_profile){24, 2, 5, 1};
   case 0x44030A20: /* Adreno 829 */
      return (struct ir3_a8xx_codegen_profile){26, 2, 5, 1};
   case 0x44050001:
   case 0xffff44050000: /* Adreno 830 */
      return (struct ir3_a8xx_codegen_profile){32, 2, 5, 1};
   case 0xffff44050A31: /* Adreno 840 */
      return (struct ir3_a8xx_codegen_profile){32, 2, 5, 1};
   default:
      return (struct ir3_a8xx_codegen_profile){28, 2, 5, 1};
   }
}

static void
__debug_init(void)
{
   ir3_shader_debug = debug_get_option_ir3_shader_debug();
   ir3_shader_override_path =
      __normal_user() ? debug_get_option_ir3_shader_override_path() : NULL;

   if (ir3_shader_override_path) {
      ir3_shader_debug |= IR3_DBG_NOCACHE;
   }

   ir3_shader_bisect_init();
}

static void
ir3_compiler_debug_init(void)
{
   static util_once_flag once = UTIL_ONCE_FLAG_INIT;
   util_call_once(&once, __debug_init);
}

struct ir3_compiler *
ir3_compiler_create(struct fd_device *dev, const struct fd_dev_id *dev_id,
                    const struct fd_dev_info *dev_info,
                    const struct ir3_compiler_options *options)
{
   struct ir3_compiler *compiler = rzalloc(NULL, struct ir3_compiler);

   ir3_compiler_debug_init();

   compiler->dev = dev;
   compiler->dev_id = dev_id;
   compiler->gen = fd_dev_gen(dev_id);
   compiler->is_64bit = fd_dev_64b(dev_id);
   compiler->options = *options;
   compiler->info = dev_info;

   compiler->branchstack_size = dev_info->props.has_dual_wave_dispatch ? 512 : 256;
   compiler->max_branchstack = 64;
   compiler->max_variable_workgroup_size = 1024;

   compiler->num_predicates = 1;
   compiler->bitops_can_write_predicates = false;
   compiler->has_branch_and_or = false;
   compiler->has_rpt_bary_f = false;
   compiler->has_alias_tex = false;
   compiler->delay_slots.alu_to_alu = 3;
   compiler->delay_slots.non_alu = 6;
   compiler->delay_slots.cat3_src2_read = 2;

   /* Initialize A8xx flag */
   compiler->is_a8xx = (compiler->gen >= 8);

   if (compiler->gen >= 6) {
      compiler->samgq_workaround = true;
      compiler->max_const_pipeline = 512;
      compiler->max_const_frag = 512;
      compiler->max_const_geom = 512;
      compiler->max_const_safe = 100;
      compiler->max_const_compute = compiler->gen >= 7 ? 512 : 256;

      if (dev_info->props.is_a702) {
         compiler->max_const_compute = 128;
         compiler->max_const_pipeline = 256;
         compiler->max_const_frag = 128;
         compiler->max_const_geom = 128;
         compiler->max_const_safe = 128;
      }

      compiler->has_clip_cull = true;
      compiler->has_preamble = true;

      if (compiler->gen == 6 && options->shared_push_consts) {
         compiler->shared_consts_base_offset = 504;
         compiler->shared_consts_size = 8;
         compiler->geom_shared_consts_size_quirk = 16;
      } else {
         compiler->shared_consts_base_offset = -1;
         compiler->shared_consts_size = 0;
         compiler->geom_shared_consts_size_quirk = 0;
      }

      compiler->num_predicates = 4;
      compiler->bitops_can_write_predicates = true;
      compiler->has_branch_and_or = true;
      compiler->has_predication = true;
      compiler->has_rpt_bary_f = true;
      compiler->has_shfl = true;
      compiler->mergedregs = true;
      compiler->has_alias_tex = (compiler->gen >= 7);

      if (compiler->gen == 7) {
         compiler->delay_slots.alu_to_alu = 2;
         compiler->delay_slots.non_alu = 5;
         compiler->delay_slots.cat3_src2_read = 1;
      }
   } else {
      compiler->max_const_pipeline = 512;
      compiler->max_const_geom = 512;
      compiler->max_const_frag = 512;
      compiler->max_const_compute = 512;
      compiler->max_const_safe = 256;
   }

   /* Apply A8xx-specific delay slots (same as A7xx - SAFE) */
   if (compiler->is_a8xx) {
      compiler->delay_slots.alu_to_alu = 2;
      compiler->delay_slots.non_alu = 5;
      compiler->delay_slots.cat3_src2_read = 1;
   }

   if (dev_info->compute_lb_size) {
      compiler->compute_lb_size = dev_info->compute_lb_size;
   } else {
      compiler->compute_lb_size =
         compiler->max_const_compute * 16 *
         compiler->info->wave_granularity + compiler->info->cs_shared_mem_size;
   }

   compiler->pvtmem_per_fiber_align = compiler->gen >= 4 ? 512 : 128;
   compiler->has_pvtmem = compiler->gen >= 5;
   compiler->has_isam_ssbo = compiler->gen >= 6;

   if (compiler->gen >= 6) {
      compiler->reg_size_vec4 = dev_info->props.reg_size_vec4;
   } else if (compiler->gen >= 4) {
      compiler->reg_size_vec4 = 48;
   } else {
      compiler->reg_size_vec4 = 96;
   }

   if (compiler->gen >= 4) {
      compiler->flat_bypass = true;
      compiler->levels_add_one = false;
      compiler->unminify_coords = false;
      compiler->txf_ms_with_isaml = false;
      compiler->array_index_add_half = true;
      compiler->instr_align = 16;
      compiler->const_upload_unit = 4;
   } else {
      compiler->flat_bypass = false;
      compiler->levels_add_one = true;
      compiler->unminify_coords = true;
      compiler->txf_ms_with_isaml = true;
      compiler->array_index_add_half = false;
      compiler->instr_align = 4;
      compiler->const_upload_unit = 8;
   }

   compiler->bool_type = (compiler->gen >= 5) ? TYPE_U16 : TYPE_U32;
   compiler->has_shared_regfile = compiler->gen >= 5;
   compiler->has_bitwise_triops = compiler->gen >= 5;
   compiler->cat3_rel_offset_0_quirk = compiler->gen <= 5;

   if (options->push_ubo_with_preamble)
      assert(compiler->has_preamble);

   compiler->nir_options = ir3_base_options;
   compiler->nir_options.has_iadd3 = dev_info->props.has_sad;

   /* Apply A8xx max_unroll_iterations */
   if (compiler->is_a8xx) {
      const struct ir3_a8xx_codegen_profile profile =
         ir3_a8xx_codegen_profile(dev_id->chip_id);
      compiler->nir_options.max_unroll_iterations = profile.max_unroll_iterations;
   }

   if (compiler->gen >= 6) {
      compiler->nir_options.force_indirect_unrolling = nir_var_all,
      compiler->nir_options.lower_device_index_to_zero = true;
      compiler->nir_options.instance_id_includes_base_index = true;

      if (dev_info->props.has_dp2acc || dev_info->props.has_dp4acc) {
         compiler->nir_options.has_udot_4x8 =
            compiler->nir_options.has_udot_4x8_sat = true;
         compiler->nir_options.has_sudot_4x8 =
            compiler->nir_options.has_sudot_4x8_sat = true;
      }

      if (dev_info->props.has_dp4acc && dev_info->props.has_compliant_dp4acc) {
         compiler->nir_options.has_sdot_4x8 =
            compiler->nir_options.has_sdot_4x8_sat = true;
      }
   } else if (compiler->gen >= 3 && compiler->gen <= 5) {
      compiler->nir_options.vertex_id_zero_based = true;
   } else if (compiler->gen <= 2) {
      compiler->nir_options.force_indirect_unrolling = nir_var_all;
   }

   if (compiler->gen >= 5) {
      compiler->nir_options.max_workgroup_count[0] =
         compiler->nir_options.max_workgroup_count[1] =
         compiler->nir_options.max_workgroup_count[2] = 65535;
      compiler->nir_options.max_workgroup_invocations =
         dev_info->threadsize_base * dev_info->max_waves;
      if ((compiler->gen >= 6) && dev_info->props.supports_double_threadsize)
         compiler->nir_options.max_workgroup_invocations *= 2;
   }

   if (options->lower_base_vertex) {
      compiler->nir_options.lower_base_vertex = true;
   }

   if (compiler->gen >= 5 && !(ir3_shader_debug & IR3_DBG_NOFP16))
      compiler->nir_options.support_16bit_alu = true;

   compiler->nir_options.support_indirect_inputs =
      BITFIELD_BIT(MESA_SHADER_TESS_CTRL) |
      BITFIELD_BIT(MESA_SHADER_TESS_EVAL);
   compiler->nir_options.support_indirect_outputs = (uint8_t)BITFIELD_MASK(MESA_SHADER_STAGES);
   compiler->nir_options.max_offset_shift = ir3_nir_max_offset_shift;

   if (!options->disable_cache)
      ir3_disk_cache_init(compiler);

   return compiler;
}

const nir_shader_compiler_options *
ir3_get_compiler_options(struct ir3_compiler *compiler)
{
   return &compiler->nir_options;
}

const char *
ir3_shader_debug_as_string()
{
   return debug_dump_flags(shader_debug_options, ir3_shader_debug);
}