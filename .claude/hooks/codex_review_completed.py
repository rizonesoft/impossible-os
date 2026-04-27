#!/usr/bin/env python3
"""Track Codex review trigger / receive state for the hard gate.

PostToolUse hook on `Bash` and `Skill`. Writes / updates
`.claude/state/last-codex-review.json` so the companion PreToolUse
hook (`receiving_review_required.py`) knows whether the most recent
external Codex review has been processed through
`Skill(superpowers:receiving-code-review)` before the agent starts
editing code.

Two events:

  1. **Trigger** -- a Codex review just ran.
     Writes state with `received: false`, captures HEAD sha and a
     working-tree hash, records the trigger source (Bash command or
     Skill name) so the BLOCK message can name it later.

  2. **Receive** -- the receiving-code-review skill just completed.
     Reads the current state, flips `received: true`, sets
     `received_timestamp_ns`. If no state exists (no prior trigger),
     no-op -- the receive skill is harmless to invoke standalone.

Trigger detection (Bash side -- shlex tokenized, argv[0]/argv[1]):

  * `argv[0]` basename or path contains `codex-companion.mjs` AND the
    next positional is a real review subcommand (`review`,
    `adversarial-review`, `task`) -- NOT a metadata/control subcommand
    (`status`, `cancel`, `task-worker`, `setup`), OR
  * `argv[0]` is `node` AND `argv[1]` contains `codex-companion.mjs`
    AND argv[2] is a real review subcommand (same filter), OR
  * `argv[0]` basename is `codex` AND `argv[1]` is `review` or `e`
    (the bare CLI / exec alias for review).

  Substring scan over the full command line was rejected after Codex
  adversarial review caught false positives from `rg codex-companion.mjs`,
  `git grep "codex review"`, and heredoc bodies containing the literal.

  Subcommand filter added 2026-04-27 after a session-lifecycle-hook
  `codex-companion.mjs status` call falsely registered as a review
  trigger and blocked subsequent edits. Real review subcommands write
  output that needs receiving-code-review processing; metadata/control
  subcommands do not.

Trigger detection (Skill side):

  * `Skill` matcher with `tool_input.skill` (or `tool_input.name`,
    per the existing tolerant pattern in `section_review_required.py`)
    in the codex-* skill set.

Fail-open on any error (malformed JSON, git not on PATH, write
fails) -- this hook is observability + the receive gate; a hook
crash should never block tool calls.

Owner: TODO-08-automation-hardening section 3.
"""

import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

# Skill names that count as Codex review triggers. Both the
# bare-name and the marketplace-prefixed spellings are accepted
# because the harness sometimes resolves one form to the other.
CODEX_TRIGGER_SKILLS = frozenset({
    "codex-adversarial-review-section",
    "codex-review-todo",
    "codex-design-review",
    "codex-impact-analysis",
    "codex-test-coverage",
    "codex-consistency-audit",
    "codex-perf-review",
    "codex-fix-review",
})

# Receive-skill names. Match both the namespace-prefixed form and
# the bare name -- the marketplaces plugin sometimes resolves the
# bare name (verified pattern in section_review_required.py and
# design_review_required.py).
RECEIVE_SKILL_NAMES = frozenset({
    "superpowers:receiving-code-review",
    "receiving-code-review",
})


def _repo_root() -> Path | None:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return None
    return Path(out) if out else None


def _state_path(root: Path) -> Path:
    return root / ".claude" / "state" / "last-codex-review.json"


def _head_sha(root: Path) -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=str(root),
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return ""


def _tree_hash(root: Path) -> str:
    try:
        out = subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=str(root),
            text=True, timeout=3, stderr=subprocess.DEVNULL,
        )
    except Exception:
        return ""
    return hashlib.sha256(out.encode("utf-8")).hexdigest()


# Source-code path filter shared with section_commit_gate.py. Kept
# here as the canonical definition; the gate imports this module's
# constant for parity. Mirrors the §4 spec: source = .c/.h/.asm/.S
# under src|include OR .py/.mjs/.sh under scripts.
_SOURCE_KERNEL_EXTS = (".c", ".h", ".asm", ".S")
_SOURCE_KERNEL_PREFIXES = ("src/", "include/")
_SOURCE_TOOLING_EXTS = (".py", ".mjs", ".sh")
_SOURCE_TOOLING_PREFIXES = ("scripts/",)


def _is_source_path(path: str) -> bool:
    """True if `path` is a section-commit-relevant source file."""
    p = path.replace("\\", "/")
    if any(p.startswith(pre) for pre in _SOURCE_KERNEL_PREFIXES) and \
            p.endswith(_SOURCE_KERNEL_EXTS):
        return True
    if any(p.startswith(pre) for pre in _SOURCE_TOOLING_PREFIXES) and \
            p.endswith(_SOURCE_TOOLING_EXTS):
        return True
    return False


