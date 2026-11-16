#!/usr/bin/env python3
"""에이전트 배지 호스트 쪽(docs/agent-badge.md §4) 단위 테스트 (보드 불필요).

    python3 test_agent_filter.py

세션 파일 agent 기록 · 선택 캐시별 집계 필터 · 응답 파싱 · push_loop 의 "agent 만
바뀌어도 전송" · "응답 선택 ≠ 캐시면 재집계 한 바퀴 더".
"""
from __future__ import annotations

import json
import os
import tempfile
import time
import unittest
from pathlib import Path

_TMP = tempfile.mkdtemp(prefix="claudebot-test-")
os.environ["CLAUDEBOT_STATE_DIR"] = _TMP
os.environ["CLAUDEBOT_STATE_LOG"] = str(Path(_TMP) / "log")
os.environ["CLAUDEBOT_ACCOUNT_LABEL"] = "week"   # 계정 이름 대신 예전 칸 이름으로 시험한다

import boardnet as bn          # noqa: E402
import claude_state as cs      # noqa: E402
import push_usage as pu        # noqa: E402


def _session(name: str, state: str, agent: str | None, at: float | None = None, ctx: str = "x") -> None:
    d: dict = {"state": state, "ctx": ctx, "at": at if at is not None else time.time()}
    if agent is not None:
        d["agent"] = agent
    (cs.STATE_DIR / f"{name}.json").write_text(json.dumps(d))


