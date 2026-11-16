#!/usr/bin/env python3
"""보드 화면 덤프(GET /screen, RGB565 LE) → PNG. 픽셀 확인용.

    python3 tools/screen.py out.png            # 보드에서 받아 저장
    python3 tools/screen.py out.png raw.bin    # 받은 파일을 변환만
"""
from __future__ import annotations

import os
import struct
import sys
import urllib.request
from pathlib import Path

from PIL import Image

# 보드 주소는 CLAUDEBOT_URL(=http://<board-ip>/usage) 또는 SCREEN_URL(=.../screen) 로 준다.
URL = os.environ.get("SCREEN_URL") or os.environ.get("CLAUDEBOT_URL", "").replace("/usage", "/screen")


def fetch() -> tuple[bytes, int, int, int]:
    if not URL:
        sys.exit("set CLAUDEBOT_URL=http://<board-ip>/usage (or SCREEN_URL)")
    with urllib.request.urlopen(URL, timeout=15) as r:
        data = r.read()
        w, h, stride = (int(r.headers[k]) for k in ("X-Width", "X-Height", "X-Stride"))
    return data, w, h, stride


def to_png(data: bytes, w: int, h: int, stride: int, out: Path, scale: int = 2) -> None:
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        row = data[y * stride : y * stride + w * 2]
        for x in range(w):
            (v,) = struct.unpack_from("<H", row, x * 2)
            px[x, y] = (((v >> 11) & 0x1F) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3)
    im.resize((w * scale, h * scale), Image.NEAREST).save(out)


if __name__ == "__main__":
    out = Path(sys.argv[1])
    if len(sys.argv) > 2:
        raw = Path(sys.argv[2]).read_bytes()
        to_png(raw, 320, 240, 640, out)
    else:
        data, w, h, stride = fetch()
        to_png(data, w, h, stride, out)
    print("wrote", out)
