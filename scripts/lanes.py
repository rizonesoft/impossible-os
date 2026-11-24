#!/usr/bin/env python3
"""The lane map and the live lane records: the one reader and writer of both (the lane map and the commit check, in the automation-hardening TODO).

The map is .conclave/lanes.toml. A live extra lane keeps a record at
$XDG_STATE_HOME/impossible-os/lanes/<lane>.json (default ~/.local/state); it is live while the record names it,
its pid exists and its heartbeat is within stale_secs. A record that exists but cannot be read is "damaged": it
reserves the lane's paths against everyone else and gives the lane itself no right to commit.

    python3 scripts/lanes.py beat <lane> --pid P --task T   # write the lane's record (temp file plus rename)
    python3 scripts/lanes.py clear <lane>                   # remove it
    python3 scripts/lanes.py live                           # each extra lane's state: live, off or damaged
    python3 scripts/lanes.py check-staged                   # the commit check (.githooks/pre-commit, prepare-commit-msg)
    python3 scripts/lanes.py check-staged --replay          # a rebase's own commit of a pick

check-staged exits 0 to allow, 1 to refuse, 2 when it cannot judge (a commit is refused for both).
SKIP_LANE_CHECK=1 with a SKIP_LANE_CHECK_REASON allows the commit and prints the reason.
"""
from __future__ import annotations

import bisect
import errno
import json
import math
import os
import posixpath
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
CHECKOUT_DIRS = (".conclave", "scripts")           # the map and this script live in every checkout


class LaneError(Exception):
    """The map or the repository cannot be read, so nothing can be judged."""


class GitStalled(LaneError):
    """git did not run to an answer (it timed out or could not start), so nothing about the repository follows
    from it: never read as "absent"."""


def _need(cond, msg):
    if not cond:
        raise LaneError(f".conclave/lanes.toml: {msg}")


def _canonical(v) -> bool:
    """A repository path in git's own spelling: relative, `/`-separated, no `.`, `..` or empty component. Git
    reports `user/x.c`, so a map entry spelled `./user/` would match nothing and release its reservation."""
    if not isinstance(v, str) or re.fullmatch(r"[\w.-][\w./-]*", v) is None:    # the characters a path token has
        return False
    parts = v[:-1].split("/") if v.endswith("/") else v.split("/")
    return all(p not in ("", ".", "..") for p in parts)


def _strings(value) -> bool:
    return isinstance(value, list) and all(_canonical(v) for v in value)


MAP = ".conclave/lanes.toml"
MAP_MAX, LINE_MAX, ENTRY_MAX, NAME_MAX = 128 * 1024, 500, 240, 64     # bytes, characters: what keeps reading and judging by it quick
AMENDED = ("COUNT.md", "README.md")                  # .githooks/post-commit


