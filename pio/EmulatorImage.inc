// Dedicated ota_1 entrypoint. It intentionally supplies only the small set of
// board services used by GameboyAdvance.ino, so shell/network/OG-Game-Boy/
// radio globals never enter this image's static memory map.

#include <Arduino.h>
#include <FS.h>
#include <M5Unified.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <hal/axi_icm_ll.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "../BoardPins.h"
#include "../src/EmulatorBoot.h"

constexpr int DISPLAY_WIDTH = 1280;
constexpr int DISPLAY_HEIGHT = 720;
constexpr int DISPLAY_TEXT_SIZE = 2;
constexpr int RADIO_VOLUME_MAX = 21;
constexpr int C_RESET = 0;
constexpr int C_WHITE = 37;
constexpr int C_RED = 31;
constexpr int C_GREEN = 32;
constexpr int C_YELLOW = 33;
constexpr int C_CYAN = 36;

auto& tft = M5.Display;
M5Canvas frameSprite(&M5.Display);
bool displayDirty = false;
bool dappCanvasActive = false;
bool sdCardMounted = false;
bool emulatorDisplayReady = false;
String cwd = "/";
const String SD_MOUNT = "/sd";
String currentCommand;

enum LineInputResult { LINE_NO_INPUT, LINE_EDITING, LINE_SUBMITTED };
enum UserEscState { UESC_NONE, UESC_GOT_ESC, UESC_GOT_CSI };
struct LineEditState {
    UserEscState escState = UESC_NONE;
    String escParams;
    bool lastByteWasCR = false;
};

struct RoutedPath {
    fs::FS* fs;
    String realPath;
    bool isSd;
};

static SemaphoreHandle_t emulatorI2cMutex = nullptr;
static int emulatorVolume = 12;

void ledPulseError() { Serial.println("[emulator] activity error"); }
void ledPulseInput() {}
void ledPulseStorageRead(bool) {}
void ledSetKeyboardActive(bool) {}
void ledSetSdMounted(bool) {}
void ledService() {}

void boardI2cBegin() {
    if (emulatorI2cMutex == nullptr) {
        emulatorI2cMutex = xSemaphoreCreateRecursiveMutex();
    }
}  // Serializes the codec and Tab5 panel expander on their shared internal bus.

