#!/usr/bin/env python3
# block-via: none (advisory decision CLI -- the review skills CONSULT it; it
# never exits 2 into a tool call, so a bug can slow the loop but never wedge it).
"""Convergence-based review redispatch decision (P2.1 + P2.2).

The primary churn mechanism: NEVER redispatch an UNCHANGED review kind on
UNCHANGED relevant inputs. A review kind's verdict is a function of the CONTENT
of the files that kind cares about; if none of them moved since that kind's last
verdict, a redispatch would only re-derive the same verdict (turn + Codex cost
with zero new signal). Continue a kind only while its relevant inputs actually
change (a fix that touches them may surface a NEW Critical/High); stop when they
are stable. There is NO round cap -- a review that keeps changing inputs keeps
running (stall detection lives in review_round_guard.py, a complementary signal).

P2.2 -- per-kind x per-file invalidation: one changed file must not re-trigger
ALL kinds. Each kind scopes to only the paths it reviews:

  adversarial / perf / re-adversarial : source only  (src/include/user/tools)
  consistency / design                : source + TODO tree

So a docs/TODO-only fix leaves the source-only kinds' fingerprints UNCHANGED
(they converge -> skip), and only the TODO-aware kinds re-run. A source edit
re-runs the source kinds. (perf stays scoped to all source rather than a
guessed "hot-path" subset: narrowing it further risks SKIPPING a real perf
review -- the unsafe direction for a review-quality gate -- so it is left
conservative. Hot-path narrowing is a separate, later optimization.)

Fail-open EVERYWHERE: an unknown kind, a git error, or any exception returns
REDISPATCH. The gate can only ever SKIP a redundant review, never suppress a
needed one.

CLI:
  review_convergence.py should-redispatch <slice> <kind>
        exit 0 + "REDISPATCH ..."  -> the kind's inputs changed (or no prior
              verdict / cannot tell): dispatch it.
        exit 1 + "CONVERGED ..."   -> inputs unchanged since this kind's last
              recorded verdict: SKIP the redispatch.
  review_convergence.py record <slice> <kind>
        Store this kind's current relevant-input fingerprint as its last
        verdict. Call AFTER a kind's review round resolves.
  review_convergence.py status <slice>
  review_convergence.py --selftest

SLICE id convention matches review_round_guard.py: "<todo-path>#<section>".
"""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

def _state_path(root: Path) -> Path:
    # State lives under the RESOLVED repo root (not the hook's own location), so
    # a hook copy operating on another tree -- or a test on a temp repo -- reads
    # and writes THAT tree's state, never the hook-repo's.
    return root / ".claude" / "state" / "review-convergence.json"


SRC_PATHS = ["src", "include", "user", "resources", "tools",
             "Makefile", "scripts/build.sh", "boot.conf"]
TODO_PATHS = ["todo"]

# Per-kind relevant path scopes (P2.2). Keys are normalized review kinds.
#
# COVERAGE IS LOAD-BEARING (token-saver v02, 2026-07-28). A kind missing from
# this table fails open forever: `should_redispatch` cannot fingerprint it, so
# it ALWAYS redispatches and the gate can never suppress that kind. The canary
# recorded "0 of 2 rounds suppressed" and one of the four all-time redispatch
# records reads "unknown kind 'test-coverage' -- fail-open to redispatch" --
# the gate was not mis-keyed, it simply had never been told the kind existed.
# The repo's live vocabulary (grep `review-kind: X` across skills/hooks/scripts)
# is adversarial, re-adversarial, perf, performance, consistency, design,
# gap-audit, test-coverage, adversarial-impl; all nine are covered below and
# test_review_convergence.py re-derives that list from the tree so the next kind
# someone adds fails the suite instead of silently failing open.
#
# Scope choice errs WIDE on purpose: a wider scope invalidates the stored
# verdict more often (more redispatch), while a narrow one suppresses more.
# Only the second direction can skip a review that was actually needed.
KIND_SCOPES = {
    "adversarial": ("src",),
    "re-adversarial": ("src",),
    "adversarial-impl": ("src",),
    "perf": ("src",),
    "performance": ("src",),
    "test-coverage": ("src",),
    "consistency": ("src", "todo"),
    "design": ("src", "todo"),
    "gap-audit": ("src", "todo"),
}


