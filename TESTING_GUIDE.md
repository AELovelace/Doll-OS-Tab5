# DOLL-OS Tab5 testing guide

> Current diagnostic build: the ESP task-watchdog API is compiled for Arduino linkage but
> is not initialized, AppRunner checkpoints do not reset it or sleep, and AppRunner canvas
> glyphs use text size 1.

Use both PlatformIO environments for release hardware checks. `tab5` builds the
ordinary OS and OG Game Boy core in `ota_0`; `emulator` builds the lean GBA-only runtime in
`ota_1`. Both pin pioarduino 54.03.21, Arduino-ESP32 3.2.1, and ESP-IDF 5.4.2.
Keep a 115200-baud serial monitor open only after upload completes so opening
the port does not reset the board mid-flash.

Build, upload, and verify both application slots with:

```powershell
.\ps\Flash-DualImages.ps1
```

The script writes Doll-OS at `0x10000`, writes the emulator at `0x650000`, and
then verifies both images. A standard upload of only the default `tab5`
environment does not install the emulator image. Use `-Port COMx` when the board
is assigned a different serial port, or `-SkipBuild` to reuse current artifacts.
Never flash the emulator `firmware.bin` at the default `0x10000` address.

The first move from the old single-image table is a storage migration. LittleFS
moves from `0x650000` to `0x950000`, and the emulator overwrites part of its old
region. Back up internal `/apps`, settings, and credentials before flashing;
restore them afterward or allow `initStorage()` to seed a fresh filesystem. SD
ROMs, saves, and music are not relocated.

The Arduino `tab5` sketch profile remains available for comparison and pins
Arduino-ESP32 3.3.5; it does not contain the custom ESP-IDF cache configuration
required by the cyan-flash fix.

AppRunner canvas FLIPs use a dirty-row map. They must not run the full-screen
`frameSprite` versus `displayShadow` PSRAM comparison: doing so reads 3.68MB for
every small Tetris or Snake update and can starve continuous DSI scanout even on
the known-good core.

The ST7123 uses M5GFX's native 80MHz DPI clock and 1040Mbps DSI lane rate. Both
DW-GDMA AXI read ports are assigned QoS priority 15 after `M5.begin()`. A cyan
or blue full-screen flash indicates that continuous PSRAM scanout was starved;
it is not an AppRunner canvas color or clear operation.

M5Stack's flicker-free M5Tab5 UserDemo and Espressif's DSI-underrun guidance use
performance optimization, PSRAM XIP, and 128-byte cache lines. The stock
Arduino-ESP32 3.3.5 libraries instead ship precompiled for size, no PSRAM XIP,
and 64-byte cache lines. Editing the generated Arduino `sdkconfig` or
`sdkconfig.h` cannot change those precompiled libraries; the release test build
uses the checked-in PlatformIO Arduino + ESP-IDF hybrid environment.

The L2 cache is back to IDF's 128KB default. It is carved out of internal SRAM by
`esp_system/ld/esp32p4/memory.ld.in` (`SRAM_HIGH_SIZE = 0x80000 -
CONFIG_CACHE_L2_CACHE_SIZE`), so the former 256KB setting spent 128KB of the pool
that hosted WiFi, mbedTLS and the USB host need. If a DSI underrun reappears
*only* after this change, the 256KB cache is the first thing to restore.

`sdkconfig.tab5` and `sdkconfig.emulator` are generated and gitignored, and each
*overrides* `sdkconfig.defaults`. After editing `sdkconfig.defaults`, delete both
or the new values are silently ignored. Changing a cache or memory option also
needs stale linker intermediates cleared, otherwise an image can link against
the previous memory map with no warning:

```powershell
Remove-Item sdkconfig.tab5, sdkconfig.emulator, `
  .pio\build\tab5\memory.ld, .pio\build\tab5\sections.ld, `
  .pio\build\tab5\esp-idf\esp_system\ld\memory.ld.in, `
  .pio\build\tab5\esp-idf\esp_system\ld\sections.ld.in, `
  .pio\build\emulator\memory.ld, .pio\build\emulator\sections.ld, `
  .pio\build\emulator\esp-idf\esp_system\ld\memory.ld.in, `
  .pio\build\emulator\esp-idf\esp_system\ld\sections.ld.in -ErrorAction SilentlyContinue
```

