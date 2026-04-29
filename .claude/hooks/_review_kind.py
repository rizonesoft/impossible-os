#!/usr/bin/env python3
"""Shared review-kind classifier for codex-companion.mjs adversarial-review
Bash invocations. Used by both phase1_evidence_gate (PreToolUse BLOCK) and
skill_step_map (observer step recording). Single source of truth for
"what counts as a [review-kind: X] dispatch" closes the drift named in
TODO-08 §17 deferred-XREF (M2).

Rule: scan the prompt argument's FIRST non-blank line for the marker.
This matches codex_review_completed.py _detect_review_kind: scanning the
entire body lets earlier markers in repository text or example diffs spoof
the attribution.
"""
import re

_REVIEW_KIND_RE = re.compile(
    r"\[\s*review[-_ ]kind\s*:\s*([a-zA-Z][a-zA-Z-]*)\s*\]",
    re.IGNORECASE,
)


def detect_review_kind_from_cmd(cmd: str) -> str:
    """Return the [review-kind: X] value from the first non-blank line of
    a codex-companion.mjs adversarial-review Bash command's prompt arg.
    Returns empty string when no marker is on the first non-blank line,
    when the command is not a codex dispatch, or when the prompt cannot
    be located.

    Multi-line dispatches with shell line-continuation (`\\<newline>`)
    classify as empty here -- the substring-and-split path is not
    shell-aware. The proper fix (shlex tokenize + segment-by-separator
    + heredoc body strip, matching codex_review_completed.py
    _is_codex_bash_trigger) is tracked as a follow-up section in
    00-infrastructure/TODO-08-automation-hardening; it also closes a
    pre-existing heredoc-spoofing class where Bash text containing a
    quoted Codex example would falsely classify as a dispatch.
    """
    if not isinstance(cmd, str) or not cmd:
        return ""
    if "codex-companion.mjs" not in cmd:
        return ""
    if "adversarial-review" not in cmd:
        return ""
    after = cmd.split("adversarial-review", 1)[1].lstrip()
    body = after
    # Skip a HEREDOC opener line if present.
    if body.startswith("\"$(cat <<") or body.startswith("'$(cat <<"):
        nl = body.find("\n")
        if nl >= 0:
            body = body[nl + 1:]
    elif body.startswith('"') or body.startswith("'"):
        body = body[1:]
    for ln in body.splitlines():
        s = ln.strip()
        if not s:
            continue
        m = _REVIEW_KIND_RE.search(s)
        if not m:
            return ""
        return m.group(1).lower()
    return ""


def is_dispatch_of_kind(cmd: str, kind: str) -> bool:
    """Convenience wrapper: detect_review_kind_from_cmd(cmd) == kind."""
    return detect_review_kind_from_cmd(cmd) == kind
