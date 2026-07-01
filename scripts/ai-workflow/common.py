#!/usr/bin/env python3
"""Common helpers for the repo-owned AI workflow protocol."""
from __future__ import annotations

import datetime as _dt
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator


REVIEW_KINDS = (
    "design",
    "adversarial-impl",
    "adversarial",
    "consistency",
    "perf",
    "test-coverage",
    "gap-audit",
    "re-adversarial",
)

REQUIRED_SHIP_REVIEW_KINDS = ("adversarial", "consistency", "perf")
GIT_BLOB_BATCH_SIZE = 128


def repo_root() -> Path:
    env = os.environ.get("AI_WORKFLOW_REPO_ROOT")
    if env:
        return Path(env).resolve()
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True,
            stderr=subprocess.DEVNULL,
            timeout=3,
        ).strip()
        if out:
            return Path(out).resolve()
    except Exception:
        pass
    return Path.cwd().resolve()


def state_dir(root: Path | None = None) -> Path:
    env = os.environ.get("AI_WORKFLOW_STATE_DIR")
    if env:
        path = Path(env)
        if not path.is_absolute():
            path = (repo_root() / path).resolve()
        return path
    root = root or repo_root()
    return root / ".ai-workflow"


def ensure_state_dir(root: Path | None = None) -> Path:
    path = state_dir(root)
    path.mkdir(parents=True, exist_ok=True)
    return path


def now_ns() -> int:
    return time.time_ns()


def now_iso() -> str:
    return _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat()


def today_iso() -> str:
    return _dt.date.today().isoformat()


def read_json(path: Path, default: Any) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return default