Then confirm the regenerated `.pio\build\tab5\memory.ld` reads
`sram_high (RW) : org = 0x4FF40000, len = 0x80000 - 0x20000`. A `- 0x40000` there
means the 256KB-cache memory map is still in force.

Before flashing a display-regression build, verify both generated configurations:

```powershell
rg "CONFIG_(BOOTLOADER_APP_ROLLBACK_ENABLE|COMPILER_OPTIMIZATION_PERF|SPIRAM_SPEED_200M|SPIRAM_XIP_FROM_PSRAM|CACHE_L2_CACHE_128KB|CACHE_L2_CACHE_LINE_128B|SPIRAM_TRY_ALLOCATE_WIFI_LWIP|ESP_HOSTED_MEMPOOL_PREFER_SPIRAM)" sdkconfig.tab5 sdkconfig.emulator
```

The positive settings must be enabled and the three placement-preference
settings must remain unset in both images. `CONFIG_SPIRAM_SPEED_20M=y` is a hard
test failure; it means `CONFIG_IDF_EXPERIMENTAL_FEATURES=y` did not take effect.

Every display strip completes its own M5GFX transaction. Do not wrap multiple
separated strips in one outer `startWrite()`/`endWrite()` pair:
`Panel_FrameBufferBase` unions their coordinates into one bounding rectangle,
which can turn a status-bar plus command-bar update into a full 1.84MB cache
writeback and trigger the blue underrun.

## Display and `.dapp` canvas regression

1. Boot to the shell and confirm the log reports either the ST7121 or ST7123
   panel and a non-zero `logical=1280x720` display size. A touch-firmware read
   failure may be logged, but the DSI-ID fallback must still detect the panel.
   For ST7123, the project M5GFX patch must retain the native 80MHz DPI clock
   and 1040Mbps DSI lane rate.
2. Build with the PlatformIO `tab5` environment. Confirm the boot log reports
   200MHz PSRAM and a PSRAM `dapp canvas shadow`. The frame sprite and shadow
   should each report approximately 1.84MB. Record `[psram] heap: internal
   free=...` and compare it against the pre-change baseline of ~114KB; this
   change set should raise it substantially.
3. Run `tetris` for at least five minutes, moving and rotating pieces often so
   the canvas produces frequent `FLIP` updates.
4. Confirm every update moves directly between complete frames. The panel must
   not flash cyan, reveal a partly cleared canvas, or disturb the status and
   command bars.
5. Repeat with `snake` and one non-canvas `.dapp` to cover both display paths.
6. Exit each app and confirm the terminal history returns without stale canvas
   rows.

## Tab5-enhanced application regression

Run these checks with the local Tab5 keyboard; touch must remain inert throughout.

1. Run `calendar`. Confirm all seven weekday lanes, six possible week rows, the
   selected date, reminder text, and shortcut footer fit together. Add and delete
   a reminder, then restart the app and confirm its save file remains compatible.
2. Run `files`. Browse a directory containing more than 28 entries, change pages,
   enter and leave a directory, preview a file longer than one screen, and cancel
   a delete. Confirm the title, size column, status line, and footer do not overlap.
3. Run `today` with Todo and Control data present. Confirm focus/device, tasks,
   and service events occupy three simultaneous panels; change the focus and
   verify `/apps/today.focus` survives restart.
4. Run `sheet`. Move through all twenty rows without viewport scrolling, enter a
   label longer than five characters, evaluate a formula, save, wipe, and reload.
5. Run `tracker-music`. Confirm all sixteen steps, three tracks, song chain, and
   three shortcut rows remain visible. Exercise help plus the 24-entry save/load
   browser and reload an existing TM5 song.
6. Run `dappstore`. Confirm its two-column action desk appears, then browse and
   inspect package details. Dapper output must switch cleanly to the terminal and
   the canvas menu must return after the acknowledgement prompt.
7. Run `paint`. Confirm the entire 56x28 artwork and permanent tool/palette panel
   fit together. Save and reload existing `/apps/paint.dat` artwork unchanged.
8. Run `grotto2`. Confirm the title is no longer clipped and the map, player,
   status dashboard, movement legend, and zone name fit in the wide map frame.
9. Exercise the expanded data canvases: `dappchat` at 100x40, `page` and `reader`
   with 34 visible content rows, `plot` with its 98x30 graph, `synth` with 80-column
   waveforms, and `sysmon` with its 90-sample/45-second heap history.
