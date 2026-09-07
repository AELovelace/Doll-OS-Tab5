#pragma once

#include "BoardVariant.h"

// Shared hardware map. Keep board-dependent pins here so sketch .ino files,
// native .cpp modules, and diagnostics cannot quietly select different wiring.
#ifdef FNK0104N_3P5_320x480_ST77922
static constexpr const char* DOLL_BOARD_NAME = "FNK0104N";

static constexpr int SD_MMC_CLK_PIN = 5;
static constexpr int SD_MMC_CMD_PIN = 4;
static constexpr int SD_MMC_D0_PIN = 6;
static constexpr int SD_MMC_D1_PIN = 7;
static constexpr int SD_MMC_D2_PIN = 2;
static constexpr int SD_MMC_D3_PIN = 3;
static constexpr int BATTERY_ADC_PIN = 8;

static constexpr int AUDIO_I2S_MCLK_PIN = 17;
static constexpr int AUDIO_I2S_BCLK_PIN = 18;
static constexpr int AUDIO_I2S_DIN_PIN = 16;
static constexpr int AUDIO_I2S_DOUT_PIN = 15;
static constexpr int AUDIO_I2S_WS_PIN = 21;
static constexpr int AUDIO_AMP_ENABLE_PIN = 1;
static constexpr int AUDIO_I2C_SCL_PIN = 39;
static constexpr int AUDIO_I2C_SDA_PIN = 38;

// GPIO21 and GPIO2 are occupied by audio WS and SD D2 on the N. GPIO45/46
// have no onboard role in Freenove's N pin map and become the DS-Slave pair.
// Keep GPIO45 as TX: an external UART TX idles high and must not drive GPIO45's
// VDD_SPI strap high during reset. GPIO46 is the safer receive side.
static constexpr int KEYBOARD_SERIAL_RX_PIN = 46;
static constexpr int SLAVE_LINK_TX_PIN = 45;
static constexpr int DOLL_DISPLAY_BACKLIGHT_PIN = 41;

#define DOLL_REAR_RGB_LED_PIN 40
#else
static constexpr const char* DOLL_BOARD_NAME =
#ifdef FNK0104S_4P0_320x480_ST7796
    "FNK0104S";
#else
    "FNK0104AB";
#endif

static constexpr int SD_MMC_CLK_PIN = 38;
static constexpr int SD_MMC_CMD_PIN = 40;
static constexpr int SD_MMC_D0_PIN = 39;
static constexpr int SD_MMC_D1_PIN = 41;
static constexpr int SD_MMC_D2_PIN = 48;
static constexpr int SD_MMC_D3_PIN = 47;
static constexpr int BATTERY_ADC_PIN = 9;

static constexpr int AUDIO_I2S_MCLK_PIN = 4;
static constexpr int AUDIO_I2S_BCLK_PIN = 5;
static constexpr int AUDIO_I2S_DIN_PIN = 6;
static constexpr int AUDIO_I2S_DOUT_PIN = 8;
static constexpr int AUDIO_I2S_WS_PIN = 7;
static constexpr int AUDIO_AMP_ENABLE_PIN = 1;
static constexpr int AUDIO_I2C_SCL_PIN = 15;
static constexpr int AUDIO_I2C_SDA_PIN = 16;

static constexpr int KEYBOARD_SERIAL_RX_PIN = 21;
static constexpr int SLAVE_LINK_TX_PIN = 2;
static constexpr int DOLL_DISPLAY_BACKLIGHT_PIN = 45;

#define DOLL_REAR_RGB_LED_PIN 42
#endif

static constexpr uint32_t AUDIO_I2C_SPEED = 400000;

static_assert(KEYBOARD_SERIAL_RX_PIN != SLAVE_LINK_TX_PIN,
              "DS-Slave RX and TX pins must differ");
static_assert(KEYBOARD_SERIAL_RX_PIN != SD_MMC_CLK_PIN &&
              KEYBOARD_SERIAL_RX_PIN != SD_MMC_CMD_PIN &&
              KEYBOARD_SERIAL_RX_PIN != SD_MMC_D0_PIN &&
              KEYBOARD_SERIAL_RX_PIN != SD_MMC_D1_PIN &&
              KEYBOARD_SERIAL_RX_PIN != SD_MMC_D2_PIN &&
              KEYBOARD_SERIAL_RX_PIN != SD_MMC_D3_PIN &&
              KEYBOARD_SERIAL_RX_PIN != AUDIO_I2S_MCLK_PIN &&
              KEYBOARD_SERIAL_RX_PIN != AUDIO_I2S_BCLK_PIN &&
              KEYBOARD_SERIAL_RX_PIN != AUDIO_I2S_DIN_PIN &&
              KEYBOARD_SERIAL_RX_PIN != AUDIO_I2S_DOUT_PIN &&
              KEYBOARD_SERIAL_RX_PIN != AUDIO_I2S_WS_PIN,
              "DS-Slave RX conflicts with an onboard bus");
static_assert(SLAVE_LINK_TX_PIN != SD_MMC_CLK_PIN &&
              SLAVE_LINK_TX_PIN != SD_MMC_CMD_PIN &&
              SLAVE_LINK_TX_PIN != SD_MMC_D0_PIN &&
              SLAVE_LINK_TX_PIN != SD_MMC_D1_PIN &&
              SLAVE_LINK_TX_PIN != SD_MMC_D2_PIN &&
              SLAVE_LINK_TX_PIN != SD_MMC_D3_PIN &&
              SLAVE_LINK_TX_PIN != AUDIO_I2S_MCLK_PIN &&
              SLAVE_LINK_TX_PIN != AUDIO_I2S_BCLK_PIN &&
              SLAVE_LINK_TX_PIN != AUDIO_I2S_DIN_PIN &&
              SLAVE_LINK_TX_PIN != AUDIO_I2S_DOUT_PIN &&
              SLAVE_LINK_TX_PIN != AUDIO_I2S_WS_PIN,
              "DS-Slave TX conflicts with an onboard bus");
