# Coverage receiver

A small service the T-Deck uploads its coverage log to when it is home on WiFi, plus a map of
everything it has received. Standard library Python, one SQLite file, no other dependencies.

## Run it

    COV_API_KEYS='long-random-key:alex' python3 server.py        # port 8765, data in ./data

or with Docker: copy `docker-compose.example.yml` to `docker-compose.yml`, change the keys, `docker compose up -d`.

Make keys with `python3 -c "import secrets;print(secrets.token_urlsafe(32))"`. Give every device its own key
(`key:label`), and the label is how its rows are named when the device does not say.

Put it behind TLS. The release firmware refuses `http://`, and positions travel in these requests.
Open the map at `https://your-host/?key=YOUR-KEY`. Anyone with a key can read every device's data, so
treat keys like passwords and put the map behind your own access control if it faces the internet.

## Point a device at it

Either on the device: **Settings → Advanced → Receiver** (the address, `https://your-host/api/coverage`) and
**Upload key**, or put a file `upload.conf` in the `tdos` folder on its SD card, address on line 1 and key on
line 2. The device reads it at start-up, saves the values, and deletes the file so the key does not stay on the card.

The device uploads on its own when it is parked (not moving for 90 seconds) and its saved home WiFi network
is in range, then every 10 minutes while there is something new. **Upload now** in Advanced tries at once, and
**Upload status** shows how the last attempt went. Nothing is sent while it is driving or at 15% battery or less.

## What the device sends

    POST /api/coverage            header  X-API-Key: <key>
    {"device": "Alex", "fw": "0.3.2", "data": [
       {"type": "RX", "lat": 30.2672, "lon": -97.7431, "timestamp": 1792260240,
        "node": "15D5", "hops": 1, "rssi": -114, "snr": -9.75, "extra": ""}, ... up to 50 rows ]}

Types: `RX` heard packet, `DISC` repeater answered a discovery probe (`extra` = `up=<snr the repeater heard us at>`),
`DM` direct-message probe (`extra` = `ok;<ms>` or `fail`), `STAT` airtime counters. Rows are stored once:
sending a batch again is harmless, which is what lets the device retry after a dropped connection.

The shape follows MeshMapper's third-party API (`{"data": [...]}`, 1 to 50 pings, `X-API-Key`), with extra
fields for relay, hops and probe results.

## Other endpoints

`GET /api/points?key=..[&device=..][&since=unix][&limit=n]`, `GET /api/summary?key=..`, `GET /health`.

## Test

    python3 -m unittest -v test_receiver