10. Exercise the wide game set: `2048`, `four`, `habits`, `life`, `lightsout`,
    `mastermind`, `mines`, `simon`, `snake`, `sudoku`, and `tetris`. Confirm every
    board remains intact and each score/control panel occupies the new side space.
11. Run `hex` and `lamp`; confirm their inspectors, live values, and instructions
    remain visible together without covering editable data.
12. In `browse`, `feeds`, `requests`, `data`, `contacts`, `drill`, `notes`, and
    `passwords`, exercise the widened records/pages and confirm long values are not
    truncated at the former Cardputer-era widths.
13. Launch every remaining terminal-native app once and confirm its menu, prompt,
    and output use the Tab5 terminal cleanly without horizontal clipping.
14. Exit every app with its documented key and confirm the command bar, status bar,
   and shell history are restored without stale pixels or a hidden caret.

## Game Boy display and ES8388 audio regression

1. Boot once without `/wifi.cfg` and with the unchanged `YOUR_WIFI_SSID`
   default. Confirm the log says `WiFi skipped: no saved credentials`, reaches
   the shell with `Telnet dormant`, and never reports `Brownout detector was
   triggered` or an `Invalid mbox` assertion.
2. Launch the Tetris ROM in `fit` mode. It must start in-process without a reboot
   or application-partition change. Play continuously for at least five minutes.
   Tab5 `fit` is a centered, integer-scaled 480x432
   image. Confirm the log reports `[gb] display rotation=3 logical=1280x720` and
   the game is upright in landscape rather than 90 degrees clockwise; the
   GB launch reapplies Doll-OS's counterclockwise panel correction defensively.
   Confirm falling pieces update without a black screen, cyan flashes, or partial frames.
   The launch log should say `Tab5 safe-video mode: audio deferred`; video and
   USB controls must be proven stable before codec ownership is restored.
3. Open and close the Escape settings menu several times. Confirm both the menu
   and resumed game replace the complete frame without stale pixels.
4. Repeat the test in `1x` mode so the native emulator framebuffer also crosses
   the Game-Boy-only internal staging strip.
5. Confirm the serial log reports `gbFrame: 46080 bytes -> INTERNAL RAM`. The
   first `[gb] frame=...` diagnostic must have a changing `source_hash` and a
   nonzero `nonblack` count. The probe deliberately performs no DSI panel
   readback because ST7123/M5GFX readback can block; record the last `[gb]`
   breadcrumb if the glass still appears black.
6. Confirm the USB keyboard stays enumerated throughout launch and exit; an
   `EspUsbHost: Device disconnected` line is a failure even if video continues.
7. After video/input pass this regression, restore codec ownership in a separate
   change and confirm the serial log reports `ES8388 codec up`, never attempts
   ES8311, and switches cleanly between radio and Game Boy audio.

## Game Boy Advance CPU, pacing, and memory regression

1. Build with `pio run -e emulator`. Confirm its generated `compile_commands.json`
   passes `GBA_P4_THUMB_DYNAREC=0`, `GBA_RUNTIME_HOT_STATS=0`, and
   `GBA_SOUND_DIAGNOSTICS=0`, and `GBA_RFU_ENABLED=0` to the gpSP sources. The
   first keeps the retired JIT out unless an experiment opts in; the next two
   remove release-only counter writes from decoded blocks and audio samples.
   RFU is disabled because this frontend always launches with
   `SERIAL_MODE_DISABLED`; enabling wireless-adapter emulation requires restoring
   both the compile definition and a transport. Launch a GBA ROM from `/sd/gba`
   and confirm the shell announces a reboot. The next serial boot must claim a
   Game Boy Advance record and must not log shell WiFi, telnet, FTP, radio,
   LittleFS, or shell-canvas initialization.
2. Inspect the EWRAM decision and `[gba] memory` lines. The former must show the
   256 KB request, current internal free/largest blocks, the 128 KB reserve, and
   the selected `L2` or `PSRAM` placement. The latter must report `IWRAM=L2/32K`, `MAP=L2`,
   `VRAM=L2`, `IO=L2`, `PRE=64K`, and `JIT=0K`. `EWRAM=L2` is preferred when the
   physical 256 KB image fits while preserving the 128 KB host reserve;
   `EWRAM=PSRAM` is the safe fallback. The Escape menu must likewise show
   `V:L2`; a PSRAM marker for IWRAM, MAP, VRAM, or IO fails the hot-memory
   placement test. Record the following `post-allocation internal-free` and
   `largest` figures with performance results so heap placement changes are not
   mistaken for CPU-engine changes.
