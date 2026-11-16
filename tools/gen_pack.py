#!/usr/bin/env python3
"""캐릭터 팩 테이블 생성기 (docs/character-pack.md, docs/workshop.md §3).

    python3 tools/gen_pack.py packs/<name> [packs/<name2> ...] <out.cc>

`packs/<name>/pack.json` 과 각 세트의 `sets/<set>/manifest.json`(pixelpal render.py --board
가 쓴 것)을 읽어 펌웨어가 쓰는 테이블(`kCharacterPacks[]`·`kCharacterPackCount`,
character_pack.h)을 C++ 로 굽는다. 팩 순서가 옷장 CHAR 탭 순서이고 첫 팩이 첫 부팅 기본값이다.
CMake 가 configure 때 부른다 — 손으로 돌릴 일은 검증뿐이다.

pack.json:
    {
      "name": "bolt",
      "sets": ["bolt", "bolt_plain"],      # sets/<set>/ 폴더 이름 = pixelpal 캐릭터 name
      "default": "bolt",                   # 없으면 첫 세트
      "map": {"thinking": "busy", ...},    # 보드 상태 8종 → pixelpal 상태. 없는 키는 기본표
      "ride": "ride",                      # (선택) 대기 화면 미니카 행동 상태. 왼쪽용은 "<ride>_l" 이 manifest 에 있으면 자동,
                                           #   다른 이름이면 "ride_left": "…". 없으면 idle 프레임으로 좌우 미끄러지는 옛 방식
      "touch": {"tap": "heart", "double": "dizzy"},  # 대기 화면 몸통 탭·더블탭 반응
      "set_map":   {"manbok_cap": {"done": "happy"}}, # 세트별 map 덮어쓰기 (포즈 컷이 없는 착장 세트용)
      "set_touch": {"manbok_cap": {"tap": "heart"}}, # 세트별 touch 덮어쓰기
      "scale": {"idle": 1.5, "state": 2},            # 화면 배율(숫자면 둘 다). 프레임은 그대로 98×64, 펌웨어가 nearest 확대
      "items": "items"                     # (선택) 옷장 아이템 오버레이 폴더: items.json + pp_<name>_item_<key>_img.c
                                           #   (pixelpal export_items.py). 없으면 옷장은 빈 채로 열린다
      "seats": "seats",                    # (선택) 자리 소품 폴더: seats.json + pp_<name>_seat_<sky>_img.c (pixelpal mb_seats.py).
                                           #   배경마다 몸 뒤 그림 + raise 만큼 몸 올림. 없으면 바닥에 앉는다
      "scenes_when_shared": true,          # (선택) 장면 팩과 같이 구우면 그 장면을 같이 쓴다(옛 items·seats·ride 는 뺀다)
      "scene_prefix": "mb_",               # (선택) 장면 몸·모자 assets 이름 앞머리
      "scenes": true                       # (선택) 장면 전용 팩. 배경·옷장이 장면 9개·전용 옷 탭으로 돈다
    }

세트 manifest 의 states.<st>.offsets([[dx,dy],...])·items(bool) 는 아이템 오버레이가 프레임을 따라가는 데 쓴다
(pixelpal animate_overlay.py 가 적는다). 없으면 오프셋 0·아이템 숨김으로 굽는다(옛 manifest 호환).

표준 라이브러리만 쓴다 — IDF 가상환경 파이썬에는 PIL 이 없다.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

# UsageState 순서 (usage_screen.h). idle 포함 8종 — 바꾸면 character_pack.h 도 같이.
BOARD_STATES = ["idle", "thinking", "working", "juggling", "permission", "error", "done", "sleeping"]
DEFAULT_MAP = {
    "idle": "idle", "thinking": "busy", "working": "busy", "juggling": "dizzy",
    "permission": "heart", "error": "error", "done": "happy", "sleeping": "sleep",
}
DEFAULT_TOUCH = {"tap": "heart", "double": "dizzy"}
# 터치·콕 반응 상태. manifest 에 있으면 쓰고 없으면 펌웨어가 tap·double 로 대체한다.
# 순서는 character_pack.h PackReaction 과 같다.
REACTIONS = ["pet", "sneeze", "pout", "dizzy", "highfive"]
SYMBOL_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def fail(msg: str) -> None:
    print(f"gen_pack: {msg}", file=sys.stderr)
    sys.exit(1)


def load_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        fail(f"{path} 가 없다")
    except json.JSONDecodeError as e:
        fail(f"{path}: JSON 오류 {e}")
    return {}


def anim_ref(sym_prefix: str, st: str, spec: dict) -> tuple[str, int, int]:
    """(프레임 배열 심볼, 프레임 수, 프레임 ms)"""
    frames = int(spec.get("frames", 0))
    fps = float(spec.get("fps", 4))
    if frames <= 0 or fps <= 0:
        fail(f"{sym_prefix}: 상태 {st} 의 frames/fps 가 이상하다 ({spec})")
    return f"kF_{sym_prefix}_{st}", frames, max(1, round(1000 / fps))


def offsets_of(sym_prefix: str, st: str, spec: dict) -> tuple[list[int], bool]:
    """manifest states.<st>.offsets → 평탄한 [dx0,dy0,dx1,dy1,...]. 없으면 [](오프셋 0). items 는 없으면 False."""
    frames = int(spec.get("frames", 0))
    offs = spec.get("offsets")
    if offs is None:
        return [], bool(spec.get("items", False))
    if not isinstance(offs, list) or len(offs) != frames or any(not isinstance(o, list) or len(o) != 2 for o in offs):
        fail(f"{sym_prefix}: 상태 {st} 의 offsets 가 frames({frames})와 안 맞는다: {offs}")
    flat: list[int] = []
    for dx, dy in offs:
        if not (-128 <= int(dx) <= 127 and -128 <= int(dy) <= 127):
            fail(f"{sym_prefix}: 상태 {st} 오프셋 {dx},{dy} 가 int8 범위를 넘는다")
        flat += [int(dx), int(dy)]
    return flat, bool(spec.get("items", False))


SLOT_ORDER = ["head", "face", "hand", "neck"]   # ClawdSlot 순서 (neck = fx 자리, item_tables.h)


# UsageSky 순서 (usage_screen.h) — 자리 소품 표는 이 순서로 kUsageSkyCount 개를 채운다.
SKY_ORDER = ["brown", "navy", "day", "sunset", "room", "camp", "space", "cafe", "alley", "sea", "sakura", "library", "site"]


def seat_tables(pack_dir: Path, pack: dict, name: str) -> tuple[list[str], list[str], str]:
    """pack.json "seats" 폴더의 seats.json → PackSeat[kUsageSkyCount] (sky 키로 자리 채움). (externs, 테이블 줄, 초기화 식)"""
    folder = pack.get("seats")
    if folder is None:
        return [], [], "nullptr"
    seats_dir = pack_dir / str(folder)
    meta = load_json(seats_dir / "seats.json")
    seats = meta.get("seats")
    if not isinstance(seats, list):
        fail(f"{seats_dir}/seats.json 에 seats 배열이 없다 — pixelpal mb_seats.py 로 뽑았는지 확인")
    by_sky: dict[str, str] = {}
    externs: list[str] = []
    for st in seats:
        sky = str(st.get("sky", ""))
        if sky not in SKY_ORDER:
            fail(f"seats.json: 모르는 sky '{sky}' — {SKY_ORDER}")
        if sky in by_sky:
            fail(f"seats.json: sky '{sky}' 가 두 번 있다")
        img = str(st.get("img", "")).removesuffix(".c")
        if not img or not (seats_dir / f"{img}.c").exists():
            fail(f"{seats_dir}/{img}.c 가 없다 ({sky})")
        externs.append(f"extern const lv_image_dsc_t {img};")
        x, y, raise_ = (int(st.get(k, 0)) for k in ("x", "y", "raise"))
        if not (-128 <= x <= 127 and -128 <= y <= 127 and 0 <= raise_ <= 255):
            fail(f"seats.json: {sky} 의 x/y/raise 가 범위를 넘는다 ({x},{y},{raise_})")
        by_sky[sky] = f"    {{&{img}, {x}, {y}, {raise_}}},   // {sky}"
    arr = f"kSeats_{name}"
    lines = [f"const PackSeat {arr}[] = {{"]
    for sky in SKY_ORDER:
        lines.append(by_sky.get(sky, f"    {{nullptr, 0, 0, 0}},   // {sky} (자리 없음)"))
    lines.append("};")
    return externs, lines, arr


def item_tables(pack_dir: Path, pack: dict, name: str) -> tuple[list[str], list[str], str]:
    """pack.json "items" 폴더의 items.json → 슬롯별 ClawdItem 테이블. (externs, 테이블 정의 줄, 초기화 식)"""
    folder = pack.get("items")
    if folder is None:
        return [], [], "{nullptr, nullptr, nullptr, nullptr}, {0, 0, 0, 0}, nullptr"
    items_dir = pack_dir / str(folder)
    meta = load_json(items_dir / "items.json")
    items = meta.get("items")
    if not isinstance(items, list):
        fail(f"{items_dir}/items.json 에 items 배열이 없다 — pixelpal export_items.py 로 뽑았는지 확인")
    none_thumb = str(meta.get("none_thumb", "")).removesuffix(".c")
    if not none_thumb or not (items_dir / f"{none_thumb}.c").exists():
        fail(f"{items_dir}: none_thumb '{none_thumb}.c' 가 없다")
    externs = [f"extern const lv_image_dsc_t {none_thumb};"]
    by_slot: dict[str, list[str]] = {s: [] for s in SLOT_ORDER}
    seen: set[str] = set()
    for it in items:
        key, slot = str(it.get("key", "")), str(it.get("slot", ""))
        if slot not in by_slot:
            fail(f"items.json: {key} 의 슬롯 '{slot}' 은 모른다 — {SLOT_ORDER}")
        if not SYMBOL_RE.match(key) or key == "none":
            fail(f"items.json: 아이템 키 '{key}' 는 C 식별자여야 하고 none 은 예약이다")
        if key in seen:
            fail(f"items.json: 키 '{key}' 가 두 번 있다")
        seen.add(key)
        img = str(it.get("img", "")).removesuffix(".c")
        thumb = str(it.get("thumb", "")).removesuffix(".c")
        for sym in (img, thumb):
            if not sym or not (items_dir / f"{sym}.c").exists():
                fail(f"{items_dir}/{sym}.c 가 없다 ({key})")
            externs.append(f"extern const lv_image_dsc_t {sym};")
        x, y, w, h = (int(it.get(k, 0)) for k in ("x", "y", "w", "h"))
        if not (0 <= x <= 127 and 0 <= y <= 127 and 1 <= w <= 255 and 1 <= h <= 255):
            fail(f"items.json: {key} 의 x/y/w/h 가 범위를 넘는다 ({x},{y},{w},{h})")
        # ClawdItem{key, dx, dy, w, h, img, img_l, thumb}: 팩 아이템은 프레임 좌표 1px 셀, 반전본 없음(nullptr)
        by_slot[slot].append(f'    {{"{key}", {x}, {y}, {w}, {h}, &{img}, nullptr, &{thumb}}},')
    lines: list[str] = []
    ptrs: list[str] = []
    counts: list[str] = []
    for slot in SLOT_ORDER:
        rows = by_slot[slot]
        arr = f"kItems_{name}_{slot}"
        lines.append(f"const ClawdItem {arr}[] = {{")
        lines.append(f'    {{"none", 0, 0, 0, 0, nullptr, nullptr, &{none_thumb}}},')
        lines += rows
        lines.append("};")
        ptrs.append(arr)
        counts.append(str(len(rows) + 1))
    init = f"{{{', '.join(ptrs)}}}, {{{', '.join(counts)}}}, &{none_thumb}"
    return externs, lines, init


MAX_NAME = 8   # NVS 키 outfit_<이름> 이 15자 제한에 걸리지 않는 길이


def build_pack(pack_dir: Path, shared_scenes: bool = False) -> dict:
    """팩 폴더 하나 → 테이블 조각. 내부 배열 이름에 팩·세트 이름이 붙어 있어 팩끼리 충돌하지 않는다"""
    pack = load_json(pack_dir / "pack.json")
    # 장면 팩과 같이 구우면 그 장면을 같이 쓴다(pack.json "scenes_when_shared"). 이 빌드에서는 옛
    # 옷장·자리 소품·미니카가 없고 몸은 assets 의 <scene_prefix>char_<장면>.argb 다.
    if shared_scenes and pack.get("scenes_when_shared"):
        pack = {k: v for k, v in pack.items() if k not in ("items", "seats", "ride", "ride_left")}
        pack["scenes"] = True

    name = str(pack.get("name") or pack_dir.name)
    if not SYMBOL_RE.match(name):
        fail(f"name '{name}' 은 C 식별자여야 한다 (영문·숫자·_)")
    if len(name) > MAX_NAME:
        fail(f"name '{name}' 은 {MAX_NAME}자 이하여야 한다 (NVS 키 outfit_<이름> 15자 제한)")
    sets = pack.get("sets")
    if not isinstance(sets, list) or not sets:
        fail("pack.json 의 sets 는 비어 있지 않은 배열이어야 한다")
    if len(set(sets)) != len(sets):
        fail("sets 에 같은 이름이 두 번 있다")
    default = pack.get("default", sets[0])
    if default not in sets:
        fail(f"default '{default}' 가 sets 에 없다")
    state_map = {**DEFAULT_MAP, **pack.get("map", {})}
    unknown = [k for k in state_map if k not in BOARD_STATES]
    if unknown:
        fail(f"map 에 모르는 보드 상태 {unknown} — 가능한 것: {BOARD_STATES}")
    touch = {**DEFAULT_TOUCH, **pack.get("touch", {})}
    raw_scale = pack.get("scale", 1.0)
    if isinstance(raw_scale, dict):
        scales = {k: float(raw_scale.get(k, 1.0)) for k in ("idle", "state")}
        bad_keys = [k for k in raw_scale if k not in ("idle", "state")]
        if bad_keys:
            fail(f"scale 의 모르는 키 {bad_keys} — idle·state 만")
    else:
        scales = {"idle": float(raw_scale), "state": float(raw_scale)}
    for k, v in scales.items():
        if not (0.5 <= v <= 3.0):
            fail(f"scale.{k} {v} 는 0.5~3 사이여야 한다")
    scale256 = int(round(scales["idle"] * 256))
    state_scale256 = int(round(scales["state"] * 256))

    externs: list[str] = []
    arrays: list[str] = []
    set_rows: list[str] = []
    for set_key in sets:
        if not SYMBOL_RE.match(set_key):
            fail(f"세트 '{set_key}' 는 C 식별자여야 한다")
        set_dir = pack_dir / "sets" / set_key
        manifest = load_json(set_dir / "manifest.json")
        sym_prefix = str(manifest.get("name") or set_key)
        states = manifest.get("states") or {}
        if not isinstance(states, dict) or not states:
            fail(f"{set_dir}/manifest.json 에 states 가 없다 — render.py --board 로 구웠는지 확인")
        # 세트별 덮어쓰기: 포즈 컷(banzai·lie…)이 기본 세트에만 있을 때 착장 세트는 레이어식 상태로 대신한다
        set_map_over = (pack.get("set_map") or {}).get(set_key, {})
        bad = [k for k in set_map_over if k not in BOARD_STATES]
        if bad:
            fail(f"set_map['{set_key}'] 에 모르는 보드 상태 {bad}")
        state_map = {**DEFAULT_MAP, **pack.get("map", {}), **set_map_over}
        touch = {**DEFAULT_TOUCH, **pack.get("touch", {}), **(pack.get("set_touch") or {}).get(set_key, {})}
        # 미니카 행동(pack.json "ride"): 모든 세트에 있어야 한다. 왼쪽용은 "ride_left" 또는 "<ride>_l"(있을 때만).
        ride = pack.get("ride")
        ride_left = pack.get("ride_left")
        if ride is not None:
            if not isinstance(ride, str) or ride not in states:
                fail(f"세트 '{set_key}' 에 ride 상태 {ride!r} 프레임이 없다 (있는 것: {sorted(states)})")
            if ride_left is None and f"{ride}_l" in states:
                ride_left = f"{ride}_l"
            if ride_left is not None and ride_left not in states:
                fail(f"세트 '{set_key}' 에 ride_left 상태 {ride_left!r} 프레임이 없다 (있는 것: {sorted(states)})")
        elif ride_left is not None:
            fail("ride_left 는 ride 와 같이 써야 한다")
        needed = set(state_map.values()) | set(touch.values()) | {s for s in (ride, ride_left) if s}
        reactions = [st if st in states else None for st in REACTIONS]
        needed |= {st for st in reactions if st}
        missing = sorted(needed - set(states))
        if missing:
            fail(f"세트 '{set_key}' 에 상태 {missing} 프레임이 없다 (있는 것: {sorted(states)})")

        # 프레임 배열: 파일 존재까지 확인해 링크 에러를 configure 단계로 당긴다.
        for st in sorted(needed):
            arr, count, _ = anim_ref(sym_prefix, st, states[st])
            syms = []
            for i in range(count):
                sym = f"pp_{sym_prefix}_{st}_{i}_img"
                if not (set_dir / "board" / f"{sym}.c").exists():
                    fail(f"{set_dir}/board/{sym}.c 가 없다")
                externs.append(f"extern const lv_image_dsc_t {sym};")
                syms.append(f"&{sym}")
            arrays.append(f"static const lv_image_dsc_t* const {arr}[{count}] = {{{', '.join(syms)}}};")
            flat, _ = offsets_of(sym_prefix, st, states[st])
            if flat:
                arrays.append(f"static const int8_t kO_{sym_prefix}_{st}[{len(flat)}] = {{{', '.join(map(str, flat))}}};")

        def anim(st: str) -> str:
            arr, count, ms = anim_ref(sym_prefix, st, states[st])
            flat, items = offsets_of(sym_prefix, st, states[st])
            offs = f"kO_{sym_prefix}_{st}" if flat else "nullptr"
            return f"{{{arr}, {count}, {ms}, {offs}, {'true' if items else 'false'}}}"

        state_rows = ", ".join(anim(state_map[bs]) for bs in BOARD_STATES)
        ride_row = anim(ride) if ride else "{nullptr, 0, 0}"
        ride_left_row = anim(ride_left) if ride_left else "{nullptr, 0, 0}"
        reaction_rows = ", ".join(anim(st) if st else "{nullptr, 0, 0}" for st in reactions)
        set_rows.append(
            f'    {{"{set_key}", {{{state_rows}}}, {ride_row}, {ride_left_row}, {anim(touch["tap"])}, {anim(touch["double"])}, {{{reaction_rows}}}}},')

    item_externs, item_lines, item_init = item_tables(pack_dir, pack, name)
    seat_externs, seat_lines, seat_init = seat_tables(pack_dir, pack, name)
    default_map = {**DEFAULT_MAP, **pack.get("map", {})}
    has_scenes = "true" if pack.get("scenes") else "false"
    scene_prefix = str(pack.get("scene_prefix", ""))
    lines = [
        *arrays,
        "",
        "// 옷장 아이템 (pack.json items → items.json). 슬롯 순서 head·face·hand·neck, [0] 은 none.",
        *item_lines,
        "",
        "// 자리 소품 (pack.json seats → seats.json). UsageSky 순서.",
        *seat_lines,
        "",
        "// UsageState 순서(팩 기본 map, 세트별 set_map 은 행마다 반영됨): "
        + ", ".join(f"{bs}→{default_map[bs]}" for bs in BOARD_STATES),
        f"const PackSet kSets_{name}[] = {{",
        *set_rows,
        "};",
        "",
    ]
    n_items = sum(1 for ln in item_lines if ln.startswith('    {"')) - (len(SLOT_ORDER) if item_lines else 0)
    n_seats = sum(1 for ln in seat_lines if ln.startswith("    {&"))
    return {
        "name": name,
        "sets": sets,
        "default": default,
        "externs": set(externs) | set(item_externs) | set(seat_externs),
        "lines": lines,
        "row": f'    {{"{name}", kSets_{name}, {len(sets)}, {sets.index(default)}, {scale256}, {state_scale256}, '
               f'{item_init}, {seat_init}, {has_scenes}, "{scene_prefix}"}},',
        "summary": f"{name} sets={sets} default={default} items={n_items} seats={n_seats}",
    }


def main() -> None:
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    pack_dirs = [Path(p).resolve() for p in sys.argv[1:-1]]
    out = Path(sys.argv[-1]).resolve()
    any_scenes = any(load_json(d / "pack.json").get("scenes") for d in pack_dirs)
    packs = [build_pack(d, shared_scenes=any_scenes) for d in pack_dirs]

    names = [p["name"] for p in packs]
    if len(set(names)) != len(names):
        fail(f"같은 팩 이름이 두 번 있다: {names}")
    all_sets = [s for p in packs for s in p["sets"]]
    if len(set(all_sets)) != len(all_sets):
        fail(f"세트 이름은 팩끼리도 겹치면 안 된다(C 심볼): {all_sets}")

    lines = [
        f"// tools/gen_pack.py 가 {', '.join(d.name for d in pack_dirs)} 의 pack.json 에서 구운 것. 손으로 고치지 말 것.",
        '#include "character_pack.h"',
        "",
        "extern \"C\" {",
        *sorted(set().union(*(p["externs"] for p in packs))),
        "}",
        "",
        "namespace {",
        *[ln for p in packs for ln in p["lines"]],
        "}  // namespace",
        "",
        "const CharacterPack kCharacterPacks[] = {",
        *[p["row"] for p in packs],
        "};",
        f"const uint8_t kCharacterPackCount = {len(packs)};",
        "",
    ]
    out.parent.mkdir(parents=True, exist_ok=True)
    text = "\n".join(lines)
    if not out.exists() or out.read_text(encoding="utf-8") != text:
        out.write_text(text, encoding="utf-8")   # 같으면 안 건드려 재컴파일을 피한다
    print(f"gen_pack: {'; '.join(p['summary'] for p in packs)} -> {out}")


if __name__ == "__main__":
    main()
