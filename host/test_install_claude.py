#!/usr/bin/env python3
"""install_claude.py 시험 — 임시 설정 폴더에서만 돈다(실제 ~/.claude 는 안 건드림).

    python3 -m pytest -q test_install_claude.py
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import install_claude as ic  # noqa: E402
import install_hooks as ih  # noqa: E402

HOOK_PY = "/opt/homebrew/bin/python3"


def run(cfg: Path, *args: str, rc: int = 0) -> str:
    env = {**os.environ, "CLAUDEBOT_HOOK_PYTHON": HOOK_PY}   # 보드 접속 확인 없이 고정
    r = subprocess.run([sys.executable, str(HERE / "install_claude.py"), "--config-dir", str(cfg),
                        "--allow-worktree", *args], capture_output=True, text=True, check=False, env=env)
    assert r.returncode == rc, (r.returncode, r.stdout, r.stderr)
    return r.stdout


class InstallClaudeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.cfg = Path(tempfile.mkdtemp(prefix="claudebot-cfg-")) / ".claude-test"
        self.cfg.mkdir()
        self.settings = self.cfg / "settings.json"

    def load(self) -> dict:
        return json.loads(self.settings.read_text())

    def test_fresh_install_adds_all_hooks_and_tee(self) -> None:
        run(self.cfg)
        d = self.load()
        for ev in ic.STATE_EVENTS:
            self.assertTrue(any(ic.which_script(e) == "claude_state.py" for e in d["hooks"][ev]), ev)
        self.assertTrue(any(ic.which_script(e) == "push_usage.py" for e in d["hooks"]["Stop"]))
        self.assertEqual(d["statusLine"]["command"], f"{ic.TEE_PREFIX} >/dev/null")

    def test_keeps_other_hooks_and_wraps_statusline_and_is_idempotent(self) -> None:
        other = {"hooks": [{"type": "command", "command": "echo hi"}]}
        self.settings.write_text(json.dumps({"hooks": {"Stop": [other]}, "model": "x",
                                             "statusLine": {"type": "command", "command": "echo LINE"}}))
        run(self.cfg)
        first = self.settings.read_text()
        self.assertIn("변경 없음", run(self.cfg))
        self.assertEqual(self.settings.read_text(), first)
        d = self.load()
        self.assertIn(other, d["hooks"]["Stop"])
        self.assertEqual(d["model"], "x")
        self.assertTrue(list(self.cfg.glob("settings.json.claudebot-bak-*")))

    def test_wrapped_statusline_runs_and_writes_per_account_cache(self) -> None:
        self.settings.write_text(json.dumps({"statusLine": {"type": "command", "command": "cat | wc -c | tr -d ' '"}}))
        run(self.cfg)
        cmd = self.load()["statusLine"]["command"]
        cache = Path(f"/tmp/claudebot-statusline-{self.cfg.name}.json")
        cache.unlink(missing_ok=True)
        out = subprocess.run(["sh", "-c", cmd], input='{"rate_limits":{}}', capture_output=True, text=True,
                             env={**os.environ, "CLAUDE_CONFIG_DIR": str(self.cfg)}, check=True).stdout
        self.assertEqual(out.strip(), "18")                       # 원래 statusline 은 그대로 입력을 받는다
        self.assertEqual(cache.read_text(), '{"rate_limits":{}}')
        cache.unlink()

    def test_old_path_hooks_are_repointed_and_legacy_tee_renamed(self) -> None:
        old = {"hooks": [{"type": "command", "command": 'python3 "/old/host/claude_state.py"',
                          "timeout": 5, "async": True}]}
        self.settings.write_text(json.dumps({"hooks": {"Stop": [old]},
                                             "statusLine": {"command": "tee /tmp/claudebot-statusline.json | hud"}}))
        run(self.cfg)
        d = self.load()
        cmds = [h["command"] for e in d["hooks"]["Stop"] for h in e["hooks"]]
        self.assertNotIn('python3 "/old/host/claude_state.py"', cmds)
        self.assertEqual(sum("claude_state.py" in c for c in cmds), 1)
        self.assertEqual(d["statusLine"]["command"], f"{ic.TEE_PREFIX} | hud")

    def test_remove_restores_original(self) -> None:
        orig = {"statusLine": {"type": "command", "command": "echo LINE"}, "hooks": {"Stop": [
            {"hooks": [{"type": "command", "command": "echo hi"}]}]}}
        self.settings.write_text(json.dumps(orig))
        run(self.cfg)
        run(self.cfg, "--remove")
        self.assertEqual(self.load(), orig)


    def test_duplicate_hooks_from_two_folders_become_one(self) -> None:
        dup = [{"hooks": [{"type": "command", "command": f'python3 "/{x}/host/push_usage.py"', "timeout": 5}]}
               for x in ("a", "b")]
        self.settings.write_text(json.dumps({"hooks": {"Stop": dup}}))
        run(self.cfg)
        cmds = [h["command"] for e in self.load()["hooks"]["Stop"] for h in e["hooks"]]
        self.assertEqual(sum("push_usage.py" in c for c in cmds), 1)
        self.assertNotIn('python3 "/b/host/push_usage.py"', cmds)

    def test_script_with_legacy_tee_is_not_wrapped_and_warns(self) -> None:
        script = self.cfg / "hud.sh"
        script.write_text('IN=$(mktemp)\ntee /tmp/claudebot-statusline.json > "$IN"\ncat "$IN"\n')
        cmd = "sh ${CLAUDE_CONFIG_DIR:-$HOME/.claude}/hud.sh"
        self.settings.write_text(json.dumps({"statusLine": {"type": "command", "command": cmd}}))
        out = run(self.cfg, rc=2)
        self.assertIn("!! statusLine", out)
        self.assertEqual(self.load()["statusLine"]["command"], cmd)

    def test_script_with_account_tee_is_left_alone(self) -> None:
        script = self.cfg / "hud.sh"
        script.write_text(f'tee "{ic.STATUSLINE_TEE_PATH}" > /dev/null\n')
        cmd = f"sh {script}"
        self.settings.write_text(json.dumps({"statusLine": {"type": "command", "command": cmd}}))
        self.assertNotIn("!!", run(self.cfg))
        self.assertEqual(self.load()["statusLine"]["command"], cmd)

    def test_remove_restores_upgraded_legacy_tee(self) -> None:
        orig = {"statusLine": {"command": "tee /tmp/claudebot-statusline.json | hud"}}
        self.settings.write_text(json.dumps(orig))
        run(self.cfg)
        run(self.cfg, "--remove")
        self.assertEqual(self.load(), orig)

    def test_refuses_inside_git_worktree(self) -> None:
        r = subprocess.run([sys.executable, str(HERE / "install_claude.py"), "--config-dir", str(self.cfg)],
                           capture_output=True, text=True, check=False)
        if ic.in_linked_worktree():
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("워크트리", r.stderr)
            self.assertFalse(self.settings.exists())


    def test_other_hooks_in_same_entry_survive_merge_and_remove(self) -> None:
        mixed = {"matcher": "Bash", "hooks": [
            {"type": "command", "command": 'python3 "/old/host/push_usage.py"', "timeout": 5},
            {"type": "command", "command": "echo mine"}]}
        self.settings.write_text(json.dumps({"hooks": {"Stop": [mixed]}}))
        run(self.cfg)
        stop = self.load()["hooks"]["Stop"]
        self.assertIn({"type": "command", "command": "echo mine"}, stop[0]["hooks"])
        self.assertEqual(stop[0]["matcher"], "Bash")
        run(self.cfg, "--remove")
        self.assertEqual(self.load()["hooks"]["Stop"], [{"matcher": "Bash", "hooks": [
            {"type": "command", "command": "echo mine"}]}])



    def test_hook_uses_picked_python_and_rewrites_bare_python3(self) -> None:
        old = {"hooks": [{"type": "command", "command": f'python3 "{ic.USAGE_SCRIPT}"', "timeout": 5}]}
        self.settings.write_text(json.dumps({"hooks": {"Stop": [old]}}))
        out = run(self.cfg)
        self.assertIn(f"훅 파이썬: {HOOK_PY}", out)
        cmds = [h["command"] for lst in self.load()["hooks"].values() for e in lst for h in e["hooks"]]
        self.assertTrue(cmds and all(c.startswith(f"{HOOK_PY} ") for c in cmds), cmds)
        self.assertNotIn(f'python3 "{ic.USAGE_SCRIPT}"', cmds)


class CodexMergeTest(unittest.TestCase):
    def test_merge_rewrites_bare_python3_in_place(self) -> None:
        other = {"hooks": [{"type": "command", "command": "echo hi"}]}
        ours = ih.hook_entry(ih.STATE_SCRIPT, "python3", "Stop")
        data = {"hooks": {"Stop": [other, ours]}}
        data, n = ih.merge(data, HOOK_PY)
        stop = data["hooks"]["Stop"]
        self.assertEqual(stop[0], other)                         # 남의 훅은 그대로
        self.assertEqual(stop[1]["hooks"][0]["command"], f'{HOOK_PY} "{ih.STATE_SCRIPT}"')  # 제자리에서 고침
        self.assertEqual(stop[2]["hooks"][0]["command"], f'{HOOK_PY} "{ih.USAGE_SCRIPT}"')  # 없던 것은 덧붙임
        self.assertEqual(n, len(ih.STATE_EVENTS) + 1)            # Stop 상태 1줄 고침 + 나머지 8 + 사용량 1
        self.assertEqual(ih.merge(data, HOOK_PY)[1], 0)          # 다시 돌려도 그대로

    def test_merge_drops_duplicates_from_other_folder_and_old_timeout(self) -> None:
        dup = {"hooks": [{"type": "command", "command": 'python3 "/other/host/push_usage.py"', "timeout": 9}]}
        mine = {"hooks": [{"type": "command", "command": f'python3 "{ih.USAGE_SCRIPT}"', "timeout": 9}]}
        data = {"hooks": {"Stop": [mine, dup]}}
        data, _ = ih.merge(data, HOOK_PY)
        usage = [h for e in data["hooks"]["Stop"] for h in e["hooks"] if "push_usage.py" in h["command"]]
        self.assertEqual(usage, [ih.hook_entry(ih.USAGE_SCRIPT, HOOK_PY, "Stop")["hooks"][0]])
        keys = [k for k, _ in ih.our_hook_keys(data, Path("/x/hooks.json")) if ":stop:" in k]
        self.assertEqual(len(keys), len(set(keys)))

    def test_quotes_python_path_with_space(self) -> None:
        cmd = ih.hook_entry(ih.STATE_SCRIPT, "/A B/python3", "Stop")["hooks"][0]["command"]
        self.assertTrue(cmd.startswith("'/A B/python3' "), cmd)


if __name__ == "__main__":
    unittest.main()
