#!/usr/bin/env python3
"""Regression tests for overnight-launch driver selection and report layout."""
from __future__ import annotations

import os
import pathlib
import stat
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
LAUNCH = REPO / "scripts" / "overnight" / "overnight-launch.sh"
SUPERVISOR = REPO / "scripts" / "overnight" / "codex-sequencer-supervisor.sh"


def _write_exe(path: pathlib.Path, text: str) -> None:
    path.write_text(text, encoding="ascii")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def _run(project: pathlib.Path, driver: str, bin_path: pathlib.Path, out_dir: pathlib.Path):
    env = dict(os.environ)
    env["OVERNIGHT_REPORT_KEEP"] = "3"
    env["OUT_DIR"] = str(out_dir)
    if driver == "codex":
        env["CODEX_BIN"] = str(bin_path)
        env["OVERNIGHT_CODEX_SUPERVISOR_SMOKE"] = "1"
    else:
        env["CLAUDE_BIN"] = str(bin_path)
        env["OVERNIGHT_NO_CHROMEMCP"] = "1"
    return subprocess.run(
        [
            "bash",
            str(LAUNCH),
            str(project),
            "todo/TODO-Claude-Overnight-Runner.md",
            "bypassPermissions",
            driver,
        ],
        text=True,
        capture_output=True,
        env=env,
        timeout=30,
    )


def test_codex_driver_uses_codex_runtime_and_guard_env() -> None:
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        out_dir = root / "out"
        bin_dir = root / "bin"
        project = root / "project"
        out_dir.mkdir()
        bin_dir.mkdir()
        (project / ".codex").mkdir(parents=True)
        (project / "todo").mkdir()
        (project / "todo" / "TODO-Claude-Overnight-Runner.md").write_text("# runner\n", encoding="ascii")
        fake = bin_dir / "codex"
        _write_exe(fake, """#!/usr/bin/env bash
printf '%s\n' "$@" > "$OUT_DIR/codex.args"
{
  echo "OVERNIGHT_DRIVER=$OVERNIGHT_DRIVER"
  echo "OVERNIGHT_SEQUENCER_RUN=$OVERNIGHT_SEQUENCER_RUN"
  echo "AI_WORKFLOW_CODEX_DRIVER=$AI_WORKFLOW_CODEX_DRIVER"
  echo "AI_WORKFLOW_ENFORCE_SHARED_GATES=$AI_WORKFLOW_ENFORCE_SHARED_GATES"
} > "$OUT_DIR/codex.env"
printf '%s\n' '{"type":"session.started","session_id":"test"}'
printf '%s\n' '{"type":"exec_command.started","cmd":"python3 .claude/hooks/run_phase_guard.py status"}'
printf '%s\n' '{"type":"item.completed","item":{"type":"message","role":"assistant","content":[{"type":"output_text","text":"Codex report line"}]}}'
printf '%s\n' '{"type":"result","result":"codex done"}'
""")
        proc = _run(project, "codex", fake, out_dir)
        assert proc.returncode == 0, proc.stderr + proc.stdout
        args = (out_dir / "codex.args").read_text(encoding="ascii")
        env = (out_dir / "codex.env").read_text(encoding="ascii")
        assert "exec\n" in args
        assert "--json\n" in args
        assert "--cd\n" in args and f"{project}\n" in args
        assert "--sandbox\ndanger-full-access\n" in args
        assert "--ask-for-approval\nnever\n" in args
        assert args.index("--ask-for-approval\n") < args.index("exec\n")
        assert "--model" not in args and "--effort" not in args
        assert "Codex running as the active overnight steering driver" in args
        assert "bounded supervisor smoke path" in args
        assert "Never ask the operator for permission" in args
        assert "OVERNIGHT_DRIVER=codex" in env
        assert "OVERNIGHT_SEQUENCER_RUN=1" in env
        assert "AI_WORKFLOW_CODEX_DRIVER=1" in env
        assert "AI_WORKFLOW_ENFORCE_SHARED_GATES=1" in env
        latest = project / ".codex" / "overnight" / "reports" / "latest.log"
        assert latest.exists()
        report = latest.read_text(encoding="utf-8")
        assert "driver: codex" in report
        assert "runtime: .codex/overnight" in report
        assert "codex_sandbox: danger-full-access" in report
        assert "codex-supervisor: codex step: smoke" in report
        assert "tool: Bash  python3 .claude/hooks/run_phase_guard.py status" in report
        assert "Codex report line" in report
        assert "=== final ===" in report and "codex done" in report
        assert not (project / ".claude" / "overnight").exists()


