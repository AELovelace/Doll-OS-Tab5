#ifndef DOLL_OS_TAB5_BOARD_PINS_H
#define DOLL_OS_TAB5_BOARD_PINS_H

#include "BoardVariant.h"

static constexpr const char* DOLL_BOARD_NAME = "M5Stack Tab5";

// The Tab5 microSD slot is wired to the ESP32-P4 SDMMC peripheral.
static constexpr int SD_MMC_CLK_PIN = 43;
static constexpr int SD_MMC_CMD_PIN = 44;
static constexpr int SD_MMC_D0_PIN = 39;
static constexpr int SD_MMC_D1_PIN = 40;
static constexpr int SD_MMC_D2_PIN = 41;
static constexpr int SD_MMC_D3_PIN = 42;

// The official Tab5 Keyboard uses the dedicated Ext.Port1 connection.
static constexpr int TAB5_KEYBOARD_SDA_PIN = 0;
static constexpr int TAB5_KEYBOARD_SCL_PIN = 1;
static constexpr int TAB5_KEYBOARD_INTERRUPT_PIN = 50;

// Rotation 3 presents this Tab5 panel as upright 1280x720 landscape.
static constexpr int TAB5_DISPLAY_ROTATION = 3;
// The adjacent rotation presents the same panel as 720x1280 portrait. Keeping
// this explicit makes the status-bar tablet toggle easy to reverse on hardware
// if a later panel revision reports its native orientation differently.
static constexpr int TAB5_DISPLAY_PORTRAIT_ROTATION = 2;

// ESP32-P4 reaches the onboard ESP32-C6 radio through this SDIO2 bus.
static constexpr int WIFI_SDIO_CLK_PIN = 12;
static constexpr int WIFI_SDIO_CMD_PIN = 13;
static constexpr int WIFI_SDIO_D0_PIN = 11;
static constexpr int WIFI_SDIO_D1_PIN = 10;
static constexpr int WIFI_SDIO_D2_PIN = 9;
static constexpr int WIFI_SDIO_D3_PIN = 8;
static constexpr int WIFI_SDIO_RESET_PIN = 15;

// ES8388 playback and ES7210 capture share the Tab5 audio clocks and I2C bus.
static constexpr int AUDIO_I2S_MCLK_PIN = 30;
static constexpr int AUDIO_I2S_BCLK_PIN = 27;
static constexpr int AUDIO_I2S_DIN_PIN = 28;
static constexpr int AUDIO_I2S_DOUT_PIN = 26;
static constexpr int AUDIO_I2S_WS_PIN = 29;
static constexpr int AUDIO_I2C_SCL_PIN = 32;
static constexpr int AUDIO_I2C_SDA_PIN = 31;
static constexpr int AUDIO_AMP_ENABLE_PIN = -1;
static constexpr uint32_t AUDIO_I2C_SPEED = 400000;

// Tab5 indicator and amplifier power are controlled through onboard expanders.
#define DOLL_REAR_RGB_LED_PIN -1

#endif  // DOLL_OS_TAB5_BOARD_PINS_H
