//   Gameboy.ino
//   "gb" app -- runs the vendored gnuboy Game Boy / Game Boy Color core
//   (src/emulator/gnuboy, GPLv2, ported from github.com/dkyazzentwatwa/cube-boy)
//   as a full-screen takeover on this board's TFT panel, driven by the BLE
//   keyboard through DS-Slave's gamepad mode.
//
//   This is the one place in DOLL-OS that does NOT follow the telnet-mirror model:
//   an emulator is a real-time framebuffer + low-latency input, so `gb <rom>`
//   seizes loop() (it runs its own inner loop until the player quits), draws
//   the 160x144 GB frame through the active panel's fastest supported path,
//   and reads raw button events off the keyboard link instead of the line
//   editor. On exit it hands control cleanly back to the shell.
//
//   Input: DOLL-OS tells DS-Slave to enter gamepad mode ("GAME 1"). In that mode the
//   slave stops sending ASCII and instead sends 2-byte button events:
//       0xF0 <bit>   button DOWN (bit = a GB_PAD_* bit, 0x01..0x80)
//       0xF1 <bit>   button UP
//       0xF2         QUIT (Ctrl+T on the keyboard) -- leave the emulator
//       0xF3         MENU (Escape) -- open the settings menu below
//   We hold a live button bitmap, so holding a key = held button (the whole
//   reason gamepad mode exists; the normal keystroke path is edge-only and
//   can't express "still held"). See ../DS-Slave/DS-Slave.ino.
//
//   Menu: Escape pauses the game and puts a modal settings menu on the panel --
//   display mode (fit/1x, switchable live), volume, save/load state, resume,
//   quit. It reuses the same button events for navigation, so it needs no
//   extra keys.
//
//   Speed: the frame loop's job is to run gnuboy exactly 59.7 times a second --
//   the game's whole clock, music included, is derived from that, so falling
//   behind doesn't drop frames, it plays the game in slow motion. The panel
//   can't be fed that fast (see the loop's frame-skip note), so drawing is what
//   gets dropped, not emulation. `gb` prints both rates on exit.
//
//   Audio: the APU plays through the onboard ES8311 + speaker (src/AudioOut.*).
//   That codec normally belongs to Radio.ino, which holds both of the S3's I2S
//   controllers, so launching a game calls radioReleaseAudio() to take them
//   back and AudioOut::end() hands them over again on exit. If any of that
//   fails the game just runs silent -- audio is never a reason not to launch.
//   Volume is the shell's radio volume ("radio vol <0-21>"), read per frame.

#include "src/GameBoyHost.h"
#include "src/AudioOut.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <memory>

static GameBoyHost gbHost;

// TEMPORARY launch tracing -- delete this macro and every GB_TRACE() call once the
// black-screen-on-launch hang is pinned down. Serial only, flushed after every line,
// so the last line printed is the last step that completed.
#define GB_TRACE(msg) do { Serial.println("[gb-trace] " msg); Serial.flush(); } while (0)

// Output rectangle on the panel. "fit" scales 160x144 up to the tallest box
// that fits DISPLAY_HEIGHT while keeping aspect (letterboxed left/right); "1x"
// is native 160x144 centered (much smaller, but the cheapest to push -- use it
// if the scaled frame rate feels low). Chosen per-launch: `gb <rom> [1x|fit]`.
static const int GB_W = GameBoyHost::kWidth;    // 160
static const int GB_H = GameBoyHost::kHeight;   // 144

static uint16_t* gbScaleBuf = nullptr;   // outW*outH RGB565, in PSRAM
static int16_t* gbColMap = nullptr;      // outW source-column lookup
static int16_t* gbRowMap = nullptr;      // outH source-row lookup
static int gbOutW = 0, gbOutH = 0, gbOutX = 0, gbOutY = 0;
static bool gbFitMode = true;

// How many frames in a row may go undrawn before we push one anyway -- a floor
// under the picture, not a target. The frame timer decides skipping normally, so
// in 1x mode (a 13.7ms push) this barely comes into play and nearly every frame
// is drawn. It only binds in fit mode, where a 38ms push plus ~3ms of emulation
// needs roughly one frame in four to leave any slack at all; at 2 the loop can
// just barely hold realtime, which is no margin for a heavier scene.
#ifdef FNK0104N_3P5_320x480_ST77922
static const int kMaxFrameSkip = 5;
#else
static const int kMaxFrameSkip = 3;
#endif
static const int kGbRomMenuMax = 128;
static const char* kGbRomDir = "/sd/gb";

static void gbFreeScale() {
    if (gbScaleBuf) { heap_caps_free(gbScaleBuf); gbScaleBuf = nullptr; }
    if (gbColMap)   { heap_caps_free(gbColMap);   gbColMap = nullptr; }
    if (gbRowMap)   { heap_caps_free(gbRowMap);   gbRowMap = nullptr; }
}