def parse_map(text: str) -> dict:
    """The validated map: {'stale_secs', 'generated', 'lanes': {name: table}, 'index': {paths entry: lane},
    'entries': every paths entry, sorted, 'tops': their first components, 'longest': the longest entry's length, 'bare': the lanes with an entry that has no slash}.
    Anything unexpected refuses."""
    _need(len(text.encode(errors="replace")) <= MAP_MAX, f"the map is longer than {MAP_MAX} bytes")
    # A key with thousands of dots takes memory by its square, and is all on one line.
    _need(all(len(line) <= LINE_MAX for line in text.split("\n")), f"the map has a line longer than {LINE_MAX} characters")
    try:
        data = tomllib.loads(text)
    except (tomllib.TOMLDecodeError, RecursionError) as exc:
        raise LaneError(f"{MAP} cannot be read ({type(exc).__name__}: {exc})") from exc
    _need(set(data) <= TOP_KEYS, f"unknown keys {sorted(set(data) - TOP_KEYS)}")
    stale = data.get("stale_secs", 180)
    _need(isinstance(stale, int) and not isinstance(stale, bool) and stale > 0, "stale_secs must be a positive integer")
    generated = data.get("generated", [])
    _need(_strings(generated), "generated must be a list of relative paths")
    _need(set(AMENDED) <= set(generated), f"generated must hold {' and '.join(AMENDED)}: post-commit amends them "
          "into every lane's commits")
    lanes = data.get("lane")
    _need(isinstance(lanes, dict) and MAIN in lanes, "needs a [lane.main] table")
    owned: list[tuple[str, str]] = []
    trees: dict[str, str] = {}
    for name, lane in lanes.items():
        _need(re.fullmatch(r"[a-z][a-z0-9-]*", name) is not None, f"lane name {name!r} is not lower-case-with-dashes")
        _need(len(name) <= NAME_MAX, f"lane name {name[:20]!r}... is longer than {NAME_MAX} characters")
        _need(isinstance(lane, dict) and set(lane) == LANE_KEYS,
              f"[lane.{name}] needs exactly {sorted(LANE_KEYS)}")
        _need(all(isinstance(lane[k], str) and lane[k] for k in ("engine", "model", "worktree")),
              f"[lane.{name}] engine, model and worktree must be non-empty strings")
        _need(not os.path.isabs(lane["worktree"]), f"[lane.{name}] worktree must be relative to the primary tree")
        _need("\0" not in lane["worktree"], f"[lane.{name}] worktree must hold no NUL")
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
            _need(len(p) <= ENTRY_MAX, f"[lane.{name}] path {p[:40]!r}... is longer than {ENTRY_MAX} characters")
            owned.append((p, name))
    # Sorted, whatever a directory prefix holds comes straight after it, so neighbours are the only overlaps.
    owned.sort()
    for (p, name), (q, other) in zip(owned, owned[1:]):
        _need(not _covers(p, q), f"{other} path {q!r} overlaps {name} path {p!r}")
    entries = [p for p, _name in owned]
    named = sorted([*entries, *generated])
    for p in named:
        q = None if p.endswith("/") else next(_under(named, p), None)
        _need(q is None, f"path {p!r} is a file where path {q!r} needs a directory")
    return {"stale_secs": stale, "generated": list(generated), "lanes": lanes, "index": dict(owned), "entries": entries,
            "tops": {p.partition("/")[0] for p in entries},
            "longest": max((len(p) for p, _name in owned), default=0),
            "bare": {name for p, name in owned if "/" not in p}}


def _under(entries: list[str], path: str):
    """The sorted entries that need a directory at `path`: whatever starts with it and a slash is one run of them."""
    prefix = path + "/"
    at = bisect.bisect_left(entries, prefix)
    while at < len(entries) and entries[at].startswith(prefix):
        yield entries[at]
        at += 1


def _covers(entry: str, path: str) -> bool:
    """A paths entry holds a path: a prefix ending in "/" holds everything under it, anything else is one file."""
    return path.startswith(entry) if entry.endswith("/") else path == entry


def owner(lane_map: dict, path: str) -> str:
    """The lane whose paths hold `path`, else main: one lookup for each directory above it, or none when no entry
    starts as it does."""
    if path.partition("/")[0] not in lane_map["tops"]:
        return MAIN
    index = lane_map["index"]
    if path in index:
        return index[path]
    at = path.find("/")
    while 0 <= at < lane_map["longest"]:
        if path[:at + 1] in index:
            return index[path[:at + 1]]
        at = path.find("/", at + 1)
    return MAIN


def state_dir() -> Path:
    base = os.environ.get("XDG_STATE_HOME", "")
    if not os.path.isabs(base):                    # the XDG rule: relative is ignored (it would differ per worktree)
        base = os.path.join(os.path.expanduser("~"), ".local", "state")
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


RECORD_MAX = 64 * 1024


def _read_record(path: Path) -> bytes:
    """A record's bytes. Anything but a small regular file is an OSError other than FileNotFoundError: a FIFO in its
    place must not hold the commit that reads it, and a link to nothing is not an absent record."""
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW | os.O_CLOEXEC)
    with open(fd, "rb") as f:
        st = os.fstat(f.fileno())
        if not stat.S_ISREG(st.st_mode) or st.st_size > RECORD_MAX:
            raise OSError(errno.EINVAL, "not a small regular file", str(path))
        return f.read(RECORD_MAX)


