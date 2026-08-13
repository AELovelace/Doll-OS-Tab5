# DOLL-OS Tab5 testing guide

> Current diagnostic build: the ESP task-watchdog API is compiled for Arduino linkage but
> is not initialized, AppRunner checkpoints do not reset it or sleep, and AppRunner canvas
> glyphs use text size 1.

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

`sdkconfig.tab5` is generated and gitignored, and it *overrides* `sdkconfig.defaults`.
After editing `sdkconfig.defaults`, delete `sdkconfig.tab5` or the new values are
silently ignored. Changing a cache or memory option additionally needs the stale
linker intermediates cleared, otherwise the firmware links against the previous
memory map with no warning:

```powershell
Remove-Item sdkconfig.tab5, .pio\build\tab5\memory.ld, .pio\build\tab5\sections.ld, `
  .pio\build\tab5\esp-idf\esp_system\ld\memory.ld.in, `
  .pio\build\tab5\esp-idf\esp_system\ld\sections.ld.in -ErrorAction SilentlyContinue
```

Then confirm the regenerated `.pio\build\tab5\memory.ld` reads
`sram_high (RW) : org = 0x4FF40000, len = 0x80000 - 0x20000`. A `- 0x40000` there
means the 256KB-cache memory map is still in force.

Before flashing a display-regression build, verify the generated configuration:

```powershell
rg "CONFIG_(COMPILER_OPTIMIZATION_PERF|SPIRAM_SPEED_200M|SPIRAM_XIP_FROM_PSRAM|CACHE_L2_CACHE_128KB|CACHE_L2_CACHE_LINE_128B|SPIRAM_TRY_ALLOCATE_WIFI_LWIP|ESP_HOSTED_MEMPOOL_PREFER_SPIRAM)" sdkconfig.tab5
```

All seven settings must be enabled. `CONFIG_SPIRAM_SPEED_20M=y` is a hard test
failure; it means `CONFIG_IDF_EXPERIMENTAL_FEATURES=y` did not take effect.

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
2. Launch the Tetris ROM in `fit` mode and play continuously for at least five
   minutes. Tab5 `fit` is a centered, integer-scaled 480x432 image; confirm
   falling pieces update without a black screen, cyan flashes, or partial frames.
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

## Game Boy Advance JIT and memory regression

1. Build with `pio run -e tab5`. In the generated firmware map, confirm
   `gba_p4_thumb_jit_compile` and `gba_p4_thumb_jit_validate_and_commit` are in
   `.text.unlikely` rather than folded into the JIT lookup hot path.
2. Launch a GBA ROM from `/sd/gba`. Confirm the shell announces a reboot, then
   the next serial boot says `Starting GBA minimal mode` and never logs
   `initDisplay`, LittleFS, WiFi, telnet, FTP, or shell startup. Inspect the
   `[gba] memory` line: it must report `MAP=L2` and `VRAM=L2`. The Escape menu
   must likewise show `V:L2`; a PSRAM marker fails the hot-memory placement test.
   Record the JIT capacity selected after those allocations—192, 160, 128, 96,
   or a smaller safe fallback—alongside every benchmark result.
3. Confirm the CPU engine initially reads `Fast dispatch only (locked)` and
   `[gba jitdbg]` reports engine 4. This diagnostic mode enables only the
   hand-written ARM/Thumb fast dispatcher: JIT generation and batch execution
   must remain disabled. Every `[gba perf]` line must therefore retain zero JIT
   operations/builds, `batch=0/0`, and advancing `fast=hits/misses` counters.
   Compare emulated FPS at the intro, title, and Birch sequence with the earlier
   isolated-JIT captures. If the title restarts, preserve the input line and the
   first performance/reset reports after it; this localizes the original fault
   to the dispatcher without JIT or batch interference.

   In the separately selectable isolated-JIT mode, every new JIT block is
   validated eight times against the stock
   interpreter before trust, and a mismatch must quarantine only that block while
   engine 2 continues. Run a
   Thumb-heavy game for at least five minutes, then open the Escape menu twice
   and confirm emulated FPS, core time, and JIT hit/miss counts continue moving.
   In each idle 300-frame report, `jit=used/capacity`, `ops`, `build`, `full`, `reuse`,
   and `wait/reject/probe` must remain internally consistent; gameplay must not
   freeze when `full` changes to one or adaptive `reuse` begins advancing. The
   paired `[gba jitdbg]` line must retain engine 2 with zero resets and bad PCs.
   JIT counters must advance while `batch=0/0`; guard trips may rise only when
   the matching block is rejected and stock-interpreter execution continues.
4. Save and reload a state, then resume for another two minutes. This flushes
   the executable cache; graphics, controls, timers, and audio must remain
   deterministic while the blocks pass their eight validation runs again.