def test_claude_driver_remains_default_runtime() -> None:
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        out_dir = root / "out"
        bin_dir = root / "bin"
        project = root / "project"
        out_dir.mkdir()
        bin_dir.mkdir()
        (project / "todo").mkdir(parents=True)
        (project / "todo" / "TODO-Claude-Overnight-Runner.md").write_text("# runner\n", encoding="ascii")
        fake = bin_dir / "claude"
        _write_exe(fake, """#!/usr/bin/env bash
printf '%s\n' "$@" > "$OUT_DIR/claude.args"
printf '%s\n' '{"type":"assistant","message":{"content":[{"type":"text","text":"Claude report line"}]}}'
printf '%s\n' '{"type":"result","result":"claude done"}'
""")
        proc = _run(project, "claude", fake, out_dir)
        assert proc.returncode == 0, proc.stderr + proc.stdout
        args = (out_dir / "claude.args").read_text(encoding="ascii")
        assert "-p\n" in args
        assert "--permission-mode\nbypassPermissions\n" in args
        assert "Invoke the overnight-sequencer skill" in args
        latest = project / ".claude" / "overnight" / "reports" / "latest.log"
        assert latest.exists()
        report = latest.read_text(encoding="utf-8")
        assert "driver: claude" in report
        assert "runtime: .claude/overnight" in report
        assert "Claude report line" in report
        assert "claude done" in report
        assert not (project / ".codex" / "overnight").exists()


def test_codex_supervisor_block_file_noops_before_codex_invocation() -> None:
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        project = root / "project"
        out_dir = root / "out"
        out_dir.mkdir()
        (project / ".codex" / "overnight").mkdir(parents=True)
        (project / "todo").mkdir()
        (project / "todo" / "TODO-Claude-Overnight-Runner.md").write_text("# runner\n", encoding="ascii")
        (project / ".codex" / "overnight" / "codex-supervisor.block").write_text(
            "reason=fixture\n", encoding="ascii"
        )
        fake = root / "codex"
        _write_exe(fake, """#!/usr/bin/env bash
touch "$OUT_DIR/codex.invoked"
exit 99
""")
        env = dict(os.environ)
        env["OUT_DIR"] = str(out_dir)
        env["OVERNIGHT_CODEX_SUPERVISOR_SMOKE"] = "1"
        proc = subprocess.run(
            [
                "bash",
                str(SUPERVISOR),
                str(project),
                "todo/TODO-Claude-Overnight-Runner.md",
                str(fake),
                "danger-full-access",
            ],
            text=True,
            capture_output=True,
            env=env,
            timeout=10,
        )
        assert proc.returncode == 0, proc.stderr + proc.stdout
        assert "codex-supervisor: blocked:" in proc.stdout
        assert not (out_dir / "codex.invoked").exists()


def test_codex_supervisor_rejects_malformed_numeric_env() -> None:
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        project = root / "project"
        project.mkdir()
        fake = root / "codex"
        _write_exe(fake, "#!/usr/bin/env bash\nexit 0\n")
        env = dict(os.environ)
        env["OVERNIGHT_CODEX_SUPERVISOR_STEPS"] = "nope"
        proc = subprocess.run(
            [
                "bash",
                str(SUPERVISOR),
                str(project),
                "todo/TODO-Claude-Overnight-Runner.md",
                str(fake),
                "danger-full-access",
            ],
            text=True,
            capture_output=True,
            env=env,
            timeout=10,
        )
        assert proc.returncode == 2
        assert "OVERNIGHT_CODEX_SUPERVISOR_STEPS must be a positive integer" in proc.stderr


if __name__ == "__main__":
    test_codex_driver_uses_codex_runtime_and_guard_env()
    test_claude_driver_remains_default_runtime()
    test_codex_supervisor_block_file_noops_before_codex_invocation()
    test_codex_supervisor_rejects_malformed_numeric_env()
    print("PASS: overnight launch driver modes")