def lane_state(lane_map: dict, name: str, now: float | None = None) -> str:
    """live, off (no record, or a stale or dead one) or damaged (a record that cannot be read)."""
    path = state_dir() / f"{name}.json"
    try:
        raw = _read_record(path)
    except FileNotFoundError:
        return OFF
    except OSError:
        return DAMAGED                             # fail-direction: an unreadable record reserves, never frees
    try:
        rec = json.loads(raw)
        pid, beat = rec["pid"], rec["heartbeat"]
        ok = (isinstance(rec, dict) and rec.get("lane") == name and _pid_ok(pid) and isinstance(rec["task"], str)
              and isinstance(beat, (int, float)) and not isinstance(beat, bool) and math.isfinite(beat))
    except (ValueError, KeyError, TypeError, OverflowError, RecursionError):
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


def displaced_by(lane_map: dict, lane_states: dict[str, str], path: str) -> str | None:
    """The extra lane whose reserved paths cannot exist beside a file at `path`, or None: the file is a directory one
    of them needs, or it is under one that is a file."""
    if path.partition("/")[0] not in lane_map["tops"]:
        return None
    index = lane_map["index"]
    names = {index[q] for q in _under(lane_map["entries"], path)}
    at = path.find("/")
    while 0 <= at <= lane_map["longest"]:
        names.add(index.get(path[:at], MAIN))
        at = path.find("/", at + 1)
    return next((n for n in sorted(names) if n != MAIN and lane_states.get(n) in (LIVE, DAMAGED)), None)


# A run of path characters that is not the rest of an absolute path or a URL; the second holds a slash.
_WORD = re.compile(r"(?<![\w./-])[\w.-][\w./-]*")
_PATH = re.compile(r"(?<![\w./-])[\w.-]+/[\w./-]*")


def _paths_named(text: str, folder: str, bare: bool):
    """The repository paths `text` names, words with no slash only when `bare`. `./` is the repository root, and a
    path that then starts `../` is relative to `folder`, the text's own directory. A sentence's full stop is
    dropped, and a run in Markdown emphasis (`_user/x_`) is read both with and without its underscores."""
    for m in (_WORD if bare else _PATH).finditer(text):
        word = m.group()
        dots = len(word) - len(word.rstrip("."))
        if dots and dots != 2:                         # a full stop or an ellipsis; two are the parent directory
            word = word[:-dots]
        words = [word]
        if word[:1] == "_" or word[-1:] == "_":
            words.append(word.lstrip("_").rstrip("_."))
        for word in words:
            if "/" not in word:
                yield word
                continue
            word = word[re.match(r"(?:\./+)*", word).end():]
            path = posixpath.normpath(posixpath.join(folder, word) if word.startswith("../") else word)
            yield path + ("/" if word.endswith("/") else "")


def _is_prose(lane_map: dict, path: str) -> bool:
    """Whether a run under a one-word directory is two words (`user/kernel`, `user/pass/host`): a path there ends in a
    slash or a file's name, which has a full stop, an underscore, a digit or a capital."""
    last = path.rpartition("/")[2]
    return path[:path.index("/") + 1] in lane_map["index"] and bool(last) and not re.search(r"[._A-Z0-9]", last)


def names_reserved_path(lane_map: dict, lane_states: dict[str, str], text: str, folder: str = "") -> str | None:
    """The live (or damaged) lane one of whose paths `text` names, or None: a section whose text says it edits
    a file in a lane cannot be finished outside that lane. A TODO's path is a cross-reference, not an edit, and a
    one-word directory needs its slash (`user` alone is a word) and a file under it (_is_prose). `folder` is the text's own directory."""
    bare = any(lane_states.get(name) in (LIVE, DAMAGED) for name in lane_map["bare"])
    for path in _paths_named(text, folder, bare):
        if "/" not in path:                            # most words: one lookup, no directory above it
            name = lane_map["index"].get(path, MAIN)
        elif path.startswith("todo/") or _is_prose(lane_map, path):
            continue
        else:
            name = owner(lane_map, path)
        if name == MAIN and "/" in path:
            name = owner(lane_map, path + "/")         # `src/apps` names the directory `src/apps/`
        if name != MAIN and lane_states.get(name) in (LIVE, DAMAGED):
            return name
    return None