5. At the Pokémon title screen, wait at least ten seconds before pressing A,
   then enter and leave the in-game Start menu repeatedly. No input-dependent
   branch may invoke SoftReset, replay the intro, freeze, or corrupt the save.
   A touchscreen edge must first produce `[gba touch]` with `buttons=010`, then
   `[gba-input]`. A contact with `pressed=1` and `buttons=000` missed the hitbox.
   Use Start for the diagnostic title transition: the first Start edge emits
   `[gba-step] start=080` and arms one 600-frame capture while leaving CPU mode
   at 4. Normal A presses never arm or extend the expensive trace, so gameplay
   returns to representative performance logging when the capture completes.
   If it does, preserve the first `[gba-jit] guard` line and all following
   `[gba-jit] trace` lines before relaunching. Reason `53575253` is SoftReset,
   `42414441` is a bad ARM fetch, `42414454` is a bad Thumb fetch, and a
   `4A0000xx` reason is a generated-state or straight-line-PC mismatch.
   A `4A0000xx` mismatch must reject only that generated block, continue with
   engine 2, and leave the title/game state intact. SoftReset and bad-fetch guards may fall
   back to engine 0 because they indicate a wider CPU-state failure.
6. From the Escape menu, activate `CPU engine` and confirm it remains
   `Fast dispatch only (locked)` with a JIT/batch quarantine note. Do not enable
   Batch or Turbo until the title-state corruption is isolated.
   Stack writes and WRAM reads are enabled only with the byte-overlay validator.
   The reference pass must place PUSH and SP-relative stores in its shadow
   transaction, then compare generated RAM with that expected result. General
   The `STRB` failure at `08001008` was an IWRAM-arm register-cache bug: the
   store source must be loaded before generated control flow splits into EWRAM
   and IWRAM paths. On any mismatch the validator restores original bytes before
   stock gpSP retries.
7. Confirm `[gb audio] ready, primed 1280 frames`, an `ES8388 readback ... OK`
   line, and an amp report ending in `pin driving`. During three performance
   windows, `[gba i2s]` pushed frames must keep advancing without drops and
   `[gba audio]` must report nonzero samples. If it stays zero, preserve the paired
   `[gba mixer]` and `[gba mixflow]` lines so SOUNDCNT, cumulative nonzero FIFO
   writes, and timer consumption can be distinguished. Listen for continuous, correctly pitched sound through
   drawn-frame bursts and after opening/resuming the menu.
8. Choose Quit ROM. Confirm the battery save is written, the device reboots once,
   and the ordinary Doll-OS shell returns with display history, WiFi, and telnet
   initialized normally. Launch a different ROM, then return to the first ROM;
   no compiled block or rejection state may leak between rebooted sessions.
9. Repeat with a ROM larger than the 8 MB cache. Confirm `rom=loads+prefetches`
   advances without crashes and compare average core time against an 8 MB-or-
   smaller ROM so SD paging is not mistaken for a JIT regression.
10. Record at least three consecutive 120-frame `[gba perf]` lines for the same
   gameplay segment before and after a JIT or memory-placement change. Compare
   core time and emulated FPS separately from audio and blit time; use the
   per-window hit/miss/try and `ops` deltas rather than lifetime totals. Record
   `break=group:count` so unsupported-opcode work is not confused with cache churn.
11. While a ROM is running, press the hardware reset once. The following boot must
   log that the previous game-mode boot did not exit cleanly, clear the ticket,
   fully reset/wake the ST7123 panel, and enter Doll-OS instead of relaunching the
   ROM. Reset again to confirm the display lights on both software-reset boots,
   normal boots remain normal, and no crash loop is possible.
12. Remove the SD card after scheduling a launch, or test with an unreadable ROM.
   Minimal mode must show a bounded launch failure, clear its ticket, and reboot
   to Doll-OS. Reinserting the card must not unexpectedly relaunch that ROM.
13. Compare free PSRAM/internal heap from the old in-shell launch and minimal
    launch logs. Minimal mode must omit allocation of the ~1.8 MB `frameSprite`
    and ~1.8 MB `displayShadow`; the bare-panel Escape menu and touch controls
    must still redraw completely at 1x, 2x, and 3x.
14. For every shell-to-GBA and GBA-to-shell transition, confirm serial prints
    `[display] restart fence: panel reset held low` before reset. The ST7123 must
    light without removing USB power; a dark panel that recovers only after a
    cold boot fails this test even when emulator performance logs continue.

If the boot log says `dapp canvas shadow: ... unavailable`, treat the display
test as failed even if no corruption appears: the firmware has lost its
low-bandwidth canvas path.
