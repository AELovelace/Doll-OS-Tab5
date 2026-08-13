//   Radio.ino
//   Background internet-radio player on the board's onboard ES8388 codec + speaker,
//   ported from the standalone sgcrelay firmware (../sgcrelay/sgcrelay.ino). What
//   carried over: the codec/I2S bring-up sequence (verbatim from Freenove's
//   Sketch_07.1_Music via sgcrelay), the ESP32-audioI2S streaming, and the
//   retry-with-backoff reconnect logic. What didn't: sgcrelay's LovyanGFX touch UI
//   (this panel belongs to Display.ino's TFT_eSPI mirror -- volume is a shell
//   command now), its Wi-Fi handling (WiFiManager.ino owns STA), and the WS2812/
//   BOOT-button controls (redundant with the shell).
//
//   Unlike sgcrelay, playback does NOT run on the main loop: DOLL-OS's modal sessions
//   (ssh, outbound telnet) monopolize loop() for their whole duration and
//   drawDisplayFrame()'s full-frame SPI push adds jitter, so audio.loop() is pumped
//   from a dedicated long-lived FreeRTOS task (same pattern as Ssh.ino's, but
//   persistent). The shell talks to the task through a one-slot command mailbox and
//   the task publishes status back through fixed-size shared buffers -- both guarded
//   by radioMux. ESP32-audioI2S's weak callbacks (audio_showstreamtitle etc.) fire
//   in the *task's* context, so they must never call outLine() themselves (it writes
//   the telnet socket and display history the main loop owns); they stash an
//   announcement instead, which radioService() (called every loop() tick) prints.
//
//   Codec, SD, and DS-Slave wiring differs substantially on the N variant. All
//   values come through BoardPins.h so every audio owner uses the same map.

#include "Audio.h"
#include <SD_MMC.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <new>   //std::nothrow -- radioEnsureCodec heap-constructs the Audio engine

//RADIO_VOLUME_MAX lives in global.h -- Gameboy.ino's settings menu shows the level
//too, and the .ino files concatenate alphabetically, so Gameboy.ino is compiled
//above this file and can't see a constant defined here.
const unsigned long RADIO_STREAM_RETRY_MS = 5000;
const int RADIO_TASK_STACK_SIZE = 12288;     //MP3 decode runs on this stack (audio.loop())
const int RADIO_DIRECTORY_MAX_STATIONS = 16;
const int RADIO_DIRECTORY_NAME_MAX = 64;
const int RADIO_DIRECTORY_URL_MAX = 256;
const int RADIO_DIRECTORY_BASE_MAX = 224;

//   one-slot command mailbox, shell -> task (kinds: RadioCommandKind, global.h).
//   A second command before the task consumed the first simply overwrites it --
//   single-user shell, latest wins.
static portMUX_TYPE radioMux = portMUX_INITIALIZER_UNLOCKED;
static RadioCommandKind radioCmdKind = RADIO_CMD_NONE;
static char radioCmdUrl[256] = "";
static int radioCmdValue = 0;                 //volume or relative seek seconds, by command kind

//   status published by the task, snapshotted by the shell/service under radioMux.
//   Fixed char buffers, not Strings: a String copy allocates, and allocating inside
//   a spinlock critical section is asking for trouble.
static RadioState radioState = RADIO_OFF;
static char radioTitle[128] = "";            //ICY StreamTitle (or station name until one arrives)
static char radioArtist[MUSIC_METADATA_MAX] = "";
static char radioAlbum[MUSIC_METADATA_MAX] = "";
static char radioUrl[256] = "";              //last URL played, reused by a bare "radio play"
static char radioFilePath[MUSIC_PATH_MAX] = ""; //active SD_MMC path (without the /sd namespace prefix)
static int radioVolume = RADIO_DEFAULT_VOLUME;
static char radioAnnounceText[160] = "";     //one pending line for radioService() to print
static int radioAnnounceColor = C_WHITE;
static bool radioAnnouncePending = false;

static bool radioDefaultsInitialized = false;

static const uint8_t RADIO_SOURCE_NONE = 0;
static const uint8_t RADIO_SOURCE_STREAM = 1;
static const uint8_t RADIO_SOURCE_FILE = 2;
static uint8_t radioSourceKind = RADIO_SOURCE_NONE;
static uint32_t radioCurrentSeconds = 0;
static uint32_t radioDurationSeconds = 0;
static bool radioLocalEofPending = false;

static char radioDirectoryNames[RADIO_DIRECTORY_MAX_STATIONS][RADIO_DIRECTORY_NAME_MAX];
static char radioDirectoryUrls[RADIO_DIRECTORY_MAX_STATIONS][RADIO_DIRECTORY_URL_MAX];
static bool radioDirectoryRunning[RADIO_DIRECTORY_MAX_STATIONS];
static int radioDirectoryClients[RADIO_DIRECTORY_MAX_STATIONS];
static int radioDirectoryCount = 0;
static char radioDirectoryBase[RADIO_DIRECTORY_BASE_MAX] = "";

//Runs once, lazily, the first time "radio" is used post-boot. radioVolume's static
//initializer above runs at global-construction time, before LittleFS is mounted, so
//a "settings set radio.volume" override can't be read there -- it has to be applied
//here instead (same lazy-init shape as Asuka.ino's asukaEnsureDefaults()).
static void radioEnsureDefaults() {
    if (radioDefaultsInitialized) {
        return;
    }
    radioDefaultsInitialized = true;
    int savedVolume = settingsGet("radio.volume", String(RADIO_DEFAULT_VOLUME)).toInt();
    if (savedVolume < 0 || savedVolume > RADIO_VOLUME_MAX) {
        return;
    }
    portENTER_CRITICAL(&radioMux);
    radioVolume = savedVolume;
    portEXIT_CRITICAL(&radioMux);
}