def _git(root: Path, *args: str) -> str | None:
    try:
        r = subprocess.run(["git", "-C", str(root), *args],
                           capture_output=True, timeout=60)
        return r.stdout.decode("utf-8", "replace") if r.returncode == 0 else None
    except Exception:
        return None


def _fingerprint(root: Path, scopes: tuple) -> str | None:
    """Content fingerprint of a kind's scope paths: HEAD tree + worktree diff +
    untracked file CONTENTS (name-only would serve a stale answer over an edited
    untracked file). None on any git failure -> caller fails open to REDISPATCH."""
    paths: list = []
    for s in scopes:
        paths += SRC_PATHS if s == "src" else TODO_PATHS
    tree = _git(root, "ls-tree", "-r", "HEAD", "--", *paths)
    diff = _git(root, "diff", "HEAD", "--", *paths)
    untracked = _git(root, "ls-files", "-o", "--exclude-standard", "--", *paths)
    if tree is None or diff is None or untracked is None:
        return None
    h = hashlib.sha256()
    h.update(tree.encode())
    h.update(diff.encode())
    names = [n for n in untracked.splitlines() if n.strip()]
    h.update("\n".join(names).encode())
    if names:
        try:
            r = subprocess.run(
                ["git", "-C", str(root), "hash-object", "--stdin-paths"],
                input="\n".join(names).encode(), capture_output=True, timeout=120)
            if r.returncode != 0:
                return None
            h.update(r.stdout)
        except Exception:
            return None
    return h.hexdigest()


def _load(root: Path) -> dict:
    try:
        d = json.loads(_state_path(root).read_text(encoding="utf-8"))
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def _save(root: Path, d: dict) -> None:
    try:
        sp = _state_path(root)
        sp.parent.mkdir(parents=True, exist_ok=True)
        tmp = sp.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
        tmp.replace(sp)
    except OSError:
        pass


def _norm_kind(kind: str) -> str:
    return (kind or "").strip().lower()


def record(root: Path, slice_id: str, kind: str) -> bool:
    """Store the kind's current relevant fingerprint as its last verdict."""
    k = _norm_kind(kind)
    scopes = KIND_SCOPES.get(k)
    if not scopes:
        return False
    fp = _fingerprint(root, scopes)
    if fp is None:
        return False
    d = _load(root)
    d.setdefault(slice_id, {})[k] = {"fp": fp, "ts": time.time_ns()}
    _save(root, d)
    return True


def should_redispatch(root: Path, slice_id: str, kind: str) -> tuple[bool, str]:
    """(redispatch?, reason). Fail-open to True (REDISPATCH) on anything unknown."""
    k = _norm_kind(kind)
    scopes = KIND_SCOPES.get(k)
    if not scopes:
        return (True, f"unknown kind {kind!r} -- fail-open to redispatch")
    prior = _load(root).get(slice_id, {}).get(k)
    if not prior or not isinstance(prior, dict) or not prior.get("fp"):
        return (True, f"no prior {k} verdict for {slice_id} -- first dispatch")
    cur = _fingerprint(root, scopes)
    if cur is None:
        return (True, "cannot fingerprint (git unavailable) -- fail-open")
    if cur == prior["fp"]:
        return (False, f"{k} inputs unchanged since last verdict "
                       f"(scope: {'+'.join(scopes)}) -- CONVERGED, skip redispatch")
    return (True, f"{k} inputs changed since last verdict "
                  f"(scope: {'+'.join(scopes)}) -- redispatch")


