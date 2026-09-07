//   global.h
//   shared state used across DOLL-OS -- the telnet-interface port of DOLL-OS.
//
//   Telnet is the sole *input* path (this is what the user asked to replace
//   DOLL-OS's physical keyboard with), but this board's TFT panel is driven as a
//   mirror of the telnet session -- "a second screen for the cardputer" -- so it
//   still needs DOLL-OS's pixel-wrapped terminal history/ANSI-filter machinery.
//   That machinery lives here for the same reason DOLL-OS kept it in global.h:
//   it's used by hoisted function prototypes and by subclasses declared in files
//   further down the concatenated sketch.
#pragma once

#include <WiFi.h>
#ifdef FNK0104N_3P5_320x480_ST77922
    #include "DollST77922.h"
#endif
#include <TFT_eSPI.h>
#include <ArduinoJson.h>
//   Pulled in here (not just in Radio.ino) so the ESP32-audioI2S `Audio` class is
//   declared before the Arduino sketch builder's auto-generated function
//   prototypes. radioAudioInfo(Audio::msg_t) (Radio.ino) gets a synthetic
//   prototype hoisted to the top of the concatenated sketch, above Radio.ino's
//   own `#include "Audio.h"`; without Audio visible that early the prototype
//   fails to parse ("'Audio' has not been declared"). global.h is included first
//   from DS.ino, so declaring it here fixes the ordering -- the same reason every
//   other cross-file type lives in this file.
#include "Audio.h"

//   Rear WS2812 RGB LED for app/runtime effects. Pin defaults match Freenove's
//   FNK0104 RGB examples: GPIO42 on the AB/S variants, GPIO40 on the N variant.
//   Set REAR_RGB_LED_PIN in config.h to override, or -1 to disable LED control.
#ifndef REAR_RGB_LED_PIN
    #define REAR_RGB_LED_PIN DOLL_REAR_RGB_LED_PIN
#endif
#ifndef REAR_RGB_LED_BRIGHTNESS
    #define REAR_RGB_LED_BRIGHTNESS 255
#endif
#ifndef DOLL_DISPLAY_UPSIDE_DOWN
    #define DOLL_DISPLAY_UPSIDE_DOWN 0
#endif

