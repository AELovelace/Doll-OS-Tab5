// "gba" app -- the ESP32-P4 gpSP core hosted by Doll-OS.
//
// Bare `gba` opens a recursive /sd/gba picker. While a game is running,
// Escape (or the touch MENU button) opens display, volume, save/load-state,
// resume, and quit controls. Display scaling is integer-only so pixels remain
// crisp and the expensive DSI work can be skipped independently of emulation.

#include "src/GameBoyAdvanceHost.h"
#include "src/AudioOut.h"
#include "src/emulator/gpsp/doll_gba_bridge.h"

#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_memory_utils.h"
#include "esp_system.h"
#include "esp32-hal-cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <stddef.h>
#include <cstring>
#include <new>

static GameBoyAdvanceHost gbaHost;
static constexpr int GBA_W = GameBoyAdvanceHost::kWidth;
static constexpr int GBA_H = GameBoyAdvanceHost::kHeight;
static constexpr int GBA_MAX_FRAME_SKIP = 5;
static constexpr uint32_t GBA_PANEL_INTERVAL_US = 66000;
static constexpr int GBA_ROM_MENU_MAX = 128;
static constexpr size_t GBA_BOOT_PATH_MAX = 384;
static constexpr uint32_t GBA_BOOT_MAGIC = 0x47424144;  // "DABG" tags a Doll-OS GBA boot ticket.
static constexpr uint16_t GBA_BOOT_VERSION = 1;
static const char* GBA_ROM_DIR = "/sd/gba";

enum GbaBootPhase : uint8_t {
    GBA_BOOT_EMPTY = 0,
    GBA_BOOT_PENDING = 1,
    GBA_BOOT_RUNNING = 2,
};

struct GbaBootTicket {
    uint32_t magic;
    uint16_t version;
    uint8_t phase;
    uint8_t scale;
    int8_t frameSkip;
    uint8_t reserved[3];
    char romPath[GBA_BOOT_PATH_MAX];
    uint32_t checksum;
};

// RTC no-init memory survives esp_restart(), unlike the normal heap. A checksum and
// version make random cold-boot contents harmless, while the phase doubles as the
// crash-loop fuse: a second boot that sees RUNNING abandons game mode and starts OS.
RTC_NOINIT_ATTR static GbaBootTicket gbaBootTicket;

static int gbaScale = 3;
static int gbaOutW = GBA_W * 3;
static int gbaOutH = GBA_H * 3;
static int gbaOutX = (DISPLAY_WIDTH - GBA_W * 3) / 2;
static int gbaOutY = (DISPLAY_HEIGHT - GBA_H * 3) / 2;
// Core frame skip and physical panel cadence are independent. Zero renders
// every emulated frame; the DSI panel is still capped near 15 Hz. Positive
// values skip that many emulated render passes before producing a new frame.
static int gbaFrameSkip = 0;
static String gbaRomVfs;
static bool gbaStandaloneMode = false;
static uint32_t gbaLastEmuFps10 = 0;
static uint32_t gbaLastCoreUs = 0;
static uint32_t gbaLastDrawCoreUs = 0;
static uint32_t gbaLastSkipCoreUs = 0;
static uint32_t gbaLastAudioUs = 0;
static uint32_t gbaLastBlitUs = 0;

static uint32_t gbaBootChecksum() {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&gbaBootTicket);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < offsetof(GbaBootTicket, checksum); ++i) {
        hash = (hash ^ bytes[i]) * 16777619u;
    }
    return hash;
}  // Seals every launch field with a compact FNV-1a integrity check.

static void gbaSealBootTicket() {
    gbaBootTicket.checksum = gbaBootChecksum();
}  // Refreshes integrity after changing the ticket's lifecycle phase.

static void gbaClearBootTicket() {
    std::memset(&gbaBootTicket, 0, sizeof(gbaBootTicket));
}  // Prevents a completed or failed game session from relaunching after reboot.

static bool gbaBootTicketValid() {
    return gbaBootTicket.magic == GBA_BOOT_MAGIC &&
           gbaBootTicket.version == GBA_BOOT_VERSION &&
           (gbaBootTicket.phase == GBA_BOOT_PENDING ||
            gbaBootTicket.phase == GBA_BOOT_RUNNING) &&
           gbaBootTicket.scale >= 1 && gbaBootTicket.scale <= 3 &&
           gbaBootTicket.frameSkip >= -1 &&
           gbaBootTicket.frameSkip <= GBA_MAX_FRAME_SKIP &&
           gbaBootTicket.romPath[0] != '\0' &&
           gbaBootTicket.romPath[GBA_BOOT_PATH_MAX - 1] == '\0' &&
           std::strncmp(gbaBootTicket.romPath, "/sdcard/", 8) == 0 &&
           gbaBootTicket.checksum == gbaBootChecksum();
}  // Rejects corrupt, stale, unsupported, and non-SD launch requests before boot.

bool gbaClaimBootMode() {
    if (gbaBootTicket.magic != GBA_BOOT_MAGIC) return false;
    if (!gbaBootTicketValid()) {
        Serial.println("[gba boot] discarded invalid RTC launch ticket");
        gbaClearBootTicket();
        return false;
    }
    if (gbaBootTicket.phase == GBA_BOOT_RUNNING) {
        Serial.println("[gba boot] previous game-mode boot did not exit cleanly; recovering to Doll-OS");
        gbaClearBootTicket();
        return false;
    }
    gbaBootTicket.phase = GBA_BOOT_RUNNING;
    gbaSealBootTicket();
    gbaStandaloneMode = true;
    Serial.printf("[gba boot] claimed %s at %ux, frame skip %d\n",
                  gbaBootTicket.romPath, gbaBootTicket.scale,
                  gbaBootTicket.frameSkip);
    return true;
}  // Consumes PENDING once and arms automatic normal-OS recovery on any reset.

static bool gbaScheduleBoot(const String& romVfs, int scale, int frameSkip) {
    if (!romVfs.startsWith("/sdcard/")) return false;
    if (romVfs.length() >= GBA_BOOT_PATH_MAX) return false;
    std::memset(&gbaBootTicket, 0, sizeof(gbaBootTicket));
    gbaBootTicket.magic = GBA_BOOT_MAGIC;
    gbaBootTicket.version = GBA_BOOT_VERSION;
    gbaBootTicket.phase = GBA_BOOT_PENDING;
    gbaBootTicket.scale = static_cast<uint8_t>(constrain(scale, 1, 3));
    gbaBootTicket.frameSkip = static_cast<int8_t>(
        constrain(frameSkip, -1, GBA_MAX_FRAME_SKIP));
    std::memcpy(gbaBootTicket.romPath, romVfs.c_str(), romVfs.length() + 1);
    gbaSealBootTicket();
    return true;
}  // Writes the ROM and runtime choices into reset-persistent memory.

