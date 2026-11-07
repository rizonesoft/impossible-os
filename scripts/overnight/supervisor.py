#!/usr/bin/env python3
"""The frozen part of the run: undo a bad machinery commit, keep health, raise alerts.

A run may edit its own machinery (docs/specs/2026-11-07-run-supervisor-design.md).
This file may not be edited by a run, because it is what recovers from that going
wrong. The heartbeat calls it every few minutes, and it must never stop the run: every
failure here is reported and the heartbeat carries on.

Each check records:
  machinery commits. When UNDO_AFTER_HOURS pass with no commit outside the machinery
    AND the session is idle, they are reverted and an alert sent. Only when idle,
    because the session shares this working tree and must not have it change underneath.
  run-health.json: sections shipped today, hours since the last ship, gate warnings
    fired, machinery commits and reverts. No dollars: the run is on a subscription,
    and the rates in cost-summary.py are relative sizing, not a bill.
  repair-needed: nothing has shipped for REPAIR_AFTER_HOURS.

Usage: supervisor.py PROJECT_DIR [--idle] [--now EPOCH]
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import time
from pathlib import Path

UNDO_AFTER_HOURS = 3          # no commit outside the machinery, session idle
REPAIR_AFTER_HOURS = 6
MACHINERY_PREFIXES = (".claude/hooks/", ".claude/skills/", ".claude/gate-modes.json",
                      "scripts/overnight/", ".githooks/")
SHIP_RE = re.compile(r"^\+> \*\*(?:Verified|Quality reviewed):\*\*", re.M)
# deadline: each git call is bounded here, and a push by its own gate-heavy limit.
GIT_TIMEOUT = 60
PUSH_TIMEOUT = 1800


def git(root: Path, *args, timeout=GIT_TIMEOUT):
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True,
                          text=True, timeout=timeout)


def commits_since(root: Path, start: str) -> list:
    if not start:
        return []
    r = git(root, "log", "--format=%H", f"{start}..HEAD")
    return r.stdout.split() if r.returncode == 0 else []


def files_of(root: Path, sha: str) -> list:
    r = git(root, "show", "--name-only", "--format=", sha)
    return [f for f in r.stdout.splitlines() if f]


def is_machinery(path: str) -> bool:
    return path.startswith(MACHINERY_PREFIXES)


def ships_in(root: Path, sha: str) -> int:
    """Section-ship stamps a commit adds under todo/."""
    r = git(root, "show", "--format=", "--unified=0", sha, "--", "todo")
    return len(SHIP_RE.findall(r.stdout)) if r.returncode == 0 else 0


def alert(root: Path, message: str) -> None:
    script = root / "scripts/overnight/notify.sh"
    try:
        subprocess.run(["bash", str(script), str(root), "critical", message],
                       capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        pass  # fail-direction: a lost alert must not stop the run; health still shows it


def read_json(path: Path, default):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return default  # fail-direction: a missing record starts fresh


def write_json(path: Path, data) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=1), encoding="utf-8")
    tmp.replace(path)


def undo(root: Path, state: dict, pending: list, now: int, idle: bool) -> tuple:
    """Revert the whole pending batch, newest first, once its oldest commit is due.

    As one batch, because a later commit may fix an earlier one: undoing only the older
    conflicts, and undoing only the newer restores the bug it fixed. It stops at the
    first failure and keeps that commit and everything older, so nothing is left half
    undone and forgotten. Returns (reverted, still pending)."""
    limit = UNDO_AFTER_HOURS * 3600
    if not idle or not pending or now - min(p["seen"] for p in pending) < limit:
        return [], pending
    ordered = sorted(pending, key=lambda p: p["order"], reverse=True)

    def throttled(message):
        if now - int(state.get("last_undo_alert", 0)) >= 3600:
            alert(root, message)
            state["last_undo_alert"] = now

    reverted = []
    keep = ordered
    if git(root, "status", "--porcelain").stdout.strip():
        throttled(f"{len(ordered)} machinery commit(s) are due for undo, but the tree is dirty.")
        return [], pending
    for i, entry in enumerate(ordered):
        r = git(root, "revert", "--no-edit", entry["sha"], timeout=PUSH_TIMEOUT)
        if r.returncode != 0:
            git(root, "revert", "--abort")
            throttled(f"Could not undo machinery commit {entry['sha'][:12]}: {r.stderr.strip()[:200]}")
            keep = ordered[i:]
            break
        reverted.append(entry["sha"])
        keep = ordered[i + 1:]
    if reverted:
        # The reverts stand whether or not this works, and the run pushes them with its
        # next push. A timeout here must not lose the record of what was undone.
        try:
            pushed_ok = git(root, "push", "origin", "HEAD", timeout=PUSH_TIMEOUT).returncode == 0
        except subprocess.SubprocessError:
            pushed_ok = False
        pushed = "and pushed" if pushed_ok else "but the push failed"
        alert(root, f"Nothing was produced after machinery commits "
                    f"{', '.join(s[:12] for s in reverted)}: reverted {pushed}.")
    return reverted, keep


def check(root: Path, idle: bool, now: int) -> dict:
    state_dir = root / ".claude/overnight"
    state_path = state_dir / "supervisor-state.json"
    state = read_json(state_path, {})
    pending = state.get("pending", [])
    order = int(state.get("order", 0))
    state.setdefault("first_seen", now)

    head = git(root, "rev-parse", "HEAD").stdout.strip()
    new = commits_since(root, state.get("head") or head)
    shipped = 0
    produced = False
    for sha in reversed(new):
        files = files_of(root, sha)
        shipped += ships_in(root, sha)
        if any(not is_machinery(f) for f in files):
            produced = True
        if any(is_machinery(f) for f in files):
            order += 1
            pending.append({"sha": sha, "seen": now, "order": order})
    if produced:
        pending = []  # the run worked with these changes in place

    reverted, pending = undo(root, state, pending, now, idle)
    state["head"] = git(root, "rev-parse", "HEAD").stdout.strip()

    day = time.strftime("%Y-%m-%d", time.gmtime(now))
    if state.get("day") != day:
        state.update(day=day, shipped_today=0)
    state["shipped_today"] = int(state.get("shipped_today", 0)) + shipped
    state["total_shipped"] = int(state.get("total_shipped", 0)) + shipped
    if shipped:
        state["last_ship"] = now
    last_ship = int(state.get("last_ship") or state["first_seen"])
    hours = (now - last_ship) / 3600

    repair = state_dir / "repair-needed"
    if hours >= REPAIR_AFTER_HOURS:
        if now - int(state.get("last_stall_alert", 0)) >= REPAIR_AFTER_HOURS * 3600:
            alert(root, f"Nothing has shipped for {hours:.1f} hours.")
            state["last_stall_alert"] = now
        repair.parent.mkdir(parents=True, exist_ok=True)
        repair.write_text(f"{hours:.1f}\n", encoding="utf-8")
    elif repair.exists():
        repair.unlink()

    gates: dict = {}
    try:
        for line in (state_dir / "gate-log.jsonl").read_text(encoding="utf-8").splitlines():
            name = json.loads(line).get("gate", "?")
            gates[name] = gates.get(name, 0) + 1
    except (OSError, ValueError):
        pass  # fail-direction: no log yet reads as no warnings

    state.update(pending=pending, order=order)
    state["reverted"] = state.get("reverted", []) + reverted
    write_json(state_path, state)
    health = {
        "updated": now, "sections_shipped_today": state["shipped_today"],
        "sections_shipped_total": state["total_shipped"],
        "hours_since_ship": round(hours, 1), "gate_warnings": gates,
        "machinery_commits_pending": len(pending),
        "machinery_reverts": state["reverted"], "repair_needed": repair.exists(),
    }
    write_json(state_dir / "run-health.json", health)
    return health


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("project_dir")
    ap.add_argument("--idle", action="store_true")
    ap.add_argument("--now", type=int, default=None)
    args = ap.parse_args()
    now = args.now if args.now is not None else int(time.time())
    print(json.dumps(check(Path(args.project_dir).resolve(), args.idle, now)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
