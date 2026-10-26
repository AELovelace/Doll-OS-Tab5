//   KeyboardSerial.ino
//   Second UART that receives keystrokes from DS-Slave -- the companion ESP32-S3 that
//   bridges a BLE HID keyboard to a serial line (see ../DS-Slave/DS-Slave.ino). DS-Slave
//   decodes each BLE key report into a plain byte stream: printable ASCII, CR for Enter,
//   0x08 for Backspace, and ESC/CSI sequences for the arrows/Home/End/Delete/function
//   keys. That is exactly the vocabulary DOLL-OS's telnet line editor already speaks, so the
//   received bytes are fed straight into the shared processLineEditByte() (TelnetServer.ino)
//   -- the BLE keyboard becomes a second way to drive the shell, working with or without
//   a telnet client attached.
//   DS-Slave can also send private out-of-band controls: 0xF4 = volume up,
//   0xF5 = volume down, 0xF6 = paired sleep, and 0xF7 = wake beacon, plus one byte
//   per button-bar press -- 0xF8 Start, 0xF9 Select, 0xFA B, 0xFB A. Those are
//   consumed here before line editing/raw forwarding.
//
//   The button-bar bytes only arrive while game mode is off. With "GAME 1" (SlaveLink.ino)
//   the same buttons are part of the merged held-button bitmap the emulator reads instead
//   (0xF0 down / 0xF1 up), so the two vocabularies can never collide. What a press *means*
//   outside a game depends on what is using the audio and the screen, which is
//   PadButtons.ino's job -- here they are only decoded and parked.
//
//   Wiring (this board <-> DS-Slave):
//     DOLL-OS RX = KEYBOARD_SERIAL_RX_PIN <- DS-Slave TX = GPIO17
//     DOLL-OS TX = SLAVE_LINK_TX_PIN      -> DS-Slave RX = GPIO18
//     DOLL-OS GND          <-> DS-Slave GND           (shared ground -- carried by the power pair below)
//     DOLL-OS 5V/VIN       ->  DS-Slave 5V/VIN        (DOLL-OS powers DS-Slave; see the current note in setup)
//   BoardPins.h selects GPIO21/2 on AB/S and GPIO46/45 on N; the N's GPIO21/2
//   are occupied by audio WS and SD D2. Both ends run 115200 8N1.

//UART peripheral 1 -- UART0 backs the USB serial console (Serial), so the keyboard link
//gets its own peripheral. UART1 is full-duplex: RX carries keystrokes from DS-Slave and
//TX carries commands back to it. Using the hardware transmitter is important at 115200;
//software bit timing was vulnerable to cache/interrupt stalls and produced corrupt commands.
HardwareSerial KeyboardSerial(1);

static const uint32_t KEYBOARD_SERIAL_BAUD = 115200;
static const uint8_t KEYBOARD_LINK_VOLUME_UP = 0xF4;
static const uint8_t KEYBOARD_LINK_VOLUME_DOWN = 0xF5;
static const uint8_t KEYBOARD_LINK_SYSTEM_SLEEP = 0xF6;
static const uint8_t KEYBOARD_LINK_SYSTEM_WAKE = 0xF7;
//one byte per button-bar press edge -- no release event, because every action these
//drive is a one-shot (skip a track, toggle pause, open an app), not a held state
static const uint8_t KEYBOARD_LINK_PAD_START = 0xF8;
static const uint8_t KEYBOARD_LINK_PAD_SELECT = 0xF9;
static const uint8_t KEYBOARD_LINK_PAD_B = 0xFA;
static const uint8_t KEYBOARD_LINK_PAD_A = 0xFB;

//own line-edit parse state (see LineEditState in global.h) so a mid-escape keystroke
//can't tangle with the telnet client's in-progress parse
static LineEditState keyboardLineState;

static PadButton keyboardLinkPadButton(uint8_t ch) {
    switch (ch) {
        case KEYBOARD_LINK_PAD_START:  return PAD_BTN_START;
        case KEYBOARD_LINK_PAD_SELECT: return PAD_BTN_SELECT;
        case KEYBOARD_LINK_PAD_B:      return PAD_BTN_B;
        case KEYBOARD_LINK_PAD_A:      return PAD_BTN_A;
        default:                       return PAD_BTN_NONE;
    }
}

