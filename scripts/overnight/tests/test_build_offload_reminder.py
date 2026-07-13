#!/usr/bin/env python3
"""Tests for build_offload_reminder.py run-artifact.sh exemption.

Codex audit 2026-07-13 (verified): the matcher fires on the INNER
`bash scripts/build.sh` even when it is correctly wrapped as
`bash scripts/overnight/run-artifact.sh <label> -- bash scripts/build.sh`
(it fired 9x on one properly-wrapped sequence). Promoting the reminder to a
BLOCK as-is would block the sanctioned route, so a command carrying an OUTER
run-artifact.sh wrapper must be exempt; a bare command must still fire.
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
    spec = importlib.util.spec_from_file_location("build_offload_reminder", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _run_main(mod, cmd, root):
    # Force the gating predicates so the test exercises only the matcher +
    # exemption, not live sequencer state.
    mod._in_sections = lambda r: True
    mod._recent_checks_runner_dispatch = lambda r: False
    mod._repo_root = lambda: root
    payload = {"tool_name": "Bash", "tool_input": {"command": cmd}}
    old_in, old_out = sys.stdin, sys.stdout
    sys.stdin = io.StringIO(json.dumps(payload))
    sys.stdout = io.StringIO()
    try:
        rc = mod.main()
        out = sys.stdout.getvalue()
    finally:
        sys.stdin, sys.stdout = old_in, old_out
    return rc, out


def test_wrapped_command_is_exempt():
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        rc, out = _run_main(
            mod,
            "bash scripts/overnight/run-artifact.sh build1 -- bash scripts/build.sh",
            root)
        assert rc == 0, rc
        assert out.strip() == "", "wrapped route must NOT fire: " + repr(out)


def test_bare_command_still_fires():
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        rc, out = _run_main(mod, "bash scripts/build.sh", root)
        assert rc == 0, rc
        assert "build-offload" in out, "bare route must fire: " + repr(out)


def test_wrapped_test_and_smoke_also_exempt():
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        for inner in ("bash scripts/test.sh SUITE=mm", "bash scripts/test-smoke.sh"):
            rc, out = _run_main(
                mod, f"bash scripts/overnight/run-artifact.sh lbl -- {inner}", root)
            assert out.strip() == "", f"{inner!r} wrapped must not fire: {out!r}"


if __name__ == "__main__":
    test_wrapped_command_is_exempt()
    test_bare_command_still_fires()
    test_wrapped_test_and_smoke_also_exempt()
    print("PASS: build_offload_reminder run-artifact.sh exemption")
