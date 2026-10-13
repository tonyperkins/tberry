# tdeck-maps

Builds the offline map tiles that tdeck-os reads from the SD card (`/maps/<z>/<x>/<y>.png`).

Pipeline, all offline once downloaded:

1. **Planetiler** turns an OpenStreetMap extract into vector tiles (OpenMapTiles schema).
2. **tileserver-gl** renders those with a dark MapLibre style into 256 px PNG tiles.
3. `render_tiles.py` walks the bounding box and writes the SD card layout.

Region: Austin, TX plus 100 miles. Centre 30.2672 N, 97.7431 W.
Bounding box: S 28.822, N 31.713, W -99.417, E -96.069.

## Rebuilding

```sh
./build_vectors.sh                       # ~5 min, downloads ~2 GB the first time   # writes data/austin.mbtiles; must exist before docker run
python3 prepare_style.py                 # adapts + brightens the Dark Matter style
docker run -d --name tdeck-tiles -p 8080:8080 \
  -v "$PWD/server":/data -v "$PWD/data/austin.mbtiles":/data/austin.mbtiles:ro \
  maptiler/tileserver-gl:latest --config /data/config.json
# glyph fonts: docker cp tdeck-tiles:/usr/src/app/node_modules/tileserver-gl-styles/fonts/. server/fonts/
python3 render_tiles.py --bbox -99.417 28.822 -96.069 31.713 --zooms 6 14 --out out --palette 64
python3 render_tiles.py --centre 30.2672 -97.7431 --radius-km 40 --zooms 15 15 --out out --palette 64
python3 render_tiles.py --centre 30.2672 -97.7431 --radius-km 20 --zooms 16 16 --out out --palette 64
```

Copy `out/maps` to the root of the SD card as `/maps`.

## Credits

Map data: OpenStreetMap contributors (ODbL). Vector tiles: Planetiler, OpenMapTiles schema.
Style: Dark Matter by OpenMapTiles / MapTiler, derived from CartoDB Basemaps, designed by Stamen and
Paul Norman for CartoDB Inc. (CC-BY 3.0), code under BSD 3-Clause. Colours are brightened for small
screens in `prepare_style.py`.
