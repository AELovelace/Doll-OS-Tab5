#pragma once

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

namespace doll::emulator {

constexpr const char* kOsPartitionLabel = "app0";
constexpr const char* kEmulatorPartitionLabel = "emulator";
constexpr size_t kLaunchPathMax = 384;

enum class Kind : uint8_t {
  None = 0,
  GameBoyAdvance = 2,
};

enum class Phase : uint8_t {
  Empty = 0,
  Pending = 1,
  Running = 2,
};

struct LaunchRecord {
  uint32_t magic;
  uint16_t version;
  Kind kind;
  Phase phase;
  uint8_t scale;
  int8_t frameSkip;
  uint8_t volume;
  uint8_t reserved;
  char romPath[kLaunchPathMax];
  uint32_t checksum;
};

bool schedule(Kind kind, const String& romPath, uint8_t scale,
              int8_t frameSkip, uint8_t volume);
bool claim(LaunchRecord& record, String& error);
bool read(LaunchRecord& record);
void clear();
bool selectPartition(const char* label, String* error = nullptr);
const char* kindName(Kind kind);

}  // namespace doll::emulator
