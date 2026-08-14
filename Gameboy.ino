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
//   Audio: the APU plays through the onboard ES8388 + speaker (src/AudioOut.*).
//   That codec normally belongs to Radio.ino, which holds the active I2S TX
//   controller, so launching a game calls radioReleaseAudio() to take it
//   back and AudioOut::end() hands them over again on exit. If any of that
//   fails the game just runs silent -- audio is never a reason not to launch.
//   Volume is the shell's radio volume ("radio vol <0-21>"), read per frame.

#include "src/GameBoyHost.h"
#include "src/AudioOut.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static GameBoyHost gbHost;

// Output rectangle on the panel. "fit" selects the board's largest safe scale
// (a centered integer 3x on Tab5); "1x" is native 160x144 centered. Chosen
// per-launch: `gb <rom> [1x|fit]`.
static const int GB_W = GameBoyHost::kWidth;    // 160
static const int GB_H = GameBoyHost::kHeight;   // 144
#if defined(DOLL_BOARD_TAB5)
// Match the shell's safe per-transaction byte range while avoiding hundreds of
// tiny cache-writeback transactions. 480 x 24 x 2 = 23 KB per fit-mode strip.
static const int GB_TAB5_STAGE_ROWS = 24;
#endif

static uint16_t* gbScaleBuf = nullptr;   // scaled RGB565 frame, or a Tab5 row strip
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
#if defined(DOLL_BOARD_TAB5)
static const int kMaxFrameSkip = 5;
#elif defined(FNK0104N_3P5_320x480_ST77922)
static const int kMaxFrameSkip = 5;
#else
static const int kMaxFrameSkip = 3;
#endif
static const int kGbRomMenuMax = 128;
static const char* kGbRomDir = "/sd/gb";

#if defined(DOLL_BOARD_TAB5)
// Compact shell launcher. It lives entirely in the 32-pixel status bar, so it
// never steals terminal space. Its y range deliberately ends before the ROM
// picker's top-right MENU control begins at y=30.
// The GBA launcher (GameboyAdvance.ino) takes the rightmost slot, so this one
// sits one button-plus-gap to its left. Keep the two in step if either moves.
static constexpr int GB_LAUNCH_W = 64;
static constexpr int GB_LAUNCH_H = 25;
static constexpr int GB_LAUNCH_Y = 3;
static constexpr int GB_LAUNCH_X = DISPLAY_WIDTH - 72 - GB_LAUNCH_W - 4;

int gbMainTouchLauncherLeft() {
    return GB_LAUNCH_X;
}

void gbDrawMainTouchLauncher() {
    frameSprite.fillRoundRect(GB_LAUNCH_X, GB_LAUNCH_Y,
                              GB_LAUNCH_W, GB_LAUNCH_H, 7, 0x2104);
    frameSprite.drawRoundRect(GB_LAUNCH_X, GB_LAUNCH_Y,
                              GB_LAUNCH_W, GB_LAUNCH_H, 7, TFT_CYAN);
    frameSprite.setTextDatum(MC_DATUM);
    frameSprite.setTextColor(TFT_WHITE);
    frameSprite.drawString("GB", GB_LAUNCH_X + GB_LAUNCH_W / 2,
                          GB_LAUNCH_Y + GB_LAUNCH_H / 2);
    frameSprite.setTextDatum(TL_DATUM);
}

void gbServiceMainTouch() {
    static bool launcherWasDown = false;
    M5.update();

    bool launcherDown = false;
    if (!dappCanvasActive) {
        const uint8_t count = M5.Touch.getCount();
        for (uint8_t i = 0; i < count; i++) {
            const auto& touch = M5.Touch.getDetail(i);
            if (!touch.isPressed()) continue;
            if (touch.x >= GB_LAUNCH_X && touch.x < GB_LAUNCH_X + GB_LAUNCH_W &&
                touch.y >= GB_LAUNCH_Y && touch.y < GB_LAUNCH_Y + GB_LAUNCH_H) {
                launcherDown = true;
                break;
            }
        }
    }

    const bool launch = launcherDown && !launcherWasDown;
    launcherWasDown = launcherDown;
    if (launch) {
        String command = "gb";
        commandProcessor(command);  // same history, picker, and cleanup path as typed `gb`
    }
}
#else
int gbMainTouchLauncherLeft() { return DISPLAY_WIDTH; }
void gbDrawMainTouchLauncher() {}
void gbServiceMainTouch() {}
#endif

