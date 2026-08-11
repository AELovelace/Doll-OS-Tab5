// KeyboardSerial.ino
// The filename is retained to keep inherited callers stable while its backend
// now reads the official Tab5 Keyboard instead of a DS-Slave UART.

#include <M5UnitUnified.h>
#include <M5UnitUnifiedKEYBOARD.h>
#include <Wire.h>

#include <DollInput.h>

using doll::input::HidTerminalCodec;
using doll::input::KeyboardHub;
using doll::input::KeyboardSource;
using doll::input::KeyEvent;

static m5::unit::UnitUnified tab5KeyboardUnits;
static m5::unit::UnitTab5Keyboard tab5Keyboard;
static KeyboardHub keyboardHub;
static HidTerminalCodec keyboardCodec;
static bool tab5KeyboardReady = false;

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

static void keyboardEncodePendingEvents() {
    KeyEvent event;
    while (keyboardHub.next(event)) {
        uint8_t encoded[HidTerminalCodec::kMaxEncodedBytes]{};
        const size_t count = keyboardCodec.encode(event, encoded, sizeof(encoded));
        for (size_t i = 0; i < count; ++i) {
            if (!keyboardQueuePush(encoded[i])) {
                Serial.println("[input] terminal byte queue full");
                ledPulseError();
                return;
            }
        }
        if (count != 0) {
            ledPulseInput();
        }
    }
}  // Converts normalized HID transitions into DOLL-OS terminal bytes.

static void keyboardPumpHardware() {
    if (!tab5KeyboardReady) {
        return;
    }

    tab5KeyboardUnits.update();
    while (!tab5Keyboard.empty()) {
        const auto event = tab5Keyboard.oldest();
        tab5Keyboard.discard();
        if (event.type != m5::unit::tab5_keyboard::EventType::Hid) {
            continue;
        }

        uint8_t usages[KeyboardHub::kBootReportKeyCount]{};
        usages[0] = event.hid.keycode;             // A zero keycode is the device's release report.
        if (!keyboardHub.submitBootReport(KeyboardSource::Tab5,
                                          event.modifier, usages,
                                          KeyboardHub::kBootReportKeyCount)) {
            Serial.println("[input] normalized event queue full");
            ledPulseError();
        }
    }
    keyboardEncodePendingEvents();
}  // Drains the I2C keyboard and services the shared input hub on the main task.

void initKeyboardSerial() {
    keyboardByteHead = 0;
    keyboardByteTail = 0;
    keyboardByteCount = 0;
    keyboardHub.reset();
    keyboardCodec.reset();

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
