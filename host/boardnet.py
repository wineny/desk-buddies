"""보드로 POST 를 보내되, 실패하면 ARP 를 깨우고 한 번 더 시도한다.

2026-08-29 밤에 한 시간 넘게 모든 전송이 `timed out` / `No route to host` 로
실패했는데 보드는 멀쩡했다. 원인은 맥이 보드의 MAC 주소를 ARP 로 못 푼 것:
보드가 WiFi 절전(MAX_MODEM) 중이라 브로드캐스트 ARP 요청을 자주 놓치고, 맥은
몇 번 묻다 포기한다. 트래픽이 끊겨 ARP 캐시가 만료된 뒤(20분쯤 안 쓰면)부터
재발한다. `ping` 을 짧은 간격으로 여러 발 쏘면 그중 하나가 보드가 깨어 있는
순간에 걸려 ARP 가 풀린다 — 그 다음 POST 는 곧바로 200 이다.

펌웨어 쪽 근본 대책(절전 끄기)과 별개로, 호스트도 이 보험을 든다. 훅 자체는
5초 안에 끝나야 하지만 여기는 떼어놓은 자식에서 도니 2~3초 더 써도 된다.

2026-09-05 추가: 와이파이 망이 바뀌면(집↔회사) 보드는 알아서 새 망에 붙는데
호스트만 옛 IP 를 들고 있어 모든 전송이 실패했다. ARP 재시도까지 실패하면
find_board.py(MAC 스캔)를 돌려 board.json 을 갱신하고 새 주소로 한 번 더
보낸다 — 망을 옮겨도 첫 실패 한 번이면 이후 푸시가 스스로 살아난다. HTTP
에러(400 등)는 보드가 살아 있다는 뜻이라 재탐색하지 않고, 보드가 정말 없는
망에서 실패가 반복돼도 스윕은 10분에 1번만 돈다(쿨다운 스탬프).

2026-09-06 추가(docs/agent-badge.md §3·§4): 보드가 POST /usage 응답에 지금 고른
에이전트를 실어 준다 — {"ok":1,"agent":"both|claude|codex"}. post_json 은 그 본문을
돌려주고, 여기의 agent 캐시 헬퍼(STATE_DIR/agent-selected)가 호스트 쪽 기억을
맡는다. 옛 펌웨어의 "ok" 는 JSON 이 아니라 both 로 친다.
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
from typing import Callable
from urllib.parse import urlsplit

PING_COUNT = 5
PING_INTERVAL_S = 0.3
PING_WAIT_MS = 500

HERE = Path(__file__).resolve().parent
STATE_DIR = Path(os.environ.get("CLAUDEBOT_STATE_DIR", "/tmp/claudebot-state"))
AGENT_SELECTED_FILE = STATE_DIR / "agent-selected"   # 보드가 고른 에이전트 캐시
# 캐시 수명. 캐시는 POST 응답으로만 갱신되는데 그 캐시가 전송을 막기도 하니(다른 쪽
# 에이전트만 도는 머신), 낡으면 both 로 되돌려 다음 push 가 보드에 닿게 한다 — 보드가
# 걸러도 응답으로 다시 맞춘다(리뷰 HIGH: 자기 강화 교착 방지). 5분에 헛 push 1번.
AGENT_SELECTED_TTL_S = 5 * 60
# "both" 는 호스트 내부값 = "보드 선택을 모른다(옛 펌웨어 "ok" 응답·캐시 없음·TTL 만료)"
# → 걸러내지 않고 다 보낸다. 보드 쪽 BOTH 모드는 2026-09-06 에 뺐다(docs/agent-badge.md §10);
# 새 펌웨어는 응답에 claude|codex 만 실어 온다.
AGENTS = ("both", "claude", "codex")
# QA 조용 창: 이 파일이 있고 30분 이내면 훅은 보드로 아무것도 안 보낸다 (상태·사용량 둘 다).
# 펌웨어 QA(firmware/tools/qa_*.py) 중 다른 세션의 훅 push 가 화면·배지·NVS 캐시를
# 건드리는 것을 막는다. 러너가 만들고 지운다.
QA_QUIET_FILE = STATE_DIR / "qa-quiet"
QA_QUIET_MAX_AGE_S = 30 * 60
REDISCOVER_STAMP = Path("/tmp/claudebot-find-board.stamp")
REDISCOVER_COOLDOWN_S = 600   # 보드 없는 망에서 실패가 반복돼도 스윕은 10분에 1번
REDISCOVER_TIMEOUT_S = 45     # /24 스윕 ~5초, /22 도 ~15초 — 여유 포함


def host_of(url: str) -> str:
    return urlsplit(url).hostname or ""


def qa_quiet() -> bool:
    """QA 조용 창이 켜져 있으면 True. 낡은 플래그(30분 초과)는 무시하고 지운다."""
    try:
        if not QA_QUIET_FILE.exists():
            return False
        if time.time() - QA_QUIET_FILE.stat().st_mtime > QA_QUIET_MAX_AGE_S:
            QA_QUIET_FILE.unlink(missing_ok=True)
            return False
        return True
    except OSError:
        return False


# --- 보드가 고른 에이전트 캐시 (docs/agent-badge.md §4) ---------------------------


def read_agent_selected() -> str:
    """캐시된 보드 선택. 없거나 이상하거나 낡았으면(TTL) both(= 걸러내지 않음)."""
    try:
        if time.time() - AGENT_SELECTED_FILE.stat().st_mtime > AGENT_SELECTED_TTL_S:
            return "both"
        v = AGENT_SELECTED_FILE.read_text(encoding="utf-8").strip()
    except OSError:
        return "both"
    return v if v in AGENTS else "both"


def write_agent_selected(agent: str) -> None:
    if agent not in AGENTS:
        return
    try:
        AGENT_SELECTED_FILE.parent.mkdir(parents=True, exist_ok=True)
        tmp = AGENT_SELECTED_FILE.with_name(f"{AGENT_SELECTED_FILE.name}.{os.getpid()}.tmp")
        tmp.write_text(agent, encoding="utf-8")
        tmp.replace(AGENT_SELECTED_FILE)
    except OSError:
        pass


def agent_from_response(body: bytes | None) -> str:
    """POST /usage 응답 본문 → 보드 선택(claude|codex). JSON 이 아니면(옛 펌웨어 "ok") both(=모름)."""
    if not body:
        return "both"
    try:
        d = json.loads(body.decode("utf-8", "replace"))
    except ValueError:
        return "both"
    a = d.get("agent") if isinstance(d, dict) else None
    return a if a in AGENTS else "both"


def remember_agent_selected(body: bytes | None, log: Callable[[str], None]) -> tuple[str, bool]:
    """응답으로 캐시를 갱신한다. (선택, 바뀌었나) — 바뀌었으면 호출부가 재집계한다."""
    new = agent_from_response(body)
    old = read_agent_selected()
    write_agent_selected(new)   # 같아도 다시 써서 TTL 을 연장한다
    if new != old:
        log(f"보드 선택 {old} → {new}")
    return new, new != old


# --- 에이전트별 마지막 cells 캐시 (docs/agent-badge.md §11) ---------------------------
#
# 배지를 탭해 선택을 바꾸면 표정(state)은 재집계로 곧 따라오지만 한도·사용량 칸은 그
# 에이전트의 다음 Stop 훅(7초 ccusage)까지 직전 값 그대로였다. 그래서 push_usage 가 보낸
# cells 를 에이전트별로 적어 두고, 호스트가 "선택이 바뀌었다" 를 처음 아는 순간(POST 응답)
# 에 고른 쪽의 마지막 cells 를 한 번 더 보내 메운다. 보드가 걸러낸 push 도 그 에이전트의
# 값으로는 유효하니 똑같이 적는다. 낡음 기준은 두지 않는다 — 오래된 값이라도 빈 칸·남의
# 값보다 낫고, 다음 실제 push 가 곧 덮는다. 세션 파일과 같은 폴더라 claude_state.aggregate
# 가 이 파일을 건너뛴다(cells- 접두).

CELLS_CACHE_PREFIX = "cells-"


def cells_cache_path(agent: str) -> Path:
    return STATE_DIR / f"{CELLS_CACHE_PREFIX}{agent}.json"


def save_cells(agent: str, cells: list) -> None:
    if agent not in ("claude", "codex") or not isinstance(cells, list) or not cells:
        return
    try:
        path = cells_cache_path(agent)
        path.parent.mkdir(parents=True, exist_ok=True)
        # 훅 자식이 여럿 동시에 돌므로 tmp 이름에 pid — 같은 tmp 를 서로 덮지 않게
        tmp = path.with_name(f"{path.name}.{os.getpid()}.tmp")
        tmp.write_text(json.dumps({"cells": cells, "saved_at": time.time()}, ensure_ascii=False),
                       encoding="utf-8")
        tmp.replace(path)
    except OSError:
        pass


def load_cells(agent: str) -> list | None:
    """에이전트의 마지막 cells. 없거나 깨졌으면 None."""
    try:
        d = json.loads(cells_cache_path(agent).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    cells = d.get("cells") if isinstance(d, dict) else None
    return cells if isinstance(cells, list) and cells else None


def resend_cells(agent: str, url: str, timeout_s: float, log: Callable[[str], None]) -> bool:
    """선택이 agent 로 바뀐 직후 그 에이전트의 캐시 cells 를 한 번 보낸다.

    캐시가 없으면 아무것도 안 하고 False. 실패해도 예외를 올리지 않는다 — 호출부는
    상태 push 나 훅 처리 중이라 여기서 막히면 안 된다.
    """
    cells = load_cells(agent)
    if cells is None:
        log(f"전환 메우기: {agent} cells 캐시 없음")
        return False
    try:
        post_json(url, {"cells": cells, "agent": agent}, timeout_s, log)
    except (urllib.error.URLError, OSError) as e:
        log(f"전환 메우기: {agent} cells 재전송 실패: {e}")
        return False
    log(f"전환 메우기: {agent} cells 재전송")
    return True


def wake_arp(url: str) -> bool:
    """짧은 ping 여러 발로 보드의 ARP 항목을 살린다. 한 발이라도 돌아오면 True."""
    host = host_of(url)
    if not host:
        return False
    try:
        r = subprocess.run(
            ["ping", "-c", str(PING_COUNT), "-i", str(PING_INTERVAL_S),
             "-W", str(PING_WAIT_MS), host],
            capture_output=True,
            timeout=PING_COUNT * (PING_INTERVAL_S + PING_WAIT_MS / 1000) + 2,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    return r.returncode == 0


def _board_json_url() -> str | None:
    try:
        url = json.loads((HERE / "board.json").read_text(encoding="utf-8")).get("url")
        return url if isinstance(url, str) else None
    except (OSError, json.JSONDecodeError):
        return None


def rediscover(log: Callable[[str], None]) -> str | None:
    """find_board.py 로 board.json 을 갱신하고 새 URL 을 돌려준다.

    쿨다운(10분) 안이거나 실행이 실패하면 None. find_board 는 board.json 의
    IP 가 여전히 보드면(일시 장애) 스윕 없이 곧장 끝난다 — 그 경우도 URL 이
    안 바뀌었으니 호출부가 재시도를 건너뛴다.
    """
    try:
        if (REDISCOVER_STAMP.exists()
                and time.time() - REDISCOVER_STAMP.stat().st_mtime < REDISCOVER_COOLDOWN_S):
            return None
        REDISCOVER_STAMP.touch()   # 병렬 훅 자식들의 스윕 중복 방지 (먼저 찍고 돈다)
    except OSError:
        pass
    try:
        r = subprocess.run(
            [sys.executable, str(HERE / "find_board.py")],
            capture_output=True, text=True, timeout=REDISCOVER_TIMEOUT_S, check=False,
        )
        last = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr.strip()[:120]
        log(f"보드 재탐색 rc={r.returncode}: {last}")
    except (OSError, subprocess.TimeoutExpired) as e:
        log(f"보드 재탐색 실패: {e}")
        return None
    return _board_json_url()


def post_json(url: str, payload: dict, timeout_s: float, log: Callable[[str], None],
              ensure_ascii: bool = False) -> bytes:
    """POST 한 번. 실패하면 ARP 를 깨우고 한 번 더, 그래도 못 닿으면 보드를
    재탐색(find_board)해 새 주소로 마지막 한 번. 다 실패면 예외를 올린다.

    반환값은 **응답 본문**(새 펌웨어 {"ok":1,"agent":"both"} / 옛 펌웨어 b"ok").
    agent_from_response() 로 읽는다.
    """
    data = json.dumps(payload, ensure_ascii=ensure_ascii).encode("utf-8")

    def send(to_url: str, note: str) -> bytes:
        req = urllib.request.Request(
            to_url, data=data, headers={"Content-Type": "application/json"}, method="POST"
        )
        with urllib.request.urlopen(req, timeout=timeout_s) as r:
            body = r.read()
            log(f"보냄 {r.status}{note} {data.decode('utf-8')[:160]}")
            return body

    try:
        return send(url, "")
    except (urllib.error.URLError, OSError) as first:
        alive = wake_arp(url)
        log(f"전송 실패({first}) → ARP 깨우기 {'응답 있음' if alive else '응답 없음'}, 재시도")
    try:
        return send(url, " (재시도)")
    except urllib.error.HTTPError:
        raise   # HTTP 에러 = 보드는 살아 있고 payload 가 거부됐다 — 재탐색 무의미
    except (urllib.error.URLError, OSError):
        # 망이 바뀌어 보드 IP 가 달라졌을 수 있다 (집↔회사).
        new_url = rediscover(log)
        if not new_url or host_of(new_url) == host_of(url):
            raise
        return send(new_url, " (재탐색)")
