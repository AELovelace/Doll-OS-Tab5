# DOLL-OS + DS-Slave complete wiring guide

This is the complete wiring reference for a Freenove FNK0104 DOLL-OS main unit
and a Lonely Binary ESP32-S3 N16R8/IPEX companion running DS-Slave. It includes
the optional 128x64 status OLED, rotary encoder, and external antenna.

The firmware pin maps are the source of truth. Select the correct Freenove model
in `BoardVariant.h` before compiling because the FNK0104N uses different UART
pins from the FNK0104A/B/S boards.

## Parts

- One Freenove FNK0104 ESP32-S3 display board:
  - FNK0104A/B: 2.8-inch 240x320 ILI9341;
  - FNK0104N: 3.5-inch 320x480 ST77922; or
  - FNK0104S: 4.0-inch 320x480 ST7796-family panel.
- One Lonely Binary ESP32-S3 IPEX board, N16R8 recommended, for DS-Slave.
- The supplied 2.4 GHz IPEX/U.FL antenna for the DS-Slave board.
- One four-pin 128x64 SSD1306 I2C OLED configured at address `0x3C`.
- One rotary encoder:
  - a bare EC11-style encoder; or
  - a 5-pin KY-040-style module labelled `CLK`, `DT`, `SW`, `+`, and `GND`.
- Three wires for the UART link, or five when the main unit also supplies power.
- Suitable USB cables and a regulated 5 V supply.

## Read this before wiring

- Turn all power off before connecting or moving wires.
- Every GPIO in this build uses **3.3 V logic**. Never apply 5 V to a GPIO,
  UART, I2C, or encoder signal pin.
- Cross the UART signals: TX goes to RX and RX goes to TX.
- All connected devices must share ground.
- Follow the labels printed on modules. Do not trust connector position or wire
  colour; OLED and encoder manufacturers frequently rearrange their pins.
- Do not join two independently powered 5 V/VIN rails. If both ESP32 boards are
  attached to USB, connect only UART TX, UART RX, and GND between them.
- The IPEX slave board must have its external 2.4 GHz antenna attached before
  normal Wi-Fi/BLE use.

## System overview

```text
Lonely Binary ESP32-S3 (DS-Slave)
     |-- GPIO8 ------- SSD1306 serial data (SDA/DAT)
     |-- GPIO9 ------- SSD1306 serial clock (SCL/CLK)
     |-- GPIO5 ------- rotary push switch (SW)
     |-- GPIO6 ------- rotary clock/channel A (CLK/A)
     |-- GPIO7 ------- rotary data/channel B (DT/B)
     |-- GPIO17 TX --> DOLL-OS UART RX
     |<- GPIO18 RX --- DOLL-OS UART TX
     |-- GND --------- DOLL-OS UART GND
     `<- 5V/VIN ------ DOLL-OS UART 5V OUT

DOLL-OS main UART pins:
  FNK0104A/B/S: RX GPIO21, TX GPIO2
  FNK0104N:     RX GPIO46, TX GPIO45