static TaskHandle_t radioTaskHandle = NULL;

//   task-local -- only the radio task touches these after creation.
//   radioAudio is heap-constructed lazily on the first "radio play" (radioEnsureCodec)
//   rather than being a global: ESP32-audioI2S's constructor immediately allocates the
//   I2S channel's DMA buffers (~28KB of DMA-capable *internal* RAM -- PSRAM can't serve
//   DMA) and spawns its decode task, and as a global that all happened before setup(),
//   starving the WiFi stack's bring-up of internal RAM (seen as an esp-sha OOM -> crash
//   in ieee80211_hostap_attach, back when DOLL-OS still ran a softAP). Deferring it means the
//   cost is only paid after WiFi is up,
//   and only if the radio is actually used.
static Audio* radioAudio = nullptr;
static bool radioCodecReady = false;
static volatile bool radioReleased = false;  //set by the task once RADIO_CMD_RELEASE has torn the
                                              //I2S controllers down -- radioReleaseAudio() waits on it
static bool radioWantPlaying = false;        //user intent: keep the stream up (drives auto-reconnect)
static bool radioPaused = false;
static unsigned long radioLastAttemptMs = 0;

//---------------------------------------------------------------------------
//task side

//stash one line for the main loop to print -- safe to call from the task/callbacks,
//where outLine() is not
static void radioAnnounce(const char* text, int color) {
    portENTER_CRITICAL(&radioMux);
    strncpy(radioAnnounceText, text, sizeof(radioAnnounceText) - 1);
    radioAnnounceText[sizeof(radioAnnounceText) - 1] = '\0';
    radioAnnounceColor = color;
    radioAnnouncePending = true;
    portEXIT_CRITICAL(&radioMux);
}

static void radioSetState(RadioState s) {
    portENTER_CRITICAL(&radioMux);
    radioState = s;
    portEXIT_CRITICAL(&radioMux);
    ledSetRadioState(s);
}

static constexpr uint8_t TAB5_ES8388_ADDRESS = 0x10;
static constexpr uint8_t TAB5_PI4IO1_ADDRESS = 0x43;

struct AudioCodecRegister {
    uint8_t reg;
    uint8_t value;
};

//Both helpers below run on radioTask while loop() is polling the INA226 on the same
//bus, so each holds boardI2cLock() across its whole transaction (see global.h). The
//lock is taken *outside* the retry loop: a retry only makes sense if the bus is
//genuinely busy, and releasing between attempts would let the racing reader back in.
static bool audioCodecWrite(uint8_t reg, uint8_t value) {
    if (!boardI2cLock(1000)) {
        Serial.printf("[audio] I2C bus busy, skipped ES8388 register 0x%02X\n", reg);
        return false;
    }
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        ok = M5.In_I2C.writeRegister8(TAB5_ES8388_ADDRESS, reg, value, AUDIO_I2C_SPEED);
        if (!ok) {
            delay(1);
        }
    }
    boardI2cUnlock();
    if (!ok) {
        Serial.printf("[audio] ES8388 write failed at register 0x%02X\n", reg);
    }
    return ok;
}  // Writes one codec register with short retries for a busy shared internal bus.

//PI4IO1 register 0x05 is a read-modify-write of a byte this board also keeps the LCD
//reset (bit4) and touch reset (bit5) in, so an unlocked bitOn here can write those
//lines back low and take the panel down with it. This is the access that made "radio
//play" kill the tablet outright; the lock is what makes it safe, not the retry.
static bool audioCodecSetAmp(bool enabled) {
    if (!boardI2cLock(1000)) {
        Serial.println("[audio] I2C bus busy, speaker amp left unchanged");
        return false;
    }
    // Driving the output byte alone is not enough to make a pin drive. The
    // PI4IO5V6408 gates every output behind two more registers: 0x03 selects
    // input vs output and 0x07 forces high-impedance. Anything that reset the
    // expander -- it is independently powered and survives esp_restart, same as
    // the panel -- leaves the amp pin floating, and writing 0x05 then looks like
    // it worked while nothing reaches the speaker. Assert all three on enable.
    bool ok = true;
    if (enabled) {
        ok = M5.In_I2C.bitOn(TAB5_PI4IO1_ADDRESS, 0x03, 0b00000010, AUDIO_I2C_SPEED) && ok;
        ok = M5.In_I2C.bitOff(TAB5_PI4IO1_ADDRESS, 0x07, 0b00000010, AUDIO_I2C_SPEED) && ok;
    }
    ok = (enabled
        ? M5.In_I2C.bitOn(TAB5_PI4IO1_ADDRESS, 0x05, 0b00000010, AUDIO_I2C_SPEED)
        : M5.In_I2C.bitOff(TAB5_PI4IO1_ADDRESS, 0x05, 0b00000010, AUDIO_I2C_SPEED)) && ok;

    const uint8_t dir = M5.In_I2C.readRegister8(TAB5_PI4IO1_ADDRESS, 0x03, AUDIO_I2C_SPEED);
    const uint8_t out = M5.In_I2C.readRegister8(TAB5_PI4IO1_ADDRESS, 0x05, AUDIO_I2C_SPEED);
    const uint8_t hiz = M5.In_I2C.readRegister8(TAB5_PI4IO1_ADDRESS, 0x07, AUDIO_I2C_SPEED);
    boardI2cUnlock();

    Serial.printf("[audio] amp %s dir=0x%02X out=0x%02X hiz=0x%02X -> pin %s\n",
                  enabled ? "on" : "off", dir, out, hiz,
                  ((dir & 0x02) && (out & 0x02) && !(hiz & 0x02)) ? "driving" : "NOT driving");
    if (!ok) {
        Serial.println("[audio] Tab5 speaker amplifier control failed");
    }
    return ok;
}  // Controls the Tab5 speaker amp through its onboard PI4IO expander.

