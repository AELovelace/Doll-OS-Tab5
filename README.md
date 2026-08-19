# DOLL-OS for M5Stack Tab5

> **Port status:** the complete inherited application now builds through the
> pinned PlatformIO Arduino + ESP-IDF environment for Tab5. Display, power,
> storage, and the official Tab5 Keyboard are integrated; USB HID is ready for
> hardware validation and BLE HID comes next. See
> [the Tab5 port plan](docs/TAB5_PORT_PLAN.md) and
> [the upstream-sync policy](docs/UPSTREAM_SYNC.md).

This fork targets only the M5Stack Tab5. Its local interface is the official
Tab5 Keyboard, with BLE HID and USB HID keyboards joining the same input hub.
Touchscreen input is intentionally disabled for the initial releases.

The Tab5 interface uses a 2× base font scale for the shell, history, status,
command bar, editor, and modal screens. Character-grid `.dapp` canvases retain
their adaptive scaling so larger playfields still fit on the panel.

The command set is inherited from DOLL-OS-FNK. Features that still depend on
FNK-only hardware are called out below instead of being presented as complete.

## Inherited FNK baseline

DOLL-OS is my attempt at making my dream OS for the ESP32. It features many useful
commands and features you'd expect from a desktop operating system, while booting 
in under 2 seconds. Paired with a router for tailscaling DOLL-OS becomes less of a 
curio. With it's built-in ssh and telnet capabilities, it's a powerful remote management
tool. Can you do everything you can do with DOLL-OS on a smartphone? Yes, but that's not
why you're on the market for an operating system for a 15 dollar microcontroller, is it?

DOLL-OS-FNK is a shell-style OS for the Freenove ESP32-S3 display board (FNK0104-series, sold as
the "FNK1014B" kit). A fork of DOLL-OS that swaps the upstream M5Cardputer's
sprite display + physical keyboard for a telnet session as the input path, with
the board's TFT panel running as a live output mirror. A companion sketch
(`DS-Slave`) bridges BLE keyboards and gamepads in over UART so the device is
usable with no network.

> TODO: screenshot / photo of the panel here.

---

## Features

> TODO: trim this to the ones worth leading with, drop the rest into the command table.

- **Shell** — command dispatch, history ring, path-aware prompt, mirrored across
  telnet and the panel simultaneously
- **Storage** — unified LittleFS + SD path namespace (`/` flash, `/sd` card),
  `ls`/`cd`/`cp`/`mv`/`rm`/`cat`
- **`.dapp` apps** — text executables with their own scripting language; see
  [docs/DAPP.md](docs/DAPP.md) and the [browser emulator + playground](dapp-web/)
- **Editor** — full-screen text editor (`edit`)
- **Networking** — telnet server + client, SSH client, FTP server, ping/ARP
  sweep, IP tools, MQTT (`motoko`)
- **Radio** — inherited MP3 playback UI using the Tab5 ES8388 audio backend
- **Music library** — full-screen local MP3 player with recursive `/sd/music`
  scanning, ID3 metadata, search, and PSRAM-backed catalog storage
- **Game Boy / Game Boy Advance emulators** — `gb` runs inside Doll-OS while
  `gba` reboots into a lean second image; Tab5 and USB keyboards share
  held-button controls
- **ASUKA** — local LLM chat with tool calling (search / weather / URL fetch / time)
- **Input hub** — the official Tab5 Keyboard and USB HID keyboards are active
  producers for the same event queue; BLE HID remains planned

---

## Hardware

| | |
|---|---|
| Board | M5Stack Tab5 (ESP32-P4 + ESP32-C6) |
| Panel | 5" 1280×720, managed by M5Unified/M5GFX |
| Flash / PSRAM | 16MB flash, 32MB PSRAM |
| Storage | SD_MMC card slot + LittleFS |
| Audio | onboard ES8388 playback / ES7210 capture; ES8388 playback backend active |
| Local input | official Tab5 Keyboard over its expansion connector |
| USB input | USB HID keyboards through the onboard USB-A host port |
| Future input | Bluetooth keyboards through the shared input hub |

Touch input is deliberately ignored. The USB-A host port is powered at startup,
and USB keyboard hot-plug events feed the same input hub as the Tab5 Keyboard.

<details>
<summary>Inherited FNK hardware history</summary>

