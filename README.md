# DOLL-OS for M5Stack Tab5

> **Port status:** foundation work is in progress. The committed FNK application
> is preserved as the upstream baseline while the Tab5 display, storage, audio,
> power, and keyboard backends are brought up. See
> [the Tab5 port plan](docs/TAB5_PORT_PLAN.md) and
> [the upstream-sync policy](docs/UPSTREAM_SYNC.md).

This fork targets only the M5Stack Tab5. Its local interface is the official
Tab5 Keyboard, with BLE HID and USB HID keyboards joining the same input hub.
Touchscreen input is intentionally disabled for the initial releases.

The text below documents the inherited FNK baseline and will be revised as each
Tab5 milestone becomes functional.

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
- **Radio** — MP3 stream playback over I²S (ES8311 codec)
- **Music library** — full-screen local MP3 player with recursive `/sd/music`
  scanning, ID3 metadata, search, and PSRAM-backed catalog storage
- **Game Boy emulator** — `gb`, gnuboy port, gamepad via DS-Slave
- **ASUKA** — local LLM chat with tool calling (search / weather / URL fetch / time)
- **BLE input bridge** — DS-Slave connects keyboard + gamepad at once and merges
  them into one UART stream

---

## Hardware

| | |
|---|---|
| Board | Freenove FNK0104-series ESP32-S3 display kit |
| Panel | 2.8" 240×320 ILI9341, 3.5" 320×480 ST77922 QSPI, or 4.0" ST7796 |
| Flash / PSRAM | 16MB flash, OPI PSRAM (required) |
| Storage | SD_MMC card slot + LittleFS |
| Audio | ES8311 codec over I²S |
| Companion | second ESP32-S3 running `DS-Slave` (BLE HID → UART) |

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

NOTE: THIS VERSION OF DOLL-OS REQUIRES A 16R8, AND IS NOT GUARANTEED TO WORK ON ANYTHING OTHER THAN
AN S3
---

## Getting started

### 1. Configure

```powershell
copy config.h.example config.h
```

Fill in Wi-Fi credentials, FTP password, and any API keys. `config.h` is
gitignored so secrets stay local. Select the hardware model once in
`BoardVariant.h`; that selection drives the panel, storage, audio, battery,
rear LED, and DS-Slave wiring together.

### 2. Build and flash

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
| `slave` | talk to DS-Slave |
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

---

## Project layout

> TODO: prune to what a newcomer actually needs to find.

```text
DS.ino               entry point, setup/loop
CommandProcessor.ino tokenizing, history, dispatch table
Display.ino          TFT panel mirror
Storage.ino          LittleFS + SD unified namespace
AppRunner.ino        .dapp interpreter
Edit.ino             text editor
Gameboy.ino          gnuboy port
Music.ino            PSRAM-backed local MP3 library/player
Radio.ino            audio streaming
Asuka.ino            LLM chat        AsukaTools.ino  its tool calls
SlaveLink.ino        outbound channel to DS-Slave
KeyboardSerial.ino   inbound keystrokes from DS-Slave
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
> - Game Boy audio is still muted
> - Wi-Fi autoconnect vs. radio contention
> - FTP storage backend depends on a hand edit to the library's own header

---

## Credits

> TODO: upstream DOLL-OS, gnuboy, TFT_eSPI (Freenove fork), tinyexpr,
> ESP32-audioI2S, LibSSH-ESP32, SimpleFTPServer, NimBLE.

## License

> TODO.
