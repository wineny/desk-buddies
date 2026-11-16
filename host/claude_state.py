#!/usr/bin/env python3
"""Claude Code 훅 → 세션 상태 집계 → 보드로 표정 상태 밀어넣기.

훅 11종(UserPromptSubmit, Pre/PostToolUse, PostToolUseFailure, Stop,
SubagentStart/Stop, Notification, PermissionRequest, SessionStart/End)이 전부
이 스크립트를 부른다. push_usage.py 와 같은 패턴으로 **부모는 자식을 떼어놓고
즉시 끝난다** — 훅이 Claude 를 막으면 안 된다.

자식이 하는 일:
  1) 세션별 상태 파일 갱신  /tmp/claudebot-state/<session_id>.json
  2) 전 세션을 우선순위로 집계  (권한 > 에러 > 서브에이전트 > 작업 > 생각 > 완료 > 대기)
  3) 집계 결과를 desired.json 에 적고, 푸셔 락을 잡은 자식 하나만 보드에 POST

전송이 단일 비행(single-flight)인 이유: 이벤트가 피크에 초당 5건씩 오는데
보드가 꺼져 있으면 타임아웃이 이벤트마다 직렬화되어 자식 프로세스가 무한히
쌓인다(코드 리뷰 실측). 그래서 상태 갱신 락과 전송 락을 분리하고, 전송 락을
못 잡은 자식은 그냥 죽는다 — 락을 쥔 푸셔가 루프 끝에 desired 최신값을 다시
확인하므로 마지막 상태는 반드시 전달된다. 보드 전송 실패 시 60초 백오프.

시간 복귀 규칙(에러 5초→작업, 완료 8초→대기, 60초 무이벤트→대기, 10분→수면)은
**펌웨어의 1초 틱이 처리한다.** 호스트는 이벤트가 있을 때만 보낸다. 다만 집계할
때 낡은 세션 파일에는 같은 감쇠를 적용해, 죽은 세션이 화면을 붙들지 않게 한다.

ctx 는 ASCII 만 보낸다 — 보드의 몬세라트 폰트에 한글 글리프가 없다.

Codex CLI(0.153.2+) 도 ~/.codex/hooks.json 으로 같은 이벤트·같은 페이로드를
보낸다(2026-09-06 실측: session_id·hook_event_name·tool_name·tool_use_id·
transcript_path 전부 동일). 다만 PostToolUseFailure 와 Notification 이 없다 —
Codex 의 도구 실패는 PostToolUse 의 tool_response 로는 알 수 없고(exit 1 도 빈
문자열) 세션 로그의 function_call_output 에 "Process exited with code N" 으로만
남아서, codex_tool_failed() 가 transcript_path 꼬리에서 그걸 읽어 error 를 만든다.

에이전트 배지(docs/agent-badge.md §4, 2026-09-06): 세션 파일에 agent(claude|codex)
를 적고, 보드가 POST 응답으로 알려 준 선택(STATE_DIR/agent-selected, boardnet 헬퍼)이
both 가 아니면 집계를 그 에이전트 세션으로 좁힌다. 페이로드에 이긴 세션의 agent 를
붙여 보드가 배지를 바꾸고(both) 또는 한 번 더 거른다(캐시가 낡았을 때의 이중 안전).
응답의 선택이 캐시와 다르면 캐시를 고치고 곧바로 재집계해 한 바퀴 더 민다 — 전환
직후 고른 쪽의 마지막 상태를 메운다(§6).

서브에이전트 실측(2026-08-29): SubagentStart/Stop 은 **부모 session_id 로
온다.** 다만 백그라운드 서브에이전트는 부모 턴이 끝난(Stop) 뒤에도 살아
있으므로, Stop 에서 카운터를 리셋하면 나중의 SubagentStop 이 어긋난다(리뷰
HIGH-2). 그래서 시작 시각 리스트로 관리하고 Stop 은 건드리지 않는다.
"""

from __future__ import annotations

