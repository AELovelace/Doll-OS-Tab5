#include "DollInput.h"

#include <string.h>

namespace doll {
namespace input {

namespace {
constexpr uint8_t kModifierControl = 0x11;
constexpr uint8_t kModifierShift = 0x22;
constexpr uint8_t kModifierAlt = 0x44;
constexpr uint8_t kUsageErrorRollover = 0x01;
constexpr uint8_t kUsagePostFail = 0x02;
constexpr uint8_t kUsageErrorUndefined = 0x03;
constexpr uint8_t kUsageCapsLock = 0x39;
}  // namespace

KeyboardHub::KeyboardHub() {
    reset();
}  // Initializes every source and queue counter to a deterministic empty state.

void KeyboardHub::reset() {
    for (KeyEvent& event : queue_) {
        event = KeyEvent{};
    }
    for (SourceState& state : states_) {
        state = SourceState{};
    }
    head_ = 0;
    tail_ = 0;
    count_ = 0;
    sequence_ = 0;
    dropped_ = 0;
}  // Clears all queued events and per-transport held-key state.

size_t KeyboardHub::sourceIndex(KeyboardSource source) {
    const size_t index = static_cast<size_t>(source);
    return index < static_cast<size_t>(KeyboardSource::Count) ? index : 0;
}  // Converts a source enum into a bounded state-array index.

bool KeyboardHub::bitmapGet(const uint8_t* bitmap, uint8_t usage) {
    return (bitmap[usage >> 3] & static_cast<uint8_t>(1U << (usage & 7U))) != 0;
}  // Reads one HID usage from the compact 256-key bitmap.

void KeyboardHub::bitmapSet(uint8_t* bitmap, uint8_t usage, bool value) {
    const uint8_t mask = static_cast<uint8_t>(1U << (usage & 7U));
    if (value) {
        bitmap[usage >> 3] |= mask;
    } else {
        bitmap[usage >> 3] &= static_cast<uint8_t>(~mask);
    }
}  // Updates one HID usage without disturbing neighboring key states.

size_t KeyboardHub::freeSlots() const {
    return kQueueCapacity - count_;
}  // Reports how many complete key events can be accepted atomically.

bool KeyboardHub::enqueue(KeyboardSource source, KeyEventType type,
                          uint8_t usage, uint8_t modifiersValue) {
    if (count_ >= kQueueCapacity) {
        ++dropped_;
        return false;
    }
    queue_[tail_] = KeyEvent{source, type, usage, modifiersValue, ++sequence_};
    tail_ = (tail_ + 1U) % kQueueCapacity;
    ++count_;
    return true;
}  // Appends one event while preserving FIFO ordering and overflow accounting.

void KeyboardHub::forceEnqueue(KeyboardSource source, KeyEventType type,
                               uint8_t usage, uint8_t modifiersValue) {
    if (count_ >= kQueueCapacity) {
        head_ = (head_ + 1U) % kQueueCapacity;
        --count_;
        ++dropped_;
    }
    enqueue(source, type, usage, modifiersValue);
}  // Guarantees delivery of source-reset events by replacing the oldest event if needed.

void KeyboardHub::clearSource(KeyboardSource source) {
    SourceState& state = states_[sourceIndex(source)];
    memset(state.pressed, 0, sizeof(state.pressed));
    state.modifiers = 0;
}  // Releases the hub's complete held-key snapshot for one transport.

bool KeyboardHub::setConnected(KeyboardSource source, bool connected) {
    SourceState& state = states_[sourceIndex(source)];
    if (state.connected == connected) {
        return true;
    }
    if (connected) {
        if (!enqueue(source, KeyEventType::Connected, 0, 0)) {
            return false;
        }
        state.connected = true;
        return true;
    }

    forceEnqueue(source, KeyEventType::ResetSource, 0, state.modifiers);
    clearSource(source);
    state.connected = false;
    forceEnqueue(source, KeyEventType::Disconnected, 0, 0);
    return true;
}  // Announces hot-plug changes and forces a held-key reset before disconnect.

bool KeyboardHub::submitBootReport(KeyboardSource source, uint8_t modifiersValue,
                                   const uint8_t* usages, size_t usageCount) {
    SourceState& state = states_[sourceIndex(source)];
    uint8_t nextPressed[32]{};
    const size_t boundedCount = usageCount < kBootReportKeyCount
        ? usageCount : kBootReportKeyCount;

    for (size_t i = 0; i < boundedCount; ++i) {
        const uint8_t usage = usages ? usages[i] : 0;
        if (usage == 0 || usage == kUsageErrorRollover ||
            usage == kUsagePostFail || usage == kUsageErrorUndefined) {
            continue;
        }
        bitmapSet(nextPressed, usage, true);
    }

    size_t required = state.connected ? 0U : 1U;
    if (state.modifiers != modifiersValue) {
        ++required;
    }
    for (size_t usage = 0; usage < 256; ++usage) {
        const uint8_t key = static_cast<uint8_t>(usage);
        if (bitmapGet(state.pressed, key) != bitmapGet(nextPressed, key)) {
            ++required;
        }
    }
    if (freeSlots() < required) {
        ++dropped_;
        return false;
    }

    if (!state.connected) {
        enqueue(source, KeyEventType::Connected, 0, 0);
        state.connected = true;
    }
    if (state.modifiers != modifiersValue) {
        enqueue(source, KeyEventType::ModifiersChanged, 0, modifiersValue);
    }
    for (size_t usage = 0; usage < 256; ++usage) {
        const uint8_t key = static_cast<uint8_t>(usage);
        if (bitmapGet(state.pressed, key) && !bitmapGet(nextPressed, key)) {
            enqueue(source, KeyEventType::Released, key, modifiersValue);
        }
    }
    for (size_t usage = 0; usage < 256; ++usage) {
        const uint8_t key = static_cast<uint8_t>(usage);
        if (!bitmapGet(state.pressed, key) && bitmapGet(nextPressed, key)) {
            enqueue(source, KeyEventType::Pressed, key, modifiersValue);
        }
    }

    memcpy(state.pressed, nextPressed, sizeof(state.pressed));
    state.modifiers = modifiersValue;
    return true;
}  // Atomically differences a six-key HID boot report into ordered transitions.

bool KeyboardHub::submitKey(KeyboardSource source, uint8_t usage, bool pressed,
                            uint8_t modifiersValue, bool repeat) {
    SourceState& state = states_[sourceIndex(source)];
    const bool wasPressed = bitmapGet(state.pressed, usage);
    size_t required = state.connected ? 0U : 1U;
    if (state.modifiers != modifiersValue) {
        ++required;
    }
    if (repeat || wasPressed != pressed) {
        ++required;
    }
    if (freeSlots() < required) {
        ++dropped_;
        return false;
    }

    if (!state.connected) {
        enqueue(source, KeyEventType::Connected, 0, 0);
        state.connected = true;
    }
    if (state.modifiers != modifiersValue) {
        enqueue(source, KeyEventType::ModifiersChanged, 0, modifiersValue);
    }
    if (repeat) {
        enqueue(source, KeyEventType::Repeat, usage, modifiersValue);
    } else if (wasPressed != pressed) {
        enqueue(source, pressed ? KeyEventType::Pressed : KeyEventType::Released,
                usage, modifiersValue);
        bitmapSet(state.pressed, usage, pressed);
    }
    state.modifiers = modifiersValue;
    return true;
}  // Accepts an individual transition for transports that do not send boot reports.

bool KeyboardHub::next(KeyEvent& event) {
    if (count_ == 0) {
        return false;
    }
    event = queue_[head_];
    head_ = (head_ + 1U) % kQueueCapacity;
    --count_;
    return true;
}  // Removes and returns the oldest event for the main DOLL-OS input service.

size_t KeyboardHub::available() const {
    return count_;
}  // Returns the current number of queued events.

uint32_t KeyboardHub::droppedEvents() const {
    return dropped_;
}  // Exposes overflow telemetry for diagnostics and soak tests.

bool KeyboardHub::isConnected(KeyboardSource source) const {
    return states_[sourceIndex(source)].connected;
}  // Reports whether one transport currently owns an attached keyboard.

bool KeyboardHub::isPressed(KeyboardSource source, uint8_t usage) const {
    return bitmapGet(states_[sourceIndex(source)].pressed, usage);
}  // Reads one source-local held key without combining transport state.

uint8_t KeyboardHub::modifiers(KeyboardSource source) const {
    return states_[sourceIndex(source)].modifiers;
}  // Returns the latest HID modifier byte for one keyboard transport.

HidTerminalCodec::HidTerminalCodec() {
    reset();
}  // Starts every keyboard source with caps lock disabled.

size_t HidTerminalCodec::sourceIndex(KeyboardSource source) {
    const size_t index = static_cast<size_t>(source);
    return index < static_cast<size_t>(KeyboardSource::Count) ? index : 0;
}  // Converts a source enum into a bounded codec-state index.

void HidTerminalCodec::reset() {
    memset(capsLock_, 0, sizeof(capsLock_));
}  // Clears transport-local text-translation state.

void HidTerminalCodec::resetSource(KeyboardSource source) {
    capsLock_[sourceIndex(source)] = false;
}  // Clears text state after a keyboard disconnects or loses synchronization.

bool HidTerminalCodec::hasShift(uint8_t modifiersValue) {
    return (modifiersValue & kModifierShift) != 0;
}  // Recognizes either left or right Shift in a HID modifier byte.

bool HidTerminalCodec::hasControl(uint8_t modifiersValue) {
    return (modifiersValue & kModifierControl) != 0;
}  // Recognizes either left or right Control in a HID modifier byte.

bool HidTerminalCodec::hasAlt(uint8_t modifiersValue) {
    return (modifiersValue & kModifierAlt) != 0;
}  // Recognizes either left or right Alt in a HID modifier byte.

char HidTerminalCodec::printableForUsage(uint8_t usage, bool shifted, bool capsLock) {
    if (usage >= 0x04 && usage <= 0x1D) {
        const bool upper = shifted != capsLock;
        return static_cast<char>((upper ? 'A' : 'a') + (usage - 0x04));
    }
    if (usage >= 0x1E && usage <= 0x27) {
        static constexpr char normal[] = "1234567890";
        static constexpr char shiftedChars[] = "!@#$%^&*()";
        return shifted ? shiftedChars[usage - 0x1E] : normal[usage - 0x1E];
    }

    switch (usage) {
        case 0x2C: return ' ';
        case 0x2D: return shifted ? '_' : '-';
        case 0x2E: return shifted ? '+' : '=';
        case 0x2F: return shifted ? '{' : '[';
        case 0x30: return shifted ? '}' : ']';
        case 0x31: return shifted ? '|' : '\\';
        case 0x32: return shifted ? '~' : '#';
        case 0x33: return shifted ? ':' : ';';
        case 0x34: return shifted ? '"' : '\'';
        case 0x35: return shifted ? '~' : '`';
        case 0x36: return shifted ? '<' : ',';
        case 0x37: return shifted ? '>' : '.';
        case 0x38: return shifted ? '?' : '/';
        case 0x54: return '/';
        case 0x55: return '*';
        case 0x56: return '-';
        case 0x57: return '+';
        case 0x59: return '1';
        case 0x5A: return '2';
        case 0x5B: return '3';
        case 0x5C: return '4';
        case 0x5D: return '5';
        case 0x5E: return '6';
        case 0x5F: return '7';
        case 0x60: return '8';
        case 0x61: return '9';
        case 0x62: return '0';
        case 0x63: return '.';
        default: return 0;
    }
}  // Maps the US ANSI printable and keypad HID usages into ASCII.

const char* HidTerminalCodec::sequenceForUsage(uint8_t usage) {
    switch (usage) {
        case 0x28: return "\r";
        case 0x29: return "\x1B";
        case 0x2A: return "\x08";
        case 0x2B: return "\t";
        case 0x3A: return "\x1BOP";
        case 0x3B: return "\x1BOQ";
        case 0x3C: return "\x1BOR";
        case 0x3D: return "\x1BOS";
        case 0x3E: return "\x1B[15~";
        case 0x3F: return "\x1B[17~";
        case 0x40: return "\x1B[18~";
        case 0x41: return "\x1B[19~";
        case 0x42: return "\x1B[20~";
        case 0x43: return "\x1B[21~";
        case 0x44: return "\x1B[23~";
        case 0x45: return "\x1B[24~";
        case 0x49: return "\x1B[2~";
        case 0x4A: return "\x1B[H";
        case 0x4B: return "\x1B[5~";
        case 0x4C: return "\x1B[3~";
        case 0x4D: return "\x1B[F";
        case 0x4E: return "\x1B[6~";
        case 0x4F: return "\x1B[C";
        case 0x50: return "\x1B[D";
        case 0x51: return "\x1B[B";
        case 0x52: return "\x1B[A";
        case 0x58: return "\r";
        default: return nullptr;
    }
}  // Maps non-printing HID usages into the terminal sequences DOLL-OS already consumes.

size_t HidTerminalCodec::appendByte(uint8_t byte, uint8_t* output,
                                    size_t capacity, size_t offset) {
    if (output && offset < capacity) {
        output[offset] = byte;
        return offset + 1U;
    }
    return offset;
}  // Appends one byte only when the caller supplied enough output space.

size_t HidTerminalCodec::appendText(const char* text, uint8_t* output,
                                    size_t capacity, size_t offset) {
    if (!text || !output) {
        return offset;
    }
    while (*text != '\0' && offset < capacity) {
        output[offset++] = static_cast<uint8_t>(*text++);
    }
    return offset;
}  // Copies a short escape sequence without writing past the caller's buffer.

size_t HidTerminalCodec::encode(const KeyEvent& event, uint8_t* output,
                                size_t capacity) {
    const size_t source = sourceIndex(event.source);
    if (event.type == KeyEventType::ResetSource ||
        event.type == KeyEventType::Disconnected) {
        capsLock_[source] = false;
        return 0;
    }
    if (event.type != KeyEventType::Pressed && event.type != KeyEventType::Repeat) {
        return 0;
    }
    if (event.usage == kUsageCapsLock && event.type == KeyEventType::Pressed) {
        capsLock_[source] = !capsLock_[source];
        return 0;
    }

    size_t offset = 0;
    if (hasAlt(event.modifiers)) {
        offset = appendByte(0x1B, output, capacity, offset);
    }

    if ((event.usage == 0x51 || event.usage == 0x52) &&
        hasControl(event.modifiers)) {
        const char* sequence = event.usage == 0x52
            ? "\x1B[1;5A" : "\x1B[1;5B";
        return appendText(sequence, output, capacity, offset);
    }
    if ((event.usage == 0x51 || event.usage == 0x52) &&
        hasShift(event.modifiers)) {
        const char* sequence = event.usage == 0x52
            ? "\x1B[1;2A" : "\x1B[1;2B";
        return appendText(sequence, output, capacity, offset);
    }

    const char* sequence = sequenceForUsage(event.usage);
    if (sequence) {
        return appendText(sequence, output, capacity, offset);
    }

    char character = printableForUsage(
        event.usage, hasShift(event.modifiers), capsLock_[source]);
    if (character == 0) {
        return offset;
    }
    if (hasControl(event.modifiers)) {
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 1);
        } else if (character >= 'A' && character <= '_') {
            character = static_cast<char>(character & 0x1F);
        } else if (character == '?') {
            character = 0x7F;
        }
    }
    return appendByte(static_cast<uint8_t>(character), output, capacity, offset);
}  // Converts press/repeat events into the existing terminal byte vocabulary.

