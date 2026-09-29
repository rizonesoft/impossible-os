#!/usr/bin/env python3
# block-via: exit 2 (whole-file re-reads of agent-covered paths, 3rd+ occurrence)
"""Make an agent dispatch REPLACE the main-session read, not layer on top of it.

`CLAUDE.md` already states the rule -- "An agent dispatch must REPLACE expected
main-session reads, never add a layer on top" -- and nothing enforced it.
MEASURED 2026-07-27: 48 agent dispatches against 4,400 main-session Bash calls,
1,412 Reads and 1,343 Greps. The sidechain did 1,016 greps for 14,584 output
tokens, i.e. it did the cheap work correctly and the main session then did the
same work again in the expensive context.

`inflight_race_guard` (D1) already warns when a read RACES an agent that is
still running, but it CLEARS its map on SubagentStop -- the moment the report
lands and the coverage becomes durable, the guard stops caring. This hook owns
exactly that other half: coverage AFTER the agent returned.

Modes:
    record   PostToolUse (Task|Agent)  -- persist what the returned agent covered
    (gate)   PreToolUse  (Read)        -- warn, then block, on a covered re-read

WHAT IS GATED, AND WHAT DELIBERATELY IS NOT
  * Only WHOLE-FILE reads. A slice read (`offset`/`limit`) is ALWAYS allowed --
    that is the trust-contract verification read, the main session confirming
    ONE load-bearing finding at file:line before acting on it. It is the read
    that PROTECTS quality, it is cheap because it is bounded, and the acceptance
    criterion for this work requires it to stay free. Suppressing it would trade
    a real guarantee for tokens, which is the Never-cut line.
  * Only paths an EXPLORATORY agent actually covered, recorded from its dispatch
    prompt and its returned report.
  * Only within the SAME session (a fresh worker has neither the report nor the
    context, so its reads are legitimate).
  * WARN twice, BLOCK from the third. The first read after a report is often a
    legitimate spot-check; the third is the layering this exists to stop, and by
    then the message has already been shown twice.

Fail-open on every error path. Kill switch: `AGENT_COVERAGE_DISABLE=1`.
Code: [AGENT-COVERED] -- docs/infrastructure/hook-codes.md
Selftest: python3 agent_coverage_gate.py --selftest
"""
from __future__ import annotations

import json
import os
import re
import sys
from pathlib import Path

STATE_REL = ".claude/state/agent-coverage.json"
WARN_LIMIT = 2          # warnings before the block
MAX_PATHS = 200
MAX_REPORT_BYTES = 60_000

# Same shape inflight_race_guard uses, widened with the doc/markdown surfaces an
# analyst also maps.
_FILE_RE = re.compile(r"\b([\w./-]+\.(?:c|h|asm|S|cc|cpp|hpp|py|md|sh|json))\b")

# Only READ-ONLY exploratory/mapper agents establish coverage. A judgment or
# executor dispatch does not map a file surface, so recording it would suppress
# reads nothing had actually covered.
_COVERING_AGENTS = {
    "kernel-explorer", "section-context-mapper", "concurrency-evidence-mapper",
    "review-evidence-mapper", "test-coverage-mapper", "xref-dependency-mapper",
    "todo-validation-mapper", "serial-log-auditor", "overnight-log-explorer",
    "ssdt-auditor", "doc-sync-auditor", "todo-hygiene-auditor", "Explore",
}

_MSG = (
    "[AGENT-COVERED {mode}] {path} was already mapped by the `{agent}` dispatch "
    "that returned earlier in this session -- its report covers this file, and "
    "re-reading it whole is the 'dispatch ADDS a layer' pattern CLAUDE.md "
    "forbids ('an agent dispatch must REPLACE expected main-session reads'). "
    "Measured 2026-07-27: 48 dispatches against 1,412 main-session Reads. "
    "Reuse the report. If you need to VERIFY one finding, read the SLICE around "
    "it -- `Read(offset, limit)` is always allowed and is the trust-contract "
    "read this gate deliberately protects. Kill switch: AGENT_COVERAGE_DISABLE=1."
)


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _load(sp: Path, session_id: str) -> dict:
    try:
        st = json.loads(sp.read_text(encoding="utf-8"))
        if not isinstance(st, dict):
            raise ValueError
    except Exception:
        st = {}
    # Session-bound: a fresh worker holds neither the report nor the context, so
    # its reads are legitimate and must not inherit a previous session's map.
    if st.get("session_id") != session_id:
        return {"session_id": session_id, "covered": {}, "hits": {}}
    st.setdefault("covered", {})
    st.setdefault("hits", {})
    return st


