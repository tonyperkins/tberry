# tberry

Standalone **MeshCore messenger firmware for the LilyGO T-Deck Plus**: a pocket LoRa mesh messenger
with its own keyboard, touchscreen and GPS, that needs no phone. It speaks standard MeshCore, so it
works alongside the official apps and any other MeshCore node.

The design goal is a device a non-technical person can use, with the technical controls (radio
parameters, PIN-protected) still reachable.

> Status: working daily-driver prototype. See [Known limits](#known-limits) before relying on it.

## What it does

- **Messages**: direct messages with delivery ticks and round-trip time, and channels (the Public
  channel is preconfigured). Replies use the `@[Name]` convention; tap a message to reply, hold it for
  details. Quoted parents, reactions shown as badges, mentions of you highlighted. The message box has
  an emoji picker and a button to clear the text; emoji are drawn from the SD card. Hashtag channels
  (`#test`, `#austin`, ...) can be joined with the **+** button and left from their thread.
- **Message details**: route, hop count, SNR and RSSI, the repeater path (named when unambiguous),
  clock difference, airtime, how many copies were heard.
- **Signal**: one-tap range test to a contact (round trip, hops, RSSI, SNR), logged to the SD card.
- **Nodes**: everyone heard, with signal and age.
- **Map**: offline map from the SD card with your position, nodes that advertise one, and "show where
  this message came from".
- **Status**: battery, uptime, packets, airtime, storage, health, and GPS detail (fix, satellites,
  accuracy, altitude, speed).
- **Settings**: name, time zone, daylight saving, GPS, brightness, volume, channel sounds, screen
  timeout, and a PIN-protected **Advanced** area for the radio (preset, frequency, bandwidth, SF, CR,
  TX power) with Send advert, Run setup again and Restore defaults.
- **First-boot setup**: a four-screen guide (welcome, name, time zone, gesture tips) the first time a
  new device is woken. It can be run again from Settings, Advanced.
- **Interface**: lock screen that resumes the screen you were on (including an unsent message),
  home grid, edge swipes (left edge: back, bottom edge: home, top edge: quick panel with brightness,
  volume, advert, lock, GPS), sound effects.

## Repository layout

| Folder | What |
|---|---|
| [`tdeck-os/`](tdeck-os) | the firmware (PlatformIO, Arduino, LVGL) |
| [`tdeck-os/docs/backlog/`](tdeck-os/docs/backlog) | design notes and tickets, one file each |
| [`tdeck-maps/`](tdeck-maps) | builds the offline map tiles for the SD card |
| [`tdeck-hello/`](tdeck-hello) | the small hardware bring-up firmware (display, touch, keyboard, radio) |

## Building

Requirements: [PlatformIO](https://platformio.org/) and a LilyGO T-Deck Plus on USB.

```sh
git clone --recurse-submodules <this repository>
cd tberry/tdeck-os
pio run -e tdeck                              # build
pio run -e tdeck -t upload --upload-port /dev/ttyACM0
```

Notes that cost time to find out:

- The platform is pinned (`espressif32@6.12.0`). An unpinned `espressif32` resolves to a newer SDK
  that lacks what MeshCore needs.
- If PlatformIO's esptool fails with `No module named 'intelhex'`, run
  `pipx inject platformio intelhex`.
- MeshCore is a submodule at a pinned commit; update it deliberately, not with a plain `git pull`.
- The radio and the SD card must share the display's SPI object (`TFT_eSPI::getSPIinstance()`); the
  global `SPI` object re-routes the pins and blanks the display.
- Do not call `Serial.setTxTimeoutMs(0)`: on the ESP32-S3's USB serial it makes a write wait forever
  when nobody is reading the port, which looks exactly like a frozen device. All logging goes through
  `logf()` in `main.cpp`, which only writes when there is room.

## SD card

FAT32. Everything is optional; without a card the device works but does not keep message history.

```
/tdos/msgs/<key>.log    message history, one append-only file per conversation
/tdos/signal.csv        range-test results
/tdos/idlog.csv         research log for reaction codes (see TDOS-008)
/maps/<z>/<x>/<y>.png   map tiles   (see tdeck-maps/README.md)
/emoji/<codepoint>.bin  emoji images (see tdeck-os/tools/make_emoji_pack.py)
/emoji/pack.bin         the same images in one file (see below)
```

### Emoji pack

`tdeck-os/tools/make_emoji_pack.py` converts the Noto emoji into small image files. Copy the output
folder to the card as `/emoji/`, then bundle it with

```sh
python3 tdeck-os/tools/pack_emoji.py emoji emoji/pack.bin
```

and put `pack.bin` in `/emoji/` too. Looking up one file among 1,400 on a FAT card takes tens of
milliseconds, which made the emoji picker take seconds to open. With `pack.bin` the firmware reads
everything into PSRAM once at start-up (about a second, 864 KB) and lookups are instant. Without it,
emoji still work, just slower.

Mount the card without the `flush` option when copying tens of thousands of tiles
(`mount -o noatime,async`): the desktop default is roughly 30 times slower.

## Updating over the air

Settings has **WiFi** (pick a network, type the password) and **Software update**. The device
fetches `releases/latest/download/manifest.json` from this repository's GitHub releases, shows the
version on offer, and installs it when asked. Nothing is installed unless its Ed25519 signature,
size and SHA-256 all match; the public key is compiled into the firmware.

To publish: raise `TDOS_VERSION` and `TDOS_BUILD` in `tdeck-os/include/version.h`, then run
`tdeck-os/tools/publish_release.sh "notes"`. The signing key lives outside the repository at
`~/.config/tberry/signing.key` and must be backed up; without it devices can only be updated over
USB. `tools/make_ca_bundle.py` regenerates the certificate bundle used for HTTPS.

## Debug build and remote control

`pio run -e tdeck-remote` builds the same firmware with a serial remote control compiled in. With it,
`tdeck-os/tools/remote.py` can tap, swipe, type, move the trackball and save screenshots over USB, for
example `python3 tdeck-os/tools/remote.py "tap 160 120" "wait 500" "shot home.png"`. The normal build
leaves this out, About shows "debug" when it is present, and `tdeck-os/tools/release.sh` builds the
release image and checks that no debug code is in it.

## Privacy

The device does **not** broadcast its position. Identity (the private key), settings and contacts
live in the device's own flash, never on the removable card.

## Known limits

- Reaction codes are not decoded, so sending reactions is not implemented (TDOS-008).
- The thread view draws only the newest 36 messages of a conversation.
- The message box takes emoji from the picker only; the T-Deck keyboard has no emoji keys, and
  holding a key does not repeat (the keyboard sends one event per press).
- No real-time clock chip: the clock is blank after power-up until GPS or a mesh advert provides the
  time.
- The font covers Latin-1 and Latin Extended-A; other scripts show as `?`.
- Only the US radio preset has been tested on air.

## Licence

MIT, see [LICENSE](LICENSE). Third-party components and data have their own licences, listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
