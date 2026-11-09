//   Ssh.ino
//   SSH client for the "ssh" command. Almost all protocol/crypto work is handled
//   by libssh_esp32, so this file only wires it into DOLL-OS's telnet session -- the
//   same modal pattern Motoko.ino uses for PubSubClient, plus the RemoteSession
//   framework for the raw interactive-shell phase.
//
//   Ported from DOLL-OS's ssh.ino. The password phase now uses the shared
//   readLineEditedInput() in masked mode instead of readKeyboard(). The telnet
//   side gets true raw passthrough of the remote pty's stdout/stderr (a real
//   telnet client renders the remote's own ANSI natively -- simpler and more
//   faithful than DOLL-OS's per-row color approximation). The board's TFT panel
//   can't do that, though, so the same bytes are also run through Display.ino's
//   ANSI filter -- DOLL-OS's original approach, revived for the mirrored panel only.
#include "libssh_esp32.h"   //Arduino/ESP32 glue; must precede libssh.h per the library's own examples
#include <libssh/libssh.h>

static const int SSH_DEFAULT_PORT = 22;
static const long SSH_CONNECT_TIMEOUT_SEC = 8;
static const int SSH_PTY_COLS = 80;   //generic default -- no NAWS negotiation with the telnet client to learn its real size
static const int SSH_PTY_ROWS = 24;

//ssh_connect()'s mbedtls key exchange (and the rest of the session -- auth, channel
//setup, interactive shell) needs far more stack than the ~8KB default Arduino loop
//task provides; every official libssh_esp32 client example runs this work on its own
//task with a stack in this range rather than calling it straight from setup()/loop().
static const unsigned int SSH_TASK_STACK_SIZE = 40960;

//   That stack is the largest single internal-RAM allocation in the firmware, and
//   internal RAM is exactly what a playing radio has already drained -- so it goes to
//   PSRAM instead (the S3 build sets CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY). Safe
//   here because the session touches no flash and handleSshCommand blocks the shell for
//   its whole lifetime; the TCB stays internal, as FreeRTOS requires. Allocated once and
//   reused rather than per session, so repeated sessions don't churn the heap.
static StackType_t* sshTaskStack = nullptr;
static StaticTask_t sshTaskTcb;

String sshInputBuffer = "";   //password-entry phase only; the shell phase forwards raw keystrokes and has no local buffer

//ANSI filter/stream state for mirroring stdout/stderr onto the display panel only
//(see Display.ino) -- kept separate per stream so interleaved output can't corrupt
//each other's in-progress row
AnsiFilterState sshStdoutAnsi;
AnsiFilterState sshStderrAnsi;
DisplayStreamState sshStdoutDisplayStream;
DisplayStreamState sshStderrDisplayStream;
uint16_t sshStdoutDisplayColor = TFT_WHITE;
uint16_t sshStderrDisplayColor = TFT_RED;

static bool sshLibInitialized = false;

//drains whatever's already buffered on one ssh stream (stdout or stderr) without
//blocking. Bytes go two places: straight to telnetClient (true passthrough -- a
//real telnet client's terminal renders the remote's own ANSI natively) and through
//Display.ino's ANSI filter into the mirrored panel's history.
//
//takes a void* rather than ssh_channel: Arduino hoists this function's prototype to
//the top of the combined sketch, before Ssh.ino's own #include <libssh/libssh.h> runs,
//so an ssh_channel parameter would reference an as-yet-undefined type
void sshPumpStream(void* channelPtr, int isStderr) {
    ssh_channel channel = (ssh_channel)channelPtr;
    DisplayStreamState& dispStream = isStderr ? sshStderrDisplayStream : sshStdoutDisplayStream;
    AnsiFilterState& ansi = isStderr ? sshStderrAnsi : sshStdoutAnsi;
    uint16_t& dispColor = isStderr ? sshStderrDisplayColor : sshStdoutDisplayColor;
    uint16_t defaultColor = isStderr ? TFT_RED : TFT_WHITE;

    //read size kept small so each call only pulls a small slice of data before
    //RemoteSession::run() gets back to its own drawDisplayFrame() call -- closer to how
    //a real terminal renders character-by-character. The real telnetClient passthrough
    //below is unaffected since that's just a fast queued write.
    char buf[8];
    int n;
    if ((n = ssh_channel_read_nonblocking(channel, buf, sizeof(buf), isStderr)) > 0) {
        if (telnetClient && telnetClient.connected()) {
            telnetClient.write((const uint8_t*)buf, n);
        }

        for (int i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\r') {
                displayStreamCarriageReturn(dispStream);
                continue;
            }
            if (ch == '\n') {
                displayStreamNewline(dispStream);
                continue;
            }
            char outCh;
            bool colorChanged, isBackspace, isCarriageReturn;
            DisplayEraseKind erase;
            if (ansiFilterByte(ansi, (uint8_t)ch, defaultColor, dispColor, outCh, colorChanged, isBackspace, isCarriageReturn, erase)) {
                displayStreamPutChar(dispStream, outCh, dispColor);
            } else if (isBackspace) {
                displayStreamBackspace(dispStream);
            } else if (isCarriageReturn) {
                displayStreamCarriageReturn(dispStream);
            } else if (erase != DISPLAY_ERASE_NONE) {
                displayStreamErase(dispStream, erase);
            }
        }
    }
}

