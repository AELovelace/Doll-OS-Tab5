/* gameplaySP
 *
 * Copyright (C) 2006 Exophase <exophase@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

extern "C" {
  #include "common.h"
  #include "cpu_instrument.h"
}

#ifdef RETRO_GO
#include <sdkconfig.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <string.h>
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include <hal/cache_ll.h>
#include <soc/ext_mem_defs.h>
#endif
#define GBA_HOT_DATA_ATTR DRAM_ATTR
#ifndef GBA_DECODED_BLOCK_CACHE
#define GBA_DECODED_BLOCK_CACHE 0
#endif

#ifndef GBA_P4_THUMB_DYNAREC
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define GBA_P4_THUMB_DYNAREC 1
#else
#define GBA_P4_THUMB_DYNAREC 0
#endif
#endif

#ifndef GBA_THUMB_PROFILE
#define GBA_THUMB_PROFILE 0
#endif
#ifndef GBA_SWI_HLE
#define GBA_SWI_HLE 0
#endif
#else
#define GBA_HOT_DATA_ATTR
#define GBA_DECODED_BLOCK_CACHE 0
#define GBA_SWI_HLE 0
#endif

extern "C" {
u32 gba_block_cache_hits = 0;
u32 gba_block_cache_misses = 0;
u32 gba_block_cache_inserts = 0;
u32 gba_block_cache_bytes = 0;
u32 gba_thumb_alu_fast_ops = 0;
u32 gba_thumb_branch_fast_ops = 0;
u32 gba_thumb_hireg_fast_ops = 0;
u32 gba_thumb_ldr_fast_ops = 0;
u32 gba_thumb_ldrh_fast_ops = 0;
u32 gba_thumb_ldrb_fast_ops = 0;
u32 gba_thumb_pcldr_fast_ops = 0;
u32 gba_thumb_str_fast_ops = 0;
u32 gba_thumb_strh_fast_ops = 0;
u32 gba_thumb_strb_fast_ops = 0;
u32 gba_thumb_push_fast_ops = 0;
u32 gba_thumb_pop_fast_ops = 0;
u32 gba_thumb_low_fast_ops = 0;
u32 gba_thumb_imm_fast_ops = 0;
u32 gba_thumb_jit_hits = 0;
u32 gba_thumb_jit_misses = 0;
u32 gba_thumb_jit_compiles = 0;
u32 gba_thumb_jit_ops = 0;
u32 gba_thumb_jit_flushes = 0;
u32 gba_thumb_jit_bytes = 0;
u32 gba_thumb_jit_attempts = 0;
u32 gba_thumb_jit_region_skips = 0;
u32 gba_thumb_jit_short_blocks = 0;
u32 gba_thumb_jit_disabled = 0;
u32 gba_thumb_jit_validate_passes = 0;
u32 gba_thumb_jit_validate_failures = 0;
u32 gba_thumb_jit_fail_pc = 0;
u32 gba_thumb_jit_fail_opcode = 0;
u32 gba_thumb_jit_fail_index = 0;
u32 gba_thumb_jit_fail_expected = 0;
u32 gba_thumb_jit_fail_actual = 0;
u32 gba_thumb_jit_fail_reason = 0;
u32 gba_thumb_jit_arena_full = 0;
u32 gba_thumb_jit_used_bytes = 0;
u32 gba_thumb_jit_reject_hits = 0;
u32 gba_thumb_jit_hot_waits = 0;
u32 gba_thumb_jit_reuses = 0;
u32 gba_thumb_jit_adapt_probes = 0;
u32 gba_thumb_jit_top_break = 0;
u32 gba_thumb_jit_top_break_count = 0;
u32 gba_thumb_batch_runs = 0;
u32 gba_thumb_batch_ops = 0;
u32 gba_thumb_batch_enabled = 0;
// The hand-written ARM and Thumb fast paths ran unconditionally, so "Safe" was
// never the stock interpreter. Clearing this drops both back to gpSP's own
// decode, which is what the mode is supposed to mean when a game misbehaves.
u32 gba_interp_fast_enabled = 1;
u32 gba_thumb_jit_runtime_enabled = 0;
u32 gba_thumb_jit_debug_validate = 0;
u32 gba_thumb_jit_guard_trips = 0;
u32 gba_thumb_jit_last_pc = 0;
u32 gba_thumb_jit_last_end_pc = 0;
u32 gba_thumb_jit_last_ret = 0;
u32 gba_thumb_jit_last_signature = 0;
#if GBA_P4_THUMB_DYNAREC
static u32 *gba_thumb_jit_break_histogram = NULL;
static u32 *gba_thumb_jit_fail_histogram = NULL;
#endif
#if GBA_THUMB_PROFILE
u32 gba_thumb_fallback_histogram[256] = {0};
u32 gba_thumb_opcode_histogram[256] = {0};
u32 gba_thumb_swi_histogram[256] = {0};
u32 gba_arm_swi_histogram[256] = {0};
u32 gba_arm_opcode_histogram[256] = {0};
u32 gba_arm_exact_opcode[64] = {0};
u32 gba_arm_exact_pc[64] = {0};
u32 gba_arm_exact_count[64] = {0};
#endif
u32 gba_arm_hot_fast_hits = 0;
u32 gba_arm_hot_fast_misses = 0;
#if GBA_THUMB_PROFILE
u32 gba_arm_bios_pc_histogram[4096] = {0};
#endif
u32 gba_arm_ops = 0;
u32 gba_execute_calls = 0;
u32 gba_execute_halt_updates = 0;
u32 gba_execute_arm_updates = 0;
u32 gba_execute_thumb_updates = 0;
u32 gba_execute_last_pc = 0;
u32 gba_execute_last_cpsr = 0;
u32 gba_execute_last_halt = 0;
u32 gba_execute_last_update_ret = 0;
u32 gba_execute_last_cycles = 0;
u32 gba_bad_pc_count = 0;
u32 gba_bad_pc_last = 0;
u32 gba_bad_pc_last_cpsr = 0;
u32 gba_swi_hle_softreset_count = 0;
u32 gba_swi_hle_ramreset_count = 0;
u32 gba_swi_hle_unpack_count = 0;
u32 gba_swi_hle_lz77_count = 0;
u32 gba_swi_hle_huff_count = 0;
u32 gba_swi_hle_rl_count = 0;
u32 gba_swi_hle_diff_count = 0;
u32 gba_bios_init_loop_hle_count = 0;
u32 gba_bios_reset_path_hle_count = 0;
// A guest that restarts without issuing SWI SoftReset got to the BIOS reset
// vector by branching to a null or corrupt address. These record the first such
// entry so the offending call site can be read straight off the serial log.
u32 gba_guest_reset_trips = 0;
u32 gba_guest_entry_trips = 0;
u32 gba_guest_reset_prev_pc = 0;
u32 gba_guest_reset_lr = 0;
u32 gba_guest_reset_sp = 0;
u32 gba_guest_reset_cpsr = 0;

#if GBA_THUMB_PROFILE
static inline void gba_arm_profile_exact_opcode(u32 pc, u32 opcode)
{
  u32 slot = (opcode ^ (opcode >> 7) ^ (opcode >> 16) ^ (opcode >> 24)) & 63U;
  for(u32 probe = 0; probe < 4; probe++)
  {
    u32 index = (slot + probe) & 63U;
    if(gba_arm_exact_count[index] == 0 || gba_arm_exact_opcode[index] == opcode)
    {
      gba_arm_exact_opcode[index] = opcode;
      gba_arm_exact_pc[index] = pc;
      gba_arm_exact_count[index]++;
      return;
    }
  }

  u32 min_index = slot;
  for(u32 probe = 1; probe < 4; probe++)
  {
    u32 index = (slot + probe) & 63U;
    if(gba_arm_exact_count[index] < gba_arm_exact_count[min_index])
      min_index = index;
  }
  if(gba_arm_exact_count[min_index] <= 4)
  {
    gba_arm_exact_opcode[min_index] = opcode;
    gba_arm_exact_pc[min_index] = pc;
    gba_arm_exact_count[min_index] = 1;
  }
}

#define gba_thumb_profile_opcode(opcode) \
  do { gba_thumb_opcode_histogram[((opcode) >> 8) & 0xFF]++; } while(0)
#define gba_thumb_profile_fallback(opcode) \
  do { gba_thumb_fallback_histogram[((opcode) >> 8) & 0xFF]++; } while(0)
#define gba_profile_thumb_swi(swinum) \
  do { gba_thumb_swi_histogram[(swinum) & 0xFF]++; } while(0)
#define gba_profile_arm_swi(swinum) \
  do { gba_arm_swi_histogram[(swinum) & 0xFF]++; } while(0)
#define gba_arm_profile_opcode(opcode) \
  do { gba_arm_opcode_histogram[((opcode) >> 20) & 0xFF]++; gba_arm_profile_exact_opcode(reg[REG_PC], (opcode)); gba_arm_ops++; } while(0)
#else
#define gba_thumb_profile_opcode(opcode) do { } while(0)
#define gba_thumb_profile_fallback(opcode) do { } while(0)
#define gba_profile_thumb_swi(swinum) do { } while(0)
#define gba_profile_arm_swi(swinum) do { } while(0)
#define gba_arm_profile_opcode(opcode) do { } while(0)
#endif
}

#if GBA_DECODED_BLOCK_CACHE
#define GBA_DECODED_BLOCK_OPS     8
#define GBA_DECODED_BLOCK_ENTRIES 2048

typedef struct
{
  u32 pc;
  u16 mode;
  u16 op_count;
  u32 tag;
  u32 opcode[GBA_DECODED_BLOCK_OPS];
} gba_decoded_block_t;

static gba_decoded_block_t *gba_block_cache;
static u32 gba_block_cache_tick;
static u32 gba_block_cache_last_arm_pc = 0xFFFFFFFF;
static u32 gba_block_cache_last_thumb_pc = 0xFFFFFFFF;

static inline bool gba_arm_block_ends(u32 opcode)
{
  if(((opcode >> 25) & 0x07) == 0x05)
    return true;

  if((opcode & 0x0FFFFFF0) == 0x012FFF10)
    return true;

  return ((opcode >> 24) & 0x0F) == 0x0F;
}

static inline bool gba_thumb_block_ends(u32 opcode)
{
  u32 top = opcode >> 8;

  if((opcode & 0xFF87) == 0x4700)
    return true;

  return top >= 0xD0;
}

static void gba_block_cache_init(void)
{
  if(gba_block_cache)
    return;

  const size_t bytes = sizeof(gba_decoded_block_t) * GBA_DECODED_BLOCK_ENTRIES;
  gba_block_cache = (gba_decoded_block_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(!gba_block_cache)
    return;

  memset(gba_block_cache, 0, bytes);
  gba_block_cache_bytes = (u32)bytes;
}

static inline gba_decoded_block_t *gba_block_cache_lookup(u32 pc, u32 mode)
{
  gba_block_cache_init();
  if(!gba_block_cache)
    return NULL;

  u32 hash = (pc >> (mode ? 1 : 2)) ^ (pc >> 15) ^ (mode * 0x9E37);
  return &gba_block_cache[hash & (GBA_DECODED_BLOCK_ENTRIES - 1)];
}

static inline void gba_block_cache_touch_arm(u32 pc, u8 *pc_address_block)
{
  pc &= ~0x03;
  if(pc == gba_block_cache_last_arm_pc + 4)
  {
    gba_block_cache_last_arm_pc = pc;
    return;
  }
  gba_block_cache_last_arm_pc = pc;

  gba_decoded_block_t *block = gba_block_cache_lookup(pc, 0);
  if(!block)
    return;

  if(block->pc == pc && block->mode == 0)
  {
    gba_block_cache_hits++;
    block->tag = ++gba_block_cache_tick;
    return;
  }

  gba_block_cache_misses++;
  block->pc = pc;
  block->mode = 0;
  block->tag = ++gba_block_cache_tick;
  block->op_count = 0;

  u32 offset = pc & 0x7FFF;
  for(u32 i = 0; i < GBA_DECODED_BLOCK_OPS && offset <= (0x8000 - 4); i++, offset += 4)
  {
    u32 opcode = readaddress32(pc_address_block, offset);
    block->opcode[i] = opcode;
    block->op_count++;
    if(gba_arm_block_ends(opcode))
      break;
  }

  gba_block_cache_inserts++;
}

static inline void gba_block_cache_touch_thumb(u32 pc, u8 *pc_address_block)
{
  pc &= ~0x01;
  if(pc == gba_block_cache_last_thumb_pc + 2)
  {
    gba_block_cache_last_thumb_pc = pc;
    return;
  }
  gba_block_cache_last_thumb_pc = pc;

  gba_decoded_block_t *block = gba_block_cache_lookup(pc, 1);
  if(!block)
    return;

  if(block->pc == pc && block->mode == 1)
  {
    gba_block_cache_hits++;
    block->tag = ++gba_block_cache_tick;
    return;
  }

  gba_block_cache_misses++;
  block->pc = pc;
  block->mode = 1;
  block->tag = ++gba_block_cache_tick;
  block->op_count = 0;

  u32 offset = pc & 0x7FFF;
  for(u32 i = 0; i < GBA_DECODED_BLOCK_OPS && offset <= (0x8000 - 2); i++, offset += 2)
  {
    u32 opcode = readaddress16(pc_address_block, offset);
    block->opcode[i] = opcode;
    block->op_count++;
    if(gba_thumb_block_ends(opcode))
      break;
  }

  gba_block_cache_inserts++;
}
#else
#define gba_block_cache_touch_arm(pc, pc_address_block)   do { } while(0)
#define gba_block_cache_touch_thumb(pc, pc_address_block) do { } while(0)
#endif

#if GBA_THUMB_PROFILE
#define gba_arm_hot_fast_hit() do { gba_arm_hot_fast_hits++; } while(0)
#define gba_arm_hot_fast_miss() do { gba_arm_hot_fast_misses++; } while(0)
#else
#define gba_arm_hot_fast_hit() do { } while(0)
#define gba_arm_hot_fast_miss() do { } while(0)
#endif

#if GBA_P4_THUMB_DYNAREC && defined(CONFIG_IDF_TARGET_ESP32P4)
#ifndef GBA_P4_THUMB_JIT_ENTRIES
#define GBA_P4_THUMB_JIT_ENTRIES     2048
#endif
#ifndef GBA_P4_THUMB_JIT_MAX_OPS
#define GBA_P4_THUMB_JIT_MAX_OPS     8
#endif
#ifndef GBA_P4_THUMB_JIT_WAYS
#define GBA_P4_THUMB_JIT_WAYS        4
#endif
#define GBA_P4_THUMB_JIT_SETS        (GBA_P4_THUMB_JIT_ENTRIES / GBA_P4_THUMB_JIT_WAYS)
#if (GBA_P4_THUMB_JIT_ENTRIES % GBA_P4_THUMB_JIT_WAYS) != 0 || \
    (GBA_P4_THUMB_JIT_SETS & (GBA_P4_THUMB_JIT_SETS - 1)) != 0
#error "GBA Thumb JIT sets must be a power of two"
#endif
#define GBA_P4_THUMB_JIT_MIN_OPS     3
#ifndef GBA_P4_THUMB_JIT_ARENA_BYTES
#define GBA_P4_THUMB_JIT_ARENA_BYTES (64 * 1024)
#endif
#ifndef GBA_P4_THUMB_JIT_BANKS
#define GBA_P4_THUMB_JIT_BANKS       1
#endif
#ifndef GBA_P4_THUMB_JIT_REJECTS
#define GBA_P4_THUMB_JIT_REJECTS     1024
#endif
#define GBA_P4_THUMB_JIT_ALU40_LOGIC 1
#define GBA_P4_THUMB_JIT_ALU40_SHIFTS 1
#define GBA_P4_THUMB_JIT_ALU42       1
#define GBA_P4_THUMB_JIT_ALU43_LOGIC 1
#define GBA_P4_THUMB_JIT_HIREG_ALU   1
#define GBA_P4_THUMB_JIT_HIREG_MOV   1
#define GBA_P4_THUMB_JIT_STACK_READS 1
#define GBA_P4_THUMB_JIT_STACK_WRITES 1
#ifndef GBA_P4_THUMB_JIT_WRAM_LOADS
#define GBA_P4_THUMB_JIT_WRAM_LOADS  0
#endif
#ifndef GBA_P4_THUMB_JIT_WRAM_STORES
#define GBA_P4_THUMB_JIT_WRAM_STORES 0
#endif
#define GBA_P4_THUMB_JIT_TRUST_VALIDATIONS 8
#define GBA_P4_THUMB_JIT_RECYCLE_ARENA 0
#define GBA_P4_THUMB_JIT_REUSE_EXHAUSTED 1
#ifndef GBA_P4_THUMB_JIT_HOT_ENTRIES
#define GBA_P4_THUMB_JIT_HOT_ENTRIES 2048
#endif
#ifndef GBA_P4_THUMB_JIT_HOT_THRESHOLD
#define GBA_P4_THUMB_JIT_HOT_THRESHOLD 32
#endif
#define GBA_P4_THUMB_JIT_STALE_RECYCLE 0
#define GBA_P4_THUMB_JIT_STALE_MISS_THRESHOLD 1200000
#define GBA_P4_THUMB_JIT_STALE_HIT_LIMIT 64
#define GBA_P4_THUMB_JIT_SUSPEND_MISSES 8192
#define GBA_P4_THUMB_JIT_SUSPEND_OPS 4194304
#define GBA_P4_THUMB_JIT_ADAPT_SAMPLE_MASK 63U
#define GBA_P4_THUMB_JIT_RET_OPS_MASK 0xFFFFU
#define GBA_P4_THUMB_JIT_RET_EXTRA_SHIFT 16
#define GBA_P4_THUMB_JIT_RET_EXTRA_MASK 0xFFU
#define GBA_P4_THUMB_JIT_RET_ARM_SWITCH (1U << 24)

typedef u32 (*gba_p4_thumb_jit_fn)(u32 *regs, u32 *flags);

typedef struct
{
  u32 pc;
  u32 code_words;
  u16 op_count;
  u16 validated;
  u16 extra_cycles;
  u16 can_bail;
  u16 opcodes[GBA_P4_THUMB_JIT_MAX_OPS];
  gba_p4_thumb_jit_fn fn;
} gba_p4_thumb_jit_entry_t;

#define GBA_P4_THUMB_JIT_TRACE_COUNT 16
typedef struct
{
  u32 pc;
  u32 end_pc;
  u32 sp;
  u32 lr;
  u32 ret;
  u32 signature;
} gba_p4_thumb_jit_trace_t;

static gba_p4_thumb_jit_trace_t gba_p4_thumb_jit_trace[GBA_P4_THUMB_JIT_TRACE_COUNT];
static u32 gba_p4_thumb_jit_trace_head;
static bool gba_p4_thumb_jit_fault_reported;

// These tables cost roughly 320 KB in the current associative configuration.
// Keeping them as fixed BSS starves unrelated shell/network features even when
// GBA has never run, so allocate them lazily from PSRAM with the executable arena.
static gba_p4_thumb_jit_entry_t *gba_p4_thumb_jit_cache;
static u8 *gba_p4_thumb_jit_replacement;
static u32 *gba_p4_thumb_jit_reject_pc;
static u32 *gba_p4_thumb_jit_reject_sig;
static u16 *gba_p4_thumb_jit_reject_break;
static u32 *gba_p4_thumb_jit_hot_pc;
static u8 *gba_p4_thumb_jit_hot_count;
static u32 *gba_p4_thumb_jit_exec[GBA_P4_THUMB_JIT_BANKS];
static volatile u32 *gba_p4_thumb_jit_write[GBA_P4_THUMB_JIT_BANKS];
static u32 gba_p4_thumb_jit_used_words[GBA_P4_THUMB_JIT_BANKS];
static u32 gba_p4_thumb_jit_capacity_words[GBA_P4_THUMB_JIT_BANKS];
static u32 gba_p4_thumb_jit_bank_count;
static u32 gba_p4_thumb_jit_bank_index;
static u32 gba_p4_thumb_jit_exhausted_hits;
static u32 gba_p4_thumb_jit_exhausted_misses;
static u32 gba_p4_thumb_jit_probe_suspend;
static u32 gba_p4_thumb_jit_adapt_counter;
static bool gba_p4_thumb_jit_ready;
static bool gba_p4_thumb_jit_disabled;
static bool gba_p4_thumb_jit_arena_exhausted;

enum
{
  RV_ZERO = 0,
  RV_RA = 1,
  RV_T0 = 5,
  RV_T1 = 6,
  RV_T2 = 7,
  RV_A0 = 10,
  RV_A1 = 11,
  RV_A2 = 12,
  RV_A3 = 13,
  RV_A4 = 14,
  RV_A5 = 15,
  RV_A6 = 16,
  RV_A7 = 17,
  RV_T3 = 28,
  RV_T4 = 29,
  RV_T5 = 30,
  RV_T6 = 31,
};

static inline u32 rv_r(u32 funct7, u32 rs2, u32 rs1, u32 funct3, u32 rd)
{
  return ((funct7 & 0x7F) << 25) | ((rs2 & 0x1F) << 20) |
         ((rs1 & 0x1F) << 15) | ((funct3 & 0x07) << 12) |
         ((rd & 0x1F) << 7) | 0x33;
}

static inline u32 rv_i(s32 imm, u32 rs1, u32 funct3, u32 rd, u32 opcode)
{
  return (((u32)imm & 0xFFF) << 20) | ((rs1 & 0x1F) << 15) |
         ((funct3 & 0x07) << 12) | ((rd & 0x1F) << 7) | (opcode & 0x7F);
}

static inline u32 rv_s(s32 imm, u32 rs2, u32 rs1, u32 funct3)
{
  u32 uimm = (u32)imm & 0xFFF;
  return ((uimm >> 5) << 25) | ((rs2 & 0x1F) << 20) |
         ((rs1 & 0x1F) << 15) | ((funct3 & 0x07) << 12) |
         ((uimm & 0x1F) << 7) | 0x23;
}

static inline u32 rv_lui(u32 rd, u32 imm20)
{
  return ((imm20 & 0xFFFFF) << 12) | ((rd & 0x1F) << 7) | 0x37;
}

static inline u32 rv_add(u32 rd, u32 rs1, u32 rs2)   { return rv_r(0x00, rs2, rs1, 0x0, rd); }
static inline u32 rv_mul(u32 rd, u32 rs1, u32 rs2)   { return rv_r(0x01, rs2, rs1, 0x0, rd); }
static inline u32 rv_sub(u32 rd, u32 rs1, u32 rs2)   { return rv_r(0x20, rs2, rs1, 0x0, rd); }
static inline u32 rv_sll(u32 rd, u32 rs1, u32 rs2)   { return rv_r(0x00, rs2, rs1, 0x1, rd); }
static inline u32 rv_sltu(u32 rd, u32 rs1, u32 rs2)  { return rv_r(0x00, rs2, rs1, 0x3, rd); }
static inline u32 rv_xor(u32 rd, u32 rs1, u32 rs2)   { return rv_r(0x00, rs2, rs1, 0x4, rd); }
static inline u32 rv_srl(u32 rd, u32 rs1, u32 rs2)   { return rv_r(0x00, rs2, rs1, 0x5, rd); }
static inline u32 rv_or(u32 rd, u32 rs1, u32 rs2)    { return rv_r(0x00, rs2, rs1, 0x6, rd); }
static inline u32 rv_and(u32 rd, u32 rs1, u32 rs2)   { return rv_r(0x00, rs2, rs1, 0x7, rd); }
static inline u32 rv_addi(u32 rd, u32 rs1, s32 imm)  { return rv_i(imm, rs1, 0x0, rd, 0x13); }
static inline u32 rv_sltiu(u32 rd, u32 rs1, s32 imm) { return rv_i(imm, rs1, 0x3, rd, 0x13); }
static inline u32 rv_xori(u32 rd, u32 rs1, s32 imm)  { return rv_i(imm, rs1, 0x4, rd, 0x13); }
static inline u32 rv_andi(u32 rd, u32 rs1, s32 imm)  { return rv_i(imm, rs1, 0x7, rd, 0x13); }
static inline u32 rv_slli(u32 rd, u32 rs1, u32 sh)   { return rv_i((s32)(sh & 0x1F), rs1, 0x1, rd, 0x13); }
static inline u32 rv_srli(u32 rd, u32 rs1, u32 sh)   { return rv_i((s32)sh, rs1, 0x5, rd, 0x13); }
static inline u32 rv_srai(u32 rd, u32 rs1, u32 sh)   { return rv_i((s32)(0x400 | (sh & 0x1F)), rs1, 0x5, rd, 0x13); }
static inline u32 rv_lw(u32 rd, u32 rs1, s32 imm)    { return rv_i(imm, rs1, 0x2, rd, 0x03); }
static inline u32 rv_lbu(u32 rd, u32 rs1, s32 imm)   { return rv_i(imm, rs1, 0x4, rd, 0x03); }
static inline u32 rv_lhu(u32 rd, u32 rs1, s32 imm)   { return rv_i(imm, rs1, 0x5, rd, 0x03); }
static inline u32 rv_sb(u32 rs2, u32 rs1, s32 imm)   { return rv_s(imm, rs2, rs1, 0x0); }
static inline u32 rv_sh(u32 rs2, u32 rs1, s32 imm)   { return rv_s(imm, rs2, rs1, 0x1); }
static inline u32 rv_sw(u32 rs2, u32 rs1, s32 imm)   { return rv_s(imm, rs2, rs1, 0x2); }

static inline u32 rv_b(s32 imm, u32 rs2, u32 rs1, u32 funct3)
{
  u32 uimm = (u32)imm;
  return (((uimm >> 12) & 0x01) << 31) |
         (((uimm >> 5) & 0x3F) << 25) |
         ((rs2 & 0x1F) << 20) |
         ((rs1 & 0x1F) << 15) |
         ((funct3 & 0x07) << 12) |
         (((uimm >> 1) & 0x0F) << 8) |
         (((uimm >> 11) & 0x01) << 7) |
         0x63;
}

static inline u32 rv_beq(u32 rs1, u32 rs2, s32 imm) { return rv_b(imm, rs2, rs1, 0x0); }
static inline u32 rv_bne(u32 rs1, u32 rs2, s32 imm) { return rv_b(imm, rs2, rs1, 0x1); }

typedef struct
{
  volatile u32 *write;
  u32 *exec;
  u32 words;
  u32 capacity_words;
  u8 cache_gba_reg[6];
  u8 cache_dirty[6];
  u8 cache_next;
} gba_p4_rv_emit_t;

static inline bool gba_p4_emit(gba_p4_rv_emit_t *emit, u32 instruction)
{
  if(emit->words >= emit->capacity_words)
    return false;
  emit->write[emit->words++] = instruction;
  return true;
}

static bool gba_p4_emit_li32(gba_p4_rv_emit_t *emit, u32 rd, u32 value)
{
  u32 hi = (value + 0x800) >> 12;
  s32 lo = (s32)(value - (hi << 12));

  if(hi == 0)
    return gba_p4_emit(emit, rv_addi(rd, RV_ZERO, lo));

  return gba_p4_emit(emit, rv_lui(rd, hi)) &&
         gba_p4_emit(emit, rv_addi(rd, rd, lo));
}

static bool gba_p4_emit_branch_placeholder(gba_p4_rv_emit_t *emit, u32 *position)
{
  *position = emit->words;
  return gba_p4_emit(emit, 0);
}

static bool gba_p4_patch_branch(gba_p4_rv_emit_t *emit, u32 position,
    u32 target_words, u32 rs1, u32 rs2, u32 funct3)
{
  s32 imm = (s32)(target_words - position) * 4;
  if((imm & 1) || imm < -4096 || imm > 4094 || position >= emit->words)
    return false;

  emit->write[position] = rv_b(imm, rs2, rs1, funct3);
  return true;
}

static inline bool gba_p4_thumb_jit_l2_cached_ptr(const void *ptr)
{
  uintptr_t addr = (uintptr_t)ptr;
  return addr >= SOC_IRAM0_ADDRESS_LOW && addr < SOC_IRAM0_ADDRESS_HIGH;
}

static inline volatile u32 *gba_p4_thumb_jit_write_alias(void *exec_ptr)
{
  if(gba_p4_thumb_jit_l2_cached_ptr(exec_ptr))
    return (volatile u32 *)CACHE_LL_L2MEM_NON_CACHE_ADDR(exec_ptr);
  return (volatile u32 *)exec_ptr;
}

static inline void gba_p4_thumb_jit_sync(void *exec_ptr, u32 used_words)
{
  const uintptr_t line = 32;
  uintptr_t exec_start = (uintptr_t)exec_ptr & ~(line - 1);
  uintptr_t exec_end = ((uintptr_t)exec_ptr + used_words * sizeof(u32) + line - 1) & ~(line - 1);

  __asm__ volatile("fence rw,rw" ::: "memory");
  cache_ll_invalidate_addr(CACHE_LL_LEVEL_ALL, CACHE_TYPE_INSTRUCTION,
      CACHE_LL_ID_ALL, (uint32_t)exec_start, (uint32_t)(exec_end - exec_start));
  __builtin___clear_cache((char *)exec_start, (char *)exec_end);
  __asm__ volatile("fence.i" ::: "memory");
}

static bool gba_p4_thumb_jit_init(void)
{
  if(gba_p4_thumb_jit_ready || gba_p4_thumb_jit_disabled)
    return gba_p4_thumb_jit_ready;

  static const u32 arena_sizes[] =
  {
    GBA_P4_THUMB_JIT_ARENA_BYTES,
    160 * 1024,
    128 * 1024,
    96 * 1024,
    80 * 1024,
    64 * 1024,
    48 * 1024,
    32 * 1024,
    24 * 1024,
    16 * 1024,
    12 * 1024,
    8 * 1024,
    4 * 1024,
  };

  u32 arena_bytes = 0;
  memset(gba_p4_thumb_jit_exec, 0, sizeof(gba_p4_thumb_jit_exec));
  memset((void *)gba_p4_thumb_jit_write, 0, sizeof(gba_p4_thumb_jit_write));
  memset(gba_p4_thumb_jit_used_words, 0, sizeof(gba_p4_thumb_jit_used_words));
  memset(gba_p4_thumb_jit_capacity_words, 0, sizeof(gba_p4_thumb_jit_capacity_words));
  gba_p4_thumb_jit_bank_count = 0;
  gba_p4_thumb_jit_bank_index = 0;
  gba_p4_thumb_jit_exhausted_hits = 0;
  gba_p4_thumb_jit_exhausted_misses = 0;
  gba_p4_thumb_jit_probe_suspend = 0;
  gba_p4_thumb_jit_adapt_counter = 0;

  gba_p4_thumb_jit_cache = (gba_p4_thumb_jit_entry_t *)heap_caps_calloc(
      GBA_P4_THUMB_JIT_ENTRIES, sizeof(*gba_p4_thumb_jit_cache),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  gba_p4_thumb_jit_replacement = (u8 *)heap_caps_calloc(
      GBA_P4_THUMB_JIT_SETS, sizeof(*gba_p4_thumb_jit_replacement),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  gba_p4_thumb_jit_reject_pc = (u32 *)heap_caps_calloc(
      GBA_P4_THUMB_JIT_REJECTS, sizeof(*gba_p4_thumb_jit_reject_pc),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  gba_p4_thumb_jit_reject_sig = (u32 *)heap_caps_calloc(
      GBA_P4_THUMB_JIT_REJECTS, sizeof(*gba_p4_thumb_jit_reject_sig),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  gba_p4_thumb_jit_reject_break = (u16 *)heap_caps_calloc(
      GBA_P4_THUMB_JIT_REJECTS, sizeof(*gba_p4_thumb_jit_reject_break),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  gba_p4_thumb_jit_hot_pc = (u32 *)heap_caps_calloc(
      GBA_P4_THUMB_JIT_HOT_ENTRIES, sizeof(*gba_p4_thumb_jit_hot_pc),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  gba_p4_thumb_jit_hot_count = (u8 *)heap_caps_calloc(
      GBA_P4_THUMB_JIT_HOT_ENTRIES, sizeof(*gba_p4_thumb_jit_hot_count),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  gba_thumb_jit_break_histogram = (u32 *)heap_caps_calloc(
      256, sizeof(*gba_thumb_jit_break_histogram),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  gba_thumb_jit_fail_histogram = (u32 *)heap_caps_calloc(
      256, sizeof(*gba_thumb_jit_fail_histogram),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(!(gba_p4_thumb_jit_cache && gba_p4_thumb_jit_replacement &&
       gba_p4_thumb_jit_reject_pc &&
       gba_p4_thumb_jit_reject_sig && gba_p4_thumb_jit_reject_break &&
       gba_p4_thumb_jit_hot_pc && gba_p4_thumb_jit_hot_count &&
       gba_thumb_jit_break_histogram && gba_thumb_jit_fail_histogram))
  {
    if(gba_p4_thumb_jit_cache) heap_caps_free(gba_p4_thumb_jit_cache);
    if(gba_p4_thumb_jit_replacement) heap_caps_free(gba_p4_thumb_jit_replacement);
    if(gba_p4_thumb_jit_reject_pc) heap_caps_free(gba_p4_thumb_jit_reject_pc);
    if(gba_p4_thumb_jit_reject_sig) heap_caps_free(gba_p4_thumb_jit_reject_sig);
    if(gba_p4_thumb_jit_reject_break) heap_caps_free(gba_p4_thumb_jit_reject_break);
    if(gba_p4_thumb_jit_hot_pc) heap_caps_free(gba_p4_thumb_jit_hot_pc);
    if(gba_p4_thumb_jit_hot_count) heap_caps_free(gba_p4_thumb_jit_hot_count);
    if(gba_thumb_jit_break_histogram) heap_caps_free(gba_thumb_jit_break_histogram);
    if(gba_thumb_jit_fail_histogram) heap_caps_free(gba_thumb_jit_fail_histogram);
    gba_p4_thumb_jit_cache = NULL;
    gba_p4_thumb_jit_replacement = NULL;
    gba_p4_thumb_jit_reject_pc = NULL;
    gba_p4_thumb_jit_reject_sig = NULL;
    gba_p4_thumb_jit_reject_break = NULL;
    gba_p4_thumb_jit_hot_pc = NULL;
    gba_p4_thumb_jit_hot_count = NULL;
    gba_thumb_jit_break_histogram = NULL;
    gba_thumb_jit_fail_histogram = NULL;
    gba_p4_thumb_jit_disabled = true;
    gba_thumb_jit_disabled++;
    return false;
  }

  while(gba_p4_thumb_jit_bank_count < GBA_P4_THUMB_JIT_BANKS)
  {
    u32 *bank = NULL;
    u32 bank_bytes = 0;

    for(u32 i = 0; i < sizeof(arena_sizes) / sizeof(arena_sizes[0]); i++)
    {
      bank = (u32 *)heap_caps_aligned_alloc(32,
          arena_sizes[i], MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
      if(bank && esp_ptr_executable(bank))
      {
        bank_bytes = arena_sizes[i];
        break;
      }

      if(bank)
      {
        heap_caps_free(bank);
        bank = NULL;
      }
    }

    if(!bank)
      break;

    u32 index = gba_p4_thumb_jit_bank_count++;
    gba_p4_thumb_jit_exec[index] = bank;
    gba_p4_thumb_jit_write[index] = gba_p4_thumb_jit_write_alias(bank);
    gba_p4_thumb_jit_capacity_words[index] = bank_bytes / sizeof(u32);
    arena_bytes += bank_bytes;
  }

  if(!gba_p4_thumb_jit_bank_count)
  {
    heap_caps_free(gba_p4_thumb_jit_cache);
    heap_caps_free(gba_p4_thumb_jit_replacement);
    heap_caps_free(gba_p4_thumb_jit_reject_pc);
    heap_caps_free(gba_p4_thumb_jit_reject_sig);
    heap_caps_free(gba_p4_thumb_jit_reject_break);
    heap_caps_free(gba_p4_thumb_jit_hot_pc);
    heap_caps_free(gba_p4_thumb_jit_hot_count);
    heap_caps_free(gba_thumb_jit_break_histogram);
    heap_caps_free(gba_thumb_jit_fail_histogram);
    gba_p4_thumb_jit_cache = NULL;
    gba_p4_thumb_jit_replacement = NULL;
    gba_p4_thumb_jit_reject_pc = NULL;
    gba_p4_thumb_jit_reject_sig = NULL;
    gba_p4_thumb_jit_reject_break = NULL;
    gba_p4_thumb_jit_hot_pc = NULL;
    gba_p4_thumb_jit_hot_count = NULL;
    gba_thumb_jit_break_histogram = NULL;
    gba_thumb_jit_fail_histogram = NULL;
    gba_p4_thumb_jit_disabled = true;
    gba_thumb_jit_disabled++;
    return false;
  }

  gba_p4_thumb_jit_arena_exhausted = false;
  memset(gba_p4_thumb_jit_cache, 0,
      GBA_P4_THUMB_JIT_ENTRIES * sizeof(*gba_p4_thumb_jit_cache));
  memset(gba_p4_thumb_jit_replacement, 0,
      GBA_P4_THUMB_JIT_SETS * sizeof(*gba_p4_thumb_jit_replacement));
  memset(gba_p4_thumb_jit_reject_pc, 0,
      GBA_P4_THUMB_JIT_REJECTS * sizeof(*gba_p4_thumb_jit_reject_pc));
  memset(gba_p4_thumb_jit_reject_sig, 0,
      GBA_P4_THUMB_JIT_REJECTS * sizeof(*gba_p4_thumb_jit_reject_sig));
  memset(gba_p4_thumb_jit_reject_break, 0,
      GBA_P4_THUMB_JIT_REJECTS * sizeof(*gba_p4_thumb_jit_reject_break));
  memset(gba_p4_thumb_jit_hot_pc, 0,
      GBA_P4_THUMB_JIT_HOT_ENTRIES * sizeof(*gba_p4_thumb_jit_hot_pc));
  memset(gba_p4_thumb_jit_hot_count, 0,
      GBA_P4_THUMB_JIT_HOT_ENTRIES * sizeof(*gba_p4_thumb_jit_hot_count));
  gba_thumb_jit_bytes = arena_bytes;
  gba_p4_thumb_jit_ready = true;
  return true;
}

extern "C" void gba_p4_thumb_jit_preinit(void)
{
  (void)gba_p4_thumb_jit_init();
}

static void gba_p4_thumb_jit_flush(void)
{
  memset(gba_p4_thumb_jit_cache, 0,
      GBA_P4_THUMB_JIT_ENTRIES * sizeof(*gba_p4_thumb_jit_cache));
  memset(gba_p4_thumb_jit_replacement, 0,
      GBA_P4_THUMB_JIT_SETS * sizeof(*gba_p4_thumb_jit_replacement));
  memset(gba_p4_thumb_jit_used_words, 0, sizeof(gba_p4_thumb_jit_used_words));
  gba_p4_thumb_jit_bank_index = 0;
  gba_p4_thumb_jit_exhausted_hits = 0;
  gba_p4_thumb_jit_exhausted_misses = 0;
  gba_p4_thumb_jit_probe_suspend = 0;
  gba_p4_thumb_jit_adapt_counter = 0;
  gba_p4_thumb_jit_arena_exhausted = false;
  gba_thumb_jit_used_bytes = 0;
  gba_thumb_jit_flushes++;
}

extern "C" void gba_p4_thumb_jit_reset_stats(void)
{
  gba_thumb_jit_hits = 0;
  gba_thumb_jit_misses = 0;
  gba_thumb_jit_compiles = 0;
  gba_thumb_jit_ops = 0;
  gba_thumb_jit_flushes = 0;
  gba_thumb_jit_attempts = 0;
  gba_thumb_jit_region_skips = 0;
  gba_thumb_jit_short_blocks = 0;
  gba_thumb_jit_disabled = 0;
  gba_thumb_jit_validate_passes = 0;
  gba_thumb_jit_validate_failures = 0;
  gba_thumb_jit_arena_full = 0;
  gba_thumb_jit_reject_hits = 0;
  gba_thumb_jit_hot_waits = 0;
  gba_thumb_jit_reuses = 0;
  gba_thumb_jit_adapt_probes = 0;
  gba_thumb_jit_top_break = 0;
  gba_thumb_jit_top_break_count = 0;
  gba_thumb_batch_runs = 0;
  gba_thumb_batch_ops = 0;
  gba_thumb_jit_guard_trips = 0;
  gba_thumb_jit_last_pc = 0;
  gba_thumb_jit_last_end_pc = 0;
  gba_thumb_jit_last_ret = 0;
  gba_thumb_jit_last_signature = 0;
  gba_p4_thumb_jit_trace_head = 0;
  gba_p4_thumb_jit_fault_reported = false;
  memset(gba_p4_thumb_jit_trace, 0, sizeof(gba_p4_thumb_jit_trace));
  if(gba_thumb_jit_break_histogram)
    memset(gba_thumb_jit_break_histogram, 0, 256 * sizeof(*gba_thumb_jit_break_histogram));
} // Starts each ROM with clean JIT telemetry while preserving its allocated arena.

extern "C" void gba_p4_thumb_jit_report_fault(u32 reason, u32 fault_pc)
{
  const bool validation_fault = (reason & 0xFF000000U) == 0x4A000000U;
  gba_thumb_jit_guard_trips++;
  gba_thumb_jit_runtime_enabled = 0;
  if(!validation_fault)
    gba_thumb_batch_enabled = 0;
  if(gba_p4_thumb_jit_fault_reported)
    return;

  gba_p4_thumb_jit_fault_reported = true;
  ESP_LOGE("gba-jit", "guard reason=%08lx fault=%08lx last=%08lx->%08lx ret=%08lx sig=%08lx",
      (unsigned long)reason, (unsigned long)fault_pc,
      (unsigned long)gba_thumb_jit_last_pc,
      (unsigned long)gba_thumb_jit_last_end_pc,
      (unsigned long)gba_thumb_jit_last_ret,
      (unsigned long)gba_thumb_jit_last_signature);

  u32 available = gba_p4_thumb_jit_trace_head < GBA_P4_THUMB_JIT_TRACE_COUNT ?
      gba_p4_thumb_jit_trace_head : GBA_P4_THUMB_JIT_TRACE_COUNT;
  u32 first = gba_p4_thumb_jit_trace_head - available;
  for(u32 i = 0; i < available; i++)
  {
    const gba_p4_thumb_jit_trace_t *trace =
        &gba_p4_thumb_jit_trace[(first + i) & (GBA_P4_THUMB_JIT_TRACE_COUNT - 1)];
    ESP_LOGE("gba-jit", "trace[%02lu] pc=%08lx end=%08lx sp=%08lx lr=%08lx ret=%08lx sig=%08lx",
        (unsigned long)i, (unsigned long)trace->pc,
        (unsigned long)trace->end_pc, (unsigned long)trace->sp,
        (unsigned long)trace->lr, (unsigned long)trace->ret,
        (unsigned long)trace->signature);
  }
} // Falls back to Batch after a JIT mismatch, but freezes both engines for CPU faults.

extern "C" void gba_p4_thumb_jit_reset(void)
{
  if(gba_p4_thumb_jit_ready)
    gba_p4_thumb_jit_flush();
}

extern "C" void gba_p4_thumb_jit_shutdown(void)
{
  for(u32 i = 0; i < gba_p4_thumb_jit_bank_count; i++)
  {
    if(gba_p4_thumb_jit_exec[i])
      heap_caps_free(gba_p4_thumb_jit_exec[i]);
    gba_p4_thumb_jit_exec[i] = NULL;
    gba_p4_thumb_jit_write[i] = NULL;
  }
  if(gba_p4_thumb_jit_cache) heap_caps_free(gba_p4_thumb_jit_cache);
  if(gba_p4_thumb_jit_replacement) heap_caps_free(gba_p4_thumb_jit_replacement);
  if(gba_p4_thumb_jit_reject_pc) heap_caps_free(gba_p4_thumb_jit_reject_pc);
  if(gba_p4_thumb_jit_reject_sig) heap_caps_free(gba_p4_thumb_jit_reject_sig);
  if(gba_p4_thumb_jit_reject_break) heap_caps_free(gba_p4_thumb_jit_reject_break);
  if(gba_p4_thumb_jit_hot_pc) heap_caps_free(gba_p4_thumb_jit_hot_pc);
  if(gba_p4_thumb_jit_hot_count) heap_caps_free(gba_p4_thumb_jit_hot_count);
  if(gba_thumb_jit_break_histogram) heap_caps_free(gba_thumb_jit_break_histogram);
  if(gba_thumb_jit_fail_histogram) heap_caps_free(gba_thumb_jit_fail_histogram);
  gba_p4_thumb_jit_cache = NULL;
  gba_p4_thumb_jit_replacement = NULL;
  gba_p4_thumb_jit_reject_pc = NULL;
  gba_p4_thumb_jit_reject_sig = NULL;
  gba_p4_thumb_jit_reject_break = NULL;
  gba_p4_thumb_jit_hot_pc = NULL;
  gba_p4_thumb_jit_hot_count = NULL;
  gba_thumb_jit_break_histogram = NULL;
  gba_thumb_jit_fail_histogram = NULL;
  gba_p4_thumb_jit_bank_count = 0;
  gba_p4_thumb_jit_bank_index = 0;
  gba_p4_thumb_jit_ready = false;
  gba_p4_thumb_jit_disabled = false;
  gba_p4_thumb_jit_arena_exhausted = false;
  gba_thumb_jit_bytes = 0;
}

static inline bool gba_p4_thumb_jit_recycle_arena(void)
{
#if GBA_P4_THUMB_JIT_RECYCLE_ARENA
  if(!gba_p4_thumb_jit_ready || !gba_p4_thumb_jit_bank_count)
    return false;

  gba_p4_thumb_jit_flush();
  return true;
#else
  return false;
#endif
}

static inline bool gba_p4_thumb_jit_recycle_stale_arena(void)
{
#if GBA_P4_THUMB_JIT_STALE_RECYCLE
  if(!gba_p4_thumb_jit_ready || !gba_p4_thumb_jit_bank_count ||
     !gba_p4_thumb_jit_arena_exhausted)
    return false;

  if(gba_p4_thumb_jit_exhausted_misses < GBA_P4_THUMB_JIT_STALE_MISS_THRESHOLD)
    return false;

  if(gba_p4_thumb_jit_exhausted_hits > GBA_P4_THUMB_JIT_STALE_HIT_LIMIT)
  {
    gba_p4_thumb_jit_exhausted_hits = 0;
    gba_p4_thumb_jit_exhausted_misses = 0;
    return false;
  }

  gba_p4_thumb_jit_flush();
  return true;
#else
  return false;
#endif
}

static inline bool gba_p4_thumb_jit_can_allocate_after_full(void)
{
#if GBA_P4_THUMB_JIT_REUSE_EXHAUSTED || \
    GBA_P4_THUMB_JIT_RECYCLE_ARENA || \
    GBA_P4_THUMB_JIT_STALE_RECYCLE
  return true;
#else
  return false;
#endif
}

static inline u32 gba_p4_thumb_jit_hash(u32 pc)
{
  u32 set = ((pc >> 1) ^ (pc >> 7) ^ (pc >> 15)) &
      (GBA_P4_THUMB_JIT_SETS - 1);
  return set * GBA_P4_THUMB_JIT_WAYS;
}

static inline gba_p4_thumb_jit_entry_t *gba_p4_thumb_jit_lookup(u32 pc)
{
  u32 base = gba_p4_thumb_jit_hash(pc);
  for(u32 way = 0; way < GBA_P4_THUMB_JIT_WAYS; way++)
  {
    gba_p4_thumb_jit_entry_t *entry = &gba_p4_thumb_jit_cache[base + way];
    if(entry->pc == pc && entry->fn)
      return entry;
  }
  return NULL;
} // Searches every way so unrelated hot blocks with the same set remain resident.

static inline gba_p4_thumb_jit_entry_t *gba_p4_thumb_jit_select_entry(u32 pc)
{
  u32 base = gba_p4_thumb_jit_hash(pc);
  u32 set = base / GBA_P4_THUMB_JIT_WAYS;

  if(!gba_p4_thumb_jit_arena_exhausted)
  {
    for(u32 way = 0; way < GBA_P4_THUMB_JIT_WAYS; way++)
    {
      gba_p4_thumb_jit_entry_t *entry = &gba_p4_thumb_jit_cache[base + way];
      if(!entry->fn)
        return entry;
    }
  }

  u32 first = gba_p4_thumb_jit_replacement[set]++ % GBA_P4_THUMB_JIT_WAYS;
  for(u32 probe = 0; probe < GBA_P4_THUMB_JIT_WAYS; probe++)
  {
    gba_p4_thumb_jit_entry_t *entry =
        &gba_p4_thumb_jit_cache[base + ((first + probe) % GBA_P4_THUMB_JIT_WAYS)];
    if(!gba_p4_thumb_jit_arena_exhausted || (entry->fn && entry->code_words))
      return entry;
  }
  return NULL;
} // Uses empty ways first, then round-robin replacement within the colliding set.

static inline bool gba_p4_thumb_jit_hot_enough(u32 pc)
{
#if GBA_P4_THUMB_JIT_HOT_THRESHOLD <= 1
  (void)pc;
  return true;
#else
  u32 index = ((pc >> 1) ^ (pc >> 8) ^ (pc >> 14)) &
      (GBA_P4_THUMB_JIT_HOT_ENTRIES - 1);

  if(gba_p4_thumb_jit_hot_pc[index] != pc)
  {
    gba_p4_thumb_jit_hot_pc[index] = pc;
    gba_p4_thumb_jit_hot_count[index] = 1;
    return false;
  }

  if(gba_p4_thumb_jit_hot_count[index] < GBA_P4_THUMB_JIT_HOT_THRESHOLD)
  {
    gba_p4_thumb_jit_hot_count[index]++;
    return false;
  }

  return true;
#endif
}

static inline bool gba_p4_thumb_jit_region_allowed(u32 region)
{
  return region >= 0x08 && region <= 0x0D;
}

static inline bool gba_p4_thumb_jit_terminal_opcode(u32 opcode)
{
  u32 top = (opcode >> 8) & 0xFF;
  return (top >= 0xD0 && top <= 0xDD) ||
         (top >= 0xE0 && top <= 0xE7) ||
         (top >= 0xF8 && top <= 0xFF);
}

static inline bool gba_p4_thumb_jit_pc_write_opcode(u32 opcode)
{
  u32 top = (opcode >> 8) & 0xFF;
  u32 rd = ((opcode >> 4) & 0x08) | (opcode & 0x07);
  return (GBA_P4_THUMB_JIT_HIREG_MOV && top == 0x46 && rd == REG_PC) ||
         top == 0x47;
}

static inline bool gba_p4_thumb_jit_wram_load_opcode(u32 opcode)
{
  u32 top = (opcode >> 8) & 0xFF;
  return GBA_P4_THUMB_JIT_WRAM_LOADS &&
      ((top >= 0x68 && top <= 0x6F) ||
       (top >= 0x78 && top <= 0x7F) ||
       (top >= 0x88 && top <= 0x8F));
}

static inline bool gba_p4_thumb_jit_wram_store_opcode(u32 opcode)
{
  u32 top = (opcode >> 8) & 0xFF;
  return GBA_P4_THUMB_JIT_WRAM_STORES &&
      ((top >= 0x60 && top <= 0x67) ||
       (top >= 0x70 && top <= 0x77) ||
       (top >= 0x80 && top <= 0x87));
}

static inline bool gba_p4_thumb_jit_allow_single_opcode(u32 opcode)
{
  (void)opcode;
  return false;
}

static inline bool gba_p4_thumb_jit_read_literal32(u32 address, u32 *value)
{
  u32 region = address >> 24;
  if((address & 0x03) || region < 0x08 || region > 0x0D)
    return false;

  u8 *map = memory_map_read[address >> 15];
  if(!map)
    return false;

  *value = readaddress32(map, address & 0x7FFF);
  return true;
}

static inline u32 gba_p4_thumb_jit_reject_hash(u32 pc)
{
  return ((pc >> 1) ^ (pc >> 6) ^ (pc >> 13)) & (GBA_P4_THUMB_JIT_REJECTS - 1);
}

static inline u32 gba_p4_thumb_jit_make_sig(u32 pc, u8 *pc_address_block)
{
  u32 offset = pc & 0x7FFF;
  u32 op0 = (offset <= (0x8000 - 2)) ? readaddress16(pc_address_block, offset) : 0xFFFF;
  u32 op1 = (offset <= (0x8000 - 4)) ? readaddress16(pc_address_block, offset + 2) : 0xFFFF;
  return op0 | (op1 << 16);
}

static inline bool gba_p4_thumb_jit_is_rejected(u32 pc, u8 *pc_address_block)
{
  (void)pc_address_block;
  u32 index = gba_p4_thumb_jit_reject_hash(pc);
  if(gba_p4_thumb_jit_reject_pc[index] != pc)
    return false;

  gba_thumb_jit_reject_hits++;
  if(gba_p4_thumb_jit_reject_break[index])
  {
    u32 group = gba_p4_thumb_jit_reject_break[index] - 1;
    u32 count = ++gba_thumb_jit_break_histogram[group];
    if(count > gba_thumb_jit_top_break_count)
    {
      gba_thumb_jit_top_break = group;
      gba_thumb_jit_top_break_count = count;
    }
  }

  return true;
}

static inline void gba_p4_thumb_jit_clear_entry(u32 pc)
{
  gba_p4_thumb_jit_entry_t *entry = gba_p4_thumb_jit_lookup(pc);
  if(entry)
    memset(entry, 0, sizeof(*entry));
}

static inline bool gba_p4_thumb_jit_control_flow_opcode(u32 opcode)
{
  u32 top = (opcode >> 8) & 0xFF;
  return (top >= 0xD0 && top <= 0xDD) ||
         (top >= 0xE0 && top <= 0xE7) ||
         (top >= 0xF0 && top <= 0xFF) ||
         gba_p4_thumb_jit_pc_write_opcode(opcode);
} // Keeps data-dependent branches, calls, and returns in the exact interpreter.

static inline bool gba_p4_thumb_jit_supported_opcode(u32 opcode)
{
  u32 top = (opcode >> 8) & 0xFF;
  if(top <= 0x3F)
    return true;

  if(GBA_P4_THUMB_JIT_ALU40_LOGIC && top == 0x40)
  {
    u32 subop = (opcode >> 6) & 0x03;
    return subop <= 0x01 ||
        (GBA_P4_THUMB_JIT_ALU40_SHIFTS && subop <= 0x03);
  }

  if(GBA_P4_THUMB_JIT_ALU42 && top == 0x42)
    return true;

  if(GBA_P4_THUMB_JIT_ALU43_LOGIC && top == 0x43)
    return true;

  if(GBA_P4_THUMB_JIT_HIREG_ALU && (top == 0x44 || top == 0x45))
  {
    u32 rs = (opcode >> 3) & 0x0F;
    u32 rd = ((opcode >> 4) & 0x08) | (opcode & 0x07);
    return top == 0x45 || rd != REG_PC;
  }

  if(GBA_P4_THUMB_JIT_HIREG_MOV && top == 0x46)
    return true;

  if(top == 0x47)
    return true;

  if(top >= 0x48 && top <= 0x4F)
    return true;

  if(top >= 0xA0 && top <= 0xAF)
    return true;

  if(top >= 0xB0 && top <= 0xB3)
    return true;

  if(GBA_P4_THUMB_JIT_STACK_READS && top >= 0x98 && top <= 0x9F)
    return true;

  if(GBA_P4_THUMB_JIT_STACK_WRITES && top >= 0x90 && top <= 0x97)
    return true;

  if(GBA_P4_THUMB_JIT_STACK_WRITES && (top == 0xB4 || top == 0xB5))
    return (opcode & 0xFF) != 0 || top == 0xB5;

  if(GBA_P4_THUMB_JIT_STACK_READS && top == 0xBC)
    return (opcode & 0xFF) != 0;

  if(gba_p4_thumb_jit_wram_store_opcode(opcode))
    return true;

  if(gba_p4_thumb_jit_wram_load_opcode(opcode))
    return true;

  if(top >= 0xF0 && top <= 0xFF)
    return true;

  if(gba_p4_thumb_jit_terminal_opcode(opcode))
    return true;

  return false;
}

static u32 gba_p4_thumb_jit_find_break_group(u32 pc, u8 *pc_address_block)
{
  u32 offset = pc & 0x7FFF;
  u32 count = 0;

  while(count < GBA_P4_THUMB_JIT_MAX_OPS && offset <= (0x8000 - 2))
  {
    u32 opcode = readaddress16(pc_address_block, offset);
    if(!gba_p4_thumb_jit_supported_opcode(opcode))
      return (opcode >> 8) & 0xFF;

    count++;
    if(gba_p4_thumb_jit_terminal_opcode(opcode) ||
       gba_p4_thumb_jit_pc_write_opcode(opcode))
      return 0x100;
    offset += 2;
  }

  return 0x100;
}

static inline void gba_p4_thumb_jit_mark_rejected(u32 pc, u8 *pc_address_block)
{
  gba_p4_thumb_jit_clear_entry(pc);

  u32 index = gba_p4_thumb_jit_reject_hash(pc);
  u32 break_group = gba_p4_thumb_jit_find_break_group(pc, pc_address_block);
  gba_p4_thumb_jit_reject_pc[index] = pc;
  gba_p4_thumb_jit_reject_sig[index] =
      gba_p4_thumb_jit_make_sig(pc, pc_address_block);
  gba_p4_thumb_jit_reject_break[index] =
      (break_group < 0x100) ? (u16)(break_group + 1) : 0;

  if(break_group < 0x100)
  {
    u32 count = ++gba_thumb_jit_break_histogram[break_group];
    if(count > gba_thumb_jit_top_break_count)
    {
      gba_thumb_jit_top_break = break_group;
      gba_thumb_jit_top_break_count = count;
    }
  }
}

static u32 gba_p4_thumb_jit_collect(u32 pc, u8 *pc_address_block, u16 *opcodes)
{
  u32 offset = pc & 0x7FFF;
  u32 count = 0;

  while(count < GBA_P4_THUMB_JIT_MAX_OPS && offset <= (0x8000 - 2))
  {
    u32 opcode = readaddress16(pc_address_block, offset);
    if(gba_p4_thumb_jit_control_flow_opcode(opcode))
      break;
    if(!gba_p4_thumb_jit_supported_opcode(opcode))
      break;
    opcodes[count++] = (u16)opcode;
    offset += 2;
  }

  return count;
}

static inline bool gba_p4_thumb_jit_matches(gba_p4_thumb_jit_entry_t *entry,
    u32 pc, u8 *pc_address_block)
{
  if(entry->pc != pc || !entry->fn)
    return false;

  if(entry->op_count < GBA_P4_THUMB_JIT_MIN_OPS &&
     (entry->op_count == 0 || !gba_p4_thumb_jit_allow_single_opcode(entry->opcodes[0])))
    return false;

  // Cartridge ROM is immutable after loading, and load/state transitions flush
  // the JIT. Once the safety interpreter has validated a block twice, its PC is
  // therefore a sufficient identity check and the hot path can avoid rereading
  // as many as 16 opcodes from the PSRAM-backed ROM cache on every execution.
  if(entry->validated >= GBA_P4_THUMB_JIT_TRUST_VALIDATIONS)
    return true;

  u32 offset = pc & 0x7FFF;
  for(u32 i = 0; i < entry->op_count; i++, offset += 2)
  {
    if(readaddress16(pc_address_block, offset) != entry->opcodes[i])
      return false;
  }

  return true;
}

static inline void gba_p4_thumb_jit_sim_nz(u32 value, u32 *flags)
{
  flags[0] = value >> 31;
  flags[1] = value == 0;
}

static inline void gba_p4_thumb_jit_sim_add_flags(u32 lhs, u32 rhs,
    u32 dest, u32 *flags)
{
  gba_p4_thumb_jit_sim_nz(dest, flags);
  flags[2] = dest < rhs;
  flags[3] = (~(lhs ^ rhs) & (lhs ^ dest)) >> 31;
}

static inline void gba_p4_thumb_jit_sim_sub_flags(u32 lhs, u32 rhs,
    u32 dest, u32 *flags)
{
  gba_p4_thumb_jit_sim_nz(dest, flags);
  flags[2] = lhs >= rhs;
  flags[3] = ((lhs ^ rhs) & (lhs ^ dest)) >> 31;
}

static bool gba_p4_thumb_jit_simulate_one(u32 opcode, u32 *sim_regs,
    u32 *sim_flags)
{
  u32 top = (opcode >> 8) & 0xFF;
  u32 dest;

  if(top < 0x18)
  {
    u32 rd = opcode & 0x07;
    u32 rs = (opcode >> 3) & 0x07;
    u32 offset = (opcode >> 6) & 0x1F;
    u32 src = sim_regs[rs];

    switch((opcode >> 11) & 0x03)
    {
      case 0x00:
        dest = src << offset;
        if(offset)
          sim_flags[2] = (src >> (32 - offset)) & 0x01;
        break;

      case 0x01:
        if(offset == 0)
        {
          sim_flags[2] = src >> 31;
          dest = 0;
        }
        else
        {
          sim_flags[2] = (src >> (offset - 1)) & 0x01;
          dest = src >> offset;
        }
        break;

      default:
        if(offset == 0)
        {
          dest = (u32)((s32)src >> 31);
          sim_flags[2] = dest & 0x01;
        }
        else
        {
          sim_flags[2] = (src >> (offset - 1)) & 0x01;
          dest = (u32)((s32)src >> offset);
        }
        break;
    }

    gba_p4_thumb_jit_sim_nz(dest, sim_flags);
    sim_regs[rd] = dest;
    sim_regs[REG_PC] += 2;
    return true;
  }

  if(top <= 0x1F)
  {
    u32 rd = opcode & 0x07;
    u32 rs = (opcode >> 3) & 0x07;
    u32 lhs = sim_regs[rs];
    u32 rhs = (opcode & 0x0400) ? ((opcode >> 6) & 0x07) :
        sim_regs[(opcode >> 6) & 0x07];

    if(opcode & 0x0200)
    {
      dest = lhs - rhs;
      gba_p4_thumb_jit_sim_sub_flags(lhs, rhs, dest, sim_flags);
    }
    else
    {
      dest = lhs + rhs;
      gba_p4_thumb_jit_sim_add_flags(lhs, rhs, dest, sim_flags);
    }

    sim_regs[rd] = dest;
    sim_regs[REG_PC] += 2;
    return true;
  }

  if(top >= 0x20 && top <= 0x3F)
  {
    u32 rd = top & 0x07;
    u32 imm = opcode & 0xFF;
    u32 src = sim_regs[rd];

    switch(top >> 3)
    {
      case 0x04:
        dest = imm;
        gba_p4_thumb_jit_sim_nz(dest, sim_flags);
        sim_regs[rd] = dest;
        break;

      case 0x05:
        dest = src - imm;
        gba_p4_thumb_jit_sim_sub_flags(src, imm, dest, sim_flags);
        break;

      case 0x06:
        dest = src + imm;
        gba_p4_thumb_jit_sim_add_flags(src, imm, dest, sim_flags);
        sim_regs[rd] = dest;
        break;

      default:
        dest = src - imm;
        gba_p4_thumb_jit_sim_sub_flags(src, imm, dest, sim_flags);
        sim_regs[rd] = dest;
        break;
    }

    sim_regs[REG_PC] += 2;
    return true;
  }

  if(GBA_P4_THUMB_JIT_ALU40_LOGIC && top == 0x40)
  {
    u32 rd = opcode & 0x07;
    u32 rs = (opcode >> 3) & 0x07;
    u32 subop = (opcode >> 6) & 0x03;
    u32 lhs = sim_regs[rd];
    u32 rhs = sim_regs[rs];

    if(subop > 0x01 && !GBA_P4_THUMB_JIT_ALU40_SHIFTS)
      return false;

    switch(subop)
    {
      case 0x00:
        dest = lhs & rhs;
        break;

      case 0x01:
        dest = lhs ^ rhs;
        break;

      case 0x02:
        dest = lhs;
        if(rhs != 0)
        {
          if(rhs > 31)
          {
            sim_flags[2] = (rhs == 32) ? (dest & 0x01) : 0;
            dest = 0;
          }
          else
          {
            sim_flags[2] = (dest >> (32 - rhs)) & 0x01;
            dest <<= rhs;
          }
        }
        break;

      default:
        dest = lhs;
        if(rhs != 0)
        {
          if(rhs > 31)
          {
            sim_flags[2] = (rhs == 32) ? (dest >> 31) : 0;
            dest = 0;
          }
          else
          {
            sim_flags[2] = (dest >> (rhs - 1)) & 0x01;
            dest >>= rhs;
          }
        }
        break;
    }

    gba_p4_thumb_jit_sim_nz(dest, sim_flags);
    sim_regs[rd] = dest;
    sim_regs[REG_PC] += 2;
    return true;
  }

  if(GBA_P4_THUMB_JIT_ALU42 && top == 0x42)
  {
    u32 rd = opcode & 0x07;
    u32 rs = (opcode >> 3) & 0x07;
    u32 subop = (opcode >> 6) & 0x03;
    u32 lhs = sim_regs[rd];
    u32 rhs = sim_regs[rs];

    switch(subop)
    {
      case 0x00:
        dest = lhs & rhs;
        gba_p4_thumb_jit_sim_nz(dest, sim_flags);
        break;

      case 0x01:
        dest = 0 - rhs;
        gba_p4_thumb_jit_sim_sub_flags(0, rhs, dest, sim_flags);
        sim_regs[rd] = dest;
        break;

      case 0x02:
        dest = lhs - rhs;
        gba_p4_thumb_jit_sim_sub_flags(lhs, rhs, dest, sim_flags);
        break;

      default:
        dest = lhs + rhs;
        gba_p4_thumb_jit_sim_add_flags(lhs, rhs, dest, sim_flags);
        break;
    }

    sim_regs[REG_PC] += 2;
    return true;
  }

  if(GBA_P4_THUMB_JIT_ALU43_LOGIC && top == 0x43)
  {
    u32 rd = opcode & 0x07;
    u32 rs = (opcode >> 3) & 0x07;
    u32 subop = (opcode >> 6) & 0x03;
    u32 lhs = sim_regs[rd];
    u32 rhs = sim_regs[rs];

    switch(subop)
    {
      case 0x00:
        dest = lhs | rhs;
        break;

      case 0x01:
        dest = lhs * rhs;
        break;

      case 0x02:
        dest = lhs & ~rhs;
        break;

      case 0x03:
        dest = ~rhs;
        break;

      default:
        return false;
    }

    gba_p4_thumb_jit_sim_nz(dest, sim_flags);
    sim_regs[rd] = dest;
    sim_regs[REG_PC] += 2;
    return true;
  }

  if(GBA_P4_THUMB_JIT_HIREG_ALU && (top == 0x44 || top == 0x45))
  {
    u32 rs = (opcode >> 3) & 0x0F;
    u32 rd = ((opcode >> 4) & 0x08) | (opcode & 0x07);
    if(top == 0x44 && rd == REG_PC)
      return false;

    u32 lhs = (rd == REG_PC) ? (sim_regs[REG_PC] + 4) : sim_regs[rd];
    u32 rhs = (rs == REG_PC) ? (sim_regs[REG_PC] + 4) : sim_regs[rs];
    dest = (top == 0x44) ? (lhs + rhs) : (lhs - rhs);
    if(top == 0x44)
      sim_regs[rd] = dest;
    else
      gba_p4_thumb_jit_sim_sub_flags(lhs, rhs, dest, sim_flags);

    sim_regs[REG_PC] += 2;
    return true;
  }

  if(top >= 0x48 && top <= 0x4F)
  {
    u32 rd = top & 0x07;
    u32 address = (sim_regs[REG_PC] & ~0x02U) + 4 + ((opcode & 0xFF) * 4);
    u32 value;
    if(!gba_p4_thumb_jit_read_literal32(address, &value))
      return false;

    sim_regs[rd] = value;
    sim_regs[REG_PC] += 2;
    return true;
  }

  if(top >= 0xD0 && top <= 0xDD)
  {
    bool taken;
    switch(top & 0x0F)
    {
      case 0x0: taken = sim_flags[1] == 1; break;
      case 0x1: taken = sim_flags[1] == 0; break;
      case 0x2: taken = sim_flags[2] == 1; break;
      case 0x3: taken = sim_flags[2] == 0; break;
      case 0x4: taken = sim_flags[0] == 1; break;
      case 0x5: taken = sim_flags[0] == 0; break;
      case 0x6: taken = sim_flags[3] == 1; break;
      case 0x7: taken = sim_flags[3] == 0; break;
      case 0x8: taken = sim_flags[2] & (sim_flags[1] ^ 1); break;
      case 0x9: taken = (sim_flags[2] == 0) | sim_flags[1]; break;
      case 0xA: taken = sim_flags[0] == sim_flags[3]; break;
      case 0xB: taken = sim_flags[0] != sim_flags[3]; break;
      case 0xC: taken = (sim_flags[1] == 0) & (sim_flags[0] == sim_flags[3]); break;
      default:  taken = sim_flags[1] | (sim_flags[0] != sim_flags[3]); break;
    }

    s32 offset = (s8)(opcode & 0xFF);
    sim_regs[REG_PC] += taken ? ((offset * 2) + 4) : 2;
    return true;
  }

  if(top >= 0xE0 && top <= 0xE7)
  {
    u32 offset = opcode & 0x07FF;
    sim_regs[REG_PC] += ((s32)(offset << 21) >> 20) + 4;
    return true;
  }

  if(top >= 0xF0 && top <= 0xFF)
  {
    u32 offset = opcode & 0x07FF;
    if(top <= 0xF7)
    {
      sim_regs[REG_LR] = sim_regs[REG_PC] + 4 + ((s32)(offset << 21) >> 9);
      sim_regs[REG_PC] += 2;
    }
    else
    {
      u32 newpc = sim_regs[REG_LR] + (offset * 2);
      sim_regs[REG_LR] = sim_regs[REG_PC] + 3;
      sim_regs[REG_PC] = newpc;
    }
    return true;
  }

  if(GBA_P4_THUMB_JIT_HIREG_MOV && top == 0x46)
  {
    u32 rs = (opcode >> 3) & 0x0F;
    u32 rd = ((opcode >> 4) & 0x08) | (opcode & 0x07);

    u32 value = (rs == REG_PC) ? (sim_regs[REG_PC] + 4) : sim_regs[rs];
    if(rd == REG_PC)
      sim_regs[REG_PC] = value & ~1U;
    else
    {
      sim_regs[rd] = value;
      sim_regs[REG_PC] += 2;
    }
    return true;
  }

  if(top == 0x47)
  {
    u32 rs = (opcode >> 3) & 0x0F;
    u32 src = (rs == REG_PC) ? (sim_regs[REG_PC] + 4) : sim_regs[rs];
    if(src & 0x01)
      sim_regs[REG_PC] = src - 1;
    else
    {
      sim_regs[REG_PC] = src;
      sim_regs[REG_CPSR] &= ~0x20U;
    }
    return true;
  }

  if(top >= 0xA0 && top <= 0xAF)
  {
    u32 rd = top & 0x07;
    u32 base = (top < 0xA8) ? ((sim_regs[REG_PC] & ~0x02U) + 4) :
        sim_regs[REG_SP];
    sim_regs[rd] = base + ((opcode & 0xFF) * 4);
    sim_regs[REG_PC] += 2;
    return true;
  }

  if(top >= 0xB0 && top <= 0xB3)
  {
    u32 imm = (opcode & 0x7F) * 4;
    if((opcode >> 7) & 0x01)
      sim_regs[REG_SP] -= imm;
    else
      sim_regs[REG_SP] += imm;
    sim_regs[REG_PC] += 2;
    return true;
  }

  if(GBA_P4_THUMB_JIT_STACK_READS && top >= 0x98 && top <= 0x9F)
  {
    u32 rd = top & 0x07;
    u32 address = (sim_regs[REG_SP] + ((opcode & 0xFF) * 4)) & ~3U;
    if((address >> 24) != 0x03)
      return false;

    sim_regs[rd] = readaddress32(iwram, (address & 0x7FFF) + 0x8000);
    sim_regs[REG_PC] += 2;
    return true;
  }

  if(GBA_P4_THUMB_JIT_STACK_WRITES && top >= 0x90 && top <= 0x97)
  {
    u32 rd = top & 0x07;
    u32 address = (sim_regs[REG_SP] + ((opcode & 0xFF) * 4)) & ~3U;
    if((address >> 24) != 0x03)
      return false;

    address32(iwram, (address & 0x7FFF) + 0x8000) = eswap32(sim_regs[rd]);
    sim_regs[REG_PC] += 2;
    return true;
  }

  if(GBA_P4_THUMB_JIT_STACK_WRITES && (top == 0xB4 || top == 0xB5))
  {
    u32 reglist = opcode & 0xFF;
    u32 has_lr = top == 0xB5;
    u32 numops = bit_count[reglist] + has_lr;
    u32 address = (sim_regs[REG_SP] - numops * 4) & ~3U;
    if(!numops || (address >> 24) != 0x03 ||
       ((address & 0x7FFF) + numops * 4) > 0x8000)
      return false;

    sim_regs[REG_SP] = address;
    u32 offset = 0;
    for(u32 i = 0; i < 8; i++)
    {
      if(reglist & (1U << i))
      {
        address32(iwram, ((address + offset) & 0x7FFF) + 0x8000) =
            eswap32(sim_regs[i]);
        offset += 4;
      }
    }
    if(has_lr)
      address32(iwram, ((address + offset) & 0x7FFF) + 0x8000) =
          eswap32(sim_regs[REG_LR]);
    sim_regs[REG_PC] += 2;
    return true;
  }

  if(GBA_P4_THUMB_JIT_STACK_READS && top == 0xBC)
  {
    u32 reglist = opcode & 0xFF;
    u32 address = sim_regs[REG_SP] & ~3U;
    if(!reglist || (address >> 24) != 0x03)
      return false;

    u32 offset = 0;
    for(u32 i = 0; i < 8; i++)
    {
      if(reglist & (1U << i))
      {
        sim_regs[i] = readaddress32(iwram, ((address + offset) & 0x7FFF) + 0x8000);
        offset += 4;
      }
    }
    sim_regs[REG_SP] += offset;
    sim_regs[REG_PC] += 2;
    return true;
  }

  if(gba_p4_thumb_jit_wram_store_opcode(opcode))
  {
    u32 rb = (opcode >> 3) & 0x07;
    u32 rd = opcode & 0x07;
    u32 address;

    if(top >= 0x60 && top <= 0x67)
    {
      address = sim_regs[rb] + (((opcode >> 6) & 0x1F) * 4);
      if(address & 0x03)
        return false;
    }
    else if(top >= 0x80 && top <= 0x87)
    {
      address = sim_regs[rb] + (((opcode >> 6) & 0x1F) * 2);
      if(address & 0x01)
        return false;
    }
    else
      address = sim_regs[rb] + ((opcode >> 6) & 0x1F);

    switch(address >> 24)
    {
      case 0x02:
        if(top >= 0x60 && top <= 0x67)
          address32(ewram, address & 0x3FFFF) = eswap32(sim_regs[rd]);
        else if(top >= 0x80 && top <= 0x87)
          address16(ewram, address & 0x3FFFF) = eswap16((u16)sim_regs[rd]);
        else
          ewram[address & 0x3FFFF] = (u8)sim_regs[rd];
        break;

      case 0x03:
        if(top >= 0x60 && top <= 0x67)
          address32(iwram, (address & 0x7FFF) + 0x8000) = eswap32(sim_regs[rd]);
        else if(top >= 0x80 && top <= 0x87)
          address16(iwram, (address & 0x7FFF) + 0x8000) = eswap16((u16)sim_regs[rd]);
        else
          iwram[(address & 0x7FFF) + 0x8000] = (u8)sim_regs[rd];
        break;

      default:
        return false;
    }

    sim_regs[REG_PC] += 2;
    return true;
  }

  if(gba_p4_thumb_jit_wram_load_opcode(opcode))
  {
    u32 rb = (opcode >> 3) & 0x07;
    u32 rd = opcode & 0x07;
    u32 address;

    if(top >= 0x68 && top <= 0x6F)
    {
      address = sim_regs[rb] + (((opcode >> 6) & 0x1F) * 4);
      if(address & 0x03)
        return false;
    }
    else if(top >= 0x88 && top <= 0x8F)
    {
      address = sim_regs[rb] + (((opcode >> 6) & 0x1F) * 2);
      if(address & 0x01)
        return false;
    }
    else
      address = sim_regs[rb] + ((opcode >> 6) & 0x1F);

    switch(address >> 24)
    {
      case 0x02:
        if(top >= 0x68 && top <= 0x6F)
          sim_regs[rd] = readaddress32(ewram, address & 0x3FFFF);
        else if(top >= 0x88 && top <= 0x8F)
          sim_regs[rd] = readaddress16(ewram, address & 0x3FFFF);
        else
          sim_regs[rd] = ewram[address & 0x3FFFF];
        break;

      case 0x03:
        if(top >= 0x68 && top <= 0x6F)
          sim_regs[rd] = readaddress32(iwram, (address & 0x7FFF) + 0x8000);
        else if(top >= 0x88 && top <= 0x8F)
          sim_regs[rd] = readaddress16(iwram, (address & 0x7FFF) + 0x8000);
        else
          sim_regs[rd] = iwram[(address & 0x7FFF) + 0x8000];
        break;

      default:
        return false;
    }

    sim_regs[REG_PC] += 2;
    return true;
  }

  return false;
}

static inline bool gba_p4_thumb_jit_record_fail(gba_p4_thumb_jit_entry_t *entry,
    u32 reason, u32 index, u32 expected, u32 actual)
{
  gba_thumb_jit_fail_pc = entry ? entry->pc : 0;
  gba_thumb_jit_fail_opcode = 0;
  if(entry && entry->op_count)
  {
    gba_thumb_jit_fail_opcode =
        (index < entry->op_count) ? entry->opcodes[index] : entry->opcodes[0];
    for(u32 i = 0; i < entry->op_count; i++)
      gba_thumb_jit_fail_histogram[(entry->opcodes[i] >> 8) & 0xFF]++;
  }
  gba_thumb_jit_fail_index = index;
  gba_thumb_jit_fail_expected = expected;
  gba_thumb_jit_fail_actual = actual;
  gba_thumb_jit_fail_reason = reason;
  if(reason != 1)
    gba_p4_thumb_jit_report_fault(0x4A000000U | reason,
        entry ? entry->pc : reg[REG_PC]);
  return false;
}

static __attribute__((noinline, cold)) bool gba_p4_thumb_jit_validate_and_commit(
    gba_p4_thumb_jit_entry_t *entry,
    u32 &n_flag, u32 &z_flag, u32 &c_flag, u32 &v_flag, u32 *jit_ret)
{
  struct
  {
    u32 pre;
    u32 regs[REG_ARCH_COUNT];
    u32 post;
  } jit_state = {0x4A525047U, {0}, 0x4A52504FU};

  u32 sim_regs[REG_ARCH_COUNT];
  u32 sim_flags[4] = {n_flag, z_flag, c_flag, v_flag};

  memcpy(jit_state.regs, reg, sizeof(sim_regs));
  memcpy(sim_regs, reg, sizeof(sim_regs));

  for(u32 i = 0; i < entry->op_count; i++)
  {
    if(!gba_p4_thumb_jit_simulate_one(entry->opcodes[i], sim_regs, sim_flags))
      return gba_p4_thumb_jit_record_fail(entry, 1, i, 0, 0);
  }

  struct
  {
    u32 pre;
    u32 flags[4];
    u32 post;
  } jit_flags = {0x4A495450U, {n_flag, z_flag, c_flag, v_flag}, 0x4A49544FU};

  u32 executed = entry->fn(jit_state.regs, jit_flags.flags);
  if(jit_state.pre != 0x4A525047U)
    return gba_p4_thumb_jit_record_fail(entry, 2, 0, 0x4A525047U, jit_state.pre);
  if(jit_state.post != 0x4A52504FU)
    return gba_p4_thumb_jit_record_fail(entry, 3, 0, 0x4A52504FU, jit_state.post);
  if(jit_flags.pre != 0x4A495450U)
    return gba_p4_thumb_jit_record_fail(entry, 4, 0, 0x4A495450U, jit_flags.pre);
  if(jit_flags.post != 0x4A49544FU)
    return gba_p4_thumb_jit_record_fail(entry, 5, 0, 0x4A49544FU, jit_flags.post);

  u32 executed_ops = executed & GBA_P4_THUMB_JIT_RET_OPS_MASK;
  u32 executed_extra = (executed >> GBA_P4_THUMB_JIT_RET_EXTRA_SHIFT) &
      GBA_P4_THUMB_JIT_RET_EXTRA_MASK;

  if(executed_ops != entry->op_count)
  {
    // Returning early is the emitter's designed escape, not a model mismatch: a
    // WRAM access whose address turns out to sit outside EWRAM/IWRAM branches to
    // the bail stub, which parks PC on the offending instruction and reports the
    // ops retired so far. `gba_p4_thumb_jit_execute_committed` has always
    // accepted that. Treating it as a fault here disabled both accelerators on
    // the first pointer that left WRAM -- and since JIT_DEBUG forces this path
    // for every execution, trusted blocks never reached the tolerant one.
    if(!entry->can_bail || executed_ops >= entry->op_count)
      return gba_p4_thumb_jit_record_fail(entry, 6, 0, entry->op_count,
          executed_ops);
    if(executed_extra)
      return gba_p4_thumb_jit_record_fail(entry, 7, 0, 0, executed_extra);

    // The reference pass above modelled the whole block, so it has run past the
    // bail point. Rebuild it over just the retired instructions so the register
    // and flag comparison below comes from a matching amount of work.
    memcpy(sim_regs, reg, sizeof(sim_regs));
    sim_flags[0] = n_flag;
    sim_flags[1] = z_flag;
    sim_flags[2] = c_flag;
    sim_flags[3] = v_flag;
    for(u32 i = 0; i < executed_ops; i++)
    {
      if(!gba_p4_thumb_jit_simulate_one(entry->opcodes[i], sim_regs, sim_flags))
        return gba_p4_thumb_jit_record_fail(entry, 1, i, 0, 0);
    }
  }
  else if(executed_extra != entry->extra_cycles)
    return gba_p4_thumb_jit_record_fail(entry, 7, 0, entry->extra_cycles,
        executed_extra);

  for(u32 i = 0; i < REG_ARCH_COUNT; i++)
  {
    if(jit_state.regs[i] != sim_regs[i])
      return gba_p4_thumb_jit_record_fail(entry, 0x10 + i, i,
          sim_regs[i], jit_state.regs[i]);
  }

  for(u32 i = 0; i < 4; i++)
  {
    if((jit_flags.flags[i] & 1) != (sim_flags[i] & 1))
      return gba_p4_thumb_jit_record_fail(entry, 0x40 + i, i,
          sim_flags[i] & 1, jit_flags.flags[i] & 1);
  }

  memcpy(reg, jit_state.regs, sizeof(sim_regs));
  n_flag = jit_flags.flags[0] & 1;
  z_flag = jit_flags.flags[1] & 1;
  c_flag = jit_flags.flags[2] & 1;
  v_flag = jit_flags.flags[3] & 1;
  entry->validated++;
  gba_thumb_jit_validate_passes++;
  if(jit_ret)
    *jit_ret = executed;
  return true;
}

static bool gba_p4_thumb_jit_execute_committed(gba_p4_thumb_jit_entry_t *entry,
    u32 &n_flag, u32 &z_flag, u32 &c_flag, u32 &v_flag, u32 *jit_ret)
{
  u32 flags[4] = {n_flag, z_flag, c_flag, v_flag};
  u32 executed = entry->fn(reg, flags);
  u32 executed_ops = executed & GBA_P4_THUMB_JIT_RET_OPS_MASK;
  u32 extra_cycles = (executed >> GBA_P4_THUMB_JIT_RET_EXTRA_SHIFT) &
      GBA_P4_THUMB_JIT_RET_EXTRA_MASK;

  if(executed_ops != entry->op_count)
  {
    if(!entry->can_bail || executed_ops >= entry->op_count)
      return gba_p4_thumb_jit_record_fail(entry, 6, 0, entry->op_count,
          executed_ops);

    if(extra_cycles)
      return gba_p4_thumb_jit_record_fail(entry, 7, 0, 0, extra_cycles);
  }
  else if(extra_cycles != entry->extra_cycles)
    return gba_p4_thumb_jit_record_fail(entry, 7, 0, entry->extra_cycles,
        extra_cycles);

  n_flag = flags[0] & 1;
  z_flag = flags[1] & 1;
  c_flag = flags[2] & 1;
  v_flag = flags[3] & 1;
  if(jit_ret)
    *jit_ret = executed;
  return true;
}

static bool gba_p4_thumb_jit_emit_store_flag(gba_p4_rv_emit_t *emit,
    u32 flag_index, u32 value_reg)
{
  return gba_p4_emit(emit, rv_sw(value_reg, RV_A1, flag_index * sizeof(u32)));
}

static bool gba_p4_thumb_jit_emit_nz(gba_p4_rv_emit_t *emit, u32 value_reg)
{
  return gba_p4_emit(emit, rv_srli(RV_T3, value_reg, 31)) &&
         gba_p4_thumb_jit_emit_store_flag(emit, 0, RV_T3) &&
         gba_p4_emit(emit, rv_sltiu(RV_T3, value_reg, 1)) &&
         gba_p4_thumb_jit_emit_store_flag(emit, 1, RV_T3);
}

static bool gba_p4_thumb_jit_emit_add_flags(gba_p4_rv_emit_t *emit)
{
  return gba_p4_thumb_jit_emit_nz(emit, RV_T2) &&
         gba_p4_emit(emit, rv_sltu(RV_T3, RV_T2, RV_T1)) &&
         gba_p4_thumb_jit_emit_store_flag(emit, 2, RV_T3) &&
         gba_p4_emit(emit, rv_xor(RV_T3, RV_T0, RV_T1)) &&
         gba_p4_emit(emit, rv_xori(RV_T3, RV_T3, -1)) &&
         gba_p4_emit(emit, rv_xor(RV_T4, RV_T0, RV_T2)) &&
         gba_p4_emit(emit, rv_and(RV_T3, RV_T3, RV_T4)) &&
         gba_p4_emit(emit, rv_srli(RV_T3, RV_T3, 31)) &&
         gba_p4_thumb_jit_emit_store_flag(emit, 3, RV_T3);
}

static bool gba_p4_thumb_jit_emit_sub_flags(gba_p4_rv_emit_t *emit)
{
  return gba_p4_thumb_jit_emit_nz(emit, RV_T2) &&
         gba_p4_emit(emit, rv_sltu(RV_T3, RV_T0, RV_T1)) &&
         gba_p4_emit(emit, rv_xori(RV_T3, RV_T3, 1)) &&
         gba_p4_thumb_jit_emit_store_flag(emit, 2, RV_T3) &&
         gba_p4_emit(emit, rv_xor(RV_T3, RV_T0, RV_T1)) &&
         gba_p4_emit(emit, rv_xor(RV_T4, RV_T0, RV_T2)) &&
         gba_p4_emit(emit, rv_and(RV_T3, RV_T3, RV_T4)) &&
         gba_p4_emit(emit, rv_srli(RV_T3, RV_T3, 31)) &&
         gba_p4_thumb_jit_emit_store_flag(emit, 3, RV_T3);
}

static inline void gba_p4_thumb_jit_cache_init(gba_p4_rv_emit_t *emit)
{
  memset(emit->cache_gba_reg, 0xFF, sizeof(emit->cache_gba_reg));
  memset(emit->cache_dirty, 0, sizeof(emit->cache_dirty));
  emit->cache_next = 0;
}

static inline bool gba_p4_thumb_jit_cacheable_reg(u32 gba_reg)
{
  return gba_reg < 8;
}

static inline u32 gba_p4_thumb_jit_cache_rv_reg(u32 slot)
{
  static const u8 regs[] = { RV_A2, RV_A3, RV_A4, RV_A5, RV_A6, RV_A7 };
  return regs[slot];
}

static int gba_p4_thumb_jit_cache_find(gba_p4_rv_emit_t *emit, u32 gba_reg)
{
  for(u32 i = 0; i < sizeof(emit->cache_gba_reg); i++)
  {
    if(emit->cache_gba_reg[i] == gba_reg)
      return (int)i;
  }
  return -1;
}

static bool gba_p4_thumb_jit_cache_flush_slot(gba_p4_rv_emit_t *emit, u32 slot)
{
  if(slot >= sizeof(emit->cache_gba_reg) || emit->cache_gba_reg[slot] == 0xFF)
    return true;

  if(emit->cache_dirty[slot])
  {
    if(!gba_p4_emit(emit, rv_sw(gba_p4_thumb_jit_cache_rv_reg(slot), RV_A0,
          emit->cache_gba_reg[slot] * sizeof(u32))))
      return false;
    emit->cache_dirty[slot] = 0;
  }
  return true;
}

static bool gba_p4_thumb_jit_cache_flush_all(gba_p4_rv_emit_t *emit)
{
  for(u32 i = 0; i < sizeof(emit->cache_gba_reg); i++)
  {
    if(!gba_p4_thumb_jit_cache_flush_slot(emit, i))
      return false;
  }
  return true;
}

static bool gba_p4_thumb_jit_cache_emit_flush_all_preserve(gba_p4_rv_emit_t *emit)
{
  for(u32 i = 0; i < sizeof(emit->cache_gba_reg); i++)
  {
    if(emit->cache_gba_reg[i] != 0xFF && emit->cache_dirty[i])
    {
      if(!gba_p4_emit(emit, rv_sw(gba_p4_thumb_jit_cache_rv_reg(i), RV_A0,
            emit->cache_gba_reg[i] * sizeof(u32))))
        return false;
    }
  }
  return true;
}

static int gba_p4_thumb_jit_cache_alloc(gba_p4_rv_emit_t *emit, u32 gba_reg,
    bool load_existing)
{
  int slot = gba_p4_thumb_jit_cache_find(emit, gba_reg);
  if(slot >= 0)
    return slot;

  for(u32 i = 0; i < sizeof(emit->cache_gba_reg); i++)
  {
    if(emit->cache_gba_reg[i] == 0xFF)
    {
      slot = (int)i;
      break;
    }
  }

  if(slot < 0)
  {
    slot = emit->cache_next++ % sizeof(emit->cache_gba_reg);
    if(!gba_p4_thumb_jit_cache_flush_slot(emit, (u32)slot))
      return -1;
  }

  emit->cache_gba_reg[slot] = (u8)gba_reg;
  emit->cache_dirty[slot] = 0;
  if(load_existing &&
     !gba_p4_emit(emit, rv_lw(gba_p4_thumb_jit_cache_rv_reg((u32)slot), RV_A0,
          gba_reg * sizeof(u32))))
  {
    emit->cache_gba_reg[slot] = 0xFF;
    return -1;
  }
  return slot;
}

static inline bool gba_p4_thumb_jit_load_reg(gba_p4_rv_emit_t *emit,
    u32 rv_reg, u32 gba_reg)
{
  if(gba_p4_thumb_jit_cacheable_reg(gba_reg))
  {
    int slot = gba_p4_thumb_jit_cache_alloc(emit, gba_reg, true);
    return slot >= 0 &&
        gba_p4_emit(emit, rv_addi(rv_reg,
            gba_p4_thumb_jit_cache_rv_reg((u32)slot), 0));
  }

  return gba_p4_emit(emit, rv_lw(rv_reg, RV_A0, gba_reg * sizeof(u32)));
}

static inline bool gba_p4_thumb_jit_store_reg(gba_p4_rv_emit_t *emit,
    u32 gba_reg, u32 rv_reg)
{
  if(gba_p4_thumb_jit_cacheable_reg(gba_reg))
  {
    int slot = gba_p4_thumb_jit_cache_alloc(emit, gba_reg, false);
    return slot >= 0 &&
        gba_p4_emit(emit, rv_addi(gba_p4_thumb_jit_cache_rv_reg((u32)slot),
            rv_reg, 0)) &&
        (emit->cache_dirty[slot] = 1);
  }

  return gba_p4_emit(emit, rv_sw(rv_reg, RV_A0, gba_reg * sizeof(u32)));
}

static bool gba_p4_thumb_jit_emit_low_op(gba_p4_rv_emit_t *emit, u32 opcode)
{
  u32 top = (opcode >> 8) & 0xFF;
  u32 rd = opcode & 0x07;
  u32 rs = (opcode >> 3) & 0x07;

  if(top < 0x18)
  {
    u32 offset = (opcode >> 6) & 0x1F;
    u32 shift_op = (opcode >> 11) & 0x03;

    if(!gba_p4_thumb_jit_load_reg(emit, RV_T0, rs))
      return false;

    switch(shift_op)
    {
      case 0x00:
        if(!gba_p4_emit(emit, rv_slli(RV_T2, RV_T0, offset)))
          return false;
        if(offset)
        {
          if(!(gba_p4_emit(emit, rv_srli(RV_T3, RV_T0, 32 - offset)) &&
               gba_p4_emit(emit, rv_andi(RV_T3, RV_T3, 1)) &&
               gba_p4_thumb_jit_emit_store_flag(emit, 2, RV_T3)))
            return false;
        }
        break;

      case 0x01:
        if(offset == 0)
        {
          if(!(gba_p4_emit(emit, rv_srli(RV_T3, RV_T0, 31)) &&
               gba_p4_thumb_jit_emit_store_flag(emit, 2, RV_T3) &&
               gba_p4_emit(emit, rv_addi(RV_T2, RV_ZERO, 0))))
            return false;
        }
        else if(!(gba_p4_emit(emit, rv_srli(RV_T3, RV_T0, offset - 1)) &&
                  gba_p4_emit(emit, rv_andi(RV_T3, RV_T3, 1)) &&
                  gba_p4_thumb_jit_emit_store_flag(emit, 2, RV_T3) &&
                  gba_p4_emit(emit, rv_srli(RV_T2, RV_T0, offset))))
          return false;
        break;

      default:
        if(offset == 0)
        {
          if(!(gba_p4_emit(emit, rv_srai(RV_T2, RV_T0, 31)) &&
               gba_p4_emit(emit, rv_andi(RV_T3, RV_T2, 1)) &&
               gba_p4_thumb_jit_emit_store_flag(emit, 2, RV_T3)))
            return false;
        }
        else if(!(gba_p4_emit(emit, rv_srli(RV_T3, RV_T0, offset - 1)) &&
                  gba_p4_emit(emit, rv_andi(RV_T3, RV_T3, 1)) &&
                  gba_p4_thumb_jit_emit_store_flag(emit, 2, RV_T3) &&
                  gba_p4_emit(emit, rv_srai(RV_T2, RV_T0, offset))))
          return false;
        break;
    }

    return gba_p4_thumb_jit_store_reg(emit, rd, RV_T2) &&
           gba_p4_thumb_jit_emit_nz(emit, RV_T2);
  }

  u32 rhs_field = (opcode >> 6) & 0x07;
  bool is_imm = (opcode & 0x0400) != 0;
  bool is_sub = (opcode & 0x0200) != 0;

  if(!(gba_p4_thumb_jit_load_reg(emit, RV_T0, rs) &&
       (is_imm ? gba_p4_emit(emit, rv_addi(RV_T1, RV_ZERO, rhs_field)) :
                 gba_p4_thumb_jit_load_reg(emit, RV_T1, rhs_field))))
    return false;

  if(is_sub)
    return gba_p4_emit(emit, rv_sub(RV_T2, RV_T0, RV_T1)) &&
           gba_p4_thumb_jit_store_reg(emit, rd, RV_T2) &&
           gba_p4_thumb_jit_emit_sub_flags(emit);

  return gba_p4_emit(emit, rv_add(RV_T2, RV_T0, RV_T1)) &&
         gba_p4_thumb_jit_store_reg(emit, rd, RV_T2) &&
         gba_p4_thumb_jit_emit_add_flags(emit);
}

static bool gba_p4_thumb_jit_emit_imm_op(gba_p4_rv_emit_t *emit, u32 opcode)
{
  u32 top = (opcode >> 8) & 0xFF;
  u32 rd = top & 0x07;
  u32 imm = opcode & 0xFF;
  u32 op = top >> 3;

  if(op == 0x04)
  {
    /* MOV r0..7, imm: writes N/Z only. */
    int slot = gba_p4_thumb_jit_cache_alloc(emit, rd, false);
    if(slot < 0)
      return false;

    u32 dst = gba_p4_thumb_jit_cache_rv_reg((u32)slot);
    emit->cache_dirty[slot] = 1;
    return gba_p4_emit(emit, rv_addi(dst, RV_ZERO, imm)) &&
           gba_p4_emit(emit, rv_addi(RV_T3, RV_ZERO, 0)) &&
           gba_p4_thumb_jit_emit_store_flag(emit, 0, RV_T3) &&
           gba_p4_emit(emit, rv_addi(RV_T3, RV_ZERO, imm == 0)) &&
           gba_p4_thumb_jit_emit_store_flag(emit, 1, RV_T3);
  }

  if(!(gba_p4_thumb_jit_load_reg(emit, RV_T0, rd) &&
       gba_p4_emit(emit, rv_addi(RV_T1, RV_ZERO, imm))))
    return false;

  if(op == 0x06)
  {
    /* ADD r0..7, imm */
    if(!(gba_p4_emit(emit, rv_add(RV_T2, RV_T0, RV_T1)) &&
         gba_p4_thumb_jit_store_reg(emit, rd, RV_T2) &&
         gba_p4_thumb_jit_emit_add_flags(emit)))
      return false;
    return true;
  }

  /* CMP/SUB r0..7, imm */
  if(!(gba_p4_emit(emit, rv_sub(RV_T2, RV_T0, RV_T1)) &&
       (op == 0x05 || gba_p4_thumb_jit_store_reg(emit, rd, RV_T2)) &&
       gba_p4_thumb_jit_emit_sub_flags(emit)))
    return false;

  return true;
}

static bool gba_p4_thumb_jit_emit_alu42_op(gba_p4_rv_emit_t *emit, u32 opcode)
{
  u32 rd = opcode & 0x07;
  u32 rs = (opcode >> 3) & 0x07;
  u32 subop = (opcode >> 6) & 0x03;

  if(!(gba_p4_thumb_jit_load_reg(emit, RV_T0, rd) &&
       gba_p4_thumb_jit_load_reg(emit, RV_T1, rs)))
    return false;

  switch(subop)
  {
    case 0x00:
      /* TST rd, rs */
      return gba_p4_emit(emit, rv_and(RV_T2, RV_T0, RV_T1)) &&
             gba_p4_thumb_jit_emit_nz(emit, RV_T2);

    case 0x01:
      /* NEG rd, rs */
      return gba_p4_emit(emit, rv_addi(RV_T0, RV_ZERO, 0)) &&
             gba_p4_emit(emit, rv_sub(RV_T2, RV_T0, RV_T1)) &&
             gba_p4_thumb_jit_store_reg(emit, rd, RV_T2) &&
             gba_p4_thumb_jit_emit_sub_flags(emit);

    case 0x02:
      /* CMP rd, rs */
      return gba_p4_emit(emit, rv_sub(RV_T2, RV_T0, RV_T1)) &&
             gba_p4_thumb_jit_emit_sub_flags(emit);

    default:
      /* CMN rd, rs */
      return gba_p4_emit(emit, rv_add(RV_T2, RV_T0, RV_T1)) &&
             gba_p4_thumb_jit_emit_add_flags(emit);
  }
}

static bool gba_p4_thumb_jit_emit_alu40_logic_op(gba_p4_rv_emit_t *emit, u32 opcode)
{
  u32 rd = opcode & 0x07;
  u32 rs = (opcode >> 3) & 0x07;
  u32 subop = (opcode >> 6) & 0x03;

  if(subop > 0x01 && !GBA_P4_THUMB_JIT_ALU40_SHIFTS)
    return false;

  if(!(gba_p4_thumb_jit_load_reg(emit, RV_T0, rd) &&
       gba_p4_thumb_jit_load_reg(emit, RV_T1, rs)))
    return false;

  if(subop == 0x00)
  {
    if(!gba_p4_emit(emit, rv_and(RV_T2, RV_T0, RV_T1)))
      return false;
  }
  else if(subop == 0x01)
  {
    if(!gba_p4_emit(emit, rv_xor(RV_T2, RV_T0, RV_T1)))
      return false;
  }

  if(subop >= 0x02)
  {
    bool is_lsr = subop == 0x03;

    /*
     * Register shifts preserve C when shift count is zero. Emit branchless
     * masks so the generated block remains safe for validation/reuse.
     */
    if(!(gba_p4_emit(emit, rv_sltu(RV_T3, RV_ZERO, RV_T1)) &&
         gba_p4_emit(emit, rv_sub(RV_T3, RV_ZERO, RV_T3)) &&
         gba_p4_emit(emit, rv_addi(RV_T4, RV_ZERO, 32)) &&
         gba_p4_emit(emit, rv_sltu(RV_T4, RV_T1, RV_T4)) &&
         gba_p4_emit(emit, rv_and(RV_T4, RV_T4, RV_T3)) &&
         gba_p4_emit(emit, rv_sub(RV_T4, RV_ZERO, RV_T4)) &&
         gba_p4_emit(emit, is_lsr ? rv_srl(RV_T2, RV_T0, RV_T1) :
                                    rv_sll(RV_T2, RV_T0, RV_T1)) &&
         gba_p4_emit(emit, rv_and(RV_T2, RV_T2, RV_T4)) &&
         gba_p4_emit(emit, rv_xori(RV_T5, RV_T3, -1)) &&
         gba_p4_emit(emit, rv_and(RV_T5, RV_T5, RV_T0)) &&
         gba_p4_emit(emit, rv_or(RV_T2, RV_T2, RV_T5))))
      return false;

    if(!gba_p4_thumb_jit_store_reg(emit, rd, RV_T2))
      return false;

    if(is_lsr)
    {
      if(!(gba_p4_emit(emit, rv_addi(RV_T5, RV_T1, -1)) &&
           gba_p4_emit(emit, rv_srl(RV_T5, RV_T0, RV_T5)) &&
           gba_p4_emit(emit, rv_andi(RV_T5, RV_T5, 1)) &&
           gba_p4_emit(emit, rv_and(RV_T5, RV_T5, RV_T4)) &&
           gba_p4_emit(emit, rv_xori(RV_T6, RV_T1, 32)) &&
           gba_p4_emit(emit, rv_sltiu(RV_T6, RV_T6, 1)) &&
           gba_p4_emit(emit, rv_srli(RV_T4, RV_T0, 31)) &&
           gba_p4_emit(emit, rv_and(RV_T6, RV_T6, RV_T4))))
        return false;
    }
    else
    {
      if(!(gba_p4_emit(emit, rv_addi(RV_T5, RV_ZERO, 32)) &&
           gba_p4_emit(emit, rv_sub(RV_T5, RV_T5, RV_T1)) &&
           gba_p4_emit(emit, rv_srl(RV_T5, RV_T0, RV_T5)) &&
           gba_p4_emit(emit, rv_andi(RV_T5, RV_T5, 1)) &&
           gba_p4_emit(emit, rv_and(RV_T5, RV_T5, RV_T4)) &&
           gba_p4_emit(emit, rv_xori(RV_T6, RV_T1, 32)) &&
           gba_p4_emit(emit, rv_sltiu(RV_T6, RV_T6, 1)) &&
           gba_p4_emit(emit, rv_andi(RV_T4, RV_T0, 1)) &&
           gba_p4_emit(emit, rv_and(RV_T6, RV_T6, RV_T4))))
        return false;
    }

    return gba_p4_emit(emit, rv_or(RV_T5, RV_T5, RV_T6)) &&
           gba_p4_emit(emit, rv_lw(RV_T6, RV_A1, 2 * sizeof(u32))) &&
           gba_p4_emit(emit, rv_xori(RV_T4, RV_T3, -1)) &&
           gba_p4_emit(emit, rv_and(RV_T6, RV_T6, RV_T4)) &&
           gba_p4_emit(emit, rv_and(RV_T5, RV_T5, RV_T3)) &&
           gba_p4_emit(emit, rv_or(RV_T5, RV_T5, RV_T6)) &&
           gba_p4_thumb_jit_emit_store_flag(emit, 2, RV_T5) &&
           gba_p4_thumb_jit_emit_nz(emit, RV_T2);
  }

  return gba_p4_thumb_jit_store_reg(emit, rd, RV_T2) &&
         gba_p4_thumb_jit_emit_nz(emit, RV_T2);
}

static bool gba_p4_thumb_jit_emit_alu43_logic_op(gba_p4_rv_emit_t *emit, u32 opcode)
{
  u32 rd = opcode & 0x07;
  u32 rs = (opcode >> 3) & 0x07;
  u32 subop = (opcode >> 6) & 0x03;

  if(!(gba_p4_thumb_jit_load_reg(emit, RV_T0, rd) &&
       gba_p4_thumb_jit_load_reg(emit, RV_T1, rs)))
    return false;

  switch(subop)
  {
    case 0x00:
      if(!gba_p4_emit(emit, rv_or(RV_T2, RV_T0, RV_T1)))
        return false;
      break;

    case 0x01:
      if(!gba_p4_emit(emit, rv_mul(RV_T2, RV_T0, RV_T1)))
        return false;
      break;

    case 0x02:
      if(!(gba_p4_emit(emit, rv_xori(RV_T1, RV_T1, -1)) &&
           gba_p4_emit(emit, rv_and(RV_T2, RV_T0, RV_T1))))
        return false;
      break;

    case 0x03:
      if(!gba_p4_emit(emit, rv_xori(RV_T2, RV_T1, -1)))
        return false;
      break;

    default:
      return false;
  }

  return gba_p4_thumb_jit_store_reg(emit, rd, RV_T2) &&
         gba_p4_thumb_jit_emit_nz(emit, RV_T2);
}

static bool gba_p4_thumb_jit_load_hireg_value(gba_p4_rv_emit_t *emit,
    u32 rv_reg, u32 gba_reg, u32 pc)
{
  if(gba_reg == REG_PC)
    return gba_p4_emit_li32(emit, rv_reg, pc + 4);

  return gba_p4_thumb_jit_load_reg(emit, rv_reg, gba_reg);
}

static bool gba_p4_thumb_jit_emit_hireg_mov_op(gba_p4_rv_emit_t *emit,
    u32 opcode, u32 pc)
{
  u32 rs = (opcode >> 3) & 0x0F;
  u32 rd = ((opcode >> 4) & 0x08) | (opcode & 0x07);

  if(!gba_p4_thumb_jit_load_hireg_value(emit, RV_T2, rs, pc))
    return false;

  if(rd == REG_PC)
  {
    return gba_p4_emit(emit, rv_andi(RV_T2, RV_T2, -2)) &&
           gba_p4_thumb_jit_store_reg(emit, REG_PC, RV_T2);
  }

  return gba_p4_thumb_jit_store_reg(emit, rd, RV_T2);
}

static bool gba_p4_thumb_jit_emit_bx_op(gba_p4_rv_emit_t *emit,
    u32 opcode, u32 pc, u32 op_count)
{
  u32 rs = (opcode >> 3) & 0x0F;
  u32 thumb_path = 0;

  if(!(gba_p4_thumb_jit_load_hireg_value(emit, RV_T0, rs, pc) &&
       gba_p4_emit(emit, rv_andi(RV_T1, RV_T0, 1)) &&
       gba_p4_thumb_jit_cache_flush_all(emit) &&
       gba_p4_emit_branch_placeholder(emit, &thumb_path)))
    return false;

  if(!(gba_p4_thumb_jit_store_reg(emit, REG_PC, RV_T0) &&
       gba_p4_thumb_jit_load_reg(emit, RV_T2, REG_CPSR) &&
       gba_p4_emit(emit, rv_andi(RV_T2, RV_T2, ~0x20)) &&
       gba_p4_thumb_jit_store_reg(emit, REG_CPSR, RV_T2) &&
       gba_p4_emit_li32(emit, RV_A0, op_count | GBA_P4_THUMB_JIT_RET_ARM_SWITCH) &&
       gba_p4_emit(emit, 0x00008067)))
    return false;

  if(!gba_p4_patch_branch(emit, thumb_path, emit->words, RV_T1, RV_ZERO, 0x1))
    return false;

  return gba_p4_emit(emit, rv_addi(RV_T0, RV_T0, -1)) &&
         gba_p4_thumb_jit_store_reg(emit, REG_PC, RV_T0) &&
         gba_p4_emit_li32(emit, RV_A0, op_count) &&
         gba_p4_emit(emit, 0x00008067);
}

static bool gba_p4_thumb_jit_emit_hireg_alu_op(gba_p4_rv_emit_t *emit,
    u32 opcode, u32 pc)
{
  u32 top = (opcode >> 8) & 0xFF;
  u32 rs = (opcode >> 3) & 0x0F;
  u32 rd = ((opcode >> 4) & 0x08) | (opcode & 0x07);

  if(top == 0x44 && rd == REG_PC)
    return false;

  if(!(gba_p4_thumb_jit_load_hireg_value(emit, RV_T0, rd, pc) &&
       gba_p4_thumb_jit_load_hireg_value(emit, RV_T1, rs, pc)))
    return false;

  if(top == 0x44)
  {
    return gba_p4_emit(emit, rv_add(RV_T2, RV_T0, RV_T1)) &&
           gba_p4_thumb_jit_store_reg(emit, rd, RV_T2);
  }

  return gba_p4_emit(emit, rv_sub(RV_T2, RV_T0, RV_T1)) &&
         gba_p4_thumb_jit_emit_sub_flags(emit);
}

static bool gba_p4_thumb_jit_emit_pcldr_op(gba_p4_rv_emit_t *emit,
    u32 opcode, u32 pc)
{
  u32 rd = (opcode >> 8) & 0x07;
  u32 address = (pc & ~0x02U) + 4 + ((opcode & 0xFF) * 4);
  u32 value;

  if(!gba_p4_thumb_jit_read_literal32(address, &value))
    return false;

  return gba_p4_emit_li32(emit, RV_T2, value) &&
         gba_p4_thumb_jit_store_reg(emit, rd, RV_T2);
}

static bool gba_p4_thumb_jit_emit_bl_low_op(gba_p4_rv_emit_t *emit,
    u32 opcode, u32 pc)
{
  u32 offset = opcode & 0x07FF;
  u32 lr = pc + 4 + ((s32)(offset << 21) >> 9);

  return gba_p4_emit_li32(emit, RV_T2, lr) &&
         gba_p4_thumb_jit_store_reg(emit, REG_LR, RV_T2);
}

static bool gba_p4_thumb_jit_emit_bl_high_op(gba_p4_rv_emit_t *emit,
    u32 opcode, u32 pc)
{
  u32 offset = (opcode & 0x07FF) * 2;

  return gba_p4_thumb_jit_load_reg(emit, RV_T0, REG_LR) &&
         gba_p4_emit_li32(emit, RV_T1, offset) &&
         gba_p4_emit(emit, rv_add(RV_T0, RV_T0, RV_T1)) &&
         gba_p4_emit_li32(emit, RV_T2, pc + 3) &&
         gba_p4_thumb_jit_store_reg(emit, REG_LR, RV_T2) &&
         gba_p4_thumb_jit_store_reg(emit, REG_PC, RV_T0);
}

static bool gba_p4_thumb_jit_emit_add_pcsp_op(gba_p4_rv_emit_t *emit,
    u32 opcode, u32 pc)
{
  u32 top = (opcode >> 8) & 0xFF;
  u32 rd = top & 0x07;
  u32 imm = (opcode & 0xFF) * 4;

  if(top < 0xA8)
  {
    u32 value = (pc & ~0x02U) + 4 + imm;
    return gba_p4_emit_li32(emit, RV_T2, value) &&
           gba_p4_thumb_jit_store_reg(emit, rd, RV_T2);
  }

  return gba_p4_thumb_jit_load_reg(emit, RV_T0, REG_SP) &&
         gba_p4_emit(emit, rv_addi(RV_T2, RV_T0, imm)) &&
         gba_p4_thumb_jit_store_reg(emit, rd, RV_T2);
}

static bool gba_p4_thumb_jit_emit_add_sp_op(gba_p4_rv_emit_t *emit, u32 opcode)
{
  u32 imm = (opcode & 0x7F) * 4;
  s32 delta = ((opcode >> 7) & 0x01) ? -(s32)imm : (s32)imm;

  return gba_p4_thumb_jit_load_reg(emit, RV_T0, REG_SP) &&
         gba_p4_emit(emit, rv_addi(RV_T2, RV_T0, delta)) &&
         gba_p4_thumb_jit_store_reg(emit, REG_SP, RV_T2);
}

static bool gba_p4_thumb_jit_emit_iwram_addr(gba_p4_rv_emit_t *emit,
    u32 gba_addr_reg)
{
  return gba_p4_emit(emit, rv_slli(gba_addr_reg, gba_addr_reg, 17)) &&
         gba_p4_emit(emit, rv_srli(gba_addr_reg, gba_addr_reg, 17)) &&
         gba_p4_emit_li32(emit, RV_T1, (u32)(uintptr_t)(iwram + 0x8000)) &&
         gba_p4_emit(emit, rv_add(gba_addr_reg, gba_addr_reg, RV_T1));
}

static bool gba_p4_thumb_jit_emit_sp_str_op(gba_p4_rv_emit_t *emit, u32 opcode)
{
  u32 rd = (opcode >> 8) & 0x07;
  u32 imm = (opcode & 0xFF) * 4;

  return gba_p4_thumb_jit_load_reg(emit, RV_T0, REG_SP) &&
         gba_p4_emit(emit, rv_addi(RV_T0, RV_T0, imm)) &&
         gba_p4_emit(emit, rv_andi(RV_T0, RV_T0, -4)) &&
         gba_p4_thumb_jit_emit_iwram_addr(emit, RV_T0) &&
         gba_p4_thumb_jit_load_reg(emit, RV_T2, rd) &&
         gba_p4_emit(emit, rv_sw(RV_T2, RV_T0, 0));
}

static bool gba_p4_thumb_jit_emit_push_op(gba_p4_rv_emit_t *emit, u32 opcode)
{
  u32 reglist = opcode & 0xFF;
  u32 has_lr = ((opcode >> 8) & 0xFF) == 0xB5;
  u32 numops = bit_count[reglist] + has_lr;
  if(!numops)
    return false;

  if(!(gba_p4_thumb_jit_load_reg(emit, RV_T0, REG_SP) &&
       gba_p4_emit(emit, rv_addi(RV_T0, RV_T0, -(s32)(numops * 4))) &&
       gba_p4_emit(emit, rv_andi(RV_T0, RV_T0, -4)) &&
       gba_p4_emit(emit, rv_addi(RV_T3, RV_T0, 0)) &&
       gba_p4_thumb_jit_store_reg(emit, REG_SP, RV_T3) &&
       gba_p4_thumb_jit_emit_iwram_addr(emit, RV_T0)))
    return false;

  u32 offset = 0;
  for(u32 i = 0; i < 8; i++)
  {
    if(reglist & (1U << i))
    {
      if(!(gba_p4_thumb_jit_load_reg(emit, RV_T2, i) &&
           gba_p4_emit(emit, rv_sw(RV_T2, RV_T0, offset))))
        return false;
      offset += 4;
    }
  }

  return !has_lr ||
      (gba_p4_thumb_jit_load_reg(emit, RV_T2, REG_LR) &&
       gba_p4_emit(emit, rv_sw(RV_T2, RV_T0, offset)));
}

static bool gba_p4_thumb_jit_emit_sp_ldr_op(gba_p4_rv_emit_t *emit, u32 opcode)
{
  u32 rd = ((opcode >> 8) & 0x07);
  u32 imm = (opcode & 0xFF) * 4;

  return gba_p4_thumb_jit_load_reg(emit, RV_T0, REG_SP) &&
         gba_p4_emit(emit, rv_addi(RV_T0, RV_T0, imm)) &&
         gba_p4_emit(emit, rv_andi(RV_T0, RV_T0, -4)) &&
         gba_p4_thumb_jit_emit_iwram_addr(emit, RV_T0) &&
         gba_p4_emit(emit, rv_lw(RV_T2, RV_T0, 0)) &&
         gba_p4_thumb_jit_store_reg(emit, rd, RV_T2);
}

static bool gba_p4_thumb_jit_emit_pop_op(gba_p4_rv_emit_t *emit, u32 opcode)
{
  u32 reglist = opcode & 0xFF;
  if(!reglist)
    return false;

  if(!(gba_p4_thumb_jit_load_reg(emit, RV_T0, REG_SP) &&
       gba_p4_emit(emit, rv_andi(RV_T0, RV_T0, -4)) &&
       gba_p4_thumb_jit_emit_iwram_addr(emit, RV_T0)))
    return false;

  u32 offset = 0;
  for(u32 i = 0; i < 8; i++)
  {
    if(reglist & (1U << i))
    {
      if(!(gba_p4_emit(emit, rv_lw(RV_T2, RV_T0, offset)) &&
           gba_p4_thumb_jit_store_reg(emit, i, RV_T2)))
        return false;
      offset += 4;
    }
  }

  return gba_p4_thumb_jit_load_reg(emit, RV_T3, REG_SP) &&
         gba_p4_emit(emit, rv_addi(RV_T3, RV_T3, offset)) &&
         gba_p4_thumb_jit_store_reg(emit, REG_SP, RV_T3);
}

static bool gba_p4_thumb_jit_emit_bail(gba_p4_rv_emit_t *emit, u32 pc,
    u32 executed_ops)
{
  return gba_p4_thumb_jit_cache_emit_flush_all_preserve(emit) &&
         gba_p4_emit_li32(emit, RV_T2, pc) &&
         gba_p4_thumb_jit_store_reg(emit, REG_PC, RV_T2) &&
         gba_p4_emit_li32(emit, RV_A0, executed_ops) &&
         gba_p4_emit(emit, 0x00008067);
}

static bool gba_p4_thumb_jit_emit_wram_load_imm_op(gba_p4_rv_emit_t *emit,
    u32 opcode, u32 pc, u32 executed_ops)
{
  u32 top = (opcode >> 8) & 0xFF;
  u32 rb = (opcode >> 3) & 0x07;
  u32 rd = opcode & 0x07;
  u32 imm = (opcode >> 6) & 0x1F;
  u32 width;
  u32 align_mask;

  if(top >= 0x68 && top <= 0x6F)
  {
    imm *= 4;
    width = 4;
    align_mask = 3;
  }
  else if(top >= 0x88 && top <= 0x8F)
  {
    imm *= 2;
    width = 2;
    align_mask = 1;
  }
  else
  {
    width = 1;
    align_mask = 0;
  }

  if(!(gba_p4_thumb_jit_load_reg(emit, RV_T0, rb) &&
       gba_p4_emit(emit, rv_addi(RV_T0, RV_T0, imm)) &&
       gba_p4_emit(emit, rv_srli(RV_T3, RV_T0, 24))))
    return false;

  if(align_mask)
  {
    if(!(gba_p4_emit(emit, rv_andi(RV_T5, RV_T0, align_mask)) &&
         gba_p4_emit(emit, rv_sltiu(RV_T5, RV_T5, 1))))
      return false;
  }
  else if(!gba_p4_emit(emit, rv_addi(RV_T5, RV_ZERO, 1)))
    return false;

  if(!(gba_p4_emit(emit, rv_addi(RV_T4, RV_T3, -2)) &&
       gba_p4_emit(emit, rv_sltiu(RV_T4, RV_T4, 2)) &&
       gba_p4_emit(emit, rv_and(RV_T4, RV_T4, RV_T5))))
    return false;

  u32 branch_ok_pos;
  if(!gba_p4_emit_branch_placeholder(emit, &branch_ok_pos))
    return false;

  if(!gba_p4_thumb_jit_emit_bail(emit, pc, executed_ops))
    return false;

  u32 load_dispatch = emit->words;
  if(!gba_p4_patch_branch(emit, branch_ok_pos, load_dispatch,
      RV_T4, RV_ZERO, 0x1))
    return false;

  u32 branch_iwram_pos;
  if(!(gba_p4_emit(emit, rv_addi(RV_T4, RV_T3, -3)) &&
       gba_p4_emit(emit, rv_sltiu(RV_T4, RV_T4, 1)) &&
       gba_p4_emit_branch_placeholder(emit, &branch_iwram_pos)))
    return false;

  if(!(gba_p4_emit(emit, rv_slli(RV_T0, RV_T0, 14)) &&
       gba_p4_emit(emit, rv_srli(RV_T0, RV_T0, 14)) &&
       gba_p4_emit_li32(emit, RV_T1, (u32)(uintptr_t)ewram) &&
       gba_p4_emit(emit, rv_add(RV_T0, RV_T0, RV_T1))))
    return false;

  if(width == 4)
  {
    if(!gba_p4_emit(emit, rv_lw(RV_T2, RV_T0, 0)))
      return false;
  }
  else if(width == 2)
  {
    if(!gba_p4_emit(emit, rv_lhu(RV_T2, RV_T0, 0)))
      return false;
  }
  else if(!gba_p4_emit(emit, rv_lbu(RV_T2, RV_T0, 0)))
    return false;

  if(!gba_p4_thumb_jit_store_reg(emit, rd, RV_T2))
    return false;

  u32 jump_end_pos;
  if(!gba_p4_emit_branch_placeholder(emit, &jump_end_pos))
    return false;

  u32 iwram_path = emit->words;
  if(!gba_p4_patch_branch(emit, branch_iwram_pos, iwram_path,
      RV_T4, RV_ZERO, 0x1))
    return false;

  if(!(gba_p4_thumb_jit_load_reg(emit, RV_T0, rb) &&
       gba_p4_emit(emit, rv_addi(RV_T0, RV_T0, imm)) &&
       gba_p4_emit(emit, rv_slli(RV_T0, RV_T0, 17)) &&
       gba_p4_emit(emit, rv_srli(RV_T0, RV_T0, 17)) &&
       gba_p4_emit_li32(emit, RV_T1, (u32)(uintptr_t)(iwram + 0x8000)) &&
       gba_p4_emit(emit, rv_add(RV_T0, RV_T0, RV_T1))))
    return false;

  if(width == 4)
  {
    if(!gba_p4_emit(emit, rv_lw(RV_T2, RV_T0, 0)))
      return false;
  }
  else if(width == 2)
  {
    if(!gba_p4_emit(emit, rv_lhu(RV_T2, RV_T0, 0)))
      return false;
  }
  else if(!gba_p4_emit(emit, rv_lbu(RV_T2, RV_T0, 0)))
    return false;

  return gba_p4_thumb_jit_store_reg(emit, rd, RV_T2) &&
         gba_p4_patch_branch(emit, jump_end_pos, emit->words,
             RV_ZERO, RV_ZERO, 0x0);
}

static bool gba_p4_thumb_jit_emit_wram_store_imm_op(gba_p4_rv_emit_t *emit,
    u32 opcode, u32 pc, u32 executed_ops)
{
  u32 top = (opcode >> 8) & 0xFF;
  u32 rb = (opcode >> 3) & 0x07;
  u32 rd = opcode & 0x07;
  u32 imm = (opcode >> 6) & 0x1F;
  u32 width;
  u32 align_mask;

  if(top >= 0x60 && top <= 0x67)
  {
    imm *= 4;
    width = 4;
    align_mask = 3;
  }
  else if(top >= 0x80 && top <= 0x87)
  {
    imm *= 2;
    width = 2;
    align_mask = 1;
  }
  else
  {
    width = 1;
    align_mask = 0;
  }

  if(!(gba_p4_thumb_jit_load_reg(emit, RV_T0, rb) &&
       gba_p4_emit(emit, rv_addi(RV_T0, RV_T0, imm)) &&
       gba_p4_emit(emit, rv_srli(RV_T3, RV_T0, 24))))
    return false;

  if(align_mask)
  {
    if(!(gba_p4_emit(emit, rv_andi(RV_T5, RV_T0, align_mask)) &&
         gba_p4_emit(emit, rv_sltiu(RV_T5, RV_T5, 1))))
      return false;
  }
  else if(!gba_p4_emit(emit, rv_addi(RV_T5, RV_ZERO, 1)))
    return false;

  if(!(gba_p4_emit(emit, rv_addi(RV_T4, RV_T3, -2)) &&
       gba_p4_emit(emit, rv_sltiu(RV_T4, RV_T4, 2)) &&
       gba_p4_emit(emit, rv_and(RV_T4, RV_T4, RV_T5))))
    return false;

  u32 branch_ok_pos;
  if(!gba_p4_emit_branch_placeholder(emit, &branch_ok_pos))
    return false;

  if(!gba_p4_thumb_jit_emit_bail(emit, pc, executed_ops))
    return false;

  u32 store_dispatch = emit->words;
  if(!gba_p4_patch_branch(emit, branch_ok_pos, store_dispatch,
      RV_T4, RV_ZERO, 0x1))
    return false;

  u32 branch_iwram_pos;
  if(!(gba_p4_emit(emit, rv_addi(RV_T4, RV_T3, -3)) &&
       gba_p4_emit(emit, rv_sltiu(RV_T4, RV_T4, 1)) &&
       gba_p4_emit_branch_placeholder(emit, &branch_iwram_pos)))
    return false;

  if(!(gba_p4_emit(emit, rv_slli(RV_T0, RV_T0, 14)) &&
       gba_p4_emit(emit, rv_srli(RV_T0, RV_T0, 14)) &&
       gba_p4_emit_li32(emit, RV_T1, (u32)(uintptr_t)ewram) &&
       gba_p4_emit(emit, rv_add(RV_T0, RV_T0, RV_T1)) &&
       gba_p4_thumb_jit_load_reg(emit, RV_T2, rd)))
    return false;

  if(width == 4)
  {
    if(!gba_p4_emit(emit, rv_sw(RV_T2, RV_T0, 0)))
      return false;
  }
  else if(width == 2)
  {
    if(!gba_p4_emit(emit, rv_sh(RV_T2, RV_T0, 0)))
      return false;
  }
  else if(!gba_p4_emit(emit, rv_sb(RV_T2, RV_T0, 0)))
    return false;

  u32 jump_end_pos;
  if(!gba_p4_emit_branch_placeholder(emit, &jump_end_pos))
    return false;

  u32 iwram_path = emit->words;
  if(!gba_p4_patch_branch(emit, branch_iwram_pos, iwram_path,
      RV_T4, RV_ZERO, 0x1))
    return false;

  if(!(gba_p4_thumb_jit_load_reg(emit, RV_T0, rb) &&
       gba_p4_emit(emit, rv_addi(RV_T0, RV_T0, imm)) &&
       gba_p4_emit(emit, rv_slli(RV_T0, RV_T0, 17)) &&
       gba_p4_emit(emit, rv_srli(RV_T0, RV_T0, 17)) &&
       gba_p4_emit_li32(emit, RV_T1, (u32)(uintptr_t)(iwram + 0x8000)) &&
       gba_p4_emit(emit, rv_add(RV_T0, RV_T0, RV_T1)) &&
       gba_p4_thumb_jit_load_reg(emit, RV_T2, rd)))
    return false;

  if(width == 4)
  {
    if(!gba_p4_emit(emit, rv_sw(RV_T2, RV_T0, 0)))
      return false;
  }
  else if(width == 2)
  {
    if(!gba_p4_emit(emit, rv_sh(RV_T2, RV_T0, 0)))
      return false;
  }
  else if(!gba_p4_emit(emit, rv_sb(RV_T2, RV_T0, 0)))
    return false;

  return gba_p4_patch_branch(emit, jump_end_pos, emit->words,
      RV_ZERO, RV_ZERO, 0x0);
}

static inline bool gba_p4_thumb_jit_load_flag(gba_p4_rv_emit_t *emit,
    u32 rv_reg, u32 flag_index)
{
  return gba_p4_emit(emit, rv_lw(rv_reg, RV_A1, flag_index * sizeof(u32))) &&
         gba_p4_emit(emit, rv_andi(rv_reg, rv_reg, 1));
}

static bool gba_p4_thumb_jit_emit_cond(gba_p4_rv_emit_t *emit, u32 cond)
{
  switch(cond)
  {
    case 0x0:  // EQ
      return gba_p4_thumb_jit_load_flag(emit, RV_T2, 1);

    case 0x1:  // NE
      return gba_p4_thumb_jit_load_flag(emit, RV_T2, 1) &&
             gba_p4_emit(emit, rv_xori(RV_T2, RV_T2, 1));

    case 0x2:  // CS
      return gba_p4_thumb_jit_load_flag(emit, RV_T2, 2);

    case 0x3:  // CC
      return gba_p4_thumb_jit_load_flag(emit, RV_T2, 2) &&
             gba_p4_emit(emit, rv_xori(RV_T2, RV_T2, 1));

    case 0x4:  // MI
      return gba_p4_thumb_jit_load_flag(emit, RV_T2, 0);

    case 0x5:  // PL
      return gba_p4_thumb_jit_load_flag(emit, RV_T2, 0) &&
             gba_p4_emit(emit, rv_xori(RV_T2, RV_T2, 1));

    case 0x6:  // VS
      return gba_p4_thumb_jit_load_flag(emit, RV_T2, 3);

    case 0x7:  // VC
      return gba_p4_thumb_jit_load_flag(emit, RV_T2, 3) &&
             gba_p4_emit(emit, rv_xori(RV_T2, RV_T2, 1));

    case 0x8:  // HI: C && !Z
      return gba_p4_thumb_jit_load_flag(emit, RV_T0, 2) &&
             gba_p4_thumb_jit_load_flag(emit, RV_T1, 1) &&
             gba_p4_emit(emit, rv_xori(RV_T1, RV_T1, 1)) &&
             gba_p4_emit(emit, rv_and(RV_T2, RV_T0, RV_T1));

    case 0x9:  // LS: !C || Z
      return gba_p4_thumb_jit_load_flag(emit, RV_T0, 2) &&
             gba_p4_thumb_jit_load_flag(emit, RV_T1, 1) &&
             gba_p4_emit(emit, rv_xori(RV_T0, RV_T0, 1)) &&
             gba_p4_emit(emit, rv_or(RV_T2, RV_T0, RV_T1)) &&
             gba_p4_emit(emit, rv_andi(RV_T2, RV_T2, 1));

    case 0xA:  // GE: N == V
      return gba_p4_thumb_jit_load_flag(emit, RV_T0, 0) &&
             gba_p4_thumb_jit_load_flag(emit, RV_T1, 3) &&
             gba_p4_emit(emit, rv_xor(RV_T2, RV_T0, RV_T1)) &&
             gba_p4_emit(emit, rv_xori(RV_T2, RV_T2, 1));

    case 0xB:  // LT: N != V
      return gba_p4_thumb_jit_load_flag(emit, RV_T0, 0) &&
             gba_p4_thumb_jit_load_flag(emit, RV_T1, 3) &&
             gba_p4_emit(emit, rv_xor(RV_T2, RV_T0, RV_T1));

    case 0xC:  // GT: !Z && N == V
      return gba_p4_thumb_jit_load_flag(emit, RV_T0, 1) &&
             gba_p4_thumb_jit_load_flag(emit, RV_T1, 0) &&
             gba_p4_thumb_jit_load_flag(emit, RV_T2, 3) &&
             gba_p4_emit(emit, rv_xori(RV_T0, RV_T0, 1)) &&
             gba_p4_emit(emit, rv_xor(RV_T1, RV_T1, RV_T2)) &&
             gba_p4_emit(emit, rv_xori(RV_T1, RV_T1, 1)) &&
             gba_p4_emit(emit, rv_and(RV_T2, RV_T0, RV_T1));

    case 0xD:  // LE: Z || N != V
      return gba_p4_thumb_jit_load_flag(emit, RV_T0, 1) &&
             gba_p4_thumb_jit_load_flag(emit, RV_T1, 0) &&
             gba_p4_thumb_jit_load_flag(emit, RV_T2, 3) &&
             gba_p4_emit(emit, rv_xor(RV_T1, RV_T1, RV_T2)) &&
             gba_p4_emit(emit, rv_or(RV_T2, RV_T0, RV_T1)) &&
             gba_p4_emit(emit, rv_andi(RV_T2, RV_T2, 1));

    default:
      return false;
  }
}

static bool gba_p4_thumb_jit_emit_cond_branch_op(gba_p4_rv_emit_t *emit,
    u32 opcode, u32 pc)
{
  u32 cond = (opcode >> 8) & 0x0F;
  s32 offset = ((s32)((s8)(opcode & 0xFF)) * 2) + 4;
  u32 not_taken = pc + 2;
  u32 taken = pc + offset;

  return gba_p4_thumb_jit_emit_cond(emit, cond) &&
         gba_p4_emit_li32(emit, RV_T0, not_taken) &&
         gba_p4_emit_li32(emit, RV_T1, taken - not_taken) &&
         gba_p4_emit(emit, rv_sub(RV_T2, RV_ZERO, RV_T2)) &&
         gba_p4_emit(emit, rv_and(RV_T1, RV_T1, RV_T2)) &&
         gba_p4_emit(emit, rv_add(RV_T0, RV_T0, RV_T1)) &&
         gba_p4_emit(emit, rv_sw(RV_T0, RV_A0, REG_PC * sizeof(u32)));
}

static bool gba_p4_thumb_jit_emit_uncond_branch_op(gba_p4_rv_emit_t *emit,
    u32 opcode, u32 pc)
{
  u32 offset = opcode & 0x07FF;
  u32 target = pc + ((s32)(offset << 21) >> 20) + 4;

  return gba_p4_emit_li32(emit, RV_T0, target) &&
         gba_p4_emit(emit, rv_sw(RV_T0, RV_A0, REG_PC * sizeof(u32)));
}

static bool gba_p4_thumb_jit_emit_op(gba_p4_rv_emit_t *emit, u32 opcode,
    u32 pc, u32 op_index)
{
  u32 top = (opcode >> 8) & 0xFF;

  if(top <= 0x1F)
    return gba_p4_thumb_jit_emit_low_op(emit, opcode);

  if(top >= 0x20 && top <= 0x3F)
    return gba_p4_thumb_jit_emit_imm_op(emit, opcode);

  if(GBA_P4_THUMB_JIT_ALU40_LOGIC && top == 0x40)
    return gba_p4_thumb_jit_emit_alu40_logic_op(emit, opcode);

  if(GBA_P4_THUMB_JIT_ALU42 && top == 0x42)
    return gba_p4_thumb_jit_emit_alu42_op(emit, opcode);

  if(GBA_P4_THUMB_JIT_ALU43_LOGIC && top == 0x43)
    return gba_p4_thumb_jit_emit_alu43_logic_op(emit, opcode);

  if(GBA_P4_THUMB_JIT_HIREG_ALU && (top == 0x44 || top == 0x45))
    return gba_p4_thumb_jit_emit_hireg_alu_op(emit, opcode, pc);

  if(GBA_P4_THUMB_JIT_HIREG_MOV && top == 0x46)
    return gba_p4_thumb_jit_emit_hireg_mov_op(emit, opcode, pc);

  if(top == 0x47)
    return gba_p4_thumb_jit_emit_bx_op(emit, opcode, pc, op_index + 1);

  if(top >= 0x48 && top <= 0x4F)
    return gba_p4_thumb_jit_emit_pcldr_op(emit, opcode, pc);

  if(top >= 0xD0 && top <= 0xDD)
    return gba_p4_thumb_jit_emit_cond_branch_op(emit, opcode, pc);

  if(top >= 0xE0 && top <= 0xE7)
    return gba_p4_thumb_jit_emit_uncond_branch_op(emit, opcode, pc);

  if(top >= 0xA0 && top <= 0xAF)
    return gba_p4_thumb_jit_emit_add_pcsp_op(emit, opcode, pc);

  if(top >= 0xB0 && top <= 0xB3)
    return gba_p4_thumb_jit_emit_add_sp_op(emit, opcode);

  if(GBA_P4_THUMB_JIT_STACK_READS && top >= 0x98 && top <= 0x9F)
    return gba_p4_thumb_jit_emit_sp_ldr_op(emit, opcode);

  if(GBA_P4_THUMB_JIT_STACK_WRITES && top >= 0x90 && top <= 0x97)
    return gba_p4_thumb_jit_emit_sp_str_op(emit, opcode);

  if(GBA_P4_THUMB_JIT_STACK_WRITES && (top == 0xB4 || top == 0xB5))
    return gba_p4_thumb_jit_emit_push_op(emit, opcode);

  if(GBA_P4_THUMB_JIT_STACK_READS && top == 0xBC)
    return gba_p4_thumb_jit_emit_pop_op(emit, opcode);

  if(gba_p4_thumb_jit_wram_store_opcode(opcode))
    return gba_p4_thumb_jit_emit_wram_store_imm_op(emit, opcode, pc, op_index);

  if(gba_p4_thumb_jit_wram_load_opcode(opcode))
    return gba_p4_thumb_jit_emit_wram_load_imm_op(emit, opcode, pc, op_index);

  if(top >= 0xF0 && top <= 0xF7)
    return gba_p4_thumb_jit_emit_bl_low_op(emit, opcode, pc);

  if(top >= 0xF8 && top <= 0xFF)
    return gba_p4_thumb_jit_emit_bl_high_op(emit, opcode, pc);

  return false;
}

static bool gba_p4_thumb_jit_emit_block(gba_p4_rv_emit_t *emit, u32 pc,
    const u16 *opcodes, u32 op_count, bool terminal, bool pc_write,
    bool *can_bail)
{
  *can_bail = false;
  gba_p4_thumb_jit_cache_init(emit);
  for(u32 i = 0; i < op_count; i++)
  {
    if(gba_p4_thumb_jit_wram_load_opcode(opcodes[i]) ||
       gba_p4_thumb_jit_wram_store_opcode(opcodes[i]))
      *can_bail = true;

    if(!gba_p4_thumb_jit_emit_op(emit, opcodes[i], pc + i * 2, i))
      return false;
  }

  if(!gba_p4_thumb_jit_cache_flush_all(emit))
    return false;

  if(!terminal && !pc_write)
  {
    if(!(gba_p4_emit_li32(emit, RV_T0, pc + op_count * 2) &&
         gba_p4_emit(emit, rv_sw(RV_T0, RV_A0, REG_PC * sizeof(u32)))))
      return false;
  }

  u32 ret_value = op_count |
      (terminal ? (1U << GBA_P4_THUMB_JIT_RET_EXTRA_SHIFT) : 0);
  return gba_p4_emit_li32(emit, RV_A0, ret_value) &&
         gba_p4_emit(emit, 0x00008067);
}

static inline void gba_p4_thumb_jit_commit_entry(gba_p4_thumb_jit_entry_t *entry,
    u32 pc, const u16 *opcodes, u32 op_count, bool terminal, bool can_bail,
    gba_p4_rv_emit_t *emit, u32 reserve_words)
{
  memset(entry, 0, sizeof(*entry));
  entry->pc = pc;
  entry->op_count = (u16)op_count;
  entry->extra_cycles = terminal ? 1 : 0;
  entry->can_bail = can_bail ? 1 : 0;
  entry->code_words = reserve_words;
  memcpy(entry->opcodes, opcodes, sizeof(opcodes[0]) * op_count);
  entry->fn = (gba_p4_thumb_jit_fn)emit->exec;
}

static __attribute__((noinline, cold)) gba_p4_thumb_jit_entry_t *
gba_p4_thumb_jit_compile(u32 pc,
    u8 *pc_address_block)
{
  u16 opcodes[GBA_P4_THUMB_JIT_MAX_OPS];
  u32 op_count = gba_p4_thumb_jit_collect(pc, pc_address_block, opcodes);
  bool terminal = op_count && gba_p4_thumb_jit_terminal_opcode(opcodes[op_count - 1]);
  bool pc_write = op_count && gba_p4_thumb_jit_pc_write_opcode(opcodes[op_count - 1]);
  bool single_allowed = op_count == 1 && gba_p4_thumb_jit_allow_single_opcode(opcodes[0]);
  if(op_count < GBA_P4_THUMB_JIT_MIN_OPS && !terminal && !single_allowed)
  {
    gba_thumb_jit_short_blocks++;
    gba_p4_thumb_jit_mark_rejected(pc, pc_address_block);
    return NULL;
  }

  if(!gba_p4_thumb_jit_init())
    return NULL;

  gba_p4_thumb_jit_entry_t *entry = gba_p4_thumb_jit_lookup(pc);
  if(!entry)
    entry = gba_p4_thumb_jit_select_entry(pc);
  if(!entry)
    return NULL;

  if(GBA_P4_THUMB_JIT_REUSE_EXHAUSTED &&
     gba_p4_thumb_jit_arena_exhausted && entry->fn && entry->code_words)
  {
    u32 *exec = (u32 *)entry->fn;
    volatile u32 *write = gba_p4_thumb_jit_write_alias(exec);
    u32 reserve_words = entry->code_words;
    bool can_bail = false;
    memset(entry, 0, sizeof(*entry));

    gba_p4_rv_emit_t emit =
    {
      write,
      exec,
      0,
      reserve_words
    };

    if(gba_p4_thumb_jit_emit_block(&emit, pc, opcodes, op_count,
          terminal, pc_write, &can_bail))
    {
      gba_p4_thumb_jit_sync((void *)emit.exec, emit.words);
      gba_p4_thumb_jit_commit_entry(entry, pc, opcodes, op_count, terminal,
          can_bail, &emit, reserve_words);
      gba_thumb_jit_compiles++;
      gba_thumb_jit_reuses++;
      return entry;
    }
  }

  u32 bank = gba_p4_thumb_jit_bank_index;
  u32 start_words = 0;
  u32 remaining = 0;
  while(bank < gba_p4_thumb_jit_bank_count)
  {
    start_words = (gba_p4_thumb_jit_used_words[bank] + 7) & ~7U;
    remaining = (start_words < gba_p4_thumb_jit_capacity_words[bank]) ?
        (gba_p4_thumb_jit_capacity_words[bank] - start_words) : 0;
    if(remaining >= 192)
      break;
    bank++;
  }

  if(bank >= gba_p4_thumb_jit_bank_count)
  {
    if(!gba_p4_thumb_jit_arena_exhausted)
    {
      gba_p4_thumb_jit_arena_exhausted = true;
      gba_p4_thumb_jit_exhausted_hits = 0;
      gba_p4_thumb_jit_exhausted_misses = 0;
      gba_thumb_jit_arena_full++;
    }
    return NULL;
  }
  gba_p4_thumb_jit_bank_index = bank;

  gba_p4_rv_emit_t emit =
  {
    gba_p4_thumb_jit_write[bank] + start_words,
    gba_p4_thumb_jit_exec[bank] + start_words,
    0,
    remaining
  };

  bool can_bail = false;
  if(!gba_p4_thumb_jit_emit_block(&emit, pc, opcodes, op_count,
        terminal, pc_write, &can_bail))
  {
    gba_p4_thumb_jit_mark_rejected(pc, pc_address_block);
    return NULL;
  }

  gba_p4_thumb_jit_sync((void *)emit.exec, emit.words);

  u32 reserve_words = (emit.words + 7) & ~7U;
  if(reserve_words > remaining)
    reserve_words = emit.words;
  gba_p4_thumb_jit_commit_entry(entry, pc, opcodes, op_count, terminal,
      can_bail, &emit, reserve_words);

  gba_p4_thumb_jit_used_words[bank] = start_words + reserve_words;
  gba_thumb_jit_used_bytes = 0;
  for(u32 i = 0; i < gba_p4_thumb_jit_bank_count; i++)
    gba_thumb_jit_used_bytes += gba_p4_thumb_jit_used_words[i] * sizeof(u32);
  gba_thumb_jit_compiles++;
  return entry;
}

static inline int gba_p4_thumb_jit_try(u8 *pc_address_block, s32 cycles_remaining,
    u32 &n_flag, u32 &z_flag, u32 &c_flag, u32 &v_flag)
{
  if(cycles_remaining < 32)
    return 0;

  u32 pc = reg[REG_PC] & ~1U;
  u32 region = pc >> 24;

  if(!gba_p4_thumb_jit_region_allowed(region) || !pc_address_block)
  {
    gba_thumb_jit_region_skips++;
    return 0;
  }

  u32 offset = pc & 0x7FFF;
  if(offset > (0x8000 - GBA_P4_THUMB_JIT_MIN_OPS * 2))
  {
    gba_thumb_jit_short_blocks++;
    return 0;
  }

  gba_thumb_jit_attempts++;

  if(!gba_p4_thumb_jit_init())
    return 0;

  gba_p4_thumb_jit_entry_t *entry = gba_p4_thumb_jit_lookup(pc);
  if(!entry || !gba_p4_thumb_jit_matches(entry, pc, pc_address_block))
  {
    gba_thumb_jit_misses++;

    if(gba_p4_thumb_jit_arena_exhausted &&
       !gba_p4_thumb_jit_can_allocate_after_full())
    {
      gba_p4_thumb_jit_exhausted_misses++;
      if(gba_p4_thumb_jit_exhausted_misses >= GBA_P4_THUMB_JIT_SUSPEND_MISSES)
      {
        if(gba_p4_thumb_jit_exhausted_hits * 16U < gba_p4_thumb_jit_exhausted_misses)
          gba_p4_thumb_jit_probe_suspend = GBA_P4_THUMB_JIT_SUSPEND_OPS;

        gba_p4_thumb_jit_exhausted_hits = 0;
        gba_p4_thumb_jit_exhausted_misses = 0;
      }
      gba_thumb_jit_short_blocks++;
      return 0;
    }

    if(gba_p4_thumb_jit_is_rejected(pc, pc_address_block))
    {
      gba_thumb_jit_short_blocks++;
      return 0;
    }

    if(!gba_p4_thumb_jit_hot_enough(pc))
    {
      gba_thumb_jit_hot_waits++;
      gba_thumb_jit_short_blocks++;
      return 0;
    }

    if(gba_p4_thumb_jit_arena_exhausted)
    {
      gba_p4_thumb_jit_exhausted_misses++;
#if GBA_P4_THUMB_JIT_REUSE_EXHAUSTED
      // The compiler selects a reusable executable slot from any way in this
      // set. The requested PC is necessarily absent here, so `entry` may be null.
      (void)gba_p4_thumb_jit_recycle_stale_arena();
      (void)gba_p4_thumb_jit_recycle_arena();
#else
      if(!(gba_p4_thumb_jit_recycle_stale_arena() ||
           gba_p4_thumb_jit_recycle_arena()))
      {
        gba_thumb_jit_short_blocks++;
        return 0;
      }
#endif
    }
    entry = gba_p4_thumb_jit_compile(pc, pc_address_block);
    if(!entry)
    {
      if(gba_p4_thumb_jit_arena_exhausted)
      {
        if(gba_p4_thumb_jit_recycle_arena())
          entry = gba_p4_thumb_jit_compile(pc, pc_address_block);

        if(!entry)
        {
          if(gba_p4_thumb_jit_arena_exhausted)
            gba_thumb_jit_short_blocks++;
          else
            gba_p4_thumb_jit_mark_rejected(pc, pc_address_block);
          return 0;
        }
      }
      else
      {
        gba_p4_thumb_jit_mark_rejected(pc, pc_address_block);
        return 0;
      }
    }
  }
  else
  {
    gba_thumb_jit_hits++;
    if(gba_p4_thumb_jit_arena_exhausted)
    {
      gba_p4_thumb_jit_exhausted_hits++;
      gba_p4_thumb_jit_probe_suspend = 0;
    }
  }

  if(!esp_ptr_executable((void *)entry->fn))
  {
    memset(entry, 0, sizeof(*entry));
    gba_thumb_jit_disabled++;
    return 0;
  }

  u32 trace_index = gba_p4_thumb_jit_trace_head++ &
      (GBA_P4_THUMB_JIT_TRACE_COUNT - 1);
  gba_p4_thumb_jit_trace_t *trace = &gba_p4_thumb_jit_trace[trace_index];
  trace->pc = pc;
  trace->end_pc = pc;
  trace->sp = reg[REG_SP];
  trace->lr = reg[REG_LR];
  trace->ret = 0;
  trace->signature = entry->opcodes[0] |
      ((u32)entry->opcodes[entry->op_count - 1] << 16);
  gba_thumb_jit_last_pc = pc;
  gba_thumb_jit_last_end_pc = pc;
  gba_thumb_jit_last_ret = 0;
  gba_thumb_jit_last_signature = trace->signature;

  u32 jit_ret = entry->op_count;
  bool ok;
  if(!gba_thumb_jit_debug_validate &&
     entry->validated >= GBA_P4_THUMB_JIT_TRUST_VALIDATIONS)
    ok = gba_p4_thumb_jit_execute_committed(entry, n_flag, z_flag, c_flag, v_flag,
        &jit_ret);
  else
    ok = gba_p4_thumb_jit_validate_and_commit(entry, n_flag, z_flag, c_flag, v_flag,
        &jit_ret);

  trace->end_pc = reg[REG_PC];
  trace->ret = jit_ret;
  gba_thumb_jit_last_end_pc = trace->end_pc;
  gba_thumb_jit_last_ret = jit_ret;

  u32 executed_ops = jit_ret & GBA_P4_THUMB_JIT_RET_OPS_MASK;
  u32 expected_pc = pc + executed_ops * 2;
  if(ok && reg[REG_PC] != expected_pc)
    ok = gba_p4_thumb_jit_record_fail(entry, 0x80, executed_ops,
        expected_pc, reg[REG_PC]);

  if(!ok)
  {
    memset(entry, 0, sizeof(*entry));
    gba_p4_thumb_jit_mark_rejected(pc, pc_address_block);
    gba_thumb_jit_validate_failures++;
    return 0;
  }

  gba_thumb_jit_ops += jit_ret & GBA_P4_THUMB_JIT_RET_OPS_MASK;
  return (int)jit_ret;
}

static inline bool gba_p4_thumb_jit_can_start(u32 opcode, u8 *pc_address_block)
{
  if(!gba_thumb_jit_runtime_enabled)
    return false;

  u32 pc = reg[REG_PC] & ~1U;

  // The P4 emitter only accepts immutable Game Pak ROM. Reject IWRAM and
  // invalid fetches here so they never enter the larger JIT lookup routine.
  if(!pc_address_block || !gba_p4_thumb_jit_region_allowed(pc >> 24))
    return false;

  if(gba_p4_thumb_jit_probe_suspend)
  {
    gba_p4_thumb_jit_probe_suspend--;
    return false;
  }

  if(!gba_p4_thumb_jit_supported_opcode(opcode))
    return false;

  if(gba_p4_thumb_jit_control_flow_opcode(opcode))
    return false;

  if(gba_p4_thumb_jit_arena_exhausted)
  {
    if(!gba_p4_thumb_jit_ready)
      return false;

    gba_p4_thumb_jit_entry_t *entry = gba_p4_thumb_jit_lookup(pc);
    // Cached blocks remain the zero-overhead path. One in every 64 uncached
    // candidates may enter the hotness filter so a long-running game can replace
    // stale startup code without paying a PSRAM cache lookup on every instruction.
    if(entry)
      return true;
    if((++gba_p4_thumb_jit_adapt_counter & GBA_P4_THUMB_JIT_ADAPT_SAMPLE_MASK) != 0)
      return false;
    gba_thumb_jit_adapt_probes++;
    return true;
  }

  return true;
}
#else
#define GBA_P4_THUMB_JIT_RET_OPS_MASK 0xFFFFU
#define GBA_P4_THUMB_JIT_RET_EXTRA_SHIFT 16
#define GBA_P4_THUMB_JIT_RET_EXTRA_MASK 0xFFU
#define GBA_P4_THUMB_JIT_RET_ARM_SWITCH (1U << 24)

extern "C" void gba_p4_thumb_jit_preinit(void)
{
}

extern "C" void gba_p4_thumb_jit_reset(void)
{
}

extern "C" void gba_p4_thumb_jit_reset_stats(void)
{
}

extern "C" void gba_p4_thumb_jit_shutdown(void)
{
}

extern "C" void gba_p4_thumb_jit_report_fault(u32 reason, u32 fault_pc)
{
  (void)reason;
  (void)fault_pc;
  gba_thumb_jit_runtime_enabled = 0;
  gba_thumb_batch_enabled = 0;
}

static inline int gba_p4_thumb_jit_try(u8 *pc_address_block, s32 cycles_remaining,
    u32 &n_flag, u32 &z_flag, u32 &c_flag, u32 &v_flag)
{
  (void)pc_address_block;
  (void)cycles_remaining;
  (void)n_flag;
  (void)z_flag;
  (void)c_flag;
  (void)v_flag;
  return 0;
}

static inline bool gba_p4_thumb_jit_can_start(u32 opcode, u8 *pc_address_block)
{
  (void)opcode;
  (void)pc_address_block;
  return false;
}
#endif

static inline bool gba_thumb_execute_low_fast(u32 opcode, u32 &n_flag, u32 &z_flag,
    u32 &c_flag, u32 &v_flag)
{
  u32 op = (opcode >> 8) & 0xFF;
  if(op > 0x1F)
    return false;

  u32 rd = opcode & 0x07;
  u32 rs = (opcode >> 3) & 0x07;
  u32 dest;

  if(op < 0x18)
  {
    u32 offset = (opcode >> 6) & 0x1F;
    u32 src = reg[rs];

    switch((opcode >> 11) & 0x03)
    {
      case 0x00:
        /* LSL rd, rs, offset */
        dest = src << offset;
        if(offset)
          c_flag = (src >> (32 - offset)) & 0x01;
        break;

      case 0x01:
        /* LSR rd, rs, offset */
        if(offset == 0)
        {
          c_flag = src >> 31;
          dest = 0;
        }
        else
        {
          c_flag = (src >> (offset - 1)) & 0x01;
          dest = src >> offset;
        }
        break;

      default:
        /* ASR rd, rs, offset */
        if(offset == 0)
        {
          dest = (u32)((s32)src >> 31);
          c_flag = dest & 0x01;
        }
        else
        {
          c_flag = (src >> (offset - 1)) & 0x01;
          dest = (u32)((s32)src >> offset);
        }
        break;
    }

    n_flag = dest >> 31;
    z_flag = dest == 0;
    reg[rd] = dest;
  }
  else
  {
    u32 rhs = (opcode & 0x0400) ? ((opcode >> 6) & 0x07) : reg[(opcode >> 6) & 0x07];
    u32 lhs = reg[rs];

    if(op & 0x02)
    {
      /* SUB rd, rs, rn/imm3 */
      dest = lhs - rhs;
      n_flag = dest >> 31;
      z_flag = dest == 0;
      c_flag = lhs >= rhs;
      v_flag = ((lhs ^ rhs) & (lhs ^ dest)) >> 31;
    }
    else
    {
      /* ADD rd, rs, rn/imm3 */
      dest = lhs + rhs;
      n_flag = dest >> 31;
      z_flag = dest == 0;
      c_flag = dest < rhs;
      v_flag = (~(lhs ^ rhs) & (lhs ^ dest)) >> 31;
    }

    reg[rd] = dest;
  }

  reg[REG_PC] += 2;
  gba_thumb_low_fast_ops++;
  return true;
}

static inline __attribute__((always_inline)) bool gba_thumb_execute_lsl_imm_fast(u32 opcode,
    u32 &n_flag, u32 &z_flag, u32 &c_flag)
{
  u32 rd = opcode & 0x07;
  u32 rs = (opcode >> 3) & 0x07;
  u32 offset = (opcode >> 6) & 0x1F;
  u32 src = reg[rs];
  u32 dest = src << offset;

  if(offset)
    c_flag = (src >> (32 - offset)) & 0x01;

  n_flag = dest >> 31;
  z_flag = dest == 0;
  reg[rd] = dest;
  reg[REG_PC] += 2;
  gba_thumb_low_fast_ops++;
  return true;
}

static inline __attribute__((always_inline)) bool gba_thumb_execute_addsub_reg_fast(u32 opcode,
    u32 &n_flag, u32 &z_flag, u32 &c_flag, u32 &v_flag)
{
  u32 rd = opcode & 0x07;
  u32 rs = (opcode >> 3) & 0x07;
  u32 rn = (opcode >> 6) & 0x07;
  u32 lhs = reg[rs];
  u32 rhs = reg[rn];
  u32 dest;

  if(opcode & 0x0200)
  {
    dest = lhs - rhs;
    n_flag = dest >> 31;
    z_flag = dest == 0;
    c_flag = lhs >= rhs;
    v_flag = ((lhs ^ rhs) & (lhs ^ dest)) >> 31;
  }
  else
  {
    dest = lhs + rhs;
    n_flag = dest >> 31;
    z_flag = dest == 0;
    c_flag = dest < rhs;
    v_flag = (~(lhs ^ rhs) & (lhs ^ dest)) >> 31;
  }

  reg[rd] = dest;
  reg[REG_PC] += 2;
  gba_thumb_low_fast_ops++;
  return true;
}

static inline __attribute__((always_inline)) bool gba_thumb_execute_addsub_imm3_fast(u32 opcode,
    u32 &n_flag, u32 &z_flag, u32 &c_flag, u32 &v_flag)
{
  u32 rd = opcode & 0x07;
  u32 rs = (opcode >> 3) & 0x07;
  u32 lhs = reg[rs];
  u32 rhs = (opcode >> 6) & 0x07;
  u32 dest;

  if(opcode & 0x0200)
  {
    dest = lhs - rhs;
    n_flag = dest >> 31;
    z_flag = dest == 0;
    c_flag = lhs >= rhs;
    v_flag = ((lhs ^ rhs) & (lhs ^ dest)) >> 31;
  }
  else
  {
    dest = lhs + rhs;
    n_flag = dest >> 31;
    z_flag = dest == 0;
    c_flag = dest < rhs;
    v_flag = (~(lhs ^ rhs) & (lhs ^ dest)) >> 31;
  }

  reg[rd] = dest;
  reg[REG_PC] += 2;
  gba_thumb_low_fast_ops++;
  return true;
}

static inline bool gba_thumb_execute_imm_fast(u32 opcode, u32 &n_flag, u32 &z_flag,
    u32 &c_flag, u32 &v_flag)
{
  u32 op = (opcode >> 8) & 0xFF;
  if(op < 0x20 || op > 0x3F)
    return false;

  u32 rd = op & 0x07;
  u32 imm = opcode & 0xFF;
  u32 src = reg[rd];
  u32 dest;

  switch(op >> 3)
  {
    case 0x04:
      /* MOV r0..7, imm */
      dest = imm;
      n_flag = dest >> 31;
      z_flag = dest == 0;
      reg[rd] = dest;
      break;

    case 0x05:
      /* CMP r0..7, imm */
      dest = src - imm;
      n_flag = dest >> 31;
      z_flag = dest == 0;
      c_flag = src >= imm;
      v_flag = ((src ^ imm) & (src ^ dest)) >> 31;
      break;

    case 0x06:
      /* ADD r0..7, imm */
      dest = src + imm;
      n_flag = dest >> 31;
      z_flag = dest == 0;
      c_flag = dest < imm;
      v_flag = (~(src ^ imm) & (src ^ dest)) >> 31;
      reg[rd] = dest;
      break;

    default:
      /* SUB r0..7, imm */
      dest = src - imm;
      n_flag = dest >> 31;
      z_flag = dest == 0;
      c_flag = src >= imm;
      v_flag = ((src ^ imm) & (src ^ dest)) >> 31;
      reg[rd] = dest;
      break;
  }

  reg[REG_PC] += 2;
  gba_thumb_imm_fast_ops++;
  return true;
}

static inline __attribute__((always_inline)) bool gba_thumb_execute_alu_fast(u32 opcode, u32 &n_flag, u32 &z_flag,
    u32 &c_flag, u32 &v_flag)
{
  u32 op = (opcode >> 8) & 0xFF;
  if(op < 0x40 || op > 0x43)
    return false;

  u32 rs = (opcode >> 3) & 0x07;
  u32 rd = opcode & 0x07;
  u32 lhs = reg[rd];
  u32 rhs = reg[rs];
  u32 dest;

  switch(op)
  {
    case 0x40:
      switch((opcode >> 6) & 0x03)
      {
        case 0x00:
          /* AND rd, rs */
          dest = lhs & rhs;
          n_flag = dest >> 31;
          z_flag = dest == 0;
          reg[rd] = dest;
          break;
        case 0x01:
          /* EOR rd, rs */
          dest = lhs ^ rhs;
          n_flag = dest >> 31;
          z_flag = dest == 0;
          reg[rd] = dest;
          break;
        case 0x02:
          /* LSL rd, rs */
          dest = lhs;
          if(rhs != 0)
          {
            if(rhs > 31)
            {
              c_flag = (rhs == 32) ? (dest & 0x01) : 0;
              dest = 0;
            }
            else
            {
              c_flag = (dest >> (32 - rhs)) & 0x01;
              dest <<= rhs;
            }
          }
          n_flag = dest >> 31;
          z_flag = dest == 0;
          reg[rd] = dest;
          break;
        default:
          /* LSR rd, rs */
          dest = lhs;
          if(rhs != 0)
          {
            if(rhs > 31)
            {
              c_flag = (rhs == 32) ? (dest >> 31) : 0;
              dest = 0;
            }
            else
            {
              c_flag = (dest >> (rhs - 1)) & 0x01;
              dest >>= rhs;
            }
          }
          n_flag = dest >> 31;
          z_flag = dest == 0;
          reg[rd] = dest;
          break;
      }
      break;

    case 0x41:
      switch((opcode >> 6) & 0x03)
      {
        case 0x00:
          /* ASR rd, rs */
          dest = lhs;
          if(rhs != 0)
          {
            if(rhs > 31)
            {
              dest = (s32)dest >> 31;
              c_flag = dest & 0x01;
            }
            else
            {
              c_flag = (dest >> (rhs - 1)) & 0x01;
              dest = (s32)dest >> rhs;
            }
          }
          n_flag = dest >> 31;
          z_flag = dest == 0;
          reg[rd] = dest;
          break;
        case 0x01:
          /* ADC rd, rs */
          {
            u32 carry = c_flag;
            dest = lhs + rhs;
            c_flag = (dest < rhs);
            dest += carry;
            c_flag |= (dest < carry);
            n_flag = dest >> 31;
            z_flag = dest == 0;
            v_flag = (~(lhs ^ rhs) & (lhs ^ dest)) >> 31;
            reg[rd] = dest;
          }
          break;
        case 0x02:
          /* SBC rd, rs */
          {
            u32 carry = c_flag;
            dest = lhs + (~rhs) + carry;
            n_flag = dest >> 31;
            z_flag = dest == 0;
            c_flag = (lhs > rhs) || (lhs == rhs && carry);
            v_flag = ((lhs ^ rhs) & (lhs ^ dest)) >> 31;
            reg[rd] = dest;
          }
          break;
        default:
          /* ROR rd, rs */
          dest = lhs;
          if(rhs != 0)
          {
            u32 shift = rhs & 31;
            c_flag = (dest >> ((rhs - 1) & 31)) & 0x01;
            if(shift != 0)
              dest = (dest >> shift) | (dest << (32 - shift));
          }
          n_flag = dest >> 31;
          z_flag = dest == 0;
          reg[rd] = dest;
          break;
      }
      break;

    case 0x42:
      switch((opcode >> 6) & 0x03)
      {
        case 0x00:
          /* TST rd, rs */
          dest = lhs & rhs;
          n_flag = dest >> 31;
          z_flag = dest == 0;
          break;
        case 0x01:
          /* NEG rd, rs */
          dest = 0 - rhs;
          n_flag = dest >> 31;
          z_flag = dest == 0;
          c_flag = rhs == 0;
          v_flag = rhs == 0x80000000;
          reg[rd] = dest;
          break;
        case 0x02:
          /* CMP rd, rs */
          dest = lhs - rhs;
          n_flag = dest >> 31;
          z_flag = dest == 0;
          c_flag = lhs >= rhs;
          v_flag = ((lhs ^ rhs) & (lhs ^ dest)) >> 31;
          break;
        default:
          /* CMN rd, rs */
          dest = lhs + rhs;
          n_flag = dest >> 31;
          z_flag = dest == 0;
          c_flag = dest < rhs;
          v_flag = (~(lhs ^ rhs) & (lhs ^ dest)) >> 31;
          break;
      }
      break;

    default:
      switch((opcode >> 6) & 0x03)
      {
        case 0x00:
          /* ORR rd, rs */
          dest = lhs | rhs;
          n_flag = dest >> 31;
          z_flag = dest == 0;
          reg[rd] = dest;
          break;
        case 0x01:
          /* MUL rd, rs */
          dest = lhs * rhs;
          n_flag = dest >> 31;
          z_flag = dest == 0;
          reg[rd] = dest;
          break;
        case 0x02:
          /* BIC rd, rs */
          dest = lhs & ~rhs;
          n_flag = dest >> 31;
          z_flag = dest == 0;
          reg[rd] = dest;
          break;
        case 0x03:
          /* MVN rd, rs */
          dest = ~rhs;
          n_flag = dest >> 31;
          z_flag = dest == 0;
          reg[rd] = dest;
          break;
        default:
          return false;
      }
      break;
  }

  reg[REG_PC] += 2;
  gba_thumb_alu_fast_ops++;
  return true;
}

static inline bool gba_thumb_execute_hireg_fast(u32 opcode, u32 &n_flag,
    u32 &z_flag, u32 &c_flag, u32 &v_flag)
{
  u32 op = (opcode >> 8) & 0xFF;
  if(op < 0x44 || op > 0x46)
    return false;

  u32 rs = (opcode >> 3) & 0x0F;
  u32 rd = ((opcode >> 4) & 0x08) | (opcode & 0x07);

  if(op == 0x46 && rd != REG_PC && rs != REG_PC)
  {
    reg[rd] = reg[rs];
    reg[REG_PC] += 2;
    gba_thumb_hireg_fast_ops++;
    return true;
  }

  u32 rhs = (rs == REG_PC) ? (reg[REG_PC] + 4) : reg[rs];

  if(op == 0x45)
  {
    u32 lhs = (rd == REG_PC) ? (reg[REG_PC] + 4) : reg[rd];
    u32 dest = lhs - rhs;
    n_flag = dest >> 31;
    z_flag = dest == 0;
    c_flag = lhs >= rhs;
    v_flag = ((lhs ^ rhs) & (lhs ^ dest)) >> 31;
    reg[REG_PC] += 2;
    gba_thumb_hireg_fast_ops++;
    return true;
  }

  u32 dest = (op == 0x44) ? (((rd == REG_PC) ? (reg[REG_PC] + 4) : reg[rd]) + rhs) : rhs;
  if(rd == REG_PC)
  {
    reg[REG_PC] = dest & ~1U;
    gba_thumb_hireg_fast_ops++;
    return true;
  }
  else
    reg[rd] = dest;
  reg[REG_PC] += 2;
  gba_thumb_hireg_fast_ops++;
  return true;
}

static inline bool gba_thumb_execute_branch_fast(u32 opcode, u32 n_flag,
    u32 z_flag, u32 c_flag, u32 v_flag, s32 &cycles_remaining)
{
  u32 op = (opcode >> 8) & 0xFF;
  if(op < 0xD0 || op > 0xDD)
    return false;

  bool taken;
  switch(op & 0x0F)
  {
    case 0x0: taken = z_flag == 1; break;
    case 0x1: taken = z_flag == 0; break;
    case 0x2: taken = c_flag == 1; break;
    case 0x3: taken = c_flag == 0; break;
    case 0x4: taken = n_flag == 1; break;
    case 0x5: taken = n_flag == 0; break;
    case 0x6: taken = v_flag == 1; break;
    case 0x7: taken = v_flag == 0; break;
    case 0x8: taken = c_flag & (z_flag ^ 1); break;
    case 0x9: taken = (c_flag == 0) | z_flag; break;
    case 0xA: taken = n_flag == v_flag; break;
    case 0xB: taken = n_flag != v_flag; break;
    case 0xC: taken = (z_flag == 0) & (n_flag == v_flag); break;
    default:  taken = z_flag | (n_flag != v_flag); break;
  }

  s32 offset = (s8)(opcode & 0xFF);
  reg[REG_PC] += taken ? ((offset * 2) + 4) : 2;
  cycles_remaining -= ws_cyc_nseq[reg[REG_PC] >> 24][0];
  gba_thumb_branch_fast_ops++;
  return true;
}

static inline bool gba_thumb_load_direct_u32_fast(u32 address, u32 &value);
static inline bool gba_thumb_load_direct_u16_fast(u32 address, u32 &value);
static inline bool gba_thumb_load_direct_u8_fast(u32 address, u32 &value);

static inline bool gba_thumb_execute_ldrb_imm_fast(u32 opcode, s32 &cycles_remaining)
{
  u32 op = (opcode >> 8) & 0xFF;
  if(op < 0x78 || op > 0x7F)
    return false;

  u32 imm = (opcode >> 6) & 0x1F;
  u32 rb = (opcode >> 3) & 0x07;
  u32 rd = opcode & 0x07;
  u32 address = reg[rb] + imm;
  u32 value;
  u8 *map;

  reg[REG_PC] += 2;

  if(address < 0x10000000)
  {
    u8 region = address >> 24;
    cycles_remaining -= ws_cyc_nseq[region][0];
    STATS_MEMORY_ACCESS(read, u8, region);
  }

  if(gba_thumb_load_direct_u8_fast(address, value))
  {
  }
  else if((((address >> 24) == 0) && (reg[REG_PC] >= 0x4000)) ||
          (address & 0xF0000000) ||
          !(map = memory_map_read[address >> 15]))
    value = read_memory8(address);
  else
    value = readaddress8(map, address & 0x7FFF);

  reg[rd] = value;
  gba_thumb_ldrb_fast_ops++;
  return true;
}

static inline bool gba_thumb_execute_ldr_imm_fast(u32 opcode, s32 &cycles_remaining)
{
  u32 op = (opcode >> 8) & 0xFF;
  if(op < 0x68 || op > 0x6F)
    return false;

  u32 imm = (opcode >> 6) & 0x1F;
  u32 rb = (opcode >> 3) & 0x07;
  u32 rd = opcode & 0x07;
  u32 address = reg[rb] + (imm * 4);
  u32 value;
  u8 *map;

  reg[REG_PC] += 2;

  if(address < 0x10000000)
  {
    u8 region = address >> 24;
    cycles_remaining -= ws_cyc_nseq[region][1];
    STATS_MEMORY_ACCESS(read, u32, region);
  }

  if(((address & 0x03) == 0) && gba_thumb_load_direct_u32_fast(address, value))
  {
  }
  else if((((address >> 24) == 0) && (reg[REG_PC] >= 0x4000)) ||
          (address & 0xF0000003) ||
          !(map = memory_map_read[address >> 15]))
    value = read_memory32(address);
  else
    value = readaddress32(map, address & 0x7FFF);

  reg[rd] = value;
  gba_thumb_ldr_fast_ops++;
  return true;
}

static inline bool gba_thumb_execute_ldrh_imm_fast(u32 opcode, s32 &cycles_remaining)
{
  u32 op = (opcode >> 8) & 0xFF;
  if(op < 0x88 || op > 0x8F)
    return false;

  u32 imm = (opcode >> 6) & 0x1F;
  u32 rb = (opcode >> 3) & 0x07;
  u32 rd = opcode & 0x07;
  u32 address = reg[rb] + (imm * 2);
  u32 value;
  u8 *map;

  reg[REG_PC] += 2;

  if(address < 0x10000000)
  {
    u8 region = address >> 24;
    cycles_remaining -= ws_cyc_nseq[region][0];
    STATS_MEMORY_ACCESS(read, u16, region);
  }

  if(((address & 0x01) == 0) && gba_thumb_load_direct_u16_fast(address, value))
  {
  }
  else if((((address >> 24) == 0) && (reg[REG_PC] >= 0x4000)) ||
          (address & 0xF0000001) ||
          !(map = memory_map_read[address >> 15]))
    value = read_memory16(address);
  else
    value = readaddress16(map, address & 0x7FFF);

  reg[rd] = value;
  gba_thumb_ldrh_fast_ops++;
  return true;
}

static inline bool gba_thumb_execute_pcldr_fast(u32 opcode, s32 &cycles_remaining)
{
  u32 op = (opcode >> 8) & 0xFF;
  if(op < 0x48 || op > 0x4F)
    return false;

  u32 rd = (opcode >> 8) & 0x07;
  u32 imm = opcode & 0xFF;
  u32 address = (reg[REG_PC] & ~2) + (imm * 4) + 4;
  u32 value;
  u8 *map;

  reg[REG_PC] += 2;

  if(address < 0x10000000)
  {
    u8 region = address >> 24;
    cycles_remaining -= ws_cyc_nseq[region][1];
    STATS_MEMORY_ACCESS(read, u32, region);
  }

  if((((address >> 24) == 0) && (reg[REG_PC] >= 0x4000)) ||
     (address & 0xF0000003) ||
     !(map = memory_map_read[address >> 15]))
    value = read_memory32(address);
  else
    value = readaddress32(map, address & 0x7FFF);

  reg[rd] = value;
  gba_thumb_pcldr_fast_ops++;
  return true;
}

static inline bool gba_thumb_store_direct_u32_fast(u32 address, u32 value)
{
  switch(address >> 24)
  {
    case 0x02:
      address32(ewram, address & 0x3FFFF) = eswap32(value);
      return true;

    case 0x03:
      address32(iwram, (address & 0x7FFF) + 0x8000) = eswap32(value);
      return true;

    default:
      return false;
  }
}

static inline bool gba_thumb_store_direct_u16_fast(u32 address, u32 value)
{
  switch(address >> 24)
  {
    case 0x02:
      address16(ewram, address & 0x3FFFF) = eswap16((u16)value);
      return true;

    case 0x03:
      address16(iwram, (address & 0x7FFF) + 0x8000) = eswap16((u16)value);
      return true;

    default:
      return false;
  }
}

static inline bool gba_thumb_store_direct_u8_fast(u32 address, u32 value)
{
  switch(address >> 24)
  {
    case 0x02:
      ewram[address & 0x3FFFF] = (u8)value;
      return true;

    case 0x03:
      iwram[(address & 0x7FFF) + 0x8000] = (u8)value;
      return true;

    default:
      return false;
  }
}

static inline bool gba_thumb_execute_store_imm_fast(u32 opcode,
    cpu_alert_type &cpu_alert, s32 &cycles_remaining)
{
  u32 op = (opcode >> 8) & 0xFF;
  if((op < 0x60 || op > 0x67) && (op < 0x70 || op > 0x77) &&
     (op < 0x80 || op > 0x87))
    return false;

  u32 imm = (opcode >> 6) & 0x1F;
  u32 rb = (opcode >> 3) & 0x07;
  u32 rd = opcode & 0x07;
  u32 address = reg[rb] + ((op >= 0x80) ? (imm * 2) : ((op >= 0x70) ? imm : (imm * 4)));

  reg[REG_PC] += 2;

  if(op >= 0x80)
  {
    u32 aligned_address = address & ~1U;
    if(aligned_address < 0x10000000)
    {
      u8 region = aligned_address >> 24;
      cycles_remaining -= ws_cyc_nseq[region][0];
      STATS_MEMORY_ACCESS(write, u16, region);
    }
    if(!gba_thumb_store_direct_u16_fast(aligned_address, reg[rd]))
      cpu_alert |= write_memory16(aligned_address, reg[rd]);
    gba_thumb_strh_fast_ops++;
  }
  else if(op >= 0x70)
  {
    u32 aligned_address = address & ~0U;
    if(aligned_address < 0x10000000)
    {
      u8 region = aligned_address >> 24;
      cycles_remaining -= ws_cyc_nseq[region][0];
      STATS_MEMORY_ACCESS(write, u8, region);
    }
    if(!gba_thumb_store_direct_u8_fast(aligned_address, reg[rd]))
      cpu_alert |= write_memory8(aligned_address, reg[rd]);
    gba_thumb_strb_fast_ops++;
  }
  else
  {
    u32 aligned_address = address & ~3U;
    if(aligned_address < 0x10000000)
    {
      u8 region = aligned_address >> 24;
      cycles_remaining -= ws_cyc_nseq[region][1];
      STATS_MEMORY_ACCESS(write, u32, region);
    }
    if(!gba_thumb_store_direct_u32_fast(aligned_address, reg[rd]))
      cpu_alert |= write_memory32(aligned_address, reg[rd]);
    gba_thumb_str_fast_ops++;
  }

  return true;
}

static inline bool gba_thumb_load_direct_u32_fast(u32 address, u32 &value)
{
  switch(address >> 24)
  {
    case 0x02:
      value = readaddress32(ewram, address & 0x3FFFF);
      return true;

    case 0x03:
      value = readaddress32(iwram, (address & 0x7FFF) + 0x8000);
      return true;

    default:
      return false;
  }
}

static inline bool gba_thumb_load_direct_u16_fast(u32 address, u32 &value)
{
  switch(address >> 24)
  {
    case 0x02:
      value = readaddress16(ewram, address & 0x3FFFF);
      return true;

    case 0x03:
      value = readaddress16(iwram, (address & 0x7FFF) + 0x8000);
      return true;

    default:
      return false;
  }
}

static inline bool gba_thumb_load_direct_u8_fast(u32 address, u32 &value)
{
  switch(address >> 24)
  {
    case 0x02:
      value = ewram[address & 0x3FFFF];
      return true;

    case 0x03:
      value = iwram[(address & 0x7FFF) + 0x8000];
      return true;

    default:
      return false;
  }
}

GBA_HOT_DATA_ATTR const u8 bit_count[256] =
{
  0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 1, 2, 2, 3, 2, 3, 3,
  4, 2, 3, 3, 4, 3, 4, 4, 5, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4,
  4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 1, 2, 2, 3, 2,
  3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5,
  4, 5, 5, 6, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4,
  5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3,
  3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 2,
  3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6,
  4, 5, 5, 6, 5, 6, 6, 7, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5,
  6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 3, 4, 4, 5, 4, 5,
  5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6,
  7, 7, 8
};

static inline u32 gba_thumb_load_aligned32_fast(u32 address, s32 &cycles_remaining)
{
  if(address < 0x10000000)
  {
    u8 region = address >> 24;
    cycles_remaining -= ws_cyc_seq[region][1];
    STATS_MEMORY_ACCESS(read, u32, region);

    u32 value;
    if(gba_thumb_load_direct_u32_fast(address, value))
      return value;

    u8 *map = memory_map_read[address >> 15];
    if(map)
      return readaddress32(map, address & 0x7FFF);
  }

  return read_memory32(address);
}

static inline u32 gba_thumb_load_u32_fast(u32 address, s32 &cycles_remaining)
{
  if(address < 0x10000000)
  {
    u8 region = address >> 24;
    cycles_remaining -= ws_cyc_nseq[region][1];
    STATS_MEMORY_ACCESS(read, u32, region);
  }

  u32 value;
  if(((address & 0x03) == 0) && gba_thumb_load_direct_u32_fast(address, value))
    return value;

  u8 *map;
  if((((address >> 24) == 0) && (reg[REG_PC] >= 0x4000)) ||
     (address & 0xF0000003) ||
     !(map = memory_map_read[address >> 15]))
    return read_memory32(address);

  return readaddress32(map, address & 0x7FFF);
}

static inline u32 gba_thumb_load_u16_fast(u32 address, s32 &cycles_remaining)
{
  if(address < 0x10000000)
  {
    u8 region = address >> 24;
    cycles_remaining -= ws_cyc_nseq[region][0];
    STATS_MEMORY_ACCESS(read, u16, region);
  }

  u32 value;
  if(((address & 0x01) == 0) && gba_thumb_load_direct_u16_fast(address, value))
    return value;

  u8 *map;
  if((((address >> 24) == 0) && (reg[REG_PC] >= 0x4000)) ||
     (address & 0xF0000001) ||
     !(map = memory_map_read[address >> 15]))
    return read_memory16(address);

  return readaddress16(map, address & 0x7FFF);
}

static inline u32 gba_thumb_load_u8_fast(u32 address, s32 &cycles_remaining)
{
  if(address < 0x10000000)
  {
    u8 region = address >> 24;
    cycles_remaining -= ws_cyc_nseq[region][0];
    STATS_MEMORY_ACCESS(read, u8, region);
  }

  u32 value;
  if(gba_thumb_load_direct_u8_fast(address, value))
    return value;

  u8 *map;
  if((((address >> 24) == 0) && (reg[REG_PC] >= 0x4000)) ||
     (address & 0xF0000000) ||
     !(map = memory_map_read[address >> 15]))
    return read_memory8(address);

  return readaddress8(map, address & 0x7FFF);
}

static inline u32 gba_thumb_load_s16_fast(u32 address, s32 &cycles_remaining)
{
  if(address < 0x10000000)
  {
    u8 region = address >> 24;
    cycles_remaining -= ws_cyc_nseq[region][0];
    STATS_MEMORY_ACCESS(read, s16, region);

    if((address & 0x01) == 0)
    {
      u32 value;
      if(gba_thumb_load_direct_u16_fast(address, value))
        return (u32)((s16)value);

      u8 *map = memory_map_read[address >> 15];
      if(map)
        return (u32)((s16)readaddress16(map, address & 0x7FFF));
    }
  }

  return (u32)((s16)read_memory16_signed(address));
}

static inline u32 gba_thumb_load_s8_fast(u32 address, s32 &cycles_remaining)
{
  if(address < 0x10000000)
  {
    u8 region = address >> 24;
    cycles_remaining -= ws_cyc_nseq[region][0];
    STATS_MEMORY_ACCESS(read, s8, region);

    u32 value;
    if(gba_thumb_load_direct_u8_fast(address, value))
      return (u32)((s8)value);

    u8 *map = memory_map_read[address >> 15];
    if(map)
      return (u32)((s8)readaddress8(map, address & 0x7FFF));
  }

  return read_memory8s(address);
}

static inline void gba_thumb_store_aligned32_fast(u32 address, u32 value,
    cpu_alert_type &cpu_alert, s32 &cycles_remaining)
{
  if(address < 0x10000000)
  {
    u8 region = address >> 24;
    cycles_remaining -= ws_cyc_seq[region][1];
    STATS_MEMORY_ACCESS(write, u32, region);
  }

  if(!gba_thumb_store_direct_u32_fast(address, value))
    cpu_alert |= write_memory32(address, value);
}

static inline void gba_thumb_store_u32_fast(u32 address, u32 value,
    cpu_alert_type &cpu_alert, s32 &cycles_remaining)
{
  u32 aligned_address = address & ~3U;
  if(aligned_address < 0x10000000)
  {
    u8 region = aligned_address >> 24;
    cycles_remaining -= ws_cyc_nseq[region][1];
    STATS_MEMORY_ACCESS(write, u32, region);
  }

  if(!gba_thumb_store_direct_u32_fast(aligned_address, value))
    cpu_alert |= write_memory32(aligned_address, value);
}

static inline void gba_thumb_store_u16_fast(u32 address, u32 value,
    cpu_alert_type &cpu_alert, s32 &cycles_remaining)
{
  u32 aligned_address = address & ~1U;
  if(aligned_address < 0x10000000)
  {
    u8 region = aligned_address >> 24;
    cycles_remaining -= ws_cyc_nseq[region][0];
    STATS_MEMORY_ACCESS(write, u16, region);
  }

  if(!gba_thumb_store_direct_u16_fast(aligned_address, value))
    cpu_alert |= write_memory16(aligned_address, value);
}

static inline void gba_thumb_store_u8_fast(u32 address, u32 value,
    cpu_alert_type &cpu_alert, s32 &cycles_remaining)
{
  if(address < 0x10000000)
  {
    u8 region = address >> 24;
    cycles_remaining -= ws_cyc_nseq[region][0];
    STATS_MEMORY_ACCESS(write, u8, region);
  }

  if(!gba_thumb_store_direct_u8_fast(address, value))
    cpu_alert |= write_memory8(address, value);
}

static inline bool gba_thumb_direct_ram_span(u32 address, u32 bytes,
    u8 *&base, u32 &mask, u8 &region)
{
  region = address >> 24;
  if(region == 0x02)
  {
    if(((address & 0x3FFFF) + bytes) <= 0x40000)
    {
      base = ewram;
      mask = 0x3FFFF;
      return true;
    }
  }
  else if(region == 0x03)
  {
    if(((address & 0x7FFF) + bytes) <= 0x8000)
    {
      base = iwram + 0x8000;
      mask = 0x7FFF;
      return true;
    }
  }

  return false;
}

static inline bool gba_thumb_execute_pushpop_fast(u32 opcode,
    cpu_alert_type &cpu_alert, s32 &cycles_remaining)
{
  u32 op = (opcode >> 8) & 0xFF;
  if(op != 0xB4 && op != 0xB5 && op != 0xBC && op != 0xBD)
    return false;

  u32 reglist = opcode & 0xFF;

  if(op == 0xB4 || op == 0xB5)
  {
    u32 has_lr = (op == 0xB5);
    u32 numops = bit_count[reglist] + has_lr;
    u32 address = (reg[REG_SP] - (numops * 4)) & ~3U;

    reg[REG_SP] = address;
    reg[REG_PC] += 2;

    u8 *base;
    u32 mask;
    u8 region;
    if(gba_thumb_direct_ram_span(address, numops * 4, base, mask, region))
    {
      for(u32 i = 0; i < 8; i++)
      {
        if((reglist >> i) & 0x01)
        {
          cycles_remaining -= ws_cyc_seq[region][1];
          address32(base, address & mask) = eswap32(reg[i]);
          address += 4;
        }
      }

      if(has_lr)
      {
        cycles_remaining -= ws_cyc_seq[region][1];
        address32(base, address & mask) = eswap32(reg[REG_LR]);
      }
    }
    else
    {
      for(u32 i = 0; i < 8; i++)
      {
        if((reglist >> i) & 0x01)
        {
          gba_thumb_store_aligned32_fast(address, reg[i], cpu_alert, cycles_remaining);
          address += 4;
        }
      }

      if(has_lr)
        gba_thumb_store_aligned32_fast(address, reg[REG_LR], cpu_alert, cycles_remaining);
    }

    gba_thumb_push_fast_ops++;
    return true;
  }

  u32 has_pc = (op == 0xBD);
  u32 numops = bit_count[reglist] + has_pc;
  u32 address = reg[REG_SP] & ~3U;

  reg[REG_SP] += numops * 4;
  reg[REG_PC] += 2;

  u8 *base;
  u32 mask;
  u8 region;
  if(gba_thumb_direct_ram_span(address, numops * 4, base, mask, region))
  {
    for(u32 i = 0; i < 8; i++)
    {
      if((reglist >> i) & 0x01)
      {
        cycles_remaining -= ws_cyc_seq[region][1];
        reg[i] = readaddress32(base, address & mask);
        address += 4;
      }
    }

    if(has_pc)
    {
      cycles_remaining -= ws_cyc_seq[region][1];
      reg[REG_PC] = readaddress32(base, address & mask) & ~1U;
    }
  }
  else
  {
    for(u32 i = 0; i < 8; i++)
    {
      if((reglist >> i) & 0x01)
      {
        reg[i] = gba_thumb_load_aligned32_fast(address, cycles_remaining);
        address += 4;
      }
    }

    if(has_pc)
      reg[REG_PC] = gba_thumb_load_aligned32_fast(address, cycles_remaining) & ~1U;
  }

  gba_thumb_pop_fast_ops++;
  return true;
}

static inline bool gba_thumb_execute_blockmem_fast(u32 opcode,
    cpu_alert_type &cpu_alert, s32 &cycles_remaining)
{
  u32 op = (opcode >> 8) & 0xFF;
  if(op < 0xC0 || op > 0xCF)
    return false;

  u32 rn = op & 0x07;
  u32 reglist = opcode & 0xFF;
  u32 numops = bit_count[reglist];
  u32 address = reg[rn] & ~3U;
  u32 endaddr = reg[rn] + (numops * 4);
  bool is_load = op >= 0xC8;

  if(is_load || !((reglist & (1U << rn)) && ((reglist & ((1U << rn) - 1)) == 0)))
    reg[rn] = endaddr;

  reg[REG_PC] += 2;

  if(is_load)
  {
    u8 *base;
    u32 mask;
    u8 region;
    if(gba_thumb_direct_ram_span(address, numops * 4, base, mask, region))
    {
      for(u32 i = 0; i < 8; i++)
      {
        if((reglist >> i) & 0x01)
        {
          cycles_remaining -= ws_cyc_seq[region][1];
          reg[i] = readaddress32(base, address & mask);
          address += 4;
        }
      }
    }
    else
    {
      for(u32 i = 0; i < 8; i++)
      {
        if((reglist >> i) & 0x01)
        {
          reg[i] = gba_thumb_load_aligned32_fast(address, cycles_remaining);
          address += 4;
        }
      }
    }
    gba_thumb_pop_fast_ops++;
  }
  else
  {
    u8 *base;
    u32 mask;
    u8 region;
    if(gba_thumb_direct_ram_span(address, numops * 4, base, mask, region))
    {
      for(u32 i = 0; i < 8; i++)
      {
        if((reglist >> i) & 0x01)
        {
          cycles_remaining -= ws_cyc_seq[region][1];
          address32(base, address & mask) = eswap32(reg[i]);
          address += 4;
        }
      }
    }
    else
    {
      for(u32 i = 0; i < 8; i++)
      {
        if((reglist >> i) & 0x01)
        {
          gba_thumb_store_aligned32_fast(address, reg[i], cpu_alert, cycles_remaining);
          address += 4;
        }
      }
    }
    if((reglist & (1U << rn)) && ((reglist & ((1U << rn) - 1)) == 0))
      reg[rn] = endaddr;
    gba_thumb_push_fast_ops++;
  }

  return true;
}

static inline bool gba_thumb_execute_spmem_fast(u32 opcode,
    cpu_alert_type &cpu_alert, s32 &cycles_remaining)
{
  u32 op = (opcode >> 8) & 0xFF;
  if(op < 0x90 || op > 0x9F)
    return false;

  u32 rd = op & 0x07;
  u32 address = (reg[REG_SP] + ((opcode & 0xFF) * 4)) & ~3U;

  reg[REG_PC] += 2;

  if(op < 0x98)
  {
    gba_thumb_store_aligned32_fast(address, reg[rd], cpu_alert, cycles_remaining);
    gba_thumb_str_fast_ops++;
  }
  else
  {
    reg[rd] = gba_thumb_load_aligned32_fast(address, cycles_remaining);
    gba_thumb_ldr_fast_ops++;
  }

  return true;
}

static inline bool gba_thumb_execute_uncond_branch_fast(u32 opcode,
    s32 &cycles_remaining)
{
  u32 op = (opcode >> 8) & 0xFF;
  if(op < 0xE0 || op > 0xE7)
    return false;

  u32 offset = opcode & 0x7FF;
  s32 br_offset = ((s32)(offset << 21) >> 20) + 4;
  reg[REG_PC] += br_offset;
  cycles_remaining -= ws_cyc_nseq[reg[REG_PC] >> 24][0];
  gba_thumb_branch_fast_ops++;
  return true;
}

static inline bool gba_thumb_execute_add_sp_fast(u32 opcode)
{
  u32 op = (opcode >> 8) & 0xFF;
  if(op < 0xB0 || op > 0xB3)
    return false;

  u32 imm = (opcode & 0x7F) * 4;
  if((opcode >> 7) & 0x01)
    reg[REG_SP] -= imm;
  else
    reg[REG_SP] += imm;

  reg[REG_PC] += 2;
  gba_thumb_imm_fast_ops++;
  return true;
}

static inline bool gba_thumb_execute_add_pcsp_fast(u32 opcode)
{
  u32 op = (opcode >> 8) & 0xFF;
  if(op < 0xA0 || op > 0xAF)
    return false;

  u32 rd = op & 0x07;
  u32 base = (op < 0xA8) ? ((reg[REG_PC] & ~2U) + 4) : reg[REG_SP];
  reg[rd] = base + ((opcode & 0xFF) * 4);
  reg[REG_PC] += 2;
  gba_thumb_imm_fast_ops++;
  return true;
}

static inline bool gba_thumb_execute_ldsh_reg_fast(u32 opcode,
    s32 &cycles_remaining)
{
  u32 op = (opcode >> 8) & 0xFF;
  if(op != 0x5E && op != 0x5F)
    return false;

  u32 ro = (opcode >> 6) & 0x07;
  u32 rb = (opcode >> 3) & 0x07;
  u32 rd = opcode & 0x07;
  u32 address = reg[rb] + reg[ro];

  reg[REG_PC] += 2;
  reg[rd] = gba_thumb_load_s16_fast(address, cycles_remaining);
  gba_thumb_ldrh_fast_ops++;
  return true;
}

static inline bool gba_thumb_execute_ldsb_reg_fast(u32 opcode,
    s32 &cycles_remaining)
{
  u32 op = (opcode >> 8) & 0xFF;
  if(op != 0x56 && op != 0x57)
    return false;

  u32 ro = (opcode >> 6) & 0x07;
  u32 rb = (opcode >> 3) & 0x07;
  u32 rd = opcode & 0x07;
  u32 address = reg[rb] + reg[ro];

  reg[REG_PC] += 2;
  reg[rd] = gba_thumb_load_s8_fast(address, cycles_remaining);
  gba_thumb_ldrb_fast_ops++;
  return true;
}

static inline __attribute__((always_inline)) bool gba_thumb_execute_mem_reg_fast(u32 opcode,
    cpu_alert_type &cpu_alert, s32 &cycles_remaining)
{
  u32 op = (opcode >> 8) & 0xFF;
  if(op < 0x50 || op > 0x5D || op == 0x56 || op == 0x57)
    return false;

  u32 ro = (opcode >> 6) & 0x07;
  u32 rb = (opcode >> 3) & 0x07;
  u32 rd = opcode & 0x07;
  u32 address = reg[rb] + reg[ro];

  reg[REG_PC] += 2;

  switch(op & 0x0E)
  {
    case 0x00:
      gba_thumb_store_u32_fast(address, reg[rd], cpu_alert, cycles_remaining);
      gba_thumb_str_fast_ops++;
      return true;

    case 0x02:
      gba_thumb_store_u16_fast(address, reg[rd], cpu_alert, cycles_remaining);
      gba_thumb_strh_fast_ops++;
      return true;

    case 0x04:
      gba_thumb_store_u8_fast(address, reg[rd], cpu_alert, cycles_remaining);
      gba_thumb_strb_fast_ops++;
      return true;

    case 0x08:
      reg[rd] = gba_thumb_load_u32_fast(address, cycles_remaining);
      gba_thumb_ldr_fast_ops++;
      return true;

    case 0x0A:
      reg[rd] = gba_thumb_load_u16_fast(address, cycles_remaining);
      gba_thumb_ldrh_fast_ops++;
      return true;

    case 0x0C:
      reg[rd] = gba_thumb_load_u8_fast(address, cycles_remaining);
      gba_thumb_ldrb_fast_ops++;
      return true;

    default:
      return false;
  }
}

static inline u32 gba_hle_sqrt_u32(u32 value)
{
  u32 root = 0;
  u32 bit = 1u << 30;

  while(bit > value)
    bit >>= 2;

  while(bit)
  {
    if(value >= root + bit)
    {
      value -= root + bit;
      root = (root >> 1) + bit;
    }
    else
    {
      root >>= 1;
    }
    bit >>= 2;
  }

  return root;
}

static inline bool gba_hle_div(s32 numerator, s32 denominator)
{
  if(denominator == 0)
    return false;

  s32 quotient = numerator / denominator;
  s32 remainder = numerator % denominator;

  reg[0] = (u32)quotient;
  reg[1] = (u32)remainder;
  reg[3] = (quotient < 0) ? (u32)(-(s64)quotient) : (u32)quotient;
  return true;
}

static const s16 gba_hle_sine_quarter[65] =
{
  (s16)0x0000, (s16)0x0192, (s16)0x0323, (s16)0x04B5,
  (s16)0x0645, (s16)0x07D5, (s16)0x0964, (s16)0x0AF1,
  (s16)0x0C7C, (s16)0x0E05, (s16)0x0F8C, (s16)0x1111,
  (s16)0x1294, (s16)0x1413, (s16)0x158F, (s16)0x1708,
  (s16)0x187D, (s16)0x19EF, (s16)0x1B5D, (s16)0x1CC6,
  (s16)0x1E2B, (s16)0x1F8B, (s16)0x20E7, (s16)0x223D,
  (s16)0x238E, (s16)0x24DA, (s16)0x261F, (s16)0x275F,
  (s16)0x2899, (s16)0x29CD, (s16)0x2AFA, (s16)0x2C21,
  (s16)0x2D41, (s16)0x2E5A, (s16)0x2F6B, (s16)0x3076,
  (s16)0x3179, (s16)0x3274, (s16)0x3367, (s16)0x3453,
  (s16)0x3536, (s16)0x3612, (s16)0x36E5, (s16)0x37AF,
  (s16)0x3871, (s16)0x392A, (s16)0x39DA, (s16)0x3A82,
  (s16)0x3B20, (s16)0x3BB6, (s16)0x3C42, (s16)0x3CC5,
  (s16)0x3D3E, (s16)0x3DAE, (s16)0x3E14, (s16)0x3E71,
  (s16)0x3EC5, (s16)0x3F0E, (s16)0x3F4E, (s16)0x3F84,
  (s16)0x3FB1, (s16)0x3FD3, (s16)0x3FEC, (s16)0x3FFB,
  (s16)0x4000
};

static inline s32 gba_hle_sine(u32 theta)
{
  theta &= 0xFF;
  if(theta <= 0x40)
    return gba_hle_sine_quarter[theta];
  if(theta <= 0x80)
    return gba_hle_sine_quarter[0x80 - theta];
  if(theta <= 0xC0)
    return -gba_hle_sine_quarter[theta - 0x80];
  return -gba_hle_sine_quarter[0x100 - theta];
}

static inline u8 gba_hle_read8_fast(u32 address)
{
  switch(address >> 24)
  {
    case 0x02:
      return readaddress8(ewram, address & 0x3FFFF);

    case 0x03:
      return readaddress8(iwram, (address & 0x7FFF) + 0x8000);

    case 0x05:
      return readaddress8(palette_ram, address & 0x3FF);

    case 0x06:
      address &= 0x1FFFF;
      if(address >= 0x18000)
        address -= 0x8000;
      return readaddress8(vram, address);

    case 0x07:
      return readaddress8(oam_ram, address & 0x3FF);

    default:
      return (u8)read_memory8(address);
  }
}

static inline u16 gba_hle_read16_fast(u32 address)
{
  switch(address >> 24)
  {
    case 0x02:
      return readaddress16(ewram, address & 0x3FFFF);

    case 0x03:
      return readaddress16(iwram, (address & 0x7FFF) + 0x8000);

    case 0x05:
      return readaddress16(palette_ram, address & 0x3FF);

    case 0x06:
      address &= 0x1FFFF;
      if(address >= 0x18000)
        address -= 0x8000;
      return readaddress16(vram, address);

    case 0x07:
      return readaddress16(oam_ram, address & 0x3FF);

    default:
      return (u16)read_memory16(address);
  }
}

static inline u32 gba_hle_read32_fast(u32 address)
{
  switch(address >> 24)
  {
    case 0x02:
      return readaddress32(ewram, address & 0x3FFFF);

    case 0x03:
      return readaddress32(iwram, (address & 0x7FFF) + 0x8000);

    case 0x05:
      return readaddress32(palette_ram, address & 0x3FF);

    case 0x06:
      address &= 0x1FFFF;
      if(address >= 0x18000)
        address -= 0x8000;
      return readaddress32(vram, address);

    case 0x07:
      return readaddress32(oam_ram, address & 0x3FF);

    default:
      return read_memory32(address);
  }
}

static inline void gba_hle_write8_fast(u32 address, u8 value,
    cpu_alert_type &cpu_alert)
{
  switch(address >> 24)
  {
    case 0x02:
      address8(ewram, address & 0x3FFFF) = value;
      break;

    case 0x03:
      address8(iwram, (address & 0x7FFF) + 0x8000) = value;
      break;

    default:
      cpu_alert |= write_memory8(address, value);
      break;
  }
}

static inline void gba_hle_write16_fast(u32 address, u16 value,
    cpu_alert_type &cpu_alert)
{
  switch(address >> 24)
  {
    case 0x02:
      address16(ewram, address & 0x3FFFF) = eswap16(value);
      break;

    case 0x03:
      address16(iwram, (address & 0x7FFF) + 0x8000) = eswap16(value);
      break;

    case 0x06:
      address &= 0x1FFFF;
      if(address >= 0x18000)
        address -= 0x8000;
      address16(vram, address) = eswap16(value);
      break;

    case 0x07:
      reg[OAM_UPDATED] = 1;
      address16(oam_ram, address & 0x3FF) = eswap16(value);
      break;

    default:
      cpu_alert |= write_memory16(address, value);
      break;
  }
}

static inline void gba_hle_write32_fast(u32 address, u32 value,
    cpu_alert_type &cpu_alert)
{
  switch(address >> 24)
  {
    case 0x02:
      address32(ewram, address & 0x3FFFF) = eswap32(value);
      break;

    case 0x03:
      address32(iwram, (address & 0x7FFF) + 0x8000) = eswap32(value);
      break;

    case 0x06:
      address &= 0x1FFFF;
      if(address >= 0x18000)
        address -= 0x8000;
      address32(vram, address) = eswap32(value);
      break;

    case 0x07:
      reg[OAM_UPDATED] = 1;
      address32(oam_ram, address & 0x3FF) = eswap32(value);
      break;

    default:
      cpu_alert |= write_memory32(address, value);
      break;
  }
}

static inline void gba_hle_cpuset(cpu_alert_type &cpu_alert, bool fast)
{
  u32 src = reg[0];
  u32 dst = reg[1];
  u32 mode = reg[2];
  bool fixed = (mode & (1u << 24)) != 0;
  bool word = fast || ((mode & (1u << 26)) != 0);
  u32 count = mode & 0x001FFFFF;

  // CpuFastSet's r2 bits 0-20 are a word count that callers are required to keep
  // a multiple of eight -- the eight-word block is the BIOS's internal unrolling,
  // not the unit of the field. Scaling by eight here overran every destination by
  // 8x and quietly shredded whatever followed it in memory.

  if(word)
  {
    src &= ~3U;
    dst &= ~3U;
    u32 value = fixed ? gba_hle_read32_fast(src) : 0;
    for(u32 i = 0; i < count; i++)
    {
      if(!fixed)
        value = gba_hle_read32_fast(src), src += 4;
      gba_hle_write32_fast(dst, value, cpu_alert);
      dst += 4;
    }
  }
  else
  {
    src &= ~1U;
    dst &= ~1U;
    u32 value = fixed ? gba_hle_read16_fast(src) : 0;
    for(u32 i = 0; i < count; i++)
    {
      if(!fixed)
        value = gba_hle_read16_fast(src), src += 2;
      gba_hle_write16_fast(dst, (u16)value, cpu_alert);
      dst += 2;
    }
  }
}

static inline bool gba_hle_source_range_valid(u32 source, u32 len)
{
  return ((source & 0x0E000000) != 0) &&
         (((source + len) & 0x0E000000) != 0);
}

static inline void gba_hle_bitunpack(cpu_alert_type &cpu_alert)
{
  u32 src = reg[0];
  u32 dst = reg[1];
  u32 info = reg[2];
  int len = gba_hle_read16_fast(info);

  gba_swi_hle_unpack_count++;

  if(!gba_hle_source_range_valid(src, (u32)len))
    return;

  int bits = gba_hle_read8_fast(info + 2);
  int data_size = gba_hle_read8_fast(info + 3);
  u32 base = gba_hle_read32_fast(info + 4);
  bool add_base = (base & 0x80000000) != 0;
  base &= 0x7FFFFFFF;

  if(bits <= 0 || bits > 8 || data_size <= 0 || data_size > 32)
    return;

  int revbits = 8 - bits;
  u32 data = 0;
  int bitwritecount = 0;

  while(len-- > 0)
  {
    u32 mask = 0xFF >> revbits;
    u8 b = gba_hle_read8_fast(src++);
    int bitcount = 0;

    while(bitcount < 8)
    {
      u32 d = b & mask;
      u32 value = d >> bitcount;
      if(d || add_base)
        value += base;

      data |= value << bitwritecount;
      bitwritecount += data_size;
      if(bitwritecount >= 32)
      {
        gba_hle_write32_fast(dst, data, cpu_alert);
        dst += 4;
        data = 0;
        bitwritecount = 0;
      }

      mask <<= bits;
      bitcount += bits;
    }
  }
}

static inline void gba_hle_lz77_uncomp(cpu_alert_type &cpu_alert, bool vram_dest)
{
  u32 src = reg[0];
  u32 dst = reg[1];
  u32 header = gba_hle_read32_fast(src);
  src += 4;
  int len = header >> 8;

  gba_swi_hle_lz77_count++;

  if(!gba_hle_source_range_valid(src, (header >> 8) & 0x1FFFFF))
    return;

  if(vram_dest)
  {
    int byte_count = 0;
    int byte_shift = 0;
    u32 write_value = 0;

    while(len > 0)
    {
      u8 flags = gba_hle_read8_fast(src++);
      for(int i = 0; i < 8 && len > 0; i++, flags <<= 1)
      {
        if(flags & 0x80)
        {
          u16 data = ((u16)gba_hle_read8_fast(src++)) << 8;
          data |= gba_hle_read8_fast(src++);
          int length = (data >> 12) + 3;
          int offset = data & 0x0FFF;
          u32 window = dst + byte_count - offset - 1;

          while(length-- > 0 && len > 0)
          {
            // VRAM only takes 16-bit writes, so one output byte can still be
            // sitting in `write_value` rather than in memory. A match reaching
            // back that far would otherwise read stale memory through `dst`.
            u8 match = (window >= dst) ?
                (u8)(write_value >> ((window - dst) * 8)) :
                gba_hle_read8_fast(window);
            window++;
            write_value |= ((u32)match) << byte_shift;
            byte_shift += 8;
            byte_count++;

            if(byte_count == 2)
            {
              gba_hle_write16_fast(dst, (u16)write_value, cpu_alert);
              dst += 2;
              byte_count = 0;
              byte_shift = 0;
              write_value = 0;
            }
            len--;
          }
        }
        else
        {
          write_value |= ((u32)gba_hle_read8_fast(src++)) << byte_shift;
          byte_shift += 8;
          byte_count++;
          if(byte_count == 2)
          {
            gba_hle_write16_fast(dst, (u16)write_value, cpu_alert);
            dst += 2;
            byte_count = 0;
            byte_shift = 0;
            write_value = 0;
          }
          len--;
        }
      }
    }
    return;
  }

  while(len > 0)
  {
    u8 flags = gba_hle_read8_fast(src++);
    for(int i = 0; i < 8 && len > 0; i++, flags <<= 1)
    {
      if(flags & 0x80)
      {
        u16 data = ((u16)gba_hle_read8_fast(src++)) << 8;
        data |= gba_hle_read8_fast(src++);
        int length = (data >> 12) + 3;
        int offset = data & 0x0FFF;
        u32 window = dst - offset - 1;

        while(length-- > 0 && len > 0)
        {
          gba_hle_write8_fast(dst++, gba_hle_read8_fast(window++), cpu_alert);
          len--;
        }
      }
      else
      {
        gba_hle_write8_fast(dst++, gba_hle_read8_fast(src++), cpu_alert);
        len--;
      }
    }
  }
}

static inline void gba_hle_huff_uncomp(cpu_alert_type &cpu_alert)
{
  u32 src = reg[0];
  u32 dst = reg[1];
  u32 header = gba_hle_read32_fast(src);
  src += 4;
  int len = header >> 8;

  gba_swi_hle_huff_count++;

  if(!gba_hle_source_range_valid(src, (header >> 8) & 0x1FFFFF))
    return;

  u8 tree_size = gba_hle_read8_fast(src++);
  u32 tree_start = src;
  src += ((tree_size + 1) << 1) - 1;

  u32 mask = 0x80000000;
  u32 data = gba_hle_read32_fast(src);
  src += 4;
  int pos = 0;
  u8 root_node = gba_hle_read8_fast(tree_start);
  u8 current_node = root_node;
  bool write_data = false;
  int byte_shift = 0;
  int byte_count = 0;
  u32 write_value = 0;

  if((header & 0x0F) == 8)
  {
    while(len > 0)
    {
      if(pos == 0)
        pos++;
      else
        pos += (((current_node & 0x3F) + 1) << 1);

      if(data & mask)
      {
        if(current_node & 0x40)
          write_data = true;
        current_node = gba_hle_read8_fast(tree_start + pos + 1);
      }
      else
      {
        if(current_node & 0x80)
          write_data = true;
        current_node = gba_hle_read8_fast(tree_start + pos);
      }

      if(write_data)
      {
        write_value |= ((u32)current_node) << byte_shift;
        byte_count++;
        byte_shift += 8;
        pos = 0;
        current_node = root_node;
        write_data = false;

        if(byte_count == 4)
        {
          gba_hle_write32_fast(dst, write_value, cpu_alert);
          dst += 4;
          write_value = 0;
          byte_count = 0;
          byte_shift = 0;
          len -= 4;
        }
      }

      mask >>= 1;
      if(mask == 0)
      {
        mask = 0x80000000;
        data = gba_hle_read32_fast(src);
        src += 4;
      }
    }
  }
  else
  {
    int half_len = 0;
    int value = 0;

    while(len > 0)
    {
      if(pos == 0)
        pos++;
      else
        pos += (((current_node & 0x3F) + 1) << 1);

      if(data & mask)
      {
        if(current_node & 0x40)
          write_data = true;
        current_node = gba_hle_read8_fast(tree_start + pos + 1);
      }
      else
      {
        if(current_node & 0x80)
          write_data = true;
        current_node = gba_hle_read8_fast(tree_start + pos);
      }

      if(write_data)
      {
        if(half_len == 0)
          value |= current_node;
        else
          value |= current_node << 4;

        half_len += 4;
        if(half_len == 8)
        {
          write_value |= ((u32)value) << byte_shift;
          byte_count++;
          byte_shift += 8;
          half_len = 0;
          value = 0;

          if(byte_count == 4)
          {
            gba_hle_write32_fast(dst, write_value, cpu_alert);
            dst += 4;
            write_value = 0;
            byte_count = 0;
            byte_shift = 0;
            len -= 4;
          }
        }

        pos = 0;
        current_node = root_node;
        write_data = false;
      }

      mask >>= 1;
      if(mask == 0)
      {
        mask = 0x80000000;
        data = gba_hle_read32_fast(src);
        src += 4;
      }
    }
  }
}

static inline void gba_hle_rl_uncomp(cpu_alert_type &cpu_alert, bool vram_dest)
{
  u32 src = reg[0];
  u32 dst = reg[1];
  u32 header = gba_hle_read32_fast(vram_dest ? (src & ~3U) : src);
  src += 4;
  int len = header >> 8;

  gba_swi_hle_rl_count++;

  if(!gba_hle_source_range_valid(src, (header >> 8) & 0x1FFFFF))
    return;

  if(vram_dest)
  {
    int byte_count = 0;
    int byte_shift = 0;
    u32 write_value = 0;

    while(len > 0)
    {
      u8 flags = gba_hle_read8_fast(src++);
      int length = flags & 0x7F;
      if(flags & 0x80)
      {
        u8 data = gba_hle_read8_fast(src++);
        length += 3;
        while(length-- > 0 && len > 0)
        {
          write_value |= ((u32)data) << byte_shift;
          byte_shift += 8;
          byte_count++;
          if(byte_count == 2)
          {
            gba_hle_write16_fast(dst, (u16)write_value, cpu_alert);
            dst += 2;
            byte_count = 0;
            byte_shift = 0;
            write_value = 0;
          }
          len--;
        }
      }
      else
      {
        length++;
        while(length-- > 0 && len > 0)
        {
          write_value |= ((u32)gba_hle_read8_fast(src++)) << byte_shift;
          byte_shift += 8;
          byte_count++;
          if(byte_count == 2)
          {
            gba_hle_write16_fast(dst, (u16)write_value, cpu_alert);
            dst += 2;
            byte_count = 0;
            byte_shift = 0;
            write_value = 0;
          }
          len--;
        }
      }
    }
    return;
  }

  while(len > 0)
  {
    u8 flags = gba_hle_read8_fast(src++);
    int length = flags & 0x7F;
    if(flags & 0x80)
    {
      u8 data = gba_hle_read8_fast(src++);
      length += 3;
      while(length-- > 0 && len > 0)
      {
        gba_hle_write8_fast(dst++, data, cpu_alert);
        len--;
      }
    }
    else
    {
      length++;
      while(length-- > 0 && len > 0)
      {
        gba_hle_write8_fast(dst++, gba_hle_read8_fast(src++), cpu_alert);
        len--;
      }
    }
  }
}

static inline void gba_hle_diff8_unfilter(cpu_alert_type &cpu_alert,
    bool vram_dest)
{
  u32 src = reg[0];
  u32 dst = reg[1];
  u32 header = gba_hle_read32_fast(src);
  src += 4;
  int len = header >> 8;

  gba_swi_hle_diff_count++;

  if(!gba_hle_source_range_valid(src, (header >> 8) & 0x1FFFFF) || len <= 0)
    return;

  u8 data = gba_hle_read8_fast(src++);

  if(vram_dest)
  {
    u16 write_data = data;
    int shift = 8;
    int bytes = 1;

    while(len >= 2)
    {
      u8 diff = gba_hle_read8_fast(src++);
      data += diff;
      write_data |= ((u16)data) << shift;
      bytes++;
      shift += 8;
      if(bytes == 2)
      {
        gba_hle_write16_fast(dst, write_data, cpu_alert);
        dst += 2;
        len -= 2;
        bytes = 0;
        write_data = 0;
        shift = 0;
      }
    }
    return;
  }

  gba_hle_write8_fast(dst++, data, cpu_alert);
  len--;

  while(len > 0)
  {
    u8 diff = gba_hle_read8_fast(src++);
    data += diff;
    gba_hle_write8_fast(dst++, data, cpu_alert);
    len--;
  }
}

static inline void gba_hle_diff16_unfilter(cpu_alert_type &cpu_alert)
{
  u32 src = reg[0];
  u32 dst = reg[1];
  u32 header = gba_hle_read32_fast(src);
  src += 4;
  int len = header >> 8;

  gba_swi_hle_diff_count++;

  if(!gba_hle_source_range_valid(src, (header >> 8) & 0x1FFFFF) || len < 2)
    return;

  u16 data = gba_hle_read16_fast(src);
  src += 2;
  gba_hle_write16_fast(dst, data, cpu_alert);
  dst += 2;
  len -= 2;

  while(len >= 2)
  {
    u16 diff = gba_hle_read16_fast(src);
    src += 2;
    data += diff;
    gba_hle_write16_fast(dst, data, cpu_alert);
    dst += 2;
    len -= 2;
  }
}

static inline void gba_hle_bgaffineset(cpu_alert_type &cpu_alert)
{
  u32 src = reg[0];
  u32 dst = reg[1];
  u32 count = reg[2];

  while(count--)
  {
    s32 cx = (s32)gba_hle_read32_fast(src); src += 4;
    s32 cy = (s32)gba_hle_read32_fast(src); src += 4;
    s16 dispx = (s16)gba_hle_read16_fast(src); src += 2;
    s16 dispy = (s16)gba_hle_read16_fast(src); src += 2;
    s16 rx = (s16)gba_hle_read16_fast(src); src += 2;
    s16 ry = (s16)gba_hle_read16_fast(src); src += 2;
    u32 theta = gba_hle_read16_fast(src) >> 8; src += 4;

    s32 a = gba_hle_sine(theta + 0x40);
    s32 b = gba_hle_sine(theta);
    s16 dx = (s16)(((s32)rx * a) >> 14);
    s16 dmx = (s16)(((s32)rx * b) >> 14);
    s16 dy = (s16)(((s32)ry * b) >> 14);
    s16 dmy = (s16)(((s32)ry * a) >> 14);

    gba_hle_write16_fast(dst, (u16)dx, cpu_alert); dst += 2;
    gba_hle_write16_fast(dst, (u16)-dmx, cpu_alert); dst += 2;
    gba_hle_write16_fast(dst, (u16)dy, cpu_alert); dst += 2;
    gba_hle_write16_fast(dst, (u16)dmy, cpu_alert); dst += 2;

    s32 startx = cx - dx * dispx + dmx * dispy;
    s32 starty = cy - dy * dispx - dmy * dispy;
    gba_hle_write32_fast(dst, (u32)startx, cpu_alert); dst += 4;
    gba_hle_write32_fast(dst, (u32)starty, cpu_alert); dst += 4;
  }
}

static inline void gba_hle_objaffineset(cpu_alert_type &cpu_alert)
{
  u32 src = reg[0];
  u32 dst = reg[1];
  u32 count = reg[2];
  u32 offset = reg[3];

  while(count--)
  {
    s16 rx = (s16)gba_hle_read16_fast(src); src += 2;
    s16 ry = (s16)gba_hle_read16_fast(src); src += 2;
    u32 theta = gba_hle_read16_fast(src) >> 8; src += 4;

    s32 a = gba_hle_sine(theta + 0x40);
    s32 b = gba_hle_sine(theta);
    s16 dx = (s16)(((s32)rx * a) >> 14);
    s16 dmx = (s16)(((s32)rx * b) >> 14);
    s16 dy = (s16)(((s32)ry * b) >> 14);
    s16 dmy = (s16)(((s32)ry * a) >> 14);

    gba_hle_write16_fast(dst, (u16)dx, cpu_alert); dst += offset;
    gba_hle_write16_fast(dst, (u16)-dmx, cpu_alert); dst += offset;
    gba_hle_write16_fast(dst, (u16)dy, cpu_alert); dst += offset;
    gba_hle_write16_fast(dst, (u16)dmy, cpu_alert); dst += offset;
  }
}

static inline void gba_hle_softreset(void)
{
  const u32 advance = (reg[REG_CPSR] & 0x20) ? 2 : 4;
  const u32 target = iwram[0xFFFA] ? 0x02000000 : 0x08000000;

  gba_swi_hle_softreset_count++;
  if(gba_thumb_jit_runtime_enabled || gba_thumb_batch_enabled)
    gba_p4_thumb_jit_report_fault(0x53575253U, reg[REG_PC]);

  memset(&iwram[0xFE00], 0, 0x200);

  memset(reg_mode, 0, sizeof(reg_mode));
  REG_MODE(MODE_USER)[5] = 0x03007F00;
  REG_MODE(MODE_IRQ)[5] = 0x03007FA0;
  REG_MODE(MODE_FIQ)[5] = 0x03007FA0;
  REG_MODE(MODE_SUPERVISOR)[5] = 0x03007FE0;
  REG_SPSR(MODE_IRQ) = 0;
  REG_SPSR(MODE_SUPERVISOR) = 0;

  for(u32 i = 0; i < 13; i++)
    reg[i] = 0;

  reg[REG_SP] = 0x03007F00;
  reg[REG_LR] = 0;
  reg[REG_CPSR] = 0x0000001F;
  reg[CPU_MODE] = MODE_SYSTEM;
  reg[CPU_HALT_STATE] = CPU_ACTIVE;
  reg[REG_PC] = target - advance;
}

static inline void gba_hle_register_ram_reset(u32 flags)
{
  gba_swi_hle_ramreset_count++;
  write_ioreg(REG_DISPCNT, 0x80);

  if(flags & 0x01)
    memset(ewram, 0, GBA_EWRAM_SIZE);

  if(flags & 0x02)
    memset(&iwram[0x8000], 0, 0x7E00);

  if(flags & 0x04)
  {
    memset(palette_ram, 0, 512 * sizeof(*palette_ram));
    memset(palette_ram_converted, 0, 512 * sizeof(*palette_ram_converted));
  }

  if(flags & 0x08)
    memset(vram, 0, GBA_VRAM_SIZE);

  if(flags & 0x10)
    memset(oam_ram, 0, 512 * sizeof(*oam_ram));

  if(flags & 0x80)
  {
    for(u32 i = 0; i < 0x10; i++)
      write_ioreg(REG_DISPCNT + i, 0);
    for(u32 i = 0; i < 0x0F; i++)
      write_ioreg(REG_BG0CNT + i, 0);
    for(u32 i = 0; i < 0x20; i++)
      write_ioreg(REG_BG2PA + i, 0);
    for(u32 i = 0; i < 0x18; i++)
      write_ioreg(REG_DMA0SAD + i, 0);

    write_ioreg(REG_P1, 0);
    write_ioreg(REG_BG2PA, 0x100);
    write_ioreg(REG_BG2PD, 0x100);
    write_ioreg(REG_BG3PA, 0x100);
    write_ioreg(REG_BG3PD, 0x100);
  }

  if(flags & 0x20)
  {
    for(u32 i = 0; i < 8; i++)
      write_ioreg(REG_TMXD(0) + i, 0);
    write_ioreg(REG_RCNT, 0x8000);
    for(u32 i = 0; i < 7; i++)
      write_ioreg(REG_SIODATA32_L + i, 0);
  }

  if(flags & 0x40)
  {
    write_ioreg(REG_SOUNDCNT_L, 0);
    write_ioreg(REG_SOUNDCNT_H, 0);
    write_ioreg(REG_SOUNDCNT_X, 0);
    for(u32 i = REG_SOUND1CNT_L; i <= REG_SOUNDWAVE_7; i++)
      write_ioreg(i, 0);
  }
}

static inline bool gba_execute_swi_hle(u32 swinum, cpu_alert_type &cpu_alert)
{
#if !GBA_SWI_HLE
  (void)swinum;
  (void)cpu_alert;
  return false;
#else
  switch(swinum)
  {
    case 0x00:  // SoftReset
      gba_hle_softreset();
      return true;

    case 0x01:  // RegisterRamReset
      gba_hle_register_ram_reset(reg[0] & 0xFF);
      return true;

    case 0x02:  // Halt
      reg[CPU_HALT_STATE] = CPU_HALT;
      cpu_alert |= CPU_ALERT_HALT;
      return true;

    case 0x03:  // Stop
      reg[CPU_HALT_STATE] = CPU_STOP;
      cpu_alert |= CPU_ALERT_HALT;
      return true;

    case 0x04:  // IntrWait: r0=discard, r1=wait flags
    {
      u32 wait_flags = reg[1] & 0x3FFF;
      if(!wait_flags)
        return true;

      u32 iflags = read_ioreg(REG_IF);
      if(reg[0])
      {
        // BIOS semantics: discard already-pending requested flags first.
        iflags &= ~wait_flags;
        write_ioreg(REG_IF, iflags);
      }

      u32 matched = iflags & wait_flags;
      if(matched)
      {
        write_ioreg(REG_IF, iflags & ~matched);
        return true;
      }

      write_ioreg(REG_IME, 1);
      reg[CPU_HALT_STATE] = CPU_HALT;
      cpu_alert |= CPU_ALERT_HALT;
      return true;
    }

    case 0x05:  // VBlankIntrWait
    {
      u32 iflags = read_ioreg(REG_IF) & ~IRQ_VBLANK;
      write_ioreg(REG_IF, iflags);
      write_ioreg(REG_IME, 1);
      reg[CPU_HALT_STATE] = CPU_HALT;
      cpu_alert |= CPU_ALERT_HALT;
      return true;
    }

    case 0x06:  // Div: r0 / r1
      return gba_hle_div((s32)reg[0], (s32)reg[1]);

    case 0x07:  // DivArm: r1 / r0
      return gba_hle_div((s32)reg[1], (s32)reg[0]);

    case 0x08:  // Sqrt
      reg[0] = gba_hle_sqrt_u32(reg[0]);
      return true;

#if GBA_SWI_HLE >= 2
    case 0x0B:  // CpuSet
      gba_hle_cpuset(cpu_alert, false);
      return true;

    case 0x0C:  // CpuFastSet
      gba_hle_cpuset(cpu_alert, true);
      return true;

    case 0x0E:  // BgAffineSet
      gba_hle_bgaffineset(cpu_alert);
      return true;

    case 0x0F:  // ObjAffineSet
      gba_hle_objaffineset(cpu_alert);
      return true;

    case 0x10:  // BitUnPack
      gba_hle_bitunpack(cpu_alert);
      return true;

    case 0x11:  // LZ77UnCompWram
      gba_hle_lz77_uncomp(cpu_alert, false);
      return true;

    case 0x12:  // LZ77UnCompVram
      gba_hle_lz77_uncomp(cpu_alert, true);
      return true;

    case 0x13:  // HuffUnComp
      gba_hle_huff_uncomp(cpu_alert);
      return true;

    case 0x14:  // RLUnCompWram
      gba_hle_rl_uncomp(cpu_alert, false);
      return true;

    case 0x15:  // RLUnCompVram
      gba_hle_rl_uncomp(cpu_alert, true);
      return true;

    case 0x16:  // Diff8bitUnFilterWram
      gba_hle_diff8_unfilter(cpu_alert, false);
      return true;

    case 0x17:  // Diff8bitUnFilterVram
      gba_hle_diff8_unfilter(cpu_alert, true);
      return true;

    case 0x18:  // Diff16bitUnFilter
      gba_hle_diff16_unfilter(cpu_alert);
      return true;
#endif

    default:
      return false;
  }
#endif
}

static inline __attribute__((always_inline)) int gba_thumb_execute_hot_exact_fast(
    u32 top, u32 opcode, u32 &n_flag, u32 &z_flag, u32 &c_flag,
    u32 &v_flag, s32 &cycles_remaining)
{
  switch(top)
  {
    case 0x00: /* LSL rd, rs, imm */
    {
      u32 rd = opcode & 0x07;
      u32 rs = (opcode >> 3) & 0x07;
      u32 offset = (opcode >> 6) & 0x1F;
      u32 src = reg[rs];
      u32 dest = src << offset;

      if(offset)
        c_flag = (src >> (32 - offset)) & 0x01;

      n_flag = dest >> 31;
      z_flag = dest == 0;
      reg[rd] = dest;
      reg[REG_PC] += 2;
      gba_thumb_low_fast_ops++;
      return 1;
    }

    case 0x18: /* ADD rd, rs, rn */
    case 0x19:
    {
      u32 rd = opcode & 0x07;
      u32 rs = (opcode >> 3) & 0x07;
      u32 rn = (opcode >> 6) & 0x07;
      u32 lhs = reg[rs];
      u32 rhs = reg[rn];
      u32 dest = lhs + rhs;

      n_flag = dest >> 31;
      z_flag = dest == 0;
      c_flag = dest < rhs;
      v_flag = (~(lhs ^ rhs) & (lhs ^ dest)) >> 31;
      reg[rd] = dest;
      reg[REG_PC] += 2;
      gba_thumb_low_fast_ops++;
      return 1;
    }

    case 0x1C: /* ADD rd, rs, imm3 */
    {
      u32 rd = opcode & 0x07;
      u32 rs = (opcode >> 3) & 0x07;
      u32 lhs = reg[rs];
      u32 rhs = (opcode >> 6) & 0x07;
      u32 dest = lhs + rhs;

      n_flag = dest >> 31;
      z_flag = dest == 0;
      c_flag = dest < rhs;
      v_flag = (~(lhs ^ rhs) & (lhs ^ dest)) >> 31;
      reg[rd] = dest;
      reg[REG_PC] += 2;
      gba_thumb_low_fast_ops++;
      return 1;
    }

    case 0x28: /* CMP r0, imm8 */
    {
      u32 src = reg[0];
      u32 imm = opcode & 0xFF;
      u32 dest = src - imm;

      n_flag = dest >> 31;
      z_flag = dest == 0;
      c_flag = src >= imm;
      v_flag = ((src ^ imm) & (src ^ dest)) >> 31;
      reg[REG_PC] += 2;
      gba_thumb_imm_fast_ops++;
      return 1;
    }

    case 0x40:
    {
      u32 rs = (opcode >> 3) & 0x07;
      u32 rd = opcode & 0x07;
      u32 lhs = reg[rd];
      u32 rhs = reg[rs];
      u32 dest;

      switch((opcode >> 6) & 0x03)
      {
        case 0x00:
          dest = lhs & rhs;
          break;
        case 0x01:
          dest = lhs ^ rhs;
          break;
        case 0x02:
          dest = lhs;
          if(rhs != 0)
          {
            if(rhs > 31)
            {
              c_flag = (rhs == 32) ? (dest & 0x01) : 0;
              dest = 0;
            }
            else
            {
              c_flag = (dest >> (32 - rhs)) & 0x01;
              dest <<= rhs;
            }
          }
          break;
        default:
          dest = lhs;
          if(rhs != 0)
          {
            if(rhs > 31)
            {
              c_flag = (rhs == 32) ? (dest >> 31) : 0;
              dest = 0;
            }
            else
            {
              c_flag = (dest >> (rhs - 1)) & 0x01;
              dest >>= rhs;
            }
          }
          break;
      }

      n_flag = dest >> 31;
      z_flag = dest == 0;
      reg[rd] = dest;
      reg[REG_PC] += 2;
      gba_thumb_alu_fast_ops++;
      return 1;
    }

    case 0x46: /* MOV high register, hot non-PC case */
    {
      u32 rs = (opcode >> 3) & 0x0F;
      u32 rd = ((opcode >> 4) & 0x08) | (opcode & 0x07);
      if(rd == REG_PC || rs == REG_PC)
        return 0;

      reg[rd] = reg[rs];
      reg[REG_PC] += 2;
      gba_thumb_hireg_fast_ops++;
      return 1;
    }

    case 0x78: /* LDRB rd, [rb, imm5] */
    {
      u32 imm = (opcode >> 6) & 0x1F;
      u32 rb = (opcode >> 3) & 0x07;
      u32 rd = opcode & 0x07;
      u32 address = reg[rb] + imm;
      u32 value;
      u8 *map;

      reg[REG_PC] += 2;

      if(address < 0x10000000)
      {
        u8 region = address >> 24;
        cycles_remaining -= ws_cyc_nseq[region][0];
        STATS_MEMORY_ACCESS(read, u8, region);
      }

      if(gba_thumb_load_direct_u8_fast(address, value))
      {
      }
      else if((((address >> 24) == 0) && (reg[REG_PC] >= 0x4000)) ||
              (address & 0xF0000000) ||
              !(map = memory_map_read[address >> 15]))
        value = read_memory8(address);
      else
        value = readaddress8(map, address & 0x7FFF);

      reg[rd] = value;
      gba_thumb_ldrb_fast_ops++;
      return 1;
    }

    case 0xD0: /* BEQ */
    case 0xD1: /* BNE */
    {
      bool taken = (top == 0xD0) ? (z_flag == 1) : (z_flag == 0);
      s32 offset = (s8)(opcode & 0xFF);
      reg[REG_PC] += taken ? ((offset * 2) + 4) : 2;
      cycles_remaining -= ws_cyc_nseq[reg[REG_PC] >> 24][0];
      gba_thumb_branch_fast_ops++;
      return 1;
    }

    default:
      return 0;
  }
}

static inline __attribute__((always_inline)) int gba_thumb_execute_fast_dispatch(u32 opcode, u32 &n_flag,
    u32 &z_flag, u32 &c_flag, u32 &v_flag, cpu_alert_type &cpu_alert,
    s32 &cycles_remaining)
{
  u32 top = (opcode >> 8) & 0xFF;
  int hot_result = gba_thumb_execute_hot_exact_fast(top, opcode, n_flag, z_flag,
      c_flag, v_flag, cycles_remaining);
  if(hot_result)
    return hot_result;

  switch(top)
  {
    case 0x00 ... 0x07:
      return gba_thumb_execute_lsl_imm_fast(opcode, n_flag, z_flag, c_flag);

    case 0x08 ... 0x17:
      return gba_thumb_execute_low_fast(opcode, n_flag, z_flag, c_flag, v_flag);

    case 0x18 ... 0x1B:
      return gba_thumb_execute_addsub_reg_fast(opcode, n_flag, z_flag, c_flag, v_flag);

    case 0x1C ... 0x1F:
      return gba_thumb_execute_addsub_imm3_fast(opcode, n_flag, z_flag, c_flag, v_flag);

    case 0x20 ... 0x3F:
      return gba_thumb_execute_imm_fast(opcode, n_flag, z_flag, c_flag, v_flag);

    case 0x40 ... 0x43:
      return gba_thumb_execute_alu_fast(opcode, n_flag, z_flag, c_flag, v_flag);

    case 0x44 ... 0x46:
      return gba_thumb_execute_hireg_fast(opcode, n_flag, z_flag, c_flag, v_flag);

    case 0x47:
    {
      u32 rs = (opcode >> 3) & 0x0F;
      reg[REG_PC] += 4;
      u32 src = reg[rs];
      gba_thumb_hireg_fast_ops++;
      if(src & 0x01)
      {
        reg[REG_PC] = src - 1;
        return 1;
      }

      reg[REG_PC] = src;
      reg[REG_CPSR] &= ~0x20;
      return 2;
    }

    case 0x48 ... 0x4F:
      return gba_thumb_execute_pcldr_fast(opcode, cycles_remaining);

    case 0x50 ... 0x55:
    case 0x58 ... 0x5D:
      return gba_thumb_execute_mem_reg_fast(opcode, cpu_alert, cycles_remaining);

    case 0x56 ... 0x57:
      return gba_thumb_execute_ldsb_reg_fast(opcode, cycles_remaining);

    case 0x5E ... 0x5F:
      return gba_thumb_execute_ldsh_reg_fast(opcode, cycles_remaining);

    case 0x60 ... 0x67:
    case 0x70 ... 0x77:
    case 0x80 ... 0x87:
      return gba_thumb_execute_store_imm_fast(opcode, cpu_alert, cycles_remaining);

    case 0x68 ... 0x6F:
      return gba_thumb_execute_ldr_imm_fast(opcode, cycles_remaining);

    case 0x78 ... 0x7F:
      return gba_thumb_execute_ldrb_imm_fast(opcode, cycles_remaining);

    case 0x88 ... 0x8F:
      return gba_thumb_execute_ldrh_imm_fast(opcode, cycles_remaining);

    case 0x90 ... 0x9F:
      return gba_thumb_execute_spmem_fast(opcode, cpu_alert, cycles_remaining);

    case 0xA0 ... 0xAF:
      return gba_thumb_execute_add_pcsp_fast(opcode);

    case 0xB0 ... 0xB3:
      return gba_thumb_execute_add_sp_fast(opcode);

    case 0xB4 ... 0xB5:
    case 0xBC ... 0xBD:
      return gba_thumb_execute_pushpop_fast(opcode, cpu_alert, cycles_remaining);

    case 0xC0 ... 0xCF:
      return gba_thumb_execute_blockmem_fast(opcode, cpu_alert, cycles_remaining);

    case 0xD0 ... 0xDD:
      return gba_thumb_execute_branch_fast(opcode, n_flag, z_flag, c_flag, v_flag, cycles_remaining);

    case 0xDF:
    {
      u32 swinum = opcode & 0xFF;
      gba_profile_thumb_swi(swinum);
      if(gba_execute_swi_hle(swinum, cpu_alert))
      {
        reg[REG_PC] += 2;
        cycles_remaining -= 64;
        gba_thumb_branch_fast_ops++;
        return 1;
      }
      return 0;
    }

    case 0xE0 ... 0xE7:
      return gba_thumb_execute_uncond_branch_fast(opcode, cycles_remaining);

    case 0xF0 ... 0xFF:
    {
      u32 offset = opcode & 0x07FF;
      if(top <= 0xF7)
      {
        reg[REG_LR] = reg[REG_PC] + 4 + ((s32)(offset << 21) >> 9);
        reg[REG_PC] += 2;
      }
      else
      {
        u32 newpc = reg[REG_LR] + (offset * 2);
        reg[REG_LR] = reg[REG_PC] + 3;
        reg[REG_PC] = newpc;
        cycles_remaining -= ws_cyc_nseq[newpc >> 24][0];
      }
      gba_thumb_branch_fast_ops++;
      return 1;
    }

    default:
      return 0;
  }
}


#define arm_decode_data_proc_reg(opcode)                                      \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F;                                                     \
  (void)rd;                                                                   \
  (void)rn;                                                                   \
  using_register(arm, rd, op_dest);                                           \
  using_register(arm, rn, op_src);                                            \
  using_register(arm, rm, op_src)                                             \

#define arm_decode_data_proc_imm(opcode)                                      \
  u32 imm;                                                                    \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 imm_ror = ((opcode >> 8) & 0xF) << 1;                                   \
  (void)rd;                                                                   \
  (void)rn;                                                                   \
  ror(imm, opcode & 0xFF, imm_ror);                                           \
  using_register(arm, rd, op_dest);                                           \
  using_register(arm, rn, op_src)                                             \

#define arm_decode_psr_reg(opcode)                                            \
  u32 psr_pfield = ((opcode >> 16) & 1) | ((opcode >> 18) & 2);               \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F;                                                     \
  (void)rd;                                                                   \
  (void)rm;                                                                   \
  (void)psr_pfield;                                                           \
  using_register(arm, rd, op_dest);                                           \
  using_register(arm, rm, op_src)                                             \

#define arm_decode_psr_imm(opcode)                                            \
  u32 imm;                                                                    \
  u32 psr_pfield = ((opcode >> 16) & 1) | ((opcode >> 18) & 2);               \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  (void)rd;                                                                   \
  ror(imm, opcode & 0xFF, ((opcode >> 8) & 0x0F) * 2);                        \
  using_register(arm, rd, op_dest)                                            \

#define arm_decode_branchx(opcode)                                            \
  u32 rn = opcode & 0x0F;                                                     \
  using_register(arm, rn, branch_target)                                      \

#define arm_decode_multiply()                                                 \
  u32 rd = (opcode >> 16) & 0x0F;                                             \
  u32 rn = (opcode >> 12) & 0x0F;                                             \
  u32 rs = (opcode >> 8) & 0x0F;                                              \
  u32 rm = opcode & 0x0F;                                                     \
  (void)rn;                                                                   \
  using_register(arm, rd, op_dest);                                           \
  using_register(arm, rn, op_src);                                            \
  using_register(arm, rm, op_src)                                             \

#define arm_decode_multiply_long()                                            \
  u32 rdhi = (opcode >> 16) & 0x0F;                                           \
  u32 rdlo = (opcode >> 12) & 0x0F;                                           \
  u32 rn = (opcode >> 8) & 0x0F;                                              \
  u32 rm = opcode & 0x0F;                                                     \
  using_register(arm, rdhi, op_dest);                                         \
  using_register(arm, rdlo, op_dest);                                         \
  using_register(arm, rn, op_src);                                            \
  using_register(arm, rm, op_src)                                             \

#define arm_decode_swap()                                                     \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F;                                                     \
  using_register(arm, rd, memory_target);                                     \
  using_register(arm, rn, memory_base);                                       \
  using_register(arm, rm, memory_target)                                      \

#define arm_decode_half_trans_r()                                             \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F;                                                     \
  using_register(arm, rd, memory_target);                                     \
  using_register(arm, rn, memory_base);                                       \
  using_register(arm, rm, memory_offset)                                      \

#define arm_decode_half_trans_of()                                            \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 offset = ((opcode >> 4) & 0xF0) | (opcode & 0x0F);                      \
  using_register(arm, rd, memory_target);                                     \
  using_register(arm, rn, memory_base)                                        \

#define arm_decode_data_trans_imm()                                           \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 offset = opcode & 0x0FFF;                                               \
  using_register(arm, rd, memory_target);                                     \
  using_register(arm, rn, memory_base)                                        \

#define arm_decode_data_trans_reg()                                           \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F;                                                     \
  using_register(arm, rd, memory_target);                                     \
  using_register(arm, rn, memory_base);                                       \
  using_register(arm, rm, memory_offset)                                      \

#define arm_decode_block_trans()                                              \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 reg_list = opcode & 0xFFFF;                                             \
  using_register(arm, rn, memory_base);                                       \
  using_register_list(arm, reg_list, 16)                                      \

#define arm_decode_branch()                                                   \
  s32 offset = ((s32)((u32)(opcode << 8))) >> 6                               \


#define thumb_decode_shift()                                                  \
  u32 imm = (opcode >> 6) & 0x1F;                                             \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07;                                                     \
  using_register(thumb, rd, op_dest);                                         \
  using_register(thumb, rs, op_shift)                                         \

#define thumb_decode_add_sub()                                                \
  u32 rn = (opcode >> 6) & 0x07;                                              \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07;                                                     \
  using_register(thumb, rd, op_dest);                                         \
  using_register(thumb, rn, op_src);                                          \
  using_register(thumb, rs, op_src)                                           \

#define thumb_decode_add_sub_imm()                                            \
  u32 imm = (opcode >> 6) & 0x07;                                             \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07;                                                     \
  using_register(thumb, rd, op_src_dest);                                     \
  using_register(thumb, rs, op_src)                                           \

#define thumb_decode_imm()                                                    \
  u32 imm = opcode & 0xFF;                                                    \
  using_register(thumb, ((opcode >> 8) & 0x07), op_dest)                      \

#define thumb_decode_alu_op()                                                 \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07;                                                     \
  using_register(thumb, rd, op_src_dest);                                     \
  using_register(thumb, rs, op_src)                                           \

#define thumb_decode_hireg_op()                                               \
  u32 rs = (opcode >> 3) & 0x0F;                                              \
  u32 rd = ((opcode >> 4) & 0x08) | (opcode & 0x07);                          \
  (void)rd;                                                                   \
  using_register(thumb, rd, op_src_dest);                                     \
  using_register(thumb, rs, op_src)                                           \


#define thumb_decode_mem_reg()                                                \
  u32 ro = (opcode >> 6) & 0x07;                                              \
  u32 rb = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07;                                                     \
  using_register(thumb, rd, memory_target);                                   \
  using_register(thumb, rb, memory_base);                                     \
  using_register(thumb, ro, memory_offset)                                    \


#define thumb_decode_mem_imm()                                                \
  u32 imm = (opcode >> 6) & 0x1F;                                             \
  u32 rb = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07;                                                     \
  using_register(thumb, rd, memory_target);                                   \
  using_register(thumb, rb, memory_base)                                      \


#define thumb_decode_add_sp()                                                 \
  u32 imm = opcode & 0x7F;                                                    \
  using_register(thumb, REG_SP, op_dest)                                      \

#define thumb_decode_rlist()                                                  \
  u32 reg_list = opcode & 0xFF;                                               \
  using_register_list(thumb, rlist, 8)                                        \

#define thumb_decode_branch_cond()                                            \
  s32 offset = (s8)(opcode & 0xFF)                                            \

#define thumb_decode_branch()                                                 \
  u32 offset = opcode & 0x07FF                                                \


#define get_shift_register(dest)                                              \
  u32 shift = reg[(opcode >> 8) & 0x0F] & 0xFF;                               \
  using_register(arm, ((opcode >> 8) & 0x0F), op_shift);                      \
  dest = reg[rm];                                                             \
  if(rm == 15)                                                                \
    dest += 4                                                                 \


#define calculate_z_flag(dest)                                                \
  z_flag = (dest == 0)                                                        \

#define calculate_n_flag(dest)                                                \
  n_flag = ((signed)dest < 0)                                                 \

#define calculate_c_flag_sub(dest, src_a, src_b, carry)                       \
  c_flag = (carry) ? ((unsigned)src_b <= (unsigned)src_a) :                   \
                     ((unsigned)src_b < (unsigned)src_a);                     \

#define calculate_v_flag_sub(dest, src_a, src_b)                              \
  v_flag = (((src_a ^ src_b) & (~src_b ^ dest)) >> 31)

#define calculate_v_flag_add(dest, src_a, src_b)                              \
  v_flag = ((~((src_a) ^ (src_b)) & ((src_a) ^ (dest))) >> 31)

#define calculate_reg_sh()                                                    \
  u32 reg_sh = 0;                                                             \
  switch((opcode >> 4) & 0x07)                                                \
  {                                                                           \
    /* LSL imm */                                                             \
    case 0x0:                                                                 \
    {                                                                         \
      reg_sh = reg[rm] << ((opcode >> 7) & 0x1F);                             \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* LSL reg */                                                             \
    case 0x1:                                                                 \
    {                                                                         \
      get_shift_register(reg_sh);                                             \
      if(shift <= 31)                                                         \
        reg_sh = reg_sh << shift;                                             \
      else                                                                    \
        reg_sh = 0;                                                           \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* LSR imm */                                                             \
    case 0x2:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
      if(imm == 0)                                                            \
        reg_sh = 0;                                                           \
      else                                                                    \
        reg_sh = reg[rm] >> imm;                                              \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* LSR reg */                                                             \
    case 0x3:                                                                 \
    {                                                                         \
      get_shift_register(reg_sh);                                             \
      if(shift <= 31)                                                         \
        reg_sh = reg_sh >> shift;                                             \
      else                                                                    \
        reg_sh = 0;                                                           \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ASR imm */                                                             \
    case 0x4:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
      reg_sh = reg[rm];                                                       \
                                                                              \
      if(imm == 0)                                                            \
        reg_sh = (s32)reg_sh >> 31;                                           \
      else                                                                    \
        reg_sh = (s32)reg_sh >> imm;                                          \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ASR reg */                                                             \
    case 0x5:                                                                 \
    {                                                                         \
      get_shift_register(reg_sh);                                             \
      if(shift <= 31)                                                         \
        reg_sh = (s32)reg_sh >> shift;                                        \
      else                                                                    \
        reg_sh = (s32)reg_sh >> 31;                                           \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ROR imm */                                                             \
    case 0x6:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
                                                                              \
      if(imm == 0)                                                            \
        reg_sh = (reg[rm] >> 1) | (c_flag << 31);                             \
      else                                                                    \
        ror(reg_sh, reg[rm], imm);                                            \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ROR reg */                                                             \
    case 0x7:                                                                 \
    {                                                                         \
      get_shift_register(reg_sh);                                             \
      ror(reg_sh, reg_sh, shift);                                             \
      break;                                                                  \
    }                                                                         \
  }                                                                           \

#define calculate_reg_sh_flags()                                              \
  u32 reg_sh = 0;                                                             \
  switch((opcode >> 4) & 0x07)                                                \
  {                                                                           \
    /* LSL imm */                                                             \
    case 0x0:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
      reg_sh = reg[rm];                                                       \
                                                                              \
      if(imm != 0)                                                            \
      {                                                                       \
        c_flag = (reg_sh >> (32 - imm)) & 0x01;                               \
        reg_sh <<= imm;                                                       \
      }                                                                       \
                                                                              \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* LSL reg */                                                             \
    case 0x1:                                                                 \
    {                                                                         \
      get_shift_register(reg_sh);                                             \
      if(shift != 0)                                                          \
      {                                                                       \
        if(shift > 31)                                                        \
        {                                                                     \
          if(shift == 32)                                                     \
            c_flag = reg_sh & 0x01;                                           \
          else                                                                \
            c_flag = 0;                                                       \
          reg_sh = 0;                                                         \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          c_flag = (reg_sh >> (32 - shift)) & 0x01;                           \
          reg_sh <<= shift;                                                   \
        }                                                                     \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* LSR imm */                                                             \
    case 0x2:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
      reg_sh = reg[rm];                                                       \
      if(imm == 0)                                                            \
      {                                                                       \
        c_flag = reg_sh >> 31;                                                \
        reg_sh = 0;                                                           \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        c_flag = (reg_sh >> (imm - 1)) & 0x01;                                \
        reg_sh >>= imm;                                                       \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* LSR reg */                                                             \
    case 0x3:                                                                 \
    {                                                                         \
      get_shift_register(reg_sh);                                             \
      if(shift != 0)                                                          \
      {                                                                       \
        if(shift > 31)                                                        \
        {                                                                     \
          if(shift == 32)                                                     \
            c_flag = (reg_sh >> 31) & 0x01;                                   \
          else                                                                \
            c_flag = 0;                                                       \
          reg_sh = 0;                                                         \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          c_flag = (reg_sh >> (shift - 1)) & 0x01;                            \
          reg_sh >>= shift;                                                   \
        }                                                                     \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ASR imm */                                                             \
    case 0x4:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
      reg_sh = reg[rm];                                                       \
      if(imm == 0)                                                            \
      {                                                                       \
        reg_sh = (s32)reg_sh >> 31;                                           \
        c_flag = reg_sh & 0x01;                                               \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        c_flag = (reg_sh >> (imm - 1)) & 0x01;                                \
        reg_sh = (s32)reg_sh >> imm;                                          \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ASR reg */                                                             \
    case 0x5:                                                                 \
    {                                                                         \
      get_shift_register(reg_sh);                                             \
      if(shift != 0)                                                          \
      {                                                                       \
        if(shift > 31)                                                        \
        {                                                                     \
          reg_sh = (s32)reg_sh >> 31;                                         \
          c_flag = reg_sh & 0x01;                                             \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          c_flag = (reg_sh >> (shift - 1)) & 0x01;                            \
          reg_sh = (s32)reg_sh >> shift;                                      \
        }                                                                     \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ROR imm */                                                             \
    case 0x6:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
      reg_sh = reg[rm];                                                       \
      if(imm == 0)                                                            \
      {                                                                       \
        u32 old_c_flag = c_flag;                                              \
        c_flag = reg_sh & 0x01;                                               \
        reg_sh = (reg_sh >> 1) | (old_c_flag << 31);                          \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        c_flag = (reg_sh >> (imm - 1)) & 0x01;                                \
        ror(reg_sh, reg_sh, imm);                                             \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ROR reg */                                                             \
    case 0x7:                                                                 \
    {                                                                         \
      get_shift_register(reg_sh);                                             \
      if(shift != 0)                                                          \
      {                                                                       \
        c_flag = (reg_sh >> (shift - 1)) & 0x01;                              \
        ror(reg_sh, reg_sh, shift);                                           \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
  }                                                                           \

#define calculate_reg_offset()                                                \
  u32 reg_offset = 0;                                                         \
  switch((opcode >> 5) & 0x03)                                                \
  {                                                                           \
    /* LSL imm */                                                             \
    case 0x0:                                                                 \
    {                                                                         \
      reg_offset = reg[rm] << ((opcode >> 7) & 0x1F);                         \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* LSR imm */                                                             \
    case 0x1:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
      if(imm == 0)                                                            \
        reg_offset = 0;                                                       \
      else                                                                    \
        reg_offset = reg[rm] >> imm;                                          \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ASR imm */                                                             \
    case 0x2:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
      if(imm == 0)                                                            \
        reg_offset = (s32)reg[rm] >> 31;                                      \
      else                                                                    \
        reg_offset = (s32)reg[rm] >> imm;                                     \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ROR imm */                                                             \
    case 0x3:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
      if(imm == 0)                                                            \
        reg_offset = (reg[rm] >> 1) | (c_flag << 31);                         \
      else                                                                    \
        ror(reg_offset, reg[rm], imm);                                        \
      break;                                                                  \
    }                                                                         \
  }                                                                           \

#define calculate_flags_add(dest, src_a, src_b)                               \
  calculate_z_flag(dest);                                                     \
  calculate_n_flag(dest);                                                     \
  calculate_v_flag_add(dest, src_a, src_b)                                    \

#define calculate_flags_sub(dest, src_a, src_b, carry)                        \
  calculate_z_flag(dest);                                                     \
  calculate_n_flag(dest);                                                     \
  calculate_c_flag_sub(dest, src_a, src_b, carry);                            \
  calculate_v_flag_sub(dest, src_a, src_b)                                    \

#define calculate_flags_logic(dest)                                           \
  calculate_z_flag(dest);                                                     \
  calculate_n_flag(dest)                                                      \

#define extract_flags()                                                       \
  n_flag = reg[REG_CPSR] >> 31;                                               \
  z_flag = (reg[REG_CPSR] >> 30) & 0x01;                                      \
  c_flag = (reg[REG_CPSR] >> 29) & 0x01;                                      \
  v_flag = (reg[REG_CPSR] >> 28) & 0x01;                                      \

#define collapse_flags()                                                      \
  reg[REG_CPSR] = (n_flag << 31) | (z_flag << 30) | (c_flag << 29) |          \
   (v_flag << 28) | (reg[REG_CPSR] & 0xFF)                                    \

#define check_pc_region()                                                     \
  new_pc_region = (reg[REG_PC] >> 15);                                        \
  if(new_pc_region != pc_region)                                              \
  {                                                                           \
    pc_region = new_pc_region;                                                \
    if(new_pc_region >= (GBA_MEMORY_MAP_READ_SIZE / sizeof(u8 *)))            \
    {                                                                         \
      pc_address_block = NULL;                                                \
    }                                                                         \
    else                                                                      \
    {                                                                         \
      pc_address_block = memory_map_read[new_pc_region];                      \
      touch_gamepak_page(pc_region);                                          \
                                                                                \
      if(!pc_address_block)                                                   \
        pc_address_block = load_gamepak_page(pc_region & 0x3FF);              \
    }                                                                         \
  }                                                                           \


#define arm_pc_offset(val)                                                    \
  reg[REG_PC] += val                                                          \

#define arm_next_instruction()                                                \
{                                                                             \
  arm_pc_offset(4);                                                           \
  goto skip_instruction;                                                      \
}                                                                             \

#define thumb_pc_offset(val)                                                  \
  reg[REG_PC] += val                                                          \


// It should be okay to still generate result flags, spsr will overwrite them.
// This is pretty infrequent (returning from interrupt handlers, et al) so
// probably not worth optimizing for.

#define check_for_interrupts()                                                \
  if((read_ioreg(REG_IE) & read_ioreg(REG_IF)) &&                             \
   read_ioreg(REG_IME) && ((reg[REG_CPSR] & 0x80) == 0))                      \
  {                                                                           \
    REG_MODE(MODE_IRQ)[6] = reg[REG_PC] + 4;                                  \
    REG_SPSR(MODE_IRQ) = reg[REG_CPSR];                                       \
    reg[REG_CPSR] = 0xD2;                                                     \
    reg[REG_PC] = 0x00000018;                                                 \
    set_cpu_mode(MODE_IRQ);                                                   \
    goto arm_loop;                                                            \
  }                                                                           \

#define arm_spsr_restore()                                                    \
  {                                                                           \
    if(reg[CPU_MODE] != MODE_USER && reg[CPU_MODE] != MODE_SYSTEM)            \
    {                                                                         \
      reg[REG_CPSR] = REG_SPSR(reg[CPU_MODE]);                                \
      extract_flags();                                                        \
      set_cpu_mode(cpu_modes[reg[REG_CPSR] & 0xF]);                           \
      check_for_interrupts();                                                 \
    }                                                                         \
                                                                              \
    if(reg[REG_CPSR] & 0x20)                                                  \
      goto thumb_loop;                                                        \
  }                                                                           \

#define arm_spsr_restore_check()                                              \
  if(rd == REG_PC)                                                            \
  {                                                                           \
    arm_spsr_restore()                                                        \
  }                                                                           \

#define arm_spsr_restore_ldm_check()                                          \
  if (opcode & 0x8000)   /* PC is in the LDM reg list */                      \
  {                                                                           \
    arm_spsr_restore()                                                        \
  }                                                                           \

#define arm_data_proc_flags_reg()                                             \
  arm_decode_data_proc_reg(opcode);                                           \
  calculate_reg_sh_flags()                                                    \

#define arm_data_proc_reg()                                                   \
  arm_decode_data_proc_reg(opcode);                                           \
  calculate_reg_sh()                                                          \

#define arm_data_proc_flags_imm()                                             \
  arm_decode_data_proc_imm(opcode)                                            \
  if(imm_ror)                                                                 \
    c_flag = (imm >> 31);  /* imm is rotated already! */                      \

#define arm_data_proc_imm()                                                   \
  arm_decode_data_proc_imm(opcode)                                            \

#define arm_data_proc(expr, type)                                             \
{                                                                             \
  u32 dest;                                                                   \
  arm_pc_offset(8);                                                           \
  arm_data_proc_##type();                                                     \
  dest = expr;                                                                \
  arm_pc_offset(-4);                                                          \
  reg[rd] = dest;                                                             \
}                                                                             \

#define flags_vars(src_a, src_b)                                              \
  u32 dest;                                                                   \
  const u32 _sa = src_a;                                                      \
  const u32 _sb = src_b                                                       \

#define arm_data_proc_logic_flags(expr, type)                                 \
{                                                                             \
  arm_pc_offset(8);                                                           \
  arm_data_proc_flags_##type();                                               \
  u32 dest = expr;                                                            \
  calculate_flags_logic(dest);                                                \
  arm_pc_offset(-4);                                                          \
  reg[rd] = dest;                                                             \
  arm_spsr_restore_check();                                                   \
}                                                                             \

#define arm_data_proc_add_flags(src_a, src_b, src_c, type)                    \
{                                                                             \
  u32 _sc = src_c;                                                            \
  arm_pc_offset(8);                                                           \
  arm_data_proc_##type();                                                     \
  flags_vars(src_a, src_b);                                                   \
  dest = _sa + _sb;                                                           \
  c_flag = (dest < _sb);                                                      \
  dest += _sc;                                                                \
  c_flag |= (dest < _sc);                                                     \
  calculate_flags_add(dest, _sa, _sb);                                        \
  arm_pc_offset(-4);                                                          \
  reg[rd] = dest;                                                             \
  arm_spsr_restore_check();                                                   \
}

#define arm_data_proc_sub_flags(src_a, src_b, src_c, type)                    \
{                                                                             \
  u32 _sc = src_c;                                                            \
  arm_pc_offset(8);                                                           \
  arm_data_proc_##type();                                                     \
  flags_vars(src_a, src_b);                                                   \
  dest = _sa + (~(_sb)) + _sc;                                                \
  calculate_flags_sub(dest, _sa, _sb, _sc);                                   \
  arm_pc_offset(-4);                                                          \
  reg[rd] = dest;                                                             \
  arm_spsr_restore_check();                                                   \
}                                                                             \

#define arm_data_proc_test_logic(expr, type)                                  \
{                                                                             \
  arm_pc_offset(8);                                                           \
  arm_data_proc_flags_##type();                                               \
  u32 dest = expr;                                                            \
  calculate_flags_logic(dest);                                                \
  arm_pc_offset(-4);                                                          \
}                                                                             \

#define arm_data_proc_test_add(src_a, src_b, type)                            \
{                                                                             \
  arm_pc_offset(8);                                                           \
  arm_data_proc_##type();                                                     \
  flags_vars(src_a, src_b);                                                   \
  dest = _sa + _sb;                                                           \
  c_flag = (dest < _sb);                                                      \
  calculate_flags_add(dest, _sa, _sb);                                        \
  arm_pc_offset(-4);                                                          \
}                                                                             \

#define arm_data_proc_test_sub(src_a, src_b, type)                            \
{                                                                             \
  arm_pc_offset(8);                                                           \
  arm_data_proc_##type();                                                     \
  flags_vars(src_a, src_b);                                                   \
  dest = _sa - _sb;                                                           \
  calculate_flags_sub(dest, _sa, _sb, 1);                                     \
  arm_pc_offset(-4);                                                          \
}                                                                             \

#define arm_multiply_flags_yes(_dest)                                         \
  calculate_z_flag(_dest);                                                    \
  calculate_n_flag(_dest);                                                    \

#define arm_multiply_flags_no(_dest)                                          \

#define arm_multiply_long_flags_yes(_dest_lo, _dest_hi)                       \
  z_flag = (_dest_lo == 0) & (_dest_hi == 0);                                 \
  calculate_n_flag(_dest_hi)                                                  \

#define arm_multiply_long_flags_no(_dest_lo, _dest_hi)                        \

#define arm_multiply(add_op, flags)                                           \
{                                                                             \
  u32 dest;                                                                   \
  arm_decode_multiply();                                                      \
  dest = (reg[rm] * reg[rs]) add_op;                                          \
  arm_multiply_flags_##flags(dest);                                           \
  reg[rd] = dest;                                                             \
  arm_pc_offset(4);                                                           \
}                                                                             \

#define arm_multiply_long_addop(type)                                         \
  + ((type##64)((((type##64)reg[rdhi]) << 32) | reg[rdlo]));                  \

#define arm_multiply_long(add_op, flags, type)                                \
{                                                                             \
  type##64 dest;                                                              \
  u32 dest_lo;                                                                \
  u32 dest_hi;                                                                \
  arm_decode_multiply_long();                                                 \
  dest = ((type##64)((type##32)reg[rm]) *                                     \
   (type##64)((type##32)reg[rn])) add_op;                                     \
  dest_lo = (u32)dest;                                                        \
  dest_hi = (u32)(dest >> 32);                                                \
  arm_multiply_long_flags_##flags(dest_lo, dest_hi);                          \
  reg[rdlo] = dest_lo;                                                        \
  reg[rdhi] = dest_hi;                                                        \
  arm_pc_offset(4);                                                           \
}                                                                             \

// Index by PRS fields (1 and 4 only!) and User-Privileged mode
// In user mode some bits are read only
// Bit #4 is always set to one (so all modes are 1XXXX)
// Reserved bits are always zero and cannot be modified
const u32 cpsr_masks[4][2] =
{
  // User, Privileged
  {0x00000000, 0x00000000},
  {0x00000020, 0x000000EF},
  {0xF0000000, 0xF0000000},
  {0xF0000020, 0xF00000EF}
};

// SPSR is always a privileged instruction
const u32 spsr_masks[4] = { 0x00000000, 0x000000EF, 0xF0000000, 0xF00000EF };

#define arm_psr_read(dummy, psr_reg)                                          \
  collapse_flags();                                                           \
  reg[rd] = psr_reg                                                           \

#define arm_psr_store_cpsr(source)                                            \
  const u32 store_mask = cpsr_masks[psr_pfield][PRIVMODE(reg[CPU_MODE])];     \
  reg[REG_CPSR] = (source & store_mask) | (reg[REG_CPSR] & (~store_mask));    \
  extract_flags();                                                            \
  if(store_mask & 0xFF)                                                       \
  {                                                                           \
    set_cpu_mode(cpu_modes[reg[REG_CPSR] & 0xF]);                             \
    check_for_interrupts();                                                   \
  }                                                                           \

#define arm_psr_store_spsr(source)                                            \
  const u32 store_mask = spsr_masks[psr_pfield];                              \
  u32 _psr = REG_SPSR(reg[CPU_MODE]);                                         \
  REG_SPSR(reg[CPU_MODE]) = (source & store_mask) | (_psr & (~store_mask))    \

#define arm_psr_store(source, psr_reg)                                        \
  arm_psr_store_##psr_reg(source)                                             \

#define arm_psr_src_reg reg[rm]

#define arm_psr_src_imm imm

#define arm_psr(op_type, transfer_type, psr_reg)                              \
{                                                                             \
  arm_decode_psr_##op_type(opcode);                                           \
  arm_pc_offset(4);                                                           \
  arm_psr_##transfer_type(arm_psr_src_##op_type, psr_reg);                    \
}                                                                             \

#define arm_data_trans_reg()                                                  \
  arm_decode_data_trans_reg();                                                \
  calculate_reg_offset()                                                      \

#define arm_data_trans_imm()                                                  \
  arm_decode_data_trans_imm()                                                 \

#define arm_data_trans_half_reg()                                             \
  arm_decode_half_trans_r()                                                   \

#define arm_data_trans_half_imm()                                             \
  arm_decode_half_trans_of()                                                  \

#define aligned_address_mask8  0xF0000000
#define aligned_address_mask16 0xF0000001
#define aligned_address_mask32 0xF0000003

#define fast_read_memory(size, type, addr, dest, readfn)                      \
{                                                                             \
  u8 *map;                                                                    \
  u32 _address = addr;                                                        \
                                                                              \
  if(_address < 0x10000000)                                                   \
  {                                                                           \
    /* Account for cycles and other stats */                                  \
    u8 region = _address >> 24;                                               \
    cycles_remaining -= ws_cyc_nseq[region][(size - 8) / 16];                 \
    STATS_MEMORY_ACCESS(read, type, region);                                  \
  }                                                                           \
                                                                              \
  if (                                                                        \
     (((_address >> 24) == 0) && (reg[REG_PC] >= 0x4000)) ||  /* BIOS read */ \
     (_address & aligned_address_mask##size) ||      /* Unaligned access */   \
     !(map = memory_map_read[_address >> 15])        /* Unmapped memory */    \
  )                                                                           \
  {                                                                           \
    dest = (type)(readfn)(_address);                                          \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    /* Aligned and mapped read */                                             \
    dest = (type)readaddress##size(map, (_address & 0x7FFF));                 \
  }                                                                           \
}                                                                             \

#define fast_write_memory(size, type, address, value)                         \
{                                                                             \
  u32 _address = (address) & ~(aligned_address_mask##size & 0x03);            \
  if(_address < 0x10000000)                                                   \
  {                                                                           \
    u8 region = _address >> 24;                                               \
    cycles_remaining -= ws_cyc_nseq[region][(size - 8) / 16];                 \
    STATS_MEMORY_ACCESS(write, type, region);                                 \
  }                                                                           \
                                                                              \
  cpu_alert |= write_memory##size(_address, value);                           \
}                                                                             \

#define load_aligned32(address, dest)                                         \
{                                                                             \
  u32 _address = address;                                                     \
  u8 *map = memory_map_read[_address >> 15];                                  \
  if(_address < 0x10000000)                                                   \
  {                                                                           \
    /* Account for cycles and other stats */                                  \
    u8 region = _address >> 24;                                               \
    cycles_remaining -= ws_cyc_seq[region][1];                                \
    STATS_MEMORY_ACCESS(read, u32, region);                                   \
  }                                                                           \
  if(_address < 0x10000000 && map)                                            \
  {                                                                           \
    dest = readaddress32(map, _address & 0x7FFF);                             \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    dest = read_memory32(_address);                                           \
  }                                                                           \
}                                                                             \

#define store_aligned32(address, value)                                       \
{                                                                             \
  u32 _address = address;                                                     \
  if(_address < 0x10000000)                                                   \
  {                                                                           \
    /* Account for cycles and other stats */                                  \
    u8 region = _address >> 24;                                               \
    cycles_remaining -= ws_cyc_seq[region][1];                                \
    STATS_MEMORY_ACCESS(write, u32, region);                                  \
  }                                                                           \
  cpu_alert |= write_memory32(_address, value);                               \
}                                                                             \

#define load_memory_u8(address, dest)                                         \
  fast_read_memory(8, u8, address, dest, read_memory8)                        \

#define load_memory_u16(address, dest)                                        \
  fast_read_memory(16, u16, address, dest, read_memory16)                     \

#define load_memory_u32(address, dest)                                        \
  fast_read_memory(32, u32, address, dest, read_memory32)                     \

#define load_memory_s8(address, dest)                                         \
  fast_read_memory(8, s8, address, dest, read_memory8)                        \

#define load_memory_s16(address, dest)                                        \
  fast_read_memory(16, s16, address, dest, read_memory16_signed)              \

#define store_memory_u8(address, value)                                       \
  fast_write_memory(8, u8, address, value)                                    \

#define store_memory_u16(address, value)                                      \
  fast_write_memory(16, u16, address, value)                                  \

#define store_memory_u32(address, value)                                      \
  fast_write_memory(32, u32, address, value)                                  \

#define no_op                                                                 \

#define arm_access_memory_writeback_yes(off_op)                               \
  reg[rn] = address off_op                                                    \

#define arm_access_memory_writeback_no(off_op)                                \

#define arm_access_memory_pc_preadjust_load()                                 \

#define arm_access_memory_pc_preadjust_store()                                \
  u32 reg_op = reg[rd];                                                       \
  if(rd == 15)                                                                \
    reg_op += 4                                                               \

#define load_reg_op reg[rd]                                                   \

#define store_reg_op reg_op                                                   \

#define arm_access_memory(access_type, off_op, off_type, mem_type,            \
 wb, wb_off_op)                                                               \
{                                                                             \
  arm_pc_offset(8);                                                           \
  arm_data_trans_##off_type();                                                \
  u32 address = reg[rn] off_op;                                               \
  arm_access_memory_pc_preadjust_##access_type();                             \
                                                                              \
  arm_pc_offset(-4);                                                          \
  arm_access_memory_writeback_##wb(wb_off_op);                                \
  access_type##_memory_##mem_type(address, access_type##_reg_op);             \
}                                                                             \

// Excutes an LDM/STM instruction

typedef enum { AccLoad, AccStore } AccMode;
typedef enum { AddrPreInc, AddrPreDec, AddrPostInc, AddrPostDec } AddrMode;

template<AccMode mode, bool writeback, bool sbit, AddrMode addr_mode>
inline cpu_alert_type exec_arm_block_mem(u32 rn, u32 reglist, s32 &cycles_remaining) {
  cpu_alert_type cpu_alert = CPU_ALERT_NONE;
  // Register register usage stats.
  using_register(arm, rn, memory_base);
  using_register_list(arm, reglist, 16);

  // Calcualte base address.
  u32 base = reg[rn];
  u32 numops = (bit_count[reglist >> 8] + bit_count[reglist & 0xFF]);
  s32 addr_off = (addr_mode == AddrPreInc || addr_mode == AddrPostInc) ? 4 : -4;  // Address incr/decr amount.
  u32 endaddr = base + addr_off * numops;

  u32 address = (addr_mode == AddrPreInc)  ? base + 4 :
                (addr_mode == AddrPostInc) ? base :
                (addr_mode == AddrPreDec)  ? endaddr : endaddr + 4;
  address &= ~3U;

  // If sbit is set, change to user mode and back, so to write the user regs.
  // However for LDM {PC} we restore CPSR from SPSR.
  // TODO: implement CPSR restore, only USER mode is now implemented.
  u32 old_cpsr = reg[REG_CPSR];
  if (sbit && (mode == AccStore || rn != REG_PC))
    set_cpu_mode(MODE_USER);

  // If base is in the reglist and writeback is enabled, the value of the
  // written register depends on the write cycle (ARM7TDM manual 4.11.6).
  // If the register is the first, the written value is the original value,
  // otherwise the update base register is written. For LDM loaded date
  // takes always precendence.
  bool wrbck_base = (1 << rn) & reglist;
  bool base_first = (((1 << rn) - 1) & reglist) == 0;
  bool writeback_first = (mode == AccLoad) || !(wrbck_base && base_first);

  if (writeback && writeback_first)
    reg[rn] = endaddr;

  arm_pc_offset(4);  // Advance PC

  for (u32 i = 0; i < 16; i++)  {
    if ((reglist >> i) & 0x01) {
      if (mode == AccLoad) {
        load_aligned32(address, reg[i]);
      } else {
        store_aligned32(address, (i == REG_PC) ? reg[REG_PC] + 4 : reg[i]);
      }
      address += 4;
    }
  }

  if (writeback && !writeback_first)
    reg[rn] = endaddr;

  if (sbit && (mode == AccStore || rn != REG_PC))
    set_cpu_mode(cpu_modes[old_cpsr & 0xF]);

  return cpu_alert;
}

template<AccMode mode, AddrMode addr_mode>
inline cpu_alert_type exec_thumb_block_mem(u32 rn, u32 reglist, s32 &cycles_remaining) {
  cpu_alert_type cpu_alert = CPU_ALERT_NONE;
  // Register register usage stats.
  using_register(arm, rn, memory_base);
  using_register_list(arm, reglist, 16);

  // Calcualte base address.
  u32 base = reg[rn];
  u32 numops = bit_count[reglist & 0xFF] + (bit_count[reglist >> 8] ? 1 : 0);
  s32 addr_off = (addr_mode == AddrPreInc || addr_mode == AddrPostInc) ? 4 : -4;  // Address incr/decr amount.
  u32 endaddr = base + addr_off * numops;

  u32 address = (addr_mode == AddrPreInc)  ? base + 4 :
                (addr_mode == AddrPostInc) ? base :
                (addr_mode == AddrPreDec)  ? endaddr : endaddr + 4;
  address &= ~3U;

  // Similar to the ARM version, just a bit simpler. See above.
  bool wrbck_base = (1 << rn) & reglist;
  bool base_first = (((1 << rn) - 1) & reglist) == 0;
  bool writeback_first = (mode == AccLoad) || !(wrbck_base && base_first);

  if (writeback_first)
    reg[rn] = endaddr;

  thumb_pc_offset(2);  // Advance PC

  if (mode == AccLoad) {
    for (u32 i = 0; i < 8; i++)  {
      if ((reglist >> i) & 0x01) {
        load_aligned32(address, reg[i]);
        address += 4;
      }
    }
    if (reglist & (1 << REG_PC)) {
      load_aligned32(address, reg[REG_PC]);
      reg[REG_PC] &= ~0x01;
    }
  } else {
    for (u32 i = 0; i < 8; i++)  {
      if ((reglist >> i) & 0x01) {
        store_aligned32(address, reg[i]);
        address += 4;
      }
    }
    if (reglist & (1 << REG_LR)) {
      store_aligned32(address, reg[REG_LR]);
    }
  }

  if (!writeback_first)
    reg[rn] = endaddr;

  return cpu_alert;
}

static inline bool gba_arm_execute_hot_fast_single(u32 opcode,
  u32 &n_flag, u32 &z_flag, u32 &c_flag, u32 &v_flag,
  s32 &cycles_remaining)
{
  switch(opcode)
  {
    case 0xE080EBCE: /* ADD r14, r0, r14, ASR #23 */
      reg[14] = reg[0] + (u32)((s32)reg[14] >> 23);
      reg[REG_PC] += 4;
      gba_arm_hot_fast_hit();
      return true;

    case 0xE1A004C1: /* MOV r0, r1, ASR #9 */
      reg[0] = (u32)((s32)reg[1] >> 9);
      reg[REG_PC] += 4;
      gba_arm_hot_fast_hit();
      return true;

    case 0xE2588004: /* SUBS r8, r8, #4 */
    {
      const u32 src = reg[8];
      const u32 dest = src - 4;
      reg[8] = dest;
      z_flag = (dest == 0);
      n_flag = dest >> 31;
      c_flag = 4 <= src;
      v_flag = (((src ^ 4) & (src ^ dest)) >> 31);
      reg[REG_PC] += 4;
      gba_arm_hot_fast_hit();
      return true;
    }

    case 0xCAFFFFF0: /* BGT pc - 56 */
      reg[REG_PC] -= 56;
      cycles_remaining -= ws_cyc_nseq[(reg[REG_PC] >> 24) & 0xF][1];
      gba_arm_hot_fast_hit();
      return true;

    case 0xE4856004: /* STR r6, [r5], #4 */
    {
      const u32 address = reg[5] & ~3U;
      const u8 region = address >> 24;
      if(region == 0x03)
        address32(iwram, (address & 0x7FFF) + 0x8000) = eswap32(reg[6]);
      else if(region == 0x02)
        address32(ewram, address & 0x3FFFF) = eswap32(reg[6]);
      else
      {
        gba_arm_hot_fast_miss();
        return false;
      }
      cycles_remaining -= ws_cyc_nseq[region][1];
      reg[5] += 4;
      reg[REG_PC] += 4;
      gba_arm_hot_fast_hit();
      return true;
    }

    case 0xE19710D6: /* LDRSB r1, [r7 + r6] */
    {
      const u32 address = reg[7] + reg[6];
      const u8 region = address >> 24;
      if(region == 0x03)
        reg[1] = (u32)(s32)(s8)iwram[(address & 0x7FFF) + 0x8000];
      else if(region == 0x02)
        reg[1] = (u32)(s32)(s8)ewram[address & 0x3FFFF];
      else
      {
        gba_arm_hot_fast_miss();
        return false;
      }
      cycles_remaining -= ws_cyc_nseq[region][0];
      reg[REG_PC] += 4;
      gba_arm_hot_fast_hit();
      return true;
    }

    case 0xE1D510D0: /* LDRSB r1, [r5 + #208] */
    {
      const u32 address = reg[5] + 208;
      const u8 region = address >> 24;
      if(region == 0x03)
        reg[1] = (u32)(s32)(s8)iwram[(address & 0x7FFF) + 0x8000];
      else if(region == 0x02)
        reg[1] = (u32)(s32)(s8)ewram[address & 0x3FFFF];
      else
      {
        gba_arm_hot_fast_miss();
        return false;
      }
      cycles_remaining -= ws_cyc_nseq[region][0];
      reg[REG_PC] += 4;
      gba_arm_hot_fast_hit();
      return true;
    }

    case 0xE3C330DF: /* BIC r3, r3, #0xDF */
      reg[3] &= ~0xDFU;
      reg[REG_PC] += 4;
      gba_arm_hot_fast_hit();
      return true;
  }

  const u32 op = (opcode >> 20) & 0xFF;

  if(op >= 0xA0 && op <= 0xAF)
  {
    const s32 offset = ((s32)((u32)(opcode << 8))) >> 6;
    reg[REG_PC] += offset + 8;
    cycles_remaining -= ws_cyc_nseq[(reg[REG_PC] >> 24) & 0xF][1];
    gba_arm_hot_fast_hit();
    return true;
  }

  if(op >= 0xB0 && op <= 0xBF)
  {
    const s32 offset = ((s32)((u32)(opcode << 8))) >> 6;
    reg[REG_LR] = reg[REG_PC] + 4;
    reg[REG_PC] += offset + 8;
    cycles_remaining -= ws_cyc_nseq[(reg[REG_PC] >> 24) & 0xF][1];
    gba_arm_hot_fast_hit();
    return true;
  }

  const u32 rd = (opcode >> 12) & 0x0F;
  const u32 rn = (opcode >> 16) & 0x0F;
  if(rd == REG_PC)
  {
    gba_arm_hot_fast_miss();
    return false;
  }

  switch(op)
  {
    case 0x00: /* AND rd, rn, rm */
    case 0x04: /* SUB rd, rn, rm */
    case 0x08: /* ADD rd, rn, rm */
    case 0x1A: /* MOV rd, rm */
    case 0x1B: /* MOVS rd, rm */
    {
      if((opcode & 0x90) == 0x90 || (opcode & 0x10) != 0)
      {
        gba_arm_hot_fast_miss();
        return false;
      }
      const u32 rm = opcode & 0x0F;
      if(((op != 0x1A && op != 0x1B) && rn == REG_PC) || rm == REG_PC)
      {
        gba_arm_hot_fast_miss();
        return false;
      }

      const u32 shift_imm = (opcode >> 7) & 0x1F;
      const u32 shift_type = (opcode >> 5) & 0x03;
      const u32 src = reg[rm];
      u32 reg_sh;
      u32 new_c = c_flag;
      switch(shift_type)
      {
        case 0:
          reg_sh = src << shift_imm;
          if(op == 0x1B && shift_imm)
            new_c = (src >> (32 - shift_imm)) & 1;
          break;
        case 1:
          if(shift_imm == 0)
          {
            reg_sh = 0;
            if(op == 0x1B)
              new_c = src >> 31;
          }
          else
          {
            reg_sh = src >> shift_imm;
            if(op == 0x1B)
              new_c = (src >> (shift_imm - 1)) & 1;
          }
          break;
        case 2:
          if(shift_imm == 0)
          {
            reg_sh = (u32)((s32)src >> 31);
            if(op == 0x1B)
              new_c = reg_sh & 1;
          }
          else
          {
            reg_sh = (u32)((s32)src >> shift_imm);
            if(op == 0x1B)
              new_c = (src >> (shift_imm - 1)) & 1;
          }
          break;
        default:
          if(shift_imm == 0)
          {
            reg_sh = (src >> 1) | (c_flag << 31);
            if(op == 0x1B)
              new_c = src & 1;
          }
          else
          {
            reg_sh = (src >> shift_imm) | (src << (32 - shift_imm));
            if(op == 0x1B)
              new_c = (src >> (shift_imm - 1)) & 1;
          }
          break;
      }

      u32 dest;
      if(op == 0x00)
        dest = reg[rn] & reg_sh;
      else if(op == 0x04)
        dest = reg[rn] - reg_sh;
      else if(op == 0x08)
        dest = reg[rn] + reg_sh;
      else
      {
        dest = reg_sh;
        if(op == 0x1B)
          c_flag = new_c;
      }
      if(op == 0x1B)
      {
        z_flag = (dest == 0);
        n_flag = dest >> 31;
      }
      reg[rd] = dest;
      reg[REG_PC] += 4;
      gba_arm_hot_fast_hit();
      return true;
    }

    case 0x19: /* LDRSB rd, [rn + rm] */
    case 0x1D: /* LDRSB rd, [rn + imm] */
    {
      if(((opcode >> 5) & 0x03) != 2 || rd == REG_PC || rn == REG_PC)
      {
        gba_arm_hot_fast_miss();
        return false;
      }

      u32 address;
      if(op == 0x19)
      {
        const u32 rm = opcode & 0x0F;
        if(rm == REG_PC)
        {
          gba_arm_hot_fast_miss();
          return false;
        }
        address = reg[rn] + reg[rm];
      }
      else
      {
        const u32 offset = ((opcode >> 4) & 0xF0) | (opcode & 0x0F);
        address = reg[rn] + offset;
      }

      u8 region = address >> 24;
      if(region == 0x03)
      {
        cycles_remaining -= ws_cyc_nseq[region][0];
        reg[rd] = (u32)(s32)(s8)iwram[(address & 0x7FFF) + 0x8000];
      }
      else if(region == 0x02)
      {
        cycles_remaining -= ws_cyc_nseq[region][0];
        reg[rd] = (u32)(s32)(s8)ewram[address & 0x3FFFF];
      }
      else
      {
        gba_arm_hot_fast_miss();
        return false;
      }
      reg[REG_PC] += 4;
      gba_arm_hot_fast_hit();
      return true;
    }

    case 0x48: /* STR rd, [rn], +imm */
    {
      if(rd == REG_PC || rn == REG_PC)
      {
        gba_arm_hot_fast_miss();
        return false;
      }
      u32 address = reg[rn] & ~3U;
      u8 region = address >> 24;
      if(region != 0x02 && region != 0x03)
      {
        gba_arm_hot_fast_miss();
        return false;
      }
      const u32 offset = opcode & 0x0FFF;
      cycles_remaining -= ws_cyc_nseq[region][1];
      if(!gba_thumb_store_direct_u32_fast(address, reg[rd]))
      {
        gba_arm_hot_fast_miss();
        return false;
      }
      reg[rn] = reg[rn] + offset;
      reg[REG_PC] += 4;
      gba_arm_hot_fast_hit();
      return true;
    }

    case 0x25: /* SUBS rd, rn, imm */
    case 0x29: /* ADDS rd, rn, imm */
    case 0x3C: /* BIC rd, rn, imm */
    case 0x28: /* ADD rd, rn, imm */
    case 0x3A: /* MOV rd, imm */
    {
      if((op != 0x3A) && rn == REG_PC)
      {
        gba_arm_hot_fast_miss();
        return false;
      }

      const u32 imm8 = opcode & 0xFF;
      const u32 rot = ((opcode >> 8) & 0x0F) * 2;
      const u32 imm = rot ? ((imm8 >> rot) | (imm8 << (32 - rot))) : imm8;
      u32 dest;
      if(op == 0x25)
      {
        const u32 src = reg[rn];
        dest = src - imm;
        z_flag = (dest == 0);
        n_flag = dest >> 31;
        c_flag = imm <= src;
        v_flag = (((src ^ imm) & (src ^ dest)) >> 31);
      }
      else if(op == 0x29)
      {
        const u32 src = reg[rn];
        dest = src + imm;
        z_flag = (dest == 0);
        n_flag = dest >> 31;
        c_flag = dest < imm;
        v_flag = ((~(src ^ imm) & (src ^ dest)) >> 31);
      }
      else if(op == 0x3C)
      {
        dest = reg[rn] & ~imm;
      }
      else if(op == 0x28)
      {
        dest = reg[rn] + imm;
      }
      else
      {
        dest = imm;
      }

      reg[rd] = dest;
      reg[REG_PC] += 4;
      gba_arm_hot_fast_hit();
      return true;
    }
  }

  gba_arm_hot_fast_miss();
  return false;
}

static inline bool gba_arm_condition_passed_fast(u32 condition,
  u32 n_flag, u32 z_flag, u32 c_flag, u32 v_flag)
{
  switch(condition)
  {
    case 0x0: return z_flag != 0;                         /* EQ */
    case 0x1: return z_flag == 0;                         /* NE */
    case 0x2: return c_flag != 0;                         /* CS */
    case 0x3: return c_flag == 0;                         /* CC */
    case 0x4: return n_flag != 0;                         /* MI */
    case 0x5: return n_flag == 0;                         /* PL */
    case 0x6: return v_flag != 0;                         /* VS */
    case 0x7: return v_flag == 0;                         /* VC */
    case 0x8: return c_flag != 0 && z_flag == 0;          /* HI */
    case 0x9: return !(c_flag != 0 && z_flag == 0);       /* LS */
    case 0xA: return n_flag == v_flag;                    /* GE */
    case 0xB: return n_flag != v_flag;                    /* LT */
    case 0xC: return z_flag == 0 && n_flag == v_flag;     /* GT */
    case 0xD: return z_flag != 0 || n_flag != v_flag;     /* LE */
    case 0xE: return true;                                /* AL */
  default:  return false;                               /* NV/reserved */
  }
}

static inline bool gba_arm_execute_hot_fast(u32 opcode,
  u32 &n_flag, u32 &z_flag, u32 &c_flag, u32 &v_flag,
  s32 &cycles_remaining)
{
  return gba_arm_execute_hot_fast_single(opcode, n_flag, z_flag, c_flag,
      v_flag, cycles_remaining);
}

static inline bool gba_arm_hot_fast_can_start(u32 opcode)
{
  const u32 op = (opcode >> 20) & 0xFF;
  if(op >= 0xA0 && op <= 0xBF)
    return true;

  switch(op)
  {
    case 0x00:
    case 0x04:
    case 0x08:
    case 0x19:
    case 0x1A:
    case 0x1B:
    case 0x1D:
    case 0x25:
    case 0x28:
    case 0x29:
    case 0x3A:
    case 0x3C:
    case 0x48:
      return true;
    default:
      return false;
  }
}

#define arm_swap(type)                                                        \
{                                                                             \
  arm_decode_swap();                                                          \
  u32 temp;                                                                   \
  load_memory_##type(reg[rn], temp);                                          \
  store_memory_##type(reg[rn], reg[rm]);                                      \
  reg[rd] = temp;                                                             \
  arm_pc_offset(4);                                                           \
}                                                                             \

// Types: add_sub, add_sub_imm, alu_op, imm
// Affects N/Z/C/V flags

#define thumb_add(type, dest_reg, src_a, src_b, src_c)                        \
{                                                                             \
  const u32 _sc = src_c;                                                      \
  thumb_decode_##type();                                                      \
  const u32 _sa = src_a;                                                      \
  const u32 _sb = src_b;                                                      \
  u32 dest = _sa + _sb;                                                       \
  c_flag = (dest < _sb);                                                      \
  dest += _sc;                                                                \
  c_flag |= (dest < _sc);                                                     \
  calculate_flags_add(dest, _sa, _sb);                                        \
  reg[dest_reg] = dest;                                                       \
  thumb_pc_offset(2);                                                         \
}                                                                             \

#define thumb_add_noflags(type, dest_reg, src_a, src_b)                       \
{                                                                             \
  thumb_decode_##type();                                                      \
  u32 dest = (src_a) + (src_b);                                               \
  reg[dest_reg] = dest;                                                       \
  thumb_pc_offset(2);                                                         \
}                                                                             \

#define thumb_sub(type, dest_reg, src_a, src_b, src_c)                        \
{                                                                             \
  thumb_decode_##type();                                                      \
  const u32 _sa = src_a;                                                      \
  const u32 _sb = src_b;                                                      \
  const u32 _sc = src_c;                                                      \
  u32 dest = _sa + (~_sb) + _sc;                                              \
  calculate_flags_sub(dest, _sa, _sb, _sc);                                   \
  reg[dest_reg] = dest;                                                       \
  thumb_pc_offset(2);                                                         \
}                                                                             \

// Affects N/Z flags

#define thumb_logic(type, dest_reg, expr)                                     \
{                                                                             \
  thumb_decode_##type();                                                      \
  u32 dest = expr;                                                            \
  calculate_flags_logic(dest);                                                \
  reg[dest_reg] = dest;                                                       \
  thumb_pc_offset(2);                                                         \
}                                                                             \

// Decode types: shift, alu_op
// Operation types: lsl, lsr, asr, ror
// Affects N/Z/C flags

#define thumb_shift_lsl_reg()                                                 \
  u32 shift = reg[rs];                                                        \
  u32 dest = reg[rd];                                                         \
  if(shift != 0)                                                              \
  {                                                                           \
    if(shift > 31)                                                            \
    {                                                                         \
      if(shift == 32)                                                         \
        c_flag = dest & 0x01;                                                 \
      else                                                                    \
        c_flag = 0;                                                           \
      dest = 0;                                                               \
    }                                                                         \
    else                                                                      \
    {                                                                         \
      c_flag = (dest >> (32 - shift)) & 0x01;                                 \
      dest <<= shift;                                                         \
    }                                                                         \
  }                                                                           \

#define thumb_shift_lsr_reg()                                                 \
  u32 shift = reg[rs];                                                        \
  u32 dest = reg[rd];                                                         \
  if(shift != 0)                                                              \
  {                                                                           \
    if(shift > 31)                                                            \
    {                                                                         \
      if(shift == 32)                                                         \
        c_flag = dest >> 31;                                                  \
      else                                                                    \
        c_flag = 0;                                                           \
      dest = 0;                                                               \
    }                                                                         \
    else                                                                      \
    {                                                                         \
      c_flag = (dest >> (shift - 1)) & 0x01;                                  \
      dest >>= shift;                                                         \
    }                                                                         \
  }                                                                           \

#define thumb_shift_asr_reg()                                                 \
  u32 shift = reg[rs];                                                        \
  u32 dest = reg[rd];                                                         \
  if(shift != 0)                                                              \
  {                                                                           \
    if(shift > 31)                                                            \
    {                                                                         \
      dest = (s32)dest >> 31;                                                 \
      c_flag = dest & 0x01;                                                   \
    }                                                                         \
    else                                                                      \
    {                                                                         \
      c_flag = (dest >> (shift - 1)) & 0x01;                                  \
      dest = (s32)dest >> shift;                                              \
    }                                                                         \
  }                                                                           \

#define thumb_shift_ror_reg()                                                 \
  u32 shift = reg[rs];                                                        \
  u32 dest = reg[rd];                                                         \
  if(shift != 0)                                                              \
  {                                                                           \
    c_flag = (dest >> (shift - 1)) & 0x01;                                    \
    ror(dest, dest, shift);                                                   \
  }                                                                           \

#define thumb_shift_lsl_imm()                                                 \
  u32 dest = reg[rs];                                                         \
  if(imm != 0)                                                                \
  {                                                                           \
    c_flag = (dest >> (32 - imm)) & 0x01;                                     \
    dest <<= imm;                                                             \
  }                                                                           \

#define thumb_shift_lsr_imm()                                                 \
  u32 dest;                                                                   \
  if(imm == 0)                                                                \
  {                                                                           \
    dest = 0;                                                                 \
    c_flag = reg[rs] >> 31;                                                   \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    dest = reg[rs];                                                           \
    c_flag = (dest >> (imm - 1)) & 0x01;                                      \
    dest >>= imm;                                                             \
  }                                                                           \

#define thumb_shift_asr_imm()                                                 \
  u32 dest;                                                                   \
  if(imm == 0)                                                                \
  {                                                                           \
    dest = (s32)reg[rs] >> 31;                                                \
    c_flag = dest & 0x01;                                                     \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    dest = reg[rs];                                                           \
    c_flag = (dest >> (imm - 1)) & 0x01;                                      \
    dest = (s32)dest >> imm;                                                  \
  }                                                                           \

#define thumb_shift_ror_imm()                                                 \
  u32 dest = reg[rs];                                                         \
  if(imm == 0)                                                                \
  {                                                                           \
    u32 old_c_flag = c_flag;                                                  \
    c_flag = dest & 0x01;                                                     \
    dest = (dest >> 1) | (old_c_flag << 31);                                  \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    c_flag = (dest >> (imm - 1)) & 0x01;                                      \
    ror(dest, dest, imm);                                                     \
  }                                                                           \

#define thumb_shift(decode_type, op_type, value_type)                         \
{                                                                             \
  thumb_decode_##decode_type();                                               \
  thumb_shift_##op_type##_##value_type();                                     \
  calculate_flags_logic(dest);                                                \
  reg[rd] = dest;                                                             \
  thumb_pc_offset(2);                                                         \
}                                                                             \

#define thumb_test_add(type, src_a, src_b)                                    \
{                                                                             \
  thumb_decode_##type();                                                      \
  const u32 _sa = src_a;                                                      \
  const u32 _sb = src_b;                                                      \
  u32 dest = _sa + _sb;                                                       \
  c_flag = (dest < _sb);                                                      \
  calculate_flags_add(dest, src_a, src_b);                                    \
  thumb_pc_offset(2);                                                         \
}                                                                             \

#define thumb_test_sub(type, src_a, src_b)                                    \
{                                                                             \
  thumb_decode_##type();                                                      \
  const u32 _sa = src_a;                                                      \
  const u32 _sb = src_b;                                                      \
  u32 dest = _sa - _sb;                                                       \
  calculate_flags_sub(dest, src_a, src_b, 1);                                 \
  thumb_pc_offset(2);                                                         \
}                                                                             \

#define thumb_test_logic(type, expr)                                          \
{                                                                             \
  thumb_decode_##type();                                                      \
  u32 dest = expr;                                                            \
  calculate_flags_logic(dest);                                                \
  thumb_pc_offset(2);                                                         \
}

#define thumb_hireg_op(expr)                                                  \
{                                                                             \
  thumb_pc_offset(4);                                                         \
  thumb_decode_hireg_op();                                                    \
  u32 dest = expr;                                                            \
  thumb_pc_offset(-2);                                                        \
  if(rd == 15)                                                                \
  {                                                                           \
    reg[REG_PC] = dest & ~0x01;                                               \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    reg[rd] = dest;                                                           \
  }                                                                           \
}                                                                             \

// Operation types: imm, mem_reg, mem_imm

#define thumb_access_memory(access_type, op_type, address, reg_op,            \
 mem_type)                                                                    \
{                                                                             \
  thumb_pc_offset(2);                                                         \
  thumb_decode_##op_type();                                                   \
  access_type##_memory_##mem_type(address, reg_op);                           \
}                                                                             \

#define thumb_conditional_branch(condition)                                   \
{                                                                             \
  thumb_decode_branch_cond();                                                 \
  if(condition)                                                               \
  {                                                                           \
    thumb_pc_offset((offset * 2) + 4);                                        \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    thumb_pc_offset(2);                                                       \
  }                                                                           \
  cycles_remaining -= ws_cyc_nseq[reg[REG_PC] >> 24][0];                      \
}                                                                             \

// When a mode change occurs from non-FIQ to non-FIQ retire the current
// reg[13] and reg[14] into reg_mode[cpu_mode][5] and reg_mode[cpu_mode][6]
// respectively and load into reg[13] and reg[14] reg_mode[new_mode][5] and
// reg_mode[new_mode][6]. When swapping to/from FIQ retire/load reg[8]
// through reg[14] to/from reg_mode[MODE_FIQ][0] through reg_mode[MODE_FIQ][6].

const u32 cpu_modes[16] =
{
  MODE_USER, MODE_FIQ, MODE_IRQ, MODE_SUPERVISOR,
  MODE_INVALID, MODE_INVALID, MODE_INVALID, MODE_ABORT,
  MODE_INVALID, MODE_INVALID, MODE_INVALID, MODE_UNDEFINED,
  MODE_INVALID, MODE_INVALID, MODE_INVALID, MODE_SYSTEM
};

// ARM/Thumb mode is stored in the flags directly, this is simpler than
// shadowing it since it has a constant 1bit represenation.

u32 instruction_count = 0;

void set_cpu_mode(cpu_mode_type new_mode)
{
  cpu_mode_type cpu_mode = reg[CPU_MODE];

  if(cpu_mode == new_mode)
     return;

  if(new_mode == MODE_FIQ)
  {
     for (u32 i = 8; i < 15; i++)
        REG_MODE(cpu_mode)[i - 8] = reg[i];
  }
  else
  {
     REG_MODE(cpu_mode)[5] = reg[REG_SP];
     REG_MODE(cpu_mode)[6] = reg[REG_LR];
  }

  if(cpu_mode == MODE_FIQ)
  {
     for (u32 i = 8; i < 15; i++)
        reg[i] = REG_MODE(new_mode)[i - 8];
  }
  else
  {
     reg[REG_SP] = REG_MODE(new_mode)[5];
     reg[REG_LR] = REG_MODE(new_mode)[6];
  }

  reg[CPU_MODE] = new_mode;
}

#define cpu_has_interrupt()                                 \
  (!(reg[REG_CPSR] & 0x80) && read_ioreg(REG_IME) &&        \
    (read_ioreg(REG_IE) & read_ioreg(REG_IF)))

// Returns whether the CPU has a pending interrupt.
cpu_alert_type check_interrupt() {
  return (cpu_has_interrupt()) ? CPU_ALERT_IRQ : CPU_ALERT_NONE;
}

// Checks for pending IRQs and raises them. This changes the CPU mode
// which means that it must be called with a valid CPU state.
u32 check_and_raise_interrupts()
{
  // Check any IRQ flag pending, IME and CPSR-IRQ enabled
  if (cpu_has_interrupt())
  {
    // Value after the FIQ returns, should be improved
    reg[REG_BUS_VALUE] = 0xe55ec002;

    // Interrupt handler in BIOS
    REG_MODE(MODE_IRQ)[6] = reg[REG_PC] + 4;
    REG_SPSR(MODE_IRQ) = reg[REG_CPSR];
    reg[REG_CPSR] = 0xD2;
    reg[REG_PC] = 0x00000018;

    set_cpu_mode(MODE_IRQ);

    // Wake up CPU if it is stopped/sleeping.
    if (reg[CPU_HALT_STATE] == CPU_STOP ||
        reg[CPU_HALT_STATE] == CPU_HALT)
      reg[CPU_HALT_STATE] = CPU_ACTIVE;

    return 1;
  }
  return 0;
}

// This function marks a pending interrupt but does not raise it.
// It simply updates IF register and returns whether the IRQ needs
// to be raised (that is, IE/IME/CPSR enable the IRQ).
// Safe to call via dynarec without proper registers saved.
cpu_alert_type flag_interrupt(irq_type irq_raised)
{
  // Flag interrupt
  write_ioreg(REG_IF, read_ioreg(REG_IF) | irq_raised);

  return check_interrupt();
}

#ifndef HAVE_DYNAREC

// When switching modes set spsr[new_mode] to cpsr. Modifying PC as the
// target of a data proc instruction will set cpsr to spsr[cpu_mode].
u32 reg[64];
u32 spsr[6];
u32 reg_mode[7][7];

#ifndef RETRO_GO
u16 oam_ram[512];
u16 palette_ram[512];
u16 palette_ram_converted[512];
u8 ewram[1024 * 256 * 2];
u8 iwram[1024 * 32 * 2];
u8 vram[1024 * 96];
u8 *memory_map_read[8 * 1024];
u16 io_registers[512];
#endif
#endif

void execute_arm(u32 cycles)
{
  u32 opcode;
  u32 condition;
  u32 n_flag, z_flag, c_flag, v_flag;
  u32 pc_region = (reg[REG_PC] >> 15);
  u8 *pc_address_block = NULL;
  u32 new_pc_region;
  s32 cycles_remaining;
  u32 update_ret;
  cpu_alert_type cpu_alert;
  u32 arm_fast_attempted;

  // Hoisted out of the decode loops: the engine mode only changes between
  // frames, but as a global the compiler had to reload it after every call that
  // could write memory -- once per interpreted instruction on the hot path.
  const u32 interp_fast = gba_interp_fast_enabled;

  gba_execute_calls++;
  gba_execute_last_cycles = cycles;
  gba_execute_last_pc = reg[REG_PC];
  gba_execute_last_cpsr = reg[REG_CPSR];
  gba_execute_last_halt = reg[CPU_HALT_STATE];
  gba_execute_last_update_ret = 0;

  if(pc_region < (GBA_MEMORY_MAP_READ_SIZE / sizeof(u8 *)))
  {
    pc_address_block = memory_map_read[pc_region];
    if(!pc_address_block)
      pc_address_block = load_gamepak_page(pc_region & 0x3FF);
    touch_gamepak_page(pc_region);
  }

  cycles_remaining = cycles;
  while(1)
  {
    /* Do not execute until CPU is active */
    if (reg[CPU_HALT_STATE] != CPU_ACTIVE) {
       u32 ret = update_gba(cycles_remaining);
       gba_execute_halt_updates++;
       gba_execute_last_update_ret = ret;
       gba_execute_last_pc = reg[REG_PC];
       gba_execute_last_cpsr = reg[REG_CPSR];
       gba_execute_last_halt = reg[CPU_HALT_STATE];
       if (completed_frame(ret))
          return;

       cycles_remaining = cycles_to_run(ret);
    }

    cpu_alert = CPU_ALERT_NONE;
    extract_flags();

    if(reg[REG_CPSR] & 0x20)
      goto thumb_loop;

    do
    {
arm_loop:

       collapse_flags();

       /* Process cheats if we are about to execute the cheat hook */
       if (reg[REG_PC] == cheat_master_hook)
          process_cheats();

       // Reaching the BIOS reset vector restarts the game. A legitimate restart
       // arrives through SWI SoftReset, and legitimate BIOS entries land on the
       // SWI (0x08) or IRQ (0x18) vectors, so anything down here came from a
       // branch to a null or corrupt target. The vector is ARM code, so a BX or
       // POP {pc} to zero lands in this loop; capture the caller once. LR is the
       // primary clue when the derail came from a BL through a bad pointer.
       if(reg[REG_PC] < 0x08)
       {
          gba_guest_reset_trips++;
          if(gba_guest_reset_trips == 1)
          {
             gba_guest_reset_prev_pc = gba_execute_last_pc;
             gba_guest_reset_lr = reg[REG_LR];
             gba_guest_reset_sp = reg[REG_SP];
             gba_guest_reset_cpsr = reg[REG_CPSR];
             ESP_LOGE("gba-reset",
                 "guest hit BIOS reset vector pc=%08lx lr=%08lx sp=%08lx cpsr=%08lx lastpc=%08lx",
                 (unsigned long)reg[REG_PC], (unsigned long)reg[REG_LR],
                 (unsigned long)reg[REG_SP], (unsigned long)reg[REG_CPSR],
                 (unsigned long)gba_execute_last_pc);
          }
       }

       // A guest can also restart by branching straight at the ROM header rather
       // than through the BIOS. Booting legitimately uses the first entry, so
       // report from the second onwards.
       if(reg[REG_PC] == 0x08000000)
       {
          gba_guest_entry_trips++;
          if(gba_guest_entry_trips == 2)
          {
             gba_guest_reset_prev_pc = gba_execute_last_pc;
             gba_guest_reset_lr = reg[REG_LR];
             gba_guest_reset_sp = reg[REG_SP];
             gba_guest_reset_cpsr = reg[REG_CPSR];
             ESP_LOGE("gba-reset",
                 "guest re-entered ROM header lr=%08lx sp=%08lx cpsr=%08lx lastpc=%08lx",
                 (unsigned long)reg[REG_LR], (unsigned long)reg[REG_SP],
                 (unsigned long)reg[REG_CPSR], (unsigned long)gba_execute_last_pc);
          }
       }

       if((reg[REG_PC] >= 0x0000186C) && (reg[REG_PC] <= 0x00001874) &&
          (reg[3] == 0x04000000) && ((s32)reg[1] < 0) &&
          (reg[1] >= 0xFFFFFE00))
       {
          memset(&iwram[0xFE00], 0, 0x200);
          reg[1] = 0;
          n_flag = 0;
          z_flag = 1;
          c_flag = 1;
          v_flag = 0;
          reg[REG_PC] = 0x00001878;
          cycles_remaining -= 384;
          gba_bios_init_loop_hle_count++;
          goto arm_loop;
       }

       /* Execute ARM instruction */
       using_instruction(arm);
       check_pc_region();
       reg[REG_PC] &= ~0x03;
#if GBA_THUMB_PROFILE
       if(reg[REG_PC] < 0x4000)
          gba_arm_bios_pc_histogram[reg[REG_PC] >> 2]++;
#endif
       if(!pc_address_block)
       {
          gba_bad_pc_count++;
          gba_bad_pc_last = reg[REG_PC];
          gba_bad_pc_last_cpsr = reg[REG_CPSR];
          if(gba_thumb_jit_runtime_enabled || gba_thumb_batch_enabled)
            gba_p4_thumb_jit_report_fault(0x42414441U, reg[REG_PC]);
          opcode = reg[REG_BUS_VALUE];
       }
       else
       {
          gba_block_cache_touch_arm(reg[REG_PC], pc_address_block);
          opcode = readaddress32(pc_address_block, (reg[REG_PC] & 0x7FFF));
       }
       gba_arm_profile_opcode(opcode);
       condition = opcode >> 28;
       arm_fast_attempted = 0;

       if(interp_fast && opcode == 0xCAFFFFF0) /* BGT pc - 56 */
       {
          if(z_flag | (n_flag != v_flag))
          {
             reg[REG_PC] += 4;
             goto skip_instruction;
          }

          reg[REG_PC] -= 56;
          cycles_remaining -= ws_cyc_nseq[(reg[REG_PC] >> 24) & 0xF][1];
          gba_arm_hot_fast_hit();
          goto skip_instruction;
       }

       if(interp_fast && condition == 0xE &&
          gba_arm_hot_fast_can_start(opcode))
       {
          arm_fast_attempted = 1;
          if(gba_arm_execute_hot_fast(opcode, n_flag, z_flag, c_flag, v_flag,
            cycles_remaining))
             goto skip_instruction;
       }

       switch(condition)
       {
          case 0x0:
             /* EQ */
             if(!z_flag)
                arm_next_instruction();
             break;
          case 0x1:
             /* NE      */
             if(z_flag)
                arm_next_instruction();
             break;
          case 0x2:
             /* CS       */
             if(!c_flag)
                arm_next_instruction();
             break;
          case 0x3:
             /* CC       */
             if(c_flag)
                arm_next_instruction();
             break;
          case 0x4:
             /* MI       */
             if(!n_flag)
                arm_next_instruction();
             break;

          case 0x5:
             /* PL       */
             if(n_flag)
                arm_next_instruction();
             break;

          case 0x6:
             /* VS       */
             if(!v_flag)
                arm_next_instruction();
             break;

          case 0x7:
             /* VC       */
             if(v_flag)
                arm_next_instruction();
             break;

          case 0x8:
             /* HI       */
             if((c_flag == 0) | z_flag)
                arm_next_instruction();
             break;

          case 0x9:
             /* LS       */
             if(c_flag & (z_flag ^ 1))
                arm_next_instruction();
             break;

          case 0xA:
             /* GE       */
             if(n_flag != v_flag)
                arm_next_instruction();
             break;

          case 0xB:
             /* LT       */
             if(n_flag == v_flag)
                arm_next_instruction();
             break;

          case 0xC:
             /* GT       */
             if(z_flag | (n_flag != v_flag))
                arm_next_instruction();
             break;

          case 0xD:
             /* LE       */
             if((z_flag == 0) & (n_flag == v_flag))
                arm_next_instruction();
             break;

          case 0xE:
             /* AL       */
             break;

          case 0xF:
             /* Reserved - treat as "never" */
             arm_next_instruction();
             break;
       }

       #ifdef TRACE_INSTRUCTIONS
       interp_trace_instruction(reg[REG_PC], 1);
       #endif

       if(!arm_fast_attempted && gba_arm_hot_fast_can_start(opcode) &&
          gba_arm_execute_hot_fast(opcode, n_flag, z_flag, c_flag, v_flag,
            cycles_remaining))
          goto skip_instruction;

       switch((opcode >> 20) & 0xFF)
       {
          case 0x00:
             if((opcode & 0x90) == 0x90)
             {
                if(opcode & 0x20)
                {
                   /* STRH rd, [rn], -rm */
                   arm_access_memory(store, no_op, half_reg, u16, yes, - reg[rm]);
                }
                else
                {
                   /* MUL rd, rm, rs */
                   arm_multiply(no_op, no);
                }
             }
             else
             {
                /* AND rd, rn, reg_op */
                arm_data_proc(reg[rn] & reg_sh, reg);
             }
             break;

          case 0x01:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 0:
                      /* MULS rd, rm, rs */
                      arm_multiply(no_op, yes);
                      break;

                   case 1:
                      /* LDRH rd, [rn], -rm */
                      arm_access_memory(load, no_op, half_reg, u16, yes, - reg[rm]);
                      break;

                   case 2:
                      /* LDRSB rd, [rn], -rm */
                      arm_access_memory(load, no_op, half_reg, s8, yes, - reg[rm]);
                      break;

                   case 3:
                      /* LDRSH rd, [rn], -rm */
                      arm_access_memory(load, no_op, half_reg, s16, yes, - reg[rm]);
                      break;
                }
             }
             else
             {
                /* ANDS rd, rn, reg_op */
                arm_data_proc_logic_flags(reg[rn] & reg_sh, reg);
             }
             break;

          case 0x02:
             if((opcode & 0x90) == 0x90)
             {
                if(opcode & 0x20)
                {
                   /* STRH rd, [rn], -rm */
                   arm_access_memory(store, no_op, half_reg, u16, yes, - reg[rm]);
                }
                else
                {
                   /* MLA rd, rm, rs, rn */
                   arm_multiply(+ reg[rn], no);
                }
             }
             else
             {
                /* EOR rd, rn, reg_op */
                arm_data_proc(reg[rn] ^ reg_sh, reg);
             }
             break;

          case 0x03:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 0:
                      /* MLAS rd, rm, rs, rn */
                      arm_multiply(+ reg[rn], yes);
                      break;

                   case 1:
                      /* LDRH rd, [rn], -rm */
                      arm_access_memory(load, no_op, half_reg, u16, yes, - reg[rm]);
                      break;

                   case 2:
                      /* LDRSB rd, [rn], -rm */
                      arm_access_memory(load, no_op, half_reg, s8, yes, - reg[rm]);
                      break;

                   case 3:
                      /* LDRSH rd, [rn], -rm */
                      arm_access_memory(load, no_op, half_reg, s16, yes, - reg[rm]);
                      break;
                }
             }
             else
             {
                /* EORS rd, rn, reg_op */
                arm_data_proc_logic_flags(reg[rn] ^ reg_sh, reg);
             }
             break;

          case 0x04:
             if((opcode & 0x90) == 0x90)
             {
                /* STRH rd, [rn], -imm */
                arm_access_memory(store, no_op, half_imm, u16, yes, - offset);
             }
             else
             {
                /* SUB rd, rn, reg_op */
                arm_data_proc(reg[rn] - reg_sh, reg);
             }
             break;

          case 0x05:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn], -imm */
                      arm_access_memory(load, no_op, half_imm, u16, yes, - offset);
                      break;

                   case 2:
                      /* LDRSB rd, [rn], -imm */
                      arm_access_memory(load, no_op, half_imm, s8, yes, - offset);
                      break;

                   case 3:
                      /* LDRSH rd, [rn], -imm */
                      arm_access_memory(load, no_op, half_imm, s16, yes, - offset);
                      break;
                }
             }
             else
             {
                /* SUBS rd, rn, reg_op */
                arm_data_proc_sub_flags(reg[rn], reg_sh, 1, reg);
             }
             break;

          case 0x06:
             if((opcode & 0x90) == 0x90)
             {
                /* STRH rd, [rn], -imm */
                arm_access_memory(store, no_op, half_imm, u16, yes, - offset);
             }
             else
             {
                /* RSB rd, rn, reg_op */
                arm_data_proc(reg_sh - reg[rn], reg);
             }
             break;

          case 0x07:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn], -imm */
                      arm_access_memory(load, no_op, half_imm, u16, yes, - offset);
                      break;

                   case 2:
                      /* LDRSB rd, [rn], -imm */
                      arm_access_memory(load, no_op, half_imm, s8, yes, - offset);
                      break;

                   case 3:
                      /* LDRSH rd, [rn], -imm */
                      arm_access_memory(load, no_op, half_imm, s16, yes, - offset);
                      break;
                }
             }
             else
             {
                /* RSBS rd, rn, reg_op */
                arm_data_proc_sub_flags(reg_sh, reg[rn], 1, reg);
             }
             break;

          case 0x08:
             if((opcode & 0x90) == 0x90)
             {
                if(opcode & 0x20)
                {
                   /* STRH rd, [rn], +rm */
                   arm_access_memory(store, no_op, half_reg, u16, yes, + reg[rm]);
                }
                else
                {
                   /* UMULL rd, rm, rs */
                   arm_multiply_long(no_op, no, u);
                }
             }
             else
             {
                /* ADD rd, rn, reg_op */
                arm_data_proc(reg[rn] + reg_sh, reg);
             }
             break;

          case 0x09:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 0:
                      /* UMULLS rdlo, rdhi, rm, rs */
                      arm_multiply_long(no_op, yes, u);
                      break;

                   case 1:
                      /* LDRH rd, [rn], +rm */
                      arm_access_memory(load, no_op, half_reg, u16, yes, + reg[rm]);
                      break;

                   case 2:
                      /* LDRSB rd, [rn], +rm */
                      arm_access_memory(load, no_op, half_reg, s8, yes, + reg[rm]);
                      break;

                   case 3:
                      /* LDRSH rd, [rn], +rm */
                      arm_access_memory(load, no_op, half_reg, s16, yes, + reg[rm]);
                      break;
                }
             }
             else
             {
                /* ADDS rd, rn, reg_op */
                arm_data_proc_add_flags(reg[rn], reg_sh, 0, reg);
             }
             break;

          case 0x0A:
             if((opcode & 0x90) == 0x90)
             {
                if(opcode & 0x20)
                {
                   /* STRH rd, [rn], +rm */
                   arm_access_memory(store, no_op, half_reg, u16, yes, + reg[rm]);
                }
                else
                {
                   /* UMLAL rd, rm, rs */
                   arm_multiply_long(arm_multiply_long_addop(u), no, u);
                }
             }
             else
             {
                /* ADC rd, rn, reg_op */
                arm_data_proc(reg[rn] + reg_sh + c_flag, reg);
             }
             break;

          case 0x0B:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 0:
                      /* UMLALS rdlo, rdhi, rm, rs */
                      arm_multiply_long(arm_multiply_long_addop(u), yes, u);
                      break;

                   case 1:
                      /* LDRH rd, [rn], +rm */
                      arm_access_memory(load, no_op, half_reg, u16, yes, + reg[rm]);
                      break;

                   case 2:
                      /* LDRSB rd, [rn], +rm */
                      arm_access_memory(load, no_op, half_reg, s8, yes, + reg[rm]);
                      break;

                   case 3:
                      /* LDRSH rd, [rn], +rm */
                      arm_access_memory(load, no_op, half_reg, s16, yes, + reg[rm]);
                      break;
                }
             }
             else
             {
                /* ADCS rd, rn, reg_op */
                arm_data_proc_add_flags(reg[rn], reg_sh, c_flag, reg);
             }
             break;

          case 0x0C:
             if((opcode & 0x90) == 0x90)
             {
                if(opcode & 0x20)
                {
                   /* STRH rd, [rn], +imm */
                   arm_access_memory(store, no_op, half_imm, u16, yes, + offset);
                }
                else
                {
                   /* SMULL rd, rm, rs */
                   arm_multiply_long(no_op, no, s);
                }
             }
             else
             {
                /* SBC rd, rn, reg_op */
                arm_data_proc(reg[rn] - (reg_sh + (c_flag ^ 1)), reg);
             }
             break;

          case 0x0D:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 0:
                      /* SMULLS rdlo, rdhi, rm, rs */
                      arm_multiply_long(no_op, yes, s);
                      break;

                   case 1:
                      /* LDRH rd, [rn], +imm */
                      arm_access_memory(load, no_op, half_imm, u16, yes, + offset);
                      break;

                   case 2:
                      /* LDRSB rd, [rn], +imm */
                      arm_access_memory(load, no_op, half_imm, s8, yes, + offset);
                      break;

                   case 3:
                      /* LDRSH rd, [rn], +imm */
                      arm_access_memory(load, no_op, half_imm, s16, yes, + offset);
                      break;
                }
             }
             else
             {
                /* SBCS rd, rn, reg_op */
                arm_data_proc_sub_flags(reg[rn], reg_sh, c_flag, reg);
             }
             break;

          case 0x0E:
             if((opcode & 0x90) == 0x90)
             {
                if(opcode & 0x20)
                {
                   /* STRH rd, [rn], +imm */
                   arm_access_memory(store, no_op, half_imm, u16, yes, + offset);
                }
                else
                {
                   /* SMLAL rd, rm, rs */
                   arm_multiply_long(arm_multiply_long_addop(s), no, s);
                }
             }
             else
             {
                /* RSC rd, rn, reg_op */
                arm_data_proc(reg_sh - reg[rn] + c_flag - 1, reg);
             }
             break;

          case 0x0F:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 0:
                      /* SMLALS rdlo, rdhi, rm, rs */
                      arm_multiply_long(arm_multiply_long_addop(s), yes, s);
                      break;

                   case 1:
                      /* LDRH rd, [rn], +imm */
                      arm_access_memory(load, no_op, half_imm, u16, yes, + offset);
                      break;

                   case 2:
                      /* LDRSB rd, [rn], +imm */
                      arm_access_memory(load, no_op, half_imm, s8, yes, + offset);
                      break;

                   case 3:
                      /* LDRSH rd, [rn], +imm */
                      arm_access_memory(load, no_op, half_imm, s16, yes, + offset);
                      break;
                }
             }
             else
             {
                /* RSCS rd, rn, reg_op */
                arm_data_proc_sub_flags(reg_sh, reg[rn], c_flag, reg);
             }
             break;

          case 0x10:
             if((opcode & 0x90) == 0x90)
             {
                if(opcode & 0x20)
                {
                   /* STRH rd, [rn - rm] */
                   arm_access_memory(store, - reg[rm], half_reg, u16, no, no_op);
                }
                else
                {
                   /* SWP rd, rm, [rn] */
                   arm_swap(u32);
                }
             }
             else
             {
                /* MRS rd, cpsr */
                arm_psr(reg, read, reg[REG_CPSR]);
             }
             break;

          case 0x11:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn - rm] */
                      arm_access_memory(load, - reg[rm], half_reg, u16, no, no_op);
                      break;

                   case 2:
                      /* LDRSB rd, [rn - rm] */
                      arm_access_memory(load, - reg[rm], half_reg, s8, no, no_op);
                      break;

                   case 3:
                      /* LDRSH rd, [rn - rm] */
                      arm_access_memory(load, - reg[rm], half_reg, s16, no, no_op);
                      break;
                }
             }
             else
             {
                /* TST rd, rn, reg_op */
                arm_data_proc_test_logic(reg[rn] & reg_sh, reg);
             }
             break;

          case 0x12:
             if((opcode & 0x90) == 0x90)
             {
                /* STRH rd, [rn - rm]! */
                arm_access_memory(store, - reg[rm], half_reg, u16, yes, no_op);
             }
             else
             {
                if(opcode & 0x10)
                {
                   /* BX rn */
                   arm_decode_branchx(opcode);
                   u32 src = reg[rn];
                   if(src & 0x01)
                   {
                      reg[REG_PC] = src - 1;
                      reg[REG_CPSR] |= 0x20;
                      goto thumb_loop;
                   }
                   else
                   {
                      reg[REG_PC] = src;
                   }
                   cycles_remaining -= ws_cyc_nseq[reg[REG_PC] >> 24][1];
                }
                else
                {
                   /* MSR cpsr, rm */
                   arm_psr(reg, store, cpsr);
                }
             }
             break;

          case 0x13:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn - rm]! */
                      arm_access_memory(load, - reg[rm], half_reg, u16, yes, no_op);
                      break;

                   case 2:
                      /* LDRSB rd, [rn - rm]! */
                      arm_access_memory(load, - reg[rm], half_reg, s8, yes, no_op);
                      break;

                   case 3:
                      /* LDRSH rd, [rn - rm]! */
                      arm_access_memory(load, - reg[rm], half_reg, s16, yes, no_op);
                      break;
                }
             }
             else
             {
                /* TEQ rd, rn, reg_op */
                arm_data_proc_test_logic(reg[rn] ^ reg_sh, reg);
             }
             break;

          case 0x14:
             if((opcode & 0x90) == 0x90)
             {
                if(opcode & 0x20)
                {
                   /* STRH rd, [rn - imm] */
                   arm_access_memory(store, - offset, half_imm, u16, no, no_op);
                }
                else
                {
                   /* SWPB rd, rm, [rn] */
                   arm_swap(u8);
                }
             }
             else
             {
                /* MRS rd, spsr */
                arm_psr(reg, read, REG_SPSR(reg[CPU_MODE]));
             }
             break;

          case 0x15:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn - imm] */
                      arm_access_memory(load, - offset, half_imm, u16, no, no_op);
                      break;

                   case 2:
                      /* LDRSB rd, [rn - imm] */
                      arm_access_memory(load, - offset, half_imm, s8, no, no_op);
                      break;

                   case 3:
                      /* LDRSH rd, [rn - imm] */
                      arm_access_memory(load, - offset, half_imm, s16, no, no_op);
                      break;
                }
             }
             else
             {
                /* CMP rn, reg_op */
                arm_data_proc_test_sub(reg[rn], reg_sh, reg);
             }
             break;

          case 0x16:
             if((opcode & 0x90) == 0x90)
             {
                /* STRH rd, [rn - imm]! */
                arm_access_memory(store, - offset, half_imm, u16, yes, no_op);
             }
             else
             {
                /* MSR spsr, rm */
                arm_psr(reg, store, spsr);
             }
             break;

          case 0x17:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn - imm]! */
                      arm_access_memory(load, - offset, half_imm, u16, yes, no_op);
                      break;

                   case 2:
                      /* LDRSB rd, [rn - imm]! */
                      arm_access_memory(load, - offset, half_imm, s8, yes, no_op);
                      break;

                   case 3:
                      /* LDRSH rd, [rn - imm]! */
                      arm_access_memory(load, - offset, half_imm, s16, yes, no_op);
                      break;
                }
             }
             else
             {
                /* CMN rd, rn, reg_op */
                arm_data_proc_test_add(reg[rn], reg_sh, reg);
             }
             break;

          case 0x18:
             if((opcode & 0x90) == 0x90)
             {
                /* STRH rd, [rn + rm] */
                arm_access_memory(store, + reg[rm], half_reg, u16, no, no_op);
             }
             else
             {
                /* ORR rd, rn, reg_op */
                arm_data_proc(reg[rn] | reg_sh, reg);
             }
             break;

          case 0x19:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn + rm] */
                      arm_access_memory(load, + reg[rm], half_reg, u16, no, no_op);
                      break;

                   case 2:
                      /* LDRSB rd, [rn + rm] */
                      arm_access_memory(load, + reg[rm], half_reg, s8, no, no_op);
                      break;

                   case 3:
                      /* LDRSH rd, [rn + rm] */
                      arm_access_memory(load, + reg[rm], half_reg, s16, no, no_op);
                      break;
                }
             }
             else
             {
                /* ORRS rd, rn, reg_op */
                arm_data_proc_logic_flags(reg[rn] | reg_sh, reg);
             }
             break;

          case 0x1A:
             if((opcode & 0x90) == 0x90)
             {
                /* STRH rd, [rn + rm]! */
                arm_access_memory(store, + reg[rm], half_reg, u16, yes, no_op);
             }
             else
             {
                /* MOV rd, reg_op */
                arm_data_proc(reg_sh, reg);
             }
             break;

          case 0x1B:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn + rm]! */
                      arm_access_memory(load, + reg[rm], half_reg, u16, yes, no_op);
                      break;

                   case 2:
                      /* LDRSB rd, [rn + rm]! */
                      arm_access_memory(load, + reg[rm], half_reg, s8, yes, no_op);
                      break;

                   case 3:
                      /* LDRSH rd, [rn + rm]! */
                      arm_access_memory(load, + reg[rm], half_reg, s16, yes, no_op);
                      break;
                }
             }
             else
             {
                /* MOVS rd, reg_op */
                arm_data_proc_logic_flags(reg_sh, reg);
             }
             break;

          case 0x1C:
             if((opcode & 0x90) == 0x90)
             {
                /* STRH rd, [rn + imm] */
                arm_access_memory(store, + offset, half_imm, u16, no, no_op);
             }
             else
             {
                /* BIC rd, rn, reg_op */
                arm_data_proc(reg[rn] & (~reg_sh), reg);
             }
             break;

          case 0x1D:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn + imm] */
                      arm_access_memory(load, + offset, half_imm, u16, no, no_op);
                      break;

                   case 2:
                      /* LDRSB rd, [rn + imm] */
                      arm_access_memory(load, + offset, half_imm, s8, no, no_op);
                      break;

                   case 3:
                      /* LDRSH rd, [rn + imm] */
                      arm_access_memory(load, + offset, half_imm, s16, no, no_op);
                      break;
                }
             }
             else
             {
                /* BICS rd, rn, reg_op */
                arm_data_proc_logic_flags(reg[rn] & (~reg_sh), reg);
             }
             break;

          case 0x1E:
             if((opcode & 0x90) == 0x90)
             {
                /* STRH rd, [rn + imm]! */
                arm_access_memory(store, + offset, half_imm, u16, yes, no_op);
             }
             else
             {
                /* MVN rd, reg_op */
                arm_data_proc(~reg_sh, reg);
             }
             break;

          case 0x1F:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn + imm]! */
                      arm_access_memory(load, + offset, half_imm, u16, yes, no_op);
                      break;

                   case 2:
                      /* LDRSB rd, [rn + imm]! */
                      arm_access_memory(load, + offset, half_imm, s8, yes, no_op);
                      break;

                   case 3:
                      /* LDRSH rd, [rn + imm]! */
                      arm_access_memory(load, + offset, half_imm, s16, yes, no_op);
                      break;
                }
             }
             else
             {
                /* MVNS rd, rn, reg_op */
                arm_data_proc_logic_flags(~reg_sh, reg);
             }
             break;

          case 0x20:
             /* AND rd, rn, imm */
             arm_data_proc(reg[rn] & imm, imm);
             break;

          case 0x21:
             /* ANDS rd, rn, imm */
             arm_data_proc_logic_flags(reg[rn] & imm, imm);
             break;

          case 0x22:
             /* EOR rd, rn, imm */
             arm_data_proc(reg[rn] ^ imm, imm);
             break;

          case 0x23:
             /* EORS rd, rn, imm */
             arm_data_proc_logic_flags(reg[rn] ^ imm, imm);
             break;

          case 0x24:
             /* SUB rd, rn, imm */
             arm_data_proc(reg[rn] - imm, imm);
             break;

          case 0x25:
             /* SUBS rd, rn, imm */
             arm_data_proc_sub_flags(reg[rn], imm, 1, imm);
             break;

          case 0x26:
             /* RSB rd, rn, imm */
             arm_data_proc(imm - reg[rn], imm);
             break;

          case 0x27:
             /* RSBS rd, rn, imm */
             arm_data_proc_sub_flags(imm, reg[rn], 1, imm);
             break;

          case 0x28:
             /* ADD rd, rn, imm */
             arm_data_proc(reg[rn] + imm, imm);
             break;

          case 0x29:
             /* ADDS rd, rn, imm */
             arm_data_proc_add_flags(reg[rn], imm, 0, imm);
             break;

          case 0x2A:
             /* ADC rd, rn, imm */
             arm_data_proc(reg[rn] + imm + c_flag, imm);
             break;

          case 0x2B:
             /* ADCS rd, rn, imm */
             arm_data_proc_add_flags(reg[rn], imm, c_flag, imm);
             break;

          case 0x2C:
             /* SBC rd, rn, imm */
             arm_data_proc(reg[rn] - imm + c_flag - 1, imm);
             break;

          case 0x2D:
             /* SBCS rd, rn, imm */
             arm_data_proc_sub_flags(reg[rn], imm, c_flag, imm);
             break;

          case 0x2E:
             /* RSC rd, rn, imm */
             arm_data_proc(imm - reg[rn] + c_flag - 1, imm);
             break;

          case 0x2F:
             /* RSCS rd, rn, imm */
             arm_data_proc_sub_flags(imm, reg[rn], c_flag, imm);
             break;

          case 0x30:
          case 0x31:
             /* TST rn, imm */
             arm_data_proc_test_logic(reg[rn] & imm, imm);
             break;

          case 0x32:
             /* MSR cpsr, imm */
             arm_psr(imm, store, cpsr);
             break;

          case 0x33:
             /* TEQ rn, imm */
             arm_data_proc_test_logic(reg[rn] ^ imm, imm);
             break;

          case 0x34:
          case 0x35:
             /* CMP rn, imm */
             arm_data_proc_test_sub(reg[rn], imm, imm);
             break;

          case 0x36:
             /* MSR spsr, imm */
             arm_psr(imm, store, spsr);
             break;

          case 0x37:
             /* CMN rn, imm */
             arm_data_proc_test_add(reg[rn], imm, imm);
             break;

          case 0x38:
             /* ORR rd, rn, imm */
             arm_data_proc(reg[rn] | imm, imm);
             break;

          case 0x39:
             /* ORRS rd, rn, imm */
             arm_data_proc_logic_flags(reg[rn] | imm, imm);
             break;

          case 0x3A:
             /* MOV rd, imm */
             arm_data_proc(imm, imm);
             break;

          case 0x3B:
             /* MOVS rd, imm */
             arm_data_proc_logic_flags(imm, imm);
             break;

          case 0x3C:
             /* BIC rd, rn, imm */
             arm_data_proc(reg[rn] & (~imm), imm);
             break;

          case 0x3D:
             /* BICS rd, rn, imm */
             arm_data_proc_logic_flags(reg[rn] & (~imm), imm);
             break;

          case 0x3E:
             /* MVN rd, imm */
             arm_data_proc(~imm, imm);
             break;

          case 0x3F:
             /* MVNS rd, imm */
             arm_data_proc_logic_flags(~imm, imm);
             break;

          case 0x40:
             /* STR rd, [rn], -imm */
             arm_access_memory(store, no_op, imm, u32, yes, - offset);
             break;

          case 0x41:
             /* LDR rd, [rn], -imm */
             arm_access_memory(load, no_op, imm, u32, yes, - offset);
             break;

          case 0x42:
             /* STRT rd, [rn], -imm */
             arm_access_memory(store, no_op, imm, u32, yes, - offset);
             break;

          case 0x43:
             /* LDRT rd, [rn], -imm */
             arm_access_memory(load, no_op, imm, u32, yes, - offset);
             break;

          case 0x44:
             /* STRB rd, [rn], -imm */
             arm_access_memory(store, no_op, imm, u8, yes, - offset);
             break;

          case 0x45:
             /* LDRB rd, [rn], -imm */
             arm_access_memory(load, no_op, imm, u8, yes, - offset);
             break;

          case 0x46:
             /* STRBT rd, [rn], -imm */
             arm_access_memory(store, no_op, imm, u8, yes, - offset);
             break;

          case 0x47:
             /* LDRBT rd, [rn], -imm */
             arm_access_memory(load, no_op, imm, u8, yes, - offset);
             break;

          case 0x48:
             /* STR rd, [rn], +imm */
             arm_access_memory(store, no_op, imm, u32, yes, + offset);
             break;

          case 0x49:
             /* LDR rd, [rn], +imm */
             arm_access_memory(load, no_op, imm, u32, yes, + offset);
             break;

          case 0x4A:
             /* STRT rd, [rn], +imm */
             arm_access_memory(store, no_op, imm, u32, yes, + offset);
             break;

          case 0x4B:
             /* LDRT rd, [rn], +imm */
             arm_access_memory(load, no_op, imm, u32, yes, + offset);
             break;

          case 0x4C:
             /* STRB rd, [rn], +imm */
             arm_access_memory(store, no_op, imm, u8, yes, + offset);
             break;

          case 0x4D:
             /* LDRB rd, [rn], +imm */
             arm_access_memory(load, no_op, imm, u8, yes, + offset);
             break;

          case 0x4E:
             /* STRBT rd, [rn], +imm */
             arm_access_memory(store, no_op, imm, u8, yes, + offset);
             break;

          case 0x4F:
             /* LDRBT rd, [rn], +imm */
             arm_access_memory(load, no_op, imm, u8, yes, + offset);
             break;

          case 0x50:
             /* STR rd, [rn - imm] */
             arm_access_memory(store, - offset, imm, u32, no, no_op);
             break;

          case 0x51:
             /* LDR rd, [rn - imm] */
             arm_access_memory(load, - offset, imm, u32, no, no_op);
             break;

          case 0x52:
             /* STR rd, [rn - imm]! */
             arm_access_memory(store, - offset, imm, u32, yes, no_op);
             break;

          case 0x53:
             /* LDR rd, [rn - imm]! */
             arm_access_memory(load, - offset, imm, u32, yes, no_op);
             break;

          case 0x54:
             /* STRB rd, [rn - imm] */
             arm_access_memory(store, - offset, imm, u8, no, no_op);
             break;

          case 0x55:
             /* LDRB rd, [rn - imm] */
             arm_access_memory(load, - offset, imm, u8, no, no_op);
             break;

          case 0x56:
             /* STRB rd, [rn - imm]! */
             arm_access_memory(store, - offset, imm, u8, yes, no_op);
             break;

          case 0x57:
             /* LDRB rd, [rn - imm]! */
             arm_access_memory(load, - offset, imm, u8, yes, no_op);
             break;

          case 0x58:
             /* STR rd, [rn + imm] */
             arm_access_memory(store, + offset, imm, u32, no, no_op);
             break;

          case 0x59:
             /* LDR rd, [rn + imm] */
             arm_access_memory(load, + offset, imm, u32, no, no_op);
             break;

          case 0x5A:
             /* STR rd, [rn + imm]! */
             arm_access_memory(store, + offset, imm, u32, yes, no_op);
             break;

          case 0x5B:
             /* LDR rd, [rn + imm]! */
             arm_access_memory(load, + offset, imm, u32, yes, no_op);
             break;

          case 0x5C:
             /* STRB rd, [rn + imm] */
             arm_access_memory(store, + offset, imm, u8, no, no_op);
             break;

          case 0x5D:
             /* LDRB rd, [rn + imm] */
             arm_access_memory(load, + offset, imm, u8, no, no_op);
             break;

          case 0x5E:
             /* STRB rd, [rn + imm]! */
             arm_access_memory(store, + offset, imm, u8, yes, no_op);
             break;

          case 0x5F:
             /* LDRBT rd, [rn + imm]! */
             arm_access_memory(load, + offset, imm, u8, yes, no_op);
             break;

          case 0x60:
             /* STR rd, [rn], -reg_op */
             arm_access_memory(store, no_op, reg, u32, yes, - reg_offset);
             break;

          case 0x61:
             /* LDR rd, [rn], -reg_op */
             arm_access_memory(load, no_op, reg, u32, yes, - reg_offset);
             break;

          case 0x62:
             /* STRT rd, [rn], -reg_op */
             arm_access_memory(store, no_op, reg, u32, yes, - reg_offset);
             break;

          case 0x63:
             /* LDRT rd, [rn], -reg_op */
             arm_access_memory(load, no_op, reg, u32, yes, - reg_offset);
             break;

          case 0x64:
             /* STRB rd, [rn], -reg_op */
             arm_access_memory(store, no_op, reg, u8, yes, - reg_offset);
             break;

          case 0x65:
             /* LDRB rd, [rn], -reg_op */
             arm_access_memory(load, no_op, reg, u8, yes, - reg_offset);
             break;

          case 0x66:
             /* STRBT rd, [rn], -reg_op */
             arm_access_memory(store, no_op, reg, u8, yes, - reg_offset);
             break;

          case 0x67:
             /* LDRBT rd, [rn], -reg_op */
             arm_access_memory(load, no_op, reg, u8, yes, - reg_offset);
             break;

          case 0x68:
             /* STR rd, [rn], +reg_op */
             arm_access_memory(store, no_op, reg, u32, yes, + reg_offset);
             break;

          case 0x69:
             /* LDR rd, [rn], +reg_op */
             arm_access_memory(load, no_op, reg, u32, yes, + reg_offset);
             break;

          case 0x6A:
             /* STRT rd, [rn], +reg_op */
             arm_access_memory(store, no_op, reg, u32, yes, + reg_offset);
             break;

          case 0x6B:
             /* LDRT rd, [rn], +reg_op */
             arm_access_memory(load, no_op, reg, u32, yes, + reg_offset);
             break;

          case 0x6C:
             /* STRB rd, [rn], +reg_op */
             arm_access_memory(store, no_op, reg, u8, yes, + reg_offset);
             break;

          case 0x6D:
             /* LDRB rd, [rn], +reg_op */
             arm_access_memory(load, no_op, reg, u8, yes, + reg_offset);
             break;

          case 0x6E:
             /* STRBT rd, [rn], +reg_op */
             arm_access_memory(store, no_op, reg, u8, yes, + reg_offset);
             break;

          case 0x6F:
             /* LDRBT rd, [rn], +reg_op */
             arm_access_memory(load, no_op, reg, u8, yes, + reg_offset);
             break;

          case 0x70:
             /* STR rd, [rn - reg_op] */
             arm_access_memory(store, - reg_offset, reg, u32, no, no_op);
             break;

          case 0x71:
             /* LDR rd, [rn - reg_op] */
             arm_access_memory(load, - reg_offset, reg, u32, no, no_op);
             break;

          case 0x72:
             /* STR rd, [rn - reg_op]! */
             arm_access_memory(store, - reg_offset, reg, u32, yes, no_op);
             break;

          case 0x73:
             /* LDR rd, [rn - reg_op]! */
             arm_access_memory(load, - reg_offset, reg, u32, yes, no_op);
             break;

          case 0x74:
             /* STRB rd, [rn - reg_op] */
             arm_access_memory(store, - reg_offset, reg, u8, no, no_op);
             break;

          case 0x75:
             /* LDRB rd, [rn - reg_op] */
             arm_access_memory(load, - reg_offset, reg, u8, no, no_op);
             break;

          case 0x76:
             /* STRB rd, [rn - reg_op]! */
             arm_access_memory(store, - reg_offset, reg, u8, yes, no_op);
             break;

          case 0x77:
             /* LDRB rd, [rn - reg_op]! */
             arm_access_memory(load, - reg_offset, reg, u8, yes, no_op);
             break;

          case 0x78:
             /* STR rd, [rn + reg_op] */
             arm_access_memory(store, + reg_offset, reg, u32, no, no_op);
             break;

          case 0x79:
             /* LDR rd, [rn + reg_op] */
             arm_access_memory(load, + reg_offset, reg, u32, no, no_op);
             break;

          case 0x7A:
             /* STR rd, [rn + reg_op]! */
             arm_access_memory(store, + reg_offset, reg, u32, yes, no_op);
             break;

          case 0x7B:
             /* LDR rd, [rn + reg_op]! */
             arm_access_memory(load, + reg_offset, reg, u32, yes, no_op);
             break;

          case 0x7C:
             /* STRB rd, [rn + reg_op] */
             arm_access_memory(store, + reg_offset, reg, u8, no, no_op);
             break;

          case 0x7D:
             /* LDRB rd, [rn + reg_op] */
             arm_access_memory(load, + reg_offset, reg, u8, no, no_op);
             break;

          case 0x7E:
             /* STRB rd, [rn + reg_op]! */
             arm_access_memory(store, + reg_offset, reg, u8, yes, no_op);
             break;

          case 0x7F:
             /* LDRBT rd, [rn + reg_op]! */
             arm_access_memory(load, + reg_offset, reg, u8, yes, no_op);
             break;

          /* STM instructions: STMDA, STMIA, STMDB, STMIB */

          case 0x80:   /* STMDA rn, rlist */
            cpu_alert |= exec_arm_block_mem<AccStore, false, false, AddrPostDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x88:   /* STMIA rn, rlist */
            cpu_alert |= exec_arm_block_mem<AccStore, false, false, AddrPostInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x90:   /* STMDB rn, rlist */
            cpu_alert |= exec_arm_block_mem<AccStore, false, false, AddrPreDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x98:   /* STMIB rn, rlist */
            cpu_alert |= exec_arm_block_mem<AccStore, false, false, AddrPreInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;

          case 0x82:   /* STMDA rn!, rlist */
            cpu_alert |= exec_arm_block_mem<AccStore, true, false, AddrPostDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x8A:   /* STMIA rn!, rlist */
            cpu_alert |= exec_arm_block_mem<AccStore, true, false, AddrPostInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x92:   /* STMDB rn!, rlist */
            cpu_alert |= exec_arm_block_mem<AccStore, true, false, AddrPreDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x9A:   /* STMIB rn!, rlist */
            cpu_alert |= exec_arm_block_mem<AccStore, true, false, AddrPreInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;

          case 0x84:   /* STMDA rn, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccStore, false, true, AddrPostDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x8C:   /* STMIA rn, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccStore, false, true, AddrPostInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x94:   /* STMDB rn, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccStore, false, true, AddrPreDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x9C:   /* STMIB rn, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccStore, false, true, AddrPreInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;

          case 0x86:   /* STMDA rn!, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccStore, true, true, AddrPostDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x8E:   /* STMIA rn!, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccStore, true, true, AddrPostInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x96:   /* STMDB rn!, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccStore, true, true, AddrPreDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x9E:   /* STMIB rn!, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccStore, true, true, AddrPreInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;


          /* LDM instructions: LDMDA, LDMIA, LDMDB, LDMIB */

          case 0x81:   /* LDMDA rn, rlist */
            cpu_alert |= exec_arm_block_mem<AccLoad, false, false, AddrPostDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x89:   /* LDMIA rn, rlist */
            cpu_alert |= exec_arm_block_mem<AccLoad, false, false, AddrPostInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x91:   /* LDMDB rn, rlist */
            cpu_alert |= exec_arm_block_mem<AccLoad, false, false, AddrPreDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x99:   /* LDMIB rn, rlist */
            cpu_alert |= exec_arm_block_mem<AccLoad, false, false, AddrPreInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;

          case 0x83:   /* LDMDA rn!, rlist */
            cpu_alert |= exec_arm_block_mem<AccLoad, true, false, AddrPostDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x8B:   /* LDMIA rn!, rlist */
            cpu_alert |= exec_arm_block_mem<AccLoad, true, false, AddrPostInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x93:   /* LDMDB rn!, rlist */
            cpu_alert |= exec_arm_block_mem<AccLoad, true, false, AddrPreDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x9B:   /* LDMIB rn!, rlist */
            cpu_alert |= exec_arm_block_mem<AccLoad, true, false, AddrPreInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;

          case 0x85:   /* LDMDA rn, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccLoad, false, true, AddrPostDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            arm_spsr_restore_ldm_check();
            break;
          case 0x8D:   /* LDMIA rn, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccLoad, false, true, AddrPostInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            arm_spsr_restore_ldm_check();
            break;
          case 0x95:   /* LDMDB rn, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccLoad, false, true, AddrPreDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            arm_spsr_restore_ldm_check();
            break;
          case 0x9D:   /* LDMIB rn, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccLoad, false, true, AddrPreInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            arm_spsr_restore_ldm_check();
            break;

          case 0x87:   /* LDMDA rn!, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccLoad, true, true, AddrPostDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            arm_spsr_restore_ldm_check();
            break;
          case 0x8F:   /* LDMIA rn!, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccLoad, true, true, AddrPostInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            arm_spsr_restore_ldm_check();
            break;
          case 0x97:   /* LDMDB rn!, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccLoad, true, true, AddrPreDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            arm_spsr_restore_ldm_check();
            break;
          case 0x9F:   /* LDMIB rn!, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccLoad, true, true, AddrPreInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            arm_spsr_restore_ldm_check();
            break;


          case 0xA0 ... 0xAF:
             {
                /* B offset */
                arm_decode_branch();
                reg[REG_PC] += offset + 8;
                cycles_remaining -= ws_cyc_nseq[reg[REG_PC] >> 24][1];
                break;
             }

          case 0xB0 ... 0xBF:
             {
                /* BL offset */
                arm_decode_branch();
                reg[REG_LR] = reg[REG_PC] + 4;
                reg[REG_PC] += offset + 8;
                cycles_remaining -= ws_cyc_nseq[reg[REG_PC] >> 24][1];
                break;
             }

#ifdef HAVE_UNUSED
          case 0xC0 ... 0xEF:
             /* coprocessor instructions, reserved on GBA */
             break;
#endif

          case 0xF0 ... 0xFF:
            {
            u32 swinum = (opcode >> 16) & 0xFF;
            gba_profile_arm_swi(swinum);
            if(gba_execute_swi_hle(swinum, cpu_alert))
            {
              arm_pc_offset(4);
              cycles_remaining -= 64;
              break;
            }
            collapse_flags();
            reg[REG_BUS_VALUE] = 0xe3a02004;  // After SWI, we read bios[0xE4]
            REG_MODE(MODE_SUPERVISOR)[6] = reg[REG_PC] + 4;
            REG_SPSR(MODE_SUPERVISOR) = reg[REG_CPSR];
            reg[REG_PC] = 0x00000008;
            // Move to ARM mode, Supervisor mode and disable IRQs
            reg[REG_CPSR] = (reg[REG_CPSR] & ~0x3F) | 0x13 | 0x80;
            set_cpu_mode(MODE_SUPERVISOR);
            break;
            }
       }

skip_instruction:

       /* End of Execute ARM instruction */
       cycles_remaining -= ws_cyc_seq[(reg[REG_PC] >> 24) & 0xF][1];

       if (reg[REG_PC] == idle_loop_target_pc && cycles_remaining > 0) cycles_remaining = 0;

       if (cpu_alert & (CPU_ALERT_HALT | CPU_ALERT_IRQ))
         goto alert;

    } while(cycles_remaining > 0);

    collapse_flags();
    update_ret = update_gba(cycles_remaining);
    gba_execute_arm_updates++;
    gba_execute_last_update_ret = update_ret;
    gba_execute_last_pc = reg[REG_PC];
    gba_execute_last_cpsr = reg[REG_CPSR];
    gba_execute_last_halt = reg[CPU_HALT_STATE];
    if (completed_frame(update_ret))
       return;
    cycles_remaining = cycles_to_run(update_ret);
    continue;

    do
    {
thumb_loop:

       /* Process cheats if we are about to execute the cheat hook */
       if (reg[REG_PC] == cheat_master_hook)
       {
          collapse_flags();
          process_cheats();
       }

       /* Execute THUMB instruction */

       using_instruction(thumb);
       check_pc_region();
       reg[REG_PC] &= ~0x01;
       bool opcode_prefetched = false;
       if(pc_address_block)
       {
          gba_block_cache_touch_thumb(reg[REG_PC], pc_address_block);
          opcode = readaddress16(pc_address_block, (reg[REG_PC] & 0x7FFF));
          opcode_prefetched = true;
       }

       if(opcode_prefetched && gba_p4_thumb_jit_can_start(opcode, pc_address_block))
       {
          int jit_ops = gba_p4_thumb_jit_try(pc_address_block, cycles_remaining,
             n_flag, z_flag, c_flag, v_flag);
          if(jit_ops > 0)
          {
             u32 jit_ret = (u32)jit_ops;
             bool jit_arm_switch = (jit_ret & GBA_P4_THUMB_JIT_RET_ARM_SWITCH) != 0;
             u32 jit_extra = (jit_ret >> GBA_P4_THUMB_JIT_RET_EXTRA_SHIFT) &
                GBA_P4_THUMB_JIT_RET_EXTRA_MASK;
             jit_ops = (int)(jit_ret & GBA_P4_THUMB_JIT_RET_OPS_MASK);
             cycles_remaining -= ws_cyc_seq[(reg[REG_PC] >> 24) & 0xF][0] * jit_ops;
             if(jit_extra)
                cycles_remaining -= ws_cyc_nseq[(reg[REG_PC] >> 24) & 0xF][0] * jit_extra;
             if(jit_arm_switch)
             {
                collapse_flags();
                goto arm_loop;
             }
             if (reg[REG_PC] == idle_loop_target_pc && cycles_remaining > 0) cycles_remaining = 0;
             if (cpu_alert & (CPU_ALERT_HALT | CPU_ALERT_IRQ))
               goto alert;
             continue;
          }
       }

       bool fast_dispatch_already_missed = false;
       bool opcode_already_profiled = false;
#if !defined(TRACE_INSTRUCTIONS) && !defined(REGISTER_USAGE_ANALYZE) && !GBA_DECODED_BLOCK_CACHE
       if(gba_thumb_batch_enabled && opcode_prefetched && cycles_remaining >= 32)
       {
          constexpr u32 GBA_THUMB_BATCH_MAX = 16;
          const u32 batch_pc_region = reg[REG_PC] >> 15;
          u32 batch_ops = 0;

          while(batch_ops < GBA_THUMB_BATCH_MAX)
          {
             u32 batch_pc = reg[REG_PC];
             gba_thumb_profile_opcode(opcode);
             opcode_already_profiled = true;
             int fast_result = gba_thumb_execute_fast_dispatch(opcode, n_flag,
                z_flag, c_flag, v_flag, cpu_alert, cycles_remaining);
             if(!fast_result)
             {
                fast_dispatch_already_missed = true;
                break;
             }

             batch_ops++;
             if(fast_result == 2)
             {
                gba_thumb_batch_runs++;
                gba_thumb_batch_ops += batch_ops;
                collapse_flags();
                goto arm_loop;
             }

             cycles_remaining -= ws_cyc_seq[(reg[REG_PC] >> 24) & 0xF][0];
             if(reg[REG_PC] == idle_loop_target_pc && cycles_remaining > 0)
                cycles_remaining = 0;
             if(cpu_alert & (CPU_ALERT_HALT | CPU_ALERT_IRQ))
             {
                gba_thumb_batch_runs++;
                gba_thumb_batch_ops += batch_ops;
                goto alert;
             }

             // A changed PC is a real basic-block boundary. Return to the outer
             // loop there so the JIT sees stable branch targets instead of every
             // interior instruction in a straight-line sequence.
             if(cycles_remaining <= 0 || reg[REG_PC] != batch_pc + 2 ||
                (reg[REG_PC] >> 15) != batch_pc_region ||
                reg[REG_PC] == cheat_master_hook ||
                batch_ops == GBA_THUMB_BATCH_MAX)
                break;

             opcode = readaddress16(pc_address_block, reg[REG_PC] & 0x7FFF);
             opcode_already_profiled = false;
          }

          if(batch_ops)
          {
             gba_thumb_batch_runs++;
             gba_thumb_batch_ops += batch_ops;
             if(!fast_dispatch_already_missed)
                continue;
          }
       }
#endif

       if(!pc_address_block)
       {
          gba_bad_pc_count++;
          gba_bad_pc_last = reg[REG_PC];
          gba_bad_pc_last_cpsr = reg[REG_CPSR];
          if(gba_thumb_jit_runtime_enabled || gba_thumb_batch_enabled)
            gba_p4_thumb_jit_report_fault(0x42414454U, reg[REG_PC]);
          opcode = (reg[REG_BUS_VALUE] >> ((reg[REG_PC] & 0x02) << 3)) & 0xFFFF;
       }
       else if(!opcode_prefetched)
          opcode = readaddress16(pc_address_block, (reg[REG_PC] & 0x7FFF));
       if(!opcode_already_profiled)
          gba_thumb_profile_opcode(opcode);

       #ifdef TRACE_INSTRUCTIONS
       interp_trace_instruction(reg[REG_PC], 0);
       #endif

       if(interp_fast && !fast_dispatch_already_missed)
       {
          int fast_result = gba_thumb_execute_fast_dispatch(opcode, n_flag, z_flag,
             c_flag, v_flag, cpu_alert, cycles_remaining);
          if(fast_result == 1)
             goto thumb_instruction_done;
          if(fast_result == 2)
          {
             collapse_flags();
             goto arm_loop;
          }
       }

       gba_thumb_profile_fallback(opcode);

       switch((opcode >> 8) & 0xFF)
       {
          case 0x00 ... 0x07:
             /* LSL rd, rs, offset */
             thumb_shift(shift, lsl, imm);
             break;

          case 0x08 ... 0x0F:
             /* LSR rd, rs, offset */
             thumb_shift(shift, lsr, imm);
             break;

          case 0x10 ... 0x17:
             /* ASR rd, rs, offset */
             thumb_shift(shift, asr, imm);
             break;

          case 0x18:
          case 0x19:
             /* ADD rd, rs, rn */
             thumb_add(add_sub, rd, reg[rs], reg[rn], 0);
             break;

          case 0x1A:
          case 0x1B:
             /* SUB rd, rs, rn */
             thumb_sub(add_sub, rd, reg[rs], reg[rn], 1);
             break;

          case 0x1C:
          case 0x1D:
             /* ADD rd, rs, imm */
             thumb_add(add_sub_imm, rd, reg[rs], imm, 0);
             break;

          case 0x1E:
          case 0x1F:
             /* SUB rd, rs, imm */
             thumb_sub(add_sub_imm, rd, reg[rs], imm, 1);
             break;

          case 0x20 ... 0x27:
             /* MOV r0..7, imm */
             thumb_logic(imm, ((opcode >> 8) & 7), imm);
             break;

          case 0x28 ... 0x2F:
             /* CMP r0..7, imm */
             thumb_test_sub(imm, reg[(opcode >> 8) & 7], imm);
             break;

          case 0x30 ... 0x37:
             /* ADD r0..7, imm */
             thumb_add(imm, ((opcode >> 8) & 7), reg[(opcode >> 8) & 7], imm, 0);
             break;

          case 0x38 ... 0x3F:
             /* SUB r0..7, imm */
             thumb_sub(imm, ((opcode >> 8) & 7), reg[(opcode >> 8) & 7], imm, 1);
             break;

          case 0x40:
             switch((opcode >> 6) & 0x03)
             {
                case 0x00:
                   /* AND rd, rs */
                   thumb_logic(alu_op, rd, reg[rd] & reg[rs]);
                   break;

                case 0x01:
                   /* EOR rd, rs */
                   thumb_logic(alu_op, rd, reg[rd] ^ reg[rs]);
                   break;

                case 0x02:
                   /* LSL rd, rs */
                   thumb_shift(alu_op, lsl, reg);
                   break;

                case 0x03:
                   /* LSR rd, rs */
                   thumb_shift(alu_op, lsr, reg);
                   break;
             }
             break;

          case 0x41:
             switch((opcode >> 6) & 0x03)
             {
                case 0x00:
                   /* ASR rd, rs */
                   thumb_shift(alu_op, asr, reg);
                   break;

                case 0x01:
                   /* ADC rd, rs */
                   thumb_add(alu_op, rd, reg[rd], reg[rs], c_flag);
                   break;

                case 0x02:
                   /* SBC rd, rs */
                   thumb_sub(alu_op, rd, reg[rd], reg[rs], c_flag);
                   break;

                case 0x03:
                   /* ROR rd, rs */
                   thumb_shift(alu_op, ror, reg);
                   break;
             }
             break;

          case 0x42:
             switch((opcode >> 6) & 0x03)
             {
                case 0x00:
                   /* TST rd, rs */
                   thumb_test_logic(alu_op, reg[rd] & reg[rs]);
                   break;

                case 0x01:
                   /* NEG rd, rs */
                   thumb_sub(alu_op, rd, 0, reg[rs], 1);
                   break;

                case 0x02:
                   /* CMP rd, rs */
                   thumb_test_sub(alu_op, reg[rd], reg[rs]);
                   break;

                case 0x03:
                   /* CMN rd, rs */
                   thumb_test_add(alu_op, reg[rd], reg[rs]);
                   break;
             }
             break;

          case 0x43:
             switch((opcode >> 6) & 0x03)
             {
                case 0x00:
                   /* ORR rd, rs */
                   thumb_logic(alu_op, rd, reg[rd] | reg[rs]);
                   break;

                case 0x01:
                   /* MUL rd, rs */
                   thumb_logic(alu_op, rd, reg[rd] * reg[rs]);
                   break;

                case 0x02:
                   /* BIC rd, rs */
                   thumb_logic(alu_op, rd, reg[rd] & (~reg[rs]));
                   break;

                case 0x03:
                   /* MVN rd, rs */
                   thumb_logic(alu_op, rd, ~reg[rs]);
                   break;
             }
             break;

          case 0x44:
             /* ADD rd, rs */
             thumb_hireg_op(reg[rd] + reg[rs]);
             break;

          case 0x45:
             /* CMP rd, rs */
             {
                thumb_pc_offset(4);
                thumb_decode_hireg_op();
                u32 _sa = reg[rd];
                u32 _sb = reg[rs];
                u32 dest = _sa - _sb;
                thumb_pc_offset(-2);
                calculate_flags_sub(dest, _sa, _sb, 1);
             }
             break;

          case 0x46:
             /* MOV rd, rs */
             thumb_hireg_op(reg[rs]);
             break;

          case 0x47:
             /* BX rs */
             {
                thumb_decode_hireg_op();
                u32 src;
                thumb_pc_offset(4);
                src = reg[rs];
                if(src & 0x01)
                {
                   reg[REG_PC] = src - 1;
                }
                else
                {
                   /* Switch to ARM mode */
                   reg[REG_PC] = src;
                   reg[REG_CPSR] &= ~0x20;
                   collapse_flags();
                   goto arm_loop;
                }
             }
             break;

          case 0x48 ... 0x4F:
             /* LDR r0..7, [pc + imm] */
             thumb_access_memory(load, imm, ((reg[REG_PC] - 2) & ~2) + (imm * 4) + 4, reg[(opcode >> 8) & 7], u32);
             break;

          case 0x50:
          case 0x51:
             /* STR rd, [rb + ro] */
             thumb_access_memory(store, mem_reg, reg[rb] + reg[ro], reg[rd], u32);
             break;

          case 0x52:
          case 0x53:
             /* STRH rd, [rb + ro] */
             thumb_access_memory(store, mem_reg, reg[rb] + reg[ro], reg[rd], u16);
             break;

          case 0x54:
          case 0x55:
             /* STRB rd, [rb + ro] */
             thumb_access_memory(store, mem_reg, reg[rb] + reg[ro], reg[rd], u8);
             break;

          case 0x56:
          case 0x57:
             /* LDSB rd, [rb + ro] */
             thumb_access_memory(load, mem_reg, reg[rb] + reg[ro], reg[rd], s8);
             break;

          case 0x58:
          case 0x59:
             /* LDR rd, [rb + ro] */
             thumb_access_memory(load, mem_reg, reg[rb] + reg[ro], reg[rd], u32);
             break;

          case 0x5A:
          case 0x5B:
             /* LDRH rd, [rb + ro] */
             thumb_access_memory(load, mem_reg, reg[rb] + reg[ro], reg[rd], u16);
             break;

          case 0x5C:
          case 0x5D:
             /* LDRB rd, [rb + ro] */
             thumb_access_memory(load, mem_reg, reg[rb] + reg[ro], reg[rd], u8);
             break;

          case 0x5E:
          case 0x5F:
             /* LDSH rd, [rb + ro] */
             thumb_access_memory(load, mem_reg, reg[rb] + reg[ro], reg[rd], s16);
             break;

          case 0x60 ... 0x67:
             /* STR rd, [rb + imm] */
             thumb_access_memory(store, mem_imm, reg[rb] + (imm * 4), reg[rd], u32);
             break;

          case 0x68 ... 0x6F:
             /* LDR rd, [rb + imm] */
             thumb_access_memory(load, mem_imm, reg[rb] + (imm * 4), reg[rd], u32);
             break;

          case 0x70 ... 0x77:
             /* STRB rd, [rb + imm] */
             thumb_access_memory(store, mem_imm, reg[rb] + imm, reg[rd], u8);
             break;

          case 0x78 ... 0x7F:
             /* LDRB rd, [rb + imm] */
             thumb_access_memory(load, mem_imm, reg[rb] + imm, reg[rd], u8);
             break;

          case 0x80 ... 0x87:
             /* STRH rd, [rb + imm] */
             thumb_access_memory(store, mem_imm, reg[rb] + (imm * 2), reg[rd], u16);
             break;

          case 0x88 ... 0x8F:
             /* LDRH rd, [rb + imm] */
             thumb_access_memory(load, mem_imm, reg[rb] + (imm * 2), reg[rd], u16);
             break;

          case 0x90 ... 0x97:
             /* STR r0..7, [sp + imm] */
             thumb_access_memory(store, imm, reg[REG_SP] + (imm * 4), reg[(opcode >> 8) & 7], u32);
             break;

          case 0x98 ... 0x9F:
             /* LDR r0..7, [sp + imm] */
             thumb_access_memory(load, imm, reg[REG_SP] + (imm * 4), reg[(opcode >> 8) & 7], u32);
             break;

          case 0xA0 ... 0xA7:
             /* ADD r0..7, pc, +imm */
             thumb_add_noflags(imm, ((opcode >> 8) & 7), (reg[REG_PC] & ~2) + 4, (imm * 4));
             break;

          case 0xA8 ... 0xAF:
             /* ADD r0..7, sp, +imm */
             thumb_add_noflags(imm, ((opcode >> 8) & 7), reg[REG_SP], (imm * 4));
             break;

          case 0xB0:
          case 0xB1:
          case 0xB2:
          case 0xB3:
             if((opcode >> 7) & 0x01)
             {
                /* ADD sp, -imm */
                thumb_add_noflags(add_sp, 13, reg[REG_SP], -(imm * 4));
             }
             else
             {
                /* ADD sp, +imm */
                thumb_add_noflags(add_sp, 13, reg[REG_SP], (imm * 4));
             }
             break;

          case 0xB4:  /* PUSH rlist */
             cpu_alert |= exec_thumb_block_mem<AccStore, AddrPreDec>(
               REG_SP, opcode & 0xFF, cycles_remaining);
             break;

          case 0xB5:  /* PUSH rlist, lr */
             cpu_alert |= exec_thumb_block_mem<AccStore, AddrPreDec>(
               REG_SP, (opcode & 0xFF) | (1 << REG_LR), cycles_remaining);
             break;

          case 0xBC:  /* POP rlist */
             cpu_alert |= exec_thumb_block_mem<AccLoad, AddrPostInc>(
               REG_SP, opcode & 0xFF, cycles_remaining);
             break;

          case 0xBD:  /* POP rlist, pc */
             cpu_alert |= exec_thumb_block_mem<AccLoad, AddrPostInc>(
               REG_SP, (opcode & 0xFF) | (1 << REG_PC), cycles_remaining);
             break;

          case 0xC0 ... 0xC7:    /* STMIA r0..7!, rlist */
             cpu_alert |= exec_thumb_block_mem<AccStore, AddrPostInc>(
               (opcode >> 8) & 7, (opcode & 0xFF), cycles_remaining);
             break;

          case 0xC8 ... 0xCF:    /* LDMIA r0..7!, rlist */
             cpu_alert |= exec_thumb_block_mem<AccLoad, AddrPostInc>(
               (opcode >> 8) & 7, (opcode & 0xFF), cycles_remaining);
             break;

          case 0xD0:   /* BEQ label */
             thumb_conditional_branch(z_flag == 1);
             break;
          case 0xD1:   /* BNE label */
             thumb_conditional_branch(z_flag == 0);
             break;
          case 0xD2:   /* BCS label */
             thumb_conditional_branch(c_flag == 1);
             break;
          case 0xD3:   /* BCC label */
             thumb_conditional_branch(c_flag == 0);
             break;
          case 0xD4:   /* BMI label */
             thumb_conditional_branch(n_flag == 1);
             break;
          case 0xD5:   /* BPL label */
             thumb_conditional_branch(n_flag == 0);
             break;
          case 0xD6:   /* BVS label */
             thumb_conditional_branch(v_flag == 1);
             break;
          case 0xD7:   /* BVC label */
             thumb_conditional_branch(v_flag == 0);
             break;
          case 0xD8:   /* BHI label */
             thumb_conditional_branch(c_flag & (z_flag ^ 1));
             break;
          case 0xD9:   /* BLS label */
             thumb_conditional_branch((c_flag == 0) | z_flag);
             break;
          case 0xDA:   /* BGE label */
             thumb_conditional_branch(n_flag == v_flag);
             break;
          case 0xDB:   /* BLT label */
             thumb_conditional_branch(n_flag != v_flag);
             break;
          case 0xDC:   /* BGT label */
             thumb_conditional_branch((z_flag == 0) & (n_flag == v_flag));
             break;
          case 0xDD:   /* BLE label */
             thumb_conditional_branch(z_flag | (n_flag != v_flag));
             break;

          case 0xDF:
             {
             u32 swinum = opcode & 0xFF;
             gba_profile_thumb_swi(swinum);
             if(gba_execute_swi_hle(swinum, cpu_alert))
             {
                thumb_pc_offset(2);
                cycles_remaining -= 64;
                break;
             }
             collapse_flags();
             REG_MODE(MODE_SUPERVISOR)[6] = reg[REG_PC] + 2;
             REG_SPSR(MODE_SUPERVISOR) = reg[REG_CPSR];
             reg[REG_PC] = 0x00000008;
             // Move to ARM mode, Supervisor mode and disable IRQs
             reg[REG_CPSR] = (reg[REG_CPSR] & ~0x3F) | 0x13 | 0x80;
             set_cpu_mode(MODE_SUPERVISOR);
             reg[REG_BUS_VALUE] = 0xe3a02004;  // After SWI, we read bios[0xE4]
             goto arm_loop;
             break;
             }

          case 0xE0 ... 0xE7:
             {
                /* B label */
                thumb_decode_branch();
                s32 br_offset = ((s32)(offset << 21) >> 20) + 4;
                reg[REG_PC] += br_offset;
                cycles_remaining -= ws_cyc_nseq[reg[REG_PC] >> 24][0];
                break;
             }

          case 0xF0 ... 0xF7:
             {
                /* (low word) BL label */
                thumb_decode_branch();
                reg[REG_LR] = reg[REG_PC] + 4 + ((s32)(offset << 21) >> 9);
                thumb_pc_offset(2);
                break;
             }

          case 0xF8 ... 0xFF:
             {
                /* (high word) BL label */
                thumb_decode_branch();
                u32 newlr = (reg[REG_PC] + 2) | 0x01;
                u32 newpc = reg[REG_LR] + (offset * 2);
                reg[REG_LR] = newlr;
                reg[REG_PC] = newpc;
                cycles_remaining -= ws_cyc_nseq[newpc >> 24][0];
                break;
             }
       }

       /* End of Execute THUMB instruction */
thumb_instruction_done:
       cycles_remaining -= ws_cyc_seq[(reg[REG_PC] >> 24) & 0xF][0];

       if (reg[REG_PC] == idle_loop_target_pc && cycles_remaining > 0) cycles_remaining = 0;

       if (cpu_alert & (CPU_ALERT_HALT | CPU_ALERT_IRQ))
          goto alert;

    } while(cycles_remaining > 0);

    collapse_flags();
    update_ret = update_gba(cycles_remaining);
    gba_execute_thumb_updates++;
    gba_execute_last_update_ret = update_ret;
    gba_execute_last_pc = reg[REG_PC];
    gba_execute_last_cpsr = reg[REG_CPSR];
    gba_execute_last_halt = reg[CPU_HALT_STATE];
    if (completed_frame(update_ret))
       return;
    cycles_remaining = cycles_to_run(update_ret);
    continue;

    alert:
      /* CPU stopped or switch to IRQ handler */
      collapse_flags();
  }
}

void init_cpu(void)
{
  // Initialize CPU registers
  memset(reg, 0, REG_USERDEF * sizeof(u32));
  memset(reg_mode, 0, sizeof(reg_mode));
  for (u32 i = 0; i < sizeof(spsr)/sizeof(spsr[0]); i++)
    spsr[i] = 0x00000010;

  reg[CPU_HALT_STATE] = CPU_ACTIVE;
  reg[REG_SLEEP_CYCLES] = 0;

  if (selected_boot_mode == boot_game) {
    reg[REG_SP] = 0x03007F00;
    reg[REG_PC] = 0x08000000;
    reg[REG_CPSR] = 0x0000001F;   // system mode
    reg[CPU_MODE] = MODE_SYSTEM;
  } else {
    reg[REG_SP] = 0x03007F00;
    reg[REG_PC] = 0x00000000;
    reg[REG_CPSR] = 0x00000013 | 0xC0;  // supervisor
    reg[CPU_MODE] = MODE_SUPERVISOR;
  }

  // Stack pointers are set by BIOS, we set them
  // nevertheless, should we not boot from BIOS
  REG_MODE(MODE_USER)[5] = 0x03007F00;
  REG_MODE(MODE_IRQ)[5] = 0x03007FA0;
  REG_MODE(MODE_FIQ)[5] = 0x03007FA0;
  REG_MODE(MODE_SUPERVISOR)[5] = 0x03007FE0;
}

bool cpu_check_savestate(const u8 *src)
{
  const u8 *cpudoc = bson_find_key(src, "cpu");
  if (!cpudoc)
    return false;

  return bson_contains_key(cpudoc, "bus-value", BSON_TYPE_INT32) &&
         bson_contains_key(cpudoc, "regs", BSON_TYPE_ARR) &&
         bson_contains_key(cpudoc, "spsr", BSON_TYPE_ARR) &&
         bson_contains_key(cpudoc, "regmod", BSON_TYPE_ARR);
}


bool cpu_read_savestate(const u8 *src)
{
  const u8 *cpudoc = bson_find_key(src, "cpu");
  return bson_read_int32(cpudoc, "bus-value", &reg[REG_BUS_VALUE]) &&
         bson_read_int32_array(cpudoc, "regs", reg, REG_ARCH_COUNT) &&
         bson_read_int32_array(cpudoc, "spsr", spsr, 6) &&
         bson_read_int32_array(cpudoc, "regmod", (u32*)reg_mode, 7*7);
}

unsigned cpu_write_savestate(u8 *dst)
{
  u8 *wbptr, *startp = dst;
  bson_start_document(dst, "cpu", wbptr);
  bson_write_int32array(dst, "regs", reg, REG_ARCH_COUNT);
  bson_write_int32array(dst, "spsr", spsr, 6);
  bson_write_int32array(dst, "regmod", reg_mode, 7*7);
  bson_write_int32(dst, "bus-value", reg[REG_BUS_VALUE]);

  bson_finish_document(dst, wbptr);
  return (unsigned int)(dst - startp);
}