void audioCodecSetOutputEnabled(bool enabled) {
    audioCodecSetAmp(enabled);
}  // Lets emulator/synth owners mute the amp before releasing their I2S clocks.

//Programs the Tab5 ES8388 once, then selects the clock ratio for the current I2S
//owner. Both ESP32-audioI2S (patched at build time) and the Game Boy sink use the
//board-native 128x MCLK, keeping the high-frequency clock out of the speaker's analog
//noise floor. Caller already has MCLK running because the ES8388 operates as an I2S
//slave. Not static: AudioOut.cpp reuses it.
// File scope so audioCodecForceReinit() can clear them. The ES8388 is on the
// shared internal bus and is not reset by an esp_restart, so a latch that only
// ever programs it once cannot recover a codec that lost its configuration.
static bool audioCodecRegsReady = false;
static uint16_t audioCodecConfiguredMclk = 0;

void audioCodecForceReinit() {
    audioCodecRegsReady = false;
    audioCodecConfiguredMclk = 0;
}  // Next audioCodecEnsure() rewrites every register, soft reset included.

bool audioCodecEnsure(uint16_t mclkMultiple) {
    bool& codecRegsReady = audioCodecRegsReady;
    uint16_t& configuredMclkMultiple = audioCodecConfiguredMclk;
    static constexpr AudioCodecRegister initRegisters[] = {
        {0, 0x80}, {0, 0x00}, {0, 0x00}, {0, 0x0E},
        {1, 0x00}, {2, 0x0A}, {3, 0xFF}, {4, 0x3C},
        {5, 0x00}, {6, 0x00}, {7, 0x7C}, {8, 0x00},
        {23, 0x18}, {25, 0x20}, {26, 0x00}, {27, 0x00},
        {28, 0x08}, {29, 0x00}, {38, 0x00}, {39, 0xB8},
        {42, 0xB8}, {43, 0x08}, {45, 0x00}, {46, 0x21},
        {47, 0x21}, {48, 0x21}, {49, 0x21},
    };

    if (!codecRegsReady) {
        for (const AudioCodecRegister& setting : initRegisters) {
            if (!audioCodecWrite(setting.reg, setting.value)) {
                Serial.println("[audio] ES8388 codec init failed (expected at I2C 0x10)");
                return false;
            }
        }
        codecRegsReady = true;
        Serial.println("[audio] ES8388 codec up");
    }

    uint8_t ratioRegister = 0;
    if (mclkMultiple == 128) {
        ratioRegister = 0x00;
    } else if (mclkMultiple == 384) {
        //ES8388 DAC Control 2 bits 4:0 encode 384x as 00011. 0x06 is
        //768x, which makes the codec's sample clock disagree with the
        //384x MCLK generated by ESP32-audioI2S and can produce silence.
        ratioRegister = 0x03;
    } else {
        Serial.printf("[audio] unsupported ES8388 MCLK multiple: %u\n", mclkMultiple);
        return false;
    }
    if (configuredMclkMultiple != mclkMultiple) {
        if (!audioCodecWrite(24, ratioRegister)) {
            return false;
        }
        configuredMclkMultiple = mclkMultiple;
    }
    return audioCodecSetAmp(true);
}

//Codec + I2S bring-up, once, lazily on the first "radio play".
static bool radioEnsureCodec() {
    if (radioCodecReady) {
        return true;
    }

    if (radioAudio == nullptr) {
        //The ES8388 only needs the Audio engine's own live MCLK; the old second
        //"bootstrap" controller was an ES8311 workaround and fought these same pins.
        radioAudio = new (std::nothrow) Audio(I2S_NUM_0);
        if (radioAudio == nullptr) {
            radioAnnounce("radio: out of memory for audio engine", C_RED);
            return false;
        }
    }

    //setPinout fails if the ctor couldn't get an I2S controller (or the reconfig itself
    //fails) -- tear the engine back down so the next attempt starts from a clean slate
    //instead of driving a NULL channel handle
    if (!radioAudio->setPinout(AUDIO_I2S_BCLK_PIN, AUDIO_I2S_WS_PIN,
                               AUDIO_I2S_DOUT_PIN, AUDIO_I2S_MCLK_PIN)) {
        delete radioAudio;
        radioAudio = nullptr;
        radioAnnounce("radio: audio engine could not attach I2S", C_RED);
        return false;
    }
    if (!audioCodecEnsure(128)) {
        delete radioAudio;
        radioAudio = nullptr;
        radioAnnounce("radio: ES8388 codec init failed (see serial log)", C_RED);
        return false;
    }
    //v3.4.x delivers titles/station/eof/info through this one callback (radioAudioInfo).
    //It's a static member, so registering once is enough -- do it before any connect.
    Audio::audio_info_callback = radioAudioInfo;
    //cap the blocking connect so an unreachable host can't hold this task (core 1, prio
    //above loopTask) past the ~5s task-watchdog window and reset the board.
    radioAudio->setConnectionTimeout(3000, 4000);
    radioAudio->setVolume(radioVolume);

    radioCodecReady = true;
    return true;
}

static void radioConnect(const char* url) {
    portENTER_CRITICAL(&radioMux);
    radioSourceKind = RADIO_SOURCE_STREAM;
    radioTitle[0] = '\0';
    radioArtist[0] = '\0';
    radioAlbum[0] = '\0';
    radioFilePath[0] = '\0';
    radioCurrentSeconds = 0;
    radioDurationSeconds = 0;
    radioLocalEofPending = false;
    portEXIT_CRITICAL(&radioMux);
    radioSetState(RADIO_CONNECTING);
    radioLastAttemptMs = millis();
    Serial.printf("[radio] connecting: %s\n", url);
    radioAudio->connecttohost(url);
}

