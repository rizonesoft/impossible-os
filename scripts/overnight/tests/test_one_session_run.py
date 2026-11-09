#!/usr/bin/env python3
"""One session, its stop guard and its heartbeat. tmux is a shim that logs its calls."""
import os
import pathlib
import shutil
import stat
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parents[3]
HOOKS = REPO / ".claude/hooks"
sys.path.insert(0, str(HOOKS))


def _fixture(d, alive=True, idle=0):
    root = pathlib.Path(d) / "repo"
    (root / "scripts/overnight").mkdir(parents=True)
    (root / ".claude/state").mkdir(parents=True)
    for rel in ("scripts/overnight/heartbeat.sh", "scripts/overnight/run-common.sh"):
        shutil.copy(REPO / rel, root / rel)
    (root / "scripts/overnight/supervisor.py").write_text(
        'print(\'{"repair_needed": %s}\' % ("true" if __import__("os").environ.get("STALLED") else "false"))\n')
    (root / "scripts/overnight/notify.sh").write_text("echo notified >> \"$LOG\"\n")
    (root / ".claude/state/sequencer-armed").write_text("x\n")
    bindir = pathlib.Path(d) / "bin"
    bindir.mkdir()
    log = pathlib.Path(d) / "calls.log"
    tmux = bindir / "tmux"
    tmux.write_text('#!/bin/bash\necho "tmux $*" >> "$LOG"\n'
                    '[ "$1" = has-session ] && exit "${SESSION_RC:-0}"\n'
                    '[ "$1" = list-clients ] && echo "${CLIENTS:-}"\nexit 0\n')
    tmux.chmod(tmux.stat().st_mode | stat.S_IEXEC)
    tdir = pathlib.Path(d) / "transcripts"
    tdir.mkdir()
    (root / ".claude/state/run-session-id").write_text("abc\n")
    os.utime(root / ".claude/state/run-session-id", (time.time() - idle, time.time() - idle))
    # A newer transcript from an attended session must not read as this run working.
    t = tdir / "abc.jsonl"
    (tdir / "attended.jsonl").write_text("{}\n")
    t.write_text("{}\n")
    os.utime(t, (time.time() - idle, time.time() - idle))
    env = {**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}", "LOG": str(log),
           "SESSION_RC": "0" if alive else "1", "HEARTBEAT_TRANSCRIPTS": str(tdir),
           "RUN_NOTIFY_ENV": "/nonexistent"}
    return root, log, env


