# DOLL-OS Freenove Fork: Porting Notes

This repository is a DOLL-OS fork that moves the upstream M5Cardputer build
(sprite display + physical keyboard) to a Freenove ESP32-S3 display board
(FNK0104-series, sold as "FNK1014B"). Telnet is the sole *input* path -- this is
what replaces upstream DOLL-OS's physical keyboard -- but
the board's own TFT panel still runs as a live mirror of the telnet session ("a
second screen for the cardputer"), output-only. This document is the delta
against DOLL-OS's own `docs/architecture.md` and `docs/api-guide.md` in the
DOLL-OS repo -- read those first for the parts that didn't change.

## Building this sketch

**Open it in the Arduino IDE and select the `esp32s3` profile** from the profile
dropdown in the toolbar (sketch.yaml defines it) before Verify/Upload. That one
selection replaces manually setting Tools > Board/Flash Size/Partition Scheme/
PSRAM/USB Mode -- including `PartitionScheme=custom`, which tells the esp32 core
to use this sketch's own `partitions.csv` (see "Flash/RAM headroom" below)
instead of one of its 4MB-fixed stock tables. It also resolves a real conflict:
this board needs a
Freenove-customized fork of `TFT_eSPI` (adds the `ST77922` driver + per-panel pin
configs stock TFT_eSPI doesn't have), but your global `libraries/TFT_eSPI` install
is a *different*, unrelated config that `bigscreen`/`Evil-M5Project/Evil-CYD-Beta`
depend on. The profile keeps this fork's copy (`DS/libraries/TFT_eSPI`,
`DS/libraries/TFT_eSPI_Setups`) fully sketch-local via `sketch.yaml`, so neither
project's config touches the other. If you ever compile via bare `arduino-cli`
instead of the IDE, the profile applies automatically (`default_profile: esp32s3`).

## What carried over unchanged (logic-only port)

Everything that wasn't display/keyboard-specific ported with only the output
calls changed (`addWrappedHistoryLine(...)` -> `outLine(...)`):

- `CommandProcessor.ino`: tokenizing, command history ring buffer, dispatch table
- `Storage.ino`: unified LittleFS + SD path namespace, `ls`/`cd`/`pwd`/`cat`
- `WiFiManager.ino`: scan/connect/save credentials (STA-only -- see below)
- `IPTools.ino`, `Ping.ino`: IP info, ping sweep, ARP scan
- `Calc.ino` + `tinyexpr.c/h`: unchanged math engine
- `Dice.ino`: unchanged
- `SysInfo.ino`: heap instrumentation (`free`), extended with a real `battery`
  command -- see below
- `Motoko.ino`: MQTT chat, same Q&A shape
- `Asuka.ino`: native local LLM text chat command over this fork's existing shell
- `Ssh.ino`: libssh_esp32 client, same dedicated-FreeRTOS-task pattern (mbedtls's
  handshake still needs more stack than the loop task provides, board-independent)

## What changed and why

### The interface itself

DOLL-OS's whole input/output model was built for a screen (`terminal.ino`'s
pixel-wrapped sprite history) and a physical keyboard (`hardware.ino`'s
`readKeyboard()`/`readRawKeyBytes()`). Replacing that with telnet meant:

- **Output** (`Output.ino`): a telnet client is a real terminal with its own line
  wrapping and scrollback, so for the *telnet side*, DOLL-OS's pixel-measuring
  wrap logic and scroll-offset bookkeeping are gone. `outLine(text[, color])`
  writes real ANSI SGR codes to the socket instead of setting a per-row
  `uint16_t`. It also mirrors the same line onto the TFT panel (`Display.ino`) --
  see "Display mirror" below for why that side of `terminal.ino` came back.

- **Input** (`TelnetServer.ino`, replaces `hardware.ino` + `terminal.ino`'s
  `drawCommandBar`): this fork negotiates the connecting client into character-at-a-
  time mode with server-side echo (`IAC WILL ECHO`, `IAC WILL SUPPRESS-GO-AHEAD`)
  once per connection, then implements a real line editor against that byte
  stream -- `readLineEditedInput()` plays the exact role DOLL-OS's
  `readKeyboard()` did (same reuse across the shell, Motoko's prompts, and ssh's
  password entry), including Up/Down arrow history recall and Left/Right cursor
  movement, redrawn via `\r` + erase-to-end + reprint on every edit.

