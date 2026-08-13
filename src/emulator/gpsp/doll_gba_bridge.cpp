#include <Arduino.h>

#include "doll_gba_bridge.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"

extern "C" {
#include "common.h"
}

#include "open_gba_bios.h"

extern "C" {
extern u32 gba_thumb_jit_last_pc;
extern u32 gba_thumb_jit_last_end_pc;
extern u32 gba_thumb_jit_last_ret;
extern u32 gba_thumb_jit_last_signature;
extern timer_type timer[4];
}

namespace {
uint16_t currentButtons = 0;
uint16_t previousDebugButtons = 0;
uint32_t transitionDebugFrames = 0;
uint32_t transitionDebugSequence = 0;

uint32_t hashDebugMemory(const uint8_t* data, size_t size) {
  uint32_t hash = 2166136261U;
  for (size_t offset = 0; offset < size; ++offset) {
    hash = (hash ^ data[offset]) * 16777619U;
  }
  return hash;
}  // Fingerprints all working RAM so a game-state rewind is visible in serial.

void logTransitionStep(uint16_t buttons) {
  const uint32_t ewramHash = hashDebugMemory(ewram, GBA_EWRAM_SIZE);
  const uint32_t iwramHash = hashDebugMemory(iwram, GBA_IWRAM_SIZE);
  Serial.printf(
      "[gba-step] "
      "n=%lu frame=%lu key=%03x p1=%04x pc=%08lx lr=%08lx sp=%08lx cpsr=%08lx "
      "r0=%08lx r1=%08lx r2=%08lx r3=%08lx irq=%04x/%04x/%04x "
      "disp=%04x vc=%u mem=%08lx/%08lx jit=%08lx->%08lx:%08lx:%08lx\n",
      (unsigned long)transitionDebugSequence++, (unsigned long)frame_counter,
      static_cast<unsigned>(buttons), static_cast<unsigned>(read_ioreg(REG_P1)),
      (unsigned long)reg[REG_PC],
      (unsigned long)reg[REG_LR], (unsigned long)reg[REG_SP],
      (unsigned long)reg[REG_CPSR], (unsigned long)reg[0],
      (unsigned long)reg[1], (unsigned long)reg[2], (unsigned long)reg[3],
      static_cast<unsigned>(read_ioreg(REG_IE)),
      static_cast<unsigned>(read_ioreg(REG_IF)),
      static_cast<unsigned>(read_ioreg(REG_IME)),
      static_cast<unsigned>(read_ioreg(REG_DISPCNT)),
      static_cast<unsigned>(read_ioreg(REG_VCOUNT)),
      (unsigned long)ewramHash, (unsigned long)iwramHash,
      (unsigned long)gba_thumb_jit_last_pc,
      (unsigned long)gba_thumb_jit_last_end_pc,
      (unsigned long)gba_thumb_jit_last_ret,
      (unsigned long)gba_thumb_jit_last_signature);
}  // Captures CPU, IRQ, video, memory, and JIT state every third post-A frame.

void* allocRegion(size_t size, bool preferInternal) {
  uint32_t preferred = MALLOC_CAP_8BIT |
      (preferInternal ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM);
  void* result = heap_caps_calloc(1, size, preferred);
  if (!result) {
    uint32_t fallback = MALLOC_CAP_8BIT |
        (preferInternal ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
    result = heap_caps_calloc(1, size, fallback);
  }
  return result;
}

int16_t inputCallback(unsigned, unsigned, unsigned, unsigned id) {
  (void)id;
  int16_t result = 0;
  if (currentButtons & 0x001) result |= 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT;
  if (currentButtons & 0x002) result |= 1 << RETRO_DEVICE_ID_JOYPAD_LEFT;
  if (currentButtons & 0x004) result |= 1 << RETRO_DEVICE_ID_JOYPAD_UP;
  if (currentButtons & 0x008) result |= 1 << RETRO_DEVICE_ID_JOYPAD_DOWN;
  if (currentButtons & 0x010) result |= 1 << RETRO_DEVICE_ID_JOYPAD_A;
  if (currentButtons & 0x020) result |= 1 << RETRO_DEVICE_ID_JOYPAD_B;
  if (currentButtons & 0x040) result |= 1 << RETRO_DEVICE_ID_JOYPAD_SELECT;
  if (currentButtons & 0x080) result |= 1 << RETRO_DEVICE_ID_JOYPAD_START;
  if (currentButtons & 0x100) result |= 1 << RETRO_DEVICE_ID_JOYPAD_L;
  if (currentButtons & 0x200) result |= 1 << RETRO_DEVICE_ID_JOYPAD_R;
  return result;
}

void releaseCoreMemory() {
  if (!gbsp_memory) return;
  if (gbsp_memory->p_iwram) heap_caps_free(gbsp_memory->p_iwram);
  if (gbsp_memory->p_memory_map_read) heap_caps_free(gbsp_memory->p_memory_map_read);
  if (gbsp_memory->p_vram) heap_caps_free(gbsp_memory->p_vram);
  if (gbsp_memory->p_ewram) heap_caps_free(gbsp_memory->p_ewram);
  if (gbsp_memory->p_bios_rom) heap_caps_free(gbsp_memory->p_bios_rom);
  if (gbsp_memory->p_gamepak_backup) heap_caps_free(gbsp_memory->p_gamepak_backup);
  if (gbsp_memory->p_palette_ram) heap_caps_free(gbsp_memory->p_palette_ram);
  if (gbsp_memory->p_oam_ram) heap_caps_free(gbsp_memory->p_oam_ram);
  if (gbsp_memory->p_palette_ram_converted) heap_caps_free(gbsp_memory->p_palette_ram_converted);
  if (gbsp_memory->p_io_registers) heap_caps_free(gbsp_memory->p_io_registers);
  heap_caps_free(gbsp_memory);
  gbsp_memory = nullptr;
}
}  // namespace

extern "C" {
gbsp_memory_t* gbsp_memory = nullptr;
u32 skip_next_frame = 0;
u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES] = {};
u32 translation_gate_targets = 0;
int dynarec_enable = 0;
boot_mode selected_boot_mode = boot_game;
int sprite_limit = 1;

void execute_arm(u32 cycles);
void gba_p4_thumb_jit_preinit(void);
void gba_p4_thumb_jit_reset(void);
void gba_p4_thumb_jit_reset_stats(void);
void gba_p4_thumb_jit_shutdown(void);
bool gba_video_scratch_init(void);
void gba_video_scratch_term(void);
extern u32 gba_thumb_jit_bytes;
extern u32 gba_thumb_jit_hits;
extern u32 gba_thumb_jit_misses;
extern u32 gba_thumb_jit_compiles;
extern u32 gba_thumb_jit_attempts;
extern u32 gba_thumb_jit_disabled;
extern u32 gba_thumb_jit_ops;
extern u32 gba_thumb_jit_used_bytes;
extern u32 gba_thumb_jit_arena_full;
extern u32 gba_thumb_jit_short_blocks;
extern u32 gba_thumb_jit_reject_hits;
extern u32 gba_thumb_jit_hot_waits;
extern u32 gba_thumb_jit_reuses;
extern u32 gba_thumb_jit_adapt_probes;
extern u32 gba_thumb_jit_top_break;
extern u32 gba_thumb_jit_top_break_count;
extern u32 gba_thumb_batch_runs;
extern u32 gba_thumb_batch_ops;
extern u32 gba_thumb_batch_enabled;
extern u32 gba_interp_fast_enabled;
extern u32 gba_thumb_jit_runtime_enabled;
extern u32 gba_thumb_jit_debug_validate;
extern u32 gba_thumb_jit_guard_trips;
extern u32 gba_thumb_jit_last_pc;
extern u32 gba_thumb_jit_last_end_pc;
extern u32 gba_thumb_jit_last_ret;
extern u32 gba_thumb_jit_last_signature;
extern u32 gba_swi_hle_softreset_count;
extern u32 gba_bad_pc_count;
extern u32 gba_rom_page_loads;
extern u32 gba_rom_page_prefetches;
extern u32 gba_execute_arm_updates;
extern u32 gba_execute_thumb_updates;
extern u32 gba_execute_halt_updates;
extern u32 gba_execute_last_pc;
extern u32 gba_execute_last_cpsr;
extern u32 gba_bios_init_loop_hle_count;
extern u32 gba_guest_reset_trips;
extern u32 gba_guest_entry_trips;
extern u32 gba_guest_reset_prev_pc;
extern u32 gba_guest_reset_lr;
extern u32 gba_guest_reset_sp;
void set_fastforward_override(bool) {}
// Doll-OS does not expose gpSP's libretro RFU transport yet. Serial mode is
// disabled at load time; these no-op callbacks satisfy the dormant RFU path.
void netpacket_send(uint16_t, const void*, size_t) {}
void netpacket_poll_receive(void) {}

bool doll_gba_core_begin(uint16_t* framebuffer) {
  if (!framebuffer || gbsp_memory) return false;
  gbsp_memory = static_cast<gbsp_memory_t*>(
      heap_caps_calloc(1, sizeof(*gbsp_memory), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!gbsp_memory) return false;

  // None of gpSP's large scratch tables belong to the OS boot footprint.
  // Materialize them in PSRAM only when the foreground GBA app starts.
  if (!gba_memory_scratch_init() || !gba_sound_scratch_init() ||
      !gba_video_scratch_init()) {
    gba_video_scratch_term();
    gba_sound_scratch_term();
    gba_memory_scratch_term();
    releaseCoreMemory();
    return false;
  }

  // Keep the CPU's working RAM, page map, and emulated VRAM in L2. Drawn frames
  // touch VRAM far more consistently than the low-coverage JIT, so protect it
  // before allowing the executable arena to consume the remaining internal heap.
  gbsp_memory->p_iwram = static_cast<u8*>(allocRegion(GBA_IWRAM_SIZE, true));
  // The 32 KB read map is consulted by instruction fetches and most emulated
  // loads. Reserve it before the JIT and VRAM so normal operation keeps this
  // high-frequency pointer table in internal L2 instead of PSRAM.
  gbsp_memory->p_memory_map_read = static_cast<u8**>(
      allocRegion(GBA_MEMORY_MAP_READ_SIZE, true));
  gbsp_memory->p_palette_ram = static_cast<u16*>(allocRegion(512 * sizeof(u16), true));
  gbsp_memory->p_oam_ram = static_cast<u16*>(allocRegion(512 * sizeof(u16), true));
  gbsp_memory->p_palette_ram_converted = static_cast<u16*>(allocRegion(512 * sizeof(u16), true));
  gbsp_memory->p_io_registers = static_cast<u16*>(allocRegion(512 * sizeof(u16), true));

  gbsp_memory->p_vram = static_cast<u8*>(allocRegion(GBA_VRAM_SIZE, true));
  gba_p4_thumb_jit_preinit();

  // The large EWRAM and ROM backing stores remain the right PSRAM residents.
  gbsp_memory->p_ewram = static_cast<u8*>(allocRegion(GBA_EWRAM_SIZE, false));
  gbsp_memory->p_bios_rom = static_cast<u8*>(allocRegion(GBA_BIOS_ROM_SIZE, false));
  gbsp_memory->p_gamepak_backup = static_cast<u8*>(allocRegion(GBA_GAMEPAK_BACKUP_SIZE, false));
  if (!gbsp_memory->p_iwram || !gbsp_memory->p_memory_map_read ||
      !gbsp_memory->p_vram || !gbsp_memory->p_ewram ||
      !gbsp_memory->p_bios_rom || !gbsp_memory->p_gamepak_backup ||
      !gbsp_memory->p_palette_ram || !gbsp_memory->p_oam_ram ||
      !gbsp_memory->p_palette_ram_converted || !gbsp_memory->p_io_registers) {
    gba_p4_thumb_jit_shutdown();
    gba_video_scratch_term();
    gba_sound_scratch_term();
    gba_memory_scratch_term();
    releaseCoreMemory();
    return false;
  }

  ESP_LOGI("gba", "memory IWRAM=%s MAP=%s VRAM=%s IO=%s JIT=%luK",
           esp_ptr_internal(gbsp_memory->p_iwram) ? "L2" : "PSRAM",
           esp_ptr_internal(gbsp_memory->p_memory_map_read) ? "L2" : "PSRAM",
           esp_ptr_internal(gbsp_memory->p_vram) ? "L2" : "PSRAM",
           esp_ptr_internal(gbsp_memory->p_io_registers) ? "L2" : "PSRAM",
           static_cast<unsigned long>(gba_thumb_jit_bytes / 1024));

  gba_screen_pixels = framebuffer;
  libretro_supports_bitmasks = true;
  retro_set_input_state(inputCallback);
  init_gamepak_buffer();
  init_sound();
  memcpy(bios_rom, open_gba_bios_rom, GBA_BIOS_ROM_SIZE);
  currentButtons = 0;
  previousDebugButtons = 0;
  transitionDebugFrames = 0;
  transitionDebugSequence = 0;
  return true;
}

bool doll_gba_core_load(const char* rom_path) {
  if (!gbsp_memory || !rom_path || !rom_path[0]) return false;
  memset(gamepak_backup, 0xFF, GBA_GAMEPAK_BACKUP_SIZE);
  if (load_gamepak(nullptr, rom_path, FEAT_AUTODETECT,
                   FEAT_AUTODETECT, SERIAL_MODE_DISABLED) != 0) {
    return false;
  }
  gba_p4_thumb_jit_reset();
  gba_p4_thumb_jit_reset_stats();
  previousDebugButtons = 0;
  transitionDebugFrames = 0;
  transitionDebugSequence = 0;
  // Run the JIT without the batch engine or hand-written fast interpreter. Each
  // ROM block is checked eight times against stock gpSP before becoming trusted,
  // isolating useful code generation from the path that corrupted the title.
  doll_gba_core_set_cpu_mode(DOLL_GBA_CPU_JIT_ISOLATED);
  gba_rom_page_loads = gba_rom_page_prefetches = 0;
  selected_boot_mode = boot_game;
  reset_gba();
  write_ioreg(REG_DISPCNT, 0x0000);
  return true;
}

void doll_gba_core_stop(void) {
  if (!gbsp_memory) {
    gba_p4_thumb_jit_shutdown();
    gba_video_scratch_term();
    gba_sound_scratch_term();
    gba_memory_scratch_term();
    return;
  }
  currentButtons = 0;
  memory_term();
  gba_video_scratch_term();
  gba_sound_scratch_term();
  gba_memory_scratch_term();
  releaseCoreMemory();
  gba_p4_thumb_jit_shutdown();
  gba_screen_pixels = nullptr;
}

void doll_gba_core_run(uint16_t buttons, bool draw) {
  if (!gbsp_memory) return;
  const uint16_t changedButtons = buttons ^ previousDebugButtons;
  const uint16_t actionPressed = changedButtons & buttons & 0x090U;
  currentButtons = buttons;
  skip_next_frame = draw ? 0 : 1;
  update_input();
  if (changedButtons) {
    Serial.printf("[gba-input] frame=%lu key=%03x changed=%03x p1=%04x pc=%08lx lr=%08lx sp=%08lx\n",
        (unsigned long)frame_counter, static_cast<unsigned>(buttons),
        static_cast<unsigned>(changedButtons),
        static_cast<unsigned>(read_ioreg(REG_P1)), (unsigned long)reg[REG_PC],
        (unsigned long)reg[REG_LR], (unsigned long)reg[REG_SP]);
  }
  if (actionPressed) {
    transitionDebugFrames = 600;
    transitionDebugSequence = 0;
    Serial.printf(
        "[gba-step] action=%03x capture armed for 600 frames; CPU mode remains %lu\n",
        static_cast<unsigned>(actionPressed),
        (unsigned long)doll_gba_core_get_cpu_mode());
  }
  previousDebugButtons = buttons;
  rumble_frame_reset();
  clear_gamepak_stickybits();
  execute_arm(execute_cycles);
  if (transitionDebugFrames) {
    if ((transitionDebugFrames % 3U) == 0U) logTransitionStep(buttons);
    --transitionDebugFrames;
    if (!transitionDebugFrames) {
      Serial.printf("[gba-step] capture complete; CPU mode remains %lu\n",
          (unsigned long)doll_gba_core_get_cpu_mode());
    }
  }
}

bool doll_gba_core_debug_capture_active(void) {
  return transitionDebugFrames != 0;
}  // Lets the frontend increase serial detail only around an A/Start transition.

void doll_gba_core_set_cpu_mode(uint32_t mode) {
  if (mode >= DOLL_GBA_CPU_MODE_COUNT) mode = DOLL_GBA_CPU_SAFE;
  gba_thumb_batch_enabled = mode == DOLL_GBA_CPU_BATCH || mode == DOLL_GBA_CPU_TURBO;
  gba_thumb_jit_runtime_enabled =
      mode == DOLL_GBA_CPU_JIT_ISOLATED || mode == DOLL_GBA_CPU_TURBO;
  // Isolated JIT validates each block eight times, then permits the trusted hot
  // path. Continuous comparison is intentionally off so this build measures the
  // acceleration we can actually ship rather than diagnostic double execution.
  gba_thumb_jit_debug_validate = 0;
  // Only the explicitly unsafe combined mode may enter the hand-written ARM or
  // Thumb dispatcher. Isolated JIT always falls back to stock gpSP per opcode.
  gba_interp_fast_enabled = mode == DOLL_GBA_CPU_TURBO;
}  // Selects isolated accelerators, a checked JIT, or combined turbo execution.

uint32_t doll_gba_core_get_cpu_mode(void) {
  if (gba_thumb_jit_runtime_enabled && gba_thumb_batch_enabled) return DOLL_GBA_CPU_TURBO;
  if (gba_thumb_jit_runtime_enabled) return DOLL_GBA_CPU_JIT_ISOLATED;
  if (gba_thumb_batch_enabled) return DOLL_GBA_CPU_BATCH;
  return DOLL_GBA_CPU_SAFE;
}  // Reports the active accelerator combination to the diagnostic menu.

uint32_t doll_gba_core_read_audio(int16_t* stereo, uint32_t frames) {
  return (gbsp_memory && stereo) ? sound_read_samples(stereo, frames) : 0;
}

void* doll_gba_core_save_data(void) {
  return gbsp_memory ? gamepak_backup : nullptr;
}

bool doll_gba_core_save_state(void* output, size_t size) {
  if (!gbsp_memory || !output || size != GBA_STATE_MEM_SIZE) return false;
  memset(output, 0, size);
  gba_save_state(output);
  return true;
}

bool doll_gba_core_load_state(const void* input, size_t size) {
  if (!(gbsp_memory && input && size == GBA_STATE_MEM_SIZE && gba_load_state(input))) {
    return false;
  }
  gba_p4_thumb_jit_reset();
  return true;
}

void doll_gba_core_get_perf(doll_gba_perf_stats_t* stats) {
  if (!stats) return;
  stats->jit_bytes = gba_thumb_jit_bytes;
  stats->jit_hits = gba_thumb_jit_hits;
  stats->jit_misses = gba_thumb_jit_misses;
  stats->jit_compiles = gba_thumb_jit_compiles;
  stats->jit_attempts = gba_thumb_jit_attempts;
  stats->jit_disabled = gba_thumb_jit_disabled;
  stats->jit_ops = gba_thumb_jit_ops;
  stats->jit_used_bytes = gba_thumb_jit_used_bytes;
  stats->jit_arena_full = gba_thumb_jit_arena_full;
  stats->jit_short_blocks = gba_thumb_jit_short_blocks;
  stats->jit_reject_hits = gba_thumb_jit_reject_hits;
  stats->jit_hot_waits = gba_thumb_jit_hot_waits;
  stats->jit_reuses = gba_thumb_jit_reuses;
  stats->jit_adapt_probes = gba_thumb_jit_adapt_probes;
  stats->jit_top_break = gba_thumb_jit_top_break;
  stats->jit_top_break_count = gba_thumb_jit_top_break_count;
  stats->thumb_batch_runs = gba_thumb_batch_runs;
  stats->thumb_batch_ops = gba_thumb_batch_ops;
  stats->vram_internal = gbsp_memory && esp_ptr_internal(gbsp_memory->p_vram);
  stats->cpu_mode = doll_gba_core_get_cpu_mode();
  stats->softreset_count = gba_swi_hle_softreset_count;
  stats->bad_pc_count = gba_bad_pc_count;
  stats->jit_guard_trips = gba_thumb_jit_guard_trips;
  stats->jit_last_pc = gba_thumb_jit_last_pc;
  stats->jit_last_end_pc = gba_thumb_jit_last_end_pc;
  stats->jit_last_ret = gba_thumb_jit_last_ret;
  stats->jit_last_signature = gba_thumb_jit_last_signature;
  stats->rom_page_loads = gba_rom_page_loads;
  stats->rom_page_prefetches = gba_rom_page_prefetches;
  stats->arm_updates = gba_execute_arm_updates;
  stats->thumb_updates = gba_execute_thumb_updates;
  stats->halt_updates = gba_execute_halt_updates;
  stats->last_pc = gba_execute_last_pc;
  stats->last_cpsr = gba_execute_last_cpsr;
  stats->bios_init_loops = gba_bios_init_loop_hle_count;
  stats->guest_reset_trips = gba_guest_reset_trips;
  stats->guest_entry_trips = gba_guest_entry_trips;
  stats->sound_on = sound_on;
  stats->sound_read_calls = sound_read_calls;
  stats->sound_samples_requested = sound_samples_requested;
  stats->sound_samples_returned = sound_samples_returned;
  stats->sound_last_available = sound_last_samples_available;
  stats->sound_max_available = sound_max_samples_available;
  stats->sound_drop_events = sound_drop_events;
  stats->sound_nonzero_samples = sound_nonzero_samples;
  stats->sound_peak_sample = sound_peak_sample;
  stats->sound_underrun_samples = sound_underrun_samples;
  stats->sound_cnt_l = read_ioreg(REG_SOUNDCNT_L);
  stats->sound_cnt_h = read_ioreg(REG_SOUNDCNT_H);
  stats->sound_cnt_x = read_ioreg(REG_SOUNDCNT_X);
  stats->sound_gbc_active = (gbc_sound_channel[0].active_flag ? 1U : 0U) |
      (gbc_sound_channel[1].active_flag ? 2U : 0U) |
      (gbc_sound_channel[2].active_flag ? 4U : 0U) |
      (gbc_sound_channel[3].active_flag ? 8U : 0U);
  stats->sound_direct_status = (direct_sound_channel[0].status & 3U) |
      ((direct_sound_channel[1].status & 3U) << 2U);
  stats->sound_gbc_volume = (gbc_sound_master_volume_right & 7U) |
      ((gbc_sound_master_volume_left & 7U) << 4U) |
      ((gbc_sound_master_volume & 3U) << 8U);
  stats->sound_direct_fifo =
      ((direct_sound_channel[0].fifo_top - direct_sound_channel[0].fifo_base) & 31U) |
      (((direct_sound_channel[1].fifo_top - direct_sound_channel[1].fifo_base) & 31U) << 8U);
  stats->sound_timer_state = (timer[0].status & 3U) |
      ((timer[0].direct_sound_channels & 3U) << 4U) |
      ((timer[1].status & 3U) << 8U) |
      ((timer[1].direct_sound_channels & 3U) << 12U);
  stats->sound_dma_state = (dma[1].start_type & 7U) |
      ((dma[1].direct_sound_channel & 3U) << 4U) |
      ((dma[2].start_type & 7U) << 8U) |
      ((dma[2].direct_sound_channel & 3U) << 12U);
  stats->sound_buffer_base = sound_buffer_base;
  stats->sound_gbc_buffer_index = gbc_sound_buffer_index;
  stats->sound_direct_buffer_a = direct_sound_channel[0].buffer_index;
  stats->sound_direct_buffer_b = direct_sound_channel[1].buffer_index;
  stats->sound_timer_calls_a = sound_timer_calls[0];
  stats->sound_timer_calls_b = sound_timer_calls[1];
  stats->sound_fifo_nonzero_a = 0;
  stats->sound_fifo_nonzero_b = 0;
  stats->sound_fifo_peak_a = 0;
  stats->sound_fifo_peak_b = 0;
  for (uint32_t channel = 0; channel < 2; ++channel) {
    const direct_sound_struct& direct = direct_sound_channel[channel];
    const uint32_t depth = (direct.fifo_top - direct.fifo_base) & 31U;
    for (uint32_t offset = 0; offset < depth; ++offset) {
      const int32_t sample = direct.fifo[(direct.fifo_base + offset) & 31U];
      const uint32_t magnitude = sample < 0 ? static_cast<uint32_t>(-sample)
                                             : static_cast<uint32_t>(sample);
      if (sample) {
        if (channel == 0) ++stats->sound_fifo_nonzero_a;
        else ++stats->sound_fifo_nonzero_b;
      }
      uint32_t& peak = channel == 0 ? stats->sound_fifo_peak_a
                                    : stats->sound_fifo_peak_b;
      if (magnitude > peak) peak = magnitude;
    }
  }
  stats->sound_fifo_words_a = sound_fifo_queue_words[0];
  stats->sound_fifo_words_b = sound_fifo_queue_words[1];
  stats->sound_fifo_nonzero_bytes_a = sound_fifo_queue_nonzero_bytes[0];
  stats->sound_fifo_nonzero_bytes_b = sound_fifo_queue_nonzero_bytes[1];
  stats->sound_timer_nonzero_a = sound_timer_nonzero_samples[0];
  stats->sound_timer_nonzero_b = sound_timer_nonzero_samples[1];
  stats->sound_timer_peak_a = sound_timer_peak_samples[0];
  stats->sound_timer_peak_b = sound_timer_peak_samples[1];
  stats->sound_fifo_empty_reads = sound_fifo_empty_reads;
  stats->sound_fifo_short_reads = sound_fifo_short_reads;
  stats->guest_reset_prev_pc = gba_guest_reset_prev_pc;
  stats->guest_reset_lr = gba_guest_reset_lr;
  stats->guest_reset_sp = gba_guest_reset_sp;
}
}  // extern "C"
