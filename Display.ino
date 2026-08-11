//   Display.ino
// Drives the Tab5 panel as the shared shell display. It is output-only for this
// milestone: touchscreen events are never read. Rendering uses an M5Canvas so
// the inherited terminal-history and ANSI code stays independent of the panel
// controller fitted to a particular Tab5 revision.
//
//   The panel follows the tail of history live by default, same as DOLL-OS, but
//   Shift+Up/Down (recognized in TelnetServer.ino's handleCsiSequence and
//   RemoteSession.ino's raw-session byte classifier) walks displayScrollOffset
//   back through it via displayScrollBy() below.

//   Boot / init

//   Panel pushes.
//
//   Redrawing the whole sprite every frame is cheap -- it is memory. The blit is the
//   expensive half: the 1280x720 16bpp canvas is about 1.8MB, and a canvas app that moves
//   one character would otherwise pay the full transfer cost on
//   every FLIP. That transfer, not the interpreter, is what set the ceiling on how fast a
//   dapp game could feel.
//
//   Frames are diffed against a shadow copy of what the panel was last sent, and only the
//   rows that actually differ go to the M5GFX display surface.
//
//   The shadow can go wrong one way -- if something draws to the panel *without* going through the
//   sprite, the shadow no longer describes the glass. Gameboy.ino does exactly that, so any
//   such path has to call displayInvalidateShadow(). Allocation failure is not a failure
//   mode: a null shadow just means every push is a full one, which is the old behaviour.
static uint16_t* displayShadow = nullptr;
static bool displayShadowValid = false;
//The Tab5 panel framebuffer, frameSprite and displayShadow all live in PSRAM. Panel
//updates used to detour through an internal-RAM strip because M5GFX warns about
//PSRAM-to-PSRAM memcpy corruption -- but that warning is scoped to
//Panel_FrameBufferBase::copyRect, which copies a region of the framebuffer onto itself.
//Our pushes and shadow syncs have disjoint source and destination buffers, and the strip
//was not what caused the panel blinking. It cost ~20KB of the scarce internal pool, so
//transfers now read straight out of PSRAM.
//
//The row chunking survives for the other reason it existed: Panel_FrameBufferBase tracks
//one bounding rectangle per transaction, so pushing in bounded strips keeps a sparse
//update from turning into one giant cache writeback that can starve continuous DSI scanout.
static const int DISPLAY_PUSH_ROWS = 8;
static DappCanvasCell* displayCanvasShadow = nullptr;
static int displayCanvasShadowCols = 0;
static int displayCanvasShadowRows = 0;
static bool displayCanvasShadowValid = false;
static bool displayDappDirtyRows[DISPLAY_HEIGHT] = {};

static void clearDappDirtyRows() {
    memset(displayDappDirtyRows, 0, sizeof(displayDappDirtyRows));
}  // Starts a canvas FLIP with no panel rows scheduled for transfer.

static void markDappDirtyRows(int y, int height) {
    int firstRow = max(0, y);
    int lastRow = min(DISPLAY_HEIGHT, y + height);
    for (int row = firstRow; row < lastRow; row++) {
        displayDappDirtyRows[row] = true;
    }
}  // Records only the screen rows whose final pixels changed during this FLIP.

void displayInvalidateDappCanvas() {
    displayCanvasShadowValid = false;
    displayCanvasShadowCols = 0;
    displayCanvasShadowRows = 0;
}  // Forces the next AppRunner canvas FLIP to rebuild its complete panel region.

void displayInvalidateShadow() {
    displayShadowValid = false;
}

void pushDisplayImageStaged(int x, int y, int width, int height,
                            const uint16_t* pixels, bool swapBytes) {
    if (!pixels || width <= 0 || height <= 0) {
        return;
    }

    //Each strip must complete its own M5GFX transaction. Panel_FrameBufferBase tracks one
    //bounding rectangle per transaction, so wrapping every strip together turns a sparse
    //update into one giant cache writeback that can starve continuous DSI scanout.
    bool oldSwapBytes = tft.getSwapBytes();
    tft.setSwapBytes(swapBytes);
    int pushed = 0;
    while (pushed < height) {
        int rowsThisPush = min(DISPLAY_PUSH_ROWS, height - pushed);
        const uint16_t* src = pixels + (size_t)pushed * width;
        tft.pushImage(x, y + pushed, width, rowsThisPush,
                      const_cast<uint16_t*>(src));
        pushed += rowsThisPush;
    }
    tft.setSwapBytes(oldSwapBytes);
    displayInvalidateShadow();
}  // Pushes arbitrary RGB565 images from PSRAM-backed app buffers in bounded strips.

static void pushDisplayRows(int y, int rowCount) {
    if (rowCount <= 0) {
        return;
    }

    uint16_t* frame = (uint16_t*)frameSprite.getBuffer();
    if (!frame) {
        return;
    }

    //Sprite rows go to the panel directly. The strip bound is transaction sizing, not a copy.
    bool oldSwapBytes = tft.getSwapBytes();
    tft.setSwapBytes(false);
    int pushed = 0;
    while (pushed < rowCount) {
        int rowsThisPush = min(DISPLAY_PUSH_ROWS, rowCount - pushed);
        uint16_t* src = frame + (size_t)(y + pushed) * DISPLAY_WIDTH;
        tft.pushImage(0, y + pushed, DISPLAY_WIDTH, rowsThisPush, src);
        pushed += rowsThisPush;
    }
    tft.setSwapBytes(oldSwapBytes);
}  // Copies complete sprite rows to the panel in bounded-writeback strips.

