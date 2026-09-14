//   TelnetClient.ino
//   plain TCP telnet client for the "telnet" command -- DOLL-OS's own outbound telnet
//   client, distinct from TelnetServer.ino (which is DOLL-OS *being* a telnet server for
//   the user). No encryption, just a raw socket wired into the RemoteSession
//   framework, plus just enough IAC option negotiation (RFC 854/855) that
//   well-behaved remote servers stop spamming control bytes into the stream.
//
//   Ported from DOLL-OS's telnet.ino. Every plain byte from the remote (including
//   its own ANSI/color escape codes) is written straight through to telnetClient --
//   the user's own telnet client is a real terminal and renders it natively. This
//   file's IAC handling is the mirror image of what TelnetServer.ino does for
//   incoming connections: there DOLL-OS is the well-behaved server; here it's the
//   well-behaved client dialing out.
//
//   The board's TFT panel mirrors this session too (Display.ino) but can't render
//   real ANSI, so the same plain bytes are also run through Display.ino's ANSI
//   filter -- DOLL-OS's original approach, revived for the mirrored panel only.
static const int REMOTE_TELNET_DEFAULT_PORT = 23;
static const long REMOTE_TELNET_CONNECT_TIMEOUT_SEC = 8;

static const uint8_t RT_IAC  = 255;
static const uint8_t RT_DONT = 254;
static const uint8_t RT_DO   = 253;
static const uint8_t RT_WONT = 252;
static const uint8_t RT_WILL = 251;
static const uint8_t RT_SB   = 250;
static const uint8_t RT_SE   = 240;

static const uint8_t RT_OPT_ECHO = 1;
static const uint8_t RT_OPT_SGA  = 3;

enum RemoteTelnetParseState { RT_NORMAL, RT_GOT_IAC, RT_GOT_CMD, RT_GOT_SB, RT_GOT_SB_IAC };

WiFiClient remoteTelnetClient;
static RemoteTelnetParseState remoteTelnetState = RT_NORMAL;
static uint8_t remoteTelnetPendingCmd = 0;

//ANSI filter/stream state for mirroring this session onto the display panel only (Display.ino)
AnsiFilterState remoteTelnetAnsi;
DisplayStreamState remoteTelnetDisplayStream;
uint16_t remoteTelnetDisplayColor = TFT_WHITE;

//refuses everything the remote server asks us (DO) to do, but accepts ECHO and
//SUPPRESS-GO-AHEAD when the server announces it will do them (WILL) itself -- that's
//what puts well-behaved servers into character-at-a-time mode instead of the
//line-buffered NVT default
static void remoteTelnetReplyNegotiation(uint8_t cmd, uint8_t option) {
    uint8_t reply;
    if (cmd == RT_DO) {
        reply = RT_WONT;
    } else {
        reply = (option == RT_OPT_ECHO || option == RT_OPT_SGA) ? RT_DO : RT_DONT;
    }
    uint8_t buf[3] = { RT_IAC, reply, option };
    remoteTelnetClient.write(buf, 3);
}

//feeds one plain (non-IAC) byte to the display's ANSI filter -- telnet passthrough
//itself doesn't need this, it's purely for the mirrored panel (see Display.ino)
static void remoteTelnetMirrorToDisplay(uint8_t ch) {
    if (ch == '\r') {
        displayStreamCarriageReturn(remoteTelnetDisplayStream);
        return;
    }
    if (ch == '\n') {
        displayStreamNewline(remoteTelnetDisplayStream);
        return;
    }
    char outCh;
    bool colorChanged, isBackspace, isCarriageReturn;
    DisplayEraseKind erase;
    if (ansiFilterByte(remoteTelnetAnsi, ch, TFT_WHITE, remoteTelnetDisplayColor, outCh, colorChanged, isBackspace, isCarriageReturn, erase)) {
        displayStreamPutChar(remoteTelnetDisplayStream, outCh, remoteTelnetDisplayColor);
    } else if (isBackspace) {
        displayStreamBackspace(remoteTelnetDisplayStream);
    } else if (isCarriageReturn) {
        displayStreamCarriageReturn(remoteTelnetDisplayStream);
    } else if (erase != DISPLAY_ERASE_NONE) {
        displayStreamErase(remoteTelnetDisplayStream, erase);
    }
}

