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

  * `argv[0]` basename or path contains `codex-companion.mjs` (our
    review skills' canonical wrapper), OR
  * `argv[0]` is `node` AND `argv[1]` contains `codex-companion.mjs`
    (the typical `node "/abs/path/to/codex-companion.mjs"` shape), OR
  * `argv[0]` basename is `codex` AND `argv[1]` is `review` or `e`
    (the bare CLI / exec alias for review).

  Substring scan over the full command line was rejected after Codex
  adversarial review caught false positives from `rg codex-companion.mjs`,
  `git grep "codex review"`, and heredoc bodies containing the literal.

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
    _wrapper_value_flags = {
        "sudo": frozenset({"-u", "-g", "-n", "-p", "-r", "-h", "-D", "-C"}),
        "doas": frozenset({"-u", "-C"}),
        "timeout": frozenset({"-k", "--kill-after", "-s", "--signal"}),
        "nice": frozenset({"-n", "--adjustment"}),
        "ionice": frozenset({"-c", "--class", "-n", "--classdata", "-p", "--pid", "-P", "-u", "-t"}),
        "env": frozenset({"-u", "--unset", "-S", "--split-string", "-C", "--chdir"}),
        "stdbuf": frozenset({"-i", "-o", "-e"}),
        "command": frozenset({"-p"}),
        "taskset": frozenset({"-c", "--cpu-list", "-p", "--pid"}),
        "chrt": frozenset({"-p", "--pid"}),
        "setpriv": frozenset({"--reuid", "--regid", "--clear-groups", "--groups", "--inh-caps", "--ambient-caps", "--bounding-set"}),
        "cgexec": frozenset({"-g", "--sticky"}),
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
    head_base = head.rsplit("/", 1)[-1]

    if "codex-companion.mjs" in head_base:
        return True
    if head_base == "node" and "codex-companion.mjs" in second:
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
    """Detect a real Codex invocation in a Bash command.

    Tokenize via shlex, trim heredoc / sub-content, segment by Bash
    control operators (&&, ||, ;, |, &), then scan EACH segment for
    a Codex invocation. Same shape as section-2 codex_model_flag_block.py
    -- review-pipeline adversarial H4 caught divergence where
    `true && codex review prompt` and `taskset -c 0 codex review`
    were missed by the segmentless single-argv check.
    """
    if not isinstance(cmd, str) or not cmd.strip():
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
        state = {
            "timestamp_ns": now_ns,
            "trigger": label,
            "trigger_files": trigger_files,
            "trigger_blobs": trigger_blobs,
            "head_sha": _head_sha(root),
            "tree_hash": _tree_hash(root),
            "received": False,
            "received_timestamp_ns": None,
        }
        _write_atomic(state_path, state)
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
