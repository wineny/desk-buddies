#!/usr/bin/env python3
"""훅 명령에 쓸 파이썬을 고른다 — 보드에 실제로 접속되는 것으로.

맥은 앱마다 「로컬 네트워크」 권한을 따로 준다. python.org 설치판
(/Library/Frameworks/Python.framework/…/python3)은 이 권한에 막혀 보드로 소켓을 못 열고
`No route to host` 가 난다(curl 은 됨). 훅을 그냥 `python3 …` 로 깔면 PATH 에 따라 이 판이
잡혀 보드 전송이 실패하고, 60초 쉬었다 다시 보내느라 자막이 늦는다(2026-11-08, 24시간 408번
중 137번 실패). 그래서 설치기는 절대 경로를 박는다.

고르는 순서: homebrew python3 → /usr/local → 설치기를 돌린 파이썬(sys.executable) → /usr/bin.
board.json 에 보드 주소가 있으면 각 후보로 보드에 TCP 접속을 해 보고 처음 되는 것을 쓴다.
아무것도 안 되면(보드 꺼짐·다른 망) 접속 확인 없이 첫 후보를 쓴다.

    python3 hook_python.py          # 고른 결과만 출력
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path
from urllib.parse import urlsplit

HERE = Path(__file__).resolve().parent

PREFERRED = ("/opt/homebrew/bin/python3", "/usr/local/bin/python3")
FALLBACK = ("/usr/bin/python3",)
PROBE_TIMEOUT_S = 3
PROBE_CODE = ("import socket, sys\n"
              "socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=float(sys.argv[3])).close()\n")


def board_json_url() -> str | None:
    """board.json 의 보드 주소. boardnet 은 import 때 상태 폴더를 정하므로 여기서 따로 읽는다."""
    try:
        url = json.loads((HERE / "board.json").read_text(encoding="utf-8")).get("url")
    except (OSError, ValueError):
        return None
    return url if isinstance(url, str) else None


BLOCKED_PREFIX = "/Library/Frameworks/Python.framework/"   # python.org 설치판 — 로컬 네트워크 권한에 잘 막힌다


def is_framework(p: str) -> bool:
    return str(Path(p).resolve()).startswith(BLOCKED_PREFIX)


def candidates() -> list[str]:
    """실행 가능한 후보(같은 실물은 한 번만, python.org 판은 맨 뒤). 경로는 원래 이름 그대로.

    /usr/local/bin/python3 는 python.org 판의 링크인 경우가 많아 실물로 판단한다.
    """
    out, seen = [], set()
    for p in (*PREFERRED, sys.executable, *FALLBACK):
        if not p or not os.access(p, os.X_OK):
            continue
        real = str(Path(p).resolve())
        if real in seen:
            continue
        seen.add(real)
        out.append(p)
    return sorted(out, key=is_framework)       # 안정 정렬: 순서는 지키고 python.org 판만 뒤로


def can_reach(python: str, host: str, port: int) -> bool:
    """그 파이썬으로 보드에 TCP 접속이 되나. 맥 권한은 실행 파일마다 다르니 꼭 그 파일로 돌린다."""
    try:
        r = subprocess.run([python, "-I", "-c", PROBE_CODE, host, str(port), str(PROBE_TIMEOUT_S)],
                           capture_output=True, timeout=PROBE_TIMEOUT_S + 3, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return r.returncode == 0


def pick_python(board_url: str | None = None) -> tuple[str, str]:
    """(파이썬 절대 경로, 고른 이유 한 줄)."""
    env = os.environ.get("CLAUDEBOT_HOOK_PYTHON", "").strip()
    if env:
        if not os.access(env, os.X_OK):
            print(f"!! CLAUDEBOT_HOOK_PYTHON={env} 는 실행 파일이 아니에요 — 훅이 돌지 않아요", file=sys.stderr)
        return env, "CLAUDEBOT_HOOK_PYTHON 으로 지정됨"
    cands = candidates()
    if not cands:
        return "python3", "절대 경로 후보가 없어 python3 그대로"
    url = board_url or os.environ.get("CLAUDEBOT_BOARD_URL") or board_json_url()
    parts = urlsplit(url) if url else None
    if not parts or not parts.hostname:
        return ([c for c in cands if not is_framework(c)] or cands)[0], "보드 주소를 몰라 접속 확인 없이 고름"
    port = parts.port or 80
    blocked = []
    for c in cands:
        if can_reach(c, parts.hostname, port):
            note = f"보드 {parts.hostname} 접속 확인"
            return c, note + (f" (안 된 것: {', '.join(blocked)})" if blocked else "")
        blocked.append(c)
    safe = [c for c in cands if not is_framework(c)] or cands
    return safe[0], f"어느 파이썬으로도 보드 {parts.hostname} 에 안 닿아(보드 꺼짐?) 접속 확인 없이 고름"


if __name__ == "__main__":
    py, why = pick_python()
    print(f"{py}  — {why}")
