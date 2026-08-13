#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  DOLL_GBA_WIDTH = 240,
  DOLL_GBA_HEIGHT = 160,
  DOLL_GBA_FRAME_BYTES = 240 * 161 * 2,
  DOLL_GBA_SAVE_BYTES = 128 * 1024,
  DOLL_GBA_STATE_BYTES = 416 * 1024,
  DOLL_GBA_SOUND_FREQUENCY = 32768,
};

enum {
  DOLL_GBA_CPU_SAFE = 0,
  DOLL_GBA_CPU_BATCH = 1,
  DOLL_GBA_CPU_JIT_DEBUG = 2,
  DOLL_GBA_CPU_TURBO = 3,
  DOLL_GBA_CPU_MODE_COUNT = 4,
};

typedef struct {
  uint32_t jit_bytes;
  uint32_t jit_hits;
  uint32_t jit_misses;
  uint32_t jit_compiles;
  uint32_t jit_attempts;
  uint32_t jit_disabled;
  uint32_t jit_ops;
  uint32_t jit_used_bytes;
  uint32_t jit_arena_full;
  uint32_t jit_short_blocks;
  uint32_t jit_reject_hits;
  uint32_t jit_hot_waits;
  uint32_t jit_reuses;
  uint32_t jit_adapt_probes;
  uint32_t jit_top_break;
  uint32_t jit_top_break_count;
  uint32_t thumb_batch_runs;
  uint32_t thumb_batch_ops;
  uint32_t vram_internal;
  uint32_t cpu_mode;
  uint32_t softreset_count;
  uint32_t bad_pc_count;
  uint32_t jit_guard_trips;
  uint32_t jit_last_pc;
  uint32_t jit_last_end_pc;
  uint32_t jit_last_ret;
  uint32_t jit_last_signature;
  uint32_t rom_page_loads;
  uint32_t rom_page_prefetches;
  uint32_t arm_updates;
  uint32_t thumb_updates;
  uint32_t halt_updates;
  uint32_t last_pc;
  uint32_t last_cpsr;
} doll_gba_perf_stats_t;

bool doll_gba_core_begin(uint16_t* framebuffer);
bool doll_gba_core_load(const char* rom_path);
void doll_gba_core_stop(void);
void doll_gba_core_run(uint16_t buttons, bool draw);
void doll_gba_core_set_cpu_mode(uint32_t mode);
uint32_t doll_gba_core_get_cpu_mode(void);
uint32_t doll_gba_core_read_audio(int16_t* stereo, uint32_t frames);
void* doll_gba_core_save_data(void);
bool doll_gba_core_save_state(void* output, size_t size);
bool doll_gba_core_load_state(const void* input, size_t size);
void doll_gba_core_get_perf(doll_gba_perf_stats_t* stats);

#ifdef __cplusplus
}
#endif
