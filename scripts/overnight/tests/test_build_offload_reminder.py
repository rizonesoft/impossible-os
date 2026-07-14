#!/usr/bin/env python3
"""Tests for build_offload_reminder.py -- run-artifact.sh exemption + P3.4 BLOCK.

Codex audit 2026-07-13 (verified): the matcher fired on the INNER
`bash scripts/build.sh` even when correctly wrapped as
`bash scripts/overnight/run-artifact.sh <label> -- bash scripts/build.sh`
(9x on one sequence). The OUTER-wrapper exemption (prereq 1) lets P3.4 promote
the reminder from WARN to BLOCK (exit 2) without deadlocking the sanctioned path:
a BARE build/test in the SECTIONS phase is rerouted; the wrapped form is exempt.
"""
import importlib.util
import io
import json
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
HOOK = HERE.parent.parent.parent / ".claude/hooks/build_offload_reminder.py"


def _load():
    # Production runs `python3 .claude/hooks/build_offload_reminder.py`, so the
    # hook dir is on sys.path and `import _offload_log` (the dedup log) resolves.
    if str(HOOK.parent) not in sys.path:
        sys.path.insert(0, str(HOOK.parent))
    spec = importlib.util.spec_from_file_location("build_offload_reminder", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _run_main(mod, cmd, root):
    # Force the gating predicates so the test exercises only the matcher +
    # exemption + block, not live sequencer state.
    mod._in_sections = lambda r: True
    mod._recent_checks_runner_dispatch = lambda r: False
    mod._repo_root = lambda: root
    payload = {"tool_name": "Bash", "tool_input": {"command": cmd}}
    old = (sys.stdin, sys.stdout, sys.stderr)
    sys.stdin, sys.stdout, sys.stderr = (io.StringIO(json.dumps(payload)),
                                         io.StringIO(), io.StringIO())
    try:
        rc = mod.main()
        out, err = sys.stdout.getvalue(), sys.stderr.getvalue()
    finally:
        sys.stdin, sys.stdout, sys.stderr = old
    return rc, out, err


def test_wrapped_command_is_exempt():
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        rc, out, err = _run_main(
            mod,
            "bash scripts/overnight/run-artifact.sh build1 -- bash scripts/build.sh",
            root)
        assert rc == 0 and out.strip() == "" and err.strip() == "", (rc, out, err)


def test_bare_command_blocks_with_reroute():
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        rc, out, err = _run_main(mod, "bash scripts/build.sh", root)
        assert rc == 2, "P3.4: bare build in SECTIONS must BLOCK (exit 2): " + repr(rc)
        assert "build-offload BLOCK" in err and "run-artifact.sh" in err, err


def test_wrapped_test_and_smoke_also_exempt():
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        for inner in ("bash scripts/test.sh SUITE=mm", "bash scripts/test-smoke.sh"):
            rc, out, err = _run_main(
                mod, f"bash scripts/overnight/run-artifact.sh lbl -- {inner}", root)
            assert rc == 0 and err.strip() == "", f"{inner!r} wrapped must be exempt"


def test_block_is_idempotent_but_log_dedups():
    # P3.4: every bare attempt keeps blocking (reroute), but the offload-events
    # log dedups so a repeat does not spam it (prereq 3 now gates logging only).
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        rc1, _, err1 = _run_main(mod, "bash scripts/build.sh", root)
        rc2, _, err2 = _run_main(mod, "bash scripts/build.sh", root)
        assert rc1 == 2 and rc2 == 2, (rc1, rc2)          # both block
        assert "build-offload BLOCK" in err1 and "build-offload BLOCK" in err2
        log = root / ".claude/state/offload-events.jsonl"
        entries = [json.loads(x) for x in log.read_text().splitlines() if x.strip()]
        builds = [e for e in entries if e.get("detail") == "bash scripts/build.sh"]
        assert len(builds) == 1, f"log must dedup the repeat: {entries}"
        # A different script blocks too and logs its own entry.
        rc3, _, err3 = _run_main(mod, "bash scripts/test.sh SUITE=mm", root)
        assert rc3 == 2 and "build-offload BLOCK" in err3


def test_message_has_no_checks_runner_text():
    # P3.4 prereq 2: the deprecated checks-runner routing text is removed.
    mod = _load()
    assert "checks-runner" not in mod._MSG, mod._MSG


def test_lint_is_exempt_from_block():
    # J2b: lint is cheap -> not in the BLOCK matcher, so a bare lint does NOT block.
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        rc, out, err = _run_main(mod, "bash scripts/lint.sh", root)
        assert rc == 0 and err.strip() == "", (rc, err)


if __name__ == "__main__":
    test_wrapped_command_is_exempt()
    test_bare_command_blocks_with_reroute()
    test_wrapped_test_and_smoke_also_exempt()
    test_block_is_idempotent_but_log_dedups()
    test_message_has_no_checks_runner_text()
    test_lint_is_exempt_from_block()
    print("PASS: build_offload_reminder exemption + P3.4 BLOCK + dedup + lint-exempt")
