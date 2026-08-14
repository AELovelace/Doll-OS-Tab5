set(DOLL_OS_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

# Keep the existing sketch and emulator sources in their canonical locations.
idf_component_register(
    SRCS
        "DollOSTab5.cpp"
        "${DOLL_OS_ROOT}/tinyexpr.c"
        "${DOLL_OS_ROOT}/src/AudioOut.cpp"
        "${DOLL_OS_ROOT}/src/DollInput/DollInput.cpp"
        "${DOLL_OS_ROOT}/src/EmulatorBoot.cpp"
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
# board frontend. Batch+fast consistently beats the experimental Thumb JIT on
# this target, so generated code and BIOS HLE are compiled out for now.
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
    COMPILE_DEFINITIONS "RETRO_GO=1;ROM_BUFFER_SIZE=16;GBA_SOUND_FREQUENCY=32768;OVERCLOCK_60FPS=1;GBA_SWI_HLE=0;GBA_P4_THUMB_DYNAREC=0;GBA_XTENSA_DYNAREC=0;GBA_RUNTIME_HOT_STATS=0;GBA_SOUND_DIAGNOSTICS=0;GBA_RFU_ENABLED=0"
    COMPILE_OPTIONS "-O3;-fomit-frame-pointer;-ffast-math"
)
