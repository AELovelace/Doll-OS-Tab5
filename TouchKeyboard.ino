// TouchKeyboard.ino
// Portrait-mode status toggle and a local QWERTY keyboard. Every key is encoded
// as the same terminal-byte vocabulary used by USB and the official Tab5
// keyboard, so shell prompts, modal prompts, SSH/telnet sessions, and .dapp
// input all keep their existing input path.

#define TK_CHAR(lo, hi) { lo, hi, lo, hi, TKA_TEXT, 2 }
#define TK_TEXT(label, bytes, units) { label, label, bytes, bytes, TKA_TEXT, units }
#define TK_ACTION(label, action, units) { label, label, "", "", action, units }

static const TouchKeySpec touchRow0[] = {
    TK_CHAR("1", "!"), TK_CHAR("2", "@"), TK_CHAR("3", "#"), TK_CHAR("4", "$"),
    TK_CHAR("5", "%"), TK_CHAR("6", "^"), TK_CHAR("7", "&"), TK_CHAR("8", "*"),
    TK_CHAR("9", "("), TK_CHAR("0", ")"), TK_CHAR("-", "_"), TK_CHAR("=", "+"),
};
static const TouchKeySpec touchRow1[] = {
    TK_CHAR("q", "Q"), TK_CHAR("w", "W"), TK_CHAR("e", "E"), TK_CHAR("r", "R"),
    TK_CHAR("t", "T"), TK_CHAR("y", "Y"), TK_CHAR("u", "U"), TK_CHAR("i", "I"),
    TK_CHAR("o", "O"), TK_CHAR("p", "P"),
};
static const TouchKeySpec touchRow2[] = {
    TK_CHAR("a", "A"), TK_CHAR("s", "S"), TK_CHAR("d", "D"), TK_CHAR("f", "F"),
    TK_CHAR("g", "G"), TK_CHAR("h", "H"), TK_CHAR("j", "J"), TK_CHAR("k", "K"),
    TK_CHAR("l", "L"), TK_CHAR(";", ":"), TK_CHAR("'", "\""),
};
static const TouchKeySpec touchRow3[] = {
    TK_ACTION("SHIFT", TKA_SHIFT, 3),
    TK_CHAR("z", "Z"), TK_CHAR("x", "X"), TK_CHAR("c", "C"), TK_CHAR("v", "V"),
    TK_CHAR("b", "B"), TK_CHAR("n", "N"), TK_CHAR("m", "M"),
    TK_CHAR(",", "<"), TK_CHAR(".", ">"),
    TK_ACTION("BKSP", TKA_BACKSPACE, 3),
};
static const TouchKeySpec touchRow4[] = {
    TK_ACTION("SYM", TKA_SYMBOLS, 3), TK_ACTION("ESC", TKA_ESCAPE, 2),
    TK_ACTION("CTRL", TKA_CTRL, 3), TK_ACTION("SPACE", TKA_SPACE, 7),
    TK_TEXT("/", "/", 2), TK_ACTION("<", TKA_LEFT, 2),
    TK_ACTION(">", TKA_RIGHT, 2), TK_ACTION("ENTER", TKA_ENTER, 4),
};

