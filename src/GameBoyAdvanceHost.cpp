#include "GameBoyAdvanceHost.h"

#include "AudioOut.h"
#include "emulator/gpsp/doll_gba_bridge.h"
#include "esp_heap_caps.h"

namespace {
constexpr size_t kAudioMaxFrames = 600;
constexpr uint32_t kSaveIntervalMs = 30000;
constexpr uint32_t kWorkerSave = 1U << 0;
constexpr uint32_t kWorkerPresent = 1U << 1;
constexpr uint32_t kWorkerStop = 1U << 2;
constexpr uint32_t kWorkerInput = 1U << 3;

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
  // expanded batch predecoder can use internal L2 for hot ROM blocks.
  frames_[0] = static_cast<uint16_t*>(allocBuffer(DOLL_GBA_FRAME_BYTES, false));
  frames_[1] = static_cast<uint16_t*>(allocBuffer(DOLL_GBA_FRAME_BYTES, false));
  stereoScratch_ = static_cast<int16_t*>(
      allocBuffer(kAudioMaxFrames * 2 * sizeof(int16_t), false));
  saveSnapshot_ = static_cast<uint8_t*>(allocBuffer(DOLL_GBA_SAVE_BYTES, false));
  return frames_[0] && frames_[1] && stereoScratch_ && saveSnapshot_;
}  // Allocates double-buffered video and one native-stereo audio transfer buffer.

void GameBoyAdvanceHost::releaseCoreMemory() {
  if (frames_[0]) heap_caps_free(frames_[0]);
  if (frames_[1]) heap_caps_free(frames_[1]);
  if (stereoScratch_) heap_caps_free(stereoScratch_);
  if (saveSnapshot_) heap_caps_free(saveSnapshot_);
  frames_[0] = nullptr;
  frames_[1] = nullptr;
  stereoScratch_ = nullptr;
  saveSnapshot_ = nullptr;
}  // Releases every foreground-only GBA host allocation.

bool GameBoyAdvanceHost::begin() {
  if (ready_) return true;
  // Reserve the low-priority save task's small internal stack before gpSP asks
  // L2 for its foreground-only working set.
  renderFrame_ = 0;
  if (!allocateCoreMemory() || !startSaveTask() || !doll_gba_core_begin(frames_[0])) {
    stopSaveTask();
    doll_gba_core_stop();
    releaseCoreMemory();
    status_ = "GBA memory allocation failed";
    return false;
  }
  ready_ = true;
  status_ = "GBA core ready";
  doll_gba_perf_stats_t perf = {};
  doll_gba_core_get_perf(&perf);
  Serial.printf("[gba] P4 gpSP core ready, 8 MB ROM cache, predecode=%luK, JIT=off\n",
                static_cast<unsigned long>(perf.thumb_predecode_bytes / 1024));
  return true;
}

bool GameBoyAdvanceHost::load(const String& romPath, const String& savePath) {
  if (loaded_) stop();
  if (!begin()) return false;
  if (!doll_gba_core_load(romPath.c_str())) {
    status_ = "GBA ROM load failed";
    stopSaveTask();
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
    memcpy(saveSnapshot_, saveData, DOLL_GBA_SAVE_BYTES);
  }

  buttons_ = 0;
  audioRemainder_ = 0;
  lastSaveMs_ = millis();
  loaded_ = true;
  status_ = "Playing " + romPath;
  return true;
}

bool GameBoyAdvanceHost::startSaveTask() {
  saveBusy_ = false;
  saveFailed_ = false;
  saveStop_ = false;
  saveStopped_ = false;
  presentationBusy_ = false;
  inputBusy_ = false;
  presentedFrames_ = 0;
  presentationTimeUs_ = 0;
  inputPollTimeUs_ = 0;
  polledButtons_ = 0;
  inputEvents_ = 0;
  if (xTaskCreatePinnedToCore(saveTaskEntry, "gba_save", 4096, this, 1,
                              &saveTask_, 0) == pdPASS) {
    return true;
  }
  saveTask_ = nullptr;
  saveStopped_ = true;
  return false;
}