static void radioConnectFile(const char* path) {
    portENTER_CRITICAL(&radioMux);
    radioSourceKind = RADIO_SOURCE_FILE;
    radioTitle[0] = '\0';
    radioArtist[0] = '\0';
    radioAlbum[0] = '\0';
    strncpy(radioFilePath, path, sizeof(radioFilePath) - 1);
    radioFilePath[sizeof(radioFilePath) - 1] = '\0';
    radioCurrentSeconds = 0;
    radioDurationSeconds = 0;
    radioLocalEofPending = false;
    portEXIT_CRITICAL(&radioMux);

    radioSetState(RADIO_CONNECTING);
    Serial.printf("[music] opening: %s\n", path);
    if (!radioAudio->connecttoFS(SD_MMC, path)) {
        radioSetState(RADIO_ERROR);
        radioAnnounce("music: could not open track", C_RED);
        return;
    }
    radioSetState(RADIO_PLAYING);
}

static void radioTaskHandleCommand() {
    //snapshot + clear the mailbox under the lock, act on it outside
    portENTER_CRITICAL(&radioMux);
    RadioCommandKind kind = radioCmdKind;
    char url[sizeof(radioCmdUrl)];
    strcpy(url, radioCmdUrl);
    int value = radioCmdValue;
    radioCmdKind = RADIO_CMD_NONE;
    portEXIT_CRITICAL(&radioMux);

    switch (kind) {
        case RADIO_CMD_NONE:
            break;
        case RADIO_CMD_PLAY:
            if (!radioEnsureCodec()) {
                radioSetState(RADIO_ERROR);
                break;
            }
            radioWantPlaying = true;
            radioPaused = false;
            radioConnect(url);
            break;
        case RADIO_CMD_PLAY_FILE:
            if (!radioEnsureCodec()) {
                radioSetState(RADIO_ERROR);
                break;
            }
            radioWantPlaying = false;   //stream-only reconnect intent
            radioPaused = false;
            radioConnectFile(url);
            break;
        case RADIO_CMD_PAUSE:
            if (radioSourceKind == RADIO_SOURCE_NONE || radioAudio == nullptr) {
                break;
            }
            radioAudio->pauseResume();
            radioPaused = !radioAudio->isRunning();
            radioSetState(radioPaused ? RADIO_PAUSED : RADIO_PLAYING);
            radioAnnounce(radioPaused ? "radio: paused" : "radio: resumed", C_PINK);
            break;
        case RADIO_CMD_STOP:
            radioWantPlaying = false;
            radioPaused = false;
            portENTER_CRITICAL(&radioMux);
            radioSourceKind = RADIO_SOURCE_NONE;
            radioCurrentSeconds = 0;
            radioDurationSeconds = 0;
            radioLocalEofPending = false;
            portEXIT_CRITICAL(&radioMux);
            if (radioAudio != nullptr) {
                radioAudio->stopSong();
            }
            radioSetState(RADIO_STOPPED);
            radioAnnounce("radio: stopped", C_PINK);
            break;
        case RADIO_CMD_VOLUME:
            if (radioAudio != nullptr) {
                radioAudio->setVolume(value);
            }
            break;
        case RADIO_CMD_SEEK:
            if (radioAudio != nullptr && radioSourceKind == RADIO_SOURCE_FILE) {
                radioAudio->setTimeOffset(value);
            }
            break;
        case RADIO_CMD_RELEASE:
            //Give the I2S controller back (see radioReleaseAudio). Done here, on the
            //task, rather than by the caller: radioAudio is task-local and audio.loop()
            //is running on this stack -- deleting the engine from the main loop would
            //race the decoder mid-frame.
            radioWantPlaying = false;
            radioPaused = false;
            portENTER_CRITICAL(&radioMux);
            radioSourceKind = RADIO_SOURCE_NONE;
            radioCurrentSeconds = 0;
            radioDurationSeconds = 0;
            radioLocalEofPending = false;
            portEXIT_CRITICAL(&radioMux);
            if (radioAudio != nullptr) {
                radioAudio->stopSong();
                delete radioAudio;
                radioAudio = nullptr;
            }
            audioCodecSetOutputEnabled(false);
            //Codec base registers stay programmed (audioCodecEnsure keeps its own latch);
            //only the streaming side has to be rebuilt on the next "radio play"
            radioCodecReady = false;
            radioSetState(RADIO_STOPPED);
            radioReleased = true;
            break;
    }
}

//long-lived task: pumps the decoder, consumes shell commands, and keeps the stream
//up (reconnect with backoff) whenever the user's intent is "playing" -- mirrors
//sgcrelay's loop(), which this task replaces
static void radioTaskEntry(void* pvParameters) {
    (void)pvParameters;
    unsigned long lastProgressMs = 0;
    while (true) {
        radioTaskHandleCommand();
        if (radioAudio != nullptr) {
            radioAudio->loop();
        }

        bool localEnded = false;
        portENTER_CRITICAL(&radioMux);
        if (radioLocalEofPending) {
            localEnded = true;
            radioLocalEofPending = false;
        }
        portEXIT_CRITICAL(&radioMux);
        if (localEnded) {
            char nextPath[MUSIC_PATH_MAX];
            if (musicNextFileForAudioTask(nextPath, sizeof(nextPath))) {
                radioConnectFile(nextPath);
            }
        }

        if (radioAudio != nullptr && radioSourceKind == RADIO_SOURCE_FILE
            && millis() - lastProgressMs >= 250) {
            uint32_t current = radioAudio->getAudioCurrentTime();
            uint32_t duration = radioAudio->getAudioFileDuration();
            portENTER_CRITICAL(&radioMux);
            radioCurrentSeconds = current;
            radioDurationSeconds = duration;
            portEXIT_CRITICAL(&radioMux);
            lastProgressMs = millis();
        }

        if (radioWantPlaying && !radioPaused) {
            if (radioAudio->isRunning()) {
                radioSetState(RADIO_PLAYING);
            } else if (WiFi.status() == WL_CONNECTED
                       && millis() - radioLastAttemptMs > RADIO_STREAM_RETRY_MS) {
                Serial.println("[radio] stream not running, retrying...");
                char url[sizeof(radioUrl)];
                portENTER_CRITICAL(&radioMux);
                strcpy(url, radioUrl);
                portEXIT_CRITICAL(&radioMux);
                radioConnect(url);
            }
        }

        vTaskDelay(1);   //yield so the core-1 idle task still runs (watchdog) -- the
                          //decoder's internal buffering rides out far longer gaps
    }
}

