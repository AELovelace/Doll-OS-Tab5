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
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <new>

static GameBoyAdvanceHost gbaHost;
static constexpr int GBA_W = GameBoyAdvanceHost::kWidth;
static constexpr int GBA_H = GameBoyAdvanceHost::kHeight;
static constexpr int GBA_STAGE_ROWS = 24;
static constexpr int GBA_MAX_FRAME_SKIP = 5;
static constexpr int GBA_ROM_MENU_MAX = 128;
static const char* GBA_ROM_DIR = "/sd/gba";

static uint16_t* gbaStage = nullptr;
static int gbaScale = 3;
static int gbaOutW = GBA_W * 3;
static int gbaOutH = GBA_H * 3;
static int gbaOutX = (DISPLAY_WIDTH - GBA_W * 3) / 2;
static int gbaOutY = (DISPLAY_HEIGHT - GBA_H * 3) / 2;
static String gbaRomVfs;

static void gbaFreeDisplay() {
    if (gbaStage) {
        heap_caps_free(gbaStage);
        gbaStage = nullptr;
    }
}

static bool gbaSetupDisplay() {
    gbaOutW = GBA_W * gbaScale;
    gbaOutH = GBA_H * gbaScale;
    gbaOutX = (DISPLAY_WIDTH - gbaOutW) / 2;
    gbaOutY = (DISPLAY_HEIGHT - gbaOutH) / 2;
    if (gbaScale == 1) return true;

    const size_t bytes = static_cast<size_t>(gbaOutW) * GBA_STAGE_ROWS * sizeof(uint16_t);
    gbaStage = static_cast<uint16_t*>(
        heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!gbaStage) {
        gbaStage = static_cast<uint16_t*>(
            heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    Serial.printf("[gba] display=%dx stage=%u bytes (%s)\n", gbaScale,
                  static_cast<unsigned>(bytes),
                  gbaStage && esp_ptr_external_ram(gbaStage) ? "PSRAM" : "internal");
    return gbaStage != nullptr;
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

static void gbaDrawTouchControls() {
    const uint16_t fill = 0x2104;
    frameSprite.fillRoundRect(8, 285, 144, 230, 20, fill);
    frameSprite.drawRoundRect(8, 285, 144, 230, 20, TFT_CYAN);
    frameSprite.drawLine(80, 310, 80, 490, TFT_WHITE);
    frameSprite.drawLine(25, 400, 135, 400, TFT_WHITE);

    frameSprite.fillCircle(1200, 350, 62, fill);
    frameSprite.drawCircle(1200, 350, 62, TFT_PINK);
    frameSprite.fillCircle(1200, 505, 62, fill);
    frameSprite.drawCircle(1200, 505, 62, TFT_PINK);

    frameSprite.fillRoundRect(18, 38, 124, 64, 18, fill);
    frameSprite.drawRoundRect(18, 38, 124, 64, 18, TFT_YELLOW);
    frameSprite.fillRoundRect(1138, 38, 124, 64, 18, fill);
    frameSprite.drawRoundRect(1138, 38, 124, 64, 18, TFT_YELLOW);

    frameSprite.fillRoundRect(420, 656, 170, 48, 18, fill);
    frameSprite.drawRoundRect(420, 656, 170, 48, 18, TFT_CYAN);
    frameSprite.fillRoundRect(690, 656, 170, 48, 18, fill);
    frameSprite.drawRoundRect(690, 656, 170, 48, 18, TFT_CYAN);
    frameSprite.fillRoundRect(1040, 656, 210, 48, 18, fill);
    frameSprite.drawRoundRect(1040, 656, 210, 48, 18, TFT_RED);

    frameSprite.setTextDatum(MC_DATUM);
    frameSprite.setTextColor(TFT_WHITE);
    frameSprite.drawString("D-PAD", 80, 400);
    frameSprite.drawString("A", 1200, 350);
    frameSprite.drawString("B", 1200, 505);
    frameSprite.drawString("L", 80, 70);
    frameSprite.drawString("R", 1200, 70);
    frameSprite.drawString("SELECT", 505, 680);
    frameSprite.drawString("START", 775, 680);
    frameSprite.drawString("MENU", 1145, 680);
    frameSprite.setTextDatum(TL_DATUM);
}

static void gbaClearPanel() {
    frameSprite.fillSprite(TFT_BLACK);
    gbaDrawTouchControls();
    displayInvalidateShadow();
    pushDisplayFrame();
}

static void gbaBlitFrame() {
    const uint16_t* source = gbaHost.frame();
    if (!source) return;

    const bool oldFrameSwap = frameSprite.getSwapBytes();
    const bool oldPanelSwap = tft.getSwapBytes();
    frameSprite.setSwapBytes(true);
    tft.setSwapBytes(true);

    if (gbaScale == 1) {
        frameSprite.pushImage(gbaOutX, gbaOutY, GBA_W, GBA_H,
                              const_cast<uint16_t*>(source));
        tft.pushImage(gbaOutX, gbaOutY, GBA_W, GBA_H,
                      const_cast<uint16_t*>(source));
    } else if (gbaStage) {
        const int sourceRowsPerStage = GBA_STAGE_ROWS / gbaScale;
        for (int sourceY = 0; sourceY < GBA_H; sourceY += sourceRowsPerStage) {
            const int sourceRows = min(sourceRowsPerStage, GBA_H - sourceY);
            const int outputRows = sourceRows * gbaScale;
            for (int localY = 0; localY < sourceRows; ++localY) {
                const uint16_t* sourceRow = source +
                    static_cast<size_t>(sourceY + localY) * GBA_W;
                for (int duplicateY = 0; duplicateY < gbaScale; ++duplicateY) {
                    uint16_t* output = gbaStage +
                        static_cast<size_t>(localY * gbaScale + duplicateY) * gbaOutW;
                    for (int x = 0; x < GBA_W; ++x) {
                        const uint16_t color = sourceRow[x];
                        const int outputX = x * gbaScale;
                        for (int duplicateX = 0; duplicateX < gbaScale; ++duplicateX) {
                            output[outputX + duplicateX] = color;
                        }
                    }
                }
            }
            const int outputY = sourceY * gbaScale;
            frameSprite.pushImage(gbaOutX, gbaOutY + outputY,
                                  gbaOutW, outputRows, gbaStage);
            tft.pushImage(gbaOutX, gbaOutY + outputY,
                          gbaOutW, outputRows, gbaStage);
        }
    }

    frameSprite.setSwapBytes(oldFrameSwap);
    tft.setSwapBytes(oldPanelSwap);
    displayInvalidateShadow();
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
        } else if (x < 170 && y >= 260 && y < 540) {
            const int dx = x - 80;
            const int dy = y - 400;
            if (dx < -25) next |= GameBoyAdvanceHost::kLeft;
            if (dx > 25) next |= GameBoyAdvanceHost::kRight;
            if (dy < -35) next |= GameBoyAdvanceHost::kUp;
            if (dy > 35) next |= GameBoyAdvanceHost::kDown;
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
    if (item == GBA_MENU_VOLUME) {
        return String(radioGetVolume()) + "/" + String(RADIO_VOLUME_MAX);
    }
    return "";
}

static void gbaDrawMenu(int selected, const String& note) {
    const int rowH = 22;
    const int width = 280;
    const int left = (DISPLAY_WIDTH - width) / 2;
    const int top = (DISPLAY_HEIGHT - (GBA_MENU_COUNT * rowH + 72)) / 2;
    frameSprite.fillSprite(TFT_BLACK);
    frameSprite.drawRect(left - 10, top - 10, width + 20,
                         GBA_MENU_COUNT * rowH + 72, TFT_PINK);
    frameSprite.setTextDatum(TL_DATUM);
    frameSprite.setTextColor(TFT_PINK, TFT_BLACK);
    frameSprite.drawString("GAME BOY ADVANCE", left, top);
    frameSprite.drawFastHLine(left, top + 14, width, TFT_PINK);

    for (int item = 0; item < GBA_MENU_COUNT; ++item) {
        const int y = top + 26 + item * rowH;
        frameSprite.setTextColor(item == selected ? TFT_YELLOW : TFT_WHITE, TFT_BLACK);
        frameSprite.drawString(item == selected ? ">" : " ", left, y);
        const char* label = "";
        switch (item) {
            case GBA_MENU_DISPLAY: label = "Display"; break;
            case GBA_MENU_VOLUME: label = "Volume"; break;
            case GBA_MENU_SAVE_STATE: label = "Save state"; break;
            case GBA_MENU_LOAD_STATE: label = "Load state"; break;
            case GBA_MENU_RESUME: label = "Resume"; break;
            case GBA_MENU_QUIT: label = "Quit ROM"; break;
        }
        frameSprite.drawString(label, left + 14, y);
        const String value = gbaMenuValue(item);
        if (value.length()) {
            frameSprite.setTextDatum(TR_DATUM);
            frameSprite.setTextColor(TFT_CYAN, TFT_BLACK);
            frameSprite.drawString(value, left + width, y);
            frameSprite.setTextDatum(TL_DATUM);
        }
    }
    const int footY = top + 30 + GBA_MENU_COUNT * rowH;
    if (note.length()) {
        frameSprite.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
        frameSprite.drawString(note, left, footY);
    }
    frameSprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
    frameSprite.drawString("D-pad move/change  A ok  B/Menu back", left, footY + 16);
    gbaDrawTouchControls();
    displayInvalidateShadow();
    pushDisplayFrame();
}

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
    outLine("  Escape/touch MENU: display, volume, save/load state, quit.", C_CYAN);
    outLine("  States sit next to the ROM as <name>.gstate.", C_CYAN);
    outLine("  Controls: arrows/WASD, A/B, Enter, Backspace; touch adds L/R.", C_WHITE);
    outLine("  Quit: Ctrl+T, or choose Quit ROM from the menu.", C_WHITE);
}

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
        gbaScale = partCount >= 3 && gbaIsModeArg(parts[2]) ? gbaModeScale(parts[2]) : 3;
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
    const String saveVfs = gbSiblingPath(gbaRomVfs, ".sav");
    ledPulseStorageRead(romLogical.startsWith("/sd/"));

    if (!gbaHost.load(gbaRomVfs, saveVfs)) {
        const String status = gbaHost.status();
        gbaHost.stop();
        gbaFreeDisplay();
        outLine("gba: " + status, C_RED);
        return;
    }
    // Let the core reserve its bounded executable/hot-memory working set
    // before allocating an optional scale strip. The strip can fall back to
    // PSRAM; executable code cannot.
    if (!gbaSetupDisplay()) {
        gbaScale = 1;
        gbaSetupDisplay();
        outLine("gba: low memory, using native 1x", C_YELLOW);
    }

    bool audioUp = false;
    if (radioReleaseAudio()) audioUp = AudioOut::begin();
    if (!audioUp) outLine("gba: audio unavailable -- running silent", C_YELLOW);
    outLine("gba: launching " + romLogical + " at " + String(gbaScale) + "x -- Escape for menu", C_GREEN);
    drawDisplayFrame();

    slaveLinkSendLine("GAME 1");
    delay(20);
    gbaClearPanel();

    uint8_t legacyButtons = 0;
    uint16_t touchButtons = 0;
    constexpr uint32_t frameUs = 16743;  // 280896 GBA cycles at 16.777216 MHz
    uint32_t nextFrame = micros() + frameUs;
    const uint32_t startedMs = millis();
    uint32_t framesRun = 0;
    uint32_t framesDrawn = 0;
    uint64_t coreTimeUs = 0;
    uint64_t blitTimeUs = 0;
    uint32_t perfStartedUs = micros();
    uint32_t perfFrames = 0;
    uint32_t perfDraws = 0;
    int skipped = GBA_MAX_FRAME_SKIP;

    for (;;) {
        uint8_t events = gbPumpInput(legacyButtons);
        events |= gbaPumpTouch(touchButtons);
        if (events & GB_EVT_QUIT) break;
        if (events & GB_EVT_MENU) {
            displayInvalidateShadow();
            if (gbaRunMenu(legacyButtons, touchButtons)) break;
            gbaClearPanel();
            nextFrame = micros() + frameUs;
            skipped = GBA_MAX_FRAME_SKIP;
        }

        const bool late = static_cast<int32_t>(micros() - nextFrame) > 0;
        const bool draw = !late || skipped >= GBA_MAX_FRAME_SKIP;
        gbaHost.setButtons(static_cast<uint16_t>(legacyButtons) | touchButtons);
        const uint32_t coreStartedUs = micros();
        gbaHost.runFrame(draw);
        coreTimeUs += static_cast<uint32_t>(micros() - coreStartedUs);
        ++perfFrames;
        if (draw) {
            const uint32_t blitStartedUs = micros();
            gbaBlitFrame();
            blitTimeUs += static_cast<uint32_t>(micros() - blitStartedUs);
            ++framesDrawn;
            ++perfDraws;
            skipped = 0;
        } else {
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
            doll_gba_perf_stats_t coreStats = {};
            doll_gba_core_get_perf(&coreStats);
            Serial.printf("[gba perf] mode=%dx emu=%lu.%lu drawn=%lu.%lu core=%lluus blit=%lluus jit=%luK hit=%lu miss=%lu build=%lu rom=%lu+%lu\n",
                          gbaScale,
                          static_cast<unsigned long>(emuFps10 / 10),
                          static_cast<unsigned long>(emuFps10 % 10),
                          static_cast<unsigned long>(drawFps10 / 10),
                          static_cast<unsigned long>(drawFps10 % 10),
                          coreTimeUs / perfFrames,
                          perfDraws ? blitTimeUs / perfDraws : 0,
                          static_cast<unsigned long>(coreStats.jit_bytes / 1024),
                          static_cast<unsigned long>(coreStats.jit_hits),
                          static_cast<unsigned long>(coreStats.jit_misses),
                          static_cast<unsigned long>(coreStats.jit_compiles),
                          static_cast<unsigned long>(coreStats.rom_page_loads),
                          static_cast<unsigned long>(coreStats.rom_page_prefetches));
            Serial.flush();
            coreTimeUs = 0;
            blitTimeUs = 0;
            perfFrames = 0;
            perfDraws = 0;
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
        outLine("gba: " + String(framesRun * 1000.0f / ranMs, 1) +
                " fps emulated, " + String(framesDrawn * 1000.0f / ranMs, 1) +
                " fps drawn", C_CYAN);
    }
    displayDirty = true;
    displayInvalidateShadow();
    drawDisplayFrame();
    printPrompt();
}
