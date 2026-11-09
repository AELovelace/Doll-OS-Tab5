#pragma once

#include <Arduino.h>

// Minimal host boundary around the vendored GPLv2 gnuboy core. Its CPU-rendered
// framebuffer and cart data live in PSRAM; audio scratch stays internal for I2S.
class GameBoyHost {
 public:
  static constexpr int16_t kWidth = 160;
  static constexpr int16_t kHeight = 144;

  enum Button : uint8_t {
    kRight = 0x01, kLeft = 0x02, kUp = 0x04, kDown = 0x08,
    kA = 0x10, kB = 0x20, kSelect = 0x40, kStart = 0x80,
  };

  bool begin();
  bool load(const String& romPath, const String& savePath);
  void stop();
  void runFrame(bool draw = true);
  void setButtons(uint8_t buttons);
  void tickSave();
  // Full-machine snapshots (gnuboy's own serializer). Safe between frames.
  bool saveState(const char* path);
  bool loadState(const char* path);

  bool loaded() const { return loaded_; }
  const uint16_t* frame() const { return frame_; }
  const String& status() const { return status_; }

 private:
  uint16_t* frame_ = nullptr;
  int16_t* soundScratch_ = nullptr;
  String savePath_;
  String status_;
  uint32_t dirtyAtMs_ = 0;
  uint32_t lastSaveMs_ = 0;
  bool ready_ = false;
  bool loaded_ = false;
  bool savePending_ = false;
};
