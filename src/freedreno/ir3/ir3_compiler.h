/*
 * Copyright © 2013 Rob Clark <robclark@freedesktop.org>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#ifndef IR3_COMPILER_H_
#define IR3_COMPILER_H_

#include "compiler/nir/nir.h"
#include "util/disk_cache.h"
#include "util/log.h"
#include "util/perf/cpu_trace.h"

#include "freedreno_dev_info.h"

#include "ir3.h"

BEGINC;

struct ir3_ra_reg_set;
struct ir3_shader;

struct ir3_compiler_options {
   bool push_ubo_with_preamble;
   bool disable_cache;
   int bindless_fb_read_descriptor;
   int bindless_fb_read_slot;
   bool storage_16bit;
   bool storage_8bit;
   bool lower_base_vertex;
   bool shared_push_consts;
   bool dual_color_blend_by_location;
   uint64_t uche_trap_base;
};

struct ir3_compiler {
   struct fd_device *dev;
   const struct fd_dev_id *dev_id;
   uint8_t gen;
   uint32_t shader_count;

   struct disk_cache *disk_cache;

   struct nir_shader_compiler_options nir_options;
   struct ir3_compiler_options options;

   bool is_64bit;
   bool flat_bypass;
   bool levels_add_one;
   bool unminify_coords;
   bool txf_ms_with_isaml;
   bool array_index_add_half;
   bool samgq_workaround;
   bool mergedregs;

   const struct fd_dev_info *info;

   uint16_t max_const_pipeline;
   uint16_t max_const_geom;
   uint16_t max_const_frag;
   uint16_t max_const_safe;
   uint16_t max_const_compute;
   uint32_t compute_lb_size;
   uint32_t instr_align;
   uint32_t const_upload_unit;
   uint32_t reg_size_vec4;
   uint32_t branchstack_size;
   uint32_t max_branchstack;
   uint32_t pvtmem_per_fiber_align;

   bool has_clip_cull;
   bool has_pvtmem;
   bool has_isam_ssbo;
   bool cs_lock_unlock_quirk;
   bool has_shfl;
   bool has_bitwise_triops;

   uint32_t num_predicates;
   bool bitops_can_write_predicates;
   bool has_branch_and_or;
   bool has_predication;
   uint32_t max_variable_workgroup_size;

   type_t bool_type;
   bool has_shared_regfile;
   bool has_preamble;

   uint16_t shared_consts_base_offset;
   uint64_t shared_consts_size;
   uint64_t geom_shared_consts_size_quirk;

   bool has_rpt_bary_f;
   bool has_alias_tex;
   bool cat3_rel_offset_0_quirk;

   /* A8xx chip-specific tuning (SAFE - no FP16 lowering) */
   bool is_a8xx;

   struct {
      unsigned alu_to_alu;
      unsigned non_alu;
      unsigned cat3_src2_read;
   } delay_slots;
};

void ir3_compiler_destroy(struct ir3_compiler *compiler);
struct ir3_compiler *ir3_compiler_create(struct fd_device *dev,
                                         const struct fd_dev_id *dev_id,
                                         const struct fd_dev_info *dev_info,
                                         const struct ir3_compiler_options *options);

void ir3_disk_cache_init(struct ir3_compiler *compiler);
void ir3_disk_cache_init_shader_key(struct ir3_compiler *compiler,
                                    struct ir3_shader *shader);
struct ir3_shader_variant *ir3_retrieve_variant(struct blob_reader *blob,
                                                struct ir3_compiler *compiler,
                                                void *mem_ctx);
void ir3_store_variant(struct blob *blob, const struct ir3_shader_variant *v);
bool ir3_disk_cache_retrieve(struct ir3_shader *shader,
                             struct ir3_shader_variant *v);
void ir3_disk_cache_store(struct ir3_shader *shader,
                          struct ir3_shader_variant *v);

const nir_shader_compiler_options *
ir3_get_compiler_options(struct ir3_compiler *compiler);

int ir3_compile_shader_nir(struct ir3_compiler *compiler,
                           struct ir3_shader *shader,
                           struct ir3_shader_variant *so);

static inline unsigned
ir3_pointer_size(struct ir3_compiler *compiler)
{
   return compiler->is_64bit ? 2 : 1;
}