//   Shared rear-LED API for native modules (.ino/.cpp) and AppRunner opcodes.
//   These are safe to call on builds without LED support: availability is queryable,
//   and setters become no-ops when unavailable.
struct LedRgb {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

bool rearLedAvailable();
void rearLedSetRgb(uint8_t red, uint8_t green, uint8_t blue);
void rearLedSetRgbLong(long red, long green, long blue);
void rearLedOff();

//   OS-level indicator surface layered over the same rear RGB LED. Transient pulses
//   show activity (storage, network, input) while persistent states show what the
//   board is doing when idle. App LED effects get an override so the .dapp LED opcode
//   still behaves like a direct light command while a script is running.
void ledBegin();
void ledService();
void ledPulseStorageRead(bool isSd);
void ledPulseStorageWrite(bool isSd);
void ledPulseNetwork();
void ledPulseInput();
void ledPulseError();
void ledSetSdMounted(bool mounted);
void ledSetWifiConnected(bool connected);
void ledSetFtpActive(bool active);
void ledSetTelnetConnected(bool connected);
void ledSetKeyboardActive(bool active);
void ledSetAppOverrideRgb(uint8_t red, uint8_t green, uint8_t blue);
void ledSetAppOverrideRgbLong(long red, long green, long blue);
void ledClearAppOverride();
void ledPrepareForSleep();

//   Manual paired-board sleep path. DS-Slave sends the request over its private
//   UART controls; Power.ino owns light sleep while Display.ino owns panel power.
void enterSystemLightSleep();
void displaySetSleeping(bool sleeping);

//   Display panel geometry, keyed off the same FNK0104* board-variant macro
//   config.h already defines for SD_MMC/battery pins. Native panel resolution is
//   portrait; the display runs rotated to landscape (see Display.ino), hence
//   width/height are swapped from the driver's native W x H here.
#ifdef FNK0104N_3P5_320x480_ST77922
    const int DISPLAY_WIDTH = 480;
    const int DISPLAY_HEIGHT = 320;
#elif defined(FNK0104S_4P0_320x480_ST7796)
    const int DISPLAY_WIDTH = 480;
    const int DISPLAY_HEIGHT = 320;
#else
    const int DISPLAY_WIDTH = 320;
    const int DISPLAY_HEIGHT = 240;
#endif

//   Telnet server + the one connected client. DOLL-OS permits a single interactive
//   session at a time, same as the original scaffold -- DOLL-OS's whole keyboard/
//   command model assumes one local user too, so this isn't a new constraint.
WiFiServer telnetServer(TELNET_PORT);
WiFiClient telnetClient;

//command logic
String currentCommand = "";      //shell's live input buffer, filled by the line editor in TelnetServer.ino
int commandCursorPos = 0;        //index within the buffer currently being edited (shared -- only one buffer is ever active at a time)
const int COMMAND_MAX_LEN = 256; //cap on any single line-edited buffer (shell command, motoko message, ssh password, ...)
String activeInputPrompt = "> "; //label for whichever buffer readLineEditedInput() is currently editing -- set by
                                  //each call site right after calling it (TelnetServer.ino/Motoko.ino/Ssh.ino) so
                                  //Display.ino's mirrored command bar shows the right prompt/content/masking
bool activeInputMasked = false;  //true while the active buffer is a password prompt (ssh) -- display shows '*' too
String activeInputText = "";     //current content of whichever buffer is active, mirrored for the display

//result of one readLineEditedInput() call (TelnetServer.ino) -- declared here rather than
//there because it's a custom type used as a hoisted function's return type, and callers in
//other files (Motoko.ino, Ssh.ino) need it visible regardless of .ino concatenation order
enum LineInputResult { LINE_NO_INPUT, LINE_EDITING, LINE_SUBMITTED };

//per-input-source line-edit state (escape/CSI parsing + CRLF pairing). DOLL-OS had a
//single physical keyboard, so this was implicit module state in one place. DOLL-OS now has
//two sources feeding the same line editor -- the telnet client (TelnetServer.ino) and
//the BLE-keyboard UART bridge (KeyboardSerial.ino) -- so each keeps its own copy: a
//half-finished escape sequence arriving on one source can't corrupt the other's parse.
enum UserEscState { UESC_NONE, UESC_GOT_ESC, UESC_GOT_CSI };
struct LineEditState {
    UserEscState escState = UESC_NONE;
    String escParams = "";
    bool lastByteWasCR = false;
};

//Alias.ino: file-backed shell aliases. The struct lives here because Alias.ino has
//static helpers with AliasEntry parameters, and Arduino hoists their prototypes above
//the tab's own declarations.
struct AliasEntry {
    String name;
    String expansion;
};

//Settings.ino: file-backed runtime settings overriding config.h defaults. The
//struct lives here for the same reason as AliasEntry above -- Settings.ino's
//static helpers take SettingsEntry parameters, and Arduino hoists prototypes
//for every file above the tab that declares the type.
struct SettingsEntry {
    String key;
    String value;
};

//WiFiManager.ino: ordered, file-backed station credentials. This type lives here
//because Arduino hoists prototypes for helpers that accept WifiCredential arrays
//above the tab-local implementation.
static const int WIFI_MAX_SAVED_NETWORKS = 16;
struct WifiCredential {
    String ssid;
    String password;
};

//explicit prototype for readLineEditedInput (TelnetServer.ino), needed because
//Motoko.ino and Ssh.ino call it but sort alphabetically before TelnetServer.ino in
//the concatenated build
LineInputResult readLineEditedInput(String& text);
int telnetReadFilteredByte();

//keyboard-bridge counterparts of the two telnet input readers, needed by callers that sort
//before KeyboardSerial.ino in the concatenated build (RemoteSession.ino, Ssh.ino). See
//KeyboardSerial.ino: readKeyboardLineEditedInput() mirrors readLineEditedInput() for the
//line-edited prompts; keyboardReadRawByte() feeds the RemoteSession raw passthrough.
LineInputResult readKeyboardLineEditedInput(String& text);
int keyboardReadRawByte();
int keyboardPeekRawByte();

//shared per-byte line editor both input sources push into (TelnetServer.ino). Applies
//one input byte to `text`, using `st` for escape/CR context; echoCrlfToTelnet controls
//whether an accepted Enter echoes CRLF to the telnet client (telnet: yes so the remote
//terminal advances a line; keyboard bridge: no, there's nothing to echo to).
LineInputResult processLineEditByte(String& text, uint8_t ch, LineEditState& st, bool echoCrlfToTelnet);

//   Editor (Edit.ino) -- the "edit" app's logical key vocabulary and its per-input-source
//   escape/CSI parse state. Both are used only inside Edit.ino, but they still have to
//   live here: the Arduino builder generates hoisted prototypes for that file's `static`
//   functions too, and a prototype mentioning EditKey/EditKeyState lands above Edit.ino's
//   own definitions. Same trap, and the same fix, as LineInputResult and RadioState above.
//
//   Two EditKeyState instances exist (telnet, keyboard bridge) for the same reason
//   LineEditState is per-source: a half-finished escape sequence arriving on one input
//   source must not corrupt the other's parse.
enum EditKey {
    EK_NONE, EK_CHAR, EK_ENTER, EK_TAB, EK_BACKSPACE, EK_DELETE,
    EK_LEFT, EK_RIGHT, EK_UP, EK_DOWN,
    EK_HOME, EK_END, EK_PGUP, EK_PGDN,
    EK_SAVE, EK_EXIT, EK_CANCEL,
    EK_CUT, EK_UNCUT, EK_SEARCH, EK_GOTO, EK_UNDO, EK_HELP
};
struct EditKeyState {
    UserEscState esc = UESC_NONE;
    String params = "";
    bool lastByteWasCR = false;
};

//command history (sent commands, recalled with the Up/Down arrow keys like a real shell)
const int COMMAND_HISTORY_MAX = 30;
String commandHistory[COMMAND_HISTORY_MAX];
int commandHistoryCount = 0;
int commandHistoryHead = 0;
int commandHistoryIndex = -1;
String commandHistoryDraft = "";

//storage
bool sdCardMounted = false;
String cwd = "/";
const String SD_MOUNT = "/sd";

//Output.ino: the path-aware shell prompt built from cwd ("/sd/roms > ") and the echo of a
//submitted command. Declared here because nearly every file that prints a prompt or runs a
//command sorts before Output.ino in the concatenated build.
String shellPrompt();
void echoCommandLine(const String& entered);

//result of routing an absolute unified-namespace path onto the physical filesystem that owns it
struct RoutedPath {
    fs::FS* fs;
    String realPath;
    bool isSd;
};

//Dapper.ino: package/catalog types used by auto-generated prototypes. These must
//be visible here for the same Arduino prototype-hoisting reason as DappProgram.
struct DapperRecord {
    int packageFormat = 0;
    String id;
    String name;
    String summary;
    String version;
    String runtimeMin;
    String runtimeMaxExclusive;
    String sha256;
    String url;
    size_t size = 0;
    bool compatible = false;
    String incompatibility;
};

struct DapperInstalled {
    DapperRecord record;
    String repository;
    String installedPath;
};

//AppRunner.ino: these live here instead of inside AppRunner.ino because the Arduino
//builder hoists prototypes for static functions above the tab's own type definitions.
//   One script line, decoded once at load. It used to hold nothing but the raw source text,
//   which appExecute then re-trimmed, re-split and re-matched against a chain of ~70 String
//   comparisons every single time the line ran. See the opcode table in AppRunner.ino.
struct DappLine {
    String arg;          //everything after the opcode, trimmed; empty when there is none
    String opText;       //source spelling, kept only for DAPP_OP_UNKNOWN's error message
    uint8_t opcode;      //DAPP_OP_* -- DAPP_OP_LABEL covers blanks, comments and ':' labels
    int16_t jumpTarget;  //pre-resolved destination line for GOTO/GOSUB/IF/IFEQ/IFNE, else -1
};

struct DappLabel {
    String name;
    int lineIndex;
};

struct DappVar {
    String name;
    long value;
    bool used;
};

struct DappStringVar {
    String name;
    String value;
    bool used;
};

//a DIM'd numeric array. `values` doesn't own its memory -- every array is carved out of
//DappProgram::arrayPool by a bump pointer, so there is one PSRAM block for all of them
//and nothing to free per-array. See appDimArray in AppRunner.ino.
struct DappArray {
    String name;
    long* values;
    int size;
    bool used;
};

//a loaded .dapp program plus its interpreter state. Every array here is a PSRAM
//allocation rather than a stack array -- see the storage comment in AppRunner.ino for
//why. The elements hold Strings, so alloc() placement-news them and the destructor
//unwinds them; that destructor is what makes the early-return paths in handleRunCommand
//leak-free, so keep this stack-scoped and never copy it.
struct DappProgram {
    DappLine* lines = nullptr;
    DappLabel* labels = nullptr;
    DappVar* vars = nullptr;
    DappStringVar* stringVars = nullptr;
    DappArray* arrays = nullptr;
    long* arrayPool = nullptr;      //backing cells every DIM'd array carves from
    int* callStack = nullptr;       //GOSUB return addresses (line indices)
    int lineCount = 0;
    int labelCount = 0;
    int arrayPoolUsed = 0;
    int callDepth = 0;
    bool echoInput = true;

