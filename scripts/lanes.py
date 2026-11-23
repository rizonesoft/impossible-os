#!/usr/bin/env python3
"""The lane map and the live lane records: the one reader and writer of both (the lane map and the commit check, in the automation-hardening TODO).

The map is .conclave/lanes.toml. A live extra lane keeps a record at
$XDG_STATE_HOME/impossible-os/lanes/<lane>.json (default ~/.local/state); it is live while the record names it,
its pid exists and its heartbeat is within stale_secs. A record that exists but cannot be read is "damaged": it
reserves the lane's paths against everyone else and gives the lane itself no right to commit.

    python3 scripts/lanes.py beat <lane> --pid P --task T   # write the lane's record (temp file plus rename)
    python3 scripts/lanes.py clear <lane>                   # remove it
    python3 scripts/lanes.py live                           # each extra lane's state: live, off or damaged
    python3 scripts/lanes.py owner <path>                   # the extra lane whose paths hold it, else main
    python3 scripts/lanes.py check-staged                   # the commit check (.githooks/pre-commit, prepare-commit-msg)
    python3 scripts/lanes.py check-staged --replay          # a rebase's own commit of a pick

check-staged exits 0 to allow, 1 to refuse, 2 when it cannot judge (a commit is refused for both).
SKIP_LANE_CHECK=1 with a SKIP_LANE_CHECK_REASON allows the commit and prints the reason.
"""
from __future__ import annotations

import errno
import json
import os
import re
import stat
import subprocess
import sys
import time
import tomllib
from pathlib import Path

MAIN = "main"
OUTPUTS = ("commits", "reports")
LANE_KEYS = {"engine", "model", "worktree", "paths", "output", "limits"}
TOP_KEYS = {"stale_secs", "generated", "lane"}
LIVE, OFF, DAMAGED = "live", "off", "damaged"
GIT_SECS = 10


class LaneError(Exception):
    """The map or the repository cannot be read, so nothing can be judged."""


def _need(cond, msg):
    if not cond:
        raise LaneError(f".conclave/lanes.toml: {msg}")


def _canonical(v) -> bool:
    """A repository path in git's own spelling: relative, `/`-separated, no `.`, `..` or empty component. Git
    reports `user/x.c`, so a map entry spelled `./user/` would match nothing and release its reservation."""
    if not isinstance(v, str) or not v or v.startswith("/") or "\\" in v:
        return False
    parts = v[:-1].split("/") if v.endswith("/") else v.split("/")
    return all(p not in ("", ".", "..") for p in parts)


def _strings(value) -> bool:
    return isinstance(value, list) and all(_canonical(v) for v in value)


MAP = ".conclave/lanes.toml"


def load_map(root: Path) -> dict:
    """The validated map in the working tree (scheduling and the lane CLI; the commit check reads the
    committed one, `committed_map`)."""
    try:
        text = (Path(root) / MAP).read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        raise LaneError(f"{MAP} cannot be read ({type(exc).__name__}: {exc})") from exc
    return parse_map(text)