static bool handleKeyboardLinkControl(uint8_t ch) {
    if (ch == KEYBOARD_LINK_VOLUME_UP) {
        radioAdjustVolume(1);
        ledPulseInput();
        return true;
    }
    if (ch == KEYBOARD_LINK_VOLUME_DOWN) {
        radioAdjustVolume(-1);
        ledPulseInput();
        return true;
    }
    if (ch == KEYBOARD_LINK_SYSTEM_SLEEP) {
        enterSystemLightSleep();                   // Preserve DOLL-OS state until the slave restarts.
        return true;
    }
    if (ch == KEYBOARD_LINK_SYSTEM_WAKE) {
        return true;                               // Consume redundant wake bytes after GPIO wakeup.
    }
    PadButton pad = keyboardLinkPadButton(ch);
    if (pad != PAD_BTN_NONE) {
        padButtonPost(pad);                        // acted on by whoever owns the screen (PadButtons.ino)
        ledPulseInput();
        return true;
    }
    return false;
}

static int keyboardReadUserByte() {
    while (KeyboardSerial.available() > 0) {
        uint8_t ch = (uint8_t)KeyboardSerial.read();
        if (handleKeyboardLinkControl(ch)) {
            continue;
        }
        ledPulseInput();
        return ch;
    }
    return -1;
}

void initKeyboardSerial() {
    KeyboardSerial.begin(KEYBOARD_SERIAL_BAUD, SERIAL_8N1,
                         KEYBOARD_SERIAL_RX_PIN, SLAVE_LINK_TX_PIN);
    ledSetKeyboardActive(true);
    Serial.printf("[boot] keyboard UART RX=%d TX=%d baud=%lu\n",
                  KEYBOARD_SERIAL_RX_PIN, SLAVE_LINK_TX_PIN,
                  (unsigned long)KEYBOARD_SERIAL_BAUD);
}

//drains whatever DS-Slave has sent this tick, feeding each byte through the same line
//editor the telnet client uses. Both edit the one shared currentCommand buffer -- DOLL-OS is
//a single-user shell (global.h), so the keyboard and a telnet client are just two ways in
//for the same user. Mirrors the submit/reprompt dance of readTelnetClient().
void readKeyboardSerial() {
    while (true) {
        int raw = keyboardReadUserByte();
        if (raw < 0) {
            break;
        }
        uint8_t ch = (uint8_t)raw;
        //the terminate chord at the prompt: no app is running to receive it, so a track
        //still playing behind the shell is what it stops (Music.ino)
        if (musicHandleShellTerminate(ch)) {
            continue;
        }
        LineInputResult r = processLineEditByte(currentCommand, ch, keyboardLineState, false);
        if (r == LINE_NO_INPUT) {
            continue;
        }
        setActiveInput(shellPrompt(), currentCommand, false);
        if (r == LINE_SUBMITTED) {
            commandProcessor(currentCommand);
            setActiveInput(shellPrompt(), currentCommand, false);   //commandProcessor() clears the buffer, and a
                                                                     //"cd" just moved the prompt -- reflect both
            printPrompt();
        }
    }
}

//reads and applies one keyboard-bridge byte to a line-edited buffer, mirroring
//readLineEditedInput() (TelnetServer.ino) but sourced from the DS-Slave UART and never
//echoing CRLF (there's no telnet client to echo to). readKeyboardSerial() above can't be
//reused for this: the modal input phases that need it (ssh's password prompt) block loop(),
//so they poll one source at a time themselves rather than running the whole shell reader.
LineInputResult readKeyboardLineEditedInput(String& text) {
    int raw = keyboardReadUserByte();
    if (raw < 0) {
        return LINE_NO_INPUT;
    }
    return processLineEditByte(text, (uint8_t)raw, keyboardLineState, false);
}

//reads one raw keyboard-bridge byte for the RemoteSession raw-passthrough phase, or -1 if
//none is waiting. No line editing here -- the raw session classifies/forwards bytes itself
//(see readRawUserBytes, RemoteSession.ino), exactly as it does for raw telnet bytes.
int keyboardReadRawByte() {
    return keyboardReadUserByte();
}

//looks at the next keyboard-bridge byte without consuming it, or -1 if none is waiting.
//Used by the .dapp runtime's abort check (AppRunner.ino appPollAbortChord), which must
//not steal bytes the app's own KEY/INPUT reads are about to consume.
int keyboardPeekRawByte() {
    while (KeyboardSerial.available() > 0) {
        uint8_t ch = (uint8_t)KeyboardSerial.peek();
        if (!handleKeyboardLinkControl(ch)) {
            return ch;
        }
        KeyboardSerial.read();
    }
    return -1;
}
