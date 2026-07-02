#!/usr/bin/env python3
# block-via: exit 2 (subagent Bash hazard commands only; main session untouched)
"""PreToolUse(Bash): runtime backstop for the runner agent class.

The runner agents (checks-runner, git-historian, gh-query-runner) carry Bash
for named idempotent commands; their prompts forbid mutations and Codex. This
hook makes the highest-risk prohibitions ENFORCED rather than prose: when the
Bash call originates from a SUBAGENT context (the hook payload's
transcript_path is an agent transcript, `agent-*.jsonl`, not the main-session
transcript), it BLOCKs:

  - any Codex surface (codex CLI, scripts/codex-*.sh wrappers,
    codex-companion.mjs) -- a subagent-issued dispatch would corrupt
    review-receipt state;
  - mutating git (verb-aware, walking git global options like -C/-c/--git-dir
    first, so `git -C /path commit` cannot slip past) -- the main session is
    the sole committer;
  - mutating gh (noun/verb-aware after gh global flags like -R, plus any
    -X*/--method/-f/-F/--field/--input form on gh api);
  - shell indirection from a subagent: `bash -c`/`sh -c` bodies are recursed
    into; `eval`/`source` are blocked outright;
  - SKIP_*/override env assignments -- gate escapes are main-session
    decisions.

Detection is shlex-tokenized + control-operator segmented (reusing the
_codex_dispatch machinery), NOT raw regex -- raw scanning was bypassed by
`git -C`, `bash -lc "codex ..."`, `gh -R ... pr comment`, and `-XPOST`
(re-adversarial 2026-07-02). If a subagent command cannot be tokenized AND
mentions a hazard word, it is blocked (a runner has no business running
unparseable shell); otherwise unparseable commands pass. Main-session Bash
calls are untouched. Fail-open on any unexpected error: a broken guard must
never block real work. The static side (roster-gated tools allowlist) is
lint.sh Check 14.
"""
from __future__ import annotations

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _codex_dispatch as _cd  # noqa: E402

_MAX_DEPTH = 5

_SHELLS = {"bash", "sh", "zsh", "dash", "ksh"}
_CODEX_BASENAMES = {
    "codex",
    "codex-dispatch.sh",
    "codex-bg-dispatch.sh",
    "codex-dispatch-with-files.sh",
    "codex-companion.mjs",
}
_GIT_MUTATING = {
    "add", "commit", "push", "pull", "fetch", "reset", "checkout", "switch",
    "restore", "stash", "rebase", "merge", "tag", "cherry-pick", "revert",
    "clean", "config", "remote", "mv", "rm", "am", "apply", "bisect",
    "worktree", "submodule", "notes", "replace", "filter-branch", "gc",
    "prune", "reflog", "update-ref", "symbolic-ref", "init", "clone",
}
# git global options that take a separate value token
_GIT_VALUE_OPTS = {"-C", "-c", "--git-dir", "--work-tree", "--namespace", "--exec-path"}
_GIT_BRANCH_LIST_OK = {"--list", "-a", "-r", "-v", "-vv", "--all", "--merged",
                       "--no-merged", "--contains", "--points-at", "--sort",
                       "--format", "--show-current"}
_GH_VALUE_FLAGS = {"-R", "--repo", "--hostname"}
_GH_MUTATING_VERBS = {
    "create", "comment", "merge", "close", "reopen", "edit", "delete",
    "upload", "run", "rerun", "cancel", "enable", "disable", "set", "sync",
    "fork", "clone", "transfer", "archive", "rename", "lock", "unlock",
    "review", "ready", "checkout", "apply", "dispatch",
}
_GH_BLOCKED_NOUNS = {"secret", "variable", "codespace", "ssh-key", "gpg-key"}
_HAZARD_WORDS = ("codex", "git ", "gh ", "SKIP_")


def _is_subagent_transcript(path: str) -> bool:
    if not path:
        return False
    base = os.path.basename(path)
    return base.startswith("agent-") or "/subagents/" in path


def _base(tok: str) -> str:
    return os.path.basename(tok.rstrip("/"))


