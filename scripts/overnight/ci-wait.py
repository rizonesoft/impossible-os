#!/usr/bin/env python3
"""Wait for CI's verdict on the commit just pushed.

Operator decision 2026-11-11: a section is pushed once, complete and stamped,
and the run does not move on until that push is green in CI. `ci-check.py`
reads whatever verdict exists and never waits; this is its waiting half.

    python3 scripts/overnight/ci-wait.py [project] [--max 540] [--grace 180]

It watches the workflows `ci_status.py` names, for the commit at HEAD, which
must be the commit on the remote (push first). One call waits at most `--max`
seconds, below the 10-minute tool wall; exit 124 means "still running, issue
the same call again". No model is spent while it waits.

Exit codes
----------
  0    every watched workflow that ran on this commit succeeded
  1    a watched workflow failed: the JSON names the run and its failed jobs
  3    no verdict can be had (gh unreachable, HEAD not pushed, a run
       cancelled, no run started within 45 minutes of the push, or none
       finished within 3 hours): a note, never a reason to stop the run
  124  still queued or running after --max seconds: issue the call again
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ci_status import WORKFLOWS  # noqa: E402

POLL_S = 20
# How long a commit's runs may sit unstarted, and how long the whole wait may last, before the answer is
# "no verdict". On 2026-11-11 GitHub Actions had an outage and every run stayed queued: a wait with no end
# would have held the run for as long as the outage lasted, with nothing of ours at fault.
QUEUED_LIMIT_S = 45 * 60
TOTAL_LIMIT_S = 3 * 60 * 60
GH_TIMEOUT_S = 60
_FIELDS = "databaseId,status,conclusion,createdAt"


def verdict(runs_by_workflow: dict, elapsed: float, grace: float) -> tuple:
    """(state, detail) from each workflow's runs on the commit.

    state: 'green', 'red', 'running' or 'unknown'. A workflow with no run on
    this commit is waited for until `grace` seconds have passed (GitHub takes a
    moment to create it), then counts as not triggered by this push: several
    workflows only fire for the paths they watch. Only the newest run of a
    workflow speaks for it, so a re-run after a failure replaces the failure.
    """
    newest = {}
    for wf, runs in runs_by_workflow.items():
        if runs:
            newest[wf] = max(runs, key=lambda r: (r.get("createdAt") or "", r.get("databaseId") or 0))
    missing = [wf for wf in runs_by_workflow if wf not in newest]
    open_runs = {wf: r for wf, r in newest.items() if r.get("status") != "completed"}
    never_started = open_runs and all(r.get("status") != "in_progress" for r in open_runs.values())
    if (never_started and elapsed > QUEUED_LIMIT_S) or (open_runs and elapsed > TOTAL_LIMIT_S):
        return "unknown", {"reason": f"CI has not answered {int(elapsed // 60)} minutes after the push "
                                     f"({'no run has started' if never_started else 'runs are still going'}): "
                                     "GitHub's side, not a verdict on the commit",
                           "waiting_on": sorted(open_runs)}
    if open_runs or (missing and elapsed < grace):
        return "running", {"waiting_on": sorted(wf for wf, r in newest.items() if r.get("status") != "completed")
                           + sorted(missing)}
    failed = {wf: r for wf, r in newest.items() if r.get("conclusion") not in ("success", "skipped", "cancelled")}
    if failed:
        return "red", {"failed": {wf: r.get("databaseId") for wf, r in sorted(failed.items())}}
    cancelled = sorted(wf for wf, r in newest.items() if r.get("conclusion") == "cancelled")
    if cancelled or not newest:
        # A cancelled run never finished: the others passing says nothing about it.
        return "unknown", {"reason": "no verdict on this commit", "cancelled": cancelled}
    return "green", {"passed": sorted(newest), "not_triggered": sorted(missing)}


def _run(cmd, cwd, timeout=GH_TIMEOUT_S):
    return subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True, timeout=timeout, check=False)


def _first_seen(project, sha):
    """When this commit was first waited for, kept across calls.

    The wait for a run to appear is measured from here, not from the commit's
    date (a commit can sit behind the pre-push gates for many minutes) and not
    from this call's start (the call is issued again after exit 124)."""
    try:
        path = Path(_run(["git", "rev-parse", "--git-path", "ci-wait-first-seen"], project, 10).stdout.strip())
        if not path.is_absolute():
            path = project / path
        if path.exists():
            seen_sha, _, when = path.read_text(encoding="utf-8").strip().partition(" ")
            if seen_sha == sha:
                return float(when)
        now = time.time()
        path.write_text(f"{sha} {now}\n", encoding="utf-8")
        return now
    except (subprocess.SubprocessError, OSError, ValueError):
        return time.time()


