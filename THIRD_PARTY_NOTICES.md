# Third-party software and data

This project builds on the work below. Each item keeps its own licence; the list is for orientation,
so please read the licence files shipped by each project.

## Linked into the firmware

| Component | Used for | Licence |
|---|---|---|
| [MeshCore](https://github.com/meshcore-dev/MeshCore) (git submodule, unmodified) | mesh protocol, routing, crypto, contacts | MIT |
| [RadioLib](https://github.com/jgromes/RadioLib) | SX1262 radio driver | MIT |
| [LVGL](https://github.com/lvgl/lvgl) 8.3 | user interface | MIT |
| [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) | display driver | FreeBSD-style (see its LICENSE) |
| [PNGdec](https://github.com/bitbank2/PNGdec) | map tile decoding | Apache 2.0 |
| [Arduino Crypto](https://github.com/rweather/arduinolibs), [RTClib](https://github.com/adafruit/RTClib), [CayenneLPP](https://github.com/ElectronicCats/CayenneLPP), [base64](https://github.com/Densaugeo/base64_arduino), ed25519 (Orson Peters) | pulled in by MeshCore | MIT / zlib |
| [arduino-esp32](https://github.com/espressif/arduino-esp32) and ESP-IDF | platform | LGPL 2.1 / Apache 2.0 |

## Fonts

`tdeck-os/src/fonts/` holds Montserrat Medium (Julieta Ulanovsky), converted to LVGL's format with
`lv_font_conv`. SIL Open Font License 1.1.

## Data produced by the tooling (not stored in the repository)

| Data | Source | Licence |
|---|---|---|
| Emoji images on the SD card (`tools/make_emoji_pack.py`) | [Noto Emoji](https://github.com/googlefonts/noto-emoji), Google | Apache 2.0 (region flags are public domain) |
| Map tiles on the SD card (`tdeck-maps`) | map data (c) OpenStreetMap contributors | ODbL 1.0, attribution required (shown on the map) |
| Map style | [Dark Matter](https://github.com/openmaptiles/dark-matter-gl-style), derived from CartoDB Basemaps by Stamen and Paul Norman | code BSD-3-Clause, design CC-BY 3.0 |
| Vector tiles | [Planetiler](https://github.com/onthegomap/planetiler), OpenMapTiles schema | Apache 2.0 / CC-BY 4.0 for the schema |
| Glyph fonts for rendering labels | Noto Sans, via tileserver-gl-styles (`tdeck-maps/server/fonts`) | SIL OFL 1.1 |

## Reference only

The pin assignments and hardware behaviour of the LilyGO T-Deck Plus were cross-checked against
LilyGO's published pin map and against other open firmware for the same board. No code was copied.