def _staged_source_files(root: Path) -> list[str]:
    """Return the source-code paths currently in the git index.
    Empty list on git error; the section-commit gate treats empty
    trigger_files as 'covers nothing' so a transient git failure
    cannot let an unbound review satisfy a later commit gate.
    """
    try:
        out = subprocess.check_output(
            ["git", "diff", "--cached", "--name-only", "-z"],
            cwd=str(root), text=True, timeout=3, stderr=subprocess.DEVNULL,
        )
    except Exception:
        return []
    paths = [p for p in out.split("\x00") if p]
    return [p for p in paths if _is_source_path(p)]


def _staged_source_blobs(root: Path, paths: list[str]) -> dict:
    """Return a {path: blob_sha} map for the given staged source
    paths, captured from `git ls-files -s` (mode/sha/stage/path
    output). Used at trigger time for content-binding evidence
    (Codex C1: path-only binding lets post-review same-path edits
    sneak past). Empty dict on git error; the gate treats missing
    or partial blob coverage as evidence missing.
    """
    if not paths:
        return {}
    blobs: dict = {}
    try:
        # `git ls-files -s -z -- <paths>` yields:
        #   <mode> SP <sha> SP <stage> TAB <path> NUL
        out = subprocess.check_output(
            ["git", "ls-files", "-s", "-z", "--", *paths],
            cwd=str(root), text=True, timeout=5, stderr=subprocess.DEVNULL,
        )
    except Exception:
        return {}
    for entry in out.split("\x00"):
        if not entry:
            continue
        # Split header (mode sha stage) and path on TAB.
        if "\t" not in entry:
            continue
        header, path = entry.split("\t", 1)
        parts = header.split()
        if len(parts) != 3:
            continue
        _mode, sha, _stage = parts
        blobs[path] = sha
    return blobs


# Bash control operators that introduce a NEW command sequence.
# `true && codex review prompt` was a bypass: the original code
# treated the whole command as one argv with `true` at position 0.
# Mirrors the section-2 codex_model_flag_block.py pattern.
_COMMAND_SEPARATORS = ("&&", "||", ";", "|", "&")

# Tokens that are env-prefix assignments (FOO=bar) or wrapper commands
# we walk past when looking for the "real" Codex invocation. Includes
# taskset / chrt / setsid / setpriv / cgexec / flock for parity with
# the section-2 hook (review-pipeline adversarial H4 caught divergence).
_CODEX_WRAPPER_TOKENS = frozenset({
    "sudo", "doas", "env", "nice", "nohup", "timeout", "ionice",
    "stdbuf", "unbuffer", "chronic", "exec", "command",
    "taskset", "chrt", "setsid", "setpriv", "cgexec", "flock",
})


def _trim_heredoc_body(tokens: list[str]) -> list[str]:
    """Return tokens up to (but not including) the first construct
    that introduces SHELL SUB-CONTENT (heredoc body, process
    substitution, command substitution body). Tokens after the
    opener are NOT part of the real command line. Same shape as
    section-2 codex_model_flag_block.py _trim_heredoc_body.
    """
    for i, tok in enumerate(tokens):
        if tok in ("<<", "<<-") or tok.startswith("<<"):
            return tokens[:i]
        if tok.startswith("<(") or tok.startswith(">("):
            return tokens[:i]
        if tok.startswith("$(") or tok.startswith("${"):
            return tokens[:i]
        if tok.startswith("`"):
            return tokens[:i]
    return tokens


def _segment_by_separators(tokens: list[str]) -> list[list[str]]:
    """Split tokens by Bash control operators into a list of simple
    commands. `true && codex review prompt` -> [['true'], ['codex',
    'review', 'prompt']]. Each segment is its own argv to scan.
    """
    segs: list[list[str]] = []
    cur: list[str] = []
    for tok in tokens:
        if tok in _COMMAND_SEPARATORS:
            if cur:
                segs.append(cur)
            cur = []
        else:
            cur.append(tok)
    if cur:
        segs.append(cur)
    return segs