//owns an authenticated interactive shell channel: forwards raw keystrokes to the remote
//pty and streams stdout/stderr back to telnetClient live via sshPumpStream above.
//
//unlike sshPumpStream/runSshBlocking below, this can take ssh_channel directly instead of
//void*: Arduino's auto-prototype hoisting only pulls forward free-function signatures, not
//class member functions, so a class defined here (after Ssh.ino's own libssh.h include)
//with every method inlined in the class body never gets a prototype hoisted above that
//include in the first place.
class SshShellSession : public RemoteSession {
public:
    explicit SshShellSession(ssh_channel ch) : channel(ch) {}

protected:
    void pumpIncoming() override {
        sshPumpStream(channel, 0);
        sshPumpStream(channel, 1);
    }

    bool isClosed() override {
        return ssh_channel_is_eof(channel) || !ssh_channel_is_open(channel);
    }

    void sendBytes(const String& bytes) override {
        ssh_channel_write(channel, bytes.c_str(), bytes.length());
    }

    void onClosed() override {
        outLine("ssh: remote closed the connection", C_YELLOW);
    }

    //real unix pty erase character is DEL (0x7F), not the ASCII backspace (0x08) DOLL-OS's
    //telnet client transport wants -- see TelnetClient.ino's own override for that contrast
    String backspaceBytes() override {
        return "\x7f";
    }

private:
    ssh_channel channel;
};

//the modal loop's password phase: collects a masked password locally (ssh_userauth_password
//wants it all at once), opens the interactive shell channel on success, then hands off to
//SshShellSession for the raw keystroke-forwarding phase. "/quit" during password entry backs
//out locally; Ctrl+T backs out of an established shell (RemoteSession's escape chord).
//
//takes a void* rather than ssh_session for the same reason sshPumpStream takes a
//void*: this function's prototype gets hoisted above Ssh.ino's own libssh.h include
void runSshBlocking(void* sessionPtr, const String& user) {
    ssh_session session = (ssh_session)sessionPtr;
    ssh_channel channel = NULL;

    sshInputBuffer = "";
    commandCursorPos = 0;
    outLine("Password for " + user + ":");
    telnetClient.print("password> ");

    while (true) {
        delay(2);   //bounds how often the drawDisplayFrame() below can run

        //accept the password from either input surface -- the telnet client if one is
        //attached, otherwise the BLE keyboard. This phase runs on the ssh task while loop()
        //is blocked (handleSshCommand), so it has to poll both sources itself rather than
        //rely on loop()'s readKeyboardSerial(). A dropped/absent telnet client is no longer
        //a reason to abort: the panel + keyboard can drive the whole prompt.
        LineInputResult r = readLineEditedInput(sshInputBuffer);
        if (r == LINE_NO_INPUT) {
            r = readKeyboardLineEditedInput(sshInputBuffer);
        }
        setActiveInput("password> ", sshInputBuffer, true);   //masked on the display too
        ledService();
        drawDisplayFrame();   //this loop never returns to DS.ino's loop() until auth resolves,
                               //so the mirror has to repaint itself here too
        if (r != LINE_SUBMITTED) {
            continue;
        }

        if (sshInputBuffer == "/quit") {
            return;
        }

        String password = sshInputBuffer;
        sshInputBuffer = "";
        setActiveInput("password> ", "", true);

        bool authOk = ssh_userauth_password(session, NULL, password.c_str()) == SSH_AUTH_SUCCESS;
        password = "";   //don't linger in RAM longer than necessary

        if (!authOk) {
            outLine("ssh: authentication failed", C_RED);
            return;
        }

        channel = ssh_channel_new(session);
        if (channel == NULL ||
            ssh_channel_open_session(channel) != SSH_OK ||
            ssh_channel_request_pty(channel) != SSH_OK ||
            ssh_channel_change_pty_size(channel, SSH_PTY_COLS, SSH_PTY_ROWS) != SSH_OK ||
            ssh_channel_request_shell(channel) != SSH_OK) {
            outLine(String("ssh: shell setup failed: ") + ssh_get_error(session), C_RED);
            if (channel != NULL) {
                ssh_channel_free(channel);
            }
            return;
        }

        ssh_set_blocking(session, 0);   //non-blocking so the telnet session never stalls on a channel read
        outLine("ssh: connected (Ctrl+T quit, Ctrl+K cmd, Ctrl+up/down vol)", C_GREEN);
        break;
    }

    //no local buffer during the raw shell phase -- static hint for the display's mirrored command bar
    setActiveInput("ssh $ ", "Ctrl+T quit, Ctrl+K cmd", false);

    SshShellSession shellSession(channel);
    shellSession.run();

    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
}

