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
    a Codex dispatch Bash command's prompt arg.

    Two canonical dispatch shapes are recognized:

    1. Direct node invocation (legacy):
         node ".../codex-companion.mjs" adversarial-review "<prompt>"
       Extract the prompt as the first non-empty content after the
       `adversarial-review` token.

    2. Wrapper invocation (canonical post-section-28):
         bash scripts/codex-dispatch.sh '<prompt>'
       Extract the prompt as the first non-empty content after the
       `codex-dispatch.sh` token. The wrapper exec's into the same
       node command internally; routing the classifier through both
       shapes lets the gate attribute either form.

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

    # Wrapper shape (section-28): codex-dispatch.sh '<prompt>'. Detect
    # first because the wrapper line still mentions codex-companion.mjs
    # in a comment header that could pollute substring-only checks.
    if "codex-dispatch.sh" in cmd:
        after = cmd.split("codex-dispatch.sh", 1)[1].lstrip()
    elif "codex-companion.mjs" in cmd and "adversarial-review" in cmd:
        after = cmd.split("adversarial-review", 1)[1].lstrip()
    else:
        return ""
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
