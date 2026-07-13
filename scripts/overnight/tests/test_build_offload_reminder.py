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
    # Production runs `python3 .claude/hooks/build_offload_reminder.py`, so the
    # hook dir is on sys.path and `import _offload_log` (used by the dedup log)
    # resolves. importlib.spec_from_file_location does not add it -- do so here.
    if str(HOOK.parent) not in sys.path:
        sys.path.insert(0, str(HOOK.parent))
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


def test_dedup_suppresses_repeat_within_window():
    # P3.4 prereq 3: it fired 9x on one wrapped sequence; a repeat for the SAME
    # script within the window must be suppressed, a different script still fires.
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        _, out1 = _run_main(mod, "bash scripts/build.sh", root)
        assert "build-offload" in out1, out1
        _, out2 = _run_main(mod, "bash scripts/build.sh", root)
        assert out2.strip() == "", "repeat within window must dedup: " + repr(out2)
        _, out3 = _run_main(mod, "bash scripts/test.sh SUITE=mm", root)
        assert "build-offload" in out3, "a different script still fires: " + repr(out3)


def test_message_has_no_checks_runner_text():
    # P3.4 prereq 2: the deprecated checks-runner routing text is removed.
    mod = _load()
    assert "checks-runner" not in mod._MSG, mod._MSG


if __name__ == "__main__":
    test_wrapped_command_is_exempt()
    test_bare_command_still_fires()
    test_wrapped_test_and_smoke_also_exempt()
    test_dedup_suppresses_repeat_within_window()
    test_message_has_no_checks_runner_text()
    print("PASS: build_offload_reminder exemption + dedup + msg-cleanup")