import fcntl
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from boardnet import (  # noqa: E402  (같은 디렉터리의 전송 공용 모듈)
    CELLS_CACHE_PREFIX, post_json, qa_quiet, read_agent_selected, remember_agent_selected,
    resend_cells,
)
STATE_DIR = Path(os.environ.get("CLAUDEBOT_STATE_DIR", "/tmp/claudebot-state"))
LOG_PATH = Path(os.environ.get("CLAUDEBOT_STATE_LOG", "/tmp/claudebot-state.log"))
LAST_PUSH = STATE_DIR / "last-push.json"
DESIRED = STATE_DIR / "desired.json"
LOCK_FILE = STATE_DIR / "lock"        # 상태 갱신·집계 락
PUSH_LOCK = STATE_DIR / "push-lock"   # 전송 단일 비행 락
BACKOFF_FILE = STATE_DIR / "backoff"  # 마지막 전송 실패 시각
# QA 조용 창(qa-quiet 파일)은 boardnet.qa_quiet() — push_usage 와 공유한다.

DEFAULT_BOARD_URL = "http://192.0.2.10/usage"
POST_TIMEOUT_S = 3          # 보드 첫 TCP 연결이 1.6초쯤 걸린다(절전). 5초는 과하다.
BACKOFF_S = 60              # 전송 실패 후 이만큼 재시도 안 함
MIN_CTX_PUSH_INTERVAL_S = 3.0  # state 가 같고 ctx 만 바뀔 때의 최소 전송 간격
LOG_MAX_BYTES = 512 * 1024

# 집계 우선순위. 앞이 이긴다. idle 은 상태 파일이 없거나 다 삭아 없어졌을 때다.
PRIORITY = ["permission", "error", "juggling", "working", "thinking", "done"]

# 집계 시 낡은 세션에 적용하는 감쇠(초). 펌웨어의 복귀 규칙과 맞춰 둔다.
# 세션이 SessionEnd 없이 죽어도(터미널 강제 종료 등) 화면이 그 상태에 영원히
# 붙들리지 않게 하는 안전판이기도 하다.
DECAY_S = {
    "error": 5,
    "done": 8,
    "working": 60,
    "thinking": 60,
    # 권한 대기·저글링은 원래 이벤트로만 풀리지만, 죽은 세션 보험으로 30분.
    "permission": 30 * 60,
    "juggling": 30 * 60,
}

STALE_SESSION_S = 24 * 3600   # 이보다 오래된 세션 파일은 지운다
SUBAGENT_MAX_AGE_S = 30 * 60  # SubagentStop 이 유실된 시작 기록의 수명


def log(msg: str) -> None:
    try:
        if LOG_PATH.exists() and LOG_PATH.stat().st_size > LOG_MAX_BYTES:
            LOG_PATH.write_text("", encoding="utf-8")
        with LOG_PATH.open("a", encoding="utf-8") as f:
            f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}\n")
    except OSError:
        pass


def ensure_state_dir() -> bool:
    try:
        STATE_DIR.mkdir(parents=True, exist_ok=True, mode=0o700)
        return STATE_DIR.is_dir()
    except OSError as e:
        log(f"상태 폴더 준비 실패: {e}")
        return False


def ascii_only(s: str, limit: int = 28) -> str:
    """보드 폰트가 한글을 못 그린다. ASCII 외는 버리고 길이도 자른다."""
    out = "".join(c for c in s if 32 <= ord(c) < 127)
    out = " ".join(out.split())
    return out[:limit]


# --- 훅 이벤트 → 세션 상태 -------------------------------------------------


def session_path(session_id: str) -> Path:
    safe = "".join(c for c in session_id if c.isalnum() or c in "-_")[:64]
    return STATE_DIR / f"{safe or 'unknown'}.json"


def read_json(path: Path) -> dict:
    try:
        d = json.loads(path.read_text(encoding="utf-8"))
        return d if isinstance(d, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def write_json(path: Path, d: dict) -> None:
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(d), encoding="utf-8")
    tmp.replace(path)


def live_subagents(s: dict, now: float) -> list[float]:
    """세션 파일의 서브에이전트 시작 시각 목록. 너무 낡은 기록은 버린다."""
    raw = s.get("subagent_starts")
    if not isinstance(raw, list):
        return []
    out: list[float] = []
    for v in raw:
        if isinstance(v, (int, float)) and now - v < SUBAGENT_MAX_AGE_S:
            out.append(float(v))
    return out


# Codex 세션 로그의 function_call_output 이 남기는 exit code 표기.
_CODEX_EXIT_RE = re.compile(r"Process exited with code (\d+)")
CODEX_TAIL_BYTES = 64 * 1024
CODEX_TAIL_TRIES = 4          # 훅이 로그보다 먼저 뜬다(21ms 실측). 짧게 기다려 본다
CODEX_TAIL_WAIT_S = 0.15


