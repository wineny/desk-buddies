#!/usr/bin/env python3
"""클로드봇 한 줄 연결 — 보드를 WiFi 에 붙인 뒤 맥에서 한 번 돌린다.

    python3 host/host/setup.py                      # 보드 찾기 + Claude Code + Codex
    python3 host/host/setup.py --config-dir ~/.claude-work   # Claude 설정 폴더를 더 지정(여러 번 가능)
    python3 host/host/setup.py --dry-run            # 파일을 안 바꾸고 결과만

순서: ① 보드 찾기(find_board.py, board.json 에 주소·MAC 저장) ② Claude Code 훅·statusline
(install_claude.py, 기본 설정 폴더 + --config-dir) ③ Codex 가 깔려 있으면 Codex 훅(install_hooks.py).
각 단계는 기존 설정을 백업하고 덧붙이기만 한다. 다시 돌려도 같은 결과다.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


WARNINGS: list[str] = []


def run(step: str, args: list[str]) -> bool:
    print(f"\n== {step}")
    rc = subprocess.run([sys.executable, *args], cwd=HERE, check=False).returncode
    if rc == 2:                                  # 됐지만 손으로 고칠 게 남음(위 !! 줄)
        WARNINGS.append(step)
        return True
    if rc != 0:
        print(f"!! {step} 실패 (코드 {rc})")
    return rc == 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config-dir", action="append", default=[], help="Claude Code 설정 폴더를 더 넣는다")
    ap.add_argument("--skip-board", action="store_true", help="보드 찾기를 건너뛴다")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    ok = True
    if not a.skip_board and not a.dry_run:
        ok &= run("1/3 보드 찾기", [str(HERE / "find_board.py"), "--probe"])
    extra = ["--dry-run"] if a.dry_run else []
    for d in [None, *a.config_dir]:
        label = f"2/3 Claude Code ({d or '기본 설정 폴더'})"
        ok &= run(label, [str(HERE / "install_claude.py"), *(["--config-dir", d] if d else []), *extra])
    if shutil.which("codex") or (Path.home() / ".codex").is_dir():
        ok &= run("3/3 Codex", [str(HERE / "install_hooks.py"), *extra])
    else:
        print("\n== 3/3 Codex — 설치돼 있지 않아 건너뜀")

    for w in WARNINGS:
        print(f"\n!! 손으로 고칠 것이 남았어요: {w} — 위의 !! 줄을 보세요.")
    print("\n" + ("끝. Claude Code 를 새로 켜고 한 번 대화하면 보드에 사용량이 떠요." if ok
                  else "일부 단계가 실패했어요. 위 메시지를 확인해 주세요."))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
