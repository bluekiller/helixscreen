#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Screenshot an ESP32 panel over its serial console.

Sends "snap"; the firmware (firmware/helixscreen-esp32/main/serial_snapshot.c)
answers with the active screen as raw-deflated RGB565, base64 on "SNAP:" lines
between HELIX-SNAP markers. Log lines interleave between them and are ignored.

    esp32_serial_snapshot.py /dev/ttyUSB0 out.png [--timeout 120] [--settle 45]
        [--tap X,Y ...] [--tap-wait 1.5]

--tap sends "tap X Y" (panel coordinates) before the screenshot, in order,
pausing --tap-wait seconds after each so the UI settles. --notes prints every
notification since boot (the toasts the bell counts) instead of a screenshot.

Needs pyserial. A board wired for auto-reset resets while RTS and DTR differ;
after opening, RTS is released before DTR so they never do. The request is
repeated until the dump starts, so a reset that happens anyway costs one boot,
not the capture, and --settle waits for a screen that has finished booting.
"""

import argparse
import base64
import struct
import sys
import time
import zlib


def rgb565_to_png(raw: bytes, width: int, height: int) -> bytes:
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # filter: none
        line = raw[y * width * 2:(y + 1) * width * 2]
        for (px,) in struct.iter_unpack("<H", line):
            rows += bytes(((px >> 11) << 3 | px >> 13, (px >> 5 & 0x3F) << 2 | (px >> 9 & 3),
                           (px & 0x1F) << 3 | (px >> 2 & 7)))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b""))


def parse_dump(lines: list[str]) -> tuple[int, int, bytes]:
    """(width, height, rgb565) from the lines of one dump; raises ValueError."""
    width = height = None
    payload = []
    for line in lines:
        if line.startswith("=====HELIX-SNAP-ERROR"):
            raise ValueError(line)
        if line.startswith("=====HELIX-SNAP "):
            _, w, h, fmt, enc = line.split()
            if (fmt, enc) != ("RGB565", "DEFLATE"):
                raise ValueError(f"unsupported dump: {fmt} {enc}")
            width, height, payload = int(w), int(h), []
        elif line.startswith("SNAP:") and width is not None:
            payload.append(line[5:].strip())
        elif line.startswith("=====HELIX-SNAP-END") and width is not None:
            compressed = base64.b64decode("".join(payload))
            if len(compressed) != int(line.split()[1]):
                raise ValueError(f"truncated: {len(compressed)} of {line.split()[1]} bytes")
            raw = zlib.decompress(compressed, -15)
            if len(raw) != width * height * 2:
                raise ValueError(f"expected {width * height * 2} pixel bytes, got {len(raw)}")
            return width, height, raw
    raise ValueError("no complete dump")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port")
    ap.add_argument("out", nargs="?", help="PNG to write (not needed with --notes)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--settle", type=float, default=0.0,
                    help="seconds to wait after opening the port before asking")
    ap.add_argument("--tap", action="append", default=[], metavar="X,Y",
                    help="tap at X,Y before the screenshot; repeatable")
    ap.add_argument("--tap-wait", type=float, default=1.5)
    ap.add_argument("--notes", action="store_true", help="print the notification history")
    args = ap.parse_args()
    if not args.notes and not args.out:
        ap.error("out is required unless --notes is given")

    import serial

    port = serial.Serial()
    port.port, port.baudrate, port.timeout = args.port, args.baud, 0.5
    # The CH340 board resets while RTS and DTR differ. Opening raises both (no
    # reset); dropping RTS first keeps them from ever disagreeing. Presetting
    # them before open() would not: pyserial applies DTR before RTS there.
    port.open()
    port.rts = False
    port.dtr = False
    port.reset_input_buffer()

    time.sleep(args.settle)
    for tap in args.tap:
        x, y = (int(v) for v in tap.split(","))
        port.write(f"\ntap {x} {y}\n".encode())
        time.sleep(args.tap_wait)
    next_ask = time.time()
    lines, buf, deadline = [], b"", next_ask + args.timeout
    if args.notes:
        port.write(b"\nnotes\n")
        while time.time() < deadline:
            buf += port.read(4096)
            *done, buf = buf.split(b"\n")
            lines += [d.decode("utf-8", "replace").rstrip("\r") for d in done]
            if any(l.startswith("=====HELIX-NOTES-END") for l in lines):
                break
        notes = [l[len("NOTE: "):] for l in lines if l.startswith("NOTE: ")]
        print("\n".join(notes) if notes else "(no notifications)")
        return 0
    while time.time() < deadline:
        started = any(l.startswith("=====HELIX-SNAP") for l in lines)
        if not started and time.time() >= next_ask:
            port.write(b"\nsnap\n")
            next_ask = time.time() + 2.0
        buf += port.read(4096)
        *done, buf = buf.split(b"\n")
        lines += [d.decode("ascii", "replace").rstrip("\r") for d in done]
        if any(l.startswith(("=====HELIX-SNAP-END", "=====HELIX-SNAP-ERROR")) for l in lines):
            break
    width, height, raw = parse_dump(lines)
    with open(args.out, "wb") as f:
        f.write(rgb565_to_png(raw, width, height))
    print(f"{args.out}: {width}x{height}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
