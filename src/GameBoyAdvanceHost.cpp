#include "GameBoyAdvanceHost.h"

#include "AudioOut.h"
#include "emulator/gpsp/doll_gba_bridge.h"
#include "esp_heap_caps.h"

namespace {
constexpr size_t kAudioMaxFrames = 600;
constexpr uint32_t kSaveIntervalMs = 30000;

void* allocBuffer(size_t size, bool preferInternal) {
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

bool readExactFile(const char* path, void* data, size_t size) {
  FILE* file = fopen(path, "rb");
  if (!file) return false;
  const bool ok = fread(data, 1, size, file) == size;
  fclose(file);
  return ok;
}

bool writeExactFile(const char* path, const void* data, size_t size) {
  FILE* file = fopen(path, "wb");
  if (!file) return false;
  const bool ok = fwrite(data, 1, size, file) == size && fflush(file) == 0;
  fclose(file);
  return ok;
}
}  // namespace

bool GameBoyAdvanceHost::allocateCoreMemory() {
  // These buffers exist only while gba owns the foreground. Keep them in
  // PSRAM so the shell/network stack retains its scarce internal heap and the
  // P4 JIT can reserve a small executable working set.
  frame_ = static_cast<uint16_t*>(allocBuffer(DOLL_GBA_FRAME_BYTES, false));
  stereoScratch_ = static_cast<int16_t*>(
      allocBuffer(kAudioMaxFrames * 2 * sizeof(int16_t), false));
  monoScratch_ = static_cast<int16_t*>(
      allocBuffer(kAudioMaxFrames * sizeof(int16_t), false));
  return frame_ && stereoScratch_ && monoScratch_;
}

void GameBoyAdvanceHost::releaseCoreMemory() {
  if (frame_) heap_caps_free(frame_);
  if (stereoScratch_) heap_caps_free(stereoScratch_);
  if (monoScratch_) heap_caps_free(monoScratch_);
  frame_ = nullptr;
  stereoScratch_ = nullptr;
  monoScratch_ = nullptr;
}

bool GameBoyAdvanceHost::begin() {
  if (ready_) return true;
  if (!allocateCoreMemory() || !doll_gba_core_begin(frame_)) {
    doll_gba_core_stop();
    releaseCoreMemory();
    status_ = "GBA memory allocation failed";
    return false;
  }
  ready_ = true;
  status_ = "GBA core ready";
  Serial.println("[gba] P4 gpSP core ready, 8 MB ROM cache, interpreter mode");
  return true;
}

bool GameBoyAdvanceHost::load(const String& romPath, const String& savePath) {
  if (loaded_) stop();
  if (!begin()) return false;
  if (!doll_gba_core_load(romPath.c_str())) {
    status_ = "GBA ROM load failed";
    doll_gba_core_stop();
    releaseCoreMemory();
    ready_ = false;
    return false;
  }

  savePath_ = savePath;
  void* saveData = doll_gba_core_save_data();
  if (saveData && !savePath_.isEmpty()) {
    FILE* save = fopen(savePath_.c_str(), "rb");
    if (save) {
      fread(saveData, 1, DOLL_GBA_SAVE_BYTES, save);
      fclose(save);
    }
  }

  buttons_ = 0;
  audioRemainder_ = 0;
  lastSaveMs_ = millis();
  loaded_ = true;
  status_ = "Playing " + romPath;
  return true;
}

bool GameBoyAdvanceHost::writeSave() {
  void* saveData = doll_gba_core_save_data();
  return loaded_ && saveData && !savePath_.isEmpty() &&
      writeExactFile(savePath_.c_str(), saveData, DOLL_GBA_SAVE_BYTES);
}

void GameBoyAdvanceHost::stop() {
  if (loaded_) writeSave();
  loaded_ = false;
  doll_gba_core_stop();
  releaseCoreMemory();
  ready_ = false;
  savePath_ = "";
  status_ = "GBA ROM closed";
}

void GameBoyAdvanceHost::submitAudio() {
  // A GBA frame is 280896 master cycles, not exactly 1/60 second. Generate
  // samples against the real 16.777216 MHz clock so the I2S ring does not
  // gradually push pacing toward 60 Hz or play Pokemon at the wrong pitch.
  constexpr uint64_t kGbaCyclesPerFrame = 280896;
  constexpr uint64_t kGbaClockHz = 16777216;
  audioRemainder_ += static_cast<uint64_t>(DOLL_GBA_SOUND_FREQUENCY) *
                     kGbaCyclesPerFrame;
  uint32_t frames = static_cast<uint32_t>(audioRemainder_ / kGbaClockHz);
  audioRemainder_ %= kGbaClockHz;
  if (frames > kAudioMaxFrames) frames = kAudioMaxFrames;
  const uint32_t produced = doll_gba_core_read_audio(stereoScratch_, frames);
  for (uint32_t i = 0; i < produced; ++i) {
    const int32_t mixed = static_cast<int32_t>(stereoScratch_[i * 2]) +
                          static_cast<int32_t>(stereoScratch_[i * 2 + 1]);
    monoScratch_[i] = static_cast<int16_t>(mixed / 2);
  }
  AudioOut::onSamples(monoScratch_, produced);
}

void GameBoyAdvanceHost::runFrame(bool draw) {
  if (!loaded_) return;
  doll_gba_core_run(buttons_, draw);
  submitAudio();
}

void GameBoyAdvanceHost::setButtons(uint16_t buttons) {
  buttons_ = buttons;
}

void GameBoyAdvanceHost::tickSave() {
  if (!loaded_ || savePath_.isEmpty()) return;
  const uint32_t now = millis();
  if (now - lastSaveMs_ >= kSaveIntervalMs) {
    writeSave();
    lastSaveMs_ = now;
  }
}

bool GameBoyAdvanceHost::saveState(const char* path) {
  if (!loaded_ || !path || !path[0]) return false;
  void* state = heap_caps_malloc(DOLL_GBA_STATE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!state) return false;
  const bool serialized = doll_gba_core_save_state(state, DOLL_GBA_STATE_BYTES);
  const bool ok = serialized && writeExactFile(path, state, DOLL_GBA_STATE_BYTES);
  heap_caps_free(state);
  return ok;
}

bool GameBoyAdvanceHost::loadState(const char* path) {
  if (!loaded_ || !path || !path[0]) return false;
  void* state = heap_caps_malloc(DOLL_GBA_STATE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!state) return false;
  const bool read = readExactFile(path, state, DOLL_GBA_STATE_BYTES);
  const bool ok = read && doll_gba_core_load_state(state, DOLL_GBA_STATE_BYTES);
  heap_caps_free(state);
  return ok;
}
