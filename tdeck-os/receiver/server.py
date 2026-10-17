#!/usr/bin/env python3
"""Coverage receiver: accepts batches from the T-Deck, stores them in SQLite, shows them on a map.

Standard library only. Configuration through the environment:

  COV_API_KEYS   comma separated list of  key  or  key:label  (the label names the key's owner). Required.
  COV_DATA_DIR   where coverage.db lives (default ./data)
  COV_PORT       listen port (default 8765)
  COV_BIND       listen address (default 0.0.0.0)

TLS is not handled here: put it behind a reverse proxy or tunnel. The device refuses plain http
in release builds, and anyone who can see the traffic can read positions.

API
  POST /api/coverage     X-API-Key header, JSON {"device": "...", "data": [ {lat, lon, timestamp, type, ...}, ... ]}
                         1 to 200 rows. Idempotent: a row already stored is counted as a duplicate, so the
                         device can safely resend a batch whose answer it did not see.
  GET  /api/points       ?key=KEY[&device=NAME][&since=UNIX][&limit=N]   rows as JSON
  GET  /api/summary      ?key=KEY                                        per device counts
  GET  /                 ?key=KEY                                        the map
  GET  /health           no key; for monitoring
"""
import hmac, json, os, re, sqlite3, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

TYPES = {"RX", "DISC", "DM", "STAT", "TX", "TRACE"}
MAX_BODY = 256 * 1024
MAX_ROWS = 200


def parse_keys(spec):
    keys = {}
    for part in (spec or "").split(","):
        part = part.strip()
        if not part:
            continue
        key, _, label = part.partition(":")
        keys[key] = label or "unnamed"
    return keys


