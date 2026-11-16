#!/usr/bin/env python3
"""콕 찌르기 에셋 → LVGL I4 C 배열 (docs/poke.md). 문구를 바꾸려면 아래 PHRASES 만 고친다.

    python3 gen_poke.py --font /path/to/Galmuri11.ttf
    python3 gen_poke.py --font /path/to/Galmuri11.ttf --preview out.png   # 4× 미리보기만
    (firmware/) touch main/CMakeLists.txt && idf.py build                 # 파일 수가 바뀌었을 때

산출: main/display/usage/art/ui_poke_heart_img.c, ui_poke_phrase_{1..5}_img.c,
      ui_poke_reply_{1..15}_img.c, ui_poke_diary_img.c, ui_manbok_menu_{1..6}_img.c, ui_poke.h

만복이 메뉴 칸 제목(docs/manbok-diary.md)은 Galmuri14 를 15px 로 굽는다(--font 옆의 Galmuri14.ttf,
없으면 --menu-font 로 준다). 바탕은 투명이라 칸 색 위에 그대로 얹는다.

폰트: 픽셀 폰트 Galmuri11 (SIL OFL 1.1, https://github.com/quiple/galmuri). 폰트 파일은 레포에
넣지 않는다 — 구운 .c 만 넣는다. 받는 법:

    npm view galmuri version
    curl -L -o Galmuri11.ttf https://cdn.jsdelivr.net/npm/galmuri@<버전>/dist/Galmuri11.ttf

렌더 크기는 12px 다. Galmuri11 은 unitsPerEm 1200 에 픽셀 하나가 100 이라 12px 에서 격자가
맞고 글자 높이가 11px 로 나온다(이름의 11 은 글자 높이). 11px 로 그리면 획이 뭉개진다.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# 패널 줄 수이자 패킷 phrase 범위(1~5)라 5개로 고정한다.
PHRASES = ["밥 먹자", "고생했어", "사랑해", "보고 싶어", "일찍 와"]
# 받은 문구마다 답장 버튼 3개(docs/poke-reply-plan.md). PHRASES 와 같은 순서로 5줄 × 3칸 고정.
# 패킷 reply 값 = (문구 번호 - 1) × 3 + 칸(1~3) → 1~15. 한 줄 3칸은 2배로 320px 한 줄에 들어가야 한다.
REPLIES = [
    ["좋아!", "10분만", "먼저 먹어"],     # 밥 먹자
    ["고마워", "너도!", "안아줘"],        # 고생했어
    ["나도 사랑해", "알아ㅋ", "♥"],       # 사랑해
    ["나도", "곧 갈게", "전화할래?"],     # 보고 싶어
    ["응!", "노력할게", "늦을듯 ㅠㅠ"],   # 일찍 와
]
REPLIES_PER_PHRASE = 3
REPLY_ROW_MAX_W = 316         # usage_screen.cc: 2배 폭 합 + 칸 사이 8px×2 가 320 안(양옆 2px)
READ_TEXT = "읽음"
HIGH5_TEXT = "하이파이브!"
# 놀러가기(docs/trip.md): 문구 패널 마지막 줄, 캐릭터가 나간 쪽 화면 글자.
TRIP_TEXT = "놀러가기"
# 놀러오기·집으로 가기: 문구 패널 오른쪽 칸(docs/trip.md).
INVITE_TEXT = "놀러오기"
HOME_TEXT = "집으로 가기"
AWAY_TEXT = "놀러 갔어요"
# 만복이 육아일기(docs/manbok-diary.md): 문구 패널 맨 위 한 줄. 분홍으로 다른 줄과 구분한다.
DIARY_TEXT = "육아일기 읽기"
DIARY_RGBA = (0xFF, 0xAA, 0xB9, 255)
# 만복이 메뉴 6칸 제목(2×3, 칸 순서). 시안 클로드봇-메뉴-시안2.png 의 색 그대로.
MENU_TITLES = [
    ("육아일기", (0xF5, 0xA3, 0xB5, 255)),
    ("초음파 사진", (0xF5, 0xA3, 0xB5, 255)),
    ("엄마 건강", (0x9F, 0xE0, 0xA5, 255)),
    ("출산 준비", (0xF3, 0xD2, 0x7A, 255)),
    ("메시지 보내기", (0xF2, 0xE7, 0xC9, 255)),
    ("놀러가기", (0xF2, 0xE7, 0xC9, 255)),
]
MENU_FONT_PX = 15             # Galmuri14 는 unitsPerEm 1500, 픽셀 하나가 100 이라 15px 에서 격자가 맞는다
MENU_MAX_W = 128              # 칸 폭 141 안

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / "main" / "display" / "usage" / "art"


def to_i4(im: Image.Image) -> tuple[bytes, int]:
    """RGBA → (팔레트 16×BGRA + 4bpp 픽셀, stride). 인덱스 0 = 투명. 반투명 색도 팔레트로 간다."""
    colors: list[tuple[int, int, int, int]] = [(0, 0, 0, 0)]
    px = im.load()
    w, h = im.size
    stride = (w + 1) // 2
    body = bytearray(stride * h)
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            key = (r, g, b, a) if a else (0, 0, 0, 0)
            if key not in colors:
                colors.append(key)
                if len(colors) > 16:
                    raise ValueError("색이 16개를 넘는다")
            idx = colors.index(key)
            i = y * stride + x // 2
            body[i] |= idx << 4 if x % 2 == 0 else idx
    palette = bytearray()
    for i in range(16):
        r, g, b, a = colors[i] if i < len(colors) else (0, 0, 0, 0)
        palette += bytes((b, g, r, a))
    return bytes(palette + body), stride

FONT_PX = 12
MAX_PHRASE_W = 140            # 패널 폭이 2w+40 이라 320 안에 들어가려면 140 까지다
PLATE_PAD_X, PLATE_PAD_Y = 4, 2

TEXT_RGBA = (0xF0, 0xE7, 0xDD, 255)
PLATE_RGBA = (0x16, 0x10, 0x08, 255)   # usage_screen.cc kBg — 어느 배경 위에서든 읽히게 판을 굽는다

# 버튼(1배)과 받는 쪽 팝(4배)이 같이 쓴다. '.' 투명, '#' 주색, 'o' 밝은 점, 'd' 외곽선.
HEART = """
................
..dddd....dddd..
.d####d..d####d.
d#oo###dd######d
d#o############d
d##############d
d##############d
.d############d.
..d##########d..
...d########d...
....d######d....
.....d####d.....
......d##d......
.......dd.......
................
................
"""

HEART_COLORS = {
    "#": (255, 92, 110, 255),
    "o": (255, 225, 230, 255),
    "d": (110, 20, 40, 255),
}


def heart_image() -> Image.Image:
    rows = HEART.strip("\n").splitlines()
    assert len(rows) == 16 and all(len(r) == 16 for r in rows), "하트는 16×16"
    im = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    px = im.load()
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            if ch != ".":
                px[x, y] = HEART_COLORS[ch]
    return im


def render_phrases(font_path: Path, texts: list[str] = PHRASES, color=TEXT_RGBA) -> list[Image.Image]:
    """1비트로 그려 안티앨리어싱이 없다. 줄 높이는 모든 문구가 같게 맞춘다."""
    font = ImageFont.truetype(str(font_path), FONT_PX)
    masks = []
    for text in texts:
        mask = Image.new("1", (320, 32), 0)
        draw = ImageDraw.Draw(mask)
        draw.fontmode = "1"
        draw.text((4, 4), text, fill=1, font=font)
        box = mask.getbbox()
        if box is None:
            raise ValueError(f"글자가 안 그려진다: {text}")
        masks.append((mask, box))
    top = min(box[1] for _, box in masks)
    bottom = max(box[3] for _, box in masks)
    images = []
    for text, (mask, box) in zip(texts, masks):
        w = box[2] - box[0]
        if w > MAX_PHRASE_W:
            raise ValueError(f"문구가 너무 길다({w}px, 최대 {MAX_PHRASE_W}px): {text}")
        im = Image.new("RGBA", (w + 2 * PLATE_PAD_X, bottom - top + 2 * PLATE_PAD_Y), PLATE_RGBA)
        px = im.load()
        src = mask.load()
        for y in range(top, bottom):
            for x in range(box[0], box[2]):
                if src[x, y]:
                    px[x - box[0] + PLATE_PAD_X, y - top + PLATE_PAD_Y] = color
        images.append(im)
    return images


def render_titles(font_path: Path) -> list[Image.Image]:
    """메뉴 칸 제목. 1비트로 그리고 바탕은 투명, 모든 제목의 줄 높이를 같게."""
    font = ImageFont.truetype(str(font_path), MENU_FONT_PX)
    masks = []
    for text, _ in MENU_TITLES:
        mask = Image.new("1", (320, 40), 0)
        draw = ImageDraw.Draw(mask)
        draw.fontmode = "1"
        draw.text((4, 4), text, fill=1, font=font)
        box = mask.getbbox()
        if box is None:
            raise ValueError(f"글자가 안 그려진다: {text}")
        if box[2] - box[0] > MENU_MAX_W:
            raise ValueError(f"메뉴 제목이 너무 길다({box[2] - box[0]}px, 최대 {MENU_MAX_W}px): {text}")
        masks.append((mask, box))
    top = min(box[1] for _, box in masks)
    bottom = max(box[3] for _, box in masks)
    images = []
    for (_, color), (mask, box) in zip(MENU_TITLES, masks):
        im = Image.new("RGBA", (box[2] - box[0], bottom - top), (0, 0, 0, 0))
        px = im.load()
        src = mask.load()
        for y in range(top, bottom):
            for x in range(box[0], box[2]):
                if src[x, y]:
                    px[x - box[0], y - top] = color
        images.append(im)
    return images


def emit_c(sym: str, im: Image.Image, note: str) -> Path:
    data, stride = to_i4(im)
    lines = [
        f"// gen_poke.py 가 구운 것 ({note}, {im.width}×{im.height} I4). 손으로 고치지 말 것.",
        '#include "lvgl.h"',
        "",
        f"static const uint8_t {sym}_data[{len(data)}] = {{",
    ]
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
    lines += [
        "};",
        "",
        f"const lv_image_dsc_t {sym} = {{",
        "    .header = {",
        "        .magic = LV_IMAGE_HEADER_MAGIC,",
        "        .cf = LV_COLOR_FORMAT_I4,",
        "        .flags = 0,",
        f"        .w = {im.width},",
        f"        .h = {im.height},",
        f"        .stride = {stride},",
        "    },",
        f"    .data_size = {len(data)},",
        f"    .data = {sym}_data,",
        "};",
        "",
    ]
    out = OUT / f"{sym}.c"
    out.write_text("\n".join(lines), encoding="utf-8")
    return out


def flat_replies() -> list[str]:
    return [t for row in REPLIES for t in row]


def check_reply_rows(images: list[Image.Image]) -> None:
    for p in range(len(REPLIES)):
        row = images[p * REPLIES_PER_PHRASE:(p + 1) * REPLIES_PER_PHRASE]
        w = sum(im.width for im in row) * 2 + 8 * (REPLIES_PER_PHRASE - 1)
        if w > REPLY_ROW_MAX_W:
            raise ValueError(f"「{PHRASES[p]}」 답장 줄이 너무 길다({w}px, 최대 {REPLY_ROW_MAX_W}px): {REPLIES[p]}")


def emit_header() -> None:
    lines = [
        "// 콕 찌르기 에셋. gen_poke.py 가 만든다 — 손으로 고치지 말 것.",
        "#pragma once",
        "",
        '#include "lvgl.h"',
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
        f"#define UI_POKE_PHRASE_COUNT {len(PHRASES)}",
        f"#define UI_POKE_REPLIES_PER_PHRASE {REPLIES_PER_PHRASE}",
        f"#define UI_POKE_REPLY_COUNT {len(REPLIES) * REPLIES_PER_PHRASE}   // 패킷 reply 1~15",
        "",
        "extern const lv_image_dsc_t ui_poke_heart_img;",
        "extern const lv_image_dsc_t ui_poke_btn_heart_img;     // 대기 화면 하트 버튼 34×30 (tools/gen_bottom_buttons.py)",
        "extern const lv_image_dsc_t ui_poke_btn_hanger_img;    // 대기 화면 옷장 버튼 34×30",
    ]
    for i, text in enumerate(PHRASES, 1):
        lines.append(f"extern const lv_image_dsc_t ui_poke_phrase_{i}_img;   // {text}")
    for i, text in enumerate(flat_replies(), 1):
        lines.append(f"extern const lv_image_dsc_t ui_poke_reply_{i}_img;   // {text}")
    lines.append(f"extern const lv_image_dsc_t ui_poke_read_img;      // {READ_TEXT}")
    lines.append(f"extern const lv_image_dsc_t ui_poke_high5_img;     // {HIGH5_TEXT}")
    lines.append(f"extern const lv_image_dsc_t ui_poke_trip_img;      // {TRIP_TEXT}")
    lines.append(f"extern const lv_image_dsc_t ui_poke_away_img;      // {AWAY_TEXT}")
    lines.append(f"extern const lv_image_dsc_t ui_poke_invite_img;    // {INVITE_TEXT}")
    lines.append(f"extern const lv_image_dsc_t ui_poke_home_img;      // {HOME_TEXT}")
    lines.append(f"extern const lv_image_dsc_t ui_poke_diary_img;     // {DIARY_TEXT}")
    lines.append(f"#define UI_MANBOK_MENU_COUNT {len(MENU_TITLES)}")
    for i, (text, _) in enumerate(MENU_TITLES, 1):
        lines.append(f"extern const lv_image_dsc_t ui_manbok_menu_{i}_img;   // {text}")
    lines += ["", "#ifdef __cplusplus", "}", "#endif", ""]
    (OUT / "ui_poke.h").write_text("\n".join(lines), encoding="utf-8")


def preview(path: Path, heart: Image.Image, phrases: list[Image.Image]) -> None:
    z = 4
    width = max(im.width for im in [heart, *phrases]) * z + 20
    height = sum(im.height * z + 10 for im in [heart, *phrases]) + 10
    sheet = Image.new("RGB", (width, height), (24, 28, 52))
    y = 10
    for im in [heart, *phrases]:
        big = im.resize((im.width * z, im.height * z), Image.NEAREST)
        sheet.paste(big, (10, y), big)
        y += big.height + 10
    sheet.save(path)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", required=True, type=Path, help="Galmuri11.ttf 경로")
    ap.add_argument("--preview", metavar="PNG", type=Path)
    ap.add_argument("--menu-font", type=Path, help="Galmuri14.ttf 경로(없으면 --font 옆에서 찾는다)")
    a = ap.parse_args()
    menu_font = a.menu_font or a.font.with_name("Galmuri14.ttf")
    if not menu_font.is_file():
        print(f"메뉴 제목 폰트가 없다: {menu_font} (Galmuri14.ttf, 받는 법은 Galmuri11 과 같다)", file=sys.stderr)
        return 2
    if not a.font.is_file():
        print(
            f"폰트가 없다: {a.font}\n"
            "받는 법: npm view galmuri version 으로 버전을 본 뒤\n"
            "  curl -L -o Galmuri11.ttf https://cdn.jsdelivr.net/npm/galmuri@<버전>/dist/Galmuri11.ttf",
            file=sys.stderr,
        )
        return 2
    if len(PHRASES) != 5:
        print("PHRASES 는 5개여야 한다 (패널 줄 수·패킷 범위)", file=sys.stderr)
        return 2
    if len(REPLIES) != len(PHRASES) or any(len(r) != REPLIES_PER_PHRASE for r in REPLIES):
        print(f"REPLIES 는 문구마다 {REPLIES_PER_PHRASE}개씩 {len(PHRASES)}줄이어야 한다", file=sys.stderr)
        return 2

    heart = heart_image()
    phrases = render_phrases(a.font)
    replies = render_phrases(a.font, flat_replies())
    check_reply_rows(replies)
    if a.preview:
        preview(a.preview, heart, phrases + replies)
        print(f"미리보기 → {a.preview}")
        return 0
    out = emit_c("ui_poke_heart_img", heart, "콕 찌르기 하트")
    print(f"{out.name}: {heart.width}×{heart.height}, {out.stat().st_size} bytes")
    for i, (text, im) in enumerate(zip(PHRASES, phrases), 1):
        out = emit_c(f"ui_poke_phrase_{i}_img", im, f"문구 {text}")
        print(f"{out.name}: {im.width}×{im.height}, {out.stat().st_size} bytes ({text})")
    for i, (text, im) in enumerate(zip(flat_replies(), replies), 1):
        emit_c(f"ui_poke_reply_{i}_img", im, f"답장 {text}")
    read_im, high5_im = render_phrases(a.font, [READ_TEXT, HIGH5_TEXT])
    emit_c("ui_poke_read_img", read_im, READ_TEXT)
    emit_c("ui_poke_high5_img", high5_im, HIGH5_TEXT)
    trip_im, away_im = render_phrases(a.font, [TRIP_TEXT, AWAY_TEXT])
    emit_c("ui_poke_trip_img", trip_im, TRIP_TEXT)
    emit_c("ui_poke_away_img", away_im, AWAY_TEXT)
    invite_im, home_im = render_phrases(a.font, [INVITE_TEXT, HOME_TEXT])
    emit_c("ui_poke_invite_img", invite_im, INVITE_TEXT)
    emit_c("ui_poke_home_img", home_im, HOME_TEXT)
    (diary_im,) = render_phrases(a.font, [DIARY_TEXT], DIARY_RGBA)
    emit_c("ui_poke_diary_img", diary_im, DIARY_TEXT)
    for i, ((text, _), im) in enumerate(zip(MENU_TITLES, render_titles(menu_font)), 1):
        emit_c(f"ui_manbok_menu_{i}_img", im, f"만복이 메뉴 {text}")
    emit_header()
    return 0


if __name__ == "__main__":
    sys.exit(main())