class AgentFilter(unittest.TestCase):
    def setUp(self):
        cs.STATE_DIR.mkdir(parents=True, exist_ok=True)
        for p in cs.STATE_DIR.iterdir():
            if p.is_file():
                p.unlink()

    # --- 세션 파일에 agent 기록 ---
    def test_apply_event_records_agent(self):
        now = time.time()
        cs.apply_event({"hook_event_name": "PreToolUse", "session_id": "a", "tool_name": "Bash",
                        "transcript_path": "/Users/x/.claude/projects/p/a.jsonl"}, now)
        cs.apply_event({"hook_event_name": "PreToolUse", "session_id": "b", "tool_name": "Bash",
                        "transcript_path": "/Users/x/.codex/sessions/2026/b.jsonl"}, now)
        self.assertEqual(cs.read_json(cs.session_path("a"))["agent"], "claude")
        self.assertEqual(cs.read_json(cs.session_path("b"))["agent"], "codex")

    # --- 집계 필터 ---
    def test_aggregate_both_uses_priority(self):
        _session("c", "permission", "claude")
        _session("x", "working", "codex")
        self.assertEqual(cs.aggregate("both"), ("permission", "x", "claude"))

    def test_aggregate_codex_only_sees_codex(self):
        _session("c", "permission", "claude")
        _session("x", "working", "codex", ctx="Bash")
        self.assertEqual(cs.aggregate("codex"), ("working", "Bash", "codex"))

    def test_aggregate_claude_only_sees_claude(self):
        _session("c", "thinking", "claude", ctx="reading prompt")
        _session("x", "permission", "codex")
        self.assertEqual(cs.aggregate("claude"), ("thinking", "reading prompt", "claude"))

    def test_aggregate_legacy_session_is_claude(self):
        _session("old", "working", None)
        self.assertEqual(cs.aggregate("claude"), ("working", "x", "claude"))
        self.assertEqual(cs.aggregate("codex"), ("idle", "", ""))

    def test_aggregate_idle_has_no_agent(self):
        self.assertEqual(cs.aggregate("both"), ("idle", "", ""))

    def test_aggregate_default_reads_cache(self):
        _session("c", "permission", "claude")
        _session("x", "working", "codex")
        bn.write_agent_selected("codex")
        self.assertEqual(cs.aggregate()[2], "codex")
        bn.write_agent_selected("both")
        self.assertEqual(cs.aggregate()[2], "claude")

    # --- 응답 파싱 ---
    def test_agent_from_response(self):
        self.assertEqual(bn.agent_from_response(b"ok"), "both")          # 옛 펌웨어
        self.assertEqual(bn.agent_from_response(b'{"ok":1,"agent":"codex"}'), "codex")
        self.assertEqual(bn.agent_from_response(b'{"ok":1,"agent":"gemini"}'), "both")
        self.assertEqual(bn.agent_from_response(b""), "both")
        self.assertEqual(bn.agent_from_response(None), "both")
        self.assertEqual(bn.agent_from_response(b"[1,2]"), "both")

    def test_read_agent_selected_defaults_both(self):
        bn.AGENT_SELECTED_FILE.unlink(missing_ok=True)
        self.assertEqual(bn.read_agent_selected(), "both")
        bn.AGENT_SELECTED_FILE.write_text("gemini")
        self.assertEqual(bn.read_agent_selected(), "both")
        bn.write_agent_selected("claude")
        self.assertEqual(bn.read_agent_selected(), "claude")

    # --- push_loop ---
    def _patch_post(self, responses: list[bytes]):
        calls: list[tuple] = []
        def fake(state, ctx, agent=""):
            calls.append((state, ctx, agent))
            return responses.pop(0) if responses else b'{"ok":1,"agent":"both"}'
        self._orig = cs.post_once
        cs.post_once = fake
        self.addCleanup(lambda: setattr(cs, "post_once", self._orig))
        return calls

    def test_push_loop_sends_when_only_agent_changes(self):
        cs.write_json(cs.LAST_PUSH, {"state": "working", "ctx": "Bash", "agent": "claude", "at": time.time() - 100})
        cs.write_json(cs.DESIRED, {"state": "working", "ctx": "Bash", "agent": "codex"})
        bn.write_agent_selected("both")
        calls = self._patch_post([b'{"ok":1,"agent":"both"}'])
        cs.push_loop()
        self.assertEqual(calls, [("working", "Bash", "codex")])
        self.assertEqual(cs.read_json(cs.LAST_PUSH)["agent"], "codex")

    def test_push_loop_throttles_agent_flip_within_interval(self):
        # BOTH 에서 둘 다 working — 배지만 뒤집히는 push 는 3초 완충을 탄다 (E2E 실측 초당 2회)
        cs.write_json(cs.LAST_PUSH, {"state": "working", "ctx": "Bash", "agent": "claude", "at": time.time()})
        cs.write_json(cs.DESIRED, {"state": "working", "ctx": "Bash", "agent": "codex"})
        calls = self._patch_post([])
        cs.push_loop()
        self.assertEqual(calls, [])

    def test_push_loop_state_change_bypasses_interval(self):
        cs.write_json(cs.LAST_PUSH, {"state": "working", "ctx": "Bash", "agent": "claude", "at": time.time()})
        cs.write_json(cs.DESIRED, {"state": "permission", "ctx": "Bash approve?", "agent": "codex"})
        calls = self._patch_post([b'{"ok":1,"agent":"both"}'])
        cs.push_loop()
        self.assertEqual(calls, [("permission", "Bash approve?", "codex")])

    def test_push_loop_skips_when_unchanged(self):
        cs.write_json(cs.LAST_PUSH, {"state": "working", "ctx": "Bash", "agent": "claude", "at": time.time()})
        cs.write_json(cs.DESIRED, {"state": "working", "ctx": "Bash", "agent": "claude"})
        calls = self._patch_post([])
        cs.push_loop()
        self.assertEqual(calls, [])

    def test_push_loop_reaggregates_on_selection_change(self):
        # Claude 는 permission, Codex 는 working. 캐시는 both → permission 을 보내지만
        # 보드가 "나는 codex" 라 답한다 → 캐시 갱신 + 재집계 → working/codex 를 한 번 더.
        _session("c", "permission", "claude", ctx="Bash approve?")
        _session("x", "working", "codex", ctx="Bash")
        bn.write_agent_selected("both")
        with cs.LOCK_FILE.open("w"):
            cs.aggregate_to_desired()
        self.assertEqual(cs.read_json(cs.DESIRED)["agent"], "claude")
        calls = self._patch_post([b'{"ok":1,"agent":"codex"}', b'{"ok":1,"agent":"codex"}'])
        cs.push_loop()
        self.assertEqual(calls, [("permission", "Bash approve?", "claude"), ("working", "Bash", "codex")])
        self.assertEqual(bn.read_agent_selected(), "codex")
        self.assertEqual(cs.read_json(cs.DESIRED)["agent"], "codex")

    def test_push_loop_fill_in_bypasses_throttle_when_state_same(self):
        # 둘 다 working. 캐시 both → claude(최신) 를 보냄 → 보드 "codex" → 재집계 결과도
        # working 이지만(agent 만 codex) 메우기 push 는 완충을 건너뛰고 나가야 한다.
        _session("c", "working", "claude", ctx="Read", at=time.time())
        _session("x", "working", "codex", ctx="Bash", at=time.time() - 1)
        bn.write_agent_selected("both")
        with cs.LOCK_FILE.open("w"):
            cs.aggregate_to_desired()
        self.assertEqual(cs.read_json(cs.DESIRED)["agent"], "claude")
        calls = self._patch_post([b'{"ok":1,"agent":"codex"}', b'{"ok":1,"agent":"codex"}'])
        cs.push_loop()
        self.assertEqual(calls, [("working", "Read", "claude"), ("working", "Bash", "codex")])
        last = cs.read_json(cs.LAST_PUSH)
        self.assertEqual((last["state"], last["agent"]), ("working", "codex"))

    def test_push_loop_filtered_push_not_recorded_as_shown(self):
        # 보드가 거른 push(선택 codex 인데 agent claude)는 last-push 에 남지 않는다
        cs.write_json(cs.LAST_PUSH, {"state": "idle", "ctx": "", "agent": "", "at": time.time() - 100})
        cs.write_json(cs.DESIRED, {"state": "working", "ctx": "Read", "agent": "claude"})
        bn.write_agent_selected("both")
        calls = self._patch_post([b'{"ok":1,"agent":"codex"}'])
        cs.push_loop()   # 재집계 결과는 idle(codex 세션 없음) == last → 끝
        self.assertEqual(calls, [("working", "Read", "claude")])
        self.assertEqual(cs.read_json(cs.LAST_PUSH)["state"], "idle")

    def test_cache_ttl_expires_to_both(self):
        bn.write_agent_selected("codex")
        self.assertEqual(bn.read_agent_selected(), "codex")
        old = time.time() - bn.AGENT_SELECTED_TTL_S - 5
        os.utime(bn.AGENT_SELECTED_FILE, (old, old))
        self.assertEqual(bn.read_agent_selected(), "both")

    # --- push_usage ---
    def _patch_pu_post(self, responses: list[str]):
        calls: list[dict] = []
        def fake(payload):
            calls.append(payload)
            sel = responses.pop(0) if responses else "both"
            bn.write_agent_selected(sel)
            return sel
        orig = pu.post
        pu.post = fake
        self.addCleanup(lambda: setattr(pu, "post", orig))
        return calls

    def test_should_push_matching_or_both(self):
        bn.write_agent_selected("both")
        calls = self._patch_pu_post([])
        self.assertTrue(pu.should_push("codex"))
        bn.write_agent_selected("codex")
        self.assertTrue(pu.should_push("codex"))
        self.assertEqual(calls, [])   # ping 없이 바로 진행

    def test_should_push_pings_when_cache_mismatch(self):
        bn.write_agent_selected("claude")
        calls = self._patch_pu_post(["claude"])
        self.assertFalse(pu.should_push("codex"))
        self.assertEqual(calls, [{"agent": "codex"}])   # 싼 ping 한 번, 7초 집계는 안 함

    def test_should_push_recovers_from_stale_cache(self):
        # 캐시는 claude 인데 보드는 이미 both(배지 탭) → ping 응답으로 알아채고 진행
        bn.write_agent_selected("claude")
        calls = self._patch_pu_post(["both"])
        self.assertTrue(pu.should_push("codex"))
        self.assertEqual(len(calls), 1)
        self.assertEqual(bn.read_agent_selected(), "both")

    def test_build_payload_stamps_agent(self):
        orig_c, orig_x = pu.build_payload_claude, pu.build_payload_codex
        pu.build_payload_claude = lambda: {"cells": ["c"]}
        pu.build_payload_codex = lambda: {"cells": ["x"]}
        self.addCleanup(lambda: setattr(pu, "build_payload_claude", orig_c))
        self.addCleanup(lambda: setattr(pu, "build_payload_codex", orig_x))
        self.assertEqual(pu.build_payload({"transcript_path": "/u/.claude/p/a.jsonl"}), {"cells": ["c"], "agent": "claude"})
        self.assertEqual(pu.build_payload({"transcript_path": "/u/.codex/sessions/b.jsonl"}), {"cells": ["x"], "agent": "codex"})

    def test_push_loop_old_firmware_keeps_both(self):
        _session("x", "working", "codex", ctx="Bash")
        bn.write_agent_selected("both")
        with cs.LOCK_FILE.open("w"):
            cs.aggregate_to_desired()
        calls = self._patch_post([b"ok"])
        cs.push_loop()
        self.assertEqual(len(calls), 1)
        self.assertEqual(bn.read_agent_selected(), "both")