def write_json_atomic(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def append_jsonl(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as f:
        f.write(json.dumps(data, sort_keys=True) + "\n")


# Process-local reentrancy depth per lock path. flock is per open-file-description,
# so a nested acquire on a fresh fd would self-deadlock; the depth counter lets a
# holder (e.g. stamp.py's atomic write+record critical section) call helpers that
# themselves take the lock (evidence.append_event). Single-threaded-CLI assumption:
# these tools never share a process across threads.
_LOCK_DEPTH: dict[str, int] = {}


@contextmanager
def workflow_lock(
    root: Path | None = None,
    *,
    name: str = "workflow.lock",
    timeout_sec: float = 5.0,
) -> Iterator[None]:
    root = root or repo_root()
    lock_path = ensure_state_dir(root) / name
    key = str(lock_path)
    if _LOCK_DEPTH.get(key, 0) > 0:
        _LOCK_DEPTH[key] += 1
        try:
            yield
        finally:
            _LOCK_DEPTH[key] -= 1
        return
    try:
        import fcntl
    except Exception as exc:
        # Fail closed: no silent lock-free fallback. The lock contract (one active
        # mutator) cannot hold without a real OS lock, and no O_EXCL fallback is
        # implemented, so refuse rather than let two sessions both "win".
        raise RuntimeError(
            f"workflow_lock requires fcntl (POSIX flock); refusing to run lock-free "
            f"for {lock_path}"
        ) from exc

    fd = os.open(str(lock_path), os.O_RDWR | os.O_CREAT, 0o600)
    locked = False
    deadline = time.monotonic() + timeout_sec
    try:
        while True:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                locked = True
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise TimeoutError(f"timed out acquiring workflow lock: {lock_path}")
                time.sleep(0.05)
        _LOCK_DEPTH[key] = 1
        try:
            yield
        finally:
            _LOCK_DEPTH[key] = 0
    finally:
        if locked:
            try:
                fcntl.flock(fd, fcntl.LOCK_UN)
            except Exception:
                pass
        os.close(fd)


def iter_jsonl(path: Path) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    if not path.exists():
        return out
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except Exception:
            continue
        if isinstance(obj, dict):
            out.append(obj)
    return out


def iter_jsonl_reverse(path: Path, block_size: int = 65536):
    if not path.exists():
        return
    with path.open("rb") as f:
        f.seek(0, os.SEEK_END)
        pos = f.tell()
        buf = b""
        while pos > 0:
            size = min(block_size, pos)
            pos -= size
            f.seek(pos)
            chunk = f.read(size) + buf
            lines = chunk.split(b"\n")
            buf = lines[0]
            for line in reversed(lines[1:]):
                text = line.decode("utf-8", errors="replace").strip()
                if not text:
                    continue
                try:
                    obj = json.loads(text)
                except Exception:
                    continue
                if isinstance(obj, dict):
                    yield obj
        text = buf.decode("utf-8", errors="replace").strip()
        if text:
            try:
                obj = json.loads(text)
            except Exception:
                return
            if isinstance(obj, dict):
                yield obj


def head_sha(root: Path | None = None) -> str:
    root = root or repo_root()
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"],
            cwd=str(root),
            text=True,
            stderr=subprocess.DEVNULL,
            timeout=3,
        ).strip()
    except Exception:
        return ""


def rel_path(path: str | Path, root: Path | None = None) -> str:
    root = root or repo_root()
    p = Path(path)
    if not p.is_absolute():
        return p.as_posix()
    try:
        return p.resolve().relative_to(root).as_posix()
    except Exception:
        return p.as_posix()


def file_digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return "sha256:" + h.hexdigest()


def git_blob_or_digest(path: str | Path, root: Path | None = None) -> str:
    root = root or repo_root()
    rel = rel_path(path, root)
    try:
        out = subprocess.check_output(
            ["git", "ls-files", "-s", "--", rel],
            cwd=str(root),
            text=True,
            stderr=subprocess.DEVNULL,
            timeout=3,
        ).strip()
        if out:
            parts = out.split()
            if len(parts) >= 2:
                return f"{parts[0]}:{parts[1]}"
    except Exception:
        pass
    full = root / rel
    if full.exists() and full.is_file():
            return file_digest(full)
    return ""


def worktree_blob_or_digest(path: str | Path, root: Path | None = None) -> str:
    root = root or repo_root()
    rel = rel_path(path, root)
    try:
        out = subprocess.check_output(
            ["git", "hash-object", "--", rel],
            cwd=str(root),
            text=True,
            stderr=subprocess.DEVNULL,
            timeout=3,
        ).strip()
        if out:
            return out
    except Exception:
        pass
    full = root / rel
    if full.exists() and full.is_file():
        return file_digest(full)
    return ""


def git_blobs_or_digests(paths: list[str], root: Path | None = None) -> dict[str, str]:
    root = root or repo_root()
    rels = list(dict.fromkeys(rel_path(path, root) for path in paths))
    tracked: dict[str, str] = {}
    for start in range(0, len(rels), GIT_BLOB_BATCH_SIZE):
        batch = rels[start:start + GIT_BLOB_BATCH_SIZE]
        try:
            out = subprocess.check_output(
                ["git", "ls-files", "-s", "--", *batch],
                cwd=str(root),
                text=True,
                stderr=subprocess.DEVNULL,
                timeout=5,
            )
            for line in out.splitlines():
                parts = line.split(None, 3)
                if len(parts) >= 4:
                    tracked[parts[3]] = f"{parts[0]}:{parts[1]}"
        except Exception:
            continue
    out: dict[str, str] = {}
    for rel in rels:
        blob = tracked.get(rel, "")
        if not blob:
            full = root / rel
            if full.exists() and full.is_file():
                blob = file_digest(full)
        out[rel] = blob
    return out


def source_binding_matches(recorded: object, current: object) -> bool:
    old = str(recorded or "")
    new = str(current or "")
    if old == new:
        return True
    old_parts = old.split(":", 1)
    new_parts = new.split(":", 1)
    old_blob = old_parts[1] if len(old_parts) == 2 and old_parts[0].isdigit() else old
    new_blob = new_parts[1] if len(new_parts) == 2 and new_parts[0].isdigit() else new
    if ":" not in old:
        return bool(old_blob and old_blob == new_blob)
    return False


def source_blobs(paths: list[str], root: Path | None = None) -> dict[str, str]:
    root = root or repo_root()
    return {
        rel: blob
        for rel, blob in git_blobs_or_digests(paths, root).items()
        if blob
    }


def event_id(payload: dict[str, Any]) -> str:
    stable = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(stable.encode("utf-8")).hexdigest()[:16]


def die(msg: str, code: int = 1) -> int:
    print(msg, file=sys.stderr)
    return code


def load_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


# XREF grammar lives in the ONE shared module scripts/ai-workflow/xref.py so the git
# hook (accepted_xref_block), todo-graph, and this stamp writer cannot drift. The
# writer predicates below are intentionally STRICTER than the git hook: the marker
# must sit INSIDE a parenthetical and the markers are item:/retrofit/helper; (a lone
# `at line` matches prose like `(see owner at line 99)` naming no checklist item).
# The git hook stays lenient so existing hand-edited stamps survive; the writer
# refuses to emit a non-owning line. Both strictness levels are defined in xref.py.
def _load_xref():
    import importlib.util as _ilu
    _p = Path(__file__).resolve().parent / "xref.py"
    _spec = _ilu.spec_from_file_location("ai_workflow_xref", str(_p))
    if _spec is None or _spec.loader is None:
        raise ImportError(f"cannot load required sibling {_p}")
    _mod = _ilu.module_from_spec(_spec)
    # Register before exec: xref.py's @dataclass resolves cls.__module__ via
    # sys.modules, which a bare spec_from_file_location load does not populate.
    sys.modules[_spec.name] = _mod
    _spec.loader.exec_module(_mod)
    return _mod


_xref = _load_xref()


def bare_xrefs(summary: str) -> list[str]:
    """Non-owning XREF clauses the stamp writer must refuse (delegates to the shared
    grammar; each ``XREF:`` clause validated independently so a bare clause is not
    masked by a later concrete one). The git-commit hook remains the authoritative
    backstop; this lets stamp.py refuse to write a poisoned line in the first place."""
    return _xref.writer_bare_xrefs(summary)


def has_concrete_todo_xref(summary: str) -> bool:
    """True when the summary carries >=1 concrete TODO XREF clause naming a real,
    greppable owner (delegates to the shared grammar)."""
    return _xref.writer_has_concrete(summary)
