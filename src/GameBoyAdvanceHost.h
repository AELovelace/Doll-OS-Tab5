#pragma once

#include <Arduino.h>

// Doll-OS host boundary for the vendored P4 gpSP core. The core remains free
// of Tab5 board drivers; the command layer owns display, input and audio.
class GameBoyAdvanceHost {
 public:
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
  void tickSave();
  bool saveState(const char* path);
  bool loadState(const char* path);

  bool loaded() const { return loaded_; }
  const uint16_t* frame() const { return frame_; }
  const String& status() const { return status_; }

 private:
  bool allocateCoreMemory();
  void releaseCoreMemory();
  bool writeSave();
  void submitAudio();

  uint16_t* frame_ = nullptr;
  int16_t* stereoScratch_ = nullptr;
  int16_t* monoScratch_ = nullptr;
  String savePath_;
  String status_;
  uint16_t buttons_ = 0;
  uint64_t audioRemainder_ = 0;
  uint32_t lastSaveMs_ = 0;
  bool ready_ = false;
  bool loaded_ = false;
};