static void gbaRestartDevice() {
    displayPrepareForRestart();
    Serial.flush();
    delay(50);
    esp_restart();
}  // Restarts only after fencing the independently powered Tab5 DSI panel.

void gbaInitMinimalDisplay() {
    tft.setRotation(TAB5_DISPLAY_ROTATION);
    tft.setTextSize(DISPLAY_TEXT_SIZE);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_PINK, TFT_BLACK);
    tft.fillScreen(TFT_BLACK);
    tft.drawString("GAME BOY ADVANCE", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 - 12);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("minimal boot", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 + 16);
    tft.setTextDatum(TL_DATUM);
    tft.display();
}  // Brings up the panel without allocating the shell's full-screen canvas buffers.

void gbaAbortBootMode(const char* reason) {
    Serial.printf("[gba boot] %s; returning to Doll-OS\n", reason ? reason : "launch failed");
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("GBA launch failed", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 - 12);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(reason ? reason : "unknown error", DISPLAY_WIDTH / 2,
                   DISPLAY_HEIGHT / 2 + 16);
    tft.setTextDatum(TL_DATUM);
    tft.display();
    gbaClearBootTicket();
    delay(1500);
    gbaRestartDevice();
}  // Shows a bounded failure message, disarms game boot, and restores the OS.

static void gbaFreeDisplay() {}

static bool gbaSetupDisplay() {
    gbaOutW = GBA_W * gbaScale;
    gbaOutH = GBA_H * gbaScale;
    gbaOutX = (DISPLAY_WIDTH - gbaOutW) / 2;
    gbaOutY = (DISPLAY_HEIGHT - gbaOutH) / 2;
    Serial.printf("[gba] display=%dx direct DSI framebuffer\n", gbaScale);
    return true;
}

static void gbaSetDisplayScale(int scale) {
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;
    gbaFreeDisplay();
    gbaScale = scale;
    if (!gbaSetupDisplay()) {
        gbaScale = 1;
        gbaSetupDisplay();
    }
}

static void gbaDrawTouchControlsTo(lgfx::LGFXBase& surface) {
    const uint16_t fill = 0x2104;
    const int dpadX = 110;
    const int dpadY = 400;
    const int arm = 75;
    const int thick = 70;
    surface.fillRoundRect(dpadX - thick / 2,
                              dpadY - arm - thick / 2,
                              thick, arm + thick / 2, 10, fill);
    surface.fillRoundRect(dpadX - thick / 2,
                              dpadY,
                              thick, arm + thick / 2, 10, fill);
    surface.fillRoundRect(dpadX - arm - thick / 2,
                              dpadY - thick / 2,
                              arm + thick / 2, thick, 10, fill);
    surface.fillRoundRect(dpadX,
                              dpadY - thick / 2,
                              arm + thick / 2, thick, 10, fill);
    surface.drawRoundRect(dpadX - thick / 2,
                              dpadY - arm - thick / 2,
                              thick, arm * 2 + thick, 10, TFT_CYAN);
    surface.drawRoundRect(dpadX - arm - thick / 2,
                              dpadY - thick / 2,
                              arm * 2 + thick, thick, 10, TFT_CYAN);

    surface.fillCircle(1200, 350, 62, fill);
    surface.drawCircle(1200, 350, 62, TFT_PINK);
    surface.fillCircle(1200, 505, 62, fill);
    surface.drawCircle(1200, 505, 62, TFT_PINK);

    surface.fillRoundRect(18, 38, 124, 64, 18, fill);
    surface.drawRoundRect(18, 38, 124, 64, 18, TFT_YELLOW);
    surface.fillRoundRect(1138, 38, 124, 64, 18, fill);
    surface.drawRoundRect(1138, 38, 124, 64, 18, TFT_YELLOW);

    surface.fillRoundRect(420, 656, 170, 48, 18, fill);
    surface.drawRoundRect(420, 656, 170, 48, 18, TFT_CYAN);
    surface.fillRoundRect(690, 656, 170, 48, 18, fill);
    surface.drawRoundRect(690, 656, 170, 48, 18, TFT_CYAN);
    surface.fillRoundRect(1040, 656, 210, 48, 18, fill);
    surface.drawRoundRect(1040, 656, 210, 48, 18, TFT_RED);

    surface.setTextDatum(MC_DATUM);
    surface.setTextColor(TFT_WHITE);
    surface.drawString("A", 1200, 350);
    surface.drawString("B", 1200, 505);
    surface.drawString("L", 80, 70);
    surface.drawString("R", 1200, 70);
    surface.drawString("SELECT", 505, 680);
    surface.drawString("START", 775, 680);
    surface.drawString("MENU", 1145, 680);
    surface.setTextDatum(TL_DATUM);
}  // Paints identical controls onto either the shell canvas or bare DSI panel.

static void gbaDrawTouchControls() {
    if (gbaStandaloneMode) gbaDrawTouchControlsTo(tft);
    else gbaDrawTouchControlsTo(frameSprite);
}  // Chooses the zero-extra-buffer surface while the OS is intentionally absent.

static void gbaClearPanel() {
    if (gbaStandaloneMode) tft.fillScreen(TFT_BLACK);
    else frameSprite.fillSprite(TFT_BLACK);
    gbaDrawTouchControls();
    if (gbaStandaloneMode) tft.display();
    else {
        displayInvalidateShadow();
        pushDisplayFrame();
    }
}  // Clears and presents without touching unallocated shell display state.

static void gbaBlitFrame() {
    const uint16_t* source = gbaHost.frame();
    if (!source) return;

    // Tab5's physical DSI framebuffer is 720x1280 portrait while the UI is
    // rotation 3 (1280x720). A normal pushImage therefore takes M5GFX's
    // per-pixel rotation path. At 3x that measured 63 ms, and the old path also
    // copied every game frame through frameSprite. Write the already-rotated
    // game rectangle directly into the panel framebuffer instead: logical
    // (x,y) maps to physical (y, 1279-x). display() then performs the required
    // cache writeback for just this rectangle.
    auto* panel = static_cast<lgfx::Panel_DSI*>(tft.getPanel());
    uint16_t* panelFrame = panel
        ? static_cast<uint16_t*>(panel->config_detail().buffer) : nullptr;
    if (!panelFrame) return;

    constexpr int panelStride = DISPLAY_HEIGHT;  // physical width: 720 pixels
    for (int sourceX = 0; sourceX < GBA_W; ++sourceX) {
        uint16_t* outputRows[3] = {};
        for (int duplicateX = 0; duplicateX < gbaScale; ++duplicateX) {
            const int logicalX = gbaOutX + sourceX * gbaScale + duplicateX;
            const int physicalY = DISPLAY_WIDTH - 1 - logicalX;
            outputRows[duplicateX] = panelFrame +
                static_cast<size_t>(physicalY) * panelStride + gbaOutY;
        }
        for (int sourceY = 0; sourceY < GBA_H; ++sourceY) {
            const uint16_t color =
                source[static_cast<size_t>(sourceY) * GBA_W + sourceX];
            const int outputY = sourceY * gbaScale;
            for (int duplicateX = 0; duplicateX < gbaScale; ++duplicateX) {
                uint16_t* output = outputRows[duplicateX] + outputY;
                for (int duplicateY = 0; duplicateY < gbaScale; ++duplicateY) {
                    output[duplicateY] = color;
                }
            }
        }
    }
    tft.display(gbaOutX, gbaOutY, gbaOutW, gbaOutH);
    if (!gbaStandaloneMode) displayInvalidateShadow();
}