CODEX_CELLS = [
    {"name": "5h", "left": "3%", "right": "4h 1m"},
    {"name": "week", "left": "9%", "right": "5d 2h"},
    {"name": "today", "left": "23.6k", "right": ""},
    {"name": "week", "left": "1.2M", "right": ""},
]
CLAUDE_CELLS = [
    {"name": "5h", "left": "24%", "right": "3h 16m"},
    {"name": "week", "left": "22%", "right": "1d 9h"},
    {"name": "today", "left": "$584", "right": "621M"},
    {"name": "week", "left": "$3611", "right": "3.5B"},
]


class CellsCache(unittest.TestCase):
    """§11 전환 직후 cells 메우기 + statusline 낡음 폴백 (보드 불필요)."""

    def setUp(self):
        cs.STATE_DIR.mkdir(parents=True, exist_ok=True)
        for p in cs.STATE_DIR.iterdir():
            if p.is_file():
                p.unlink()

    def _patch_post_json(self, responses: list[bytes] | None = None):
        """boardnet.post_json(재전송이 쓰는 것)과 push_usage.post_json(post 가 쓰는 것) 둘 다."""
        calls: list[tuple[str, dict]] = []
        resp = list(responses or [])
        def fake(url, payload, timeout_s, log, ensure_ascii=False):
            calls.append((url, payload))
            return resp.pop(0) if resp else b'{"ok":1,"agent":"claude"}'
        for mod in (bn, pu):
            orig = mod.post_json
            mod.post_json = fake
            self.addCleanup(lambda m=mod, o=orig: setattr(m, "post_json", o))
        return calls

    # --- 캐시 저장/로드 ---
    def test_save_load_roundtrip(self):
        self.assertIsNone(bn.load_cells("codex"))
        bn.save_cells("codex", CODEX_CELLS)
        self.assertEqual(bn.load_cells("codex"), CODEX_CELLS)
        self.assertIsNone(bn.load_cells("claude"))
        bn.save_cells("both", CLAUDE_CELLS)      # both 는 캐시 대상이 아니다
        bn.save_cells("claude", [])              # 빈 cells 는 안 적는다
        self.assertIsNone(bn.load_cells("both"))
        self.assertIsNone(bn.load_cells("claude"))

    def test_aggregate_ignores_cells_cache_files(self):
        bn.save_cells("codex", CODEX_CELLS)
        bn.save_cells("claude", CLAUDE_CELLS)
        _session("c", "working", "claude", ctx="Read")
        self.assertEqual(cs.aggregate("both"), ("working", "Read", "claude"))
        self.assertTrue(bn.cells_cache_path("codex").exists())   # 낡은 세션으로 오인해 지우지 않는다

    # --- push_loop: 선택 전환 → cells 재전송 ---
    def test_push_loop_resends_cached_cells_on_selection_change(self):
        _session("c", "permission", "claude", ctx="Bash approve?")
        _session("x", "working", "codex", ctx="Bash")
        bn.save_cells("codex", CODEX_CELLS)
        bn.write_agent_selected("claude")
        with cs.LOCK_FILE.open("w"):
            cs.aggregate_to_desired()
        orig = cs.post_once
        cs.post_once = lambda state, ctx, agent="": b'{"ok":1,"agent":"codex"}'
        self.addCleanup(lambda: setattr(cs, "post_once", orig))
        calls = self._patch_post_json()
        cs.push_loop()
        self.assertEqual([p for _, p in calls], [{"cells": CODEX_CELLS, "agent": "codex"}])

    def test_push_loop_no_resend_without_cache(self):
        _session("c", "permission", "claude", ctx="Bash approve?")
        _session("x", "working", "codex", ctx="Bash")
        bn.write_agent_selected("claude")
        with cs.LOCK_FILE.open("w"):
            cs.aggregate_to_desired()
        orig = cs.post_once
        cs.post_once = lambda state, ctx, agent="": b'{"ok":1,"agent":"codex"}'
        self.addCleanup(lambda: setattr(cs, "post_once", orig))
        calls = self._patch_post_json()
        cs.push_loop()
        self.assertEqual(calls, [])
        self.assertEqual(bn.read_agent_selected(), "codex")   # 재집계는 그대로 돈다

    def test_push_loop_no_resend_when_selection_unchanged(self):
        _session("x", "working", "codex", ctx="Bash")
        bn.save_cells("codex", CODEX_CELLS)
        bn.write_agent_selected("codex")
        with cs.LOCK_FILE.open("w"):
            cs.aggregate_to_desired()
        orig = cs.post_once
        cs.post_once = lambda state, ctx, agent="": b'{"ok":1,"agent":"codex"}'
        self.addCleanup(lambda: setattr(cs, "post_once", orig))
        calls = self._patch_post_json()
        cs.push_loop()
        self.assertEqual(calls, [])

    def test_resend_swallows_transport_error(self):
        import urllib.error
        bn.save_cells("codex", CODEX_CELLS)
        def boom(url, payload, timeout_s, log, ensure_ascii=False):
            raise urllib.error.URLError("no route")
        orig = bn.post_json
        bn.post_json = boom
        self.addCleanup(lambda: setattr(bn, "post_json", orig))
        self.assertFalse(bn.resend_cells("codex", "http://x/usage", 1, lambda m: None))

    # --- push_usage.post: 캐시 저장 + 전환 즉시 메우기 ---
    def test_post_saves_cells_even_when_board_filters(self):
        bn.write_agent_selected("claude")
        calls = self._patch_post_json([b'{"ok":1,"agent":"claude"}'])
        sel = pu.post({"cells": CODEX_CELLS, "agent": "codex", pu.LIMITS_REAL_KEY: True})
        self.assertEqual(sel, "claude")
        self.assertEqual(bn.load_cells("codex"), CODEX_CELLS)
        self.assertEqual(len(calls), 1)   # 선택이 안 바뀌었으니 재전송 없음

    def test_post_ping_resends_cache_when_selection_changed(self):
        # 캐시 claude, 보드는 이미 codex(배지 탭). Codex 훅의 ping 응답에서 처음 알고 codex 캐시를 메운다
        bn.write_agent_selected("claude")
        bn.save_cells("codex", CODEX_CELLS)
        calls = self._patch_post_json([b'{"ok":1,"agent":"codex"}', b'{"ok":1,"agent":"codex"}'])
        pu.post({"agent": "codex"})
        self.assertEqual([p for _, p in calls],
                         [{"agent": "codex"}, {"cells": CODEX_CELLS, "agent": "codex"}])
        self.assertIsNone(bn.load_cells("claude"))   # ping 은 캐시에 안 적는다

    def test_post_claude_cells_learns_codex_selected_and_fills(self):
        # 캐시 claude(==훅) 라 ping 없이 Claude cells 전송 → 응답 codex → codex 캐시 재전송
        bn.write_agent_selected("claude")
        bn.save_cells("codex", CODEX_CELLS)
        calls = self._patch_post_json([b'{"ok":1,"agent":"codex"}', b'{"ok":1,"agent":"codex"}'])
        pu.post({"cells": CLAUDE_CELLS, "agent": "claude", pu.LIMITS_REAL_KEY: True})
        self.assertEqual([p for _, p in calls][1], {"cells": CODEX_CELLS, "agent": "codex"})
        self.assertEqual(bn.load_cells("claude"), CLAUDE_CELLS)   # 걸러졌어도 claude 값으로 저장

    def test_post_no_redundant_resend_when_fresh_cells_are_the_selected_ones(self):
        # 캐시 both(TTL 만료) → Claude cells 전송 → 응답 claude(changed) — 방금 보낸 게 곧 최신이라 재전송 안 함
        bn.AGENT_SELECTED_FILE.unlink(missing_ok=True)
        bn.save_cells("claude", CODEX_CELLS)   # 일부러 낡은 값 — 이게 덮어쓰면 안 된다
        calls = self._patch_post_json([b'{"ok":1,"agent":"claude"}'])
        pu.post({"cells": CLAUDE_CELLS, "agent": "claude", pu.LIMITS_REAL_KEY: True})
        self.assertEqual(len(calls), 1)
        self.assertEqual(bn.load_cells("claude"), CLAUDE_CELLS)

    # --- statusline 낡음 → 마지막 실제 한도 재사용 ---
    def _no_statusline(self):
        orig = pu.read_statusline_payload
        pu.read_statusline_payload = lambda: {}
        self.addCleanup(lambda: setattr(pu, "read_statusline_payload", orig))

    def test_stale_statusline_reuses_cached_limits_with_tilde(self):
        self._no_statusline()
        bn.save_cells("claude", CLAUDE_CELLS)
        self.assertEqual(pu.claude_limit_cells({"block_elapsed_pct": 70, "week_pct": 100}), ([
            {"name": "5h~", "left": "24%", "right": "3h 16m"},
            {"name": "week~", "left": "22%", "right": "1d 9h"},
        ], False))

    def test_stale_statusline_without_cache_falls_back_to_estimate(self):
        self._no_statusline()
        self.assertEqual(pu.claude_limit_cells({"block_elapsed_pct": 70, "week_pct": 100}), ([
            {"name": "5h~", "left": "70%", "right": ""},
            {"name": "week~", "left": "100%", "right": ""},
        ], False))

    def test_fresh_statusline_wins_over_cache(self):
        orig = pu.read_statusline_payload
        pu.read_statusline_payload = lambda: {"rate_limits": {
            "five_hour": {"used_percentage": 8, "resets_at": time.time() + 3600},
            "seven_day": {"used_percentage": 32, "resets_at": time.time() + 86400 * 2},
        }}
        self.addCleanup(lambda: setattr(pu, "read_statusline_payload", orig))
        bn.save_cells("claude", CLAUDE_CELLS)
        cells, real = pu.claude_limit_cells({})
        self.assertTrue(real)
        self.assertEqual([c["name"] for c in cells], ["5h", "week"])
        self.assertEqual(cells[0]["left"], "8%")

    def test_missing_five_hour_window_sends_empty_cell(self):
        cells = pu.limit_cells({"rate_limits": {"seven_day": {"used_percentage": 32, "resets_at": time.time() + 86400}}})
        self.assertEqual([c["name"] for c in cells], ["5h", "week"])   # 자리는 지킨다
        self.assertEqual(cells[0], {"name": "5h", "left": "", "right": ""})   # 0% 로 찍지 않는다
        self.assertEqual(cells[1]["left"], "32%")
        # 낡은 값 재사용에서도 빈 칸은 빈 칸으로 남는다
        self.assertEqual(pu.stale_limit_cells(cells)[0]["left"], "")

    def test_stale_limit_cells_rejects_wrong_shape(self):
        self.assertIsNone(pu.stale_limit_cells(None))
        self.assertIsNone(pu.stale_limit_cells([{"name": "today", "left": "$1"}, {"name": "week", "left": "$2"}]))
        # 캐시 자체가 '~' 추정이었어도 모양은 맞으니 그대로(값은 그 추정) — 캐시 없음보다 낫다
        self.assertEqual(pu.stale_limit_cells([{"name": "5h~", "left": "70%", "right": ""},
                                                {"name": "week~", "left": "9%", "right": ""}])[1]["left"], "9%")

    # --- 캐시 오염 방지 (리뷰 HIGH): 추정·낡은 값은 "마지막 실제 한도" 로 저장되지 않는다 ---
    ESTIMATE = [{"name": "5h~", "left": "70%", "right": ""}, {"name": "week~", "left": "100%", "right": ""}]
    USAGE2 = [{"name": "today", "left": "$9", "right": "1M"}, {"name": "week", "left": "$99", "right": "9M"}]

    def test_post_does_not_cache_estimate_when_no_real_limits(self):
        self._patch_post_json()
        pu.post({"cells": self.ESTIMATE + self.USAGE2, "agent": "claude", pu.LIMITS_REAL_KEY: False})
        self.assertIsNone(bn.load_cells("claude"))

    def test_post_stale_run_keeps_real_limits_and_refreshes_usage(self):
        bn.save_cells("claude", CLAUDE_CELLS)
        self._patch_post_json()
        stale = [{"name": "5h~", "left": "24%", "right": "3h 16m"}, {"name": "week~", "left": "22%", "right": "1d 9h"}]
        pu.post({"cells": stale + self.USAGE2, "agent": "claude", pu.LIMITS_REAL_KEY: False})
        self.assertEqual(bn.load_cells("claude"), CLAUDE_CELLS[:2] + self.USAGE2)
        # 그래서 다음 낡음 폴백도 여전히 실제 한도(24%/22%)를 든다 — 추정(70%/100%)이 아니라
        self._no_statusline()
        self.assertEqual(pu.claude_limit_cells({"block_elapsed_pct": 70, "week_pct": 100})[0][0]["left"], "24%")

    def test_post_real_limits_replace_cache_and_flag_not_sent(self):
        bn.save_cells("claude", CODEX_CELLS)
        calls = self._patch_post_json()
        pu.post({"cells": CLAUDE_CELLS, "agent": "claude", pu.LIMITS_REAL_KEY: True})
        self.assertEqual(bn.load_cells("claude"), CLAUDE_CELLS)
        self.assertNotIn(pu.LIMITS_REAL_KEY, calls[0][1])   # 보드로는 안 나간다

    def test_cache_cells_no_real_prev_means_no_write(self):
        pu.cache_cells("codex", self.ESTIMATE + self.USAGE2, False)
        self.assertIsNone(bn.load_cells("codex"))
        pu.cache_cells("codex", CODEX_CELLS, True)
        pu.cache_cells("codex", self.ESTIMATE + self.USAGE2, False)
        self.assertEqual(bn.load_cells("codex"), CODEX_CELLS[:2] + self.USAGE2)


if __name__ == "__main__":
    unittest.main(verbosity=1)