def _segment_is_codex_invocation(seg_tokens: list[str]) -> bool:
    """Test whether a SINGLE command segment (already separated from
    operator chains and stripped of heredoc body) is a Codex review
    invocation. Strips env-prefix + wrapper-prefix (with proper
    flag/value handling), then inspects argv[0]/argv[1].

    Forms accepted:
      - argv[0] basename contains `codex-companion.mjs`
      - argv[0] == `node` AND argv[1] contains `codex-companion.mjs`
      - argv[0] basename is `codex` AND, after walking past Codex
        global options, the real subcommand is `review` or `e`
    """
    out = list(seg_tokens)
    # Strip leading env-assignments.
    while out and "=" in out[0] and not out[0].startswith("="):
        head = out[0].split("=", 1)[0]
        if head and (head[0].isalpha() or head[0] == "_") and all(
            c.isalnum() or c == "_" for c in head
        ):
            out.pop(0)
        else:
            break
    if not out:
        return False
    # Per-wrapper value-flag table. Used to walk past `--flag value`
    # cleanly; `--flag=value` attached form is handled by detecting
    # `=` in the flag token.
    # §4 round-4/round-5 corrections: drop NO-VALUE flags so they
    # don't pop the next positional (which would be the real command).
    # `sudo -n` (--non-interactive), `command -p` (default-PATH),
    # `ionice -t` (ignore-failure), `cgexec --sticky` are all no-value.
    # Setpriv expanded to current util-linux 2.39+ names; flock's
    # `-c/--command` is shell-descent (handled separately in §4 gate
    # but kept as a value flag here since this hook only needs to
    # detect Codex invocations -- it doesn't recurse into shell
    # strings, so treating -c as opaque value-flag is safe).
    _wrapper_value_flags = {
        "sudo": frozenset({"-u", "-g", "-p", "-r", "-h", "-D", "-C"}),
        "doas": frozenset({"-u", "-C"}),
        "timeout": frozenset({"-k", "--kill-after", "-s", "--signal"}),
        "nice": frozenset({"-n", "--adjustment"}),
        "ionice": frozenset({"-c", "--class", "-n", "--classdata", "-p", "--pid", "-P", "-u"}),
        "env": frozenset({"-u", "--unset", "-S", "--split-string", "-C", "--chdir"}),
        "stdbuf": frozenset({"-i", "-o", "-e"}),
        "command": frozenset(),
        "taskset": frozenset({"-c", "--cpu-list", "-p", "--pid"}),
        "chrt": frozenset({"-p", "--pid"}),
        "setpriv": frozenset({"--reuid", "--regid",
                              "--ruid", "--euid", "--rgid", "--egid",
                              "--groups", "--inh-caps", "--ambient-caps",
                              "--bounding-set", "--securebits",
                              "--pdeathsig", "--selinux-label",
                              "--apparmor-profile"}),
        "cgexec": frozenset({"-g"}),
        "flock": frozenset({"-w", "--timeout", "-E", "--conflict-exit-code", "-c", "--command"}),
    }

    def _is_duration(tok: str) -> bool:
        if not tok:
            return False
        if tok[0].isdigit():
            if tok[-1] in "smhd" and len(tok) > 1:
                return tok[:-1].replace(".", "", 1).isdigit()
            return tok.replace(".", "", 1).isdigit()
        return False

    # Walk wrappers. `taskset -c 0 codex review prompt` works:
    # taskset popped, `-c 0` consumed (flag with value), then codex
    # is the head.
    while out and out[0] in _CODEX_WRAPPER_TOKENS:
        wrapper = out.pop(0)
        value_flags = _wrapper_value_flags.get(wrapper, frozenset())
        while out and out[0].startswith("-"):
            flag = out.pop(0)
            if "=" in flag:
                continue
            if flag in value_flags and out:
                out.pop(0)
        # Wrapper-specific positional args before the real command.
        if wrapper == "timeout" and out and _is_duration(out[0]):
            out.pop(0)
        elif wrapper == "env":
            while out and "=" in out[0] and not out[0].startswith("="):
                head_a = out[0].split("=", 1)[0]
                if head_a and (head_a[0].isalpha() or head_a[0] == "_") and all(
                    c.isalnum() or c == "_" for c in head_a
                ):
                    out.pop(0)
                else:
                    break
        elif wrapper == "nice" and out and out[0].lstrip("-").isdigit():
            out.pop(0)
        elif wrapper == "chrt" and out and out[0].isdigit():
            # `chrt -f 10 cmd` -- after -f flag, 10 is the priority.
            out.pop(0)

    if not out:
        return False
    head = out[0]
    second = out[1] if len(out) > 1 else ""
    third = out[2] if len(out) > 2 else ""
    head_base = head.rsplit("/", 1)[-1]

    # Real review subcommands. Anything else (status, cancel,
    # task-worker, setup) is metadata/control and must NOT register
    # as a review trigger -- that was the false-positive that locked
    # edits after a session-lifecycle status call (2026-04-27).
    _COMPANION_REVIEW_SUBCOMMANDS = frozenset({
        "review", "adversarial-review", "task",
    })

    def _companion_subcmd_is_review(subcmd: str) -> bool:
        # Strip any leading flags (e.g. accidental `--` before subcmd).
        return subcmd in _COMPANION_REVIEW_SUBCOMMANDS

    if "codex-companion.mjs" in head_base:
        # Direct: <path>/codex-companion.mjs <subcmd> ...
        return _companion_subcmd_is_review(second)
    if head_base == "node" and "codex-companion.mjs" in second:
        # Wrapped: node <path>/codex-companion.mjs <subcmd> ...
        return _companion_subcmd_is_review(third)

    if head_base == "codex":
        # Walk past Codex global options to find the real subcommand.
        # H8 fix: include the FULL value-flag set from section-2's
        # codex_model_flag_block.py CODEX_GLOBAL_VALUE_FLAGS so
        # `codex --enable feature review` etc. correctly identify
        # `review` as the subcommand (pre-fix: `feature` was treated
        # as the subcommand and the gate missed the trigger).
        codex_global_value_flags = (
            "-c", "--config", "--enable", "--disable", "--remote",
            "--bearer-token-env-var",
        )
        idx = 1
        while idx < len(out):
            tok = out[idx]
            if not tok.startswith("-"):
                break
            if tok in codex_global_value_flags:
                # Two-token form: `--flag value`; skip the value.
                # Attached form `--flag=value` is detected via "="
                # in the flag and skipped without a separate value pop.
                if "=" in tok:
                    idx += 1
                else:
                    idx += 2
                continue
            idx += 1
        subcmd = out[idx] if idx < len(out) else ""
        if subcmd in ("review", "e"):
            return True
    return False


