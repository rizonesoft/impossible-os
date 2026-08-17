#!/usr/bin/env python3
# block-via: warning-only (SubagentStop logger; never blocks)
"""SubagentStop hook -- subagent budget audit (TODO-08 §11).

Records {ts_ns, subagent_type, duration_ms, tool_uses_total} to
.claude/state/subagent-log.jsonl per subagent invocation. If a single
subagent runs more than RUNAWAY_TOOL_USES tool calls or more than
RUNAWAY_DURATION_MS milliseconds, append a runaway-warning line to
.claude/state/acknowledged-but-skipped.log so SessionStart surfaces it.

Per design Q2: this hook reads the SubagentStop event payload first
(subagent_type, duration_ms, tool_uses_total are best-effort fields the
harness MAY pass) and falls back to walking transcript_path for the same
metrics.
"""
import json
import os
import subprocess
import sys
import time
from typing import Optional


_SUBAGENT_LOG_REL = ".claude/state/subagent-log.jsonl"
_SKIP_LOG_REL = ".claude/state/acknowledged-but-skipped.log"
# Union of observed SubagentStop payload key NAMES (never values). Settles
# whether a PreToolUse-on-Agent stamp can be correlated back to a Stop event,
# which is the unproven precondition of the dead duration arm in main().
_PAYLOAD_KEYS_REL = ".claude/state/subagent-payload-keys.json"

RUNAWAY_TOOL_USES = 30
RUNAWAY_DURATION_MS = 600 * 1000  # 10 minutes


def _repo_root() -> Optional[str]:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip() or None
    except Exception:
        return None


def _ts_ns() -> int:
    return time.time_ns() if hasattr(time, "time_ns") else int(time.time() * 1e9)


def _walk_transcript_for_subagent(path: str, agent_id: str):
    """Fallback: count tool_use events scoped to a subagent invocation
    by walking transcript_path. Returns (subagent_type, tool_uses_total)
    or (None, 0) if not derivable.
    """
    if not path or not os.path.exists(path):
        return (None, 0)
    sub_type = None
    count = 0
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                try:
                    ev = json.loads(line)
                except Exception:
                    continue
                msg = ev.get("message")
                if not isinstance(msg, dict):
                    continue
                content = msg.get("content")
                if not isinstance(content, list):
                    continue
                for c in content:
                    if not isinstance(c, dict):
                        continue
                    if c.get("type") != "tool_use":
                        continue
                    if c.get("name") == "Agent":
                        inp = c.get("input") or {}
                        sub_type = inp.get("subagent_type") or sub_type
                    else:
                        # Naive: any tool_use after the most recent Agent
                        # call counts toward that subagent's budget.
                        if sub_type:
                            count += 1
    except Exception:
        return (sub_type, count)
    return (sub_type, count)


def _count_leaf_tool_uses(path: str):
    """Count tool_use events in a subagent's OWN (leaf) transcript.

    Unlike the parent transcript walk above, a leaf transcript
    (`agent_transcript_path`) contains ONLY this subagent's turns, so every
    tool_use in it belongs to the subagent -- the parent/child boundary
    ambiguity that disabled parent-transcript counting (2026-04-28 Codex M2)
    does NOT apply here, making the count reliable. The prior code read the
    (often-absent) `transcript_path` and never consumed a count, so a real
    36-tool dispatch recorded 0 (Codex audit 2026-07-13, verified). Returns an
    int count, or None if the leaf path is absent/unreadable.
    """
    if not path or not os.path.exists(path):
        return None
    count = 0
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                try:
                    ev = json.loads(line)
                except Exception:
                    continue
                msg = ev.get("message")
                if not isinstance(msg, dict):
                    continue
                content = msg.get("content")
                if not isinstance(content, list):
                    continue
                for c in content:
                    if isinstance(c, dict) and c.get("type") == "tool_use":
                        count += 1
    except Exception:
        return None
    return count


