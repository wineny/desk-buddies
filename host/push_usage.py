#!/usr/bin/env python3
"""Claude Code 스톱 훅 → 보드로 사용량 밀어넣기.

응답이 끝날 때마다 Claude Code 가 이 스크립트를 부른다. 집계에 7초쯤 걸리는데
스톱 훅은 동기적으로 블로킹되므로, 그대로 하면 응답마다 7초씩 더 기다리게 된다.
그래서 **부모는 자식을 떼어놓고 즉시 끝낸다**(수십 ms). 집계와 전송은 자식이
백그라운드에서 한다. 실패해도 조용히 로그만 남기고 죽는다 — 훅이 사용자 작업을
막는 일은 없어야 한다.

보드 주소는 아래 순서로 정한다:
  1) 환경변수 CLAUDEBOT_BOARD_URL
  2) 같은 폴더의 board.json 의 "url"
  3) 기본값 DEFAULT_BOARD_URL
DHCP 로 주소가 바뀌면 조용히 실패하므로, 공유기에서 보드 MAC
(AA:BB:CC:DD:EE:FF) 에 고정 IP 를 예약해 두는 편이 좋다.

Codex CLI 도 같은 스크립트를 Stop 훅으로 부른다(~/.codex/hooks.json — 훅 페이로드
모양이 Claude Code 와 같다, 2026-09-06 실측). 어느 쪽이 불렀는지는 페이로드의
transcript_path 로 안다: ~/.codex/sessions/ 아래면 Codex. Codex 는 statusline 이
없지만 세션 로그에 한도가 그대로 찍혀서 codex_usage.py 가 거기서 읽는다.

에이전트 배지(docs/agent-badge.md §4): cells 페이로드에 "agent" 를 붙여 보드가
배지를 바꾸거나(both) 고르지 않은 쪽을 거른다. 보드 선택 캐시(agent-selected)가 다른
에이전트면 7초짜리 ccusage 를 헛돌리지 않게 전송 자체를 건너뛴다.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from boardnet import (  # noqa: E402  (같은 디렉터리의 전송 공용 모듈)
    load_cells, post_json, qa_quiet, read_agent_selected, remember_agent_selected,
    resend_cells, save_cells,
)
import account  # noqa: E402  (계정별 한도 캐시·계정 이름)
DEFAULT_BOARD_URL = "http://192.0.2.10/usage"
LOG_PATH = Path(os.environ.get("CLAUDEBOT_LOG", "/tmp/claudebot-push.log"))
POST_TIMEOUT_S = 5

# usage_tools.collect() 를 그대로 재사용한다(금액·주간·5시간 창).
XIAOZHI_MCP_DIR = HERE.parent / "xiaozhi_mcp"

# 아래 두 막대는 진짜 사용 한도 잔량이다.
#
# 한도는 **statusline 훅 페이로드**에만 실려 온다. 스톱 훅에는 안 들어온다
# (스톱 훅이 받는 키: session_id transcript_path cwd prompt_id permission_mode
#  effort hook_event_name stop_hook_active last_assistant_message
#  background_tasks session_crons — 2026-08-23 실측). `claude /status --json`
# 도 없고 대화 기록 JSONL 에도 없어서, 한동안 5시간 창은 "흘러간 시간", 주간은
# "쓴 돈 ÷ 임의 예산 $2000" 으로 때워 두고 있었다.
#
# statusline 페이로드에는 이렇게 들어온다(2026-08-23 실측):
#   "rate_limits": {
#     "five_hour":  {"used_percentage": 11, "resets_at": 1787500200},
#     "seven_day":  {"used_percentage": 22, "resets_at": 1787922000}
#   }
#
# 그래서 ~/.claude/settings.json 의 statusLine 을 `tee` 로 감싸 페이로드를
# STATUSLINE_CACHE 에 떨어뜨려 두고, 스톱 훅인 이 스크립트가 그 파일을 읽는다.
# 감싸는 형태라 기존 HUD 는 그대로 돌아간다:
#   "command": "tee /tmp/claudebot-statusline.json | sh .../omc-hud-cache.sh ..."
# tee 가 파일을 못 열어도 stdin→stdout 복사는 계속하고 파이프라인 종료 코드는
# 뒤쪽 HUD 것이라, 이 장치가 깨져도 HUD 는 죽지 않는다.
# 한도 칸 이름. 보드가 대문자로 찍는다("5H" / "WEEK"). 한때 "7d" 였는데,
# 2026-08-29 대기 화면을 목업대로 바꾸면서 오른쪽 열(비용)이 화면에서 빠져
# "week"(달력 주)와 헷갈릴 일이 없어졌고, 시안 표기가 WEEK 라 맞췄다.
BLOCK_LABEL = "5h"
WEEK_LABEL = "week"

# statusline 이 떨궈 놓는 페이로드. 재부팅하면 사라지지만 세션이 살아 있으면
# 몇 초 안에 다시 생긴다.
# 설정 폴더(CLAUDE_CONFIG_DIR)마다 따로 둔다 — 계정 여럿을 쓰면 한 파일에선 섞였다(account.py).
STATUSLINE_CACHE = account.statusline_cache()
# 이보다 오래된 캐시는 안 쓴다. 5시간 창은 리셋되면 값이 확 떨어지므로 낡은 값을
# 그대로 보여주면 거짓말이 된다. 여러 세션이 같은 파일을 덮어써도 상관없다 —
# 한도는 세션이 아니라 계정 단위라 어느 세션이 쓴 값이든 같다.
STATUSLINE_MAX_AGE_S = 15 * 60

# rate_limits 의 키 → 화면 라벨. 순서가 곧 화면의 위아래 순서다.
# 펌웨어는 창을 두 개(UsageStats::kMaxWindows)까지만 그린다.
RATE_LIMIT_WINDOWS = (
    ("five_hour", BLOCK_LABEL),
    ("seven_day", WEEK_LABEL),
)


def log(msg: str) -> None:
    try:
        with LOG_PATH.open("a", encoding="utf-8") as f:
            f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}\n")
    except OSError:
        pass


# --- 화면에 찍을 글자 만들기 ---------------------------------------------

# 컨텍스트 사용량은 화면에서 뺐다. 세션을 여러 개 동시에 돌리면 "방금 응답을
# 끝낸 세션" 하나의 값이라 매번 다른 세션 것이 떠서 읽을 수가 없었다.


def fmt_money(usd: float) -> str:
    """$196 / $12.3 / $0.42. 자릿수가 늘수록 소수점을 줄여 폭을 일정하게 둔다."""
    if usd >= 100:
        return f"${usd:.0f}"
    if usd >= 10:
        return f"${usd:.1f}"
    return f"${usd:.2f}"


def fmt_tokens(n: int) -> str:
    """156M / 1.6B / 820k. 화면 폭이 좁아 세 글자 안팎으로 줄인다.

    1억(=100M)을 넘으면 소수점을 뗀다. "156.1M" 은 길기만 하고 그 자리 숫자가
    의미를 갖지 않는다.
    """
    if n >= 1_000_000_000:
        return f"{n / 1_000_000_000:.1f}B"
    if n >= 100_000_000:
        return f"{n / 1_000_000:.0f}M"
    if n >= 1_000_000:
        return f"{n / 1_000_000:.1f}M"
    if n >= 1_000:
        return f"{n / 1_000:.0f}k"
    return str(n)


# --- 진짜 한도: statusline 이 떨궈 둔 페이로드에서 읽는다 -----------------


def read_statusline_payload() -> dict:
    """statusline 캐시를 읽는다. 못 읽으면 빈 dict."""
    try:
        age = time.time() - STATUSLINE_CACHE.stat().st_mtime
    except OSError:
        log("한도: statusline 캐시 없음 (statusLine 이 tee 로 감싸져 있는지 확인)")
        return {}
    if age > STATUSLINE_MAX_AGE_S:
        log(f"한도: statusline 캐시가 낡음 ({int(age)}초 전)")
        return {}
    try:
        # tee 가 덮어쓰는 중에 읽으면 JSON 이 찢어져 있을 수 있다. 흔한 일이
        # 아니고 다음 응답 때 다시 읽으니 조용히 넘긴다.
        return json.loads(STATUSLINE_CACHE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        log(f"한도: statusline 캐시 읽기 실패: {e}")
        return {}


def remaining_label(resets_at: object) -> str:
    """리셋까지 남은 시간을 '2h 40m' / '4d 23h' / '35m' 으로. 모르면 빈 문자열.

    단위 하나(`3h`)로는 오차가 최대 30분이라 "곧 리셋되나"를 판단하기 어렵다.
    두 단위까지 적는다. 라벨 칸은 32바이트라 넉넉하지만 화면 폭(160px)이 좁고
    넘치면 잘리므로(LV_LABEL_LONG_CLIP) 세 단위로는 가지 않는다. 최장은
    "Weekly - 6d 23h" 15자다.
    """
    if not isinstance(resets_at, (int, float)):
        return ""
    left = resets_at - time.time()
    if left <= 0:
        # 캐시가 찍힌 뒤 창이 리셋된 경우. 남은 시간만 빼고 퍼센트는 그대로
        # 보낸다 — 최대 STATUSLINE_MAX_AGE_S 만큼만 어긋난다.
        return ""

    # 먼저 분 단위로 반올림해 두고 거기서 쪼갠다. 큰 단위부터 자르면 4h 59.6m 이
    # "4h 60m" 이 되는 자리올림을 따로 처리해야 하는데, 분으로 모아 두면 divmod
    # 가 알아서 5h 로 넘겨 준다. 1분 미만이라도 "0m" 은 이미 리셋된 것처럼
    # 보이므로 1분으로 막는다.
    minutes = max(1, int(left / 60 + 0.5))
    if minutes < 60:
        return f"{minutes}m"
    hours, minutes = divmod(minutes, 60)
    if hours < 24:
        return f"{hours}h {minutes}m" if minutes else f"{hours}h"
    # 하루 넘게 남았으면 분은 의미가 없다. 시간까지만 적는다.
    days, hours = divmod(hours, 24)
    return f"{days}d {hours}h" if hours else f"{days}d"


def limit_cells(payload: dict, week_label: str = WEEK_LABEL) -> list | None:
    """statusline 페이로드 → 왼쪽 열(한도) 칸 둘. 못 만들면 None.

    화면에는 "5h  27% | 2h 40m" 처럼 찍힌다. 퍼센트가 왼쪽, 리셋까지 남은
    시간이 오른쪽이다.
    """
    limits = payload.get("rate_limits")
    if not isinstance(limits, dict):
        return None

    cells = []
    found = 0
    for key, label in RATE_LIMIT_WINDOWS:
        label = week_label if label == WEEK_LABEL else label
        entry = limits.get(key)
        pct = entry.get("used_percentage") if isinstance(entry, dict) else None
        if not isinstance(pct, (int, float)):
            # 창 하나가 빠져도 자리는 지킨다. 순서가 곧 화면 자리라서, 5h 가
            # 빠지면 week 가 왼쪽 블록으로 밀리고 today 비용이 오른쪽 블록에
            # 올라와 "5h 대신 $909" 가 된다 (2026-08-29 20:08 실측 — statusline
            # 이 5시간 창을 새로 열 때 five_hour 를 잠깐 안 실어 보낸다).
            # left 를 비우면 보드가 그 칸을 숨긴다. 없는 한도를 "0%" 로 찍지 않는다
            # — 5시간 한도가 없는 요금제·Codex 계정에선 영영 0% 로 남기 때문이다.
            # 잠깐 빠지는 경우는 마지막 값을 붙들지 않는다. 다음 statusline 이면
            # 돌아오고, 새 창이 열리는 순간의 옛 값은 어차피 맞지 않는다.
            cells.append({"name": label, "left": "", "right": ""})
            continue
        found += 1
        cells.append(
            {
                "name": label,
                # 쓴 양이다. 남은 양이 아니다 — 0 에서 시작해 100 으로 찬다.
                "left": f"{max(0, min(100, int(pct)))}%",
                "right": remaining_label(entry.get("resets_at")),
            }
        )
    return cells if found else None


def stale_limit_cells(cached: list | None, week_label: str = WEEK_LABEL) -> list | None:
    """에이전트별 cells 캐시(boardnet.load_cells)의 한도 칸 둘을 '~' 붙여 재사용한다.

    statusline 캐시가 낡았을 때(자리 비운 뒤 첫 훅) 예산 추정 대신 **마지막 실제 한도**를
    보여 준다 — "5h~ 24% | 3h 16m" 꼴. '~' 는 이제 "낡은 값" 표시다. 오른쪽(리셋까지)도
    그대로 두는데, 창이 지났으면 어긋난 값이지만 '~' 가 그 뜻이다. 캐시 모양이 아니면
    None(호출부가 추정으로 넘어간다).
    """
    if not isinstance(cached, list) or len(cached) < 2:
        return None
    out = []
    for cell, label in zip(cached[:2], (BLOCK_LABEL, week_label)):
        if not isinstance(cell, dict) or str(cell.get("name", "")).rstrip("~") != label:
            return None
        out.append({"name": f"{label}~", "left": str(cell.get("left", "0%")),
                    "right": str(cell.get("right", ""))})
    return out


def fallback_limit_cells(s: dict, week_label: str = WEEK_LABEL) -> list:
    """한도를 정말 아무 데서도 못 구했을 때(statusline 도, cells 캐시도 없음)의 추정치.

    5시간은 창에서 흘러간 시간, 주간은 이번 주 쓴 돈 ÷ $2000(usage_tools 의 임의 예산)
    이라 실제 한도와 뜻이 다르다. 2026-09-06 14:41 에 "week~ 100%"(실제 22%) 를 찍은 게
    이 추정이었고, 그 뒤로 statusline 이 낡으면 stale_limit_cells 가 먼저 든다 — 여기는
    첫 설치처럼 캐시가 아예 없는 머신에서만 나온다. 남은 시간은 알 수 없으니 오른쪽을
    비운다(구분자도 같이 사라진다).
    """
    return [
        {
            "name": f"{BLOCK_LABEL}~",
            "left": f"{int(s.get('block_elapsed_pct', 0) or 0)}%",
            "right": "",
        },
        {
            "name": f"{week_label}~",
            "left": f"{int(s.get('week_pct', 0) or 0)}%",
            "right": "",
        },
    ]


def usage_cells(s: dict) -> list:
    """오른쪽 열(쓴 양) 칸 둘. "today  $212 | 176M" 처럼 찍힌다.

    토큰은 캐시까지 포함한 전량이다. ccusage 의 inputTokens 는 캐시를 빼고 세기
    때문에 그것만 쓰면 실제의 1% 도 안 된다.
    """
    today_tokens = (
        int(s.get("today_input_tokens", 0) or 0)
        + int(s.get("today_output_tokens", 0) or 0)
        + int(s.get("today_cache_read_tokens", 0) or 0)
        + int(s.get("today_cache_creation_tokens", 0) or 0)
    )
    return [
        {
            "name": "today",
            "left": fmt_money(float(s.get("today_cost_usd", 0.0))),
            "right": fmt_tokens(today_tokens),
        },
        {
            "name": "week",
            "left": fmt_money(float(s.get("week_cost_usd", 0.0))),
            "right": fmt_tokens(int(s.get("week_total_tokens", 0) or 0)),
        },
    ]


# --- 보드로 보낼 값 만들기 ------------------------------------------------


def detect_agent(hook_payload: dict) -> str:
    """"claude" 또는 "codex". 환경변수 CLAUDEBOT_AGENT 가 있으면 그것이 우선."""
    env = os.environ.get("CLAUDEBOT_AGENT", "").strip().lower()
    if env in ("claude", "codex"):
        return env
    tp = str(hook_payload.get("transcript_path", "") or "")
    return "codex" if "/.codex/" in tp or "\\.codex\\" in tp else "claude"


def codex_usage_cells(s: dict) -> list:
    """Codex 오른쪽 열. 비용이 없으니 토큰 전량만 — "today  23.6k" 꼴."""
    return [
        {"name": "today", "left": fmt_tokens(int(s.get("today_total_tokens", 0) or 0)), "right": ""},
        {"name": "week", "left": fmt_tokens(int(s.get("week_total_tokens", 0) or 0)), "right": ""},
    ]


def build_payload_codex() -> dict:
    import codex_usage  # noqa: E402  (같은 디렉터리)

    # Stop 훅은 세션 로그의 token_count 줄보다 먼저 뜰 수 있다(PostToolUse 가
    # function_call_output 보다 21ms 앞선 것을 실측). 이 워커는 이미 부모에서
    # 떼어져 있으니 1초 기다려도 Codex 를 막지 않는다.
    time.sleep(1.0)
    s = codex_usage.collect()
    age = codex_usage.limits_age_s(s)
    limits = limit_cells(codex_usage.statusline_like(s))
    limits_real = limits is not None
    if limits is None:
        limits = stale_limit_cells(load_cells("codex"))
        if limits is not None:
            log("한도(codex): 세션 로그에 rate_limits 가 없어 마지막 실제 한도 재사용(~)")
        else:
            limits = [
                {"name": f"{BLOCK_LABEL}~", "left": "0%", "right": ""},
                {"name": f"{WEEK_LABEL}~", "left": "0%", "right": ""},
            ]
            log("한도(codex): 세션 로그에 rate_limits 가 없어 빈 칸")
    elif age is not None and age > STATUSLINE_MAX_AGE_S:
        log(f"한도(codex): 마지막 rate_limits 가 {int(age)}초 전 것 (그대로 사용)")
    cells = limits + codex_usage_cells(s)
    log(f"칸(codex, 파일 {s.get('files_read', 0)}개): {cells}")
    return {"cells": cells, LIMITS_REAL_KEY: limits_real}


def build_payload(hook_payload: dict) -> dict:
    agent = detect_agent(hook_payload)
    payload = build_payload_codex() if agent == "codex" else build_payload_claude()
    payload["agent"] = agent   # 보드가 배지를 바꾸거나(both) 거른다(다른 쪽 선택)
    return payload


def build_payload_claude() -> dict:

    sys.path.insert(0, str(XIAOZHI_MCP_DIR))
    import usage_tools  # noqa: E402  (경로를 넣은 뒤에야 import 된다)

    s = usage_tools.collect()  # 약 7초

    # 순서가 곧 화면 자리다. 앞 둘이 왼쪽 열(위·아래), 뒤 둘이 오른쪽 열.
    limits, real = claude_limit_cells(s)
    cells = limits + usage_cells(s)
    log(f"칸: {cells}")
    return {"cells": cells, LIMITS_REAL_KEY: real}


# build_payload_* 가 payload 에 얹는 호스트 내부 표식. "앞 두 칸(한도)이 지금 실제로 받은
# 값인가". post() 가 보드로 보내기 전에 떼고, 캐시에 적을지 정하는 데만 쓴다 —
# 추정·낡은 값을 캐시에 넣으면 다음 낡음 폴백이 그걸 "마지막 실제 한도" 로 되풀이한다.
LIMITS_REAL_KEY = "_limits_real"


def claude_limit_cells(s: dict) -> tuple[list, bool]:
    """왼쪽 열(한도) 둘 + 실제값인가. statusline(실시간) → cells 캐시의 마지막 실제 한도('~')
    → 추정('~'). 사용량 칸(today/week 비용)은 ccusage 로 언제나 실시간이라 여기와 무관하다.
    """
    # 주간 칸 이름 = 계정 이름("ny" → 보드에 "NY"). 다른 계정의 캐시는 이름이 달라 재사용하지 않는다.
    label = account.account_label()
    limits = limit_cells(read_statusline_payload(), label)
    if limits is not None:
        return limits, True
    limits = stale_limit_cells(load_cells("claude"), label)
    if limits is not None:
        log(f"한도: statusline 못 씀 → 마지막 실제 한도 재사용 "
            f"({limits[0]['name']} {limits[0]['left']}, {limits[1]['name']} {limits[1]['left']})")
        return limits, False
    log("한도: statusline 도 cells 캐시도 없어 추정치로 채움")
    return fallback_limit_cells(s, label), False


def cache_cells(agent: str, cells: list, limits_real: bool) -> None:
    """에이전트별 cells 캐시 갱신. 한도 칸이 실제값일 때만 통째로 적는다.

    한도가 낡은 값/추정이면 캐시의 **실제 한도는 지키고 사용량 칸만** 갈아 끼운다(그래서
    자리를 오래 비워도 마지막 실제 한도가 캐시에 남는다). 캐시에 실제 한도가 없으면
    아무것도 안 적는다 — 추정치를 한 번이라도 적으면 영영 "마지막 실제 한도" 행세를 한다
    (리뷰 HIGH).
    """
    if limits_real:
        save_cells(agent, cells)
        return
    prev = load_cells(agent)
    if not prev or len(prev) < 2 or any(str(c.get("name", "")).endswith("~") for c in prev[:2]):
        return
    save_cells(agent, prev[:2] + cells[2:])


def board_url() -> str:
    env = os.environ.get("CLAUDEBOT_BOARD_URL")
    if env:
        return env
    cfg = HERE / "board.json"
    if cfg.is_file():
        try:
            url = json.loads(cfg.read_text(encoding="utf-8")).get("url")
            if url:
                return url
        except (OSError, json.JSONDecodeError) as e:
            log(f"board.json 읽기 실패: {e}")
    return DEFAULT_BOARD_URL


def post(payload: dict) -> str:
    """보드로 보내고, 응답이 알려 준 보드 선택(both|claude|codex)을 캐시에 적고 돌려준다."""
    # 실패하면 boardnet 이 ARP 를 깨우고 한 번 더 보낸다(절전 중인 보드가
    # ARP 를 놓쳐 한 시간 넘게 연속 실패했던 2026-08-29 사고의 보험).
    url = board_url()
    limits_real = bool(payload.pop(LIMITS_REAL_KEY, False))   # 호스트 내부 표식, 보드엔 안 보낸다
    body = post_json(url, payload, POST_TIMEOUT_S, lambda m: log(f"{m} ← {url}"))
    selected, changed = remember_agent_selected(body, log)
    agent = payload.get("agent")
    cells = payload.get("cells")
    if agent in ("claude", "codex") and isinstance(cells, list) and cells:
        # 보드가 걸렀어도(선택 ≠ agent) 그 에이전트의 값으로는 유효 — 전환 메우기 재료 (§11)
        cache_cells(agent, cells, limits_real)
    if changed and selected in ("claude", "codex") and not (selected == agent and cells):
        # 이 응답에서 선택이 바뀐 걸 처음 알았다. 방금 보낸 게 고른 쪽의 cells 그 자체가
        # 아니면(ping 이거나 다른 에이전트 cells) 고른 쪽의 마지막 cells 를 바로 메운다.
        resend_cells(selected, url, POST_TIMEOUT_S, lambda m: log(f"{m} ← {url}"))
    return selected


def should_push(agent: str) -> bool:
    """보드 선택 캐시가 다른 에이전트면 7초짜리 집계를 건너뛴다 — 단, 캐시가 낡았을 수
    있으니(배지 탭은 보드에서만 일어난다) 먼저 {"agent": X} 한 줄을 보내 응답으로 선택을
    다시 확인한다. 보드는 agent 단독 페이로드를 200 으로 받고(걸러도 handled) 현재 선택을
    답한다. 그래서 캐시가 전송을 막고 전송이 캐시를 못 고치는 교착(리뷰 HIGH)이 없다.
    """
    selected = read_agent_selected()
    if selected == "both" or selected == agent:
        return True
    selected = post({"agent": agent})
    if selected == "both" or selected == agent:
        log(f"보드 선택 재확인 → {selected}, 전송 진행")
        return True
    log(f"건너뜀: 보드 선택 {selected}, 이 훅은 {agent}")
    return False


# --- 실행 경로 -------------------------------------------------------------


def run_worker(payload_path: str) -> int:
    """떼어놓은 자식. 여기서 느린 일을 전부 한다."""
    try:
        hook_payload = json.loads(Path(payload_path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        log(f"페이로드 읽기 실패: {e}")
        return 0
    finally:
        try:
            os.unlink(payload_path)
        except OSError:
            pass

    try:
        if not should_push(detect_agent(hook_payload)):
            return 0
        post(build_payload(hook_payload))
    except (urllib.error.URLError, OSError) as e:
        # 보드가 꺼져 있거나 주소가 바뀐 흔한 경우. 조용히 넘긴다.
        log(f"전송 실패(보드 꺼짐/주소 변경 가능): {e}")
    except Exception as e:  # 집계 실패 등
        log(f"오류: {type(e).__name__}: {e}")
    return 0


def main() -> int:
    if len(sys.argv) > 2 and sys.argv[1] == "--worker":
        return run_worker(sys.argv[2])

    if qa_quiet():
        return 0   # 펌웨어 QA 조용 창 — cells push 가 배지·NVS 캐시를 건드리지 않게
    # 부모: 페이로드를 파일로 넘기고 자식을 떼어놓은 뒤 즉시 끝낸다.
    raw = sys.stdin.read()
    tmp = Path(f"/tmp/claudebot-hook-{os.getpid()}-{int(time.time()*1000)}.json")
    try:
        tmp.write_text(raw or "{}", encoding="utf-8")
        subprocess.Popen(
            [sys.executable, str(Path(__file__).resolve()), "--worker", str(tmp)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,  # 부모가 죽어도 살아남는다
        )
    except Exception as e:
        log(f"자식 실행 실패: {e}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
