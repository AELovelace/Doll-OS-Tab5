# DOLL-OS settings guide

DOLL-OS has two interfaces named Settings:

1. The shell `settings` command stores persistent configuration overrides.
2. The DS-Slave rotary Settings menu controls pairing, game mode, reconnection,
   and sleep.

This guide covers every supported persistent key and every rotary-menu control.

## Shell settings commands

| Command | Result |
| --- | --- |
| `settings` | List saved overrides. Keys containing `pass` or `key` are masked. |
| `settings help` | Show command syntax and the built-in key list. |
| `settings get <key>` | Show whether a key is set and, if set, its stored value. |
| `settings set <key> <value>` | Create or replace an override. Values may contain spaces. |
| `settings unset <key>` | Remove an override and return to the compiled `config.h` default. |

Example:

```text
settings
settings set radio.volume 12
settings get radio.volume
settings unset radio.volume
reboot
```

Key names are case-sensitive; use the lowercase spelling shown below. DOLL-OS
accepts unknown keys, but they have no effect because no subsystem reads them.
A reboot after `set` or `unset` is the reliable way to apply changes.

Settings are stored in `/system/conf/settings.dsys`. They normally survive an
app-only firmware update, but erasing the filesystem removes them. The store
holds at most 32 entries. Keys may be at most 32 characters and cannot contain
spaces, quotes, `#`, or `=`. Values are trimmed and limited to 160 characters.

### Secret warning

The plain `settings` listing masks keys whose names contain `pass` or `key`.
However, `settings get <key>` prints the stored value literally. Do not run
`settings get ftp.pass`, `settings get asuka.brave_key`, or
`settings get asuka.owm_key` while streaming, taking screenshots, or sharing a
terminal log. The settings file itself is plaintext, not encrypted storage.

## Complete key reference

There are 15 settings currently consumed by firmware.

### FTP server

| Key | Value | Purpose |
| --- | --- | --- |
| `ftp.user` | Username text | Login name accepted by `ftp on`. |
| `ftp.pass` | Password text | Login password accepted by `ftp on`; masked in listings. |

```text
settings set ftp.user doll-os
settings set ftp.pass replace-this-password
reboot
```

FTP is plaintext and should be exposed only on a trusted LAN. If it is already
running, stop and restart it—or reboot—before testing changed credentials.

### Motoko MQTT client

| Key | Value | Purpose |
| --- | --- | --- |
| `motoko.broker` | Hostname or IPv4 address without a URL scheme | Default MQTT broker used by bare `motoko`. |
| `motoko.port` | Integer `1` through `65535` | Default MQTT TCP port; normally `1883`. |

```text
settings set motoko.broker mqtt.example.net
settings set motoko.port 1883
reboot
```

Arguments typed directly after `motoko` override these defaults for that launch.

### Internet radio

| Key | Value | Purpose |
| --- | --- | --- |
| `radio.url` | `http://` or `https://` stream URL | Default station used when `radio play` has no URL. |
| `radio.volume` | Integer `0` through `21` | Initial radio/audio volume. Invalid values are ignored. |
| `radio.directory_url` | `http://` or `https://` directory endpoint | Source used by the station browser/list. |

```text
settings set radio.url https://radio.example.net/live.mp3
settings set radio.volume 12
settings set radio.directory_url https://radio.example.net/directory.json
reboot
```

`radio vol <0-21>` changes the active session. Set `radio.volume` when that
level should become the post-boot default.

### ASUKA LLM endpoint

| Key | Value | Purpose |
| --- | --- | --- |
| `asuka.llm_host` | Hostname or IP address without `http://` | Host running the OpenAI-compatible chat endpoint. |
| `asuka.llm_port` | Integer `1` through `65535` | TCP port for the LLM endpoint. |
| `asuka.llm_path` | HTTP path beginning with `/` | Request path, commonly `/v1/chat/completions`. |

```text
settings set asuka.llm_host 192.168.1.50
settings set asuka.llm_port 9090
settings set asuka.llm_path /v1/chat/completions
reboot
```

Inside ASUKA, `/host` and `/port` change only the running session. Persistent
endpoint changes belong in the settings keys above.

### ASUKA search and weather

| Key | Value | Purpose |
| --- | --- | --- |
| `asuka.brave_key` | Brave Search API key | Enables ASUKA's live search tool. |
| `asuka.owm_key` | OpenWeather API key | Enables ASUKA's weather tool. |
| `asuka.owm_location` | Human-readable place name | Label displayed with weather results; spaces are allowed. |
| `asuka.owm_lat` | Decimal `-90` through `90` | Weather-query latitude. |
| `asuka.owm_lon` | Decimal `-180` through `180` | Weather-query longitude. |

Example using deliberately fake credentials:

```text
settings set asuka.brave_key YOUR_BRAVE_API_KEY
settings set asuka.owm_key YOUR_OPENWEATHER_API_KEY
settings set asuka.owm_location Portland, Oregon, US
settings set asuka.owm_lat 45.5152
settings set asuka.owm_lon -122.6784
reboot
```

Use both coordinate keys when changing location. The label is descriptive; the
coordinates select where OpenWeather queries. Inside ASUKA,
`/weather <lat> <lon> <label>` changes only the current session.