static const TouchKeySpec touchSymRow0[] = {
    TK_TEXT("!", "!", 2), TK_TEXT("@", "@", 2), TK_TEXT("#", "#", 2),
    TK_TEXT("$", "$", 2), TK_TEXT("%", "%", 2), TK_TEXT("^", "^", 2),
    TK_TEXT("&", "&", 2), TK_TEXT("*", "*", 2), TK_TEXT("(", "(", 2),
    TK_TEXT(")", ")", 2), TK_TEXT("_", "_", 2), TK_TEXT("+", "+", 2),
};
static const TouchKeySpec touchSymRow1[] = {
    TK_TEXT("[", "[", 2), TK_TEXT("]", "]", 2), TK_TEXT("{", "{", 2),
    TK_TEXT("}", "}", 2), TK_TEXT("<", "<", 2), TK_TEXT(">", ">", 2),
    TK_TEXT("/", "/", 2), TK_TEXT("\\", "\\", 2), TK_TEXT("|", "|", 2),
    TK_TEXT("`", "`", 2), TK_TEXT("~", "~", 2),
};
static const TouchKeySpec touchSymRow2[] = {
    TK_TEXT(";", ";", 2), TK_TEXT(":", ":", 2), TK_TEXT("'", "'", 2),
    TK_TEXT("\"", "\"", 2), TK_TEXT(",", ",", 2), TK_TEXT(".", ".", 2),
    TK_TEXT("?", "?", 2), TK_TEXT("-", "-", 2), TK_TEXT("=", "=", 2),
};
static const TouchKeySpec touchSymRow3[] = {
    TK_TEXT("1", "1", 2), TK_TEXT("2", "2", 2), TK_TEXT("3", "3", 2),
    TK_TEXT("4", "4", 2), TK_TEXT("5", "5", 2), TK_TEXT("6", "6", 2),
    TK_TEXT("7", "7", 2), TK_TEXT("8", "8", 2), TK_TEXT("9", "9", 2),
    TK_TEXT("0", "0", 2), TK_ACTION("BKSP", TKA_BACKSPACE, 3),
};
static const TouchKeySpec touchSymRow4[] = {
    TK_ACTION("ABC", TKA_SYMBOLS, 3), TK_ACTION("ESC", TKA_ESCAPE, 2),
    TK_ACTION("CTRL", TKA_CTRL, 3), TK_ACTION("SPACE", TKA_SPACE, 7),
    TK_TEXT("/", "/", 2), TK_ACTION("<", TKA_LEFT, 2),
    TK_ACTION(">", TKA_RIGHT, 2), TK_ACTION("ENTER", TKA_ENTER, 4),
};

#undef TK_CHAR
#undef TK_TEXT
#undef TK_ACTION

static bool touchShift = false;
static bool touchSymbols = false;
static bool touchCtrl = false;
static bool touchWasDown = false;
static int touchPressedKey = -1;
static uint32_t touchPressedAt = 0;
static uint32_t touchRepeatedAt = 0;

static constexpr int TOUCH_KEY_ROWS = 5;
static constexpr int TOUCH_KEY_GAP = 4;
static constexpr int TOUCH_KEY_PAD = 5;
static constexpr int TOUCH_MODE_BUTTON_W = 76;
static constexpr int TOUCH_MODE_BUTTON_H = 25;
static constexpr int TOUCH_MODE_BUTTON_Y = 3;

static void touchModeButtonRect(int& x, int& y, int& w, int& h) {
    displayUseTerminalTextSize();
    x = DISPLAY_PADDING + frameSprite.textWidth("DOLL-OS") + 8;
    y = TOUCH_MODE_BUTTON_Y;
    w = TOUCH_MODE_BUTTON_W;
    h = TOUCH_MODE_BUTTON_H;
}

static void touchGetRow(int row, const TouchKeySpec*& keys, size_t& count) {
    if (touchSymbols) {
        switch (row) {
            case 0: keys = touchSymRow0; count = sizeof(touchSymRow0) / sizeof(*touchSymRow0); return;
            case 1: keys = touchSymRow1; count = sizeof(touchSymRow1) / sizeof(*touchSymRow1); return;
            case 2: keys = touchSymRow2; count = sizeof(touchSymRow2) / sizeof(*touchSymRow2); return;
            case 3: keys = touchSymRow3; count = sizeof(touchSymRow3) / sizeof(*touchSymRow3); return;
            default: keys = touchSymRow4; count = sizeof(touchSymRow4) / sizeof(*touchSymRow4); return;
        }
    }
    switch (row) {
        case 0: keys = touchRow0; count = sizeof(touchRow0) / sizeof(*touchRow0); return;
        case 1: keys = touchRow1; count = sizeof(touchRow1) / sizeof(*touchRow1); return;
        case 2: keys = touchRow2; count = sizeof(touchRow2) / sizeof(*touchRow2); return;
        case 3: keys = touchRow3; count = sizeof(touchRow3) / sizeof(*touchRow3); return;
        default: keys = touchRow4; count = sizeof(touchRow4) / sizeof(*touchRow4); return;
    }
}

