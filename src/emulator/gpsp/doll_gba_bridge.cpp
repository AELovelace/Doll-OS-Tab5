#include "doll_gba_bridge.h"

#include "esp_heap_caps.h"

extern "C" {
#include "common.h"
}

#include "open_gba_bios.h"

namespace {
uint16_t currentButtons = 0;

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
void gba_p4_thumb_jit_shutdown(void);
extern u32 gba_thumb_jit_bytes;
extern u32 gba_thumb_jit_hits;
extern u32 gba_thumb_jit_misses;
extern u32 gba_thumb_jit_compiles;
extern u32 gba_rom_page_loads;
extern u32 gba_rom_page_prefetches;
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

  // Reserve the bounded executable arena before ordinary core buffers consume
  // the P4's shared L2 SRAM. Large data buffers can safely fall back to PSRAM;
  // generated code cannot.
  gba_p4_thumb_jit_preinit();

  gbsp_memory->p_iwram = static_cast<u8*>(allocRegion(GBA_IWRAM_SIZE, true));
  gbsp_memory->p_memory_map_read = static_cast<u8**>(allocRegion(GBA_MEMORY_MAP_READ_SIZE, true));
  gbsp_memory->p_vram = static_cast<u8*>(allocRegion(GBA_VRAM_SIZE, true));
  gbsp_memory->p_ewram = static_cast<u8*>(allocRegion(GBA_EWRAM_SIZE, false));
  gbsp_memory->p_bios_rom = static_cast<u8*>(allocRegion(GBA_BIOS_ROM_SIZE, true));
  gbsp_memory->p_gamepak_backup = static_cast<u8*>(allocRegion(GBA_GAMEPAK_BACKUP_SIZE, false));
  if (!gbsp_memory->p_iwram || !gbsp_memory->p_memory_map_read ||
      !gbsp_memory->p_vram || !gbsp_memory->p_ewram ||
      !gbsp_memory->p_bios_rom || !gbsp_memory->p_gamepak_backup) {
    gba_p4_thumb_jit_shutdown();
    releaseCoreMemory();
    return false;
  }

  gba_screen_pixels = framebuffer;
  libretro_supports_bitmasks = true;
  retro_set_input_state(inputCallback);
  init_gamepak_buffer();
  init_sound();
  memcpy(bios_rom, open_gba_bios_rom, GBA_BIOS_ROM_SIZE);
  currentButtons = 0;
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
  gba_thumb_jit_hits = gba_thumb_jit_misses = gba_thumb_jit_compiles = 0;
  gba_rom_page_loads = gba_rom_page_prefetches = 0;
  selected_boot_mode = boot_game;
  reset_gba();
  write_ioreg(REG_DISPCNT, 0x0000);
  return true;
}

void doll_gba_core_stop(void) {
  if (!gbsp_memory) {
    gba_p4_thumb_jit_shutdown();
    return;
  }
  currentButtons = 0;
  memory_term();
  releaseCoreMemory();
  gba_p4_thumb_jit_shutdown();
  gba_screen_pixels = nullptr;
}

void doll_gba_core_run(uint16_t buttons, bool draw) {
  if (!gbsp_memory) return;
  currentButtons = buttons;
  skip_next_frame = draw ? 0 : 1;
  update_input();
  rumble_frame_reset();
  clear_gamepak_stickybits();
  execute_arm(execute_cycles);
}

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
  stats->rom_page_loads = gba_rom_page_loads;
  stats->rom_page_prefetches = gba_rom_page_prefetches;
}
}  // extern "C"