static uint8_t gbaPumpTouch(uint16_t& buttons) {
    M5.update();
    uint16_t next = 0;
    bool menuDown = false;

    const uint8_t count = M5.Touch.getCount();
    for (uint8_t i = 0; i < count; ++i) {
        const auto& touch = M5.Touch.getDetail(i);
        if (!touch.isPressed()) continue;
        const int x = touch.x;
        const int y = touch.y;

        if (x >= 18 && x < 142 && y >= 38 && y < 102) {
            next |= GameBoyAdvanceHost::kL;
        } else if (x >= 1138 && x < 1262 && y >= 38 && y < 102) {
            next |= GameBoyAdvanceHost::kR;
        } else if (x < 230 && y >= 280 && y < 520) {
            const int dx = x - 110;
            const int dy = y - 400;
            if (dx < -30) next |= GameBoyAdvanceHost::kLeft;
            if (dx > 30) next |= GameBoyAdvanceHost::kRight;
            if (dy < -30) next |= GameBoyAdvanceHost::kUp;
            if (dy > 30) next |= GameBoyAdvanceHost::kDown;
        } else if (x >= 1120 && y >= 275 && y < 430) {
            next |= GameBoyAdvanceHost::kA;
        } else if (x >= 1120 && y >= 430 && y < 590) {
            next |= GameBoyAdvanceHost::kB;
        } else if (x >= 400 && x < 620 && y >= 640) {
            next |= GameBoyAdvanceHost::kSelect;
        } else if (x >= 660 && x < 900 && y >= 640) {
            next |= GameBoyAdvanceHost::kStart;
        } else if (x >= 1000 && y >= 640) {
            menuDown = true;
        }
    }

    static bool menuWasDown = false;
    const uint8_t events = (menuDown && !menuWasDown) ? GB_EVT_MENU : 0;
    menuWasDown = menuDown;
    buttons = next;
    return events;
}

static bool gbaIsRomPath(String path) {
    path.toLowerCase();
    return path.endsWith(".gba") || path.endsWith(".agb") || path.endsWith(".bin");
}

static bool gbaIsModeArg(const String& arg) {
    return arg == "1x" || arg == "2x" || arg == "3x";
}

static int gbaModeScale(const String& arg) {
    if (arg == "1x") return 1;
    if (arg == "2x") return 2;
    return 3;
}

static void gbaInsertRomName(String names[], int& count, const String& name, bool& truncated) {
    if (count >= GBA_ROM_MENU_MAX) {
        truncated = true;
        return;
    }
    int insertAt = count;
    while (insertAt > 0 && gbNameLess(name, names[insertAt - 1])) {
        names[insertAt] = names[insertAt - 1];
        --insertAt;
    }
    names[insertAt] = name;
    ++count;
}

static void gbaCollectRomNamesInDir(fs::FS& fs, const String& realDir,
                                    const String& relativeDir, String names[],
                                    int& count, bool& truncated) {
    ledPulseStorageRead(true);
    File dir = fs.open(realDir);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
    }

    File entry = dir.openNextFile();
    while (entry) {
        const String name = gbBaseName(entry.name());
        const bool isDir = entry.isDirectory();
        entry.close();
        if (name.length()) {
            if (isDir) {
                gbaCollectRomNamesInDir(fs, gbJoinChildPath(realDir, name),
                                        gbJoinRelativePath(relativeDir, name),
                                        names, count, truncated);
            } else if (gbaIsRomPath(name)) {
                gbaInsertRomName(names, count,
                                 gbJoinRelativePath(relativeDir, name), truncated);
            }
        }
        entry = dir.openNextFile();
    }
    dir.close();
}

static int gbaCollectRomNames(String names[], bool& truncated) {
    truncated = false;
    int count = 0;
    if (!sdCardMounted) return 0;
    RoutedPath path = routePath(GBA_ROM_DIR);
    gbaCollectRomNamesInDir(*path.fs, path.realPath, "", names, count, truncated);
    return count;
}

static void gbaDrawRomMenu(String names[], int count, int selected, bool truncated) {
    const int left = 365;
    const int width = 550;
    const int top = 28;
    const int listTop = 56;
    const int footY = 620;
    const int rowH = 18;
    int visibleRows = min(count, (footY - listTop - 4) / rowH);
    int first = max(0, selected - visibleRows / 2);
    if (first + visibleRows > count) first = max(0, count - visibleRows);

    frameSprite.fillSprite(TFT_BLACK);
    frameSprite.setTextDatum(TL_DATUM);
    frameSprite.setTextColor(TFT_PINK, TFT_BLACK);
    frameSprite.drawString("GAME BOY ADVANCE ROMS", left, top);
    frameSprite.setTextDatum(TR_DATUM);
    frameSprite.setTextColor(TFT_CYAN, TFT_BLACK);
    frameSprite.drawString(String(selected + 1) + "/" + String(count), left + width, top);
    frameSprite.setTextDatum(TL_DATUM);
    frameSprite.drawFastHLine(left, top + 14, width, TFT_PINK);

    for (int row = 0; row < visibleRows; ++row) {
        const int index = first + row;
        const int y = listTop + row * rowH;
        const bool on = index == selected;
        frameSprite.setTextColor(on ? TFT_YELLOW : TFT_WHITE, TFT_BLACK);
        frameSprite.drawString(on ? ">" : " ", left, y);
        frameSprite.drawString(gbFitMenuText(names[index], width - 18), left + 12, y);
    }
    frameSprite.setTextColor(truncated ? TFT_YELLOW : TFT_DARKGREY, TFT_BLACK);
    frameSprite.drawString(truncated ? "showing first 128 ROMs" :
                           "D-pad move/page  A/Start choose  B/Menu cancel", left, footY);
    if (truncated) {
        frameSprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
        frameSprite.drawString("D-pad move/page  A/Start choose  B/Menu cancel", left, footY + 12);
    }
    gbaDrawTouchControls();
    displayInvalidateShadow();
    pushDisplayFrame();
}