//feeds one incoming byte through the IAC state machine; plain bytes (including bare
//\r/\n and the remote's own ANSI escapes) are written straight to telnetClient --
//a real terminal on the other end interprets all of that natively -- and mirrored
//onto the display panel via the ANSI filter above
static void remoteTelnetProcessByte(uint8_t ch) {
    switch (remoteTelnetState) {
        case RT_NORMAL:
            if (ch == RT_IAC) {
                remoteTelnetState = RT_GOT_IAC;
            } else {
                if (telnetClient && telnetClient.connected()) {
                    telnetClient.write(&ch, 1);
                }
                remoteTelnetMirrorToDisplay(ch);
            }
            break;

        case RT_GOT_IAC:
            if (ch == RT_IAC) {   //escaped 0xFF data byte
                uint8_t literal = 0xFF;
                if (telnetClient && telnetClient.connected()) {
                    telnetClient.write(&literal, 1);
                }
                remoteTelnetState = RT_NORMAL;
            } else if (ch == RT_SB) {
                remoteTelnetState = RT_GOT_SB;
            } else if (ch == RT_WILL || ch == RT_WONT || ch == RT_DO || ch == RT_DONT) {
                remoteTelnetPendingCmd = ch;
                remoteTelnetState = RT_GOT_CMD;
            } else {
                remoteTelnetState = RT_NORMAL;   //other IAC commands (NOP, AYT, GA, ...) need no reply
            }
            break;

        case RT_GOT_CMD:
            if (remoteTelnetPendingCmd == RT_DO || remoteTelnetPendingCmd == RT_WILL) {
                remoteTelnetReplyNegotiation(remoteTelnetPendingCmd, ch);
            }
            remoteTelnetState = RT_NORMAL;
            break;

        case RT_GOT_SB:
            if (ch == RT_IAC) {
                remoteTelnetState = RT_GOT_SB_IAC;
            }
            break;

        case RT_GOT_SB_IAC:
            remoteTelnetState = (ch == RT_SE) ? RT_NORMAL : RT_GOT_SB;
            break;
    }
}

//drains whatever's already buffered on the outbound socket without blocking. Capped
//per call so a fast/chatty remote can't keep this looping forever -- RemoteSession::run()
//needs to get back to its own drawDisplayFrame() call every iteration for the display
//mirror to stay live. Kept small so each redraw only represents a small slice of new
//content, closer to how a real terminal renders character-by-character.
static void pumpRemoteTelnetStream() {
    const int maxBytesPerPump = 8;
    int processed = 0;
    while (processed < maxBytesPerPump && remoteTelnetClient.available() > 0) {
        remoteTelnetProcessByte((uint8_t)remoteTelnetClient.read());
        processed++;
    }
}

//forwards the user's raw keystrokes into the outbound socket and streams the remote's
//bytes back via the IAC parser above. Ctrl+T disconnects locally (RemoteSession's
//escape chord); the remote end closing the socket also exits.
class TelnetClientSession : public RemoteSession {
protected:
    void pumpIncoming() override {
        pumpRemoteTelnetStream();
    }

    bool isClosed() override {
        return !remoteTelnetClient.connected();
    }

    void sendBytes(const String& bytes) override {
        //RFC 854 NVT requires CR be followed by LF (or NUL); the user's Enter keystroke
        //only produces "\r"
        if (bytes == "\r") {
            remoteTelnetClient.write((const uint8_t*)"\r\n", 2);
        } else {
            remoteTelnetClient.write((const uint8_t*)bytes.c_str(), bytes.length());
        }
    }

    void onClosed() override {
        outLine("telnet: remote closed the connection", C_YELLOW);
    }

    //classic telnet/BBS servers (e.g. telehack.com) implement their own line editor
    //against the original ASCII backspace (0x08), not DEL (0x7F)
    String backspaceBytes() override {
        return "\x08";
    }
};

//Expected forms: telnet host | telnet host port
void handleTelnetCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        outLine("Usage: telnet host [port]");
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        outLine("telnet: WiFi not connected. Run 'wifi connect' first.", C_RED);
        return;
    }

    String host = parts[1];
    int port = REMOTE_TELNET_DEFAULT_PORT;
    if (partCount > 2) {
        int parsedPort = parts[2].toInt();
        if (parsedPort > 0) {
            port = parsedPort;
        }
    }

    outLine("Connecting to " + host + ":" + String(port), C_PINK);
    telnetClient.flush();

    remoteTelnetClient.setTimeout(REMOTE_TELNET_CONNECT_TIMEOUT_SEC);
    if (!remoteTelnetClient.connect(host.c_str(), port)) {
        outLine("telnet: connect failed", C_RED);
        return;
    }
    remoteTelnetClient.setNoDelay(true);   //interactive session sends single bytes per keystroke --
                                            //without this, Nagle's algorithm + the remote's delayed
                                            //ACK stall every keystroke by up to ~200ms

    outLine("telnet: connected (Ctrl+T quit, Ctrl+K cmd, Ctrl+up/down vol)", C_GREEN);
    remoteTelnetState = RT_NORMAL;
    displayStreamReset(remoteTelnetDisplayStream);
    remoteTelnetAnsi = AnsiFilterState();
    remoteTelnetDisplayColor = TFT_WHITE;

    //no local buffer during the raw session -- static hint for the display's mirrored command bar
    setActiveInput("telnet> ", "Ctrl+T quit, Ctrl+K cmd", false);

    TelnetClientSession session;
    session.run();
    displayStreamReset(remoteTelnetDisplayStream);   //defensive: no stale row ownership survives past this session

    remoteTelnetClient.stop();

    setActiveInput(shellPrompt(), "", false);

    outLine("telnet: session ended");
    //no printPrompt() here -- readTelnetClient()'s loop (TelnetServer.ino) already
    //reprints the prompt after every command, including this one, once we return
}
