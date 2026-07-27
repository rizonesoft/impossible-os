#!/usr/bin/env python3
# block-via: warning-only (HELPER module -- never blocks; pure state read/write)
"""Shared per-session budget for ADVISORY hook injections (token-saver T1-4).

Every `systemMessage` an advisory hook emits is not a one-off cost: it lands in
the context and is then re-read in the cached prefix on every later turn of the
session. Cache-read is ~81% of run spend, so a reminder that fires 487 times is
charged 487 times AND compounds.

MEASURED 2026-07-27 from `.claude/state/offload-events.jsonl` (1,974 fire
events). The headline "1,609 fires / 14% follow-through" conflates two very
different things, and only one of them is this helper's business:

    hook                        injections  follows   class
    interactive_offload_router         487        0   REMINDER  <- budgeted
    build_offload_reminder             441        -   BLOCK     <- NOT budgeted
    codex_wait_discipline              228        -   BLOCK
    websearch_offload_gate             180        -   BLOCK
    agent_dispatch_required             26        -   BLOCK+WARN

A BLOCK's "fire" means it BLOCKED, not that it advised and was ignored -- its
compliance is enforced, so a follow-rate is meaningless for it and rate-limiting
it would punch a hole in a gate. This helper is for REMINDER-class hooks ONLY,
and `scripts/test-tooling.sh` asserts no hook with a `return 2` imports it.

`build_offload_reminder` was listed REMINDER in MANIFEST.md but is a P3.4
BLOCK-with-reroute (`return 2`); the class was corrected 2026-07-27 and it is
NOT budgeted. That left exactly ONE genuine candidate:
`interactive_offload_router`, 487 injections with ZERO recorded follows.

CONTRACT
    should_emit(root, hook, cap=2, session_id="") -> bool

    True  = emit the advisory (and the emission is counted).
    False = stay silent; the suppression is recorded in state so the volume is
            still measurable, which is what makes the retire-or-promote decision
            in T1-4(b) possible later without guessing.

The first `cap` injections per session carry the whole teaching value; the 40th
identical reminder changes no behaviour and is pure cache-read. Session-scoped,
so every fresh worker starts with its full budget and a rollover re-teaches.

FAIL-OPEN, deliberately in the EMIT direction: any error returns True. A broken
budget must never silence a hook -- the failure mode of over-reminding is cost,
the failure mode of wrongly-silencing is a lost guardrail.
"""
from __future__ import annotations

import json
from pathlib import Path

STATE_REL = ".claude/state/advisory-budget.json"
DEFAULT_CAP = 2


def _load(path: Path, session_id: str) -> dict:
    try:
        st = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(st, dict):
            raise ValueError
    except Exception:
        st = {}
    # A new session gets a fresh budget: the point is per-session teaching, and
    # a rollover SHOULD re-teach the new worker.
    if st.get("session_id") != session_id:
        return {"session_id": session_id, "counts": {}, "suppressed": {}}
    st.setdefault("counts", {})
    st.setdefault("suppressed", {})
    if not isinstance(st["counts"], dict):
        st["counts"] = {}
    if not isinstance(st["suppressed"], dict):
        st["suppressed"] = {}
    return st


def should_emit(root, hook: str, cap: int = DEFAULT_CAP,
                session_id: str = "") -> bool:
    """Return True if `hook` may inject now; False once its budget is spent."""
    try:
        if not hook:
            return True
        # NO SESSION IDENTITY -> NO BUDGET. The contract is "at most `cap`
        # injections PER SESSION"; without a session id there is no session to
        # scope to, and counting anyway would accumulate across unrelated
        # invocations and silence the hook permanently. Real hook payloads carry
        # `session_id`; anything invoking a hook without one (a direct CLI call,
        # a fixture) is outside the contract and must not be rate-limited.
        # Caught by the existing offload_router tests, which drive the hook with
        # no session and were silenced from the third case onward.
        if not session_id:
            return True
        p = Path(root) / STATE_REL
        st = _load(p, session_id or "")
        n = int(st["counts"].get(hook) or 0)
        allow = n < max(1, int(cap))
        if allow:
            st["counts"][hook] = n + 1
        else:
            st["suppressed"][hook] = int(st["suppressed"].get(hook) or 0) + 1
        try:
            p.parent.mkdir(parents=True, exist_ok=True)
            tmp = p.with_suffix(p.suffix + ".tmp")
            tmp.write_text(json.dumps(st))
            tmp.replace(p)
        except Exception:
            pass
        return allow
    except Exception:
        return True     # fail-open: never silence on a helper bug


def _selftest() -> int:
    import tempfile
    fails = []
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        (root / ".claude/state").mkdir(parents=True)
        s = "sess-1"
        got = [should_emit(root, "h", cap=2, session_id=s) for _ in range(5)]
        if got != [True, True, False, False, False]:
            fails.append(f"cap-2 sequence wrong: {got}")
        st = json.loads((root / STATE_REL).read_text())
        if st["counts"].get("h") != 2:
            fails.append("emitted count not capped at 2")
        if st["suppressed"].get("h") != 3:
            fails.append("suppressions not recorded (T1-4(b) needs the volume)")
        # A different hook has its own independent budget.
        if not should_emit(root, "other", cap=2, session_id=s):
            fails.append("budgets are not per-hook")
        # A new session resets.
        if not should_emit(root, "h", cap=2, session_id="sess-2"):
            fails.append("new session did not reset the budget")
        # Fail-open on an unwritable/garbage state file.
        (root / STATE_REL).write_text("{not json")
        if not should_emit(root, "h", cap=2, session_id="sess-3"):
            fails.append("malformed state did not fail open")
        # No session identity -> no budget (the contract is PER SESSION).
        got = [should_emit(root, "nosess", cap=2, session_id="") for _ in range(5)]
        if got != [True] * 5:
            fails.append(f"empty session_id was rate-limited: {got}")
    if fails:
        for f in fails:
            print("FAIL:", f)
        return 1
    print("_advisory_budget selftest OK")
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(_selftest() if "--selftest" in sys.argv else 0)