static bool gbaPickRom(String& romLogical) {
    if (!sdCardMounted) {
        outLine("gba: SD not mounted (insert card and reboot)", C_RED);
        return false;
    }
    String* names = new (std::nothrow) String[GBA_ROM_MENU_MAX];
    if (!names) {
        outLine("gba: not enough memory for the ROM picker", C_RED);
        return false;
    }
    bool truncated = false;
    const int count = gbaCollectRomNames(names, truncated);
    if (!count) {
        delete[] names;
        outLine("gba: no .gba/.agb/.bin files found in /sd/gba", C_YELLOW);
        return false;
    }

    outLine("gba: choose a ROM from /sd/gba", C_CYAN);
    drawDisplayFrame();
    slaveLinkSendLine("GAME 1");
    delay(20);
    gbResetTelnetMenuInput();

    uint8_t legacyButtons = 0;
    uint16_t touchButtons = 0;
    uint16_t previous = 0;
    int selected = 0;
    bool redraw = true;
    for (;;) {
        if (redraw) {
            gbaDrawRomMenu(names, count, selected, truncated);
            redraw = false;
        }
        uint8_t telnetPressed = 0;
        uint8_t events = gbPumpInput(legacyButtons);
        events |= gbaPumpTouch(touchButtons);
        events |= gbPumpTelnetMenuInput(telnetPressed);
        if (events & (GB_EVT_QUIT | GB_EVT_MENU)) {
            delete[] names;
            slaveLinkSendLine("GAME 0");
            outLine("gba: ROM selection cancelled", C_YELLOW);
            return false;
        }
        const uint16_t combined = static_cast<uint16_t>(legacyButtons) | touchButtons;
        const uint16_t pressed = (combined & ~previous) | telnetPressed;
        previous = combined;
        if (!pressed) {
            delay(15);
            continue;
        }
        if (pressed & GameBoyAdvanceHost::kUp) {
            selected = (selected + count - 1) % count;
        } else if (pressed & GameBoyAdvanceHost::kDown) {
            selected = (selected + 1) % count;
        } else if (pressed & GameBoyAdvanceHost::kLeft) {
            selected = max(0, selected - 10);
        } else if (pressed & GameBoyAdvanceHost::kRight) {
            selected = min(count - 1, selected + 10);
        } else if (pressed & GameBoyAdvanceHost::kB) {
            delete[] names;
            slaveLinkSendLine("GAME 0");
            outLine("gba: ROM selection cancelled", C_YELLOW);
            return false;
        } else if (pressed & (GameBoyAdvanceHost::kA | GameBoyAdvanceHost::kStart)) {
            slaveLinkSendLine("GAME 0");
            romLogical = String(GBA_ROM_DIR) + "/" + names[selected];
            delete[] names;
            return true;
        }
        redraw = true;
    }
}

enum GbaMenuItem : uint8_t {
    GBA_MENU_DISPLAY,
    GBA_MENU_FRAME_SKIP,
    GBA_MENU_CPU_ENGINE,
    GBA_MENU_VOLUME,
    GBA_MENU_SAVE_STATE,
    GBA_MENU_LOAD_STATE,
    GBA_MENU_RESUME,
    GBA_MENU_QUIT,
    GBA_MENU_COUNT,
};

static String gbaStatePath() {
    return gbSiblingPath(gbaRomVfs, ".gstate");
}

static String gbaMenuValue(int item) {
    if (item == GBA_MENU_DISPLAY) return String(gbaScale) + "x";
    if (item == GBA_MENU_FRAME_SKIP) {
        if (gbaFrameSkip < 0) return "Auto";
        return String(gbaFrameSkip) + " (render 1/" +
               String(gbaFrameSkip + 1) + ")";
    }
    if (item == GBA_MENU_VOLUME) {
        return String(radioGetVolume()) + "/" + String(RADIO_VOLUME_MAX);
    }
    if (item == GBA_MENU_CPU_ENGINE) {
        const uint32_t mode = doll_gba_core_get_cpu_mode();
        if (mode == DOLL_GBA_CPU_BATCH) return "Batch";
        if (mode == DOLL_GBA_CPU_JIT_DEBUG) return "JIT trace";
        if (mode == DOLL_GBA_CPU_TURBO) return "Turbo";
        return "Safe";
    }
    return "";
}

static void gbaDrawMenuTo(lgfx::LGFXBase& surface, int selected, const String& note) {
    const int rowH = 22;
    const int width = 460;
    const int left = (DISPLAY_WIDTH - width) / 2;
    const int boxH = GBA_MENU_COUNT * rowH + 106;
    const int top = (DISPLAY_HEIGHT - boxH) / 2;
    surface.fillRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, TFT_BLACK);
    surface.drawRect(left - 10, top - 10, width + 20, boxH, TFT_PINK);
    surface.setTextDatum(TL_DATUM);
    surface.setTextColor(TFT_PINK, TFT_BLACK);
    surface.drawString("GAME BOY ADVANCE", left, top);
    surface.drawFastHLine(left, top + 14, width, TFT_PINK);

    for (int item = 0; item < GBA_MENU_COUNT; ++item) {
        const int y = top + 26 + item * rowH;
        surface.setTextColor(item == selected ? TFT_YELLOW : TFT_WHITE, TFT_BLACK);
        surface.drawString(item == selected ? ">" : " ", left, y);
        const char* label = "";
        switch (item) {
            case GBA_MENU_DISPLAY: label = "Display"; break;
            case GBA_MENU_FRAME_SKIP: label = "Frame skip"; break;
            case GBA_MENU_CPU_ENGINE: label = "CPU engine"; break;
            case GBA_MENU_VOLUME: label = "Volume"; break;
            case GBA_MENU_SAVE_STATE: label = "Save state"; break;
            case GBA_MENU_LOAD_STATE: label = "Load state"; break;
            case GBA_MENU_RESUME: label = "Resume"; break;
            case GBA_MENU_QUIT: label = "Quit ROM"; break;
        }
        surface.drawString(label, left + 14, y);
        const String value = gbaMenuValue(item);
        if (value.length()) {
            surface.setTextDatum(TR_DATUM);
            surface.setTextColor(TFT_CYAN, TFT_BLACK);
            surface.drawString(value, left + width, y);
            surface.setTextDatum(TL_DATUM);
        }
    }
    const int footY = top + 30 + GBA_MENU_COUNT * rowH;
    doll_gba_perf_stats_t perf = {};
    doll_gba_core_get_perf(&perf);
    surface.setTextColor(TFT_CYAN, TFT_BLACK);
    surface.drawString(
        "Perf " + String(gbaLastEmuFps10 / 10) + "." + String(gbaLastEmuFps10 % 10) +
        " fps  core " + String(gbaLastCoreUs / 1000) + "ms  draw/skip " +
        String(gbaLastDrawCoreUs / 1000) + "/" + String(gbaLastSkipCoreUs / 1000) + "ms",
        left, footY);
    surface.drawString(
        "Audio " + String(gbaLastAudioUs / 1000) + "ms  blit " +
        String(gbaLastBlitUs / 1000) + "ms  JIT " + String(perf.jit_bytes / 1024) +
        "K " + String(perf.jit_hits) + "/" + String(perf.jit_misses) +
        " V:" + (perf.vram_internal ? "L2" : "P"),
        left, footY + 16);
    surface.drawString(
        "Guard " + String(perf.jit_guard_trips) + " reset/pc " +
        String(perf.softreset_count) + "/" + String(perf.bad_pc_count) +
        " last " + String(perf.jit_last_pc, HEX) + ">" +
        String(perf.jit_last_end_pc, HEX),
        left, footY + 32);
    if (note.length()) {
        surface.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
        surface.drawString(note, left, footY + 48);
    }
    surface.setTextColor(TFT_DARKGREY, TFT_BLACK);
    surface.drawString("D-pad move/change  A ok  B/Menu back", left, footY + 64);
    gbaDrawTouchControlsTo(surface);
}  // Draws the in-game menu without depending on a particular framebuffer owner.