//---------------------------------------------------------------------------
//ESP32-audioI2S status callback. As of v3.4.x the library funnels every event
//(the old weak audio_info/audio_showstreamtitle/audio_showstation/audio_eof_stream
//free functions are gone) through one std::function<void(msg_t)>, registered in
//radioEnsureCodec. It fires inside the radio task (audio.loop()'s caller), so the
//same rule as before holds: radioAnnounce()/shared-buffer writes and Serial only --
//never outLine(). m.msg points at a library scratch buffer valid only for this call,
//so anything kept is copied out synchronously here.
static bool radioCopyMetadataValue(const char* text, const char* prefix,
                                   char* destination, size_t destinationSize) {
    size_t prefixLength = strlen(prefix);
    if (strncmp(text, prefix, prefixLength) != 0) {
        return false;
    }
    const char* value = text + prefixLength;
    while (*value == ' ') value++;
    portENTER_CRITICAL(&radioMux);
    strncpy(destination, value, destinationSize - 1);
    destination[destinationSize - 1] = '\0';
    portEXIT_CRITICAL(&radioMux);
    return true;
}

void radioAudioInfo(Audio::msg_t m) {
    const char* text = (m.msg != nullptr) ? m.msg : "";
    uint8_t sourceKind;
    portENTER_CRITICAL(&radioMux);
    sourceKind = radioSourceKind;
    portEXIT_CRITICAL(&radioMux);
    switch (m.e) {
        case Audio::evt_id3data:
            if (sourceKind == RADIO_SOURCE_FILE) {
                if (!radioCopyMetadataValue(text, "Title:", radioTitle, sizeof(radioTitle))
                    && !radioCopyMetadataValue(text, "Title/Songname/Content description:", radioTitle, sizeof(radioTitle))
                    && !radioCopyMetadataValue(text, "Artist:", radioArtist, sizeof(radioArtist))
                    && !radioCopyMetadataValue(text, "Lead artist(s)/Lead performer(s)/Soloist(s)/Performing group:", radioArtist, sizeof(radioArtist))) {
                    if (!radioCopyMetadataValue(text, "Album:", radioAlbum, sizeof(radioAlbum))) {
                        radioCopyMetadataValue(text, "Album/Movie/Show title:", radioAlbum, sizeof(radioAlbum));
                    }
                }
            }
            Serial.printf("[music] metadata: %s\n", text);
            break;
        case Audio::evt_streamtitle: {
            if (sourceKind != RADIO_SOURCE_STREAM) break;
            portENTER_CRITICAL(&radioMux);
            strncpy(radioTitle, text, sizeof(radioTitle) - 1);
            radioTitle[sizeof(radioTitle) - 1] = '\0';
            portEXIT_CRITICAL(&radioMux);
            String line = "radio: now playing: " + String(text);
            radioAnnounce(line.c_str(), C_CYAN);
            break;
        }
        case Audio::evt_name: {
            //station name -- placeholder until the first ICY StreamTitle arrives
            if (sourceKind == RADIO_SOURCE_STREAM) {
                portENTER_CRITICAL(&radioMux);
                if (radioTitle[0] == '\0') {
                    strncpy(radioTitle, text, sizeof(radioTitle) - 1);
                    radioTitle[sizeof(radioTitle) - 1] = '\0';
                }
                portEXIT_CRITICAL(&radioMux);
                Serial.printf("[radio] station: %s\n", text);
            }
            break;
        }
        case Audio::evt_eof:
            if (sourceKind == RADIO_SOURCE_FILE) {
                Serial.printf("[music] track ended: %s\n", text);
                portENTER_CRITICAL(&radioMux);
                radioLocalEofPending = true;
                radioCurrentSeconds = radioDurationSeconds;
                portEXIT_CRITICAL(&radioMux);
                radioSetState(RADIO_STOPPED);
            } else if (sourceKind == RADIO_SOURCE_STREAM) {
                Serial.printf("[radio] stream ended: %s\n", text);
                radioSetState(RADIO_ERROR);
                //no announce -- the task's reconnect logic retries on its own; only the
                //user-visible state (radio status) reflects the hiccup
            }
            break;
        default:
            //evt_info / evt_log / bitrate / icy-url etc. -- serial trace, as audio_info did
            Serial.printf("[radio] %s\n", text);
            break;
    }
}

//---------------------------------------------------------------------------
//main-loop side

//called every loop() tick (DS.ino): prints whatever the task/callbacks stashed.
//This is the only place radio output enters the telnet socket + display mirror,
//keeping both single-writer (the main loop).
void radioService() {
    if (!radioAnnouncePending) {   //racy peek is fine -- worst case we print next tick
        return;
    }
    char text[sizeof(radioAnnounceText)];
    int color;
    portENTER_CRITICAL(&radioMux);
    strcpy(text, radioAnnounceText);
    color = radioAnnounceColor;
    radioAnnouncePending = false;
    portEXIT_CRITICAL(&radioMux);
    outLine(String(text), color);
}

