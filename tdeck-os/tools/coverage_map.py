#!/usr/bin/env python3
"""Turn the T-Deck's /tdos/coverage.csv (and coverage.old) into a map you can open in a browser.

    python3 tools/coverage_map.py /media/you/CARD/tdos coverage.html [--geojson coverage.geojson]

Rows (utc,lat,lon,type,node,hops,rssi,snr,extra):
  RX    a packet heard, best reading per ~100 m cell and relay   -> dot coloured by signal strength
  DISC  a repeater answered the discovery probe                  -> bigger dot, popup shows repeater and both SNRs
  DM    a direct-message probe to the target                     -> green ring if delivered, red cross if not
  STAT  airtime counters (used for the summary only)
The map needs internet for the OpenStreetMap tiles and Leaflet, nothing else.
"""
import argparse, csv, html, json, os, sys


def load(folder):
    rows = []
    for name in ("coverage.old", "coverage.csv"):
        p = os.path.join(folder, name)
        if os.path.exists(p):
            with open(p, newline="") as f:
                for r in csv.DictReader(f):
                    try:
                        r["lat"], r["lon"] = float(r["lat"]), float(r["lon"])
                        r["rssi"], r["snr"], r["hops"] = int(r["rssi"]), float(r["snr"]), int(r["hops"])
                        r["utc"] = int(r["utc"])
                    except (ValueError, KeyError, TypeError):
                        continue
                    rows.append(r)
    return rows


def colour(rssi):
    # strong to weak: green, yellow, orange, red
    return "#2fbf71" if rssi >= -90 else "#e8c547" if rssi >= -105 else "#f08a3c" if rssi >= -117 else "#e04f4f"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder", help="the tdos folder on the SD card (or a copy of it)")
    ap.add_argument("out", help="HTML file to write")
    ap.add_argument("--geojson", help="also write the points as GeoJSON")
    a = ap.parse_args()
    rows = [r for r in load(a.folder) if r["type"] != "STAT" and (r["lat"] or r["lon"])]
    if not rows:
        sys.exit("no coverage rows found")
    feats = []
    for r in rows:
        feats.append({"type": "Feature", "geometry": {"type": "Point", "coordinates": [r["lon"], r["lat"]]},
                      "properties": {k: r[k] for k in ("utc", "type", "node", "hops", "rssi", "snr", "extra")}})
    if a.geojson:
        with open(a.geojson, "w") as f:
            json.dump({"type": "FeatureCollection", "features": feats}, f)
    pts = []
    for r in rows:
        t = r["type"]
        ok = t == "DM" and r["extra"].startswith("ok")
        tip = f"{t} {r['node']} rssi {r['rssi']} snr {r['snr']} hops {r['hops']} {r['extra']}"
        pts.append([r["lat"], r["lon"], t, ok, colour(r["rssi"]) if t != "DM" else ("#2fbf71" if ok else "#e04f4f"), html.escape(tip)])
    n = {t: sum(1 for r in rows if r["type"] == t) for t in ("RX", "DISC", "DM")}
    dm_ok = sum(1 for r in rows if r["type"] == "DM" and r["extra"].startswith("ok"))
    page = f"""<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Coverage</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<style>html,body,#m{{height:100%;margin:0}}#s{{position:absolute;z-index:999;right:8px;top:8px;background:#fffd;padding:6px 10px;font:13px sans-serif;border-radius:6px}}</style>
</head><body><div id="m"></div>
<div id="s">{n['RX']} heard &middot; {n['DISC']} discovery &middot; DM probes {dm_ok}/{n['DM']} delivered<br>
<span style="color:#2fbf71">&#9679;</span> strong <span style="color:#e8c547">&#9679;</span> ok <span style="color:#f08a3c">&#9679;</span> weak <span style="color:#e04f4f">&#9679;</span> very weak / failed</div>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script>
var d={json.dumps(pts)};
var m=L.map('m');L.tileLayer('https://tile.openstreetmap.org/{{z}}/{{x}}/{{y}}.png',{{maxZoom:19,attribution:'&copy; OpenStreetMap contributors'}}).addTo(m);
var b=[];
d.forEach(function(p){{var ll=[p[0],p[1]];b.push(ll);var mk;
 if(p[2]=='DM')mk=L.circleMarker(ll,{{radius:9,color:p[4],weight:3,fillOpacity:p[3]?0.1:0.5,fillColor:p[4]}});
 else if(p[2]=='DISC')mk=L.circleMarker(ll,{{radius:7,color:'#234',weight:1.5,fillColor:p[4],fillOpacity:0.9}});
 else mk=L.circleMarker(ll,{{radius:5,weight:0,fillColor:p[4],fillOpacity:0.75}});
 mk.bindTooltip(p[5]).addTo(m);}});
m.fitBounds(b,{{padding:[30,30]}});
</script></body></html>"""
    with open(a.out, "w") as f:
        f.write(page)
    print(f"wrote {a.out}: {len(rows)} points ({n})")


if __name__ == "__main__":
    main()
