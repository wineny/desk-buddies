#!/usr/bin/env python3
"""클로드봇 훅을 Codex CLI 에 등록한다 (~/.codex/hooks.json 병합).

Codex CLI 0.153.2+ 는 Claude Code 와 같은 이름·같은 페이로드의 라이프사이클 훅을
~/.codex/hooks.json 에서 읽는다 (2026-09-06 실측). 이 스크립트는 그 파일에
claude_state.py(상태 9종) 와 push_usage.py(Stop) 를 **덧붙인다** — 기존 항목은
건드리지 않고, 이미 들어 있으면 중복 추가하지 않는다. 원본은 옆에 백업한다.

    python3 install_hooks.py                 # ~/.codex/hooks.json 에 등록
    python3 install_hooks.py --remove        # 우리 항목만 제거
    python3 install_hooks.py --project DIR   # DIR/.codex/hooks.json 에 (QA·리허설용)
    python3 install_hooks.py --dry-run       # 결과만 출력

Codex 는 새 훅을 **신뢰(trust) 하기 전엔 실행하지 않는다.** 신뢰는 config.toml 의
`[hooks.state."<hooks.json 절대경로>:<event_snake>:<group>:<index>"] trusted_hash`
에 해시로 박히는데, 이 스크립트는 그 해시를 **직접 계산해 써넣는다**(기본 동작,
`--no-trust` 로 끔) — 자기 스크립트를 자기가 설치하는 것이니 TUI 리뷰를 거칠 이유가
없다. 해시 규칙은 codex-rs `hooks/src/engine/discovery.rs::hook_hash` +
`config/src/fingerprint.rs::version_for_toml` 을 옮긴 것으로, 2026-09-06 에 TUI 가
기록한 trusted_hash 3개와 바이트 단위로 일치함을 확인했다:

    identity = {"event_name": "post_tool_use", ["matcher": …,] "hooks": [handler]}
    handler  = {"type": "command", "command": …, ["timeout": N,] "async": false}
    hash     = sha256( json.dumps(키 정렬, 구분자 ',' ':' ) )

TUI 로 하고 싶으면 `codex` → "Hooks need review" → Review 에서 `host` 항목만 t.
목록엔 원래 미신뢰였던 다른 훅도 같이 뜨니 "Trust all" 은 누르지 말 것. 비대화형
QA 는 `codex exec --dangerously-bypass-hook-trust`.

Claude Code 쪽 설치는 README "훅 설치" 절대로 settings.json 을 손으로 고친다 —
statusLine 을 tee 로 감싸는 단계가 있어 자동 병합이 위험해서 여기 넣지 않았다.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shlex
import shutil
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from hook_python import pick_python  # noqa: E402
STATE_SCRIPT = HERE / "claude_state.py"
USAGE_SCRIPT = HERE / "push_usage.py"

# Codex 가 내놓는 이벤트 중 상태 집계에 쓰는 것. PostToolUseFailure·Notification 은
# Codex 에 없다(PreCompact/PostCompact 는 있지만 상태와 무관).
STATE_EVENTS = (
    "SessionStart", "SessionEnd", "UserPromptSubmit", "PreToolUse", "PostToolUse",
    "PermissionRequest", "SubagentStart", "SubagentStop", "Stop",
)
HOOK_TIMEOUT_S = 5
SESSION_END_TIMEOUT_S = 3   # Codex 가 SessionEnd 는 3초로 클램프하며 경고를 띄운다 (실측)


def hook_entry(script: Path, python: str, event: str) -> dict:
    # 부모는 자식을 떼어놓고 즉시 끝나므로 async 가 없어도 Codex 를 막지 않는다.
    return {"hooks": [{"type": "command",
                       "command": f'{shlex.quote(python)} "{script}"',
                       "timeout": SESSION_END_TIMEOUT_S if event == "SessionEnd" else HOOK_TIMEOUT_S}]}


def is_ours(entry: dict) -> bool:
    for h in entry.get("hooks", []) or []:
        cmd = str(h.get("command", ""))
        if STATE_SCRIPT.name in cmd and "host" in cmd:
            return True
        if USAGE_SCRIPT.name in cmd and "host" in cmd:
            return True
    return False


def load(path: Path) -> dict:
    if not path.is_file():
        return {"hooks": {}}
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise SystemExit(f"{path}: 최상위가 객체가 아니다")
    data.setdefault("hooks", {})
    return data


def merge(data: dict, python: str) -> tuple[dict, int]:
    """없으면 덧붙이고, 있으면 우리 줄의 명령(파이썬·경로)만 새것으로 바꾼다. 바꾼 개수를 돌려준다.

    줄을 제자리에서 고치므로 trust 키(<group>:<index>)는 그대로고 해시만 새로 계산된다.
    중복을 지우면 뒤 묶음의 번호가 당겨지지만 trust 키는 merge 뒤에 다시 계산하므로 맞는다.
    """
    hooks = data["hooks"]
    changed = 0
    want = [(ev, STATE_SCRIPT) for ev in STATE_EVENTS] + [("Stop", USAGE_SCRIPT)]
    for ev, script in want:
        lst = hooks.setdefault(ev, [])
        new = hook_entry(script, python, ev)["hooks"][0]
        mine = [h for e in lst if is_ours(e) for h in (e.get("hooks") or [])
                if script.name in str(h.get("command", "")) and "host" in str(h.get("command", ""))]
        if not mine:
            lst.append({"hooks": [new]})
            changed += 1
            continue
        first, dups = mine[0], mine[1:]
        if any(first.get(k) != v for k, v in new.items()):
            first.clear()
            first.update(new)                  # 명령·timeout 을 새것으로(제자리)
            changed += 1
        for e in lst:                          # 다른 폴더에서 깐 중복은 그 한 줄만 지운다
            hs = e.get("hooks") or []
            if any(h is d for h in hs for d in dups):
                e["hooks"] = [h for h in hs if all(h is not d for d in dups)]
        if dups:
            lst[:] = [e for e in lst if e.get("hooks")]
            changed += len(dups)
    return data, changed


def remove(data: dict) -> tuple[dict, int]:
    removed = 0
    for ev, lst in list(data["hooks"].items()):
        keep = [e for e in lst if not is_ours(e)]
        removed += len(lst) - len(keep)
        if keep:
            data["hooks"][ev] = keep
        else:
            del data["hooks"][ev]
    return data, removed


# ---- Codex trust 해시 (codex-rs discovery.rs::hook_hash 포팅) ----------------

EVENT_LABEL = {
    "PreToolUse": "pre_tool_use", "PermissionRequest": "permission_request",
    "PostToolUse": "post_tool_use", "PreCompact": "pre_compact", "PostCompact": "post_compact",
    "SessionStart": "session_start", "SessionEnd": "session_end",
    "UserPromptSubmit": "user_prompt_submit", "SubagentStart": "subagent_start",
    "SubagentStop": "subagent_stop", "Stop": "stop", "Interrupt": "interrupt",
}


def _canon(v):
    if isinstance(v, dict):
        return {k: _canon(v[k]) for k in sorted(v)}
    if isinstance(v, list):
        return [_canon(x) for x in v]
    return v


def hook_trust_hash(event: str, group: dict, handler: dict) -> str:
    """TUI 가 trust 때 기록하는 값과 동일한 sha256:… 문자열.

    Rust 쪽은 Option::None 을 TOML 로 못 옮기므로 없는 필드는 빠지고, `async` 는
    bool 이라 항상 들어간다. command 타입만 지원한다(우리 훅은 전부 command).
    """
    if handler.get("type", "command") != "command":
        raise ValueError("command 타입 훅만 해시할 수 있다")
    h = {"type": "command", "command": handler["command"], "async": bool(handler.get("async", False))}
    for src, dst in (("timeout", "timeout"), ("statusMessage", "statusMessage"),
                     ("commandWindows", "commandWindows"), ("additionalContextLimit", "additionalContextLimit")):
        if handler.get(src) is not None:
            h[dst] = handler[src]
    ident = {"event_name": EVENT_LABEL[event], "hooks": [h]}
    if group.get("matcher") is not None:
        ident["matcher"] = group["matcher"]
    raw = json.dumps(_canon(ident), separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return "sha256:" + hashlib.sha256(raw).hexdigest()


def our_hook_keys(data: dict, hooks_path: Path) -> list[tuple[str, str]]:
    """(config 키, 해시) — 우리 항목 전부. 키는 <hooks.json 절대경로>:<label>:<group>:<index>."""
    out = []
    for ev, lst in data["hooks"].items():
        for gi, entry in enumerate(lst):
            if not is_ours(entry):
                continue
            for hi, h in enumerate(entry.get("hooks", [])):
                out.append((f"{hooks_path}:{EVENT_LABEL[ev]}:{gi}:{hi}", hook_trust_hash(ev, entry, h)))
    return out


def write_trust(config_path: Path, keys: list[tuple[str, str]], dry_run: bool) -> int:
    """config.toml 의 [hooks.state."…"] 블록을 추가/갱신한다. 다른 내용은 건드리지 않는다."""
    text = config_path.read_text(encoding="utf-8") if config_path.is_file() else ""
    changed = 0
    for key, digest in keys:
        pat = re.compile(r'(\[hooks\.state\."' + re.escape(key) + r'"\]\n(?:(?!\[)[^\n]*\n)*?)trusted_hash = "[^"]*"')
        m = pat.search(text)
        if m:
            if f'trusted_hash = "{digest}"' in m.group(0):
                continue
            text = text[:m.start()] + m.group(1) + f'trusted_hash = "{digest}"' + text[m.end():]
        else:
            if not text.endswith("\n"):
                text += "\n"
            text += f'\n[hooks.state."{key}"]\ntrusted_hash = "{digest}"\n'
        changed += 1
    if changed and not dry_run:
        if config_path.is_file():
            bak = config_path.with_name(f"{config_path.name}.claudebot-bak-{time.strftime('%Y%m%d-%H%M%S')}")
            shutil.copy2(config_path, bak)
            print(f"백업: {bak}")
        config_path.write_text(text, encoding="utf-8")
    return changed


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--project", metavar="DIR", help="DIR/.codex/hooks.json 에 쓴다 (기본: ~/.codex/hooks.json)")
    ap.add_argument("--python", help="훅 명령의 파이썬 (기본: 보드에 접속되는 것을 골라 절대 경로로, hook_python.py)")
    ap.add_argument("--remove", action="store_true", help="우리 항목만 제거")
    ap.add_argument("--dry-run", action="store_true", help="파일을 쓰지 않고 결과만 출력")
    ap.add_argument("--no-trust", action="store_true", help="config.toml 에 trust 해시를 쓰지 않는다 (TUI 에서 수동 trust)")
    ap.add_argument("--config", metavar="TOML", help="trust 를 기록할 config.toml (기본 ~/.codex/config.toml)")
    a = ap.parse_args()

    for s in (STATE_SCRIPT, USAGE_SCRIPT):
        if not s.is_file():
            raise SystemExit(f"스크립트가 없다: {s}")

    target = (Path(a.project).resolve() / ".codex" / "hooks.json") if a.project \
        else Path.home() / ".codex" / "hooks.json"
    data = load(target)
    if not a.remove and not a.python:
        a.python, why = pick_python()
        print(f"  훅 파이썬: {a.python} — {why}")
    data, n = remove(data) if a.remove else merge(data, a.python)

    config_path = Path(a.config).expanduser() if a.config else Path.home() / ".codex" / "config.toml"
    trust_keys = [] if (a.remove or a.no_trust) else our_hook_keys(data, target)

    if a.dry_run:
        json.dump(data, sys.stdout, indent=2, ensure_ascii=False)
        print(f"\n[dry-run] {'제거' if a.remove else '추가·갱신'} {n}개 → {target}")
        for k, d in trust_keys:
            print(f"[dry-run] trust {k} = {d[:23]}…")
        return 0

    if n:
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.is_file():
            bak = target.with_name(f"{target.name}.claudebot-bak-{time.strftime('%Y%m%d-%H%M%S')}")
            shutil.copy2(target, bak)
            print(f"백업: {bak}")
        target.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"{'제거' if a.remove else '추가·갱신'} {n}개 → {target}")
    else:
        print(f"hooks.json 변경 없음 — 이미 {'없다' if a.remove else '등록돼 있다'}: {target}")

    if trust_keys:
        t = write_trust(config_path, trust_keys, dry_run=False)
        print(f"trust 기록 {t}개 (총 {len(trust_keys)}개 항목) → {config_path}")
        print("\n확인: Codex 로 아무 작업이나 한 뒤 tail /tmp/claudebot-state.log /tmp/claudebot-push.log")
    elif not a.remove:
        print("\n다음: `codex` 를 띄우고 Review 에서 host 항목만 trust 하라 (--no-trust 였음).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
