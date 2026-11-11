#!/usr/bin/env python3
"""The verdict rule of scripts/overnight/ci-wait.py: what counts as green, red, still running and unknown."""
import importlib.util
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("ci_wait", HERE.parent / "ci-wait.py")
cw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cw)


def run(status, conclusion=None, created="2026-11-11T10:00:00Z", rid=1):
    return {"databaseId": rid, "status": status, "conclusion": conclusion, "createdAt": created}


def test_green_needs_every_run_finished_and_successful():
    both = {"build.yml": [run("completed", "success")], "todo-graph.yml": [run("completed", "success")]}
    assert cw.verdict(both, 10, 180) == ("green", {"passed": ["build.yml", "todo-graph.yml"], "not_triggered": []})


def test_a_queued_or_running_run_is_not_a_verdict():
    for status in ("queued", "in_progress"):
        state, detail = cw.verdict({"build.yml": [run(status)], "todo-graph.yml": [run("completed", "success")]}, 900, 180)
        assert state == "running" and detail["waiting_on"] == ["build.yml"], (status, state, detail)


def test_a_missing_run_is_waited_for_then_counts_as_not_triggered():
    runs = {"build.yml": [run("completed", "success")], "todo-graph.yml": []}
    assert cw.verdict(runs, 60, 180)[0] == "running"
    assert cw.verdict(runs, 181, 180) == ("green", {"passed": ["build.yml"], "not_triggered": ["todo-graph.yml"]})
    # Nothing ran at all: that is no verdict, never a green.
    assert cw.verdict({"build.yml": [], "todo-graph.yml": []}, 181, 180)[0] == "unknown"


def test_a_failure_is_red_and_names_the_run():
    runs = {"build.yml": [run("completed", "failure", rid=77)], "todo-graph.yml": [run("completed", "success")]}
    assert cw.verdict(runs, 10, 180) == ("red", {"failed": {"build.yml": 77}})
    assert cw.verdict({"build.yml": [run("completed", "timed_out", rid=5)]}, 10, 180)[0] == "red"


def test_a_rerun_replaces_the_failure_it_reran():
    runs = {"build.yml": [run("completed", "failure", "2026-11-11T10:00:00Z", 1),
                          run("completed", "success", "2026-11-11T10:30:00Z", 2)]}
    assert cw.verdict(runs, 10, 180)[0] == "green"
    runs["build.yml"].append(run("in_progress", None, "2026-11-11T10:40:00Z", 3))
    assert cw.verdict(runs, 10, 180)[0] == "running"


def test_a_cancelled_run_alone_is_no_verdict():
    assert cw.verdict({"build.yml": [run("completed", "cancelled")]}, 10, 180)[0] == "unknown"
    # A cancelled build is not certified by another workflow passing.
    mixed = {"build.yml": [run("completed", "cancelled")], "todo-graph.yml": [run("completed", "success")]}
    assert cw.verdict(mixed, 10, 180)[0] == "unknown"


def test_the_wait_for_a_run_starts_when_the_commit_is_first_seen():
    import subprocess
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        subprocess.run(["git", "init", "-q", str(root)], check=True)
        first = cw._first_seen(root, "a" * 40)
        assert cw._first_seen(root, "a" * 40) == first       # the same commit keeps its first sighting across calls
        assert cw._first_seen(root, "b" * 40) >= first       # another commit starts its own
        assert cw._first_seen(root, "a" * 40) > first        # and replaces the record


def test_the_report_holds_only_the_last_poll():
    import io
    import json
    from contextlib import redirect_stdout
    polls = iter([{"build.yml": [run("in_progress")]}, {"build.yml": [run("completed", "success")]}])
    saved = (cw._runs, cw._run, cw._first_seen, cw.time.sleep, cw.WORKFLOWS)

    class Done:
        returncode, stderr = 0, ""

        def __init__(self, stdout):
            self.stdout = stdout

    cw._runs = lambda project, sha, budget: next(polls)
    cw._run = lambda cmd, cwd, timeout=60: Done("a" * 40 + "\n" if cmd[1] == "rev-parse" else "a" * 40 + "\trefs/heads/main\n")
    cw._first_seen = lambda project, sha: cw.time.time()
    cw.time.sleep = lambda s: None
    cw.WORKFLOWS = ("build.yml",)
    try:
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = cw.main(["."])
    finally:
        cw._runs, cw._run, cw._first_seen, cw.time.sleep, cw.WORKFLOWS = saved
    report = json.loads(buf.getvalue())
    assert rc == 0 and report["state"] == "green" and "waiting_on" not in report, report


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    for t in tests:
        t()
    print(f"PASS: ci-wait ({len(tests)} tests)")
