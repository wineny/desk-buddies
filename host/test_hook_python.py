#!/usr/bin/env python3
"""hook_python.py 시험 — 보드 대신 이 맥의 임시 TCP 서버에 접속해 본다.

    python3 -m pytest -q test_hook_python.py
"""
from __future__ import annotations

import os
import socket
import sys
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import hook_python as hp  # noqa: E402


class HookPythonTest(unittest.TestCase):
    def setUp(self) -> None:
        env = mock.patch.dict(os.environ)          # 끝나면 원래 환경으로 되돌린다
        env.start()
        self.addCleanup(env.stop)
        os.environ.pop("CLAUDEBOT_HOOK_PYTHON", None)
        os.environ.pop("CLAUDEBOT_BOARD_URL", None)

    def test_candidates_are_unique_executables(self) -> None:
        cands = hp.candidates()
        self.assertTrue(cands)
        self.assertEqual(len({str(Path(c).resolve()) for c in cands}), len(cands))
        self.assertTrue(all(os.access(c, os.X_OK) for c in cands))

    def test_env_override_wins(self) -> None:
        with mock.patch.dict(os.environ, {"CLAUDEBOT_HOOK_PYTHON": "/x/python3"}):
            self.assertEqual(hp.pick_python("http://127.0.0.1:9/usage")[0], "/x/python3")

    def test_picks_first_candidate_that_reaches_board(self) -> None:
        with socket.socket() as srv:
            srv.bind(("127.0.0.1", 0))
            srv.listen(8)
            url = f"http://127.0.0.1:{srv.getsockname()[1]}/usage"
            self.assertTrue(hp.can_reach(sys.executable, "127.0.0.1", srv.getsockname()[1]))
            with mock.patch.object(hp, "candidates", return_value=["/blocked/python3", sys.executable]), \
                 mock.patch.object(hp, "can_reach", side_effect=lambda py, h, p: py == sys.executable):
                py, why = hp.pick_python(url)
        self.assertEqual(py, sys.executable)
        self.assertIn("/blocked/python3", why)

    def test_nothing_reaches_falls_back_to_first(self) -> None:
        with mock.patch.object(hp, "candidates", return_value=["/a/python3", "/b/python3"]), \
             mock.patch.object(hp, "can_reach", return_value=False):
            py, why = hp.pick_python("http://192.0.2.1/usage")
        self.assertEqual(py, "/a/python3")
        self.assertIn("안 닿아", why)

    def test_framework_python_goes_last_and_never_fallback(self) -> None:
        fw = "/Library/Frameworks/Python.framework/Versions/3.12/bin/python3"
        with mock.patch.object(hp, "is_framework", side_effect=lambda p: p == fw), \
             mock.patch.object(hp, "candidates", return_value=[fw, "/opt/homebrew/bin/python3"][::-1]), \
             mock.patch.object(hp, "can_reach", return_value=False):
            self.assertEqual(hp.pick_python("http://192.0.2.1/usage")[0], "/opt/homebrew/bin/python3")
        with mock.patch.object(hp, "is_framework", side_effect=lambda p: p.startswith("/Library/Frameworks/")):
            order = sorted([fw, "/opt/homebrew/bin/python3", "/usr/bin/python3"], key=hp.is_framework)
        self.assertEqual(order[-1], fw)

    def test_closed_port_is_unreachable(self) -> None:
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]          # 듣지 않는 포트
        self.assertFalse(hp.can_reach(sys.executable, "127.0.0.1", port))


if __name__ == "__main__":
    unittest.main()
