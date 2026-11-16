#!/usr/bin/env python3
"""옷장 언락 판정 (docs/wardrobe.md 획득 규칙) + 보드 전달 대기열.

claude_state.py 의 자식(훅마다 뜨는 백그라운드 프로세스)이 부른다. 판정은 전부
`~/.claudebot/wardrobe.json` 미러 하나로 한다 — 보드 NVS 비트마스크의 원본이
이 파일이라, 보드를 바꿔도 여기서 다시 흘려보내면 된다.

언락 키는 펌웨어 `wardrobe_store.cc` 의 kUnlockKeys 와 같아야 한다:
    crown   하루에 서브에이전트 10개 이상 (SubagentStart 일일 카운터)
    shades  에러 없이 50턴 연속 (Stop 카운터, PostToolUseFailure 에 리셋)
    lofi    밤 23시 이후 이벤트가 있는 날 5일
    flag    커밋 감지 (Stop 때 cwd 의 git HEAD 가 지난번과 달라짐)

전달은 at-least-once: 언락 즉시 보드로 {"unlock": key} 를 보내 보고, 실패하면
"acked" 에 안 넣고 다음 훅 때 다시 시도한다. 보드는 이미 가진 비트를 다시
받아도 "already owned" 로 무시하므로 중복 전송은 무해하다.

붕대(주간 한도 100%)는 시안에 아이템이 없어 뺐다.
"""

from __future__ import annotations

import fcntl
import json
import subprocess
import time
from datetime import datetime
from pathlib import Path
from typing import Callable

MIRROR = Path.home() / ".claudebot" / "wardrobe.json"

CROWN_SUBAGENTS_PER_DAY = 10
SHADES_CLEAN_TURNS = 50
LOFI_LATE_DAYS = 5
LOFI_FROM_HOUR = 23
KEEP_DAYS = 21          # 일일 카운터 보존 기간
GIT_TIMEOUT_S = 2.0


def _read(path: Path) -> dict:
    try:
        d = json.loads(path.read_text(encoding="utf-8"))
        return d if isinstance(d, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def _write(path: Path, d: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(d, ensure_ascii=False, indent=1), encoding="utf-8")
    tmp.replace(path)


def _git_head(cwd: str) -> str | None:
    try:
        r = subprocess.run(
            ["git", "-C", cwd, "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=GIT_TIMEOUT_S,
        )
        return r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else None
    except (OSError, subprocess.TimeoutExpired):
        return None


def _prune_days(days: dict, today: str) -> dict:
    keys = sorted(days)[-KEEP_DAYS:]
    return {k: days[k] for k in keys if k <= today}


def update_on_event(payload: dict, ts: float, mirror: dict,
                    git_head: Callable[[str], str | None] = _git_head) -> list[str]:
    """이벤트 하나를 카운터에 반영하고 **새로** 언락된 키 목록을 돌려준다.

    mirror 는 제자리에서 고친다(저장은 process() 가 한다). git_head 는 테스트에서
    바꿔 끼우는 주입점.
    """
    event = str(payload.get("hook_event_name", "") or "")
    counters = mirror.setdefault("counters", {})
    unlocked: list[str] = mirror.setdefault("unlocked", [])
    new: list[str] = []
    local = datetime.fromtimestamp(ts)
    today = local.strftime("%Y-%m-%d")

    def earn(key: str) -> None:
        if key not in unlocked:
            unlocked.append(key)
            new.append(key)

    # crown — 하루 서브에이전트 수
    if event == "SubagentStart":
        days = counters.setdefault("subagents", {})
        days[today] = int(days.get(today, 0)) + 1
        counters["subagents"] = _prune_days(days, today)
        if days[today] >= CROWN_SUBAGENTS_PER_DAY:
            earn("crown")

    # shades — 에러 없이 50턴
    if event == "PostToolUseFailure":
        counters["turns_since_error"] = 0
    elif event == "Stop":
        counters["turns_since_error"] = int(counters.get("turns_since_error", 0)) + 1
        if counters["turns_since_error"] >= SHADES_CLEAN_TURNS:
            earn("shades")

    # lofi — 밤 23시 이후 이벤트가 있는 날 5일
    if local.hour >= LOFI_FROM_HOUR:
        late = counters.setdefault("late_dates", [])
        if today not in late:
            late.append(today)
            counters["late_dates"] = sorted(late)[-KEEP_DAYS:]
        if len(counters["late_dates"]) >= LOFI_LATE_DAYS:
            earn("lofi")

    # flag — Stop 때 git HEAD 변화 (언락 전까지만 git 을 부른다)
    if event == "Stop" and "flag" not in unlocked:
        cwd = str(payload.get("cwd", "") or "")
        if cwd:
            head = git_head(cwd)
            if head is not None:
                heads = counters.setdefault("git_heads", {})
                prev = heads.get(cwd)
                heads[cwd] = head
                if prev is not None and prev != head:
                    earn("flag")

    return new


def pending(mirror: dict) -> list[str]:
    acked = mirror.get("acked", [])
    return [k for k in mirror.get("unlocked", []) if k not in acked]


def process(payload: dict, ts: float,
            post: Callable[[dict], None],
            log: Callable[[str], None] = lambda _m: None,
            mirror_path: Path = MIRROR) -> list[str]:
    """이벤트 반영 + 미전달 언락 전송. claude_state.py 의 자식에서 부른다.

    post 는 성공 시 조용히 리턴, 실패 시 예외를 던지는 함수(post_json 래핑).
    미러 파일 자체를 fcntl 락으로 잠가 자식끼리의 경합을 막는다.
    """
    mirror_path.parent.mkdir(parents=True, exist_ok=True)
    lock_path = mirror_path.with_suffix(".lock")
    with lock_path.open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        mirror = _read(mirror_path)
        new = update_on_event(payload, ts, mirror)
        for key in new:
            log(f"옷장 언락: {key}")
        todo = pending(mirror)
        acked = mirror.setdefault("acked", [])
        for key in todo:
            try:
                post({"unlock": key})
                acked.append(key)
                mirror["acked_at"] = time.time()
                log(f"옷장 언락 전달: {key}")
            except Exception as e:  # noqa: BLE001 — 보드가 꺼져 있으면 다음 훅 때 재시도
                log(f"옷장 언락 전달 실패({key}): {e}")
                break
        _write(mirror_path, mirror)
        return new
