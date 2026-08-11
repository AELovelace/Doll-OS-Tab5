set(DOLL_OS_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

# Keep the existing sketch and emulator sources in their canonical locations.
idf_component_register(
    SRCS
        "DollOSTab5.cpp"
        "${DOLL_OS_ROOT}/tinyexpr.c"
        "${DOLL_OS_ROOT}/src/AudioOut.cpp"
        "${DOLL_OS_ROOT}/src/DollInput/DollInput.cpp"
        "${DOLL_OS_ROOT}/src/GameBoyHost.cpp"
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
        "${DOLL_OS_ROOT}/src/emulator/gnuboy"
)
