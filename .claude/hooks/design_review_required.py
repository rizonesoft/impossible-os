#!/usr/bin/env python3
# Pre-implementation design-review gate (TODO-07-style protection):
# once a Skill(implement-todo-section, ...) invocation is seen in the
# transcript, the agent MUST dispatch a Codex design review (Bash to
# codex-companion.mjs adversarial-review with the word "design" in the
# prompt) BEFORE editing any source code. The block fires on Edit /
# Write / MultiEdit on non-markdown / non-.claude files.
#
# Why this hook exists: feedback_codex_design_review_mandatory memory
# tracks the recurring pattern of skipping step 4 of implement-todo-
# section because its conditional ("for complex/high-risk sections")
# wording lets me judge the section as "straightforward enough to
# skip". That judgment has been wrong every time -- TODO-07 §11 + §12
# adversarial reviews caught design-shape issues that a pre-code Codex
# pass would have caught for a fraction of the iteration cost. Memory
# alone has not been enough (per feedback_skill_invocation_drift, the
# durable fix is hooks, not skill edits).
#
# Opt-out: SKIP_DESIGN_REVIEW_HOOK=1 env var. Use sparingly --
# legitimate cases: docs-only sections, stamp-only edits, sections
# explicitly classified as no-design-needed (pure constant additions).
import json
import os
import re
import sys


def _is_code_target(path: str) -> bool:
    """Return True if path looks like a source-code edit that should
    wait for the design review.

    Allowed without design review (return False here):
      - Markdown files (TODO sections, docs)
      - .claude/ tooling (settings.json, hooks, skills)
      - Build artifacts (build/, build.log)
      - Test runner BAT files (scripts/debug/**/*.bat)

    Everything else (code: .py, .c, .h, .asm, .sh, .ps1, .ts, etc.) is
    a code edit; block until design review fires.
    """
    if not path:
        return False
    norm = path.replace("\\", "/")
    if norm.endswith(".md"):
        return False
    if "/.claude/" in norm or norm.startswith(".claude/"):
        return False
    if "/build/" in norm or norm.startswith("build/"):
        return False
    # Coverage docs are auto-regenerated, not a code edit.
    if "/docs/test-coverage/" in norm:
        return False
    if norm.endswith(".bat") and "/scripts/debug/" in norm:
        return False
    return True


def _scan_transcript(path: str) -> tuple[bool, bool, str]:
    """Walk the session transcript JSONL and return:
      (impl_seen, design_seen_after_impl, impl_args)
    impl_seen          -- a Skill(implement-todo-section, ...) was invoked
    design_seen_after  -- a Bash dispatch to codex-companion.mjs
                          adversarial-review with "design" in the
                          prompt was invoked AFTER impl_seen
    impl_args          -- args of the most recent implement-todo-section
                          invocation (for the error message)
    """
    impl_seen = False
    design_seen = False
    impl_args = ""
    if not path or not os.path.exists(path):
        return (False, False, "")
    try:
        f = open(path, encoding="utf-8")
    except OSError:
        return (False, False, "")
    with f:
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
                if not isinstance(c, dict) or c.get("type") != "tool_use":
                    continue
                name = c.get("name") or ""
                inp = c.get("input") or {}
                if name == "Skill":
                    skill = inp.get("skill") or inp.get("name") or ""
                    if skill == "implement-todo-section":
                        impl_seen = True
                        # Reset design flag: every fresh implement-
                        # todo-section call starts a new gate window.
                        design_seen = False
                        impl_args = inp.get("args", "")
                elif name == "Bash" and impl_seen:
                    cmd = (inp.get("command") or "")
                    if ("codex-companion.mjs" in cmd
                            and "adversarial-review" in cmd
                            and re.search(r"\bdesign\b", cmd, re.I)):
                        design_seen = True
    return (impl_seen, design_seen, impl_args)


def main() -> int:
    if os.environ.get("SKIP_DESIGN_REVIEW_HOOK", "") == "1":
        return 0
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    tn = d.get("tool_name", "")
    if tn not in ("Edit", "Write", "MultiEdit"):
        return 0
    ti = d.get("tool_input", {}) or {}
    target = ti.get("file_path") or ti.get("path") or ""
    if not _is_code_target(target):
        return 0

    transcript = d.get("transcript_path", "")
    impl_seen, design_seen, impl_args = _scan_transcript(transcript)
    if not impl_seen:
        return 0
    if design_seen:
        return 0

    sys.stderr.write(
        "[codex-design-review REQUIRED -- pre-code gate] "
        "implement-todo-section was invoked (args: " + impl_args[:120] + ") "
        "but no Codex design dispatch has been seen yet. Step 4 of "
        "implement-todo-section requires a design pass BEFORE editing "
        "any source code -- target " + target + " is a code edit. "
        "Required: dispatch via the Codex plugin BEFORE this Edit/Write, e.g.\n"
        "  node /home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs adversarial-review \"<design review prompt covering the planned section>\"\n"
        "The prompt must contain the word 'design' for this hook to "
        "recognize it as a design pass. After Codex returns, walk every "
        "finding via the receiving-code-review pattern (verify, Fix / "
        "Reject / Accept), then proceed to code. "
        "Allowed while gated: Read / Grep / Glob (info gathering); "
        "Edits to .md / .claude/ / .bat / docs/test-coverage/ files. "
        "Opt-out for legitimate false positives (docs-only sections, "
        "stamp-only edits, pure constant additions): SKIP_DESIGN_REVIEW_HOOK=1 "
        "env var on the next tool call. "
        "Canonical rule: feedback_codex_design_review_mandatory memory + "
        "implement-todo-section SKILL.md step 4."
    )
    return 2


if __name__ == "__main__":
    sys.exit(main())
