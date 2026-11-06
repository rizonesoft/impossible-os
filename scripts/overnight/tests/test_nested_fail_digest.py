#!/usr/bin/env python3
"""A failing nested suite must name its failure in the push and CI logs.

The digest matched `[FAIL]` only at column 0, but the suites indent it, so a failure
reached the log as `842/844 passed` with no name, and mutation lines mentioning FAIL
under `[PASS]` took its place. CI keeps no artifact, so that line is all there is.
"""
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[3]


def digest(text: str) -> str:
    src = (REPO / "scripts/test-tooling.sh").read_text()
    body = re.search(r"_tt_nested_fail_detail\(\) \{.*?\n\}", src, re.S).group(0)
    pipe = re.search(r"printf .%s.n. \"\$out\" \| (grep -E .*?)\| sed", body, re.S).group(1)
    return subprocess.run(["bash", "-c", f'printf "%s\\n" "$OUT" | {pipe}'], env={"OUT": text, "PATH": "/usr/bin:/bin"},
                          capture_output=True, text=True, timeout=20).stdout


def test_indented_failure_is_named():
    out = digest("  [PASS] one\n  [FAIL] identity gate: no clone\n[test_build] 1/2 passed\n")
    assert "identity gate" in out, out


def test_passing_lines_that_mention_fail_are_not_reported():
    out = digest("  [PASS] mutation-check -- rewritten COORDINATES FAIL the check\n")
    assert out.strip() == "", out


def test_tally_line_stays_out():
    assert digest("[test_build] FAIL: 1/2 passed\n").strip() == ""


if __name__ == "__main__":
    test_indented_failure_is_named()
    test_passing_lines_that_mention_fail_are_not_reported()
    test_tally_line_stays_out()
    print("PASS test_nested_fail_digest")
    sys.exit(0)