def _git_verdict(seg: list[str]) -> str:
    """seg[0] is git. Walk global options, judge the subcommand."""
    i = 1
    while i < len(seg):
        tok = seg[i]
        if tok in _GIT_VALUE_OPTS:
            i += 2
            continue
        if tok.startswith("--") and "=" in tok:
            i += 1
            continue
        if tok.startswith("-"):
            i += 1
            continue
        # first non-option token = subcommand
        if tok == "branch":
            rest = seg[i + 1:]
            listing = all(
                (r.startswith("-") and r.split("=", 1)[0] in _GIT_BRANCH_LIST_OK)
                or ("--list" in rest and not r.startswith("-"))
                for r in rest
            )
            return "" if listing and ("--list" in rest or all(r.startswith("-") for r in rest)) else "mutating git (branch)"
        if tok in _GIT_MUTATING:
            return f"mutating git ({tok})"
        return ""
    return ""


def _gh_verdict(seg: list[str]) -> str:
    """seg[0] is gh. Any explicit method/field form or mutating noun+verb."""
    for tok in seg[1:]:
        if tok.startswith("-X") or tok == "--method" or tok.startswith("--method="):
            return "mutating gh api method"
        if tok in ("-f", "-F", "--field", "--raw-field", "--input") or \
                tok.startswith(("--field=", "--raw-field=", "--input=")):
            return "mutating gh api fields"
    words = []
    i = 1
    while i < len(seg) and len(words) < 2:
        tok = seg[i]
        if tok in _GH_VALUE_FLAGS:
            i += 2
            continue
        if tok.startswith("-"):
            i += 1
            continue
        words.append(tok)
        i += 1
    noun = words[0] if words else ""
    verb = words[1] if len(words) > 1 else ""
    if noun in _GH_BLOCKED_NOUNS:
        return f"gh {noun} (mutating surface)"
    if noun == "auth" and verb != "status":
        return "gh auth mutation"
    if verb in _GH_MUTATING_VERBS:
        return f"mutating gh ({noun} {verb})"
    return ""


def _scan_command(cmd: str, depth: int = 0) -> str:
    """Return a non-empty hazard label if cmd must be blocked."""
    if depth > _MAX_DEPTH:
        return "indirection depth"
    try:
        tokens = _cd._tokenize(cmd)
    except Exception:
        tokens = None
    if tokens is None:
        low = cmd
        if any(w in low for w in _HAZARD_WORDS):
            return "unparseable command mentioning a guarded surface"
        return ""
    for seg in _cd._segment_by_separators(tokens):
        # SKIP_ overrides in the env prefix
        for tok in seg:
            if re.match(r"^SKIP_[A-Z_]+=", tok):
                return "gate-skip override"
            if _base(tok.split("=", 1)[-1] if "=" in tok else tok) in _CODEX_BASENAMES:
                return "codex surface"
        seg = _cd._strip_env_and_wrappers(list(seg))
        if not seg:
            continue
        head = _base(seg[0])
        if head in ("eval", "source", "."):
            return "shell indirection (eval/source)"
        if head == "xargs":
            label = _scan_command(" ".join(seg[1:]), depth + 1)
            if label:
                return label
            continue
        if head in _SHELLS:
            # recurse into every -c style body; flag-only args before it
            for j, tok in enumerate(seg[1:], start=1):
                if tok.startswith("-") and "c" in tok.lstrip("-") and j + 1 <= len(seg) - 1:
                    label = _scan_command(seg[j + 1], depth + 1)
                    if label:
                        return label
                    break
            continue
        if head == "codex":
            return "codex CLI"
        if head == "git":
            label = _git_verdict(seg)
            if label:
                return label
            continue
        if head == "gh":
            label = _gh_verdict(seg)
            if label:
                return label
            continue
    return ""


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
    label = _scan_command(cmd)
    if label:
        sys.stderr.write(
            f"[runner-bash-guard BLOCK] subagent Bash call matches a forbidden "
            f"surface ({label}). Runner agents execute named verification/query "
            f"commands only; edits, git/gh mutations, gate skips, shell "
            f"indirection, and ALL Codex dispatch belong to the main session. "
            f"Return your findings as text instead.\n"
        )
        return 2
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        sys.exit(0)  # fail-open: a broken guard must never block real work