static void pushDappDirtyRows() {
    int row = 0;
    while (row < DISPLAY_HEIGHT) {
        while (row < DISPLAY_HEIGHT && !displayDappDirtyRows[row]) {
            row++;
        }
        int firstRow = row;
        while (row < DISPLAY_HEIGHT && displayDappDirtyRows[row]) {
            row++;
        }
        if (firstRow < row) {
            pushDisplayRows(firstRow, row - firstRow);
        }
    }

    //Canvas mode deliberately bypasses the full-frame PSRAM shadow. Keeping it valid
    //would require another PSRAM write for every changed row, while invalidating it costs
    //only one full resynchronization after the app exits.
    displayInvalidateShadow();
}  // Transfers canvas changes without scanning or copying the two 1.84MB PSRAM images.

static bool copyDisplayRowsToShadow(int y, int rowCount) {
    uint16_t* frame = (uint16_t*)frameSprite.getBuffer();
    if (!frame || !displayShadow || rowCount <= 0) {
        return false;
    }

    //The sprite and its shadow are two distinct PSRAM allocations, so one straight memcpy
    //is safe: M5GFX's corruption warning covers copying a framebuffer region onto itself.
    const size_t offset = (size_t)y * DISPLAY_WIDTH;
    memcpy(displayShadow + offset, frame + offset,
           (size_t)rowCount * DISPLAY_WIDTH * sizeof(uint16_t));
    return true;
}  // Synchronizes the diff shadow with the sprite rows just sent to the panel.

void pushDisplayFrame() {
    uint16_t* frame = (uint16_t*)frameSprite.getBuffer();
    const size_t rowWords = (size_t)DISPLAY_WIDTH;
    const size_t rowBytes = rowWords * sizeof(uint16_t);

    //Nothing can be sent before createSprite has allocated the canvas.
    //createSprite has run, and the row pointers below would be offsets from null
    if (!frame) {
        return;
    }

    if (!displayShadow || !displayShadowValid) {
        //pushDisplayRows commits each strip as its own transaction. Combining the full
        //frame into one would force a 1.84MB cache writeback burst.
        pushDisplayRows(0, DISPLAY_HEIGHT);
        displayShadowValid = copyDisplayRowsToShadow(0, DISPLAY_HEIGHT);
        return;
    }

    int row = 0;
    while (row < DISPLAY_HEIGHT) {
        const uint16_t* frameRow = frame + (size_t)row * rowWords;
        if (memcmp(frameRow, displayShadow + (size_t)row * rowWords, rowBytes) == 0) {
            row++;
            continue;
        }
        //walk the whole run of changed rows so they go out as one transfer rather than one
        //setAddrWindow per row
        int start = row;
        while (row < DISPLAY_HEIGHT &&
               memcmp(frame + (size_t)row * rowWords,
                      displayShadow + (size_t)row * rowWords, rowBytes) != 0) {
            row++;
        }
        pushDisplayRows(start, row - start);
        if (!copyDisplayRowsToShadow(start, row - start)) {
            displayShadowValid = false;
        }
    }
}

