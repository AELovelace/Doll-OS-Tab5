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
- Doll-OS bounds it to one 48 KB executable bank with reduced lookup tables so
  the emulator coexists with the Tab5 DSI framebuffer, Hosted WiFi, and USB.
- JIT lookup metadata is allocated lazily in PSRAM, and both it and the
  executable bank are released when the GBA command returns to the shell.
- `GBA_SOUND_FREQUENCY=32768` matches Doll-OS `AudioOut`.
