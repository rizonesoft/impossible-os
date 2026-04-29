#!/usr/bin/env python3
# block-via: warning-only (HELPER module -- never blocks; pure predicate
# function, no exit codes, no stderr writes).
"""Policy-scoped review-pipeline passthrough predicate.

Owner: 00-infrastructure/TODO-08-automation-hardening (PreToolUse
prefix-allowlist standardization section). Companion to `_skip_env.py`
(which unifies HOW gates read SKIP envs); this module unifies WHICH bash
command prefixes count as "review-pipeline tooling" for gates that need
to let the review-pipeline tooling run while blocking unrelated work.

**SCOPE: review-pipeline gates ONLY.** This is NOT a generic
"trusted-command" primitive. The prefix set permits broad code-execution
shapes (`node `, `python3 `, `bash scripts/`) because the post-ship
review pipeline runs Codex dispatches, inline scripted helpers, and
repo tooling scripts. A gate with a different threat model (e.g. a
write-sensitive gate, a kernel-touching gate) MUST NOT inherit from
this helper -- it would launder code-execution prefixes into a context
where they are not safe. Future policy-scoped allowlists get their own
helper module (e.g. `_kernel_touching_passthrough.py`), not extensions
to this one.

Per the section-29 design review H1 finding (2026-04-29): centralizing
under a generic `TRUSTED_TOOL_PREFIXES` name would let future gates
auto-allow these prefixes without examining whether the policy fits
their threat model. The policy-scoped naming makes the inheritance
contract explicit: only gates that ARE review-pipeline gates may
consume this helper.
"""

# Canonical review-pipeline prefixes. Each entry is the literal prefix
# string a Bash command must START with for the gate to short-circuit.
# Trailing space is significant: `git ` matches `git commit` but NOT
# `gitsom-other-tool`. Rationale per prefix:
#
#   git  -- git commit/diff/log/status during the review pipeline.
#   bash scripts/  -- bash scripts/build.sh / bash scripts/test.sh /
#                     bash scripts/lint.sh / bash scripts/codex-dispatch.sh.
#                     The slash anchor prevents `bash /tmp/random.sh`
#                     from matching.
#   node  -- node "$HOME/.../codex-companion.mjs" adversarial-review
#            (legacy direct shape; the section-28 wrapper goes through
#            `bash scripts/codex-dispatch.sh` which matches above).
#   python3  -- python3 - <<HEREDOC for inline scripted helpers (test
#               fixtures, validators, JSON munging).
#   grep  / rg  -- read-only code search during evidence-mapping.
#   awk  / sed  -- read-only text processing during evidence-mapping.
#                  sed is read-only here because all in-place edits go
#                  through Edit/Write tools; the gate wraps Bash, where
#                  sed without `-i` is read-only.
#   wc  / head  / tail  -- read-only viewers during evidence-mapping.
#   ls  / cat  / find  -- read-only navigation during evidence-mapping.
#   cd  -- change-directory inside a single-shell tool invocation.
REVIEW_PIPELINE_PREFIXES = frozenset({
    "git ",
    "bash scripts/",
    "node ",
    "python3 ",
    "grep ",
    "awk ",
    "sed ",
    "wc ",
    "head ",
    "tail ",
    "ls ",
    "cat ",
    "find ",
    "rg ",
    "cd ",
})


def is_review_pipeline_passthrough(cmd: str) -> bool:
    """Return True if `cmd` is a review-pipeline-tooling Bash command
    that gates with the review-pipeline-passthrough policy should let
    through without further evaluation.

    The check is FIRST-TOKEN literal-prefix only. Chained commands
    (`cmd1 && cmd2`) are NOT decomposed here -- the policy is "is the
    first thing this Bash command does a review-pipeline operation?".
    Gates that need chained-command bypass detection (e.g. blocking
    `git status && rm -rf /`) must layer that detection separately
    (see `codex_review_completed.py` `_segment_by_separators`).

    DO NOT consume this from gates outside the review-pipeline policy.
    A future write-sensitive gate that auto-allows these prefixes is
    a security regression; define your own policy-scoped helper
    instead.
    """
    if not isinstance(cmd, str):
        return False
    cmd_stripped = cmd.lstrip()
    for prefix in REVIEW_PIPELINE_PREFIXES:
        if cmd_stripped.startswith(prefix):
            return True
    return False
