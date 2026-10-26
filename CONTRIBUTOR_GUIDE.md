# Contributor Guide

## Repository layout

- `firmware/DS/` contains the main DOLL-OS Arduino sketch.
- `libraries/` contains the repository-owned TFT and audio libraries.
- `../DS-Slave/` contains the paired BLE/USB input bridge sketch.
- `python/game_editor_gui.py` is the supported desktop modding/flashing tool
  entry point.
- `python/sakura_flasher/` contains logic shared by the GUI and unit tests.
- `ps/` contains PowerShell launch and maintenance scripts.

## Sakura Flasher changes

Keep UI behavior in `game_editor_gui.py` and keep parsing, file mutation, and
Arduino command construction in `sakura_flasher/core.py`. The core module must
remain importable without PySide6 so its behavior can be tested in headless CI.

When adding a GUI-managed C++ option:

1. add the field to the appropriate allowlist in `core.py`;
2. add its control and human-readable label in `game_editor_gui.py`;
3. add a round-trip test that proves comments and unrelated values survive;
4. document whether changing it erases or invalidates existing device data.

Never place Wi-Fi passwords, FTP passwords, or API keys on an Arduino CLI
command line or in the build log. DOLL-OS secrets belong only in the ignored
`firmware/DS/config.h` file.

## Firmware profiles

Both sketches expose a default `esp32s3` profile through `sketch.yaml`. Sakura
Flasher compiles into `.arduino-build/sakura-flasher/<target>` and uploads from
that explicit build directory. Keeping compilation and upload separate prevents
the uploader from silently using a stale artifact.

The current development profiles use the sibling `Arduino/libraries/` folder
for libraries already managed by the Arduino IDE. If the repository layout
changes, update both profiles and perform the two real compile checks in
`TESTING_GUIDE.md` before merging.

## Python style

- Use type hints for public helpers and document what every function completes.
- Keep file writes atomic when changing credentials or hardware headers.
- Pass subprocess arguments as a list to `QProcess`; do not build shell command
  strings from user-controlled values.
- Keep long operations asynchronous so the window can repaint and Cancel stays
  usable.
