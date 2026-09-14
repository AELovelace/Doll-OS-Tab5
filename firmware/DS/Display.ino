//   Display.ino
//   drives the TFT panel as a mirror of the telnet session -- "a second screen for
//   the cardputer," per the user's request. Telnet remains the only *input* path;
//   this is output-only. Ported from DOLL-OS's terminal.ino (history wrapping/
//   rendering) and ansi.ino (SGR color interpretation for raw remote byte streams),
//   retargeted from an M5GFX sprite to a TFT_eSPI one -- the API shapes
//   (drawString/textWidth/setTextColor/setTextDatum/fillSprite) are close enough
//   that the porting is mostly a rename, not a redesign.
//
//   The panel follows the tail of history live by default, same as DOLL-OS, but
//   Shift+Up/Down (recognized in TelnetServer.ino's handleCsiSequence and
//   RemoteSession.ino's raw-session byte classifier) walks displayScrollOffset
//   back through it via displayScrollBy() below.

//   Boot / init

//   Panel pushes.
//
//   Redrawing the whole sprite every frame is cheap -- it is memory. The blit is the
//   expensive half: 16bpp over the panel bus is ~150KB per frame at 320x240 and ~300KB at
//   480x320, and a canvas app that moves one character was paying the full price of it on
//   every FLIP. That transfer, not the interpreter, is what set the ceiling on how fast a
//   dapp game could feel.
//
//   Frames are diffed against a shadow copy of what the panel was last sent, and only the
//   rows that actually differ go over the bus. On N, those logical landscape rows are
//   transformed into native portrait strips before reaching Freenove's rotation-0 writer.
//
//   The shadow can go wrong one way -- if something draws to the panel *without* going through the
//   sprite, the shadow no longer describes the glass. Gameboy.ino does exactly that, so any
//   such path has to call displayInvalidateShadow(). Allocation failure is not a failure
//   mode: a null shadow just means every push is a full one, which is the old behaviour.
static uint16_t* displayShadow = nullptr;
static bool displayShadowValid = false;

void displayInvalidateShadow() {
    displayShadowValid = false;
}

static void pushDisplayRows(int y, int rowCount) {
    if (rowCount <= 0) {
        return;
    }
    uint16_t* src = (uint16_t*)frameSprite.getPointer() + (size_t)y * DISPLAY_WIDTH;
#ifdef FNK0104N_3P5_320x480_ST77922
    // Freenove's LVGL path rounds native regions to four-pixel boundaries.
    // Align logical rows the same way; after rotation these become native X.
    int alignedY = y & ~0x3;
    int alignedEnd = min(DISPLAY_HEIGHT, (y + rowCount + 3) & ~0x3);
    int alignedRows = alignedEnd - alignedY;
    src = (uint16_t*)frameSprite.getPointer() + (size_t)alignedY * DISPLAY_WIDTH;
    tft_st77922.Fill_Colors_Landscape(0, alignedY, DISPLAY_WIDTH, alignedRows, src);
#else
    //mirrors what TFT_eSprite::pushSprite does for a 16bpp sprite -- the sprite's buffer is
    //already in the panel's byte order, so the swap has to be off for the transfer
    bool oldSwapBytes = tft.getSwapBytes();
    tft.setSwapBytes(false);
    tft.pushImage(0, y, DISPLAY_WIDTH, rowCount, src);
    tft.setSwapBytes(oldSwapBytes);
#endif
}

void pushDisplayFrame() {
    uint16_t* frame = (uint16_t*)frameSprite.getPointer();
    const size_t rowWords = (size_t)DISPLAY_WIDTH;
    const size_t rowBytes = rowWords * sizeof(uint16_t);

    //TFT_eSprite::pushSprite used to do this guard for us -- nothing to send before
    //createSprite has run, and the row pointers below would be offsets from null
    if (!frame) {
        return;
    }

    if (!displayShadow || !displayShadowValid) {
        pushDisplayRows(0, DISPLAY_HEIGHT);
        if (displayShadow) {
            memcpy(displayShadow, frame, rowBytes * DISPLAY_HEIGHT);
            displayShadowValid = true;
        }
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
            memcpy(displayShadow + (size_t)row * rowWords, frame + (size_t)row * rowWords, rowBytes);
            row++;
        }
        pushDisplayRows(start, row - start);
    }
}

