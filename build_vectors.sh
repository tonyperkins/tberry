#!/bin/sh
# Stage 1: vector tiles for the region. Downloads the Texas extract and the shared datasets
# Planetiler needs (a few GB the first time), then builds data/austin.mbtiles.
set -e
cd "$(dirname "$0")"
docker run --rm \
  -e JAVA_TOOL_OPTIONS="-Xmx8g" \
  -v "$(pwd)/data":/data \
  ghcr.io/onthegomap/planetiler:latest \
  --download --area=texas \
  --bounds=-99.417,28.822,-96.069,31.713 \
  --maxzoom=14 \
  --output=/data/austin.mbtiles --force
