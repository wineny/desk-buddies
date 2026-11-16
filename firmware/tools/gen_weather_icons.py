#!/usr/bin/env python3
"""날씨 그림 6장 → LVGL ARGB8888 C 배열. 윗줄 시계 칸용 14×14, 날씨 판(시간별·이번 주)용 20×20.

    python3 firmware/tools/gen_weather_icons.py                # C 배열 굽기
    python3 firmware/tools/gen_weather_icons.py --preview a.png  # 실제 크기 + 4배 미리보기도

그림은 이 스크립트가 도형으로 직접 그린다(160px 에 그려 줄임). 글꼴·남의 그림을 쓰지 않는다.
결과: firmware/main/display/usage/art/weather_icons.c
순서는 usage_screen.h 의 UsageWeatherIcon 과 같아야 한다.
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "firmware/main/display/usage/art/weather_icons.c"
RENDER_PX = 160
# (크기, C 이름 꼬리). 12px 은 눈 결정이 뭉개져 시계 칸을 14px 로 올렸다.
SIZES = [(14, ""), (20, "_20")]

SUN = (255, 196, 40, 255)
SUN_RAY = (255, 160, 30, 255)
CLOUD = (236, 240, 246, 255)
CLOUD_DARK = (180, 190, 205, 255)
RAIN = (80, 160, 255, 255)
SNOW = (200, 235, 255, 255)
MOON = (255, 224, 120, 255)


def _canvas():
    im = Image.new("RGBA", (RENDER_PX, RENDER_PX), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)


def _sun(d, cx, cy, r, rays=True):
    if rays:
        for k in range(8):
            a = k * math.pi / 4
            x0, y0 = cx + math.cos(a) * r * 1.35, cy + math.sin(a) * r * 1.35
            x1, y1 = cx + math.cos(a) * r * 1.85, cy + math.sin(a) * r * 1.85
            d.line((x0, y0, x1, y1), fill=SUN_RAY, width=max(4, int(r * 0.32)))
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=SUN)


def _cloud(d, cx, cy, w, fill):
    h = w * 0.42
    d.rounded_rectangle((cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), radius=h / 2, fill=fill)
    r1, r2 = w * 0.24, w * 0.18
    d.ellipse((cx - w * 0.12 - r1, cy - h / 2 - r1 * 0.9, cx - w * 0.12 + r1, cy - h / 2 + r1 * 1.1), fill=fill)
    d.ellipse((cx + w * 0.18 - r2, cy - h / 2 - r2 * 0.5, cx + w * 0.18 + r2, cy - h / 2 + r2 * 1.5), fill=fill)


def draw(name: str) -> Image.Image:
    im, d = _canvas()
    c = RENDER_PX / 2
    if name == "clear":
        _sun(d, c, c, 34)
    elif name == "partly":
        _sun(d, c - 18, c - 20, 28)
        _cloud(d, c + 10, c + 26, 104, CLOUD)
    elif name == "cloud":
        _cloud(d, c + 14, c - 10, 80, CLOUD_DARK)
        _cloud(d, c - 6, c + 18, 120, CLOUD)
    elif name == "rain":
        _cloud(d, c, c - 22, 124, CLOUD_DARK)
        for i, x in enumerate((-36, 0, 36)):
            y = c + 22 + (i % 2) * 10
            d.line((c + x + 6, y, c + x - 6, y + 30), fill=RAIN, width=12)
    elif name == "snow":
        for k in range(3):
            a = k * math.pi / 3
            dx, dy = math.cos(a) * 62, math.sin(a) * 62
            d.line((c - dx, c - dy, c + dx, c + dy), fill=SNOW, width=14)
        d.ellipse((c - 14, c - 14, c + 14, c + 14), fill=SNOW)
    elif name == "moon":
        d.ellipse((c - 52, c - 52, c + 52, c + 52), fill=MOON)
        d.ellipse((c - 22, c - 70, c + 70, c + 22), fill=(0, 0, 0, 0))
    else:
        raise ValueError(name)
    return im.crop(im.split()[3].point(lambda v: 255 if v > 8 else 0).getbbox())


# 순서 = UsageWeatherIcon.
ICONS = ["clear", "partly", "cloud", "rain", "snow", "moon"]


def fit(im: Image.Image, px: int) -> Image.Image:
    """긴 쪽을 px 로 줄여 px×px 가운데에 둔다. 알파를 곱해 줄여 가장자리가 검게 번지지 않게 한다."""
    w, h = im.size
    size = (max(1, round(w * px / max(w, h))), max(1, round(h * px / max(w, h))))
    a = np.asarray(im).astype(np.float32) / 255
    a[..., :3] *= a[..., 3:4]
    pre = [Image.fromarray(a[..., i]).resize(size, Image.Resampling.LANCZOS) for i in range(4)]
    b = np.stack([np.asarray(p) for p in pre], -1).clip(0, 1)
    al = b[..., 3:4]
    rgb = np.where(al > 1e-3, b[..., :3] / np.maximum(al, 1e-3), 0).clip(0, 1)
    small = Image.fromarray((np.dstack([rgb, al]) * 255).astype(np.uint8), "RGBA")
    out = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    out.paste(small, ((px - size[0]) // 2, (px - size[1]) // 2))
    return out


def c_block(symbol: str, im: Image.Image) -> str:
    a = np.asarray(im.convert("RGBA"))
    blob = a[..., [2, 1, 0, 3]].tobytes()   # LVGL ARGB8888 = 메모리 순서 B G R A
    rows = [", ".join(f"0x{v:02x}" for v in blob[i:i + 16]) for i in range(0, len(blob), 16)]
    return (
        f"static const uint8_t {symbol}_data[{len(blob)}] = {{\n    " + ",\n    ".join(rows) + "\n};\n\n"
        f"const lv_image_dsc_t {symbol} = {{\n"
        "    .header = {\n"
        "        .magic = LV_IMAGE_HEADER_MAGIC,\n"
        "        .cf = LV_COLOR_FORMAT_ARGB8888,\n"
        "        .flags = 0,\n"
        f"        .w = {im.width},\n"
        f"        .h = {im.height},\n"
        f"        .stride = {im.width * 4},\n"
        "    },\n"
        f"    .data_size = {len(blob)},\n"
        f"    .data = {symbol}_data,\n"
        "};\n"
    )


def preview(sets: list[list[Image.Image]], path: Path) -> None:
    """어두운 판(보드 칸 색) 위에 크기마다 실제 크기 한 줄, 4배(최근접) 한 줄."""
    bg = (0x16, 0x10, 0x08, 255)
    gap = 8
    cell = max(im.width for icons in sets for im in icons) * 4
    h = gap + sum(icons[0].height + gap + icons[0].height * 4 + gap for icons in sets)
    sheet = Image.new("RGBA", (gap + len(sets[0]) * (cell + gap), h), bg)
    y = gap
    for icons in sets:
        px = icons[0].width
        for i, im in enumerate(icons):
            x = gap + i * (cell + gap)
            sheet.alpha_composite(im, (x + (cell - px) // 2, y))
            sheet.alpha_composite(im.resize((px * 4, px * 4), Image.Resampling.NEAREST),
                                  (x + (cell - px * 4) // 2, y + px + gap))
        y += px + gap + px * 4 + gap
    sheet.convert("RGB").save(path)
    print("preview", path)


def main() -> int:
    raw = [(name, draw(name)) for name in ICONS]
    sizes = "·".join(f"{px}×{px}" for px, _ in SIZES)
    parts = [f"// gen_weather_icons.py 가 만든 날씨 그림({sizes} ARGB8888). 손으로 고치지 말 것.\n"
             '#include "lvgl.h"\n']
    sets = []
    for px, tail in SIZES:
        # 작게 줄이면 해 빛살·눈 결정이 뭉개진다. 언샤프로 가장자리를 조금 세운다.
        icons = [fit(im, px).filter(ImageFilter.UnsharpMask(1, 120, 0)) for _, im in raw]
        for (name, _), im in zip(raw, icons):
            parts.append(c_block(f"weather_icon_{name}{tail}_img", im))
        sets.append(icons)
    OUT.write_text("\n".join(parts), encoding="utf-8")
    print("wrote", OUT.relative_to(ROOT))
    if "--preview" in sys.argv:
        preview(sets, Path(sys.argv[sys.argv.index("--preview") + 1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