def _once(root, env):
    r = subprocess.run(["bash", "scripts/overnight/heartbeat.sh", "--once"], cwd=root, env=env,
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r
    return r


def test_a_working_session_is_left_alone():
    with tempfile.TemporaryDirectory() as d:
        root, log, env = _fixture(d, idle=60)
        _once(root, env)
        calls = log.read_text()
        assert "send-keys" not in calls and "new-session" not in calls, calls


def test_a_dead_session_restarts_with_continue_and_alerts():
    with tempfile.TemporaryDirectory() as d:
        root, log, env = _fixture(d, alive=False)
        _once(root, env)
        calls = log.read_text()
        assert "new-session" in calls and "--resume abc" in calls and "notified" in calls, calls
        assert "--continue" not in calls, "--continue could resume an attended session"
        assert "OVERNIGHT_SEQUENCER_RUN=1" in calls and "CLAUDE_CODE_AUTO_COMPACT_WINDOW=400000" in calls
        # ChromeMCP stays off for this repo's run (the systemd units set this until 2026-11-07).
        assert "MCP_NO_AUTO_CHROME=1" in calls and "MCP_NO_AUTO_BRIDGE=1" in calls, calls


def test_an_id_that_never_became_a_conversation_starts_fresh():
    with tempfile.TemporaryDirectory() as d:
        root, log, env = _fixture(d, alive=False)
        (pathlib.Path(d) / "transcripts/abc.jsonl").unlink()
        _once(root, env)
        calls = log.read_text()
        assert "--session-id" in calls and "--resume" not in calls, calls


def test_refusal_undo_is_not_offered_while_someone_is_attached():
    with tempfile.TemporaryDirectory() as d:
        root, log, env = _fixture(d, idle=4000)
        (root / "scripts/overnight/supervisor.py").write_text(
            'import sys,os\nopen(os.environ["LOG"],"a").write("supervisor %s\\n" % sys.argv[1:])\nprint("{}")\n')
        _once(root, {**env, "CLIENTS": "/dev/pts/3: x"})
        assert "--idle" not in log.read_text(), log.read_text()
        _once(root, env)
        assert "--idle" in log.read_text(), log.read_text()


def test_refusal_a_session_that_keeps_dying_backs_off():
    with tempfile.TemporaryDirectory() as d:
        root, log, env = _fixture(d, alive=False)
        _once(root, env)
        _once(root, env)
        assert log.read_text().count("new-session") == 1, log.read_text()


def test_an_idle_session_is_nudged():
    with tempfile.TemporaryDirectory() as d:
        root, log, env = _fixture(d, idle=4000)
        _once(root, env)
        assert "Continue the run." in log.read_text()


def test_refusal_a_person_attached_is_never_typed_over():
    with tempfile.TemporaryDirectory() as d:
        root, log, env = _fixture(d, idle=4000)
        _once(root, {**env, "CLIENTS": "/dev/pts/3: impossible-os-run"})
        assert "send-keys" not in log.read_text(), log.read_text()


def _real_tmux(*args, sock):
    return subprocess.run(["tmux", "-L", sock, *args], capture_output=True, text=True, timeout=20)


def test_real_tmux_a_live_heartbeat_does_not_make_a_dead_run_look_alive():
    """The shim cannot model prefix matching, which is exactly what broke this."""
    if not shutil.which("tmux"):
        return
    sock = f"impossible-test-{os.getpid()}"
    try:
        assert _real_tmux("new-session", "-d", "-s", "impossible-os-run-x", "sleep 60", sock=sock).returncode == 0
        assert _real_tmux("has-session", "-t", "impossible-os-run", sock=sock).returncode == 0, \
            "the control: a bare name matches by prefix"
        assert _real_tmux("has-session", "-t", "=impossible-os-run", sock=sock).returncode != 0
        assert _real_tmux("has-session", "-t", "=impossible-os-run-x", sock=sock).returncode == 0
        text = (REPO / "scripts/overnight/run-common.sh").read_text()
        assert "RUN_HEARTBEAT_SESSION:-impossible-os-heartbeat" in text
        for rel in ("scripts/run.sh", "scripts/overnight/heartbeat.sh"):
            assert '-t "$RUN_SESSION"' not in (REPO / rel).read_text(), rel
    finally:
        _real_tmux("kill-server", sock=sock)


def test_refusal_the_run_cannot_stop_itself():
    r = subprocess.run(["bash", str(REPO / "scripts/run.sh"), "stop"], capture_output=True, text=True,
                       env={**os.environ, "OVERNIGHT_SEQUENCER_RUN": "1"}, timeout=30)
    assert r.returncode == 1 and "cannot stop itself" in r.stderr, r
    import run_phase_guard as g
    assert g._is_self_teardown("bash scripts/run.sh stop")


def test_refusal_every_frozen_file_is_refused_through_the_whole_path():
    import gate
    import run_phase_guard as g
    state = {"active": True, "phase": "SECTIONS"}
    os.environ["OVERNIGHT_SEQUENCER_RUN"] = "1"
    try:
        for rel in sorted(gate.FROZEN):
            target = str(REPO / rel)
            allow, msg = g.evaluate("Edit", {"file_path": target}, state, armed=True,
                                    headless=True)
            assert not allow and msg.startswith("[SEQ-CONTROL-PLANE]"), rel
        # The rest of the machinery is the run's to repair.
        allow, _ = g.evaluate("Edit", {"file_path": str(REPO / ".claude/hooks/section_commit_gate.py")},
                              state, armed=True, headless=True)
        assert allow
    finally:
        del os.environ["OVERNIGHT_SEQUENCER_RUN"]


def test_a_stalled_idle_session_is_told_to_diagnose():
    with tempfile.TemporaryDirectory() as d:
        root, log, env = _fixture(d, idle=4000)
        _once(root, {**env, "STALLED": "1"})
        calls = log.read_text()
        assert "find out why" in calls and "Continue the run." not in calls, calls


def test_refusal_a_stopped_run_is_not_restarted():
    with tempfile.TemporaryDirectory() as d:
        root, log, env = _fixture(d, alive=False)
        (root / ".claude/state/sequencer-armed").unlink()
        _once(root, env)
        assert not log.exists() or "new-session" not in log.read_text()
    with tempfile.TemporaryDirectory() as d:
        root, log, env = _fixture(d, alive=False)
        (root / ".claude/state/sequencer-fixpoint").write_text("done\n")
        _once(root, env)
        assert not log.exists() or "new-session" not in log.read_text()


def _autostart_fixture(d):
    """A copy of the run's scripts with tmux and systemctl as logging shims."""
    root, log, env = _fixture(d, alive=False)
    for rel in ("scripts/run.sh", "scripts/overnight/skill_denies.py",
                ".claude/hooks/run_phase_guard.py", ".claude/settings.json"):
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(REPO / rel, root / rel)
    bindir = pathlib.Path(d) / "bin"
    sysctl = bindir / "systemctl"
    sysctl.write_text('#!/bin/bash\necho "systemctl $*" >> "$LOG"\nexit 0\n')
    sysctl.chmod(sysctl.stat().st_mode | stat.S_IEXEC)
    claude = bindir / "claude"
    claude.write_text("#!/bin/bash\nexit 0\n")
    claude.chmod(claude.stat().st_mode | stat.S_IEXEC)
    units = pathlib.Path(d) / "units"
    return root, log, {**env, "RUN_SYSTEMD_DIR": str(units)}, units


def _autostart(root, env):
    r = subprocess.run(["bash", "scripts/run.sh", "autostart"], cwd=root, env=env,
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r
    return r


def test_autostart_resumes_a_live_run_and_installs_the_boot_service():
    with tempfile.TemporaryDirectory() as d:
        root, log, env, units = _autostart_fixture(d)
        _autostart(root, env)
        calls = log.read_text()
        assert "new-session -d -s impossible-os-run" in calls and "--resume abc" in calls, calls
        unit = (units / "impossible-os-run.service").read_text()
        assert "RemainAfterExit=yes" in unit, "the tmux server must outlive ExecStart"
        assert f'ExecStart=/bin/bash "{root}/scripts/run.sh" autostart' in unit, unit
        assert f'Environment="PATH={env["PATH"]}"' in unit, unit
        assert "systemctl --user enable impossible-os-run.service" in calls, calls


def test_refusal_start_without_claude_on_path_does_not_claim_started():
    """A desktop shortcut runs a non-login bash with no ~/.local/bin: the pane died at once
    while `start` printed "started" (2026-11-09)."""
    with tempfile.TemporaryDirectory() as d:
        root, log, env, _ = _autostart_fixture(d)
        (pathlib.Path(d) / "bin/claude").unlink()
        tools = pathlib.Path(d) / "tools"
        tools.mkdir()
        for name in ("tmux", "systemctl", "git", "python3", "clang-19", "nasm", "ld.lld-19",
                     "bash", "awk", "date", "rm", "mkdir", "cat", "mktemp", "cmp", "mv", "sed"):
            src = pathlib.Path(d) / "bin" / name
            src = src if src.exists() else shutil.which(name)
            if src:
                (tools / name).symlink_to(src)
        r = subprocess.run(["bash", "scripts/run.sh", "start"], cwd=root,
                           env={**env, "PATH": str(tools)}, capture_output=True, text=True, timeout=60)
        assert r.returncode == 1 and "REFUSED: claude is not on PATH" in r.stderr, r
        assert "started" not in r.stdout, r
        assert not log.exists() or "new-session" not in log.read_text()


def test_refusal_autostart_leaves_a_stopped_or_finished_run_down():
    with tempfile.TemporaryDirectory() as d:
        root, log, env, units = _autostart_fixture(d)
        (root / ".claude/state/sequencer-armed").unlink()
        assert "no live run" in _autostart(root, env).stdout
        assert not log.exists() or "new-session" not in log.read_text()
    with tempfile.TemporaryDirectory() as d:
        root, log, env, units = _autostart_fixture(d)
        (root / ".claude/state/sequencer-fixpoint").write_text("done\n")
        assert "no live run" in _autostart(root, env).stdout
        assert not log.exists() or "new-session" not in log.read_text()


def test_refusal_autostart_leaves_a_finished_bounded_run_down():
    """A bounded run that shipped its sections keeps its marker, so the bound must be read."""
    with tempfile.TemporaryDirectory() as d:
        root, log, env, units = _autostart_fixture(d)
        (root / ".claude/hooks/run_phase_guard.py").write_text(
            "import sys\nsys.exit(0 if sys.argv[1:] == ['ship-limit-reached'] else 1)\n")
        assert "no live run" in _autostart(root, env).stdout
        assert not log.exists() or "new-session" not in log.read_text()


def test_the_boot_service_escapes_systemd_specifiers():
    """A `%` or `$` passed through raw would be expanded by systemd at boot."""
    with tempfile.TemporaryDirectory() as d:
        root, log, env, units = _autostart_fixture(d)
        weird = str(pathlib.Path(d) / "x%h$Y")
        _autostart(root, {**env, "PATH": f"{env['PATH']}:{weird}"})
        unit = (units / "impossible-os-run.service").read_text()
        assert "x%%h$Y" in unit and "x%h$Y" not in unit.replace("%%", ""), unit


def _stop(armed=True, fixpoint=False, oracle="NEEDS_WORK", rollover=False):
    import run_phase_guard as g
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        g.ARMED_MARKER = root / "armed"
        g.FIXPOINT_SENTINEL = root / "fix"
        g.STATE_PATH = root / "state.json"
        if armed:
            g.ARMED_MARKER.write_text("x")
        if fixpoint:
            g.FIXPOINT_SENTINEL.write_text("x")
        g._live_oracle_status = lambda: oracle
        g.repo_root = lambda: root
        g.save_state({"active": True, "rollover": {"pending": rollover, "epoch": time.time()}})
        os.environ["OVERNIGHT_SEQUENCER_RUN"] = "1"
        try:
            return g.handle_stop()
        finally:
            del os.environ["OVERNIGHT_SEQUENCER_RUN"]


def ROW(n, status):
    return f"| s | {n} | Section {n} | none | [{status}] |\n"


def _bounded(d):
    root = pathlib.Path(d)
    def git(*a):
        subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@t", *a],
                       check=True, capture_output=True)
    git("init", "-q")
    origin = root / "origin.git"
    subprocess.run(["git", "init", "-q", "--bare", str(origin)], check=True)
    (root / ".gitignore").write_text("origin.git\n.claude/\n")
    (root / "todo").mkdir()
    (root / "todo/T.md").write_text(ROW(1, " "))
    git("add", "-A")
    git("commit", "-qm", "start")
    branch = subprocess.run(["git", "-C", str(root), "branch", "--show-current"],
                            capture_output=True, text=True).stdout.strip()
    git("remote", "add", "origin", str(origin))
    git("push", "-q", "-u", "origin", branch)
    start = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                           capture_output=True, text=True).stdout.strip()
    (root / ".claude/state").mkdir(parents=True)
    (root / ".claude/state/run-ship-limit").write_text(f"1 {start}\n")
    return root, git