def _runs(project, sha, budget):
    out = {}
    for wf in WORKFLOWS:
        r = _run(["gh", "run", "list", "--workflow", wf, "--commit", sha, "--limit", "20", "--json", _FIELDS], project,
                 max(5, min(GH_TIMEOUT_S, budget())))
        if r.returncode != 0:
            raise RuntimeError(f"gh run list failed for {wf}: {(r.stderr or '').strip()[:200]}")
        out[wf] = json.loads(r.stdout or "[]")
    return out


def _failed_jobs(project, run_id):
    r = _run(["gh", "run", "view", str(run_id), "--json", "jobs"], project)
    if r.returncode != 0:
        return []
    return [{"job": j.get("name"), "steps": [s.get("name") for s in j.get("steps") or []
                                             if s.get("conclusion") == "failure"]}
            for j in json.loads(r.stdout or "{}").get("jobs") or [] if j.get("conclusion") == "failure"]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("project", nargs="?", default=".")
    ap.add_argument("--max", type=int, default=540, help="seconds this call may wait (default 540)")
    ap.add_argument("--grace", type=int, default=180, help="seconds to wait for a run to appear (default 180)")
    args = ap.parse_args(argv)
    project = Path(args.project).resolve()
    out = {"schema": "ci-wait-v1", "state": "unknown"}

    def done(code):
        print(json.dumps(out, indent=1))
        return code

    try:
        head = _run(["git", "rev-parse", "HEAD"], project, 10).stdout.strip()
        remote = _run(["git", "ls-remote", "origin", "refs/heads/main"], project).stdout.split()
    except (subprocess.SubprocessError, OSError) as exc:
        out["reason"] = f"git unavailable ({str(exc)[:120]})"
        return done(3)
    out["sha"] = head
    if not remote or remote[0] != head:
        out["reason"] = "HEAD is not the commit on origin/main: push first (or the remote moved: rebase and push)"
        return done(3)

    first_seen = _first_seen(project, head)
    deadline = time.monotonic() + args.max

    def budget():
        return deadline - time.monotonic()

    while True:
        try:
            state, detail = verdict(_runs(project, head, budget), time.time() - first_seen, args.grace)
        except (subprocess.SubprocessError, OSError, RuntimeError, ValueError) as exc:
            out["reason"] = f"CI unavailable ({str(exc)[:200]})"
            return done(3)
        for key in ("waiting_on", "passed", "not_triggered", "failed", "cancelled", "reason"):
            out.pop(key, None)  # what an earlier poll of this call reported
        out.update(state=state, **detail)
        if state == "green":
            return done(0)
        if state == "red":
            out["failed_jobs"] = {wf: _failed_jobs(project, rid) for wf, rid in detail["failed"].items()}
            return done(1)
        if state == "unknown":
            return done(3)
        # Leave room for one more round of queries before the call's own limit.
        if budget() < POLL_S + len(WORKFLOWS) * 5:
            return done(124)
        time.sleep(POLL_S)


if __name__ == "__main__":
    sys.exit(main())