def parse_map(text: str) -> dict:
    """The validated map: {'stale_secs', 'generated', 'lanes': {name: table}}. Anything unexpected refuses."""
    try:
        data = tomllib.loads(text)
    except tomllib.TOMLDecodeError as exc:
        raise LaneError(f"{MAP} cannot be read ({type(exc).__name__}: {exc})") from exc
    _need(set(data) <= TOP_KEYS, f"unknown keys {sorted(set(data) - TOP_KEYS)}")
    stale = data.get("stale_secs", 180)
    _need(isinstance(stale, int) and not isinstance(stale, bool) and stale > 0, "stale_secs must be a positive integer")
    generated = data.get("generated", [])
    _need(_strings(generated), "generated must be a list of relative paths")
    lanes = data.get("lane")
    _need(isinstance(lanes, dict) and MAIN in lanes, "needs a [lane.main] table")
    owned: dict[str, str] = {}
    trees: dict[str, str] = {}
    for name, lane in lanes.items():
        _need(re.fullmatch(r"[a-z][a-z0-9-]*", name) is not None, f"lane name {name!r} is not lower-case-with-dashes")
        _need(isinstance(lane, dict) and set(lane) == LANE_KEYS,
              f"[lane.{name}] needs exactly {sorted(LANE_KEYS)}")
        _need(all(isinstance(lane[k], str) and lane[k] for k in ("engine", "model", "worktree")),
              f"[lane.{name}] engine, model and worktree must be non-empty strings")
        _need(not os.path.isabs(lane["worktree"]), f"[lane.{name}] worktree must be relative to the primary tree")
        _need(lane["output"] in OUTPUTS, f"[lane.{name}] output must be one of {OUTPUTS}")
        _need(isinstance(lane["limits"], dict), f"[lane.{name}] limits must be a table")
        _need(_strings(lane["paths"]), f"[lane.{name}] paths must be a list of relative paths")
        if name == MAIN:
            _need(not lane["paths"] and lane["worktree"] == ".",
                  "[lane.main] owns the remainder: paths = [] and worktree = \".\"")
        # The committing lane is found by its worktree, so two lanes in one tree would share permissions.
        tree = os.path.normpath(lane["worktree"])
        _need(tree not in trees, f"[lane.{name}] worktree {lane['worktree']!r} is also lane {trees.get(tree)}'s")
        trees[tree] = name
        for p in lane["paths"]:
            for q, other in owned.items():
                _need(not (_covers(p, q) or _covers(q, p)), f"{name} path {p!r} overlaps {other} path {q!r}")
            owned[p] = name
    return {"stale_secs": stale, "generated": list(generated), "lanes": lanes}


def _covers(entry: str, path: str) -> bool:
    """A paths entry holds a path: a prefix ending in "/" holds everything under it, anything else is one file."""
    return path.startswith(entry) if entry.endswith("/") else path == entry


def owner(lane_map: dict, path: str) -> str:
    for name, lane in lane_map["lanes"].items():
        if any(_covers(p, path) for p in lane["paths"]):
            return name
    return MAIN


def state_dir() -> Path:
    base = os.environ.get("XDG_STATE_HOME") or os.path.join(os.path.expanduser("~"), ".local", "state")
    return Path(base) / "impossible-os" / "lanes"


PID_MAX = 2**31 - 1                                # the largest process id any supported host hands out


def _pid_ok(pid) -> bool:
    return isinstance(pid, int) and not isinstance(pid, bool) and 0 < pid <= PID_MAX


