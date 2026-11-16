#!/usr/bin/env python3
"""Open-Meteo → 보드 날씨 옷 (docs/wardrobe.md 자동 규칙).

30분마다 launchd 가 부른다(com.claudebot.weather.plist). 보드는 LOCK 이 아닐
때만 규칙표(비→우산, 눈→털모자, 30℃→선글라스, 밤→잠옷 모자, 아침→커피)로
갈아입는다. 좌표는 board.json 의 "lat"/"lon", 없으면 서울.

    python3 push_weather.py            # 조회 + 보드 전송
    python3 push_weather.py --dry-run  # 페이로드만 출력 (보드 없이 확인)

설치(집에서 한 번):
    cp com.claudebot.weather.plist ~/Library/LaunchAgents/ && \
    launchctl load ~/Library/LaunchAgents/com.claudebot.weather.plist
"""

from __future__ import annotations

import json
import os
import sys
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from boardnet import post_json, remember_agent_selected  # noqa: E402

LOG_PATH = Path(os.environ.get("CLAUDEBOT_WEATHER_LOG", "/tmp/claudebot-weather.log"))
DEFAULT_LAT, DEFAULT_LON = 37.5665, 126.9780   # 서울
POST_TIMEOUT_S = 5

# Open-Meteo WMO weather code → kind. 규칙표는 rain/snow 만 옷을 바꾸지만
# clear/cloud 도 보내 둔다(나중에 배경·이펙트 규칙이 쓸 수 있게).
RAIN_CODES = set(range(51, 68)) | set(range(80, 83)) | {95, 96, 99}
SNOW_CODES = set(range(71, 78)) | {85, 86}


def log(msg: str) -> None:
    try:
        with LOG_PATH.open("a", encoding="utf-8") as f:
            f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}\n")
    except OSError:
        pass


def board_config() -> dict:
    try:
        return json.loads((HERE / "board.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def kind_of(code: int) -> str:
    if code in RAIN_CODES:
        return "rain"
    if code in SNOW_CODES:
        return "snow"
    if code <= 1:
        return "clear"
    return "cloud"


def fetch_weather(lat: float, lon: float) -> dict:
    url = (
        "https://api.open-meteo.com/v1/forecast"
        f"?latitude={lat}&longitude={lon}&current=temperature_2m,weather_code,is_day"
    )
    with urllib.request.urlopen(url, timeout=10) as r:
        cur = json.loads(r.read())["current"]
    return {
        "kind": kind_of(int(cur["weather_code"])),
        "temp": round(float(cur["temperature_2m"])),
        "night": int(cur.get("is_day", 1)) == 0,
    }


def main() -> int:
    cfg = board_config()
    lat = float(cfg.get("lat", DEFAULT_LAT))
    lon = float(cfg.get("lon", DEFAULT_LON))
    try:
        payload = {"weather": fetch_weather(lat, lon)}
    except Exception as e:  # noqa: BLE001 — 네트워크 없는 곳에서도 조용히
        log(f"날씨 조회 실패: {e}")
        return 0
    if "--dry-run" in sys.argv:
        print(json.dumps(payload, ensure_ascii=False))
        return 0
    url = os.environ.get("CLAUDEBOT_BOARD_URL") or cfg.get("url", "")
    if not url:
        log("보드 주소 없음")
        return 0
    try:
        remember_agent_selected(post_json(url, payload, POST_TIMEOUT_S, log), log)
        log(f"전송 {payload['weather']}")
    except Exception as e:  # noqa: BLE001 — 보드 꺼짐은 흔한 일
        log(f"전송 실패(보드 꺼짐/외출 가능): {e}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
