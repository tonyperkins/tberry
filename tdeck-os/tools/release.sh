#!/bin/sh
# Build the release firmware (no remote control) and check that no debug code is in the image.
# Flash it with:  pio run -e tdeck -t upload --upload-port /dev/ttyACM0
set -e
cd "$(dirname "$0")/.."
pio run -e tdeck
BIN=.pio/build/tdeck/firmware.bin
if grep -aq "swipe %d %d %d %d" "$BIN"; then
  echo "ERROR: remote-control code found in $BIN" >&2
  exit 1
fi
echo "release image OK: $(wc -c < "$BIN") bytes, no remote-control code"