def is_codex_payload(payload: dict) -> bool:
    tp = str(payload.get("transcript_path", "") or "")
    return "/.codex/" in tp or "\\.codex\\" in tp


def codex_tool_failed(payload: dict) -> bool:
    """Codex PostToolUse 가 실패한 도구 호출인지. 모르면 False(오탐 없음).

    transcript_path(rollout jsonl) 꼬리에서 이 tool_use_id(=call_id) 의
    function_call_output 줄을 찾아 exit code 가 0 이 아니면 True.
    """
    if not is_codex_payload(payload):
        return False
    call_id = str(payload.get("tool_use_id", "") or "")
    tp = str(payload.get("transcript_path", "") or "")
    if not call_id or not tp:
        return False
    needle = f'"call_id":"{call_id}"'
    for attempt in range(CODEX_TAIL_TRIES):
        try:
            with open(tp, "rb") as f:
                f.seek(0, os.SEEK_END)
                size = f.tell()
                f.seek(max(0, size - CODEX_TAIL_BYTES))
                tail = f.read().decode("utf-8", errors="replace")
        except OSError:
            return False
        for line in reversed(tail.splitlines()):
            if needle in line and '"function_call_output"' in line:
                m = _CODEX_EXIT_RE.search(line)
                return bool(m and m.group(1) != "0")
        if attempt + 1 < CODEX_TAIL_TRIES:
            time.sleep(CODEX_TAIL_WAIT_S)
    return False


def apply_event(payload: dict, event_ts: float) -> None:
    """훅 페이로드 하나를 세션 파일에 반영한다."""
    event = payload.get("hook_event_name", "")
    session_id = str(payload.get("session_id", "unknown") or "unknown")
    path = session_path(session_id)
    s = read_json(path)

    # 전송 대기열이 밀렸을 때 낡은 이벤트가 최신 상태를 덮지 않게 (리뷰 MEDIUM-7)
    prev_at = s.get("at")
    if isinstance(prev_at, (int, float)) and event_ts < prev_at:
        return

    now = event_ts
    subagents = live_subagents(s, now)
    # 누구의 세션인지 (docs/agent-badge.md §4). Codex 는 transcript_path 로 구분.
    s["agent"] = "codex" if is_codex_payload(payload) else "claude"
    tool = ascii_only(str(payload.get("tool_name", "") or ""))
    if tool:
        s["last_tool"] = tool
    else:
        # SubagentStop 등 도구 이름이 없는 이벤트에서는 마지막 도구를 이어 쓴다
        tool = str(s.get("last_tool", "") or "")

    if event == "SessionEnd":
        try:
            path.unlink()
        except OSError:
            pass
        return

    if event == "SessionStart":
        state, ctx = "idle", ""
        subagents = []
    elif event == "UserPromptSubmit":
        state, ctx = "thinking", "reading prompt"
        s["prompt_started"] = now
    elif event == "PostToolUse" and codex_tool_failed(payload):
        # Codex 에는 PostToolUseFailure 가 없다 — 세션 로그의 exit code 로 대신
        state, ctx = "error", tool or "tool failed"
    elif event in ("PreToolUse", "PostToolUse"):
        # 서브에이전트가 도는 동안의 도구 이벤트는 저글링을 유지한다
        state = "juggling" if subagents else "working"
        ctx = f"{len(subagents)} agents" if subagents else tool
    elif event == "PostToolUseFailure":
        state, ctx = "error", tool or "tool failed"
    elif event == "SubagentStart":
        subagents.append(now)
        state, ctx = "juggling", f"{len(subagents)} agents"
    elif event == "SubagentStop":
        if subagents:
            subagents.pop(0)
        if subagents:
            state, ctx = "juggling", f"{len(subagents)} agents"
        else:
            # 마지막 서브에이전트가 끝났다. 부모가 이미 done/permission 이면
            # 그 상태를 되살리지 않고 그대로 둔다 (리뷰 HIGH-2).
            cur = str(s.get("state", "idle"))
            if cur == "juggling":
                state, ctx = "working", tool
            else:
                state, ctx = cur, str(s.get("ctx", ""))
    elif event == "PermissionRequest":
        # 권한 대기의 정확한 신호원. Notification 의 문구 추측보다 우선한다.
        state, ctx = "permission", (f"{tool} approve?" if tool else "approve?")
    elif event == "Notification":
        # 권한 요청 알림만 permission 으로. "waiting for your input" 같은
        # 유휴 알림을 permission 으로 잡으면 최상위 우선순위 + 30분 감쇠라
        # 다른 세션의 작업 표시를 30분간 덮어버린다 (리뷰 HIGH-1 실측).
        msg = str(payload.get("message", "") or "")
        low = msg.lower()
        if "permission" in low or "approv" in low:
            # 문구를 그대로 보내면 두 줄로 접혀 캐릭터를 가린다. 도구 이름만
            # 뽑아 "Bash approve?" 로 줄인다 ("... to use Bash" 꼴).
            tool_from_msg = ""
            if " to use " in low:
                rest = msg[low.index(" to use ") + len(" to use ") :].split()
                tool_from_msg = ascii_only(rest[0]) if rest else ""
            state, ctx = "permission", (f"{tool_from_msg} approve?" if tool_from_msg else "approve?")
        else:
            return  # 상태 변경 없음
    elif event == "Stop":
        started = s.get("prompt_started")
        elapsed = ""
        if isinstance(started, (int, float)) and now > started:
            m, sec = divmod(int(now - started), 60)
            elapsed = f"turn {m}m {sec}s" if m else f"turn {sec}s"
        state, ctx = "done", elapsed
        # subagent_starts 는 건드리지 않는다 — 백그라운드 서브에이전트는
        # 부모 턴이 끝나도 살아 있고, 여기서 리셋하면 나중의 SubagentStop
        # 이 어긋난다 (리뷰 HIGH-2).
        s["prompt_started"] = None
    else:
        return  # 모르는 이벤트는 무시

    s.update({"state": state, "ctx": ctx, "at": now, "subagent_starts": subagents})
    write_json(path, s)