static void radioPostCommand(RadioCommandKind kind, const char* url, int value) {
    portENTER_CRITICAL(&radioMux);
    radioCmdKind = kind;
    if (url != NULL) {
        strncpy(radioCmdUrl, url, sizeof(radioCmdUrl) - 1);
        radioCmdUrl[sizeof(radioCmdUrl) - 1] = '\0';
        if (kind == RADIO_CMD_PLAY) strcpy(radioUrl, radioCmdUrl);
    }
    radioCmdValue = value;
    portEXIT_CRITICAL(&radioMux);
}

static bool radioEnsureTask() {
    if (radioTaskHandle != NULL) {
        return true;
    }
    //same core as the ssh task (portNUM_PROCESSORS - 1); priority above the loop
    //task (1) so decode keeps up, below ssh's +3 so an active ssh session stays
    //responsive during its short lifetime
    BaseType_t created = xTaskCreatePinnedToCore(radioTaskEntry, "radioTask", RADIO_TASK_STACK_SIZE,
        NULL, (tskIDLE_PRIORITY + 2), &radioTaskHandle, portNUM_PROCESSORS - 1);
    if (created != pdPASS) {
        radioTaskHandle = NULL;
        outLine("radio: could not start playback task (out of memory)", C_RED);
        return false;
    }
    return true;
}

//Local-file surface used by Music.ino. Audio remains owned by radioTask: callers only
//post commands and read fixed snapshots, so the decoder/File object is never touched
//from the UI task while its internal decode task is running.
bool radioPlayLocalFile(const char* realPath) {
    if (!realPath || realPath[0] == '\0' || !radioEnsureTask()) {
        return false;
    }
    radioPostCommand(RADIO_CMD_PLAY_FILE, realPath, 0);
    return true;
}

void radioTogglePlaybackPause() {
    if (radioTaskHandle != NULL) {
        radioPostCommand(RADIO_CMD_PAUSE, NULL, 0);
    }
}

void radioStopPlayback() {
    if (radioTaskHandle != NULL) {
        radioPostCommand(RADIO_CMD_STOP, NULL, 0);
    }
}

void radioSeekPlayback(int seconds) {
    if (radioTaskHandle != NULL && seconds != 0) {
        radioPostCommand(RADIO_CMD_SEEK, NULL, seconds);
    }
}

bool radioTakeLocalEof() {
    bool pending;
    portENTER_CRITICAL(&radioMux);
    pending = radioLocalEofPending;
    radioLocalEofPending = false;
    portEXIT_CRITICAL(&radioMux);
    return pending;
}

void radioGetPlaybackSnapshot(RadioState& state, bool& isLocal,
                              char* title, size_t titleSize,
                              char* artist, size_t artistSize,
                              char* album, size_t albumSize,
                              uint32_t& currentSeconds, uint32_t& durationSeconds) {
    portENTER_CRITICAL(&radioMux);
    state = radioState;
    isLocal = radioSourceKind == RADIO_SOURCE_FILE;
    if (title && titleSize > 0) {
        strncpy(title, radioTitle, titleSize - 1);
        title[titleSize - 1] = '\0';
    }
    if (artist && artistSize > 0) {
        strncpy(artist, radioArtist, artistSize - 1);
        artist[artistSize - 1] = '\0';
    }
    if (album && albumSize > 0) {
        strncpy(album, radioAlbum, albumSize - 1);
        album[albumSize - 1] = '\0';
    }
    currentSeconds = radioCurrentSeconds;
    durationSeconds = radioDurationSeconds;
    portEXIT_CRITICAL(&radioMux);
}

static const char* radioStateName(RadioState s) {
    switch (s) {
        case RADIO_OFF:        return "off";
        case RADIO_CONNECTING: return "connecting";
        case RADIO_PLAYING:    return "playing";
        case RADIO_PAUSED:     return "paused";
        case RADIO_STOPPED:    return "stopped";
        case RADIO_ERROR:      return "error (retrying)";
    }
    return "?";
}

static void radioPrintStatus() {
    portENTER_CRITICAL(&radioMux);
    RadioState state = radioState;
    char title[sizeof(radioTitle)];
    strcpy(title, radioTitle);
    char url[sizeof(radioUrl)];
    strcpy(url, radioUrl);
    int volume = radioVolume;
    portEXIT_CRITICAL(&radioMux);

    outLine("");
    outLine("Radio status", C_CYAN);
    outLine("------------");
    outLine("State: " + String(radioStateName(state)));
    if (url[0] != '\0') {
        outLine("Stream: " + String(url));
    }
    if (title[0] != '\0') {
        outLine("Now playing: " + String(title));
    }
    outLine("Volume: " + String(volume) + "/" + String(RADIO_VOLUME_MAX));
    outLine("");
}

static void radioServiceUi() {
    ftpService();
    radioService();
    maintainInternetConnection();
    ledService();
    drawDisplayFrame();
    delay(1);
}

static String radioDirectoryBaseUrl(const String& url) {
    int schemeEnd = url.indexOf("://");
    int lastSlash = url.lastIndexOf('/');
    if (schemeEnd >= 0 && lastSlash > schemeEnd + 2) {
        return url.substring(0, lastSlash);
    }
    return url;
}

static String radioDirectoryPlayableUrl(const String& pathOrUrl) {
    if (pathOrUrl.startsWith("http://") || pathOrUrl.startsWith("https://")) {
        return pathOrUrl;
    }
    String path = pathOrUrl;
    if (!path.startsWith("/")) {
        path = "/" + path;
    }
    String base = radioDirectoryBase[0] != '\0' ? String(radioDirectoryBase) : radioDirectoryBaseUrl(RADIO_DIRECTORY_URL);
    return base + path;
}