HidGamepadCodec::HidGamepadCodec() {
    reset(nullptr, 0, false);
}  // Starts every transport with no held Game Boy buttons or modal actions.

size_t HidGamepadCodec::sourceIndex(KeyboardSource source) {
    const size_t index = static_cast<size_t>(source);
    return index < static_cast<size_t>(KeyboardSource::Count) ? index : 0;
}  // Converts a checked transport identifier into its independent game state slot.

uint8_t HidGamepadCodec::gamepadBitForUsage(uint8_t usage) {
    switch (usage) {
        case 0x4F: case 0x07: return 0x01;  // Right Arrow or D drives Right.
        case 0x50: case 0x04: return 0x02;  // Left Arrow or A drives Left.
        case 0x52: case 0x1A: return 0x04;  // Up Arrow or W drives Up.
        case 0x51: case 0x16: return 0x08;  // Down Arrow or S drives Down.
        case 0x11: return 0x10;             // N is the Game Boy A button.
        case 0x10: return 0x20;             // M is the Game Boy B button.
        case 0x31: return 0x40;             // Backslash is Select.
        case 0x28: return 0x80;             // Enter is Start.
        default: return 0x00;
    }
}  // Preserves the established DS-Slave keyboard-to-Game-Boy layout.

bool HidGamepadCodec::hasControl(uint8_t modifiersValue) {
    return (modifiersValue & kModifierControl) != 0;
}  // Recognizes either left or right Control for the Ctrl+T quit chord.

