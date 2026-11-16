"""Claude Code 계정 구분 — 한도 캐시 파일 경로와 보드에 찍을 계정 이름.

한 맥에서 설정 폴더(CLAUDE_CONFIG_DIR)를 바꿔 가며 계정 여럿을 쓰면(예: ccd·ccc·ccs)
statusline 이 한 파일에 덮어써서 남의 계정 한도가 섞여 보였다(2026-11-07). 그래서

- 한도 캐시를 설정 폴더마다 따로 둔다: 기본 폴더(~/.claude, 변수 없음)는 예전 경로
  /tmp/claudebot-statusline.json 그대로, 다른 폴더는 /tmp/claudebot-statusline-<폴더 이름>.json.
  statusline 쪽 tee 는 STATUSLINE_TEE_PATH 셸 식으로 같은 경로를 만든다.
- 주간 한도 칸 이름을 "week" 대신 계정 이름으로 찍는다. 계정을 하나만 써도 같다.

계정 이름 = ~/.claudebot/accounts.json 의 별명(이메일 또는 설정 폴더 이름 → 별명) →
없으면 이메일 @ 앞부분. 보드 글꼴이 영문뿐이라 영문·숫자·.-_ 만 남기고 6자로 자른다.
"""
from __future__ import annotations

import json
import os
import re
from pathlib import Path

LEGACY_CACHE = Path("/tmp/claudebot-statusline.json")
NICKNAMES = Path.home() / ".claudebot" / "accounts.json"
LABEL_MAX = 6
FALLBACK_LABEL = "week"

# statusline 명령 안에서 쓰는 셸 식. statusline_cache() 와 같은 경로가 나와야 한다.
STATUSLINE_TEE_PATH = '/tmp/claudebot-statusline${CLAUDE_CONFIG_DIR:+-$(basename "$CLAUDE_CONFIG_DIR")}.json'


def config_dir() -> Path | None:
    """설정 폴더. 변수가 없으면(기본 ~/.claude) None."""
    raw = os.environ.get("CLAUDE_CONFIG_DIR", "").strip()
    return Path(raw).expanduser() if raw else None


def statusline_cache() -> Path:
    override = os.environ.get("CLAUDEBOT_STATUSLINE_CACHE")
    if override:
        return Path(override)
    d = config_dir()
    return LEGACY_CACHE if d is None else Path(f"/tmp/claudebot-statusline-{d.name}.json")


def account_email() -> str:
    """로그인한 계정 이메일. 못 찾으면 빈 문자열."""
    d = config_dir()
    candidates = [d / ".claude.json"] if d is not None else [Path.home() / ".claude" / ".claude.json",
                                                              Path.home() / ".claude.json"]
    for p in candidates:
        try:
            acct = json.loads(p.read_text(encoding="utf-8")).get("oauthAccount") or {}
        except (OSError, json.JSONDecodeError, AttributeError):
            continue
        email = acct.get("emailAddress") if isinstance(acct, dict) else None
        if isinstance(email, str) and "@" in email:
            return email
    return ""


def clean(name: str) -> str:
    return re.sub(r"[^a-z0-9._-]", "", name.lower())[:LABEL_MAX]


def account_label() -> str:
    forced = os.environ.get("CLAUDEBOT_ACCOUNT_LABEL", "")   # 시험·강제용
    if clean(forced):
        return clean(forced)
    email = account_email()
    d = config_dir()
    try:
        nick = json.loads(NICKNAMES.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        nick = {}
    if isinstance(nick, dict):
        for key in (email, d.name if d is not None else ".claude"):
            if key and isinstance(nick.get(key), str) and clean(nick[key]):
                return clean(nick[key])
    return clean(email.split("@", 1)[0]) or FALLBACK_LABEL
