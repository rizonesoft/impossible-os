#!/usr/bin/env python3
"""capture-open-findings.py: the arm-time warning for a skipped close-out."""
import pathlib
import re
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
TOOL = REPO / "scripts/overnight/capture-open-findings.py"
ARM = REPO / ".claude/skills/overnight-sequencer/arm-sequencer.sh"

CARRIED = "## Carried forward from v20\n\n- [ ] carried item\n\n"


def _count(body):
    with tempfile.TemporaryDirectory() as d:
        f = pathlib.Path(d) / "x-v21.md"
        f.write_text(body)
        r = subprocess.run([sys.executable, str(TOOL), str(f)],
                           capture_output=True, text=True, timeout=20)
    assert r.returncode == 0, r.stderr
    return int(r.stdout)


def test_fresh_version_counts_zero():
    assert _count(CARRIED + "## Found live this cycle\n\n<!-- nothing yet -->\n") == 0


def test_carried_items_do_not_count():
    assert _count(CARRIED) == 0


def test_findings_a_run_left_are_counted():
    body = CARRIED + "## Found live this cycle\n\n- [ ] one\n  - detail\n- [ ] two\n"
    assert _count(body) == 2


def test_only_verdicted_items_are_discounted():
    """Control: one verdict must not clear a neighbouring item with none."""
    body = ("## Found live this cycle\n\n- [ ] one\n  - **VERDICT:** carried\n"
            "  - **VERDICT:** carried again\n- [ ] two\n- [x] done\n")
    assert _count(body) == 1


def test_unreadable_file_is_exit_2():
    r = subprocess.run([sys.executable, str(TOOL), "/nonexistent/x.md"],
                       capture_output=True, text=True, timeout=20)
    assert r.returncode == 2


def test_arm_warns_and_does_not_exit_on_open_findings():
    """The warning stays advisory until a real cycle shows its precision: a re-arm
    inside one cycle is legitimate. The CLOSED refusal beside it must survive."""
    text = ARM.read_text()
    block = re.search(r"capture-open-findings\.py.*?\n\s*fi\n", text, re.S)
    assert block, "arm-sequencer must call capture-open-findings.py"
    assert "WARNING" in block.group(0) and "exit" not in block.group(0)
    assert "REFUSED to arm: the newest capture file is CLOSED." in text


if __name__ == "__main__":
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
    print(f"PASS {len(fns)}")