def any_live(lane_states: dict[str, str]) -> bool:
    """Whether any extra lane is live, or has a record that cannot be read."""
    return any(s in (LIVE, DAMAGED) for s in lane_states.values())


def any_reserving(lane_map: dict, lane_states: dict[str, str]) -> bool:
    """Whether any extra lane with paths holds them now: when none does, nothing can be reserved."""
    return any(name != MAIN and lane["paths"] and lane_states.get(name) in (LIVE, DAMAGED)
               for name, lane in lane_map["lanes"].items())


def has_records() -> bool:
    """Whether any lane has a record. A records directory that cannot be listed raises (OSError): it never reads
    as no record."""
    try:
        return any(n.endswith(".json") and not n.startswith(".") for n in os.listdir(state_dir()))
    except FileNotFoundError:
        return False


def beat(name: str, pid: int, task: str) -> Path:
    if not _pid_ok(pid):
        raise LaneError(f"pid {pid} is not a process id (1 to {PID_MAX})")
    record = json.dumps({"lane": name, "pid": pid, "heartbeat": time.time(), "task": task}).encode()
    if len(record) > RECORD_MAX:
        raise LaneError(f"the task is too long for a record ({len(record)} bytes; at most {RECORD_MAX})")
    d = state_dir()
    d.mkdir(parents=True, exist_ok=True)
    path = d / f"{name}.json"
    tmp = d / f".{name}.json.{os.getpid()}.tmp"
    with open(tmp, "wb") as f:
        f.write(record)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    return path


def clear(name: str) -> None:
    if name == MAIN or re.fullmatch(r"[a-z][a-z0-9-]*", name) is None:   # a record name, never a path
        raise LaneError(f"{name!r} is not a lane name")
    try:
        (state_dir() / f"{name}.json").unlink()
    except FileNotFoundError:
        pass


def _git(root: Path, *args: str, deadline: float | None = None, ok: tuple[int, ...] = (0,),
         stdin: bytes | None = None) -> bytes:
    """git's stdout, bounded by GIT_SECS, and by `deadline` (time.monotonic()) when a caller shares a budget.
    An exit status outside `ok` refuses."""
    secs = GIT_SECS if deadline is None else min(GIT_SECS, deadline - time.monotonic())
    if secs <= 0:
        raise GitStalled(f"git {args[0]} has no time left in its caller's budget")
    try:
        # A local replacement ref must not change what the check sees: it judges the objects git stores.
        r = subprocess.run(["git", "--no-replace-objects", "-c", "core.fsmonitor=false", "-C", str(root), *args], capture_output=True,
                           input=stdin, timeout=secs)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise GitStalled(f"git {args[0]} could not run ({type(exc).__name__})") from exc
    if r.returncode not in ok:
        raise LaneError(f"git {' '.join(args[:2])} failed: {r.stderr.decode(errors='replace').strip()[:200]}")
    return r.stdout


def _rev_path(toplevel: Path, *args: str) -> Path:
    """The path git rev-parse prints, byte for byte (a name may end in a space or not be UTF-8)."""
    return Path(os.fsdecode(_git(toplevel, "rev-parse", *args).removesuffix(b"\n")))


def lane_trees(lane_map: dict, toplevel: Path) -> dict[Path, str]:
    """Each lane's worktree as the directory it resolves to. Two lanes resolving to one directory (a symlink
    alias, or another spelling of the primary tree) refuse: the committing lane is found by its tree."""
    common = _rev_path(toplevel, "--path-format=absolute", "--git-common-dir")
    primary = common.resolve().parent
    # A git directory kept elsewhere does not say where the tree is: its parent holds no checkout (known by both
    # directories the check itself needs, so an unstaged deletion of one file changes nothing).
    if common.name != ".git" or not all((primary / d).is_dir() for d in CHECKOUT_DIRS):
        raise LaneError(f"the primary tree cannot be found from its git directory {common}")
    seen: dict[Path, str] = {}
    for name, lane in lane_map["lanes"].items():
        try:
            tree = (primary / lane["worktree"]).resolve()
        except (OSError, RuntimeError, ValueError) as exc:         # RuntimeError: a symlink loop, before 3.13
            raise LaneError(f"lane {name}'s worktree cannot be resolved ({type(exc).__name__})") from exc
        if tree in seen:
            raise LaneError(f"lanes {seen[tree]} and {name} name the same worktree {tree}")
        seen[tree] = name
    return seen


