#!/usr/bin/env python3
"""Shared CI-verdict collection for the overnight sequencer.

Both `preflight.py` (run start) and `ci-check.py` (per-section boundary) need
the same question answered: is there a red CI run that belongs to OUR history
and has NOT been fixed yet? The two carried independent copies of that logic and
both copies shared the same bug, so the fix lives here once and they import it.

The bug (measured 2026-07-28): the check was "a failed run's head SHA is an
ancestor of local HEAD". That is true FOREVER once any pushed commit goes red --
the failing SHA never stops being an ancestor. Commit 051df5e0 broke the tooling
pack, eb2af727 fixed it three commits later, and CI went green at HEAD; both
gates still reported `ours_red` and demanded a diagnosis of an already-fixed
failure. A gate that cannot observe its own repair is a false-alarm machine: it
fires at every section boundary for the rest of the run, and the only way to
learn it is stale is to re-diagnose the failure by hand each time.

Supersession is the missing half. A failure F is superseded when a SUCCESSFUL
run S of the same workflow exists such that S is also on our history and either

  * F's SHA is a strict ancestor of S's SHA  (we committed a fix on top), or
  * S and F share a SHA and S ran later      (a re-run of the same commit).

Only an unsuperseded, ours failure sets `ours_red`.

`--limit 3` per workflow was too thin to see a fix land: three pushes after a
break and the successful run that proves the repair has already fallen off the
window, which would resurrect the false alarm. The window is 10 -- still one
`gh` query per workflow, still zero model tokens.
"""
from __future__ import annotations

import json
import subprocess

WORKFLOWS = ("build.yml", "todo-graph.yml")
LIMIT = 10
_FIELDS = "headSha,conclusion,status,workflowName,createdAt"


def _git(project, args, timeout=10):
    return subprocess.run(["git", "-C", str(project)] + args,
                          capture_output=True, text=True, timeout=timeout)


def collect(project, workflows=WORKFLOWS, limit=LIMIT):
    """Return {'available', 'ours_red', 'runs', 'notes'} for the CI state.

    Never raises for CI-side problems: an unreachable or unauthenticated `gh`
    lands in `notes` and leaves `ours_red` False. An unattended run must not
    stop because GitHub is down.
    """
    out = {"available": False, "ours_red": False, "runs": [], "notes": []}

    try:
        head = _git(project, ["rev-parse", "HEAD"]).stdout.strip()
    except (subprocess.SubprocessError, OSError) as exc:
        out["notes"].append(f"git unavailable ({str(exc)[:120]})")
        return out
    if not head:
        out["notes"].append("git unavailable (no HEAD)")
        return out

    anc_cache: dict[tuple[str, str], bool] = {}

    def is_ancestor(a, b):
        """True when commit `a` is an ancestor of (or equal to) commit `b`."""
        if not a or not b:
            return False
        key = (a, b)
        if key not in anc_cache:
            try:
                r = _git(project, ["merge-base", "--is-ancestor", a, b])
                anc_cache[key] = r.returncode == 0
            except (subprocess.SubprocessError, OSError):
                anc_cache[key] = False
        return anc_cache[key]

    for wf in workflows:
        try:
            r = subprocess.run(
                ["gh", "run", "list", "--workflow", wf, "--limit", str(limit),
                 "--json", _FIELDS],
                cwd=str(project), capture_output=True, text=True, timeout=60)
            if r.returncode != 0:
                raise RuntimeError((r.stderr or "").strip()[:200])
            recs = json.loads(r.stdout or "[]")
        except (subprocess.SubprocessError, OSError, RuntimeError,
                ValueError) as exc:
            out["notes"].append(
                f"CI unavailable for {wf} ({str(exc)[:160]}) -- not a failure")
            continue

        out["available"] = True

        # Successful runs on our history are the evidence that a later failure
        # entry is already repaired.
        #
        # A CANCELLED run on a DESCENDANT commit counts too, and refusing it was
        # a real trap. `cancel-in-progress` cancels a run the moment the next
        # push supersedes it, so on a branch that pushes often almost nothing
        # ever reaches `success`: measured 2026-08-06 across the last 100
        # build.yml runs -- 75 cancelled, 5 failure, 20 success, with the newest
        # success roughly two days stale. An old failure therefore stayed
        # `ours_red` forever, and the doctrine ("fix it before shipping the next
        # section") re-charged that instruction at EVERY section boundary
        # against a failure that had already been repaired.
        #
        # A cancellation is weak evidence -- it proves the workflow STARTED on
        # that commit, not that it passed -- so it is admitted only for a run
        # whose head is a strict DESCENDANT of the failing commit, i.e. work
        # that came after the fix. It cannot manufacture a green for the failing
        # commit itself, and a genuine still-red HEAD keeps failing on its own
        # newer run.
        greens = [rec for rec in recs
                  if rec.get("conclusion") == "success"
                  and is_ancestor(rec.get("headSha", ""), head)]
        cancelled = [rec for rec in recs
                     if rec.get("conclusion") == "cancelled"
                     and is_ancestor(rec.get("headSha", ""), head)]

        for rec in recs:
            sha = rec.get("headSha", "") or ""
            entry = {"workflow": wf, "sha": sha[:12],
                     "status": rec.get("status"),
                     "conclusion": rec.get("conclusion")}
            if rec.get("conclusion") == "failure" and sha:
                entry["ours"] = is_ancestor(sha, head)
                if entry["ours"]:
                    fixer = _superseded_by(rec, greens, is_ancestor)
                    if fixer:
                        entry["superseded_by"] = fixer[:12]
                    else:
                        weak = _superseded_by(rec, cancelled, is_ancestor)
                        if weak:
                            entry["superseded_by"] = weak[:12]
                            entry["evidence"] = "cancelled-descendant (weak)"
                            out["notes"].append(
                                f"{wf} {sha[:12]} failed, and the only later run "
                                f"on our history is CANCELLED ({weak[:12]}). "
                                f"Treated as superseded -- cancel-in-progress "
                                f"makes success rare on a fast-moving branch -- "
                                f"but this is weak evidence, not a green.")
                        else:
                            out["ours_red"] = True
            out["runs"].append(entry)

    if out["ours_red"]:
        out["notes"].append(
            "A failed run's head SHA is an ancestor of local HEAD and no later "
            "successful run of that workflow supersedes it. Fix BEFORE shipping "
            "the next section: pushing on top of a red CI compounds the bisect "
            "surface for whoever diagnoses it.")

    return out


def _superseded_by(failure, greens, is_ancestor):
    """Return the SHA of a green run that proves `failure` is already fixed."""
    fsha = failure.get("headSha", "") or ""
    fat = failure.get("createdAt") or ""
    for green in greens:
        gsha = green.get("headSha", "") or ""
        if not gsha:
            continue
        if gsha == fsha:
            # Same commit re-run: only a LATER green supersedes.
            if (green.get("createdAt") or "") > fat:
                return gsha
            continue
        if is_ancestor(fsha, gsha):
            return gsha
    return ""