size_t HidGamepadCodec::appendByte(uint8_t byte, uint8_t* output,
                                   size_t capacity, size_t offset) {
    if (output && offset < capacity) {
        output[offset] = byte;
        return offset + 1U;
    }
    return offset;
}  // Appends one game protocol byte without overrunning the caller's buffer.

size_t HidGamepadCodec::appendPair(uint8_t prefix, uint8_t value,
                                   uint8_t* output, size_t capacity,
                                   size_t offset) {
    offset = appendByte(prefix, output, capacity, offset);
    return appendByte(value, output, capacity, offset);
}  // Appends one DOWN or UP record in the inherited two-byte game protocol.

size_t HidGamepadCodec::emitMerged(bool menuPressed, uint8_t* output,
                                   size_t capacity) {
    uint8_t nextMask = 0;
    bool nextQuit = false;
    for (const SourceState& state : states_) {
        nextMask |= state.mask;
        nextQuit = nextQuit ||
            (state.quitKeyDown && hasControl(state.modifiers));
    }

    size_t offset = 0;
    const uint8_t down = nextMask & static_cast<uint8_t>(~mergedMask_);
    const uint8_t up = mergedMask_ & static_cast<uint8_t>(~nextMask);
    for (uint16_t bit = 1; bit <= 0x80; bit <<= 1) {
        if (down & bit) {
            offset = appendPair(0xF0, static_cast<uint8_t>(bit),
                                output, capacity, offset);
        }
    }
    for (uint16_t bit = 1; bit <= 0x80; bit <<= 1) {
        if (up & bit) {
            offset = appendPair(0xF1, static_cast<uint8_t>(bit),
                                output, capacity, offset);
        }
    }
    if (nextQuit && !mergedQuit_) {
        offset = appendByte(0xF2, output, capacity, offset);
    }
    if (menuPressed) {
        offset = appendByte(0xF3, output, capacity, offset);
    }
    mergedMask_ = nextMask;
    mergedQuit_ = nextQuit;
    return offset;
}  // Merges simultaneous keyboards and emits only changed buttons and modal edges.

