#!/usr/bin/env python3
"""Starting is one command, and it refuses only what makes running impossible."""
import os
import pathlib
import re
import subprocess

REPO = pathlib.Path(__file__).resolve().parents[3]
ARM = REPO / ".claude/skills/overnight-sequencer/arm-sequencer.sh"
TEXT = ARM.read_text()


def test_the_start_up_blocks_are_gone():
    for gone in ("canary_unproven", "CANARY_STAMP", "ARM_FORCE", "capture-open-findings",
                 "the newest capture file is CLOSED", "REFUSED to arm: pre-arm",
                 "REFUSED to arm unattended"):
        assert gone not in TEXT, gone


def test_refusal_what_makes_running_impossible_still_refuses():
    refusals = re.findall(r"REFUSED to arm: [^\n\"]*", TEXT)
    assert any("is not installed" in r for r in refusals), refusals
    assert any("token does not authenticate" in r for r in refusals), refusals
    assert any("permission deny" in r for r in refusals), refusals
    assert len(refusals) == 3, refusals


def test_old_invocations_still_work():
    assert re.search(r"--force\)\s*\n\s*shift", TEXT)
    r = subprocess.run(["bash", str(ARM), "--record-canary"], cwd=REPO,
                       capture_output=True, text=True)
    assert r.returncode == 0 and "retired" in r.stdout, r
    assert not (REPO / ".claude/state/sequencer-canary-ok.new").exists()


def test_a_second_worktree_arms_from_the_primary():
    assert 'exec bash "$_PRIMARY_ROOT/.claude/skills/overnight-sequencer/arm-sequencer.sh" "$@"' in TEXT


def test_failed_health_check_warns_and_continues():
    block = TEXT[TEXT.index("Pre-arm health report"):TEXT.index("# Arm. Marker first")]
    assert "WARN:" in block and "exit" not in block


def test_shell_syntax():
    subprocess.run(["bash", "-n", str(ARM)], check=True)


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("OK")
