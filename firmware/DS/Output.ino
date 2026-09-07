//   Output.ino
//   Line output helpers -- the telnet-port replacement for DOLL-OS's terminal.ino.
//
//   DOLL-OS's terminal was a scrolling grid of pixels: addWrappedHistoryLine had to
//   measure glyph widths and wrap text itself, and every row's color lived in a ring
//   buffer so the sprite could be redrawn from scratch every tick. A telnet client is
//   a real terminal -- it already wraps long lines and keeps its own scrollback --
//   so none of that is needed for the telnet side. Output is just bytes written to
//   the socket, with real ANSI SGR codes for color instead of a per-row uint16_t.
//
//   The board's own TFT panel still mirrors this output (see Display.ino), so
//   outLine()/outClearScreen() feed both: the telnet client gets real ANSI, the
//   panel gets DOLL-OS's original pixel-wrapped-history treatment.
//
//   outLine() is the direct replacement for addWrappedHistoryLine() at every call
//   site ported from DOLL-OS.

void outLine(const String& text) {
    outLine(text, C_WHITE);
}

void outLine(const String& text, int color) {
    addDisplayLine(text, ansiCodeToPixelColor(color));

    if (!telnetClient || !telnetClient.connected()) {
        return;
    }
    if (color != C_WHITE) {
        telnetClient.print("\x1b[");
        telnetClient.print(color);
        telnetClient.print("m");
    }
    telnetClient.print(text);
    if (color != C_WHITE) {
        telnetClient.print("\x1b[0m");
    }
    telnetClient.print("\r\n");
}

//clears the client's terminal screen and homes the cursor (ANSI CSI 2J + CSI H)
void outClearScreen() {
    clearDisplayHistory();

    if (!telnetClient || !telnetClient.connected()) {
        return;
    }
    telnetClient.print("\x1b[2J\x1b[H");
}

//the shell's prompt, path-aware like a real shell: "/sd/roms > ". Single source of truth
//for the three places a prompt shows up -- the telnet prompt (printPrompt below), the
//panel's mirrored command bar (setActiveInput call sites), and the echo of each submitted
//command (echoCommandLine below) -- so all three always agree on where "here" is. Read
//fresh at every call site rather than cached, since "cd" moves cwd underneath it.
const int SHELL_PROMPT_PATH_MAX = 24;

String shellPrompt() {
    String path = cwd;
    if (path.length() > SHELL_PROMPT_PATH_MAX) {
        //keep the tail -- the deepest segments are the ones that say where you are, and an
        //unbounded prompt would eat the command bar (drawDisplayCommandBar splits the bar's
        //width between the prompt and the text being typed)
        path = "..." + path.substring(path.length() - (SHELL_PROMPT_PATH_MAX - 3));
    }
    return path + " > ";
}

void printPrompt() {
    if (telnetClient && telnetClient.connected()) {
        telnetClient.print(shellPrompt());
    }
}

//echoes a submitted command into the scrollback so the transcript reads like a real shell
//instead of showing output with no record of what asked for it.
//
//The two surfaces need different halves of the line: the panel's history never sees typed
//input at all (the live buffer lives in the command bar, which is cleared on submit), so it
//gets the whole "path > cmd" line. The telnet client already has the prompt on screen --
//printPrompt() wrote it and the line editor is deliberately not echoing keystrokes back
//(see TelnetServer.ino's header) -- so it gets just the command text, completing that line.
//This is why readTelnetClient() submits with echoCrlfToTelnet=false: the newline below is
//the one that ends the prompt line, and an earlier CRLF would strand the prompt above it.
void echoCommandLine(const String& entered) {
    addDisplayLine(shellPrompt() + entered, ansiCodeToPixelColor(C_CYAN));

    if (!telnetClient || !telnetClient.connected()) {
        return;
    }
    telnetClient.print("\x1b[");
    telnetClient.print(C_CYAN);
    telnetClient.print("m");
    telnetClient.print(entered);
    telnetClient.print("\x1b[0m\r\n");
}
