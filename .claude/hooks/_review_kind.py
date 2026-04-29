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

    TODO-08 section-30: the dispatch detection + prompt extraction
    delegate to the shared `_codex_dispatch` helper which uses shell-
    aware parsing (shlex tokenize + segment-by-control-operator + per-
    segment heredoc-body strip + argv shape match). This closes the
    substring-classifier spoofing class (heredoc bodies, prose mentions,
    search commands) and adds multi-line continuation support.

    Three dispatch shapes are recognized via the helper:
      1. node ".../codex-companion.mjs" adversarial-review "<prompt>"
      2. bash scripts/codex-dispatch.sh '<prompt>' (section-28 wrapper)
      3. codex review "<prompt>" / codex e "<prompt>" (bare CLI)

    Returns empty string when the command is not a Codex dispatch OR
    the prompt's first non-blank line carries no marker.
    """
    import sys
    from pathlib import Path
    _hook_dir = Path(__file__).resolve().parent
    if str(_hook_dir) not in sys.path:
        sys.path.insert(0, str(_hook_dir))
    from _codex_dispatch import extract_dispatch_prompt
    body = extract_dispatch_prompt(cmd)
    if not body:
        return ""
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