def test_a_bounded_run_stops_only_once_the_section_is_shipped_and_pushed():
    import run_phase_guard as g
    with tempfile.TemporaryDirectory() as d:
        root, git = _bounded(d)
        assert not g.ship_limit_reached(root), "nothing shipped yet"
        (root / "todo/T.md").write_text(ROW(1, "/") + "> **Verified:** a\n")
        git("add", "-A")
        git("commit", "-qm", "work in progress")
        git("push", "-q")
        assert not g.ship_limit_reached(root), "a row in progress is not a ship"
        (root / "todo/T.md").write_text(ROW(1, "x") + "> **Verified:** a\n> **Quality reviewed:** b\n")
        git("add", "-A")
        git("commit", "-qm", "ship")
        assert not g.ship_limit_reached(root), "not pushed yet"
        git("push", "-q")
        assert g.ship_limit_reached(root)


def test_git_colour_settings_do_not_hide_a_ship():
    import run_phase_guard as g
    with tempfile.TemporaryDirectory() as d:
        root, git = _bounded(d)
        git("config", "color.ui", "always")
        (root / "todo/T.md").write_text(ROW(1, "x"))
        git("add", "-A")
        git("commit", "-qm", "ship")
        git("push", "-q")
        assert g.ship_limit_reached(root)


