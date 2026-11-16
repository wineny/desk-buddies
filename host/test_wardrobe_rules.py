#!/usr/bin/env python3
"""wardrobe_rules 단위 테스트 (보드 불필요).  python3 test_wardrobe_rules.py"""
from __future__ import annotations

import tempfile
import time
import unittest
from datetime import datetime
from pathlib import Path

import wardrobe_rules as wr


def ts(day: int, hour: int) -> float:
    return datetime(2026, 8, day, hour, 0, 0).timestamp()


class Rules(unittest.TestCase):
    def test_crown_10_subagents_a_day(self):
        m: dict = {}
        for _ in range(9):
            self.assertEqual(wr.update_on_event({"hook_event_name": "SubagentStart"}, ts(31, 10), m), [])
        self.assertEqual(wr.update_on_event({"hook_event_name": "SubagentStart"}, ts(31, 10), m), ["crown"])
        # 다음날 다시 10개 채워도 이미 언락 — new 없음
        for _ in range(10):
            self.assertEqual(wr.update_on_event({"hook_event_name": "SubagentStart"}, ts(30, 10) + 86400 * 2, m), [])

    def test_crown_not_across_days(self):
        m: dict = {}
        for d in (20, 21):
            for _ in range(6):
                wr.update_on_event({"hook_event_name": "SubagentStart"}, ts(d, 10), m)
        self.assertNotIn("crown", m["unlocked"])

    def test_shades_50_clean_turns_reset_on_error(self):
        m: dict = {}
        for _ in range(49):
            self.assertEqual(wr.update_on_event({"hook_event_name": "Stop"}, ts(31, 10), m), [])
        wr.update_on_event({"hook_event_name": "PostToolUseFailure"}, ts(31, 10), m)
        for _ in range(49):
            self.assertEqual(wr.update_on_event({"hook_event_name": "Stop"}, ts(31, 11), m), [])
        self.assertEqual(wr.update_on_event({"hook_event_name": "Stop"}, ts(31, 11), m), ["shades"])

    def test_lofi_5_late_days(self):
        m: dict = {}
        for d in (20, 21, 22, 23):
            wr.update_on_event({"hook_event_name": "Stop"}, ts(d, 23), m)
        self.assertNotIn("lofi", m.get("unlocked", []))
        self.assertEqual(wr.update_on_event({"hook_event_name": "Stop"}, ts(24, 23), m), ["lofi"])
        # 22시는 안 친다
        m2: dict = {}
        for d in (20, 21, 22, 23, 24):
            wr.update_on_event({"hook_event_name": "Stop"}, ts(d, 22), m2)
        self.assertNotIn("lofi", m2.get("unlocked", []))

    def test_flag_on_head_change(self):
        heads = iter(["aaa", "aaa", "bbb"])
        gh = lambda _cwd: next(heads)  # noqa: E731
        m: dict = {}
        p = {"hook_event_name": "Stop", "cwd": "/repo"}
        self.assertEqual(wr.update_on_event(p, ts(31, 10), m, git_head=gh), [])   # 첫 관측 = 기준점
        self.assertEqual(wr.update_on_event(p, ts(31, 10), m, git_head=gh), [])   # 그대로
        self.assertEqual(wr.update_on_event(p, ts(31, 10), m, git_head=gh), ["flag"])  # 변화 = 커밋
        # 언락 뒤에는 git 을 부르지 않는다
        self.assertEqual(wr.update_on_event(p, ts(31, 10), m, git_head=lambda _c: self.fail("called")), [])

    def test_process_delivery_retry(self):
        with tempfile.TemporaryDirectory() as td:
            mp = Path(td) / "wardrobe.json"
            calls: list[dict] = []

            def failing_post(_p: dict) -> None:
                raise OSError("board off")

            # 언락은 됐지만 전송 실패 → acked 비어 있음
            for _ in range(50):
                wr.process({"hook_event_name": "Stop"}, ts(31, 10), failing_post, mirror_path=mp)
            m = wr._read(mp)
            self.assertIn("shades", m["unlocked"])
            self.assertEqual(m.get("acked", []), [])
            # 보드가 살아나면 다음 이벤트 때 전달
            wr.process({"hook_event_name": "UserPromptSubmit"}, ts(31, 10), calls.append, mirror_path=mp)
            self.assertEqual(calls, [{"unlock": "shades"}])
            self.assertEqual(wr._read(mp)["acked"], ["shades"])
            # 재전송 없음
            calls.clear()
            wr.process({"hook_event_name": "UserPromptSubmit"}, ts(31, 10), calls.append, mirror_path=mp)
            self.assertEqual(calls, [])

    def test_prune_keeps_counters_small(self):
        m: dict = {}
        base = ts(1, 10)
        for d in range(1, 31):
            wr.update_on_event({"hook_event_name": "SubagentStart"}, base + d * 86400, m)
        self.assertLessEqual(len(m["counters"]["subagents"]), wr.KEEP_DAYS)


if __name__ == "__main__":
    unittest.main(verbosity=1)
