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


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0

    root = _repo_root()
    if not root:
        return 0

    sub_type = d.get("subagent_type") or d.get("agent_type") or ""
    duration_ms = d.get("duration_ms")
    tool_uses_total = d.get("tool_uses_total")

    # Fallback: walk transcript when payload is missing.
    if not sub_type or tool_uses_total is None:
        st, count = _walk_transcript_for_subagent(
            d.get("transcript_path", ""), d.get("agent_id", ""))
        if not sub_type:
            sub_type = st or "unknown"
        if tool_uses_total is None:
            tool_uses_total = count

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

    # Runaway detection.
    runaway = False
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
