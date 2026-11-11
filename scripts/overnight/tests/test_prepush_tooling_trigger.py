#!/usr/bin/env python3
"""The pre-push tooling trigger: a Check 7 baseline bump alone does not gate.

2026-09-28 canary: two docs-section stamp commits each moved lint Check 7's
`total`, the sanctioned repair edited only scripts/lint/stub-lint-baseline.json,
and that data file pulled the ~15-23 minute pack in behind the push. The
trigger now exempts exactly that file. Refusal-direction controls: the checker
itself, any other lint file, and a baseline bump pushed WITH a real tooling
change must all still gate.
"""
from __future__ import annotations

import pathlib
import re
import subprocess

REPO = pathlib.Path(__file__).resolve().parents[3]
HOOK = REPO / ".githooks/pre-push"


def _regexes():
    src = HOOK.read_text(encoding="utf-8")
    paths = re.search(r"^TOOLING_PATHS_RE='([^']+)'", src, re.M).group(1)
    exempt = re.search(r"^TOOLING_EXEMPT_RE='([^']+)'", src, re.M).group(1)
    assert 'grep -vE "$TOOLING_EXEMPT_RE"' in src, "the exemption must be applied to TOOLING_HITS"
    return paths, exempt


def _hits(files):
    paths, exempt = _regexes()
    r = subprocess.run(["bash", "-c", 'grep -E "$1" | grep -vE "$2" || true', "x", paths, exempt],
                       input="\n".join(files) + "\n", capture_output=True, text=True, timeout=10)
    return [l for l in r.stdout.splitlines() if l]


def test_baseline_only_push_does_not_gate():
    assert _hits(["scripts/lint/stub-lint-baseline.json", "todo/09-desktop-shell/TODO-07.md", "COUNT.md"]) == []


def test_the_checker_still_gates():
    assert _hits(["scripts/lint/check_stub_behind_stamp.py"]) == ["scripts/lint/check_stub_behind_stamp.py"]


def test_baseline_with_a_real_tooling_change_still_gates():
    got = _hits(["scripts/lint/stub-lint-baseline.json", ".claude/hooks/x.py"])
    assert got == [".claude/hooks/x.py"], got


def test_the_exemption_is_exact_not_a_prefix():
    got = _hits(["scripts/lint/stub-lint-baseline.json.bak", "scripts/lint/stub-lint-baseline.jsonx"])
    assert len(got) == 2, got


READS = REPO / "scripts/hooks/tooling-pack-reads.sh"


def test_a_source_the_pack_reads_is_listed_so_a_push_touching_it_gates(tmp_path):
    """Live 2026-11-11: 0fd8bf1c0 changed test_usermode.c only, the pack was not run, and CI went red."""
    done = subprocess.run(["bash", str(READS)], capture_output=True, text=True, timeout=30)
    listed = done.stdout.split()
    assert done.returncode == 0 and "src/kernel/test/test_usermode.c" in listed, done
    assert all(p.startswith(("src/", "include/", "user/")) for p in listed) and listed == sorted(set(listed))
    # The list is derived: a check written a minute ago is covered without editing the hook.
    pack = tmp_path / "pack.sh"
    pack.write_text('grep -q x "$REPO_ROOT/src/kernel/new_thing.c"\ncat "$REPO_ROOT/scripts/lint.sh" "$REPO_ROOT/src/a b.c"\n')
    done = subprocess.run(["bash", str(READS), str(pack)], capture_output=True, text=True, timeout=30)
    assert done.stdout.split() == ["src/kernel/new_thing.c"], done.stdout
    assert subprocess.run(["bash", str(READS), str(tmp_path / "absent.sh")], capture_output=True,
                          timeout=30).returncode == 2
    hook = HOOK.read_text(encoding="utf-8")
    assert hook.index("tooling-pack-reads.sh") < hook.index('if [ -n "$TOOLING_HITS" ]; then'), \
        "the reads must join TOOLING_HITS before the gate decides"
    assert "refusing rather than guessing" in hook


if __name__ == "__main__":
    test_baseline_only_push_does_not_gate()
    test_the_checker_still_gates()
    test_baseline_with_a_real_tooling_change_still_gates()
    test_the_exemption_is_exact_not_a_prefix()
    print("PASS: pre-push tooling trigger")
