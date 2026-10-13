#!/usr/bin/env python3
"""Work out how a MeshCore reaction's 8 character code is derived (TDOS-008).

Input: /tdos/idlog.csv from the SD card, which the firmware writes for every channel message:
    rx_utc,sender_ts,hops,packet_hash8,raw_text
Reaction lines look like  "Sender: @[Author]<emoji> <8 chars>".  For each one the script finds
candidate target messages (earlier lines whose sender is Author) and tries many derivations of
the code from the target's fields, printing any that reproduce the code.

  python3 tools/find_reaction_id.py idlog.csv
"""
import csv, hashlib, re, struct, sys, zlib

B36 = "0123456789abcdefghijklmnopqrstuvwxyz"
REACT = re.compile(r"^(?P<sender>.+?): @\[(?P<author>.+?)\](?P<emoji>\S+) (?P<code>[0-9a-z]{8})\s*$")

def from36(s):
    n = 0
    for ch in s:
        n = n * 36 + B36.index(ch)
    return n

def to36(n):
    s = ""
    while n:
        s = B36[n % 36] + s
        n //= 36
    return s

def fnv1a(data, bits):
    prime, off, mask = (0x100000001b3, 0xcbf29ce484222325, (1 << 64) - 1) if bits == 64 else (0x01000193, 0x811c9dc5, (1 << 32) - 1)
    h = off
    for b in data:
        h ^= b
        h = (h * prime) & mask
    return h

def digests(data):
    d = {}
    for n in ("md5", "sha1", "sha256", "sha512", "sha3_256", "blake2b", "blake2s"):
        d[n] = hashlib.new(n, data).digest()
    d["fnv64"] = struct.pack(">Q", fnv1a(data, 64))
    d["fnv32"] = struct.pack(">I", fnv1a(data, 32))
    d["crc32"] = struct.pack(">I", zlib.crc32(data))
    return d

def values(digest):
    n = int.from_bytes(digest, "big")
    yield "int mod 36^8", n % (36 ** 8)
    yield "low 40 bits", n & ((1 << 40) - 1)
    yield "top 40 bits", n >> max(0, 8 * len(digest) - 40)
    s = to36(n)
    if len(s) >= 8:
        yield "base36 first 8", from36(s[:8])
        yield "base36 last 8", from36(s[-8:])
    for i in range(0, len(digest) - 4):
        yield "bytes %d..%d BE" % (i, i + 5), int.from_bytes(digest[i:i + 5], "big")
        yield "bytes %d..%d LE" % (i, i + 5), int.from_bytes(digest[i:i + 5], "little")

def inputs(row):
    ts, text, h8 = row["sender_ts"], row["raw_text"], bytes.fromhex(row["packet_hash8"])
    sender, _, body = text.partition(": ")
    yield "packet hash bytes", h8
    yield "packet hash hex", h8.hex().encode()
    for label, s in (("raw text", text), ("body", body), ("ts+raw", ts + text), ("ts+body", ts + body),
                     ("raw+ts", text + ts), ("body+ts", body + ts), ("ts:raw", ts + ":" + text),
                     ("sender+ts+body", sender + ts + body), ("ts+sender+body", ts + sender + body),
                     ("sender:ts:body", sender + ":" + ts + ":" + body)):
        yield label, s.encode("utf-8")
    yield "ts as 4 bytes LE", struct.pack("<I", int(ts))
    yield "ts as 4 bytes BE", struct.pack(">I", int(ts))
    yield "ts decimal", ts.encode()

def main(path):
    rows = list(csv.DictReader(open(path, encoding="utf-8")))
    print("%d logged channel messages" % len(rows))
    reactions = [(i, REACT.match(r["raw_text"])) for i, r in enumerate(rows)]
    reactions = [(i, m) for i, m in reactions if m]
    print("%d reactions" % len(reactions))
    found = {}
    for i, m in reactions:
        want = from36(m.group("code"))
        author = m.group("author")
        cands = [rows[j] for j in range(i - 1, max(-1, i - 60), -1) if rows[j]["raw_text"].startswith(author + ": ")]
        for cand in cands:
            for ilabel, data in inputs(cand):
                dg = digests(data) if ilabel not in ("packet hash bytes",) else {"raw": data, **digests(data)}
                for dname, d in dg.items():
                    for vname, v in values(d):
                        if v == want:
                            found.setdefault((ilabel, dname, vname), []).append(m.group("code"))
    if not found:
        print("no derivation matched yet; collect more reactions and rerun")
    for k, codes in sorted(found.items(), key=lambda kv: -len(kv[1])):
        print("MATCH x%d  input=%s  digest=%s  value=%s  codes=%s" % (len(codes), k[0], k[1], k[2], ",".join(codes[:4])))

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "idlog.csv")