    //out-of-band error channel. Value lookup happens several frames deep inside text
    //expansion (appExpandText -> appStringValueOf -> appArrayCell), where there is no way
    //to return a failure -- so a bad array index records itself here and appExecute checks
    //it after every instruction. Without this, an out-of-range read would quietly evaluate
    //to 0, which is the one bug a script driving a few hundred array cells cannot find.
    String fault = "";

    bool alloc();
    ~DappProgram();

    DappProgram() {}
    DappProgram(const DappProgram&) = delete;
    DappProgram& operator=(const DappProgram&) = delete;
};

//per-input-source escape state for the .dapp runtime's KEY opcode (AppRunner.ino). Here
//rather than there for the same hoisted-prototype reason as EditKeyState above: the
//builder lifts a prototype mentioning DappKeyState to the top of the sketch. Two
//instances exist (telnet, keyboard bridge) so a half-arrived arrow key on one source
//cannot corrupt the other's parse -- the rule LineEditState follows too.
enum DappKeyPhase { DKEY_NORMAL, DKEY_ESC, DKEY_CSI };
struct DappKeyState {
    DappKeyPhase phase = DKEY_NORMAL;
    String params = "";
    unsigned long escAtMs = 0;
};

//shared helpers used across app/file command tabs
#define DOLL_BOARD_ID "fnk0104"
#define DAPP_RUNTIME_VERSION "1.9.0"

//Runtime-owned PCM synth used by the .dapp WAVE/WAVESTOP opcodes. It borrows
//the same ES8311/I2S output surface as Game Boy and releases it on app exit.
enum DappWaveType : uint8_t {
  DAPP_WAVE_OFF = 0,
  DAPP_WAVE_SINE,
  DAPP_WAVE_TRIANGLE,
  DAPP_WAVE_SQUARE,
  DAPP_WAVE_SAWTOOTH,
  DAPP_WAVE_NOISE
};
bool dappSynthSetChannel(int channelNumber, String waveform, long frequency, long level);
void dappSynthEnd();
bool dappSynthLastOk();
#define DAPP_PACKAGE_FORMAT 1

int splitCommand(const String& input, String parts[], int maxParts);
bool dappStorageMkdir(const String& resolvedPath);
bool dappStorageCopy(const String& sourceResolved, const String& destResolved);
bool dappStorageMove(const String& sourceResolved, const String& destResolved);
void ensureDefaultAliases();
bool expandCommandAlias(String& command, String& aliasName, String& aliasExpansion);
String resolvePath(const String& cwd, const String& inputPath);
RoutedPath routePath(const String& resolvedPath);
bool ensureSystemConfDirectory();
String settingsGet(const String& key, const String& fallback);
bool settingsSet(const String& key, const String& value);
bool settingsUnset(const String& key);
void handleSettingsCommand(const String parts[], int partCount);
void handleAliasCommand(const String parts[], int partCount);
void handleAppsCommand(const String parts[], int partCount);
void handleDapperCommand(const String parts[], int partCount);
bool dapperFetchPackageForEdit(const String& request, const String& targetPreference,
                               String& sourcePath, String& suggestedSavePath,
                               String& loadedLabel, String& error);
void handleRunCommand(const String parts[], int partCount);
void handleAsukaCommand(const String parts[], int partCount);
void handleUnaliasCommand(const String parts[], int partCount);
void ftpService();
void radioService();
void maintainInternetConnection();
void drawDisplayFrame();
int readBatteryPercent();
int wifiIsConnected();
void runWifiManagerApp();
void initInternalStorage();

//heap instrumentation (see SysInfo.ino)
const int HEAP_CHECKPOINT_MAX = 16;
struct HeapCheckpoint {
    const char* tag;
    uint32_t freeHeap;
    uint32_t largestBlock;
    uint32_t minFreeHeap;
};
HeapCheckpoint heapCheckpoints[HEAP_CHECKPOINT_MAX];
int heapCheckpointCount = 0;
int heapCheckpointHead = 0;

//   ANSI SGR foreground color codes -- DOLL-OS mapped these onto 16-bit sprite
//   colors per history row; here they're the real escape codes a telnet client's
//   own terminal emulator renders directly. See Output.ino for outLine().
const int C_RESET   = 0;
const int C_WHITE   = 37;
const int C_BLACK   = 30;
const int C_RED     = 31;
const int C_GREEN   = 32;
const int C_YELLOW  = 33;
const int C_BLUE    = 34;
const int C_MAGENTA = 35;
const int C_CYAN    = 36;
const int C_PINK    = 95;   //bright magenta stands in for DOLL-OS's PINK accent color

//   Radio (Radio.ino) -- background ICY/MP3 stream player on the board's onboard
//   ES8311 codec, ported from the standalone sgcrelay firmware. Runs in its own
//   FreeRTOS task so playback survives modal sessions (ssh, outbound telnet) and
//   display pushes. The enum lives here rather than Radio.ino for the same
//   hoisted-prototype reason as LineInputResult above.
enum RadioState { RADIO_OFF, RADIO_CONNECTING, RADIO_PLAYING, RADIO_PAUSED, RADIO_STOPPED, RADIO_ERROR };
void ledSetRadioState(RadioState state);

//ESP32-audioI2S's software volume scale, and the level the Game Boy emulator mixes
//its APU output at (src/AudioOut.cpp) -- one notion of loudness for the whole board.
//Here rather than in Radio.ino for the same hoisting reason as the enums: Gameboy.ino
//sorts above Radio.ino in the concatenated sketch and its settings menu displays this.
const int RADIO_VOLUME_MAX = 21;

//one-slot command mailbox kinds, shell -> radio task (also here for hoisting: the
//poster/consumer function signatures use it)
//RADIO_CMD_RELEASE is the Game Boy emulator's: it stops the stream and tears the
//radio's I2S controllers back down so src/AudioOut.cpp can claim one (the S3 has
//exactly two, and a playing radio holds both). See radioReleaseAudio().
enum RadioCommandKind {
    RADIO_CMD_NONE,
    RADIO_CMD_PLAY,
    RADIO_CMD_PLAY_FILE,
    RADIO_CMD_PAUSE,
    RADIO_CMD_STOP,
    RADIO_CMD_VOLUME,
    RADIO_CMD_SEEK,
    RADIO_CMD_RELEASE
};

//   Music library/player (Music.ino). The whole catalog is one PSRAM allocation:
//   fixed fields avoid hundreds of small String allocations leaking back into the
//   scarce internal heap while still leaving generous room for paths and ID3 text.
//   Fixed-width records mean the count has to be bounded up front, and the bound is
//   what the slab costs: 552 bytes a track, so 1024 is ~552KB of PSRAM claimed on the
//   first "music" and held until reboot. Raising it further is linear and safe up to
//   65535, where the uint16_t track indexes below run out.
const int MUSIC_LIBRARY_MAX_TRACKS = 1024;
const int MUSIC_PATH_MAX = 256;
const int MUSIC_METADATA_MAX = 96;
struct MusicTrack {
    char path[MUSIC_PATH_MAX];       //DOLL-OS logical path, always /sd/music/...
    char title[MUSIC_METADATA_MAX];
    char artist[MUSIC_METADATA_MAX];
    char album[MUSIC_METADATA_MAX];
    uint32_t fileSize;
    uint16_t trackNumber;
};

//Runs of tracks in the sorted catalog. musicCompareTracks orders by artist then album,
//so every album -- and every artist's block of albums -- is already contiguous. Both
//indexes are therefore just (first, count) spans into the arrays below them, with no
//second copy of the metadata and nothing to keep in sync beyond a rescan.
struct MusicAlbum {
    uint16_t firstTrack;
    uint16_t trackCount;
};

struct MusicArtist {
    uint16_t firstAlbum;
    uint16_t albumCount;
    uint16_t firstTrack;
    uint16_t trackCount;
};

//The player is a drill-down browser rooted at a three-entry menu. Artists descends
//Artists -> that artist's Albums -> that album's Tracks; Albums enters the album list
//unscoped; Tracks jumps straight to every track. MUSIC_VIEW_TRACKS also renders search
//results, in which case no album is open and Escape returns to the root.
enum MusicView { MUSIC_VIEW_ROOT, MUSIC_VIEW_ARTISTS, MUSIC_VIEW_ALBUMS, MUSIC_VIEW_TRACKS };
const int MUSIC_VIEW_COUNT = 4;
const int MUSIC_ROOT_ROWS = 3;

//MK_ABORT is the terminate chord (Ctrl+T / ^X, which is what DS-Slave's rotary
//Settings > Terminate App sends). Kept distinct from MK_ESCAPE because the two mean
//different things here: Escape and q close the screen and deliberately leave the
//track playing, while terminate is asked for when someone wants the thing to stop.
enum MusicKey {
    MK_NONE, MK_CHAR, MK_ENTER, MK_ESCAPE, MK_ABORT, MK_BACKSPACE,
    MK_UP, MK_DOWN, MK_LEFT, MK_RIGHT, MK_PAGE_UP, MK_PAGE_DOWN
};
struct MusicKeyState {
    UserEscState esc = UESC_NONE;
    String params = "";
    unsigned long escAtMs = 0;
    bool lastByteWasCR = false;
};

//   Button bar (PadButtons.ino) -- the DS-Slave button bar's face buttons as the OS
//   sees them, after KeyboardSerial.ino has decoded the private link bytes the slave
//   sends for them while game mode is off. The enum lives here rather than in
//   PadButtons.ino for the usual hoisted-prototype reason: KeyboardSerial.ino and
//   Music.ino both take one as a parameter and sort above PadButtons.ino in the
//   concatenated sketch.
enum PadButton { PAD_BTN_NONE, PAD_BTN_START, PAD_BTN_SELECT, PAD_BTN_B, PAD_BTN_A };

extern String sshInputBuffer;
extern String motokoChannel;
extern String motokoInputBuffer;
extern WiFiClient remoteTelnetClient;   //outbound socket for the "telnet" client command (TelnetClient.ino) --
                                         //named distinctly from telnetClient (our server's connected user) above

//   Display mirror (Display.ino). The TFT panel isn't a second *input* device --
//   telnet remains the only way to control DOLL-OS -- it just shows the same session a
//   connected telnet client sees, the way the user asked: "as if it was a second
//   screen for the cardputer." That means reviving DOLL-OS's pixel-wrapped
//   history/ANSI-filter machinery, just retargeted from an M5GFX sprite to a
//   TFT_eSPI one.
//
//   The N-variant panel (ST77922, QSPI) pushes its whole frame in one call through
//   a different object than the other two panels' plain TFT_eSPI, so tft/tft_qspi/
//   tft_st77922 are all declared conditionally -- but every panel draws onto the
//   same single full-screen frameSprite, so Display.ino's drawing code itself
//   doesn't need to branch per panel, only the final "push this frame" step does.
#ifdef FNK0104N_3P5_320x480_ST77922
    TFT_eSPI tft_qspi = TFT_eSPI();
    TFT_eSprite frameSprite = TFT_eSprite(&tft_qspi);
    DollST77922 tft_st77922 = DollST77922();
#else
    TFT_eSPI tft = TFT_eSPI();
    TFT_eSprite frameSprite = TFT_eSprite(&tft);
#endif

const int DISPLAY_STATUS_BAR_HEIGHT = 16;
const int DISPLAY_COMMAND_BAR_HEIGHT = 20;
const int DISPLAY_PADDING = 4;
//   .dapp canvas (AppRunner.ino) -- a fixed character grid a script can address by cell
//   instead of appending scrolling lines, which is what a game needs. While
//   dappCanvasActive is set, drawDisplayFrame() paints this grid over the terminal
//   area instead of the history.
//
//   These live here (rather than as AppRunner.ino file statics) for the same reason
//   displayHistoryRows does: Display.ino has to read the buffer directly to render it,
//   and .ino concatenation order makes a file-static invisible to it. The lifetime rule
//   that keeps that safe is one-way: appCanvasEnd() clears dappCanvasActive *before*
//   freeing the cells, so the renderer can never see a live flag with a dead pointer.
struct DappCanvasCell {
    char ch;
    uint8_t color;   //ANSI SGR code (C_* in this file), converted at draw time
};
DappCanvasCell* dappCanvasCells = nullptr;
int dappCanvasCols = 0;
int dappCanvasRows = 0;
bool dappCanvasActive = false;

//drawDisplayFrame() (Display.ino) skips its redraw + SPI push entirely unless this
//is set -- every history/command-bar mutation marks it via markDisplayDirty() so a
//full-frame push only happens when something actually changed, instead of on every
//loop() tick regardless of activity
bool displayDirty = true;   //starts true so the first frame after boot always draws
const unsigned long DISPLAY_STATUS_REFRESH_MS = 1000;   //separate timer so the MEM/BAT
                                                          //readout in the status bar still
                                                          //ticks over while otherwise idle
unsigned long displayLastStatusRefresh = 0;

//command-bar caret blink. Like DISPLAY_STATUS_REFRESH_MS above, this is a second reason
//drawDisplayFrame() may redraw an otherwise-clean frame: each time the blink phase flips
//the frame is pushed again so the '|' caret in the mirrored command bar visibly blinks.
const unsigned long DISPLAY_CURSOR_BLINK_MS = 500;   //half-period: on 500ms, off 500ms
bool displayLastCursorPhase = false;                 //phase drawn last frame, to detect a flip

const int DISPLAY_HISTORY_MAX_LINES = 200;
const int DISPLAY_HISTORY_ROW_MAX_CHARS = 128;
struct DisplayHistoryRow {
    char text[DISPLAY_HISTORY_ROW_MAX_CHARS];
    uint16_t color = TFT_WHITE;
};
//allocated at boot (initDisplay, via psramOrInternalCalloc) rather than as a static
//array so it lands in PSRAM when available -- ~26KB (200 rows x 130B), the largest
//movable app buffer, kept out of internal SRAM where WiFi/TLS need the room
DisplayHistoryRow* displayHistoryRows = nullptr;
int displayHistoryCount = 0;
int displayHistoryHead = 0;

//how many lines back from the live tail drawDisplayHistory() (Display.ino) is currently
//showing -- 0 means pinned to the newest line (the panel's original behavior). Nudged by
//Shift+Up/Down at the shell prompt (TelnetServer.ino's handleCsiSequence) and during raw
//ssh/telnet sessions (RemoteSession.ino), via displayScrollBy().
int displayScrollOffset = 0;

//ANSI/UTF-8 filtering for raw remote byte streams mirrored onto the display (ssh
//shell, outbound telnet client) -- telnet itself gets true unfiltered passthrough
//(see Ssh.ino/TelnetClient.ino), this is display-only, same reasoning DOLL-OS's
//ansi.ino documents: the panel can't render real ANSI, so SGR color is
//interpreted into a per-row pixel color and everything else is dropped.
enum AnsiParseState { ANSI_TEXT, ANSI_ESC, ANSI_CSI, ANSI_OSC, ANSI_OSC_ESC };

//which part of the current line a CSI K erase asks to clear. The parameter matters because
//a remote line editor redrawing its input line picks whichever spelling it likes -- "\rESC[K"
//(home, then clear forward) and "ESC[2K\r" (clear the whole row, then home) are both common,
//and treating the second as the first erased nothing at all.
enum DisplayEraseKind { DISPLAY_ERASE_NONE, DISPLAY_ERASE_TO_END, DISPLAY_ERASE_TO_START, DISPLAY_ERASE_ALL };

struct AnsiFilterState {
    AnsiParseState state = ANSI_TEXT;
    String csiParams = "";
    int utf8Remaining = 0;
};

//per-stream state for the display's incremental line-building API (Display.ino's
//displayStreamPutChar/Newline/...) -- one instance per independent byte stream so
//interleaved streams (ssh stdout vs stderr) don't corrupt each other's in-progress row
struct DisplayStreamState {
    String pendingRow = "";
    size_t cursorCol = 0;
    //how many earlier panel rows the current logical line has wrapped onto (0 = this is the
    //line's first/only panel row). Lets displayStreamBackspace merge back up through a
    //panel-side wrap instead of stalling at column 0 while the remote keeps deleting. Reset
    //whenever a new logical line starts (newline/reset/another stream taking the open row).
    int wrapDepth = 0;
    //set the instant a width-wrap happens; consumed when the next panel row is actually
    //materialized, so that row is counted as a wrap-continuation rather than a fresh line
    //(the wrap and the first char of the new row are two separate displayStreamPutChar calls).
    bool wrapPending = false;
    //a CR arrived on a line that had wrapped, and it isn't yet known whether it meant "redraw
    //this line" (collapse the wrap) or was just the CR half of a CRLF (leave it alone). Held
    //until the next write/erase/newline says which -- see displayStreamCarriageReturn.
    bool crPending = false;
};

extern AnsiFilterState sshStdoutAnsi;
extern AnsiFilterState sshStderrAnsi;
extern AnsiFilterState remoteTelnetAnsi;
extern DisplayStreamState sshStdoutDisplayStream;
extern DisplayStreamState sshStderrDisplayStream;
extern DisplayStreamState remoteTelnetDisplayStream;

//which DisplayStreamState currently "owns" the last row in displayHistoryRows --
//nullptr = no stream owns an open row right now
DisplayStreamState* displayOpenRowOwner = nullptr;

void displayStreamReset(DisplayStreamState& st);
void displayStreamNewline(DisplayStreamState& st);
void displayStreamPutChar(DisplayStreamState& st, char ch, uint16_t color);
void displayStreamCarriageReturn(DisplayStreamState& st);

//   Shared modal loop for character-oriented remote sessions (ssh shell, outbound
//   telnet client). Port of DOLL-OS's RemoteSession: same shape, but both ends of
//   "local" are now telnetClient (the connected user) instead of a keyboard + sprite
//   -- pumpIncoming() writes remote bytes straight to telnetClient (a real terminal
//   renders ANSI/color natively, so unlike DOLL-OS this needs no escape-sequence
//   reinterpretation), and the local-input side reads raw bytes back off telnetClient
//   via readRawUserBytes() (TelnetServer.ino) instead of a keyboard poll.
//
//   Declared here rather than alongside its subclasses for the same reason as in
//   DOLL-OS: the Arduino IDE hoists every .ino's function prototypes above all
//   #includes, so a base class used by a subclass further down the sketch must
//   already be visible.
class RemoteSession {
public:
    virtual ~RemoteSession() {}

    //runs until the remote closes, the user's own telnet client disconnects, or the
    //user sends the local escape chord (Ctrl+T -- see readRawUserBytes in TelnetServer.ino)
    void run();

protected:
    virtual void pumpIncoming() = 0;                   //drain the remote transport, write bytes straight to telnetClient
    virtual bool isClosed() = 0;                        //has the remote end gone away
    virtual void sendBytes(const String& bytes) = 0;    //forward raw bytes from the user to the remote
    virtual void onClosed() {}                           //called once, the first time isClosed() is observed true

    //byte(s) sent to the remote when the user's client sends backspace/delete (0x08 or 0x7F).
    //Differs by transport -- a real pty (ssh) wants DEL (0x7F); classic telnet/BBS line
    //editors want ASCII backspace (0x08). Override per subclass.
    virtual String backspaceBytes() { return "\x7f"; }

private:
    //Ctrl+K handler (RemoteSession.ino) -- runs one shell command via commandProcessor()
    //without ending the session. Not virtual: identical for every subclass, unlike
    //pumpIncoming/isClosed/sendBytes which are transport-specific.
    void runInlineCommandPrompt();
};
