#!/usr/bin/env python3
"""Adapt the Dark Matter style for the local tileserver-gl: local vector source, local glyphs and
sprite, and the one font the renderer image ships (Noto Sans Regular)."""
import colorsys, json, os, re, shutil, sys

# Dark Matter is built for large bright screens; on a 320x240 LCD its dark greys vanish. Every
# colour is brightened by this gain and offset, then water and green areas get a hue so the map
# reads at a glance.
GAIN, OFFSET = 2.5, 10

def _clamp(v):
    return max(0, min(255, int(round(v))))

def boost(color):
    """Brighten one CSS colour string; anything we do not understand is returned unchanged."""
    c = color.strip()
    m = re.match(r"^#([0-9a-fA-F]{3}|[0-9a-fA-F]{6})$", c)
    if m:
        h = m.group(1)
        if len(h) == 3:
            h = "".join(ch * 2 for ch in h)
        r, g, b = int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)
    else:
        m = re.match(r"^rgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*(?:,\s*([\d.]+))?\s*\)$", c)
        if m:
            r, g, b = float(m.group(1)), float(m.group(2)), float(m.group(3))
        else:
            m = re.match(r"^hsla?\(\s*([\d.]+)\s*,\s*([\d.]+)%\s*,\s*([\d.]+)%\s*(?:,\s*([\d.]+))?\s*\)$", c)
            if not m:
                return color
            rr, gg, bb = colorsys.hls_to_rgb(float(m.group(1)) / 360.0, float(m.group(3)) / 100.0, float(m.group(2)) / 100.0)
            r, g, b = rr * 255, gg * 255, bb * 255
    return "rgb(%d,%d,%d)" % (_clamp(r * GAIN + OFFSET), _clamp(g * GAIN + OFFSET), _clamp(b * GAIN + OFFSET))

def boost_all(o):
    if isinstance(o, str):
        return boost(o) if re.match(r"^(#|rgb|hsl)", o.strip()) else o
    if isinstance(o, list):
        return [boost_all(x) for x in o]
    if isinstance(o, dict):
        return {k: boost_all(v) for k, v in o.items()}
    return o

SRC = "style/style-dark-matter.json"
OUT_DIR = "server/styles"
os.makedirs(OUT_DIR, exist_ok=True)

s = json.load(open(SRC))
s["name"] = "tdeck dark"
s["sources"] = {"openmaptiles": {"type": "vector", "url": "mbtiles://{austin}"}}
s["glyphs"] = "{fontstack}/{range}.pbf"
s["sprite"] = "{styleJsonFolder}/sprite"
# overrides after the boost: a map that matches the app (its background is the app's ink colour)
OVERRIDE = {
    "background": {"background-color": "#0A0E14"},
    "water": {"fill-color": "#1b3f66"},
}
GREEN = {"landcover_wood": 1, "landcover_grass": 1, "landuse_park": 1, "park_national_park": 1, "park_nature_reserve": 1}
for layer in s["layers"]:
    layer["paint"] = boost_all(layer.get("paint", {}))
    if layer["id"] in OVERRIDE:
        layer["paint"].update(OVERRIDE[layer["id"]])
    if layer["id"] in GREEN and "fill-color" in layer["paint"]:
        layer["paint"]["fill-color"] = "#173326"
    lay = layer.get("layout", {})
    if "text-font" in lay:
        lay["text-font"] = ["Noto Sans Regular"]
json.dump(s, open(os.path.join(OUT_DIR, "dark.json"), "w"), indent=1)
for f in os.listdir("style/sprite"):
    shutil.copy(os.path.join("style/sprite", f), os.path.join(OUT_DIR, f))

cfg = {
    "options": {"paths": {"root": "/data", "fonts": "fonts", "sprites": "styles", "styles": "styles", "mbtiles": "."},
                "serveAllStyles": False, "formatQuality": {"png": 9}},
    "styles": {"dark": {"style": "dark.json", "tilejson": {"bounds": [-99.417, 28.822, -96.069, 31.713]}}},
    "data": {"austin": {"mbtiles": "austin.mbtiles"}},
}
json.dump(cfg, open("server/config.json", "w"), indent=1)
print("style and config written to server/")