def _pid_exists(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except OSError as exc:
        return exc.errno == errno.EPERM            # it exists, owned by someone else
    return True


def lane_state(lane_map: dict, name: str, now: float | None = None) -> str:
    """live, off (no record, or a stale or dead one) or damaged (a record that cannot be read)."""
    path = state_dir() / f"{name}.json"
    try:
        raw = path.read_bytes()
    except FileNotFoundError:
        return OFF
    except OSError:
        return DAMAGED                             # fail-direction: an unreadable record reserves, never frees
    try:
        rec = json.loads(raw)
        pid, beat = rec["pid"], rec["heartbeat"]
        ok = (isinstance(rec, dict) and rec.get("lane") == name and _pid_ok(pid)
              and isinstance(beat, (int, float)) and not isinstance(beat, bool))
    except (ValueError, KeyError, TypeError):
        ok = False
    if not ok:
        return DAMAGED
    now = time.time() if now is None else now
    if now - beat > lane_map["stale_secs"] or not _pid_exists(pid):
        return OFF
    return LIVE


def states(lane_map: dict) -> dict[str, str]:
    """Each extra lane's state (main is never recorded: it owns the remainder)."""
    return {name: lane_state(lane_map, name) for name in lane_map["lanes"] if name != MAIN}


def reserved_by(lane_map: dict, lane_states: dict[str, str], path: str) -> str | None:
    """The extra lane that holds `path` against everyone else now, or None."""
    name = owner(lane_map, path)
    return name if name != MAIN and lane_states.get(name) in (LIVE, DAMAGED) else None


def names_reserved_path(lane_map: dict, lane_states: dict[str, str], text: str) -> str | None:
    """The live (or damaged) lane one of whose paths `text` names, or None: a section whose text says it edits
    a file in a lane cannot be finished outside that lane."""
    # A repository-relative `./user/x` names the same file as `user/x`; a nested `vendor/user/x` does not.
    if "./" in text:
        text = re.sub(r"(?<![\w./-])\./", " ", text)
    for name, lane in lane_map["lanes"].items():
        if name == MAIN or lane_states.get(name) not in (LIVE, DAMAGED):
            continue
        for p in lane["paths"]:
            if p in text and re.search(r"(?<![\w./-])" + re.escape(p), text):
                return name
    return None


def any_reserving(lane_map: dict, lane_states: dict[str, str]) -> bool:
    """Whether any extra lane with paths holds them now: when none does, nothing can be reserved."""
    return any(name != MAIN and lane["paths"] and lane_states.get(name) in (LIVE, DAMAGED)
               for name, lane in lane_map["lanes"].items())


def beat(name: str, pid: int, task: str) -> Path:
    if not _pid_ok(pid):
        raise LaneError(f"pid {pid} is not a process id (1 to {PID_MAX})")
    d = state_dir()
    d.mkdir(parents=True, exist_ok=True)
    path = d / f"{name}.json"
    tmp = d / f".{name}.json.{os.getpid()}.tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump({"lane": name, "pid": pid, "heartbeat": time.time(), "task": task}, f)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    return path


def clear(name: str) -> None:
    if re.fullmatch(r"[a-z][a-z0-9-]*", name) is None:       # a record name, never a path
        raise LaneError(f"{name!r} is not a lane name")
    try:
        (state_dir() / f"{name}.json").unlink()
    except FileNotFoundError:
        pass


def _git(root: Path, *args: str, deadline: float | None = None) -> bytes:
    """git's stdout, bounded by GIT_SECS, and by `deadline` (time.monotonic()) when a caller shares a budget."""
    secs = GIT_SECS if deadline is None else min(GIT_SECS, deadline - time.monotonic())
    if secs <= 0:
        raise LaneError(f"git {args[0]} has no time left in its caller's budget")
    try:
        # A local replacement ref must not change what the check sees: it judges the objects git stores.
        r = subprocess.run(["git", "--no-replace-objects", "-C", str(root), *args], capture_output=True,
                           timeout=secs)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise LaneError(f"git {args[0]} could not run ({type(exc).__name__})") from exc
    if r.returncode != 0:
        raise LaneError(f"git {' '.join(args[:2])} failed: {r.stderr.decode(errors='replace').strip()[:200]}")
    return r.stdout


def committer(lane_map: dict, toplevel: Path) -> str | None:
    """The lane whose worktree is this tree, or None for a tree that is no lane's (an attended worktree)."""
    common = Path(_git(toplevel, "rev-parse", "--path-format=absolute", "--git-common-dir").decode().strip())
    primary = common.resolve().parent
    here = Path(toplevel).resolve()
    seen: dict[Path, str] = {}
    for name, lane in lane_map["lanes"].items():
        tree = (primary / lane["worktree"]).resolve()
        if tree in seen:                           # a symlink alias of another lane's tree: whose would it be?
            raise LaneError(f"lanes {seen[tree]} and {name} name the same worktree {tree}")
        seen[tree] = name
    return seen.get(here)


def staged_paths(toplevel: Path, base: str | None = None) -> list[str]:
    """The paths the index changes against HEAD, or against commit `base`."""
    out = _git(toplevel, "diff", "--cached", "--name-only", "-z", "--no-renames", "--no-ext-diff",
               *([base] if base else []))
    return [p for p in out.decode("utf-8", errors="surrogateescape").split("\0") if p]


def judge(lane_map: dict, me: str | None, paths: list[str], lane_states: dict[str, str]) -> list[str]:
    """The reasons this commit is refused; empty when it is allowed."""
    lanes = lane_map["lanes"]
    if me is not None and lanes[me]["output"] == "reports":
        return [f"lane {me} only writes reports and never commits"]
    if me not in (None, MAIN):
        mine = lane_states.get(me)
        if mine != LIVE:
            why = "its record cannot be read" if mine == DAMAGED else "it has no fresh record"
            return [f"lane {me} is not live ({why}); start it with its launcher, or clear the record with "
                    f"`python3 scripts/lanes.py clear {me}`"]
    reasons = []
    for p in paths:
        if p in lane_map["generated"]:
            continue
        if me not in (None, MAIN):
            if owner(lane_map, p) != me:
                reasons.append(f"{p} is outside lane {me}'s paths")
            continue
        held = reserved_by(lane_map, lane_states, p)
        if held:
            how = "live" if lane_states[held] == LIVE else "reserved (its record cannot be read)"
            reasons.append(f"{p} belongs to lane {held}, which is {how}")
    return reasons


def _blob(toplevel: Path, spec: str, deadline: float | None = None) -> str | None:
    try:
        return _git(toplevel, "show", spec, deadline=deadline).decode("utf-8")
    except (LaneError, UnicodeDecodeError):
        return None


def committed_map(toplevel: Path, deadline: float | None = None) -> dict:
    """The map in force before this commit: HEAD's, so an edit in progress (staged or not) never releases a
    reservation. Only the commit that first adds the map is judged by the copy it stages. `deadline` bounds
    both reads together (triage runs inside the Stop hook's budget)."""
    text = _blob(toplevel, f"HEAD:{MAP}", deadline)
    if text is None:
        text = _blob(toplevel, f":{MAP}", deadline)
    if text is None:
        raise LaneError(f"{MAP} is in neither HEAD nor the index")
    return parse_map(text)


STATE_FILE_MAX = 64 * 1024


def _git_path(toplevel: Path, name: str) -> Path:
    return Path(_git(toplevel, "rev-parse", "--path-format=absolute", "--git-path", name).decode().strip())


def _state_lines(path: Path) -> list[str]:
    """The lines of a small git state file (MERGE_HEAD, REBASE_HEAD), [] when it is absent. Anything but a
    small regular file refuses: a FIFO in its place would otherwise hold the commit forever."""
    try:
        st = os.stat(path)
    except FileNotFoundError:
        return []
    except OSError as exc:
        raise LaneError(f"{path.name} cannot be read ({type(exc).__name__})") from exc
    if not stat.S_ISREG(st.st_mode) or st.st_size > STATE_FILE_MAX:
        raise LaneError(f"{path.name} is not a small regular file")
    try:
        return [line.strip() for line in path.read_text(encoding="ascii").splitlines() if line.strip()]
    except (OSError, UnicodeDecodeError) as exc:
        raise LaneError(f"{path.name} cannot be read ({type(exc).__name__})") from exc


def other_parents(toplevel: Path) -> list[str]:
    """The commits this commit's result is also measured against: a pending merge's other parents (MERGE_HEAD,
    one line each for an octopus), or, in a rebase stopped on a commit, that commit (REBASE_HEAD)."""
    merging = _state_lines(_git_path(toplevel, "MERGE_HEAD"))
    if merging:
        return merging
    if _git_path(toplevel, "rebase-merge").is_dir() or _git_path(toplevel, "rebase-apply").is_dir():
        return _state_lines(_git_path(toplevel, "REBASE_HEAD"))[:1]
    return []


def authored_paths(toplevel: Path, parents: list[str]) -> list[str]:
    """The paths this commit authors. A path whose result equals some parent's (a merge's other parent, the
    commit a stopped rebase replays) is inherited from a commit already judged; only a result that differs from
    every one of them (a resolution, an extra edit) is this commit's."""
    paths = staged_paths(toplevel)
    for parent in parents:
        theirs = set(staged_paths(toplevel, parent))
        paths = [p for p in paths if p in theirs]
    return paths


def check_staged(cwd: Path, replay: bool = False) -> int:
    """Judge the staged commit on the paths it authors (`authored_paths`). `replay` is a rebase's own commit
    of a pick: a clean pick replays a commit already judged, so only who may commit at all is judged; a pick the
    rebase stopped on (REBASE_HEAD) is judged on what was added to it."""
    try:
        toplevel = Path(_git(cwd, "rev-parse", "--show-toplevel").decode().strip())
        lane_map = committed_map(toplevel)
        me = committer(lane_map, toplevel)
        # The map the commit leaves behind is validated on every path, a merge or rebase included: a map
        # that does not validate never lands, because every later commit is judged by it.
        if MAP in staged_paths(toplevel):
            staged = _blob(toplevel, f":{MAP}")
            if staged is None:
                raise LaneError(f"this commit deletes {MAP}; every later commit needs it")
            parse_map(staged)
        parents = other_parents(toplevel)
        paths = [] if replay and not parents else authored_paths(toplevel, parents)
        reasons = judge(lane_map, me, paths, states(lane_map))
    except LaneError as exc:
        print(f"[lane-check] BLOCK -- the lane check cannot judge this commit: {exc}", file=sys.stderr)
        return 2
    if not reasons:
        return 0
    if os.environ.get("SKIP_LANE_CHECK") == "1":
        reason = os.environ.get("SKIP_LANE_CHECK_REASON", "").strip()
        if reason:
            print(f"[lane-check] SKIP_LANE_CHECK=1 ({reason}): allowed despite: {'; '.join(reasons[:5])}",
                  file=sys.stderr)
            return 0
    who = me or "a tree that is no lane's (judged as main)"
    print(f"[lane-check] BLOCK -- commit by {who} refused:", file=sys.stderr)
    for r in reasons[:20]:
        print(f"[lane-check]   {r}", file=sys.stderr)
    if len(reasons) > 20:
        print(f"[lane-check]   (+{len(reasons) - 20} more)", file=sys.stderr)
    print("[lane-check]   Leave a live lane's files to it. Override (stated reason required): "
          "SKIP_LANE_CHECK=1 SKIP_LANE_CHECK_REASON=\"...\"", file=sys.stderr)
    return 1


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__, file=sys.stderr)
        return 2
    cmd, rest = argv[0], argv[1:]
    if cmd == "check-staged" and rest in ([], ["--replay"]):
        return check_staged(Path.cwd(), replay=bool(rest))
    root = Path(__file__).resolve().parent.parent
    try:
        lane_map = load_map(root)
        if cmd == "beat" and len(rest) == 5 and rest[1] == "--pid" and rest[3] == "--task":
            name = rest[0]
            if name == MAIN or name not in lane_map["lanes"]:
                raise LaneError(f"{name!r} is not an extra lane in the map")
            print(beat(name, int(rest[2]), rest[4]))
            return 0
        if cmd == "clear" and len(rest) == 1:
            if rest[0] == MAIN or rest[0] not in lane_map["lanes"]:
                raise LaneError(f"{rest[0]!r} is not an extra lane in the map")
            clear(rest[0])
            return 0
        if cmd == "live" and not rest:
            print(json.dumps(states(lane_map)))
            return 0
        if cmd == "owner" and len(rest) == 1:
            print(owner(lane_map, rest[0]))
            return 0
    except (LaneError, ValueError) as exc:
        print(f"[lanes] {exc}", file=sys.stderr)
        return 2
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
