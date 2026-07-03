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


# (pattern, agent, note) -- first match wins; keep the list ordered by
# specificity (most specific domain first, generic exploration last).
_ROUTES = [
    (r"\b(?:validate|check|audit)\b.{0,40}\bTODO-\d+",
     "todo-validation-mapper",
     "structural evidence map (sections/table/stamps/XREFs/Inputs)"),
    (r"\b(?:run|rerun|re-run)\b.{0,30}\b(?:build|test(?:s|-suite)?|lint|smoke)\b",
     "checks-runner",
     "runs the named verification script, returns verdict + failure digest"),
    (r"\b(?:when|what commit|which commit|who)\b.{0,50}\b(?:br(?:eak|oke|oken)|chang(?:e|ed)|introduc|regress)",
     "git-historian",
     "read-only git archaeology, returns a commit-trail digest"),
    (r"\b(?:ci|workflow|pipeline|pull request|pr\b|issue)s?\b.{0,40}\b(?:status|state|red|green|fail|pass|list)",
     "gh-query-runner",
     "read-only gh queries, returns the state without paginated output"),
    (r"\bserial\b.{0,20}\blog|boot.{0,10}log.{0,30}\b(?:analy|audit|check|look)",
     "serial-log-auditor",
     "sweeps the log set, returns an anomaly timeline"),
    (r"\bovernight\b.{0,30}\b(?:log|run|report|transcript|cost|token)",
     "overnight-log-explorer",
     "run-transcript cost/behavior digest"),
    (r"\b(?:test )?coverage\b.{0,30}\b(?:gap|missing|map|check)|\bwhat(?:'s| is) untested\b",
     "test-coverage-mapper",
     "coverage brief: spec status, signatures, wiring points"),
    (r"\b(?:explore|trace|how does|walk (?:me )?through|call path|who calls)\b.{0,60}\b(?:kernel|boot|driver|src/|scheduler|vmm|pmm|vfs|irq|apic)",
     "kernel-explorer",
     "execution-path trace + integration surface"),
    (r"\b(?:explore|find where|search (?:the )?(?:code|repo|codebase)|where is)\b",
     "Explore",
     "broad read-only fan-out search"),
]


_HINT = (
    "[offload hint -- not a block] This prompt matches an offloadable shape. "
    "Default: dispatch Agent(subagent_type=\"{agent}\") -- {note} -- and keep "
    "the main context lean; verify load-bearing findings at file:line before "
    "acting (trust contract). Work inline only if the task is genuinely one "
    "or two reads. Doctrine: CLAUDE.md \"Specialist agents\" interactive "
    "offload table."
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

    for pat, agent, note in _ROUTES:
        if re.search(pat, prompt, re.I):
            print(json.dumps(
                {"systemMessage": _HINT.format(agent=agent, note=note)}))
            return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
