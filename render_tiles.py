#!/usr/bin/env python3
"""Fetch raster tiles for a bounding box from the local tileserver-gl and write them in the layout
the device reads from the SD card:  <out>/maps/<z>/<x>/<y>.png

  python3 render_tiles.py --bbox -99.417 28.822 -96.069 31.713 --zooms 6 14 --out out
  python3 render_tiles.py --centre 30.2672 -97.7431 --radius-km 40 --zooms 15 16 --out out

Tiles that already exist are skipped, so a run can be repeated or extended. With --palette N each
tile is reduced to N colours, which makes dark maps several times smaller and quicker to decode.
"""
import argparse, io, math, os, sys, time, urllib.request
from concurrent.futures import ThreadPoolExecutor

def tile_xy(lat, lon, z):
    n = 2 ** z
    x = int((lon + 180.0) / 360.0 * n)
    y = int((1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n)
    return min(max(x, 0), n - 1), min(max(y, 0), n - 1)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bbox", nargs=4, type=float, metavar=("W", "S", "E", "N"))
    ap.add_argument("--centre", nargs=2, type=float, metavar=("LAT", "LON"))
    ap.add_argument("--radius-km", type=float, default=0)
    ap.add_argument("--zooms", nargs=2, type=int, required=True, metavar=("MIN", "MAX"))
    ap.add_argument("--out", default="out")
    ap.add_argument("--server", default="http://localhost:8080")
    ap.add_argument("--style", default="dark")
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--palette", type=int, default=0, help="reduce tiles to this many colours (0 = keep)")
    a = ap.parse_args()

    if a.bbox:
        w, s, e, n = a.bbox
    elif a.centre and a.radius_km:
        lat, lon = a.centre
        dlat = a.radius_km / 111.32
        dlon = a.radius_km / (111.32 * math.cos(math.radians(lat)))
        w, s, e, n = lon - dlon, lat - dlat, lon + dlon, lat + dlat
    else:
        ap.error("give --bbox, or --centre with --radius-km")

    quant = None
    if a.palette:
        from PIL import Image
        quant = Image

    jobs = []
    for z in range(a.zooms[0], a.zooms[1] + 1):
        x0, y1 = tile_xy(s, w, z)
        x1, y0 = tile_xy(n, e, z)
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                jobs.append((z, x, y))
    print("%d tiles, zoom %d-%d" % (len(jobs), a.zooms[0], a.zooms[1]), flush=True)

    done = skipped = failed = 0
    t0 = time.time()

    def fetch(job):
        z, x, y = job
        path = os.path.join(a.out, "maps", str(z), str(x), "%d.png" % y)
        if os.path.exists(path):
            return "skip"
        url = "%s/styles/%s/%d/%d/%d.png" % (a.server, a.style, z, x, y)
        for attempt in range(3):
            try:
                data = urllib.request.urlopen(url, timeout=60).read()
                break
            except Exception:
                time.sleep(1 + attempt)
        else:
            return "fail"
        if quant:
            im = quant.open(io.BytesIO(data)).convert("RGB").quantize(colors=a.palette, method=quant.Quantize.MEDIANCUT, dither=quant.Dither.NONE)
            buf = io.BytesIO()
            im.save(buf, format="PNG", optimize=True)
            data = buf.getvalue()
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path + ".tmp", "wb") as f:
            f.write(data)
        os.replace(path + ".tmp", path)
        return "ok"

    with ThreadPoolExecutor(a.threads) as ex:
        for i, r in enumerate(ex.map(fetch, jobs), 1):
            if r == "ok": done += 1
            elif r == "skip": skipped += 1
            else: failed += 1
            if i % 500 == 0 or i == len(jobs):
                rate = i / max(time.time() - t0, 0.1)
                print("%d/%d  %.0f tiles/s  ok=%d skipped=%d failed=%d" % (i, len(jobs), rate, done, skipped, failed), flush=True)
    return 1 if failed else 0

if __name__ == "__main__":
    sys.exit(main())