bool boardI2cLock(uint32_t timeoutMs) {
    return emulatorI2cMutex == nullptr ||
           xSemaphoreTakeRecursive(emulatorI2cMutex,
                                   pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void boardI2cUnlock() {
    if (emulatorI2cMutex != nullptr) {
        xSemaphoreGiveRecursive(emulatorI2cMutex);
    }
}

int radioGetVolume() { return emulatorVolume; }

void radioAdjustVolume(int delta) {
    emulatorVolume = constrain(emulatorVolume + delta, 0, RADIO_VOLUME_MAX);
}  // Keeps the in-game menu's live mixer control without starting radio services.

bool radioReleaseAudio() { return true; }

static constexpr uint8_t EMULATOR_ES8388_ADDRESS = 0x10;
static constexpr uint8_t EMULATOR_PI4IO1_ADDRESS = 0x43;
static bool emulatorCodecReady = false;
static uint16_t emulatorCodecMclk = 0;

static bool emulatorCodecWrite(uint8_t reg, uint8_t value) {
    if (!boardI2cLock(1000)) return false;
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
        ok = M5.In_I2C.writeRegister8(
            EMULATOR_ES8388_ADDRESS, reg, value, AUDIO_I2C_SPEED);
        if (!ok) delay(1);
    }
    boardI2cUnlock();
    return ok;
}  // Retries codec writes while excluding panel-expander transactions.

static bool emulatorCodecSetAmp(bool enabled) {
    if (!boardI2cLock(1000)) return false;
    bool ok = true;
    if (enabled) {
        ok = M5.In_I2C.bitOn(EMULATOR_PI4IO1_ADDRESS, 0x03, 0x02,
                             AUDIO_I2C_SPEED) && ok;
        ok = M5.In_I2C.bitOff(EMULATOR_PI4IO1_ADDRESS, 0x07, 0x02,
                              AUDIO_I2C_SPEED) && ok;
    }
    ok = (enabled
        ? M5.In_I2C.bitOn(EMULATOR_PI4IO1_ADDRESS, 0x05, 0x02,
                          AUDIO_I2C_SPEED)
        : M5.In_I2C.bitOff(EMULATOR_PI4IO1_ADDRESS, 0x05, 0x02,
                           AUDIO_I2C_SPEED)) && ok;
    boardI2cUnlock();
    return ok;
}  // Drives the Tab5 amplifier without disturbing LCD/touch reset bits.

void audioCodecSetOutputEnabled(bool enabled) {
    emulatorCodecSetAmp(enabled);
}

void audioCodecForceReinit() {
    emulatorCodecReady = false;
    emulatorCodecMclk = 0;
}

bool audioCodecEnsure(uint16_t mclkMultiple) {
    struct RegisterValue { uint8_t reg; uint8_t value; };
    static constexpr RegisterValue initRegisters[] = {
        {0, 0x0E}, {1, 0x00}, {2, 0x0A}, {3, 0xFF}, {4, 0x3C},
        {5, 0x00}, {6, 0x00}, {7, 0x7C}, {8, 0x00},
        {23, 0x18}, {25, 0x20}, {26, 0x00}, {27, 0x00},
        {28, 0x08}, {29, 0x00}, {38, 0x00}, {39, 0xB8},
        {42, 0xB8}, {43, 0x08}, {45, 0x00}, {46, 0x21},
        {47, 0x21}, {48, 0x21}, {49, 0x21},
    };

    if (!emulatorCodecReady) {
        if (!emulatorCodecWrite(0, 0x80)) return false;
        delay(2);
        if (!emulatorCodecWrite(0, 0x00)) return false;
        delay(10);
        for (const auto& setting : initRegisters) {
            if (!emulatorCodecWrite(setting.reg, setting.value)) return false;
        }
        emulatorCodecReady = true;
        Serial.println("[audio] emulator ES8388 codec up");
    }

    const uint8_t ratio = mclkMultiple == 128 ? 0x00
                           : mclkMultiple == 384 ? 0x03 : 0xFF;
    if (ratio == 0xFF) return false;
    if (emulatorCodecMclk != mclkMultiple) {
        if (!emulatorCodecWrite(24, ratio)) return false;
        emulatorCodecMclk = mclkMultiple;
    }
    return emulatorCodecSetAmp(true);
}  // Programs the same ES8388 path as Doll-OS without linking the radio engine.

void displayInvalidateShadow() {}

void pushDisplayFrame() {
    if (frameSprite.getBuffer() == nullptr) return;
    frameSprite.pushSprite(0, 0);
    tft.display();
}  // Presents the GB menu canvas without the Doll-OS shadow/diff machinery.

void drawDisplayFrame() { pushDisplayFrame(); }
void printPrompt() {}
void setActiveInput(const String&, const String&, bool) {}
String shellPrompt() { return "> "; }
void commandProcessor(String&) {}
int telnetReadFilteredByte() { return -1; }

LineInputResult processLineEditByte(String&, uint8_t, LineEditState&, bool) {
    return LINE_NO_INPUT;
}  // Satisfies keyboard shell adapters that are dead-stripped from this image.

void outLine(const String& line, int = C_WHITE) {
    Serial.println(line);
}  // Routes inherited emulator status messages to the debug console.

String resolvePath(const String& base, const String& input) {
    if (input.startsWith("/")) return input;
    return base == "/" ? "/" + input : base + "/" + input;
}

RoutedPath routePath(const String& path) {
    const bool isSd = path == SD_MOUNT || path.startsWith(SD_MOUNT + "/");
    if (isSd) {
        String real = path.substring(SD_MOUNT.length());
        if (real.isEmpty()) real = "/";
        return {&SD_MMC, real, true};
    }
    return {&SD_MMC, path, false};
}  // Exists only for unused picker helpers; launch records already carry VFS paths.

bool initSdStorageOnly() {
    if (sdCardMounted) return true;
    SD_MMC.setPins(SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN,
                   SD_MMC_D1_PIN, SD_MMC_D2_PIN, SD_MMC_D3_PIN);
    sdCardMounted = SD_MMC.begin("/sdcard", false, false);
    Serial.printf("[emulator] SD %s\n", sdCardMounted ? "ready" : "not detected");
    return sdCardMounted;
}  // Mounts the shared ROM/save card without LittleFS or shell storage services.

void displayPrepareForRestart() {
    tft.setBrightness(0);
    tft.sleep();
    delay(20);
    bool resetHeld = false;
    if (boardI2cLock(1000)) {
        resetHeld = M5.In_I2C.bitOff(0x43, 0x05, 0x30, 100000);
        boardI2cUnlock();
    }
    Serial.printf("[display] restart fence: panel reset %s\n",
                  resetHeld ? "held low" : "write failed");
    delay(20);
}  // Prevents the independently powered DSI receiver from latching across reboot.

static void emulatorReturnToOs(const char* reason) {
    Serial.printf("[emulator boot] %s; returning to Doll-OS\n", reason);
    doll::emulator::clear();
    String error;
    if (!doll::emulator::selectPartition(
            doll::emulator::kOsPartitionLabel, &error)) {
        Serial.printf("[emulator boot] %s\n", error.c_str());
        delay(5000);
        esp_restart();
    }
    if (emulatorDisplayReady) {
        displayPrepareForRestart();
    }
    Serial.flush();
    delay(50);
    esp_restart();
}  // Makes missing, stale, and crashed launch records self-recovering.

#include "../KeyboardSerial.ino"
#include "../SlaveLink.ino"
#include "../GameboyAdvance.ino"

static void prioritizeEmulatorDisplay() {
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(0, 0, 15);
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(1, 0, 15);
    axi_icm_ll_set_cpu_qos_arbiter_prio(14, 14);
}  // Mirrors the known-good Doll-OS DSI/SDMMC arbitration priorities.

void setup() {
    Serial.begin(115200);
    delay(50);

    doll::emulator::LaunchRecord launch{};
    String launchError;
    if (!doll::emulator::claim(launch, launchError)) {
        emulatorReturnToOs(launchError.c_str());
        return;
    }
    emulatorVolume = launch.volume;
    Serial.printf("[emulator boot] claimed %s ROM %s\n",
                  doll::emulator::kindName(launch.kind), launch.romPath);

    auto config = M5.config();
    config.serial_baudrate = 0;
    config.clear_display = true;
    config.output_power = true;
    config.internal_imu = false;
    config.internal_rtc = false;
    config.internal_mic = false;
    config.internal_spk = false;
    config.external_imu = false;
    config.external_rtc = false;
    M5.begin(config);
    emulatorDisplayReady = true;
    prioritizeEmulatorDisplay();
    M5.Power.setExtOutput(true);
    boardI2cBegin();

    if (psramFound()) {
        heap_caps_malloc_extmem_enable(512);
    }
    if (!initSdStorageOnly()) {
        emulatorReturnToOs("SD card unavailable");
        return;
    }
    initKeyboardSerial();
    slaveLinkBegin();

    const esp_err_t otaValidation = esp_ota_mark_app_valid_cancel_rollback();
    if (otaValidation != ESP_OK) {
        emulatorReturnToOs(esp_err_to_name(otaValidation));
        return;
    }  // Commits ota_1 only after launch recovery and required hardware are ready.

    gbaInitMinimalDisplay();
    gbaRunBootMode(launch);
    emulatorReturnToOs("Game Boy Advance session returned unexpectedly");
}

void loop() {
    delay(1000);
}
