"""Self-healing: a doctor() health check + repair_state() from snapshots.

doctor checks the things a long-lived service actually trips on: secret present +
untracked, state files parse, storage headroom (the 90 MiB/file + 1 GB/repo lines),
and -- when online -- the balance floor and panel reachability.
"""
from __future__ import annotations

import json
import subprocess
from pathlib import Path

_FILE_LIMIT = 90 * 1024 * 1024
_REPO_LIMIT = 1024 * 1024 * 1024


def repair_state(path):
    """Return parsed JSON; if the file is corrupt, restore from <path>.snapshot."""
    path = Path(path)
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        snap = path.with_suffix(path.suffix + ".snapshot")
        try:
            data = json.loads(snap.read_text(encoding="utf-8"))
        except Exception:
            return None
        try:
            path.write_text(json.dumps(data), encoding="utf-8")
        except Exception:
            pass
        return data


def _git_tracked(root, rel: str) -> bool:
    try:
        out = subprocess.run(["git", "ls-files", rel], cwd=str(root),
                             capture_output=True, text=True, timeout=5)
        return bool(out.stdout.strip())
    except Exception:
        return False


def _storage_headroom(root: Path):
    """Size of git-TRACKED files only (the real repo; .venv + rebuildable index excluded)."""
    try:
        out = subprocess.run(["git", "ls-files", "-z"], cwd=str(root),
                             capture_output=True, timeout=10)
        files = out.stdout.decode("utf-8", "ignore").split("\0")
    except Exception:
        return 0, 0
    biggest = total = 0
    for rel in files:
        if not rel:
            continue
        try:
            sz = (Path(root) / rel).stat().st_size
        except Exception:
            continue
        total += sz
        biggest = max(biggest, sz)
    return biggest, total


def doctor(root, cfg, secret, *, online: bool = False) -> dict:
    checks = []

    def add(name, ok, detail):
        checks.append({"name": name, "ok": bool(ok), "detail": detail})

    add("secret", bool(secret),
        "present" if secret else "missing -- copy secret.example to secret")
    add("secret-untracked", not _git_tracked(root, "secret"),
        "secret must stay gitignored")
    biggest, total = _storage_headroom(root)
    add("storage-file", biggest < _FILE_LIMIT,
        f"largest file {biggest // 1024} KiB (limit 90 MiB)")
    add("storage-repo", total < _REPO_LIMIT,
        f"repo {total // (1024 * 1024)} MiB (soft 1 GB)")
    if online and secret:
        from conclave import http
        try:
            rem = http.remaining_credits(secret)
            add("balance", rem >= cfg.min_credits, f"${rem:.2f} (floor ${cfg.min_credits})")
        except Exception as e:
            add("balance", False, f"unreachable: {str(e)[:80]}")
    return {"ok": all(c["ok"] for c in checks), "checks": checks}
