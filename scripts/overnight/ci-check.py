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
section boundary picks it up.

Exit codes
----------
  0  no red run attributable to our history (green, pending, or unavailable)
  1  a FAILED run whose head SHA is an ancestor of local HEAD -- ours, and red

CI being unreachable is a NOTE, never a failure: an unattended run must not stop
because GitHub is down or `gh` is unauthenticated.
"""
import json
import subprocess
import sys


def _run(cmd, timeout):
    try:
        return subprocess.run(cmd, capture_output=True, text=True,
                              timeout=timeout)
    except (subprocess.TimeoutExpired, FileNotFoundError) as exc:
        raise RuntimeError(str(exc)[:200]) from exc


def main() -> int:
    project = sys.argv[1] if len(sys.argv) > 1 else "."
    out = {"schema": "ci-check-v1", "ours_red": False, "available": False,
           "runs": [], "notes": []}

    try:
        head = _run(["git", "-C", project, "rev-parse", "HEAD"], 10).stdout.strip()
    except RuntimeError as exc:
        out["notes"].append(f"git unavailable ({exc})")
        print(json.dumps(out, indent=2))
        return 0

    for wf in ("build.yml", "todo-graph.yml"):
        try:
            r = _run(["gh", "run", "list", "--workflow", wf, "--limit", "3",
                      "--json", "headSha,conclusion,status,workflowName"], 60)
            if r.returncode != 0:
                raise RuntimeError((r.stderr or "").strip()[:200])
            out["available"] = True
            for rec in json.loads(r.stdout or "[]"):
                sha = rec.get("headSha", "") or ""
                entry = {"workflow": wf, "sha": sha[:12],
                         "status": rec.get("status"),
                         "conclusion": rec.get("conclusion")}
                # Only a FAILED run matters, and only if it is on OUR history.
                # A red run on a branch we never merged says nothing about this
                # tree, and treating it as ours would stop an unattended run on
                # somebody else's breakage.
                if rec.get("conclusion") == "failure" and sha:
                    anc = _run(["git", "-C", project, "merge-base",
                                "--is-ancestor", sha, head], 10)
                    entry["ours"] = anc.returncode == 0
                    if entry["ours"]:
                        out["ours_red"] = True
                out["runs"].append(entry)
        except (RuntimeError, ValueError) as exc:
            out["notes"].append(f"CI unavailable for {wf} ({exc}) -- not a failure")

    if out["ours_red"]:
        out["notes"].append(
            "A failed run's head SHA is an ancestor of local HEAD. Fix BEFORE "
            "shipping the next section: pushing on top of a red CI compounds "
            "the bisect surface for whoever diagnoses it.")

    print(json.dumps(out, indent=2))
    return 1 if out["ours_red"] else 0


if __name__ == "__main__":
    sys.exit(main())