static void gbaDrawMenu(int selected, const String& note) {
    if (gbaStandaloneMode) {
        gbaDrawMenuTo(tft, selected, note);
        tft.display();
    } else {
        gbaDrawMenuTo(frameSprite, selected, note);
        displayInvalidateShadow();
        pushDisplayFrame();
    }
}  // Presents through DSI directly in minimal boot and through the shell normally.

static bool gbaRunMenu(uint8_t& legacyButtons, uint16_t& touchButtons) {
    int selected = 0;
    String note;
    uint16_t previous = static_cast<uint16_t>(legacyButtons) | touchButtons;
    bool redraw = true;
    for (;;) {
        if (redraw) {
            gbaDrawMenu(selected, note);
            redraw = false;
        }
        uint8_t events = gbPumpInput(legacyButtons);
        events |= gbaPumpTouch(touchButtons);
        if (events & GB_EVT_QUIT) return true;
        if (events & GB_EVT_MENU) return false;

        const uint16_t combined = static_cast<uint16_t>(legacyButtons) | touchButtons;
        const uint16_t pressed = combined & ~previous;
        previous = combined;
        if (!pressed) {
            delay(15);
            continue;
        }
        redraw = true;
        if (pressed & GameBoyAdvanceHost::kUp) {
            selected = (selected + GBA_MENU_COUNT - 1) % GBA_MENU_COUNT;
            continue;
        }
        if (pressed & GameBoyAdvanceHost::kDown) {
            selected = (selected + 1) % GBA_MENU_COUNT;
            continue;
        }
        if (pressed & GameBoyAdvanceHost::kB) return false;

        const bool left = pressed & GameBoyAdvanceHost::kLeft;
        const bool right = pressed & GameBoyAdvanceHost::kRight;
        const bool ok = pressed & (GameBoyAdvanceHost::kA | GameBoyAdvanceHost::kStart);
        if (!left && !right && !ok) continue;
        switch (selected) {
            case GBA_MENU_DISPLAY: {
                int next = gbaScale;
                if (left) next = next == 1 ? 3 : next - 1;
                else next = next == 3 ? 1 : next + 1;
                gbaSetDisplayScale(next);
                note = String(gbaScale) + "x: " + String(gbaOutW) + "x" + String(gbaOutH);
                break;
            }
            case GBA_MENU_FRAME_SKIP: {
                if (left) {
                    gbaFrameSkip = gbaFrameSkip <= -1 ? GBA_MAX_FRAME_SKIP
                                                      : gbaFrameSkip - 1;
                } else {
                    gbaFrameSkip = gbaFrameSkip >= GBA_MAX_FRAME_SKIP
                                       ? -1 : gbaFrameSkip + 1;
                }
                note = gbaFrameSkip < 0
                    ? "automatic deadline-based skipping"
                    : "render 1 of every " + String(gbaFrameSkip + 1) +
                      " emulated frames; panel remains capped at 66ms";
                break;
            }
            case GBA_MENU_CPU_ENGINE: {
                uint32_t mode = doll_gba_core_get_cpu_mode();
                if (left) mode = mode == DOLL_GBA_CPU_SAFE
                    ? DOLL_GBA_CPU_TURBO : mode - 1;
                else mode = (mode + 1) % DOLL_GBA_CPU_MODE_COUNT;
                doll_gba_core_set_cpu_mode(mode);
                note = mode == DOLL_GBA_CPU_SAFE
                    ? "exact single-step interpreter"
                    : (mode == DOLL_GBA_CPU_BATCH
                        ? "batched interpreter; JIT disabled"
                        : (mode == DOLL_GBA_CPU_JIT_DEBUG
                            ? "JIT only; every compiled block is checked"
                            : "batched interpreter plus trusted JIT"));
                break;
            }
            case GBA_MENU_VOLUME:
                if (left) radioAdjustVolume(-1);
                else if (right) radioAdjustVolume(1);
                else if (radioGetVolume() >= RADIO_VOLUME_MAX) radioAdjustVolume(-RADIO_VOLUME_MAX);
                else radioAdjustVolume(1);
                note = "shared with the radio";
                break;
            case GBA_MENU_SAVE_STATE:
                if (ok) {
                    ledPulseStorageRead(true);
                    note = gbaHost.saveState(gbaStatePath().c_str())
                               ? "state saved" : "save failed (card full or read-only?)";
                }
                break;
            case GBA_MENU_LOAD_STATE:
                if (ok) {
                    ledPulseStorageRead(true);
                    note = gbaHost.loadState(gbaStatePath().c_str())
                               ? "state loaded" : "load failed (no state saved yet?)";
                }
                break;
            case GBA_MENU_RESUME:
                if (ok) return false;
                break;
            case GBA_MENU_QUIT:
                if (ok) return true;
                break;
        }
    }
}

static void gbaPrintUsage() {
    outLine("Usage: gba [rom.gba|.agb|.bin] [1x|2x|3x]", C_CYAN);
    outLine("  Bare 'gba' opens the recursive /sd/gba ROM picker.", C_CYAN);
    outLine("  3x is default; display scale can also be changed in-game.", C_CYAN);
    outLine("  Escape/touch MENU: display, CPU engine, volume, states, quit.", C_CYAN);
    outLine("  States sit next to the ROM as <name>.gstate.", C_CYAN);
    outLine("  Controls: arrows/WASD, A/B, Enter, Backspace; touch adds L/R.", C_WHITE);
    outLine("  Quit: Ctrl+T, or choose Quit ROM from the menu.", C_WHITE);
}