def test_refusal_correcting_a_stamp_ships_nothing():
    import run_phase_guard as g
    with tempfile.TemporaryDirectory() as d:
        root, git = _bounded(d)
        (root / "todo/T.md").write_text(ROW(1, "x") + "> **Quality reviewed:** old\n")
        git("add", "-A")
        git("commit", "-qm", "before the run")
        git("push", "-q")
        start = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                               capture_output=True, text=True).stdout.strip()
        (root / ".claude/state/run-ship-limit").write_text(f"1 {start}\n")
        (root / "todo/T.md").write_text(ROW(1, "x") + "> **Quality reviewed:** corrected\n")
        git("add", "-A")
        git("commit", "-qm", "correct a stamp")
        git("push", "-q")
        assert not g.ship_limit_reached(root)


def test_refusal_editing_a_shipped_row_is_not_a_ship():
    import run_phase_guard as g
    with tempfile.TemporaryDirectory() as d:
        root, git = _bounded(d)
        (root / "todo/T.md").write_text(ROW(1, "x"))
        git("add", "-A")
        git("commit", "-qm", "shipped earlier")
        git("push", "-q")
        start = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                               capture_output=True, text=True).stdout.strip()
        (root / ".claude/state/run-ship-limit").write_text(f"1 {start}\n")
        (root / "todo/T.md").write_text(ROW(1, "x").replace("Section 1", "Renamed"))
        git("add", "-A")
        git("commit", "-qm", "rename the row")
        git("push", "-q")
        assert not g.ship_limit_reached(root)


