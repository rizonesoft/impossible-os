#!/usr/bin/env python3
# block-via: warning-only (UserPromptSubmit hint; never blocks the prompt)
"""UserPromptSubmit hook: interactive agent-offload router.

The specialist-agent fleet (.claude/agents/) is wired into the pipeline
skills, but interactive sessions historically run the same legwork
in-context (measured: the 2026-07-02 overnight run dispatched checks-runner
0 times against 156 in-context script runs; interactive sessions have NO
nudge at all -- agent_dispatch_required.py is overnight-SECTIONS-only by
design). This hook closes the interactive gap at the cheapest point: when
the user's prompt matches an offloadable task shape, inject a one-line
systemMessage naming the default agent. Warning-only; the main session
stays free to work inline when the task is genuinely small.

Patterns are intentionally narrow (mirrors user_prompt_doctrine.py Q3
rationale): match task VERBS + domain nouns, not bare keywords, to avoid
false-positives on ordinary prompts.
"""
import json
import re
import sys


# (pattern, kind, target, note) -- kind is "agent" (dispatch it) or
# "script" (run it: green mechanics spend no model). First match wins; keep the list ordered by
# specificity (most specific domain first, generic exploration last).
_ROUTES = [
    (r"\b(?:validate|check|audit)\b.{0,40}\bTODO-\d+",
     "agent", "todo-validation-mapper",
     "run scripts/todo-graph/query.py first; the mapper maps only the fuzzy "
     "residue (sections/table/stamps/XREFs/Inputs)"),
    (r"\b(?:run|rerun|re-run)\b.{0,30}\b(?:build|test(?:s|-suite)?|lint|smoke)\b",
     "script", "bash scripts/overnight/run-artifact.sh <label> -- <command>",
     "a deterministic envelope, no model; dispatch diagnostic-digester only "
     "on a FAIL envelope"),
    (r"\b(?:when|what commit|which commit|who)\b.{0,50}\b(?:br(?:eak|oke|oken)|chang(?:e|ed)|introduc|regress)",
     "agent", "git-historian",
     "read-only git archaeology, returns a commit-trail digest"),
    (r"\b(?:ci|workflow|pipeline)s?\b.{0,40}\b(?:status|state|red|green|fail|pass|list)",
     "script", "gh run list --json headSha,status,conclusion,workflowName",
     "a direct query, no model; gh-query-runner only for --log-failed slices "
     "of a run already confirmed red"),
    (r"\b(?:pull request|pr\b|issue)s?\b.{0,40}\b(?:status|state|list)",
     "agent", "gh-query-runner",
     "read-only gh pr/issue queries, returns the state without paginated output"),
    (r"\bserial\b.{0,20}\blog|boot.{0,10}log.{0,30}\b(?:analy|audit|check|look)",
     "agent", "serial-log-auditor",
     "sweeps the log set, returns an anomaly timeline"),
    (r"\bovernight\b.{0,30}\b(?:log|run|report|transcript|cost|token)",
     "agent", "overnight-log-explorer",
     "run-transcript cost/behavior digest"),
    (r"\b(?:test )?coverage\b.{0,30}\b(?:gap|missing|map|check)|\bwhat(?:'s| is) untested\b",
     "agent", "test-coverage-mapper",
     "coverage brief: spec status, signatures, wiring points"),
    # D2: "where do the X helpers live / is there a Y primitive" hunts. These
    # otherwise run as 3-6 inline greps before any dispatch (measured: the 6-round
    # kstrdup/kstrndup/memcpy hunt). A location/existence question over a code
    # surface is a mapper dispatch, not a grep chain.
    (r"\b(?:where (?:do|does|are|is)|is there|do we have|does .{0,20} have|which)\b"
     r".{0,50}\b(?:helper|function|primitive|api|symbol|struct|macro|routine|"
     r"wrapper|allocator|lock|intrinsic)s?\b",
     "agent", "kernel-explorer",
     "locate/inventory the helpers+symbols in one map, not N inline greps"),
    (r"\b(?:explore|trace|how does|walk (?:me )?through|call path|who calls)\b.{0,60}\b(?:kernel|boot|driver|src/|scheduler|vmm|pmm|vfs|irq|apic)",
     "agent", "kernel-explorer",
     "execution-path trace + integration surface"),
    (r"\b(?:explore|find where|search (?:the )?(?:code|repo|codebase)|where is)\b",
     "agent", "Explore",
     "broad read-only fan-out search"),
]


_HINT = (
    "[offload hint -- not a block] This prompt matches an offloadable shape. "
    "Default: dispatch Agent(subagent_type=\"{target}\") -- {note} -- and keep "
    "the main context lean; verify load-bearing findings at file:line before "
    "acting (trust contract). Work inline only if the task is genuinely one "
    "or two reads. Doctrine: CLAUDE.md \"Interactive offload\"."
)

_SCRIPT_HINT = (
    "[offload hint -- not a block] This prompt matches a deterministic shape. "
    "Run `{target}` -- {note}. Doctrine: CLAUDE.md \"Interactive offload\" "
    "(green mechanics never spend a model)."
)


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict):
        return 0

    prompt = d.get("prompt") or d.get("user_prompt") or ""
    if not isinstance(prompt, str) or not prompt:
        return 0

    for pat, kind, target, note in _ROUTES:
        if re.search(pat, prompt, re.I):
            # T1-4: budget this advisory. Measured 2026-07-27: 487 injections
            # across the offload-events log with ZERO recorded follows -- the
            # single largest advisory emitter and the least-heeded. The first
            # couple of hints per session carry the whole teaching value; the
            # rest are pure cache-read, re-charged on every later turn.
            # Suppressions are still COUNTED in state, so the volume stays
            # measurable for the retire-or-promote call (T1-4(b)).
            try:
                from pathlib import Path
                import _advisory_budget
                if not _advisory_budget.should_emit(
                        Path.cwd(), "interactive_offload_router",
                        cap=2, session_id=str(d.get("session_id") or "")):
                    return 0
            except Exception:
                pass    # fail-open: a budget bug must never silence the hook
            try:
                import _offload_log
                from pathlib import Path
                _offload_log.log_event(Path.cwd(), "fire",
                                       "interactive_offload_router", target)
            except Exception:
                pass
            print(json.dumps(
                {"systemMessage": (_SCRIPT_HINT if kind == "script" else _HINT).format(
                    target=target, note=note)}))
            return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