def _save(sp: Path, st: dict) -> None:
    try:
        sp.parent.mkdir(parents=True, exist_ok=True)
        tmp = sp.with_suffix(sp.suffix + ".tmp")
        tmp.write_text(json.dumps(st))
        tmp.replace(sp)
    except Exception:
        pass


def _paths(text: str) -> set:
    return {m.group(1) for m in _FILE_RE.finditer(text or "")}


def record() -> int:
    """PostToolUse(Task|Agent): persist what the returned agent covered."""
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict):
        return 0
    ti = d.get("tool_input") or {}
    if not isinstance(ti, dict):
        return 0
    agent = str(ti.get("subagent_type") or ti.get("agent") or "")
    if agent not in _COVERING_AGENTS:
        return 0
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import agent_result_cache
        report = agent_result_cache._extract_report(d.get("tool_response"))
    except Exception:
        report = ""
    text = str(ti.get("prompt") or "") + "\n" + (report or "")[:MAX_REPORT_BYTES]
    paths = _paths(text)
    if not paths:
        return 0
    sp = _repo_root() / STATE_REL
    st = _load(sp, str(d.get("session_id") or ""))
    for p in list(paths)[:MAX_PATHS]:
        st["covered"].setdefault(os.path.basename(p), agent)
    # Bound the map.
    if len(st["covered"]) > MAX_PATHS:
        st["covered"] = dict(list(st["covered"].items())[-MAX_PATHS:])
    _save(sp, st)
    return 0


def main() -> int:
    """PreToolUse(Read): warn, then block, on a covered WHOLE-FILE re-read."""
    if os.environ.get("AGENT_COVERAGE_DISABLE") == "1":
        return 0
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict) or d.get("tool_name") != "Read":
        return 0
    ti = d.get("tool_input") or {}
    if not isinstance(ti, dict):
        return 0
    # THE CARVE-OUT: a slice read is the trust-contract verification read and is
    # always free. Only an unbounded whole-file read is the layering shape.
    if ti.get("offset") is not None or ti.get("limit") is not None:
        return 0
    fp = str(ti.get("file_path") or "")
    if not fp:
        return 0
    # A subagent's own reads are the work being delegated; never gate them.
    # Identity comes from POSITIVE agent markers (agent_id, agent_type,
    # agent_transcript_path, a subagent transcript path), not from
    # `transcript_path` alone: a subagent's payload carries the PARENT's
    # transcript path, so the path test read every subagent as the main session
    # (2026-09-28 canary: 61 subagent Reads refused in one cycle). Same fix as
    # websearch_offload_gate's 2026-07-31 one.
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import runner_bash_guard
        if runner_bash_guard.is_subagent_payload(d):
            return 0
    except Exception:
        pass

    sp = _repo_root() / STATE_REL
    st = _load(sp, str(d.get("session_id") or ""))
    base = os.path.basename(fp)
    agent = st.get("covered", {}).get(base)
    if not agent:
        return 0
    n = int(st.get("hits", {}).get(base) or 0) + 1
    st.setdefault("hits", {})[base] = n
    _save(sp, st)
    mode = "warn" if n <= WARN_LIMIT else "BLOCK"
    sys.stderr.write(_MSG.format(mode=mode, path=base, agent=agent) + "\n")
    try:
        import _offload_log
        _offload_log.log_event(_repo_root(), "fire", "agent_coverage_gate",
                               f"{base} {mode}")
    except Exception:
        pass
    return 2 if n > WARN_LIMIT else 0


# --------------------------------------------------------------------------

