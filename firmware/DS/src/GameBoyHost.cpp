#include "GameBoyHost.h"

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
}

bool GameBoyHost::begin() {
  if (ready_) return true;
  const size_t framePixels = kWidth * kHeight;
  frame_ = static_cast<uint16_t*>(heap_caps_calloc(
      framePixels, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  const bool frameInPsram = frame_ != nullptr;
  if (!frame_) {
    frame_ = static_cast<uint16_t*>(heap_caps_calloc(
        framePixels, sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  soundScratch_ = static_cast<int16_t*>(heap_caps_malloc(
      kSoundScratchSamples * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!frame_ || !soundScratch_) {
    if (frame_) heap_caps_free(frame_);
    if (soundScratch_) heap_caps_free(soundScratch_);
    frame_ = nullptr;
    soundScratch_ = nullptr;
    status_ = "Game Boy buffers unavailable";
    return false;
  }
  Serial.printf("[psram] gbFrame: %u bytes -> %s\n",
                (unsigned)(framePixels * sizeof(uint16_t)),
                frameInPsram ? "PSRAM" : "INTERNAL RAM (PSRAM unavailable)");
  // The callback is always registered, even if the codec never came up: gnuboy
  // is init'd once for the life of the firmware, so binding on AudioOut's state
  // here would freeze the first launch's answer in forever. AudioOut::onSamples
  // drops on the floor while it isn't ready, which is the same silence for a
  // fraction of a frame's work.
  //
  // Mono, not stereo: the board has one speaker on one I2S slot -- see the
  // mixdown note in AudioOut::onSamples.
  if (gnuboy_init(kSampleRate, GB_AUDIO_MONO_S16, GB_PIXEL_565_LE, nullptr,
                  &audioTrampoline) != 0) {
    heap_caps_free(frame_);
    heap_caps_free(soundScratch_);
    frame_ = nullptr;
    soundScratch_ = nullptr;
    status_ = "gnuboy initialization failed";
    return false;
  }
  gnuboy_set_framebuffer(frame_);
  gnuboy_set_soundbuffer(soundScratch_, kSoundScratchSamples);
  ready_ = true;
  status_ = "Game Boy ready";
  return true;
}

bool GameBoyHost::load(const String& romPath, const String& savePath) {
  if (!begin()) return false;
  if (loaded_) stop();
  if (romPath.isEmpty() || gnuboy_load_rom_file(romPath.c_str()) != 0) {
    status_ = "ROM load failed";
    return false;
  }
  savePath_ = savePath;
  gnuboy_reset(true);
  if (!savePath_.isEmpty()) gnuboy_load_sram(savePath_.c_str());
  loaded_ = true;
  savePending_ = false;
  lastSaveMs_ = 0;
  status_ = "Playing " + romPath;
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
