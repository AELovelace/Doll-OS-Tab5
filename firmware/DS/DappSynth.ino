//   DappSynth.ino
//   Three-channel PCM synthesizer for the .dapp WAVE opcode. The oscillator task
//   owns phase/noise state while the interpreter only updates a tiny guarded set
//   of channel parameters. Audio uses the same ES8311/I2S handoff as Game Boy.

#include "src/AudioOut.h"
#include <math.h>

struct DappWaveChannel {
    DappWaveType type;
    uint32_t increment;
    uint8_t level;
};

static const int DAPP_SYNTH_CHANNELS = 3;
static const int DAPP_SYNTH_BUFFER_FRAMES = 256;
static const int DAPP_SYNTH_TASK_STACK = 4096;
static const int DAPP_SYNTH_PEAK_PER_CHANNEL = 9000;

static portMUX_TYPE dappSynthMux = portMUX_INITIALIZER_UNLOCKED;
static DappWaveChannel dappSynthChannels[DAPP_SYNTH_CHANNELS] = {};
static TaskHandle_t dappSynthTaskHandle = NULL;
static volatile bool dappSynthStopRequested = false;
static bool dappSynthLastStartOk = true;
static int16_t dappSynthSine[256];
static bool dappSynthSineReady = false;

static DappWaveType dappSynthParseType(String name) {
    name.trim();
    name.toLowerCase();
    if (name == "sine" || name == "sin") return DAPP_WAVE_SINE;
    if (name == "triangle" || name == "tri") return DAPP_WAVE_TRIANGLE;
    if (name == "square" || name == "sq") return DAPP_WAVE_SQUARE;
    if (name == "sawtooth" || name == "saw") return DAPP_WAVE_SAWTOOTH;
    if (name == "noise") return DAPP_WAVE_NOISE;
    return DAPP_WAVE_OFF;
}

static int16_t dappSynthTriangle(uint32_t phase) {
    const uint8_t p = (uint8_t)(phase >> 24);
    int32_t value = (p < 128)
        ? (-32767 + (int32_t)p * 512)
        : (32767 - (int32_t)(p - 128) * 512);
    return (int16_t)value;
}

//linear ramp: the top 16 bits of the running phase counter, recentered around
//zero, rise for one full cycle and then wrap -- no lookup table needed, unlike
//sine, because the ramp *is* the phase.
static int16_t dappSynthSawtooth(uint32_t phase) {
    return (int16_t)((int32_t)(phase >> 16) - 32768);
}

static void dappSynthTask(void* parameter) {
    (void)parameter;
    uint32_t phase[DAPP_SYNTH_CHANNELS] = {};
    int16_t heldNoise[DAPP_SYNTH_CHANNELS] = {};
    uint32_t noiseState = esp_random() | 1U;
    int16_t samples[DAPP_SYNTH_BUFFER_FRAMES];

    while (!dappSynthStopRequested) {
        DappWaveChannel channels[DAPP_SYNTH_CHANNELS];
        portENTER_CRITICAL(&dappSynthMux);
        for (int channel = 0; channel < DAPP_SYNTH_CHANNELS; channel++) {
            channels[channel] = dappSynthChannels[channel];
        }
        portEXIT_CRITICAL(&dappSynthMux);

        for (int frame = 0; frame < DAPP_SYNTH_BUFFER_FRAMES; frame++) {
            int32_t mixed = 0;
            for (int channel = 0; channel < DAPP_SYNTH_CHANNELS; channel++) {
                const DappWaveChannel& setting = channels[channel];
                if (setting.type == DAPP_WAVE_OFF || setting.level == 0) continue;

                int32_t raw = 0;
                if (setting.type == DAPP_WAVE_SINE) {
                    raw = dappSynthSine[(uint8_t)(phase[channel] >> 24)];
                } else if (setting.type == DAPP_WAVE_TRIANGLE) {
                    raw = (int32_t)dappSynthTriangle(phase[channel]) * DAPP_SYNTH_PEAK_PER_CHANNEL / 32767;
                } else if (setting.type == DAPP_WAVE_SAWTOOTH) {
                    raw = (int32_t)dappSynthSawtooth(phase[channel]) * DAPP_SYNTH_PEAK_PER_CHANNEL / 32767;
                } else if (setting.type == DAPP_WAVE_SQUARE) {
                    raw = (phase[channel] & 0x80000000U)
                        ? -DAPP_SYNTH_PEAK_PER_CHANNEL : DAPP_SYNTH_PEAK_PER_CHANNEL;
                } else if (setting.type == DAPP_WAVE_NOISE) {
                    uint32_t nextPhase = phase[channel] + setting.increment;
                    if (nextPhase < phase[channel] || heldNoise[channel] == 0) {
                        noiseState ^= noiseState << 13;
                        noiseState ^= noiseState >> 17;
                        noiseState ^= noiseState << 5;
                        heldNoise[channel] = (int16_t)(noiseState >> 16);
                    }
                    raw = (int32_t)heldNoise[channel] * DAPP_SYNTH_PEAK_PER_CHANNEL / 32767;
                }

                mixed += raw * setting.level / 100;
                phase[channel] += setting.increment;
            }
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            samples[frame] = (int16_t)mixed;
        }

        AudioOut::onSamples(samples, DAPP_SYNTH_BUFFER_FRAMES);
        taskYIELD();
    }

    portENTER_CRITICAL(&dappSynthMux);
    dappSynthTaskHandle = NULL;
    portEXIT_CRITICAL(&dappSynthMux);
    vTaskDelete(NULL);
}

