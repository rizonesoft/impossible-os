#!/usr/bin/env python3
# block-via: warning-only (STATE hook -- emits stderr warnings on lock contention / state errors but never exits 2; the BLOCK partner is receiving_review_required.py which reads this hook's state file).
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
import datetime as _dt
import json
import os
import re
import shlex
import subprocess
import sys
import time
from contextlib import contextmanager
from pathlib import Path

# TODO-08 section-30: shared shell-aware Codex dispatch helper. The
# helpers _trim_heredoc_body / _segment_by_separators /
# _segment_is_codex_invocation / _is_codex_bash_trigger / _bash_prompt_arg
# below are re-bound from the helper module so this file's existing
# call sites work unchanged AND _review_kind.py shares the same parser.
_HOOK_DIR = Path(__file__).resolve().parent
if str(_HOOK_DIR) not in sys.path:
    sys.path.insert(0, str(_HOOK_DIR))
import _codex_dispatch as _cd  # noqa: E402

# Skill names that count as Codex review triggers. Both the
# bare-name and the marketplace-prefixed spellings are accepted
# because the harness sometimes resolves one form to the other.
CODEX_TRIGGER_SKILLS = frozenset({
    "codex-adversarial-review-section",
    "codex-review-todo",
    "codex-design-review",
    "codex-gap-audit",
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
_GIT_BLOB_BATCH_SIZE = 128


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


def _history_path(root: Path) -> Path:
    """Ring-buffer JSONL of recent reviews for section_commit_gate's
    blob-match fallback. Prevents re-staging an identical content tree
    after a fix-iterate-fix loop from invalidating an already-received
    review whose blob SHAs still match. Last 16 entries kept."""
    return root / ".claude" / "state" / "codex-review-history.jsonl"


def _append_history(root: Path, state: dict, max_entries: int = 16) -> None:
    """Append `state` to the ring buffer, truncating to last
    `max_entries`. Best-effort -- silent on I/O error.
    """
    try:
        hp = _history_path(root)
        hp.parent.mkdir(parents=True, exist_ok=True)
        existing: list = []
        if hp.exists():
            for line in hp.read_text(encoding="utf-8").splitlines():
                line = line.strip()
                if not line:
                    continue
                try:
                    existing.append(json.loads(line))
                except Exception:
                    continue
        existing.append(state)
        # Keep last N, oldest discarded.
        existing = existing[-max_entries:]
        tmp = hp.with_suffix(".tmp")
        tmp.write_text(
            "\n".join(json.dumps(s) for s in existing) + "\n",
            encoding="utf-8",
        )
        os.replace(tmp, hp)
    except Exception:
        pass


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
_REVIEW_GOVERNANCE_PREFIXES = (
    ".claude/hooks/",
    ".claude/skills/",
    ".githooks/",
    "docs/",
    "todo/",
)
_REVIEW_GOVERNANCE_FILES = ("AGENTS.md", "CLAUDE.md", ".gitignore")


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


def _is_review_scope_path(path: str) -> bool:
    p = path.replace("\\", "/")
    if _is_source_path(p):
        return True
    if p in _REVIEW_GOVERNANCE_FILES:
        return True
    return any(p.startswith(pre) for pre in _REVIEW_GOVERNANCE_PREFIXES)


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


def _staged_review_files(root: Path) -> list[str]:
    try:
        out = subprocess.check_output(
            ["git", "diff", "--cached", "--name-only", "-z"],
            cwd=str(root), text=True, timeout=3, stderr=subprocess.DEVNULL,
        )
    except Exception:
        return []
    paths = [p for p in out.split("\x00") if p]
    return [p for p in paths if _is_review_scope_path(p)]


def _staged_source_blobs(root: Path, paths: list[str]) -> dict:
    """Return a {path: mode:blob_sha} map for the given staged source
    paths, captured from `git ls-files -s` (mode/sha/stage/path
    output). Used at trigger time for content-binding evidence
    (Codex C1: path-only binding lets post-review same-path edits
    sneak past). Empty dict on git error; the gate treats missing
    or partial blob coverage as evidence missing.
    """
    if not paths:
        return {}
    staged_paths = list(dict.fromkeys(paths))
    blobs: dict = {}
    for start in range(0, len(staged_paths), _GIT_BLOB_BATCH_SIZE):
        batch = staged_paths[start:start + _GIT_BLOB_BATCH_SIZE]
        try:
            # `git ls-files -s -z -- <paths>` yields:
            #   <mode> SP <sha> SP <stage> TAB <path> NUL
            out = subprocess.check_output(
                ["git", "ls-files", "-s", "-z", "--", *batch],
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
            mode, sha, _stage = parts
            blobs[path] = f"{mode}:{sha}"
    return blobs


def _worktree_review_files(root: Path) -> list[str]:
    """B3: review-scope source files that differ from HEAD in the WORKING TREE
    (staged OR unstaged), restricted to files that still exist on disk.

    Reviews are dispatched over the working tree BEFORE staging (the flow is
    implement -> review -> stage+commit; a find-and-fix cycle re-reviews the
    fixed working tree). The staged-only capture (_staged_review_files) left
    trigger_files EMPTY for those reviews, which forced the SKIP_REVIEW_HOOK
    bypasses seen in the 2026-07-13 canary. Binding to the working tree the
    reviewer actually saw stays content-SAFE: the section-commit gate compares
    the STAGED blob at commit against these blobs, so the committed content must
    byte-match what was reviewed (an edit after the review still mismatches and
    blocks). Empty on git error -- the gate treats empty as 'covers nothing'."""
    try:
        out = subprocess.check_output(
            ["git", "diff", "HEAD", "--name-only", "-z"],
            cwd=str(root), text=True, timeout=3, stderr=subprocess.DEVNULL,
        )
    except Exception:
        return []
    paths = [p for p in out.split("\x00") if p and _is_review_scope_path(p)]
    # Only files that still exist can be content-hashed; a staged deletion has
    # no reviewable content and is handled by the gate's own coverage logic.
    return [p for p in paths if (root / p).exists()]


def _worktree_source_blobs(root: Path, paths: list[str]) -> dict:
    """{path: blob_sha} for the WORKING-TREE content of each path (via
    `git hash-object`) -- the exact bytes the reviewer saw. Bare SHA (no mode
    prefix); the gate's _source_binding_matches strips the mode from the staged
    `mode:sha` at commit, so a bare-sha trigger binds against the staged blob.
    Empty dict on git error or count mismatch (gate treats missing blobs as
    evidence-missing -- fail-closed)."""
    if not paths:
        return {}
    uniq = list(dict.fromkeys(paths))
    blobs: dict = {}
    for start in range(0, len(uniq), _GIT_BLOB_BATCH_SIZE):
        batch = uniq[start:start + _GIT_BLOB_BATCH_SIZE]
        try:
            out = subprocess.check_output(
                ["git", "hash-object", "--", *batch],
                cwd=str(root), text=True, timeout=5, stderr=subprocess.DEVNULL,
            )
        except Exception:
            return {}
        shas = [s.strip() for s in out.splitlines() if s.strip()]
        if len(shas) != len(batch):
            return {}  # alignment lost -> fail-closed
        for p, sha in zip(batch, shas):
            blobs[p] = sha
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


def _trim_heredoc_body(tokens):
    """Re-bound from _codex_dispatch helper (TODO-08 section-30)."""
    return _cd._trim_heredoc_body(tokens)


def _segment_by_separators(tokens):
    """Re-bound from _codex_dispatch helper (TODO-08 section-30)."""
    return _cd._segment_by_separators(tokens)


def _segment_is_codex_invocation(seg_tokens):
    """Re-bound from _codex_dispatch helper (TODO-08 section-30)."""
    return _cd._segment_is_codex_invocation(seg_tokens)


def _segment_is_codex_invocation_LEGACY_UNUSED(seg_tokens: list[str]) -> bool:
    """Original inline implementation kept for reference; the active
    code path now goes through _cd._segment_is_codex_invocation. To be
    removed in a follow-up cleanup.
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
    # Section-28 canonical wrapper: bash scripts/codex-dispatch.sh '<prompt>'.
    # The wrapper exec's into the same node + codex-companion.mjs +
    # adversarial-review chain internally; here we recognize the wrapper
    # shape itself so the gate attributes the dispatch to (todo_path,
    # review-kind) via _detect_review_kind reading argv[1].
    if head_base in ("bash", "sh", "/bin/bash", "/bin/sh") and \
            second.endswith("codex-dispatch.sh"):
        return True
    if head_base == "codex-dispatch.sh" or head_base.endswith("/codex-dispatch.sh"):
        return True

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
    """Re-bound from _codex_dispatch.is_codex_dispatch (TODO-08
    section-30). The shared helper handles all 3 dispatch shapes
    (direct-node, wrapper, bare-CLI) plus heredoc-body / prose-mention
    rejection + multi-line continuation support.
    """
    return _cd.is_codex_dispatch(cmd)


# Write-capable Codex markers. The companion pins `adversarial-review` to a
# read-only sandbox; the WRITE surface is `task --write` (workspace-write) or
# a CLI sandbox override. A write dispatch is Codex making CHANGES (interactive
# rescue, Option A 2026-07-11), NOT a review -- it must NEVER register as a
# review trigger, or its receipt would satisfy/queue a review gate it never
# performed. These markers never appear on a legit read-only review dispatch,
# so matching them is safe (fail-safe direction: a firewalled write can't
# corrupt receipt state).
_CODEX_WRITE_FLAGS = frozenset({
    "--write",                                    # companion `task --write`
    "--full-auto",                                # CLI: sandboxed auto-exec
    "--dangerously-bypass-approvals-and-sandbox",  # CLI: danger-full-access
    "--yolo",
})
_CODEX_WRITE_SANDBOX_VALUES = frozenset({"workspace-write", "danger-full-access"})


def _is_codex_write_dispatch(cmd: str) -> bool:
    """True if a Codex dispatch carries a WRITE/sandbox-escalation marker.
    Caller must have confirmed `_is_codex_bash_trigger(cmd)` first so a bare
    `--write` on some unrelated command cannot false-match."""
    import shlex
    try:
        toks = shlex.split(cmd)
    except ValueError:
        toks = cmd.split()
    for i, t in enumerate(toks):
        if t in _CODEX_WRITE_FLAGS:
            return True
        # --sandbox <val> / -s <val> / --sandbox=<val>
        if t in ("--sandbox", "-s"):
            nxt = toks[i + 1] if i + 1 < len(toks) else ""
            if nxt in _CODEX_WRITE_SANDBOX_VALUES:
                return True
        if t.startswith("--sandbox="):
            if t.split("=", 1)[1] in _CODEX_WRITE_SANDBOX_VALUES:
                return True
        # -c sandbox_mode="workspace-write" / -c 'sandbox_permissions=[...write...]'
        if t == "-c":
            nxt = toks[i + 1] if i + 1 < len(toks) else ""
            if "sandbox" in nxt and any(v in nxt for v in
                                        _CODEX_WRITE_SANDBOX_VALUES | {"write"}):
                return True
        elif t.startswith("-c") and "sandbox" in t and \
                any(v in t for v in _CODEX_WRITE_SANDBOX_VALUES | {"write"}):
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

# Skill -> dispatch kind. The three step-8 dispatches satisfy the
# four-dispatch gate; codex-gap-audit maps so its receipts mirror to
# the shared ledger with the right kind (file-level lifecycle
# evidence) without counting toward step 8. codex-design-review /
# codex-fix-review / codex-impact-analysis / codex-test-coverage /
# codex-review-todo remain valid triggers (recorded in
# last-codex-review.json) with prompt-marker attribution.
DISPATCH_KIND_BY_SKILL = {
    "codex-adversarial-review-section": "adversarial",
    "codex-consistency-audit": "consistency",
    "codex-perf-review": "perf",
    "codex-gap-audit": "gap-audit",
}

# Bash-side: the codex-companion.mjs invocation is identical across
# the three dispatches; only the prompt content differs. SKILL.md
# step 8 + codex-prompt-template.md require a leading
# `[review-kind: <kind>]` marker at the start of the prompt. The
# regex tolerates whitespace and different bracket spellings.
_REVIEW_KIND_RE = re.compile(
    r"\[\s*review[-_ ]kind\s*:\s*(adversarial|consistency|perf|re-adversarial|adversarial-impl|test-coverage|design|gap-audit)\s*\]",
    re.IGNORECASE,
)

# TODO path locator: matches `todo/<domain>/TODO-XX-<slug>.md`. First
# match wins; multiple matches surface a stderr WARN. The regex is
# anchored on the literal `todo/` prefix so it does not match
# `~/.claude/projects/.../todo/...` or other unrelated paths.
_TODO_PATH_RE = re.compile(r"(?<![\w/-])todo/[\w./-]+TODO-\d[\w./-]*\.md")

# Section number locator: `§5`, `§ 5`, `section 5`. Captured for the
# `section` field in the state entry; not used by the gate.
# `section\s+` required WHITESPACE, so the hyphenated form the runner actually
# emits -- `[review-kind: perf] <todo-path> section-39 ...` -- parsed to NOTHING.
# Measured 2026-07-31 on TODO-04 section 39: three perf dispatches were correctly
# recognised as dispatches (kind detection handles every compound shape), but
# each one recorded a section-less stamp, so `perf_section` kept the PREVIOUS
# section's value (38) and the commit gate reported "perf (source changed since
# dispatch)" -- which reads as staleness when the truth was misattribution. Cost:
# 3 refused commits and 3 redundant Codex dispatches, ~15 min. Separators are now
# whitespace, hyphen, underscore, or colon.
_SECTION_RE = re.compile(r"(?:§\s*|section[\s\-_:]+)(\d+)", re.IGNORECASE)

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
    """Re-bound from _codex_dispatch.extract_dispatch_prompt (TODO-08
    section-30). The shared helper handles all 3 dispatch shapes
    (direct-node, wrapper, bare-CLI) plus heredoc-body / prose-mention
    rejection + multi-line continuation support.
    """
    return _cd.extract_dispatch_prompt(cmd)


def _detect_review_kind(skill_name: str, prompt: str) -> str:
    """Return one of `"adversarial" | "consistency" | "perf" | "re-adversarial" | ""`.

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


def _all_dispatch_targets(cmd: str, root: Path) -> list:
    """Every `(kind, todo_path, section)` a compound Bash call dispatches.

    The SINGULAR extractor stops at the first dispatch, so a review wave
    issued as several broker calls in ONE Bash invocation stamped only the
    leading kind and left every other kind carrying its PREVIOUS section's
    timestamp. That then reads at the commit gate as "kind K is stale" on a
    review which was genuinely performed, against a file list belonging to an
    unrelated section -- and the cheapest way out looks like re-dispatching an
    already-approved round, which is the churn the convergence gate exists to
    stop.

    `extract_dispatch_prompts` already exists for exactly this truncation
    class (it fixed the same bug in skill_step_map's step attribution,
    MEASURED 2026-08-03); the stamp path simply never adopted it. Each
    dispatch carries its OWN todo path and section, so they are resolved
    per-prompt rather than inherited from the leading leg.

    Prose mentions, heredoc bodies and non-dispatch segments are rejected by
    the shared helper, so this widens WHICH dispatches are attributed without
    widening WHAT counts as a dispatch.
    """
    out: list = []
    if not cmd:
        return out
    try:
        prompts = _cd.extract_dispatch_prompts(cmd)
    except Exception:
        return out
    for body in prompts:
        kind = _detect_review_kind("", body)
        if not kind:
            continue
        todo_path, section = _detect_todo_path(body, root)
        if not todo_path:
            continue
        out.append((kind, todo_path, section))
    return out


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


def _detect_run_metadata(prompt: str, now_ns: int, review_kind: str) -> tuple[str, str, str]:
    """Return (driver_run_id, review_run_id, prompt_review_run_id_hint).

    The trusted `review_run_id` is ALWAYS generated here at receipt-process
    time -- a prompt-authored id is a correlation HINT only, never the
    identity the obligations resolver trusts (a mutating session could
    otherwise forge a distinct reviewer run by writing one into its own
    dispatch prompt). The driver id stays prompt-derived: it exists to
    EXCLUDE same-run evidence, so a forged value only ever excludes more.
    """
    driver_run_id = ""
    prompt_hint = ""
    if prompt:
        dm = re.search(r"\bdriver[-_ ]run[-_ ]id\s*[:=]\s*([A-Za-z0-9_.:-]+)", prompt)
        rm = re.search(r"\breview[-_ ]run[-_ ]id\s*[:=]\s*([A-Za-z0-9_.:-]+)", prompt)
        if dm:
            driver_run_id = dm.group(1)
        if rm:
            prompt_hint = rm.group(1)
    suffix = review_kind or "unknown"
    review_run_id = f"codex-review-{suffix}-{now_ns}"
    return driver_run_id, review_run_id, prompt_hint


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
) -> tuple[bool, str]:
    """Update last-review-stamps.json[todo_path] with `kind` timestamp,
    per-kind `dispatch_head_sha`, and per-kind section metadata.

    Returns `(ok, error_str)` for honest observability (see TODO-08
    section 24 H1 fix). `ok=False` covers: invalid kind, lock-acquire
    timeout (writer skipped to avoid race), or `_write_atomic` failure.
    Fail-open at the call site (main() does not crash on
    observability failure); the boolean lets the JSONL diagnostic
    record the truth instead of inferring success from absence of
    a propagated exception.

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
    if not todo_path or kind not in ("adversarial", "consistency", "perf", "re-adversarial", "adversarial-impl", "test-coverage", "design", "gap-audit"):
        return (False, f"invalid kind={kind!r} or empty todo_path")
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
            # MONOTONIC, not wall clock (v14 close-out, 2026-08-16). This
            # host's realtime clock steps; a backward step kept
            # `time.time() < deadline` true past the budget, so the 2s bound
            # was not a bound -- measured live as suite failures with
            # elapsed ~= the 4s holder sleep and an EMPTY warn (the hook
            # never timed out, it acquired after the holder released). The
            # TEST side was migrated to monotonic earlier; the hook it
            # measures was not, so the fix had landed on the observer only.
            acquired = False
            deadline = time.monotonic() + 2.0  # 2s budget; harness timeout 5s
            sleep_s = 0.01
            while time.monotonic() < deadline:
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
                return (False, "lock acquire timeout (>2s)")
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
        if section:
            entry[f"{kind}_section"] = section.lstrip("§").strip()
        else:
            # FAIL CLOSED on an unparseable section. Leaving the previous value
            # in place is fail-OPEN: the stamp says this kind was reviewed for a
            # section it was never dispatched against, and a later gate can be
            # satisfied by a review belonging to different code. Clearing costs
            # at worst one honest re-dispatch; keeping it can pass an unreviewed
            # section. This is the stale-value half of the 2026-07-31 defect
            # above -- the parser fix stops it arising, this stops it mattering
            # if any other prompt shape ever fails to parse.
            entry.pop(f"{kind}_section", None)
        if dispatch_head_sha:
            entry[f"{kind}_head"] = dispatch_head_sha
        # Preserve any unexpected keys but ensure the three canonical
        # ones exist so the gate's lookup is total.
        for k in ("adversarial", "consistency", "perf"):
            entry.setdefault(k, None)
        state[todo_path] = entry
        ok, err = _write_atomic(path, state)
        return (ok, err)
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
            # Receipt-state firewall (Option A, 2026-07-11): a WRITE-capable
            # Codex dispatch (`task --write` / sandbox override) is Codex
            # implementing changes, not reviewing. It must never touch
            # last-codex-review.json -- otherwise an interactive rescue would
            # queue or satisfy a review gate it never performed. Reviews stay
            # read-only (`adversarial-review`, the broker, `task` w/o --write)
            # and register normally.
            if _is_codex_write_dispatch(cmd):
                return ("", "")
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


def _write_atomic(path: Path, data: dict) -> tuple[bool, str]:
    """Atomic write via per-process tmp file + os.replace.

    Returns `(ok, error_str)` so callers can record honest write
    success in observability records (see TODO-08 section 24 H1
    fix: previously the function swallowed every exception and
    returned None, so the diagnostic-log `write_ok` field stayed
    True on actual failure -- the JSONL diagnostic could lie).
    Internal cleanup + stderr WARN preserved so callers retain the
    fail-open contract (don't crash main() on observability failure).

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
        return (True, "")
    except Exception as exc:
        # Best-effort cleanup of the unique tmp on failure. Ignore
        # cleanup errors -- the next state-dir GC sweep handles it.
        try:
            if tmp.exists():
                tmp.unlink()
        except Exception:
            pass
        err = f"{type(exc).__name__}: {exc}"
        sys.stderr.write(
            f"[codex-review-state] WARN: state write failed ({err})\n"
        )
        return (False, err)


def _load_state(path: Path) -> dict | None:
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def _debug_log(root, event: str, **fields) -> None:
    """TODO-08 §22 #4 / §24 state-file diagnosis: env-gated JSONL log
    to `.claude/state/codex-review-debug.jsonl` so missing-write
    incidents leave a structured trail (one JSON object per line so
    `jq` / log-shipping tools can ingest it). Opt-in via
    `CODEX_REVIEW_DEBUG=1`; silent in normal operation. Never blocks.
    The §24 canonical schema:
        {ts_ns, event, classifier_kind, classifier_label,
         last_codex_review_write_ok, last_codex_review_write_path,
         stamp_write_ok, error, ...}
    Extra kwargs are merged into the record; unknown fields land at
    the top level so future events can extend without a schema bump.
    """
    if os.environ.get("CODEX_REVIEW_DEBUG", "") != "1":
        return
    if root is None:
        return
    try:
        p = root / ".claude" / "state" / "codex-review-debug.jsonl"
        p.parent.mkdir(parents=True, exist_ok=True)
        rec = {"ts_ns": time.time_ns(), "event": event}
        rec.update(fields)
        with open(p, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec, default=str) + "\n")
    except Exception:
        pass


def _out_kind_from_name(name: str) -> str:
    """'<ts>-<kind>.out' -> '<kind>' (e.g. 20260714-024817-re-adversarial.out)."""
    m = re.match(r".*?-([a-z][a-z-]*)\.out$", name)
    return m.group(1) if m else ""


_VERDICT_RE = re.compile(r'"verdict"\s*:\s*"([^"]+)"')
# Approve-class structured verdicts. An explicit needs-attention / reject /
# request-changes is NOT clean, no matter what clean-sounding prose surrounds it.
_APPROVE_VERDICTS = frozenset({"approve", "approved", "lgtm", "ship", "clean",
                               "pass", "no-findings", "no_findings"})


def _out_is_clean(root: Path, out_path: Path) -> bool:
    """C-RECV: a review .out is CLEAN iff ALL hold. Hardened after the C-RECV
    adversarial review (2026-07-14) found three spoofs in the first cut:

      (F2) The broker's FINAL non-empty line is exactly `Turn completed (rc=0)`.
           Anchoring to the last line -- not any `Turn completed` the review BODY
           may quote (a meta-review of review infra does exactly that) -- means an
           embedded marker can never shadow a real terminal rc!=0 (crash).
      (F3) The AUTHORITATIVE structured verdict (the LAST `{"verdict":"..."}`
           Codex emits) is approve-class. A clean-sounding substring is NOT enough
           -- `Verdict: needs-attention` + `No material findings.` must NOT pass.
      (--) The canonical pipeline parser (review-envelope.leg_summary) ALSO sees
           no crash + zero findings, and no severity token appears in the tail.

    FAIL-SAFE in every direction: a parse failure, a non-rc0 final line, a missing
    or non-approve verdict, any finding, or a severity word all return False, and
    the model must still triage the review through receiving-code-review."""
    try:
        text = out_path.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return False
    lines = [ln for ln in text.splitlines() if ln.strip()]
    if not lines:
        return False
    # (F2) broker terminal sentinel must be the LAST line and rc==0.
    if not re.match(r"^Turn completed \(rc=0\)$", lines[-1].strip()):
        return False
    # (F3) authoritative structured verdict must be approve-class.
    verdicts = _VERDICT_RE.findall(text)
    if not verdicts or verdicts[-1].strip().lower() not in _APPROVE_VERDICTS:
        return False
    # canonical parser cross-check: no crash, zero findings.
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "review_envelope_recv", str(root / "scripts/overnight/review-envelope.py"))
        env = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(env)
        leg = env.leg_summary({"logFile": str(out_path)})
    except Exception:
        return False
    if leg.get("crashed") or leg.get("findings"):
        return False
    tail = " ".join(leg.get("tail") or []).lower()
    if re.search(r"\b(critical|high|medium|low)\b", tail):
        return False
    return True


def _maybe_auto_receive_clean(payload: dict, root) -> None:
    """C-RECV: mark a COMPLETED broker review received when its verdict is
    demonstrably CLEAN, so a clean find-and-fix re-adversarial (which has nothing
    to triage, so the model never invokes receiving-code-review) is usable gate
    evidence instead of forcing a SKIP_REVIEW_HOOK. Detected on a
    `wait-for-codex-verdict.sh <out>` poll. Content-binding is UNTOUCHED -- the
    gate still requires trigger_blobs == staged blobs, so this only removes the
    opt-out for a clean re-review; it never accepts unreviewed content."""
    if root is None or payload.get("tool_name") != "Bash":
        return
    cmd = str((payload.get("tool_input") or {}).get("command") or "")
    if "wait-for-codex-verdict.sh" not in cmd:
        return
    try:
        import shlex
        toks = shlex.split(cmd)
    except Exception:
        toks = cmd.split()
    outs = [t for t in toks if t.endswith(".out")]
    if not outs:
        return
    state_path = _state_path(root)
    state = _load_state(state_path)
    if not isinstance(state, dict) or state.get("received") is True:
        return
    rec_kind = str(state.get("review_kind") or "")
    # (C-RECV review F1) NEVER auto-receive when the record has no kind -- an
    # empty kind must not act as a wildcard that matches any .out.
    if not rec_kind:
        return
    rec_ts = state.get("timestamp_ns")
    for out_rel in outs:
        p = Path(out_rel) if os.path.isabs(out_rel) else (root / out_rel)
        out_kind = _out_kind_from_name(p.name)
        # (F1) Require an EXACT, non-empty kind match. A multi-log wait may list
        # several .out paths; the current record tracks exactly ONE review.
        if not out_kind or out_kind != rec_kind:
            continue
        # (F1) Reject a STALE same-kind artifact from a PRIOR round: the .out the
        # current record tracks was created at/after this record's dispatch, so an
        # older clean same-kind .out (earlier mtime) must not complete the current
        # (possibly finding-bearing) state.
        try:
            if isinstance(rec_ts, int) and p.stat().st_mtime < (rec_ts / 1e9) - 5:
                continue
        except Exception:
            continue
        if _out_is_clean(root, p):
            state["received"] = True
            state["received_timestamp_ns"] = time.time_ns()
            state["received_via"] = "auto-clean-broker-completion (C-RECV)"
            ok, _err = _write_atomic(state_path, state)
            if ok:
                _append_history(root, state)
                _debug_log(root, "auto_receive_clean", kind=rec_kind, out=str(p))
            return


# The two dispatch wrappers refuse a malformed prompt at argv time (bad argc, or
# a bare `--flag` token that codex-companion would re-split into a real CLI
# option). On a refusal `exec node` NEVER runs, so no Codex process starts, no
# review artifact is written, and there is nothing to receive -- yet this hook
# fires on the Bash call regardless of exit status, so the refusal used to
# register as a performed review: it opened a reception obligation the agent
# could only clear with an override, and, worse, it STAMPED
# last-review-stamps.json as though the kind had been reviewed. Anchored on the
# wrappers' own banner, which nothing else emits. Fails OPEN (records) whenever
# the response cannot be read, so a real review is never dropped.
_WRAPPER_REFUSAL = ("[codex-dispatch] BLOCK", "[review-broker] BLOCK")


def _dispatch_was_refused(payload: dict) -> bool:
    """True when the tool_response shows a dispatch wrapper refused at argv
    time, i.e. no review ran and there is nothing to record."""
    try:
        resp = payload.get("tool_response")
        if isinstance(resp, dict):
            text = " ".join(
                str(resp.get(k) or "") for k in ("stdout", "stderr", "output", "error")
            )
        elif isinstance(resp, str):
            text = resp
        else:
            return False
        return any(m in text for m in _WRAPPER_REFUSAL)
    except Exception:
        return False


def main() -> int:
    try:
        payload = json.load(sys.stdin)
    except Exception:
        return 0  # malformed input -- fail open
    root_for_debug = _repo_root()
    _debug_log(root_for_debug, "main_entry",
               tool_name=payload.get("tool_name", ""),
               event_type=payload.get("event_type", ""))
    kind, label = _classify(payload)
    _debug_log(root_for_debug, "classify", kind=kind, label=label)
    if not kind:
        # C-RECV: a `wait-for-codex-verdict.sh` poll is not a trigger/receive,
        # but a COMPLETED broker review with a demonstrably-clean verdict should
        # mark itself received (nothing to triage) so the gate can use it.
        _maybe_auto_receive_clean(payload, root_for_debug)
        return 0
    root = _repo_root()
    if root is None:
        return 0  # outside git repo, nothing to track
    state_path = _state_path(root)
    now_ns = time.time_ns()
    if kind == "trigger" and _dispatch_was_refused(payload):
        # Refused at argv time: no review ran. Recording it would open a
        # reception obligation and stamp a review that never happened.
        sys.stderr.write(
            "[codex-review-state] dispatch was REFUSED by its wrapper "
            "(no review ran); not recording a trigger or a stamp.\n"
        )
        return 0
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
        # Capture the source files the reviewer saw at trigger time PLUS their
        # blob SHAs. Path-binding alone is insufficient (Codex C1: git diff
        # covers foo.c at trigger, then agent edits foo.c, then commit -- gate
        # would pass without this binding). B3: prefer the WORKING-TREE view
        # (staged OR unstaged vs HEAD) because reviews are dispatched BEFORE
        # staging; the staged-only capture left trigger_files empty and forced
        # SKIP_REVIEW_HOOK. Fall back to the staged view when the working tree is
        # clean vs HEAD (e.g. a review dispatched over already-committed code).
        trigger_files = _worktree_review_files(root)
        if trigger_files:
            trigger_blobs = _worktree_source_blobs(root, trigger_files)
        else:
            trigger_files = _staged_review_files(root)
            trigger_blobs = _staged_source_blobs(root, trigger_files)
        head_at_dispatch = _head_sha(root)
        state = {
            "timestamp_ns": now_ns,
            # WHO dispatched this review. The gate that consumes this record is
            # per-repo shared state, so without an owner it blocks EVERY session
            # in the repo -- measured 2026-07-24: the headless overnight run
            # dispatched a review and 6 seconds later the interactive operator's
            # unrelated Edit was BLOCKed by it, then a Write to a scratchpad file
            # outside the repo entirely.
            #
            # session_id, not driver_run_id: driver_run_id is parsed from an
            # optional `driver-run-id:` prompt marker and is EMPTY on every real
            # dispatch, so binding to it would bind to nothing. session_id comes
            # from the harness payload and is always present. Same idiom as
            # review_dispatch_gate.py's per-session entry filter.
            "session_id": payload.get("session_id") or "",
            # Secondary signal, and the one CLAUDE.md already documents as THE
            # headless discriminator. Kept alongside session_id so a record can
            # still be attributed after a session id is forgotten, and so an
            # operator reading the state file can see at a glance whether the
            # runner or a human produced it.
            "overnight_run": os.environ.get("OVERNIGHT_SEQUENCER_RUN") == "1",
            "trigger": label,
            "trigger_files": trigger_files,
            "trigger_blobs": trigger_blobs,
            "head_sha": head_at_dispatch,
            "tree_hash": _tree_hash(root),
            "received": False,
            "received_timestamp_ns": None,
        }
        # TODO-08 section 24 observability: capture write success/failure
        # so missing-write incidents leave a structured trail in
        # codex-review-debug.jsonl. _write_atomic returns (ok, err) so the
        # diagnostic CANNOT lie about success when the writer fail-open
        # path swallowed an OSError -- the H1 fix from the section-24
        # review pipeline. The write itself remains best-effort; we add
        # honest diagnosis, not new failure modes.
        write_ok, write_err = _write_atomic(state_path, state)
        _debug_log(
            root, "trigger_state_write",
            classifier_kind=kind,
            classifier_label=label[:120],
            last_codex_review_write_ok=write_ok,
            last_codex_review_write_path=str(state_path),
            error=write_err,
        )
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
        driver_run_id, review_run_id, prompt_hint = _detect_run_metadata(prompt, now_ns, review_kind)
        state["review_kind"] = review_kind or ""
        state["todo_path"] = todo_path
        state["section"] = section.lstrip("§")
        state["driver_run_id"] = driver_run_id
        state["review_run_id"] = review_run_id
        if prompt_hint:
            state["prompt_review_run_id_hint"] = prompt_hint
        # a background dispatch (companion `task` / bg wrapper) starts a job;
        # its later receive is telemetry, not completed-review proof
        raw_cmd = ""
        ti_for_bg = payload.get("tool_input") or {}
        if isinstance(ti_for_bg, dict):
            raw_cmd = str(ti_for_bg.get("command") or "")
        state["background_dispatch"] = bool(raw_cmd and _cd.is_background_dispatch(raw_cmd))
        # The review BROKER is a background dispatch (its instant {logFile}
        # return is not a completed review -- P1.3), but it IS a real review
        # wrapper, so it still earns a per-kind stamp. A generic background
        # dispatch (companion `task --background`) proves only that a job
        # started, so it stays excluded from the stamp PROOF surface.
        is_broker = bool(raw_cmd and _cd.is_review_broker_dispatch(raw_cmd))
        state["review_broker_dispatch"] = is_broker
        _write_atomic(state_path, state)
        stamp_attempted = bool(
            review_kind and todo_path
            and (not state.get("background_dispatch") or is_broker)
        )
        if stamp_attempted:
            stamp_ok, stamp_err = _record_stamp(
                root, todo_path, section, review_kind, now_ns,
                dispatch_head_sha=head_at_dispatch,
            )
            # A bundled wave stamps EVERY kind it dispatched, not just the
            # leading one -- see _all_dispatch_targets. The command-level
            # eligibility above (broker / non-background) already decided
            # that this invocation may stamp at all; this only fans the
            # decision out across the dispatches the same command carries.
            _seen_targets = {(review_kind, todo_path)}
            for _k, _tp, _sec in _all_dispatch_targets(raw_cmd, root):
                if (_k, _tp) in _seen_targets:
                    continue
                _seen_targets.add((_k, _tp))
                _ok2, _err2 = _record_stamp(
                    root, _tp, _sec, _k, now_ns,
                    dispatch_head_sha=head_at_dispatch,
                )
                _debug_log(
                    root, "trigger_stamp_write_bundled",
                    review_kind=_k, todo_path=_tp, section=_sec,
                    stamp_attempted=True, stamp_write_ok=_ok2, error=_err2,
                )
        elif state.get("background_dispatch"):
            stamp_ok = False
            stamp_err = "skipped (non-broker background dispatch is not review proof)"
        else:
            stamp_ok = False
            stamp_err = "skipped (missing review_kind or todo_path)"
        _debug_log(
            root, "trigger_stamp_write",
            review_kind=review_kind or "unknown",
            todo_path=todo_path,
            section=section,
            stamp_attempted=stamp_attempted,
            stamp_write_ok=stamp_ok,
            error=stamp_err,
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
    # Mirror to ring-buffer history so section_commit_gate can fall back
    # to "any recent received review whose blobs match" when blob SHAs
    # drift between staged-tree restages.
    _append_history(root, state)
    return 0


def _selftest() -> int:
    """Receipt-state firewall: write dispatches excluded, reviews kept."""
    fails = []

    def classify_bash(cmd):
        return _classify({"tool_name": "Bash", "tool_input": {"command": cmd}})[0]

    COMP = "node /x/codex-companion.mjs"
    # WRITE dispatches -> must NOT be a review trigger (no-op).
    write_cases = [
        f"{COMP} task --write 'fix the bug in src/kernel/foo.c'",
        f"{COMP} task --background --write 'implement X'",
        "codex exec --sandbox workspace-write 'do it'",
        "codex exec -s danger-full-access 'do it'",
        "codex --full-auto 'go'",
        "bash scripts/codex-dispatch.sh 'x' && codex task --write 'y'",
    ]
    for c in write_cases:
        if classify_bash(c) != "":
            fails.append(f"write dispatch recorded as review trigger: {c[:60]}")
    # READ-ONLY review dispatches -> must STAY review triggers.
    review_cases = [
        f"{COMP} adversarial-review 'review this'",
        f"{COMP} task --background 'review-kind: design ...'",  # no --write
        "bash scripts/codex-dispatch.sh '[review-kind: adversarial] todo/x.md body'",
        "bash scripts/overnight/review-broker-codex-dispatch.sh '[review-kind: design] todo/x.md body'",
    ]
    for c in review_cases:
        if classify_bash(c) != "trigger":
            fails.append(f"read-only review lost its trigger: {c[:60]}")
    # A bare `--write` on a NON-codex command must not be misread (it isn't a
    # codex dispatch, so it's a no-op either way -- sanity check).
    if classify_bash("git commit --write-something") != "":
        fails.append("non-codex --write misclassified")

    if fails:
        for f in fails:
            print("FAIL:", f, file=sys.stderr)
        print(f"codex_review_completed selftest: {len(fails)} failure(s)",
              file=sys.stderr)
        return 1
    print("codex_review_completed selftest OK (receipt-state firewall)")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(_selftest())
    sys.exit(main())