//heap-allocated so it survives the handoff from handleSshCommand's stack frame to the
//dedicated ssh task's (sshTaskEntry frees it once the session ends)
struct SshTaskArgs {
    String user;
    String host;
    unsigned int port;
};

//true for the lifetime of the dedicated ssh task; handleSshCommand blocks on this so the
//loop task's own telnet read/accept calls never run concurrently with the ssh task's
static volatile bool sshTaskRunning = false;

//session connect through teardown -- moved out of handleSshCommand so all of it, not just
//ssh_connect() itself, runs on the dedicated ssh task and gets SSH_TASK_STACK_SIZE bytes
//of stack rather than the default loop task's
void sshConnectAndRun(const String& user, const String& host, unsigned int port) {
    if (!sshLibInitialized) {
        //this port doesn't run its C++ static constructor reliably on ESP32/Arduino,
        //so libssh_begin() (== the same init ssh_init() would trigger) must be called
        //explicitly before any other libssh call, per the library's own examples
        libssh_begin();
        sshLibInitialized = true;
    }

    ssh_session session = ssh_new();
    if (session == NULL) {
        outLine("ssh: could not allocate session", C_RED);
        return;
    }

    long timeout = SSH_CONNECT_TIMEOUT_SEC;
    ssh_options_set(session, SSH_OPTIONS_HOST, host.c_str());
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    ssh_options_set(session, SSH_OPTIONS_USER, user.c_str());
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);

    //LibSSH-ESP32 drops the old SHA-1 "ssh-rsa" host-key/signature algorithm from its
    //default list (kex.c's DEFAULT_HOSTKEYS/DEFAULT_PUBLIC_KEY_ALGORITHMS only offer
    //ed25519, ecdsa, and rsa-sha2-256/512), matching modern OpenSSH's SHA-1 deprecation.
    //Older devices (routers, switches, embedded gear, ancient OpenSSH) that only speak
    //ssh-rsa then fail to connect with "no matching host key algorithm". "+ssh-rsa"
    //appends it back onto the default list rather than replacing it, so this only adds
    //compatibility -- it doesn't drop any of the already-preferred modern algorithms.
    ssh_options_set(session, SSH_OPTIONS_HOSTKEYS, "+ssh-rsa");
    ssh_options_set(session, SSH_OPTIONS_PUBLICKEY_ACCEPTED_TYPES, "+ssh-rsa");

    outLine("Connecting to " + host + ":" + String(port) + " as " + user, C_PINK);
    telnetClient.flush();

    if (ssh_connect(session) != SSH_OK) {
        outLine(String("ssh: connect failed: ") + ssh_get_error(session), C_RED);
        ssh_free(session);
        return;
    }

    //NOTE: this accepts whatever host key the server presents without checking it
    //against a known-hosts store -- there isn't one on this device yet, so a
    //network-position attacker could substitute keys undetected. Acceptable for
    //trusted local-network sysadmin use; not a substitute for real host key pinning.

    displayStreamReset(sshStdoutDisplayStream);
    displayStreamReset(sshStderrDisplayStream);
    sshStdoutAnsi = AnsiFilterState();
    sshStderrAnsi = AnsiFilterState();
    sshStdoutDisplayColor = TFT_WHITE;
    sshStderrDisplayColor = TFT_RED;

    //after the key exchange, which is the session's peak internal-RAM moment
    recordHeapCheckpoint("ssh connected");

    runSshBlocking(session, user);

    //defensive: no stale row ownership survives past this session
    displayStreamReset(sshStdoutDisplayStream);
    displayStreamReset(sshStderrDisplayStream);

    ssh_disconnect(session);
    ssh_free(session);
    recordHeapCheckpoint("ssh freed");

    setActiveInput(shellPrompt(), "", false);

    outLine("ssh: session ended");
    //no printPrompt() here -- handleSshCommand's caller (commandProcessor(), via
    //readTelnetClient()'s loop in TelnetServer.ino) already reprints the prompt after
    //every command, including this one, once sshTaskRunning goes false and it returns
}

