# Doll-OS gpSP import

This directory is a source import from
[`x213212/gba-p4`](https://github.com/x213212/gba-p4), pinned to commit
`03aeeb07d026fe3138e8cf906e42fb14d00e48b7`.

The imported core is based on gpSP for libretro and is licensed under GPL-2.0.
See `COPYING`, `README.md`, and `original_readme.txt` in this directory.

Doll-OS does not use the fork's Retro-Go board frontend. `GameBoyAdvanceHost`
provides the platform boundary for the Tab5 display, ES8388 audio, input, SD
paths, saves, and lifecycle. Batch+fast and asynchronous Thumb preclassification
are the active accelerators; the experimental RISC-V Thumb dynarec is retired.

Build policy:

- `RETRO_GO=1` selects the fork's dynamically allocated GBA memory layout.
- `ROM_BUFFER_SIZE=16` caps the PSRAM ROM cache; larger ROMs page from SD. Based
  on the measured 8 MiB-cache baseline, the projected save-state peak is about
  20.3 MiB, leaving roughly 11.7 MiB of the Tab5's 32 MiB PSRAM for allocator
  and runtime overhead; confirm that projection in the next hardware soak.
- `GBA_P4_THUMB_DYNAREC=0` is both the source default and an explicit gpSP
  component definition. Experimental builds must opt in deliberately; release
  ELFs contain only the no-op compatibility stubs and allocate no JIT arena,
  validation transactions, front cache, or flight recorder.
- Straight-line Thumb instructions execute through the shared fast handlers in
  batches of up to 16. The release CPU selector exposes `Safe`, `Batch`, `Fast`,
  and `Batch+fast`; retired JIT mode numbers map to `Batch+fast`.
- Core 1 admits hot ROM PCs into a 64 KB, 512-set, four-way preclassification
  cache. Entries retain eight raw opcodes plus their handler kinds, while core 0
  classifies immutable request snapshots through paired SPSC queues.
- Core 1 installs completed entries only between guest frames and owns cache
  lookup, CLOCK reference bits, replacement, and eviction. This prevents the
  worker from mutating live cache state during emulation.
- Per-way probe-depth counters are disabled in release builds after the baseline
  capture showed ways three and four serving nearly half of late cache hits.
  Diagnostic builds can opt in with `GBA_THUMB_PREDECODE_PROBE_PROFILE=1`.
- `GBA_RUNTIME_HOT_STATS=0` removes hit, miss, and completed-block counter writes
  from release dispatch. Coverage captures can opt in, but their FPS is not a
  release-performance result. `Safe` now guards both Thumb and ARM handwritten
  paths, so engine comparisons use a genuine stock-interpreter baseline.
- The 32 KB read-memory page map, 96 KB VRAM, physical 32 KB IWRAM image, and I/O
  storage are reserved in internal L2 before the preclassification cache. The
  retired dynarec's second 32 KB IWRAM SMC-marker shadow is compiled out.
  Allocation placement and fallbacks are reported in the launch log and as
  `V:L2`/`V:P` in the menu.
- With the retired dynarec compiled out, EWRAM allocates only its physical
  256 KB image instead of retaining a second 256 KB SMC-marker shadow. Startup
  prefers internal EWRAM only when the largest free block can also preserve a
  128 KB reserve for audio and host tasks; otherwise it falls back to PSRAM.
  Launch telemetry reports free and largest internal blocks before this decision
  and again after all core allocations.
- `GBA_RFU_ENABLED=0` replaces wireless-adapter emulation with inert stubs. The
  Doll-OS frontend always passes `SERIAL_MODE_DISABLED` and has no RFU transport,
  so retaining the RFU session, peer, and packet buffers only consumed internal
  BSS. A future link-cable feature must restore both pieces deliberately.
- `GBA_SOUND_FREQUENCY=32768` matches Doll-OS `AudioOut`. The sink primes five
  DMA descriptors with silence before enabling the amp and pads the core's short
  startup read, preserving the queue cushion through panel-update bursts.
  `GBA_SOUND_DIAGNOSTICS=0` removes per-sample quality and Direct Sound flow
  counters from release builds while leaving operational underrun accounting.

Runtime lifecycle:

- The shell resolves an SD ROM, writes a versioned/checksummed launch record to
  shared NVS, selects the `emulator` (`ota_1`) partition, and calls
  `esp_restart()`.
- The dedicated image claims that record before hardware or large allocations.
  It initializes only the panel/touch, board I2C, SD, keyboard/USB, audio, and
  gpSP; the OG Game Boy core stays in Doll-OS. Shell canvas/history, LittleFS,
  WiFi, telnet, FTP, radio, and
  the command runtime remain linked only into Doll-OS (`ota_0`).
- The record changes from `PENDING` to `RUNNING` before hardware initialization.
  A reset that sees `RUNNING` clears it and selects Doll-OS. ESP-IDF OTA rollback
  independently returns to the last validated image if setup fails before the
  NVS recovery path is usable.
- Quit flushes the battery save, shuts down gpSP/audio, clears the record,
  selects `app0`, and restarts into Doll-OS. GBA launches are SD-only
  because both images mount the card at `/sdcard`.
- Touch contact changes print raw coordinates and their mapped button mask. The
  first Start edge arms one 600-frame trace without changing CPU engine,
  so the input-dependent path is tested under the selected accelerator. Idle performance logs use a
  300-frame window; capture windows use ten frames so terminal copies retain the
  useful transition instead of filling with repeated intro telemetry.
- Diagnostic builds with `GBA_SOUND_DIAGNOSTICS=1` include cumulative nonzero
  FIFO bytes and timer-consumed samples. These counters avoid the recurring
  buffer-phase alias that made instantaneous FIFO snapshots appear empty.
- Before either reboot, Doll-OS sleeps the ST7123, disables its backlight, and
  holds PI4IO1 LCD/touch reset low. The expander and panel remain powered while
  the P4 resets, so this fence prevents an abrupt DSI stop from latching the
  display dark until a physical power cycle.