#if defined(DOLL_BOARD_TAB5)
// Rightmost of the two status-bar launchers; the GB one (Gameboy.ino) sits to
// its left. Same 32-pixel band and same y range, so neither steals terminal
// space nor collides with the ROM picker's MENU control at y=30.
static constexpr int GBA_LAUNCH_W = 64;
static constexpr int GBA_LAUNCH_H = 25;
static constexpr int GBA_LAUNCH_Y = 3;
static constexpr int GBA_LAUNCH_X = DISPLAY_WIDTH - 72;

void gbaDrawMainTouchLauncher() {
    frameSprite.fillRoundRect(GBA_LAUNCH_X, GBA_LAUNCH_Y,
                              GBA_LAUNCH_W, GBA_LAUNCH_H, 7, 0x2104);
    frameSprite.drawRoundRect(GBA_LAUNCH_X, GBA_LAUNCH_Y,
                              GBA_LAUNCH_W, GBA_LAUNCH_H, 7, TFT_MAGENTA);
    frameSprite.setTextDatum(MC_DATUM);
    frameSprite.setTextColor(TFT_WHITE);
    frameSprite.drawString("GBA", GBA_LAUNCH_X + GBA_LAUNCH_W / 2,
                          GBA_LAUNCH_Y + GBA_LAUNCH_H / 2);
    frameSprite.setTextDatum(TL_DATUM);
}

void gbaServiceMainTouch() {
    static bool launcherWasDown = false;
    M5.update();

    bool launcherDown = false;
    if (!dappCanvasActive) {
        const uint8_t count = M5.Touch.getCount();
        for (uint8_t i = 0; i < count; i++) {
            const auto& touch = M5.Touch.getDetail(i);
            if (!touch.isPressed()) continue;
            if (touch.x >= GBA_LAUNCH_X && touch.x < GBA_LAUNCH_X + GBA_LAUNCH_W &&
                touch.y >= GBA_LAUNCH_Y && touch.y < GBA_LAUNCH_Y + GBA_LAUNCH_H) {
                launcherDown = true;
                break;
            }
        }
    }

    const bool launch = launcherDown && !launcherWasDown;
    launcherWasDown = launcherDown;
    if (launch) {
        String command = "gba";
        commandProcessor(command);  // same history, picker, and cleanup path as typed `gba`
    }
}
#else
void gbaDrawMainTouchLauncher() {}
void gbaServiceMainTouch() {}
#endif

