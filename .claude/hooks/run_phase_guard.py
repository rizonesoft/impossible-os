#!/usr/bin/env python3
"""
run_phase_guard.py -- the unattended run's stop guard, safety refusals and cursor.

Keeps the run's one session (todo/TODO-Claude-Overnight-Runner.md) from asking a
human, tearing itself down, or stopping before fixpoint, and records where it is.

Roles (dispatched on argv[1]):
  pretool   PreToolUse hook: reads {tool_name, tool_input} on stdin.
            exit 0 = allow, exit 2 + stderr = BLOCK.
  stop      Stop hook: for the unattended run (env discriminator
            OVERNIGHT_SEQUENCER_RUN=1, set by scripts/run.sh) while the run
            marker exists and the run is not at FIXPOINT, exit 2 keeps the one
            session working. Only the operator's `run.sh stop` (which removes
            the marker) ends it; the heartbeat restarts a dead session.
            Interactive sessions (env unset) are never blocked.
  CLI      start / status / phase / cursor / progress / next-pass / fixpoint /
            clear / ship-limit-reached / rollover / selftest -- the
            overnight-sequencer skill records its position and verifies each
            shipped section's checkpoint through these.

State (run cursor only; deferrals live in the TODO files, not here):
  .claude/state/sequencer-run.json
    {active, phase, pass_no, domain, file, section_idx, progress_this_pass,
     started_at, updated_at}

The refusals that remain are safety checks: no AskUserQuestion, no self-teardown,
no git worktree mutation, no Codex write, and no edit to a frozen file
(.claude/hooks/gate.py FROZEN). The phase and cursor are a record the skill keeps,
not an enforced order: the process gates that enforced it were deleted on
2026-11-09, after a watched run showed nothing depended on them.
"""
import json
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

HOOK_DIR = Path(__file__).resolve().parent


def repo_root():
    d = HOOK_DIR
    while d != d.parent:
        if (d / ".git").is_dir():
            return d
        d = d.parent
    return Path.cwd()


STATE_PATH = repo_root() / ".claude/state/sequencer-run.json"
# Written by `scripts/run.sh start` and removed by `run.sh stop` or an
# oracle-verified FIXPOINT. Present means a run is live, so the Stop hook keeps
# its session working even if the cursor was reset to inactive.
ARMED_MARKER = repo_root() / ".claude/state/sequencer-armed"
# Written ONLY by `run_phase_guard.py fixpoint` AFTER the triage oracle confirms
# zero remaining work. The headless run's Stop is blocked unless this exists, so
# the ONLY graceful self-stop is a machine-verified completion. The agent cannot
# fabricate it: creating/editing it (or the cursor, or the marker) is a blocked
# self-teardown, and `phase FIXPOINT` / `clear` are blocked too. The run cannot
# talk itself into stopping on a reserved decision or a per-file blocker.
FIXPOINT_SENTINEL = repo_root() / ".claude/state/sequencer-fixpoint"


def is_headless():
    """True only inside the unattended run's session.

    scripts/overnight/run-common.sh starts that session with
    OVERNIGHT_SEQUENCER_RUN=1, so it and its hook subprocesses see it; an
    interactive operator session in the same repo does not. The guard governs
    only the run, so it can be absolute there while never trapping the human
    who shares the repo's settings.json hooks.
    """
    return os.environ.get("OVERNIGHT_SEQUENCER_RUN") == "1"


# Bash command shapes that would let the headless run tear itself down or fake a
# completion. Only the human (interactive session, env discriminator absent) may
# run these. NOTE: `run_phase_guard.py fixpoint` is intentionally NOT blocked --
# it is the legitimate completion path and is itself oracle-gated (it REFUSES
# unless the triage oracle reports zero remaining work), so it cannot be abused.
_STATE_FILES = ("sequencer-armed", "sequencer-run.json", "sequencer-fixpoint")
# touch/cp/dd close the fabricate-the-fixpoint-sentinel bypass (runner-kit
# 2026-07-03: touch/cp/dd wrote the sentinel without tripping the old list).
_MUTATE_OPS = ("rm ", "rm-", "mv ", "unlink", "truncate", "tee ", " > ", ">>",
               "touch ", "cp ", "dd ", "os.remove", "os.unlink", "rmtree",
               "shutil.")


# Codex write/sandbox-escalation markers (mirrors
# codex_review_completed._CODEX_WRITE_FLAGS). A dispatch carrying any of these
# is Codex making CHANGES, not reviewing -- interactive-only per Option A.
_CODEX_WRITE_MARKERS = ("--write", "--full-auto", "--yolo",
                        "--dangerously-bypass-approvals-and-sandbox")
_CODEX_WRITE_SANDBOX = ("workspace-write", "danger-full-access")


def _is_codex_write_dispatch(cmd):
    """True iff cmd is a Codex dispatch AND carries a write marker. Kept
    self-contained (a frozenset/shlex scan) so the guard has no heavy import
    on its hot pretool path."""
    c = cmd or ""
    if not ("codex" in c):  # cheap prefilter; not a codex surface -> not write
        return False
    import shlex
    try:
        toks = shlex.split(c)
    except ValueError:
        toks = c.split()
    # confirm it's actually a codex dispatch surface, not prose mentioning it
    is_codex = any(t == "codex" or t.endswith("codex-companion.mjs")
                   or t.endswith("codex-dispatch.sh")
                   or t.endswith("codex-bg-dispatch.sh")
                   or t.endswith("codex-dispatch-with-files.sh") for t in toks)
    if not is_codex:
        return False
    for i, t in enumerate(toks):
        if t in _CODEX_WRITE_MARKERS:
            return True
        if t in ("--sandbox", "-s"):
            if (toks[i + 1] if i + 1 < len(toks) else "") in _CODEX_WRITE_SANDBOX:
                return True
        if t.startswith("--sandbox=") and t.split("=", 1)[1] in _CODEX_WRITE_SANDBOX:
            return True
    return False


_HEREDOC_RE = re.compile(r"<<-?\s*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\1")


def _strip_git_message_bodies(cmd):
    """Drop git COMMIT-MESSAGE text from a command line before scanning it.

    A message body is DATA, not an instruction. `[SEQ-TEARDOWN]` substring-tested
    the whole command, so a commit message that DESCRIBED a teardown path -- "the
    breaker now stops the suite", "aborts" -- was refused as if it had performed
    one (v08, live 2026-08-02 during TODO-04 section 56). The cost was not the
    refusal: it was that the message got rewritten to launder the vocabulary,
    replacing the accurate "aborts the suite" with "ends the suite early". A gate
    that cannot read prose should not be editing it.

    NARROW BY CONSTRUCTION -- only `git` commands, and only their message
    arguments. A heredoc feeding `bash`/`sh`/`python3` is EXECUTABLE, so
    stripping bodies wholesale would launder a real teardown through
    `bash <<'EOF' ... systemctl stop overnight ... EOF`. That shape must keep
    blocking, and the selftest pins it.
    """
    if not cmd or "git" not in cmd:
        return cmd
    out = cmd
    # 1. Heredoc bodies, but only where the redirect belongs to a git command.
    for m in list(_HEREDOC_RE.finditer(cmd)):
        head = cmd[:m.start()]
        # the command owning this redirect: last control operator to here
        seg = re.split(r"[;&|]", head)[-1].strip()
        first = seg.split()[0] if seg.split() else ""
        if first.rsplit("/", 1)[-1] != "git":
            continue                       # executable heredoc -- leave it alone
        delim = m.group(2)
        body = re.search(r"\n(.*?)(?:^|\n)" + re.escape(delim) + r"\s*(?:\n|$)",
                         cmd[m.end():], re.S)
        if body:
            out = out.replace(body.group(1), " ")
    # 2. `-m <text>` / `--message=<text>` / `-F <file>` arguments.
    try:
        toks = shlex.split(out)
    except ValueError:
        return out                          # unparseable -> scan it unchanged
    if not any(t.rsplit("/", 1)[-1] == "git" for t in toks):
        return out
    keep, skip = [], False
    for t in toks:
        if skip:
            skip = False
            continue
        if t in ("-m", "--message", "-F", "--file"):
            skip = True
            continue
        if t.startswith("--message=") or t.startswith("--file="):
            continue
        keep.append(t)
    return " ".join(keep)


# How many refused rollovers one section gets before the queue moves on without it.
# An entry written before this field existed counts as one refusal.
ROLLOVER_ATTEMPTS = 2


def _refusal_holds_the_queue(refused):
    """True while a refused rollover still earns another attempt at its section."""
    return int((refused or {}).get("attempts") or 1) < ROLLOVER_ATTEMPTS