- **Remote sessions** (`RemoteSession.ino`, `Ssh.ino`'s shell phase,
  `TelnetClient.ino`): DOLL-OS's `RemoteSession` base class owned a poll/pump/
  redraw loop between a keyboard and a sprite. Here "local" is the one connected
  `telnetClient` in both directions: `pumpIncoming()` writes remote bytes
  straight to it, and `readRawUserBytes()` reads the user's keystrokes back off
  it. The local escape chord changed from the physical **Fn+Q** to **Ctrl+T**
  (0x14) -- chosen because it isn't commonly bound by remote shells or BBS-style
  line editors the way Ctrl+C/D/Z are.

- **Telnet gets true ANSI passthrough**: for the telnet side only, `ssh` and
  outbound `telnet` forward remote bytes through untouched (after stripping only
  the telnet protocol's own IAC negotiation bytes, a wire-protocol concern, not a
  display one) instead of DOLL-OS's SGR-to-`uint16_t` reinterpretation -- a real
  terminal emulator renders the remote's actual ANSI natively, which is simpler
  *and* more faithful than DOLL-OS's per-row color approximation ever was.

### Display mirror

The TFT panel isn't a second *input* device -- telnet remains the only way to
control this fork -- but per the user's request it runs as a live mirror of the telnet
session, "as if it was a second screen for the cardputer." That revived most of
DOLL-OS's `terminal.ino` (pixel-wrapped history, wrap-on-width, per-row color)
and `ansi.ino` (SGR interpretation for raw remote streams), now living in
`Display.ino` and retargeted from an M5GFX sprite to a `TFT_eSPI`/`TFT_eSprite`
one -- the API shapes are close enough that the port is mostly a rename:

- `Output.ino`'s `outLine()`/`outClearScreen()` feed both sides: real ANSI to
  the telnet socket, pixel-wrapped rows to the panel's history ring buffer
  (`addDisplayLine()`).
