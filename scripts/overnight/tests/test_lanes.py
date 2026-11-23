"""The lane map and the commit check (automation-hardening TODO), through the real hooks in throwaway repositories.

Each repository has real linked worktrees for its lanes and runs the repository's own .githooks/pre-commit and
prepare-commit-msg (core.hooksPath points at a copy holding only those two and their lib), so a refusal is the
hook's. An allowed commit is judged by the same `lanes.py check-staged` the hook runs, because past the lane
check the real pre-commit goes on to the full lint, which a throwaway repository cannot carry.
"""
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
LANES_PY = REPO / "scripts" / "lanes.py"
TRIAGE_PY = REPO / ".claude" / "hooks" / "sequencer_triage.py"
GIT = ["git", "-c", "user.email=t@example.com", "-c", "user.name=T"]

MAP = """stale_secs = 180
generated = ["COUNT.md"]

[lane.main]
engine = "claude"
model = "opus"
worktree = "."
paths = []
output = "commits"
limits = { memory = "1G", cpu = "100%" }

[lane.userland]
engine = "claude"
model = "opus"
worktree = "../wt-userland"
paths = ["user/", "src/shell/", "todo/11-apps/"]
output = "commits"
limits = { memory = "1G", cpu = "100%" }

[lane.neon-b]
engine = "agy"
model = "m"
worktree = "../wt-neon-b"
paths = []
output = "reports"
limits = { memory = "1G", cpu = "100%" }
"""


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


lanes = _load("lanes_under_test", LANES_PY)


def git(cwd, *args, check=True, env=None):
    return subprocess.run([*GIT, "-C", str(cwd), *args], capture_output=True, text=True, timeout=60,
                          check=check, env=env)


@pytest.fixture
def repo(tmp_path, monkeypatch):
    state = tmp_path / "state"
    monkeypatch.setenv("XDG_STATE_HOME", str(state))
    primary = tmp_path / "primary"
    (primary / "scripts").mkdir(parents=True)
    (primary / ".conclave").mkdir()
    (primary / ".conclave" / "lanes.toml").write_text(MAP)
    shutil.copy(LANES_PY, primary / "scripts" / "lanes.py")
    for d in ("user", "src/shell", "src/kernel", "todo/11-apps"):
        (primary / d).mkdir(parents=True, exist_ok=True)
    (primary / "user" / "a.c").write_text("a\n")
    (primary / "src" / "shell" / "cmds.c").write_text("c\n")
    (primary / "src" / "kernel" / "k.c").write_text("k\n")
    (primary / "COUNT.md").write_text("0\n")
    git(tmp_path, "init", "-q", "-b", "main", str(primary))
    git(primary, "add", "-A")
    git(primary, "commit", "-qm", "seed")
    # The hooks find the repository root as their own parent, so they sit in this repository's .githooks
    # (untracked: never part of a staged change).
    hooks = primary / ".githooks"
    (hooks / "lib").mkdir(parents=True)
    for name in ("pre-commit", "prepare-commit-msg"):
        shutil.copy(REPO / ".githooks" / name, hooks / name)
    for lib in (REPO / ".githooks" / "lib").iterdir():
        shutil.copy(lib, hooks / "lib" / lib.name)
    git(primary, "worktree", "add", "-q", "--detach", str(tmp_path / "wt-userland"))
    git(primary, "worktree", "add", "-q", "--detach", str(tmp_path / "wt-neon-b"))
    return {"tmp": tmp_path, "primary": primary, "userland": tmp_path / "wt-userland",
            "neon-b": tmp_path / "wt-neon-b", "hooks": hooks, "state": state}


def live(name, pid=None, age=0.0):
    d = lanes.state_dir()
    d.mkdir(parents=True, exist_ok=True)
    (d / f"{name}.json").write_text(json.dumps(
        {"lane": name, "pid": pid or os.getpid(), "heartbeat": time.time() - age, "task": "t"}))


def check(tree):
    return subprocess.run([sys.executable, str(tree / "scripts" / "lanes.py"), "check-staged"], cwd=tree,
                          capture_output=True, text=True, timeout=60, env=os.environ.copy())


def hooked_commit(r, tree, *args):
    git(tree, "config", "core.hooksPath", str(r["hooks"]))
    return git(tree, "commit", "-qm", "x", *args, check=False, env=os.environ.copy())


