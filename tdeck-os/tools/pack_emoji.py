#!/usr/bin/env python3
"""Bundle the per-emoji image files into one /emoji/pack.bin for fast loading.

Usage:  python3 tools/pack_emoji.py emoji_dir [pack.bin]

Finding one file among 1400+ in a FAT folder costs tens of milliseconds on the T-Deck, so the
firmware prefers a single file it reads once at start-up. Copy pack.bin into /emoji/ on the card
(the individual files can stay; they are used when pack.bin is missing).

Layout (little endian): "EMJ1", u32 count, count x (u32 codepoint, u32 offset, u32 length),
sorted by codepoint, then the image data; offsets are from the start of the data section.
"""
import os, re, struct, sys

src = sys.argv[1]
dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(src, "pack.bin")
items = []
for name in os.listdir(src):
    m = re.fullmatch(r"([0-9a-f]+)\.bin", name)
    if m:
        items.append((int(m.group(1), 16), open(os.path.join(src, name), "rb").read()))
items.sort()
head = b"EMJ1" + struct.pack("<I", len(items))
table, data, off = b"", b"", 0
for cp, blob in items:
    table += struct.pack("<III", cp, off, len(blob))
    data += blob
    off += len(blob)
open(dst, "wb").write(head + table + data)
print(f"{len(items)} emoji, {len(head) + len(table) + len(data)} bytes -> {dst}")