size_t HidGamepadCodec::reset(uint8_t* output, size_t capacity,
                              bool emitReleases) {
    size_t offset = 0;
    if (emitReleases) {
        for (uint16_t bit = 1; bit <= 0x80; bit <<= 1) {
            if (mergedMask_ & bit) {
                offset = appendPair(0xF1, static_cast<uint8_t>(bit),
                                    output, capacity, offset);
            }
        }
    }
    for (SourceState& state : states_) {
        state = SourceState{};
    }
    mergedMask_ = 0;
    mergedQuit_ = false;
    return offset;
}  // Clears held state and optionally releases buttons for a still-running game.

size_t HidGamepadCodec::encode(const KeyEvent& event, uint8_t* output,
                               size_t capacity) {
    SourceState& state = states_[sourceIndex(event.source)];
    if (event.type == KeyEventType::ResetSource ||
        event.type == KeyEventType::Disconnected) {
        state = SourceState{};
        return emitMerged(false, output, capacity);
    }

    state.modifiers = event.modifiers;
    bool menuPressed = false;
    if (event.type == KeyEventType::Pressed ||
        event.type == KeyEventType::Repeat ||
        event.type == KeyEventType::Released) {
        const bool down = event.type != KeyEventType::Released;
        const uint8_t bit = gamepadBitForUsage(event.usage);
        if (bit != 0) {
            if (down) {
                state.mask |= bit;
            } else {
                state.mask &= static_cast<uint8_t>(~bit);
            }
        }
        if (event.usage == 0x17) {
            state.quitKeyDown = down;
        }
        menuPressed = event.usage == 0x29 &&
            event.type == KeyEventType::Pressed;
    }
    return emitMerged(menuPressed, output, capacity);
}  // Converts HID transitions into held Game Boy buttons, quit, and menu events.

bool HidGamepadCodec::isToggleEvent(const KeyEvent& event) {
    return event.usage == 0x45 && event.type == KeyEventType::Pressed;
}  // Treats the rising edge of F12 as the manual keyboard game-mode switch.

}  // namespace input
}  // namespace doll
