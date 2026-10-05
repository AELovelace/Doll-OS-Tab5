#include "AudioOut.h"
#include "../BoardPins.h"

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"

// Codec bring-up lives in Radio.ino (it owns the I2C bus and the amp-enable
// pin); these are its exported entry points. Declared rather than included
// because config/globals of the sketch can't be pulled into a .cpp without
// dragging in duplicate definitions.
bool audioCodecEnsure();
int radioGetVolume();

namespace {

constexpr gpio_num_t kPinMclk = static_cast<gpio_num_t>(AUDIO_I2S_MCLK_PIN);
constexpr gpio_num_t kPinBclk = static_cast<gpio_num_t>(AUDIO_I2S_BCLK_PIN);
constexpr gpio_num_t kPinWs   = static_cast<gpio_num_t>(AUDIO_I2S_WS_PIN);
constexpr gpio_num_t kPinDout = static_cast<gpio_num_t>(AUDIO_I2S_DOUT_PIN);

// ~62ms of slack in the DMA ring (8 x 256 frames at 32768Hz). Sized against the
// worst hole the game loop can punch: one fit-mode panel push is ~38ms of SPI
// during which no samples are produced at all (see Gameboy.ino's frame loop), so
// anything shallower than that underruns on every drawn frame. The cost is
// button-to-sound latency, which this keeps to about four GB frames.
constexpr uint32_t kDmaDescNum = 8;
constexpr uint32_t kDmaFrameNum = 256;

// Staging buffer for the mono -> stereo expansion (see onSamples). One frame of
// GB audio is ~549 mono samples; this covers it in a single write.
constexpr size_t kStageFrames = 640;

// Radio's software volume scale (RADIO_VOLUME_MAX in Radio.ino). The shell's
// "radio vol" / Ctrl+Up/Down level doubles as the game's volume so there's one
// notion of loudness on the board.
constexpr int kVolumeMax = 21;

i2s_chan_handle_t txChan = nullptr;
int16_t* stage = nullptr;
bool enabled = false;   // channel is in RUNNING state -- disabling it otherwise is an error
bool ready = false;
bool discard = false;
uint32_t pushedFrames = 0;
uint32_t droppedFrames = 0;
uint32_t underrunCount = 0;

}  // namespace

// TEMPORARY launch tracing -- delete along with the GB_TRACE calls in Gameboy.ino.
#define AO_TRACE(msg) do { Serial.println("[ao-trace] " msg); Serial.flush(); } while (0)

bool AudioOut::begin() {
  AO_TRACE("begin() entered");
  if (ready) return true;

  stage = static_cast<int16_t*>(heap_caps_malloc(
      kStageFrames * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!stage) return false;
  AO_TRACE("stage buffer allocated, about to i2s_new_channel");

  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num = kDmaDescNum;
  chanCfg.dma_frame_num = kDmaFrameNum;
  chanCfg.auto_clear = true;   // ring goes silent on underrun instead of looping the last buffer
  if (i2s_new_channel(&chanCfg, &txChan, nullptr) != ESP_OK) {
    // Both controllers still spoken for -- Radio.ino didn't (or couldn't) let go.
    txChan = nullptr;
    end();
    return false;
  }

  // Built field by field rather than with a braced initializer: the IDF's
  // *_DEFAULT_CONFIG macros are C compound literals, which only compile in C++
  // as a GCC extension and have bitten this project's other IDF glue before.
  i2s_std_config_t stdCfg = {};
  stdCfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
  stdCfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO);
  stdCfg.gpio_cfg.mclk = kPinMclk;
  stdCfg.gpio_cfg.bclk = kPinBclk;
  stdCfg.gpio_cfg.ws = kPinWs;
  stdCfg.gpio_cfg.dout = kPinDout;
  stdCfg.gpio_cfg.din = I2S_GPIO_UNUSED;
  // 384x matches what es8311_codec_init() programs the codec's dividers for
  // (EXAMPLE_MCLK_MULTIPLE, es8311.h). The codec is an I2S slave, so what
  // actually has to hold is the MCLK:LRCK *ratio* -- keep it at 384 and the
  // registers stay correct even though they were computed for 16kHz.
  stdCfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;

  AO_TRACE("i2s_new_channel ok, about to init_std_mode");
  if (i2s_channel_init_std_mode(txChan, &stdCfg) != ESP_OK) {
    end();
    return false;
  }
  AO_TRACE("init_std_mode ok, about to i2s_channel_enable");
  // Clocks must already be running when the codec's registers are programmed --
  // same order Radio.ino uses (I2S up, then es8311_codec_init).
  if (i2s_channel_enable(txChan) != ESP_OK) {
    end();
    return false;
  }
  enabled = true;
  AO_TRACE("i2s_channel_enable ok, about to audioCodecEnsure (I2C)");
  if (!audioCodecEnsure()) {
    end();
    return false;
  }
  AO_TRACE("audioCodecEnsure ok, begin() complete");

  discard = false;
  pushedFrames = droppedFrames = underrunCount = 0;
  ready = true;
  return true;
}

void AudioOut::end() {
  if (txChan) {
    if (enabled) i2s_channel_disable(txChan);
    i2s_del_channel(txChan);
    txChan = nullptr;
    enabled = false;
  }
  if (stage) {
    heap_caps_free(stage);
    stage = nullptr;
  }
  ready = false;
}

bool AudioOut::available() { return ready; }

void AudioOut::setDiscard(bool on) { discard = on; }

// gnuboy calls this once per emulated frame with GB_AUDIO_MONO_S16 samples.
//
// Mono is deliberate: the ES8311 drives a single speaker off one I2S slot, so a
// true stereo feed would silently throw away everything panned to the other
// side (NR51 pans plenty of Pokemon's channels). We take gnuboy's own mixdown
// and write the same sample into both slots, which is right whichever slot the
// codec is latching.
//
// The write blocks on a full ring, but only briefly: Gameboy.ino's frame timer
// is the clock here, not the codec, and the two agree to a couple of parts in
// ten thousand. The short block just trims that drift -- letting it block for a
// whole frame instead would couple the emulator to the codec and turn every
// scheduler tick into a dropped frame.
void AudioOut::onSamples(void* buf, size_t len) {
  if (!ready || discard || !buf || len == 0) return;

  const int16_t* src = static_cast<const int16_t*>(buf);
  const int volume = radioGetVolume();
  if (volume <= 0) return;

  while (len > 0) {
    const size_t n = (len > kStageFrames) ? kStageFrames : len;
    for (size_t i = 0; i < n; i++) {
      // gnuboy's mix peaks around +/-20000, so the volume scale can't overflow
      // int16 on the way down and no clamp is needed.
      const int16_t s = (int16_t)((int32_t)src[i] * volume / kVolumeMax);
      stage[i * 2] = s;
      stage[i * 2 + 1] = s;
    }

    size_t written = 0;
    const esp_err_t err = i2s_channel_write(txChan, stage, n * 2 * sizeof(int16_t),
                                            &written, pdMS_TO_TICKS(8));
    const size_t framesWritten = written / (2 * sizeof(int16_t));
    pushedFrames += framesWritten;
    if (err != ESP_OK || framesWritten < n) {
      droppedFrames += (n - framesWritten);
      underrunCount++;
      return;   // behind the clock: drop the rest of this frame rather than pile up
    }

    src += n;
    len -= n;
  }
}

void AudioOut::stats(uint32_t& pushed, uint32_t& dropped, uint32_t& underruns) {
  pushed = pushedFrames;
  dropped = droppedFrames;
  underruns = underrunCount;
}
