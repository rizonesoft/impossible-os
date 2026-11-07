#!/usr/bin/env python3
"""The frozen part of the run: undo a bad machinery commit, keep health, raise alerts.

A run may edit its own machinery (docs/specs/2026-11-07-run-supervisor-design.md).
This file may not be edited by a run, because it is what recovers from that going
wrong. It is called once at each segment end and must never stop the run: every
failure here is reported and the launcher carries on.

Per segment it records:
  machinery commits, each with a count of later segments that produced nothing.
    At UNDO_AFTER such segments the commit is reverted and an alert sent.
  run-health.json: sections shipped today, hours since the last ship, cost per
    section, gate warnings fired, machinery commits and reverts.
  repair-needed: present when nothing has shipped for REPAIR_AFTER_HOURS. The
    launcher reads it to start a repair segment.

Usage: supervisor.py PROJECT_DIR --start-head SHA [--report FILE] [--now EPOCH]
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import time
from pathlib import Path

UNDO_AFTER = 2                # segments producing nothing after a machinery commit
REPAIR_AFTER_HOURS = 6
COST_ALERT_FACTOR = 3         # times the trailing average cost per section
MACHINERY_PREFIXES = (".claude/hooks/", ".claude/skills/", ".claude/gate-modes.json",
                      "scripts/overnight/", ".githooks/")
SHIP_RE = re.compile(r"^\+> \*\*(?:Verified|Quality reviewed):\*\*", re.M)
TOTAL_RE = re.compile(r"TOTAL\s+\$([0-9.]+)")
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


def undo(root: Path, pending: list) -> tuple:
    """Revert due commits, newest first. Returns (reverted, still pending)."""
    due = [p for p in pending if p["empty_segments"] >= UNDO_AFTER]
    keep = [p for p in pending if p["empty_segments"] < UNDO_AFTER]
    reverted = []
    for entry in sorted(due, key=lambda p: p["order"], reverse=True):
        sha = entry["sha"]
        if git(root, "status", "--porcelain").stdout.strip():
            keep.append(entry)  # retry at the next clean segment end
            alert(root, f"Machinery commit {sha[:12]} is due for undo, but the tree is dirty.")
            continue
        r = git(root, "revert", "--no-edit", sha, timeout=PUSH_TIMEOUT)
        if r.returncode != 0:
            git(root, "revert", "--abort")
            alert(root, f"Could not undo machinery commit {sha[:12]}: {r.stderr.strip()[:200]}")
            continue
        reverted.append(sha)
    if reverted:
        p = git(root, "push", "origin", "HEAD", timeout=PUSH_TIMEOUT)
        pushed = "and pushed" if p.returncode == 0 else "but the push failed"
        alert(root, "Nothing was produced after machinery commits "
                    f"{', '.join(s[:12] for s in reverted)}: reverted {pushed}.")
    return reverted, keep


def segment_end(root: Path, start: str, report: str, now: int) -> dict:
    state_dir = root / ".claude/overnight"
    state_path = state_dir / "supervisor-state.json"
    state = read_json(state_path, {})
    pending = state.get("pending", [])
    order = int(state.get("order", 0))

    new = commits_since(root, start)
    shipped = 0
    produced = False
    for sha in reversed(new):
        files = files_of(root, sha)
        shipped += ships_in(root, sha)
        if any(not is_machinery(f) for f in files):
            produced = True
        if any(is_machinery(f) for f in files):
            order += 1
            pending.append({"sha": sha, "empty_segments": 0, "order": order})

    if produced:
        pending = []  # the run works with these changes in place
    else:
        for entry in pending:
            if entry["sha"] not in new:
                entry["empty_segments"] += 1

    reverted, pending = undo(root, pending)

    cost = 0.0
    try:
        hits = TOTAL_RE.findall(Path(report).read_text(encoding="utf-8", errors="replace"))
        cost = float(hits[-1]) if hits else 0.0
    except (OSError, ValueError):
        pass  # fail-direction: unknown cost counts as zero and raises no cost alert

    day = time.strftime("%Y-%m-%d", time.gmtime(now))
    if state.get("day") != day:
        state.update(day=day, shipped_today=0, cost_today=0.0)
    state["shipped_today"] = int(state.get("shipped_today", 0)) + shipped
    state["cost_today"] = round(float(state.get("cost_today", 0.0)) + cost, 2)
    state["total_shipped"] = int(state.get("total_shipped", 0)) + shipped
    state["total_cost"] = round(float(state.get("total_cost", 0.0)) + cost, 2)
    state["since_ship_cost"] = round(float(state.get("since_ship_cost", 0.0)) + cost, 2)
    state.setdefault("first_seen", now)
    if shipped:
        state["last_ship"] = now
    last_ship = int(state.get("last_ship") or state["first_seen"])
    hours = (now - last_ship) / 3600

    prior = state["total_shipped"] - shipped
    prior_cost = state["total_cost"] - state["since_ship_cost"]
    average = prior_cost / prior if prior > 0 else None
    if shipped:
        per_section = state["since_ship_cost"] / shipped
        if average and per_section > COST_ALERT_FACTOR * average:
            alert(root, f"Cost per section ${per_section:.2f} against an average of ${average:.2f}.")
        state["since_ship_cost"] = 0.0

    repair = state_dir / "repair-needed"
    if hours >= REPAIR_AFTER_HOURS:
        if not repair.exists() or now - int(state.get("last_stall_alert", 0)) >= REPAIR_AFTER_HOURS * 3600:
            alert(root, f"Nothing has shipped for {hours:.1f} hours.")
            state["last_stall_alert"] = now
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
        "hours_since_ship": round(hours, 1), "cost_today": state["cost_today"],
        "cost_total": state["total_cost"],
        "cost_per_section": (round(state["total_cost"] / state["total_shipped"], 2)
                             if state["total_shipped"] else None),
        "gate_warnings": gates, "machinery_commits_pending": len(pending),
        "machinery_reverts": state["reverted"], "repair_needed": repair.exists(),
    }
    write_json(state_dir / "run-health.json", health)
    return health


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("project_dir")
    ap.add_argument("--start-head", default="")
    ap.add_argument("--report", default="")
    ap.add_argument("--now", type=int, default=None)
    args = ap.parse_args()
    now = args.now if args.now is not None else int(time.time())
    print(json.dumps(segment_end(Path(args.project_dir).resolve(), args.start_head, args.report, now)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