DS-Slave always uses GPIO17 TX and GPIO18 RX on its end. On DOLL-OS, connect
RX/TX to GPIO21/2 for FNK0104AB/S or GPIO46/45 for FNK0104N. The N move keeps
the link clear of its audio WS (GPIO21) and SD D2 (GPIO2) lines.

### Paired sleep mode

The DS-Slave rotary **Settings > Sleep** item sends the private UART control
byte `0xF6`. DOLL-OS then closes its Telnet socket, stops Wi-Fi, mutes the
amplifier, darkens the rear LED, sends the TFT controller to sleep, switches off
the backlight, and enters ESP32-S3 light sleep. RAM and the current screen/shell
state remain intact.

Pressing the slave rotary dial wakes and resets the deep-sleeping slave. Its
early boot wake beacon drives DOLL-OS's keyboard UART RX low, which wakes the
main CPU; DOLL-OS restores the preserved panel frame and amplifier state first,
then restarts Wi-Fi and the Telnet listener asynchronously. Flash both boards
when adding this feature because `0xF6`/`0xF7` are a paired protocol change.

Recommended build:
https://store.freenove.com/products/fnk0104
https://lonelybinary.com/en-us/products/esp32-s3-ipex?variant=43699253706909

The FNK hardware notes above are retained only as upstream history; this fork
targets the ESP32-P4-based Tab5 exclusively.

</details>

---

## Recommended PlatformIO build

The Tab5 display continuously scans its framebuffer from PSRAM. Use the pinned
hybrid environment in `platformio.ini`; it builds Arduino as an ESP-IDF
component with M5Stack's working UserDemo cache baseline:

- performance optimization;
- 200MHz HEX PSRAM with PSRAM XIP;
- 128KB L2 cache with 128-byte cache lines;
- the project ST7123 detection and native 80MHz DPI / 1040Mbps DSI timings.

PlatformIO Core 6.1.19 and Arduino CLI are required. Arduino CLI is used only
to generate the combined `.ino` source and prototypes; PlatformIO/ESP-IDF
performs the actual firmware build. Keep the `tab5` profile and its pinned
Arduino libraries installed. On Windows, build, flash, and verify both images
with one command:

```powershell
copy config.h.example config.h
.\ps\Flash-DualImages.ps1
```

The script installs Doll-OS (including OG Game Boy) in `ota_0`, installs the
GBA-only runtime in `ota_1` at `0x650000`, and verifies both slots. A normal
`pio run -e tab5 -t
upload` updates only Doll-OS; it does not install the second image. Pass `-Port
COMx` if Windows assigns another port, or `-SkipBuild` to reinstall already
built artifacts. Moving from the former single-image table relocates LittleFS
from `0x650000` to `0x950000`. Back up internal `/apps` and configuration files
before the first dual-image flash because they cannot be preserved in place;
SD-card ROMs and saves are unaffected.

The checked-in environments target `COM38`. Do not open a serial monitor until
upload is complete because opening the port resets the board.