def stage(tree, rel, text="changed\n"):
    p = tree / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)
    git(tree, "add", "--", rel)


def test_a_commit_into_a_live_lanes_path_is_refused_and_passes_with_the_lane_off(repo):
    stage(repo["primary"], "user/a.c")
    live("userland")
    r = hooked_commit(repo, repo["primary"])
    assert r.returncode != 0 and "user/a.c belongs to lane userland, which is live" in r.stderr, r
    lanes.clear("userland")
    assert check(repo["primary"]).returncode == 0


def test_a_generated_file_always_passes(repo):
    live("userland")
    stage(repo["primary"], "COUNT.md")
    assert check(repo["primary"]).returncode == 0
    stage(repo["userland"], "COUNT.md")
    assert check(repo["userland"]).returncode == 0


def test_a_stale_heartbeat_or_a_dead_pid_is_a_lane_that_is_off(repo):
    m = lanes.load_map(repo["primary"])
    live("userland", age=181)
    assert lanes.lane_state(m, "userland") == lanes.OFF
    dead = subprocess.Popen(["true"])
    dead.wait()
    live("userland", pid=dead.pid)
    assert lanes.lane_state(m, "userland") == lanes.OFF
    live("userland")
    assert lanes.lane_state(m, "userland") == lanes.LIVE


def test_a_damaged_record_reserves_against_main_and_grants_the_lane_nothing(repo):
    d = lanes.state_dir()
    d.mkdir(parents=True)
    (d / "userland.json").write_text('{"lane": "userland", "pid": ')
    stage(repo["primary"], "src/shell/cmds.c")
    r = hooked_commit(repo, repo["primary"])
    assert r.returncode != 0 and "reserved (its record cannot be read)" in r.stderr, r
    stage(repo["userland"], "user/a.c")
    r = check(repo["userland"])
    assert r.returncode == 1 and "its record cannot be read" in r.stderr, r


def test_a_reports_lane_never_commits_not_even_an_empty_or_generated_commit(repo):
    live("neon-b")
    r = hooked_commit(repo, repo["neon-b"], "--allow-empty")
    assert r.returncode != 0 and "lane neon-b only writes reports" in r.stderr, r
    stage(repo["neon-b"], "COUNT.md")
    r = hooked_commit(repo, repo["neon-b"])
    assert r.returncode != 0 and "lane neon-b only writes reports" in r.stderr, r


def test_an_extra_lane_commits_only_inside_its_paths_and_only_while_live(repo):
    live("userland")
    stage(repo["userland"], "src/kernel/k.c")
    r = hooked_commit(repo, repo["userland"])
    assert r.returncode != 0 and "src/kernel/k.c is outside lane userland's paths" in r.stderr, r
    git(repo["userland"], "reset", "-q")
    stage(repo["userland"], "src/shell/cmds.c")
    assert check(repo["userland"]).returncode == 0
    live("userland", age=181)                  # the reservation expired: main may now edit these files
    r = check(repo["userland"])
    assert r.returncode == 1 and "lane userland is not live (it has no fresh record)" in r.stderr, r


def test_a_rename_out_of_a_live_lanes_path_is_refused(repo):
    live("userland")
    git(repo["primary"], "mv", "src/shell/cmds.c", "src/kernel/cmds.c")
    r = hooked_commit(repo, repo["primary"])
    assert r.returncode != 0 and "src/shell/cmds.c belongs to lane userland" in r.stderr, r


def test_a_revert_into_a_live_lanes_path_is_refused(repo):
    stage(repo["primary"], "user/a.c", "two\n")
    git(repo["primary"], "commit", "-qm", "edit")       # authored with userland off (no hooks yet)
    live("userland")
    git(repo["primary"], "config", "core.hooksPath", str(repo["hooks"]))
    r = git(repo["primary"], "revert", "--no-edit", "HEAD", check=False, env=os.environ.copy())
    assert r.returncode != 0 and "user/a.c belongs to lane userland" in r.stderr, r
    assert git(repo["primary"], "log", "-1", "--format=%s").stdout.strip() == "edit"


def test_the_skip_needs_a_stated_reason(repo, monkeypatch):
    live("userland")
    stage(repo["primary"], "user/a.c")
    monkeypatch.setenv("SKIP_LANE_CHECK", "1")
    assert check(repo["primary"]).returncode == 1
    monkeypatch.setenv("SKIP_LANE_CHECK_REASON", "operator repair")
    r = check(repo["primary"])
    assert r.returncode == 0 and "operator repair" in r.stderr, r