def test_a_reopened_section_shipping_again_reaches_the_bound():
    import run_phase_guard as g
    with tempfile.TemporaryDirectory() as d:
        root, git = _bounded(d)
        (root / "todo/T.md").write_text(ROW(1, "/") + "> **Quality reviewed:** august\n")
        git("add", "-A")
        git("commit", "-qm", "reopened")
        git("push", "-q")
        start = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                               capture_output=True, text=True).stdout.strip()
        (root / ".claude/state/run-ship-limit").write_text(f"1 {start}\n")
        (root / "todo/T.md").write_text(ROW(1, "x") + "> **Quality reviewed:** november\n")
        git("add", "-A")
        git("commit", "-qm", "ship again")
        git("push", "-q")
        assert g.ship_limit_reached(root)


def test_rows_in_both_real_layouts_are_read():
    sys.path.insert(0, str(REPO / "scripts/overnight"))
    import ship_count
    badge = ("+++ b/todo/a/TODO-04.md\n"
             "-| \u2b50  |  56   | Fork publication | \u00a749 |  [/]   |\n"
             "+| \u2b50  |  56   | Fork publication | \u00a749 |  [x]   |\n")
    numeric_first = ("+++ b/todo/b/TODO-14.md\n"
                     "-| 6 | Late-phase harness | \u00a75 | [ ] |\n"
                     "+| 6 | Late-phase harness | \u00a75 | [x] |\n")
    assert ship_count.ships_in_diff(badge) == 1
    assert ship_count.ships_in_diff(numeric_first) == 1
    # REFUSAL: a table row with no section number or no status is not a section.
    assert ship_count.ships_in_diff("+++ b/todo/c.md\n-| a | b | [ ] |\n+| a | b | [x] |\n") == 0


