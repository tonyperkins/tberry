#!/usr/bin/env python3
"""Convert Noto Emoji PNGs into the small image files tdeck-os reads from the SD card.

Usage:
  git clone --depth 1 --filter=blob:none --sparse https://github.com/googlefonts/noto-emoji.git
  (cd noto-emoji && git sparse-checkout set 2D/png/128)
  python3 tools/make_emoji_pack.py noto-emoji/2D/png/128 emoji --size 18

Then copy the output folder to the SD card as /emoji/ . Each file is named after its code point in
lowercase hex (1f600.bin) and holds an LVGL 8 image: a 4-byte header followed by RGB565 + alpha,
three bytes per pixel. Only single code point emoji are converted; sequences (skin tones, ZWJ
families, keycaps) are not used by the device.
"""
import argparse, os, re, struct, sys
from PIL import Image

LV_IMG_CF_TRUE_COLOR_ALPHA = 5          # enum value in LVGL 8.3 lv_img_buf.h


def convert(src, dst, size):
    im = Image.open(src).convert("RGBA")
    # premultiplied resize avoids dark fringes around soft edges
    pre = im.convert("RGBa").resize((size, size), Image.LANCZOS).convert("RGBA")
    out = bytearray()
    # header bitfield, little endian: cf:5 always_zero:3 reserved:2 w:11 h:11
    header = LV_IMG_CF_TRUE_COLOR_ALPHA | (size << 10) | (size << 21)
    out += struct.pack("<I", header)
    for r, g, b, a in pre.getdata():
        c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        out += struct.pack("<HB", c, a)
    with open(dst, "wb") as f:
        f.write(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src_dir")
    ap.add_argument("out_dir")
    ap.add_argument("--size", type=int, default=18)
    ap.add_argument("--min-codepoint", type=lambda x: int(x, 16), default=0x2190,
                    help="skip code points below this (plain text characters), hex")
    a = ap.parse_args()

    os.makedirs(a.out_dir, exist_ok=True)
    pat = re.compile(r"^emoji_u([0-9a-f]+)\.png$")
    n = skipped = 0
    for name in sorted(os.listdir(a.src_dir)):
        m = pat.match(name)
        if not m:
            skipped += 1                      # sequences contain underscores
            continue
        cp = int(m.group(1), 16)
        if cp < a.min_codepoint:
            skipped += 1
            continue
        convert(os.path.join(a.src_dir, name), os.path.join(a.out_dir, "%x.bin" % cp), a.size)
        n += 1
    with open(os.path.join(a.out_dir, "README.txt"), "w") as f:
        f.write("Emoji images for tdeck-os, converted from Noto Emoji (Google).\n"
                "Most images: Apache License 2.0. Region flags: public domain.\n"
                "https://github.com/googlefonts/noto-emoji\n")
    print("converted %d, skipped %d, size %dx%d" % (n, skipped, a.size, a.size))


if __name__ == "__main__":
    sys.exit(main())