static bool dappSynthEnsureStarted() {
    if (dappSynthTaskHandle != NULL) return true;

    if (!dappSynthSineReady) {
        for (int i = 0; i < 256; i++) {
            dappSynthSine[i] = (int16_t)(sinf((float)i * 2.0f * PI / 256.0f)
                                            * DAPP_SYNTH_PEAK_PER_CHANNEL);
        }
        dappSynthSineReady = true;
    }

    if (!radioReleaseAudio() || !AudioOut::begin()) {
        AudioOut::end();
        dappSynthLastStartOk = false;
        return false;
    }

    AudioOut::setDiscard(false);
    dappSynthStopRequested = false;
    BaseType_t created = xTaskCreatePinnedToCore(
        dappSynthTask, "dappSynth", DAPP_SYNTH_TASK_STACK, NULL,
        tskIDLE_PRIORITY + 2, &dappSynthTaskHandle, portNUM_PROCESSORS - 1);
    if (created != pdPASS) {
        dappSynthTaskHandle = NULL;
        AudioOut::end();
        dappSynthLastStartOk = false;
        return false;
    }

    dappSynthLastStartOk = true;
    return true;
}

bool dappSynthSetChannel(int channelNumber, String waveform, long frequency, long level) {
    if (channelNumber < 1 || channelNumber > DAPP_SYNTH_CHANNELS) return false;

    DappWaveType type = dappSynthParseType(waveform);
    String lowered = waveform;
    lowered.trim();
    lowered.toLowerCase();
    bool validType = lowered == "off" || lowered == "sine" || lowered == "sin" ||
                     lowered == "triangle" || lowered == "tri" ||
                     lowered == "square" || lowered == "sq" ||
                     lowered == "sawtooth" || lowered == "saw" || lowered == "noise";
    if (!validType || frequency < 1 || frequency > 12000 || level < 0 || level > 100) {
        return false;
    }

    if (type != DAPP_WAVE_OFF && !dappSynthEnsureStarted()) return false;

    DappWaveChannel updated = {};
    updated.type = type;
    updated.increment = (uint32_t)(((uint64_t)frequency << 32) / AudioOut::kSampleRate);
    updated.level = (uint8_t)level;
    portENTER_CRITICAL(&dappSynthMux);
    dappSynthChannels[channelNumber - 1] = updated;
    portEXIT_CRITICAL(&dappSynthMux);
    return true;
}

void dappSynthEnd() {
    portENTER_CRITICAL(&dappSynthMux);
    for (int channel = 0; channel < DAPP_SYNTH_CHANNELS; channel++) {
        dappSynthChannels[channel] = {};
    }
    bool running = dappSynthTaskHandle != NULL;
    portEXIT_CRITICAL(&dappSynthMux);

    if (!running) return;
    dappSynthStopRequested = true;
    for (int waited = 0; waited < 250 && dappSynthTaskHandle != NULL; waited += 5) {
        delay(5);
    }
    if (dappSynthTaskHandle != NULL) {
        vTaskDelete(dappSynthTaskHandle);
        dappSynthTaskHandle = NULL;
    }
    AudioOut::setDiscard(true);
    AudioOut::end();
    dappSynthStopRequested = false;
}

bool dappSynthLastOk() {
    return dappSynthLastStartOk;
}
