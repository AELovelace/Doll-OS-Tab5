#pragma once

#include <Arduino.h>

// Emulator audio sink: the Game Boy APU's samples on the board's onboard ES8311
// codec + speaker.
//
// DOLL-OS port note: DOLL-OS's codec is normally Radio.ino's -- it owns the I2C control
// bus, the codec registers, and *both* of the S3's two I2S controllers (one
// bootstrap channel for MCLK, one for the ESP32-audioI2S engine). So a game
// can't just open a channel: Gameboy.ino first calls radioReleaseAudio()
// (Radio.ino), which stops any stream and hands the controllers back, then
// begin() below claims one for itself and reuses the codec registers Radio
// already knows how to program (audioCodecEnsure()). end() gives it back, so a
// "radio play" after a game session brings the stream up again normally.
//
// Everything here is failure-soft: if the codec or a channel can't be had,
// ready stays false, onSamples() drops silently, and the game runs mute --
// exactly the path cube-boy takes on boards with no audio.
namespace AudioOut {
// 2^21 / 64 exactly -- the only rate where gnuboy's integer cycles-per-sample
// divider comes out whole, so pitch is true. GameBoyHost hands this to
// gnuboy_init and the I2S channel below runs at it, so the emulator's clock and
// the codec's barely drift.
constexpr uint32_t kSampleRate = 32768;

bool begin();
void end();
bool available();
void onSamples(void* buf, size_t len);   // gb_audio_cb_t shape; len = mono int16 count
void setDiscard(bool on);                // true = swallow samples (paused/quitting)
void stats(uint32_t& pushed, uint32_t& dropped, uint32_t& underruns);
}  // namespace AudioOut
