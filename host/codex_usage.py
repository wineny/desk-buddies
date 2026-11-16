#!/usr/bin/env python3
"""Codex CLI 세션 로그 → 사용량·한도. push_usage.py 의 Codex 백엔드.

Codex 는 Claude Code 와 달리 한도를 얻으려고 statusline 을 tee 로 감쌀 필요가
없다. 세션 로그(~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl)에 응답마다
`token_usage_record` 가, 턴이 끝날 때 `event_msg/token_count` 가 찍히고 그 안에
`rate_limits` 가 그대로 들어 있다 (codex-cli 0.153.2, 2026-09-06 실측):

  {"type":"token_usage_record","payload":{"usage":{"input_tokens":…,
    "cached_input_tokens":…,"output_tokens":…,"reasoning_output_tokens":…,
    "total_tokens":…}, "turn_token_usage":{…}, "thread_token_usage":{…}}}
  {"type":"event_msg","payload":{"type":"token_count","info":{…},
    "rate_limits":{"primary":  {"used_percent":7.0,"window_minutes":300,  "resets_at":…},
                   "secondary":{"used_percent":11.0,"window_minutes":10080,"resets_at":…},
                   "plan_type":"plus"}}}

`usage` 는 응답 한 번 분량이고 `turn_/thread_token_usage` 는 누적이다 — 합산은
`usage` 로만 한다(같은 턴의 두 번째 응답에서 usage 25,780 ≠ turn 49,418 로 확인).
primary 는 300분 = 5시간 창, secondary 는 10080분 = 7일 창.

비용($)은 없다. Codex 는 구독(plan_type)이라 청구 단위가 토큰이 아니다.

파일명의 시각은 UTC 다. 로컬 날짜로 오늘/이번 주를 자르려면 하루 앞 디렉터리까지
읽고 줄의 timestamp 를 로컬로 바꿔 판정한다.
"""

from __future__ import annotations

import json
import os
import re
import sys
import time
from datetime import date, datetime, timedelta, timezone
from pathlib import Path

SESSIONS_DIR = Path(os.environ.get("CODEX_SESSIONS_DIR", Path.home() / ".codex" / "sessions"))

# 한도 창 분 → Claude statusline 의 키. push_usage.limit_cells 가 그 키로 읽는다.
WINDOW_KEYS = {300: "five_hour", 10080: "seven_day"}


def _local_date(ts_iso: str) -> date | None:
    """'2026-09-06T01:47:36.897Z' → 로컬 날짜. 못 읽으면 None."""
    try:
        return datetime.fromisoformat(ts_iso.replace("Z", "+00:00")).astimezone().date()
    except (ValueError, AttributeError):
        return None


def _day_dirs(first: date, last: date) -> list[Path]:
    """로컬 [first, last] 를 덮는 UTC 날짜 디렉터리들. 있는 것만."""
    out: list[Path] = []
    d = first - timedelta(days=1)  # UTC 는 로컬(KST)보다 뒤라 하루 앞 디렉터리에 오늘 새벽분이 있다
    while d <= last:
        p = SESSIONS_DIR / f"{d.year:04d}" / f"{d.month:02d}" / f"{d.day:02d}"
        if p.is_dir():
            out.append(p)
        d += timedelta(days=1)
    return out


def _session_files(dirs: list[Path]) -> list[Path]:
    files: list[Path] = []
    for d in dirs:
        files.extend(d.glob("rollout-*.jsonl"))
    return files


def collect() -> dict:
    """오늘/이번 주 토큰 합 + 가장 최근 rate_limits.

    파일을 통째로 json 파싱하지 않는다 — 세션 하나가 수 MB(시스템 프롬프트가
    매 세션 박힌다)라 문자열 필터로 줄을 고른 뒤에만 json.loads 한다.
    """
    today = date.today()
    monday = today - timedelta(days=today.weekday())
    files = _session_files(_day_dirs(monday, today))

    today_total = today_out = 0
    week_total = week_out = 0
    latest_limits: dict | None = None
    latest_limits_ts = ""
    files_read = 0

    for f in files:
        try:
            with f.open("r", encoding="utf-8", errors="replace") as fh:
                files_read += 1
                for line in fh:
                    if "token_usage_record" in line:
                        try:
                            o = json.loads(line)
                        except json.JSONDecodeError:
                            continue
                        if o.get("type") != "token_usage_record":
                            continue
                        d = _local_date(str(o.get("timestamp", "")))
                        if d is None or d < monday or d > today:
                            continue
                        u = (o.get("payload") or {}).get("usage") or {}
                        tot = int(u.get("total_tokens", 0) or 0)
                        out = int(u.get("output_tokens", 0) or 0)
                        week_total += tot
                        week_out += out
                        if d == today:
                            today_total += tot
                            today_out += out
                    elif '"rate_limits"' in line:
                        try:
                            o = json.loads(line)
                        except json.JSONDecodeError:
                            continue
                        rl = (o.get("payload") or {}).get("rate_limits")
                        ts = str(o.get("timestamp", ""))
                        if isinstance(rl, dict) and ts > latest_limits_ts:
                            latest_limits, latest_limits_ts = rl, ts
        except OSError:
            continue

    return {
        "agent": "codex",
        "today_total_tokens": today_total,
        "today_output_tokens": today_out,
        "week_total_tokens": week_total,
        "week_output_tokens": week_out,
        "rate_limits": latest_limits or {},
        "rate_limits_at": latest_limits_ts,
        "plan_type": (latest_limits or {}).get("plan_type"),
        "files_read": files_read,
    }


def statusline_like(s: dict) -> dict:
    """collect() 결과 → Claude statusline 페이로드 모양.

    push_usage.limit_cells() 가 `rate_limits.five_hour.used_percentage` 를 읽으므로
    Codex 의 primary/secondary 를 그 이름으로 옮긴다. window_minutes 로 짝을 맞추고,
    모르는 창 길이면 primary→5h, secondary→week 순서로 떨어뜨린다.
    """
    rl = s.get("rate_limits") or {}
    out: dict = {}
    for slot, fallback in (("primary", "five_hour"), ("secondary", "seven_day")):
        w = rl.get(slot)
        if not isinstance(w, dict):
            continue
        key = WINDOW_KEYS.get(int(w.get("window_minutes", 0) or 0), fallback)
        pct = w.get("used_percent")
        if isinstance(pct, (int, float)):
            out[key] = {"used_percentage": pct, "resets_at": w.get("resets_at")}
    return {"rate_limits": out} if out else {}


def limits_age_s(s: dict) -> float | None:
    """가장 최근 rate_limits 가 몇 초 전 것인지. 없으면 None."""
    ts = s.get("rate_limits_at")
    if not ts:
        return None
    try:
        t = datetime.fromisoformat(str(ts).replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None
    return max(0.0, time.time() - t)


if __name__ == "__main__":
    t0 = time.time()
    r = collect()
    r["elapsed_s"] = round(time.time() - t0, 2)
    r["statusline_like"] = statusline_like(r)
    r["limits_age_s"] = limits_age_s(r)
    json.dump(r, sys.stdout, indent=1, ensure_ascii=False)
    print()
