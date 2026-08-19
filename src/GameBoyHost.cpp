#include "GameBoyHost.h"

#include "../BoardVariant.h"
#include "AudioOut.h"
#include "esp_heap_caps.h"

extern "C" {
#include "emulator/gnuboy/gnuboy.h"
}

namespace {
// Rate is AudioOut's (see the note there) so the emulator and the I2S channel
// agree; a mismatch would just play the game back at the wrong pitch.
constexpr uint32_t kSampleRate = AudioOut::kSampleRate;
// One frame produces ~549 mono samples; sized well above that so the callback
// fires once per frame from gnuboy_run's tail, not mid-frame on overflow.
constexpr size_t kSoundScratchSamples = 1024;
constexpr uint32_t kSaveDebounceMs = 2000;

void audioTrampoline(void* buf, size_t len) { AudioOut::onSamples(buf, len); }
// Floor between periodic SRAM flushes so always-dirty carts (RTC games mark
// SRAM dirty continuously) don't hammer the SD card every debounce period.
constexpr uint32_t kSaveMinIntervalMs = 30000;

void logMemory(const char* checkpoint) {
  Serial.printf(
      "[GBDBG host] %s heap_free=%u heap_largest=%u internal_free=%u "
      "internal_largest=%u psram_free=%u psram_largest=%u\n",
      checkpoint,
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
  Serial.flush();
}
}

bool GameBoyHost::begin() {
  Serial.printf("[GBDBG host 01] begin enter ready=%u loaded=%u\n",
                ready_ ? 1u : 0u, loaded_ ? 1u : 0u);
  Serial.flush();
  if (ready_) {
    Serial.println("[GBDBG host 02] begin reuse existing core");
    Serial.flush();
    return true;
  }
  logMemory("before host buffers");
  const size_t framePixels = kWidth * kHeight;
#if defined(DOLL_BOARD_TAB5)
  //The DSI controller continuously reads its own framebuffer from PSRAM. Keep
  //gnuboy's small, write-heavy source frame internal so LCD scanline rendering
  //cannot contend with that scanout before Gameboy.ino stages the finished image.
  frame_ = static_cast<uint16_t*>(heap_caps_calloc(
      framePixels, sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  bool frameInPsram = false;
  if (!frame_) {
    frame_ = static_cast<uint16_t*>(heap_caps_calloc(
        framePixels, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    frameInPsram = frame_ != nullptr;
  }
#else
  frame_ = static_cast<uint16_t*>(heap_caps_calloc(
      framePixels, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  const bool frameInPsram = frame_ != nullptr;
  if (!frame_) {
    frame_ = static_cast<uint16_t*>(heap_caps_calloc(
        framePixels, sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
#endif
  Serial.printf("[GBDBG host 03] framebuffer allocation ptr=%p bytes=%u location=%s\n",
                static_cast<void*>(frame_),
                static_cast<unsigned>(framePixels * sizeof(uint16_t)),
                frame_ ? (frameInPsram ? "PSRAM" : "INTERNAL") : "FAILED");
  Serial.flush();
  soundScratch_ = static_cast<int16_t*>(heap_caps_malloc(
      kSoundScratchSamples * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  Serial.printf("[GBDBG host 04] audio scratch allocation ptr=%p bytes=%u\n",
                static_cast<void*>(soundScratch_),
                static_cast<unsigned>(kSoundScratchSamples * sizeof(int16_t)));
  Serial.flush();
  if (!frame_ || !soundScratch_) {
    if (frame_) heap_caps_free(frame_);
    if (soundScratch_) heap_caps_free(soundScratch_);
    frame_ = nullptr;
    soundScratch_ = nullptr;
    status_ = "Game Boy buffers unavailable";
    logMemory("host buffer allocation FAILED");
    return false;
  }
  Serial.printf("[psram] gbFrame: %u bytes -> %s\n",
                (unsigned)(framePixels * sizeof(uint16_t)),
                frameInPsram ? "PSRAM" : "INTERNAL RAM");
  // The callback is always registered, even if the codec never came up: gnuboy
  // is init'd once for the life of the firmware, so binding on AudioOut's state
  // here would freeze the first launch's answer in forever. AudioOut::onSamples
  // drops on the floor while it isn't ready, which is the same silence for a
  // fraction of a frame's work.
  //
  // Mono, not stereo: the board has one speaker on one I2S slot -- see the
  // mixdown note in AudioOut::onSamples.
  Serial.printf("[GBDBG host 05] gnuboy_init begin rate=%u frame=%p audio=%p\n",
                static_cast<unsigned>(kSampleRate), static_cast<void*>(frame_),
                static_cast<void*>(soundScratch_));
  Serial.flush();
  const int initResult = gnuboy_init(kSampleRate, GB_AUDIO_MONO_S16,
                                     GB_PIXEL_565_LE, nullptr, &audioTrampoline);
  Serial.printf("[GBDBG host 06] gnuboy_init returned %d\n", initResult);
  Serial.flush();
  if (initResult != 0) {
    heap_caps_free(frame_);
    heap_caps_free(soundScratch_);
    frame_ = nullptr;
    soundScratch_ = nullptr;
    status_ = "gnuboy initialization failed";
    return false;
  }
  Serial.println("[GBDBG host 07] binding framebuffer");
  Serial.flush();
  gnuboy_set_framebuffer(frame_);
  Serial.println("[GBDBG host 08] binding audio scratch buffer");
  Serial.flush();
  gnuboy_set_soundbuffer(soundScratch_, kSoundScratchSamples);
  ready_ = true;
  status_ = "Game Boy ready";
  logMemory("host begin complete");
  return true;
}

bool GameBoyHost::load(const String& romPath, const String& savePath) {
  Serial.printf("[GBDBG host 10] load enter rom='%s' save='%s' loaded=%u\n",
                romPath.c_str(), savePath.c_str(), loaded_ ? 1u : 0u);
  Serial.flush();
  if (!begin()) {
    Serial.println("[GBDBG host 11] load stopped: begin failed");
    Serial.flush();
    return false;
  }
  if (loaded_) {
    Serial.println("[GBDBG host 12] stopping previously loaded ROM");
    Serial.flush();
    stop();
  }
  if (romPath.isEmpty()) {
    status_ = "ROM load failed";
    Serial.println("[GBDBG host 13] load stopped: empty ROM path");
    Serial.flush();
    return false;
  }
  logMemory("before gnuboy_load_rom_file");
  Serial.println("[GBDBG host 14] gnuboy_load_rom_file begin");
  Serial.flush();
  const int loadResult = gnuboy_load_rom_file(romPath.c_str());
  Serial.printf("[GBDBG host 15] gnuboy_load_rom_file returned %d\n", loadResult);
  Serial.flush();
  logMemory("after gnuboy_load_rom_file");
  if (loadResult != 0) {
    status_ = loadResult == -5
                  ? "ROM integrity check failed (replace corrupt ROM)"
                  : "ROM load failed (core error " + String(loadResult) + ")";
    // gnuboy_load_rom_file may already have allocated cartridge RAM and ROM
    // banks before detecting a malformed image. Release that partial load so
    // the user can replace the file and try again without rebooting.
    gnuboy_free_rom();
    return false;
  }
  savePath_ = savePath;
  Serial.println("[GBDBG host 16] hard reset begin");
  Serial.flush();
  gnuboy_reset(true);
  Serial.println("[GBDBG host 17] hard reset complete");
  Serial.flush();
  if (!savePath_.isEmpty()) {
    Serial.printf("[GBDBG host 18] SRAM load begin path='%s'\n", savePath_.c_str());
    Serial.flush();
    const int sramResult = gnuboy_load_sram(savePath_.c_str());
    Serial.printf("[GBDBG host 19] SRAM load returned %d (%s)\n", sramResult,
                  sramResult == 0 ? "loaded" : "absent/not-applicable");
    Serial.flush();
  } else {
    Serial.println("[GBDBG host 19] SRAM load skipped: empty save path");
    Serial.flush();
  }
  loaded_ = true;
  savePending_ = false;
  lastSaveMs_ = 0;
  status_ = "Playing " + romPath;
  logMemory("ROM load complete");
  Serial.println("[GBDBG host 20] load complete; ready for first frame");
  Serial.flush();
  return true;
}

void GameBoyHost::stop() {
  if (!loaded_) return;
  if (savePending_ && !savePath_.isEmpty()) gnuboy_save_sram(savePath_.c_str(), false);
  gnuboy_free_rom();
  loaded_ = false;
  savePending_ = false;
  status_ = "ROM closed";
}

void GameBoyHost::runFrame(bool draw) {
  if (!loaded_) return;
  gnuboy_run(draw);
  // Stamp only on the clean->pending transition. sram_dirty stays set until a
  // save clears it, so re-stamping every frame would keep the debounce from
  // ever expiring and periodic saves would never happen.
  if (gnuboy_sram_dirty() && !savePending_) {
    savePending_ = true;
    dirtyAtMs_ = millis();
  }
}

bool GameBoyHost::saveState(const char* path) {
  if (!loaded_ || !path || !path[0]) return false;
  return gnuboy_save_state(path) == 0;
}

bool GameBoyHost::loadState(const char* path) {
  if (!loaded_ || !path || !path[0]) return false;
  return gnuboy_load_state(path) == 0;
}

void GameBoyHost::setButtons(uint8_t buttons) {
  if (ready_) gnuboy_set_pad(buttons);
}

void GameBoyHost::tickSave() {
  if (!savePending_ || !loaded_ || savePath_.isEmpty()) return;
  const uint32_t nowMs = millis();
  if (nowMs - dirtyAtMs_ < kSaveDebounceMs) return;
  if (lastSaveMs_ != 0 && nowMs - lastSaveMs_ < kSaveMinIntervalMs) return;
  if (gnuboy_save_sram(savePath_.c_str(), false) == 0) {
    savePending_ = false;
    lastSaveMs_ = nowMs;
  }
}
