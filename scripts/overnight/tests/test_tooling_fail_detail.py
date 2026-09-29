#!/usr/bin/env python3
"""test-tooling.sh's t_fail keeps its detail in the --quiet summary.

2026-09-29: a pre-push refusal read only `- test.sh: utest_reap_qemu force-ends
a SIGTERM-resistant VM on a deadline`, because t_fail stored only the NAME and
printed the detail only when QUIET=0 -- and the gate runs --quiet. Triage then
needed a ~23-minute out-of-band pack. The control: under QUIET=1 the stored
failure line must carry the detail, flattened to one line and bounded.
"""
from __future__ import annotations

import pathlib
import re
import subprocess

REPO = pathlib.Path(__file__).resolve().parents[3]
PACK = REPO / "scripts/test-tooling.sh"


def _t_fail_src():
    src = PACK.read_text(encoding="utf-8")
    m = re.search(r"^t_fail\(\) \{\n.*?^\}\n", src, re.S | re.M)
    assert m, "t_fail() not found in test-tooling.sh"
    return m.group(0)


def _run(script):
    return subprocess.run(["bash", "-c", _t_fail_src() + script], capture_output=True,
                          text=True, timeout=30)


def test_quiet_summary_keeps_the_detail():
    r = _run('QUIET=1; FAIL=0; FAILURES=(); RED=; NC=; DIM=\n'
             't_fail "utest_reap_qemu force-ends a VM" "rc=1 signals=TERM stall-net fired"\n'
             'printf "%s\\n" "${FAILURES[@]}"; echo "stdout-quiet:$FAIL"\n')
    assert "utest_reap_qemu force-ends a VM :: rc=1 signals=TERM stall-net fired" in r.stdout, r.stdout
    assert "stdout-quiet:1" in r.stdout, r.stdout


def test_multiline_detail_is_one_bounded_line():
    r = _run('QUIET=1; FAIL=0; FAILURES=(); RED=; NC=; DIM=\n'
             't_fail "name" "$(printf "line1\\nline2"; head -c 900 /dev/zero | tr "\\0" x)"\n'
             'printf "%s\\n" "${FAILURES[@]}" | wc -l; printf "%s" "${FAILURES[0]}" | wc -c\n')
    lines, chars = r.stdout.split()
    assert lines == "1", r.stdout
    assert int(chars) <= len("name :: ") + 400, r.stdout


def test_no_detail_stores_the_bare_name():
    r = _run('QUIET=1; FAIL=0; FAILURES=(); RED=; NC=; DIM=\n'
             't_fail "just a name"\nprintf "[%s]\\n" "${FAILURES[@]}"\n')
    assert "[just a name]" in r.stdout, r.stdout


if __name__ == "__main__":
    test_quiet_summary_keeps_the_detail()
    test_multiline_detail_is_one_bounded_line()
    test_no_detail_stores_the_bare_name()
    print("PASS: t_fail keeps its detail under --quiet")