enum ir3_shader_debug {
   IR3_DBG_SHADER_VS = BITFIELD_BIT(0),
   IR3_DBG_SHADER_TCS = BITFIELD_BIT(1),
   IR3_DBG_SHADER_TES = BITFIELD_BIT(2),
   IR3_DBG_SHADER_GS = BITFIELD_BIT(3),
   IR3_DBG_SHADER_FS = BITFIELD_BIT(4),
   IR3_DBG_SHADER_CS = BITFIELD_BIT(5),
   IR3_DBG_DISASM = BITFIELD_BIT(6),
   IR3_DBG_OPTMSGS = BITFIELD_BIT(7),
   IR3_DBG_FORCES2EN = BITFIELD_BIT(8),
   IR3_DBG_NOUBOOPT = BITFIELD_BIT(9),
   IR3_DBG_NOFP16 = BITFIELD_BIT(10),
   IR3_DBG_NOCACHE = BITFIELD_BIT(11),
   IR3_DBG_SPILLALL = BITFIELD_BIT(12),
   IR3_DBG_NOPREAMBLE = BITFIELD_BIT(13),
   IR3_DBG_SHADER_INTERNAL = BITFIELD_BIT(14),
   IR3_DBG_FULLSYNC = BITFIELD_BIT(15),
   IR3_DBG_FULLNOP = BITFIELD_BIT(16),
   IR3_DBG_NOEARLYPREAMBLE = BITFIELD_BIT(17),
   IR3_DBG_NODESCPREFETCH = BITFIELD_BIT(18),
   IR3_DBG_EXPANDRPT = BITFIELD_BIT(19),
   IR3_DBG_ASM_ROUNDTRIP = BITFIELD_BIT(20),
   IR3_DBG_SCHEDMSGS = BITFIELD_BIT(21),
   IR3_DBG_RAMSGS = BITFIELD_BIT(22),
   IR3_DBG_NOALIASTEX = BITFIELD_BIT(23),
   IR3_DBG_NOALIASRT = BITFIELD_BIT(24),
};

extern enum ir3_shader_debug ir3_shader_debug;
extern const char *ir3_shader_override_path;

static inline bool
shader_debug_enabled(mesa_shader_stage type, bool internal)
{
   if (internal)
      return !!(ir3_shader_debug & IR3_DBG_SHADER_INTERNAL);

   if (ir3_shader_debug & IR3_DBG_DISASM)
      return true;

   switch (type) {
   case MESA_SHADER_VERTEX:
      return !!(ir3_shader_debug & IR3_DBG_SHADER_VS);
   case MESA_SHADER_TESS_CTRL:
      return !!(ir3_shader_debug & IR3_DBG_SHADER_TCS);
   case MESA_SHADER_TESS_EVAL:
      return !!(ir3_shader_debug & IR3_DBG_SHADER_TES);
   case MESA_SHADER_GEOMETRY:
      return !!(ir3_shader_debug & IR3_DBG_SHADER_GS);
   case MESA_SHADER_FRAGMENT:
      return !!(ir3_shader_debug & IR3_DBG_SHADER_FS);
   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_KERNEL:
      return !!(ir3_shader_debug & IR3_DBG_SHADER_CS);
   default:
      assert(0);
      return false;
   }
}

static inline void
ir3_debug_print(struct ir3 *ir, const char *when)
{
   if (ir3_shader_debug & IR3_DBG_OPTMSGS) {
      mesa_logi("%s:", when);
      ir3_print(ir);
   }
}

static inline enum ir3_shader_debug
ir3_shader_debug_hash_key()
{
   return (enum ir3_shader_debug)(
      ir3_shader_debug &
      ~(IR3_DBG_SHADER_VS | IR3_DBG_SHADER_TCS | IR3_DBG_SHADER_TES |
        IR3_DBG_SHADER_GS | IR3_DBG_SHADER_FS | IR3_DBG_SHADER_CS |
        IR3_DBG_DISASM | IR3_DBG_OPTMSGS | IR3_DBG_NOCACHE |
        IR3_DBG_SHADER_INTERNAL | IR3_DBG_SCHEDMSGS | IR3_DBG_RAMSGS));
}

const char *
ir3_shader_debug_as_string(void);

void ir3_shader_bisect_init(void);
bool ir3_shader_bisect_need_shader_key(void);
void ir3_shader_bisect_dump_id(struct ir3_shader_variant *v);
bool ir3_shader_bisect_select(struct ir3_shader_variant *v);
bool ir3_shader_bisect_disasm_select(struct ir3_shader_variant *v);

ENDC;

#endif /* IR3_COMPILER_H_ */