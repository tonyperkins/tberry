#!/usr/bin/env python3
"""Drive a T-Deck running the `tdeck-remote` build over USB and save screenshots.

Usage:  python3 tools/remote.py [--port /dev/ttyACM0] COMMAND [COMMAND ...]

Each COMMAND is one quoted string:
  "tap 160 120"            touch at x y (screen is 320x240)
  "swipe 5 120 120 120"    drag from x1 y1 to x2 y2 (edge swipes start inside 20 px of an edge)
  "key a" / "key ENTER"    one key; also BS, ESC, TAB, SPACE
  "keys hello"             type text
  "ball 1 0" / "click" / "long"   trackball step, click, long press (Home)
  "wait 500"               pause, milliseconds
  "info"                   prints mode, free LVGL memory, free heap, why the last start happened
  "crash" / "hang"         force a crash or a stuck loop, to test the crash record and watchdog
  "shot name.png"          screenshot to a PNG file

Example:  remote.py "tap 40 100" "wait 500" "shot messages.png"
"""
import struct, sys, time, zlib
import serial


def write_png(path, w, h, rgb565):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        row = rgb565[y * w * 2:(y + 1) * w * 2]
        for i in range(0, len(row), 2):
            v = row[i] | (row[i + 1] << 8)
            r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
            raw += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def readline(s, timeout=10):
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        c = s.read(1)
        if not c:
            continue
        if c == b"\n":
            return buf.decode(errors="replace")
        buf += c
    raise TimeoutError("no answer from the device")


def expect_ok(s, timeout=15):
    # skip any log lines the firmware prints between commands
    end = time.time() + timeout
    while time.time() < end:
        l = readline(s, timeout)
        if l in ("OK",):
            return
        if l.startswith("INFO"):
            print(l)
            return
        if l.startswith("ERR"):
            raise RuntimeError(l)
    raise TimeoutError("no OK")


def main():
    args = sys.argv[1:]
    port = "/dev/ttyACM0"
    if args[:1] == ["--port"]:
        port, args = args[1], args[2:]
    if not args:
        print(__doc__)
        return 1
    # Leave DTR/RTS at the defaults: toggling them makes the ESP32-S3's USB port reset the chip.
    s = serial.Serial(port, 115200, timeout=0.5)
    s.write(b"\n")                    # end any half-sent command line
    time.sleep(0.4)
    s.reset_input_buffer()
    for cmd in args:
        if cmd.startswith("shot "):
            name = cmd[5:].strip()
            s.write(b"shot\n")
            while True:
                l = readline(s, 20)
                if l.startswith("SHOT "):
                    break
            _, w, h, n = l.split()
            w, h, n = int(w), int(h), int(n)
            data = b""
            end = time.time() + 30
            while len(data) < n and time.time() < end:
                data += s.read(n - len(data))
            if len(data) != n:
                raise RuntimeError(f"short screenshot: {len(data)} of {n}")
            write_png(name, w, h, data)
            print(f"saved {name}")
            # the trailer (DONE) follows; the next command's reply tells us it is idle
            while readline(s, 10) != "DONE":
                pass
            continue
        s.write(cmd.encode() + b"\n")
        expect_ok(s)
    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
