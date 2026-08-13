#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef DOLL_GBA_VERBOSE_DIAGNOSTICS
#define DOLL_GBA_VERBOSE_DIAGNOSTICS 0
#endif

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
  DOLL_GBA_CPU_JIT_ISOLATED = 2,
  DOLL_GBA_CPU_TURBO = 3,
  DOLL_GBA_CPU_FAST_ISOLATED = 4,
  DOLL_GBA_CPU_BATCH_FAST = 5,
  DOLL_GBA_CPU_MODE_COUNT = 6,
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
  uint32_t thumb_fast_hits;
  uint32_t thumb_fast_misses;
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
  uint32_t bios_init_loops;
  uint32_t guest_reset_trips;
  uint32_t guest_entry_trips;
  uint32_t sound_on;
  uint32_t sound_read_calls;
  uint32_t sound_samples_requested;
  uint32_t sound_samples_returned;
  uint32_t sound_last_available;
  uint32_t sound_max_available;
  uint32_t sound_drop_events;
  uint32_t sound_nonzero_samples;
  uint32_t sound_peak_sample;
  uint32_t sound_underrun_samples;
  uint32_t sound_cnt_l;
  uint32_t sound_cnt_h;
  uint32_t sound_cnt_x;
  uint32_t sound_gbc_active;
  uint32_t sound_direct_status;
  uint32_t sound_gbc_volume;
  uint32_t sound_direct_fifo;
  uint32_t sound_timer_state;
  uint32_t sound_dma_state;
  uint32_t sound_buffer_base;
  uint32_t sound_gbc_buffer_index;
  uint32_t sound_direct_buffer_a;
  uint32_t sound_direct_buffer_b;
  uint32_t sound_timer_calls_a;
  uint32_t sound_timer_calls_b;
  uint32_t sound_fifo_nonzero_a;
  uint32_t sound_fifo_nonzero_b;
  uint32_t sound_fifo_peak_a;
  uint32_t sound_fifo_peak_b;
  uint32_t sound_fifo_words_a;
  uint32_t sound_fifo_words_b;
  uint32_t sound_fifo_nonzero_bytes_a;
  uint32_t sound_fifo_nonzero_bytes_b;
  uint32_t sound_timer_nonzero_a;
  uint32_t sound_timer_nonzero_b;
  uint32_t sound_timer_peak_a;
  uint32_t sound_timer_peak_b;
  uint32_t sound_fifo_empty_reads;
  uint32_t sound_fifo_short_reads;
  uint32_t guest_reset_prev_pc;
  uint32_t guest_reset_lr;
  uint32_t guest_reset_sp;
} doll_gba_perf_stats_t;

bool doll_gba_core_begin(uint16_t* framebuffer);
bool doll_gba_core_set_framebuffer(uint16_t* framebuffer);
bool doll_gba_core_load(const char* rom_path);
void doll_gba_core_stop(void);
void doll_gba_core_run(uint16_t buttons, bool draw);
bool doll_gba_core_debug_capture_active(void);
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
