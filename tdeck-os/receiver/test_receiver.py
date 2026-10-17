import json, os, tempfile, threading, unittest, urllib.error, urllib.request
import server


class ReceiverTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.srv = server.make_server(0, "127.0.0.1", {"alexkey": "alex", "tonykey": "tony"}, cls.tmp.name)
        cls.port = cls.srv.server_address[1]
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()
        cls.tmp.cleanup()

    def call(self, method, path, body=None, key=None):
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}", method=method,
                                     data=json.dumps(body).encode() if body is not None else None)
        if key:
            req.add_header("X-API-Key", key)
        try:
            with urllib.request.urlopen(req) as r:
                return r.status, json.loads(r.read() or b"null") if "json" in r.headers.get("Content-Type", "") else r.read()
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"null")

    def rows(self, n, t0=1792300000):
        return [{"type": "RX", "lat": 30.2672 + i * 1e-4, "lon": -97.7431, "timestamp": t0 + i, "node": "15D5", "hops": 1, "rssi": -100, "snr": 5.5, "extra": ""} for i in range(n)]

    def test_health_needs_no_key(self):
        self.assertEqual(self.call("GET", "/health")[0], 200)

    def test_post_requires_key(self):
        self.assertEqual(self.call("POST", "/api/coverage", {"data": self.rows(1)})[0], 401)
        self.assertEqual(self.call("POST", "/api/coverage", {"data": self.rows(1)}, "wrong")[0], 401)

    def test_store_and_idempotent(self):
        code, r = self.call("POST", "/api/coverage", {"device": "dev-a", "data": self.rows(5, 1700000000)}, "alexkey")
        self.assertEqual((code, r["stored"], r["duplicates"]), (200, 5, 0))
        code, r = self.call("POST", "/api/coverage", {"device": "dev-a", "data": self.rows(5, 1700000000)}, "alexkey")
        self.assertEqual((code, r["stored"], r["duplicates"]), (200, 0, 5))      # a resent batch changes nothing
        code, pts = self.call("GET", "/api/points?key=tonykey&device=dev-a")
        self.assertEqual(len(pts), 5)

    def test_bad_payloads(self):
        self.assertEqual(self.call("POST", "/api/coverage", {"nope": 1}, "alexkey")[0], 400)
        self.assertEqual(self.call("POST", "/api/coverage", {"data": []}, "alexkey")[0], 400)
        self.assertEqual(self.call("POST", "/api/coverage", {"data": self.rows(201)}, "alexkey")[0], 400)
        bad = [{"type": "RX", "lat": 0, "lon": 0, "timestamp": 1}, {"type": "XX", "lat": 30, "lon": -97, "timestamp": 1},
               {"type": "RX", "lat": 95, "lon": -97, "timestamp": 1}, "str", {"type": "RX", "lat": "x", "lon": 1, "timestamp": 1}]
        code, r = self.call("POST", "/api/coverage", {"device": "dev-b", "data": bad}, "alexkey")
        self.assertEqual((code, r["stored"], r["rejected"]), (200, 0, 5))

    def test_label_used_when_device_missing(self):
        self.call("POST", "/api/coverage", {"data": self.rows(1, 1700001000)}, "alexkey")
        _, summ = self.call("GET", "/api/summary?key=tonykey")
        self.assertIn("alex", [s["device"] for s in summ])

    def test_reading_needs_key_and_map_served(self):
        self.assertEqual(self.call("GET", "/api/points")[0], 401)
        code, html = self.call("GET", "/?key=tonykey")
        self.assertEqual(code, 200)
        self.assertIn(b"leaflet", html)

    def test_dm_stats(self):
        rows = [{"type": "DM", "lat": 30.3, "lon": -97.7, "timestamp": 1700002000, "extra": "ok;600", "hops": 0, "rssi": -80, "snr": 9},
                {"type": "DM", "lat": 30.4, "lon": -97.7, "timestamp": 1700002100, "extra": "fail"}]
        self.call("POST", "/api/coverage", {"device": "dev-c", "data": rows}, "alexkey")
        _, summ = self.call("GET", "/api/summary?key=tonykey")
        c = [s for s in summ if s["device"] == "dev-c"][0]
        self.assertEqual((c["dm"], c["dm_ok"]), (2, 1))


if __name__ == "__main__":
    unittest.main()
