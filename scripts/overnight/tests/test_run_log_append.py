#!/usr/bin/env python3
"""A section ship must reach docs/overnight/run-log.md before the rollover.

The run-log append was the ADVANCE step's only unguarded instruction. MEASURED
2026-08-03: six consecutive section ships left NO entry, and one commit is
subject-lined as writing the run-log entry for a ship while its diff touches
exactly two files, neither of them the log. The claim and the diff disagreed and
nothing caught it -- the section-commit gate binds build/test/smoke and review
receipts to the SHIP, and `rollover` checks tree-clean, pushed and receipts, but
nothing checked that the cursor's file gained a log line.

It hid itself, too: the gap is only visible if you grep the log for sections you
already know shipped. The cost is not tokens -- the run-log is what an operator
reads to reconstruct a night, and that stretch has to come out of `git log`.

Keyed on the COMMIT HASH, which the log already records as `SHIPPED <hash>` and
which, unlike a section number, cannot drift or be spelled three ways. The check
found a real un-logged ship on its first live run.
"""
from __future__ import annotations

import importlib.util
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
HOOK = REPO / ".claude/hooks/run_phase_guard.py"

IO_ROW = "| feat | 1 | S1 | thing | -- | {} |"


def _load():
    if str(HOOK.parent) not in sys.path:
        sys.path.insert(0, str(HOOK.parent))
    spec = importlib.util.spec_from_file_location("run_phase_guard", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _repo(tmp):
    d = pathlib.Path(tmp)
    g = ["git", "-C", str(d)]
    subprocess.run(["git", "init", "-q", str(d)], check=True)
    (d / "todo").mkdir()
    (d / "docs/overnight").mkdir(parents=True)
    (d / "todo/T.md").write_text(IO_ROW.format("[ ]") + "\n")
    (d / "docs/overnight/run-log.md").write_text("# run log\n")
    subprocess.run(g + ["add", "-A"], check=True)
    subprocess.run(g + ["-c", "user.email=t@t", "-c", "user.name=t",
                        "commit", "-qm", "base"], check=True)
    return d, g


def _ship(d, g, subject="feat: ship the section", n=1):
    # each ship flips its OWN row, so a second ship is a real diff
    p = d / "todo/T.md"
    rows = p.read_text().rstrip("\n").split("\n")
    rows = [r for r in rows if f"| {n} |" not in r]
    rows.append(IO_ROW.format("[x]").replace("| 1 |", f"| {n} |"))
    p.write_text("\n".join(rows) + "\n")
    subprocess.run(g + ["add", "-A"], check=True)
    subprocess.run(g + ["-c", "user.email=t@t", "-c", "user.name=t",
                        "commit", "-qm", subject], check=True)
    return subprocess.run(g + ["rev-parse", "HEAD"],
                          capture_output=True, text=True).stdout.strip()


def _log(d, g, text):
    p = d / "docs/overnight/run-log.md"
    p.write_text(p.read_text() + text + "\n")
    subprocess.run(g + ["add", "-A"], check=True)
    subprocess.run(g + ["-c", "user.email=t@t", "-c", "user.name=t",
                        "commit", "-qm", "doc: run-log"], check=True)


def test_an_unlogged_ship_is_reported():
    mod = _load()
    with tempfile.TemporaryDirectory() as t:
        d, g = _repo(t)
        sha = _ship(d, g)
        found = mod._unlogged_ships(d)
        assert len(found) == 1, found
        assert sha[:10] in found[0], found


def test_a_logged_ship_is_not_reported():
    mod = _load()
    with tempfile.TemporaryDirectory() as t:
        d, g = _repo(t)
        sha = _ship(d, g)
        _log(d, g, f"- 2026-08-04 | T | SHIPPED `{sha[:8]}` | done")
        assert mod._unlogged_ships(d) == []


def test_the_review_half_of_a_ship_pair_is_not_a_ship():
    """A `review:` commit re-touches the same rows; it owes no entry of its own."""
    mod = _load()
    with tempfile.TemporaryDirectory() as t:
        d, g = _repo(t)
        sha = _ship(d, g)
        _log(d, g, f"- 2026-08-04 | T | SHIPPED `{sha[:8]}` | done")
        p2 = d / "todo/T.md"; p2.write_text(p2.read_text() + "> **Verified:** ok\n")
        subprocess.run(g + ["add", "-A"], check=True)
        subprocess.run(g + ["-c", "user.email=t@t", "-c", "user.name=t",
                            "commit", "-qm", "review: T section 1"], check=True)
        assert mod._unlogged_ships(d) == []


def test_a_non_ship_commit_is_not_reported():
    mod = _load()
    with tempfile.TemporaryDirectory() as t:
        d, g = _repo(t)
        (d / "todo/T.md").write_text(IO_ROW.format("[ ]") + "\nnotes\n")
        subprocess.run(g + ["add", "-A"], check=True)
        subprocess.run(g + ["-c", "user.email=t@t", "-c", "user.name=t",
                            "commit", "-qm", "docs: notes"], check=True)
        assert mod._unlogged_ships(d) == []


def test_the_horizon_bounds_the_obligation_to_this_run():
    """A long pre-existing gap must not be re-reported forever.

    Everything older than the newest hash the log names is either recorded or
    predates the obligation; only ships after it are owed.
    """
    mod = _load()
    with tempfile.TemporaryDirectory() as t:
        d, g = _repo(t)
        old = _ship(d, g, "feat: old ship", n=1)          # never logged
        _log(d, g, f"- 2026-08-04 | T | SHIPPED `{old[:8]}` | logged")
        new = _ship(d, g, "feat: new ship", n=2)          # unlogged, after the horizon
        found = mod._unlogged_ships(d)
        assert len(found) == 1 and new[:10] in found[0], found


def test_it_never_wedges_a_run_on_io_error():
    """An audit-trail check must fail OPEN. A missing log is an operator
    problem; refusing every rollover over it would be worse than the gap."""
    mod = _load()
    with tempfile.TemporaryDirectory() as t:
        d, g = _repo(t)
        _ship(d, g)
        (d / "docs/overnight/run-log.md").unlink()
        assert mod._unlogged_ships(d) == []
        assert mod._unlogged_ships(pathlib.Path(t) / "does-not-exist") == []


if __name__ == "__main__":
    test_an_unlogged_ship_is_reported()
    test_a_logged_ship_is_not_reported()
    test_the_review_half_of_a_ship_pair_is_not_a_ship()
    test_a_non_ship_commit_is_not_reported()
    test_the_horizon_bounds_the_obligation_to_this_run()
    test_it_never_wedges_a_run_on_io_error()
    print("PASS: a section ship owes a run-log entry before rollover")