def _is_codex_bash_trigger(cmd: str) -> bool:
    """Detect a real Codex invocation in a Bash command.

    Codex post-commit perf H1 (review-pipeline post-impl): the hook
    fires on EVERY Bash PostToolUse call. shlex.split() on a 200KB
    Bash command costs ~600ms; non-Codex commands paid that cost
    needlessly. The literal screen below short-circuits the common
    case (>99% of Bash calls don't mention Codex) before any
    expensive parsing. False-positive rate: any command that happens
    to contain the substring `codex` anywhere proceeds to full
    tokenization -- which then correctly classifies it as not a real
    trigger via argv[0]/argv[1] inspection. The cheap screen mirrors
    section_commit_gate.py's `_looks_like_git_commit_screen` pattern.

    Tokenize via shlex, trim heredoc / sub-content, segment by Bash
    control operators (&&, ||, ;, |, &), then scan EACH segment for
    a Codex invocation.
    """
    if not isinstance(cmd, str) or not cmd.strip():
        return False
    if "codex" not in cmd:
        return False
    try:
        toks = shlex.split(cmd, posix=True, comments=False)
    except ValueError:
        return False
    if not toks:
        return False
    # H7 fix: trim heredoc / substitution INSIDE the segment loop
    # (per-segment) instead of globally. Pre-fix: `echo $(date) &&
    # codex review prompt` was trimmed to `[echo]` BEFORE the
    # segmenter ran -- the later codex review segment was lost.
    # Mirrors the section-2 codex_model_flag_block.py per-segment
    # trim ordering.
    for seg in _segment_by_separators(toks):
        seg = _trim_heredoc_body(seg)
        if seg and _segment_is_codex_invocation(seg):
            return True
    return False


# ----------------------------------------------------------------------
# Four-dispatch policy state (§5 last-review-stamps.json)
# ----------------------------------------------------------------------
#
# The §5 gate enforces that all three review-todo-section step-8
# dispatches (adversarial / consistency / perf) ran within 30 min of
# any commit that adds a `**Verified:**` or `**Quality reviewed:**`
# stamp to a TODO file. Dead-code dispatch was retired in
# review-todo-section/SKILL.md on 2026-04-25; this state file tracks
# the three kinds that survived.

# Skill -> dispatch kind. Only the three step-8 dispatches map here;
# codex-design-review / codex-fix-review / codex-impact-analysis /
# codex-test-coverage / codex-review-todo are valid Codex triggers
# (still recorded in last-codex-review.json) but do not satisfy the
# four-dispatch gate.
DISPATCH_KIND_BY_SKILL = {
    "codex-adversarial-review-section": "adversarial",
    "codex-consistency-audit": "consistency",
    "codex-perf-review": "perf",
}

# Bash-side: the codex-companion.mjs invocation is identical across
# the three dispatches; only the prompt content differs. SKILL.md
# step 8 + codex-prompt-template.md require a leading
# `[review-kind: <kind>]` marker at the start of the prompt. The
# regex tolerates whitespace and different bracket spellings.
_REVIEW_KIND_RE = re.compile(
    r"\[\s*review[-_ ]kind\s*:\s*(adversarial|consistency|perf)\s*\]",
    re.IGNORECASE,
)

# TODO path locator: matches `todo/<domain>/TODO-XX-<slug>.md`. First
# match wins; multiple matches surface a stderr WARN. The regex is
# anchored on the literal `todo/` prefix so it does not match
# `~/.claude/projects/.../todo/...` or other unrelated paths.
_TODO_PATH_RE = re.compile(r"(?<![\w/-])todo/[\w./-]+TODO-\d[\w./-]*\.md")

# Section number locator: `§5`, `§ 5`, `section 5`. Captured for the
# `section` field in the state entry; not used by the gate.
_SECTION_RE = re.compile(r"(?:§\s*|section\s+)(\d+)", re.IGNORECASE)

# Self-summary preambles that bias the reviewer toward agreement
# (CONSENSAGENT ACL-2025). Detected on the first ~10 non-empty lines
# of the prompt. Emit a stderr WARN; do not block (false positives
# are too easy to hit on legitimate context lines).
_SELF_SUMMARY_PATTERNS = [
    re.compile(r"^\s*I\s+(built|wrote|implemented|made|added|created|fixed)\b", re.IGNORECASE),
    re.compile(r"^\s*(Summary\s+of\s+changes|What\s+I\s+built|My\s+(implementation|approach|fix|change))", re.IGNORECASE),
    re.compile(r"^\s*Here\s+(is|'s)\s+what\s+I\s+", re.IGNORECASE),
]