def committer(lane_map: dict, toplevel: Path) -> str | None:
    """The lane whose worktree is this tree, or None for a tree that is no lane's (an attended worktree)."""
    return lane_trees(lane_map, toplevel).get(Path(toplevel).resolve())


def staged_paths(toplevel: Path, *only: str) -> dict[str, bool]:
    """The paths the index changes against HEAD, or which of `only` it changes, and whether it removes each."""
    out = _git(toplevel, "diff", "--cached", "--name-status", "-z", "--no-renames", "--no-ext-diff",
               "--ignore-submodules=none", "--", *only)         # diff.ignoreSubmodules must not hide a staged path
    fields = out.decode("utf-8", errors="surrogateescape").split("\0")
    return {p: status == "D" for status, p in zip(fields[::2], fields[1::2])}


def judge(lane_map: dict, me: str | None, paths, lane_states: dict[str, str], removed=frozenset()) -> list[str]:
    """The reasons this commit is refused; empty when it is allowed. `removed`: the paths it deletes, which leave
    nothing in a lane's way."""
    lanes = lane_map["lanes"]
    if me is not None and lanes[me]["output"] == "reports":
        return [f"lane {me} only writes reports and never commits"]
    if me not in (None, MAIN):
        mine = lane_states.get(me)
        if mine != LIVE:
            why = "its record cannot be read" if mine == DAMAGED else "it has no fresh record"
            return [f"lane {me} is not live ({why}); start it with its launcher, or clear the record with "
                    f"`python3 scripts/lanes.py clear {me}`"]
    elif not any_reserving(lane_map, lane_states):
        return []
    reasons = []
    generated = set(lane_map["generated"])
    for p in paths:
        if p in generated:
            continue
        if me not in (None, MAIN):
            if owner(lane_map, p) != me:
                reasons.append(f"{p} is outside lane {me}'s paths")
            continue
        held = reserved_by(lane_map, lane_states, p)
        if not held and p not in removed:
            held = displaced_by(lane_map, lane_states, p)
        if held:
            how = "live" if lane_states[held] == LIVE else "reserved (its record cannot be read)"
            reasons.append(f"{p} belongs to lane {held}, which is {how}")
    return reasons


def _blob(toplevel: Path, spec: str, deadline: float | None = None) -> str | None:
    """The blob's text, or None when git answered that it does not exist. A stalled git raises (GitStalled), and
    so does a blob that is there and is not text, or is longer than a map may be, which is not read: a broken map
    never reads as an absent one."""
    said = _git(toplevel, "cat-file", "--batch-check=%(objecttype) %(objectsize) %(objectname)", deadline=deadline,
                stdin=spec.encode() + b"\n").split()
    if said[-1:] == [b"missing"]:
        return None
    if len(said) != 3 or said[0] != b"blob":
        raise LaneError(f"{spec} is not a file")
    if int(said[1]) > MAP_MAX:
        raise LaneError(f"{spec} is longer than {MAP_MAX} bytes")
    data = _git(toplevel, "cat-file", "blob", said[2].decode(), deadline=deadline)
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise LaneError(f"{spec} cannot be read ({exc.reason})") from exc


def committed_map(toplevel: Path, deadline: float | None = None) -> dict:
    """The map in force before this commit: HEAD's, so an edit in progress (staged or not) never releases a
    reservation. `deadline` bounds the read (triage runs inside the Stop hook's budget)."""
    text = _blob(toplevel, f"HEAD:{MAP}", deadline)
    if text is None:
        raise LaneError(f"{MAP} is not in HEAD")
    lane_map = parse_map(text)
    if deadline is not None and time.monotonic() > deadline:
        raise GitStalled(f"{MAP} was read after its caller's budget was spent")
    return lane_map


def _git_path(toplevel: Path, name: str) -> Path:
    return _rev_path(toplevel, "--path-format=absolute", "--git-path", name)


