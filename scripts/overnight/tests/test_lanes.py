"""The lane map and the commit check (automation-hardening TODO), through the real hooks in throwaway repositories.

Each repository has real linked worktrees for its lanes and runs the repository's own .githooks/pre-commit and
prepare-commit-msg (core.hooksPath points at a copy holding only those two and their lib), so a refusal is the
hook's, and an allowed commit lands through the same hooks (the lint, which has its own suite, is stubbed).
"""
import importlib.util
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
LANES_PY = REPO / "scripts" / "lanes.py"
TRIAGE_PY = REPO / ".claude" / "hooks" / "sequencer_triage.py"
GIT = ["git", "-c", "user.email=t@example.com", "-c", "user.name=T"]

GENERATED = 'generated = ["COUNT.md", "README.md"]\n'
MAP = """stale_secs = 180
generated = ["COUNT.md", "README.md"]

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


def run(argv, cwd=None, input=None, env=None, timeout=60, check=False):
    """The command in its own process group, ended whole on a timeout: git starts hooks, checks and editors."""
    p = subprocess.Popen(argv, cwd=cwd, env=env, text=True, start_new_session=True, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, stdin=subprocess.PIPE if input is not None else subprocess.DEVNULL)
    try:
        out, err = p.communicate(input, timeout=timeout)
    except BaseException:
        os.killpg(p.pid, signal.SIGKILL)
        p.communicate()
        raise
    r = subprocess.CompletedProcess(argv, p.returncode, out, err)
    if check:
        r.check_returncode()
    return r


def git(cwd, *args, check=True, env=None, input=None):
    return run([*GIT, "-C", str(cwd), *args], check=check, env=env, input=input)


@pytest.fixture
def repo(tmp_path, monkeypatch):
    state = tmp_path / "state"
    monkeypatch.setenv("XDG_STATE_HOME", str(state))
    primary = tmp_path / "primary"
    (primary / "scripts").mkdir(parents=True)
    (primary / ".conclave").mkdir()
    (primary / ".conclave" / "lanes.toml").write_text(MAP)
    shutil.copy(LANES_PY, primary / "scripts" / "lanes.py")
    # The rest of pre-commit's chain, so an allowed commit lands through the real hooks: the dash gate runs
    # for real, and the lint (with its own suite) is stubbed.
    for helper in ("staged_dash_check.py", "bounded_run.py"):
        shutil.copy(REPO / "scripts" / helper, primary / "scripts" / helper)
    (primary / "scripts" / "lint.sh").write_text("exit 0\n")
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
    for name in ("pre-commit", "prepare-commit-msg", "commit-msg", "pre-applypatch"):
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


def check(tree, timeout=60):
    return run([sys.executable, str(tree / "scripts" / "lanes.py"), "check-staged"], cwd=tree, timeout=timeout,
               env=os.environ.copy())


def hooked_commit(r, tree, *args):
    git(tree, "config", "core.hooksPath", str(r["hooks"]))
    return git(tree, "commit", "-qm", "x", *args, check=False, env=os.environ.copy())


def head(tree):
    return git(tree, "log", "-1", "--format=%s").stdout.strip()


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
    assert head(repo["primary"]) == "seed"
    lanes.clear("userland")
    r = hooked_commit(repo, repo["primary"])
    assert r.returncode == 0 and head(repo["primary"]) == "x", r


def test_a_generated_file_always_passes(repo):
    live("userland")
    stage(repo["primary"], "COUNT.md")
    r = hooked_commit(repo, repo["primary"])
    assert r.returncode == 0 and head(repo["primary"]) == "x", r
    stage(repo["userland"], "COUNT.md", "userland\n")
    r = hooked_commit(repo, repo["userland"])
    assert r.returncode == 0 and head(repo["userland"]) == "x", r


def test_a_lane_that_starts_while_the_message_is_written_holds_its_paths_at_commit_msg(repo):
    # pre-commit judged userland off; the lane started before the message was saved.
    stage(repo["primary"], "user/a.c")
    git(repo["primary"], "config", "core.hooksPath", str(repo["hooks"]))
    starter = repo["tmp"] / "start_userland.py"
    starter.write_text(f"import sys\nsys.path.insert(0, {str(LANES_PY.parent)!r})\nimport lanes\n"
                       f"lanes.beat('userland', {os.getpid()}, 't')\n")
    editor = repo["tmp"] / "editor.sh"
    editor.write_text(f'#!/bin/sh\n"{sys.executable}" "{starter}" || exit 1\necho msg > "$1"\n')
    editor.chmod(0o755)
    r = git(repo["primary"], "commit", check=False, env={**os.environ, "GIT_EDITOR": str(editor)})
    assert r.returncode != 0 and "user/a.c belongs to lane userland, which is live" in r.stderr, r
    assert head(repo["primary"]) == "seed"


def test_a_mail_patch_into_a_live_lanes_path_is_refused_and_applies_with_the_lane_off(repo):
    stage(repo["primary"], "user/a.c", "patched\n")
    git(repo["primary"], "commit", "-qm", "patch")             # authored before any hook is installed
    patch = git(repo["primary"], "format-patch", "-1", "--stdout").stdout
    git(repo["primary"], "reset", "-q", "--hard", "HEAD~1")
    git(repo["primary"], "config", "core.hooksPath", str(repo["hooks"]))
    live("userland")
    r = run([*GIT, "-C", str(repo["primary"]), "am"], input=patch,
                       timeout=60, env=os.environ.copy())
    assert r.returncode != 0 and "user/a.c belongs to lane userland" in r.stderr, r
    assert head(repo["primary"]) == "seed"
    git(repo["primary"], "am", "--abort")
    lanes.clear("userland")
    r = run([*GIT, "-C", str(repo["primary"]), "am"], input=patch,
                       timeout=60, env=os.environ.copy())
    assert r.returncode == 0 and head(repo["primary"]) == "patch", r


def test_a_reports_lane_never_creates_a_merge_commit(repo):
    nb = repo["neon-b"]
    git(nb, "checkout", "-q", "-b", "side")
    stage(nb, "COUNT.md", "side\n")
    git(nb, "commit", "-qm", "side")                          # before any hook is installed
    stage(repo["primary"], "src/kernel/k.c")
    git(repo["primary"], "commit", "-qm", "main")
    git(nb, "config", "core.hooksPath", str(repo["hooks"]))
    live("neon-b")
    r = git(nb, "merge", "--no-edit", "main", check=False, env=os.environ.copy())
    assert r.returncode != 0 and "lane neon-b only writes reports" in r.stderr, r
    assert head(nb) == "side"


def test_a_revert_read_through_ps_is_judged_too(repo, monkeypatch):
    stage(repo["primary"], "user/a.c", "two\n")
    git(repo["primary"], "commit", "-qm", "edit")
    live("userland")
    git(repo["primary"], "config", "core.hooksPath", str(repo["hooks"]))
    monkeypatch.setenv("PREPARE_COMMIT_MSG_PROC_ROOT", str(repo["tmp"] / "no-proc"))
    r = run([*GIT, "-C", str(repo["primary"]), "revert", "--no-edit", "HEAD"], timeout=60, env=os.environ.copy())
    assert r.returncode != 0 and "user/a.c belongs to lane userland" in r.stderr, r
    assert head(repo["primary"]) == "edit"


def test_a_path_spelled_from_the_root_is_a_lane_path_and_a_nested_one_is_not(repo):
    m = lanes.committed_map(repo["primary"])
    on = {"userland": lanes.LIVE}
    assert lanes.names_reserved_path(m, on, "edit `./user/hello/x.c`") == "userland"
    assert lanes.names_reserved_path(m, on, "edit user/hello/x.c") == "userland"
    assert lanes.names_reserved_path(m, on, "edit vendor/user/hello/x.c") is None
    assert lanes.names_reserved_path(m, on, "edit a./user/x.c") is None


def test_a_stale_heartbeat_or_a_dead_pid_is_a_lane_that_is_off(repo):
    m = lanes.committed_map(repo["primary"])
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
    (lambda t: t.replace('paths = ["user/"', 'paths = ["./user/"'), "paths must be a list"),
    (lambda t: t.replace('"src/shell/"', '"src//shell/"'), "paths must be a list"),
    (lambda t: t.replace('"src/shell/"', '"src/a shell/"'), "paths must be a list"),
    (lambda t: t.replace('worktree = "../wt-neon-b"', 'worktree = "../wt-userland/"'), "is also lane userland"),
])
def test_a_map_that_does_not_validate_never_lands(repo, edit, why):
    toml = repo["primary"] / ".conclave" / "lanes.toml"
    toml.write_text(edit(toml.read_text()))
    stage(repo["primary"], "src/kernel/k.c")
    assert check(repo["primary"]).returncode == 0             # unstaged: the committed map still judges
    git(repo["primary"], "add", "--", ".conclave/lanes.toml")
    r = check(repo["primary"])
    assert r.returncode == 2 and why in r.stderr, r
    git(repo["primary"], "rm", "-qf", "--", ".conclave/lanes.toml")
    r = check(repo["primary"])
    assert r.returncode == 2 and "deletes .conclave/lanes.toml" in r.stderr, r


def test_an_unstaged_map_edit_never_releases_a_live_reservation(repo):
    toml = repo["primary"] / ".conclave" / "lanes.toml"
    toml.write_text(toml.read_text().replace('"src/shell/", ', ""))
    live("userland")
    stage(repo["primary"], "src/shell/cmds.c")
    r = hooked_commit(repo, repo["primary"], "--", "src/shell/cmds.c")
    assert r.returncode != 0 and "src/shell/cmds.c belongs to lane userland" in r.stderr, r


def test_a_replacement_ref_never_hides_a_staged_reserved_path(repo):
    p = repo["primary"]
    stage(p, "user/a.c", "new\n")
    tree = git(p, "write-tree").stdout.strip()
    fake = git(p, "commit-tree", tree, "-m", "fake").stdout.strip()
    git(p, "replace", "HEAD", fake)                            # HEAD now looks as if it already held the edit
    stage(p, "src/kernel/k.c")
    live("userland")
    r = check(p)
    assert r.returncode == 1 and "user/a.c belongs to lane userland" in r.stderr, r


def test_clear_takes_a_lane_name_never_a_path(repo):
    d = lanes.state_dir()
    d.mkdir(parents=True)
    keep = repo["tmp"] / "keep.json"
    keep.write_text("{}")
    rel = os.path.relpath(keep.with_suffix(""), d)
    r = run([sys.executable, str(repo["primary"] / "scripts" / "lanes.py"), "clear", rel])
    assert r.returncode == 2 and keep.exists(), r
    with pytest.raises(lanes.LaneError):
        lanes.clear(rel)
    assert keep.exists()


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
    monkeypatch.setattr(t, "owed_reviews", lambda root, deadline=None: (frozenset(), []))
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


def test_classify_offers_no_section_a_live_lane_holds(triage, capsys, monkeypatch):
    t, root, entries = triage
    monkeypatch.setattr(t, "plan_ready", lambda root, cache: None)
    cache = [{"file_path": "todo/00-x/TODO-01-x.md", "domain": "00-x", "sections": []}]
    live("userland")
    assert t.cmd_classify(cache, root, "todo/00-x/TODO-01-x.md") == 0
    out = json.loads(capsys.readouterr().out)
    cls = {s["n"]: s["class"] for s in out["sections"]}
    assert cls == {1: t.NEEDS_WORK, 2: t.BLOCKED} and out["lane_reserved"], out
    lanes.clear("userland")
    assert t.cmd_classify(cache, root, "todo/00-x/TODO-01-x.md") == 0
    out = json.loads(capsys.readouterr().out)
    assert {s["n"]: s["class"] for s in out["sections"]} == {1: t.NEEDS_WORK, 2: t.NEEDS_WORK}, out


def test_the_reservation_pass_scans_no_todo_classification_already_scanned(triage, monkeypatch):
    t, root, _ = triage
    calls = []
    real = t._scan_sections
    monkeypatch.setattr(t, "_scan_sections", lambda text: calls.append(1) or real(text))
    path = os.path.join(root, "todo/00-x/TODO-01-x.md")
    t.section_stamps(path)
    live("userland")
    per, held = t.lane_reservations({"file_path": "todo/00-x/TODO-01-x.md"},
                                    [(1, t.NEEDS_WORK), (2, t.NEEDS_WORK)], root, t._lane_context(root)[0])
    assert held and len(calls) == 1, calls
    lanes.clear("userland")
    assert t._lane_context(root) == (None, "")                 # nothing held: no context, no second read


def _paused_merge(repo):
    """main mid-merge of a userland branch that changed user/a.c, with userland live and the hooks on."""
    git(repo["userland"], "checkout", "-q", "-b", "ul")
    stage(repo["userland"], "user/a.c", "ul\n")
    git(repo["userland"], "commit", "-qm", "ul")
    stage(repo["primary"], "src/kernel/k.c")
    git(repo["primary"], "commit", "-qm", "main")
    git(repo["primary"], "config", "core.hooksPath", str(repo["hooks"]))
    live("userland")
    git(repo["primary"], "merge", "--no-ff", "--no-commit", "ul", env=os.environ.copy())


@pytest.mark.parametrize("damage,why", [("bad", "cannot be read"), ("delete", "deletes .conclave/lanes.toml")])
def test_a_merge_never_lands_a_broken_lane_map(repo, damage, why):
    _paused_merge(repo)
    if damage == "bad":
        stage(repo["primary"], ".conclave/lanes.toml", "[lane.main\n")
    else:
        git(repo["primary"], "rm", "-q", "--", ".conclave/lanes.toml")
    r = git(repo["primary"], "commit", "--no-edit", check=False, env=os.environ.copy())
    assert r.returncode != 0 and why in r.stderr, r
    assert head(repo["primary"]) == "main"


def test_a_pid_out_of_range_is_never_written_and_a_record_holding_one_reserves_only_its_lane(repo):
    with pytest.raises(lanes.LaneError):
        lanes.beat("userland", 2**31, "t")
    d = lanes.state_dir()
    d.mkdir(parents=True)
    (d / "userland.json").write_text(json.dumps({"lane": "userland", "pid": 2**40, "heartbeat": time.time()}))
    assert lanes.lane_state(lanes.committed_map(repo["primary"]), "userland") == lanes.DAMAGED
    stage(repo["primary"], "src/kernel/k.c")
    assert check(repo["primary"]).returncode == 0
    stage(repo["primary"], "user/a.c")
    r = check(repo["primary"])
    assert r.returncode == 1 and "reserved (its record cannot be read)" in r.stderr, r


def test_triage_schedules_by_the_committed_map_not_an_edit_in_progress(triage):
    t, root, _ = triage
    toml = Path(root) / ".conclave" / "lanes.toml"
    toml.write_text(toml.read_text().replace('"src/shell/", ', ""))
    live("userland")
    held = []
    t.pick_next({}, root, ready=None, reserved=held)
    assert {"file": "todo/00-x/TODO-01-x.md", "section": 2, "lane": "userland"} in held, held


def test_a_lane_that_starts_while_a_merge_message_is_written_holds_the_resolution(repo):
    git(repo["userland"], "checkout", "-q", "-b", "ul")
    stage(repo["userland"], "user/a.c", "ul\n")
    git(repo["userland"], "commit", "-qm", "ul")
    stage(repo["primary"], "src/kernel/k.c")
    git(repo["primary"], "commit", "-qm", "main")
    git(repo["primary"], "config", "core.hooksPath", str(repo["hooks"]))
    git(repo["primary"], "merge", "--no-ff", "--no-commit", "ul", env=os.environ.copy())
    stage(repo["primary"], "user/a.c", "neither parent\n")           # judged while userland is off
    starter = repo["tmp"] / "start_userland.py"
    starter.write_text(f"import sys\nsys.path.insert(0, {str(LANES_PY.parent)!r})\nimport lanes\n"
                       f"lanes.beat('userland', {os.getpid()}, 't')\n")
    editor = repo["tmp"] / "editor.sh"
    editor.write_text(f'#!/bin/sh\n"{sys.executable}" "{starter}" || exit 1\necho msg > "$1"\n')
    editor.chmod(0o755)
    r = git(repo["primary"], "commit", check=False, env={**os.environ, "GIT_EDITOR": str(editor)})
    assert r.returncode != 0 and "user/a.c belongs to lane userland" in r.stderr, r
    assert head(repo["primary"]) == "main"


def _rebase_onto_main(repo, branch_edits):
    """A branch from seed with one commit of `branch_edits`, while main moved src/kernel/k.c on."""
    p = repo["primary"]
    git(p, "checkout", "-q", "-b", "feat")
    for rel, text in branch_edits.items():
        stage(p, rel, text)
    git(p, "commit", "-qm", "feat")
    git(p, "checkout", "-q", "main")
    stage(p, "src/kernel/k.c", "main\n")
    git(p, "commit", "-qm", "main")
    git(p, "checkout", "-q", "feat")
    git(p, "config", "core.hooksPath", str(repo["hooks"]))
    live("userland")
    return git(p, "rebase", "main", check=False, env={**os.environ, "GIT_EDITOR": "true"})


def test_a_clean_rebase_replays_a_commit_into_a_live_lanes_path(repo):
    r = _rebase_onto_main(repo, {"user/a.c": "feat\n"})               # authored while userland was off
    assert r.returncode == 0 and head(repo["primary"]) == "feat", r


def test_an_edit_added_to_a_stopped_rebase_pick_is_judged(repo):
    r = _rebase_onto_main(repo, {"src/kernel/k.c": "feat\n"})
    assert r.returncode != 0                                          # conflict in k.c: the rebase stops
    p = repo["primary"]
    stage(p, "src/kernel/k.c", "resolved\n")
    stage(p, "user/a.c", "extra\n")                                   # an unrelated edit staged with it
    r = git(p, "rebase", "--continue", check=False, env={**os.environ, "GIT_EDITOR": "true"})
    assert r.returncode != 0 and "user/a.c belongs to lane userland" in r.stderr, r


def test_a_worktree_must_be_relative_and_one_tree_is_one_lane(repo):
    toml = repo["primary"] / ".conclave" / "lanes.toml"
    good = toml.read_text()
    toml.write_text(good.replace('worktree = "../wt-neon-b"', f'worktree = "{repo["primary"]}"'))
    with pytest.raises(lanes.LaneError, match="must be relative"):
        lanes.parse_map(toml.read_text())
    (repo["tmp"] / "alias").symlink_to(repo["userland"])
    toml.write_text(good.replace('worktree = "../wt-neon-b"', 'worktree = "../alias"'))
    git(repo["primary"], "add", "--", ".conclave/lanes.toml")
    git(repo["primary"], "commit", "-qm", "alias map")
    stage(repo["primary"], "COUNT.md")
    r = check(repo["primary"])                                        # judged by the alias map now at HEAD
    assert r.returncode == 2 and "name the same worktree" in r.stderr, r


def test_triage_spends_no_git_call_when_no_lane_has_a_record(tmp_path, monkeypatch):
    t = _load("triage_no_git", TRIAGE_PY)
    (tmp_path / "scripts").mkdir()
    shutil.copy(LANES_PY, tmp_path / "scripts" / "lanes.py")          # not a git repository: any git call fails
    monkeypatch.setenv("XDG_STATE_HOME", str(tmp_path / "state"))
    assert t._lane_context(str(tmp_path)) == (None, "")
    (tmp_path / "state" / "impossible-os" / "lanes").mkdir(parents=True)
    (tmp_path / "state" / "impossible-os" / "lanes" / "userland.json").write_text("{}")
    ctx, err = t._lane_context(str(tmp_path))
    assert ctx[1] is None and "cannot be read" in err, err
    with pytest.raises(lanes.LaneError, match="no time left"):
        lanes._git(tmp_path, "status", deadline=time.monotonic() - 1)


def _stopped_pick(repo, pick, upstream, later=None):
    """feat (one pick of `pick`, then optional clean `later` picks) rebased onto main after main committed
    `upstream`; both change src/kernel/k.c, so the rebase stops on the first pick. Userland is live."""
    p = repo["primary"]
    stage(p, "user/a.c", "1\n2\n3\n4\n5\n6\n7\n")
    git(p, "commit", "-qm", "base")
    git(p, "checkout", "-q", "-b", "feat")
    for rel, text in {**pick, "src/kernel/k.c": "feat\n"}.items():
        stage(p, rel, text)
    git(p, "commit", "-qm", "pick")
    for i, (rel, text) in enumerate((later or {}).items()):
        stage(p, rel, text)
        git(p, "commit", "-qm", f"later{i}")
    git(p, "checkout", "-q", "main")
    for rel, text in {**upstream, "src/kernel/k.c": "main\n"}.items():
        stage(p, rel, text)
    git(p, "commit", "-qm", "upstream")
    git(p, "checkout", "-q", "feat")
    git(p, "config", "core.hooksPath", str(repo["hooks"]))
    live("userland")
    r = git(p, "rebase", "main", check=False, env={**os.environ, "GIT_EDITOR": "true"})
    assert r.returncode != 0 and git(p, "rev-parse", "-q", "--verify", "AUTO_MERGE", check=False).stdout, r
    stage(p, "src/kernel/k.c", "resolved\n")
    return p


def test_a_stopped_pick_that_rolls_back_a_live_lanes_file_is_refused(repo):
    p = _stopped_pick(repo, {}, {"user/a.c": "upstream\n"})
    git(p, "checkout", "REBASE_HEAD", "--", "user/a.c")              # the pick's older copy, by mistake
    r = git(p, "rebase", "--continue", check=False, env={**os.environ, "GIT_EDITOR": "true"})
    assert r.returncode != 0 and "user/a.c belongs to lane userland" in r.stderr, r


def test_a_stopped_pick_is_judged_on_every_path_and_later_clean_picks_replay(repo):
    p = _stopped_pick(repo, {"user/a.c": "one\n2\n3\n4\n5\n6\n7\n"}, {"user/a.c": "1\n2\n3\n4\n5\n6\nseven\n"},
                      later={"user/b.c": "later\n"})
    cont = {**os.environ, "GIT_EDITOR": "true"}
    r = git(p, "rebase", "--continue", check=False, env=cont)         # finished by hand: judged on all it changes
    assert r.returncode != 0 and "user/a.c belongs to lane userland" in r.stderr, r
    git(p, "checkout", "HEAD", "--", "user/a.c")                      # left to the lane, the rebase goes on
    r = git(p, "rebase", "--continue", check=False, env=cont)
    assert r.returncode == 0 and head(p) == "later0", r               # the later pick of user/b.c is clean


@pytest.mark.parametrize("stop", ["break", "edit", "exec false"])
def test_an_edit_staged_at_any_rebase_stop_is_judged(repo, stop):
    p = repo["primary"]
    git(p, "checkout", "-q", "-b", "feat")
    for n in ("one", "two"):
        stage(p, f"src/{n}.c")
        git(p, "commit", "-qm", n)
    git(p, "config", "core.hooksPath", str(repo["hooks"]))
    live("userland")
    seq = "s/^pick/edit/;t;" if stop == "edit" else f"1a {stop}"
    env = {**os.environ, "GIT_EDITOR": "true", "GIT_SEQUENCE_EDITOR": f"sed -i '{seq}'"}
    git(p, "rebase", "-i", "--force-rebase", "main", check=False, env=env)
    assert (p / ".git" / "rebase-merge").is_dir()
    stage(p, "user/a.c", "extra\n")
    r = git(p, "rebase", "--continue", check=False, env=env)
    assert r.returncode != 0, r
    assert "user/a.c" not in git(p, "diff", "--name-only", "main", "HEAD").stdout, r


def _merge_into_main(repo, kind):
    """main merging `ul` (userland's user/a.c beside a docs file) in the shape `kind`, with userland live and the
    hooks on. Returns the paused state's tree, or the finished `git merge` for a merge git commits itself."""
    p = repo["primary"]
    git(p, "checkout", "-q", "-b", "ul")
    stage(p, "user/a.c", "ul\n")
    stage(p, "src/kernel/k.c", "ul\n")
    git(p, "commit", "-qm", "ul")
    git(p, "checkout", "-q", "-b", "third", "main")
    stage(p, "docs/third.md", "third\n")
    git(p, "commit", "-qm", "third")
    git(p, "checkout", "-q", "main")
    stage(p, "docs/main.md", "main\n")
    git(p, "commit", "-qm", "main")
    git(p, "config", "core.hooksPath", str(repo["hooks"]))
    args = {"own": ["--no-edit", "ul"], "docs": ["--no-edit", "third"], "octopus": ["--no-edit", "ul", "third"],
            "ours": ["--no-edit", "-s", "ours", "ul"], "resolve": ["--no-edit", "-s", "resolve", "ul"],
            "paused": ["--no-commit", "ul"], "paused-fifo": ["--no-commit", "ul"]}[kind]
    live("userland")
    r = git(p, "merge", "--no-ff", *args, check=False, env=os.environ.copy())
    if not kind.startswith("paused"):
        return r
    if kind == "paused-fifo":
        (p / ".git" / "MERGE_HEAD").unlink()
        os.mkfifo(p / ".git" / "MERGE_HEAD")
    git(p, "checkout", "HEAD", "--", "user/a.c", "src/kernel/k.c")    # every incoming change lost by mistake
    r = run(["bash", str(repo["hooks"] / "pre-commit")], cwd=p,
                       timeout=30, env=os.environ.copy())
    return r


@pytest.mark.parametrize("kind", ["own", "docs", "octopus", "ours", "resolve", "paused", "paused-fifo"])
def test_no_merge_lands_while_a_lane_is_live_however_it_was_made(repo, kind):
    r = _merge_into_main(repo, kind)
    assert r.returncode != 0 and "[lane-check] BLOCK" in r.stderr, r
    assert "belongs to lane userland" in r.stderr or "no merge commits while a lane is live" in r.stderr, r
    assert head(repo["primary"]) == "main"


def test_a_merge_lands_when_no_lane_is_live(repo):
    r = _merge_into_main(repo, "paused")                              # refused with userland live
    assert "no merge commits while a lane is live" in r.stderr, r
    lanes.clear("userland")
    git(repo["primary"], "checkout", "ul", "--", "user/a.c")
    r = hooked_commit(repo, repo["primary"])
    assert r.returncode == 0 and head(repo["primary"]) == "x", r


def test_a_staged_worktree_alias_never_lands_and_a_broken_map_can_be_repaired(repo):
    p = repo["primary"]
    toml = p / ".conclave" / "lanes.toml"
    good = toml.read_text()
    (repo["tmp"] / "alias").symlink_to(repo["userland"])
    toml.write_text(good.replace('worktree = "../wt-neon-b"', 'worktree = "../alias"'))
    git(p, "add", "--", ".conclave/lanes.toml")
    r = hooked_commit(repo, p)
    assert r.returncode != 0 and "name the same worktree" in r.stderr, r
    assert head(p) == "seed"
    git(p, "config", "--unset", "core.hooksPath")
    git(p, "commit", "-qm", "alias map")                              # landed past the hooks
    toml.write_text(good)
    git(p, "add", "--", ".conclave/lanes.toml")
    r = hooked_commit(repo, p)
    assert r.returncode == 0 and head(p) == "x", r                    # the repair is judged by its own map


def test_a_stalled_head_read_never_falls_back_to_the_index(repo, monkeypatch):
    calls = []

    def stalled(root, *args, deadline=None):
        calls.append(args)
        raise lanes.GitStalled("git show could not run (TimeoutExpired)")
    monkeypatch.setattr(lanes, "_git", stalled)
    with pytest.raises(lanes.GitStalled):
        lanes.committed_map(repo["primary"])
    assert calls == [("show", "HEAD:.conclave/lanes.toml")], calls


def test_a_three_way_mail_patch_into_a_live_lane_is_refused_after_a_conflict(repo):
    p = repo["primary"]
    git(p, "checkout", "-q", "-b", "mail")
    stage(p, "user/a.c", "mailed\n")
    stage(p, "src/kernel/k.c", "mail\n")
    git(p, "commit", "-qm", "mail")
    patch = git(p, "format-patch", "-1", "--stdout").stdout
    git(p, "checkout", "-q", "main")
    stage(p, "src/kernel/k.c", "main\n")
    git(p, "commit", "-qm", "main")
    git(p, "config", "core.hooksPath", str(repo["hooks"]))
    live("userland")
    (repo["tmp"] / "m.patch").write_text(patch)
    r = git(p, "am", "-3", str(repo["tmp"] / "m.patch"), check=False, env=os.environ.copy())
    assert r.returncode != 0                                          # the kernel file conflicts
    stage(p, "src/kernel/k.c", "resolved\n")
    r = git(p, "am", "--continue", check=False, env=os.environ.copy())
    assert r.returncode != 0 and "user/a.c belongs to lane userland" in r.stderr, r
    assert head(p) == "main"


def test_triage_holds_every_open_section_while_a_lane_has_a_record_and_the_map_is_unread(repo, tmp_path):
    t = _load("triage_unknown", TRIAGE_PY)
    root = tmp_path / "nogit"                                         # not a repository: every git read fails
    (root / "scripts").mkdir(parents=True)
    (root / ".conclave").mkdir()
    shutil.copy(LANES_PY, root / "scripts" / "lanes.py")
    (root / ".conclave" / "lanes.toml").write_text("")                # a working copy that holds nothing
    live("userland")
    ctx, err = t._lane_context(str(root))
    assert ctx[1] is None and "cannot be read" in err, (ctx, err)
    per, held = t.lane_reservations({"file_path": "todo/x.md"}, [(1, t.NEEDS_WORK), (2, t.DONE)], str(root), ctx)
    assert per == [(1, t.BLOCKED), (2, t.DONE)] and held == [{"file": "todo/x.md", "section": 1, "lane": "unknown"}]


def test_a_path_named_only_in_a_section_heading_is_held(repo):
    t = _load("triage_heading", TRIAGE_PY)
    todo = repo["primary"] / "todo" / "00-x" / "TODO-01-h.md"
    todo.parent.mkdir(parents=True)
    todo.write_text("# T\n\n## 1. Repair src/shell/input.c\n\n- [ ] Make the edit.\n")
    live("userland")
    ctx, err = t._lane_context(str(repo["primary"]))
    per, held = t.lane_reservations({"file_path": "todo/00-x/TODO-01-h.md"}, [(1, t.NEEDS_WORK)],
                                    str(repo["primary"]), ctx)
    assert per == [(1, t.BLOCKED)] and held[0]["lane"] == "userland", (per, held, err)


def test_a_conflicted_root_pick_with_a_live_lanes_file_is_refused(repo):
    p = repo["primary"]
    git(p, "checkout", "-q", "--orphan", "root")
    git(p, "rm", "-rq", "--cached", ".")
    for f in list(p.iterdir()):
        if f.name not in (".git", ".githooks", ".conclave"):
            shutil.rmtree(f) if f.is_dir() else f.unlink()
    git(p, "add", "--", ".conclave/lanes.toml")
    stage(p, "user/new.c", "root\n")
    stage(p, "src/kernel/k.c", "root\n")
    git(p, "commit", "-qm", "root pick")
    git(p, "config", "core.hooksPath", str(repo["hooks"]))
    live("userland")
    r = git(p, "rebase", "--root", "--onto", "main", check=False, env={**os.environ, "GIT_EDITOR": "true"})
    assert r.returncode != 0, r                                       # k.c (and the map) conflict
    stage(p, "src/kernel/k.c", "resolved\n")
    git(p, "checkout", "--ours", "--", ".conclave/lanes.toml", check=False)
    git(p, "add", "--", ".conclave/lanes.toml", check=False)
    r = git(p, "rebase", "--continue", check=False, env={**os.environ, "GIT_EDITOR": "true"})
    assert r.returncode != 0 and "user/new.c belongs to lane userland" in r.stderr, r


def test_main_never_merges_while_only_a_lane_without_paths_is_live(repo):
    p = repo["primary"]
    git(p, "checkout", "-q", "-b", "side")
    stage(p, "docs/side.md", "side\n")
    git(p, "commit", "-qm", "side")
    git(p, "checkout", "-q", "main")
    git(p, "config", "core.hooksPath", str(repo["hooks"]))
    live("neon-b")
    r = git(p, "merge", "--no-ff", "--no-edit", "side", check=False, env=os.environ.copy())
    assert r.returncode != 0 and "no merge commits while a lane is live" in r.stderr, r


def test_a_landed_merge_can_be_amended_and_the_amend_is_judged_on_what_it_changes(repo):
    p = repo["primary"]
    git(p, "checkout", "-q", "-b", "side")
    stage(p, "docs/side.md", "side\n")
    git(p, "commit", "-qm", "side")
    git(p, "checkout", "-q", "main")
    git(p, "merge", "--no-ff", "-m", "landed", "side")                # no lane is live
    git(p, "config", "core.hooksPath", str(repo["hooks"]))
    live("userland")
    r = git(p, "commit", "--amend", "-m", "reworded", check=False, env=os.environ.copy())
    assert r.returncode == 0 and head(p) == "reworded", r
    stage(p, "user/a.c", "slipped in\n")
    r = git(p, "commit", "--amend", "--no-edit", check=False, env=os.environ.copy())
    assert r.returncode != 0 and "user/a.c belongs to lane userland" in r.stderr, r


@pytest.mark.parametrize("edit", [("\"src/shell/\", ", ""), ('worktree = "../wt-userland"', 'worktree = "../elsewhere"'),
                                  ("stale_secs = 180", "stale_secs = 9")])
def test_the_map_does_not_change_while_a_lane_is_live(repo, edit):
    p = repo["primary"]
    toml = p / ".conclave" / "lanes.toml"
    good = toml.read_text()
    assert good.count(edit[0]) == 1, edit
    toml.write_text(good.replace(*edit))
    git(p, "add", "--", ".conclave/lanes.toml")
    live("userland")
    r = check(p)
    assert r.returncode == 1 and "does not change while a lane is live" in r.stderr, r
    toml.write_text(good + "# a comment\n")
    git(p, "add", "--", ".conclave/lanes.toml")
    assert check(p).returncode == 0                                   # the map itself is the same
    toml.write_text(good.replace(*edit))
    git(p, "add", "--", ".conclave/lanes.toml")
    lanes.clear("userland")
    assert check(p).returncode == 0


def test_a_stated_reason_overrides_a_commit_that_cannot_be_judged(repo, monkeypatch):
    git(repo["primary"], "rm", "-q", "--", ".conclave/lanes.toml")
    assert check(repo["primary"]).returncode == 2
    monkeypatch.setenv("SKIP_LANE_CHECK", "1")
    assert check(repo["primary"]).returncode == 2                     # no reason given
    monkeypatch.setenv("SKIP_LANE_CHECK_REASON", "reverting the lane map")
    r = check(repo["primary"])
    assert r.returncode == 0 and "reverting the lane map" in r.stderr and "deletes" in r.stderr, r


@pytest.mark.parametrize("rec", [{"heartbeat": float("nan"), "task": "t"}, {"heartbeat": float("inf"), "task": "t"},
                                 {"heartbeat": "now"}, {"heartbeat": "now", "task": 3}])
def test_a_record_beat_would_not_write_is_damaged(repo, rec):
    rec = {"lane": "userland", "pid": os.getpid(), **rec}
    if rec["heartbeat"] == "now":
        rec["heartbeat"] = time.time()
    d = lanes.state_dir()
    d.mkdir(parents=True)
    (d / "userland.json").write_text(json.dumps(rec))
    assert lanes.lane_state(lanes.committed_map(repo["primary"]), "userland") == lanes.DAMAGED
    stage(repo["userland"], "user/a.c")
    r = check(repo["userland"])
    assert r.returncode == 1 and "its record cannot be read" in r.stderr, r


@pytest.mark.parametrize("text,held", [
    ("[linker script](../../user/user.ld)", "userland"), ("edit `../../src/apps/x.c`", "userland"),
    ("everything in src/apps.", "userland"), ("`src/apps`", "userland"), ("user/x.c", "userland"),
    ("src/apps-old/x", None), ("src/apps.md", None), ("the user starts it", None), ("vendor/user/x", None),
    ("[other](../user/x)", None), ("[apps](../11-apps/TODO-01-app.md)", None),
    ("docs/inv/one.md", "neon-a"), ("docs/inv/one.md.bak", None), ("(docs/inv/one.md).", "neon-a"), ("docs/inv/one.md/x", None),
    ("[linker](./../../user/user.ld)", "userland"), ("./user/x.c", "userland"), ("docs/../user/x.c", "userland"),
    ("[todo/11-apps/TODO-01-app.md](../11-apps/TODO-01-app.md)", None), ("/user/x", None),
    ("https://example.com/user/x", None), ("user/*.c", "userland"), ("edit the Makefile.", "neon-a"),
    ("Makefile.old", None), ("Edit _user/include/test.h_.", "userland"), ("__src/apps__", "userland"),
    ("_edit user/x.c._", "userland"), ("_the Makefile_", "neon-a"), ("a_user/x", None), ("_user_", None)])
def test_the_paths_a_section_names(text, held):
    table = 'engine = "e"\nmodel = "m"\noutput = "commits"\nlimits = {}\n'
    lane_map = lanes.parse_map(
        GENERATED + f'[lane.main]\n{table}worktree = "."\npaths = []\n'
        f'[lane.userland]\n{table}worktree = "../u"\npaths = ["user/", "src/apps/", "todo/11-apps/"]\n'
        f'[lane.neon-a]\n{table}worktree = "../n"\npaths = ["docs/inv/one.md", "Makefile"]\n')
    states = {"userland": lanes.LIVE, "neon-a": lanes.LIVE}
    assert lanes.names_reserved_path(lane_map, states, text, "todo/00-x") == held


def test_triage_holds_everything_when_the_records_cannot_be_listed(triage, monkeypatch):
    t, root, _ = triage
    live("userland")
    d = lanes.state_dir()
    d.chmod(0o311)                                                    # searchable, not listable
    try:
        ctx, err = t._lane_context(root)
    finally:
        d.chmod(0o755)
    assert ctx[1] is None and "cannot be listed" in err, (ctx, err)


def test_a_large_map_is_validated_within_the_commit_checks_budget():
    files = ", ".join(f'"user/f{i}.c"' for i in range(4000))
    table = 'engine = "e"\nmodel = "m"\noutput = "commits"\nlimits = {}\n'
    text = (GENERATED + f'[lane.main]\n{table}worktree = "."\npaths = []\n'
            f'[lane.a]\n{table}worktree = "../a"\npaths = [{files}]\n')
    start = time.monotonic()
    lanes.parse_map(text)
    assert time.monotonic() - start < 1                               # 2.1 s when each pair was compared
    for extra in ('"user/"', '"user/f7.c"'):
        with pytest.raises(lanes.LaneError, match="overlaps"):
            lanes.parse_map(text + f'[lane.b]\n{table}worktree = "../b"\npaths = [{extra}]\n')


def test_one_scanned_todo_is_kept_at_a_time(triage):
    t, root, entries = triage
    for f in entries:
        t._read_scan(os.path.join(root, f))
    assert len(t._SCAN_MEMO) == 1


def test_a_broken_map_is_not_repaired_while_a_lane_has_a_record(repo):
    p = repo["primary"]
    toml = p / ".conclave" / "lanes.toml"
    good = toml.read_text()
    (repo["tmp"] / "alias").symlink_to(repo["userland"])
    for broken in (good.replace('worktree = "../wt-neon-b"', 'worktree = "../alias"').encode(), b"[lane.main\n",
                   good.encode() + b"# \xff\n", None):
        if broken is None:
            toml.unlink()
        else:
            toml.write_bytes(broken)
        git(p, "add", "-A", "--", ".conclave")
        git(p, "commit", "-qm", "broken map")                         # landed past the hooks
        toml.parent.mkdir(exist_ok=True)
        toml.write_text(good.replace('"user/", ', ""))                # the repair also releases user/
        git(p, "add", "--", ".conclave/lanes.toml")
        stage(p, "user/a.c", "main\n")
        live("userland")
        r = check(p)
        assert r.returncode == 2 and "is broken or missing and a lane has a record" in r.stderr, r
        lanes.clear("userland")
        assert check(p).returncode == 0
        git(p, "reset", "-q", "--hard")


@pytest.mark.parametrize("gitdir", ["clone.git", ".git"])
def test_a_tree_whose_git_directory_is_kept_elsewhere_is_never_judged_as_main(repo, gitdir):
    clone = repo["tmp"] / "far" / "clone"
    (repo["tmp"] / "meta" / "scripts").mkdir(parents=True)
    git(repo["tmp"], "clone", "-q", f"--separate-git-dir={repo['tmp'] / 'meta' / gitdir}", str(repo["primary"]),
        str(clone))
    git(clone, "worktree", "add", "-q", "--detach", str(repo["tmp"] / "far" / "wt-neon-b"))
    r = check(repo["tmp"] / "far" / "wt-neon-b")
    assert r.returncode == 2 and "the primary tree cannot be found" in r.stderr, r


def test_scheduling_cost_does_not_grow_with_the_paths_a_live_lane_holds():
    files = ", ".join(f'"user/f{i}.c"' for i in range(4000))
    table = 'engine = "e"\nmodel = "m"\noutput = "commits"\nlimits = {}\n'
    lane_map = lanes.parse_map(GENERATED + f'[lane.main]\n{table}worktree = "."\npaths = []\n'
                               f'[lane.a]\n{table}worktree = "../a"\npaths = [{files}]\n')
    text = "Edit src/kernel/mm/pmm.c and the notes in docs/x.md.\n" * 200
    start = time.monotonic()
    for _ in range(20):
        assert lanes.names_reserved_path(lane_map, {"a": lanes.LIVE}, text, "todo/00-x") is None
    assert time.monotonic() - start < 2                               # 4000 searches a text took over 10 s
    start = time.monotonic()
    assert lanes.judge(lane_map, lanes.MAIN, [f"src/p{i}.c" for i in range(20000)], {"a": lanes.OFF}) == []
    assert time.monotonic() - start < 2


def test_clear_removes_the_record_of_a_lane_that_left_the_map_and_works_while_the_map_is_broken(repo):
    d = lanes.state_dir()
    d.mkdir(parents=True)
    (d / "gone.json").write_text("{}")
    (repo["primary"] / ".conclave" / "lanes.toml").write_text("[lane.main\n")
    argv = [sys.executable, str(repo["primary"] / "scripts" / "lanes.py"), "clear"]
    assert run([*argv, "gone"]).returncode == 0 and not (d / "gone.json").exists()
    assert run([*argv, "main"]).returncode == 2


def test_a_commit_resolves_the_lane_worktrees_once(repo, monkeypatch):
    calls = []
    real = lanes.lane_trees
    monkeypatch.setattr(lanes, "lane_trees", lambda *a: calls.append(1) or real(*a))
    stage(repo["primary"], "src/kernel/k.c")
    assert lanes.check_staged(repo["primary"]) == 0 and len(calls) == 1, calls


def test_blockers_are_collected_while_the_corpus_is_classified(triage, capsys, monkeypatch):
    t, root, entries = triage
    (Path(root) / "todo" / "00-x" / "TODO-01-x.md").write_text(
        "# T\n\n## 1. One\n\n> **Deferred:** 2026-11-24 awaiting-hardware a board\n\n- [ ] src/shell/x.c\n")
    entries["todo/00-x/TODO-01-x.md"] = [(1, t.BLOCKED)]
    live("userland")                                                  # holds the other file
    scans = []
    real = t._scan_sections
    monkeypatch.setattr(t, "_scan_sections", lambda text: scans.append(1) or real(text))
    assert t.cmd_next({}, root) == 0
    out = json.loads(capsys.readouterr().out)
    assert out["status"] == t.BLOCKED and out["lane_reserved"], out
    assert out["blockers"] == [{"file": "todo/00-x/TODO-01-x.md", "section": 1, "awaiting": "awaiting-hardware"}]
    assert len(scans) == 1, scans


def test_a_fifo_in_place_of_a_record_is_damaged_and_holds_nothing_up(repo):
    d = lanes.state_dir()
    d.mkdir(parents=True)
    os.mkfifo(d / "userland.json")
    stage(repo["primary"], "src/kernel/k.c")
    assert check(repo["primary"], timeout=20).returncode == 0
    stage(repo["primary"], "user/a.c")
    r = check(repo["primary"], timeout=20)
    assert r.returncode == 1 and "reserved (its record cannot be read)" in r.stderr, r


def test_an_unstaged_deletion_of_the_map_blocks_no_linked_worktree(repo):
    (repo["primary"] / ".conclave" / "lanes.toml").unlink()
    live("userland")
    stage(repo["userland"], "user/a.c")
    r = check(repo["userland"])
    assert r.returncode == 0, r


def test_a_worktree_that_cannot_be_resolved_refuses_and_can_be_repaired(repo, monkeypatch):
    p = repo["primary"]
    toml = p / ".conclave" / "lanes.toml"
    good = toml.read_text()
    toml.write_text(good.replace('worktree = "../wt-neon-b"', 'worktree = "../loop/x"'))
    git(p, "add", "--", ".conclave/lanes.toml")
    git(p, "commit", "-qm", "loop map")
    real = Path.resolve

    def resolve(self, *a, **k):
        if "loop" in self.parts:
            raise RuntimeError("Symlink loop")                        # Python 3.11 and 3.12; newer ones resolve it
        return real(self, *a, **k)
    monkeypatch.setattr(Path, "resolve", resolve)
    stage(p, "src/kernel/k.c")
    assert lanes.check_staged(p) == 2
    monkeypatch.setenv("SKIP_LANE_CHECK", "1")
    monkeypatch.setenv("SKIP_LANE_CHECK_REASON", "r")
    assert lanes.check_staged(p) == 0
    monkeypatch.delenv("SKIP_LANE_CHECK")
    toml.write_text(good)
    git(p, "add", "--", ".conclave/lanes.toml")
    assert lanes.check_staged(p) == 0
    with pytest.raises(lanes.LaneError, match="NUL"):
        lanes.parse_map(good.replace('"../wt-neon-b"', r'"../wt\u0000b"'))


def test_a_slash_heavy_text_is_read_in_linear_time(repo):
    lane_map = lanes.committed_map(repo["primary"])
    text = "(data:image/png;base64," + "Ab9/" * 65536 + ")"
    start = time.monotonic()
    assert lanes.names_reserved_path(lane_map, {"userland": lanes.LIVE}, text, "todo/00-x") is None
    assert time.monotonic() - start < 2                               # 7.2 s when every prefix was looked up


def test_a_dangling_record_link_reserves_its_lane(repo):
    d = lanes.state_dir()
    d.mkdir(parents=True)
    (d / "userland.json").symlink_to(d / "moved.json")
    assert lanes.lane_state(lanes.committed_map(repo["primary"]), "userland") == lanes.DAMAGED
    stage(repo["primary"], "user/a.c")
    r = check(repo["primary"])
    assert r.returncode == 1 and "reserved (its record cannot be read)" in r.stderr, r


def test_a_heartbeat_the_reader_would_refuse_is_not_written(repo, monkeypatch):
    lane_map = lanes.committed_map(repo["primary"])
    monkeypatch.setattr(lanes.time, "time", lambda: 1.5e9)            # a fixed width in the record
    empty = lanes.beat("userland", os.getpid(), "").stat().st_size
    path = lanes.beat("userland", os.getpid(), "x" * (lanes.RECORD_MAX - empty))
    assert path.stat().st_size == lanes.RECORD_MAX and lanes.lane_state(lane_map, "userland") == lanes.LIVE
    before = path.read_bytes()
    with pytest.raises(lanes.LaneError, match="too long"):
        lanes.beat("userland", os.getpid(), "x" * (lanes.RECORD_MAX - empty + 1))
    assert path.read_bytes() == before and lanes.lane_state(lane_map, "userland") == lanes.LIVE


def test_leading_dot_components_are_read_in_linear_time(repo):
    lane_map = lanes.committed_map(repo["primary"])
    start = time.monotonic()
    assert lanes.names_reserved_path(lane_map, {"userland": lanes.LIVE}, "./" * 524288 + "../../user/x",
                                     "todo/00-x") == "userland"
    assert time.monotonic() - start < 2                               # 8.7 s when each one copied the rest


@pytest.mark.parametrize("text, held", [
    ("Edit the files in user/.", "userland"), ("Edit the files in user/...", "userland"), ("Edit user.", None),
    ("Edit user/x.c.", "userland"), ("the parent, user/..", None), ("Edit ../../user/.", "userland")])
def test_a_sentences_full_stop_keeps_a_directorys_slash(repo, text, held):
    lane_map = lanes.committed_map(repo["primary"])
    assert lanes.names_reserved_path(lane_map, {"userland": lanes.LIVE}, text, "todo/00-x") == held


def test_a_relative_state_home_is_ignored_so_every_worktree_reads_the_same_records(repo, monkeypatch):
    home = repo["tmp"] / "home"
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.setenv("XDG_STATE_HOME", ".state")
    monkeypatch.chdir(repo["userland"])
    assert lanes.state_dir() == home / ".local" / "state" / "impossible-os" / "lanes"
    live("userland")
    stage(repo["primary"], "user/a.c")
    r = check(repo["primary"])
    assert r.returncode == 1 and "user/a.c" in r.stderr, r


def test_a_staged_submodule_is_judged_whatever_git_is_set_to_show(repo):
    p = repo["primary"]
    inner = p / "user" / "embedded"
    inner.mkdir(parents=True)
    git(inner, "init", "-q")
    git(inner, "commit", "-q", "--allow-empty", "-m", "x")
    git(p, "config", "diff.ignoreSubmodules", "all")
    git(p, "add", "--", "user/embedded")
    live("userland")
    r = check(p)
    assert r.returncode == 1 and "user/embedded" in r.stderr, r


def test_words_with_no_slash_are_read_only_for_a_map_that_reserves_one(repo):
    lane_map = lanes.committed_map(repo["primary"])
    assert lane_map["bare"] == set() and list(lanes._paths_named("the user edits user/x.c today", "", False)) == ["user/x.c"]
    assert list(lanes._paths_named("a b/c", "", True)) == ["a", "b/c"]
    start = time.monotonic()
    assert list(lanes._paths_named("Ab9" * 400000, "", False)) == []
    assert time.monotonic() - start < 2


@pytest.mark.parametrize("name", [b"review ", b"review\xff"])
def test_a_worktree_is_found_by_its_exact_name(repo, name):
    tree = os.fsdecode(os.path.join(os.fsencode(repo["tmp"]), name))
    git(repo["primary"], "worktree", "add", "-q", "--detach", tree)
    live("userland")
    stage(Path(tree), "src/kernel/k.c")
    assert lanes.check_staged(Path(tree)) == 0
    stage(Path(tree), "user/a.c")
    assert lanes.check_staged(Path(tree)) == 1


def test_the_summary_and_the_classifier_agree_on_a_file_a_lane_holds(triage, capsys, monkeypatch):
    t, root, entries = triage
    path = "todo/11-apps/TODO-01-app.md"

    def said():
        cache = [{"file_path": path, "domain": "11-apps", "sections": [{"n": 1, "status": " "}]}]
        monkeypatch.setattr(t, "traversal_order", lambda c: cache)
        t.cmd_summary(cache, root)
        summary = capsys.readouterr().out.split("\n")[0].split()[0]
        t.cmd_classify(cache, root, path)
        return summary, json.loads(capsys.readouterr().out)["file_class"]
    live("userland")
    assert said() == (t.BLOCKED, t.BLOCKED)
    (lanes.state_dir() / "userland.json").unlink()
    assert said() == (t.NEEDS_WORK, t.NEEDS_WORK)


def test_a_record_nested_too_deep_is_damaged_and_another_lane_still_holds(triage, monkeypatch):
    t, root, _entries = triage
    live("userland")
    (lanes.state_dir() / "neon-b.json").write_text("[" * 60000)
    ctx, err = t._lane_context(root)
    assert err == "" and ctx[2]["neon-b"] == lanes.DAMAGED and ctx[2]["userland"] == lanes.LIVE
    stage(Path(root), "user/a.c")
    monkeypatch.setenv("SKIP_LANE_CHECK", "1")
    monkeypatch.setenv("SKIP_LANE_CHECK_REASON", "r")
    assert lanes.check_staged(Path(root)) == 0


def test_the_outage_probe_gets_only_what_the_lane_lookup_left(triage, tmp_path, monkeypatch):
    t, root, _entries = triage
    t2 = _load("triage_budget", TRIAGE_PY)                            # the real owed_reviews
    owed = Path(root) / t2.OWED_STATE
    owed.parent.mkdir(parents=True)
    owed.write_text(json.dumps([{"todo": "todo/00-x/TODO-01-x.md", "section": 1}]))
    asked = []
    real = subprocess.run
    monkeypatch.setattr(subprocess, "run", lambda *a, **k: (asked.append(k.get("timeout")), real(["true"]))[1])
    assert t2.owed_reviews(root, time.monotonic() + 0.5)[1] and 0 < asked[0] <= 0.5
    t2._OWED_MEMO.clear()
    assert t2.owed_reviews(root, time.monotonic() - 1) == (frozenset(), []) and len(asked) == 1
    seen = []
    monkeypatch.setattr(t, "_lane_context", lambda r, deadline=None: seen.append(deadline) or (None, ""))
    monkeypatch.setattr(t, "owed_reviews", lambda r, deadline=None: seen.append(deadline) or (frozenset(), []))
    t.pick_next({}, root, ready=None)
    assert seen[0] is not None and seen == [seen[0]] * 2


def test_a_lane_that_is_off_does_not_make_every_word_a_path(monkeypatch):
    table = 'engine = "e"\nmodel = "m"\noutput = "commits"\nlimits = {}\n'
    lane_map = lanes.parse_map(GENERATED + f'[lane.main]\n{table}worktree = "."\npaths = []\n'
                               f'[lane.a]\n{table}worktree = "../a"\npaths = ["Makefile"]\n'
                               f'[lane.b]\n{table}worktree = "../b"\npaths = ["docs/inv/"]\n')
    asked = []
    monkeypatch.setattr(lanes, "_paths_named", lambda text, folder, bare: asked.append(bare) or [])
    for a in (lanes.OFF, lanes.LIVE, lanes.DAMAGED):
        lanes.names_reserved_path(lane_map, {"a": a, "b": lanes.LIVE}, "the Makefile")
    assert asked == [False, True, True]


def test_the_live_command_reads_the_map_the_commit_check_judges_by(repo):
    p = repo["primary"]
    live("userland", age=60)
    toml = p / ".conclave" / "lanes.toml"
    toml.write_text("stale_secs = 30\n" + toml.read_text().replace("stale_secs = 180\n", ""))
    argv = [sys.executable, str(p / "scripts" / "lanes.py"), "live"]
    r = run(argv, env=os.environ.copy())
    assert json.loads(r.stdout)["userland"] == lanes.LIVE, r


@pytest.mark.parametrize("ref", ["main", "no lane map"])
def test_post_commit_run_by_hand_amends_nothing(repo, ref):
    tree = repo["neon-b"]
    if ref != "main":
        git(tree, "rm", "-rq", "--", "scripts/lanes.py", ".conclave")
        git(tree, "commit", "-qm", "no lane map")
    shutil.copy(REPO / ".githooks" / "post-commit", tree / "post-commit.sh")
    before = git(tree, "rev-parse", "HEAD").stdout
    env = {k: v for k, v in os.environ.items() if k != "GIT_INDEX_FILE"}
    run(["bash", "post-commit.sh"], cwd=tree, timeout=60, env=env)
    assert git(tree, "rev-parse", "HEAD").stdout == before
    assert (tree / "COUNT.md").read_text() != "0\n"                   # refreshed, as COUNT_ONLY=1 does


def test_a_lane_commit_is_amended_by_the_hook_git_runs(repo):
    p, tree = repo["primary"], repo["userland"]
    shutil.copy(REPO / ".githooks" / "post-commit", repo["hooks"] / "post-commit")
    (repo["hooks"] / "post-commit").chmod(0o755)
    live("userland")
    stage(tree, "user/a.c")
    r = run([*GIT, "-c", f"core.hooksPath={repo['hooks']}", "commit", "-qm", "x"], cwd=tree, timeout=60,
            env=os.environ.copy())
    assert r.returncode == 0 and git(tree, "show", "HEAD:COUNT.md").stdout != "0\n", r
    assert git(p, "show", "HEAD:COUNT.md").stdout == "0\n"


README_TEXT = "<!-- COUNT-BADGE:START -->\nold\n<!-- COUNT-BADGE:END -->\n"
EMIT = "import subprocess, sys\nsys.stdout.write(subprocess.run(['git', 'show', 'HEAD:README.md'], capture_output=True,\n    text=True, timeout=20).stdout)\n"


@pytest.mark.parametrize("case", ["clean", "dirty", "staged before", "staged during", "mode during", "executable"])
def test_post_commit_never_replaces_another_readme_edit(repo, case):
    p = repo["primary"]
    (p / "README.md").write_text(README_TEXT)
    (p / "scripts" / "site").mkdir()
    during = {"staged during": "import pathlib\np = pathlib.Path('README.md')\nold = p.read_text()\n"
                               "p.write_text('staged edit\\n')\n"
                               "subprocess.run(['git', 'add', 'README.md'], timeout=20)\np.write_text(old)\n",
              "mode during": "subprocess.run(['git', 'update-index', '--chmod=+x', 'README.md'], timeout=20)\n"}
    (p / "scripts" / "site" / "build.py").write_text(EMIT + during.get(case, ""))
    git(p, "add", "--", "README.md", "scripts/site")
    if case == "executable":
        (p / "README.md").chmod(0o755)
        git(p, "add", "--", "README.md")
    git(p, "commit", "-qm", "readme")
    if case == "staged before":
        (p / "README.md").write_text("staged edit\n")
        git(p, "add", "--", "README.md")
        (p / "README.md").write_text(README_TEXT)
    if case == "dirty":
        (p / "README.md").write_text("mine\n")
    shutil.copy(REPO / ".githooks" / "post-commit", repo["hooks"] / "post-commit")
    (repo["hooks"] / "post-commit").chmod(0o755)
    (p / "src/kernel/k.c").write_text("k2\n")
    r = run([*GIT, "-c", f"core.hooksPath={repo['hooks']}", "commit", "-qm", "x", "--only", "src/kernel/k.c"], cwd=p,
            timeout=60, env={**os.environ, "GIT_COMMITTER_NAME": "T", "GIT_COMMITTER_EMAIL": "t@example.com"})
    assert r.returncode == 0, r
    head, index, work = git(p, "show", "HEAD:README.md").stdout, git(p, "show", ":README.md").stdout, \
        (p / "README.md").read_text()
    # HEAD always gets the synced README and COUNT.md (the site check reads them there); another edit stays its own.
    assert "img.shields.io" in head and git(p, "show", "HEAD:COUNT.md").stdout != "0\n", r
    assert index == {"staged before": "staged edit\n", "staged during": "staged edit\n",
                     "mode during": README_TEXT}.get(case, head), r
    assert work == {"clean": head, "executable": head, "dirty": "mine\n"}.get(case, README_TEXT), r
    modes = git(p, "ls-tree", "HEAD", "--", "README.md").stdout[:6], git(p, "ls-files", "-s", "README.md").stdout[:6]
    assert modes == {"mode during": ("100644", "100755"), "executable": ("100755", "100755")}.get(case, ("100644", "100644")), r


def test_a_manual_refresh_brings_a_clean_readme_along(repo):
    p = repo["primary"]
    (p / "README.md").write_text(README_TEXT)
    (p / "scripts" / "site").mkdir()
    (p / "scripts" / "site" / "build.py").write_text(EMIT)
    git(p, "add", "--", "README.md", "scripts/site")
    git(p, "commit", "-qm", "readme")
    env = {k: v for k, v in os.environ.items() if k != "GIT_INDEX_FILE"}
    r = run(["bash", str(REPO / ".githooks" / "post-commit")], cwd=p, timeout=60, env=env)
    assert "img.shields.io" in (p / "README.md").read_text(), r
    assert git(p, "show", "HEAD:README.md").stdout == README_TEXT and git(p, "diff", "--cached", "--name-only").stdout == ""


def test_the_map_exempts_what_post_commit_amends():
    for missing in ("COUNT.md", "README.md"):
        left = "README.md" if missing == "COUNT.md" else "COUNT.md"
        with pytest.raises(lanes.LaneError, match="generated must hold COUNT.md and README.md"):
            lanes.parse_map(MAP.replace('generated = ["COUNT.md", "README.md"]', f'generated = ["{left}"]'))


def test_a_clean_replay_asks_git_for_the_map_alone(repo, monkeypatch):
    p = repo["primary"]
    stage(p, "src/kernel/k.c")
    asked = []
    real = lanes.staged_paths
    monkeypatch.setattr(lanes, "staged_paths", lambda t, *only: (asked.append(only), real(t, *only))[1])
    monkeypatch.setattr(lanes, "replaying", lambda t: True)
    assert lanes.check_staged(p, replay=True) == 0 and asked == [(lanes.MAP,)]
    assert real(p, lanes.MAP) == [] and real(p) == ["src/kernel/k.c"]


@pytest.mark.parametrize("where", ["working tree", "HEAD", "unreadable", None])
def test_triage_holds_every_section_when_the_lane_reader_is_gone(tmp_path, where):
    t = _load("triage_no_reader", TRIAGE_PY)
    mapped = where is not None
    git(tmp_path, "init", "-q")
    git(tmp_path, "commit", "-q", "--allow-empty", "-m", "x")
    if mapped:
        (tmp_path / ".conclave").mkdir()
        (tmp_path / ".conclave" / "lanes.toml").write_text(MAP)
    if where == "HEAD":
        git(tmp_path, "add", "-A")
        git(tmp_path, "commit", "-qm", "map")
        (tmp_path / ".conclave" / "lanes.toml").unlink()
    if where == "unreadable":                                         # a lookup that fails proves no absence
        shutil.rmtree(tmp_path / ".conclave")
        (tmp_path / ".git" / "HEAD").write_text("broken\n")
    ctx, why = t._lane_context(str(tmp_path))
    assert why and (ctx is not None) == mapped
    per, held = t.lane_reservations({"file_path": "todo/11-apps/TODO-01-x.md"}, [(1, t.NEEDS_WORK)], str(tmp_path), ctx)
    assert per == [(1, t.BLOCKED if mapped else t.NEEDS_WORK)] and len(held) == mapped


def test_a_run_of_triage_scans_each_todo_once_for_its_planner_too(tmp_path):
    (tmp_path / "todo" / "00-x").mkdir(parents=True)
    md = tmp_path / "todo" / "00-x" / "TODO-01-x.md"
    md.write_text("# T\n\n## 1. One\n\n- [ ] x\n")
    cache = tmp_path / "cache.json"
    cache.write_text(json.dumps([{"file_path": "todo/00-x/TODO-01-x.md", "domain": "00-x",
                                  "sections": [{"n": 1, "status": " "}]}]))
    (tmp_path / "scripts").mkdir()
    shutil.copy(REPO / "scripts" / "todo_fence.py", tmp_path / "scripts" / "todo_fence.py")
    (tmp_path / "scripts" / "todo-plan.py").write_text(
        "import os\ndef ready_rows(root, nodes):\n"
        "    import sequencer_triage as st\n"
        "    before = len(st._STAMP_MEMO)\n"
        "    st.section_stamps(os.path.join(root, 'todo/00-x/TODO-01-x.md'))\n"
        "    (root / 'seen').write_text(f'{before} {len(st._STAMP_MEMO)}')\n"
        "    return [('todo/00-x/TODO-01-x.md', 1)]\n")
    r = run([sys.executable, str(TRIAGE_PY), "--next", "--repo-root", str(tmp_path), "--cache", str(cache)],
            cwd=tmp_path, timeout=60, env=os.environ.copy())
    assert r.returncode == 0 and (tmp_path / "seen").read_text() == "1 1", r
    t = _load("triage_memo", TRIAGE_PY)
    t._STAMP_MEMO = {}
    assert t.section_stamps(str(md)) == {1: set()}
    md.write_text("# T\n\n## 1. One\n\n> **Verified:** 2026-11-24\n")
    assert t.section_stamps(str(md)) == {1: {"V"}}


def test_the_chosen_files_lifecycle_needs_no_second_scan(tmp_path, monkeypatch):
    t = _load("triage_lifecycle", TRIAGE_PY)
    t._STAMP_MEMO = {}
    one, two = tmp_path / "TODO-01-x.md", tmp_path / "TODO-02-x.md"
    one.write_text("# T\n\n> **Validated:** 2026-11-24\n\n## 1. One\n")
    two.write_text("# T\n\n## 1. One\n")
    assert t.section_stamps(str(one)) == t.section_stamps(str(two)) == {1: set()}
    monkeypatch.setattr(t, "_scan_sections", lambda text: 1 / 0)
    assert t.file_lifecycle(str(one)) == {"validated": True, "gap_audited": False}
    assert t.file_lifecycle(str(two)) == {"validated": False, "gap_audited": False}
    t._STAMP_MEMO = None
    t._SCAN_MEMO.clear()
    with pytest.raises(ZeroDivisionError):
        t.file_lifecycle(str(one))                                    # outside a one-shot run it reads the file


def test_a_todo_replaced_by_one_of_the_same_size_and_time_is_read_again(tmp_path):
    t = _load("triage_inode", TRIAGE_PY)
    t._STAMP_MEMO = {}
    md, other = tmp_path / "TODO-01-x.md", tmp_path / "new"
    md.write_text("# T\n\n## 1. One\n\n> **Verified:** 2026-11-24\n")
    assert t.section_stamps(str(md)) == {1: {"V"}}
    other.write_text("# T\n\n## 1. One\n\n> **Verifiex:** 2026-11-24\n")
    st = md.stat()
    os.utime(other, ns=(st.st_atime_ns, st.st_mtime_ns))
    os.replace(other, md)
    assert t.section_stamps(str(md)) == {1: set()}


def test_a_linked_readme_is_never_written_through(repo):
    p = repo["primary"]
    (p / "scripts" / "site").mkdir()
    (p / "scripts" / "site" / "build.py").write_text(EMIT)
    (p / "intro.md").write_text(README_TEXT)
    (p / "README.md").symlink_to("intro.md")
    git(p, "add", "--", "README.md", "intro.md", "scripts/site")
    git(p, "commit", "-qm", "link")
    (p / "intro.md").write_text("mine\n")
    shutil.copy(REPO / ".githooks" / "post-commit", repo["hooks"] / "post-commit")
    (repo["hooks"] / "post-commit").chmod(0o755)
    (p / "src/kernel/k.c").write_text("k2\n")
    r = run([*GIT, "-c", f"core.hooksPath={repo['hooks']}", "commit", "-qm", "x", "--only", "src/kernel/k.c"], cwd=p,
            timeout=60, env=os.environ.copy())
    assert r.returncode == 0 and (p / "intro.md").read_text() == "mine\n", r
    run(["bash", str(REPO / ".githooks" / "post-commit")], cwd=p, timeout=60,
        env={k: v for k, v in os.environ.items() if k != "GIT_INDEX_FILE"})
    assert (p / "intro.md").read_text() == "mine\n" and (p / "README.md").is_symlink()
    assert git(p, "show", "HEAD:COUNT.md").stdout != "0\n"


@pytest.mark.parametrize("staged", [True, False])
def test_a_failed_index_publish_is_reported_without_a_command(repo, staged):
    p = repo["primary"]
    (p / "README.md").write_text(README_TEXT)
    (p / "scripts" / "site").mkdir()
    (p / "scripts" / "site" / "build.py").write_text(EMIT)
    git(p, "add", "--", "README.md", "scripts/site")
    git(p, "commit", "-qm", "readme")
    if staged:
        (p / "README.md").write_text("staged edit\n")
        git(p, "add", "--", "README.md")
    hook = (REPO / ".githooks" / "post-commit").read_text()
    assert hook.count('if mv -f "$lock" "$idx"; then') == 1
    (repo["hooks"] / "post-commit").write_text(hook.replace('if mv -f "$lock" "$idx"; then', "if false; then"))
    (repo["hooks"] / "post-commit").chmod(0o755)
    (p / "src/kernel/k.c").write_text("k2\n")
    r = run([*GIT, "-c", f"core.hooksPath={repo['hooks']}", "commit", "-qm", "x", "--only", "src/kernel/k.c"], cwd=p,
            timeout=60, env={**os.environ, "GIT_COMMITTER_NAME": "T", "GIT_COMMITTER_EMAIL": "t@example.com"})
    # It names the state, not a command: any reset run later can drop what was staged in between.
    assert f"still holds the old COUNT.md{'' if staged else ' and README.md'}\n" in r.stderr, r
    assert "HEAD is unchanged" not in r.stderr and "reset" not in r.stderr, r
    assert git(p, "show", "HEAD:COUNT.md").stdout != "0\n" and "img.shields.io" in git(p, "show", "HEAD:README.md").stdout
    if staged:
        assert git(p, "show", ":README.md").stdout == (p / "README.md").read_text() == "staged edit\n"
    else:                                                             # a clean working README still follows HEAD
        assert (p / "README.md").read_text() == git(p, "show", "HEAD:README.md").stdout


@pytest.mark.parametrize("by_hand", [True, False])
@pytest.mark.parametrize("case", ["assume-unchanged edit", "hard link"])
def test_publishing_readme_reads_the_file_and_writes_a_new_one(repo, case, by_hand):
    p = repo["primary"]
    (p / "README.md").write_text(README_TEXT)
    (p / "README.md").chmod(0o640)
    (p / "scripts" / "site").mkdir()
    (p / "scripts" / "site" / "build.py").write_text(EMIT)
    git(p, "add", "--", "README.md", "scripts/site")
    git(p, "commit", "-qm", "readme")
    if case == "hard link":
        (repo["tmp"] / "backup.md").hardlink_to(p / "README.md")
    else:
        git(p, "update-index", "--assume-unchanged", "README.md")
        (p / "README.md").write_text("mine\n")
    hook = REPO / ".githooks" / "post-commit"
    if by_hand:
        run(["bash", str(hook)], cwd=p, timeout=60, env={k: v for k, v in os.environ.items() if k != "GIT_INDEX_FILE"})
    else:
        shutil.copy(hook, repo["hooks"] / "post-commit")
        (repo["hooks"] / "post-commit").chmod(0o755)
        (p / "src/kernel/k.c").write_text("k2\n")
        run([*GIT, "-c", f"core.hooksPath={repo['hooks']}", "commit", "-qm", "x", "--only", "src/kernel/k.c"], cwd=p,
            timeout=60, env={**os.environ, "GIT_COMMITTER_NAME": "T", "GIT_COMMITTER_EMAIL": "t@example.com"})
        assert "img.shields.io" in git(p, "show", "HEAD:README.md").stdout
    if case == "hard link":
        assert (repo["tmp"] / "backup.md").read_text() == README_TEXT
        assert "img.shields.io" in (p / "README.md").read_text() and (p / "README.md").stat().st_mode & 0o777 == 0o640
    else:
        assert (p / "README.md").read_text() == "mine\n"
    assert not list(p.glob(".README.md.*"))


def test_a_second_manual_refresh_keeps_an_unstaged_reversal(repo):
    p = repo["primary"]
    (p / "README.md").write_text(README_TEXT)
    (p / "scripts" / "site").mkdir()
    (p / "scripts" / "site" / "build.py").write_text(EMIT)
    git(p, "add", "--", "README.md", "scripts/site")
    git(p, "commit", "-qm", "readme")
    env = {k: v for k, v in os.environ.items() if k != "GIT_INDEX_FILE"}
    hook = ["bash", str(REPO / ".githooks" / "post-commit")]
    run(hook, cwd=p, timeout=60, env=env)
    git(p, "add", "--", "README.md")
    (p / "README.md").write_text(README_TEXT)                         # reversed in the working tree only
    run(hook, cwd=p, timeout=60, env=env)
    assert (p / "README.md").read_text() == README_TEXT and "img.shields.io" in git(p, "show", ":README.md").stdout


@pytest.mark.parametrize("index_fails", [False, True])
@pytest.mark.parametrize("edited", [False, True])
def test_a_readme_that_cannot_be_written_is_reported(repo, edited, index_fails):
    p = repo["primary"]
    (p / "README.md").write_text(README_TEXT)
    (p / "scripts" / "site").mkdir()
    (p / "scripts" / "site" / "build.py").write_text(EMIT)
    git(p, "add", "--", "README.md", "scripts/site")
    git(p, "commit", "-qm", "readme")
    hook = (REPO / ".githooks" / "post-commit").read_text()
    if index_fails:
        hook = hook.replace('if mv -f "$lock" "$idx"; then', "if false; then")
    (repo["hooks"] / "post-commit").write_text(hook)
    (repo["hooks"] / "post-commit").chmod(0o755)
    if edited:
        (p / "README.md").write_text("mine\n")
    (p / "src/kernel/k.c").write_text("k2\n")
    p.chmod(0o555)
    try:
        r = run([*GIT, "-c", f"core.hooksPath={repo['hooks']}", "commit", "-qm", "x", "--only", "src/kernel/k.c"], cwd=p,
                timeout=60, env={**os.environ, "GIT_COMMITTER_NAME": "T", "GIT_COMMITTER_EMAIL": "t@example.com"})
    finally:
        p.chmod(0o755)
    assert "img.shields.io" in git(p, "show", "HEAD:README.md").stdout, r
    assert ("README.md could not be written" in r.stderr) == (not edited), r
    if edited:
        assert (p / "README.md").read_text() == "mine\n"
    else:
        assert (p / "README.md").read_text() == README_TEXT           # as it was, and the report says so


def test_an_expired_lane_map_lookup_asks_git_nothing(tmp_path, monkeypatch):
    t = _load("triage_expired", TRIAGE_PY)
    git(tmp_path, "init", "-q")
    monkeypatch.setattr(t.subprocess, "run", lambda *a, **k: 1 / 0)
    assert t._has_lane_map(str(tmp_path), time.monotonic() - 1) is True
    with pytest.raises(ZeroDivisionError):
        t._has_lane_map(str(tmp_path), time.monotonic() + 60)


def test_a_held_index_lock_leaves_the_commit_and_every_entry_alone(repo):
    p = repo["primary"]
    (p / "README.md").write_text(README_TEXT)
    (p / "scripts" / "site").mkdir()
    (p / "scripts" / "site" / "build.py").write_text(EMIT + "open('.git/index.lock', 'x').close()\n")
    git(p, "add", "--", "README.md", "scripts/site")
    git(p, "commit", "-qm", "readme")
    (p / "README.md").write_text("staged edit\n")
    git(p, "add", "--", "README.md")
    shutil.copy(REPO / ".githooks" / "post-commit", repo["hooks"] / "post-commit")
    (repo["hooks"] / "post-commit").chmod(0o755)
    (p / "src/kernel/k.c").write_text("k2\n")
    r = run([*GIT, "-c", f"core.hooksPath={repo['hooks']}", "commit", "-qm", "x", "--only", "src/kernel/k.c"], cwd=p,
            timeout=60, env={**os.environ, "GIT_COMMITTER_NAME": "T", "GIT_COMMITTER_EMAIL": "t@example.com"})
    assert (p / ".git" / "index.lock").exists(), r                    # never another process's lock to remove
    (p / ".git" / "index.lock").unlink()
    assert "could not be committed" in r.stderr and "run" not in r.stderr.split("could not be committed")[1], r
    assert git(p, "show", "HEAD:README.md").stdout == README_TEXT
    assert git(p, "show", ":README.md").stdout == (p / "README.md").read_text() == "staged edit\n"
    # as the report says, the next commit brings them
    (p / "scripts" / "site" / "build.py").write_text(EMIT)
    (p / "src/kernel/k.c").write_text("k3\n")
    run([*GIT, "-c", f"core.hooksPath={repo['hooks']}", "commit", "-qm", "y", "--only", "src/kernel/k.c"], cwd=p,
        timeout=60, env={**os.environ, "GIT_COMMITTER_NAME": "T", "GIT_COMMITTER_EMAIL": "t@example.com"})
    assert "img.shields.io" in git(p, "show", "HEAD:README.md").stdout and git(p, "show", "HEAD:COUNT.md").stdout != "0\n"
    assert git(p, "show", ":README.md").stdout == (p / "README.md").read_text() == "staged edit\n"


@pytest.mark.parametrize("case", ["lock held", "HEAD moved", "HEAD moved during the count"])
def test_a_count_only_amend_is_pinned_and_a_refusal_is_reported(repo, case):
    p = repo["primary"]
    (p / "intro.md").write_text(README_TEXT)
    (p / "README.md").symlink_to("intro.md")                          # no README to sync: COUNT.md alone is amended
    git(p, "add", "--", "README.md", "intro.md")
    git(p, "commit", "-qm", "link")
    hook = (REPO / ".githooks" / "post-commit").read_text()
    call = "if plumb_amend; then"
    assert hook.count(call) == 1
    move = 'b="$(git commit-tree -p HEAD -m theirs "HEAD^{tree}")"; git update-ref HEAD "$b"; echo "$b" > "$ROOT/.git/b"\n'
    if case == "lock held":
        edited = hook.replace(call, ': > "$ROOT/.git/index.lock"\n' + call)
    elif case == "HEAD moved":
        edited = hook.replace(call, move + call)
    else:
        start = "# Vendored files to exclude"
        assert hook.count(start) == 1
        edited = hook.replace(start, move + start)
    (repo["hooks"] / "post-commit").write_text(edited)
    (repo["hooks"] / "post-commit").chmod(0o755)
    env = {**os.environ, "GIT_COMMITTER_NAME": "T", "GIT_COMMITTER_EMAIL": "t@example.com",
           "GIT_AUTHOR_NAME": "T", "GIT_AUTHOR_EMAIL": "t@example.com"}
    (p / "src/kernel/k.c").write_text("k2\n")
    r = run([*GIT, "-c", f"core.hooksPath={repo['hooks']}", "commit", "-qm", "x", "--only", "src/kernel/k.c"], cwd=p,
            timeout=60, env=env)
    assert r.returncode == 0 and "the new COUNT.md could not be committed" in r.stderr, r
    assert git(p, "show", "HEAD:COUNT.md").stdout == "0\n"
    if case == "lock held":
        (p / ".git" / "index.lock").unlink()
        assert git(p, "log", "-1", "--format=%s").stdout == "x\n"
    else:                                                             # the later commit keeps its identity
        assert git(p, "rev-parse", "HEAD").stdout == (p / ".git" / "b").read_text()
    (repo["hooks"] / "post-commit").write_text(hook)                  # as the report says, the next commit brings it
    (p / "src/kernel/k.c").write_text("k3\n")
    r = run([*GIT, "-c", f"core.hooksPath={repo['hooks']}", "commit", "-qm", "y", "--only", "src/kernel/k.c"], cwd=p,
            timeout=60, env=env)
    assert "post-commit:" not in r.stderr and git(p, "show", "HEAD:COUNT.md").stdout != "0\n", r
    assert git(p, "log", "-1", "--format=%s").stdout == "y\n" and git(p, "diff", "--cached", "--name-only").stdout == ""
    assert (p / "README.md").is_symlink() and git(p, "ls-tree", "HEAD", "--", "README.md").stdout[:6] == "120000"


def test_an_unchanged_readme_is_not_rewritten(repo):
    p = repo["primary"]
    (p / "README.md").write_text(README_TEXT)
    (p / "scripts" / "site").mkdir()
    (p / "scripts" / "site" / "build.py").write_text(EMIT)
    git(p, "add", "--", "README.md", "scripts/site")
    git(p, "commit", "-qm", "readme")
    env = {k: v for k, v in os.environ.items() if k != "GIT_INDEX_FILE"}
    hook = ["bash", str(REPO / ".githooks" / "post-commit")]
    for _ in range(3):                                                # the badge's own line counts, so it settles
        run(hook, cwd=p, timeout=60, env=env)
        git(p, "commit", "-qm", "badge", "--allow-empty", "--only", "README.md", "COUNT.md")
    before = (p / "README.md").stat()
    run(hook, cwd=p, timeout=60, env=env)
    after = (p / "README.md").stat()
    assert (after.st_ino, after.st_mtime_ns) == (before.st_ino, before.st_mtime_ns)


@pytest.mark.parametrize("by_hand", [True, False])
@pytest.mark.parametrize("link", ["hard", "symbolic"])
def test_count_is_never_written_through_a_link(repo, link, by_hand):
    p = repo["primary"]
    other = repo["tmp"] / "mine.md"
    other.write_text("mine\n")
    (p / "COUNT.md").unlink()
    if link == "hard":
        (p / "COUNT.md").hardlink_to(other)
    else:
        (p / "COUNT.md").symlink_to(other)
    hook = REPO / ".githooks" / "post-commit"
    if by_hand:
        run(["bash", str(hook)], cwd=p, timeout=60, env={k: v for k, v in os.environ.items() if k != "GIT_INDEX_FILE"})
    else:
        shutil.copy(hook, repo["hooks"] / "post-commit")
        (repo["hooks"] / "post-commit").chmod(0o755)
        (p / "src/kernel/k.c").write_text("k2\n")
        run([*GIT, "-c", f"core.hooksPath={repo['hooks']}", "commit", "-qm", "x", "--only", "src/kernel/k.c"], cwd=p,
            timeout=60, env={**os.environ, "GIT_COMMITTER_NAME": "T", "GIT_COMMITTER_EMAIL": "t@example.com"})
        assert git(p, "show", "HEAD:COUNT.md").stdout == (p / "COUNT.md").read_text()
        assert git(p, "ls-tree", "HEAD", "--", "COUNT.md").stdout[:6] == "100644"
    assert other.read_text() == "mine\n" and "Line Count" in (p / "COUNT.md").read_text()
    assert not (p / "COUNT.md").is_symlink() and not list(p.glob(".COUNT.md.*"))


def test_the_amend_keeps_every_other_entry(repo):
    p = repo["primary"]
    for name in ("a b.txt", "caf\u00e9.txt", "t\tab.txt", "x COUNT.md", "README.md.bak", "sub/COUNT.md"):
        (p / name).parent.mkdir(exist_ok=True)
        (p / name).write_text(name)
    (p / "run.sh").write_text("x")
    (p / "run.sh").chmod(0o755)
    (p / "README.md").write_text(README_TEXT)
    (p / "scripts" / "site").mkdir()
    (p / "scripts" / "site" / "build.py").write_text(EMIT)
    git(p, "add", "-A", "--", "a b.txt", "caf\u00e9.txt", "t\tab.txt", "x COUNT.md", "README.md.bak", "sub", "run.sh",
        "README.md", "scripts/site")
    git(p, "commit", "-qm", "names")
    before = git(p, "ls-tree", "-r", "HEAD").stdout.splitlines()
    shutil.copy(REPO / ".githooks" / "post-commit", repo["hooks"] / "post-commit")
    (repo["hooks"] / "post-commit").chmod(0o755)
    run([*GIT, "-c", f"core.hooksPath={repo['hooks']}", "commit", "-qm", "x", "--allow-empty"], cwd=p,
        timeout=60, env={**os.environ, "GIT_COMMITTER_NAME": "T", "GIT_COMMITTER_EMAIL": "t@example.com"})
    after = git(p, "ls-tree", "-r", "HEAD").stdout.splitlines()
    changed = {x.split("\t")[1] for x in set(before) ^ set(after)}
    assert changed == {"COUNT.md", "README.md"} and len(before) == len(after)
    assert git(p, "status", "--porcelain", "-uno").stdout == ""


def commit_with_hook(repo, hook_text, path="src/kernel/k.c", text="k2\n"):
    p = repo["primary"]
    (repo["hooks"] / "post-commit").write_text(hook_text)
    (repo["hooks"] / "post-commit").chmod(0o755)
    (p / path).write_text(text)
    return run([*GIT, "-c", f"core.hooksPath={repo['hooks']}", "commit", "-qm", "x", "--only", path], cwd=p, timeout=60,
               env={**os.environ, "GIT_COMMITTER_NAME": "T", "GIT_COMMITTER_EMAIL": "t@example.com"})


@pytest.mark.parametrize("broken", ["filter fails", "filter drops an entry"])
def test_a_failed_amend_step_never_drops_a_file(repo, broken):
    p = repo["primary"]
    names = git(p, "ls-tree", "-r", "--name-only", "HEAD").stdout
    hook = (REPO / ".githooks" / "post-commit").read_text()
    filt = """awk -F '\\t' -v r="$rb" '$2 != "COUNT.md\""""
    assert hook.count(filt) == 1
    hook = hook.replace(filt, filt.replace("awk", "false") if broken == "filter fails"
                        else filt.replace("'$2", "'$2 != \"src\" && $2"))
    r = commit_with_hook(repo, hook)
    assert "could not be committed" in r.stderr and git(p, "ls-tree", "-r", "--name-only", "HEAD").stdout == names, r
    assert git(p, "show", "HEAD:src/kernel/k.c").stdout == "k2\n" and git(p, "diff", "--cached", "--name-only").stdout == ""


def test_a_busy_ref_is_refused_at_once(repo):
    p = repo["primary"]
    git(p, "config", "core.filesRefLockTimeout", "-1")
    hook = (REPO / ".githooks" / "post-commit").read_text()
    call = "if plumb_amend; then"
    assert hook.count(call) == 1
    lock = p / ".git" / (git(p, "symbolic-ref", "HEAD").stdout.strip() + ".lock")
    r = commit_with_hook(repo, hook.replace(call, f': > "{lock}"\n' + call))
    lock.unlink()
    assert r.returncode == 0 and "could not be committed" in r.stderr, r
    assert not (p / ".git" / "index.lock").exists() and git(p, "show", "HEAD:COUNT.md").stdout == "0\n"


@pytest.mark.parametrize("kind", ["directory", "link to a directory"])
def test_count_is_never_moved_into_a_directory(repo, kind):
    p = repo["primary"]
    (p / "COUNT.md").unlink()
    target = p / "COUNT.md"
    if kind != "directory":
        target = repo["tmp"] / "dir"
        (p / "COUNT.md").symlink_to(target)
    target.mkdir()
    r = run(["bash", str(REPO / ".githooks" / "post-commit")], cwd=p, timeout=60,
            env={k: v for k, v in os.environ.items() if k != "GIT_INDEX_FILE"})
    assert list(target.iterdir()) == [] and not list(p.glob(".COUNT.md.*"))
    if kind == "directory":
        assert "COUNT.md could not be written" in r.stderr, r
    else:
        assert (p / "COUNT.md").is_file() and not (p / "COUNT.md").is_symlink()


def test_a_count_that_cannot_be_generated_changes_nothing(repo):
    hook = (REPO / ".githooks" / "post-commit").read_text()
    write = 'if ! cat > "$COUNT_TARGET" << EOF'
    assert hook.count(write) == 1
    p = repo["primary"]
    r = commit_with_hook(repo, hook.replace(write, 'if ! { head -c 10 > "$COUNT_TARGET"; false; } << EOF'))   # a short write
    assert "could not be generated" in r.stderr and git(p, "show", "HEAD:COUNT.md").stdout == "0\n", r
    assert (p / "COUNT.md").read_text() == "0\n"