```

## 1. Main unit to DS-Slave UART

Both ends use 115200 baud, 8 data bits, no parity, and one stop bit.

| Signal | DS-Slave endpoint | FNK0104A/B/S endpoint | FNK0104N endpoint |
| --- | --- | --- | --- |
| Keystrokes and wake controls | GPIO17 `TX` | GPIO21 `RX` | GPIO46 `RX` |
| Commands to the slave | GPIO18 `RX` | GPIO2 `TX` | GPIO45 `TX` |
| Power | `5V`/`VIN` | UART-header `5V OUT` | UART-header `5V OUT` |
| Reference | `GND` | UART-header `GND` | UART-header `GND` |

In plain language:

### FNK0104A, FNK0104B, or FNK0104S

- DS-Slave GPIO17 -> Freenove GPIO21.
- DS-Slave GPIO18 <- Freenove GPIO2.
- DS-Slave GND <- Freenove UART-header GND.
- DS-Slave 5V/VIN <- Freenove UART-header 5V OUT.

### FNK0104N

- DS-Slave GPIO17 -> Freenove GPIO46.
- DS-Slave GPIO18 <- Freenove GPIO45.
- DS-Slave GND <- Freenove UART-header GND.
- DS-Slave 5V/VIN <- Freenove UART-header 5V OUT.

GPIO21 and GPIO2 are not available for the link on the FNK0104N: that model
already uses them for I2S audio word-select and SD-card D2. GPIO45 is kept on
the main unit's TX side so the slave cannot drive that ESP32-S3 strapping pin
during reset.

## 2. Power wiring

The reference build uses the Freenove board's UART-header 5 V output to power
DS-Slave. The complete main-to-slave harness therefore has five conductors:
5V, GND, main TX, main RX, and the corresponding crossed slave UART signals.

### Normal installed arrangement

- Power the Freenove main board from its normal USB-C connector or regulated
  main supply.
- Freenove UART-header `5V OUT` -> DS-Slave `5V`/`VIN`.
- Freenove UART-header `GND` -> DS-Slave `GND`.
- Leave the DS-Slave UART/programming USB connector unplugged during normal use.

The FNK board now powers the companion, OLED, and encoder.

### Temporary flashing/debug arrangement

- Turn off and unplug the FNK main unit.
- Disconnect the FNK UART-header `5V OUT` wire from DS-Slave.
- Power/program DS-Slave through its UART/programming USB-C connector.
- UART TX/RX and GND may remain connected if the unpowered FNK board tolerates
  them, but disconnecting the entire five-wire harness is the safest option.
- Remove the programming USB cable before restoring the FNK 5V OUT wire.

Never connect FNK 5V OUT and a live DS-Slave USB 5 V source simultaneously;
that can backfeed one board from the other.

## 3. SSD1306 status OLED

The supported panel is a four-pin 128x64 SSD1306 I2C module at address `0x3C`.
Read the labels printed beside the OLED header and wire it only to the DS-Slave
board:

| OLED pin label | What it does | Lonely Binary DS-Slave pin |
| --- | --- | --- |
| `GND` | Power and signal ground | **GND** |
| `VCC`, `VDD`, or `VIN` | OLED power input | **3V3** |
| `SCL`, `SCK`, or `CLK` | I2C serial clock | **GPIO9** |
| `SDA`, `DAT`, or `DIN` | I2C serial data | **GPIO8** |

In one line: **GND -> GND, VCC -> 3V3, SCL -> GPIO9, and SDA ->
GPIO8.** Many four-pin modules happen to arrange their header as
`GND, VCC, SCL, SDA`, but this order is not guaranteed. Follow the silkscreen
labels on the particular OLED instead of copying left-to-right pin positions
from a photograph. Do not power an unlabeled module until its pinout has been
confirmed from its listing, schematic, or continuity measurements.

Use 3.3 V for the OLED even when its listing claims 5 V compatibility. This
keeps any module pull-up resistors at a safe ESP32-S3 logic level. The firmware
probes address `0x3C`; a module strapped to `0x3D` must be reconfigured or built
with a matching `SLAVE_OLED_ADDRESS` override.

## 4. Rotary encoder

The firmware enables internal pull-ups. Turning the encoder changes volume;
pressing it enters/selects the on-screen settings menu and also wakes the slave
from deep sleep.

For every menu item and control action, see the
[DOLL-OS settings guide](docs/SETTINGS_GUIDE.md#ds-slave-rotary-settings-controls).

### Five-pin KY-040-style module

Read the labels printed beside the module header and connect them as follows:

| Encoder module label | What it does | Lonely Binary DS-Slave pin |
| --- | --- | --- |
| `SW` | Shaft push-button signal | **GPIO5** |
| `CLK` | Rotation clock / channel A | **GPIO6** |
| `DT` | Rotation data / channel B | **GPIO7** |
| `+` or `VCC` | Module pull-up power | **3V3** |
| `GND` | Common ground | **GND** |

In one line: **SW -> GPIO5, CLK -> GPIO6, DT -> GPIO7, + -> 3V3, and
GND -> GND.** Header order varies between module manufacturers, so follow the
printed `SW/CLK/DT/+/GND` labels rather than copying the physical order in a
photograph.

Never connect the encoder module's `+` pin to 5 V. Its pull-ups would then put
5 V on the ESP32-S3 inputs.

### Bare mechanical encoder

| Bare encoder contact | What it does | Lonely Binary DS-Slave pin |
| --- | --- | --- |
| Switch contact 1 | Shaft push-button signal | **GPIO5** |
| Switch contact 2 | Shaft push-button return | **GND** |
| Channel A contact | Rotation clock | **GPIO6** |
| Channel B contact | Rotation data | **GPIO7** |
| Encoder common contact | Common return for A and B | **GND** |

Most bare EC11-style encoders have a group of three rotation contacts and a
separate group of two push-switch contacts:

- The **middle terminal in the three-contact group is usually encoder common**;
  connect it to GND.
- The **two outside terminals in that group are A and B**; connect A/CLK to
  GPIO6 and B/DT to GPIO7.
- The **separate pair are the push switch**; connect either one to GPIO5 and the
  other to GND. A mechanical switch has no polarity.

Do not rely on left-versus-right terminal position without checking the part's
datasheet. With power disconnected, a multimeter in continuity mode identifies
the switch pair because those two contacts short only while the shaft is
pressed. The three remaining contacts are the rotary common, A, and B.

A passive bare encoder needs no VCC wire. If clockwise operation is reversed,
either exchange the A/B wires or change `SLAVE_ROTARY_REVERSED` in the
DS-Slave `BoardVariant.h` file.

## 5. Status RGB LED and antenna

- The reference Lonely Binary board's single WS2812 status pixel is onboard on
  GPIO48. It needs no external wiring.
- Attach the supplied 2.4 GHz antenna to the board's IPEX/U.FL socket. Press the
  plug straight down; do not lever or twist the tiny socket.
- Keep the antenna away from the display back, battery, ground planes, and
  bundles of power wire when arranging the enclosure.

## 6. Connections that are already onboard

The Freenove TFT, touch controller where fitted, SD slot, ES8311 audio codec,
speaker amplifier, battery sensing, backlight, and rear RGB LED are built into
the FNK0104 kit. Do not add jumper wires for them. `BoardVariant.h` and
`BoardPins.h` select their internal wiring.

The DS-Slave status WS2812 is also onboard. The only external DS-Slave wiring
is therefore UART/power, OLED, rotary encoder, and the snap-on antenna.

## 7. Pre-power checklist

- [ ] The selected `BoardVariant.h` entry matches the physical FNK0104 model.
- [ ] DS-Slave GPIO17 goes to the main unit's RX, not its TX.
- [ ] DS-Slave GPIO18 goes to the main unit's TX, not its RX.
- [ ] The two boards share GND.
- [ ] No GPIO is connected to 5 V.
- [ ] OLED VCC and encoder-module VCC are connected to 3V3.
- [ ] FNK UART-header 5V OUT reaches DS-Slave 5V/VIN.
- [ ] No live DS-Slave programming USB cable is backfeeding that 5 V link.
- [ ] The IPEX antenna is installed.
- [ ] There are no loose strands or solder bridges.

## 8. First boot and test

1. Flash DOLL-OS with the correct Freenove variant selected.
   For an FNK0104S, keep its display bus at the project setup's stable 40 MHz
   setting. If the enclosure mounts the panel upside down, set
   `DOLL_DISPLAY_UPSIDE_DOWN` to `1` in `BoardVariant.h` before compiling.
2. Flash DS-Slave using the currently documented board settings.
3. Open the slave's UART debug console at 115200 baud. A healthy boot reports:
   - `UART1 TX=17 RX=18 baud=115200`;
   - `Status WS2812 pin=48`;
   - the SSD1306 at `0x3C` on SDA 8/SCL 9;
   - the rotary pins A 6/B 7/button 5.
4. Confirm the OLED displays `DS-SLAVE` and status information.
5. Turn and press the encoder. If rotation is backward, swap A/B or change the
   reversal setting.
6. Pair a BLE keyboard/controller and type into DOLL-OS.
7. From DOLL-OS, run `slave status`. The request travels to the companion; its
   detailed reply appears on the slave's UART debug console.
8. Test the rotary Settings > Sleep action and press the dial to wake both units.

For the FNK0104S panel, persistent colored static or speckling usually means the
SPI transfer is unstable; verify that only the 40 MHz `SPI_FREQUENCY` line is
enabled in `libraries/TFT_eSPI_Setups/FNK0104S_4.0_320x480_ST7796.h`. A clean,
stable picture with red and blue exchanged is instead a color-order problem and
should be diagnosed separately from bus noise.

## Quick wiring block for Reddit

```text
DS-SLAVE UART (Lonely Binary ESP32-S3 N16R8/IPEX)
  GPIO17 TX -> FNK0104A/B/S GPIO21 RX
            -> FNK0104N     GPIO46 RX
  GPIO18 RX <- FNK0104A/B/S GPIO2 TX
            <- FNK0104N     GPIO45 TX
  5V/VIN    <- FNK0104 UART-header 5V OUT
  GND       <- FNK0104 UART-header GND

SSD1306 128x64 I2C OLED, address 0x3C
  GND (power/signal ground) -> GND
  VCC/VDD/VIN (power)       -> 3V3
  SCL/SCK/CLK (I2C clock)   -> GPIO9
  SDA/DAT/DIN (I2C data)    -> GPIO8

KY-040 ROTARY MODULE
  SW  (push switch)       -> GPIO5
  CLK (clock/channel A)   -> GPIO6
  DT  (data/channel B)    -> GPIO7
  +   -> 3V3
  GND -> GND

ONBOARD
  Status WS2812 = GPIO48, no wire
  IPEX/U.FL     = supplied 2.4GHz antenna
```

Reference hardware: [Freenove FNK0104 project](https://github.com/Freenove/Freenove_ESP32_S3_Display)
and [Lonely Binary ESP32-S3 IPEX N16R8](https://lonelybinary.com/en-us/products/esp32-s3-ipex?variant=43699253706909).