static bool radioDirectoryAddStation(JsonObjectConst station) {
    if (radioDirectoryCount >= RADIO_DIRECTORY_MAX_STATIONS) {
        return false;
    }

    String name = String((const char*)(station["name"] | ""));
    String path = String((const char*)(station["path"] | ""));
    if (path.length() == 0) path = String((const char*)(station["url"] | ""));
    if (path.length() == 0) path = String((const char*)(station["stream"] | ""));
    if (path.length() == 0) path = String((const char*)(station["stream_url"] | ""));
    if (path.length() == 0) path = String((const char*)(station["inputUrl"] | ""));
    if (path.length() == 0) {
        return false;
    }
    if (name.length() == 0) {
        name = path;
    }

    String playableUrl = radioDirectoryPlayableUrl(path);
    strncpy(radioDirectoryNames[radioDirectoryCount], name.c_str(), RADIO_DIRECTORY_NAME_MAX - 1);
    radioDirectoryNames[radioDirectoryCount][RADIO_DIRECTORY_NAME_MAX - 1] = '\0';
    strncpy(radioDirectoryUrls[radioDirectoryCount], playableUrl.c_str(), RADIO_DIRECTORY_URL_MAX - 1);
    radioDirectoryUrls[radioDirectoryCount][RADIO_DIRECTORY_URL_MAX - 1] = '\0';
    radioDirectoryRunning[radioDirectoryCount] = station["running"] | false;
    radioDirectoryClients[radioDirectoryCount] = station["clients"] | 0;
    radioDirectoryCount++;
    return true;
}

static bool radioDirectoryReadArray(JsonArrayConst stations) {
    bool added = false;
    for (JsonObjectConst station : stations) {
        added = radioDirectoryAddStation(station) || added;
    }
    return added;
}

static bool radioFetchDirectory(String& error) {
    radioDirectoryCount = 0;
    radioDirectoryBase[0] = '\0';
    if (WiFi.status() != WL_CONNECTED) {
        error = "WiFi not connected. Run 'wifi connect' first.";
        return false;
    }

    String directoryUrl = settingsGet("radio.directory_url", RADIO_DIRECTORY_URL);
    String baseUrl = radioDirectoryBaseUrl(directoryUrl);
    strncpy(radioDirectoryBase, baseUrl.c_str(), RADIO_DIRECTORY_BASE_MAX - 1);
    radioDirectoryBase[RADIO_DIRECTORY_BASE_MAX - 1] = '\0';
    bool secure = directoryUrl.startsWith("https://");
    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    HTTPClient http;
    http.setTimeout(10000);
    if (secure) {
        secureClient.setInsecure();
        if (!http.begin(secureClient, directoryUrl)) {
            error = "could not open " + directoryUrl;
            return false;
        }
    } else if (!http.begin(plainClient, directoryUrl)) {
        error = "could not open " + directoryUrl;
        return false;
    }

    http.addHeader("Accept", "application/json");
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("User-Agent", "DOLL-OS-radio/1.0");
    ledPulseNetwork();
    int httpCode = http.GET();
    String body = http.getString();
    http.end();

    if (httpCode != HTTP_CODE_OK) {
        error = "directory returned HTTP " + String(httpCode);
        return false;
    }

    JsonDocument doc;
    DeserializationError jsonError = deserializeJson(doc, body);
    if (jsonError) {
        error = "directory JSON parse failed: " + String(jsonError.c_str());
        return false;
    }

    bool added = false;
    if (doc["stations"].is<JsonArrayConst>()) {
        added = radioDirectoryReadArray(doc["stations"].as<JsonArrayConst>()) || added;
    }
    if (!added && doc["routes"].is<JsonArrayConst>()) {
        added = radioDirectoryReadArray(doc["routes"].as<JsonArrayConst>()) || added;
    }
    if (!added) {
        error = "directory JSON did not include any playable stations";
        return false;
    }
    return true;
}

static void radioPrintDirectory() {
    outLine("");
    outLine("Radio stations", C_CYAN);
    outLine("--------------");
    for (int i = 0; i < radioDirectoryCount; i++) {
        String line = String(i + 1) + ". " + String(radioDirectoryNames[i]);
        line += radioDirectoryRunning[i] ? " [on]" : " [idle]";
        line += " " + String(radioDirectoryClients[i]) + " listener";
        if (radioDirectoryClients[i] != 1) {
            line += "s";
        }
        outLine(line);
        outLine("   " + String(radioDirectoryUrls[i]));
    }
    outLine("");
}

static bool radioParseDirectoryChoice(const String& input, int& index) {
    String choice = input;
    choice.trim();
    choice.toLowerCase();
    if (choice.length() == 0 || choice == "q" || choice == "quit" || choice == "cancel") {
        index = -1;
        return true;
    }

    int number = choice.toInt();
    if (number > 0 && number <= radioDirectoryCount && String(number) == choice) {
        index = number - 1;
        return true;
    }

    for (int i = 0; i < radioDirectoryCount; i++) {
        String name = String(radioDirectoryNames[i]);
        name.toLowerCase();
        if (choice == name) {
            index = i;
            return true;
        }
    }
    return false;
}

static bool radioPromptDirectoryChoice(int& index) {
    String answer = "";
    commandCursorPos = 0;
    const String prompt = "radio station> ";
    if (telnetClient && telnetClient.connected()) {
        telnetClient.print(prompt);
    }
    setActiveInput(prompt, answer, false);

    while (true) {
        LineInputResult r = readLineEditedInput(answer);
        if (r == LINE_NO_INPUT) {
            r = readKeyboardLineEditedInput(answer);
        }
        setActiveInput(prompt, answer, false);
        radioServiceUi();

        if (r != LINE_SUBMITTED) {
            continue;
        }

        String submitted = answer;
        answer = "";
        commandCursorPos = 0;
        outLine(prompt + submitted, C_CYAN);
        if (radioParseDirectoryChoice(submitted, index)) {
            setActiveInput(shellPrompt(), "", false);
            return index >= 0;
        }

        outLine("radio: choose 1-" + String(radioDirectoryCount) + ", a station name, or q", C_RED);
        if (telnetClient && telnetClient.connected()) {
            telnetClient.print(prompt);
        }
        setActiveInput(prompt, answer, false);
    }
}

