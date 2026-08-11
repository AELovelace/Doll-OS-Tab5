# DOLL-OS Tab5 testing guide

Use the PlatformIO `tab5` environment for release hardware checks. It pins
pioarduino 54.03.21, Arduino-ESP32 3.2.1, and ESP-IDF 5.4.2 to match M5Stack's
working UserDemo generation. Keep a 115200-baud serial monitor open only after
upload completes so opening the port does not reset the board mid-flash.

Build and upload with:

```powershell
pio run -e tab5
pio run -e tab5 -t upload
```

The Arduino `tab5` sketch profile remains available for comparison and pins
Arduino-ESP32 3.3.5; it does not contain the custom ESP-IDF cache configuration
required by the cyan-flash fix.

AppRunner canvas FLIPs use an internal-RAM dirty-row map. They must not run the
full-screen `frameSprite` versus `displayShadow` PSRAM comparison: doing so reads
3.68MB for every small Tetris or Snake update and can starve continuous DSI
scanout even on the known-good core.

The ST7123 uses a conservative 50MHz DPI clock and 800Mbps DSI lane rate. Both
DW-GDMA AXI read ports are assigned QoS priority 15 after `M5.begin()`. A cyan
or blue full-screen flash indicates that continuous PSRAM scanout was starved;
it is not an AppRunner canvas color or clear operation.

M5Stack's flicker-free M5Tab5 UserDemo and Espressif's DSI-underrun guidance use
performance optimization, PSRAM XIP, a 256KB L2 cache, and 128-byte cache lines.
The stock Arduino-ESP32 3.3.5 libraries instead ship precompiled for size, no
PSRAM XIP, a 128KB L2 cache, and 64-byte cache lines. Editing the generated
Arduino `sdkconfig` or `sdkconfig.h` cannot change those precompiled libraries;
the release test build uses the checked-in PlatformIO Arduino + ESP-IDF hybrid
environment.

Before flashing a display-regression build, verify the generated configuration:

```powershell
rg "CONFIG_(COMPILER_OPTIMIZATION_PERF|SPIRAM_SPEED_200M|SPIRAM_XIP_FROM_PSRAM|CACHE_L2_CACHE_256KB|CACHE_L2_CACHE_LINE_128B)" sdkconfig.tab5
```

All five settings must be enabled. `CONFIG_SPIRAM_SPEED_20M=y` is a hard test
failure; it means `CONFIG_IDF_EXPERIMENTAL_FEATURES=y` did not take effect.

Every staged display strip completes its own M5GFX transaction. Do not wrap
multiple separated strips in one outer `startWrite()`/`endWrite()` pair:
`Panel_FrameBufferBase` unions their coordinates into one bounding rectangle,
which can turn a status-bar plus command-bar update into a full 1.84MB cache
writeback and trigger the blue underrun.

## Display and `.dapp` canvas regression

1. Boot to the shell and confirm the log reports either the ST7121 or ST7123
   panel and a non-zero `logical=1280x720` display size. A touch-firmware read
   failure may be logged, but the DSI-ID fallback must still detect the panel.
   For ST7123, the project M5GFX patch must select the current diagnostic 50MHz
   DPI clock and 800Mbps DSI lane rate.
2. Build with the PlatformIO `tab5` environment. Confirm the boot log reports
   200MHz PSRAM, an internal-RAM display row-staging buffer, and an internal-RAM
   `dapp canvas shadow`.
   The same strip must protect both panel pushes and display-shadow updates;
   neither operation may copy directly between two PSRAM allocations.
3. Run `tetris` for at least five minutes, moving and rotating pieces often so
   the canvas produces frequent `FLIP` updates.
4. Confirm every update moves directly between complete frames. The panel must
   not flash cyan, reveal a partly cleared canvas, or disturb the status and
   command bars.
5. Repeat with `snake` and one non-canvas `.dapp` to cover both display paths.
6. Exit each app and confirm the terminal history returns without stale canvas
   rows.

## Game Boy display and ES8388 audio regression

1. Launch the Tetris ROM in `fit` mode and play continuously for at least five
   minutes. Confirm falling pieces update without cyan flashes or partial frames.
2. Open and close the Escape settings menu several times. Confirm both the menu
   and resumed game replace the complete frame without stale pixels.
3. Repeat the test in `1x` mode so the native emulator framebuffer also crosses
   the internal-RAM staging path.
4. Confirm the serial log reports `ES8388 codec up` and never attempts an ES8311
   address. Listen for correct Game Boy pitch and clean audio on launch and exit.
5. After quitting, play radio or local music, then launch Tetris again. Confirm
   audio ownership switches cleanly in both directions without a muted amp.

If the boot log says `row staging: ... unavailable` or
`dapp canvas shadow: ... unavailable`, treat the display test as failed even if
no corruption appears: the firmware has lost its low-bandwidth canvas path.