def _selftest() -> int:
    import tempfile
    fails = []
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        (root / "src/kernel").mkdir(parents=True)
        (root / "todo").mkdir()
        subprocess.run(["git", "init", "-q", str(root)], check=True)
        (root / "src/kernel/a.c").write_text("int a(void){return 1;}\n")
        (root / "todo/TODO-01.md").write_text("# t\n")
        subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
        subprocess.run(["git", "-C", str(root), "-c", "user.email=t@t",
                        "-c", "user.name=t", "commit", "-qm", "i"],
                       check=True, capture_output=True)
        sl = "todo/TODO-01.md#1"

        # First dispatch: no prior verdict -> REDISPATCH.
        if not should_redispatch(root, sl, "adversarial")[0]:
            fails.append("first dispatch should redispatch")
        record(root, sl, "adversarial")
        record(root, sl, "consistency")
        record(root, sl, "perf")
        # No change since verdict -> all CONVERGED (skip).
        for kd in ("adversarial", "consistency", "perf"):
            if should_redispatch(root, sl, kd)[0]:
                fails.append(f"{kd} should be CONVERGED on unchanged inputs")
        # A DOCS/TODO-only edit: source kinds stay converged, TODO kinds redispatch.
        (root / "todo/TODO-01.md").write_text("# t\nchanged\n")
        if should_redispatch(root, sl, "adversarial")[0]:
            fails.append("adversarial must stay CONVERGED after a TODO-only edit")
        if should_redispatch(root, sl, "perf")[0]:
            fails.append("perf must stay CONVERGED after a TODO-only edit")
        if not should_redispatch(root, sl, "consistency")[0]:
            fails.append("consistency must REDISPATCH after a TODO edit (scope incl. todo)")
        # A SOURCE edit: source kinds redispatch.
        (root / "src/kernel/a.c").write_text("int a(void){return 2;}\n")
        if not should_redispatch(root, sl, "adversarial")[0]:
            fails.append("adversarial must REDISPATCH after a source edit")
        if not should_redispatch(root, sl, "perf")[0]:
            fails.append("perf must REDISPATCH after a source edit")
        # Unknown kind -> fail-open redispatch.
        if not should_redispatch(root, sl, "bogus")[0]:
            fails.append("unknown kind must fail-open to redispatch")

    if fails:
        for f in fails:
            print("FAIL:", f, file=sys.stderr)
        print(f"review_convergence selftest: {len(fails)} failure(s)", file=sys.stderr)
        return 1
    print("review_convergence selftest OK (P2.1 convergence + P2.2 per-kind scope)")
    return 0


def _log_decision(root: Path, slice_id: str, kind: str, redo: bool,
                  why: str) -> None:
    """Append the convergence decision to the offload-events log (best-effort).

    Never raises and never changes the verdict: a logging failure must not turn
    a CONVERGED into a REDISPATCH or vice versa."""
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import _offload_log
        _offload_log.log_event(
            root, "redispatch" if redo else "converged",
            "review_convergence",
            f"{_norm_kind(kind)} {slice_id} -- {why[:120]}")
    except Exception:
        pass


def main(argv: list) -> int:
    if "--selftest" in argv:
        return _selftest()
    root = Path.cwd()
    # Resolve repo root (fail-open: use cwd).
    top = _git(root, "rev-parse", "--show-toplevel")
    if top and top.strip():
        root = Path(top.strip())
    if len(argv) >= 3 and argv[0] == "should-redispatch":
        redo, why = should_redispatch(root, argv[1], argv[2])
        print(("REDISPATCH: " if redo else "CONVERGED: ") + why)
        # T3-3: record BOTH outcomes so the saving is countable. A suppressed
        # round removes three things from the main context at once -- the Codex
        # verdict body, the finding triage, and a ~6.2 KB
        # `receiving-code-review` skill body -- but until now nothing recorded
        # that it happened, so the mechanism's value was unmeasurable in exactly
        # the way the agent cache's was (155 stores, 0 hits, unnoticed).
        # Logging the REDISPATCH side too makes the ratio meaningful: a
        # suppression count alone cannot say whether the gate is working or
        # simply never consulted.
        _log_decision(root, argv[1], argv[2], redo, why)
        return 0 if redo else 1
    if len(argv) >= 3 and argv[0] == "record":
        ok = record(root, argv[1], argv[2])
        print(f"recorded {argv[2]} verdict for {argv[1]}" if ok
              else f"NOT recorded ({argv[2]}): unknown kind or git unavailable")
        return 0
    if len(argv) >= 2 and argv[0] == "status":
        print(json.dumps(_load(root).get(argv[1], {}), indent=2))
        return 0
    print("usage: review_convergence.py should-redispatch|record <slice> <kind> "
          "| status <slice> | --selftest", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
