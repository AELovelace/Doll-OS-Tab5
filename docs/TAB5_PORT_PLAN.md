# DOLL-OS Tab5 port plan

This repository is the Tab5-exclusive descendant of
[`AELovelace/Doll-OS-FNK0104`](https://github.com/AELovelace/Doll-OS-FNK0104).
The FNK repository remains the source of shared shell, application, editor, and
network changes. Tab5-specific hardware code belongs here.

## Product boundary

- Target: M5Stack Tab5, including the supported display-controller revisions.
- Local keyboard: the official 70-key Tab5 Keyboard on Ext.Port1.
- Additional keyboards: BLE HID through the onboard ESP32-C6 and USB HID through
  the Tab5 USB-A host port.
- Touch: deliberately excluded from the input system. Touch reports must never
  enter DOLL-OS input queues.
- Keyboard layout for the first release: US ANSI. Layout selection comes after
  the three transports are stable.

## Input architecture

All keyboard transports terminate at one HID boundary:

```text
Tab5 Keyboard (I2C HID) --+
BLE keyboard (BLE HID) ---+--> KeyboardHub --> terminal codec --> DOLL-OS
USB keyboard (USB HID) ---+                  +-> held-key state --> games
```

`libraries/DollInput` owns source identification, connection events, per-source
held-key state, boot-keyboard report differencing, and HID-to-terminal encoding.
Transport callbacks must copy their reports into a transport-owned queue; the
main input service is the only owner that submits reports to `KeyboardHub`.
This keeps the hub deterministic even when USB and BLE callbacks run on other
FreeRTOS tasks.

Disconnecting a source produces a reset event before its disconnect event. A
consumer must release every held key belonging to that source when it receives
the reset. This prevents unplugging a USB keyboard or losing BLE from leaving a
modifier or game button stuck.

## Milestones

### M0: fork and reproducibility

- Preserve the complete FNK Git history.
- Track the source repository as `upstream-fnk`.
- Pin the first port baseline with tag `fnk-base-27313e8`.
- Add reviewed upstream-sync pull requests; never auto-merge source changes.
- Establish a reproducible ESP32-P4 bring-up build.

### M1: hardware bring-up

- Initialize M5Unified/M5GFX without registering touch as an input source.
- Verify the 1280x720 display, 32MB PSRAM, and board detection.
- Read the Tab5 Keyboard in HID mode on SDA 0, SCL 1, INT 50.
- Turn on USB-A host power and prove USB HID enumeration.
- Verify microSD, LittleFS, Wi-Fi through the ESP32-C6, and power telemetry.
- Prove BLE scanning and HCI traffic through ESP-Hosted before promising BLE HID.

### M2: local shell

- Replace the FNK TFT_eSPI backend with M5Unified/M5GFX and `M5Canvas`.
- Replace the DS-Slave UART input with `KeyboardHub`.
- Bring up the shell, telnet mirror, history, editor, settings, LittleFS, and SD.
- Recalculate all terminal and editor geometry for 1280x720.

### M3: three keyboard transports

- Finish the Tab5 Keyboard backend, including modifiers and repeats.
- Add a USB HID host backend with hot-plug and composite-device handling.
- Add a BLE HID client with bonding, reconnect, and a keyboard-management command.
- Exercise simultaneous Tab5, USB, and BLE input without shared modifier state.

### M4: shared features

- Restore Dapper, FTP, SSH, ping/ARP, MQTT, ASUKA, and bundled applications.
- Keep shared fixes source-first: land them in FNK, then merge them here.
- Add a compile and smoke-test gate to every upstream-sync pull request.

### M5: Tab5 media and power

- Replace the FNK ES8311 audio path with a Tab5 ES8388 audio backend.
- Restore radio, music, DappSynth, and Game Boy audio through that backend.
- Add keyboard-interrupt wake, display sleep, battery reporting, and shutdown.
- Tune display pushes and Game Boy scaling for the MIPI display.

## MVP acceptance criteria

- DOLL-OS boots to a readable keyboard-operated shell at 1280x720.
- Touching the screen produces no application input.
- Tab5, USB, and BLE keyboards can remain connected at the same time.
- Letters, US symbols, Ctrl/Alt/Shift, navigation, deletion, and repeat work from
  every transport.
- USB hot-unplug and BLE loss cannot leave held keys stuck.
- A bonded BLE keyboard reconnects after reboot.
- Wi-Fi, BLE, USB host, display, and microSD operate concurrently.
- A clean merge from `upstream-fnk/main` passes the fork's build and smoke tests.

## Known gates

The largest early risk is BLE on ESP32-P4. The P4 has no native radio; the Tab5
uses its ESP32-C6 as a wireless coprocessor. The port must prove a stable hosted
HCI controller path alongside Wi-Fi. If an Arduino-only build cannot expose it,
the canonical build will move to Arduino as an ESP-IDF component rather than add
a second external ESP32.

USB host is supported by the P4, but the final application must own the USB-A
role and its switched 5V rail. USB device/HID forwarding demos are not the same
as accepting a keyboard, so the port uses Espressif's HID host component.
