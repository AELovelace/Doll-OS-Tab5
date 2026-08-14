#include <Arduino.h>

#if defined(DOLL_EMULATOR_IMAGE)
#include "EmulatorImage.inc"
#else
// Arduino CLI generates this file from every root .ino and supplies the function
// prototypes that a normal Arduino sketch build inserts automatically.
#include "../.pio-generated/Doll-OS-Tab5.ino.cpp"
#endif
