//   TelnetServer.ino
//   Telnet server: connection handling, IAC negotiation, and the shared
//   line-editing input reader. This is the telnet-port replacement for DOLL-OS's
//   hardware.ino (readKeyboard) + terminal.ino's drawCommandBar -- together they
//   made up DOLL-OS's whole "regular input" path, which the user asked to replace
//   with telnet.
//
//   DOLL-OS's readKeyboard(String&) edited one buffer in place and returned true
//   on Enter; it was reused verbatim by the top-level shell, motoko's channel/
//   message prompts, and ssh's password prompt. readLineEditedInput() below plays
//   the same role: it negotiates the connecting client into character-at-a-time
//   mode (IAC WILL ECHO / WILL SUPPRESS-GO-AHEAD) once at connect time, then edits
//   whatever buffer its caller passes in. Telnet here is purely an input transport
//   -- the live view of the buffer is the TFT mirror (Display.ino, driven by
//   activeInputText/activeInputPrompt/activeInputMasked), not the telnet session
//   itself, so no edit is echoed back over the wire. WILL ECHO is still claimed
//   even though nothing is actually echoed: dropping it would flip well-behaved
//   clients back into local line-buffered mode, which would break real-time
//   history recall and, worse, make ssh's masked password prompt echo in cleartext
//   client-side. Up/Down arrows recall command history exactly like DOLL-OS's
//   Fn+;/Fn+. did (and, matching DOLL-OS's own behavior, that history is the single
//   shared commandHistory ring buffer regardless of which buffer is being edited --
//   ssh's password prompt and motoko's chat compose recall shell command history,
//   same as they did on M5Cardputer); Left/Right move the cursor; Ctrl+C cancels
//   the in-progress line.

//telnet protocol bytes (RFC 854)
static const uint8_t IAC  = 255;
static const uint8_t DONT = 254;
static const uint8_t DO   = 253;
static const uint8_t WONT = 252;
static const uint8_t WILL = 251;
static const uint8_t SB   = 250;
static const uint8_t SE   = 240;

static const uint8_t OPT_ECHO       = 1;
static const uint8_t OPT_SGA        = 3;   //suppress go-ahead
static const uint8_t OPT_LINEMODE   = 34;

enum UserIacState { UIAC_NORMAL, UIAC_GOT_IAC, UIAC_GOT_CMD, UIAC_GOT_SB, UIAC_GOT_SB_IAC };
static UserIacState userIacState = UIAC_NORMAL;

//escape/CSI/CRLF parsing state for the telnet client's own line editing. IAC state
//(userIacState above) is telnet-transport-specific and stays separate; this part is
//the generic line-edit state now shared with KeyboardSerial.ino via LineEditState.
static LineEditState telnetLineState;

void resetTelnetInputState() {
    userIacState = UIAC_NORMAL;
    telnetLineState = LineEditState();
}

//negotiates the connecting client into character-at-a-time mode with server-side
//echo -- this is the mirror image of what DOLL-OS's own outbound telnet client
//(TelnetClient.ino) looks for from servers it dials into. Sent once per connection.
void telnetNegotiateServerMode() {
    uint8_t willEcho[3] = { IAC, WILL, OPT_ECHO };
    uint8_t willSga[3]  = { IAC, WILL, OPT_SGA };
    uint8_t dontLine[3] = { IAC, DONT, OPT_LINEMODE };
    telnetClient.write(willEcho, 3);
    telnetClient.write(willSga, 3);
    telnetClient.write(dontLine, 3);
}

//reads the next byte from telnetClient with IAC negotiation sequences transparently
//consumed (no reply sent here -- our own options were already offered once at
//connect time in telnetNegotiateServerMode). Returns -1 when nothing is available
//right now. Shared by readLineEditedInput() below and by readRawUserBytes()
//(RemoteSession.ino).
int telnetReadFilteredByte() {
    while (telnetClient.available() > 0) {
        uint8_t b = (uint8_t)telnetClient.read();
        switch (userIacState) {
            case UIAC_NORMAL:
                if (b == IAC) {
                    userIacState = UIAC_GOT_IAC;
                    continue;
                }
                ledPulseInput();
                return b;

            case UIAC_GOT_IAC:
                if (b == IAC) {              //escaped 0xFF data byte
                    userIacState = UIAC_NORMAL;
                    ledPulseInput();
                    return 0xFF;
                }
                if (b == SB) {
                    userIacState = UIAC_GOT_SB;
                } else if (b == WILL || b == WONT || b == DO || b == DONT) {
                    userIacState = UIAC_GOT_CMD;
                } else {
                    userIacState = UIAC_NORMAL;   //NOP/AYT/GA/etc -- no option byte follows
                }
                continue;

            case UIAC_GOT_CMD:
                userIacState = UIAC_NORMAL;   //consume the option byte
                continue;

            case UIAC_GOT_SB:
                if (b == IAC) {
                    userIacState = UIAC_GOT_SB_IAC;
                }
                continue;

            case UIAC_GOT_SB_IAC:
                userIacState = (b == SE) ? UIAC_NORMAL : UIAC_GOT_SB;
                continue;
        }
    }
    return -1;
}