class Store:
    def __init__(self, path):
        self.lock = threading.Lock()
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.execute("""CREATE TABLE IF NOT EXISTS points (
            id INTEGER PRIMARY KEY, device TEXT NOT NULL, utc INTEGER NOT NULL, lat REAL NOT NULL, lon REAL NOT NULL,
            type TEXT NOT NULL, node TEXT NOT NULL DEFAULT '', hops INTEGER, rssi INTEGER, snr REAL, extra TEXT NOT NULL DEFAULT '',
            received INTEGER NOT NULL, UNIQUE (device, utc, lat, lon, type, node))""")
        self.db.execute("CREATE INDEX IF NOT EXISTS points_dev_utc ON points (device, utc)")
        self.db.commit()

    def add(self, device, rows):
        stored = dup = 0
        now = int(time.time())
        with self.lock:
            for r in rows:
                cur = self.db.execute(
                    "INSERT OR IGNORE INTO points (device, utc, lat, lon, type, node, hops, rssi, snr, extra, received) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                    (device, r["utc"], r["lat"], r["lon"], r["type"], r["node"], r["hops"], r["rssi"], r["snr"], r["extra"], now))
                if cur.rowcount:
                    stored += 1
                else:
                    dup += 1
            self.db.commit()
        return stored, dup

    def points(self, device=None, since=0, limit=20000):
        q = "SELECT device, utc, lat, lon, type, node, hops, rssi, snr, extra FROM points WHERE utc >= ?"
        a = [since]
        if device:
            q += " AND device = ?"
            a.append(device)
        q += " ORDER BY utc DESC LIMIT ?"
        a.append(limit)
        with self.lock:
            cur = self.db.execute(q, a)
            cols = [c[0] for c in cur.description]
            return [dict(zip(cols, row)) for row in cur.fetchall()]

    def summary(self):
        with self.lock:
            cur = self.db.execute("""SELECT device, COUNT(*), MIN(utc), MAX(utc),
                SUM(type='RX'), SUM(type='DISC'), SUM(type='DM'), SUM(type='DM' AND extra LIKE 'ok%'), MAX(received)
                FROM points GROUP BY device ORDER BY device""")
            names = ["device", "rows", "first", "last", "rx", "disc", "dm", "dm_ok", "last_received"]
            return [dict(zip(names, row)) for row in cur.fetchall()]


def clean_row(o):
    """Validate one row from the device. Returns a dict or None."""
    if not isinstance(o, dict):
        return None
    try:
        lat, lon = float(o["lat"]), float(o["lon"])
        utc = int(o.get("timestamp", o.get("utc", 0)))
    except (KeyError, TypeError, ValueError):
        return None
    if not (-90 <= lat <= 90 and -180 <= lon <= 180) or (lat == 0 and lon == 0):
        return None
    t = str(o.get("type", "")).upper()
    if t not in TYPES:
        return None

    def num(k, cast, default=None):
        try:
            return cast(o[k]) if o.get(k) is not None else default
        except (TypeError, ValueError):
            return default
    return {"utc": utc, "lat": round(lat, 6), "lon": round(lon, 6), "type": t, "node": str(o.get("node", ""))[:16],
            "hops": num("hops", int), "rssi": num("rssi", int), "snr": num("snr", float), "extra": str(o.get("extra", ""))[:64]}


class Handler(BaseHTTPRequestHandler):
    server_version = "cov-receiver/1"
    store = None
    keys = {}

    def log_message(self, fmt, *args):
        line = re.sub(r"(key=)[^&\s]*", r"\1***", fmt % args)      # never log the key a map link carries
        sys.stderr.write("%s %s\n" % (self.log_date_time_string(), line))

    def _send(self, code, body, ctype="application/json"):
        if not isinstance(body, (bytes, bytearray)):
            body = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _label(self, key):
        """The label for a key, or None. Compares every configured key so timing does not leak which prefix matched."""
        found = None
        for k, label in self.keys.items():
            if hmac.compare_digest(k.encode(), (key or "").encode()):
                found = label
        return found

    def do_POST(self):
        path = urlparse(self.path).path
        if path != "/api/coverage":
            return self._send(404, {"ok": False, "error": "not found"})
        if self._label(self.headers.get("X-API-Key")) is None:
            return self._send(401, {"ok": False, "error": "bad or missing X-API-Key"})
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            n = 0
        if n <= 0 or n > MAX_BODY:
            return self._send(413 if n > MAX_BODY else 400, {"ok": False, "error": "body size"})
        try:
            doc = json.loads(self.rfile.read(n))
            rows = doc["data"]
            assert isinstance(rows, list) and 1 <= len(rows) <= MAX_ROWS
        except Exception:
            return self._send(400, {"ok": False, "error": "expected {\"data\": [1..%d rows]}" % MAX_ROWS})
        device = str(doc.get("device") or self._label(self.headers.get("X-API-Key")))[:40]
        good = [r for r in (clean_row(o) for o in rows) if r]
        stored, dup = self.store.add(device, good)
        self.log_message("%s", f"device={device} rows={len(rows)} valid={len(good)} stored={stored} dup={dup}")
        self._send(200, {"ok": True, "stored": stored, "duplicates": dup, "rejected": len(rows) - len(good)})

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        if u.path == "/health":
            return self._send(200, {"ok": True})
        if self._label((q.get("key") or [""])[0]) is None:
            return self._send(401, {"ok": False, "error": "key required"})
        if u.path == "/api/points":
            dev = (q.get("device") or [None])[0]
            since = int((q.get("since") or ["0"])[0] or 0)
            limit = min(int((q.get("limit") or ["20000"])[0] or 20000), 100000)
            return self._send(200, self.store.points(dev, since, limit))
        if u.path == "/api/summary":
            return self._send(200, self.store.summary())
        if u.path == "/":
            return self._send(200, MAP_HTML.encode(), "text/html; charset=utf-8")
        self._send(404, {"ok": False, "error": "not found"})


MAP_HTML = """<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Coverage</title><link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<style>html,body,#m{height:100%;margin:0}#p{position:absolute;z-index:999;right:8px;top:8px;background:#fffe;padding:8px 10px;font:13px sans-serif;border-radius:6px;max-width:260px}
#p select,#p label{font-size:13px}</style></head><body><div id="m"></div>
<div id="p"><b>Coverage</b> <span id="n"></span><br>
Device <select id="dev"></select><br>Period <select id="per"><option value="86400">24 hours</option><option value="604800" selected>7 days</option><option value="2592000">30 days</option><option value="0">all</option></select><br>
<label><input type="checkbox" id="rx" checked> heard</label> <label><input type="checkbox" id="disc" checked> repeaters</label> <label><input type="checkbox" id="dm" checked> DM probes</label><br>
<span style="color:#2fbf71">&#9679;</span> strong <span style="color:#e8c547">&#9679;</span> ok <span style="color:#f08a3c">&#9679;</span> weak <span style="color:#e04f4f">&#9679;</span> very weak / failed<div id="sum"></div></div>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script>
var key=new URLSearchParams(location.search).get('key')||'';
var m=L.map('m').setView([30.27,-97.74],10);
L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/Canvas/World_Light_Gray_Base/MapServer/tile/{z}/{y}/{x}',{maxZoom:16,attribution:'Tiles &copy; Esri'}).addTo(m);
var layer=L.layerGroup().addTo(m),all=[],first=true;
function col(r){return r>=-90?'#2fbf71':r>=-105?'#e8c547':r>=-117?'#f08a3c':'#e04f4f'}
function draw(){layer.clearLayers();var b=[],n=0;
 all.forEach(function(p){
  if(p.type=='RX'&&!rx.checked||p.type=='DISC'&&!disc.checked||p.type=='DM'&&!dm.checked||p.type=='STAT')return;
  var ll=[p.lat,p.lon],ok=p.type=='DM'&&(p.extra||'').indexOf('ok')==0,c=p.type=='DM'?(ok?'#2fbf71':'#e04f4f'):col(p.rssi),mk;
  if(p.type=='DM')mk=L.circleMarker(ll,{radius:9,color:c,weight:3,fillColor:c,fillOpacity:ok?0.1:0.5});
  else if(p.type=='DISC')mk=L.circleMarker(ll,{radius:7,color:'#234',weight:1.5,fillColor:c,fillOpacity:0.9});
  else mk=L.circleMarker(ll,{radius:5,weight:0,fillColor:c,fillOpacity:0.75});
  mk.bindTooltip(new Date(p.utc*1000).toLocaleString()+' '+p.device+' '+p.type+' '+(p.node||'')+' rssi '+p.rssi+' snr '+p.snr+' hops '+p.hops+' '+(p.extra||'')).addTo(layer);b.push(ll);n++;});
 document.getElementById('n').textContent=n+' points';
 if(first&&b.length){m.fitBounds(b,{padding:[30,30]});first=false}}
function load(){var since=+per.value?Math.floor(Date.now()/1000)-per.value:0;
 fetch('/api/points?key='+encodeURIComponent(key)+'&since='+since+(dev.value?'&device='+encodeURIComponent(dev.value):'')).then(r=>r.json()).then(function(d){all=d;draw()})}
fetch('/api/summary?key='+encodeURIComponent(key)).then(r=>r.json()).then(function(s){
 dev.innerHTML='<option value="">all</option>'+s.map(x=>'<option>'+x.device+'</option>').join('');
 sum.innerHTML=s.map(x=>x.device+': '+x.rows+' rows, DM '+x.dm_ok+'/'+x.dm).join('<br>');load()});
[per,dev].forEach(e=>e.onchange=load);[rx,disc,dm].forEach(e=>e.onchange=draw);
</script></body></html>"""


def make_server(port, bind, keys, data_dir):
    os.makedirs(data_dir, exist_ok=True)
    Handler.store = Store(os.path.join(data_dir, "coverage.db"))
    Handler.keys = keys
    return ThreadingHTTPServer((bind, port), Handler)


def main():
    keys = parse_keys(os.environ.get("COV_API_KEYS"))
    if not keys:
        sys.exit("set COV_API_KEYS (for example COV_API_KEYS='long-random-key:alex')")
    srv = make_server(int(os.environ.get("COV_PORT", "8765")), os.environ.get("COV_BIND", "0.0.0.0"), keys,
                      os.environ.get("COV_DATA_DIR", "data"))
    print("coverage receiver on port", srv.server_address[1], "with", len(keys), "key(s)", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