def _record_payload_keys(root: str, payload: dict) -> None:
    """Accumulate the union of SubagentStop payload KEY NAMES.

    Why this exists. The duration arm below has been dead for two capture
    cycles: `duration_ms` is None in 0 of 1912 recorded payloads. The proposed
    repair is a PreToolUse-on-Agent hook stamping a start time keyed by an
    agent id, with elapsed computed here -- but nobody has ever established
    that the Stop payload CARRIES an id that a PreToolUse event could be
    matched against. Two cycles of filing have restated the fix without
    checking its precondition.

    So record the precondition instead of guessing at it. One segment of live
    traffic answers it deterministically: if a correlator key appears, the
    stamp fix is buildable; if none does, the arm should be DELETED rather
    than repaired.

    KEY NAMES ONLY, never values -- payloads carry transcript paths and
    free-form agent output, and this file is not a place to accumulate either.
    Bounded by the key space, so it converges to a few dozen bytes and stops
    growing. Fail-silent, like every other write in this hook: an audit
    sidecar must never be able to fail a subagent."""
    keys_path = os.path.join(root, _PAYLOAD_KEYS_REL)
    try:
        seen = set()
        if os.path.exists(keys_path):
            with open(keys_path, "r", encoding="utf-8") as f:
                prior = json.load(f)
            if isinstance(prior, dict) and isinstance(prior.get("keys"), list):
                seen = {k for k in prior["keys"] if isinstance(k, str)}
        fresh = {k for k in payload.keys() if isinstance(k, str)}
        if fresh <= seen:
            return                      # nothing new; do not rewrite the file
        merged = sorted(seen | fresh)
        os.makedirs(os.path.dirname(keys_path), exist_ok=True)
        tmp = keys_path + ".%d.tmp" % os.getpid()
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"keys": merged, "updated_ts_ns": _ts_ns()}, f, indent=1)
        os.replace(tmp, keys_path)
    except Exception:
        pass


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict):
        return 0

    root = _repo_root()
    if not root:
        return 0

    sub_type = d.get("subagent_type") or d.get("agent_type") or ""
    duration_ms = d.get("duration_ms")
    tool_uses_total = d.get("tool_uses_total")

    # Codex adversarial review 2026-04-28 M2: the prior fallback walked
    # the transcript and counted any tool_use after the most recent
    # Agent invocation, which polluted tool_uses_total with parent-
    # session tool calls and produced spurious SUBAGENT-RUNAWAY warnings.
    # The transcript boundary between subagent and parent is not
    # reliably derivable from text alone -- nested Agent calls, parent
    # tool calls after subagent return, etc. all break the count.
    #
    # New behavior: prefer payload metrics; only walk transcript for
    # subagent_type discovery (which is a narrow read of Agent input).
    # Track type-fallback and count-untrusted INDEPENDENTLY (re-
    # adversarial 2026-04-28 H1: a single conflated `fallback_used`
    # flag was wrongly suppressing the count-based runaway when only
    # subagent_type came from fallback while a real payload count was
    # present and trustworthy).
    type_fallback_used = False
    count_untrusted = False
    if not sub_type:
        st, _ = _walk_transcript_for_subagent(
            d.get("transcript_path", ""), d.get("agent_id", ""))
        sub_type = st or "unknown"
        type_fallback_used = True
    if tool_uses_total is None:
        # The harness did not pass a payload count. Count from the subagent's
        # OWN (leaf) transcript -- `agent_transcript_path` -- where every
        # tool_use belongs to this subagent, so the boundary ambiguity that
        # forbade parent-transcript counting (2026-04-28) does not apply. Only
        # if that leaf is absent/unreadable do we record 0 and disable
        # count-based runaway detection (Codex audit 2026-07-13, verified: the
        # old code read the wrong field and always recorded 0).
        leaf_count = _count_leaf_tool_uses(d.get("agent_transcript_path", ""))
        if leaf_count is not None:
            tool_uses_total = leaf_count
        else:
            tool_uses_total = 0
            count_untrusted = True

    record = {
        "ts_ns": _ts_ns(),
        "subagent_type": sub_type,
        "duration_ms": duration_ms,
        "tool_uses_total": tool_uses_total,
    }

    log_path = os.path.join(root, _SUBAGENT_LOG_REL)
    try:
        os.makedirs(os.path.dirname(log_path), exist_ok=True)
        with open(log_path, "a", encoding="utf-8") as f:
            f.write(json.dumps(record, separators=(",", ":")) + "\n")
    except Exception:
        pass

    _record_payload_keys(root, d)

    # Runaway detection. Count-based check fires only when the count
    # is trustworthy (payload-provided, not fallback-derived).
    #
    # WARNING (v14 close-out, 2026-08-16, measured): the duration arm below is
    # DEAD. The harness does NOT send `duration_ms` -- it is None in 1893 of
    # 1893 recorded SubagentStop payloads -- so `isinstance(duration_ms, int)`
    # is never true and the arm has never fired. The earlier comment here
    # ("duration_ms is always a payload field") was simply wrong. Making it
    # real needs a dispatch-time stamp keyed by agent_id in a PreToolUse-on-
    # Agent hook, then elapsed computed here; that is a separate design pass
    # (filed, overnight-runner-improvements-v14). Until then the arm guards
    # nothing, and the count arm cannot see the failure it was meant to catch
    # (long wall-clock, few tool calls). Re-baselining RUNAWAY_TOOL_USES alone
    # is explicitly NOT the fix -- it would quiet the noise while leaving that
    # gap invisible.
    runaway = False
    if not count_untrusted:
        if isinstance(tool_uses_total, int) and tool_uses_total >= RUNAWAY_TOOL_USES:
            runaway = True
    if isinstance(duration_ms, int) and duration_ms >= RUNAWAY_DURATION_MS:
        runaway = True
    if runaway:
        skip_path = os.path.join(root, _SKIP_LOG_REL)
        line = ("{ts} SUBAGENT-RUNAWAY type={t!r} duration_ms={dm} "
                "tool_uses={tu}").format(
            ts=record["ts_ns"], t=sub_type,
            dm=duration_ms, tu=tool_uses_total,
        )
        try:
            os.makedirs(os.path.dirname(skip_path), exist_ok=True)
            with open(skip_path, "a", encoding="utf-8") as f:
                f.write(line + "\n")
        except Exception:
            pass

    return 0


if __name__ == "__main__":
    sys.exit(main())
