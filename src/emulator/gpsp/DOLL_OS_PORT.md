# Doll-OS gpSP import

This directory is a source import from
[`x213212/gba-p4`](https://github.com/x213212/gba-p4), pinned to commit
`03aeeb07d026fe3138e8cf906e42fb14d00e48b7`.

The imported core is based on gpSP for libretro and is licensed under GPL-2.0.
See `COPYING`, `README.md`, and `original_readme.txt` in this directory.

Doll-OS does not use the fork's Retro-Go board frontend. `GameBoyAdvanceHost`
provides the platform boundary for the Tab5 display, ES8388 audio, input, SD
paths, saves, and lifecycle. The RISC-V Thumb dynarec is enabled after the
interpreter-only hardware milestone measured at roughly quarter-to-half speed.

Build policy:

- `RETRO_GO=1` selects the fork's dynamically allocated GBA memory layout.
- `ROM_BUFFER_SIZE=8` caps the PSRAM ROM cache; larger ROMs page from SD.
- `GBA_P4_THUMB_DYNAREC=1` enables the fork's validated ESP32-P4 Thumb JIT.
- Minimal game mode protects IWRAM, the read map, and VRAM in internal L2 before
  requesting one 192 KB executable bank. Arena fallbacks include 160, 128, 96,
  and smaller sizes so the JIT uses the best remaining contiguous block.
- Straight-line Thumb instructions execute in batches of up to 16. The JIT is
  therefore probed at stable branch targets and batch boundaries instead of at
  every interior instruction, eliminating the dominant false-miss pattern.
- The 4096-entry JIT lookup is four-way set associative. Four hot blocks with a
  colliding hash can coexist, with empty-first and round-robin replacement.
- A 24-op block ceiling and eight-hit admission threshold favor persistent loops
  while allowing frequently revisited Pokémon code to warm promptly.
- Once full, the arena samples one uncached candidate in 64 and may reuse the
  colliding block's executable slot. This lets long-running scenes replace stale
  code while ordinary misses avoid the PSRAM lookup and compilation path.
- JIT lookup metadata is allocated lazily in PSRAM, and both it and the
  executable bank disappear when quitting the ROM restarts into the full OS.
- ROM blocks retain eight interpreted validation passes. Trusted blocks
  then match by PC without rereading up to 24 immutable ROM opcodes from PSRAM;
  compilation and validation remain in cold code paths.
- Conditional/unconditional branches, calls, returns, and explicit PC writes stay
  in the batched interpreter. The JIT ends immediately before them, preventing an
  idle input-loop outcome from becoming trusted before its pressed path is seen.
- The in-game CPU engine selector separates `Safe`, `Batch`, `JIT trace`, and
  `Turbo`. `JIT trace` enables the JIT without batching and continuously compares
  every generated result with the C safety model, even after a block has warmed.
  It is the default while the Pokemon transition failure is under investigation.
- A 16-entry JIT flight recorder captures each compiled block's start/end PC,
  SP, LR, return word, and first/last opcode signature. A model mismatch,
  unexpected straight-line PC, SoftReset, or bad fetch disables both accelerators
  and emits `[gba-jit] guard` plus ordered `[gba-jit] trace` lines to serial.
- The 32 KB read-memory page map and 96 KB VRAM are reserved in internal L2 before
  the JIT, with fallbacks reported in the launch log and `V:L2`/`V:P` in the menu.
- `GBA_SOUND_FREQUENCY=32768` matches Doll-OS `AudioOut`.

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
- Before either reboot, Doll-OS sleeps the ST7123, disables its backlight, and
  holds PI4IO1 LCD/touch reset low. The expander and panel remain powered while
  the P4 resets, so this fence prevents an abrupt DSI stop from latching the
  display dark until a physical power cycle.