static void gbaRunBootSession() {
    const String saveVfs = gbSiblingPath(gbaRomVfs, ".sav");
    ledPulseStorageRead(true);

    if (!gbaHost.load(gbaRomVfs, saveVfs)) {
        const String status = gbaHost.status();
        gbaHost.stop();
        gbaFreeDisplay();
        gbaAbortBootMode(status.c_str());
        return;
    }
    // Let the core reserve its bounded executable/hot-memory working set first.
    // Scaling now writes directly into the already-allocated DSI framebuffer,
    // so changing display modes does not consume another RAM buffer.
    if (!gbaSetupDisplay()) {
        gbaScale = 1;
        gbaSetupDisplay();
        Serial.println("[gba] low memory, using native 1x");
    }

    bool audioUp = false;
    if (radioReleaseAudio()) audioUp = AudioOut::begin();
    if (!audioUp) Serial.println("[gba] audio unavailable -- running silent");
    Serial.printf("[gba] launching %s at %dx -- Escape for menu\n",
                  gbaRomVfs.c_str(), gbaScale);

    slaveLinkSendLine("GAME 1");
    delay(20);
    gbaClearPanel();

    uint8_t legacyButtons = 0;
    uint16_t touchButtons = 0;
    constexpr uint32_t frameUs = 16743;  // 280896 GBA cycles at 16.777216 MHz
    uint32_t nextFrame = micros() + frameUs;
    uint32_t nextBlitUs = micros();
    const uint32_t startedMs = millis();
    uint32_t framesRun = 0;
    uint32_t framesDrawn = 0;
    uint64_t coreTimeUs = 0;
    uint64_t drawCoreTimeUs = 0;
    uint64_t skipCoreTimeUs = 0;
    uint64_t audioTimeUs = 0;
    uint64_t blitTimeUs = 0;
    uint32_t perfStartedUs = micros();
    uint32_t perfFrames = 0;
    uint32_t perfDraws = 0;
    uint32_t perfSkips = 0;
    doll_gba_perf_stats_t modeStart = {};
    doll_gba_core_get_perf(&modeStart);
    int skipped = GBA_MAX_FRAME_SKIP;
    gbaLastEmuFps10 = 0;
    gbaLastCoreUs = 0;
    gbaLastDrawCoreUs = 0;
    gbaLastSkipCoreUs = 0;
    gbaLastAudioUs = 0;
    gbaLastBlitUs = 0;

    for (;;) {
        uint8_t events = gbPumpInput(legacyButtons);
        events |= gbaPumpTouch(touchButtons);
        if (events & GB_EVT_QUIT) break;
        if (events & GB_EVT_MENU) {
            if (gbaRunMenu(legacyButtons, touchButtons)) break;
            gbaClearPanel();
            nextFrame = micros() + frameUs;
            nextBlitUs = micros();
            skipped = gbaFrameSkip < 0 ? GBA_MAX_FRAME_SKIP : gbaFrameSkip;
        }

        const bool late = static_cast<int32_t>(micros() - nextFrame) > 0;
        const uint32_t nowUs = micros();
        const bool panelDue = static_cast<int32_t>(nowUs - nextBlitUs) >= 0;
        const bool draw = gbaFrameSkip < 0
            ? (panelDue && (!late || skipped >= GBA_MAX_FRAME_SKIP))
            : (panelDue && skipped >= gbaFrameSkip);
        gbaHost.setButtons(static_cast<uint16_t>(legacyButtons) | touchButtons);
        gbaHost.runFrame(draw);
        const uint32_t coreUs = gbaHost.lastCoreTimeUs();
        coreTimeUs += coreUs;
        audioTimeUs += gbaHost.lastAudioTimeUs();
        ++perfFrames;
        if (draw) {
            drawCoreTimeUs += coreUs;
            const uint32_t blitStartedUs = micros();
            gbaBlitFrame();
            blitTimeUs += static_cast<uint32_t>(micros() - blitStartedUs);
            ++framesDrawn;
            ++perfDraws;
            skipped = 0;
            nextBlitUs += GBA_PANEL_INTERVAL_US;
            if (static_cast<int32_t>(micros() - nextBlitUs) >=
                static_cast<int32_t>(GBA_PANEL_INTERVAL_US)) {
                nextBlitUs = micros() + GBA_PANEL_INTERVAL_US;
            }
        } else {
            skipCoreTimeUs += coreUs;
            ++perfSkips;
            ++skipped;
        }
        gbaHost.tickSave();
        ledService();
        ++framesRun;

        if (perfFrames >= 120) {
            const uint32_t elapsedUs = micros() - perfStartedUs;
            const uint32_t emuFps10 = elapsedUs
                ? static_cast<uint32_t>(static_cast<uint64_t>(perfFrames) * 10000000ULL / elapsedUs) : 0;
            const uint32_t drawFps10 = elapsedUs
                ? static_cast<uint32_t>(static_cast<uint64_t>(perfDraws) * 10000000ULL / elapsedUs) : 0;
            gbaLastEmuFps10 = emuFps10;
            gbaLastCoreUs = static_cast<uint32_t>(coreTimeUs / perfFrames);
            gbaLastDrawCoreUs = perfDraws ? static_cast<uint32_t>(drawCoreTimeUs / perfDraws) : 0;
            gbaLastSkipCoreUs = perfSkips ? static_cast<uint32_t>(skipCoreTimeUs / perfSkips) : 0;
            gbaLastAudioUs = static_cast<uint32_t>(audioTimeUs / perfFrames);
            gbaLastBlitUs = perfDraws ? static_cast<uint32_t>(blitTimeUs / perfDraws) : 0;
            doll_gba_perf_stats_t coreStats = {};
            doll_gba_core_get_perf(&coreStats);
            const uint32_t armUpdates = coreStats.arm_updates - modeStart.arm_updates;
            const uint32_t thumbUpdates = coreStats.thumb_updates - modeStart.thumb_updates;
            const uint32_t haltUpdates = coreStats.halt_updates - modeStart.halt_updates;
            const uint32_t jitHits = coreStats.jit_hits - modeStart.jit_hits;
            const uint32_t jitMisses = coreStats.jit_misses - modeStart.jit_misses;
            const uint32_t jitAttempts = coreStats.jit_attempts - modeStart.jit_attempts;
            const uint32_t jitCompiles = coreStats.jit_compiles - modeStart.jit_compiles;
            const uint32_t jitOps = coreStats.jit_ops - modeStart.jit_ops;
            Serial.printf("[gba perf] mode=%dx skip=%d emu=%lu.%lu drawn=%lu.%lu core=%lluus drawcore=%lluus skipcore=%lluus audio=%lluus blit=%lluus arm/thumb/halt=%lu/%lu/%lu pc=%08lx cpsr=%08lx jit=%lu/%luK hit/miss/try=%lu/%lu/%lu ops=%lu build=%lu full=%lu reuse=%lu wait/reject/probe=%lu/%lu/%lu break=%02lx:%lu batch=%lu/%lu vram=%s rom=%lu+%lu cpu=%luMHz\n",
                          gbaScale,
                          gbaFrameSkip,
                          static_cast<unsigned long>(emuFps10 / 10),
                          static_cast<unsigned long>(emuFps10 % 10),
                          static_cast<unsigned long>(drawFps10 / 10),
                          static_cast<unsigned long>(drawFps10 % 10),
                           coreTimeUs / perfFrames,
                           perfDraws ? drawCoreTimeUs / perfDraws : 0,
                           perfSkips ? skipCoreTimeUs / perfSkips : 0,
                           audioTimeUs / perfFrames,
                           perfDraws ? blitTimeUs / perfDraws : 0,
                           static_cast<unsigned long>(armUpdates),
                           static_cast<unsigned long>(thumbUpdates),
                           static_cast<unsigned long>(haltUpdates),
                           static_cast<unsigned long>(coreStats.last_pc),
                           static_cast<unsigned long>(coreStats.last_cpsr),
                           static_cast<unsigned long>(coreStats.jit_used_bytes / 1024),
                           static_cast<unsigned long>(coreStats.jit_bytes / 1024),
                           static_cast<unsigned long>(jitHits),
                           static_cast<unsigned long>(jitMisses),
                           static_cast<unsigned long>(jitAttempts),
                           static_cast<unsigned long>(jitOps),
                           static_cast<unsigned long>(jitCompiles),
                           static_cast<unsigned long>(coreStats.jit_arena_full),
                           static_cast<unsigned long>(coreStats.jit_reuses),
                           static_cast<unsigned long>(coreStats.jit_hot_waits),
                           static_cast<unsigned long>(coreStats.jit_reject_hits),
                           static_cast<unsigned long>(coreStats.jit_adapt_probes),
                           static_cast<unsigned long>(coreStats.jit_top_break),
                           static_cast<unsigned long>(coreStats.jit_top_break_count),
                           static_cast<unsigned long>(coreStats.thumb_batch_ops - modeStart.thumb_batch_ops),
                           static_cast<unsigned long>(coreStats.thumb_batch_runs - modeStart.thumb_batch_runs),
                           coreStats.vram_internal ? "L2" : "PSRAM",
                           static_cast<unsigned long>(coreStats.rom_page_loads),
                           static_cast<unsigned long>(coreStats.rom_page_prefetches),
                           static_cast<unsigned long>(getCpuFrequencyMhz()));
            Serial.printf("[gba jitdbg] engine=%lu reset=%lu badpc=%lu guard=%lu last=%08lx->%08lx ret=%08lx sig=%08lx\n",
                          static_cast<unsigned long>(coreStats.cpu_mode),
                          static_cast<unsigned long>(coreStats.softreset_count),
                          static_cast<unsigned long>(coreStats.bad_pc_count),
                          static_cast<unsigned long>(coreStats.jit_guard_trips),
                          static_cast<unsigned long>(coreStats.jit_last_pc),
                          static_cast<unsigned long>(coreStats.jit_last_end_pc),
                          static_cast<unsigned long>(coreStats.jit_last_ret),
                          static_cast<unsigned long>(coreStats.jit_last_signature));
            // A guest restart that never issues SWI SoftReset leaves reset=0, so
            // these name the BIOS path it took instead and the branch that got
            // it there. biosinit rising without vector rising means the reboot
            // came through the BIOS init loop rather than a null-pointer branch.
            Serial.printf("[gba reset] biosinit=%lu vector=%lu entry=%lu from=%08lx lr=%08lx sp=%08lx\n",
                          static_cast<unsigned long>(coreStats.bios_init_loops),
                          static_cast<unsigned long>(coreStats.guest_reset_trips),
                          static_cast<unsigned long>(coreStats.guest_entry_trips),
                          static_cast<unsigned long>(coreStats.guest_reset_prev_pc),
                          static_cast<unsigned long>(coreStats.guest_reset_lr),
                          static_cast<unsigned long>(coreStats.guest_reset_sp));
            // on=SOUNDCNT_X master enable. avail/max are samples sitting in the
            // core's ring; req/ret are what the host asked for versus got. Zero
            // avail means the emulated sound engine is not filling the ring at
            // all, which is a different bug from ret lagging req downstream.
            // The sink side. pushed climbing while the speaker stays silent puts
            // the fault at the codec/amp rather than anywhere in software; pushed
            // flat means onSamples is bailing before i2s_channel_write.
            uint32_t aoPushed = 0, aoDropped = 0, aoUnderruns = 0;
            AudioOut::stats(aoPushed, aoDropped, aoUnderruns);
            Serial.printf("[gba i2s] ready=%d pushed=%lu dropped=%lu underruns=%lu vol=%d\n",
                          AudioOut::available() ? 1 : 0,
                          static_cast<unsigned long>(aoPushed),
                          static_cast<unsigned long>(aoDropped),
                          static_cast<unsigned long>(aoUnderruns),
                          radioGetVolume());
            // nz/peak say whether the ring holds real audio or just silence:
            // healthy req/ret with nz=0 means the mixer never wrote anything,
            // which points at DirectSound FIFO rather than the submit path.
            Serial.printf("[gba audio] nz=%lu peak=%lu under=%lu\n",
                          static_cast<unsigned long>(coreStats.sound_nonzero_samples),
                          static_cast<unsigned long>(coreStats.sound_peak_sample),
                          static_cast<unsigned long>(coreStats.sound_underrun_samples));
            Serial.printf("[gba audio] on=%lu calls=%lu req=%lu ret=%lu avail=%lu max=%lu drops=%lu\n",
                          static_cast<unsigned long>(coreStats.sound_on),
                          static_cast<unsigned long>(coreStats.sound_read_calls),
                          static_cast<unsigned long>(coreStats.sound_samples_requested),
                          static_cast<unsigned long>(coreStats.sound_samples_returned),
                          static_cast<unsigned long>(coreStats.sound_last_available),
                          static_cast<unsigned long>(coreStats.sound_max_available),
                          static_cast<unsigned long>(coreStats.sound_drop_events));
            // Register and channel state separates a muted GBA mixer from an
            // enabled DirectSound channel whose DMA FIFO is starving.
            Serial.printf("[gba mixer] cnt=%04lx/%04lx/%04lx gbc=%lx ds=%lx fifo=%lu/%lu\n",
                          static_cast<unsigned long>(coreStats.sound_cnt_l),
                          static_cast<unsigned long>(coreStats.sound_cnt_h),
                          static_cast<unsigned long>(coreStats.sound_cnt_x),
                          static_cast<unsigned long>(coreStats.sound_gbc_active),
                          static_cast<unsigned long>(coreStats.sound_direct_status),
                          static_cast<unsigned long>(coreStats.sound_fifo_empty_reads),
                          static_cast<unsigned long>(coreStats.sound_fifo_short_reads));
            modeStart = coreStats;
            Serial.flush();
            coreTimeUs = 0;
            drawCoreTimeUs = 0;
            skipCoreTimeUs = 0;
            audioTimeUs = 0;
            blitTimeUs = 0;
            perfFrames = 0;
            perfDraws = 0;
            perfSkips = 0;
            perfStartedUs = micros();
        }

        nextFrame += frameUs;
        const int32_t remaining = static_cast<int32_t>(nextFrame - micros());
        if (remaining > 1000) {
            delay(remaining / 1000);
        } else {
            if (remaining < -static_cast<int32_t>(GBA_MAX_FRAME_SKIP * frameUs)) {
                nextFrame = micros() + frameUs;
            }
            vTaskDelay(1);
        }
    }

    const uint32_t ranMs = millis() - startedMs;
    AudioOut::setDiscard(true);
    gbaHost.stop();
    if (audioUp) AudioOut::end();
    gbaFreeDisplay();
    slaveLinkSendLine("GAME 0");

    if (ranMs) {
        Serial.printf("[gba] session ended: %.1f fps emulated, %.1f fps drawn\n",
                      framesRun * 1000.0f / ranMs,
                      framesDrawn * 1000.0f / ranMs);
    }
    gbaClearBootTicket();
    Serial.println("[gba boot] quit requested; rebooting into Doll-OS");
    gbaRestartDevice();
}  // Owns the minimal-mode emulator lifetime, including save flush and OS reboot.