void initDisplay() {
    //history ring first, so its PSRAM use is accounted before the sprite snapshot below
    displayHistoryRows = (DisplayHistoryRow*) psramOrInternalCalloc(
        DISPLAY_HISTORY_MAX_LINES, sizeof(DisplayHistoryRow), "displayHistory");

    //The frame sprite is about 1.8MB at 16bpp. Snapshot PSRAM around allocation
    //so the boot log proves where M5Canvas placed it.
    size_t psramFreeBeforeSprite = ESP.getFreePsram();
    tft.setRotation(TAB5_DISPLAY_ROTATION);
    Serial.printf("[display] rotation=%d logical=%dx%d\n",
                  TAB5_DISPLAY_ROTATION, tft.width(), tft.height());

    frameSprite.setColorDepth(16);
    frameSprite.createSprite(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    Serial.printf("[psram] frameSprite: %u bytes drawn from PSRAM (0 => it fell back to internal RAM)\n",
                  (unsigned)(psramFreeBeforeSprite - ESP.getFreePsram()));

    //Same size as the sprite. Deliberately PSRAM-or-nothing rather than going through
    //psramOrInternalCalloc: this buffer only buys speed, and taking 1.8MB of the scarce
    //   internal pool to get it would be a bad trade. A null result costs nothing but the
    //   old full-frame push.
    displayShadow = (uint16_t*) heap_caps_calloc(
        (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    displayShadowValid = false;
    Serial.printf("[psram] displayShadow: %u bytes -> %s\n",
                  (unsigned)((size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t)),
                  displayShadow ? "PSRAM (partial frame pushes enabled)"
                                : "unavailable (full frame pushes)");

    //Comparing final cells before touching the PSRAM sprite means Tetris usually redraws
    //only the old and new piece cells instead of clearing 1.6MB per FLIP. The snapshot is
    //read once per changed cell and never DMA'd, so PSRAM is the right pool for it --
    //internal RAM is the scarce one and this is ~14KB of it.
    displayCanvasShadow = (DappCanvasCell*)heap_caps_malloc(
        (size_t)DAPP_CANVAS_MAX_COLS * DAPP_CANVAS_MAX_ROWS * sizeof(DappCanvasCell),
        MALLOC_CAP_SPIRAM);
    displayInvalidateDappCanvas();
    Serial.printf("[display] dapp canvas shadow: %u bytes -> %s\n",
                  (unsigned)((size_t)DAPP_CANVAS_MAX_COLS * DAPP_CANVAS_MAX_ROWS *
                             sizeof(DappCanvasCell)),
                  displayCanvasShadow ? "PSRAM (changed-cell redraws enabled)"
                                      : "unavailable (full canvas redraws)");

    frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    frameSprite.setTextSize(DISPLAY_TEXT_SIZE);
    frameSprite.fillSprite(TFT_BLACK);
    //setup() draws the boot splash immediately after initDisplay(). Avoid a redundant
    //full black commit immediately before that first cyan frame.
}

void displaySetSleeping(bool sleeping) {
    if (sleeping) {
        tft.sleep();                              // M5GFX handles every Tab5 panel revision safely.
        return;
    }

    tft.wakeup();                                 // Restores the remembered display brightness.
    displayInvalidateShadow();                    // Force the next render to resynchronize panel RAM.
    markDisplayDirty();                           // Repaint status after network and wake state change.
}

void drawDisplayBootSplash() {
    frameSprite.fillSprite(TFT_CYAN);
    const int splashWidth = min(DISPLAY_WIDTH - (DISPLAY_PADDING * 4), 360);
    const int splashHeight = 128;
    const int splashX = (DISPLAY_WIDTH - splashWidth) / 2;
    const int splashY = (DISPLAY_HEIGHT - splashHeight) / 2;
    frameSprite.fillRect(splashX, splashY, splashWidth, splashHeight, TFT_BLACK);
    frameSprite.setTextDatum(MC_DATUM);
    frameSprite.setTextColor(TFT_PINK, TFT_BLACK);
    frameSprite.setTextSize(DISPLAY_TEXT_SIZE * 2);
    frameSprite.drawString("DOLL-OS", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 - 20);
    frameSprite.setTextSize(DISPLAY_TEXT_SIZE);
    frameSprite.drawString("booting...", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 + 28);
    frameSprite.setTextDatum(TL_DATUM);
    pushDisplayFrame();
}

//   Layout

int displayTerminalY() {
    return DISPLAY_STATUS_BAR_HEIGHT;
}
int displayTerminalHeight() {
    return DISPLAY_HEIGHT - DISPLAY_STATUS_BAR_HEIGHT - DISPLAY_COMMAND_BAR_HEIGHT;
}
int displayCommandBarY() {
    return DISPLAY_HEIGHT - DISPLAY_COMMAND_BAR_HEIGHT;
}

void markDisplayDirty() {
    displayDirty = true;
}

//single entry point for the three activeInput* globals (TelnetServer.ino, Motoko.ino,
//Ssh.ino and TelnetClient.ino all funnel through this) so the mirrored command
//bar can never go stale on the TFT because a call site forgot to mark the frame dirty
void setActiveInput(const String& prompt, const String& text, bool masked) {
    activeInputPrompt = prompt;
    activeInputText = text;
    activeInputMasked = masked;
    markDisplayDirty();
}

//   History ring buffer (mirrors DOLL-OS's HistoryRow / addWrappedHistoryLine)

static int displayCharWidth(char ch) {
    char glyph[2] = { ch, '\0' };
    return (int)frameSprite.textWidth(glyph);
}

static int displayHistoryPhysicalIndex(int logicalIndex) {
    return (displayHistoryHead + logicalIndex) % DISPLAY_HISTORY_MAX_LINES;
}

static void copyDisplayHistoryText(char* dest, const String& src) {
    int copyLen = min((int)src.length(), DISPLAY_HISTORY_ROW_MAX_CHARS - 1);
    for (int i = 0; i < copyLen; i++) {
        dest[i] = src[i];
    }
    dest[copyLen] = '\0';

    if (src.length() >= DISPLAY_HISTORY_ROW_MAX_CHARS && DISPLAY_HISTORY_ROW_MAX_CHARS > 4) {
        dest[DISPLAY_HISTORY_ROW_MAX_CHARS - 4] = '.';
        dest[DISPLAY_HISTORY_ROW_MAX_CHARS - 3] = '.';
        dest[DISPLAY_HISTORY_ROW_MAX_CHARS - 2] = '.';
        dest[DISPLAY_HISTORY_ROW_MAX_CHARS - 1] = '\0';
    }
}

static const char* displayHistoryRowText(int logicalIndex) {
    return displayHistoryRows[displayHistoryPhysicalIndex(logicalIndex)].text;
}
static uint16_t displayHistoryRowColor(int logicalIndex) {
    return displayHistoryRows[displayHistoryPhysicalIndex(logicalIndex)].color;
}

void addDisplayHistoryRow(const String& row, uint16_t color) {
    if (displayHistoryRows == nullptr) {
        return;   //allocation failed at boot -- keep count at 0 so nothing tries to render/read rows
    }
    displayOpenRowOwner = nullptr;   //a fresh row is becoming "last" via the non-streaming path

    int slot;
    if (displayHistoryCount < DISPLAY_HISTORY_MAX_LINES) {
        slot = displayHistoryPhysicalIndex(displayHistoryCount);
        displayHistoryCount++;
    } else {
        slot = displayHistoryHead;
        displayHistoryHead = (displayHistoryHead + 1) % DISPLAY_HISTORY_MAX_LINES;
    }

    copyDisplayHistoryText(displayHistoryRows[slot].text, row);
    displayHistoryRows[slot].color = color;
    markDisplayDirty();
}

void addDisplayLine(const String& line) {
    addDisplayLine(line, TFT_WHITE);
}

void addDisplayLine(const String& line, uint16_t color) {
    const int maxWidth = DISPLAY_WIDTH - (DISPLAY_PADDING * 2);
    if (maxWidth < 0) {
        return;
    }

    String row = "";
    row.reserve(min((int)line.length(), DISPLAY_HISTORY_ROW_MAX_CHARS - 1));
    int rowWidth = 0;

    for (int i = 0; i < line.length(); i++) {
        char ch = line[i];
        int charWidth = displayCharWidth(ch);

        if (row.length() > 0 && rowWidth + charWidth > maxWidth) {
            addDisplayHistoryRow(row, color);
            row = "";
            rowWidth = 0;
            if (ch == ' ') {
                continue;
            }
        }

        row += ch;
        rowWidth += charWidth;
    }

    addDisplayHistoryRow(row, color);
}

void updateLastDisplayHistoryRow(const String& row, uint16_t color) {
    if (displayHistoryCount == 0) {
        return;
    }
    int lastSlot = displayHistoryPhysicalIndex(displayHistoryCount - 1);
    copyDisplayHistoryText(displayHistoryRows[lastSlot].text, row);
    displayHistoryRows[lastSlot].color = color;
    markDisplayDirty();
}

void clearDisplayHistory() {
    displayHistoryCount = 0;
    displayHistoryHead = 0;
    displayOpenRowOwner = nullptr;
    displayScrollOffset = 0;
    markDisplayDirty();
}

//nudges the scroll-back position by `delta` lines (positive = further back/older, negative
//= toward the live tail); the upper bound depends on how many lines are actually visible,
//so it's clamped in drawDisplayHistory() instead of here -- this just keeps it non-negative.
void displayScrollBy(int delta) {
    displayScrollOffset += delta;
    if (displayScrollOffset < 0) {
        displayScrollOffset = 0;
    }
    markDisplayDirty();
}

//   Streaming API for raw remote byte mirroring (ssh, outbound telnet) -- one
//   DisplayStreamState instance per independent stream so interleaved streams
//   (ssh stdout vs stderr) don't corrupt each other's in-progress row

static void displayStreamCloseRow(DisplayStreamState& st) {
    if (displayOpenRowOwner == &st) {
        displayOpenRowOwner = nullptr;
    }
    st.pendingRow = "";
    st.cursorCol = 0;
    st.wrapDepth = 0;
    st.wrapPending = false;
    st.crPending = false;   //the line ended -- whatever a trailing CR was for, it wasn't a redraw
}

//removes just the newest history row -- used when displayStreamBackspace merges a wrapped
//stream row back into its predecessor. Only ever called for a row the stream owns and just
//created, so displayHistoryHead is left alone: this peels the tail, never the ring's head.
static void popLastDisplayHistoryRow() {
    if (displayHistoryCount == 0) {
        return;
    }
    displayHistoryCount--;
    markDisplayDirty();
}

void displayStreamReset(DisplayStreamState& st) {
    displayStreamCloseRow(st);
}

void displayStreamNewline(DisplayStreamState& st) {
    displayStreamCloseRow(st);
}

void displayStreamPutChar(DisplayStreamState& st, char ch, uint16_t color) {
    const int maxWidth = DISPLAY_WIDTH - (DISPLAY_PADDING * 2);
    if (maxWidth < 0) {
        return;
    }

    if (displayOpenRowOwner != &st) {
        st.pendingRow = "";
        st.cursorCol = 0;
        //preserve the wrap-continuation link only when the open row is free because *we* just
        //wrapped it (owner == nullptr AND wrapPending). Any other cause -- a newline, a reset,
        //or another stream owning the open row -- starts a fresh logical line, so drop the link.
        if (displayOpenRowOwner != nullptr || !st.wrapPending) {
            st.wrapDepth = 0;
            st.wrapPending = false;
        }
        st.crPending = false;   //nothing left to unwind: the row this CR homed into is gone
    } else {
        displayStreamApplyPendingCarriageReturn(st);   //first write after a CR -- it was a redraw
    }

    bool atEnd = st.cursorCol >= (size_t)st.pendingRow.length();

    if (atEnd && st.pendingRow.length() > 0 &&
        frameSprite.textWidth(st.pendingRow + ch) > maxWidth) {
        st.pendingRow = "";
        st.cursorCol = 0;
        displayOpenRowOwner = nullptr;
        st.wrapPending = true;   //the panel row about to open continues this same logical line
        if (ch == ' ') {
            return;
        }
        atEnd = true;
    }

    if (atEnd) {
        st.pendingRow += ch;
    } else {
        st.pendingRow.setCharAt(st.cursorCol, ch);
    }
    st.cursorCol++;

    if (displayOpenRowOwner == &st) {
        updateLastDisplayHistoryRow(st.pendingRow, color);
    } else {
        addDisplayHistoryRow(st.pendingRow, color);
        displayOpenRowOwner = &st;
        if (st.wrapPending) {
            st.wrapDepth++;         //this new row is a wrap-continuation, reachable by backspace
            st.wrapPending = false;
        } else {
            st.wrapDepth = 0;       //first panel row of a fresh logical line
        }
    }
}

//peels wrap-continuation rows off the tail until the logical line is back to its first panel
//row. Shared by the redraw and erase-line paths below; displayStreamBackspace does the same
//thing one row at a time as it deletes through a wrap. Only rows this stream created as
//continuations are popped, so the ring's head is safe (see popLastDisplayHistoryRow) -- the
//count > 1 guard just keeps the re-adopt read in range.
static void displayStreamUnwindWrap(DisplayStreamState& st) {
    while (st.wrapDepth > 0 && displayHistoryCount > 1) {
        popLastDisplayHistoryRow();
        st.wrapDepth--;
        st.pendingRow = String(displayHistoryRowText(displayHistoryCount - 1));
    }
    st.wrapDepth = 0;   //nonzero here means history ran out from under us (unreachable in
                        //practice: it needs a logical line longer than the whole ring) -- drop
                        //the link rather than leave a depth pointing at rows no longer ours
}

//a CR that turned out to begin a redraw, resolved at the moment the redraw's first write
//lands: collapse the panel-side wrap so the new content overwrites the old line instead of
//piling up underneath it
static void displayStreamApplyPendingCarriageReturn(DisplayStreamState& st) {
    if (!st.crPending) {
        return;
    }
    st.crPending = false;
    displayStreamUnwindWrap(st);
    st.cursorCol = 0;
}

//CR homes the cursor to the start of the *logical* line. A real terminal only homes within
//the physical row, but the panel has no notion of the remote's rows: it reflows at its own
//much narrower pixel width, so one remote row routinely becomes several panel rows, and the
//continuation rows have to come off for a redraw to land on top of the old text rather than
//below it. Without that, submitting a message long enough to wrap cleared only the panel row
//the cursor happened to be on and left the earlier ones standing as a duplicate of the
//message (the reported "only the last line gets deleted" bug).
//
//But the unwind is deferred rather than done here, because a bare CR is not yet evidence of
//anything: nearly every CR in a remote stream is the front half of a CRLF, where the line is
//finished and its wrap rows are real content to keep. Unwinding eagerly threw those away and
//truncated every wrapped line to its first panel row -- wrapping looked broken outright. Only
//a CR followed by a write or an erase means a redraw, so the flag is set here and cashed in
//by whichever of those comes first; displayStreamCloseRow (newline/reset) drops it instead.
void displayStreamCarriageReturn(DisplayStreamState& st) {
    if (displayOpenRowOwner != &st) {
        return;
    }
    st.cursorCol = 0;
    st.crPending = (st.wrapDepth > 0);
}

void displayStreamErase(DisplayStreamState& st, DisplayEraseKind kind) {
    if (displayOpenRowOwner != &st || kind == DISPLAY_ERASE_NONE) {
        return;
    }

    displayStreamApplyPendingCarriageReturn(st);   //an erase after a CR is a redraw too

    if (kind == DISPLAY_ERASE_ALL) {
        //whole line, wrap included -- same reasoning as the CR above, except an explicit
        //"erase this line" needs no deferring: it can't be the half of anything else. The
        //cursor goes to column 0 rather than holding its old column, since with the row's
        //text gone there's nothing left for a column to index into, and every real emitter
        //of ESC[2K pairs it with a CR or an absolute cursor move anyway.
        displayStreamUnwindWrap(st);
        st.pendingRow = "";
        st.cursorCol = 0;
    } else if (kind == DISPLAY_ERASE_TO_START) {
        //blanked, not removed: erasing backwards leaves the cursor where it was, so the
        //cleared cells have to keep occupying their columns for the next write to land right
        size_t upTo = min(st.cursorCol, (size_t)st.pendingRow.length());
        for (size_t i = 0; i < upTo; i++) {
            st.pendingRow.setCharAt(i, ' ');
        }
    } else {
        if (st.cursorCol >= (size_t)st.pendingRow.length()) {
            return;
        }
        st.pendingRow.remove(st.cursorCol);
    }

    updateLastDisplayHistoryRow(st.pendingRow, displayHistoryRowColor(displayHistoryCount - 1));
}

void displayStreamBackspace(DisplayStreamState& st) {
    if (displayOpenRowOwner != &st) {
        return;
    }
    //an explicit delete settles the question a pending CR left open, and settles it its own
    //way: the merge-up below already walks back through the wrap one row at a time, so the
    //bulk unwind must not also fire later and take the rest of the line with it
    st.crPending = false;
    if (st.cursorCol == 0) {
        //at the left edge of the current panel row. If this logical line wrapped onto an
        //earlier panel row, peel this (now-empty) one off and keep deleting from the end of
        //the previous row -- otherwise backspace stalls here visually while the remote's own
        //line buffer keeps shrinking (the reported "deletes line 2 but not line 1" bug).
        if (st.wrapDepth == 0) {
            return;   //genuine start of the logical line -- nothing above it belongs to us
        }
        popLastDisplayHistoryRow();
        st.wrapDepth--;
        st.pendingRow = String(displayHistoryRowText(displayHistoryCount - 1));
        st.cursorCol = st.pendingRow.length();
        //displayOpenRowOwner stays &st -- we created the row we just re-adopted, too
    }
    st.cursorCol--;
    st.pendingRow.remove(st.cursorCol, 1);
    updateLastDisplayHistoryRow(st.pendingRow, displayHistoryRowColor(displayHistoryCount - 1));
}

//   ANSI/UTF-8 filter for raw remote streams -- display-only (telnet gets true
//   unfiltered passthrough, see Ssh.ino/TelnetClient.ino); ported from DOLL-OS's
//   ansi.ino, SGR colors now map onto real TFT_eSPI pixel constants instead of a
//   sprite-native palette pulled from a different graphics stack

static uint16_t ansiSgrColor(int code, uint16_t defaultColor) {
    switch (code) {
        case 0:               return defaultColor;
        case 30: case 90:     return TFT_BLACK;
        case 31: case 91:     return TFT_RED;
        case 32: case 92:     return TFT_GREEN;
        case 33: case 93:     return TFT_YELLOW;
        case 34: case 94:     return TFT_BLUE;
        case 35: case 95:     return TFT_MAGENTA;
        case 36: case 96:     return TFT_CYAN;
        case 37: case 97:     return TFT_WHITE;
        default:              return defaultColor;
    }
}

static uint16_t ansiApplySgr(const String& params, uint16_t currentColor, uint16_t defaultColor) {
    if (params.length() == 0) {
        return defaultColor;
    }
    uint16_t color = currentColor;
    int start = 0;
    while (start <= (int)params.length()) {
        int sep = params.indexOf(';', start);
        String token = (sep == -1) ? params.substring(start) : params.substring(start, sep);
        int code = token.length() ? token.toInt() : 0;
        color = ansiSgrColor(code, defaultColor);
        if (sep == -1) {
            break;
        }
        start = sep + 1;
    }
    return color;
}

bool ansiFilterByte(AnsiFilterState& st, uint8_t ch, uint16_t defaultColor, uint16_t& color, char& outCh, bool& colorChanged, bool& isBackspace, bool& isCarriageReturn, DisplayEraseKind& erase) {
    colorChanged = false;
    isBackspace = false;
    isCarriageReturn = false;
    erase = DISPLAY_ERASE_NONE;

    switch (st.state) {
        case ANSI_TEXT:
            if (ch == 0x1B) {
                st.state = ANSI_ESC;
                return false;
            }
            if (st.utf8Remaining > 0) {
                if ((ch & 0xC0) == 0x80) {
                    st.utf8Remaining--;
                    return false;
                }
                st.utf8Remaining = 0;
            }
            if (ch >= 0x80) {
                if ((ch & 0xE0) == 0xC0)      st.utf8Remaining = 1;
                else if ((ch & 0xF0) == 0xE0) st.utf8Remaining = 2;
                else if ((ch & 0xF8) == 0xF0) st.utf8Remaining = 3;
                outCh = '?';
                return true;
            }
            if (ch == 0x08 || ch == 0x7F) {
                isBackspace = true;
                return false;
            }
            if (ch == '\t' || (ch >= 0x20 && ch < 0x7F)) {
                outCh = (char)ch;
                return true;
            }
            return false;

        case ANSI_ESC:
            if (ch == '[') {
                st.state = ANSI_CSI;
                st.csiParams = "";
            } else if (ch == ']') {
                st.state = ANSI_OSC;
            } else {
                st.state = ANSI_TEXT;
            }
            return false;

        case ANSI_CSI:
            if (ch >= 0x40 && ch <= 0x7E) {
                if (ch == 'm') {
                    color = ansiApplySgr(st.csiParams, color, defaultColor);
                    colorChanged = true;
                } else if (ch == 'K') {
                    int mode = st.csiParams.length() ? st.csiParams.toInt() : 0;
                    erase = (mode == 1) ? DISPLAY_ERASE_TO_START
                          : (mode == 2) ? DISPLAY_ERASE_ALL
                                        : DISPLAY_ERASE_TO_END;
                } else if (ch == 'G' && (st.csiParams.length() == 0 || st.csiParams.toInt() <= 1)) {
                    //CHA to column 1 is a CR by another spelling, and line editors reach for it
                    //about as often. Any other column is a real horizontal move the panel's
                    //reflowed rows can't honor, so it stays dropped like the rest of CSI.
                    isCarriageReturn = true;
                }
                st.state = ANSI_TEXT;
            } else {
                st.csiParams += (char)ch;
            }
            return false;

        case ANSI_OSC:
            if (ch == 0x07) {
                st.state = ANSI_TEXT;
            } else if (ch == 0x1B) {
                st.state = ANSI_OSC_ESC;
            }
            return false;

        case ANSI_OSC_ESC:
            st.state = (ch == '\\') ? ANSI_TEXT : ANSI_OSC;
            return false;
    }

    return false;
}

//maps Output.ino's ANSI SGR color constants (C_RED etc.) onto real TFT_eSPI pixel
//colors -- outLine()'s own generated text (help/status/etc.) uses this so it shows
//up in roughly the same color on both the telnet session and the mirrored panel
uint16_t ansiCodeToPixelColor(int code) {
    switch (code) {
        case C_BLACK:   return TFT_BLACK;
        case C_RED:     return TFT_RED;
        case C_GREEN:   return TFT_GREEN;
        case C_YELLOW:  return TFT_YELLOW;
        case C_BLUE:    return TFT_BLUE;
        case C_MAGENTA: return TFT_MAGENTA;
        case C_CYAN:    return TFT_CYAN;
        case C_PINK:    return TFT_PINK;
        default:        return TFT_WHITE;
    }
}

//   Frame rendering -- called once per loop() tick from DS.ino, mirroring
//   DOLL-OS's own per-tick drawTerminalHistory()/drawCommandBar() calls

void drawDisplayStatusBar() {
    frameSprite.fillRect(0, 0, DISPLAY_WIDTH, DISPLAY_STATUS_BAR_HEIGHT, TFT_BLACK);
    frameSprite.setTextDatum(TL_DATUM);
    frameSprite.setTextSize(DISPLAY_TEXT_SIZE);
    frameSprite.setTextColor(TFT_PINK, TFT_BLACK);
    frameSprite.drawString("DOLL-OS", DISPLAY_PADDING, 4);

    char statusText[64];
    snprintf(statusText, sizeof(statusText), "MEM:%luKB VOL:%02d BAT:%d%%",
        (unsigned long)(ESP.getFreeHeap() / 1000), radioGetVolume(), readBatteryPercent());
    frameSprite.setTextDatum(TR_DATUM);
    frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    frameSprite.drawString(statusText, DISPLAY_WIDTH - DISPLAY_PADDING, 4);

    frameSprite.drawFastHLine(0, DISPLAY_STATUS_BAR_HEIGHT - 1, DISPLAY_WIDTH, TFT_PINK);
    frameSprite.setTextDatum(TL_DATUM);
}

void drawDisplayHistory() {
    const int lineHeight = DISPLAY_TERMINAL_LINE_HEIGHT;
    const int top = displayTerminalY();
    const int height = displayTerminalHeight();

    frameSprite.fillRect(0, top, DISPLAY_WIDTH, height, TFT_BLACK);
    if (displayHistoryCount == 0) {
        return;
    }

    frameSprite.setTextDatum(TL_DATUM);
    frameSprite.setTextSize(DISPLAY_TEXT_SIZE);
    //rows are drawn starting DISPLAY_PADDING below `top`, so the space actually available
    //for text is height - DISPLAY_PADDING. Dividing the full height counted one row too
    //many for the region: on the 240px panel that put the bottom row flush at y=219, right
    //against the command bar's divider line at y=220 -- it read as the last line clipping
    //into the command bar. Subtracting the top pad drops that overhanging row.
    const int visibleLines = max(1, (height - DISPLAY_PADDING) / lineHeight);

    //Shift+Up/Down (handleCsiSequence, TelnetServer.ino/RemoteSession.ino) walks this back
    //via displayScrollBy(); clamped here rather than there because the max meaningful offset
    //depends on how many lines actually fit on screen right now
    const int maxScrollOffset = max(0, displayHistoryCount - visibleLines);
    if (displayScrollOffset > maxScrollOffset) {
        displayScrollOffset = maxScrollOffset;
    }

    const int lastLine = displayHistoryCount - 1 - displayScrollOffset;
    const int firstLine = max(0, lastLine - visibleLines + 1);

    int y = top + DISPLAY_PADDING;
    for (int i = firstLine; i <= lastLine; i++) {
        frameSprite.setTextColor(displayHistoryRowColor(i), TFT_BLACK);
        frameSprite.drawString(displayHistoryRowText(i), DISPLAY_PADDING, y);
        y += lineHeight;
    }
    frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);

    if (displayScrollOffset > 0) {
        //cheap "you're not looking at the live tail" hint, top-right of the terminal area
        frameSprite.setTextDatum(TR_DATUM);
        frameSprite.setTextColor(TFT_YELLOW, TFT_BLACK);
        frameSprite.drawString("SCROLL", DISPLAY_WIDTH - DISPLAY_PADDING, top + DISPLAY_PADDING);
        frameSprite.setTextDatum(TL_DATUM);
        frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    }
}

void drawDisplayCommandBar() {
    const int y = displayCommandBarY();
    frameSprite.fillRect(0, y, DISPLAY_WIDTH, DISPLAY_COMMAND_BAR_HEIGHT, TFT_BLACK);
    frameSprite.drawFastHLine(0, y, DISPLAY_WIDTH, TFT_WHITE);
    frameSprite.setTextDatum(TL_DATUM);
    frameSprite.setTextSize(DISPLAY_TEXT_SIZE);
    frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);

    frameSprite.drawString(activeInputPrompt, DISPLAY_PADDING, y + DISPLAY_PADDING);
    int textX = DISPLAY_PADDING + (int)frameSprite.textWidth(activeInputPrompt);
    int maxWidth = max(0, DISPLAY_WIDTH - textX - DISPLAY_PADDING);

    String shown = activeInputText;
    if (activeInputMasked) {
        String masked;
        masked.reserve(shown.length());
        for (size_t i = 0; i < shown.length(); i++) {
            masked += '*';
        }
        shown = masked;
    }

    //a too-long line drops its head (shows the tail, where typing is happening). Track how
    //many leading chars we dropped so the caret below can be mapped from its buffer index
    //(commandCursorPos, into the full activeInputText) onto what's actually visible here.
    size_t droppedFromHead = 0;
    while (shown.length() > 0 && frameSprite.textWidth(shown) > maxWidth) {
        shown = shown.substring(1);
        droppedFromHead++;
    }
    frameSprite.drawString(shown, textX, y + DISPLAY_PADDING);

    //Solid caret at the edit position. On the Tab5, blinking this by committing the DSI
    //framebuffer every 500 ms can interrupt panel scanout even when no UI content changed.
    //commandCursorPos indexes the full buffer; subtract
    //the dropped head to land in `shown`, then clamp so a cursor scrolled off the left edge
    //parks at the start of the visible window rather than drawing off-panel.
    int caretInShown = commandCursorPos - (int)droppedFromHead;
    if (caretInShown < 0) caretInShown = 0;
    if (caretInShown > (int)shown.length()) caretInShown = shown.length();
    int caretX = textX + (int)frameSprite.textWidth(shown.substring(0, caretInShown));
    if (caretX > textX + maxWidth) caretX = textX + maxWidth;
    frameSprite.drawFastVLine(caretX, y + DISPLAY_PADDING, frameSprite.fontHeight(), TFT_WHITE);
}

//   paints a running .dapp's CANVAS grid over the terminal area (AppRunner.ino's FLIP).
//   The grid is scaled to fill the area rather than drawn at a fixed cell size: a script
//   picks its playfield in cells and gets the biggest version of it the panel can show,
//   which is the only way a 10x20 well and an 80x24 status screen can both look right.
void drawDappCanvas() {
    const int top = displayTerminalY();
    const int height = displayTerminalHeight();
    if (!dappCanvasCells || dappCanvasCols <= 0 || dappCanvasRows <= 0) {
        frameSprite.fillRect(0, top, DISPLAY_WIDTH, height, TFT_BLACK);
        markDappDirtyRows(top, height);
        displayInvalidateDappCanvas();
        return;
    }

    const int cellW = max(1, (DISPLAY_WIDTH - DISPLAY_PADDING * 2) / dappCanvasCols);
    const int cellH = max(1, (height - DISPLAY_PADDING * 2) / dappCanvasRows);

    //Keep AppRunner glyphs at the user-selected compact built-in size.
    const int textSize = 1;

    //center the grid in the area it didn't divide evenly into
    const int originX = (DISPLAY_WIDTH - cellW * dappCanvasCols) / 2;
    const int originY = top + (height - cellH * dappCanvasRows) / 2;
    const bool fullRedraw = !displayCanvasShadow || !displayCanvasShadowValid ||
        displayCanvasShadowCols != dappCanvasCols ||
        displayCanvasShadowRows != dappCanvasRows;
    if (fullRedraw) {
        frameSprite.fillRect(0, top, DISPLAY_WIDTH, height, TFT_BLACK);
        markDappDirtyRows(top, height);
    }

    frameSprite.setTextSize(textSize);
    frameSprite.setTextDatum(MC_DATUM);
    //drawString (not drawChar) because only drawString honours the datum, and a 2-byte
    //stack buffer keeps that from meaning a String allocation per cell per frame
    char glyph[2] = { ' ', '\0' };
    for (int row = 0; row < dappCanvasRows; row++) {
        for (int col = 0; col < dappCanvasCols; col++) {
            const int cellIndex = row * dappCanvasCols + col;
            const DappCanvasCell& cell = dappCanvasCells[cellIndex];
            if (!fullRedraw &&
                cell.ch == displayCanvasShadow[cellIndex].ch &&
                cell.color == displayCanvasShadow[cellIndex].color) {
                continue;
            }

            const int cellX = originX + col * cellW;
            const int cellY = originY + row * cellH;
            if (!fullRedraw) {
                //Erase only a changed cell. The selected text size always fits inside its
                //cell, so this cannot clip a neighbouring glyph.
                frameSprite.fillRect(cellX, cellY, cellW, cellH, TFT_BLACK);
            }
            markDappDirtyRows(cellY, cellH);
            if (cell.ch == ' ' || cell.ch == '\0') {
                continue;   //the area is already black; skipping blanks is most of the frame
            }
            //single-argument setTextColor draws no background box, so glyphs can't clip
            //their neighbours when a cell is narrower than the font
            glyph[0] = cell.ch;
            frameSprite.setTextColor(ansiCodeToPixelColor(cell.color));
            frameSprite.drawString(glyph,
                                   cellX + cellW / 2,
                                   cellY + cellH / 2);
        }
    }

    if (displayCanvasShadow) {
        const size_t canvasBytes = (size_t)dappCanvasCols * dappCanvasRows *
            sizeof(DappCanvasCell);
        memcpy(displayCanvasShadow, dappCanvasCells, canvasBytes);
        displayCanvasShadowCols = dappCanvasCols;
        displayCanvasShadowRows = dappCanvasRows;
        displayCanvasShadowValid = true;
    }

    frameSprite.setTextSize(DISPLAY_TEXT_SIZE);
    frameSprite.setTextDatum(TL_DATUM);
    frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);
}

void drawDisplayFrame() {
    if (!displayDirty) {
        return;   //nothing changed: do not disturb the live DSI framebuffer while idle
    }
    displayDirty = false;

    if (dappCanvasActive) {
        //A canvas FLIP already knows exactly which cells changed. Preserve that knowledge
        //as a small internal-RAM row map instead of rereading 3.68MB of PSRAM to rediscover it.
        clearDappDirtyRows();
        drawDisplayStatusBar();
        markDappDirtyRows(0, DISPLAY_STATUS_BAR_HEIGHT);
        drawDappCanvas();
        drawDisplayCommandBar();
        markDappDirtyRows(DISPLAY_HEIGHT - DISPLAY_COMMAND_BAR_HEIGHT,
                          DISPLAY_COMMAND_BAR_HEIGHT);
        pushDappDirtyRows();
        return;
    }

    drawDisplayStatusBar();
    drawDisplayHistory();
    drawDisplayCommandBar();
    pushDisplayFrame();
}
