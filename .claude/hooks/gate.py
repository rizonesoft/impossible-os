#!/usr/bin/env python3
"""The frozen files: the part of the run's machinery the run may never edit.

They are what recovers from every other mistake, so `run_phase_guard.py` refuses an
edit to any of them from inside the unattended run, and `scripts/overnight/supervisor.py`
never counts them as machinery it may undo. Everything else under .claude/hooks/,
.claude/skills/ and scripts/overnight/ the run may repair, with a test.

Until 2026-11-09 this file also ran process hooks in `log` or `enforce` mode from
`.claude/gate-modes.json`. A watched run showed none of those 22 gates was needed, so
they were deleted (docs/plans/2026-11-07-run-supervisor.md, Stage 7) and the wrapper
with them.
"""
from __future__ import annotations

FROZEN = frozenset({
    ".claude/hooks/gate.py", ".claude/hooks/run_phase_guard.py", ".claude/settings.json",
    "scripts/run.sh", "scripts/overnight/run-common.sh", "scripts/overnight/heartbeat.sh",
    "scripts/overnight/supervisor.py", "scripts/overnight/ship_count.py",
})