# --- 집계 ------------------------------------------------------------------


def decayed(state: str, age: float) -> str:
    """호스트 쪽 감쇠. 펌웨어 복귀 규칙의 안전판 버전."""
    limit = DECAY_S.get(state)
    if limit is None or age <= limit:
        return state
    if state == "error":
        return decayed("working", age - limit)
    return "idle"


def aggregate(selected: str | None = None) -> tuple[str, str, str]:
    """전 세션 파일 → (state, ctx, agent) 하나.

    selected(보드 선택, 기본은 캐시)가 claude|codex 면 그 에이전트 세션만 본다 —
    Claude 가 permission 으로 박혀 있어도 Codex 를 고른 보드는 Codex 상태를 받는다.
    "both" 는 선택을 모른다는 뜻(옛 펌웨어·캐시 없음·TTL 만료)이라 전부 본다.
    agent 는 이긴 세션의 것("claude"|"codex"), idle 이면 빈 문자열. agent 키가 없는
    옛 세션 파일은 claude 로 친다.
    """
    if selected is None:
        selected = read_agent_selected()
    now = time.time()
    best_rank = len(PRIORITY)
    best: tuple[str, str, str] | None = None
    best_at = 0.0
    for p in STATE_DIR.glob("*.json"):
        if p.name in (LAST_PUSH.name, DESIRED.name) or p.name.startswith(CELLS_CACHE_PREFIX):
            continue   # 세션 파일이 아닌 것(desired·last-push·에이전트별 cells 캐시)
        s = read_json(p)
        at = s.get("at")
        if not isinstance(at, (int, float)):
            continue
        if now - at > STALE_SESSION_S:
            try:
                p.unlink()
            except OSError:
                pass
            continue
        agent = str(s.get("agent", "claude") or "claude")
        if selected != "both" and agent != selected:
            continue
        state = decayed(str(s.get("state", "idle")), now - at)
        if state == "idle" or state not in PRIORITY:
            continue
        rank = PRIORITY.index(state)
        # 같은 순위면 더 최근 이벤트의 ctx 를 보여준다
        if rank < best_rank or (rank == best_rank and at > best_at):
            best_rank = rank
            best = (state, str(s.get("ctx", "")), agent)
            best_at = at
    return best or ("idle", "", "")


