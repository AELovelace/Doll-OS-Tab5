# Tab5 bring-up

This is the first hardware gate for the Tab5 fork. It initializes the display,
enables the USB-A host power rail, opens the official Tab5 Keyboard in HID mode,
and passes its reports through `DollInput`.

It does not yet install the USB HID host class driver or BLE HCI/HID client. The
screen labels those as the next gates so a successful keyboard/display test is
not mistaken for a complete port.

## Build

Install PlatformIO Core, then run from this directory:

```powershell
pio run
pio run --target upload
pio device monitor
```

The environment follows M5Stack's ESP32-P4 pioarduino configuration and pins
all four M5 libraries to exact commits. The local `DollInput` library is found
through `lib_extra_dirs`.

## Expected result

- The display identifies M5Tab5 and reports 1280x720 plus installed PSRAM.
- `Tab5 keyboard` and `USB-A power` badges are green.
- Touching the screen does nothing.
- Tab5 Keyboard presses update the HID event and terminal-byte fields.
- The same events appear on the USB serial monitor at 115200 baud.
