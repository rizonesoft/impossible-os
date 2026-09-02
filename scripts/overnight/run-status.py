#!/usr/bin/env python3
"""Write a glance-able overnight run status (observability).

Renders .claude/overnight/run-status.md from two sources: the repo's own
runner_status.full_brief() (cursor/phase, git state, obligations, live
gotchas, recent Run Log decisions -- .claude/hooks/runner_status.py) plus the
triage oracle's per-file classification (.claude/hooks/sequencer_triage.py
--next/--summary/--blockers), so the operator can see where a long run stands
without grepping a huge report or re-deriving state that already exists.

Usage: run-status.py [PROJECT_DIR] [--stamp ISO8601]
Date is injected (--stamp) because the runner forbids Date.now() in some
contexts; falls back to "unknown" when absent. Stdlib only.
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


def _load_runner_status(project_dir: Path):
    """Import the repo's own .claude/hooks/runner_status.py module."""
    hooks_dir = str(project_dir / ".claude" / "hooks")
    if hooks_dir not in sys.path:
        sys.path.insert(0, hooks_dir)
    import runner_status  # type: ignore
    return runner_status


def _run_oracle(project_dir: Path, *args: str) -> str:
    oracle = project_dir / ".claude" / "hooks" / "sequencer_triage.py"
    if not oracle.exists():
        return ""
    try:
        return subprocess.run(
            [sys.executable, str(oracle), *args],
            cwd=str(project_dir), capture_output=True, text=True, timeout=30,
        ).stdout
    except Exception:
        return ""


def _oracle_summary(project_dir: Path) -> list[tuple[str, str]]:
    """[(STATE, relpath), ...] from `sequencer_triage.py --summary`.

    Line shape is `f"{cls:14s} {file_path}"` (space-padded, not tab-separated).
    """
    rows = []
    valid = {"DONE", "NEEDS_WORK", "BLOCKED"}
    for line in _run_oracle(project_dir, "--summary").splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[0] in valid:
            rows.append((parts[0], parts[1]))
    return rows


def _oracle_next(project_dir: Path) -> tuple[str, str]:
    """(status, file_or_empty) from `sequencer_triage.py --next` JSON."""
    out = _run_oracle(project_dir, "--next").strip()
    if not out:
        return "unknown", ""
    try:
        data = json.loads(out)
    except Exception:
        return "unknown", ""
    return str(data.get("status", "unknown")), str(data.get("file") or "")


def _liveness(project_dir: Path) -> str:
    """LIVE / DEAD / UNKNOWN from `run-liveness.sh --quiet`, which reads the
    systemd units, the launcher's flock and report freshness -- evidence a dead
    run cannot leave behind. The `Armed:` marker is a file the run REMOVES on a
    clean stop; a host restart mid-gate (observed 2026-08-28, v18 capture) left
    it in place with `active: true` beside it, so a status page reading only
    the marker said Armed about a run that no longer existed. Liveness is the
    authority; the marker is reported beside it so a stale one is visible."""
    script = project_dir / "scripts" / "overnight" / "run-liveness.sh"
    if not script.is_file():
        return "UNKNOWN"
    try:
        rc = subprocess.run(["bash", str(script), "--quiet"], cwd=str(project_dir),
                            capture_output=True, text=True, timeout=30).returncode
    except Exception:
        return "UNKNOWN"
    return {0: "LIVE", 1: "DEAD"}.get(rc, "UNKNOWN")


def _blockers(project_dir: Path) -> str:
    """Compact one-line rendering of `sequencer_triage.py --blockers` JSON."""
    out = _run_oracle(project_dir, "--blockers").strip()
    if not out:
        return "(none)"
    try:
        data = json.loads(out)
    except Exception:
        return "(none)"
    items = data.get("blockers") or []
    if not items:
        return "(none)"
    return "; ".join(
        f"{b.get('file', '?')}#{b.get('section', '?')} ({b.get('awaiting', '?')})"
        for b in items
    )


def render(project_dir: Path, stamp: str) -> str:
    try:
        runner_status = _load_runner_status(project_dir)
        brief = runner_status.full_brief(project_dir)
    except Exception as exc:  # pragma: no cover -- defensive, stdlib-only fail-open
        brief = f"(runner_status unavailable: {exc})"

    rows = _oracle_summary(project_dir)
    next_status, next_file = _oracle_next(project_dir)
    blockers = _blockers(project_dir)
    counts: dict[str, int] = {}
    for st, _ in rows:
        counts[st] = counts.get(st, 0) + 1
    armed = (project_dir / ".claude" / "state" / "sequencer-armed").exists()
    fixpoint = (project_dir / ".claude" / "state" / "sequencer-fixpoint").exists()
    live = _liveness(project_dir)

    next_note = ""
    if next_status == "DONE":
        next_note = "  (true fixpoint -- safe to disarm)"
    elif next_status == "BLOCKED":
        next_note = "  (blocked on recoverable deferrals -- staying armed, will heal)"

    lines = [
        f"# Overnight Run Status -- {stamp}",
        "",
        brief,
        "",
        f"Oracle next: {next_status}" + (f" ({next_file})" if next_file else "") + next_note,
        f"Recoverable blockers: {blockers}",
        f"Armed: {armed}   Live: {live}   Fixpoint sentinel: {fixpoint}"
        + ("   (STALE MARKER -- the run died without clearing it; nothing is "
           "running. Re-arm, and finish or discard any half-shipped section "
           "first.)" if armed and live == "DEAD" else ""),
        "Counts: " + ("  ".join(f"{k}={v}" for k, v in sorted(counts.items())) or "(none)"),
        "",
        "## Queue",
    ]
    for st, path in rows:
        lines.append(f"- [{st}] {path}")
    lines.append("")
    return "\n".join(lines)


def main(argv: list) -> int:
    project_dir = Path.cwd()
    stamp = "unknown"
    i = 0
    while i < len(argv):
        if argv[i] == "--stamp" and i + 1 < len(argv):
            stamp = argv[i + 1]
            i += 2
        else:
            project_dir = Path(argv[i]).resolve()
            i += 1
    out = project_dir / ".claude" / "overnight" / "run-status.md"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(render(project_dir, stamp), encoding="utf-8")
    print(str(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