//editing helpers below only mutate the buffer/cursor -- no wire writes. The TFT
//mirror (Display.ino) is repainted by each call site right after readLineEditedInput()
//returns, so it's the only place these edits become visible.
static void insertChar(String& text, char ch) {
    if (text.length() >= COMMAND_MAX_LEN) {
        return;
    }
    text = text.substring(0, commandCursorPos) + ch + text.substring(commandCursorPos);
    commandCursorPos++;
}

static void backspaceChar(String& text) {
    if (commandCursorPos <= 0) {
        return;
    }
    text.remove(commandCursorPos - 1, 1);
    commandCursorPos--;
}

static void deleteForwardChar(String& text) {
    if (commandCursorPos >= (int)text.length()) {
        return;
    }
    text.remove(commandCursorPos, 1);
}

static void moveCursorLeft() {
    if (commandCursorPos <= 0) {
        return;
    }
    commandCursorPos--;
}

static void moveCursorRight(const String& text) {
    if (commandCursorPos >= (int)text.length()) {
        return;
    }
    commandCursorPos++;
}

static void historyRecall(String& text, int step) {
    recallCommandHistory(step, text);
    commandCursorPos = text.length();
}

static void cancelLine(String& text) {
    text = "";
    commandCursorPos = 0;
    commandHistoryIndex = -1;
}

//pending CSI final byte, decoded against whichever buffer is currently being edited
static void handleCsiSequence(String& text, const String& params, char finalByte) {
    //Ctrl+Up/Ctrl+Down (most terminals: ESC[1;5A / ESC[1;5B) nudge radio volume, and
    //Shift+Up/Down (ESC[1;2A / ESC[1;2B) scroll the mirrored terminal history -- both
    //checked first so they take priority over the plain-arrow case below
    if (params == "1;5" && (finalByte == 'A' || finalByte == 'B')) {
        radioAdjustVolume(finalByte == 'A' ? 1 : -1);
    } else if (params == "1;2" && (finalByte == 'A' || finalByte == 'B')) {
        displayScrollBy(finalByte == 'A' ? 1 : -1);
    } else if (finalByte == 'A') {
        historyRecall(text, -1);
    } else if (finalByte == 'B') {
        historyRecall(text, 1);
    } else if (finalByte == 'C') {
        moveCursorRight(text);
    } else if (finalByte == 'D') {
        moveCursorLeft();
    } else if (finalByte == '~' && params == "3") {
        deleteForwardChar(text);
    }
    //other CSI sequences (Home/End/PgUp/PgDn/function keys) have no local
    //binding yet -- silently dropped, same policy DOLL-OS's ansi.ino used
}