void initDisplay() {
    //history ring first, so its PSRAM use is accounted before the sprite snapshot below
    displayHistoryRows = (DisplayHistoryRow*) psramOrInternalCalloc(
        DISPLAY_HISTORY_MAX_LINES, sizeof(DisplayHistoryRow), "displayHistory");

    //the frame sprite is the big one (~150KB at 16bpp). TFT_eSPI routes it to PSRAM by
    //itself when psramFound() (Sprite.cpp callocSprite); snapshot the PSRAM pool around
    //createSprite so the boot log proves whether it actually landed there.
    size_t psramFreeBeforeSprite = ESP.getFreePsram();
#ifdef FNK0104N_3P5_320x480_ST77922
    tft_st77922.Init();
    tft_st77922.Set_Rotation(0);
    frameSprite.setColorDepth(16);
    frameSprite.createSprite(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    frameSprite.setSwapBytes(true);
#else
    tft.init();
    tft.setRotation(DOLL_DISPLAY_UPSIDE_DOWN ? 3 : 1);
    frameSprite.setColorDepth(16);
    frameSprite.createSprite(DISPLAY_WIDTH, DISPLAY_HEIGHT);
#endif
    Serial.printf("[psram] frameSprite: %u bytes drawn from PSRAM (0 => it fell back to internal RAM)\n",
                  (unsigned)(psramFreeBeforeSprite - ESP.getFreePsram()));

    //   Same size as the sprite. Deliberately PSRAM-or-nothing rather than going through
    //   psramOrInternalCalloc: this buffer only buys speed, and taking 150KB of the scarce
    //   internal pool to get it would be a bad trade. A null result costs nothing but the
    //   old full-frame push.
    displayShadow = (uint16_t*) heap_caps_calloc(
        (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    displayShadowValid = false;
    Serial.printf("[psram] displayShadow: %u bytes -> %s\n",
                  (unsigned)((size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t)),
                  displayShadow ? "PSRAM (partial frame pushes enabled)"
                                : "unavailable (full frame pushes)");

    frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    frameSprite.fillSprite(TFT_BLACK);
    pushDisplayFrame();
}

void displaySetSleeping(bool sleeping) {
    if (sleeping) {
#ifdef FNK0104N_3P5_320x480_ST77922
        tft_st77922.Write_Reg(0x28, nullptr, 0);  // Disable ST77922 pixel output before sleep-in.
        delay(10);
        tft_st77922.Write_Reg(0x10, nullptr, 0);  // Put the QSPI panel controller into sleep-in.
#else
        tft.writecommand(0x28);                   // Disable ILI9341/ST7796 pixel output first.
        delay(10);
        tft.writecommand(0x10);                   // Put the SPI panel controller into sleep-in.
#endif
        delay(10);
        digitalWrite(DOLL_DISPLAY_BACKLIGHT_PIN, LOW);  // Remove the largest visible power load.
        return;
    }

#ifdef FNK0104N_3P5_320x480_ST77922
    tft_st77922.Write_Reg(0x11, nullptr, 0);      // Wake the ST77922 controller without reinitializing RAM.
#else
    tft.writecommand(0x11);                       // Wake the ILI9341/ST7796 controller in place.
#endif
    delay(120);                                   // Panel datasheets require settling after sleep-out.
#ifdef FNK0104N_3P5_320x480_ST77922
    tft_st77922.Write_Reg(0x29, nullptr, 0);      // Re-enable ST77922 display output.
#else
    tft.writecommand(0x29);                       // Re-enable ILI9341/ST7796 display output.
#endif
    digitalWrite(DOLL_DISPLAY_BACKLIGHT_PIN, HIGH);  // Light the preserved frame immediately.
    displayInvalidateShadow();                    // Force the next render to resynchronize panel RAM.
    markDisplayDirty();                           // Repaint status after network and wake state change.
}

void drawDisplayBootSplash() {
    frameSprite.fillSprite(TFT_CYAN);
    const int splashWidth = min(DISPLAY_WIDTH - (DISPLAY_PADDING * 4), 180);
    const int splashHeight = 64;
    const int splashX = (DISPLAY_WIDTH - splashWidth) / 2;
    const int splashY = (DISPLAY_HEIGHT - splashHeight) / 2;
    frameSprite.fillRect(splashX, splashY, splashWidth, splashHeight, TFT_BLACK);
    frameSprite.setTextDatum(MC_DATUM);
    frameSprite.setTextColor(TFT_PINK, TFT_BLACK);
    frameSprite.setTextSize(2);
    frameSprite.drawString("DOLL-OS", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 - 10);
    frameSprite.setTextSize(1);
    frameSprite.drawString("booting...", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 + 14);
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
    frameSprite.setTextColor(TFT_PINK, TFT_BLACK);
    frameSprite.drawString("DOLL-OS", DISPLAY_PADDING, 2);

    char statusText[64];
    //MEM is ESP.getFreeHeap(), which this core defines as MALLOC_CAP_INTERNAL only --
    //PSR is the separate PSRAM pool the big buffers live in
    snprintf(statusText, sizeof(statusText), "MEM:%luKB PSR:%luKB VOL:%02d BAT:%d%%",
        (unsigned long)(ESP.getFreeHeap() / 1000), (unsigned long)(ESP.getFreePsram() / 1000),
        radioGetVolume(), readBatteryPercent());
    frameSprite.setTextDatum(TR_DATUM);
    frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    frameSprite.drawString(statusText, DISPLAY_WIDTH - DISPLAY_PADDING, 2);

    frameSprite.drawFastHLine(0, DISPLAY_STATUS_BAR_HEIGHT - 1, DISPLAY_WIDTH, TFT_PINK);
    frameSprite.setTextDatum(TL_DATUM);
}

void drawDisplayHistory() {
    const int lineHeight = 12;
    const int top = displayTerminalY();
    const int height = displayTerminalHeight();

    frameSprite.fillRect(0, top, DISPLAY_WIDTH, height, TFT_BLACK);
    if (displayHistoryCount == 0) {
        return;
    }

    frameSprite.setTextDatum(TL_DATUM);
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

    //blinking caret at the edit position. commandCursorPos indexes the full buffer; subtract
    //the dropped head to land in `shown`, then clamp so a cursor scrolled off the left edge
    //parks at the start of the visible window rather than drawing off-panel.
    if (((millis() / DISPLAY_CURSOR_BLINK_MS) & 1UL) == 0) {
        int caretInShown = commandCursorPos - (int)droppedFromHead;
        if (caretInShown < 0) caretInShown = 0;
        if (caretInShown > (int)shown.length()) caretInShown = shown.length();
        int caretX = textX + (int)frameSprite.textWidth(shown.substring(0, caretInShown));
        if (caretX > textX + maxWidth) caretX = textX + maxWidth;
        frameSprite.drawFastVLine(caretX, y + DISPLAY_PADDING, frameSprite.fontHeight(), TFT_WHITE);
    }
}

//   paints a running .dapp's CANVAS grid over the terminal area (AppRunner.ino's FLIP).
//   The grid is scaled to fill the area rather than drawn at a fixed cell size: a script
//   picks its playfield in cells and gets the biggest version of it the panel can show,
//   which is the only way a 10x20 well and an 80x24 status screen can both look right.
void drawDappCanvas() {
    const int top = displayTerminalY();
    const int height = displayTerminalHeight();
    frameSprite.fillRect(0, top, DISPLAY_WIDTH, height, TFT_BLACK);
    if (!dappCanvasCells || dappCanvasCols <= 0 || dappCanvasRows <= 0) {
        return;
    }

    const int cellW = max(1, (DISPLAY_WIDTH - DISPLAY_PADDING * 2) / dappCanvasCols);
    const int cellH = max(1, (height - DISPLAY_PADDING * 2) / dappCanvasRows);

    //the built-in font is 6x8 at size 1, so this is how many whole multiples of it fit in
    //a cell -- a small grid gets big glyphs instead of a lot of empty space
    int textSize = min(cellW / 6, cellH / 8);
    if (textSize < 1) textSize = 1;
    if (textSize > 4) textSize = 4;

    //center the grid in the area it didn't divide evenly into
    const int originX = (DISPLAY_WIDTH - cellW * dappCanvasCols) / 2;
    const int originY = top + (height - cellH * dappCanvasRows) / 2;

    frameSprite.setTextSize(textSize);
    frameSprite.setTextDatum(MC_DATUM);
    //drawString (not drawChar) because only drawString honours the datum, and a 2-byte
    //stack buffer keeps that from meaning a String allocation per cell per frame
    char glyph[2] = { ' ', '\0' };
    for (int row = 0; row < dappCanvasRows; row++) {
        for (int col = 0; col < dappCanvasCols; col++) {
            const DappCanvasCell& cell = dappCanvasCells[row * dappCanvasCols + col];
            if (cell.ch == ' ' || cell.ch == '\0') {
                continue;   //the area is already black; skipping blanks is most of the frame
            }
            //single-argument setTextColor draws no background box, so glyphs can't clip
            //their neighbours when a cell is narrower than the font
            glyph[0] = cell.ch;
            frameSprite.setTextColor(ansiCodeToPixelColor(cell.color));
            frameSprite.drawString(glyph,
                                   originX + col * cellW + cellW / 2,
                                   originY + row * cellH + cellH / 2);
        }
    }

    frameSprite.setTextSize(1);
    frameSprite.setTextDatum(TL_DATUM);
    frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);
}

void drawDisplayFrame() {
    unsigned long now = millis();
    bool statusRefreshDue = (now - displayLastStatusRefresh) >= DISPLAY_STATUS_REFRESH_MS;
    bool cursorPhase = ((now / DISPLAY_CURSOR_BLINK_MS) & 1UL) == 0;
    bool cursorBlinkDue = (cursorPhase != displayLastCursorPhase);
    if (!displayDirty && !statusRefreshDue && !cursorBlinkDue) {
        return;   //nothing changed since the last push -- skip the full-frame redraw + SPI blit
    }
    displayDirty = false;
    displayLastStatusRefresh = now;
    displayLastCursorPhase = cursorPhase;

    drawDisplayStatusBar();
    if (dappCanvasActive) {
        drawDappCanvas();
    } else {
        drawDisplayHistory();
    }
    drawDisplayCommandBar();
    pushDisplayFrame();
}
