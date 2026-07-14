#!/usr/bin/env python3
"""section-checkpoint.py -- durable section state across rollover/resume.

A rolled-over or relaunched session used to REDISCOVER what the prior session
already knew: the same symbols, constants, receipts, and review status (measured
2026-07-11: two resumes re-derived the same facts). This persists that state at
rollover so the next session loads it instead of re-searching.

  write   aggregate current section state into
          .claude/state/section-checkpoint.json (called at rollover; atomic).
  show    print the checkpoint as bounded JSON (called on resume) -- or
          {"checkpoint": null} when none/stale.

The checkpoint is validity-bound, not clock-bound: `show` marks it stale when
HEAD moved or the working-tree digest no longer matches, so a resume never
trusts facts that the tree has since invalidated. Fail-open: any gather error
just omits that field. Stdlib only.

Usage: section-checkpoint.py write|show [--project DIR]
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from worktree_hash import changed_paths, worktree_key  # noqa: E402

CP_REL = ".claude/state/section-checkpoint.json"
# Volatile runner state + build artifacts change constantly (including this
# checkpoint file itself); they must NOT count toward "did the work tree move",
# or a checkpoint would read stale the instant it is written.
_VOLATILE = (".claude/state/", ".claude/overnight/", "build/")


def _work_changed(root) -> list:
    return [f for f in changed_paths(root) if not f.startswith(_VOLATILE)]


def _git(root, *args, timeout=15):
    try:
        r = subprocess.run(["git", "-C", str(root), *args],
                           capture_output=True, text=True, timeout=timeout)
        return r.stdout.strip() if r.returncode == 0 else ""
    except Exception:
        return ""


def _json(root, rel):
    try:
        return json.loads((root / rel).read_text())
    except Exception:
        return None


def _receipts(root):
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "receipts", str(Path(__file__).resolve().parent / "receipts.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        b_ok, _ = mod.check_build(root)
        t_ok, _ = mod.check_suite(root, "all")
        s_ok, _ = mod.check_smoke(root)
        return {"build": b_ok, "test_all": t_ok, "smoke": s_ok}
    except Exception:
        return {}


def _next_action(cp: dict, run: dict) -> str:
    """P4.2: derive the single next intended action from the checkpoint state so a
    resumed worker acts instead of re-deriving. Order matters: an unreceived
    review blocks everything else."""
    phase = run.get("phase") or "?"
    if cp.get("outstanding_review"):
        return ("receive the pending Codex review via "
                "Skill(superpowers:receiving-code-review), triage findings at "
                "file:line, then re-run the section-commit gate")
    if phase == "SECTIONS":
        sec = run.get("section_idx")
        return (f"continue section {sec}: finish implement/review, then build + "
                "commit + stamp (or advance if the section is shipped)")
    if phase == "FILE_CLOSE":
        return "finish the file-close audit (hygiene + loose ends), then ADVANCE"
    if phase in ("VALIDATE", "GAP_AUDIT", "TRIAGE", "PREFLIGHT"):
        return f"resume the {phase} stage for {run.get('file') or 'the cursor file'}"
    return f"resume phase {phase}"


def gather(root: Path) -> dict:
    changed = _work_changed(root)
    cp = {
        "ts_ns": time.time_ns(),
        "head": _git(root, "rev-parse", "HEAD"),
        "worktree_digest": worktree_key(root, changed) if changed else "clean",
        "changed_files": changed,
    }
    run = _json(root, ".claude/state/sequencer-run.json") or {}
    cp["cursor"] = {"file": run.get("file"), "phase": run.get("phase"),
                    "section_idx": run.get("section_idx"),
                    "pass_no": run.get("pass_no")}
    pack = _json(root, ".claude/state/last-section-pack.json")
    if isinstance(pack, dict):
        # only carry a pack that still matches the tree (content-bound)
        ki = pack.get("key_inputs")
        fresh = (isinstance(ki, list)
                 and worktree_key(root, ki) == pack.get("digest"))
        cp["section_pack"] = {"digest": pack.get("digest"),
                              "pack_path": pack.get("pack_path"),
                              "fresh": bool(fresh)}
    cp["receipts"] = _receipts(root)
    rev = _json(root, ".claude/state/last-codex-review.json")
    if isinstance(rev, dict):
        cp["outstanding_review"] = (rev.get("received") is not True)
    # P4.2: enriched re-orient fields (additive, fail-open PER field) so a resumed
    # worker re-orients from the checkpoint instead of re-deriving review/finding/
    # decision state -- gated by P4.4/P4.8 canaries before an unattended arm.
    cp["phase"] = run.get("phase")
    todo = run.get("file") or ""
    # Open Codex findings + verdicts for the CURRENT cursor TODO, from the ACTUAL
    # ledger file (finding-triage.jsonl -- the old code read a non-existent
    # finding-ledger.json, so findings_recorded was always absent). Bounded.
    try:
        finds = []
        for ln in (root / ".claude/state/finding-triage.jsonl").read_text(
                encoding="utf-8").splitlines():
            ln = ln.strip()
            if not ln:
                continue
            try:
                e = json.loads(ln)
            except ValueError:
                continue
            if todo and todo in (e.get("todo") or ""):
                finds.append({"loc": e.get("loc"),
                              "decision": e.get("decision"),
                              "title": (e.get("title") or "")[:100]})
        cp["findings_recorded"] = len(finds)
        cp["open_findings"] = finds[-10:]
    except Exception:
        pass
    # A count of settled decisions the resumed worker can consult (I4 registry).
    try:
        dr = (root / ".claude/state/decision-registry.jsonl").read_text(
            encoding="utf-8").splitlines()
        cp["decisions_indexed"] = sum(1 for x in dr if x.strip())
    except Exception:
        pass
    # Next intended action, DERIVED from phase + review/receipt state so the
    # resumed worker knows what to do next without re-deriving it.
    cp["next_action"] = _next_action(cp, run)
    return cp


def write(root: Path) -> int:
    cp = gather(root)
    p = root / CP_REL
    try:
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = p.with_suffix(f".{os.getpid()}.tmp")
        tmp.write_text(json.dumps(cp, indent=1))
        os.replace(str(tmp), str(p))
    except Exception:
        return 1
    print(f"section checkpoint written (head {cp.get('head','')[:12]}, "
          f"{len(cp.get('changed_files', []))} changed files)")
    return 0


def show(root: Path) -> int:
    cp = _json(root, CP_REL)
    if not isinstance(cp, dict):
        print(json.dumps({"checkpoint": None}))
        return 0
    # validity check: HEAD + worktree digest must still match
    head_now = _git(root, "rev-parse", "HEAD")
    changed = _work_changed(root)
    wt_now = worktree_key(root, changed) if changed else "clean"
    stale = (cp.get("head") != head_now or cp.get("worktree_digest") != wt_now)
    print(json.dumps({"checkpoint": cp, "stale": stale,
                      "reason": ("head/worktree moved since checkpoint"
                                 if stale else "current")}, indent=1))
    return 0


def main(argv) -> int:
    if not argv or argv[0] not in ("write", "show"):
        print("usage: section-checkpoint.py write|show [--project DIR]",
              file=sys.stderr)
        return 2
    root = Path(argv[argv.index("--project") + 1]).resolve() \
        if "--project" in argv else Path(".").resolve()
    return write(root) if argv[0] == "write" else show(root)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