def _bash_prompt_arg(cmd: str) -> str:
    """Return the prompt argument from a Codex bash invocation.

    Codex companion shapes:
      node /abs/codex-companion.mjs adversarial-review "<prompt>"
      codex review "<prompt>"
      codex e "<prompt>"

    The prompt is the FIRST positional immediately after the review
    subcommand. Earlier (pre-fix) implementation reverse-walked the
    segment, which broke whenever the bash command appended shell
    redirects: `node ... adversarial-review "<prompt>" 2>&1 | tail -3`
    segments on `|` to `[node, ..., adversarial-review, <prompt>,
    2>&1]`, and reverse iteration picked `2>&1` instead of `<prompt>`.
    This was the root cause of the §5 PostToolUse routing failure
    (state file empty in real sessions; verified by trace 2026-04-27).
    Forward-walk from the subcommand is unambiguous because review
    takes ONE positional argument.

    Returns empty string when the invocation has no detectable
    prompt arg or when shlex.split fails.
    """
    if not isinstance(cmd, str) or not cmd:
        return ""
    if "codex" not in cmd:
        return ""
    try:
        toks = shlex.split(cmd, posix=True, comments=False)
    except ValueError:
        return ""
    _COMPANION_SUBCMDS = ("review", "adversarial-review", "task")
    _CODEX_BARE_SUBCMDS = ("review", "e")

    def _first_positional_after(seg: list[str], start: int) -> str:
        """Walk past flag tokens (`--background`, `--json`,
        `--key=value`, etc.) starting at `start` and return the first
        non-flag token. Returns '' if none. Codex companion review
        subcommands take a SINGLE positional (the prompt) after any
        per-subcommand flags such as `task --background --json`."""
        idx = start
        while idx < len(seg) and seg[idx].startswith("-"):
            idx += 1
        return seg[idx] if idx < len(seg) and not seg[idx].startswith("-") else ""

    segs = _segment_by_separators(toks)
    for seg in segs:
        seg = _trim_heredoc_body(seg)
        if not (seg and _segment_is_codex_invocation(seg)):
            continue
        for i, tok in enumerate(seg):
            base = tok.rsplit("/", 1)[-1]
            # Direct: `<path>/codex-companion.mjs <subcmd> [flags...] <prompt>`
            if "codex-companion.mjs" in base:
                if i + 1 < len(seg) and seg[i + 1] in _COMPANION_SUBCMDS:
                    return _first_positional_after(seg, i + 2)
                return ""
            # `node <path>/codex-companion.mjs <subcmd> [flags...] <prompt>`
            if base == "node" and i + 1 < len(seg) and "codex-companion.mjs" in seg[i + 1]:
                if i + 2 < len(seg) and seg[i + 2] in _COMPANION_SUBCMDS:
                    return _first_positional_after(seg, i + 3)
                return ""
            # `codex [global-flags] <subcmd> <prompt>`
            if base == "codex":
                codex_global_value_flags = (
                    "-c", "--config", "--enable", "--disable", "--remote",
                    "--bearer-token-env-var",
                )
                idx = i + 1
                while idx < len(seg):
                    tok2 = seg[idx]
                    if not tok2.startswith("-"):
                        break
                    if tok2 in codex_global_value_flags:
                        if "=" in tok2:
                            idx += 1
                        else:
                            idx += 2
                        continue
                    idx += 1
                if idx < len(seg) and seg[idx] in _CODEX_BARE_SUBCMDS:
                    return _first_positional_after(seg, idx + 1)
                return ""
        return ""
    return ""


def _detect_review_kind(skill_name: str, prompt: str) -> str:
    """Return one of `"adversarial" | "consistency" | "perf" | ""`.

    Skill name takes precedence over prompt marker because the skill
    invocation is unambiguous. For Bash triggers (no skill name) the
    marker MUST appear on the first non-blank line of the prompt --
    Codex review_post_impl_A: scanning the entire body let earlier
    quoted markers in repository text or example diffs spoof the
    attribution. Anchor to the leading line; reject if multiple
    distinct kinds appear there.
    """
    if skill_name in DISPATCH_KIND_BY_SKILL:
        return DISPATCH_KIND_BY_SKILL[skill_name]
    if not prompt:
        return ""
    first_line = ""
    for ln in prompt.splitlines():
        if ln.strip():
            first_line = ln
            break
    if not first_line:
        return ""
    matches = _REVIEW_KIND_RE.findall(first_line)
    if not matches:
        return ""
    kinds = {m.lower() for m in matches}
    if len(kinds) != 1:
        sys.stderr.write(
            f"[review-stamps] WARN: leading prompt line carries multiple "
            f"review-kind markers {sorted(kinds)}; refusing to attribute "
            f"the dispatch.\n"
        )
        return ""
    return kinds.pop()