static void radioPlayUrl(const String& url) {
    if (!radioEnsureTask()) {
        return;
    }
    radioPostCommand(RADIO_CMD_PLAY, url.c_str(), 0);
    outLine("radio: connecting to " + url, C_PINK);
}

static void radioHandleListCommand(const String parts[], int partCount) {
    outLine("radio: fetching station list...", C_CYAN);
    String error;
    if (!radioFetchDirectory(error)) {
        outLine("radio: " + error, C_RED);
        return;
    }

    radioPrintDirectory();

    int index = -1;
    if (partCount > 2) {
        if (!radioParseDirectoryChoice(parts[2], index) || index < 0) {
            outLine("radio: choose 1-" + String(radioDirectoryCount) + " or a station name", C_RED);
            return;
        }
    } else if (!radioPromptDirectoryChoice(index)) {
        outLine("radio: selection cancelled", C_YELLOW);
        return;
    }

    radioPlayUrl(radioDirectoryUrls[index]);
}

//Hand the audio hardware to something else -- currently only the Game Boy emulator
//(Gameboy.ino -> src/AudioOut.cpp), which needs the active I2S TX controller and
//can't have it while ESP32-audioI2S owns it. Stops any stream, deletes the engine,
//and frees the controller.
//Nothing is auto-restored: the next "radio play" walks radioEnsureCodec again and
//rebuilds what it needs, which by then is free because the game called AudioOut::end().
//
//Blocks until the task confirms (it polls the mailbox every tick, so this is
//milliseconds); the timeout is only so a wedged radio task can't hang the shell.
//Returns true if the hardware is actually free.
bool radioReleaseAudio() {
    if (radioTaskHandle == NULL) {
        return true;   //task never started -- nothing was ever claimed
    }
    radioReleased = false;
    radioPostCommand(RADIO_CMD_RELEASE, NULL, 0);
    for (int waited = 0; waited < 2000 && !radioReleased; waited += 10) {
        delay(10);
    }
    return radioReleased;
}

//current volume, snapshotted under the mux -- the status bar (Display.ino's
//drawDisplayStatusBar) reads this each refresh instead of us printing a line on change.
int radioGetVolume() {
    int volume;
    portENTER_CRITICAL(&radioMux);
    volume = radioVolume;
    portEXIT_CRITICAL(&radioMux);
    return volume;
}

//relative volume nudge for the Ctrl+Up/Down chords (TelnetServer.ino's handleCsiSequence,
//RemoteSession.ino's raw-session escape handling) -- clamps instead of validating a typed
//number, otherwise identical to the "radio vol <n>" branch below. The new level shows in
//the status bar (VOL:xx), so there's no line printed here.
void radioAdjustVolume(int delta) {
    int volume;
    portENTER_CRITICAL(&radioMux);
    volume = radioVolume + delta;
    if (volume < 0) {
        volume = 0;
    } else if (volume > RADIO_VOLUME_MAX) {
        volume = RADIO_VOLUME_MAX;
    }
    radioVolume = volume;
    portEXIT_CRITICAL(&radioMux);
    if (radioTaskHandle != NULL) {
        radioPostCommand(RADIO_CMD_VOLUME, NULL, volume);
    }
}

//Expected forms: radio | radio status | radio list [choice] | radio play [url] | radio pause | radio stop | radio vol <0-21>
void handleRadioCommand(const String parts[], int partCount) {
    radioEnsureDefaults();
    String sub = (partCount > 1) ? parts[1] : "status";

    if (sub == "status") {
        radioPrintStatus();
        return;
    }

    if (sub == "list") {
        radioHandleListCommand(parts, partCount);
        return;
    }

    if (sub == "play") {
        if (WiFi.status() != WL_CONNECTED) {
            outLine("radio: WiFi not connected. Run 'wifi connect' first.", C_RED);
            return;
        }
        String url;
        if (partCount > 2) {
            url = parts[2];
        } else {
            portENTER_CRITICAL(&radioMux);
            url = String(radioUrl);
            portEXIT_CRITICAL(&radioMux);
            if (url.length() == 0) {
                url = settingsGet("radio.url", RADIO_DEFAULT_URL);
            }
        }
        radioPlayUrl(url);
        return;
    }

    if (sub == "pause") {
        if (radioTaskHandle == NULL) {
            outLine("radio: not playing");
            return;
        }
        radioPostCommand(RADIO_CMD_PAUSE, NULL, 0);
        return;
    }

    if (sub == "stop") {
        if (radioTaskHandle == NULL) {
            outLine("radio: not playing");
            return;
        }
        radioPostCommand(RADIO_CMD_STOP, NULL, 0);
        return;
    }

    if (sub == "vol") {
        if (partCount < 3) {
            outLine("Usage: radio vol <0-" + String(RADIO_VOLUME_MAX) + ">");
            return;
        }
        int volume = parts[2].toInt();
        if (volume < 0 || volume > RADIO_VOLUME_MAX
            || (volume == 0 && parts[2] != "0")) {
            outLine("Usage: radio vol <0-" + String(RADIO_VOLUME_MAX) + ">");
            return;
        }
        portENTER_CRITICAL(&radioMux);
        radioVolume = volume;
        portEXIT_CRITICAL(&radioMux);
        if (radioTaskHandle != NULL) {
            radioPostCommand(RADIO_CMD_VOLUME, NULL, volume);
        }
        return;
    }

    outLine("Usage: radio [status|list [choice]|play [url]|pause|stop|vol <0-" + String(RADIO_VOLUME_MAX) + ">]");
}
