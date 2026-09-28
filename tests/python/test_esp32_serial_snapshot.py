# SPDX-License-Identifier: GPL-3.0-or-later
"""scripts/esp32_serial_snapshot.py: parsing a serial dump and the RGB565 -> PNG step."""
import base64
import importlib.util
import struct
import zlib
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "esp32_serial_snapshot.py"
spec = importlib.util.spec_from_file_location("snap", SCRIPT)
snap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(snap)


def dump_lines(pixels: bytes, w: int, h: int, noise: bool = True) -> list[str]:
    comp = zlib.compressobj(6, zlib.DEFLATED, -15)
    data = comp.compress(pixels) + comp.flush()
    b64 = base64.b64encode(data).decode()
    lines = ["I (100) app_boot: boot noise", f"=====HELIX-SNAP {w} {h} RGB565 DEFLATE"]
    for i in range(0, len(b64), 76):
        lines.append("SNAP:" + b64[i:i + 76])
        if noise:
            lines.append("W (200) lvgl_glue: a log line from another task")
    lines.append(f"=====HELIX-SNAP-END {len(data)}")
    return lines


def test_a_dump_with_interleaved_logs_round_trips():
    w, h = 3, 2
    pixels = struct.pack("<6H", 0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000, 0x8410)
    assert snap.parse_dump(dump_lines(pixels, w, h)) == (w, h, pixels)


def test_rgb565_expands_to_full_range_rgb():
    png = snap.rgb565_to_png(struct.pack("<2H", 0xF800, 0xFFFF), 2, 1)
    idat = png[png.index(b"IDAT") + 4:]
    rows = zlib.decompress(idat[:struct.unpack(">I", png[png.index(b"IDAT") - 4:png.index(b"IDAT")])[0]])
    assert rows == bytes([0, 255, 0, 0, 255, 255, 255])


def test_a_truncated_dump_is_refused():
    lines = dump_lines(bytes(2 * 64 * 64), 64, 64, noise=False)
    del lines[3]  # drop one payload line
    with pytest.raises(ValueError):
        snap.parse_dump(lines)


def test_a_firmware_error_is_reported():
    with pytest.raises(ValueError, match="no memory"):
        snap.parse_dump(["=====HELIX-SNAP-ERROR no memory for the snapshot"])
