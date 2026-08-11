// KeyboardSerial.ino
// The filename is retained to keep inherited callers stable while its backend
// now reads the official Tab5 Keyboard instead of a DS-Slave UART.

#include <M5UnitUnified.h>
#include <M5UnitUnifiedKEYBOARD.h>
#include <Wire.h>
#include <EspUsbHost.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "src/DollInput/DollInput.h"

using doll::input::HidTerminalCodec;
using doll::input::HidGamepadCodec;
using doll::input::KeyboardHub;
using doll::input::KeyboardSource;
using doll::input::KeyEvent;
using doll::input::KeyEventType;

static m5::unit::UnitUnified tab5KeyboardUnits;
static m5::unit::UnitTab5Keyboard tab5Keyboard;
static KeyboardHub keyboardHub;
static HidTerminalCodec keyboardCodec;
static HidGamepadCodec keyboardGamepadCodec;
static bool tab5KeyboardReady = false;
static bool keyboardGameMode = false;

static EspUsbHost usbKeyboardHost;
static QueueHandle_t usbKeyboardEventQueue = nullptr;
static bool usbKeyboardHostReady = false;
static bool usbKeyboardConnected = false;
static volatile uint32_t usbKeyboardDroppedEvents = 0;

enum class UsbKeyboardMessageType : uint8_t {
    Key,
    Disconnected,
};

struct UsbKeyboardMessage {
    UsbKeyboardMessageType type{UsbKeyboardMessageType::Key};
    uint8_t keycode{0};
    uint8_t modifiers{0};
    bool pressed{false};
};

static constexpr size_t KEYBOARD_BYTE_QUEUE_SIZE = 192;
static uint8_t keyboardByteQueue[KEYBOARD_BYTE_QUEUE_SIZE]{};
static size_t keyboardByteHead = 0;
static size_t keyboardByteTail = 0;
static size_t keyboardByteCount = 0;

// Each input source keeps its own line-edit state so an incomplete escape
// sequence can never interfere with a telnet client's parser.
static LineEditState keyboardLineState;

static bool keyboardQueuePush(uint8_t value) {
    if (keyboardByteCount >= KEYBOARD_BYTE_QUEUE_SIZE) {
        return false;
    }
    keyboardByteQueue[keyboardByteTail] = value;
    keyboardByteTail = (keyboardByteTail + 1U) % KEYBOARD_BYTE_QUEUE_SIZE;
    ++keyboardByteCount;
    return true;
}  // Appends one translated byte without allocating in the input service.

static int keyboardQueuePop() {
    if (keyboardByteCount == 0) {
        return -1;
    }
    const uint8_t value = keyboardByteQueue[keyboardByteHead];
    keyboardByteHead = (keyboardByteHead + 1U) % KEYBOARD_BYTE_QUEUE_SIZE;
    --keyboardByteCount;
    return value;
}  // Removes the oldest terminal byte for inherited shell consumers.

static int keyboardQueuePeek() {
    return keyboardByteCount == 0 ? -1 : keyboardByteQueue[keyboardByteHead];
}  // Observes the next byte without stealing it from an app or editor.

static bool keyboardQueuePushBytes(const uint8_t* bytes, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!keyboardQueuePush(bytes[i])) {
            Serial.println("[input] keyboard byte queue full");
            ledPulseError();
            return false;
        }
    }
    return true;
}  // Moves one complete terminal or game protocol record into the shared byte queue.

void keyboardSetGameMode(bool enabled, bool emitReleases, const char* reason) {
    uint8_t encoded[HidGamepadCodec::kMaxEncodedBytes]{};
    const size_t count = keyboardGamepadCodec.reset(
        encoded, sizeof(encoded), emitReleases && keyboardGameMode);
    keyboardQueuePushBytes(encoded, count);
    keyboardGameMode = enabled;
    Serial.printf("[input] game mode=%s (%s)\n",
                  enabled ? "on" : "off", reason ? reason : "unspecified");
    ledPulseInput();
}  // Switches every local keyboard between terminal bytes and held game controls.

static void usbKeyboardQueueMessage(uint8_t type, uint8_t keycode,
                                    uint8_t modifiers, bool pressed) {
    UsbKeyboardMessage message;
    message.type = static_cast<UsbKeyboardMessageType>(type);
    message.keycode = keycode;
    message.modifiers = modifiers;
    message.pressed = pressed;
    if (!usbKeyboardEventQueue ||
        xQueueSend(usbKeyboardEventQueue, &message, 0) != pdTRUE) {
        ++usbKeyboardDroppedEvents;
    }
}  // Hands one background USB event to the main DOLL-OS task without blocking.

