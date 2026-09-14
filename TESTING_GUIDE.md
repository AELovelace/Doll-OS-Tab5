# Testing Guide

## Sakura Flasher automated checks

From the DOLL-OS repository root:

```powershell
$env:PYTHONPATH = "$PWD\python"
py -3 -m unittest discover -s python/tests -v
py -3 -m py_compile python/game_editor_gui.py python/sakura_flasher/core.py
```

The tests cover C++ string escaping, exact-one board selection, preservation of
inline hardware comments, and deterministic build/upload command queues.

## Headless GUI smoke test

This starts and closes the complete window without displaying it:

```powershell
$env:PYTHONPATH = "$PWD\python"
$env:QT_QPA_PLATFORM = 'offscreen'
py -3 -c "from PySide6.QtCore import QTimer; from PySide6.QtWidgets import QApplication; from game_editor_gui import SakuraFlasherWindow; app=QApplication([]); window=SakuraFlasherWindow(); window.show(); QTimer.singleShot(100, app.quit); app.exec()"
```

## Real firmware compile checks

Use the Arduino CLI path shown in Sakura Flasher. These checks compile but do
not upload or modify a connected board:

```powershell
arduino-cli compile --profile esp32s3 --build-path .arduino-build/sakura-main-verify firmware/DS
arduino-cli compile --profile esp32s3 --build-path .arduino-build/sakura-slave-verify ../DS-Slave
```

Both must resolve ESP32 core 3.3.10 and finish with flash/RAM usage summaries.

## Manual desktop checklist

1. Confirm the Sakura background, cards, and log remain readable at the minimum
   window size and at 125% Windows display scaling.
2. Confirm Arduino CLI is auto-detected from Arduino IDE or can be browsed to.
3. Unplug and reconnect each ESP32-S3, click **Refresh ports**, and verify its
   port appears without restarting the app.
4. Change each main-board and slave option, click **Save options**, reopen the
   app, and confirm the header values reload correctly.
5. Build each target and confirm the window remains movable and Cancel remains
   enabled throughout compilation.
6. Flash each target separately, carefully matching the selected port to the
   physical board.
7. Test **Erase all flash** only on disposable test data and confirm the warning
   appears before the upload starts.
8. Boot both devices and complete the paired sleep, keyboard, joystick, button,
   display, SD, audio, and Wi-Fi checks documented in the hardware guides.

## Release blockers

Do not publish a packaged executable until it has a bundled or bootstrapped
Arduino CLI/library toolchain, license notices for Qt/PySide6, and testing on a
clean Windows account without an existing Arduino installation.