3. Confirm startup prints `requested=5 active=5 jit=0 batch=1 fast=1` and the
   performance line reports `cpumode=5`. Batch+fast is the release default;
   retired JIT mode numbers map back to it rather than reserving an executable
   arena.
4. Create a repeatable benchmark position with a save state. Keep display scale,
   frame skip, volume, and player input unchanged for the full comparison. Open
   Escape, select `CPU engine`, and record three consecutive 300-frame
   `[gba perf]` windows for each engine:

   - `Safe` / mode 0: stock interpreter baseline, including the ARM path.
   - `Batch` / mode 1: Thumb batching and asynchronous predecode only.
   - `Fast` / mode 4: isolated hand-written fast dispatch only.
   - `Batch+fast` / mode 5: both accelerators, the release default.

   Reload the benchmark state before each engine and discard the first window
   after the reload. Compare median `core`, `corepart=cpu`, and `emu`; do not use
   `drawn` as a CPU score because panel presentation is independently capped.
   The exclusive `corepart=cpu/event/video/sound` values may be added to
   reconstruct core time apart from small bridge overhead.
5. In the release build, `batch=ops/runs`, `fast=hits/misses`, and the first
   three `pre=hit/miss/ops` values remain zero because
   `GBA_RUNTIME_HOT_STATS=0` removes their writes from the interpreter. Build a
   coverage-only diagnostic firmware with `GBA_RUNTIME_HOT_STATS=1` before
   interpreting those fields; do not compare its FPS directly with release.
   `upd=arm/thumb/halt` counts
   event-update boundaries entered from each CPU state, not guest instructions.
   `batch=ops/runs` measures batched Thumb work, `fast=hits/misses` measures the
   isolated single-op fast fallback, and
   `pre=hit/miss/ops/build/req/drop` exposes predecode coverage and worker
   pressure. `predrop=q/set/dup` separates a full request queue, the retired
   frozen-cache set-full path, and a completion whose block was already
   resident. The current 64 KB experiment uses 512 four-way sets with
   frame-boundary CLOCK replacement; `set` must remain zero.
   `prechurn=evict/stall` reports replacements and moments when core 0
   found its completion queue full; occasional evictions are expected after the
   working set fills, but sustained stalls indicate the eight-entry install
   budget is too small. A diagnostic build with
   `GBA_THUMB_PREDECODE_PROBE_PROFILE=1` reports
   `preprobe=1/2/3/4/m`, which counts hits at each last-way-predicted probe depth
   and complete four-way misses. The first four values must sum to `pre=hit`;
   the first value is the predictor's one-probe success count. Release builds
   leave all five values at zero so lookup does not
   perform an extra internal-SRAM counter update. `preocc=now/cap/high` reports
   absolute resident entries,
   allocated capacity, and the lifetime high-water mark. Compare hit rate,
   eviction rate, `corepart=cpu`, and `emu` against the captured counter-only
   frozen-cache baseline in both overworld and battle gameplay. Unsupported
   zero-op classifications remain latched only in the admission table; they must
   not increase cache residency, builds, or hits.
   `rom=loads+prefetches` must remain zero for a warmed ROM that fits in the
   8 MB cache.
6. Check pacing in both a slow and a lightweight scene. When `core` remains over
   16743 us, `front=...pace...` should be near zero even if `pace_resync` rises;
   resynchronization drops stale lateness and must not grant an extra sleep
   interval. When `core` is below budget, pacing must hold emulation near the
   native 59.7 FPS instead of allowing the game and audio pitch to run fast.
7. At 3x, confirm `blit` remains near 4 ms and `drawn` remains near 15.1 FPS.
   Presentation runs on core 0 and the 66000 us panel interval intentionally
   caps it, so blit time must not be subtracted from core-1 CPU accounting.
8. Save and reload a state, then resume for another two minutes. Graphics,
   controls, timers, audio, and CPU engine selection must remain deterministic.
   Enter and leave the in-game Start menu repeatedly; no input-dependent branch
   may replay the intro, freeze, invoke SoftReset, or corrupt the save.
