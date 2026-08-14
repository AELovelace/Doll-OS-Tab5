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
- `ROM_BUFFER_SIZE=8` caps the PSRAM ROM cache; larger ROMs page from SD.
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
- The 32 KB read-memory page map, 96 KB VRAM, IWRAM, and I/O storage are reserved
  in internal L2 before the preclassification cache. Allocation placement and
  fallbacks are reported in the launch log and as `V:L2`/`V:P` in the menu.
- `GBA_SOUND_FREQUENCY=32768` matches Doll-OS `AudioOut`. The sink primes five
  DMA descriptors with silence before enabling the amp and pads the core's short
  startup read, preserving the queue cushion through panel-update bursts.

Runtime lifecycle:

- The shell resolves an SD ROM, writes a versioned/checksummed launch ticket to
  RTC no-init memory, and calls `esp_restart()`.
- Early setup claims that ticket before large allocations. Game mode initializes
  only the panel/touch, board I2C, LED, SD, keyboard/USB, audio, and gpSP; it does
  not initialize the shell framebuffer/shadow, history, LittleFS settings, WiFi,
  telnet, FTP, or command runtime.
- The ticket changes from `PENDING` to `RUNNING` before hardware initialization.
  A reset that sees `RUNNING` clears it and boots normal Doll-OS, preventing a
  crashing ROM or peripheral failure from creating a reboot loop.
- Quit flushes the battery save, shuts down gpSP/audio, clears the ticket, and
  restarts into Doll-OS. GBA ROM launches are intentionally SD-only in this mode.
- Touch contact changes print raw coordinates and their mapped button mask. The
  first Start edge arms one 600-frame trace without changing CPU engine,
  so the input-dependent path is tested under the selected accelerator. Idle performance logs use a
  300-frame window; capture windows use ten frames so terminal copies retain the
  useful transition instead of filling with repeated intro telemetry.
- Direct Sound diagnostics include cumulative nonzero FIFO bytes and nonzero
  timer-consumed samples. These counters avoid the recurring-buffer-phase alias
  that made instantaneous FIFO snapshots appear permanently empty.
- Before either reboot, Doll-OS sleeps the ST7123, disables its backlight, and
  holds PI4IO1 LCD/touch reset low. The expander and panel remain powered while
  the P4 resets, so this fence prevents an abrupt DSI stop from latching the
  display dark until a physical power cycle.