static bool touchKeyRect(int wantedRow, int wantedKey,
                         int& x, int& y, int& w, int& h) {
    const TouchKeySpec* keys = nullptr;
    size_t count = 0;
    touchGetRow(wantedRow, keys, count);
    if (wantedKey < 0 || wantedKey >= (int)count) return false;

    int totalUnits = 0;
    for (size_t i = 0; i < count; ++i) totalUnits += keys[i].units;
    const int keyboardTop = displayHeight() - displayTouchKeyboardHeight();
    const int innerWidth = displayWidth() - TOUCH_KEY_PAD * 2;
    const int gapsWidth = TOUCH_KEY_GAP * ((int)count - 1);
    const int usableWidth = innerWidth - gapsWidth;
    h = (displayTouchKeyboardHeight() - TOUCH_KEY_PAD * 2
        - TOUCH_KEY_GAP * (TOUCH_KEY_ROWS - 1)) / TOUCH_KEY_ROWS;
    y = keyboardTop + TOUCH_KEY_PAD + wantedRow * (h + TOUCH_KEY_GAP);

    int unitBefore = 0;
    for (int i = 0; i < wantedKey; ++i) unitBefore += keys[i].units;
    const int unitThrough = unitBefore + keys[wantedKey].units;
    x = TOUCH_KEY_PAD + wantedKey * TOUCH_KEY_GAP
        + (usableWidth * unitBefore) / totalUnits;
    const int right = TOUCH_KEY_PAD + wantedKey * TOUCH_KEY_GAP
        + (usableWidth * unitThrough) / totalUnits;
    w = max(1, right - x);
    return true;
}

static int touchHitKey(int x, int y, const TouchKeySpec*& hit) {
    for (int row = 0; row < TOUCH_KEY_ROWS; ++row) {
        const TouchKeySpec* keys = nullptr;
        size_t count = 0;
        touchGetRow(row, keys, count);
        for (int key = 0; key < (int)count; ++key) {
            int keyX, keyY, keyW, keyH;
            if (!touchKeyRect(row, key, keyX, keyY, keyW, keyH)) continue;
            if (x >= keyX && x < keyX + keyW && y >= keyY && y < keyY + keyH) {
                hit = &keys[key];
                return row * 32 + key;
            }
        }
    }
    hit = nullptr;
    return -1;
}

static void touchEmitKey(const TouchKeySpec& key) {
    static const uint8_t escape[] = { 0x1B };
    static const uint8_t left[] = { 0x1B, '[', 'D' };
    static const uint8_t right[] = { 0x1B, '[', 'C' };
    switch (key.action) {
        case TKA_SHIFT:
            touchShift = !touchShift;
            markDisplayDirty();
            return;
        case TKA_SYMBOLS:
            touchSymbols = !touchSymbols;
            touchShift = false;
            markDisplayDirty();
            return;
        case TKA_CTRL:
            touchCtrl = !touchCtrl;
            markDisplayDirty();
            return;
        case TKA_BACKSPACE:
            keyboardInjectByte(0x08);
            return;
        case TKA_ENTER:
            keyboardInjectByte('\r');
            return;
        case TKA_ESCAPE:
            keyboardInjectBytes(escape, sizeof(escape));
            return;
        case TKA_LEFT:
            keyboardInjectBytes(left, sizeof(left));
            return;
        case TKA_RIGHT:
            keyboardInjectBytes(right, sizeof(right));
            return;
        case TKA_SPACE:
            keyboardInjectByte(' ');
            return;
        case TKA_TEXT:
            break;
    }

    const char* bytes = touchShift ? key.shiftedBytes : key.bytes;
    if (bytes && bytes[0]) {
        uint8_t value = static_cast<uint8_t>(bytes[0]);
        if (touchCtrl && value >= '@' && value <= '_') value &= 0x1F;
        else if (touchCtrl && value >= 'a' && value <= 'z') value = value - 'a' + 1;
        keyboardInjectByte(value);
    }
    if (touchShift || touchCtrl) {
        touchShift = false;
        touchCtrl = false;
        markDisplayDirty();
    }
}

