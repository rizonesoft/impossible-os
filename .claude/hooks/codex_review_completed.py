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


# Tokens that are env-prefix assignments (FOO=bar) or wrapper commands
# we walk past when looking for the "real" Codex invocation. Mirrors the
# §2 codex_model_flag_block.py pattern -- a Bash command like
# `RECEIVING_REVIEW_OVERRIDE=1 sudo -u dev nice -n 5 codex review ...`
# is a Codex trigger, but `rg codex-companion.mjs .` is NOT.
_CODEX_WRAPPER_TOKENS = frozenset({
    "sudo", "doas", "env", "nice", "nohup", "timeout", "ionice",
    "stdbuf", "unbuffer", "chronic", "exec", "command",
})


def _segment_command_tokens(cmd: str) -> list[str]:
    """Tokenize the Bash command via shlex (POSIX), then strip leading
    env-assignment tokens and known wrapper commands. Returns the
    remaining tokens starting with what shell would actually exec.
    Empty list on shlex failure -- caller treats as not-a-trigger."""
    try:
        toks = shlex.split(cmd, posix=True, comments=False)
    except ValueError:
        return []
    out = list(toks)
    # Strip leading FOO=bar env assignments.
    while out and "=" in out[0] and not out[0].startswith("="):
        head = out[0].split("=", 1)[0]
        if head and all(c.isalnum() or c == "_" for c in head):
            out.pop(0)
        else:
            break
    # Strip known wrapper commands and their flag args (best-effort:
    # accept tokens that look like flags right after the wrapper).
    while out and out[0] in _CODEX_WRAPPER_TOKENS:
        wrapper = out.pop(0)
        # Drop wrapper-flag tokens until we hit a non-flag word.
        while out and out[0].startswith("-"):
            # `sudo -u dev` style: drop the value too.
            flag = out[0]
            out.pop(0)
            if flag in ("-u", "-g", "-n", "-p", "-r") and out:
                out.pop(0)
        # Handle wrappers with a positional non-flag arg before the
        # real command: `timeout 600 cmd ...`, `nice -n 5 cmd`,
        # `ionice -c 2 cmd`. After the flag loop, if the next token
        # looks like a duration / numeric argument, pop it.
        if wrapper == "timeout" and out:
            # `timeout DURATION CMD` -- DURATION is digits with
            # optional s/m/h/d suffix (e.g. "600", "5m", "1h").
            tok = out[0]
            if tok and (tok[0].isdigit() or (len(tok) > 1 and tok[-1] in "smhd" and tok[:-1].replace(".","",1).isdigit())):
                out.pop(0)
    return out


def _is_codex_bash_trigger(cmd: str) -> bool:
    """Detect a real Codex invocation in a Bash command.

    Tokenize via shlex, walk past env-prefix + wrapper commands, then
    inspect argv[0]/argv[1] only. This rejects substring false-positives
    from `rg codex-companion.mjs ...`, `git grep "codex review"`, and
    heredoc bodies that happen to contain the literal -- the H1/M1
    issues from the post-impl Codex adversarial review.

    Forms accepted:
      - argv[0] basename or path contains `codex-companion.mjs`
        (wrapper script, our review skills' canonical form)
      - argv[0] == `node` AND argv[1] contains `codex-companion.mjs`
        (the typical `node "/abs/path/to/codex-companion.mjs"` shape)
      - argv[0] basename is `codex` (or path ending in `/codex`) AND
        argv[1] is one of the known review subcommands
    """
    if not isinstance(cmd, str) or not cmd.strip():
        return False
    toks = _segment_command_tokens(cmd)
    if not toks:
        return False
    head = toks[0]
    second = toks[1] if len(toks) > 1 else ""
    head_base = head.rsplit("/", 1)[-1]

    # codex-companion.mjs wrapper -- direct exec or via node.
    if "codex-companion.mjs" in head_base:
        return True
    if head_base == "node" and "codex-companion.mjs" in second:
        return True

    # Bare `codex` CLI with a review subcommand.
    if head_base == "codex":
        # `codex review` and `codex e` (exec alias) when used for
        # review. Bare `codex --version` is intentionally not a
        # trigger.
        if second in ("review", "e"):
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
    """Atomic write via .tmp + os.replace. Best-effort: a failure
    here is logged to stderr but does not block the tool call."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    try:
        tmp.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        os.replace(str(tmp), str(path))
    except Exception as exc:
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
        # Best-effort capture of the file paths the agent was
        # working with at trigger time. Only meaningful for
        # Bash(node ... codex-companion.mjs ...) where the prompt
        # often names files; leave empty for Skill triggers.
        trigger_files: list[str] = []
        state = {
            "timestamp_ns": now_ns,
            "trigger": label,
            "trigger_files": trigger_files,
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