def test_a_section_reopened_and_shipped_inside_the_run_counts():
    # Review, 2026-11-08: endpoints show [x] at both ends and read no ship.
    import run_phase_guard as g
    with tempfile.TemporaryDirectory() as d:
        root, git = _bounded(d)
        (root / "todo/T.md").write_text(ROW(1, "x"))
        git("add", "-A")
        git("commit", "-qm", "shipped before the run")
        git("push", "-q")
        start = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                               capture_output=True, text=True).stdout.strip()
        (root / ".claude/state/run-ship-limit").write_text(f"1 {start}\n")
        for status, msg in (("/", "reopen"), ("x", "ship again")):
            (root / "todo/T.md").write_text(ROW(1, status))
            git("add", "-A")
            git("commit", "-qm", msg)
        git("push", "-q")
        assert g.ship_limit_reached(root)


def test_the_real_first_run_counts_as_one_ship():
    """Known answer: the 2026-11-08 bounded run shipped TODO-04 s56 between these."""
    sys.path.insert(0, str(REPO / "scripts/overnight"))
    import ship_count
    probe = subprocess.run(["git", "-C", str(REPO), "cat-file", "-e", "200486a0b"],
                           capture_output=True)
    if probe.returncode == 0:
        assert ship_count.ships_between(REPO, "6b2eae2ac", "200486a0b") == 1


def test_refusal_an_unbounded_run_or_a_bad_limit_never_reads_as_reached():
    import run_phase_guard as g
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        assert not g.ship_limit_reached(root)
        (root / ".claude/state").mkdir(parents=True)
        (root / ".claude/state/run-ship-limit").write_text("garbage\n")
        assert not g.ship_limit_reached(root)


def test_the_bound_reaches_the_prompt_and_only_when_set():
    common = (REPO / "scripts/overnight/run-common.sh").read_text()
    assert 'prompt="$prompt$(run_limit_note)"' in common
    run = (REPO / "scripts/run.sh").read_text()
    start = run[run.index("  start)"):run.index("  attach)")]
    assert 'rm -f .claude/state/sequencer-fixpoint "$RUN_SHIP_LIMIT"' not in start, \
        "a restart must keep the run's bound"
    assert "keeping this run's bound" in start
    stop = run[run.index("  stop)"):]
    assert 'rm -f "$RUN_MARKER" "$RUN_SHIP_LIMIT"' in stop, "an old limit must not bound the next run"
    r = subprocess.run(["bash", str(REPO / "scripts/run.sh"), "start", "--ships", "0"],
                       capture_output=True, text=True, timeout=60,
                       env={**os.environ, "RUN_SESSION": "no-such-test-session"})
    assert r.returncode == 2 and "usage" in r.stderr, r


def test_stop_guard_holds_the_session_until_the_work_is_done():
    assert _stop() == 2
    # A verified rollover used to end the session for a relaunch. Nothing relaunches now.
    assert _stop(rollover=True) == 2
    assert _stop(fixpoint=True) == 0
    assert _stop(armed=False) == 0
    assert _stop(oracle="DONE") == 0
    assert _stop(oracle="BLOCKED") == 0


def test_run_script_shape():
    text = (REPO / "scripts/run.sh").read_text()
    for verb in ("start)", "attach)", "status)", "stop)", "autostart)"):
        assert verb in text, verb
    subprocess.run(["bash", "-n", str(REPO / "scripts/run.sh")], check=True)


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("OK")