static void initUsbKeyboardHost() {
    usbKeyboardEventQueue = xQueueCreate(64, sizeof(UsbKeyboardMessage));
    if (!usbKeyboardEventQueue) {
        Serial.println("[usb] failed to allocate keyboard event queue");
        ledPulseError();
        return;
    }

    usbKeyboardHost.onDeviceConnected([](const EspUsbHostDeviceInfo& device) {
        Serial.printf("[usb] connected address=%u vid=%04X pid=%04X product=%s\n",
                      device.address, device.vid, device.pid,
                      device.product ? device.product : "");
    });
    usbKeyboardHost.onDeviceDisconnected([](const EspUsbHostDeviceInfo& device) {
        Serial.printf("[usb] disconnected address=%u vid=%04X pid=%04X\n",
                      device.address, device.vid, device.pid);
        usbKeyboardQueueMessage(
            static_cast<uint8_t>(UsbKeyboardMessageType::Disconnected), 0, 0, false);
    });
    usbKeyboardHost.onKeyboard([](const EspUsbHostKeyboardEvent& event) {
        if (!event.pressed && !event.released) {
            return;
        }
        usbKeyboardQueueMessage(
            static_cast<uint8_t>(UsbKeyboardMessageType::Key),
            event.keycode, event.modifiers, event.pressed);
    });

    EspUsbHostConfig config;
    config.port = ESP_USB_HOST_PORT_DEFAULT;      // Tab5 USB-A uses the BSP default OTG host map.
    config.taskStackSize = 8192;
    usbKeyboardHostReady = usbKeyboardHost.begin(config);
    if (usbKeyboardHostReady) {
        Serial.println("[usb] HID keyboard host ready on USB-A");
    } else {
        Serial.printf("[usb] host initialization failed: %s\n",
                      usbKeyboardHost.lastErrorName());
        ledPulseError();
    }
}  // Starts USB enumeration and registers keyboard callbacks on the P4 host port.

static void keyboardPumpUsb() {
    if (!usbKeyboardEventQueue) {
        return;
    }

    UsbKeyboardMessage message;
    while (xQueueReceive(usbKeyboardEventQueue, &message, 0) == pdTRUE) {
        if (message.type == UsbKeyboardMessageType::Disconnected) {
            keyboardHub.setConnected(KeyboardSource::Usb, false);
            keyboardCodec.resetSource(KeyboardSource::Usb);
            usbKeyboardConnected = false;
            ledSetKeyboardActive(tab5KeyboardReady);
            continue;
        }

        if (!usbKeyboardConnected) {
            keyboardHub.setConnected(KeyboardSource::Usb, true);
            usbKeyboardConnected = true;
            ledSetKeyboardActive(true);
            Serial.println("[usb] keyboard input active");
        }
        if (!keyboardHub.submitKey(KeyboardSource::Usb, message.keycode,
                                   message.pressed, message.modifiers)) {
            Serial.println("[usb] normalized event queue full");
            ledPulseError();
        }
    }

    static uint32_t reportedDrops = 0;
    if (reportedDrops != usbKeyboardDroppedEvents) {
        reportedDrops = usbKeyboardDroppedEvents;
        Serial.printf("[usb] dropped keyboard events=%lu\n",
                      static_cast<unsigned long>(reportedDrops));
        ledPulseError();
    }
}  // Applies queued USB transitions to the transport-neutral hub on the main task.

static void keyboardEncodePendingEvents() {
    KeyEvent event;
    while (keyboardHub.next(event)) {
        if (event.type == KeyEventType::ResetSource ||
            event.type == KeyEventType::Disconnected) {
            keyboardCodec.resetSource(event.source);
        }
        if (HidGamepadCodec::isToggleEvent(event)) {
            keyboardSetGameMode(!keyboardGameMode, keyboardGameMode,
                                "F12 toggle");
            continue;
        }
        if (event.usage == 0x45) {
            continue;
        }

        uint8_t encoded[HidGamepadCodec::kMaxEncodedBytes]{};
        const size_t count = keyboardGameMode
            ? keyboardGamepadCodec.encode(event, encoded, sizeof(encoded))
            : keyboardCodec.encode(event, encoded, sizeof(encoded));
        if (!keyboardQueuePushBytes(encoded, count)) {
            return;
        }
        if (count != 0) {
            ledPulseInput();
        }
    }
}  // Converts normalized HID transitions into DOLL-OS terminal bytes.