def aggregate_to_desired() -> None:
    """집계해서 desired.json 에 적는다. LOCK_FILE 안에서 부를 것."""
    state, ctx, agent = aggregate()
    write_json(DESIRED, {"state": state, "ctx": ctx, "agent": agent})


# --- 전송 (단일 비행) -------------------------------------------------------


def board_url() -> str:
    env = os.environ.get("CLAUDEBOT_BOARD_URL")
    if env:
        return env
    cfg = HERE / "board.json"
    try:
        url = json.loads(cfg.read_text(encoding="utf-8")).get("url")
        if url:
            return url
    except (OSError, json.JSONDecodeError) as e:
        log(f"board.json 읽기 실패: {e}")
    return DEFAULT_BOARD_URL


def in_backoff(now: float) -> bool:
    try:
        return now - BACKOFF_FILE.stat().st_mtime < BACKOFF_S
    except OSError:
        return False


def post_once(state: str, ctx: str, agent: str = "") -> bytes | None:
    """보드로 한 번. 성공이면 응답 본문(옛 펌웨어 b"ok"), 실패면 None.

    실패하면 boardnet 이 ARP 를 깨우고 한 번 더 보낸다(절전 중인 보드가 ARP 를
    놓쳐 한 시간 넘게 연속 실패했던 2026-08-29 사고의 보험).
    """
    payload: dict = {"state": state, "ctx": ctx}
    if agent:
        payload["agent"] = agent
    try:
        return post_json(board_url(), payload, POST_TIMEOUT_S, log, ensure_ascii=True)
    except (urllib.error.URLError, OSError) as e:
        log(f"전송 실패(보드 꺼짐/주소 변경 가능): {e}")
        return None


def push_loop() -> None:
    """PUSH_LOCK 을 쥔 자식 하나만 들어온다. desired 가 잠잠해질 때까지 민다."""
    now = time.time()
    if in_backoff(now):
        return
    force = False   # 선택 전환 직후의 메우기 push 는 완충을 건너뛴다 (한 바퀴만)
    for _ in range(20):  # 폭주 보험. 정상적으로는 한두 바퀴에 끝난다.
        desired = read_json(DESIRED)
        last = read_json(LAST_PUSH)
        state = str(desired.get("state", "idle"))
        ctx = str(desired.get("ctx", ""))
        agent = str(desired.get("agent", "") or "")
        # agent 도 비교한다 — 같은 state 라도 에이전트가 바뀌면 배지 갱신을 위해 보낸다.
        if state == last.get("state") and ctx == last.get("ctx") and agent == str(last.get("agent", "") or ""):
            return
        now = time.time()
        if state == last.get("state") and not force:
            # state 는 같고 ctx·agent 만 다르다 — 세션 여러 개가 도구를 돌리면 ctx 가
            # 초당 몇 번씩 뒤집혀 화면도 못 읽고 보드만 두들긴다 (리뷰 MEDIUM-4).
            # BOTH 에서 Claude·Codex 가 동시에 working 이면 배지도 같은 꼴로 뒤집힌다
            # (2026-09-06 E2E 실측: 초당 2회) — 같은 3초 완충을 탄다. 상태가 바뀌면 즉시.
            last_at = last.get("at")
            if isinstance(last_at, (int, float)) and now - last_at < MIN_CTX_PUSH_INTERVAL_S:
                return
        body = post_once(state, ctx, agent)
        if body is None:
            try:
                BACKOFF_FILE.write_text(str(now), encoding="utf-8")
            except OSError:
                pass
            return  # last-push 를 안 남겨 백오프 뒤 재시도되게 둔다
        # 보드가 알려 준 선택이 캐시와 다르면(배지 탭으로 바꿈) 그 선택으로 다시
        # 집계해 한 바퀴 더 — 전환 직후 고른 쪽의 마지막 상태를 메운다 (§6). 그 한 바퀴는
        # 같은 state 라도 완충(3초)을 건너뛴다(force).
        selected, changed = remember_agent_selected(body, log)
        # 보드가 걸러낸 push(선택 ≠ agent, 200 이지만 화면엔 안 반영)는 "보드가 보여주는
        # 것" 으로 기록하지 않는다 — 나중에 같은 값이 와도 다시 보내야 한다.
        filtered = bool(agent) and selected != "both" and agent != selected
        if not filtered:
            write_json(LAST_PUSH, {"state": state, "ctx": ctx, "agent": agent, "at": now})
        force = changed
        if changed:
            # 한도·사용량 칸도 같이 메운다 — 고른 쪽의 마지막 cells 캐시를 한 번 (§11).
            # 캐시가 없으면 조용히 건너뛴다(다음 Stop 훅이 채운다).
            if selected in ("claude", "codex"):
                resend_cells(selected, board_url(), POST_TIMEOUT_S, log)
            with LOCK_FILE.open("w") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX)
                aggregate_to_desired()