def _selftest() -> int:  # noqa: C901
    import contextlib, io, tempfile
    fails = []

    def check(name, cond):
        if not cond:
            fails.append(name)

    tmp = Path(tempfile.mkdtemp())
    (tmp / ".claude/state").mkdir(parents=True)
    global _repo_root
    orig = _repo_root
    _repo_root = lambda: tmp                                  # noqa: E731

    def run_record(agent, prompt, report="", session="s1"):
        old = sys.stdin
        try:
            sys.stdin = io.StringIO(json.dumps({
                "tool_name": "Task", "session_id": session,
                "tool_input": {"subagent_type": agent, "prompt": prompt},
                "tool_response": report}))
            with contextlib.redirect_stderr(io.StringIO()):
                return record()
        finally:
            sys.stdin = old

    def run_read(path, session="s1", transcript="/x/main.jsonl", payload=None, **ti):
        old = sys.stdin
        try:
            body = {"file_path": path}
            body.update(ti)
            d = {"tool_name": "Read", "session_id": session,
                 "transcript_path": transcript, "tool_input": body}
            d.update(payload or {})
            sys.stdin = io.StringIO(json.dumps(d))
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                rc = main()
            return rc, err.getvalue()
        finally:
            sys.stdin = old

    run_record("kernel-explorer", "map src/kernel/quota/quota.c and task.c")
    check("uncovered-file-free", run_read("/r/src/other/zzz.c")[0] == 0)
    r1 = run_read("/r/src/kernel/quota/quota.c")
    check("1st-warns", r1[0] == 0 and "AGENT-COVERED warn" in r1[1])
    r2 = run_read("/r/src/kernel/quota/quota.c")
    check("2nd-warns", r2[0] == 0 and "warn" in r2[1])
    r3 = run_read("/r/src/kernel/quota/quota.c")
    check("3rd-blocks", r3[0] == 2 and "BLOCK" in r3[1])
    check("block-names-the-agent", "kernel-explorer" in r3[1])

    # THE CARVE-OUT: slice reads stay free no matter how many times.
    for _ in range(5):
        check("slice-always-free",
              run_read("/r/src/kernel/quota/quota.c", offset=10, limit=20)[0] == 0)

    # Subagent reads are the delegated work itself.
    check("subagent-free",
          run_read("/r/task.c", transcript="/p/agent-x.jsonl")[0] == 0)
    # The REAL subagent payload shape: the parent's transcript path plus an
    # agent marker. Before 2026-09-29 this read was refused (61 in one cycle).
    for _ in range(4):
        check("subagent-with-parent-transcript-free",
              run_read("/r/src/kernel/quota/quota.c", transcript="/x/main.jsonl",
                       payload={"agent_id": "a1b2c3", "agent_type": "section-context-mapper"})[0] == 0)
    # Refusal direction: the main session, with no agent marker, is still blocked.
    rm = run_read("/r/src/kernel/quota/quota.c")
    check("main-still-blocks-after-subagent-reads", rm[0] == 2 and "BLOCK" in rm[1])
    # A different session starts clean.
    check("other-session-free", run_read("/r/task.c", session="s2")[0] == 0)
    # Non-covering agent types establish no coverage.
    (tmp / STATE_REL).unlink(missing_ok=True)
    run_record("kernel-quality-auditor", "judge src/kernel/vmm.c")
    check("non-covering-agent-no-coverage", run_read("/r/src/kernel/vmm.c")[0] == 0)
    # Kill switch.
    (tmp / STATE_REL).unlink(missing_ok=True)
    run_record("Explore", "sweep src/kernel/pe.c")
    os.environ["AGENT_COVERAGE_DISABLE"] = "1"
    check("kill-switch", run_read("/r/src/kernel/pe.c")[0] == 0)
    del os.environ["AGENT_COVERAGE_DISABLE"]
    # Malformed payloads fail open.
    old = sys.stdin
    try:
        sys.stdin = io.StringIO("not json")
        with contextlib.redirect_stderr(io.StringIO()):
            check("malformed-fails-open", main() == 0)
    finally:
        sys.stdin = old

    _repo_root = orig
    if fails:
        sys.stderr.write("agent_coverage_gate selftest FAIL: "
                         + "; ".join(sorted(set(fails))) + "\n")
        return 1
    print("agent_coverage_gate selftest OK")
    return 0


if __name__ == "__main__":
    try:
        if "--selftest" in sys.argv:
            sys.exit(_selftest())
        sys.exit(record() if "record" in sys.argv[1:] else main())
    except Exception:
        sys.exit(0)   # fail-open: a broken coverage gate must never wedge a run
