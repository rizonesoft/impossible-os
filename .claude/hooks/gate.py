#!/usr/bin/env python3
"""Run a process hook, and decide from data whether its refusal counts.

`.claude/gate-modes.json` maps a hook to `enforce` or `log`. In `log` mode a refusal
is written to `.claude/overnight/gate-log.jsonl` and the call goes ahead, because a warning that
keeps firing is data and a refusal that wedges a run is not (2026-11-07,
docs/specs/2026-11-07-run-supervisor-design.md).

Only hooks named in LOGGABLE can ever be logged, so editing the data file cannot turn
off a product or safety check. An absent or unreadable data file means `enforce`.

Usage in settings.json: gate.py <path to hook> [hook args...]
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

# Process and cost hooks. A hook that protects the OS or prevents damage does not
# belong here, however often it fires.
LOGGABLE = frozenset({
    "agent_coverage_gate", "agent_dispatch_required", "broker_dispatch_required",
    "build_offload_reminder", "codex_wait_discipline", "design_review_required",
    "fork_context_guard", "phase1_evidence_gate", "read_cache_block",
    "receipt_surface_guard", "receiving_review_required", "review_dispatch_gate",
    "review_kind_literal_required", "section_review_required", "skill_step_block",
    "step5_quality_gate", "websearch_offload_gate",
    # run_phase_guard's own codes. TEARDOWN, WORKTREE, CODEX-WRITE and ASK stay out.
    "SEQ-PHASE", "SEQ-LIFECYCLE", "SEQ-R1", "SEQ-REDIRECT", "SEQ-CONTROL-PLANE",
})

# The part a run may never edit, because it is what recovers from every other mistake.
FROZEN = frozenset({
    ".claude/hooks/gate.py", ".claude/hooks/run_phase_guard.py", ".claude/settings.json",
    "scripts/run.sh", "scripts/overnight/run-common.sh", "scripts/overnight/heartbeat.sh",
    "scripts/overnight/supervisor.py", "scripts/overnight/ship_count.py",
})


def repo_root() -> Path:
    return Path(os.environ.get("CLAUDE_PROJECT_DIR") or Path(__file__).resolve().parents[2])


def mode_of(name: str, root: Path) -> str:
    if name not in LOGGABLE:
        return "enforce"
    try:
        modes = json.loads((root / ".claude/gate-modes.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return "enforce"  # fail-direction: unknown state keeps the gate on
    return "log" if isinstance(modes, dict) and modes.get(name) == "log" else "enforce"


def log_refusal(root: Path, name: str, text: str) -> None:
    try:
        path = root / ".claude/overnight/gate-log.jsonl"
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps({"ts": int(time.time()), "gate": name,
                                 "text": text.strip()[:400]}) + "\n")
    except OSError:
        pass  # fail-direction: a lost log line must not stop the call it describes


def main(argv: list) -> int:
    if not argv:
        print("usage: gate.py <hook path> [args...]", file=sys.stderr)
        return 2
    hook = argv[0]
    name = Path(hook).stem
    root = repo_root()
    stdin = sys.stdin.buffer.read()
    # deadline: the harness bounds each hook call, and this runs inside that bound.
    done = subprocess.run([sys.executable, hook, *argv[1:]], input=stdin, capture_output=True)
    if done.returncode == 2 and mode_of(name, root) == "log":
        log_refusal(root, name, done.stderr.decode("utf-8", "replace"))
        return 0
    sys.stdout.buffer.write(done.stdout)
    sys.stderr.buffer.write(done.stderr)
    return done.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