def run_worker(payload_path: str) -> int:
    try:
        raw = json.loads(Path(payload_path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        log(f"페이로드 읽기 실패: {e}")
        return 0
    finally:
        try:
            os.unlink(payload_path)
        except OSError:
            pass

    payload = raw.get("payload") if isinstance(raw, dict) else None
    event_ts = raw.get("ts") if isinstance(raw, dict) else None
    if not isinstance(payload, dict):
        payload = raw if isinstance(raw, dict) else {}
    if not isinstance(event_ts, (int, float)):
        event_ts = time.time()

    if not ensure_state_dir():
        return 0
    try:
        # 상태 갱신과 집계만 잠근다. 전송은 락 밖 — 보드가 죽었을 때 이벤트마다
        # 타임아웃이 직렬화되어 자식이 무한히 쌓이는 것을 막는다 (리뷰 HIGH-3).
        with LOCK_FILE.open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            apply_event(payload, float(event_ts))
            aggregate_to_desired()

        with PUSH_LOCK.open("w") as plock:
            try:
                fcntl.flock(plock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                return 0  # 다른 푸셔가 desired 최신값을 마저 민다
            push_loop()

        # 옷장 언락 판정 + 미전달분 전송 (docs/wardrobe.md). 상태 전송과 별개라
        # 실패해도 상태 표시는 안 다친다. 보드 백오프 중이면 전송은 건너뛴다.
        selection_changed = False
        try:
            import wardrobe_rules
            def post_cb(p: dict) -> None:
                nonlocal selection_changed
                if in_backoff(time.time()):
                    raise OSError("보드 백오프 중")
                sel, ch = remember_agent_selected(post_json(board_url(), p, POST_TIMEOUT_S, log), log)
                selection_changed = selection_changed or ch
                if ch and sel in ("claude", "codex"):
                    resend_cells(sel, board_url(), POST_TIMEOUT_S, log)   # §11 전환 메우기

            wardrobe_rules.process(payload, float(event_ts), post_cb, log)
        except Exception as e:  # noqa: BLE001
            log(f"옷장 판정 오류: {type(e).__name__}: {e}")
        if selection_changed:
            # 언락 전송 응답으로 선택이 바뀐 걸 알았다 — 상태도 그 선택으로 다시 민다.
            with LOCK_FILE.open("w") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX)
                aggregate_to_desired()
            with PUSH_LOCK.open("w") as plock:
                try:
                    fcntl.flock(plock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except OSError:
                    return 0
                push_loop()
    except Exception as e:  # noqa: BLE001 — 훅은 무슨 일이 있어도 조용히 죽는다
        log(f"오류: {type(e).__name__}: {e}")
    return 0


def main() -> int:
    try:
        if len(sys.argv) > 2 and sys.argv[1] == "--worker":
            return run_worker(sys.argv[2])
        if qa_quiet():
            return 0   # QA 조용 창 — 상태 기록·전송 전부 생략

        raw = sys.stdin.read()
        try:
            payload = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            payload = {}
        if not ensure_state_dir():
            return 0
        # 이벤트 발생 시각을 부모가 찍는다 — 자식이 락을 기다리다 늦게 처리해도
        # 순서 판정이 가능하다 (리뷰 MEDIUM-7).
        wrapped = {"ts": time.time(), "payload": payload}
        tmp = Path(f"/tmp/claudebot-statehook-{os.getpid()}-{int(time.time()*1000)}.json")
        tmp.write_text(json.dumps(wrapped), encoding="utf-8")
        subprocess.Popen(
            [sys.executable, str(Path(__file__).resolve()), "--worker", str(tmp)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,  # 부모가 죽어도 살아남는다
        )
    except Exception as e:  # noqa: BLE001 — 훅에 트레이스백을 절대 보이지 않는다
        log(f"부모 오류: {type(e).__name__}: {e}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