def _failed_section_leftovers(root):
    """What stops the next section starting on a failed one's tree, or ''. Reverting
    and pushing is always within the run's power, so this cannot wedge the queue."""
    try:
        dirty = subprocess.run(["git", "status", "--porcelain", "-uall"], cwd=str(root),
                               capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return "git status unavailable"
    if dirty.returncode != 0:
        return "git status unavailable"
    if dirty.stdout.strip():
        return "uncommitted changes"
    unpushed = _unpushed_count(root)
    if unpushed is None or unpushed > 0:
        return f"{unpushed} unpushed commit(s)" if unpushed else "no upstream"
    return ""


def _is_self_teardown(cmd):
    c = " ".join(_strip_git_message_bodies(cmd or "").split())
    # Guard CLI used to fake completion or wipe the cursor (fixpoint excluded).
    # Deliberately a whole-command substring, prose included. Narrowing it was tried
    # and rejected at the v22 close-out: three review rounds each found a shape that
    # reached the CLI past the narrower test, against 2 false refusals in 1,560 calls.
    if "run_phase_guard.py" in c and (" clear" in c or "phase FIXPOINT" in c):
        return True
    # Direct tampering with any guard state file.
    if any(f in c for f in _STATE_FILES) and any(op in c for op in _MUTATE_OPS):
        return True
    # The one-session run's controller and its tmux sessions.
    if re.search(r"\brun\.sh\b", c) and re.search(r"\bstop\b", c):
        return True
    if "tmux" in c and "kill" in c and "impossible-os" in c:
        return True
    # Killing the run's claude process.
    if re.search(r"\b(pkill|killall|kill)\b", c) and "claude" in c:
        return True
    return False


_WORKTREE_READONLY = {"list"}

# --- SEQ-WORKTREE classifier: COMMAND POSITION, not substring ---------------
# v17 close-out (2026-08-29). The previous form shlex-split the whole command
# and matched any `git` token anywhere, so it fired on a heredoc PAYLOAD that
# quoted the identity gate's own `git worktree add` line, on a commit MESSAGE
# describing that change, on a `sed` expression editing it, and on a doc
# example -- five live blocks across TODO-06 sections 51-55, each routed around
# in under a minute (split the literal; `-F msgfile`; write the patch to a
# file first). Two trivial bypasses are the argument for adjudicating the
# INVOCATION rather than the text. Shape mirrors codex_model_flag_block.py and
# section_commit_gate.py: heredoc bodies stripped, segments split on control
# operators, env-prefix and wrappers walked, `bash -c` descended.
#
# Refusal direction is preserved on purpose: an env prefix, a wrapper
# (`timeout`, `env`, `xargs`), a `cd x && ...` chain, an inner `bash -c`, and
# `git -C /elsewhere` are all still blocked, and a scratch repo under /tmp is
# still blocked too -- the guard has no safe notion of WHERE a path resolves,
# and a wrong guess there is a real worktree in the run's tree.
_SEQ_SEPARATORS = {"&&", "||", ";", ";;", "|", "&"}
_SEQ_WRAPPERS = {"env", "timeout", "nice", "ionice", "sudo", "doas", "nohup",
                 "setsid", "command", "exec", "time", "stdbuf", "xargs", "chronic"}
_SEQ_DATA_CONSUMERS = {"echo", "printf", "cat", "tee", "grep", "egrep", "fgrep",
                       "rg", "ag", "ack", "sed", "awk", "python", "python3",
                       "perl", "ruby", "node", "jq", "less", "more", "head",
                       "tail", "sort", "uniq", "diff", "comm", "test", "[",
                       "true", "false", "ls", "stat", "wc", "cut", "tr", "cp",
                       "mv", "touch"}
_SEQ_GIT_VALUE_OPTS = {"-C", "-c", "--git-dir", "--work-tree", "--namespace",
                       "--exec-path", "--config-env", "--super-prefix"}
_SEQ_SHELLS = {"bash", "sh", "zsh", "dash", "ksh"}
_SEQ_HEREDOC_RE = re.compile(r"<<-?\s*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\1")


def _seq_strip_heredoc_bodies(cmd):
    """Drop every heredoc BODY (opener line kept, terminator consumed) so the
    payload of `python3 - <<'PY' ... PY` is never tokenized as commands."""
    lines = (cmd or "").split("\n")
    out, i = [], 0
    while i < len(lines):
        line = lines[i]
        out.append(line)
        m = _SEQ_HEREDOC_RE.search(line)
        i += 1
        if m:
            term = m.group(2)
            while i < len(lines) and lines[i].strip() != term:
                i += 1
            i += 1  # the terminator line itself
    return "\n".join(out)


def _seq_pad_operators(cmd):
    """Space-pad UNQUOTED control operators so `a&&git ...` tokenizes as three
    tokens. Quoted text is left byte-exact."""
    out, q, i, n = [], None, 0, len(cmd)
    while i < n:
        ch = cmd[i]
        if q:
            out.append(ch)
            if ch == "\\" and q == '"' and i + 1 < n:
                out.append(cmd[i + 1])
                i += 2
                continue
            if ch == q:
                q = None
            i += 1
            continue
        if ch in ("'", '"'):
            q = ch
            out.append(ch)
            i += 1
            continue
        if ch == "\\" and i + 1 < n:
            out.append(ch)
            out.append(cmd[i + 1])
            i += 2
            continue
        two = cmd[i:i + 2]
        if two in ("&&", "||", ";;"):
            out.append(f" {two} ")
            i += 2
            continue
        if ch in (";", "|", "&"):
            out.append(f" {ch} ")
            i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def _seq_tokens(cmd):
    src = _seq_pad_operators(_seq_strip_heredoc_bodies(cmd or ""))
    try:
        return shlex.split(src, posix=True, comments=False)
    except ValueError:
        return src.split()


def _seq_segments(toks):
    segs, cur = [], []
    for tok in toks:
        if tok in _SEQ_SEPARATORS:
            if cur:
                segs.append(cur)
            cur = []
        else:
            cur.append(tok)
    if cur:
        segs.append(cur)
    return segs


def _seq_trim_subcontent(seg):
    """Cut a segment at the first token opening shell SUB-CONTENT (`$(`, a
    backtick, `<(`/`>(`); what follows belongs to another execution context."""
    for i, tok in enumerate(seg):
        if tok.startswith(("$(", "`", "<(", ">(")):
            return seg[:i]
    return seg


def _seq_is_env_assign(tok):
    if "=" not in tok or tok.startswith("="):
        return False
    head = tok.split("=", 1)[0]
    return bool(head) and (head[0].isalpha() or head[0] == "_") and all(
        c.isalnum() or c == "_" for c in head)


def _seq_strip_prefix(seg):
    """Walk env assignments and wrapper commands (plus their flags / numeric
    args) so seg[i] is the real argv[0]."""
    i = 0
    while i < len(seg) and _seq_is_env_assign(seg[i]):
        i += 1
    while i < len(seg) and os.path.basename(seg[i].rstrip("/")) in _SEQ_WRAPPERS:
        i += 1
        while i < len(seg):
            tok = seg[i]
            if tok.startswith("-") or _seq_is_env_assign(tok) or tok == "{}":
                i += 1
                continue
            try:
                float(tok.rstrip("smhd"))
                i += 1
                continue
            except ValueError:
                break
    return seg[i:]


def _seq_git_argv_is_worktree_mutation(argv):
    """argv[0] is git: walk git's global options to the subcommand."""
    j = 1
    while j < len(argv):
        tok = argv[j]
        if tok in _SEQ_GIT_VALUE_OPTS:
            j += 2
            continue
        if tok.startswith("-"):
            j += 1
            continue
        break
    if j < len(argv) and argv[j] == "worktree":
        sub = argv[j + 1] if j + 1 < len(argv) else ""
        return sub not in _WORKTREE_READONLY
    return False


def _seq_segment_is_worktree_mutation(seg):
    argv = _seq_strip_prefix(_seq_trim_subcontent(seg))
    if not argv:
        return False
    base = os.path.basename(argv[0].rstrip("/"))
    if base in _SEQ_SHELLS:
        # Descend into `bash -c '...'` / `sh -ec '...'`: the script is the
        # first positional after a flag carrying `c`.
        j, found = 1, False
        while j < len(argv) and argv[j].startswith("-"):
            flag = argv[j]
            if flag == "-c" or (not flag.startswith("--") and "c" in flag[1:]):
                found = True
                j += 1
                break
            if flag in ("--rcfile", "--init-file", "-O") and "=" not in flag:
                j += 2
                continue
            j += 1
        if found and j < len(argv):
            return _is_worktree_mutation(argv[j])
        return False
    if base == "git":
        return _seq_git_argv_is_worktree_mutation(argv)
    if base in _SEQ_DATA_CONSUMERS:
        # The rest of this segment is DATA for a text consumer (an echo, a sed
        # expression, a python argument list): never an invocation.
        return False
    # Anything else (an unknown wrapper, `find -exec`, a script name): keep
    # the old anywhere-scan for this segment so the refusal direction cannot
    # regress on a shape the prefix walk does not know.
    for i, tok in enumerate(argv[1:], start=1):
        if os.path.basename(tok.rstrip("/")) == "git" and \
                _seq_git_argv_is_worktree_mutation(argv[i:]):
            return True
    return False


def _is_worktree_mutation(cmd):
    """True when the command INVOKES a mutating `git worktree` subcommand.

    The run executes the PRIMARY worktree and has no legitimate reason to make
    another: the ONE sanctioned repair worktree is created by the operator, in
    an interactive session, where this guard is inert. Blocking every
    subcommand except read-only `list` is tighter than allowlisting a path --
    there is no path the run should be adding.

    Adjudicates the invocation, not the text: see the SEQ-WORKTREE classifier
    notes above.
    """
    if not cmd or not cmd.strip():
        return False
    for seg in _seq_segments(_seq_tokens(cmd)):
        if _seq_segment_is_worktree_mutation(seg):
            return True
    return False


# Ordered phases of the per-file pipeline.
PHASES = ["PREFLIGHT", "TRIAGE", "VALIDATE", "GAP_AUDIT", "SECTIONS",
          "FILE_CLOSE", "ADVANCE", "FIXPOINT"]

# The skills each phase of the pipeline uses. Not enforced: it is the list
# scripts/overnight/skill_denies.py reads, so `run.sh start` can refuse a
# permission deny on a skill the run needs.
PHASE_ALLOWED_SKILLS = {
    "PREFLIGHT": set(),
    "TRIAGE": set(),
    "VALIDATE": {"validate-todo-file"},
    "GAP_AUDIT": {"gap-audit-todo", "codex-gap-audit"},
    "SECTIONS": {"implement-todo-section", "implement-todo-item",
                 "implement-ssdt-range", "review-todo-section",
                 "verify-todo-section"},
    "FILE_CLOSE": {"complete-todo-file", "review-todo-section",
                   "verify-todo-section"},
    "ADVANCE": set(),
    "FIXPOINT": set(),
}

def load_state():
    if not STATE_PATH.exists():
        return {"active": False}
    try:
        with STATE_PATH.open(encoding="utf-8") as fh:
            return json.load(fh)
    except (json.JSONDecodeError, OSError):
        return {"active": False}


def save_state(state):
    """ATOMIC write (tmp + os.replace). Truncate-and-write let a concurrent
    reader (watcher poll, launcher gate) see partial JSON and misread it as
    'no active wait' -- which bypasses an unfinished review (2026-07-11)."""
    STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
    tmp = STATE_PATH.with_suffix(f".{os.getpid()}.tmp")
    with tmp.open("w", encoding="utf-8") as fh:
        json.dump(state, fh, indent=1)
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(str(tmp), str(STATE_PATH))


def _skill_name(tool_input):
    return tool_input.get("skill") or tool_input.get("name") or ""


# Where the run's machinery lives. `.claude/state/**` is excluded on purpose: it
# is the run's own notebook. Of these paths the run may edit all but the frozen
# files in .claude/hooks/gate.py, which recover from every other mistake.
_CONTROL_PLANE_ROOTS = (
    ".claude/hooks/", ".claude/skills/", "scripts/overnight/", ".githooks/",
)
_CONTROL_PLANE_FILES = (".claude/settings.json", "scripts/run.sh")


def _frozen_paths():
    """gate.FROZEN, or None when it cannot be read."""
    try:
        sys.path.insert(0, str(HOOK_DIR))
        import gate
        return gate.FROZEN
    except Exception:
        # fail-direction: None makes every control-plane path count as frozen
        return None


def _control_plane_path(target: str):
    """The control-plane path `target` resolves to, or None.

    Compares against the REPO-RELATIVE path so an absolute path, a `./` prefix
    and a `../` escape all normalise to the same answer. A target outside the
    repo is not control plane and is left alone.
    """
    if not target:
        return None
    try:
        root = repo_root().resolve()
        p = Path(target)
        p = (p if p.is_absolute() else root / p).resolve()
        rel = p.relative_to(root).as_posix()
    except Exception:
        return None
    if rel in _CONTROL_PLANE_FILES:
        return rel
    for r in _CONTROL_PLANE_ROOTS:
        if rel.startswith(r):
            return rel
    return None


def evaluate(tool_name, tool_input, state, armed=False, headless=False):
    """Return (allow: bool, message: str). Pure -- unit-testable.

    The guard governs ONLY the unattended run. An interactive operator session
    (headless=False) is never constrained: it can ask, stop and repair freely,
    which is what lets the human share the repo's hooks.
    """
    if not headless:
        return True, ""

    active = bool(state.get("active"))
    if not active and not armed:
        return True, ""

    # AskUserQuestion is never allowed inside the unattended run.
    if tool_name == "AskUserQuestion":
        return False, (
            "[SEQ-ASK] blocked: decide conservatively + log the assumption, or "
            "defer ([/] + Deferred awaiting-answer + XREF) and advance. "
            "Details: docs/infrastructure/hook-codes.md#seq-ask")

    # The frozen files recover from every other mistake, so the run may not edit
    # them. The rest of its machinery it may repair, with a test (2026-11-07).
    if tool_name in ("Edit", "Write", "MultiEdit", "NotebookEdit"):
        _target = (tool_input.get("file_path") or tool_input.get("path")
                   or tool_input.get("notebook_path") or "")
        _cp = _control_plane_path(_target)
        _frozen = _frozen_paths()
        if _cp and (_frozen is None or _cp in _frozen):
            return False, (
                f"[SEQ-CONTROL-PLANE] blocked: `{_cp}` is frozen, because it "
                f"recovers from every other mistake. Add the problem to "
                f"todo/RUN-ISSUES.md and continue. "
                f"Details: docs/infrastructure/hook-codes.md#seq-control-plane")

    # The unattended run must never tear itself down. Clear/stop is a
    # human-only operation, performed from an interactive session (where the
    # OVERNIGHT_SEQUENCER_RUN discriminator is absent and this guard is inert).
    if tool_name == "Bash" and _is_self_teardown(tool_input.get("command", "")):
        return False, (
            "[SEQ-TEARDOWN] blocked: stopping the run is the operator's. Keep "
            "going -- continue the pipeline. "
            "Details: docs/infrastructure/hook-codes.md#seq-teardown")

    # The ONE repair worktree is operator-created; the run makes none.
    if tool_name == "Bash" and _is_worktree_mutation(tool_input.get("command", "")):
        return False, (
            "[SEQ-WORKTREE] blocked: `git worktree` mutation is operator-only. "
            "The run works in the PRIMARY worktree on main; the single repair "
            "worktree is created interactively by a human. Read-only `git "
            "worktree list` is allowed. "
            "Details: docs/infrastructure/hook-codes.md#seq-worktree")

    # Codex WRITE is interactive-only (Option A, 2026-07-11). The unattended
    # run dispatches READ-ONLY reviews; a write-capable Codex mutating the
    # tree with no per-step human authorship crosses the autonomous-agent
    # boundary (CLAUDE.md). The headless main session is physically blocked
    # from write dispatches; interactive sessions (guard inert) are not.
    if tool_name == "Bash" and _is_codex_write_dispatch(tool_input.get("command", "")):
        return False, (
            "[SEQ-CODEX-WRITE] blocked: Codex write (`task --write` / sandbox "
            "override) is INTERACTIVE-ONLY (autonomous-agent boundary). The "
            "unattended run uses read-only reviews only. Implement the change "
            "yourself and dispatch a read-only review, or defer for an "
            "operator's interactive rescue. "
            "Details: docs/infrastructure/hook-codes.md#seq-codex-write")

    # Everything else passes.
    return True, ""


def handle_pretool():
    try:
        d = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return 0
    tool_name = d.get("tool_name", "")
    tool_input = d.get("tool_input", {})
    state = load_state()
    allow, msg = evaluate(tool_name, tool_input, state,
                          armed=ARMED_MARKER.exists(), headless=is_headless())
    if allow:
        return 0
    sys.stderr.write(msg)
    return 2


# ---- verified checkpoint after a shipped section -------------------------------
#
# `rollover` machine-verifies the checkpoint a shipped section leaves (clean tree,
# pushed, graph rebuild OK, content-bound receipts, no outstanding jobs). The name
# is historical: the session no longer ends there. It goes straight on in-session,
# and compaction manages the context (2026-11-07).

# B4/F2: deterministically-regenerated auto-gen docs. The runner's own
# full-suite / graph runs rewrite these, dirtying the tree; blocking the rollover
# on them forced a separate commit + receipt re-record every time (F2), and the
# bare "N change(s)" message mis-attributed them to the operator (B4). They carry
# no uncommitted WORK a fresh worker must not inherit -- the next run regenerates
# them identically -- so a delta consisting ONLY of these does not block rollover.
_ROLLOVER_AUTOGEN = frozenset({
    "docs/test-coverage/coverage.json",
    "docs/test-coverage/coverage.md",
    "COUNT.md",
    "docs/infrastructure/todo-graph.md",
})


def _dirty_path(porcelain_line: str) -> str:
    """Path from a `git status --porcelain` line ('XY <path>' / '?? <path>',
    rename 'XY old -> new')."""
    p = porcelain_line[3:].strip() if len(porcelain_line) > 3 else porcelain_line
    return p.split(" -> ")[-1].strip().strip('"')


# An XREF stamp's line-number tail: `... (item: "NAME" at line 467)`.
_XREF_LINENO_RE = re.compile(r"\bat line \d+\b")
# Owners a rollover may proceed past. Everything else is real uncommitted work.
_TOLERATED_OWNERS = ("auto-gen", "xref-repair", "gotcha-prune")


def _is_xref_lineno_repair(root: Path, path: str) -> bool:
    """True iff `path`'s UNSTAGED diff changes NOTHING but `at line N` integers
    inside XREF stamps.

    WHY (live wedge, 2026-07-28 03:42). The run refused a verified rollover with
    "blocked by the operator's in-flight TODO-25 edit". No operator had touched
    TODO-25: the dirty content was four XREF line-number bumps (467->468,
    302->303, 463->464, 464->465), every one pointing into TODO-21 -- the file
    the run had just edited, shifting those very lines. The run's OWN tooling
    (`stranded_deferrals.py --owner`, `todo-graph/build-and-validate.sh`) had
    emitted them minutes earlier, and the rollover check read the result as a
    human's work and declined to touch it. That misattribution is SELF-
    SUSTAINING: every section that shifts a cross-referenced line re-dirties a
    TODO the run will again refuse to own, so the rollover never fires without a
    human committing for it (it took operator commit `0f4d6995` to unblock).

    This is the item's blunter fallback -- classify by CONTENT rather than by
    tracking which script wrote what -- and it is deliberately strict: every
    removed line must pair with an added line that is byte-identical once the
    `at line N` integer is normalised, AND both must be XREF stamps. A hunk that
    adds or removes lines, touches anything outside an XREF, or changes a word
    fails the test and still blocks. Real section work does not have this shape.

    Tolerating (not committing) matches the existing auto-gen precedent: the
    repair stays unstaged, the next session regenerates it identically, and a
    later real commit sweeps it in. A rollover GATE that commits on a run's
    behalf would be a far larger behaviour change than the wedge warrants.
    """
    try:
        out = subprocess.run(["git", "diff", "--unified=0", "--", path],
                             cwd=str(root), capture_output=True, text=True,
                             timeout=30)
        if out.returncode != 0:
            return False
        removed, added = [], []
        for ln in out.stdout.splitlines():
            if ln.startswith(("+++", "---", "@@", "diff ", "index ")):
                continue
            if ln.startswith("-"):
                removed.append(ln[1:])
            elif ln.startswith("+"):
                added.append(ln[1:])
        if not removed or len(removed) != len(added):
            return False
        for old, new in zip(removed, added):
            if old == new:
                return False              # a pair that did not change: not a repair
            if "XREF:" not in old or "XREF:" not in new:
                return False
            if not _XREF_LINENO_RE.search(old) or not _XREF_LINENO_RE.search(new):
                return False
            if (_XREF_LINENO_RE.sub("at line N", old)
                    != _XREF_LINENO_RE.sub("at line N", new)):
                return False
        return True
    except Exception:
        return False                      # fail-closed: unsure -> keep blocking


_GOTCHAS_REL = ".claude/state/live-gotchas.md"


def _is_gotcha_expiry_prune(root: Path, path: str) -> bool:
    """True iff `path`'s UNSTAGED diff is a pure DELETION of expired entries.

    WHY (observed 2026-07-31 00:05, at arm time). `runner-doctor.py` runs at
    EVERY real launch from `overnight-launch.sh:158` and prunes live-gotchas
    entries whose `(expires YYYY-MM-DD)` has passed. It correctly never commits
    -- a launcher is not a committer -- so the deletion sits as working-tree
    dirt, classified `tracked-source`, and the next ship rollover refuses with
    "tree not clean". That first occurrence was repaired by an attended commit
    seconds later; the recurrence is scheduled, not hypothetical: two live
    entries carry `(expires 2026-08-15)`, and a watchdog relaunch on that date
    prunes them with nobody attending.

    DELETION-ONLY is the whole safety argument, and it is checked rather than
    assumed. The pruner can only drop lines (`runner-doctor.py` builds a `kept`
    list and skips the rest), so a diff that ADDS anything is not the pruner:
    it is the run appending a hazard, or an operator writing one, and that must
    keep blocking exactly as it does today. A trailing-newline-only
    normalisation is accepted for the same reason -- the pruner rewrites with
    `"\n".join(kept) + "\n"` -- but any added CONTENT line fails.

    Tolerating rather than committing matches the auto-gen and XREF-repair
    precedent: the deletion stays unstaged, the next launch re-derives it
    identically, and a later real commit sweeps it in. Fail-CLOSED on any git
    error: unsure means keep blocking."""
    try:
        out = subprocess.run(["git", "diff", "--unified=0", "--", path],
                             cwd=str(root), capture_output=True, text=True,
                             timeout=30)
        if out.returncode != 0:
            return False
        removed = added = 0
        for ln in out.stdout.splitlines():
            if ln.startswith(("+++", "---", "@@", "diff ", "index ",
                              "new file", "deleted file", "similarity",
                              "rename ", "old mode", "new mode")):
                continue
            if ln.startswith("-"):
                removed += 1
            elif ln.startswith("+"):
                added += 1
            elif ln.startswith("\\"):
                continue                  # "\ No newline at end of file"
        return removed > 0 and added == 0
    except Exception:
        return False                      # fail-closed: unsure -> keep blocking


def _dirty_owner(porcelain_line: str, root: Path | None = None) -> str:
    """Classify a `git status --porcelain` line by its XY status (F4, C-RECV-class
    review 2026-07-14). ONLY an UNSTAGED modification (` M`) of an allowlisted
    auto-gen file -- or of a todo/*.md whose diff is pure XREF line-number repair
    (2026-07-28) -- is tolerated. A STAGED change (index column set), a delete /
    rename / copy, an unmerged/conflicted entry (UU/AA/DD/AU/UA/UD/DU), or an
    untracked file is NEVER tolerated -- even on an allowlisted path -- because
    each is uncommitted index state or new content a rotation could strand or
    misinterpret. The old pathname-only test tolerated all of these on
    coverage.*/COUNT.md/todo-graph.md.

    `root` is optional so existing callers and tests keep working; without it the
    content-based XREF check is skipped and behaviour is exactly as before."""
    if porcelain_line.startswith("??"):
        return "untracked"
    xy = (porcelain_line[:2] + "  ")[:2]
    x, y = xy[0], xy[1]
    if "U" in xy or xy in ("AA", "DD"):
        return "conflict"
    # anything staged (X is a real op) or an unstaged delete/rename/copy blocks.
    if x != " " or y in ("D", "R", "C"):
        return "tracked-source"
    if y == "M":
        path = _dirty_path(porcelain_line)
        # the ONLY tolerated cases: an unstaged modification of a tracked
        # generated file, or a TODO dirtied solely by the run's own XREF
        # line-number repairs.
        if path in _ROLLOVER_AUTOGEN:
            return "auto-gen"
        if (root is not None and path == _GOTCHAS_REL
                and _is_gotcha_expiry_prune(root, path)):
            return "gotcha-prune"
        if (root is not None and path.startswith("todo/")
                and path.endswith(".md") and _is_xref_lineno_repair(root, path)):
            return "xref-repair"
    return "tracked-source"


def _rollover_failures(root: Path, state: dict) -> list:
    """Machine gates for a verified rollover checkpoint. Every failure is a
    reason the worker context may NOT rotate yet."""
    fails = []
    try:
        # UNTRACKED FILES COUNT (-uall): a stranded new file is uncommitted
        # state a fresh worker must not inherit silently. The old `-uno`
        # called a tree with untracked junk "clean" (2026-07-11).
        out = subprocess.run(["git", "status", "--porcelain", "-uall"],
                             cwd=str(root), capture_output=True, text=True,
                             timeout=30)
        if out.returncode != 0:
            fails.append("git status unavailable")
        elif out.stdout.strip():
            dirty = [ln for ln in out.stdout.splitlines() if ln.strip()]
            # B4/F2: tolerate a delta that is ONLY auto-gen docs; block on any
            # real change and NAME the paths + probable owner so the diagnostic
            # never mis-attributes the runner's own coverage.* to the operator.
            owners = {ln: _dirty_owner(ln, root) for ln in dirty}
            blocking = [ln for ln in dirty
                        if owners[ln] not in _TOLERATED_OWNERS]
            if blocking:
                named = ", ".join(f"{_dirty_path(ln)} [{owners[ln]}]"
                                  for ln in blocking[:6])
                more = f" (+{len(blocking) - 6} more)" if len(blocking) > 6 else ""
                tolerated = len(dirty) - len(blocking)
                autogen_note = (f"; tolerating {tolerated} pipeline-output "
                                f"delta(s)" if tolerated else "")
                fails.append(f"tree not clean ({len(blocking)} blocking "
                             f"change(s): {named}{more}{autogen_note})")
            # else: only pipeline output dirty -> tolerated, rollover may proceed.
    except Exception:
        fails.append("git status unavailable")
    try:
        out = subprocess.run(["git", "rev-list", "--count", "@{u}..HEAD"],
                             cwd=str(root), capture_output=True, text=True,
                             timeout=30)
        if out.returncode != 0:
            fails.append("no upstream configured (cannot verify pushed)")
        elif out.stdout.strip() != "0":
            fails.append(f"{out.stdout.strip()} commit(s) not pushed")
    except Exception:
        fails.append("git rev-list unavailable")
    try:
        out = subprocess.run(
            ["bash", "scripts/todo-graph/build-and-validate.sh", "--keep-cache"],
            cwd=str(root), capture_output=True, text=True, timeout=300)
        if out.returncode != 0:
            fails.append("todo-graph build-and-validate failed")
    except Exception:
        fails.append("todo-graph rebuild unavailable")
    # Content-bound build receipt: green build over the CURRENT build inputs.
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "overnight_receipts", str(root / "scripts/overnight/receipts.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        ok, why = mod.check_build(root)
        if not ok:
            fails.append(f"build receipt not content-valid ({why})")
        # Test + smoke receipts: a rollover asserts a green checkpoint, so the
        # test suite (and, for boot-path work, the smoke test) must be
        # content-valid over the current tree, not just the build.
        try:
            t_ok, t_why = mod.check_suite(root, "all")
            if not t_ok:
                fails.append(f"test receipt not content-valid ({t_why})")
        except Exception:
            fails.append("test receipt missing (record with receipts.py "
                         "record-suite . all after a green test.sh)")
        try:
            # Prefer the image-bound smoke receipt; accept the legacy suite
            # "smoke" receipt during migration. Fail-closed if neither is valid.
            s_ok, s_why = mod.check_smoke(root)
            if not s_ok:
                l_ok, _ = mod.check_suite(root, "smoke")
                if not l_ok:
                    fails.append(f"smoke receipt not content-valid ({s_why})")
        except Exception:
            fails.append("smoke receipt missing (record with receipts.py "
                         "record-smoke . after a green test-smoke.sh)")
    except Exception as exc:  # noqa: BLE001
        fails.append(f"build/test receipt check unavailable ({exc})")
    # No outstanding review obligation. Fail CLOSED on unreadable state.
    review_state = root / ".claude/state/last-codex-review.json"
    if review_state.exists():
        try:
            rs = json.loads(review_state.read_text())
            if rs.get("received") is not True:
                fails.append("outstanding Codex review not yet received")
        except (OSError, ValueError):
            fails.append("last-codex-review.json unreadable (fail-closed)")
    # No outstanding background jobs: a running codex-review or seq-watcher
    # unit means work is in flight the rollover would strand.
    try:
        out = subprocess.run(
            ["systemctl", "--user", "list-units", "--state=active",
             "--no-legend", "codex-rev-*", "seq-watcher-*"],
            capture_output=True, text=True, timeout=10)
        jobs = [ln.split()[0] for ln in out.stdout.splitlines() if ln.strip()]
        if jobs:
            fails.append(f"outstanding background job(s) still active: "
                         f"{', '.join(jobs[:4])}")
    except Exception:
        pass  # systemctl absent -> cannot enumerate; not a hard fail
    # The run's own audit trail: a ship that never reached the run-log is
    # invisible to the operator reconstructing the night.
    #
    # BOUNDED, because `rollover` has NO override and tells the run to "repair
    # and RE-RUN". A gate the run cannot clear is a WEDGE, and this one guards
    # BOOKKEEPING -- stalling a multi-week unattended run over a missing log
    # line would be a far worse outcome than the missing line. If the same set
    # of ships is refused _RUNLOG_MAX_REFUSALS times the gate yields, loudly.
    # The likeliest unclearable shape is a log entry written WITHOUT the commit
    # hash, which is why the message demands the hash verbatim.
    unlogged = _unlogged_ships(root)
    if unlogged:
        key = "|".join(sorted(u.split(" ")[0] for u in unlogged))
        prev = state.get("runlog_refusals") or {}
        n = (prev.get("count", 0) + 1) if prev.get("key") == key else 1
        state["runlog_refusals"] = {"key": key, "count": n}
        hashes = ", ".join(u.split(" ")[0] for u in unlogged[:3])
        if n < _RUNLOG_MAX_REFUSALS:
            fails.append(
                f"{len(unlogged)} section ship(s) missing a docs/overnight/run-log.md "
                f"entry: {', '.join(unlogged[:3])}"
                + (f" (+{len(unlogged) - 3} more)" if len(unlogged) > 3 else "")
                + f". Append an entry that CONTAINS the commit hash verbatim "
                  f"({hashes}) -- the check matches on the hash, not the section "
                  f"number -- then commit, push, and re-run rollover. "
                  f"(attempt {n}/{_RUNLOG_MAX_REFUSALS}; after that the rollover "
                  f"proceeds and the gap is reported instead of blocking)")
        else:
            print(f"[sequencer] run-log gate YIELDING after {n} refusals -- "
                  f"rollover proceeds with {len(unlogged)} unlogged ship(s): "
                  f"{hashes}. The audit trail has a hole here; FILE THIS to the "
                  f"newest todo/overnight-runner-improvements/ version.",
                  file=sys.stderr)
    else:
        state.pop("runlog_refusals", None)
    return fails


_SHIP_ROW_RE = re.compile(r"^\+.*\|\s*\[x\]\s*\|", re.M)
_HASH_RE = re.compile(r"\b([0-9a-f]{7,40})\b")
# How many consecutive rollover refusals the run-log gate is allowed before it
# yields. Three is enough for the run to read the message, write the entry and
# retry; beyond that the run is not going to clear it and blocking costs more
# than the missing line.
_RUNLOG_MAX_REFUSALS = 3


def _run_started_epoch(root: Path):
    """Epoch the current run was armed, or None.

    Prefers the arm marker's mtime -- it is written once at arm time and is not
    rewritten as the run works, so it is a stable lower bound. `started_at` in
    the run state is a DATE string ("2026-08-07"), too coarse: a run armed at
    21:47 would still sweep operator commits made at 09:00 the same day.
    """
    for rel in (".claude/state/sequencer-run.json",
                ".claude/state/sequencer-armed"):
        p = root / rel
        try:
            if p.exists():
                return int(p.stat().st_mtime)
        except OSError:
            continue
    return None


def _unlogged_ships(root: Path, limit: int = 40) -> list:
    """SHIP commits that `docs/overnight/run-log.md` never recorded.

    The run-log append is the ADVANCE step's only unguarded instruction, and
    nothing verified it: MEASURED 2026-08-03, six consecutive section ships left
    no entry at all, and commit `6e5a5a12` is subject-lined as writing the
    section-61 entry while its diff touches exactly two files, neither of them
    the log. The claim and the diff disagreed and nothing caught it.

    It hid itself, too -- the gap is only visible if you grep the log for
    sections you already know shipped. The cost is not tokens: the run-log is
    what an operator reads to reconstruct a night, and that stretch has to be
    rebuilt out of `git log` instead.

    Keyed on the COMMIT HASH, which the log already records as `SHIPPED <hash>`
    and which (unlike a section number) cannot drift or be spelled three ways.
    Bounded to the commits after the newest hash the log mentions, so a repo
    with a long pre-existing gap reports only what THIS run owes. Returns []
    on any git/IO error -- an audit-trail check must never be the thing that
    wedges a run.
    """
    log = root / "docs/overnight/run-log.md"
    try:
        text = log.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    logged = set(_HASH_RE.findall(text))
    # BOUND THE SCAN TO THIS RUN. Without a lower bound the walk reaches back
    # into commits the OPERATOR made before the arm and charges them to the run,
    # which then has to back-fill run-log lines for work it did not do -- and
    # the cheap way to clear that is to write one line per hash, silently
    # absorbing operator commits into the run's audit trail. Observed live
    # 2026-08-06 01:26: a rollover refused over 4 ships, only one of which was
    # the run's. The gate's purpose is the run's OWN honesty; charging it for
    # someone else's commits corrupts exactly the record it protects.
    since = _run_started_epoch(root)
    try:
        cmd = ["git", "log", f"-{limit}", "--format=%H"]
        if since:
            cmd.insert(2, f"--since=@{since}")
        out = subprocess.run(cmd, cwd=str(root), capture_output=True,
                             text=True, timeout=30)
        if out.returncode != 0:
            return []
        commits = [c for c in out.stdout.split() if c]
    except Exception:
        return []
    # Stop at the newest commit the log already names: everything older is
    # either recorded or predates this run's obligation.
    horizon = len(commits)
    for i, c in enumerate(commits):
        if any(c.startswith(h) for h in logged):
            horizon = i
            break
    unlogged = []
    for c in commits[:horizon]:
        try:
            d = subprocess.run(
                ["git", "show", "--format=%s", "--unified=0", c, "--", "todo/"],
                cwd=str(root), capture_output=True, text=True, timeout=30)
            if d.returncode != 0:
                continue
            body = d.stdout
            if not _SHIP_ROW_RE.search(body):
                continue
            subject = body.splitlines()[0][:60] if body.splitlines() else ""
            if subject.lower().startswith(("review:", "revert")):
                continue          # the review half of a ship pair, not a ship
            unlogged.append(f"{c[:10]} ({subject})")
        except Exception:
            continue
    return unlogged


def _unpushed_count(root: Path):
    """A1 (review 2026-07-14): number of local commits ahead of the upstream, or
    None if it cannot be determined (no upstream / git error). Used to separate a
    genuine MID-section WIP boundary (committed-but-unpushed local work -- the WIP
    rotation does not push) from a POST-ship boundary (the ship path pushes, so
    everything is upstream). A WIP rotation is only meaningful in the former; in
    the latter the full `rollover` (with its receipt/graph verification) is the
    correct exit. None -> caller treats as 'cannot prove unshipped WIP' (refuse)."""
    try:
        r = subprocess.run(["git", "-C", str(root), "rev-list", "--count",
                            "@{u}..HEAD"], capture_output=True, text=True, timeout=10)
    except Exception:
        return None
    if r.returncode != 0:
        return None
    try:
        return int(r.stdout.strip())
    except (ValueError, TypeError):
        return None


def _live_oracle_status():
    """Best-effort live triage-oracle verdict for the Stop fallback.

    Returns the oracle's `status` string (DONE / BLOCKED / NEEDS_WORK) or None
    on any failure -- callers treat None as "keep blocking" (fail-closed).
    """
    try:
        out = subprocess.run(
            [sys.executable, str(repo_root() / ".claude/hooks/sequencer_triage.py"),
             "--next"],
            capture_output=True, text=True, timeout=60, cwd=str(repo_root()))
        if out.returncode != 0:
            return None
        return json.loads(out.stdout.strip()).get("status")
    except Exception:
        return None


def _derive_section_idx(todo_path):
    """Section number the cursor is moving to, or None if it cannot be derived.

    The current section is the first one the triage oracle still classes
    NEEDS_WORK -- the same ordering the sequencer itself routes on, so the
    recorded index matches the section the worker is about to implement.

    Purely for metrics attribution: EVERY failure path returns None so the
    caller keeps the previous value. A cost-reporting field must never be able
    to fail a `cursor` move and wedge a run.
    """
    try:
        out = subprocess.run(
            [sys.executable, str(repo_root() / ".claude/hooks/sequencer_triage.py"),
             "--classify", str(todo_path)],
            capture_output=True, text=True, timeout=60, cwd=str(repo_root()))
        if out.returncode != 0:
            return None
        secs = json.loads(out.stdout.strip()).get("sections")
        if not isinstance(secs, list):
            return None
        for s in secs:
            if isinstance(s, dict) and s.get("class") == "NEEDS_WORK":
                n = s.get("n")
                return n if isinstance(n, int) else None
        return None                    # every section done: leave the cursor be
    except Exception:
        return None


def ship_limit_reached(root):
    """True once a bounded run's sections have shipped and are pushed, counted by their
    Implementation Order rows turning `[x]` (scripts/overnight/ship_count.py). Any doubt
    reads as not reached, so the guard keeps the session working."""
    try:
        limit, start = (root / ".claude/state/run-ship-limit").read_text().split()[:2]
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts/overnight"))
        import ship_count
        return ship_count.ships_between(root, start, "@{u}") >= int(limit)
    except Exception:
        return False  # fail-direction: an unbounded or unreadable run keeps going


def handle_stop():
    # The guard governs ONLY the headless unattended run -- an interactive
    # operator session is never trapped (it can stop freely).
    if not is_headless():
        return 0
    # The ONLY graceful self-stop is an oracle-verified FIXPOINT, recorded by an
    # exclusive sentinel that `run_phase_guard.py fixpoint` writes only after the
    # triage oracle confirms zero remaining work. We key on the sentinel (and the
    # immutable env discriminator), NOT on any value the agent can edit: it
    # cannot fake the sentinel (creating it is a blocked self-teardown), cannot
    # stop itself, and cannot clear the cursor. So no reserved decision, stale
    # blocker, or per-file block can ever talk the run into stopping.
    if FIXPOINT_SENTINEL.exists():
        return 0
    # The operator's `run.sh stop` removes the run marker; once it is gone the
    # run is over, so allow the stop.
    if not ARMED_MARKER.exists():
        return 0
    # A bounded run (`run.sh start --ships N`) that has shipped its sections is done.
    if ship_limit_reached(repo_root()):
        sys.stderr.write("[sequencer] stop allowed: this bounded run shipped its sections.\n")
        return 0
    # Oracle-consulting fallback (runner-kit law 2, adopted 2026-07-03): a
    # session at a TRUE lifecycle end (oracle DONE, or BLOCKED once the 3-state
    # split reports only recoverable deferrals) may end WITHOUT the sentinel --
    # the sentinel is written by the fixpoint CLI, and a session that cannot
    # reach it (lock, confusion, truncation) would otherwise idle until
    # external reap. Ending here does NOT stop the run: the marker persists, the
    # heartbeat nudges the idle session, and it (or the operator) finalizes
    # via `run_phase_guard.py fixpoint`. Fail-closed: any oracle error keeps
    # the hard block below.
    verdict = _live_oracle_status()
    if verdict in ("DONE", "BLOCKED"):
        sys.stderr.write(
            f"[sequencer] stop allowed: live triage oracle reports {verdict} "
            "(no implementable work right now). The run stays ARMED -- "
            "finalize with `python3 .claude/hooks/run_phase_guard.py fixpoint` "
            "(DONE); the heartbeat nudges a BLOCKED queue to look again.\n")
        return 0
    state = load_state()
    sys.stderr.write(
        f"[SEQ-STOP] do not stop (pass={state.get('pass_no')} "
        f"file={state.get('file')} phase={state.get('phase')}). Defer-and-"
        "advance, poll any in-flight review IN-SESSION (blocking sleep loop), "
        "and go on to the next section. Re-invoke Skill(overnight-sequencer) "
        "if you no longer have it in context. Details: docs/infrastructure/hook-codes.md#seq-stop")
    return 2


# ---- CLI (the sequencer skill drives transitions) ----

def _emit_anchor(state) -> None:
    """Best-effort one-line situational anchor on stderr; never raises.

    Only fires for an active run so interactive `status` calls stay quiet.
    """
    try:
        if not state.get("active"):
            return
        import runner_status
        sys.stderr.write("[sequencer] " + runner_status.anchor_line(repo_root()) + "\n")
    except Exception:
        pass


def cli(argv):
    cmd = argv[0] if argv else "status"
    state = load_state()
    if cmd == "start":
        state = {
            "active": True, "phase": "PREFLIGHT", "pass_no": 1,
            "domain": None, "file": None, "section_idx": 0,
            # How section_idx was last set: explicit | derived | stale. `unset`
            # is the start value: section_idx is 0 and means nothing yet.
            "section_source": "unset",
            "progress_this_pass": False,
            "started_at": argv[1] if len(argv) > 1 else "unknown",
            "updated_at": argv[1] if len(argv) > 1 else "unknown",
        }
        save_state(state)
        # The run marker is NOT removed here: it belongs to `run.sh`, and only
        # `run.sh stop` or an oracle-verified FIXPOINT removes it.
        # Clear any stale FIXPOINT sentinel from a prior run: a starting run is by
        # definition not complete, and a leftover sentinel would let Stop succeed.
        try:
            FIXPOINT_SENTINEL.unlink()
        except FileNotFoundError:
            pass
        print("[sequencer] started: pass 1, PREFLIGHT",
              file=sys.stderr)
        return 0
    if cmd == "status":
        print(json.dumps(state, indent=1))
        _emit_anchor(state)
        return 0
    if cmd == "phase":
        if len(argv) < 2 or argv[1] not in PHASES:
            print(f"[sequencer] phase needs one of {PHASES}", file=sys.stderr)
            return 1
        state["phase"] = argv[1]
        save_state(state)
        print(f"[sequencer] phase -> {argv[1]}", file=sys.stderr)
        _emit_anchor(state)
        return 0
    if cmd == "cursor":
        # cursor <domain> <file> [section_idx]
        if len(argv) >= 3:
            # P3.2: a REFUSED checkpoint may repair only the current section; a
            # cursor move to a different one waits for a `rollover` that verifies,
            # for at most ROLLOVER_ATTEMPTS refusals, after which the section parks.
            new_file = argv[2]
            new_idx = int(argv[3]) if len(argv) >= 4 else state.get("section_idx")
            rr = state.get("rollover_refused")
            if isinstance(rr, dict) and state.get("active"):
                if new_file != rr.get("file") or new_idx != rr.get("section_idx"):
                    # A failure stops one item, never the run (2026-11-07). One
                    # refusal earns a repair attempt; after that the queue moves.
                    if _refusal_holds_the_queue(rr):
                        print("[sequencer] cursor BLOCKED (P3.2): a rollover was "
                              f"REFUSED at {rr.get('file')} section "
                              f"{rr.get('section_idx')}. Repair that checkpoint "
                              "(commit/clean/push + receipts) and re-run `rollover`; "
                              f"after {ROLLOVER_ATTEMPTS} refusals this section "
                              "parks and the queue moves on.", file=sys.stderr)
                        return 1
                    leftovers = _failed_section_leftovers(repo_root())
                    if leftovers:
                        print("[sequencer] cursor BLOCKED: leave "
                              f"{rr.get('file')} section {rr.get('section_idx')} "
                              f"behind cleanly first ({leftovers}). Revert its "
                              "unpushed commits, park it with the reason, and push "
                              "the park. Then move on.", file=sys.stderr)
                        return 1
                    print("[sequencer] moving on from "
                          f"{rr.get('file')} section {rr.get('section_idx')} after "
                          f"{rr.get('attempts')} refused rollovers.", file=sys.stderr)
                    state.pop("rollover_refused", None)
            state["domain"], state["file"] = argv[1], argv[2]
            if len(argv) >= 4:
                state["section_idx"] = int(argv[3])
                state["section_source"] = "explicit"
            else:
                # No index given: derive it from the triage oracle (the first
                # section still NEEDS_WORK). On any error the previous value
                # stands, so a cursor move can never fail on it.
                derived = _derive_section_idx(argv[2])
                if derived is not None:
                    state["section_idx"] = derived
                    state["section_source"] = "derived"
                else:
                    # Oracle unavailable: the value is carried over, not measured.
                    state["section_source"] = "stale"
            save_state(state)
            print(f"[sequencer] cursor -> {argv[2]}", file=sys.stderr)
            return 0
        print("[sequencer] cursor needs <domain> <file> [idx]", file=sys.stderr)
        return 1
    if cmd == "progress":
        state["progress_this_pass"] = True
        save_state(state)
        return 0
    if cmd == "next-pass":
        state["pass_no"] = int(state.get("pass_no", 1)) + 1
        state["progress_this_pass"] = False
        state["phase"] = "TRIAGE"
        save_state(state)
        print(f"[sequencer] -> pass {state['pass_no']}", file=sys.stderr)
        return 0
    if cmd == "fixpoint":
        # FIXPOINT is the ONLY path to a permanent stop (it ends the run and
        # removes the run marker). It must NOT be fakeable: machine-verify via the
        # triage oracle (graph-truth, freshly rebuilt) that ZERO work remains.
        # If anything remains, REFUSE -- the run stays active and keeps looping.
        # (subprocess is imported at module scope; a redundant `import subprocess`
        # here previously made the name function-local across ALL of cli(), so any
        # bare subprocess.run reached before this line -- e.g. the rollover inline
        # checkpoint write -- raised UnboundLocalError.)
        root = repo_root()
        try:
            # FAIL-CLOSED on the rebuild (2026-08-08). This return value used to
            # be discarded outright -- not even assigned -- and the oracle ran
            # unconditionally on whatever cache was on disk.
            #
            # That is a hole in the one guarantee this gate exists to give.
            # `build.py` returns 3 when the corpus moved while it was reading
            # (or its git history moved) and REFUSES to write, leaving the
            # PREVIOUS cache in place; the wrapper is invoked with --keep-cache
            # and `sequencer_triage` loads that cache with check_stale=False.
            # So a corpus edit racing this checkpoint could leave a stale cache
            # classifying every file DONE, and the run would declare fixpoint on
            # evidence it had just failed to refresh. Filed by the Codex
            # adversarial review of TODO-06 section 21 and confirmed at source.
            #
            # build-and-validate.sh propagates build.py's code rather than
            # flattening it to 1, so a retryable 3 stays distinguishable from a
            # malformed-input 1 in the refusal message.
            rebuild = subprocess.run(
                ["bash", "scripts/todo-graph/build-and-validate.sh", "--keep-cache"],
                cwd=root, capture_output=True, text=True, timeout=300)
            if rebuild.returncode != 0:
                print(f"[sequencer] fixpoint REFUSED: todo-graph rebuild failed "
                      f"(rc={rebuild.returncode}). The oracle would be reading a "
                      "cache this run just failed to refresh, so DONE would be "
                      "unearned. Fail safe -- keep running, do NOT finish.",
                      file=sys.stderr)
                return 1
            out = subprocess.run(
                [sys.executable, str(HOOK_DIR / "sequencer_triage.py"), "--next"],
                cwd=root, capture_output=True, text=True, timeout=120)
            nxt = json.loads(out.stdout or "{}")
        except Exception as e:  # noqa: BLE001
            print(f"[sequencer] fixpoint REFUSED: oracle check failed ({e}). "
                  "Fail safe -- keep running, do NOT finish.", file=sys.stderr)
            return 1
        if nxt.get("status") != "DONE":
            print(f"[sequencer] fixpoint REFUSED: the oracle still reports work "
                  f"(status={nxt.get('status')}, file={nxt.get('file')}). The run "
                  "is NOT done -- continue the loop. FIXPOINT is only valid when "
                  "`sequencer_triage.py --next` returns DONE.", file=sys.stderr)
            return 1
        # P6.3 -- STRANDED-DEFERRAL GATE (promoted from advisory 2026-07-27).
        #
        # The section oracle above classifies on the Implementation Order row +
        # SECTION stamps only, so a `[/]` checklist item INSIDE a shipped
        # section is invisible to it: the section carries Verified +
        # Quality-reviewed, classifies DONE, and fixpoint never revisits it.
        # That is the one way this runner could declare the OS complete with
        # real, now-runnable work parked -- exactly the completeness hole the
        # fixpoint loop exists to close.
        #
        # This ran advisory-only while the audit's precision was unmeasured (an
        # early naive signal over-matched ~6x). A 10-item hand sample on the
        # live tree (2026-07-27) found 0 false positives, 6 items verified
        # genuinely unblocked at file:line, so it now GATES.
        #
        # It cannot wedge an unattended run: `park` is always a legal
        # disposition (with a reason), so there is a way forward for every
        # item. The gate forces a DECISION, never a particular decision.
        #
        # Fail-OPEN on infrastructure error (missing script, timeout, crash):
        # a broken audit must not strand a genuinely-complete run forever. Only
        # a clean exit 1 -- the audit ran and found undispositioned items --
        # refuses.
        try:
            aud = subprocess.run(
                [sys.executable,
                 str(repo_root() / "scripts/overnight/stranded_deferrals.py"),
                 "--gate"],
                cwd=str(repo_root()), capture_output=True, text=True, timeout=120)
            gate_rc = aud.returncode
        except Exception as e:  # noqa: BLE001
            sys.stderr.write(f"[sequencer] stranded-deferral gate could not run "
                             f"({e}) -- failing OPEN, completion not blocked\n")
            gate_rc = 0
        if gate_rc == 1:
            sys.stderr.write(aud.stderr or "")
            print("[sequencer] fixpoint REFUSED: unblocked-but-parked work "
                  "remains (stranded-deferral gate). Disposition each item "
                  "listed above (reopen / done / park with a reason), then "
                  "re-run fixpoint. The run is NOT done.", file=sys.stderr)
            return 1

        # REACHABILITY GATE (2026-08-02). Completion must also mean "no filed
        # work is invisible". The stranded gate above covers `[/]` items whose
        # OWNER shipped; it does not see a bare `- [ ]` sitting in a section
        # the oracle calls DONE -- nothing revisits those, and 333 of them were
        # counted the day this gate was added. Without this, a run could report
        # the repo complete over work that is merely unreachable rather than
        # finished, which is the exact failure the whole capture-and-file
        # discipline exists to prevent.
        #
        # Same fail-OPEN discipline as the audit above: a broken detector must
        # never strand a genuinely-complete run. Only a clean exit 1 refuses.
        try:
            reach = subprocess.run(
                [sys.executable,
                 str(repo_root() / "scripts/todo-reachability.py")],
                cwd=str(repo_root()), capture_output=True, text=True, timeout=120)
            reach_rc = reach.returncode
        except Exception as e:  # noqa: BLE001
            sys.stderr.write(f"[sequencer] reachability gate could not run "
                             f"({e}) -- failing OPEN, completion not blocked\n")
            reach_rc = 0
        if reach_rc == 1:
            sys.stderr.write((reach.stdout or "")[-4000:])
            print("[sequencer] fixpoint REFUSED: filed work exists that no "
                  "future pass will revisit (reachability gate). Repair each "
                  "item's SHAPE -- `- [/]` naming its blocker, or move it to a "
                  "section that is still open -- then re-run fixpoint. "
                  "Completion means nothing is left behind, not that the "
                  "checklist stopped growing. "
                  "REPAIR THE EXISTING ITEM; do NOT create a new section "
                  "covering the same work because this one is unreachable -- "
                  "unreachable is not absent, and a duplicate makes the corpus "
                  "worse while every counter reads as improvement.",
                  file=sys.stderr)
            return 1

        state["phase"] = "FIXPOINT"
        state["active"] = False
        save_state(state)
        # Genuine, oracle-verified completion is the one self-terminating exit:
        # write the exclusive sentinel (this is the ONLY place it is created) so
        # handle_stop will permit the stop, and remove the run marker so the
        # heartbeat does not restart a finished run.
        FIXPOINT_SENTINEL.parent.mkdir(parents=True, exist_ok=True)
        FIXPOINT_SENTINEL.write_text("oracle-verified: no remaining work\n",
                                     encoding="utf-8")
        try:
            ARMED_MARKER.unlink()
        except FileNotFoundError:
            pass
        print("[sequencer] FIXPOINT verified by oracle (no remaining work, no "
              "undispositioned stranded deferrals) -- run complete; sentinel "
              "written, armed marker removed", file=sys.stderr)
        return 0
    if cmd == "clear":
        save_state({"active": False})
        print(f"[sequencer] cleared: {' '.join(argv[1:]) or 'no reason'}", file=sys.stderr)
        return 0
    if cmd == "ship-limit-reached":
        return 0 if ship_limit_reached(repo_root()) else 1
    if cmd == "rollover":
        # Verified checkpoint after a shipped section. Machine gates decide; on
        # success the session goes straight on to the next section.
        if not state.get("active"):
            print("[sequencer] rollover REFUSED: no active run", file=sys.stderr)
            return 1
        fails = _rollover_failures(repo_root(), state)
        if fails:
            # P3.2: record the refusal + the section it happened at, so `cursor`
            # blocks an un-rotated advance into the next section until a rollover
            # verifies. Repair the current checkpoint and retry -- do not proceed.
            prior = state.get("rollover_refused")
            same = (isinstance(prior, dict)
                    and prior.get("file") == state.get("file")
                    and prior.get("section_idx") == state.get("section_idx"))
            state["rollover_refused"] = {
                "file": state.get("file"),
                "section_idx": state.get("section_idx"),
                "epoch": int(time.time()),
                "attempts": (int(prior.get("attempts") or 1) + 1) if same else 1,
            }
            save_state(state)
            print("[sequencer] rollover REFUSED (checkpoint not verified):\n"
                  + "\n".join(f"  - {f}" for f in fails)
                  + "\n(receipt failures: run build -> test -> smoke with SMOKE "
                    "LAST, each through `scripts/overnight/run-artifact.sh "
                    "j1-<step> -- bash scripts/<step>.sh`, then `python3 "
                    "scripts/overnight/receipts.py record-rollover .` -- J1, as "
                    "the overnight-sequencer skill shows it -- so the image "
                    "binding stays valid instead of a refuse-fix-refuse cascade.)"
                  + "\nRepair these on the CURRENT section and RE-RUN `rollover` "
                    "-- do NOT start the next section un-rotated (P3.2).",
                  file=sys.stderr)
            return 1
        state.pop("rollover_refused", None)  # P3.2: a verified rollover clears it
        save_state(state)
        # Durable checkpoint so a restarted or compacted session loads settled
        # facts (section-pack digest, receipts, review status) instead of
        # rediscovering them. Best-effort -- never blocks the rollover.
        try:
            subprocess.run(
                ["python3", str(repo_root()
                                / "scripts/overnight/section-checkpoint.py"),
                 "write"], cwd=str(repo_root()), capture_output=True, timeout=30)
        except Exception:
            pass
        print("[sequencer] rollover VERIFIED: clean tree, pushed, graph OK, "
              "receipts content-valid, no outstanding jobs. Go straight on to "
              "the next section in this session; compaction manages the context.",
              file=sys.stderr)
        return 0
    if cmd == "selftest":
        return selftest()
    print(f"[sequencer] unknown command: {cmd}", file=sys.stderr)
    return 1


def selftest():
    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)

    H = True  # headless: the guard only governs the unattended run
    # Interactive sessions (headless=False) are NEVER constrained -- this is the
    # anti-trap invariant that lets the operator share the repo hooks.
    a, _ = evaluate("AskUserQuestion", {}, {"active": True, "phase": "SECTIONS"}, headless=False)
    check(a, "interactive AskUserQuestion blocked (must never trap the operator)")
    a, _ = evaluate("Bash", {"command": "bash scripts/run.sh stop"},
                    {"active": True, "phase": "SECTIONS"}, headless=False)
    check(a, "interactive run.sh stop blocked (the human MUST be able to stop)")

    # AskUserQuestion always blocked in the headless run.
    for ph in PHASES:
        a, _ = evaluate("AskUserQuestion", {}, {"active": True, "phase": ph}, headless=H)
        check(not a, f"AskUserQuestion allowed in {ph}")
    # Inactive + unarmed headless allows everything.
    a, _ = evaluate("AskUserQuestion", {}, {"active": False}, headless=H)
    check(a, "AskUserQuestion blocked while headless run inactive+unarmed")
    # Headless self-teardown is blocked across all known shapes; the legit
    # oracle-gated `fixpoint` and ordinary phase transitions are NOT.
    for cmd in ("bash scripts/run.sh stop",
                "python3 .claude/hooks/run_phase_guard.py clear oops",
                "python3 .claude/hooks/run_phase_guard.py phase FIXPOINT",
                "rm -f .claude/state/sequencer-armed",
                "rm .claude/state/sequencer-run.json",
                "echo '{}' > .claude/state/sequencer-run.json",
                "python3 -c \"import os; os.remove('.claude/state/sequencer-fixpoint')\"",
                "touch .claude/state/sequencer-fixpoint",
                "cp /tmp/fake .claude/state/sequencer-fixpoint",
                "dd if=/dev/zero of=.claude/state/sequencer-fixpoint bs=1 count=1",
                "pkill -f 'claude -p'"):
        a, _ = evaluate("Bash", {"command": cmd}, {"active": True, "phase": "SECTIONS"}, headless=H)
        check(not a, f"headless self-teardown allowed: {cmd}")
    for okcmd in ("python3 .claude/hooks/run_phase_guard.py fixpoint",
                  "python3 .claude/hooks/run_phase_guard.py phase SECTIONS",
                  "python3 .claude/hooks/run_phase_guard.py next-pass",
                  "git commit -m 'x' && cat .claude/state/sequencer-run.json"):
        a, _ = evaluate("Bash", {"command": okcmd}, {"active": True, "phase": "SECTIONS"}, headless=H)
        check(a, f"legit guard/bash command blocked as self-teardown: {okcmd}")
    # Phase is a record, not an order: every skill passes in every phase.
    for ph in PHASES:
        for sk in ("implement-todo-section", "validate-todo-file", "gap-audit-todo",
                   "kernel-code-quality"):
            a, _ = evaluate("Skill", {"skill": sk}, {"active": True, "phase": ph}, headless=H)
            check(a, f"Skill({sk}) blocked in {ph}")
    a, _ = evaluate("Bash", {"command": "git commit"}, {"active": True, "phase": "SECTIONS"}, headless=H)
    check(a, "Bash blocked in SECTIONS")
    # A live run whose cursor was reset to inactive is still governed by the marker.
    a, _ = evaluate("AskUserQuestion", {}, {"active": False}, armed=True, headless=H)
    check(not a, "AskUserQuestion allowed while the run marker exists")
    a, _ = evaluate("Skill", {"skill": "anything"}, {"active": False}, armed=True, headless=H)
    check(a, "skill blocked while the run marker exists")
    a, _ = evaluate("Skill", {"skill": "anything"}, {"active": False}, armed=False, headless=H)
    check(a, "skill blocked while neither marked nor active")

    # Codex WRITE is interactive-only: blocked in the headless active run,
    # never blocked interactively; read-only reviews always pass.
    for wcmd in ("node /x/codex-companion.mjs task --write 'fix it'",
                 "codex exec --sandbox workspace-write 'do it'",
                 "codex --full-auto 'go'",
                 "bash scripts/codex-dispatch.sh 'x' && codex task --write 'y'"):
        a, _ = evaluate("Bash", {"command": wcmd}, {"active": True, "phase": "SECTIONS"}, headless=H)
        check(not a, f"headless Codex write allowed: {wcmd[:40]}")
        a, _ = evaluate("Bash", {"command": wcmd}, {"active": True, "phase": "SECTIONS"}, headless=False)
        check(a, f"interactive Codex write blocked (must not): {wcmd[:40]}")
    for rcmd in ("node /x/codex-companion.mjs adversarial-review 'review'",
                 "bash scripts/overnight/review-broker-codex-dispatch.sh '[review-kind: design] todo/x.md b'",
                 "node /x/codex-companion.mjs task --background 'review-kind design'"):
        a, _ = evaluate("Bash", {"command": rcmd}, {"active": True, "phase": "SECTIONS"}, headless=H)
        check(a, f"headless read-only review blocked: {rcmd[:40]}")

    # v08: [SEQ-TEARDOWN] substring-tested the whole command, so a commit MESSAGE
    # describing a teardown path was refused as if it had performed one -- and
    # the message then got reworded to launder the vocabulary, replacing the
    # accurate "aborts the suite" with "ends the suite early". A message body is
    # DATA. The scope is deliberately narrow (git only): a heredoc feeding
    # bash/python3 is EXECUTABLE and must keep blocking.
    _teardown_body = ("review: the breaker now stops the suite\n\nIt aborts the "
                      "suite and stops the task; systemctl stop overnight is\n"
                      "left to the operator.\n")
    for cmd, want, why in (
        ("git commit -F - <<'EOF'\n" + _teardown_body + "EOF", False,
         "git -F message body read as a teardown"),
        ('git commit -m "the breaker stops the overnight systemctl unit" -- a.md',
         False, "git -m message body read as a teardown"),
        ('git commit --message="pkill claude when wedged" -- a.md', False,
         "git --message= body read as a teardown"),
        ("pkill -f claude", True, "real pkill claude not blocked"),
        ("bash <<'EOF'\nbash scripts/run.sh stop\nEOF",
         True, "EXECUTABLE heredoc laundered a real teardown"),
        ("python3 <<'EOF'\nimport os; os.system('pkill -f claude')\nEOF", True,
         "executable python heredoc laundered a real teardown"),
        ('git commit -m "notes" -- a.md && bash scripts/run.sh stop',
         True, "a real teardown beside a git message escaped"),
        # v22 close-out: refusal shapes that broke narrower versions of this test.
        ("python3 .claude/hooks/run_phase_guard.py clear", True,
         "real guard clear not blocked"),
        ("cd x && FOO=1 python3 .claude/hooks/run_phase_guard.py phase FIXPOINT",
         True, "real phase FIXPOINT not blocked"),
        ("python3 .claude/hooks/run_phase_guard.py \\\n clear", True,
         "backslash-continued guard clear not blocked"),
        ("python3 .claude/hooks/run_phase_guard.py 2>/dev/null clear", True,
         "redirection before the verb not blocked"),
        ("bash <<'E'\npython3 .claude/hooks/run_phase_guard.py clear\nE", True,
         "executable heredoc guard clear not blocked"),
        ("printf clear > v\npython3 .claude/hooks/run_phase_guard.py $(cat v)", True,
         "verb read from a file not blocked"),
        ("python3 .claude/hooks/run_phase_guard.py $(printf clearance | head -c 5)",
         True, "verb cut from a longer word not blocked"),
        ("G=.claude/hooks/run_phase_guard.py; python3 \"$G\" clear", True,
         "guard path in a variable not blocked"),
        ("bash scripts/run.sh stop", True, "run.sh stop not blocked"),
        ("tmux kill-session -t =impossible-os-run", True, "tmux kill not blocked"),
        ("bash scripts/run.sh status", False, "run.sh status read as a teardown"),
    ):
        check(_is_self_teardown(cmd) is want, why)

    # Only the frozen files are refused; the rest of the machinery the run may
    # repair. `.claude/state/**` is the run's notebook and always writable.
    for _p, _want, _why in (
        (".claude/hooks/run_phase_guard.py", False, "frozen guard not blocked"),
        (".claude/hooks/gate.py", False, "frozen gate.py not blocked"),
        (".claude/settings.json", False, "frozen settings.json not blocked"),
        ("scripts/run.sh", False, "frozen run.sh not blocked"),
        ("./scripts/overnight/../overnight/heartbeat.sh", False, "path escape not normalised"),
        ("scripts/overnight/decision-registry.py", True, "repairable machinery blocked"),
        (".claude/hooks/section_commit_gate.py", True, "repairable hook blocked"),
        (".claude/skills/overnight-sequencer/SKILL.md", True, "skill blocked"),
        (".claude/state/live-gotchas.md", True, "run STATE wrongly blocked"),
        ("src/kernel/sched/task.c", True, "ordinary work wrongly blocked"),
    ):
        _a, _ = evaluate("Edit", {"file_path": _p},
                         {"active": True, "phase": "SECTIONS"}, headless=H)
        check(_a is _want, f"[SEQ-CONTROL-PLANE] {_why}: {_p}")
    # and it must be INERT interactively -- an operator repairs the control plane
    _a, _ = evaluate("Edit", {"file_path": ".claude/hooks/x.py"},
                     {"active": True, "phase": "SECTIONS"}, headless=False)
    check(_a, "[SEQ-CONTROL-PLANE] blocked an INTERACTIVE operator edit")

    if fails:
        for f in fails:
            print("FAIL:", f, file=sys.stderr)
        print(f"run_phase_guard selftest: {len(fails)} failure(s)", file=sys.stderr)
        return 1
    print("run_phase_guard selftest OK")
    return 0


def main(argv):
    mode = argv[1] if len(argv) > 1 else "pretool"
    if mode == "pretool":
        return handle_pretool()
    if mode == "stop":
        return handle_stop()
    return cli(argv[1:])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