9. Confirm `[gb audio] ready, primed 1280 frames`, an `ES8388 readback ... OK`
   line, and an amp report ending in `pin driving`. During three performance
   windows, audio must remain continuous and correctly pitched through drawn
   bursts and after opening or resuming the menu. A release build leaves the
   per-sample peak/jump and Direct Sound flow counters at zero. Rebuild with
   `GBA_SOUND_DIAGNOSTICS=1` only when investigating audio contents; underrun,
   queue-depth, and returned-sample accounting remains active in release.
10. Choose Quit ROM. Confirm the battery save is written, the device reboots once,
   and the ordinary Doll-OS shell returns with display history, WiFi, and telnet
   initialized normally. Launch a different ROM, then return to the first ROM;
   no predecode cache state may leak between rebooted sessions.
11. Repeat with a ROM larger than the 16 MB cache. Confirm the per-window
    `rom=loads+prefetches` values advance without crashes and compare core time
    against a 16 MB-or-smaller ROM so SD paging is not mistaken for a CPU-engine
    regression.
12. While a ROM is running, press the hardware reset once. The emulator image
   must see the `RUNNING` launch record, clear it, select `app0`, and enter
   Doll-OS instead of relaunching the ROM. Also force one reset before emulator
   setup validation and confirm bootloader rollback selects the last healthy OS.
   Reset again to confirm the display lights on every software-reset boot and no
   crash loop is possible.
13. Remove the SD card after scheduling a launch, or test with an unreadable ROM.
   The emulator image must show a bounded launch failure, clear its record, and reboot
   to Doll-OS. Reinserting the card must not unexpectedly relaunch that ROM.
14. Compare free PSRAM/internal heap from the old in-shell launch and dedicated
    image logs. GBA must omit the ~1.8 MB `frameSprite` and ~1.8 MB
    `displayShadow`; the bare-panel Escape menu and touch controls must still
    redraw completely at 1x, 2x, and 3x. GB uses one frame sprite but must not
    allocate Doll-OS's second display shadow.
15. For every shell-to-GBA and GBA-to-shell transition, confirm serial prints
    `[display] restart fence: panel reset held low` before reset. The ST7123 must
    light without removing USB power; a dark panel that recovers only after a
    cold boot fails this test even when emulator performance logs continue.

## Static and lazy allocation regression

1. After both release builds, run `riscv32-esp-elf-size -A` on the two ELFs.
   The 2026-08-13 dual-image baselines are:

   - Doll-OS: 26,737 bytes `.dram0.data`, 42,116 bytes `.dram0.bss`, and
     16,984 bytes `.dram1.bss` (85,837 fixed internal bytes total).
   - Emulator: 15,441 bytes `.dram0.data`, 56,972 bytes `.dram0.bss`, and
     8,424 bytes `.dram1.bss` (80,837 fixed internal bytes total).

   The last combined OS/emulator image totaled 110,137 fixed internal bytes, so
   the normal OS now retains 24,300 additional bytes. Losing that separation
   needs an explicit explanation.
2. Inspect both sorted ELF symbol tables. `gnuboy_init` must be present only in
   Doll-OS. `gbaRunBootMode`, `gba_thumb_predecode_hot`, and `execute_arm` must
   be absent from Doll-OS and present in the GBA image. In the OS,
   `radioDirectory` and `xAudioStack` must each
   be four-byte pointers, not the former 5,424-byte directory and 3,500-byte
   task-stack arrays. `rfu_buf`, `rfu_host`, `rfu_client`, and `rfu_peer_bcst`
   must be absent from a release ELF.
3. Boot normally, run `radio list`, select a station by number and by name, then
   start and stop playback twice. The list must remain intact in PSRAM, decoder
   startup must not report an internal-stack allocation failure, and releasing
   the radio must permit a later playback to recreate its task cleanly.
4. Keep the USB keyboard attached throughout the radio and GBA tests. Its host
   object and two 8 KB runtime task stacks intentionally remain internal until
   measured high-water marks justify a smaller stack; enumeration stability is
   more valuable than an unverified reduction.

If the boot log says `dapp canvas shadow: ... unavailable`, treat the display
test as failed even if no corruption appears: the firmware has lost its
low-bandwidth canvas path.