@pytest.mark.parametrize("edit,why", [
    (lambda t: t.replace('paths = ["user/"', 'paths = ["src/"'), "overlaps"),
    (lambda t: t.replace('worktree = "."\npaths = []', 'worktree = "."\npaths = ["x/"]', 1), "owns the remainder"),
    (lambda t: t.replace("stale_secs = 180", "stale_secs = 180\nbogus = 1"), "unknown keys"),
    (lambda t: t.replace('output = "reports"', 'output = "logs"'), "output must be one of"),
])
def test_a_map_that_does_not_validate_refuses_every_commit(repo, edit, why):
    toml = repo["primary"] / ".conclave" / "lanes.toml"
    toml.write_text(edit(toml.read_text()))
    stage(repo["primary"], "src/kernel/k.c")
    r = check(repo["primary"])
    assert r.returncode == 2 and why in r.stderr, r
    toml.unlink()
    r = check(repo["primary"])
    assert r.returncode == 2 and "cannot be read" in r.stderr, r


# Triage: a section a live lane holds is never offered to main, and held work is BLOCKED, never DONE.

SECTIONS = """# T

## 1. Kernel only

- [ ] change `src/kernel/k.c`

## 2. Uptime command

- [ ] add the command in `src/shell/cmds.c`
"""


@pytest.fixture
def triage(repo, monkeypatch):
    t = _load("triage_under_test", TRIAGE_PY)
    root = repo["primary"]
    shutil.copy(REPO / "scripts" / "todo_fence.py", root / "scripts" / "todo_fence.py")
    (root / "todo" / "00-x").mkdir(parents=True)
    (root / "todo" / "00-x" / "TODO-01-x.md").write_text(SECTIONS)
    (root / "todo" / "11-apps" / "TODO-01-app.md").write_text("# A\n\n## 1. App\n\n- [ ] x\n")
    entries = {"todo/00-x/TODO-01-x.md": [(1, t.NEEDS_WORK), (2, t.NEEDS_WORK)],
               "todo/11-apps/TODO-01-app.md": [(1, t.NEEDS_WORK)]}
    order = [{"file_path": f, "domain": f.split("/")[1], "sections": []} for f in entries]
    monkeypatch.setattr(t, "traversal_order", lambda cache: [e for e in order if e["file_path"] in entries])
    monkeypatch.setattr(t, "classify_file", lambda e, root: (t.NEEDS_WORK, list(entries[e["file_path"]])))
    monkeypatch.setattr(t, "owed_reviews", lambda root: (frozenset(), []))
    monkeypatch.setattr(t, "runnable_sections", lambda e, per, ready, blocked=frozenset(), view=None:
                        [n for n, c in per if c == t.NEEDS_WORK])
    return t, str(root), entries


def test_triage_holds_a_lanes_todo_and_a_section_that_names_a_lane_path(triage):
    t, root, _ = triage
    live("userland")
    held = []
    entry, cls, _w = t.pick_next({}, root, ready=None, reserved=held)
    assert entry["file_path"] == "todo/00-x/TODO-01-x.md" and cls == t.NEEDS_WORK
    assert {"file": "todo/00-x/TODO-01-x.md", "section": 2, "lane": "userland"} in held
    assert {"file": "todo/11-apps/TODO-01-app.md", "section": 1, "lane": "userland"} in held
    per, _h = t.lane_reservations(entry, [(1, t.NEEDS_WORK), (2, t.NEEDS_WORK)], root, t._lane_context(root)[0])
    assert per == [(1, t.NEEDS_WORK), (2, t.BLOCKED)]


def test_triage_reports_blocked_never_done_when_only_lane_work_remains_and_resumes_when_it_stops(triage):
    t, root, entries = triage
    del entries["todo/00-x/TODO-01-x.md"]
    live("userland")
    held = []
    entry, cls, _w = t.pick_next({}, root, ready=None, reserved=held)
    assert entry is None and cls == t.BLOCKED and held, (cls, held)
    lanes.clear("userland")
    entry, cls, _w = t.pick_next({}, root, ready=None)
    assert entry["file_path"] == "todo/11-apps/TODO-01-app.md" and cls == t.NEEDS_WORK