## Copy-and-edit template

Only set values that should differ from the compiled defaults:

```text
settings set ftp.user YOUR_FTP_USER
settings set ftp.pass YOUR_FTP_PASSWORD
settings set motoko.broker YOUR_MQTT_HOST
settings set motoko.port 1883
settings set radio.url YOUR_STREAM_URL
settings set radio.volume 12
settings set radio.directory_url YOUR_DIRECTORY_URL
settings set asuka.llm_host YOUR_LLM_HOST
settings set asuka.llm_port 9090
settings set asuka.llm_path /v1/chat/completions
settings set asuka.brave_key YOUR_BRAVE_API_KEY
settings set asuka.owm_key YOUR_OPENWEATHER_API_KEY
settings set asuka.owm_location YOUR_CITY_OR_LABEL
settings set asuka.owm_lat YOUR_LATITUDE
settings set asuka.owm_lon YOUR_LONGITUDE
reboot
```

Do not paste the template unchanged: placeholders do not work, and unnecessary
overrides hide future improvements to compiled defaults.

## Returning a key to its default

`unset` removes the override so firmware uses the value compiled into `config.h`:

```text
settings unset radio.url
settings unset radio.volume
reboot
```

Run `settings` afterward to confirm the overrides disappeared. If it reports
`(none -- all values are using their config.h defaults)`, none remain.

## DS-Slave rotary Settings controls

The rotary encoder has one turn control and one push switch. There are no hidden
keyboard keys for this menu.

### Normal volume screen

| Physical action | Result |
| --- | --- |
| Turn clockwise | Raise volume when `SLAVE_ROTARY_REVERSED` matches the installed wiring. |
| Turn counter-clockwise | Lower volume. |
| Press and release the shaft | Open Settings with **Pair Device** selected. |

If volume moves backward, swap A/CLK and B/DT or toggle
`SLAVE_ROTARY_REVERSED` in DS-Slave's `BoardVariant.h` and reflash it.

### Settings menu controls

| Physical action | Result |
| --- | --- |
| Turn clockwise | Move to the next item. |
| Turn counter-clockwise | Move to the previous item. |
| Press and release | Apply the highlighted item and normally return to volume mode. |

The list wraps at both ends. There is no long-press function or separate Back
key; select **Exit** to close the menu without changing anything.

### Rotary menu items

| Item | What pressing the encoder does |
| --- | --- |
| **Pair Device** | Enables pairing and scans for one additional BLE HID device. Existing devices remain saved. It will not start if both peer slots are occupied. |
| **Game Mode** | Toggles Game Mode. On sends held Game Boy button events; off restores ordinary keystroke behavior. |
| **Terminate App** | Closes whatever app owns the DOLL-OS panel and returns it to the shell. Sends both abort chords — `^X` then Ctrl+T — because different apps honour different ones. |
| **Reconnect** | Leaves pairing mode and scans for previously saved devices. |
| **Sleep** | Requests paired sleep for DOLL-OS, turns off the slave OLED/status LED, and places DS-Slave in deep sleep. |
| **Exit** | Closes Settings and returns the dial to volume control. |

After choosing **Sleep**, release the dial and press it once to wake. The wake
press restarts DS-Slave and wakes the main unit; it is not reused as another
Settings-menu press.

**Terminate App** is the escape hatch for a build with no keyboard attached: a
game launched from the button bar, or a `.dapp` stuck in a loop, otherwise has no
way out. It covers `gb`, the music player, a running `.dapp`, and an ssh or telnet
session. Two exceptions: `edit` treats it as its own `^X`, so a modified buffer
still asks whether to save, and at the bare prompt with nothing playing both bytes
are discarded, making a mistaken Terminate harmless. Game Mode is left as it is —
the emulator sends its own `GAME 0` as it exits.

In the music player it also **stops the track**, unlike `q` or Escape, which close
the screen and deliberately leave playback running in the background. That is the
difference between leaving an app and terminating it, and it means one press of the
dial genuinely silences the device. It takes precedence over the player's search
box, so a terminate is never swallowed by a half-typed filter.

Because `q` and Escape leave a track playing, that track can outlive the screen it
was started from — so a Terminate **at the shell prompt** stops it too, printing
`music: terminated`. The rule is the same one press to press: terminate stops
whatever is running, and once the player is closed the track is what is running. An
app always gets the chord first, so this only ever applies at an idle prompt. A
radio stream is not covered — it is background audio rather than an app, and `radio
stop` or the bar's middle button already turns it off.

## Troubleshooting

- **A saved key has no effect:** check spelling and case, then reboot. Unknown
  keys are stored but ignored.
- **A secret appears as `****`:** this is normal in the bare `settings` list.
- **Radio volume is unchanged:** use an integer from 0 through 21 and reboot.
- **Motoko cannot connect:** omit `mqtt://`, verify the port, and connect Wi-Fi.
- **ASUKA cannot connect:** omit `http://` from `asuka.llm_host`, start the path
  with `/`, and verify the host and port from the DOLL-OS network.
- **Weather names the right place but reports the wrong weather:** update both
  coordinate keys; the label does not select the coordinates.
- **The rotary moves backward:** swap A and B or change the reversal setting.
