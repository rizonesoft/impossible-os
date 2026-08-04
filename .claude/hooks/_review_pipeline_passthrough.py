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

import re
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
#   mkdir / chmod  -- non-destructive filesystem ops occasionally
#       needed during evidence-mapping (creating fixture dirs, fixing
#       perms on a script). `rm`, `mv`, `cp` are deliberately NOT in
#       this list -- they are destructive and the existing rejection
#       test (rpp_destructive_rejected) guards against laundering
#       them through the review-pipeline allowlist. Use SKIP_REVIEW_HOOK
#       for the rare review-pipeline call that genuinely needs `rm`.
#   tee / echo / printf -- emitting stamps / receipts / generated text
#       (e.g. `echo '{...}' > .claude/state/...`). These can clobber
#       files via shell redirection but are not in themselves
#       destructive command classes.
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
    "mkdir ",
    "chmod ",
    "tee ",
    "echo ",
    "printf ",
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
    cmd_stripped = _skip_leading_assignments(cmd.lstrip())
    if cmd_stripped is None:
        return False
    for prefix in REVIEW_PIPELINE_PREFIXES:
        if cmd_stripped.startswith(prefix):
            return True
    return False


# `NAME=` ... up to the unquoted whitespace that ends the assignment's VALUE.
_ASSIGN_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")


def _skip_leading_assignments(cmd: str):
    """Step over leading `NAME=VALUE` assignments, or None if one may execute.

    The allowlist tested the LEADING TOKEN of the raw string, so a command whose
    every part is allowlisted was still refused for beginning with a variable
    binding: `L="$(ls -t .claude/overnight/artifacts/*.log | head -1)"; grep -n
    ... "$L"` (v08, three refusals in one TODO-04 section-60 review). The gate is
    correct; only its tokenizer was too literal, and the effect was to push the
    review pipeline into less readable one-liners that repeat a glob rather than
    bind it once -- precisely when every call is on the critical path.

    An assignment does not change WHICH command runs, so stepping over it is
    faithful -- with one exception that would otherwise open a real hole: the
    VALUE can contain a command substitution, and that substitution RUNS.
    `L="$(rm -rf /)"; grep x` must not become "starts with grep". So any
    substitution inside a skipped value is itself required to be passthrough,
    and anything unparseable returns None (refuse, do not guess).
    """
    while True:
        m = _ASSIGN_RE.match(cmd)
        if not m:
            return cmd
        rest = cmd[m.end():]
        try:
            value, consumed = _read_word(rest)
        except ValueError:
            return None
        for body in _substitution_bodies(value):
            if not is_review_pipeline_passthrough(body.strip()):
                return None
        cmd = rest[consumed:].lstrip()
        # A STANDALONE assignment (`L=...; grep ...`) is its own command, so the
        # separator has to be stepped over too or the next head reads as `;`.
        # This decomposes exactly one level and the new head is still tested, so
        # `L=x; rm -rf /` stays blocked -- see the selftest.
        m2 = re.match(r"^(?:;|&&|\|\||&)\s*", cmd)
        if m2:
            cmd = cmd[m2.end():]


def _read_word(s: str):
    """The first shell WORD of `s` and how many chars it spans. Quote-aware."""
    quote, i, out = None, 0, []
    while i < len(s):
        ch = s[i]
        if quote:
            out.append(ch)
            if ch == quote:
                quote = None
            elif ch == "\\" and quote == '"' and i + 1 < len(s):
                i += 1
                out.append(s[i])
            i += 1
            continue
        if ch in "'\"":
            quote = ch
            out.append(ch)
            i += 1
            continue
        # A substitution is one WORD even though it contains spaces, so it has
        # to be spanned rather than split on -- otherwise `L=`ls -t x`` stops at
        # the first space and its body never reaches the passthrough check.
        if ch == "`":
            j = s.find("`", i + 1)
            if j < 0:
                raise ValueError("unterminated backtick")
            out.append(s[i:j + 1])
            i = j + 1
            continue
        if s.startswith("$(", i):
            depth, j = 1, i + 2
            while j < len(s) and depth:
                if s[j] == "(":
                    depth += 1
                elif s[j] == ")":
                    depth -= 1
                j += 1
            if depth:
                raise ValueError("unterminated substitution")
            out.append(s[i:j])
            i = j
            continue
        if ch == "\\" and i + 1 < len(s):
            out.append(s[i + 1])
            i += 2
            continue
        if ch.isspace() or ch in ";&|":
            break
        out.append(ch)
        i += 1
    if quote:
        raise ValueError("unterminated quote")
    return "".join(out), i


def _substitution_bodies(s: str):
    """Bodies of `$(...)` and `` `...` `` in a value, innermost included."""
    out, n, i = [], len(s), 0
    while i < n:
        if s[i] == "\\":
            i += 2
            continue
        if s[i] == "`":
            j = s.find("`", i + 1)
            if j < 0:
                break
            out.append(s[i + 1:j])
            i = j + 1
            continue
        if s.startswith("$(", i):
            depth, j = 1, i + 2
            while j < n and depth:
                if s[j] == "(":
                    depth += 1
                elif s[j] == ")":
                    depth -= 1
                j += 1
            out.append(s[i + 2:j - 1])
            out.extend(_substitution_bodies(s[i + 2:j - 1]))
            i = j
            continue
        i += 1
    return out