On Windows, pioarduino 54.03.21's esptool 5.0.0 requires Click 8.1.8. If image
generation reports `ParamType.get_metavar`, repair the PlatformIO environment
once with:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -m pip install "click==8.1.8"
```

After building, verify that both `sdkconfig.tab5` and `sdkconfig.emulator`
contain `CONFIG_SPIRAM_SPEED_200M=y`, not `CONFIG_SPIRAM_SPEED_20M=y`. The
complete dual-image and display regression procedure is in
[TESTING_GUIDE.md](TESTING_GUIDE.md).

## Legacy Arduino IDE build

The Arduino-only build remains available for comparison and sketch development,
but its precompiled ESP-IDF libraries use the smaller cache baseline and are not
the release path for the cyan-flash fix. It produces only the `ota_0` OS image;
`gb` and `gba` require the `emulator` image built and installed through
PlatformIO.

1. Install Arduino IDE 2 and Espressif's `esp32` board package version 3.3.5.
   Do not use 3.3.6 or newer for this Tab5 build yet: Arduino-ESP32 issue
   #12417 tracks a display flicker regression introduced after 3.3.5.
2. Install `ESP32Ping` 1.6 from its GitHub release. The remaining exact library
   versions, including `EspUsbHost` 2.7.3, are declared in `sketch.yaml` and
   Arduino resolves them for the `tab5` profile. If the IDE does not activate
   the profile automatically, install `EspUsbHost` 2.7.3 from Library Manager.
3. Copy `config.h.example` to `config.h`, then add private Wi-Fi credentials,
   passwords, and API keys. `config.h` is ignored by Git.
4. Open `Doll-OS-Tab5.ino` in Arduino IDE and select the `tab5` sketch profile.
5. Connect the Tab5 over USB-C, click Verify, then Upload.

The profile selects the ESP32-P4 target, 16MB flash, 32MB PSRAM, the custom
partition table, hardware USB CDC/JTAG, M5Unified/M5GFX, and the official Tab5
Keyboard library. Touch is initialized only as part of display detection and is
never read or submitted to DOLL-OS input.

Keyboard game mode uses the inherited layout: arrows/WASD are the D-pad, N/M
are A/B, Enter is Start, and backslash is Select. Escape opens the emulator
menu and Ctrl+T quits. The `gb` command switches modes automatically; F12
toggles the same mode manually for diagnostics.

To build the same project outside the IDE:

```powershell
arduino-cli compile --profile tab5 .
```

## Inherited FNK setup notes (not used by this fork)

<details>
<summary>Show the old FNK build and DS-Slave instructions</summary>

### 1. Configure

```powershell
copy config.h.example config.h
```

Fill in Wi-Fi credentials, FTP password, and any API keys. `config.h` is
gitignored so secrets stay local. Select the hardware model once in
`BoardVariant.h`; that selection drives the panel, storage, audio, battery,
rear LED, and DS-Slave wiring together.

### 2. Build and flash

After `arduino-cli` installs the `sketch.yaml` profile libraries, apply the
project's M5GFX 0.2.26 Tab5 detection fix once:

```powershell
.\ps\Patch-M5GfxTab5Detection.ps1
```

The upstream fallback recognizes the ST7123 DSI ID but forgets to select that
panel when its touch-firmware probe times out. It also drives ST7123 at an
80MHz DPI clock and its DSI link at 1040Mbps. The project patch fixes detection
and uses conservative 50MHz/800Mbps timings; startup also raises both possible
DSI DW-GDMA read ports to maximum AXI QoS. Together these settings protect the
continuously scanned PSRAM framebuffer from blue/cyan underruns.

Open the sketch in the Arduino IDE and select the **`esp32s3 Dev Module` profile** from the
toolbar dropdown before Verify/Upload. That covers board, flash size, custom
partition scheme, PSRAM, and hardware USB CDC/JTAG console in one selection,
with USB MSC/DFU-on-boot firmware disabled. It also keeps this fork's
sketch-local `TFT_eSPI` from colliding with a global install.

If the IDE shows individual **Tools** settings instead of applying the profile,
set **USB Mode** to `Hardware CDC and JTAG`, set **USB CDC On Boot** to
`Enabled`, set **USB MSC On Boot** and **USB DFU On Boot** to `Disabled`, and set
**Upload Mode** to `UART0 / Hardware CDC` and **Upload Speed** to `115200`.

For a new FNK0104N, flash `examples/FNK0104NBringup` first. Its serial report
checks SD, battery, codec I2C, and DS-Slave pins; the panel should show three
color bands plus `PARTIAL OK` near the bottom before the full OS is installed.

Install the following libraries:
WiFi at version 3.3.10
Networking at version 3.3.10
FS at version 3.3.10
TFT_eSPI at version 2.5.43
SPI at version 3.3.10
SPIFFS at version 3.3.10
ArduinoJson at version 7.4.3
ESP32-audioI2S-master at version 3.4.4
FFat at version 3.3.10
NetworkClientSecure at version 3.3.10
SD at version 3.3.10
SD_MMC at version 3.3.10
LittleFS at version 3.3.10
HTTPClient at version 3.3.10
SimpleFTPServer at version 3.0.2
ESP32Ping at version 1.6 
esp32ARP at version 0.1.3
PubSubClient at version 2.8
ESP_I2S at version 3.3.10
Wire at version 3.3.10
LibSSH-ESP32 at version 5.8.0

Flash normally. To reset your LittleFS turn on "Erase before Writing"

> TODO: gotchas worth calling out here — sketchbook path, audio library version.
> See the porting notes.

### 3. Flash DS-Slave
Install the following libraries:
FastLED at version 3.10.5
SPI at version 3.3.10
NimBLE-Arduino at version 2.5.0
SPIFFS at version 3.3.10
FS at version 3.3.10
WiFi at version 3.3.10
Networking at version 3.3.10

flash using these settings:
- **USB Mode:** `USB-OTG (TinyUSB)`
- **USB CDC On Boot:** `Disabled`
- **USB MSC On Boot:** `Disabled`
- **USB DFU On Boot:** `Disabled`

</details>

## Commands

Run `help` on the device for the live list.

| Command | Description |
|---|---|
| `alias` `unalias` | list, create, and remove command aliases stored in `/system/conf/alias.dsys` |
| `apps` | list installed `.dapp` apps |
| `asuka` | LLM chat |
| `battery` | battery voltage / percent |
| `calc` | expression evaluator |
| `cat` `cd` `cp` `del` `ls` `mkdir` `mv` `pwd` `rm` | filesystem |
| `clear` | clear the screen |
| `dapper` | search, install, update, remove, and verify `.dapp` packages |
| `dice` | dice roller |
| `edit` | text editor, including `edit --repo <id>` for repository apps |
| `free` | heap / PSRAM report |
| `ftp` | serve the SD card over FTP |
| `gb` | Game Boy emulator |
| `help` | command list |
| `ip` `ping` | network tools |
| `motoko` | MQTT client |
| `music` | scan `/sd/music`, browse/search ID3 metadata, and play local MP3 files |
| `radio` | stream MP3 audio |
| `reboot` | restart |
| `run` | run a `.dapp` app |
| `settings` | view/set/unset runtime overrides for config.h defaults (FTP, MQTT, radio, ASUKA), stored in `/system/conf/settings.dsys` |
| `slave` | legacy compatibility command; DS-Slave is not used on Tab5 |
| `ssh` | SSH client |
| `status` | Wi-Fi status |
| `telnet` | telnet client |
| `uptime` | uptime |
| `wifi` | scan / connect / save credentials |

> TODO: expand the interesting ones with usage examples — `radio`, `gb`, `asuka`,
> `ssh`, `slave`, `run`, `dapper`.

---

## Writing `.dapp` apps

Install published apps with Dapper; see [docs/DAPPER.md](docs/DAPPER.md). The
package and repository format is documented in
[docs/DAPP-PACKAGES.md](docs/DAPP-PACKAGES.md).

> TODO: a hello-world here, then point at the docs.

```text
COLOR cyan
PRINT "hello from a DOLL-OS app"
INPUT name "name> "
PRINT "hi, $name"
EXIT
```

- [docs/DAPP.md](docs/DAPP.md) — language reference
- [docs/DAPP-BOOK.md](docs/DAPP-BOOK.md) — the long-form guide
- [dapp-web/](dapp-web/) — DOLL-OS web emulator + `.dapp` IDE/runtime, no build step

### Tab5-enhanced editions

All 46 shipped apps now use ordinary package-format-1 artifacts that declare
`# @boards m5stack-tab5`. The 27 canvas apps use 80-100 column workspaces with
30-40 rows: editors and dashboards gain visible data, while games keep their
boards readable and move scores, help, and state into side panels. Terminal-native
apps use longer 90-94 column records and 34-line pages where their data model can
benefit. Existing save-file formats remain compatible with the earlier editions.

