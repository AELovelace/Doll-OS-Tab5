#include <Arduino.h>
#include <M5Unified.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedKEYBOARD.h>
#include <Wire.h>

#include <DollInput.h>

namespace {

using doll::input::HidTerminalCodec;
using doll::input::KeyboardHub;
using doll::input::KeyboardSource;
using doll::input::KeyEvent;
using doll::input::KeyEventType;

constexpr int8_t kKeyboardSda = 0;
constexpr int8_t kKeyboardScl = 1;
constexpr int8_t kKeyboardInterrupt = 50;
constexpr uint16_t kBackground = 0x0841;
constexpr uint16_t kPanel = 0x10A2;
constexpr uint16_t kAccent = TFT_CYAN;
constexpr uint16_t kMuted = 0x9CD3;

auto& display = M5.Display;
m5::unit::UnitUnified units;
m5::unit::UnitTab5Keyboard tab5Keyboard;
KeyboardHub keyboardHub;
HidTerminalCodec terminalCodec;

String lastEvent = "waiting for a key";
String lastBytes = "-";
bool keyboardReady = false;
bool usbPowerReady = false;
uint32_t eventCount = 0;

const char* sourceName(KeyboardSource source) {
    switch (source) {
        case KeyboardSource::Tab5: return "Tab5";
        case KeyboardSource::Ble: return "BLE";
        case KeyboardSource::Usb: return "USB";
        default: return "?";
    }
}  // Converts the stable transport identifier into a short diagnostic label.

const char* eventName(KeyEventType type) {
    switch (type) {
        case KeyEventType::Connected: return "connected";
        case KeyEventType::Disconnected: return "disconnected";
        case KeyEventType::ResetSource: return "reset";
        case KeyEventType::ModifiersChanged: return "modifiers";
        case KeyEventType::Pressed: return "pressed";
        case KeyEventType::Released: return "released";
        case KeyEventType::Repeat: return "repeat";
        default: return "?";
    }
}  // Converts an input transition into a readable bring-up label.

String bytesForDisplay(const uint8_t* bytes, size_t count) {
    String text;
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) {
            text += ' ';
        }
        char hex[4];
        snprintf(hex, sizeof(hex), "%02X", bytes[i]);
        text += hex;
    }
    return text.length() ? text : "-";
}  // Formats terminal output bytes as hexadecimal without hiding control sequences.

void drawBadge(int x, int y, int width, const char* label, bool ready) {
    const uint16_t color = ready ? TFT_GREEN : TFT_ORANGE;
    display.fillRoundRect(x, y, width, 42, 8, color);
    display.setTextColor(TFT_BLACK, color);
    display.setTextDatum(middle_center);
    display.drawString(label, x + width / 2, y + 21);
}  // Draws one transport-status badge without creating a touchscreen hit target.

void drawScreen() {
    display.startWrite();
    display.fillScreen(kBackground);
    display.setTextDatum(top_left);
    display.setFont(&fonts::FreeMonoBold24pt7b);
    display.setTextColor(kAccent, kBackground);
    display.drawString("DOLL-OS Tab5 bring-up", 36, 28);

    display.setFont(&fonts::FreeMonoBold18pt7b);
    drawBadge(36, 108, 250, "Tab5 keyboard", keyboardReady);
    drawBadge(306, 108, 250, "USB-A power", usbPowerReady);
    drawBadge(576, 108, 250, "USB HID: next", false);
    drawBadge(846, 108, 250, "BLE HID: gate", false);

    display.fillRoundRect(36, 182, display.width() - 72, 430, 12, kPanel);
    display.setTextDatum(top_left);
    display.setTextColor(TFT_WHITE, kPanel);
    display.drawString("Touch input: disabled by design", 64, 210);

    display.setTextColor(kMuted, kPanel);
    display.drawString("Board", 64, 276);
    display.drawString("Display", 64, 324);
    display.drawString("PSRAM", 64, 372);
    display.drawString("Last HID event", 64, 440);
    display.drawString("Terminal bytes", 64, 488);
    display.drawString("Queued / dropped", 64, 536);

    display.setTextColor(TFT_WHITE, kPanel);
    display.drawString(M5.getBoard() == m5::board_t::board_M5Tab5 ? "M5Tab5" : "unexpected board", 360, 276);
    display.drawString(String(display.width()) + " x " + String(display.height()), 360, 324);
    display.drawString(String(ESP.getPsramSize() / (1024U * 1024U)) + " MB", 360, 372);
    display.drawString(lastEvent, 360, 440);
    display.drawString(lastBytes, 360, 488);
    display.drawString(String(keyboardHub.available()) + " / " + String(keyboardHub.droppedEvents()), 360, 536);

    display.setTextColor(kMuted, kBackground);
    display.drawString("Keyboard events are printed to USB serial at 115200 baud.", 36, 650);
    display.endWrite();
}  // Repaints the output-only diagnostic screen from the latest hardware state.

