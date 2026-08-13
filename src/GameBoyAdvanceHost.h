#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Doll-OS host boundary for the vendored P4 gpSP core. The core remains free
// of Tab5 board drivers; the command layer owns display, input and audio.
class GameBoyAdvanceHost {
 public:
  using FramePresenter = void (*)(const uint16_t* frame);
  using InputPoller = uint8_t (*)(uint16_t& buttons);

  static constexpr int16_t kWidth = 240;
  static constexpr int16_t kHeight = 160;

  enum Button : uint16_t {
    kRight = 0x001,
    kLeft = 0x002,
    kUp = 0x004,
    kDown = 0x008,
    kA = 0x010,
    kB = 0x020,
    kSelect = 0x040,
    kStart = 0x080,
    kL = 0x100,
    kR = 0x200,
  };

  bool begin();
  bool load(const String& romPath, const String& savePath);
  void stop();
  void runFrame(bool draw = true);
  void setButtons(uint16_t buttons);
  void setFramePresenter(FramePresenter presenter);
  void setInputPoller(InputPoller poller);
  bool presentationReady() const;
  bool queueFrameForPresent();
  bool queueInputPoll();
  uint16_t polledButtons() const;
  uint8_t takeInputEvents();
  void waitForPresentIdle();
  void tickSave();
  bool saveState(const char* path);
  bool loadState(const char* path);

  bool loaded() const { return loaded_; }
  const uint16_t* frame() const { return frames_[renderFrame_]; }
  const String& status() const { return status_; }
  uint32_t lastCoreTimeUs() const { return lastCoreTimeUs_; }
  uint32_t lastAudioTimeUs() const { return lastAudioTimeUs_; }
  uint32_t presentedFrames() const;
  uint32_t presentationTimeUs() const;
  uint32_t inputPollTimeUs() const;

 private:
  bool allocateCoreMemory();
  void releaseCoreMemory();
  bool startSaveTask();
  void stopSaveTask();
  void waitForSaveIdle();
  void queueSaveIfDirty();
  bool writeFinalSaveIfDirty();
  static void saveTaskEntry(void* argument);
  void submitAudio();

  uint16_t* frames_[2] = {};
  int16_t* stereoScratch_ = nullptr;
  uint8_t* saveSnapshot_ = nullptr;
  TaskHandle_t saveTask_ = nullptr;
  FramePresenter framePresenter_ = nullptr;
  InputPoller inputPoller_ = nullptr;
  String savePath_;
  String status_;
  uint16_t buttons_ = 0;
  uint64_t audioRemainder_ = 0;
  uint32_t lastSaveMs_ = 0;
  uint32_t lastCoreTimeUs_ = 0;
  uint32_t lastAudioTimeUs_ = 0;
  volatile uint32_t presentedFrames_ = 0;
  volatile uint32_t presentationTimeUs_ = 0;
  volatile uint32_t inputPollTimeUs_ = 0;
  volatile uint16_t polledButtons_ = 0;
  volatile uint8_t inputEvents_ = 0;
  volatile uint8_t pendingFrame_ = 0;
  uint8_t renderFrame_ = 0;
  volatile bool presentationBusy_ = false;
  volatile bool inputBusy_ = false;
  volatile bool saveBusy_ = false;
  volatile bool saveFailed_ = false;
  volatile bool saveStop_ = false;
  volatile bool saveStopped_ = true;
  bool ready_ = false;
  bool loaded_ = false;
};
