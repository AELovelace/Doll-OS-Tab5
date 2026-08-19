#include "EmulatorBoot.h"

#include <Preferences.h>
#include <cstring>
#include <esp_err.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

namespace doll::emulator {
namespace {

constexpr uint32_t kMagic = 0x554D4544;  // "DEMU" identifies a Doll emulator launch.
constexpr uint16_t kVersion = 1;
constexpr const char* kNamespace = "emu_boot";
constexpr const char* kRecordKey = "launch";

uint32_t checksum(const LaunchRecord& record) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261U;
  for (size_t i = 0; i < offsetof(LaunchRecord, checksum); ++i) {
    hash = (hash ^ bytes[i]) * 16777619U;
  }
  return hash;
}  // Seals every persisted field so interrupted NVS writes fail closed.

bool valid(const LaunchRecord& record) {
  const bool knownKind = record.kind == Kind::GameBoyAdvance;
  const bool knownPhase = record.phase == Phase::Pending ||
                          record.phase == Phase::Running;
  return record.magic == kMagic && record.version == kVersion && knownKind &&
         knownPhase && record.scale >= 1 && record.scale <= 3 &&
         record.frameSkip >= -1 && record.frameSkip <= 5 &&
         record.volume <= 21 && record.romPath[0] != '\0' &&
         record.romPath[kLaunchPathMax - 1] == '\0' &&
         std::strncmp(record.romPath, "/sdcard/", 8) == 0 &&
         record.checksum == checksum(record);
}  // Rejects stale formats, corrupt writes, and paths outside the shared SD VFS.

bool writeRecord(const LaunchRecord& record) {
  Preferences preferences;
  if (!preferences.begin(kNamespace, false)) {
    return false;
  }
  const size_t written = preferences.putBytes(kRecordKey, &record, sizeof(record));
  preferences.end();
  return written == sizeof(record);
}  // Replaces the launch atomically through NVS's journaled blob storage.

}  // namespace

bool read(LaunchRecord& record) {
  std::memset(&record, 0, sizeof(record));
  Preferences preferences;
  if (!preferences.begin(kNamespace, true)) {
    return false;
  }
  const size_t stored = preferences.getBytesLength(kRecordKey);
  const size_t loaded = stored == sizeof(record)
      ? preferences.getBytes(kRecordKey, &record, sizeof(record)) : 0;
  preferences.end();
  return loaded == sizeof(record) && valid(record);
}

bool schedule(Kind kind, const String& romPath, uint8_t scale,
              int8_t frameSkip, uint8_t volume) {
  if (kind != Kind::GameBoyAdvance ||
      !romPath.startsWith("/sdcard/") || romPath.length() >= kLaunchPathMax) {
    return false;
  }

  LaunchRecord record{};
  record.magic = kMagic;
  record.version = kVersion;
  record.kind = kind;
  record.phase = Phase::Pending;
  record.scale = constrain(scale, 1, 3);
  record.frameSkip = constrain(frameSkip, -1, 5);
  record.volume = constrain(volume, 0, 21);
  std::memcpy(record.romPath, romPath.c_str(), romPath.length() + 1);
  record.checksum = checksum(record);
  return writeRecord(record);
}  // Persists everything the other firmware image needs before changing slots.

bool claim(LaunchRecord& record, String& error) {
  if (!read(record)) {
    error = "launch record missing or invalid";
    return false;
  }
  if (record.phase == Phase::Running) {
    error = "previous emulator boot did not exit cleanly";
    return false;
  }
  if (record.phase != Phase::Pending) {
    error = "launch record has an unsupported phase";
    return false;
  }

  record.phase = Phase::Running;
  record.checksum = checksum(record);
  if (!writeRecord(record)) {
    error = "could not arm emulator crash recovery";
    return false;
  }
  return true;
}  // Consumes PENDING before peripheral setup so the next reset returns to Doll-OS.

void clear() {
  Preferences preferences;
  if (!preferences.begin(kNamespace, false)) {
    return;
  }
  preferences.remove(kRecordKey);
  preferences.end();
}  // Prevents an intentional return to the OS from looking like a crash.

bool selectPartition(const char* label, String* error) {
  const esp_partition_t* partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
  if (partition == nullptr) {
    if (error) *error = "application partition '" + String(label) + "' was not found";
    return false;
  }
  const esp_err_t result = esp_ota_set_boot_partition(partition);
  if (result != ESP_OK) {
    if (error) {
      *error = "partition '" + String(label) + "' is not bootable: " +
               String(esp_err_to_name(result));
    }
    return false;
  }
  return true;
}  // Validates the target image and updates redundant OTA selection metadata.

const char* kindName(Kind kind) {
  switch (kind) {
    case Kind::GameBoyAdvance: return "Game Boy Advance";
    default: return "unknown emulator";
  }
}

}  // namespace doll::emulator