---

## Project layout

> TODO: prune to what a newcomer actually needs to find.

```text
Doll-OS-Tab5.ino     Arduino IDE entry point, setup/loop
CommandProcessor.ino tokenizing, history, dispatch table
Display.ino          TFT panel mirror
Storage.ino          LittleFS + SD unified namespace
AppRunner.ino        .dapp interpreter
Edit.ino             text editor
Gameboy.ino          gnuboy port
Music.ino            PSRAM-backed local MP3 library/player
Radio.ino            audio streaming
Asuka.ino            LLM chat        AsukaTools.ino  its tool calls
SlaveLink.ino        no-op compatibility layer for inherited commands
KeyboardSerial.ino   official Tab5 Keyboard HID backend
src/DollInput/       built-in transport-neutral keyboard hub
global.h  config.h   shared state / local secrets
sketch.yaml          board profile + pinned libraries
apps/                bundled .dapp sources
docs/                language reference, porting notes
tools/               book generator, bundled-app regen
dapp-web/            browser emulator + `.dapp` playground
```

---

## Known issues / roadmap

> TODO. Starting points:
> - Game Boy and radio audio need extended hardware soak testing
> - Wi-Fi autoconnect vs. radio contention
> - FTP storage backend depends on a hand edit to the library's own header

---

## Credits

> TODO: upstream DOLL-OS, gnuboy, TFT_eSPI (Freenove fork), tinyexpr,
> ESP32-audioI2S, LibSSH-ESP32, SimpleFTPServer, NimBLE.

## License

> TODO.