void gbaRunBootMode() {
    if (!gbaStandaloneMode || !gbaBootTicketValid() ||
        gbaBootTicket.phase != GBA_BOOT_RUNNING) {
        gbaAbortBootMode("launch ticket disappeared");
        return;
    }
    gbaScale = gbaBootTicket.scale;
    gbaFrameSkip = gbaBootTicket.frameSkip;
    gbaRomVfs = gbaBootTicket.romPath;
    gbaRunBootSession();
}  // Applies the claimed RTC launch ticket after only minimal hardware is initialized.

void handleGbaCommand(const String parts[], int partCount) {
    String romLogical;
    if (partCount >= 2 && gbIsHelpArg(parts[1])) {
        gbaPrintUsage();
        return;
    }
    if (partCount < 2 || (partCount == 2 && gbaIsModeArg(parts[1]))) {
        gbaScale = partCount == 2 ? gbaModeScale(parts[1]) : 3;
        if (!gbaPickRom(romLogical)) return;
    } else {
        romLogical = parts[1];
        gbaScale = partCount >= 3 && gbaIsModeArg(parts[2])
            ? gbaModeScale(parts[2]) : 3;
    }
    if (!gbaIsRomPath(romLogical)) {
        outLine("gba: expected a .gba, .agb, or .bin ROM", C_RED);
        return;
    }

    gbaRomVfs = gbVfsPath(romLogical);
    if (gbaRomVfs.isEmpty()) {
        outLine("gba: storage path is unavailable", C_RED);
        return;
    }
    if (!gbaScheduleBoot(gbaRomVfs, gbaScale, gbaFrameSkip)) {
        outLine("gba: reboot mode requires an SD ROM with a path under 384 bytes", C_RED);
        return;
    }

    outLine("gba: rebooting into minimal game mode -- quitting the ROM reboots Doll-OS",
            C_GREEN);
    drawDisplayFrame();
    Serial.printf("[gba boot] scheduled %s\n", gbaRomVfs.c_str());
    gbaRestartDevice();
}  // Converts the shell launch into a reset-persistent minimal-mode boot request.