//Generic line editor -- the shared input primitive every buffered-input feature
//(top-level shell, motoko's channel/message prompts, ssh's password prompt) reads
//through, exactly like DOLL-OS's readKeyboard(). Reads and consumes at most one
//semantic input unit (one printable char, one control action, or one escape
//sequence) per call. `text` is the live buffer being edited, indexed by the shared
//commandCursorPos (global.h) -- only one buffer is ever "active" at a time, same
//assumption DOLL-OS's single physical keyboard made. Edits are never echoed back
//over the wire (see the file header comment) -- callers repaint the TFT mirror
//themselves right after each call.
//applies one already-fetched input byte to `text`, threading escape/CR context through
//`st` so each input source (telnet, keyboard bridge) keeps its own parse. Byte-source
//agnostic: the telnet-transport concerns (IAC filtering) are handled before a byte ever
//reaches here. DS-Slave's UART bridge emits exactly this byte vocabulary -- printable
//ASCII, CR for Enter, 0x08 for Backspace, and ESC/CSI for arrows/history/Delete (see
//DS-Slave.ino emitKey()) -- which is why the BLE keyboard can share this editor verbatim.
LineInputResult processLineEditByte(String& text, uint8_t ch, LineEditState& st, bool echoCrlfToTelnet) {
    if (st.escState == UESC_GOT_ESC) {
        if (ch == '[') {
            st.escState = UESC_GOT_CSI;
            st.escParams = "";
        } else {
            st.escState = UESC_NONE;
        }
        return LINE_EDITING;
    }
    if (st.escState == UESC_GOT_CSI) {
        if (ch >= 0x40 && ch <= 0x7E) {
            handleCsiSequence(text, st.escParams, (char)ch);
            st.escState = UESC_NONE;
        } else {
            st.escParams += (char)ch;
        }
        return LINE_EDITING;
    }
    if (ch == 0x1B) {
        st.escState = UESC_GOT_ESC;
        return LINE_EDITING;
    }

    if (ch == '\r') {
        st.lastByteWasCR = true;
        if (echoCrlfToTelnet) {
            telnetClient.print("\r\n");
        }
        return LINE_SUBMITTED;
    }
    if (ch == '\n') {
        if (st.lastByteWasCR) {
            st.lastByteWasCR = false;   //swallow the LF half of a CRLF pair already handled above
            return LINE_EDITING;
        }
        if (echoCrlfToTelnet) {
            telnetClient.print("\r\n");
        }
        return LINE_SUBMITTED;
    }
    st.lastByteWasCR = false;

    if (ch == 0x03) {   //Ctrl+C
        cancelLine(text);
        return LINE_EDITING;
    }
    if (ch == 0x08 || ch == 0x7F) {   //backspace / DEL
        backspaceChar(text);
        return LINE_EDITING;
    }
    if (ch >= 0x20 && ch < 0x7F) {   //printable ASCII
        insertChar(text, (char)ch);
        return LINE_EDITING;
    }
    //other control bytes: no local binding, dropped
    return LINE_EDITING;
}

LineInputResult readLineEditedInput(String& text) {
    int raw = telnetReadFilteredByte();
    if (raw == -1) {
        return LINE_NO_INPUT;
    }
    return processLineEditByte(text, (uint8_t)raw, telnetLineState, true);
}

//clears the screen and prints the welcome banner to every active surface (the telnet
//client if one is connected, and always the panel mirror). Shared by boot (DS.ino, so
//the shell is live on the panel and BLE keyboard the moment setup() finishes -- no
//telnet client required) and by acceptTelnetClient() (so a dialing-in client gets the
//same fresh screen). Does not print the prompt -- callers do that so they can add their
//own lines in between first.
void beginShellSession() {
    outClearScreen();
    outLine("===============================", C_PINK);
    outLine("          Doll-Screen", C_PINK);
    outLine("===============================", C_PINK);
    outLine("");
    outLine("Type help for available commands.");
    outLine("");
}

void acceptTelnetClient() {
    WiFiClient newClient = telnetServer.accept();
    if (!newClient) {
        return;
    }

    if (telnetClient && telnetClient.connected()) {
        newClient.println("The terminal is already in use.");
        newClient.stop();
        return;
    }

    telnetClient = newClient;
    telnetClient.setNoDelay(true);
    ledSetTelnetConnected(true);
    ledPulseNetwork();

    currentCommand = "";
    commandCursorPos = 0;
    commandHistoryIndex = -1;
    resetTelnetInputState();

    Serial.println("Telnet client connected.");

    telnetNegotiateServerMode();

    beginShellSession();
    setActiveInput(shellPrompt(), "", false);
    printPrompt();
}

void readTelnetClient() {
    if (!telnetClient) {
        return;
    }

    if (!telnetClient.connected()) {
        Serial.println("Telnet client disconnected.");
        telnetClient.stop();
        ledSetTelnetConnected(false);
        currentCommand = "";
        commandCursorPos = 0;
        return;
    }

    while (true) {
        int raw = telnetReadFilteredByte();
        if (raw == -1) {
            break;
        }
        //same terminate chord the keyboard bridge offers the library first: at the prompt
        //there is no app to receive it, so it stops a track playing behind the shell
        if (musicHandleShellTerminate((uint8_t)raw)) {
            continue;
        }
        //not readLineEditedInput(): the shell submits with echoCrlfToTelnet=false so the
        //newline comes from echoCommandLine() (Output.ino) instead, which finishes the
        //prompt line with the command that was typed. The other line-edited prompts
        //(motoko, ssh) have nothing to echo and keep the plain CRLF.
        LineInputResult r = processLineEditByte(currentCommand, (uint8_t)raw, telnetLineState, false);
        setActiveInput(shellPrompt(), currentCommand, false);
        if (r == LINE_SUBMITTED) {
            commandProcessor(currentCommand);
            setActiveInput(shellPrompt(), currentCommand, false);   //commandProcessor() clears the buffer, and
                                                                     //a "cd" just moved the prompt -- reflect both
            printPrompt();
        }
        //LINE_EDITING: loop again in case more bytes are already buffered this tick
    }
}