- The live input line mirrors too: `readLineEditedInput()`'s three call sites
  (`TelnetServer.ino`'s shell loop, `Motoko.ino`, `Ssh.ino`'s password prompt)
  each set `activeInputPrompt`/`activeInputMasked`/`activeInputText`
  (`global.h`) right after every edit, which `Display.ino`'s command-bar
  renderer reads every tick. Unlike DOLL-OS there's no local cursor to keep
  visible on this panel (it isn't being locally edited), so an overlong line
  just drops its head instead of scrolling a tracked cursor into view.
- Raw byte-stream sessions (`ssh`'s shell phase, outbound `telnet`) have no
  local buffer to mirror, so they show a static hint (`"ssh $ "` / `"Ctrl+T to
  quit"`) instead, and their remote bytes are additionally run through
  `Display.ino`'s revived `ansiFilterByte()` (`Ssh.ino`'s `sshPumpStream()`,
  `TelnetClient.ino`'s `remoteTelnetMirrorToDisplay()`) so the panel shows
  colored output even though telnet itself gets a raw, unfiltered copy of the
  same bytes.
- No local scroll-back chord exists (no physical keyboard to send one from), so
  the panel always shows the tail of history -- it just follows along live.
**TFT_eSPI fork**: this board needs Freenove's customized TFT_eSPI (adds the
`ST77922` driver + real per-panel pin configs), installed sketch-locally
(`DS/libraries/TFT_eSPI` + `DS/libraries/TFT_eSPI_Setups`) via `sketch.yaml` --
see "Building this sketch" above for why. `BoardVariant.h` is the single model
selector shared by TFT_eSPI and the sketch; `BoardPins.h` derives display,
storage, audio, battery, LED, and DS-Slave wiring from it. The ST77922 path uses
the vendor QSPI driver. Landscape row bands are transformed into four-pixel-aligned
native portrait strips before using Freenove's tested rotation-0 region writer, so
the row-diff partial-update optimization remains available on every variant.

### Hardware differences

- **SD card**: DOLL-OS's M5Cardputer wires SD over SPI. This board's SD slot
  is on the ESP32-S3's dedicated SDMMC peripheral (4-bit bus), so `Storage.ino`
  uses `SD_MMC` instead of `SD`/`SPI`. Pins live in `BoardPins.h`, taken from
  Freenove's own example sketches for this board (`SD_MMC_CLK/CMD/D0-D3_PIN`);
  the `BoardVariant.h` selection switches between AB/S and N wiring.
- **Battery**: DOLL-OS read `M5Cardputer.Power` directly. This board has no
  fuel-gauge chip, just a divided ADC pin (confirmed against Freenove's own
  `Battery_Voltage` example) -- `SysInfo.ino`'s `readBatteryVoltage()`/
  `readBatteryPercent()` read it directly and estimate percent via a linear
  LiPo curve, exposed as a new top-level `battery` command (DOLL-OS only ever
  exposed this inline in its status bar and Motoko's `/battery`).
- **USB firmware**: USB MSC and DFU are deliberately absent, while hardware USB
  CDC on boot remains enabled because this board requires it for console/upload.
  FTP provides SD-card file transfer.
- **Networking model**: DOLL-OS only needed Wi-Fi STA mode -- losing it just
  meant losing internet access, not the UI (the screen/keyboard don't depend on
  the network). This fork at first kept an always-on SoftAP alongside STA so telnet
  stayed reachable without router credentials, but AP+STA on the S3's single
  radio cost too much streaming throughput (Radio.ino audio starved once its
  buffer drained), and the panel + BLE keyboard cover the no-network case
  anyway -- so this fork went back to STA-only.

### Radio (new, not from DOLL-OS)

`Radio.ino` is a port of the standalone sgcrelay firmware (`../sgcrelay/`) into this fork
as a background task: `radio play [url]` streams ICY/MP3 (default: the SGCRelay
Pi's relay, `config.h`) through the board's onboard ES8388 codec + speaker, with
`pause`/`stop`/`vol 0..21`/`status` subcommands. The Tab5 backend programs the
codec and PI4IO-controlled amplifier over M5Unified's internal I2C bus.
sgcrelay's LovyanGFX touch UI, Wi-Fi handling, and
LED/button controls were dropped (Display.ino owns the panel, WiFiManager.ino
owns STA, the shell replaces the physical controls).

Playback runs on a dedicated long-lived FreeRTOS task (same pattern as `ssh`'s)
rather than `loop()`, so audio survives modal ssh/telnet sessions and display
pushes. The shell posts to a one-slot mailbox; ESP32-audioI2S's weak callbacks
fire in the task and only stash announcements, which `radioService()` (called
each `loop()` tick) prints -- the main loop stays the single writer of the telnet
socket and display history.

**This forced variant-specific DS-Slave wiring**: AB/S use GPIO21 for keyboard
RX and GPIO2 for bit-bang TX. The N cannot: GPIO21 is its I2S word-select and
GPIO2 is SD D2, so it uses GPIO46/45 instead. Audio is selected from the same
map: AB/S use I2S GPIO4-8 and I2C SDA16/SCL15; N uses MCLK17, BCLK18, DIN16,
DOUT15, WS21 and I2C SDA38/SCL39. Amp-enable remains GPIO1 on all variants.

### Editor (new, not from DOLL-OS)

`Edit.ino` is a full-screen nano-style text editor (`edit <file>`), written for
this fork rather than ported. GNU nano itself was evaluated and rejected: its structure
is a conversation with ncurses/terminfo, and this fork has no cell-grid terminal layer
to shim it onto — `Display.ino`'s `ansiFilterByte()` deliberately drops cursor
addressing (only SGR, `K`, and `G`≤1 survive), so a cursor-addressed app can't
travel through the normal output path at all. Owning the render is far less work
than building a curses backend, and nano's remaining POSIX dependencies
(`fork`/`exec`, POSIX regex, `getpwuid`, wide-char locale, autotools) are absent
on ESP-IDF newlib anyway. nano is also GPLv3, which would not compose with the
GPLv2 gnuboy core already vendored under `src/emulator/`.

Structurally it follows `Gameboy.ino`: a full-screen takeover that seizes `loop()`
and runs its own inner loop until the user quits. Two differences:

- It draws into `frameSprite` and pushes via `pushDisplayFrame()`, both
  panel-variant-agnostic, so unlike `gb` it works on all three FNK0104 variants
  instead of being stubbed out on the QSPI one. Borrowing `frameSprite` is free —
  `drawDisplayFrame()` rebuilds it from scratch whenever `displayDirty` is set.
- No DS-Slave mode switch. `gb` needs `GAME 1` because it wants held-button state;
  the editor wants ASCII, which is the slave's normal mode, and DS-Slave already
  emits true control codes and CSI sequences there (`writeModifiedControl`). So
  the editor is fully usable with no telnet client attached.

Both surfaces render the same viewport — the panel gets the sprite, a connected
telnet client gets the identical grid as ANSI. Geometry (~52x21 at the 6x8 GLCD
font) comes from the panel because `TelnetServer.ino` doesn't negotiate NAWS, so
there's no way to ask a client its size; a larger terminal shows the grid in its
top-left corner.

Three things worth knowing:

- **Storage is a flat PSRAM slab plus a rebuilt line index**, not a vector of
  `String` lines. A per-line `String` array puts every line's character storage on
  the *internal* heap — `enablePsramHeap()` only routes allocations ≥512 bytes out
  to PSRAM, and edit lines are far smaller — so a few thousand lines would quietly
  eat tens of KB of the scarce pool. Caps are 128KB / 4000 lines, both in PSRAM.
- **Input is drained fully, then rendered once per pass.** A full sprite push is
  ~38ms, so rendering inside the key handler would cap typing at ~26 keys/sec and
  fall behind the bursts a telnet client delivers. The coalescing is load-bearing.
- **`radioService()` is deliberately not called** from the editor loop. It prints
  through `outLine()`, which would scribble raw lines across both surfaces
  mid-frame. Stream announcements queue and land after exit, the same as during an
  ssh/telnet session. The radio itself keeps playing — it's a separate FreeRTOS
  task, so unlike `gb` no `radioReleaseAudio()` is needed.

Key chords are nano's, and all of them are implemented: `^G` help, `^O` save,
`^X` exit, `^K`/`^U` cut+paste, `^W` search, `^_` goto line, `^Z` undo, `^A`/`^E`,
`^Y`/`^V`, `^D`, `^C` position, plus the arrow/Home/End/PgUp/PgDn cluster. DOLL-OS's
reserved Ctrl+T (detach) and Ctrl+K (inline command) don't apply here, because a
takeover doesn't route through `readRawUserBytes()` — which is what leaves `^K`
free for cut-line. `^Z` is undo rather than suspend (no job control to suspend
into, and it's far more discoverable than nano's Alt+U, which also works: the
decoder treats `ESC` + printable as Alt+key, the same spelling DS-Slave's
`writeModifiedAscii` emits).

Undo records live underneath `editInsertBytes`/`editDeleteBytes`, the two
functions every mutation funnels through, so cut and paste became undoable
without either feature knowing undo exists. 64 steps, with runs of typing and of
backspacing each coalescing into one record so 20 keystrokes are one undo rather
than 20. Insert records carry no payload (undoing one is a delete of known
length); delete records keep the removed bytes in PSRAM.

The `editSavedUndoDepth` bookkeeping is what lets undoing back to the last save
clear the modified flag instead of leaving a false "unsaved changes" prompt. Two
cases make it subtler than a counter: history evicted from the full ring makes
the saved state unreachable, and editing *after* undoing past the save point
discards the records that led to it. Both set the depth to -1 (unreachable). The
divergence test is `>=` rather than `>` because it runs after the count has
already been incremented — see the comment at the check.

The cut buffer and last search term deliberately persist across `edit` launches.
The board has no clipboard, so `^K` in one file and `^U` in another is the only
way to move text between them.

Saves go through a temp file + rename, never a truncating open on the target —
this is a battery device, and a power loss partway through an in-place write
destroys the file being edited rather than just failing. Tabs are stored as real
tabs and expanded only at render time, so Makefile-style files survive a round
trip; CRLF input is normalized to LF on load.

### ASUKA (new, not from DOLL-OS)

`Asuka.ino` adds `asuka [host] [port]` as a native shell command for local LLM
chat. It deliberately uses this fork's existing telnet server, BLE-keyboard line editor,
`outLine()` history, and TFT display stream instead of starting ASUKA's old
webserver, MQTT listener, or separate telnet console. Defaults live in `config.h`
as `ASUKA_DEFAULT_LLM_HOST`, `ASUKA_DEFAULT_LLM_PORT`,
`ASUKA_DEFAULT_LLM_PATH`, `ASUKA_BRAVE_API_KEY`,
`ASUKA_OPENWEATHER_API_KEY`, `ASUKA_OPENWEATHER_LOCATION_LABEL`,
`ASUKA_OPENWEATHER_LAT`, `ASUKA_OPENWEATHER_LON`, `ASUKA_SYSTEM_PROMPT`, and
`ASUKA_CLASSIFIER_PROMPT`. ASUKA seeds missing prompt files into LittleFS when
the command starts: `/system/conf/asuka-system.dsys` for the chat system prompt
and `/system/conf/asuka-classifier.dsys` for the tool-selection/classifier prompt. Runtime
`/system <prompt>` and `/classifier <prompt>` changes are saved back to those
files; `/system reset` and `/classifier reset` restore the compiled defaults and
save them back to flash. Weather questions call OpenWeather directly for the
configured coordinates; `/weather` shows the active location and `/weather <lat>
<lon> <label>` changes it for the current ASUKA session.

The `.dapp` runner remains separate from ASUKA. AppRunner 1.4 adds bounded
`HTTPGET`/`HTTPPOST`, scoped request headers, and small JSON escape/path helpers
for ordinary request/response apps. Persistent LLM sockets, SSE streaming, and
tool routing remain native. It also adds raw byte file I/O and the three-channel
`WAVE` synthesizer; those reuse the existing filesystem, network, ES8388, and
I2S ownership surfaces rather than creating parallel ones.

## Known constraints worth flagging

- **Flash/RAM headroom**: `esp32:esp32:esp32s3` ships stock partition schemes
  (`huge_app`, `app3M_fat9M_16MB`, etc.) sized for 4MB flash regardless of
  which `FlashSize` you pick -- selecting `16M` doesn't make `huge_app` any
  bigger, it just leaves the other 12MB of this board's flash completely
  unpartitioned and unreachable. `sketch.yaml` instead sets
  `PartitionScheme=custom`, which makes the esp32 core pick up the sketch's own
  `partitions.csv` (repo root, next to `DS.ino`) -- a single ~6.25MB app slot
  (`ota_0` subtype, so `boot_app0.bin`'s slot-select still applies even without
  an `app1`/OTA in use) plus a ~9.66MB `spiffs`-labeled partition for LittleFS
  and a small `coredump` partition for crash postmortems, sized to use the
  entire 16MB. With that table, the full build (display mirror included) is
  ~8% flash / ~36% static RAM -- effectively all the growth room this board
  has. PSRAM is `opi` (octal) -- confirmed by `esptool`'s own chip ID readout
  (`Embedded PSRAM 8MB (AP_3v3)`), which matches an ESP32-S3-WROOM-1 N16R8
  module's octal PSRAM part. Earlier in bring-up, `PSRAM=opi` appeared to hang
  boot before `setup()` ever ran, and `PSRAM=disabled` was used as a
  workaround; that turned out to be the wrong diagnosis -- `disabled` let the
  board boot, but broke the TFT SPI bus outright (backlight lit, but the panel
  never rendered anything, confirmed by bypassing this sketch entirely and
  testing Freenove's own unmodified rainbow example under both settings).
  `opi` is the correct setting for this hardware; the original hang had a
  different, still-unidentified cause that stopped reproducing once other
  bring-up issues (LittleFS partition mismatch, stale flash content) were
  fixed. The full-screen `TFT_eSprite` frame buffer (`DISPLAY_WIDTH x
  DISPLAY_HEIGHT x 2` bytes) is allocated at runtime from PSRAM automatically
  (TFT_eSPI prefers it for large sprite allocations when available), not
  counted in that static RAM figure.
- **Single session, same as DOLL-OS's single keyboard.** Only one telnet client
  can be connected at a time; a second connection attempt is rejected with a
  message, matching DOLL-OS's single-keyboard assumption throughout the shared
  input/state model (`commandCursorPos` etc. are shared globals, not per-session).
- **No known-hosts store** for `ssh` -- unchanged from DOLL-OS, still
  trusted-LAN-only.