bool beginTab5Keyboard() {
    auto config = tab5Keyboard.config();
    config.mode = m5::unit::tab5_keyboard::Mode::HID;
    config.start_periodic = true;
    config.irq_pin = kKeyboardInterrupt;
    tab5Keyboard.config(config);

    Wire.end();
    Wire.begin(kKeyboardSda, kKeyboardScl,
               tab5Keyboard.component_config().clock);
    if (!units.add(tab5Keyboard, Wire) || !units.begin()) {
        return false;
    }

    keyboardHub.setConnected(KeyboardSource::Tab5, true);
    Serial.printf("[tab5-keyboard] firmware=%02X SDA=%d SCL=%d INT=%d\n",
                  tab5Keyboard.firmwareVersion(), kKeyboardSda,
                  kKeyboardScl, kKeyboardInterrupt);
    return true;
}  // Starts the official Tab5 Keyboard in HID mode on its documented Ext.Port1 pins.

void drainTab5Keyboard() {
    units.update();
    while (!tab5Keyboard.empty()) {
        const auto event = tab5Keyboard.oldest();
        tab5Keyboard.discard();
        if (event.type != m5::unit::tab5_keyboard::EventType::Hid) {
            continue;
        }

        uint8_t usages[KeyboardHub::kBootReportKeyCount]{};
        usages[0] = event.hid.keycode;
        if (!keyboardHub.submitBootReport(KeyboardSource::Tab5,
                                          event.modifier, usages,
                                          KeyboardHub::kBootReportKeyCount)) {
            Serial.println("[input] queue full; Tab5 HID report will be retried by the next state update");
        }
    }
}  // Copies I2C HID reports into the same hub reserved for future USB and BLE backends.

void drainKeyboardHub() {
    KeyEvent event;
    bool changed = false;
    while (keyboardHub.next(event)) {
        ++eventCount;
        changed = true;
        lastEvent = String(sourceName(event.source)) + " " + eventName(event.type) +
                    " usage=0x" + String(event.usage, HEX) +
                    " mod=0x" + String(event.modifiers, HEX);

        uint8_t bytes[HidTerminalCodec::kMaxEncodedBytes]{};
        const size_t count = terminalCodec.encode(event, bytes, sizeof(bytes));
        lastBytes = bytesForDisplay(bytes, count);

        Serial.printf("[input:%lu] %s\n", static_cast<unsigned long>(event.sequence),
                      lastEvent.c_str());
        if (count != 0) {
            Serial.print("[terminal] ");
            Serial.write(bytes, count);
            Serial.print("  [");
            Serial.print(lastBytes);
            Serial.println("]");
        }
    }
    if (changed) {
        drawScreen();
    }
}  // Serializes normalized events and demonstrates the terminal-byte compatibility layer.

}  // namespace

void setup() {
    auto config = M5.config();
    config.serial_baudrate = 115200;
    config.clear_display = true;
    config.output_power = true;
    config.internal_imu = false;
    config.internal_rtc = false;
    config.internal_mic = false;
    config.internal_spk = false;
    config.external_imu = false;
    config.external_rtc = false;
    M5.begin(config);

    if (display.height() > display.width()) {
        display.setRotation(3);
    }
    display.setBrightness(160);
    display.setFont(&fonts::FreeMonoBold18pt7b);

    // USB-A is a host port in DOLL-OS; enabling its switched 5V rail is separate
    // from installing Espressif's HID host class driver in the next milestone.
    M5.Power.setExtOutput(true);  // Powers the Tab5 external ports, including the USB-A host connector.
    usbPowerReady = true;

    keyboardReady = beginTab5Keyboard();
    if (!keyboardReady) {
        Serial.println("[tab5-keyboard] initialization failed");
    }

    Serial.printf("[tab5] board=%d display=%dx%d psram=%u\n",
                  static_cast<int>(M5.getBoard()), display.width(), display.height(),
                  static_cast<unsigned>(ESP.getPsramSize()));
    drawScreen();
}  // Initializes only output and explicit keyboard transports; no touch event enters the app.

void loop() {
    // Deliberately do not call M5.update(): this bring-up has no touch/button
    // consumers. The keyboard unit owns its separate Ext.Port1 update service.
    if (keyboardReady) {
        drainTab5Keyboard();
    }
    drainKeyboardHub();
    delay(2);
}  // Services physical keyboard input while keeping the screen output-only.
