#!/usr/bin/env python3
"""account.py 단위 시험 — 계정별 한도 캐시 경로·계정 이름 (보드 불필요).

    python3 -m pytest -q test_account.py
"""
from __future__ import annotations

import json
import os
import subprocess
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

import account


class AccountTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="claudebot-acct-"))
        self.env = mock.patch.dict(os.environ, {}, clear=False)
        self.env.start()
        for k in ("CLAUDE_CONFIG_DIR", "CLAUDEBOT_STATUSLINE_CACHE", "CLAUDEBOT_ACCOUNT_LABEL"):
            os.environ.pop(k, None)
        self.nick = mock.patch.object(account, "NICKNAMES", self.tmp / "accounts.json")
        self.nick.start()

    def tearDown(self) -> None:
        self.nick.stop()
        self.env.stop()

    def make_dir(self, name: str, email: str | None) -> Path:
        d = self.tmp / name
        d.mkdir()
        if email:
            (d / ".claude.json").write_text(json.dumps({"oauthAccount": {"emailAddress": email}}))
        return d

    def test_default_dir_keeps_legacy_cache(self) -> None:
        self.assertEqual(account.statusline_cache(), Path("/tmp/claudebot-statusline.json"))

    def test_other_dir_gets_own_cache(self) -> None:
        os.environ["CLAUDE_CONFIG_DIR"] = str(self.tmp / ".claude-community")
        self.assertEqual(account.statusline_cache(), Path("/tmp/claudebot-statusline-.claude-community.json"))

    def test_shell_tee_path_matches_python(self) -> None:
        for value in (None, str(self.tmp / ".claude-support"), str(self.tmp / ".claude-support") + "/"):
            env = {k: v for k, v in os.environ.items() if k != "CLAUDE_CONFIG_DIR"}
            if value is not None:
                env["CLAUDE_CONFIG_DIR"] = value
                os.environ["CLAUDE_CONFIG_DIR"] = value
            else:
                os.environ.pop("CLAUDE_CONFIG_DIR", None)
            out = subprocess.run(["sh", "-c", f'printf %s "{account.STATUSLINE_TEE_PATH}"'],
                                 env=env, capture_output=True, text=True, check=True).stdout
            self.assertEqual(out, str(account.statusline_cache()), value)

    def test_label_from_email_local_part(self) -> None:
        os.environ["CLAUDE_CONFIG_DIR"] = str(self.make_dir(".claude-a", "user@example.com"))
        self.assertEqual(account.account_label(), "user")

    def test_label_is_ascii_and_cut_to_six(self) -> None:
        os.environ["CLAUDE_CONFIG_DIR"] = str(self.make_dir(".claude-b", "Example.User+x@example.com"))
        self.assertEqual(account.account_label(), "exampl")

    def test_nickname_by_email_or_dir(self) -> None:
        os.environ["CLAUDE_CONFIG_DIR"] = str(self.make_dir(".claude-c", "support@example.com"))
        (self.tmp / "accounts.json").write_text(json.dumps({".claude-c": "sup"}))
        self.assertEqual(account.account_label(), "sup")
        (self.tmp / "accounts.json").write_text(json.dumps({"support@example.com": "SUP2"}))
        self.assertEqual(account.account_label(), "sup2")

    def test_no_login_falls_back_to_week(self) -> None:
        os.environ["CLAUDE_CONFIG_DIR"] = str(self.make_dir(".claude-d", None))
        self.assertEqual(account.account_label(), "week")

    def test_push_usage_puts_label_on_week_cell(self) -> None:
        import push_usage as pu
        os.environ["CLAUDE_CONFIG_DIR"] = str(self.make_dir(".claude-e", "user@example.com"))
        cache = self.tmp / "sl.json"
        cache.write_text(json.dumps({"rate_limits": {"seven_day": {"used_percentage": 13,
                                                                   "resets_at": time.time() + 3 * 86400}}}))
        with mock.patch.object(pu, "STATUSLINE_CACHE", cache):
            cells, real = pu.claude_limit_cells({})
        self.assertTrue(real)
        self.assertEqual([(c["name"], c["left"]) for c in cells], [("5h", ""), ("user", "13%")])


if __name__ == "__main__":
    unittest.main()
