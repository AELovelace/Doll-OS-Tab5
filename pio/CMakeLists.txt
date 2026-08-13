set(DOLL_OS_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

# Keep the existing sketch and emulator sources in their canonical locations.
idf_component_register(
    SRCS
        "DollOSTab5.cpp"
        "${DOLL_OS_ROOT}/tinyexpr.c"
        "${DOLL_OS_ROOT}/src/AudioOut.cpp"
        "${DOLL_OS_ROOT}/src/DollInput/DollInput.cpp"
        "${DOLL_OS_ROOT}/src/GameBoyAdvanceHost.cpp"
        "${DOLL_OS_ROOT}/src/GameBoyHost.cpp"
        "${DOLL_OS_ROOT}/src/emulator/gpsp/cheats.c"
        "${DOLL_OS_ROOT}/src/emulator/gpsp/cpu.cpp"
        "${DOLL_OS_ROOT}/src/emulator/gpsp/doll_gba_bridge.cpp"
        "${DOLL_OS_ROOT}/src/emulator/gpsp/gba_cc_lut.c"
        "${DOLL_OS_ROOT}/src/emulator/gpsp/gba_memory.c"
        "${DOLL_OS_ROOT}/src/emulator/gpsp/gbp.c"
        "${DOLL_OS_ROOT}/src/emulator/gpsp/input.c"
        "${DOLL_OS_ROOT}/src/emulator/gpsp/main.c"
        "${DOLL_OS_ROOT}/src/emulator/gpsp/memmap.c"
        "${DOLL_OS_ROOT}/src/emulator/gpsp/rfu.c"
        "${DOLL_OS_ROOT}/src/emulator/gpsp/savestate.c"
        "${DOLL_OS_ROOT}/src/emulator/gpsp/serial.c"
        "${DOLL_OS_ROOT}/src/emulator/gpsp/sound.c"
        "${DOLL_OS_ROOT}/src/emulator/gpsp/video.cpp"
        "${DOLL_OS_ROOT}/src/emulator/gnuboy/cpu.c"
        "${DOLL_OS_ROOT}/src/emulator/gnuboy/gnuboy.c"
        "${DOLL_OS_ROOT}/src/emulator/gnuboy/hw.c"
        "${DOLL_OS_ROOT}/src/emulator/gnuboy/lcd.c"
        "${DOLL_OS_ROOT}/src/emulator/gnuboy/sound.c"
    INCLUDE_DIRS
        "."
        "${DOLL_OS_ROOT}"
        "${DOLL_OS_ROOT}/src"
        "${DOLL_OS_ROOT}/src/DollInput"
        "${DOLL_OS_ROOT}/src/emulator/gpsp"
        "${DOLL_OS_ROOT}/src/emulator/gnuboy"
)

# gpSP's P4 port uses dynamic core memory (RETRO_GO) but Doll-OS supplies the
# board frontend. The P4 Thumb JIT is lazy and bounded to one 96 KB executable
# bank; its lookup metadata and the cold GBA maps live in PSRAM.
set(GPSP_SOURCES
    "${DOLL_OS_ROOT}/src/GameBoyAdvanceHost.cpp"
    "${DOLL_OS_ROOT}/src/emulator/gpsp/cheats.c"
    "${DOLL_OS_ROOT}/src/emulator/gpsp/cpu.cpp"
    "${DOLL_OS_ROOT}/src/emulator/gpsp/doll_gba_bridge.cpp"
    "${DOLL_OS_ROOT}/src/emulator/gpsp/gba_cc_lut.c"
    "${DOLL_OS_ROOT}/src/emulator/gpsp/gba_memory.c"
    "${DOLL_OS_ROOT}/src/emulator/gpsp/gbp.c"
    "${DOLL_OS_ROOT}/src/emulator/gpsp/input.c"
    "${DOLL_OS_ROOT}/src/emulator/gpsp/main.c"
    "${DOLL_OS_ROOT}/src/emulator/gpsp/memmap.c"
    "${DOLL_OS_ROOT}/src/emulator/gpsp/rfu.c"
    "${DOLL_OS_ROOT}/src/emulator/gpsp/savestate.c"
    "${DOLL_OS_ROOT}/src/emulator/gpsp/serial.c"
    "${DOLL_OS_ROOT}/src/emulator/gpsp/sound.c"
    "${DOLL_OS_ROOT}/src/emulator/gpsp/video.cpp"
)
set_source_files_properties(${GPSP_SOURCES} PROPERTIES
    # The byte-overlay validator makes stack writes safe. WRAM reads remain
    # enabled, while the failing direct WRAM store emitter stays quarantined.
    COMPILE_DEFINITIONS "RETRO_GO=1;ROM_BUFFER_SIZE=8;GBA_SOUND_FREQUENCY=32768;OVERCLOCK_60FPS=1;GBA_SWI_HLE=2;GBA_P4_THUMB_DYNAREC=1;GBA_P4_THUMB_JIT_ENTRIES=4096;GBA_P4_THUMB_JIT_WAYS=4;GBA_P4_THUMB_JIT_BANKS=1;GBA_P4_THUMB_JIT_REJECTS=2048;GBA_P4_THUMB_JIT_HOT_ENTRIES=4096;GBA_P4_THUMB_JIT_HOT_THRESHOLD=8;GBA_P4_THUMB_JIT_MAX_OPS=24;GBA_P4_THUMB_JIT_STACK_WRITES=1;GBA_P4_THUMB_JIT_WRAM_LOADS=1;GBA_P4_THUMB_JIT_WRAM_STORES=0;GBA_P4_THUMB_JIT_ARENA_BYTES=196608;GBA_XTENSA_DYNAREC=0"
    COMPILE_OPTIONS "-O3;-fomit-frame-pointer;-ffast-math"
)
