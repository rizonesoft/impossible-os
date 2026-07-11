#!/usr/bin/env python3
"""Deterministic PREFLIGHT for the overnight sequencer -- no model turns.

Runs the whole preflight mechanically and emits ONE JSON verdict on stdout.
The success path (the overwhelmingly common case) costs zero model tokens:
the sequencer session makes a single Bash call and reads `ok`. Models enter
only on failure -- the JSON names the artifact to digest (Sonnet
diagnostic-digester) so Opus only diagnoses and decides, never shepherds
mechanics.

Steps (mirrors the former SKILL.md step-1 prose):
  1. todo-graph rebuild+validate (oracle freshness)
  2. preflight-stamp check: green baseline for this EXACT tree -> skip 3-4
  3. bash scripts/build.sh          (=== BUILD OK === sentinel)
  4. bash scripts/test.sh QUIET=1   (0 FAIL)
  5. record the stamp (unchanged tree never pays the baseline twice)
  6. CI status via `gh run list` DIRECTLY (a JSON-producing query needs no
     subagent): failure on an ancestor of local HEAD = ours to fix.

Exit 0 with {"ok": true} when preflight is green; exit 1 with {"ok": false,
"failures": [...]} naming each failing step + its evidence artifact.
CI unavailability (no gh auth / network) is a NOTE, never a failure.
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

_ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def run(cmd, cwd, timeout, **kw):
    return subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True,
                          timeout=timeout, **kw)


def tail(path: Path, n=5) -> str:
    try:
        return "\n".join(path.read_text(errors="replace").splitlines()[-n:])
    except OSError:
        return "(unreadable)"


def main(argv) -> int:
    project = Path(argv[0] if argv else ".").resolve()
    out = {"ok": True, "baseline": None, "steps": {}, "failures": [],
           "notes": [], "ci": None}

    def fail(step, why, artifact=None):
        out["ok"] = False
        out["failures"].append({"step": step, "why": why,
                                "artifact": artifact})

    # 1. Oracle freshness.
    try:
        r = run(["bash", "scripts/todo-graph/build-and-validate.sh",
                 "--keep-cache"], project, 300)
        out["steps"]["todo_graph"] = "ok" if r.returncode == 0 else "fail"
        if r.returncode != 0:
            fail("todo_graph", "build-and-validate failed",
                 "run `bash scripts/todo-graph/build-and-validate.sh` for output")
    except Exception as exc:  # noqa: BLE001
        fail("todo_graph", f"unavailable ({exc})")

    # 2. Cached green baseline for this exact tree?
    baseline_cached = False
    try:
        r = run([sys.executable, "scripts/overnight/preflight-stamp.py",
                 ".", "check"], project, 60)
        baseline_cached = r.returncode == 0
        out["baseline"] = ("cached: " + r.stdout.strip()) if baseline_cached \
            else "ran gates: " + r.stdout.strip()
    except Exception as exc:  # noqa: BLE001
        out["baseline"] = f"stamp check unavailable ({exc}); running gates"

    build_log = project / "build" / "build.log"
    if not baseline_cached:
        # 3. Build.
        try:
            r = run(["bash", "scripts/build.sh"], project, 1800)
            last = tail(build_log, 1)
            if "=== BUILD OK ===" in last:
                out["steps"]["build"] = "ok"
            else:
                out["steps"]["build"] = "fail"
                fail("build", f"build.log tail: {last!r}", str(build_log))
        except Exception as exc:  # noqa: BLE001
            fail("build", f"build.sh did not complete ({exc})", str(build_log))
        # 4. Tests (only meaningful on a green build).
        if out["steps"].get("build") == "ok":
            try:
                r = run(["bash", "scripts/test.sh", "QUIET=1"], project, 1800)
                summary = _ANSI_RE.sub("", "\n".join(r.stdout.splitlines()[-12:]))
                out["steps"]["tests"] = "ok" if r.returncode == 0 else "fail"
                out["steps"]["tests_summary"] = summary
                if r.returncode != 0:
                    fail("tests", "test.sh non-zero", summary)
            except Exception as exc:  # noqa: BLE001
                fail("tests", f"test.sh did not complete ({exc})")
        # 5. Record the stamp only on a fully green baseline.
        if out["steps"].get("build") == "ok" and out["steps"].get("tests") == "ok":
            try:
                run([sys.executable, "scripts/overnight/preflight-stamp.py",
                     ".", "record", "--summary",
                     "BUILD OK; " + out["steps"].get("tests_summary", "")
                     .splitlines()[-1][:120]], project, 60)
            except Exception:
                out["notes"].append("stamp record failed (non-fatal)")

    # 6. CI status -- a deterministic gh query, not a subagent dispatch.
    ci = {"available": False, "ours_red": False, "runs": []}
    try:
        head = run(["git", "rev-parse", "HEAD"], project, 10).stdout.strip()
        for wf in ("build.yml", "todo-graph.yml"):
            r = run(["gh", "run", "list", "--workflow", wf, "--limit", "3",
                     "--json", "headSha,conclusion,status,workflowName"],
                    project, 60)
            if r.returncode != 0:
                raise RuntimeError(r.stderr.strip()[:200])
            ci["available"] = True
            for rec in json.loads(r.stdout or "[]"):
                entry = {"workflow": wf, "sha": rec.get("headSha", "")[:12],
                         "status": rec.get("status"),
                         "conclusion": rec.get("conclusion")}
                if rec.get("conclusion") == "failure":
                    anc = run(["git", "merge-base", "--is-ancestor",
                               rec.get("headSha", ""), head], project, 10)
                    entry["ours"] = anc.returncode == 0
                    if entry["ours"]:
                        ci["ours_red"] = True
                ci["runs"].append(entry)
    except Exception as exc:  # noqa: BLE001
        out["notes"].append(f"CI check unavailable ({exc}) -- continuing "
                            "(never blocks on tooling absence)")
    out["ci"] = ci
    if ci.get("ours_red"):
        fail("ci", "a failed CI run's head SHA is an ancestor of local HEAD "
                   "-- our pushed work broke CI; diagnose before section work",
             "gh run view --log-failed (dispatch gh-query-runner for slices)")

    print(json.dumps(out, indent=1))
    return 0 if out["ok"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
