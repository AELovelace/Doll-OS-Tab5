//   RemoteSession.ino
//   shared modal loop for character-oriented remote sessions (ssh shell, outbound
//   telnet client) -- see the RemoteSession class declaration in global.h.
//
//   Ported from DOLL-OS's RemoteSession.ino. There, "local" meant the M5Cardputer
//   keyboard (input) and terminal sprite (output); here "local" means the one
//   connected telnetClient in both directions -- pumpIncoming() writes remote bytes
//   straight to it, and readRawUserBytes() below reads the user's raw keystrokes
//   back off it. DOLL-OS's local escape chord was Fn+Q, a physical key combo with
//   no telnet equivalent; this uses Ctrl+T (0x14) instead, chosen because it isn't
//   commonly bound by remote shells or BBS-style line editors the way Ctrl+C/D/Z are.

//escape-sequence buffer for the raw-session byte path, separate from the shell line
//editor's LineEditState (TelnetServer.ino) -- this classifier only needs to recognize
//Ctrl+Up/Ctrl+Down (ESC[1;5A / ESC[1;5B) as a volume nudge; every other escape sequence
//(plain arrows, Home/End, the remote's own function-key bindings, ...) is buffered here
//just long enough to confirm it *isn't* that one, then flushed through to the remote
//unmodified so it keeps working exactly as before this change. One tradeoff: a lone ESC
//keypress (e.g. leaving insert mode in a remote vim session) is held until the next byte
//arrives rather than forwarded instantly, same latency the shell's own line editor already
//accepts for CSI parsing -- there's no per-byte timeout anywhere in this input path.
static UserEscState rawEscState = UESC_NONE;
static String rawEscBuffer = "";

//reads one raw byte from telnetClient (IAC already stripped by telnetReadFilteredByte,
//TelnetServer.ino) and classifies it exactly like DOLL-OS's readRawKeyBytes() did for
//keyboard events: normal bytes are forwarded as-is, Ctrl+T sets escapePressed, Ctrl+K sets
//cmdModePressed, backspace/DEL sets backspacePressed instead of being forwarded raw (so
//each transport can translate it through its own backspaceBytes() override), and a
//recognized Ctrl+Up/Down escape sequence is consumed silently (see radioAdjustVolume,
//Radio.ino) instead of being forwarded at all.
bool readRawUserBytes(String& outBytes, bool& escapePressed, bool& backspacePressed, bool& cmdModePressed) {
    outBytes = "";
    escapePressed = false;
    backspacePressed = false;
    cmdModePressed = false;

    int raw = telnetReadFilteredByte();
    if (raw == -1) {
        raw = keyboardReadRawByte();   //no telnet byte waiting -- fall back to the BLE-keyboard
                                        //bridge, so the session is drivable with no telnet client
    }
    if (raw == -1) {
        return false;
    }
    uint8_t b = (uint8_t)raw;

    if (rawEscState == UESC_GOT_ESC) {
        rawEscBuffer += (char)b;
        if (b == '[') {
            rawEscState = UESC_GOT_CSI;
            return false;
        }
        //not a CSI sequence after all -- flush the two bytes seen (ESC + this one) as-is
        outBytes = rawEscBuffer;
        rawEscState = UESC_NONE;
        rawEscBuffer = "";
        return true;
    }
    if (rawEscState == UESC_GOT_CSI) {
        rawEscBuffer += (char)b;
        if (b < 0x40 || b > 0x7E) {
            return false;   //still accumulating params
        }
        //final byte reached -- params are whatever's between "ESC[" and it
        String params = rawEscBuffer.substring(2, rawEscBuffer.length() - 1);
        String fullSequence = rawEscBuffer;
        rawEscState = UESC_NONE;
        rawEscBuffer = "";
        if (params == "1;5" && (b == 'A' || b == 'B')) {
            radioAdjustVolume(b == 'A' ? 1 : -1);
            return false;   //consumed -- not forwarded to the remote
        }
        if (params == "1;2" && (b == 'A' || b == 'B')) {
            displayScrollBy(b == 'A' ? 1 : -1);
            return false;   //consumed -- not forwarded to the remote
        }
        outBytes = fullSequence;
        return true;
    }

    if (b == 0x1B) {   //possible start of an escape sequence -- buffer until it resolves
        rawEscState = UESC_GOT_ESC;
        rawEscBuffer = "\x1b";
        return false;
    }
    if (b == 0x14) {   //Ctrl+T: local "detach" chord
        escapePressed = true;
        return false;
    }
    if (b == 0x0B) {   //Ctrl+K: local "run one command without detaching" chord
        cmdModePressed = true;
        return false;
    }
    if (b == 0x08 || b == 0x7F) {
        backspacePressed = true;
        return false;
    }

    outBytes += (char)b;
    return true;
}

void RemoteSession::run() {
    bool closedNotified = false;

    while (true) {
        delay(2);

        pumpIncoming();
        ledService();
        drawDisplayFrame();   //this loop never returns to DS.ino's loop() until the
                               //session ends, so the mirror has to repaint itself here too

        if (isClosed()) {
            if (!closedNotified) {
                onClosed();
                closedNotified = true;
            }
            break;
        }
        //note: a dropped telnet client no longer ends the session -- the panel mirror and the
        //BLE keyboard are a complete UI on their own, so the session lives until the remote
        //closes (isClosed above) or the user sends the Ctrl+T detach chord. pumpIncoming()
        //already guards its own writes to telnetClient with a connected() check.

        String rawOut;
        bool escapePressed;
        bool backspacePressed;
        bool cmdModePressed;
        if (readRawUserBytes(rawOut, escapePressed, backspacePressed, cmdModePressed)) {
            sendBytes(rawOut);
        } else if (escapePressed) {
            break;
        } else if (backspacePressed) {
            sendBytes(backspaceBytes());
        } else if (cmdModePressed) {
            runInlineCommandPrompt();
        }
    }

    //final drain so output already in flight isn't lost when the loop exits
    pumpIncoming();
}

//pauses raw byte forwarding just long enough for the user to type and run one full shell
//command (e.g. "radio play <url>") via Ctrl+K, then resumes forwarding untouched -- same
//shape as Ssh.ino's password prompt (alternating readLineEditedInput()/
//readKeyboardLineEditedInput() with drawDisplayFrame() in a blocking loop) but dispatches
//through commandProcessor() instead of consuming a password, and returns to the raw
//session afterward instead of moving on to a new phase. commandProcessor() already no-ops
//on an empty line, so submitting with nothing typed is the "back out" gesture.
void RemoteSession::runInlineCommandPrompt() {
    String cmdBuffer = "";
    LineEditState cmdLineState;   //dedicated state so a half-typed inline command can't
                                   //tangle with the main shell's telnetLineState/keyboardLineState
    commandCursorPos = 0;
    setActiveInput("cmd> ", cmdBuffer, false);

    while (true) {
        delay(2);
        pumpIncoming();
        ledService();
        drawDisplayFrame();
        if (isClosed()) {
            return;   //remote died mid-typing -- discard whatever was half-entered
        }

        int raw = telnetReadFilteredByte();
        LineInputResult r;
        if (raw != -1) {
            r = processLineEditByte(cmdBuffer, (uint8_t)raw, cmdLineState, true);
        } else {
            raw = keyboardReadRawByte();
            if (raw == -1) {
                continue;
            }
            r = processLineEditByte(cmdBuffer, (uint8_t)raw, cmdLineState, false);
        }
        setActiveInput("cmd> ", cmdBuffer, false);
        if (r == LINE_SUBMITTED) {
            break;
        }
    }

    commandProcessor(cmdBuffer);
}