//entry point for the dedicated ssh task (see handleSshCommand) -- frees the args and
//clears sshTaskRunning so the loop task's wait below unblocks, then deletes itself
void sshTaskEntry(void* pvParameters) {
    SshTaskArgs* args = (SshTaskArgs*)pvParameters;
    sshConnectAndRun(args->user, args->host, args->port);
    delete args;
    sshTaskRunning = false;
    vTaskDelete(NULL);
}

//Expected forms: ssh user@host | ssh user@host port
void handleSshCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        outLine("Usage: ssh user@host [port]");
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        outLine("ssh: WiFi not connected. Run 'wifi connect' first.", C_RED);
        return;
    }

    String spec = parts[1];
    int atIndex = spec.indexOf('@');
    if (atIndex <= 0) {
        outLine("Usage: ssh user@host [port]");
        return;
    }
    String user = spec.substring(0, atIndex);
    String host = spec.substring(atIndex + 1);

    unsigned int port = SSH_DEFAULT_PORT;
    if (partCount > 2) {
        int parsedPort = parts[2].toInt();
        if (parsedPort > 0) {
            port = (unsigned int)parsedPort;
        }
    }

    SshTaskArgs* args = new SshTaskArgs{user, host, port};
    sshTaskRunning = true;
    recordHeapCheckpoint("ssh start");

    if (sshTaskStack == nullptr) {
        sshTaskStack = (StackType_t*)heap_caps_malloc(SSH_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM);
    }

    BaseType_t created;
    if (sshTaskStack != nullptr) {
        created = xTaskCreateStaticPinnedToCore(sshTaskEntry, "sshTask", SSH_TASK_STACK_SIZE,
            args, (tskIDLE_PRIORITY + 3), sshTaskStack, &sshTaskTcb,
            portNUM_PROCESSORS - 1) != NULL ? pdPASS : pdFAIL;
    } else {
        //no PSRAM (or it's full) -- fall back to the internal heap and hope it fits
        created = xTaskCreatePinnedToCore(sshTaskEntry, "sshTask", SSH_TASK_STACK_SIZE, args,
            (tskIDLE_PRIORITY + 3), NULL, portNUM_PROCESSORS - 1);
    }

    if (created != pdPASS) {
        delete args;
        sshTaskRunning = false;
        outLine("ssh: could not start session (needs " + String(SSH_TASK_STACK_SIZE)
            + " contiguous bytes; largest free internal "
            + String(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL))
            + ", psram " + String(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)) + ")", C_RED);
        return;
    }

    while (sshTaskRunning) {
        delay(10);
    }
    //sshTaskEntry clears the flag immediately before vTaskDelete(NULL); let the idle task
    //reap it before a following session reuses the same static stack and TCB
    delay(50);
}
