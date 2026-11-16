#!/usr/bin/env python3
"""클로드봇 훅을 Claude Code 설정(settings.json)에 등록한다.

    python3 install_claude.py                       # $CLAUDE_CONFIG_DIR 또는 ~/.claude
    python3 install_claude.py --config-dir ~/.claude-work
    python3 install_claude.py --remove              # 우리 항목만 뺀다
    python3 install_claude.py --dry-run             # 결과만 출력

하는 일(기존 내용은 지우지 않고, 쓰기 전에 옆에 백업한다):
- 상태 훅 11개(claude_state.py, async)와 사용량 훅(push_usage.py, Stop)을 덧붙인다.
  다른 폴더를 가리키는 클로드봇 훅이 이미 있으면 이 폴더 것으로 경로만 바꾼다.
- 한도(주간 %)는 statusline 입력에만 오므로 statusLine 명령 앞에 tee 를 붙여
  /tmp/claudebot-statusline….json 에 떨군다(account.STATUSLINE_TEE_PATH, 설정 폴더마다 따로).
  원래 statusline 은 그대로 돈다. 없으면 화면에 아무것도 안 찍는 tee 만 건다.
  statusLine 이 부르는 .sh 안에서 이미 tee 를 하면 감싸지 않는다. 그 tee 가 옛 한 파일 경로면 고치라고 알린다.
- 훅 명령의 파이썬은 `python3` 대신 보드에 실제로 접속되는 파이썬의 절대 경로로 박는다(hook_python.py).
  python.org 설치판은 맥 로컬 네트워크 권한에 막혀 보드 전송이 실패한다. 다시 돌리면 기존 줄도 바뀐다.
- git 워크트리(작업용 복사본)에서는 설치를 거절한다 — 워크트리를 지우면 훅이 깨진다.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from account import STATUSLINE_TEE_PATH  # noqa: E402
from hook_python import pick_python  # noqa: E402

STATE_SCRIPT = HERE / "claude_state.py"
USAGE_SCRIPT = HERE / "push_usage.py"
STATE_EVENTS = (
    "SessionStart", "SessionEnd", "UserPromptSubmit", "PreToolUse", "PostToolUse",
    "PostToolUseFailure", "PermissionRequest", "Notification", "SubagentStart", "SubagentStop", "Stop",
)
TIMEOUT_S = 5
TEE_PREFIX = f'tee "{STATUSLINE_TEE_PATH}"'
LEGACY_TEE = "tee /tmp/claudebot-statusline.json"


def default_config_dir() -> Path:
    raw = os.environ.get("CLAUDE_CONFIG_DIR", "").strip()
    return Path(raw).expanduser() if raw else Path.home() / ".claude"


def entry(script: Path, python: str, is_async: bool) -> dict:
    h = {"type": "command", "command": f'{shlex.quote(python)} "{script}"', "timeout": TIMEOUT_S}
    if is_async:
        h["async"] = True
    return {"hooks": [h]}


def which_script_cmd(cmd: str) -> str | None:
    """우리 훅 명령이면 스크립트 이름(claude_state.py / push_usage.py), 아니면 None."""
    if "host" in cmd:
        for s in (STATE_SCRIPT, USAGE_SCRIPT):
            if s.name in cmd:
                return s.name
    return None


def which_script(e: dict) -> str | None:
    """묶음(entry) 안에 우리 훅이 있으면 그 스크립트 이름."""
    for h in e.get("hooks", []) or []:
        name = which_script_cmd(str(h.get("command", "")))
        if name:
            return name
    return None


def merge_hooks(data: dict, python: str) -> list[str]:
    """우리 훅 한 줄만 고친다 — 같은 묶음에 든 남의 훅·matcher 는 그대로 둔다."""
    hooks = data.setdefault("hooks", {})
    notes = []
    want = [(ev, STATE_SCRIPT, True) for ev in STATE_EVENTS] + [("Stop", USAGE_SCRIPT, False)]
    for ev, script, is_async in want:
        lst = hooks.setdefault(ev, [])
        new = entry(script, python, is_async)["hooks"][0]
        found = [(e, h) for e in lst for h in (e.get("hooks") or []) if which_script_cmd(str(h.get("command", ""))) == script.name]
        if not found:
            lst.append({"hooks": [new]})
            notes.append(f"추가 {ev} {script.name}")
            continue
        if [h for _, h in found] == [new]:
            continue
        e0, h0 = found[0]
        e0["hooks"][e0["hooks"].index(h0)] = new
        for e, h in found[1:]:                 # 다른 폴더에서 깐 중복 — 그 한 줄만 지운다
            e["hooks"].remove(h)
        lst[:] = [e for e in lst if e.get("hooks")]
        notes.append(f"경로 갱신 {ev} {script.name}" + (f" (중복 {len(found) - 1}개 정리)" if len(found) > 1 else ""))
    return notes


def remove_hooks(data: dict) -> list[str]:
    notes = []
    for ev, lst in list(data.get("hooks", {}).items()):
        n = 0
        for e in lst:
            keep = [h for h in (e.get("hooks") or []) if which_script_cmd(str(h.get("command", ""))) is None]
            n += len(e.get("hooks") or []) - len(keep)
            e["hooks"] = keep
        if n:
            notes.append(f"제거 {ev} {n}개")
            lst[:] = [e for e in lst if e.get("hooks")]
            if not lst:
                del data["hooks"][ev]
    if data.get("hooks") == {}:
        del data["hooks"]                      # 우리가 만든 빈 칸은 남기지 않는다
    return notes


def called_scripts(cmd: str, config_dir: Path) -> list[Path]:
    """statusLine 명령이 부르는 .sh 파일들(설정 폴더·홈 변수를 풀어서)."""
    home = str(Path.home())
    out = []
    for tok in re.findall(r"[^\s'\"]+\.sh", cmd):
        tok = (tok.replace("${CLAUDE_CONFIG_DIR:-$HOME/.claude}", str(config_dir))
                  .replace("$CLAUDE_CONFIG_DIR", str(config_dir)).replace("${HOME}", home)
                  .replace("$HOME", home))
        p = Path(tok).expanduser()
        if p.is_file():
            out.append(p)
    return out


def is_default_dir(config_dir: Path) -> bool:
    """기본 설정 폴더(변수 없이 쓰는 ~/.claude)면 옛 한 파일 경로가 곧 그 계정 경로다."""
    return not os.environ.get("CLAUDE_CONFIG_DIR", "").strip() and \
        config_dir.expanduser().resolve() == (Path.home() / ".claude").resolve()


def wrap_statusline(data: dict, config_dir: Path) -> list[str]:
    sl = data.get("statusLine")
    cmd = sl.get("command", "") if isinstance(sl, dict) else ""
    if TEE_PREFIX in cmd:
        return []
    for script in called_scripts(cmd, config_dir):
        text = script.read_text(encoding="utf-8", errors="replace")
        if STATUSLINE_TEE_PATH in text:
            return [f"statusLine: {script} 안에서 이미 계정별 tee 를 함 — 그대로 둠"]
        if "claudebot-statusline" in text and is_default_dir(config_dir):
            return [f"statusLine: {script} 안의 tee 가 기본 계정 경로라 그대로 둠"]
        if "claudebot-statusline" in text:
            # 스크립트 안의 옛 tee 는 모든 계정이 한 파일에 쓴다. 고치지 않으면 계정이 다시 섞인다.
            return [f"!! statusLine: {script} 안의 tee 가 한 파일(/tmp/claudebot-statusline.json)에 써요.\n"
                    f"   그 줄의 경로를 \"{STATUSLINE_TEE_PATH}\" 로 바꿔 주세요(계정이 섞이지 않게)."]
    if LEGACY_TEE in cmd:                      # 예전 한 파일 tee → 설정 폴더별 경로
        sl["command"] = cmd.replace(LEGACY_TEE, TEE_PREFIX)
        return ["statusLine: tee 경로를 계정별로 바꿈"]
    if "claudebot-statusline" in cmd:
        return ["statusLine: 직접 tee 를 쓰고 있어 그대로 둠"]
    if not cmd:
        data["statusLine"] = {"type": "command", "command": f"{TEE_PREFIX} >/dev/null"}
        return ["statusLine: 새로 만듦(화면 표시 없음, 한도만 받음)"]
    sl["command"] = f"{TEE_PREFIX} | {{ {cmd}\n}}"
    return ["statusLine: 원래 명령 앞에 tee 를 붙임"]


def unwrap_statusline(data: dict) -> list[str]:
    sl = data.get("statusLine")
    cmd = sl.get("command", "") if isinstance(sl, dict) else ""
    if cmd == f"{TEE_PREFIX} >/dev/null":
        del data["statusLine"]
        return ["statusLine: 우리가 만든 것 삭제"]
    head = f"{TEE_PREFIX} | {{ "
    if cmd.startswith(head) and cmd.endswith("\n}"):
        sl["command"] = cmd[len(head):-2]
        return ["statusLine: tee 를 떼고 원래 명령으로"]
    if TEE_PREFIX in cmd:                      # 옛 한 파일 tee 를 바꿨던 것 → 되돌린다
        sl["command"] = cmd.replace(TEE_PREFIX, LEGACY_TEE)
        return ["statusLine: tee 경로를 예전 것으로 되돌림"]
    return []


def in_linked_worktree() -> bool:
    """HERE 가 git 링크 워크트리 안이면 True(.git 이 파일이고 git-dir ≠ common-dir)."""
    def git(*args: str) -> str:
        r = subprocess.run(["git", "-C", str(HERE), "rev-parse", *args], capture_output=True, text=True, check=False)
        return r.stdout.strip() if r.returncode == 0 else ""
    gd, cd = git("--absolute-git-dir"), git("--path-format=absolute", "--git-common-dir")
    return bool(gd and cd and Path(gd).resolve() != Path(cd).resolve())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config-dir", help="Claude Code 설정 폴더 (기본 $CLAUDE_CONFIG_DIR 또는 ~/.claude)")
    ap.add_argument("--python", help="훅 명령의 파이썬 (기본: 보드에 접속되는 것을 골라 절대 경로로, hook_python.py)")
    ap.add_argument("--remove", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--allow-worktree", action="store_true", help="git 워크트리에서도 설치한다(시험용)")
    a = ap.parse_args()

    if not a.remove and in_linked_worktree() and not a.allow_worktree:
        raise SystemExit(f"{HERE} 는 git 워크트리(작업용 복사본)라, 지우면 훅이 깨져요.\n"
                         "원래 레포 폴더에서 돌리거나, 정말 여기를 쓸 거면 --allow-worktree 를 붙이세요.")
    config_dir = Path(a.config_dir).expanduser() if a.config_dir else default_config_dir()
    target = config_dir / "settings.json"
    data = json.loads(target.read_text(encoding="utf-8")) if target.is_file() else {}
    if not isinstance(data, dict):
        raise SystemExit(f"{target}: 최상위가 객체가 아니다")
    before = json.dumps(data, sort_keys=True)
    if not a.remove and not a.python:
        a.python, why = pick_python()
        print(f"  훅 파이썬: {a.python} — {why}")
    notes = (remove_hooks(data) + unwrap_statusline(data)) if a.remove \
        else (merge_hooks(data, a.python) + wrap_statusline(data, config_dir))
    for n in notes:
        print(f"  {n}")
    warn = any(n.startswith("!!") for n in notes)   # 손으로 고칠 게 있으면 끝 코드 2(setup.py 가 끝에 다시 알림)
    if a.dry_run:
        json.dump(data, sys.stdout, indent=2, ensure_ascii=False)
        print(f"\n[dry-run] {target}")
        return 2 if warn else 0
    if json.dumps(data, sort_keys=True) == before:
        print(f"변경 없음: {target}")
        return 2 if warn else 0
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_file():
        bak = target.with_name(f"settings.json.claudebot-bak-{time.strftime('%Y%m%d-%H%M%S')}")
        shutil.copy2(target, bak)
        print(f"백업: {bak}")
    target.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"저장: {target}")
    return 2 if warn else 0


if __name__ == "__main__":
    sys.exit(main())