def _detect_todo_path(prompt: str, root: Path) -> tuple[str, str]:
    """Extract (todo_path, section) from the prompt. Returns ("", "")
    when no match. WARNs on stderr when multiple distinct TODO paths
    appear -- the agent's prompt is then ambiguous; first match wins
    so the dispatch still counts for SOMETHING and the gate exit-2
    catches the remaining gap.
    """
    if not prompt:
        return ("", "")
    matches = _TODO_PATH_RE.findall(prompt)
    if not matches:
        return ("", "")
    # Normalize: strip any trailing punctuation, dedup preserving order.
    seen: list[str] = []
    for m in matches:
        m_clean = m.rstrip(").,;:")
        if m_clean not in seen:
            seen.append(m_clean)
    chosen = seen[0]
    if len(seen) > 1:
        sys.stderr.write(
            f"[review-stamps] WARN: prompt names multiple TODO paths "
            f"{seen[:3]}; recording dispatch against {chosen!r} only. "
            f"Reviewer prompts should target one TODO file.\n"
        )
    # Validate the path actually resolves under the repo root. If not,
    # still record it (the agent may be referring to an under-development
    # TODO) but warn.
    abs_path = root / chosen
    if not abs_path.exists():
        sys.stderr.write(
            f"[review-stamps] WARN: prompt names TODO path {chosen!r} "
            f"that does not exist on disk. Recording anyway; the gate "
            f"will not match a non-existent path.\n"
        )
    section = ""
    sm = _SECTION_RE.search(prompt)
    if sm:
        section = f"§{sm.group(1)}"
    return (chosen, section)


def _scan_self_summary(prompt: str) -> str:
    """Return the first matched preamble line, or empty string."""
    if not prompt:
        return ""
    # Inspect the first 10 non-blank lines (the preamble window).
    lines = [ln for ln in prompt.splitlines() if ln.strip()][:10]
    for ln in lines:
        for pat in _SELF_SUMMARY_PATTERNS:
            if pat.search(ln):
                return ln.strip()[:200]
    return ""


def _stamps_path(root: Path) -> Path:
    return root / ".claude" / "state" / "last-review-stamps.json"


def _stamps_lock_path(root: Path) -> Path:
    return root / ".claude" / "state" / "last-review-stamps.lock"


def _record_stamp(
    root: Path,
    todo_path: str,
    section: str,
    kind: str,
    now_ns: int,
    dispatch_head_sha: str = "",
) -> None:
    """Update last-review-stamps.json[todo_path] with `kind` timestamp
    and per-kind `dispatch_head_sha`.

    Reads existing state; merges; atomic-writes. Fail-open on any
    error -- this is a tracking feature, never a block.

    Codex M3 (post-impl): the read-merge-write sequence is now
    serialized via fcntl.flock() on a sibling lockfile. Without the
    lock, parallel PostToolUse fires (e.g. `task --background`
    dispatches per `feedback_codex_background_fallback`) raced on
    the same JSON: both processes read the prior state, each merged
    only their own kind, last writer wins. That silently dropped one
    of the three required dispatches and surfaced as a missing-kind
    BLOCK at commit time.

    Codex M2 (post-impl): the per-kind `<kind>_head` field is the
    HEAD SHA at dispatch time. The §4 gate verifies it's an ancestor
    of the current HEAD before accepting the stamp; this prevents
    cross-branch reuse of dispatch evidence.
    """
    if not todo_path or kind not in ("adversarial", "consistency", "perf"):
        return
    path = _stamps_path(root)
    lock_path = _stamps_lock_path(root)
    path.parent.mkdir(parents=True, exist_ok=True)

    # fcntl is Unix-only. On platforms without it, fall back to the
    # legacy lock-free path (the WSL2 dev host always has fcntl, so
    # this is a safety net for portability, not a hot path).
    try:
        import fcntl
    except ImportError:
        fcntl = None  # type: ignore[assignment]

    lock_fd = None
    if fcntl is not None:
        try:
            # O_RDWR | O_CREAT lets us own the lock without truncating
            # any state another process is writing.
            lock_fd = os.open(
                str(lock_path),
                os.O_RDWR | os.O_CREAT,
                0o644,
            )
            # Codex post-commit review_D: bounded LOCK_NB with retry
            # rather than blocking LOCK_EX. A hung holder indefinitely
            # blocked every later PostToolUse stamp write before
            # eventually being SIGKILL'd by the harness's 5s hook
            # timeout (which loses the stamp anyway). Bounded retry
            # caps the wait at well under 5s and degrades gracefully
            # to lock-free with a stderr WARN if contention persists.
            acquired = False
            deadline = time.time() + 2.0  # 2s budget; harness timeout is 5s
            sleep_s = 0.01
            while time.time() < deadline:
                try:
                    fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    acquired = True
                    break
                except BlockingIOError:
                    time.sleep(sleep_s)
                    if sleep_s < 0.1:
                        sleep_s *= 2
            if not acquired:
                # Codex re-adversarial M1 (post-impl re-review): on
                # acquire-timeout we MUST NOT proceed unlocked --
                # writing the canonical JSON without serialization
                # races against the concurrent holder's eventual
                # write and silently drops one process's merge. Emit
                # a hard WARN, skip recording entirely. The gate's
                # missing-dispatch BLOCK will surface the gap on the
                # next stamp commit; the agent re-runs the dispatch.
                sys.stderr.write(
                    "[review-stamps] WARN: could not acquire "
                    "last-review-stamps.lock within 2s (likely a hung "
                    "concurrent dispatch). Refusing to write the stamp "
                    "unlocked -- a graceful-degradation write would race "
                    "with the eventual holder's write and lose data. "
                    "The dispatch is NOT recorded; the §4 commit gate "
                    "will block on the next stamp commit. Re-run this "
                    "dispatch after the contended hook finishes.\n"
                )
                try:
                    os.close(lock_fd)
                except Exception:
                    pass
                return
        except Exception:
            # Lock acquisition failed (filesystem error, etc). Proceed
            # without the lock -- the prior race window remains, but
            # losing the stamp is better than crashing the hook.
            if lock_fd is not None:
                try:
                    os.close(lock_fd)
                except Exception:
                    pass
                lock_fd = None

    try:
        state: dict = {}
        if path.exists():
            try:
                state = json.loads(path.read_text(encoding="utf-8"))
                if not isinstance(state, dict):
                    state = {}
            except Exception:
                state = {}
        entry = state.get(todo_path)
        if not isinstance(entry, dict):
            entry = {
                "section": section or "",
                "adversarial": None,
                "consistency": None,
                "perf": None,
            }
        # Update section if we have a fresh one (most recent dispatch
        # presumably names the section in flight; older entries may
        # have been section-less).
        if section:
            entry["section"] = section
        entry[kind] = now_ns
        if dispatch_head_sha:
            entry[f"{kind}_head"] = dispatch_head_sha
        # Preserve any unexpected keys but ensure the three canonical
        # ones exist so the gate's lookup is total.
        for k in ("adversarial", "consistency", "perf"):
            entry.setdefault(k, None)
        state[todo_path] = entry
        _write_atomic(path, state)
    finally:
        if lock_fd is not None and fcntl is not None:
            try:
                fcntl.flock(lock_fd, fcntl.LOCK_UN)
            except Exception:
                pass
            try:
                os.close(lock_fd)
            except Exception:
                pass


