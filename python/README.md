# DOLL-OS Sakura Flasher

Sakura Flasher is the initial Qt desktop frontend for configuring, building,
and flashing DOLL-OS and DS-Slave. It uses each sketch's `esp32s3` Arduino CLI
profile, discovers serial ports through Qt, and streams compiler/upload output
without blocking the window.

## Run from source on Windows

Python 3.11 or newer is recommended. From the repository root:

```powershell
py -3 -m pip install -r python/requirements.txt
./ps/run-sakura-flasher.ps1
```

The app looks for `arduino-cli` in this order:

1. a path previously selected in the app;
2. the `ARDUINO_CLI` environment variable;
3. the system `PATH`;
4. `tools/arduino-cli.exe` inside this repository;
5. the copy bundled with a standard Arduino IDE installation.

## Current first-release workflow

- Select **DOLL-OS main display** or **DS-Slave input bridge**.
- Select the hardware/wiring options for that target.
- For DOLL-OS, enter first-boot credentials. Secrets are stored only in the
  ignored `firmware/DS/config.h` file and are never echoed to the app log.
- Click **Build** to compile without a connected board.
- Select the correct serial port and click **Build & Flash** to compile and
  upload. The app deliberately does not guess which of a connected pair should
  receive which firmware.
- Enable **Erase all flash before upload** only for a clean reset. The app asks
  for confirmation because this deletes settings, paired-device data, and
  files stored in internal flash.

The current source release expects the neighboring layout used by this
workspace:

```text
Arduino/
  DS/
  DS-Slave/
  libraries/
```

A future packaged build can bundle Arduino CLI and the required library cache
so end users do not need that development layout.

## Architecture

- `game_editor_gui.py` owns Qt widgets, Sakura styling, serial discovery, and
  the asynchronous `QProcess` queue.
- `sakura_flasher/core.py` owns source-safe configuration editing, project
  discovery, Arduino CLI discovery, and command construction.
- `tests/test_sakura_core.py` tests the non-visual behavior without requiring a
  display server or connected ESP32.
