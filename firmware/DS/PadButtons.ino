//   PadButtons.ino
//   The DS-Slave button bar, as DOLL-OS sees it. While the slave is *not* in game mode
//   (SlaveLink.ino's "GAME 0|1"), each press of the bar's face buttons arrives on the
//   keyboard UART as one private byte instead of a keystroke -- 0xF8 Start, 0xF9 Select,
//   0xFA B, 0xFB A. KeyboardSerial.ino decodes those before line editing and parks the
//   button here; this file decides what the press *means*, which depends entirely on
//   what is using the audio and the screen at that moment:
//
//     the music player is open
//                       Start = previous track    B = select          A = next track
//     a library track is playing with the player closed
//                       Start = previous track    B = pause/resume    A = next track
//     a stream is loaded (playing, paused, or connecting)
//                       Start = previous station  B = stop            A = next station
//     nothing playing -- the plain shell
//                       Start = gb                B = radio           A = music
//
//   Precedence is "whatever is already running wins", so the launchers are only ever
//   reached from an idle shell -- pressing B mid-stream can't restart the radio under
//   itself. That precedence is also why B stops the radio rather than pausing it: a
//   paused stream still owns the bar, which would leave a keyboard-less handheld with
//   no way back to the emulator or the music library. Station stepping walks the same
//   list "radio list" fetches (Radio.ino's radioStepStation); track stepping walks the
//   library (Music.ino's musicPadTransport).
//
//   B is the odd one out: inside the open music player it is Enter on the highlighted
//   row, not pause, because the bar and the joystick are the whole input device on a
//   keyboard-less build and nothing else can descend the browser -- the joystick's click
//   sends Escape, which only ever goes back up. Pause stays on B for the row that is
//   already playing, where entering would just restart the track. See musicPadTransport.
//   Select is consumed but has no action yet: it must not be left to fall through into
//   the shell's input line, but nothing here needs a fourth verb.
//
//   Why a one-slot mailbox instead of acting where the byte is read: the byte can be
//   consumed from inside *any* modal app's input loop (the music player, a .dapp, an ssh
//   session), and "open the Game Boy emulator" must never run nested inside one of those.
//   So a press is parked with a timestamp and picked up by whoever legitimately owns the
//   screen -- the plain shell from loop() (padButtonService), the music player directly
//   (padButtonTake, since it blocks loop() for its whole run). Anything else simply lets
//   the press go stale, which is the behaviour we want: an app with no button-bar
//   vocabulary ignores the bar rather than misfiring the moment it exits.

//How long a parked press stays valid. The shell services one within a loop() tick, so
//this only has to outlast a slow frame push -- long enough that no real press is lost,
//short enough that a press swallowed by a modal app can't fire when that app returns.
static const unsigned long PAD_BUTTON_FRESH_MS = 500;

static PadButton padPendingButton = PAD_BTN_NONE;
static unsigned long padPendingAtMs = 0;

//called from KeyboardSerial.ino's control-byte filter, on the press edge. One slot,
//latest press wins -- same single-user "newest intent" rule as the radio's command
//mailbox; nobody can mean two things with one thumb.
void padButtonPost(PadButton button) {
    padPendingButton = button;
    padPendingAtMs = millis();
}

//Hands over the parked press, if there is a fresh one. A stale press is dropped rather
//than returned, so a button pressed during someone else's modal loop dies quietly.
bool padButtonTake(PadButton& button) {
    if (padPendingButton == PAD_BTN_NONE) {
        return false;
    }
    PadButton pending = padPendingButton;
    padPendingButton = PAD_BTN_NONE;
    if (millis() - padPendingAtMs > PAD_BUTTON_FRESH_MS) {
        return false;
    }
    button = pending;
    return true;
}

//Launches the app through the shell rather than calling its handler directly: the panel
//and any telnet client then show the command that ran, it lands in history, and the
//prompt comes back afterwards -- indistinguishable from typing it. Mirrors the submit
//path in readKeyboardSerial() (KeyboardSerial.ino), including leaving whatever the user
//had half-typed in currentCommand alone.
static void padRunShellCommand(const char* command) {
    String line = command;
    commandProcessor(line);
    setActiveInput(shellPrompt(), currentCommand, false);
    printPrompt();
}

//called every loop() tick (DS.ino) -- the shell's turn at the parked press. Only ever
//reached when no modal app is running, which is exactly when the launchers apply.
void padButtonService() {
    PadButton button;
    if (!padButtonTake(button)) {
        return;
    }

    if (musicPadTransportBackground(button)) {
        return;   //a library track is playing: the bar belongs to the library
    }
    if (radioPadTransport(button)) {
        return;   //a stream is loaded: the bar belongs to the radio
    }

    switch (button) {
        case PAD_BTN_START:
            padRunShellCommand("gb");      //ROM picker; it takes the bar into game mode itself
            break;
        case PAD_BTN_B:
            padRunShellCommand("radio play");   //last station, or the radio.url default
            break;
        case PAD_BTN_A:
            padRunShellCommand("music");   //full-screen library/player
            break;
        default:
            break;   //Select: reserved
    }
}
