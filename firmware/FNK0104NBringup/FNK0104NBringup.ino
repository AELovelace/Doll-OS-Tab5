#include <Arduino.h>
#include <SD_MMC.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <esp32-hal-rgb-led.h>
#include <esp_idf_version.h>
#include <esp_task_wdt.h>
#include "ST77922.h"
#include "../../BoardPins.h"

#ifndef FNK0104N_3P5_320x480_ST77922
#error "FNK0104NBringup requires the N variant in BoardVariant.h"
#endif

static TFT_eSPI spriteHost;
static TFT_eSprite frame(&spriteHost);
static ST77922 panel;

static void setTaskWdtTimeout(uint32_t timeoutMs) {
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t config = {
        .timeout_ms = timeoutMs,
        .idle_core_mask = 0x3,
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&config);
#else
    esp_task_wdt_init(timeoutMs / 1000, true);
#endif
}

static void printI2cDevices() {
    Serial.printf("I2C SDA=%d SCL=%d:", AUDIO_I2C_SDA_PIN, AUDIO_I2C_SCL_PIN);
    int found = 0;
    for (uint8_t address = 0x08; address <= 0x77; address++) {
        Wire.beginTransmission(address);
        if (Wire.endTransmission() == 0) {
            Serial.printf(" 0x%02X", address);
            found++;
        }
    }
    if (found == 0) Serial.print(" no devices");
    Serial.println();
    Serial.println("ES8311 should answer at 0x18 or 0x19.");
}

static bool testDisplay() {
    panel.Init();
    panel.Set_Rotation(0);

    frame.setColorDepth(16);
    if (!frame.createSprite(480, 320)) {
        Serial.println("Display: 480x320 sprite allocation failed; check OPI PSRAM setting");
        return false;
    }
    frame.setSwapBytes(true);
    frame.fillSprite(TFT_NAVY);
    frame.fillRect(0, 0, 160, 320, TFT_RED);
    frame.fillRect(160, 0, 160, 320, TFT_GREEN);
    frame.fillRect(320, 0, 160, 320, TFT_BLUE);
    frame.setTextColor(TFT_WHITE, TFT_BLUE);
    frame.setTextDatum(MC_DATUM);
    frame.drawString("FNK0104N", 400, 160, 4);
    panel.Fill_Colors_Landscape(0, 0, 480, 320, (uint16_t*)frame.getPointer());

    // Exercise a second update through Freenove's supported full-frame path.
    frame.fillRect(180, 250, 120, 40, TFT_BLACK);
    frame.setTextColor(TFT_YELLOW, TFT_BLACK);
    frame.setTextDatum(MC_DATUM);
    frame.drawString("REDRAW OK", 240, 270, 2);
    panel.Fill_Colors_Landscape(0, 0, 480, 320, (uint16_t*)frame.getPointer());
    return true;
}

static void testStorage() {
    SD_MMC.setPins(SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN,
                   SD_MMC_D1_PIN, SD_MMC_D2_PIN, SD_MMC_D3_PIN);
    // A missing card can keep the SDMMC OCR retry loop busy longer than the
    // default task watchdog. Match the full firmware's guarded mount.
    setTaskWdtTimeout(20000);
    bool mounted = SD_MMC.begin("/sdcard", false, false);
    setTaskWdtTimeout(5000);
    Serial.printf("SD_MMC: %s\n", mounted ? "mounted" : "not detected");
    if (!mounted) return;

    File root = SD_MMC.open("/");
    int shown = 0;
    while (root && shown < 8) {
        File entry = root.openNextFile();
        if (!entry) break;
        Serial.printf("  %s%s\n", entry.isDirectory() ? "[DIR] " : "      ", entry.name());
        entry.close();
        shown++;
    }
    if (root) root.close();
}

void setup() {
    Serial.begin(115200);
    uint32_t serialWaitStarted = millis();
    while (!Serial && millis() - serialWaitStarted < 10000) {
        delay(10);
    }
    delay(250);
    Serial.printf("\nDOLL-OS %s hardware bring-up\n", DOLL_BOARD_NAME);
    Serial.printf("PSRAM: %s, %u bytes\n", psramFound() ? "ready" : "not found",
                  (unsigned)ESP.getPsramSize());

    rgbLedWriteOrdered(DOLL_REAR_RGB_LED_PIN, LED_COLOR_ORDER_GRB, 0, 32, 0);
    Serial.printf("Rear RGB GPIO=%d: should be dim green\n", DOLL_REAR_RGB_LED_PIN);
    pinMode(KEYBOARD_SERIAL_RX_PIN, INPUT_PULLUP);
    pinMode(SLAVE_LINK_TX_PIN, OUTPUT);
    digitalWrite(SLAVE_LINK_TX_PIN, HIGH);
    Serial.printf("DS-Slave RX=%d TX=%d\n", KEYBOARD_SERIAL_RX_PIN, SLAVE_LINK_TX_PIN);

    bool displayOk = testDisplay();
    testStorage();

    Wire.begin(AUDIO_I2C_SDA_PIN, AUDIO_I2C_SCL_PIN, AUDIO_I2C_SPEED);
    printI2cDevices();

    pinMode(BATTERY_ADC_PIN, INPUT);
    float battery = analogReadMilliVolts(BATTERY_ADC_PIN) * 2.0f / 1000.0f;
    Serial.printf("Battery: %.2f V\n", battery);
    Serial.printf("Bring-up complete; display=%s.\n", displayOk ? "ok" : "failed");
}

void loop() {
    delay(2000);
}