// Builds (or rebuilds) the scale buffers for the current mode. Returns false if
// PSRAM for the scaled frame couldn't be had -- caller falls back to 1x, which
// needs no scale buffer at all.
static bool gbSetupScale() {
#ifdef FNK0104N_3P5_320x480_ST77922
    // Use an exact 2x fit on N. It leaves a small letterbox, but avoids the
    // generic 355x320 resampler and produces a four-aligned native region.
    if (gbFitMode) {
        gbOutW = GB_W * 2;
        gbOutH = GB_H * 2;
    } else {
        gbOutW = GB_W;
        gbOutH = GB_H;
    }
    gbOutX = (DISPLAY_WIDTH - gbOutW) / 2;
    gbOutY = (DISPLAY_HEIGHT - gbOutH) / 2;

    gbColMap = (int16_t*)heap_caps_malloc(gbOutW * sizeof(int16_t), MALLOC_CAP_8BIT);
    gbRowMap = (int16_t*)heap_caps_malloc(gbOutH * sizeof(int16_t), MALLOC_CAP_8BIT);
    gbScaleBuf = (uint16_t*)heap_caps_malloc((size_t)gbOutW * gbOutH * sizeof(uint16_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!gbColMap || !gbRowMap || !gbScaleBuf) {
        gbFreeScale();
        return false;
    }
    for (int x = 0; x < gbOutW; x++) gbColMap[x] = (x * GB_W) / gbOutW;
    for (int y = 0; y < gbOutH; y++) gbRowMap[y] = (y * GB_H) / gbOutH;
    return true;
#else
    if (gbFitMode) {
        gbOutH = DISPLAY_HEIGHT;                 // fill the height
        gbOutW = (GB_W * DISPLAY_HEIGHT) / GB_H; // keep aspect (~266 on 320x240)
        if (gbOutW > DISPLAY_WIDTH) {            // never wider than the panel
            gbOutW = DISPLAY_WIDTH;
            gbOutH = (GB_H * DISPLAY_WIDTH) / GB_W;
        }
    } else {
        gbOutW = GB_W;
        gbOutH = GB_H;
    }
    gbOutX = (DISPLAY_WIDTH - gbOutW) / 2;
    gbOutY = (DISPLAY_HEIGHT - gbOutH) / 2;

    if (!gbFitMode) {
        return true;
    }

    GB_TRACE("  setupScale: about to malloc maps + scale buffer");
    gbColMap = (int16_t*)heap_caps_malloc(gbOutW * sizeof(int16_t), MALLOC_CAP_8BIT);
    gbRowMap = (int16_t*)heap_caps_malloc(gbOutH * sizeof(int16_t), MALLOC_CAP_8BIT);
    gbScaleBuf = (uint16_t*)heap_caps_malloc((size_t)gbOutW * gbOutH * sizeof(uint16_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    GB_TRACE("  setupScale: mallocs returned");
    if (!gbColMap || !gbRowMap || !gbScaleBuf) {
        gbFreeScale();
        return false;
    }
    for (int x = 0; x < gbOutW; x++) gbColMap[x] = (x * GB_W) / gbOutW;
    for (int y = 0; y < gbOutH; y++) gbRowMap[y] = (y * GB_H) / gbOutH;
    GB_TRACE("  setupScale: maps filled");
    return true;
#endif
}

// Pushes one emulator frame to the panel. gnuboy renders GB_PIXEL_565_LE (native
// little-endian uint16); the panel wants big-endian, so setSwapBytes(true) makes
// pushImage swap as it streams.
static void gbBlitFrame() {
    const uint16_t* frame = gbHost.frame();
    if (!frame) return;
#ifdef FNK0104N_3P5_320x480_ST77922
    // Render directly in native portrait order. Each destination row is
    // contiguous in PSRAM and can be handed straight to Freenove's rotation-0
    // region writer, avoiding a second full-buffer landscape transpose.
    for (int nativeRow = 0; nativeRow < gbOutW; nativeRow++) {
        const int srcX = gbColMap[nativeRow];
        uint16_t* dst = gbScaleBuf + (size_t)nativeRow * gbOutH;
        for (int nativeCol = 0; nativeCol < gbOutH; nativeCol++) {
            const int logicalY = gbOutH - nativeCol - 1;
            dst[nativeCol] = __builtin_bswap16(
                frame[(size_t)gbRowMap[logicalY] * GB_W + srcX]);
        }
    }
    tft_st77922.Fill_Colors(LCD_WIDTH - (gbOutY + gbOutH), gbOutX,
                            gbOutH, gbOutW, gbScaleBuf);
#else
    const uint16_t* pixels = frame;
    int width = GB_W;
    int height = GB_H;
    if (!gbFitMode) {
#ifdef FNK0104N_3P5_320x480_ST77922
        for (size_t i = 0; i < (size_t)GB_W * GB_H; i++) {
            gbScaleBuf[i] = __builtin_bswap16(frame[i]);
        }
        pixels = gbScaleBuf;
#else
        pixels = frame;
#endif
    } else {
        for (int oy = 0; oy < gbOutH; oy++) {
            const uint16_t* srcRow = frame + (int)gbRowMap[oy] * GB_W;
            uint16_t* dst = gbScaleBuf + (size_t)oy * gbOutW;
            for (int ox = 0; ox < gbOutW; ox++) {
                uint16_t color = srcRow[gbColMap[ox]];
#ifdef FNK0104N_3P5_320x480_ST77922
                color = __builtin_bswap16(color);
#endif
                dst[ox] = color;
            }
        }
        pixels = gbScaleBuf;
        width = gbOutW;
        height = gbOutH;
    }

#ifdef FNK0104N_3P5_320x480_ST77922
    tft_st77922.Fill_Colors_Landscape(gbOutX, gbOutY, width, height,
                                      const_cast<uint16_t*>(pixels));
#else
    tft.setSwapBytes(true);
    tft.pushImage(gbOutX, gbOutY, width, height,
                  const_cast<uint16_t*>(pixels));
#endif
#endif
}

static void gbClearPanel() {
#ifdef FNK0104N_3P5_320x480_ST77922
    frameSprite.fillSprite(TFT_BLACK);
    pushDisplayFrame();
#else
    tft.fillScreen(TFT_BLACK);
#endif
}

// One-shot requests the slave can raise alongside the held-button bitmap.
static const uint8_t GB_EVT_QUIT = 0x01;   // Ctrl+T
static const uint8_t GB_EVT_MENU = 0x02;   // Escape

// Drains every button event waiting on the keyboard link and folds it into the
// live bitmap. Returns the one-shot events seen during this drain (a mask, not a
// single event: a drain can span several reports and dropping one would lose a
// keypress). Reused DOLL-OS plumbing: keyboardReadRawByte() (KeyboardSerial.ino) hands
// back one raw slave byte or -1.
static uint8_t gbPumpInput(uint8_t& buttons) {
    static uint8_t phase = 0;   // 0 = idle, 1 = expect DOWN bit, 2 = expect UP bit
    uint8_t events = 0;
    int b;
    while ((b = keyboardReadRawByte()) >= 0) {
        uint8_t by = (uint8_t)b;
        // TEMPORARY: log the first 80 bytes the slave sends while a game runs, so a
        // dead-input report can be told apart from a link that is sending nothing.
        static int gbTraceBytes = 0;
        if (gbTraceBytes < 80) {
            gbTraceBytes++;
            Serial.printf("[gb-input] 0x%02X\n", by);
            Serial.flush();
        }
        if (phase == 1) { buttons |= by; phase = 0; continue; }
        if (phase == 2) { buttons &= ~by; phase = 0; continue; }
        if (by == 0xF0) { phase = 1; }
        else if (by == 0xF1) { phase = 2; }
        else if (by == 0xF2) { events |= GB_EVT_QUIT; }   // Ctrl+T in gamepad mode
        else if (by == 0xF3) { events |= GB_EVT_MENU; }   // Escape in gamepad mode
        else if (by == 0x14) {                  // fallback: raw Ctrl+T (DC4)
            events |= GB_EVT_QUIT;               // (slave not in game mode)
        }
        // any other stray ASCII byte is ignored while a game runs
    }
    return events;
}

// Turns a DOLL-OS logical path (absolute or relative to cwd) into a stdio/VFS path
// gnuboy's fopen can open. SD is mounted at "/sdcard" (Storage.ino), LittleFS at
// "/littlefs". Returns "" if the path routes to SD but no card is mounted.
static String gbVfsPath(const String& arg) {
    String resolved = resolvePath(cwd, arg);
    RoutedPath r = routePath(resolved);
    if (r.isSd) {
        if (!sdCardMounted) return "";
        return "/sdcard" + r.realPath;
    }
    return "/littlefs" + r.realPath;
}

// romVfs -> same directory/name with the extension swapped for `ext`. Used for
// the cart's battery SRAM (".sav", written continuously while playing) and for
// the menu's whole-machine snapshot (".gbs", written only when asked).
static String gbSiblingPath(const String& romVfs, const char* ext) {
    int dot = romVfs.lastIndexOf('.');
    int slash = romVfs.lastIndexOf('/');
    if (dot > slash) return romVfs.substring(0, dot) + ext;
    return romVfs + ext;
}

static String gbSavePath(const String& romVfs) { return gbSiblingPath(romVfs, ".sav"); }
static String gbStatePath(const String& romVfs) { return gbSiblingPath(romVfs, ".gbs"); }

static bool gbIsDisplayModeArg(const String& arg) {
    return arg == "1x" || arg == "fit";
}

static bool gbIsHelpArg(const String& arg) {
    return arg == "help" || arg == "-h" || arg == "--help" || arg == "?";
}

static bool gbIsRomFileName(String name) {
    name.toLowerCase();
    return name.endsWith(".gb") || name.endsWith(".gbc");
}

static String gbBaseName(String path) {
    int slash = path.lastIndexOf('/');
    if (slash >= 0) return path.substring(slash + 1);
    return path;
}

static String gbJoinChildPath(const String& parent, const String& child) {
    if (parent.length() == 0 || parent == "/") return "/" + child;
    return parent + "/" + child;
}

static String gbJoinRelativePath(const String& parent, const String& child) {
    if (parent.length() == 0) return child;
    return parent + "/" + child;
}

static bool gbNameLess(const String& a, const String& b) {
    String al = a;
    String bl = b;
    al.toLowerCase();
    bl.toLowerCase();
    return al < bl;
}

static void gbInsertRomName(String names[], int& count, const String& name, bool& truncated) {
    if (count >= kGbRomMenuMax) {
        truncated = true;
        return;
    }

    int insertAt = count;
    while (insertAt > 0 && gbNameLess(name, names[insertAt - 1])) {
        names[insertAt] = names[insertAt - 1];
        insertAt--;
    }
    names[insertAt] = name;
    count++;
}

static void gbCollectRomNamesInDir(fs::FS& fs, const String& realDir, const String& relativeDir,
                                   String names[], int& count, bool& truncated) {
    ledPulseStorageRead(true);
    File dir = fs.open(realDir);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
    }

    File entry = dir.openNextFile();
    while (entry) {
        ledPulseStorageRead(true);
        String name = gbBaseName(entry.name());
        bool isDir = entry.isDirectory();
        entry.close();
        if (name.length() > 0) {
            if (isDir) {
                gbCollectRomNamesInDir(fs, gbJoinChildPath(realDir, name),
                                       gbJoinRelativePath(relativeDir, name),
                                       names, count, truncated);
            } else if (gbIsRomFileName(name)) {
                gbInsertRomName(names, count, gbJoinRelativePath(relativeDir, name), truncated);
            }
        }
        entry = dir.openNextFile();
    }
    dir.close();
}

static int gbCollectRomNames(String names[], bool& truncated) {
    truncated = false;
    int count = 0;
    if (!sdCardMounted) return 0;

    RoutedPath r = routePath(kGbRomDir);
    gbCollectRomNamesInDir(*r.fs, r.realPath, "", names, count, truncated);
    return count;
}

static String gbFitMenuText(String text, int maxWidth) {
    if (frameSprite.textWidth(text) <= maxWidth) return text;
    if (maxWidth <= 0) return "";

    while (text.length() > 1 && frameSprite.textWidth(text + "~") > maxWidth) {
        text.remove(text.length() - 1);
    }
    return text + "~";
}

static void gbDrawRomMenu(String names[], int count, int selected, bool truncated) {
    const int rowH = 18;
    const int top = 28;
    const int left = 14;
    const int width = DISPLAY_WIDTH - left * 2;
    const int listTop = top + 28;
    const int footY = DISPLAY_HEIGHT - 30;
    int visibleRows = (footY - listTop - 4) / rowH;
    if (visibleRows < 3) visibleRows = 3;
    if (visibleRows > count) visibleRows = count;

    int first = selected - visibleRows / 2;
    if (first < 0) first = 0;
    if (first + visibleRows > count) first = count - visibleRows;
    if (first < 0) first = 0;

    frameSprite.fillSprite(TFT_BLACK);
    frameSprite.setTextDatum(TL_DATUM);
    frameSprite.setTextColor(TFT_PINK, TFT_BLACK);
    frameSprite.drawString("GAME BOY ROMS", left, top);

    frameSprite.setTextDatum(TR_DATUM);
    frameSprite.setTextColor(TFT_CYAN, TFT_BLACK);
    frameSprite.drawString(String(selected + 1) + "/" + String(count), DISPLAY_WIDTH - left, top);
    frameSprite.setTextDatum(TL_DATUM);

    frameSprite.drawFastHLine(left, top + 14, width, TFT_PINK);

    for (int row = 0; row < visibleRows; row++) {
        const int idx = first + row;
        const int y = listTop + row * rowH;
        const bool on = idx == selected;
        frameSprite.setTextColor(on ? TFT_YELLOW : TFT_WHITE, TFT_BLACK);
        frameSprite.drawString(on ? ">" : " ", left, y);
        frameSprite.drawString(gbFitMenuText(names[idx], width - 18), left + 12, y);
    }

    frameSprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
    if (first > 0) frameSprite.drawString("^ more", left, listTop - 12);
    if (first + visibleRows < count) frameSprite.drawString("v more", left, footY - 14);

    frameSprite.setTextColor(truncated ? TFT_YELLOW : TFT_DARKGREY, TFT_BLACK);
    frameSprite.drawString(truncated ? "showing first 128 ROMs" : "W/S move  N/Enter ok  M/Esc cancel",
                           left, footY);
    if (truncated) {
        frameSprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
        frameSprite.drawString("W/S move  N/Enter ok  M/Esc cancel", left, footY + 12);
    }
    frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    pushDisplayFrame();
}

enum GbTelnetEscState : uint8_t {
    GB_TELNET_NORMAL,
    GB_TELNET_ESC,
    GB_TELNET_CSI,
};

static GbTelnetEscState gbTelnetEscState = GB_TELNET_NORMAL;
static uint32_t gbTelnetEscAtMs = 0;

static void gbResetTelnetMenuInput() {
    gbTelnetEscState = GB_TELNET_NORMAL;
    gbTelnetEscAtMs = 0;
}

static uint8_t gbPumpTelnetMenuInput(uint8_t& pressed) {
    uint8_t events = 0;
    pressed = 0;

    if (gbTelnetEscState == GB_TELNET_ESC && millis() - gbTelnetEscAtMs > 40) {
        events |= GB_EVT_MENU;
        gbTelnetEscState = GB_TELNET_NORMAL;
    }

    int b;
    while ((b = telnetReadFilteredByte()) >= 0) {
        uint8_t by = (uint8_t)b;
        if (gbTelnetEscState == GB_TELNET_ESC) {
            if (by == '[') {
                gbTelnetEscState = GB_TELNET_CSI;
            } else {
                events |= GB_EVT_MENU;
                gbTelnetEscState = GB_TELNET_NORMAL;
            }
            continue;
        }
        if (gbTelnetEscState == GB_TELNET_CSI) {
            if (by == 'A') pressed |= GameBoyHost::kUp;
            else if (by == 'B') pressed |= GameBoyHost::kDown;
            else if (by == 'C') pressed |= GameBoyHost::kRight;
            else if (by == 'D') pressed |= GameBoyHost::kLeft;
            if (by >= 0x40 && by <= 0x7E) gbTelnetEscState = GB_TELNET_NORMAL;
            continue;
        }

        if (by == 0x14 || by == 0x03) {   // Ctrl+T or Ctrl+C
            events |= GB_EVT_QUIT;
        } else if (by == 0x1B) {
            gbTelnetEscState = GB_TELNET_ESC;
            gbTelnetEscAtMs = millis();
        } else if (by == '\r' || by == '\n') {
            pressed |= GameBoyHost::kStart;
        } else if (by == 'w' || by == 'W') {
            pressed |= GameBoyHost::kUp;
        } else if (by == 's' || by == 'S') {
            pressed |= GameBoyHost::kDown;
        } else if (by == 'a' || by == 'A') {
            pressed |= GameBoyHost::kLeft;
        } else if (by == 'd' || by == 'D') {
            pressed |= GameBoyHost::kRight;
        } else if (by == 'n' || by == 'N') {
            pressed |= GameBoyHost::kA;
        } else if (by == 'm' || by == 'M') {
            pressed |= GameBoyHost::kB;
        }
    }
    return events;
}

static bool gbPickRom(String& romLogical) {
    if (!sdCardMounted) {
        outLine("gb: SD not mounted (insert card and reboot)", C_RED);
        return false;
    }

    //   Heap-allocated rather than a function-local static: at 128 Strings the array
    //   itself is 2KB that used to stay resident in internal SRAM for the whole uptime,
    //   plus one small buffer per ROM name. Over the 512-byte extmem threshold the
    //   array lands in PSRAM (see enablePsramHeap), and everything it owns is released
    //   when the picker closes.
    std::unique_ptr<String[]> names(new String[kGbRomMenuMax]);
    bool truncated = false;
    int count = gbCollectRomNames(names.get(), truncated);
    if (count == 0) {
        outLine("gb: no .gb/.gbc files found in /sd/gb", C_YELLOW);
        return false;
    }

    outLine("gb: choose a ROM from /sd/gb", C_CYAN);
    drawDisplayFrame();

    slaveLinkSendLine("GAME 1");
    delay(20);
    gbResetTelnetMenuInput();

    uint8_t buttons = 0;
    uint8_t prev = 0;
    int selected = 0;
    bool redraw = true;

    for (;;) {
        if (redraw) {
            gbDrawRomMenu(names.get(), count, selected, truncated);
            redraw = false;
        }

        uint8_t telnetPressed = 0;
        uint8_t events = gbPumpInput(buttons);
        events |= gbPumpTelnetMenuInput(telnetPressed);
        if (events & (GB_EVT_QUIT | GB_EVT_MENU)) {
            slaveLinkSendLine("GAME 0");
            outLine("gb: ROM selection cancelled", C_YELLOW);
            return false;
        }

        uint8_t pressed = (buttons & ~prev) | telnetPressed;
        prev = buttons;
        if (pressed == 0) {
            delay(15);
            continue;
        }

        if (pressed & GameBoyHost::kUp) {
            selected = (selected + count - 1) % count;
            redraw = true;
            continue;
        }
        if (pressed & GameBoyHost::kDown) {
            selected = (selected + 1) % count;
            redraw = true;
            continue;
        }
        if (pressed & GameBoyHost::kLeft) {
            selected -= 10;
            if (selected < 0) selected = 0;
            redraw = true;
            continue;
        }
        if (pressed & GameBoyHost::kRight) {
            selected += 10;
            if (selected >= count) selected = count - 1;
            redraw = true;
            continue;
        }
        if (pressed & GameBoyHost::kB) {
            slaveLinkSendLine("GAME 0");
            outLine("gb: ROM selection cancelled", C_YELLOW);
            return false;
        }
        if (pressed & (GameBoyHost::kA | GameBoyHost::kStart)) {
            slaveLinkSendLine("GAME 0");
            romLogical = String(kGbRomDir) + "/" + names[selected];
            return true;
        }
    }
}

static void gbPrintUsage() {
    outLine("Usage: gb [rom.gb|.gbc] [1x|fit]", C_CYAN);
    outLine("  Bare 'gb' opens the /sd/gb ROM picker.", C_CYAN);
    outLine("  ROM path is a normal DOLL-OS path (e.g. /sd/roms/zelda.gb).", C_CYAN);
    outLine("  fit (default) fills the panel but costs ~38ms of SPI per push,", C_CYAN);
    outLine("  so most frames go undrawn; 1x is small but near-smooth.", C_CYAN);
    outLine("  Controls (BLE keyboard via slave): WASD/arrows=D-pad, N=A,", C_CYAN);
    outLine("  M=B, Enter=Start, \\=Select, Ctrl+T=quit.", C_CYAN);
    outLine("  Xbox pad via slave: stick/d-pad, A=A, B=B, Menu=Start,", C_CYAN);
    outLine("  View=Select, LB=this menu, LB+RB=quit.", C_CYAN);
    outLine("  Esc opens the settings menu: display mode, volume, save/load", C_CYAN);
    outLine("  state, resume, quit. States sit next to the ROM as <name>.gbs.", C_CYAN);
    outLine("  Volume is shared with the radio ('radio vol <0-21>').", C_CYAN);
}

//---------------------------------------------------------------------------
//   Settings menu (Escape while a game runs)
//
//   Modal and drawn over the whole panel: emulation is paused for as long as
//   it's up, so nothing here has to be re-entrant or fast. It borrows the
//   shell's frameSprite (Display.ino) rather than allocating its own -- the
//   sprite's contents are rebuilt from scratch by drawDisplayFrame() whenever
//   displayDirty is set, which handleGbCommand already does on the way out, so
//   scribbling on it costs nothing.

static String gbRomVfs;   // set at launch; the menu needs it for state paths

enum GbMenuItem : uint8_t {
    GB_MENU_DISPLAY,
    GB_MENU_VOLUME,
    GB_MENU_SAVE_STATE,
    GB_MENU_LOAD_STATE,
    GB_MENU_RESUME,
    GB_MENU_QUIT,
    GB_MENU_COUNT,
};

// The right-hand column for the rows that carry a setting; "" for the rows that
// are just actions.
static String gbMenuValue(int item) {
    if (item == GB_MENU_DISPLAY) return gbFitMode ? "fit" : "1x";
    if (item == GB_MENU_VOLUME) {
        return String(radioGetVolume()) + "/" + String(RADIO_VOLUME_MAX);
    }
    return "";
}

// Swaps between fit and 1x while a ROM is running: frees the old scale buffers
// and builds the new ones. Falls back to 1x if the PSRAM for a scaled frame
// can't be had, same as launch does.
static void gbSetDisplayMode(bool fit) {
    gbFreeScale();
    gbFitMode = fit;
    if (!gbSetupScale()) {
        gbFitMode = false;
        gbSetupScale();
    }
}

static void gbDrawMenu(int selected, const String& note) {
    const int rowH = 18;
    const int top = (DISPLAY_HEIGHT - (GB_MENU_COUNT * rowH + 60)) / 2;
    const int left = DISPLAY_WIDTH / 2 - 110;
    const int width = 220;

    frameSprite.fillSprite(TFT_BLACK);
    frameSprite.drawRect(left - 8, top - 8, width + 16,
                         GB_MENU_COUNT * rowH + 60, TFT_PINK);

    frameSprite.setTextDatum(TL_DATUM);
    frameSprite.setTextColor(TFT_PINK, TFT_BLACK);
    frameSprite.drawString("GAME BOY", left, top);
    frameSprite.drawFastHLine(left, top + 12, width, TFT_PINK);

    for (int i = 0; i < GB_MENU_COUNT; i++) {
        const int y = top + 22 + i * rowH;
        const bool on = (i == selected);
        frameSprite.setTextColor(on ? TFT_YELLOW : TFT_WHITE, TFT_BLACK);
        frameSprite.drawString(on ? ">" : " ", left, y);

        const char* label = "";
        switch (i) {
            case GB_MENU_DISPLAY:    label = "Display";    break;
            case GB_MENU_VOLUME:     label = "Volume";     break;
            case GB_MENU_SAVE_STATE: label = "Save state"; break;
            case GB_MENU_LOAD_STATE: label = "Load state"; break;
            case GB_MENU_RESUME:     label = "Resume";     break;
            case GB_MENU_QUIT:       label = "Quit ROM";   break;
        }
        frameSprite.drawString(label, left + 12, y);

        const String value = gbMenuValue(i);
        if (value.length() > 0) {
            frameSprite.setTextDatum(TR_DATUM);
            frameSprite.setTextColor(TFT_CYAN, TFT_BLACK);
            frameSprite.drawString(value, left + width, y);
            frameSprite.setTextDatum(TL_DATUM);
        }
    }

    const int footY = top + 22 + GB_MENU_COUNT * rowH + 4;
    if (note.length() > 0) {
        frameSprite.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
        frameSprite.drawString(note, left, footY);
    }
    frameSprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
    frameSprite.drawString("W/S move  A/D change  N ok  Esc back", left, footY + 12);
    frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    pushDisplayFrame();
}

// Runs the menu until the player leaves it. Returns true if they chose to quit
// the ROM. `buttons` is kept live throughout (the slave keeps sending DOWN/UP
// while we're here), so the game doesn't inherit a stuck key on resume.
static bool gbRunMenu(uint8_t& buttons) {
    int selected = 0;
    String note;
    // Seed the edge detector with what's already held, so the keypress that
    // opened the menu -- or a D-pad direction the player hadn't let go of --
    // doesn't immediately move the cursor.
    uint8_t prev = buttons;
    bool redraw = true;

    for (;;) {
        if (redraw) {
            gbDrawMenu(selected, note);
            redraw = false;
        }

        const uint8_t events = gbPumpInput(buttons);
        if (events & GB_EVT_QUIT) return true;
        if (events & GB_EVT_MENU) return false;   // Escape closes what Escape opened

        const uint8_t pressed = buttons & ~prev;
        prev = buttons;
        if (pressed == 0) {
            delay(15);   // nothing to do; yield so IDLE runs and feeds the WDT
            continue;
        }
        redraw = true;

        if (pressed & GameBoyHost::kUp) {
            selected = (selected + GB_MENU_COUNT - 1) % GB_MENU_COUNT;
            continue;
        }
        if (pressed & GameBoyHost::kDown) {
            selected = (selected + 1) % GB_MENU_COUNT;
            continue;
        }
        if (pressed & GameBoyHost::kB) return false;   // B backs out, as it should

        const bool left = (pressed & GameBoyHost::kLeft) != 0;
        const bool right = (pressed & GameBoyHost::kRight) != 0;
        const bool ok = (pressed & (GameBoyHost::kA | GameBoyHost::kStart)) != 0;
        if (!left && !right && !ok) continue;

        switch (selected) {
            case GB_MENU_DISPLAY:
                // Left/right and A all just flip it -- there are only two modes.
                gbSetDisplayMode(!gbFitMode);
                note = gbFitMode ? "fit: fills the panel, most frames skipped"
                                 : "1x: small but near-smooth";
                break;
            case GB_MENU_VOLUME:
                // The same level the shell's "radio vol" and Ctrl+Up/Down set --
                // AudioOut::onSamples reads it per frame, so this is live the
                // moment the game resumes. A steps up and wraps round to mute,
                // so the row is usable without hunting for left/right.
                if (left) {
                    radioAdjustVolume(-1);
                } else if (right) {
                    radioAdjustVolume(1);
                } else if (radioGetVolume() >= RADIO_VOLUME_MAX) {
                    radioAdjustVolume(-RADIO_VOLUME_MAX);
                } else {
                    radioAdjustVolume(1);
                }
                note = "shared with the radio";
                break;
            case GB_MENU_SAVE_STATE:
                if (!ok) break;
                note = gbHost.saveState(gbStatePath(gbRomVfs).c_str())
                           ? "state saved" : "save failed (card full or read-only?)";
                break;
            case GB_MENU_LOAD_STATE:
                if (!ok) break;
                // A failed load can leave the machine half-restored, so say so
                // plainly rather than dropping the player back into a ROM that
                // may be in an inconsistent state.
                note = gbHost.loadState(gbStatePath(gbRomVfs).c_str())
                           ? "state loaded" : "load failed (no state saved yet?)";
                break;
            case GB_MENU_RESUME:
                if (ok) return false;
                break;
            case GB_MENU_QUIT:
                if (ok) return true;
                break;
        }
    }
}

void handleGbCommand(const String parts[], int partCount) {
    String romLogical;
    if (partCount >= 2 && gbIsHelpArg(parts[1])) {
        gbPrintUsage();
        return;
    }
    if (partCount < 2 || (partCount == 2 && gbIsDisplayModeArg(parts[1]))) {
        gbFitMode = !(partCount == 2 && parts[1] == "1x");
        if (!gbPickRom(romLogical)) return;
    } else {
        romLogical = parts[1];
        gbFitMode = !(partCount >= 3 && parts[2] == "1x");
    }

    String romVfs = gbVfsPath(romLogical);
    gbRomVfs = romVfs;   // the menu builds its state path from this
    if (romVfs.isEmpty()) {
        outLine("gb: SD not mounted (insert card and reboot)", C_RED);
        return;
    }
    ledPulseStorageRead(romLogical.startsWith("/sd/"));

    if (!gbHost.begin()) {
        outLine("gb: " + gbHost.status(), C_RED);
        return;
    }
    GB_TRACE("gbHost.begin() ok, about to load ROM");
    if (!gbHost.load(romVfs, gbSavePath(romVfs))) {
        outLine("gb: " + gbHost.status(), C_RED);
        outLine("gb: check the path -- " + romVfs, C_YELLOW);
        return;
    }
    GB_TRACE("ROM loaded, about to gbSetupScale");
    Serial.printf("[gb-trace] free: internal=%u psram=%u largest-psram=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    Serial.flush();

    if (!gbSetupScale()) {
        // Couldn't get the fitted frame buffer -- native 1x needs none on SPI
        // panels and only a much smaller byte-order buffer on the QSPI panel.
        gbFitMode = false;
        if (!gbSetupScale()) {
            gbHost.stop();
            outLine("gb: not enough PSRAM for the display buffer", C_RED);
            return;
        }
        outLine("gb: low memory, running native 1x", C_YELLOW);
    }

    // Take the codec off the radio (which otherwise holds both I2S controllers)
    // and bring up our own TX channel. Both steps are advisory: a game with no
    // sound still beats no game.
    GB_TRACE("gbSetupScale done, about to radioReleaseAudio");
    bool audioUp = false;
    if (radioReleaseAudio()) {
        GB_TRACE("radioReleaseAudio ok, about to AudioOut::begin");
        audioUp = AudioOut::begin();
        GB_TRACE("AudioOut::begin returned");
    } else {
        GB_TRACE("radioReleaseAudio returned false (radio kept the I2S)");
    }
    if (!audioUp) {
        outLine("gb: audio unavailable -- running silent", C_YELLOW);
    }

    GB_TRACE("audio done, about to outLine launch");
    outLine("gb: launching " + romLogical + " -- Ctrl+T to quit", C_GREEN);
    GB_TRACE("outLine done, about to drawDisplayFrame");
    drawDisplayFrame();   // flush that line to the panel before we take it over
    GB_TRACE("drawDisplayFrame done, about to send GAME 1");

    // Hand the panel and the keyboard over to the game.
    slaveLinkSendLine("GAME 1");   // DS-Slave: emit raw button events, not ASCII
    delay(20);
    GB_TRACE("GAME 1 sent, about to clear panel");
    gbClearPanel();
    GB_TRACE("panel cleared, entering frame loop");
    Serial.printf("[gb-trace] fit=%d out=%dx%d at %d,%d scaleBuf=%p audioUp=%d\n",
                  (int)gbFitMode, gbOutW, gbOutH, gbOutX, gbOutY,
                  (void*)gbScaleBuf, (int)audioUp);
    Serial.flush();

    uint8_t buttons = 0;
    const uint32_t frameUs = 16743;   // ~59.7 Hz, true GB frame period
    uint32_t nextFrame = micros() + frameUs;   // deadline for the frame about to run
    uint32_t framesRun = 0, framesDrawn = 0;
    int skipRun = 0;
    const uint32_t startedMs = millis();

    for (;;) {
        const uint8_t events = gbPumpInput(buttons);
        if (events & GB_EVT_QUIT) break;
        if (events & GB_EVT_MENU) {
            // Modal: emulation is paused for the duration. Audio goes quiet on
            // its own once the DMA ring drains -- nothing is producing samples.
#ifdef FNK0104N_3P5_320x480_ST77922
            // Game frames bypass frameSprite, so force the menu's first push to
            // replace every game pixel rather than trusting the shell shadow.
            displayInvalidateShadow();
#endif
            if (gbRunMenu(buttons)) break;
            gbClearPanel();                   // clear the menu and any old letterbox
            nextFrame = micros() + frameUs;   // menu time isn't the emulator falling behind
            skipRun = kMaxFrameSkip;          // and draw the frame after it, whatever the clock says
        }

        // Emulation must hold realtime -- the game's whole sense of time, music
        // included, is "one gnuboy_run per 16743us", so a slow loop doesn't just
        // drop frames, it plays Pokemon in slow motion. This panel can't hold it:
        // a fit-mode frame is 266x240x16b = 1.02Mbit, which at TFT_eSPI's 27MHz
        // (User_Setup.h) is ~38ms on the wire -- more than two frame periods
        // before a single opcode is emulated.
        //
        // So the emulator runs every frame and the *display* drops them when
        // we're late. A skipped frame is nearly free: gnuboy_run(false) turns
        // off scanline rendering as well, so all that's left is the CPU/APU,
        // which is a small fraction of the budget. The cap on consecutive skips
        // keeps a badly-behind board animating rather than freezing.
        const bool late = (int32_t)(micros() - nextFrame) > 0;
        const bool draw = !late || skipRun >= kMaxFrameSkip;

        // TEMPORARY: first few frames in detail, then a heartbeat every 30 frames
        // (~0.5s) so the log shows how far the loop actually gets.
        const bool gbTraceFrame = framesRun < 3 || (framesRun % 30) == 0;
        if (gbTraceFrame) {
            Serial.printf("[gb-trace] frame %u: setButtons/runFrame(draw=%d)\n",
                          (unsigned)framesRun, (int)draw);
            Serial.flush();
        }

        gbHost.setButtons(buttons);
        gbHost.runFrame(draw);
        if (gbTraceFrame) { GB_TRACE("  runFrame returned"); }
        if (draw) {
            gbBlitFrame();
            if (gbTraceFrame) { GB_TRACE("  gbBlitFrame returned"); }
            framesDrawn++;
            skipRun = 0;
        } else {
            skipRun++;
        }
        gbHost.tickSave();
        if (gbTraceFrame) { GB_TRACE("  tickSave returned"); }
        ledService();
        framesRun++;
        if (gbTraceFrame) { GB_TRACE("  frame complete"); }

        // Pace to ~59.7 fps when we're ahead; if we've fallen more than a few
        // frames behind, give up on that time rather than sprinting after it
        // forever. Either branch must yield: delay()/vTaskDelay() is what lets
        // CPU 1's IDLE task run and feed the task watchdog, and a sustained run
        // of behind-schedule frames used to hit neither, spinning loopTask long
        // enough to trip the ~5s WDT (see Storage.ino's setTaskWdtTimeout).
        nextFrame += frameUs;
        int32_t remaining = (int32_t)(nextFrame - micros());
        if (remaining > 1000) {
            delay(remaining / 1000);
        } else {
            if (remaining < -(int32_t)(kMaxFrameSkip * frameUs)) {
                nextFrame = micros() + frameUs;   // hopelessly behind; resync
            }
            vTaskDelay(1);   // still yield so IDLE runs and feeds the WDT
        }
    }
    const uint32_t ranMs = millis() - startedMs;

    // Shut the game down and give everything back to the shell.
    AudioOut::setDiscard(true);   // stop feeding I2S before the channel goes away
    gbHost.stop();                // flushes SRAM to the .sav
    AudioOut::end();              // releases the I2S controller for the next "radio play"
    gbFreeScale();
    slaveLinkSendLine("GAME 0");   // DS-Slave: back to normal keystroke mode

    outLine("gb: " + gbHost.status(), C_GREEN);
    // Speed report, because "is it running slow?" is otherwise guesswork and it
    // is the thing that makes the audio sound wrong. Emulated should sit at ~59.7
    // -- if it doesn't, the board couldn't keep up and the game really did run
    // slow. Drawn is whatever the panel managed. Audio drops mean the codec ran
    // out of samples (emulation behind) or had nowhere to put them (ahead).
    if (ranMs > 0) {
        uint32_t pushed = 0, dropped = 0, underruns = 0;
        AudioOut::stats(pushed, dropped, underruns);
        outLine("gb: " + String(framesRun * 1000.0f / ranMs, 1) + " fps emulated, "
                + String(framesDrawn * 1000.0f / ranMs, 1) + " fps drawn"
                + (audioUp ? (", " + String(dropped) + " audio samples dropped in "
                              + String(underruns) + " gaps") : ""), C_CYAN);
    }
    displayDirty = true;         // force a full shell repaint over the game frame
    displayInvalidateShadow();   // game frames bypass the shell's panel shadow
    drawDisplayFrame();
    printPrompt();
}