def _skill_name(tool_input: dict) -> str:
    """Return the invoked skill name. Read both `skill` and `name`
    keys per the existing tolerant pattern (section_review_required
    and design_review_required do the same -- the marketplace
    plugin layer sometimes uses one form, sometimes the other,
    Codex design review M)."""
    return (
        tool_input.get("skill")
        or tool_input.get("name")
        or ""
    )


def _classify(payload: dict) -> tuple[str, str]:
    """Classify the event. Returns (kind, label) where kind is one
    of `"trigger"`, `"receive"`, or `""` (no-op)."""
    tool_name = payload.get("tool_name", "")
    tool_input = payload.get("tool_input") or {}
    if tool_name == "Bash":
        cmd = tool_input.get("command", "")
        if _is_codex_bash_trigger(cmd):
            # Truncate cmd label to keep state file readable.
            label = cmd.strip().split("\n", 1)[0]
            if len(label) > 120:
                label = label[:117] + "..."
            return ("trigger", f"Bash({label})")
        return ("", "")
    if tool_name == "Skill":
        skill = _skill_name(tool_input)
        if skill in RECEIVE_SKILL_NAMES:
            return ("receive", skill)
        if skill in CODEX_TRIGGER_SKILLS:
            return ("trigger", f"Skill({skill})")
        return ("", "")
    return ("", "")


def _extract_prompt_and_skill(payload: dict) -> tuple[str, str]:
    """Return (prompt_text, skill_name) for §5 dispatch attribution.

    For Bash triggers the prompt is the last positional shlex token
    of the Codex invocation. For Skill triggers we read the skill
    name plus a best-effort prompt extraction from common input keys
    (`prompt`, `args`, `input`, `query`) -- the harness varies and
    we want defensive coverage so a future Skill input shape doesn't
    silently drop the prompt-marker fallback.
    """
    tool_name = payload.get("tool_name", "")
    tool_input = payload.get("tool_input") or {}
    if tool_name == "Bash":
        return (_bash_prompt_arg(tool_input.get("command", "")), "")
    if tool_name == "Skill":
        skill = _skill_name(tool_input)
        # Skill input field varies. Concatenate the obvious string
        # fields so the marker / TODO-path scan has something to chew.
        parts: list[str] = []
        for key in ("prompt", "args", "input", "query", "message"):
            val = tool_input.get(key)
            if isinstance(val, str) and val:
                parts.append(val)
        return ("\n".join(parts), skill)
    return ("", "")


