#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "DollInput.h"

using doll::input::HidTerminalCodec;
using doll::input::HidGamepadCodec;
using doll::input::KeyboardHub;
using doll::input::KeyboardSource;
using doll::input::KeyEvent;
using doll::input::KeyEventType;

namespace {

size_t drainEncoded(KeyboardHub& hub, HidTerminalCodec& codec,
                    uint8_t* output, size_t capacity) {
    size_t written = 0;
    KeyEvent event;
    while (hub.next(event)) {
        written += codec.encode(event, output + written, capacity - written);
    }
    return written;
}  // Drains every queued event through the production terminal codec.

void submitSingle(KeyboardHub& hub, KeyboardSource source,
                  uint8_t modifier, uint8_t usage) {
    uint8_t usages[KeyboardHub::kBootReportKeyCount]{};
    usages[0] = usage;
    assert(hub.submitBootReport(source, modifier, usages, sizeof(usages)));
}  // Builds a standard six-key boot report containing one usage.

void testAsciiAndModifiers() {
    KeyboardHub hub;
    HidTerminalCodec codec;
    uint8_t bytes[32]{};

    submitSingle(hub, KeyboardSource::Tab5, 0, 0x04);
    assert(drainEncoded(hub, codec, bytes, sizeof(bytes)) == 1);
    assert(bytes[0] == 'a');

    submitSingle(hub, KeyboardSource::Tab5, 0, 0);
    drainEncoded(hub, codec, bytes, sizeof(bytes));
    submitSingle(hub, KeyboardSource::Tab5, 0x02, 0x04);
    assert(drainEncoded(hub, codec, bytes, sizeof(bytes)) == 1);
    assert(bytes[0] == 'A');

    submitSingle(hub, KeyboardSource::Tab5, 0, 0);
    drainEncoded(hub, codec, bytes, sizeof(bytes));
    submitSingle(hub, KeyboardSource::Tab5, 0x01, 0x06);
    assert(drainEncoded(hub, codec, bytes, sizeof(bytes)) == 1);
    assert(bytes[0] == 0x03);

    submitSingle(hub, KeyboardSource::Tab5, 0, 0);
    drainEncoded(hub, codec, bytes, sizeof(bytes));
    submitSingle(hub, KeyboardSource::Tab5, 0x04, 0x1B);
    assert(drainEncoded(hub, codec, bytes, sizeof(bytes)) == 2);
    assert(bytes[0] == 0x1B && bytes[1] == 'x');
}  // Verifies plain, Shift, Control, and Alt text translation.

void testNavigationSequence() {
    KeyboardHub hub;
    HidTerminalCodec codec;
    uint8_t bytes[16]{};

    submitSingle(hub, KeyboardSource::Usb, 0, 0x52);
    const size_t count = drainEncoded(hub, codec, bytes, sizeof(bytes));
    assert(count == 3);
    assert(memcmp(bytes, "\x1B[A", 3) == 0);

    submitSingle(hub, KeyboardSource::Usb, 0, 0);
    drainEncoded(hub, codec, bytes, sizeof(bytes));
    submitSingle(hub, KeyboardSource::Usb, 0x01, 0x52);
    const size_t ctrlCount = drainEncoded(hub, codec, bytes, sizeof(bytes));
    assert(ctrlCount == 6);
    assert(memcmp(bytes, "\x1B[1;5A", 6) == 0);

    submitSingle(hub, KeyboardSource::Usb, 0, 0);
    drainEncoded(hub, codec, bytes, sizeof(bytes));
    submitSingle(hub, KeyboardSource::Usb, 0x01, 0x51);
    const size_t ctrlDownCount = drainEncoded(hub, codec, bytes, sizeof(bytes));
    assert(ctrlDownCount == 6);
    assert(memcmp(bytes, "\x1B[1;5B", 6) == 0);

    submitSingle(hub, KeyboardSource::Usb, 0, 0);
    drainEncoded(hub, codec, bytes, sizeof(bytes));
    submitSingle(hub, KeyboardSource::Usb, 0x02, 0x51);
    const size_t shiftCount = drainEncoded(hub, codec, bytes, sizeof(bytes));
    assert(shiftCount == 6);
    assert(memcmp(bytes, "\x1B[1;2B", 6) == 0);
}  // Confirms an HID Up Arrow becomes the shell's existing CSI sequence.

void testGamepadCodec() {
    HidGamepadCodec codec;
    uint8_t bytes[HidGamepadCodec::kMaxEncodedBytes]{};

    KeyEvent event{KeyboardSource::Usb, KeyEventType::Pressed, 0x52, 0, 1};
    assert(codec.encode(event, bytes, sizeof(bytes)) == 2);
    assert(bytes[0] == 0xF0 && bytes[1] == 0x04);

    event = KeyEvent{KeyboardSource::Tab5, KeyEventType::Pressed, 0x52, 0, 2};
    assert(codec.encode(event, bytes, sizeof(bytes)) == 0);
    event.type = KeyEventType::Released;
    assert(codec.encode(event, bytes, sizeof(bytes)) == 0);
    event = KeyEvent{KeyboardSource::Usb, KeyEventType::Released, 0x52, 0, 3};
    assert(codec.encode(event, bytes, sizeof(bytes)) == 2);
    assert(bytes[0] == 0xF1 && bytes[1] == 0x04);

    event = KeyEvent{KeyboardSource::Usb, KeyEventType::Pressed, 0x17, 0x01, 4};
    assert(codec.encode(event, bytes, sizeof(bytes)) == 1);
    assert(bytes[0] == 0xF2);
    event.type = KeyEventType::Repeat;
    assert(codec.encode(event, bytes, sizeof(bytes)) == 0);

    event = KeyEvent{KeyboardSource::Usb, KeyEventType::Pressed, 0x29, 0, 5};
    assert(codec.encode(event, bytes, sizeof(bytes)) == 1);
    assert(bytes[0] == 0xF3);

    event = KeyEvent{KeyboardSource::Usb, KeyEventType::Pressed, 0x45, 0, 6};
    assert(HidGamepadCodec::isToggleEvent(event));
    event.type = KeyEventType::Repeat;
    assert(!HidGamepadCodec::isToggleEvent(event));
}  // Verifies held buttons, multi-keyboard merging, modal edges, and F12 toggling.

void testSourceIsolationAndDisconnect() {
    KeyboardHub hub;
    HidTerminalCodec codec;
    uint8_t bytes[16]{};

    submitSingle(hub, KeyboardSource::Tab5, 0, 0x04);
    submitSingle(hub, KeyboardSource::Usb, 0, 0x05);
    drainEncoded(hub, codec, bytes, sizeof(bytes));
    assert(hub.isPressed(KeyboardSource::Tab5, 0x04));
    assert(hub.isPressed(KeyboardSource::Usb, 0x05));

    assert(hub.setConnected(KeyboardSource::Usb, false));
    assert(hub.isPressed(KeyboardSource::Tab5, 0x04));
    assert(!hub.isPressed(KeyboardSource::Usb, 0x05));

    KeyEvent reset;
    KeyEvent disconnected;
    assert(hub.next(reset));
    assert(hub.next(disconnected));
    assert(reset.source == KeyboardSource::Usb);
    assert(reset.type == KeyEventType::ResetSource);
    assert(disconnected.type == KeyEventType::Disconnected);
}  // Proves a USB hot-unplug cannot release or corrupt the Tab5 keyboard state.

void testRepeat() {
    KeyboardHub hub;
    HidTerminalCodec codec;
    uint8_t bytes[8]{};

    assert(hub.submitKey(KeyboardSource::Ble, 0x07, true, 0));
    drainEncoded(hub, codec, bytes, sizeof(bytes));
    assert(hub.submitKey(KeyboardSource::Ble, 0x07, true, 0, true));
    assert(drainEncoded(hub, codec, bytes, sizeof(bytes)) == 1);
    assert(bytes[0] == 'd');
}  // Confirms a transport-provided repeat reuses the ordinary text mapping.

}  // namespace

int main() {
    testAsciiAndModifiers();
    testNavigationSequence();
    testGamepadCodec();
    testSourceIsolationAndDisconnect();
    testRepeat();
    return 0;
}  // Runs the transport-neutral input contract as a native executable.