def merging(toplevel: Path) -> bool:
    """Whether this commit concludes a merge (MERGE_HEAD is there, whatever it holds: it is never read)."""
    return os.path.lexists(_git_path(toplevel, "MERGE_HEAD"))


def replaying(toplevel: Path) -> bool:
    """Whether a rebase is making a clean pick's commit: it replays a commit already judged. A pick the rebase
    stopped on (REBASE_HEAD) was finished by hand, so it is not one."""
    return (_git_path(toplevel, "rebase-merge").is_dir()
            and not _git(toplevel, "rev-parse", "-q", "--verify", "REBASE_HEAD", ok=(0, 1)).strip())


def check_staged(cwd: Path, replay: bool = False) -> int:
    """Judge the staged commit on every path the index changes against HEAD. Two things are not told apart from
    that: a rebase's own commit of a clean pick (`replay`) is judged only on who may commit at all, and a merge is
    refused while any lane holds paths, because the index cannot show a lane's incoming change it lost."""
    me = None
    try:
        toplevel = _rev_path(cwd, "--show-toplevel")
        # The map the commit leaves behind is validated on every path, a merge or rebase included, and as
        # fully as HEAD's is (worktrees resolved): a map that does not validate never lands, because every
        # later commit is judged by it.
        proposed, reasons, refused = None, [], 1
        clean = replay and replaying(toplevel)
        changed = staged_paths(toplevel, *([MAP] if clean else []))
        if MAP in changed:
            staged = _blob(toplevel, f":{MAP}")
            if staged is None:
                raise LaneError(f"this commit deletes {MAP}; every later commit needs it")
            proposed = parse_map(staged)
            trees = lane_trees(proposed, toplevel)
        try:
            lane_map = committed_map(toplevel)
            me = committer(lane_map, toplevel)
        except GitStalled:
            raise
        except LaneError:
            if proposed is None:
                raise
            # HEAD's map is broken or not there, so the commit that repairs or adds it is judged by its own,
            # unless a lane has a record: nothing then says which paths that lane holds.
            try:
                recorded = has_records()
            except OSError as exc:
                raise LaneError(f"the lane records cannot be listed ({type(exc).__name__})") from exc
            if recorded:
                raise LaneError(f"HEAD's {MAP} is broken or missing and a lane has a record in {state_dir()}; stop the lane "
                                "and clear its record before repairing the map") from None
            lane_map = proposed
            me = trees.get(toplevel.resolve())
        lane_states = states(lane_map)
        paths = {} if clean else changed
        reasons = judge(lane_map, me, paths, lane_states, {p for p, gone in paths.items() if gone})
        if not reasons and any_live(lane_states):
            if merging(toplevel):
                reasons = ["no merge commits while a lane is live; rebase instead"]
            elif proposed is not None and proposed != lane_map:
                reasons = [f"{MAP} does not change while a lane is live: its worktree still commits by the old one"]
    except LaneError as exc:
        reasons, refused = [f"the lane check cannot judge this commit: {exc}"], 2
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
    return refused


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__, file=sys.stderr)
        return 2
    cmd, rest = argv[0], argv[1:]
    if cmd == "check-staged" and rest in ([], ["--replay"]):
        return check_staged(Path.cwd(), replay=bool(rest))
    root = Path(__file__).resolve().parent.parent
    try:
        # Needs no map: a record is cleared after its lane left the map, and while the map is broken.
        if cmd == "clear" and len(rest) == 1:
            clear(rest[0])
            return 0
        lane_map = committed_map(root)
        if cmd == "beat" and len(rest) == 5 and rest[1] == "--pid" and rest[3] == "--task":
            name = rest[0]
            if name == MAIN or name not in lane_map["lanes"]:
                raise LaneError(f"{name!r} is not an extra lane in the map")
            print(beat(name, int(rest[2]), rest[4]))
            return 0
        if cmd == "live" and not rest:
            print(json.dumps(states(lane_map)))
            return 0
    except (LaneError, ValueError) as exc:
        print(f"[lanes] {exc}", file=sys.stderr)
        return 2
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
