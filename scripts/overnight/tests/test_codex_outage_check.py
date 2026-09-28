#!/usr/bin/env python3
"""codex-outage-check.py and the broker's outage gate.

Refusal directions: two recent backend failures refuse (including the REAL
2026-09-03 404 artifacts), and the broker exits 75 before dispatching.
Allow directions: a success breaks the streak, the window expires, a running leg
is skipped, and non-backend failures (app-server crash, local ENOENT, review-side
errors) never count.

Run: python3 scripts/overnight/tests/test_codex_outage_check.py
"""
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
CHECK = REPO / "scripts/overnight/codex-outage-check.py"
BROKER = REPO / "scripts/overnight/review-broker-codex-dispatch.sh"
REAL = [REPO / ".claude/overnight/reviews/20260903-165335-design.out",
        REPO / ".claude/overnight/reviews/20260903-165420-design.out"]

OUTAGE = ("[codex] Codex error: Reconnecting... 5/5\n[codex] Codex error: unexpected status 404 Not Found: "
          "Unknown error\n[codex] Turn failed.\nTurn completed (rc=1)\n")
STREAM = "[codex] Codex error: stream disconnected before completion\nTurn completed (rc=1)\n"
CAPACITY = "[codex] Codex error: Selected model is at capacity\nTurn completed (rc=1)\n"
OK = "# Codex Adversarial Review\nverdict: approve\nTurn completed (rc=0)\n"
CRASH = "Error: app-server exited unexpectedly (code 1)\nTurn completed (rc=1)\n"
ENOENT = "ENOENT: no such file or directory, unlink '/tmp/codex-companion/x.json'\nTurn completed (rc=1)\n"
BADREF = "fatal: Not a valid object name /\nTurn completed (rc=1)\n"
RUNNING = "[codex] Thinking...\n"


def manifest(d, legs, now):
    """legs: oldest-first list of (content_or_path, age_seconds)."""
    lines = []
    for i, (content, age) in enumerate(legs):
        if isinstance(content, Path):
            log = content
        else:
            log = Path(d) / f"leg{i}.out"
            log.write_text(content)
        lines.append(json.dumps({"ts": int(now - age), "kind": "adversarial", "logFile": str(log)}))
    m = Path(d) / "manifest.jsonl"
    m.write_text("\n".join(lines) + "\n")
    return m


def rc(legs):
    now = time.time()
    with tempfile.TemporaryDirectory() as d:
        m = manifest(d, legs, now)
        return subprocess.run([sys.executable, str(CHECK), "--manifest", str(m), "--now", str(now)],
                              capture_output=True, text=True).returncode


CASES = [
    ("two recent 404s refuse", [(OUTAGE, 120), (OUTAGE, 60)], 75),
    ("stream + capacity refuse", [(STREAM, 120), (CAPACITY, 60)], 75),
    ("a running newest leg is skipped", [(OUTAGE, 120), (OUTAGE, 60), (RUNNING, 5)], 75),
    ("one outage is not an outage", [(OK, 120), (OUTAGE, 60)], 0),
    ("a success after failures clears it", [(OUTAGE, 180), (OUTAGE, 120), (OK, 60)], 0),
    ("failures older than the window clear", [(OUTAGE, 2500), (OUTAGE, 2400)], 0),
    ("app-server crashes are not the backend", [(CRASH, 120), (CRASH, 60)], 0),
    ("local ENOENT is not the backend", [(ENOENT, 120), (ENOENT, 60)], 0),
    ("review-side errors are not the backend", [(BADREF, 120), (BADREF, 60)], 0),
    ("empty manifest allows", [], 0),
]


def broker(man, env_extra):
    env = {k: v for k, v in os.environ.items() if k != "CODEX_OUTAGE_OVERRIDE"}
    env.update({"BROKER_MANIFEST": str(man), "CODEX_COMPANION_PATH": "/nonexistent/codex-companion.mjs"})
    env.update(env_extra)
    return subprocess.run(["bash", str(BROKER), "[review-kind: perf] todo/x.md section 1 body"],
                          capture_output=True, text=True, env=env, cwd=REPO)


def main():
    fails = []
    for name, legs, want in CASES:
        got = rc(legs)
        if got != want:
            fails.append(f"{name}: rc {got}, want {want}")
    if all(p.exists() for p in REAL):
        if rc([(REAL[0], 120), (REAL[1], 60)]) != 75:
            fails.append("the real 2026-09-03 404 artifacts were not classified as an outage")
    with tempfile.TemporaryDirectory() as d:
        man = manifest(d, [(OUTAGE, 120), (OUTAGE, 60)], time.time())
        p = broker(man, {})
        if p.returncode != 75 or "backend looks DOWN" not in p.stderr:
            fails.append(f"broker did not refuse: rc {p.returncode} {p.stderr[:120]!r}")
        # Override passes the gate; it then stops at the (deliberately missing)
        # companion, which proves the gate, not the dispatch, was what refused.
        p = broker(man, {"CODEX_OUTAGE_OVERRIDE": "1"})
        if p.returncode == 75 or "codex-companion.mjs not found" not in p.stderr:
            fails.append(f"override did not pass the gate: rc {p.returncode} {p.stderr[:120]!r}")
    if fails:
        print("test_codex_outage_check FAIL:\n  " + "\n  ".join(fails))
        return 1
    print(f"test_codex_outage_check OK ({len(CASES) + 3} cases)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