void GameBoyAdvanceHost::saveTaskEntry(void* argument) {
  auto* host = static_cast<GameBoyAdvanceHost*>(argument);
  bool predecodePending = false;
  for (;;) {
    uint32_t notifications = 0;
    xTaskNotifyWait(0, UINT32_MAX, &notifications,
                    predecodePending ? 0 : portMAX_DELAY);
    if ((notifications & kWorkerStop) ||
        __atomic_load_n(&host->saveStop_, __ATOMIC_ACQUIRE)) {
      break;
    }

    // Poll first for low input latency, then present before a rare save write.
    // Both board operations remain serialized on core 0, while core 1 consumes
    // only atomic button/event snapshots and never waits on the touch bus.
    if (notifications & kWorkerInput) {
      uint16_t buttons = 0;
      const uint32_t startedUs = micros();
      const uint8_t events = host->inputPoller_
          ? host->inputPoller_(buttons) : 0;
      __atomic_add_fetch(&host->inputPollTimeUs_, micros() - startedUs,
                         __ATOMIC_RELAXED);
      __atomic_store_n(&host->polledButtons_, buttons, __ATOMIC_RELEASE);
      if (events) __atomic_fetch_or(&host->inputEvents_, events, __ATOMIC_RELEASE);
      __atomic_store_n(&host->inputBusy_, false, __ATOMIC_RELEASE);
    }

    // Present before a rare save write so display work stays predictably ahead
    // of the next emulated frame. The frontend never queues a second frame
    // until presentationBusy_ clears, so both PSRAM buffers have one owner.
    if (notifications & kWorkerPresent) {
      const uint8_t frameIndex =
          __atomic_load_n(&host->pendingFrame_, __ATOMIC_ACQUIRE);
      const uint32_t startedUs = micros();
      if (host->framePresenter_) host->framePresenter_(host->frames_[frameIndex]);
      __atomic_add_fetch(&host->presentationTimeUs_, micros() - startedUs,
                         __ATOMIC_RELAXED);
      __atomic_add_fetch(&host->presentedFrames_, 1U, __ATOMIC_RELEASE);
      __atomic_store_n(&host->presentationBusy_, false, __ATOMIC_RELEASE);
    }

    if (notifications & kWorkerSave) {
      const bool ok = writeExactFile(host->savePath_.c_str(), host->saveSnapshot_,
                                     DOLL_GBA_SAVE_BYTES);
      __atomic_store_n(&host->saveFailed_, !ok, __ATOMIC_RELEASE);
      __atomic_store_n(&host->saveBusy_, false, __ATOMIC_RELEASE);
    }

    // The emulation core only queues immutable ROM snapshots. Decode a bounded
    // group on core 0 and immediately loop while work remains, still checking
    // task notifications between groups so touch and DSI presentation stay
    // responsive. Core 1 never waits for a decoded block.
    predecodePending = doll_gba_core_predecode_worker(8) != 0;
  }
  __atomic_store_n(&host->saveStopped_, true, __ATOMIC_RELEASE);
  vTaskDelete(nullptr);
}

void GameBoyAdvanceHost::waitForSaveIdle() {
  while (__atomic_load_n(&saveBusy_, __ATOMIC_ACQUIRE)) vTaskDelay(1);
}

void GameBoyAdvanceHost::queueSaveIfDirty() {
  if (!loaded_ || !saveTask_ || !saveSnapshot_ || savePath_.isEmpty() ||
      __atomic_load_n(&saveBusy_, __ATOMIC_ACQUIRE)) {
    return;
  }

  const auto* saveData = static_cast<const uint8_t*>(doll_gba_core_save_data());
  const bool retry = __atomic_load_n(&saveFailed_, __ATOMIC_ACQUIRE);
  if (!saveData || (!retry && memcmp(saveSnapshot_, saveData, DOLL_GBA_SAVE_BYTES) == 0)) {
    return;
  }

  memcpy(saveSnapshot_, saveData, DOLL_GBA_SAVE_BYTES);
  __atomic_store_n(&saveFailed_, false, __ATOMIC_RELEASE);
  __atomic_store_n(&saveBusy_, true, __ATOMIC_RELEASE);
  xTaskNotify(saveTask_, kWorkerSave, eSetBits);
}

void GameBoyAdvanceHost::setFramePresenter(FramePresenter presenter) {
  waitForPresentIdle();
  framePresenter_ = presenter;
}

void GameBoyAdvanceHost::setInputPoller(InputPoller poller) {
  waitForPresentIdle();
  inputPoller_ = poller;
}

bool GameBoyAdvanceHost::presentationReady() const {
  return loaded_ && saveTask_ && framePresenter_ &&
      !__atomic_load_n(&presentationBusy_, __ATOMIC_ACQUIRE);
}

bool GameBoyAdvanceHost::queueFrameForPresent() {
  if (!presentationReady()) return false;

  const uint8_t completedFrame = renderFrame_;
  const uint8_t nextFrame = completedFrame ^ 1U;
  if (!doll_gba_core_set_framebuffer(frames_[nextFrame])) return false;
  renderFrame_ = nextFrame;
  __atomic_store_n(&pendingFrame_, completedFrame, __ATOMIC_RELAXED);
  __atomic_store_n(&presentationBusy_, true, __ATOMIC_RELEASE);
  xTaskNotify(saveTask_, kWorkerPresent, eSetBits);
  return true;
}