void drawTouchKeyboard() {
    if (!dappCanvasActive) {
        int buttonX, buttonY, buttonW, buttonH;
        touchModeButtonRect(buttonX, buttonY, buttonW, buttonH);
        const uint16_t modeFill = displayIsPortrait() ? 0x4008 : 0x2104;
        frameSprite.fillRoundRect(buttonX, buttonY, buttonW, buttonH, 7, modeFill);
        frameSprite.drawRoundRect(buttonX, buttonY, buttonW, buttonH, 7, TFT_PINK);
        frameSprite.setTextDatum(MC_DATUM);
        frameSprite.setTextSize(1);
        frameSprite.setTextColor(TFT_WHITE);
        frameSprite.drawString(displayIsPortrait() ? "LAND" : "PORT",
                               buttonX + buttonW / 2, buttonY + buttonH / 2);
    }

    if (!displayIsPortrait()) {
        frameSprite.setTextDatum(TL_DATUM);
        displayUseTerminalTextSize();
        return;
    }

    const int keyboardTop = displayHeight() - displayTouchKeyboardHeight();
    frameSprite.fillRect(0, keyboardTop, displayWidth(),
                         displayTouchKeyboardHeight(), 0x1082);
    frameSprite.drawFastHLine(0, keyboardTop, displayWidth(), TFT_PINK);
    frameSprite.setTextDatum(MC_DATUM);

    for (int row = 0; row < TOUCH_KEY_ROWS; ++row) {
        const TouchKeySpec* keys = nullptr;
        size_t count = 0;
        touchGetRow(row, keys, count);
        for (int key = 0; key < (int)count; ++key) {
            int x, y, w, h;
            touchKeyRect(row, key, x, y, w, h);
            const int keyId = row * 32 + key;
            const bool latched = (keys[key].action == TKA_SHIFT && touchShift)
                || (keys[key].action == TKA_CTRL && touchCtrl)
                || (keys[key].action == TKA_SYMBOLS && touchSymbols);
            const bool pressed = keyId == touchPressedKey;
            const uint16_t fill = pressed ? 0x7BEF : (latched ? 0x4010 : 0x2104);
            const uint16_t edge = latched ? TFT_PINK : TFT_CYAN;
            frameSprite.fillRoundRect(x, y, w, h, 8, fill);
            frameSprite.drawRoundRect(x, y, w, h, 8, edge);
            const char* label = touchShift && keys[key].shiftedLabel
                ? keys[key].shiftedLabel : keys[key].label;
            frameSprite.setTextColor(TFT_WHITE);
            frameSprite.setTextSize((strlen(label) > 3 || w < 44) ? 1 : 2);
            frameSprite.drawString(label, x + w / 2, y + h / 2);
        }
    }
    frameSprite.setTextDatum(TL_DATUM);
    displayUseTerminalTextSize();
    frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);
}

void touchKeyboardService() {
    static bool servicing = false;
    if (servicing) return;
    servicing = true;
    M5.update();

    bool down = false;
    int x = -1, y = -1;
    const uint8_t count = M5.Touch.getCount();
    for (uint8_t i = 0; i < count; ++i) {
        const auto& touch = M5.Touch.getDetail(i);
        if (!touch.isPressed()) continue;
        down = true;
        x = touch.x;
        y = touch.y;
        break;
    }

    if (!down) {
        if (touchPressedKey >= 0) markDisplayDirty();
        touchPressedKey = -1;
        touchWasDown = false;
        servicing = false;
        return;
    }

    if (!touchWasDown) {
        touchWasDown = true;
        int buttonX, buttonY, buttonW, buttonH;
        touchModeButtonRect(buttonX, buttonY, buttonW, buttonH);
        if (!dappCanvasActive && x >= buttonX && x < buttonX + buttonW
            && y >= buttonY && y < buttonY + buttonH) {
            displaySetPortrait(!displayIsPortrait());
            touchPressedKey = -1;
            servicing = false;
            return;
        }

        if (displayIsPortrait()) {
            const TouchKeySpec* key = nullptr;
            touchPressedKey = touchHitKey(x, y, key);
            if (key) {
                touchPressedAt = millis();
                touchRepeatedAt = touchPressedAt;
                touchEmitKey(*key);
                markDisplayDirty();
            }
        }
    } else if (touchPressedKey >= 0 && millis() - touchPressedAt >= 500
               && millis() - touchRepeatedAt >= 80) {
        const int row = touchPressedKey / 32;
        const int keyIndex = touchPressedKey % 32;
        const TouchKeySpec* keys = nullptr;
        size_t keyCount = 0;
        touchGetRow(row, keys, keyCount);
        if (keyIndex < (int)keyCount && keys[keyIndex].action == TKA_BACKSPACE) {
            keyboardInjectByte(0x08);
            touchRepeatedAt = millis();
        }
    }
    servicing = false;
}
