#!/usr/bin/env python3
# block-via: exit 2 (subagent Bash hazard commands only; main session untouched)
"""PreToolUse(Bash): runtime backstop for the runner agent class.

The runner agents (checks-runner, git-historian, gh-query-runner) carry Bash
for named idempotent commands; their prompts forbid mutations and Codex. This
hook makes the highest-risk prohibitions ENFORCED rather than prose: when the
Bash call originates from a SUBAGENT context (the hook payload's
transcript_path is an agent transcript, `agent-*.jsonl`, not the main-session
transcript), it BLOCKs:

  - any Codex surface (codex CLI, scripts/codex-*.sh, codex-companion.mjs) --
    a subagent-issued dispatch would corrupt review-receipt state;
  - mutating git verbs (add/commit/push/reset/checkout/restore/stash/rebase/
    merge/tag/branch -d/config) -- the main session is the sole committer;
  - mutating gh forms (pr create/comment/merge/..., api with -X/-f/-F,
    secret/variable/repo edit) -- outward-facing actions stay main-session;
  - SKIP_*/override env assignments -- gate escapes are main-session decisions.

Main-session Bash calls are untouched (git commit / codex-dispatch are its
job). Fail-open on any parse error or unknown payload shape: a broken guard
must never block real work. The static side (roster-gated tools allowlist)
is lint.sh Check 14.
"""
from __future__ import annotations

import json
import os
import re
import sys

_HAZARDS: list[tuple[str, str]] = [
    (r"(?:^|[\s/;|&])codex(?:\s|$)", "codex CLI"),
    (r"codex-dispatch\.sh|codex-bg-dispatch\.sh|codex-dispatch-with-files\.sh", "codex dispatch wrapper"),
    (r"codex-companion\.mjs", "codex companion"),
    (r"(?:^|[\s;|&])git\s+(?:add|commit|push|reset|checkout|restore|stash|rebase|merge|tag|cherry-pick|revert|clean|config|remote\s+(?:add|remove|set-url)|branch\s+(?:-d|-D|-m))\b", "mutating git"),
    (r"(?:^|[\s;|&])gh\s+(?:pr|issue|release|repo|workflow|secret|variable)\s+(?:create|comment|merge|close|edit|delete|upload|run|enable|disable|set)\b", "mutating gh"),
    (r"(?:^|[\s;|&])gh\s+api\s+[^|;&]*(?:-X\s|--method\s|-f\s|-F\s|--field\s)", "mutating gh api"),
    (r"(?:^|[\s;|&])SKIP_[A-Z_]+=", "gate-skip override"),
]


def _is_subagent_transcript(path: str) -> bool:
    if not path:
        return False
    base = os.path.basename(path)
    return base.startswith("agent-") or "/subagents/" in path


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict):
        return 0
    if d.get("tool_name") != "Bash":
        return 0
    if not _is_subagent_transcript(str(d.get("transcript_path") or "")):
        return 0
    ti = d.get("tool_input")
    if not isinstance(ti, dict):
        return 0
    cmd = str(ti.get("command") or "")
    if not cmd:
        return 0
    for pattern, label in _HAZARDS:
        if re.search(pattern, cmd):
            sys.stderr.write(
                f"[runner-bash-guard BLOCK] subagent Bash call matches a forbidden "
                f"surface ({label}). Runner agents execute named verification/query "
                f"commands only; edits, git/gh mutations, gate skips, and ALL Codex "
                f"dispatch belong to the main session. Return your findings as text "
                f"instead.\n"
            )
            return 2
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        sys.exit(0)  # fail-open: a broken guard must never block real work