static void keyboardPumpHardware() {
    if (tab5KeyboardReady) {
        tab5KeyboardUnits.update();
        while (!tab5Keyboard.empty()) {
            const auto event = tab5Keyboard.oldest();
            tab5Keyboard.discard();
            if (event.type != m5::unit::tab5_keyboard::EventType::Hid) {
                continue;
            }

            uint8_t usages[KeyboardHub::kBootReportKeyCount]{};
            usages[0] = event.hid.keycode;         // A zero keycode is the device's release report.
            if (!keyboardHub.submitBootReport(KeyboardSource::Tab5,
                                              event.modifier, usages,
                                              KeyboardHub::kBootReportKeyCount)) {
                Serial.println("[input] normalized event queue full");
                ledPulseError();
            }
        }
    }
    keyboardPumpUsb();
    keyboardEncodePendingEvents();
}  // Drains the I2C keyboard and services the shared input hub on the main task.

void initKeyboardSerial() {
    keyboardByteHead = 0;
    keyboardByteTail = 0;
    keyboardByteCount = 0;
    keyboardHub.reset();
    keyboardCodec.reset();
    keyboardGamepadCodec.reset(nullptr, 0, false);
    keyboardGameMode = false;

    auto config = tab5Keyboard.config();
    config.mode = m5::unit::tab5_keyboard::Mode::HID;
    config.start_periodic = true;
    config.irq_pin = TAB5_KEYBOARD_INTERRUPT_PIN;
    tab5Keyboard.config(config);

    Wire.end();
    Wire.begin(TAB5_KEYBOARD_SDA_PIN, TAB5_KEYBOARD_SCL_PIN,
               tab5Keyboard.component_config().clock);
    tab5KeyboardReady = tab5KeyboardUnits.add(tab5Keyboard, Wire)
        && tab5KeyboardUnits.begin();
    keyboardHub.setConnected(KeyboardSource::Tab5, tab5KeyboardReady);
    ledSetKeyboardActive(tab5KeyboardReady);

    if (tab5KeyboardReady) {
        Serial.printf("[boot] Tab5 Keyboard ready: firmware=%02X SDA=%d SCL=%d IRQ=%d\n",
                      tab5Keyboard.firmwareVersion(), TAB5_KEYBOARD_SDA_PIN,
                      TAB5_KEYBOARD_SCL_PIN, TAB5_KEYBOARD_INTERRUPT_PIN);
    } else {
        Serial.println("[boot] Tab5 Keyboard initialization failed");
        ledPulseError();
    }
    initUsbKeyboardHost();
}  // Initializes the official keyboard directly from the Arduino sketch.

static int keyboardReadUserByte() {
    keyboardPumpHardware();
    return keyboardQueuePop();
}  // Supplies one translated local-keyboard byte to inherited input paths.

void readKeyboardSerial() {
    while (true) {
        const int raw = keyboardReadUserByte();
        if (raw < 0) {
            break;
        }
        LineInputResult result = processLineEditByte(
            currentCommand, static_cast<uint8_t>(raw), keyboardLineState, false);
        if (result == LINE_NO_INPUT) {
            continue;
        }
        setActiveInput(shellPrompt(), currentCommand, false);
        if (result == LINE_SUBMITTED) {
            commandProcessor(currentCommand);
            setActiveInput(shellPrompt(), currentCommand, false);
            printPrompt();
        }
    }
}  // Feeds the Tab5 keyboard into the ordinary single-user shell editor.

LineInputResult readKeyboardLineEditedInput(String& text) {
    const int raw = keyboardReadUserByte();
    return raw < 0
        ? LINE_NO_INPUT
        : processLineEditByte(text, static_cast<uint8_t>(raw), keyboardLineState, false);
}  // Services modal password and prompt editors while the main loop is blocked.

int keyboardReadRawByte() {
    return keyboardReadUserByte();
}  // Provides terminal bytes to raw SSH, telnet, and application sessions.

int keyboardPeekRawByte() {
    keyboardPumpHardware();
    return keyboardQueuePeek();
}  // Lets an application inspect abort input without consuming its next key.