def _write_atomic(path: Path, data: dict) -> None:
    """Atomic write via per-process tmp file + os.replace.

    H3 (review-pipeline adversarial): the original fixed-path
    `.tmp` suffix raced when two PostToolUse hooks ran in parallel
    sessions or nested fires -- both wrote the same `.tmp`, both
    `os.replace`'d, the loser's tmp could be removed before its
    replace ran, leaving the winner's state intact while the loser
    silently dropped a trigger write. Unique tmp path (pid + a
    random suffix) eliminates the cross-process collision.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    # Unique per-process per-call tmp path. `os.replace` is atomic
    # on the same filesystem; concurrent writers each have their
    # own tmp so there's no removal race.
    import secrets
    tmp = path.with_suffix(f"{path.suffix}.{os.getpid()}.{secrets.token_hex(4)}.tmp")
    try:
        tmp.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        os.replace(str(tmp), str(path))
    except Exception as exc:
        # Best-effort cleanup of the unique tmp on failure. Ignore
        # cleanup errors -- the next state-dir GC sweep handles it.
        try:
            if tmp.exists():
                tmp.unlink()
        except Exception:
            pass
        sys.stderr.write(
            f"[codex-review-state] WARN: state write failed "
            f"({type(exc).__name__}: {exc})\n"
        )


def _load_state(path: Path) -> dict | None:
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def main() -> int:
    try:
        payload = json.load(sys.stdin)
    except Exception:
        return 0  # malformed input -- fail open
    kind, label = _classify(payload)
    if not kind:
        return 0
    root = _repo_root()
    if root is None:
        return 0  # outside git repo, nothing to track
    state_path = _state_path(root)
    now_ns = time.time_ns()
    if kind == "trigger":
        # WARN if a previous trigger is being overwritten while still
        # unreceived AND within TTL. This surfaces the "rapid-fire
        # reviews + one stale receive collapses to wrong state" risk
        # H1 from Codex adversarial review (the queue/correlation
        # fix needs `tool_use_id` from the harness; see
        # .claude/state/README.md "Why `tool_use_id` is not in the
        # schema"). The WARN goes to stderr where the user sees it
        # in the next conversation turn -- the gate semantics are
        # unchanged, but a visible workflow-violation signal lets
        # the agent / user notice.
        prior = _load_state(state_path)
        if (
            prior is not None
            and prior.get("received") is False
            and isinstance(prior.get("timestamp_ns"), int)
            and (now_ns - int(prior["timestamp_ns"])) / 1e9 < 3600
        ):
            prior_trigger = str(prior.get("trigger", "(unknown)"))[:80]
            sys.stderr.write(
                f"[codex-review-state] WARN: new trigger {label[:80]!r} "
                f"overwriting previous unreceived trigger {prior_trigger!r} "
                f"(age {(now_ns - int(prior['timestamp_ns'])) / 1e9:.0f}s). "
                f"The previous Codex review was never processed through "
                f"`Skill(superpowers:receiving-code-review)`; only the new "
                f"trigger is now tracked by the gate.\n"
            )
        # Capture currently-staged source files at trigger time
        # PLUS their blob SHAs. Path-binding alone is insufficient
        # (Codex C1: `git diff --cached` covers foo.c at trigger,
        # then agent edits foo.c, then commit -- gate would pass
        # without this binding). The blob SHA captures the exact
        # content the reviewer saw.
        trigger_files = _staged_source_files(root)
        trigger_blobs = _staged_source_blobs(root, trigger_files)
        head_at_dispatch = _head_sha(root)
        state = {
            "timestamp_ns": now_ns,
            "trigger": label,
            "trigger_files": trigger_files,
            "trigger_blobs": trigger_blobs,
            "head_sha": head_at_dispatch,
            "tree_hash": _tree_hash(root),
            "received": False,
            "received_timestamp_ns": None,
        }
        _write_atomic(state_path, state)
        # §5 four-dispatch tracking. Best-effort: extract prompt and
        # detect (kind, todo_path). Both must resolve for the dispatch
        # to count toward the four-dispatch gate. Failures are silent
        # except for explicit WARN paths inside the helpers.
        prompt, skill_for_kind = _extract_prompt_and_skill(payload)
        # No-self-summary detector (§5 item 6). WARN only -- false
        # positives on legitimate context lines are too easy to hit
        # for this to be a block.
        preamble = _scan_self_summary(prompt)
        if preamble:
            sys.stderr.write(
                f"[review-stamps] WARN: Codex prompt opens with a "
                f"first-person / summary preamble: {preamble!r}. "
                f"Reviewer prompts should NOT prepend the implementor's "
                f"narrative -- this biases the reviewer toward agreement "
                f"(CONSENSAGENT ACL-2025). Use the unbiased template at "
                f".claude/skills/review-todo-section/codex-prompt-template.md.\n"
            )
        review_kind = _detect_review_kind(skill_for_kind, prompt)
        todo_path, section = _detect_todo_path(prompt, root)
        if review_kind and todo_path:
            _record_stamp(
                root, todo_path, section, review_kind, now_ns,
                dispatch_head_sha=head_at_dispatch,
            )
        return 0
    # kind == "receive"
    state = _load_state(state_path)
    if state is None:
        # No prior trigger; receive is a no-op (the skill is harmless
        # to invoke standalone).
        return 0
    state["received"] = True
    state["received_timestamp_ns"] = now_ns
    _write_atomic(state_path, state)
    return 0


if __name__ == "__main__":
    sys.exit(main())