static void gbFreeScale() {
    if (gbScaleBuf) { heap_caps_free(gbScaleBuf); gbScaleBuf = nullptr; }
    if (gbColMap)   { heap_caps_free(gbColMap);   gbColMap = nullptr; }
    if (gbRowMap)   { heap_caps_free(gbRowMap);   gbRowMap = nullptr; }
}

// Builds (or rebuilds) the scale buffers for the current mode. Returns false if
// the board-specific scaling workspace couldn't be allocated -- caller falls
// back to 1x, which needs no scale buffer at all.
static bool gbSetupScale() {
#if defined(DOLL_BOARD_TAB5)
    // The Tab5's panel is a continuously scanned PSRAM framebuffer. Scaling a
    // 160x144 frame all the way to 800x720 makes every drawn emulator frame
    // rewrite 1.15 MB of that same PSRAM and can starve DSI scanout, leaving
    // only gbClearPanel()'s black frame visible. A crisp 3x image is 480x432
    // (414 KB), large enough on the 7-inch panel while leaving scanout headroom.
    const int scale = gbFitMode ? 3 : 1;
    gbOutW = GB_W * scale;
    gbOutH = GB_H * scale;
    gbOutX = (DISPLAY_WIDTH - gbOutW) / 2;
    gbOutY = 0;  // top-align the Game Boy picture; controls use the space below

    if (!gbFitMode) {
        return true;
    }

    gbColMap = (int16_t*)heap_caps_malloc(gbOutW * sizeof(int16_t), MALLOC_CAP_8BIT);
    gbRowMap = (int16_t*)heap_caps_malloc(gbOutH * sizeof(int16_t), MALLOC_CAP_8BIT);
    //Only four scaled rows are materialized at once. The emulator framebuffer,
    //this strip, and Display.ino's transfer strip all stay internal; the live DSI
    //framebuffer is now the sole PSRAM participant in a Game Boy panel update.
    gbScaleBuf = (uint16_t*)heap_caps_malloc(
        (size_t)gbOutW * GB_TAB5_STAGE_ROWS * sizeof(uint16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!gbColMap || !gbRowMap || !gbScaleBuf) {
        gbFreeScale();
        return false;
    }
    for (int x = 0; x < gbOutW; x++) gbColMap[x] = x / scale;
    for (int y = 0; y < gbOutH; y++) gbRowMap[y] = y / scale;
    return true;
#elif defined(FNK0104N_3P5_320x480_ST77922)
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
#endif
}

// Pushes one emulator frame to the panel. gnuboy renders GB_PIXEL_565_LE (native
// little-endian uint16); the panel wants big-endian, so setSwapBytes(true) makes
// pushImage swap as it streams.
static void gbBlitFrame() {
    const uint16_t* frame = gbHost.frame();
    if (!frame) return;
#if defined(DOLL_BOARD_TAB5)
    // Compose into frameSprite for menu/cleanup correctness, but send each
    // completed internal-RAM strip straight to the DSI framebuffer. The prior
    // path re-read the whole rectangle from PSRAM and issued 108 four-row
    // transactions twice, which hardware timing measured at ~66.9ms per blit.
    const bool oldFrameSwap = frameSprite.getSwapBytes();
    const bool oldPanelSwap = tft.getSwapBytes();
    frameSprite.setSwapBytes(true);
    tft.setSwapBytes(true);
    if (!gbFitMode) {
        frameSprite.pushImage(gbOutX, gbOutY, GB_W, GB_H,
                              const_cast<uint16_t*>(frame));
        tft.pushImage(gbOutX, gbOutY, GB_W, GB_H,
                      const_cast<uint16_t*>(frame));
        frameSprite.setSwapBytes(oldFrameSwap);
        tft.setSwapBytes(oldPanelSwap);
        displayInvalidateShadow();
        return;
    }

    int firstRow = 0;
    while (firstRow < gbOutH) {
        const int rows = min(GB_TAB5_STAGE_ROWS, gbOutH - firstRow);
        for (int localY = 0; localY < rows; localY++) {
            const int outY = firstRow + localY;
            const uint16_t* srcRow = frame + (size_t)gbRowMap[outY] * GB_W;
            uint16_t* dstRow = gbScaleBuf + (size_t)localY * gbOutW;
            for (int outX = 0; outX < gbOutW; outX++) {
                dstRow[outX] = srcRow[gbColMap[outX]];
            }
        }
        frameSprite.pushImage(gbOutX, gbOutY + firstRow, gbOutW, rows,
                              gbScaleBuf);
        tft.pushImage(gbOutX, gbOutY + firstRow, gbOutW, rows, gbScaleBuf);
        firstRow += rows;
    }
    frameSprite.setSwapBytes(oldFrameSwap);
    tft.setSwapBytes(oldPanelSwap);
    displayInvalidateShadow();
#elif defined(FNK0104N_3P5_320x480_ST77922)
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
    //The scaled frame lives in PSRAM. Route it through Display.ino's internal-RAM
    //strip and keep all strips in one transaction; direct PSRAM-to-DSI framebuffer
    //copies are what caused Tetris to flash the panel's cyan clear colour.
    pushDisplayImageStaged(gbOutX, gbOutY, width, height, pixels, true);
#endif
#endif
}

static void gbLogFrameDiagnostic(uint32_t frameNumber) {
    const uint16_t* frame = gbHost.frame();
    if (!frame) return;

    uint32_t signature = 2166136261u;
    uint32_t nonBlack = 0;
    for (size_t i = 0; i < (size_t)GB_W * GB_H; i++) {
        signature = (signature ^ frame[i]) * 16777619u;
        if (frame[i] != 0) nonBlack++;
    }
    Serial.printf("[gb] frame=%lu source_hash=%08lX nonblack=%lu/%u\n",
                  (unsigned long)frameNumber, (unsigned long)signature,
                  (unsigned long)nonBlack, (unsigned)(GB_W * GB_H));
}  // Proves gnuboy rendered pixels without using unsupported DSI panel readback.

#if defined(DOLL_BOARD_TAB5)
// Touch controls live in the wide margins around the centered 3x Game Boy
// picture. They deliberately remain outside the 480x432 game image, so no UI
// pixels obscure the game and the same layout can stay visible in the menu.
static constexpr int GB_TOUCH_DPAD_X = 180;
static constexpr int GB_TOUCH_DPAD_Y = 510;
static constexpr int GB_TOUCH_DPAD_HALF = 175;
static constexpr int GB_TOUCH_DPAD_DEAD = 44;
static constexpr int GB_TOUCH_A_X = 1160;
static constexpr int GB_TOUCH_A_Y = 430;
static constexpr int GB_TOUCH_B_X = 1010;
static constexpr int GB_TOUCH_B_Y = 550;
static constexpr int GB_TOUCH_FACE_R = 70;
static constexpr int GB_TOUCH_DOT_R = 14;
static constexpr int GB_TOUCH_DOT_BOX = GB_TOUCH_DOT_R * 2 + 1;

struct GbTouchPoint {
    int16_t x;
    int16_t y;
};

static GbTouchPoint gbTouchActivePoints[5];
static uint8_t gbTouchActivePointCount = 0;
static GbTouchPoint gbTouchDrawnPoints[5];
static uint8_t gbTouchDrawnPointCount = 0;
static bool gbTouchDotsDirty = false;
static uint16_t gbTouchDotRestore[GB_TOUCH_DOT_BOX * GB_TOUCH_DOT_BOX];

static void gbDrawTouchControls() {
    const uint16_t padFill = 0x2104;  // very dark grey; visible without glare
    const uint16_t padEdge = TFT_CYAN;
    const int arm = 120;
    const int thick = 110;

    frameSprite.fillRoundRect(GB_TOUCH_DPAD_X - thick / 2,
                              GB_TOUCH_DPAD_Y - arm - thick / 2,
                              thick, arm + thick / 2, 12, padFill);
    frameSprite.fillRoundRect(GB_TOUCH_DPAD_X - thick / 2,
                              GB_TOUCH_DPAD_Y,
                              thick, arm + thick / 2, 12, padFill);
    frameSprite.fillRoundRect(GB_TOUCH_DPAD_X - arm - thick / 2,
                              GB_TOUCH_DPAD_Y - thick / 2,
                              arm + thick / 2, thick, 12, padFill);
    frameSprite.fillRoundRect(GB_TOUCH_DPAD_X,
                              GB_TOUCH_DPAD_Y - thick / 2,
                              arm + thick / 2, thick, 12, padFill);
    frameSprite.drawRoundRect(GB_TOUCH_DPAD_X - thick / 2,
                              GB_TOUCH_DPAD_Y - arm - thick / 2,
                              thick, arm * 2 + thick, 12, padEdge);
    frameSprite.drawRoundRect(GB_TOUCH_DPAD_X - arm - thick / 2,
                              GB_TOUCH_DPAD_Y - thick / 2,
                              arm * 2 + thick, thick, 12, padEdge);

    frameSprite.fillCircle(GB_TOUCH_A_X, GB_TOUCH_A_Y, GB_TOUCH_FACE_R, 0x4008);
    frameSprite.drawCircle(GB_TOUCH_A_X, GB_TOUCH_A_Y, GB_TOUCH_FACE_R, TFT_PINK);
    frameSprite.fillCircle(GB_TOUCH_B_X, GB_TOUCH_B_Y, GB_TOUCH_FACE_R, 0x4008);
    frameSprite.drawCircle(GB_TOUCH_B_X, GB_TOUCH_B_Y, GB_TOUCH_FACE_R, TFT_PINK);

    frameSprite.fillRoundRect(485, 650, 135, 48, 18, padFill);
    frameSprite.drawRoundRect(485, 650, 135, 48, 18, padEdge);
    frameSprite.fillRoundRect(660, 650, 135, 48, 18, padFill);
    frameSprite.drawRoundRect(660, 650, 135, 48, 18, padEdge);
    frameSprite.fillRoundRect(1090, 30, 150, 52, 18, padFill);
    frameSprite.drawRoundRect(1090, 30, 150, 52, 18, TFT_YELLOW);

    frameSprite.setTextDatum(MC_DATUM);
    frameSprite.setTextColor(TFT_WHITE);
    frameSprite.drawString("A", GB_TOUCH_A_X, GB_TOUCH_A_Y);
    frameSprite.drawString("B", GB_TOUCH_B_X, GB_TOUCH_B_Y);
    frameSprite.drawString("SELECT", 552, 674);
    frameSprite.drawString("START", 727, 674);
    frameSprite.drawString("MENU", 1165, 56);
    frameSprite.setTextDatum(TL_DATUM);
}
#else
static void gbDrawTouchControls() {}
#endif

static void gbRenderTouchDots(bool force = false);

static void gbClearPanel() {
    frameSprite.fillSprite(TFT_BLACK);
    gbDrawTouchControls();
    displayInvalidateShadow();
    pushDisplayFrame();
}  // Commits black letterboxing + controls once; game frames update only their rectangle.

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

// Samples every active Tab5 contact so combinations such as diagonal+A work.
// Coordinates are already rotated into the display's 1280x720 space by
// M5Unified. MENU is edge-triggered; ordinary Game Boy buttons remain held for
// as long as their contact remains inside the corresponding control.
static uint8_t gbPumpTouch(uint8_t& buttons) {
#if !defined(DOLL_BOARD_TAB5)
    buttons = 0;
    return 0;
#else
    M5.update();
    uint8_t next = 0;
    bool menuDown = false;
    GbTouchPoint nextPoints[5];
    uint8_t nextPointCount = 0;
    const uint8_t count = M5.Touch.getCount();
    for (uint8_t i = 0; i < count; i++) {
        const auto& touch = M5.Touch.getDetail(i);
        if (!touch.isPressed()) continue;
        const int x = touch.x;
        const int y = touch.y;
        if (nextPointCount < 5) {
            nextPoints[nextPointCount].x = x;
            nextPoints[nextPointCount].y = y;
            nextPointCount++;
        }

        if (x >= 1090 && x < 1240 && y >= 30 && y < 82) {
            menuDown = true;
            continue;
        }

        const int dx = x - GB_TOUCH_DPAD_X;
        const int dy = y - GB_TOUCH_DPAD_Y;
        if (abs(dx) <= GB_TOUCH_DPAD_HALF && abs(dy) <= GB_TOUCH_DPAD_HALF) {
            if (dx < -GB_TOUCH_DPAD_DEAD) next |= GameBoyHost::kLeft;
            if (dx >  GB_TOUCH_DPAD_DEAD) next |= GameBoyHost::kRight;
            if (dy < -GB_TOUCH_DPAD_DEAD) next |= GameBoyHost::kUp;
            if (dy >  GB_TOUCH_DPAD_DEAD) next |= GameBoyHost::kDown;
        }

        const int dax = x - GB_TOUCH_A_X;
        const int day = y - GB_TOUCH_A_Y;
        if (dax * dax + day * day <= GB_TOUCH_FACE_R * GB_TOUCH_FACE_R)
            next |= GameBoyHost::kA;
        const int dbx = x - GB_TOUCH_B_X;
        const int dby = y - GB_TOUCH_B_Y;
        if (dbx * dbx + dby * dby <= GB_TOUCH_FACE_R * GB_TOUCH_FACE_R)
            next |= GameBoyHost::kB;

        if (x >= 485 && x < 620 && y >= 650 && y < 698)
            next |= GameBoyHost::kSelect;
        if (x >= 660 && x < 795 && y >= 650 && y < 698)
            next |= GameBoyHost::kStart;
    }

    static bool menuWasDown = false;
    const uint8_t events = (menuDown && !menuWasDown) ? GB_EVT_MENU : 0;
    menuWasDown = menuDown;
    bool contactsChanged = nextPointCount != gbTouchActivePointCount;
    for (uint8_t i = 0; !contactsChanged && i < nextPointCount; i++) {
        contactsChanged = nextPoints[i].x != gbTouchActivePoints[i].x ||
                          nextPoints[i].y != gbTouchActivePoints[i].y;
    }
    if (contactsChanged) {
        memcpy(gbTouchActivePoints, nextPoints,
               sizeof(GbTouchPoint) * nextPointCount);
        gbTouchActivePointCount = nextPointCount;
        gbTouchDotsDirty = true;
    }
    buttons = next;
    return events;
#endif
}

// Contact dots are a panel-only overlay: frameSprite remains the clean source
// underneath them. Before moving/removing a dot, restore its tiny rectangle
// from that source, then draw the latest contacts. Keeping the shadow clean
// avoids turning each touch movement into a full-screen DSI refresh.
static void gbRenderTouchDots(bool force) {
#if !defined(DOLL_BOARD_TAB5)
    (void)force;
#else
    if (!force && !gbTouchDotsDirty) return;
    uint16_t* frame = (uint16_t*)frameSprite.getBuffer();
    if (!frame) return;

    const bool oldSwap = tft.getSwapBytes();
    tft.setSwapBytes(false);  // frameSprite pixels are already panel-native RGB565
    for (uint8_t i = 0; i < gbTouchDrawnPointCount; i++) {
        const int x0 = max(0, (int)gbTouchDrawnPoints[i].x - GB_TOUCH_DOT_R);
        const int y0 = max(0, (int)gbTouchDrawnPoints[i].y - GB_TOUCH_DOT_R);
        const int x1 = min(DISPLAY_WIDTH, (int)gbTouchDrawnPoints[i].x + GB_TOUCH_DOT_R + 1);
        const int y1 = min(DISPLAY_HEIGHT, (int)gbTouchDrawnPoints[i].y + GB_TOUCH_DOT_R + 1);
        const int width = x1 - x0;
        const int height = y1 - y0;
        if (width <= 0 || height <= 0) continue;
        for (int row = 0; row < height; row++) {
            memcpy(gbTouchDotRestore + row * width,
                   frame + (size_t)(y0 + row) * DISPLAY_WIDTH + x0,
                   (size_t)width * sizeof(uint16_t));
        }
        tft.pushImage(x0, y0, width, height, gbTouchDotRestore);
    }
    tft.setSwapBytes(oldSwap);

    for (uint8_t i = 0; i < gbTouchActivePointCount; i++) {
        const int x = gbTouchActivePoints[i].x;
        const int y = gbTouchActivePoints[i].y;
        tft.fillCircle(x, y, 9, TFT_WHITE);
        tft.drawCircle(x, y, 12, TFT_CYAN);
        tft.drawCircle(x, y, 13, TFT_CYAN);
    }

    memcpy(gbTouchDrawnPoints, gbTouchActivePoints,
           sizeof(GbTouchPoint) * gbTouchActivePointCount);
    gbTouchDrawnPointCount = gbTouchActivePointCount;
    gbTouchDotsDirty = false;
#endif
}

// Turns a DOLL-OS logical path (absolute or relative to cwd) into a stdio/VFS path
// gnuboy's fopen can open. SD is mounted at "/sdcard" (Storage.ino), LittleFS at
// "/littlefs". Returns "" if the path routes to SD but no card is mounted.
static String gbVfsPath(const String& arg) {
    Serial.printf("[GBDBG launch 01] resolve begin cwd='%s' arg='%s' sd_mounted=%u\n",
                  cwd.c_str(), arg.c_str(), sdCardMounted ? 1u : 0u);
    Serial.flush();
    String resolved = resolvePath(cwd, arg);
    Serial.printf("[GBDBG launch 02] logical path resolved='%s'\n", resolved.c_str());
    Serial.flush();
    RoutedPath r = routePath(resolved);
    Serial.printf("[GBDBG launch 03] route is_sd=%u real_path='%s' fs=%p\n",
                  r.isSd ? 1u : 0u, r.realPath.c_str(), static_cast<void*>(r.fs));
    Serial.flush();
    if (r.isSd) {
        if (!sdCardMounted) {
            Serial.println("[GBDBG launch 04] VFS mapping failed: SD is not mounted");
            Serial.flush();
            return "";
        }
        String vfs = "/sdcard" + r.realPath;
        Serial.printf("[GBDBG launch 04] VFS path='%s'\n", vfs.c_str());
        Serial.flush();
        return vfs;
    }
    String vfs = "/littlefs" + r.realPath;
    Serial.printf("[GBDBG launch 04] VFS path='%s'\n", vfs.c_str());
    Serial.flush();
    return vfs;
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
#if defined(DOLL_BOARD_TAB5)
    // Keep the list between the touch-control rails: the D-pad ends at x=363
    // and B begins at x=940. Start/Select occupy the bottom center.
    const int left = 365;
    const int width = 550;
    const int footY = 620;
#else
    const int left = 14;
    const int width = DISPLAY_WIDTH - left * 2;
    const int footY = DISPLAY_HEIGHT - 30;
#endif
    const int listTop = top + 28;
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
    frameSprite.drawString(String(selected + 1) + "/" + String(count), left + width, top);
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
    frameSprite.drawString(truncated ? "showing first 128 ROMs" :
                           "D-pad move/page  A/Start choose  B/Menu cancel", left, footY);
    if (truncated) {
        frameSprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
        frameSprite.drawString("D-pad move/page  A/Start choose  B/Menu cancel", left, footY + 12);
    }
    frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    gbDrawTouchControls();
    pushDisplayFrame();
    gbRenderTouchDots(true);
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

    static String names[kGbRomMenuMax];
    bool truncated = false;
    int count = gbCollectRomNames(names, truncated);
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
    uint8_t touchButtons = 0;
    uint8_t prevCombined = 0;
    int selected = 0;
    bool redraw = true;

    for (;;) {
        if (redraw) {
            gbDrawRomMenu(names, count, selected, truncated);
            redraw = false;
        }

        uint8_t telnetPressed = 0;
        uint8_t events = gbPumpInput(buttons);
        events |= gbPumpTouch(touchButtons);
        gbRenderTouchDots();
        events |= gbPumpTelnetMenuInput(telnetPressed);
        if (events & (GB_EVT_QUIT | GB_EVT_MENU)) {
            slaveLinkSendLine("GAME 0");
            outLine("gb: ROM selection cancelled", C_YELLOW);
            return false;
        }

        const uint8_t combined = buttons | touchButtons;
        uint8_t pressed = (combined & ~prevCombined) | telnetPressed;
        prevCombined = combined;
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
#if defined(DOLL_BOARD_TAB5)
    outLine("  fit (default) is a centered 3x image (480x432); 1x is native", C_CYAN);
    outLine("  160x144. The bounded scale keeps DSI scanout stable.", C_CYAN);
#else
    outLine("  fit (default) fills the panel but costs ~38ms of SPI per push,", C_CYAN);
    outLine("  so most frames go undrawn; 1x is small but near-smooth.", C_CYAN);
#endif
    outLine("  Controls (BLE keyboard via slave): WASD/arrows=D-pad, N=A,", C_CYAN);
    outLine("  M=B, Enter=Start, \\=Select, Ctrl+T=quit.", C_CYAN);
    outLine("  Xbox pad via slave: stick/d-pad, A=A, B=B, Menu=Start,", C_CYAN);
    outLine("  View=Select, LB=this menu, LB+RB=quit.", C_CYAN);
#if defined(DOLL_BOARD_TAB5)
    outLine("  Touch: D-pad left, A/B right, Select/Start below, Menu top-right.", C_CYAN);
#endif
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
    frameSprite.drawString("D-pad move/change  A ok  B/Menu back", left, footY + 12);
    frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    gbDrawTouchControls();
    pushDisplayFrame();
    gbRenderTouchDots(true);
}

// Runs the menu until the player leaves it. Returns true if they chose to quit
// the ROM. `buttons` is kept live throughout (the slave keeps sending DOWN/UP
// while we're here), so the game doesn't inherit a stuck key on resume.
static bool gbRunMenu(uint8_t& buttons, uint8_t& touchButtons) {
    int selected = 0;
    String note;
    // Seed the edge detector with what's already held, so the keypress that
    // opened the menu -- or a D-pad direction the player hadn't let go of --
    // doesn't immediately move the cursor.
    uint8_t prev = buttons | touchButtons;
    bool redraw = true;

    for (;;) {
        if (redraw) {
            gbDrawMenu(selected, note);
            redraw = false;
        }

        uint8_t events = gbPumpInput(buttons);
        events |= gbPumpTouch(touchButtons);
        gbRenderTouchDots();
        if (events & GB_EVT_QUIT) return true;
        if (events & GB_EVT_MENU) return false;   // Escape closes what Escape opened

        const uint8_t combined = buttons | touchButtons;
        const uint8_t pressed = combined & ~prev;
        prev = combined;
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
#if defined(DOLL_BOARD_TAB5)
                note = gbFitMode ? "fit: top-aligned 3x, DSI-safe"
                                 : "1x: native 160x144";
#else
                note = gbFitMode ? "fit: fills the panel, most frames skipped"
                                 : "1x: small but near-smooth";
#endif
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
    Serial.printf("[GBDBG launch 00] command enter argc=%d\n", partCount);
    Serial.flush();
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
    const String saveVfs = gbSavePath(romVfs);
    Serial.printf("[GBDBG launch 05] selected logical='%s' vfs='%s' save='%s' mode=%s\n",
                  romLogical.c_str(), romVfs.c_str(), saveVfs.c_str(),
                  gbFitMode ? "fit" : "1x");
    Serial.printf("[GBDBG launch 06] memory before host heap=%u internal=%u psram=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    Serial.flush();
    ledPulseStorageRead(romLogical.startsWith("/sd/"));
    Serial.printf("[gb] launch path=%s mode=%s\n", romVfs.c_str(),
                  gbFitMode ? "fit" : "1x");
    Serial.flush();

    // Reassert the Tab5's logical landscape mapping before any Game Boy draw;
    // this corrects the portrait-native DSI panel by 90 degrees counterclockwise.
    tft.setRotation(TAB5_DISPLAY_ROTATION);
    Serial.printf("[gb] display rotation=%d logical=%dx%d\n",
                  TAB5_DISPLAY_ROTATION, tft.width(), tft.height());
    Serial.println("[GBDBG launch 07] host begin call");
    Serial.flush();
    if (!gbHost.begin()) {
        outLine("gb: " + gbHost.status(), C_RED);
        return;
    }
    Serial.println("[GBDBG launch 08] host begin complete");
    Serial.flush();
    Serial.println("[GBDBG launch 09] host ROM load call");
    Serial.flush();
    if (!gbHost.load(romVfs, saveVfs)) {
        const String loadStatus = gbHost.status();
        outLine("gb: " + loadStatus, C_RED);
        if (loadStatus.indexOf("integrity check failed") >= 0) {
            outLine("gb: replace this ROM with a verified dump", C_YELLOW);
        } else {
            outLine("gb: check the path -- " + romVfs, C_YELLOW);
        }
        return;
    }
    Serial.println("[GBDBG launch 10] host ROM load/reset/SRAM complete");
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
    Serial.printf("[gb] display workspace ready: %dx%d at %d,%d\n",
                  gbOutW, gbOutH, gbOutX, gbOutY);
    Serial.flush();

    // Take the codec off the radio and bring up our own TX channel. Both steps
    // are advisory: a game with no
    // sound still beats no game.
    bool audioUp = false;
    Serial.println("[gb] releasing shared audio");
    Serial.flush();
    if (radioReleaseAudio()) {
        Serial.println("[gb] starting Game Boy audio");
        Serial.flush();
        audioUp = AudioOut::begin();
    }
    Serial.printf("[gb] audio setup complete: %s\n", audioUp ? "ready" : "silent");
    Serial.flush();
    if (!audioUp) {
        outLine("gb: audio unavailable -- running silent", C_YELLOW);
    }

    outLine("gb: launching " + romLogical + " -- Ctrl+T to quit", C_GREEN);
    drawDisplayFrame();   // flush that line to the panel before we take it over

    // Hand the panel and the keyboard over to the game.
    slaveLinkSendLine("GAME 1");   // DS-Slave: emit raw button events, not ASCII
    delay(20);
    Serial.println("[gb] preparing first canvas frame");
    Serial.flush();
    gbClearPanel();
    Serial.println("[gb] canvas ready; entering emulation loop");
    Serial.flush();

    uint8_t buttons = 0;
    uint8_t touchButtons = 0;
    const uint32_t frameUs = 16743;   // ~59.7 Hz, true GB frame period
    uint32_t nextFrame = micros() + frameUs;   // deadline for the frame about to run
    uint32_t framesRun = 0, framesDrawn = 0;
    uint32_t perfStartedUs = micros();
    uint32_t perfRunUs = 0, perfBlitUs = 0;
    uint16_t perfFrames = 0, perfDrawn = 0;
    // Guarantee that the first emulated frame is rendered even if launch-time
    // housekeeping happens to put the deadline slightly in the past.
    int skipRun = kMaxFrameSkip;
    const uint32_t startedMs = millis();

    for (;;) {
        uint8_t events = gbPumpInput(buttons);
        events |= gbPumpTouch(touchButtons);
        if (events & GB_EVT_QUIT) break;
        if (events & GB_EVT_MENU) {
            // Modal: emulation is paused for the duration. Audio goes quiet on
            // its own once the DMA ring drains -- nothing is producing samples.
            // Game frames bypass frameSprite, so force the menu's first push to
            // replace every game pixel rather than trusting the shell shadow.
            displayInvalidateShadow();
            if (gbRunMenu(buttons, touchButtons)) break;
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

        gbHost.setButtons(buttons | touchButtons);
        uint32_t perfMarkUs = micros();
        gbHost.runFrame(draw);
        perfRunUs += micros() - perfMarkUs;
        if (draw) {
            perfMarkUs = micros();
            gbBlitFrame();
            perfBlitUs += micros() - perfMarkUs;
            framesDrawn++;
            perfDrawn++;
            skipRun = 0;
        } else {
            skipRun++;
        }
        gbRenderTouchDots(draw);
        gbHost.tickSave();
        ledService();
        framesRun++;
        perfFrames++;

        // A five-second rolling report is cheap enough to leave enabled and
        // gives us real hardware timing if either emulation or DSI blitting is
        // still the limiting side after removing the opcode tracer.
        if (perfFrames >= 300) {
            const uint32_t elapsedUs = micros() - perfStartedUs;
            const uint32_t emuFps10 = elapsedUs
                ? (uint32_t)(((uint64_t)perfFrames * 10000000ULL) / elapsedUs) : 0;
            const uint32_t drawFps10 = elapsedUs
                ? (uint32_t)(((uint64_t)perfDrawn * 10000000ULL) / elapsedUs) : 0;
            uint32_t audioPushed = 0, audioDropped = 0, audioGaps = 0;
            AudioOut::stats(audioPushed, audioDropped, audioGaps);
            Serial.printf("[gb perf] emu=%lu.%lu drawn=%lu.%lu run_avg=%luus blit_avg=%luus audio=%s pushed=%lu dropped=%lu gaps=%lu\n",
                          (unsigned long)(emuFps10 / 10), (unsigned long)(emuFps10 % 10),
                          (unsigned long)(drawFps10 / 10), (unsigned long)(drawFps10 % 10),
                          (unsigned long)(perfRunUs / perfFrames),
                          (unsigned long)(perfDrawn ? perfBlitUs / perfDrawn : 0),
                          audioUp ? "on" : "off", (unsigned long)audioPushed,
                          (unsigned long)audioDropped, (unsigned long)audioGaps);
            Serial.flush();
            perfStartedUs = micros();
            perfRunUs = perfBlitUs = 0;
            perfFrames = perfDrawn = 0;
        }

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
    if (audioUp) {
        AudioOut::end();          // releases the I2S controller for the next "radio play"
    }
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