bool GameBoyAdvanceHost::queueInputPoll() {
  if (!loaded_ || !saveTask_ || !inputPoller_ ||
      __atomic_load_n(&inputBusy_, __ATOMIC_ACQUIRE)) {
    return false;
  }
  __atomic_store_n(&inputBusy_, true, __ATOMIC_RELEASE);
  xTaskNotify(saveTask_, kWorkerInput, eSetBits);
  return true;
}

uint16_t GameBoyAdvanceHost::polledButtons() const {
  return __atomic_load_n(&polledButtons_, __ATOMIC_ACQUIRE);
}

uint8_t GameBoyAdvanceHost::takeInputEvents() {
  return __atomic_exchange_n(&inputEvents_, 0, __ATOMIC_ACQ_REL);
}

void GameBoyAdvanceHost::waitForPresentIdle() {
  while (__atomic_load_n(&presentationBusy_, __ATOMIC_ACQUIRE) ||
         __atomic_load_n(&inputBusy_, __ATOMIC_ACQUIRE)) {
    vTaskDelay(1);
  }
}

uint32_t GameBoyAdvanceHost::presentedFrames() const {
  return __atomic_load_n(&presentedFrames_, __ATOMIC_ACQUIRE);
}

uint32_t GameBoyAdvanceHost::presentationTimeUs() const {
  return __atomic_load_n(&presentationTimeUs_, __ATOMIC_ACQUIRE);
}

uint32_t GameBoyAdvanceHost::inputPollTimeUs() const {
  return __atomic_load_n(&inputPollTimeUs_, __ATOMIC_ACQUIRE);
}

bool GameBoyAdvanceHost::writeFinalSaveIfDirty() {
  const auto* saveData = static_cast<const uint8_t*>(doll_gba_core_save_data());
  if (!loaded_ || !saveData || !saveSnapshot_ || savePath_.isEmpty()) return true;
  if (!__atomic_load_n(&saveFailed_, __ATOMIC_ACQUIRE) &&
      memcmp(saveSnapshot_, saveData, DOLL_GBA_SAVE_BYTES) == 0) {
    return true;
  }
  memcpy(saveSnapshot_, saveData, DOLL_GBA_SAVE_BYTES);
  return writeExactFile(savePath_.c_str(), saveSnapshot_, DOLL_GBA_SAVE_BYTES);
}

void GameBoyAdvanceHost::stopSaveTask() {
  if (!saveTask_) return;
  waitForPresentIdle();
  waitForSaveIdle();
  (void)writeFinalSaveIfDirty();
  __atomic_store_n(&saveStop_, true, __ATOMIC_RELEASE);
  xTaskNotify(saveTask_, kWorkerStop, eSetBits);
  while (!__atomic_load_n(&saveStopped_, __ATOMIC_ACQUIRE)) vTaskDelay(1);
  saveTask_ = nullptr;
}

void GameBoyAdvanceHost::stop() {
  stopSaveTask();
  loaded_ = false;
  doll_gba_core_stop();
  releaseCoreMemory();
  ready_ = false;
  lastCoreTimeUs_ = 0;
  lastAudioTimeUs_ = 0;
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
  // The core intentionally withholds its newest samples while interpolation can
  // still touch them. Pad that short startup read so I2S receives a full clocked
  // frame and the preloaded DMA cushion does not drain before audio catches up.
  AudioOut::onStereoSamples(stereoScratch_, produced, frames);
}  // Transfers exactly one emulated frame of paced audio to the board sink.

void GameBoyAdvanceHost::runFrame(bool draw) {
  if (!loaded_) return;
  const uint32_t coreStartedUs = micros();
  doll_gba_core_run(buttons_, draw);
  lastCoreTimeUs_ = micros() - coreStartedUs;
  const uint32_t audioStartedUs = micros();
  submitAudio();
  lastAudioTimeUs_ = micros() - audioStartedUs;
}

void GameBoyAdvanceHost::setButtons(uint16_t buttons) {
  buttons_ = buttons;
}

void GameBoyAdvanceHost::tickSave() {
  if (!loaded_ || savePath_.isEmpty()) return;
  const uint32_t now = millis();
  if (now - lastSaveMs_ >= kSaveIntervalMs &&
      !__atomic_load_n(&saveBusy_, __ATOMIC_ACQUIRE)) {
    queueSaveIfDirty();
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
