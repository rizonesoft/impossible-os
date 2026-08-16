#!/usr/bin/env python3
"""Non-blocking CI verdict for the PREVIOUS section ship.

Why this exists
---------------
CI was checked at exactly two points: once at PREFLIGHT (run start) and once per
TODO FILE at close-out. Sections are pushed individually, so a section that
broke CI stayed undetected until its file closed or the next night's preflight.
That is how a fork+exec panic sat in a red `Build Impossible OS` job.

Why it does not wait
--------------------
The obvious fix -- check CI after each section -- costs a measured ~9 minutes of
`gh run watch` per section, serialized. A ten-section night would spend 90
minutes blocked on GitHub for a signal the local CI-parity gate already
approximates in 24 seconds.

So this NEVER waits. It reads whatever verdict exists right now and returns. Run
at the START of a section, it reports on the PREVIOUS section's push, which by
then has had a whole section's wall-clock to finish. Detection moves from
per-file to per-section for the cost of one JSON query and zero model tokens.

A run still in progress is NOT a verdict and never fails anything -- the next
section boundary picks it up. Neither is a failure a LATER green run already
superseded; see `ci_status.py` for why that half matters.

Exit codes
----------
  0  no red run attributable to our history (green, fixed, pending, unavailable)
  1  a FAILED run on our history that no later green run supersedes

CI being unreachable is a NOTE, never a failure: an unattended run must not stop
because GitHub is down or `gh` is unauthenticated.
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ci_status import collect  # noqa: E402


def main() -> int:
    project = sys.argv[1] if len(sys.argv) > 1 else "."
    ci = collect(project)
    out = {"schema": "ci-check-v1", "ours_red": ci["ours_red"],
           "completed_drought": ci.get("completed_drought", False),
           "available": ci["available"], "runs": ci["runs"],
           "notes": ci["notes"]}
    print(json.dumps(out, indent=2))
    return 1 if out["ours_red"] else 0


if __name__ == "__main__":
    sys.exit(main())
