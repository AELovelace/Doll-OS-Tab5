#pragma once

#include <stddef.h>
#include <stdint.h>

namespace doll {
namespace input {

enum class KeyboardSource : uint8_t {
    Tab5 = 0,
    Ble = 1,
    Usb = 2,
    Count = 3,
};

enum class KeyEventType : uint8_t {
    Connected,
    Disconnected,
    ResetSource,
    ModifiersChanged,
    Pressed,
    Released,
    Repeat,
};

struct KeyEvent {
    KeyboardSource source{KeyboardSource::Tab5};
    KeyEventType type{KeyEventType::ResetSource};
    uint8_t usage{0};
    uint8_t modifiers{0};
    uint32_t sequence{0};
};

class KeyboardHub {
public:
    static constexpr size_t kQueueCapacity = 96;
    static constexpr size_t kBootReportKeyCount = 6;

    KeyboardHub();

    void reset();
    bool setConnected(KeyboardSource source, bool connected);
    bool submitBootReport(KeyboardSource source, uint8_t modifiers,
                          const uint8_t* usages, size_t usageCount);
    bool submitKey(KeyboardSource source, uint8_t usage, bool pressed,
                   uint8_t modifiers, bool repeat = false);
    bool next(KeyEvent& event);
    size_t available() const;
    uint32_t droppedEvents() const;
    bool isConnected(KeyboardSource source) const;
    bool isPressed(KeyboardSource source, uint8_t usage) const;
    uint8_t modifiers(KeyboardSource source) const;

private:
    struct SourceState {
        bool connected{false};
        uint8_t modifiers{0};
        uint8_t pressed[32]{};
    };

    KeyEvent queue_[kQueueCapacity]{};
    SourceState states_[static_cast<size_t>(KeyboardSource::Count)]{};
    size_t head_{0};
    size_t tail_{0};
    size_t count_{0};
    uint32_t sequence_{0};
    uint32_t dropped_{0};

    static size_t sourceIndex(KeyboardSource source);
    static bool bitmapGet(const uint8_t* bitmap, uint8_t usage);
    static void bitmapSet(uint8_t* bitmap, uint8_t usage, bool value);
    size_t freeSlots() const;
    bool enqueue(KeyboardSource source, KeyEventType type,
                 uint8_t usage, uint8_t modifiers);
    void forceEnqueue(KeyboardSource source, KeyEventType type,
                      uint8_t usage, uint8_t modifiers);
    void clearSource(KeyboardSource source);
};

class HidTerminalCodec {
public:
    static constexpr size_t kMaxEncodedBytes = 8;

    HidTerminalCodec();

    void reset();
    void resetSource(KeyboardSource source);
    size_t encode(const KeyEvent& event, uint8_t* output, size_t capacity);

private:
    bool capsLock_[static_cast<size_t>(KeyboardSource::Count)]{};

    static size_t sourceIndex(KeyboardSource source);
    static bool hasShift(uint8_t modifiers);
    static bool hasControl(uint8_t modifiers);
    static bool hasAlt(uint8_t modifiers);
    static char printableForUsage(uint8_t usage, bool shifted, bool capsLock);
    static const char* sequenceForUsage(uint8_t usage);
    static size_t appendByte(uint8_t byte, uint8_t* output,
                             size_t capacity, size_t offset);
    static size_t appendText(const char* text, uint8_t* output,
                             size_t capacity, size_t offset);
};

class HidGamepadCodec {
public:
    static constexpr size_t kMaxEncodedBytes = 18;

    HidGamepadCodec();

    size_t reset(uint8_t* output, size_t capacity, bool emitReleases);
    size_t encode(const KeyEvent& event, uint8_t* output, size_t capacity);
    static bool isToggleEvent(const KeyEvent& event);

private:
    struct SourceState {
        uint8_t mask{0};
        uint8_t modifiers{0};
        bool quitKeyDown{false};
    };

    SourceState states_[static_cast<size_t>(KeyboardSource::Count)]{};
    uint8_t mergedMask_{0};
    bool mergedQuit_{false};

    static size_t sourceIndex(KeyboardSource source);
    static uint8_t gamepadBitForUsage(uint8_t usage);
    static bool hasControl(uint8_t modifiers);
    static size_t appendByte(uint8_t byte, uint8_t* output,
                             size_t capacity, size_t offset);
    static size_t appendPair(uint8_t prefix, uint8_t value, uint8_t* output,
                             size_t capacity, size_t offset);
    size_t emitMerged(bool menuPressed, uint8_t* output, size_t capacity);
};

}  // namespace input
}  // namespace doll
