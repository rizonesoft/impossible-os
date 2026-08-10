#!/usr/bin/env python3
"""PreToolUse skill-step blocker (TODO-08 §10).

On `Bash(git commit:*)`, `Skill(skill="review-todo-section")`, and the
section-commit signature (caught by section_commit_gate), looks up the
most-recent multi-step skill state from .claude/state/skill-progress.json
and refuses the call (exit 2) if the required terminal steps are missing
or the observed steps are non-contiguous.

Block message names the missing step list so the agent can run them
before retrying. Per Codex design review 2026-04-28 Q4, the documented
opt-out mirrors the existing SKIP_REVIEW_HOOK pattern:

    SKIP_SKILL_STEP_BLOCK=1 SKIP_SKILL_STEP_BLOCK_REASON="<text>"

Both required; reason is logged to .claude/state/skip-log.jsonl so the
opt-out is auditable.
"""

import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

_HOOK_DIR = Path(__file__).resolve().parent
if str(_HOOK_DIR) not in sys.path:
    sys.path.insert(0, str(_HOOK_DIR))

# pylint: disable=wrong-import-position
from skill_step_map import (  # noqa: E402
    MULTI_STEP_SKILLS,
    REQUIRED_TERMINAL_STEPS,
)
# Reuse the alias-aware git-commit classifier from section_commit_gate
# (same one post_commit_smoketest.py imports). Closes the false-positive
# observed when a python -c heredoc body contained the literal text
# "git commit" (incident 2026-04-28).
import section_commit_gate as scg  # noqa: E402
# TODO-08 §21: shared WARN+miss-log helper for the heuristic gates.
import _heuristic_misses as hm  # noqa: E402

_TOOL_HISTORY_REL = ".claude/state/tool-history.jsonl"
_HEURISTIC_EXPLORE_TOOLS = {
    "Grep", "Glob",
    "mcp__lsp-bridge__references", "mcp__lsp-bridge__definition",
    "mcp__lsp-bridge__workspace_symbol", "mcp__lsp-bridge__document_symbol",
    "mcp__lsp-bridge__implementation", "mcp__lsp-bridge__type_definition",
    "mcp__lsp-bridge__hover", "mcp__lsp-bridge__call_hierarchy_incoming",
    "mcp__lsp-bridge__call_hierarchy_outgoing", "mcp__lsp-bridge__diagnostics",
    "mcp__todo-graph__backlinks", "mcp__todo-graph__blocked",
    "mcp__todo-graph__blocking", "mcp__todo-graph__by-domain",
    "mcp__todo-graph__code", "mcp__todo-graph__code-by",
    "mcp__todo-graph__deferred", "mcp__todo-graph__deferred-by",
    "mcp__todo-graph__orphans", "mcp__todo-graph__ready",
    "mcp__todo-graph__stale", "mcp__todo-graph__stats",
}
_HEURISTIC_EXPLORE_MIN = 3
_XREF_RE = re.compile(r"->\s*XREF:\s*(?:\[[^\]]*\]\()?([\w./0-9-]+\.md)", re.IGNORECASE)


_STATE_REL = ".claude/state/skill-progress.json"
_SKIP_LOG_REL = ".claude/state/skip-log.jsonl"
_ORPHAN_SKIP_LOG_REL = ".claude/state/skill-progress-skip.log"
_ORPHAN_SKIP_LOG_MAX_LINES = 200


def _log_orphan_skip(root: str, name: str, entry: dict) -> None:
    """TODO-08 §16: append a JSONL record when _select_active_skill
    skips a `compaction_orphaned` entry. 200-line ring buffer
    (truncate from the front when over). Audit trail in case the
    skip behavior fires on what should have been a live entry.
    """
    p = os.path.join(root, _ORPHAN_SKIP_LOG_REL)
    rec = {
        "ts_ns": time.time_ns() if hasattr(time, "time_ns")
                 else int(time.time() * 1e9),
        "kind": "ORPHAN_SKIP",
        "skill": name,
        "started_head_sha": entry.get("started_head_sha", ""),
        "orphan_ts_ns": entry.get("orphan_ts_ns", 0),
        "orphan_reason": entry.get("orphan_reason", ""),
    }
    try:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        # TODO-08 §16 Codex H1-adjacent fix (2026-04-28): ring-buffer
        # rewrite must not race against a concurrent appender. Use
        # advisory flock on a sibling .lock file; per-process tmp
        # name is a uniqueness backstop so even if the lock is a
        # no-op (Windows host) two writers do not clobber each
        # others tmp.
        lock_path = p + ".lock"
        try:
            import fcntl as _fcntl  # local import for portability
            have_flock = True
        except ImportError:
            _fcntl = None
            have_flock = False
        lock_fd = None
        try:
            if have_flock:
                lock_fd = os.open(lock_path, os.O_RDWR | os.O_CREAT, 0o600)
                _fcntl.flock(lock_fd, _fcntl.LOCK_EX)
            with open(p, "a", encoding="utf-8") as f:
                f.write(json.dumps(rec) + "\n")
            # Ring-buffer truncation: if file > MAX lines, keep the tail.
            with open(p, "r", encoding="utf-8") as f:
                lines = f.readlines()
            if len(lines) > _ORPHAN_SKIP_LOG_MAX_LINES:
                lines = lines[-_ORPHAN_SKIP_LOG_MAX_LINES:]
                tmp = p + ".tmp." + str(os.getpid()) + "." + str(
                    time.time_ns() if hasattr(time, "time_ns")
                    else int(time.time() * 1e9)
                )
                with open(tmp, "w", encoding="utf-8") as f:
                    f.writelines(lines)
                os.replace(tmp, p)
        except Exception:
            pass
        finally:
            if lock_fd is not None:
                try:
                    _fcntl.flock(lock_fd, _fcntl.LOCK_UN)
                except Exception:
                    pass
                try:
                    os.close(lock_fd)
                except Exception:
                    pass
    except Exception:
        pass


def _repo_root():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip() or None
    except Exception:
        return None


def _commit_target_root(cmd: str):
    """Which repository will this `git commit` land in? Returns a realpath, or
    None when it cannot be determined.

    WHY (v13 carry, resolved 2026-08-10). This gate treats every `git commit` as
    a candidate SECTION commit, but a test fixture commits into a THROWAWAY repo
    under /tmp -- a repo with no TODO corpus, no sections and no review state,
    where the gate's whole question is meaningless. It fired anyway, so every
    fixture commit carried a standing `SKIP_SKILL_STEP_BLOCK=1`, and a standing
    opt-out is an opt-out that is set when it should not be too.

    None means UNDETERMINED and the caller must fail CLOSED. A commit whose
    destination cannot be read is far more likely to be an unusual spelling of a
    real project commit than a fixture.
    """
    try:
        import shlex
        # First segment only: a target set by a later `&&` cannot retroactively
        # move where an earlier `git commit` ran, and the whole chain is only
        # reached here because it contains one.
        for sep in ("&&", "||", ";", "|"):
            cmd = cmd.replace(sep, "\n")
        target = None
        for seg in cmd.split("\n"):
            try:
                toks = shlex.split(seg)
            except ValueError:
                return None
            if not toks:
                continue
            # `cd <dir>` -- the shape a fixture uses, and the shape the repo's
            # own doctrine discourages for exactly the reason it is parsed here.
            if toks[0] == "cd" and len(toks) >= 2:
                target = toks[1]
                continue
            if toks[0] == "(" and len(toks) >= 3 and toks[1] == "cd":
                target = toks[2]
                continue
            # `git -C <dir> ... commit`, and the two path-bearing globals.
            if "git" in toks[0]:
                for i, t in enumerate(toks):
                    if t == "-C" and i + 1 < len(toks):
                        target = toks[i + 1]
                    elif t.startswith("--work-tree="):
                        target = t.split("=", 1)[1]
                    elif t.startswith("--git-dir="):
                        target = os.path.dirname(t.split("=", 1)[1]) or "."
                break
        if target is None:
            target = os.getcwd()
        # An unexpanded variable or glob is not a path this can resolve.
        if any(ch in target for ch in "$*?`"):
            return None
        target = os.path.realpath(os.path.expanduser(target))
        if not os.path.isdir(target):
            return None
        out = subprocess.check_output(
            ["git", "-C", target, "rev-parse", "--show-toplevel"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
        return os.path.realpath(out) if out else None
    except Exception:
        return None


def _commit_is_in_project(cmd: str) -> bool:
    """True when this commit lands in THIS project's repo (so the gate applies).

    Fails CLOSED in every uncertain direction: an unreadable target, an
    unreadable project root, or any exception all return True, because a
    section-ship commit that slips this gate is the failure the gate exists to
    prevent, while a fixture commit that trips it costs one opt-out.
    """
    target = _commit_target_root(cmd)
    if target is None:
        return True
    project = os.environ.get("CLAUDE_PROJECT_DIR") or _repo_root()
    if not project:
        return True
    try:
        return os.path.realpath(project) == target
    except Exception:
        return True


def _is_blocking_signature(d: dict) -> bool:
    r"""Return True if the current tool call should be gated by the
    step-state check. Triggers:
      - Bash that is genuinely invoking `git commit` (alias-aware)
      - Skill(skill="review-todo-section")

    Uses section_commit_gate._bash_is_git_commit for the Bash classifier
    so wrapped (`gci`, `git -c alias.X='...' X`, `env VAR=val git commit`)
    and aliased commit invocations are detected, AND so the literal
    string "git commit" appearing inside a python -c heredoc body or
    similar non-command-invocation context does NOT false-positive.
    The naive `re.search(r"\bgit\s+commit\b", cmd)` we used to ship
    matched any literal occurrence; documented incident 2026-04-28
    (user tripped it when synthesizing skill-progress.json with a
    'commit ...' evidence_token string).
    """
    tn = d.get("tool_name", "")
    ti = d.get("tool_input", {}) or {}
    if tn == "Bash":
        cmd = ti.get("command") or ""
        try:
            scg._ensure_crc()
            is_commit, _ = scg._bash_is_git_commit(cmd)
        except Exception:
            return False
        if not is_commit:
            return False
        # A commit into a THROWAWAY repo is not a section ship. See
        # _commit_target_root; this fails closed on every uncertainty.
        return _commit_is_in_project(cmd)
    if tn == "Skill":
        skill = ti.get("skill") or ti.get("name") or ""
        if skill != "review-todo-section":
            return False
        # Bug fix 2026-04-30: only fire when the user is RE-INVOKING the
        # same review (args match the active state). A fresh Skill call
        # with different args is starting a NEW review whose terminal
        # steps haven't run yet -- blocking it on the PRIOR session's
        # incomplete state was the recurring friction at every section
        # boundary. The PostToolUse observer creates a fresh entry on
        # this Skill call regardless; we only want to block when the
        # user is genuinely trying to commit a stale-state re-entry.
        new_args = ti.get("args") or ""
        try:
            root = _repo_root()
            if not root:
                return True  # cannot compare; default to block (safe)
            state_p = os.path.join(root, ".claude/state/skill-progress.json")
            if not os.path.exists(state_p):
                return False  # no prior state, nothing to gate against
            with open(state_p, "r", encoding="utf-8") as fh:
                state = json.load(fh)
            entry = state.get("review-todo-section") or {}
            cur_args = entry.get("args") or ""
            if cur_args and cur_args.strip() != new_args.strip():
                return False  # different review; let observer reset
            return True
        except Exception:
            return True  # safe default
    return False


def _cur_session(payload) -> str:
    """Session id of the call being gated. Empty when unavailable, which the
    selector treats as 'cannot attribute' and therefore keeps enforcing."""
    try:
        return (payload or {}).get("session_id") or ""
    except Exception:  # noqa: BLE001 -- attribution must never raise into a gate
        return ""


def _select_active_skill(state: dict, root: str = "", cur_session: str = ""):
    """Pick the most-recently-started multi-step skill entry. Returns
    (skill_name, entry_dict) or (None, None) if state is empty.

    TODO-08 §16: skip entries with `compaction_orphaned: true`. The
    PreCompact hook marks every active entry orphaned on compaction
    because the PostToolUse step-observer cannot run during summary
    generation, so the entry's steps_observed list freezes; without
    this skip the gate would BLOCK every post-compaction commit
    (see file header for the structural catch-22 rationale)."""
    if not isinstance(state, dict):
        return (None, None)
    cur_session = cur_session or ""
    best = None
    best_ts = -1
    for name, entry in state.items():
        if name not in MULTI_STEP_SKILLS:
            continue
        if not isinstance(entry, dict):
            continue
        if entry.get("compaction_orphaned") is True:
            # §16: log the skip for audit; never gate against an
            # orphaned entry whose steps_observed froze pre-compact.
            if root:
                _log_orphan_skip(root, name, entry)
            continue
        # SESSION BINDING. skill-progress.json is per-repo shared state and its
        # entries ALREADY record session_id -- this selector simply never read
        # it, so a skill started in one session gated tool calls in every other
        # session in the repo. Measured 2026-07-24: an unrelated infra commit
        # was blocked by a review-todo-section gate inherited from a PRIOR
        # session's HEAD, and the same class blocked interactive edits while the
        # headless overnight run held an entry.
        #
        # Fail-closed on ABSENCE: an entry with no session_id still gates every
        # session, so pre-existing entries and any writer that forgets the field
        # keep their protection. Only a POSITIVE mismatch is skipped.
        ent_session = entry.get("session_id") or ""
        if ent_session and cur_session and ent_session != cur_session:
            continue
        ts = entry.get("started_ts", 0)
        if not isinstance(ts, int):
            continue
        if ts > best_ts:
            best_ts = ts
            best = (name, entry)
    return best if best else (None, None)


def _missing_terminal_steps(skill: str, observed) -> list:
    required = REQUIRED_TERMINAL_STEPS.get(skill, [])
    seen = set()
    if isinstance(observed, list):
        for o in observed:
            if isinstance(o, dict):
                n = o.get("n")
                if isinstance(n, int):
                    seen.add(n)
    return [n for n in required if n not in seen]


def _converged_kinds(root: str, entry: dict) -> set:
    """Review kinds `review_convergence` has already declared CONVERGED for the
    section this skill entry is working.

    The two gates were individually right and knew nothing about each other:
    `review_convergence should-redispatch <slice> adversarial` exits 1
    (CONVERGED -- inputs unchanged since the last verdict, so re-dispatching
    would burn a full round to re-derive the same answer), while this gate still
    demanded a step-5 observation for that kind. The only way through was the
    blanket `SKIP_SKILL_STEP_BLOCK=1`, which suppresses EVERY step check, not
    just the contradicted one.

    That is the shape that erodes a gate: when the escape hatch is wider than
    the exception, reaching for it becomes routine and it stops being evidence
    of anything. A CONVERGED verdict is positive evidence the review HAPPENED --
    convergence is recorded from a real dispatch's fingerprint -- so it
    satisfies the step rather than bypassing it.
    """
    try:
        sec = entry.get("section") or entry.get("section_n")
        todo = entry.get("todo") or entry.get("file") or entry.get("todo_path")
        if not (sec and todo):
            return set()
        import json as _json
        sp = os.path.join(root, ".claude", "state", "review-convergence.json")
        with open(sp, encoding="utf-8") as fh:
            state = _json.load(fh)
        key = f"{todo}#{sec}"
        rec = state.get(key)
        if not isinstance(rec, dict):
            return set()
        return {k for k, v in rec.items() if isinstance(v, dict) and v.get("fp")}
    except Exception:
        return set()


def _log_skip(root: str, reason: str, skill: str, missing: list) -> None:
    p = os.path.join(root, _SKIP_LOG_REL)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    rec = {
        "ts_ns": time.time_ns() if hasattr(time, "time_ns") else int(time.time() * 1e9),
        "kind": "SKIP_SKILL_STEP_BLOCK",
        "reason": reason[:240],
        "skill": skill,
        "missing": missing,
    }
    try:
        with open(p, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec) + "\n")
    except Exception:
        pass


def _read_tool_history_since(root: str, since_ts_ns: int) -> list:
    """Return tool-history.jsonl records strictly after `since_ts_ns`.

    Codex re-adversarial #2 M1 fix 2026-04-29: tool_history_writer
    live-rotates the log to `.jsonl.1` at 10 MiB. After rotation
    crosses, prior Edit/Read/Codex evidence lives only in `.1`. Read
    rotated FIRST, live SECOND, so timestamp-filtered records remain
    in chronological order. Best-effort, returns [] on any error."""
    p_live = os.path.join(root, _TOOL_HISTORY_REL)
    p_rot = p_live + ".1"
    out = []
    try:
        for path in (p_rot, p_live):
            if not os.path.exists(path):
                continue
            with open(path, "r", encoding="utf-8") as f:
                for line in f:
                    try:
                        rec = json.loads(line)
                    except Exception:
                        continue
                    if not isinstance(rec, dict):
                        continue
                    ts = rec.get("ts_ns", 0)
                    if isinstance(ts, int) and ts > since_ts_ns:
                        out.append(rec)
    except Exception:
        return []
    return out


def _is_first_src_edit(d: dict, history_since: list) -> bool:
    """True iff the current Edit/Write is the first one this skill
    session targeting src/ or include/. Conservative: ANY prior
    Edit/Write/MultiEdit on a matching path means this is not first."""
    tn = d.get("tool_name", "")
    if tn not in ("Edit", "Write", "MultiEdit"):
        return False
    ti = d.get("tool_input", {}) or {}
    fp = ti.get("file_path", "") or ti.get("path", "")
    if not isinstance(fp, str):
        return False
    if not (fp.startswith("src/") or fp.startswith("include/")
            or "/src/" in fp or "/include/" in fp):
        return False
    for r in history_since:
        if r.get("tool_name") in ("Edit", "Write", "MultiEdit"):
            t = r.get("target", "") or ""
            if t.startswith("src/") or t.startswith("include/") \
               or "/src/" in t or "/include/" in t:
                return False
    return True


def _heuristic_check_steps_1_2_3(d: dict, root: str, entry: dict) -> None:
    """TODO-08 §21 steps 1/2/3: pre-first-src-edit heuristics.

    Step 1: WARN if no Read of any todo/**/*.md happened in this skill
            session before the first Edit/Write on src/ or include/.
    Step 2: WARN if any `-> XREF: <path>.md` referenced from the read
            TODO body has NOT been Read.
    Step 3: WARN if fewer than 3 explore-class queries (Grep/Glob/MCP
            lsp-bridge or todo-graph) happened before this first Edit.
    All three are advisory; the gate never BLOCKs.

    Codex re-adversarial H1 fix 2026-04-29: the original perf optimization
    (cache `first_src_edit_seen: true` in skill-progress.json) was reverted
    because it persisted state from a PreToolUse hook BEFORE the edit was
    confirmed to succeed and bypassed the existing skill-progress flock.
    A blocked/failed first edit would still set the marker and suppress
    the heuristics on the next real first edit; concurrent observer
    writes could corrupt the state file. _is_first_src_edit already
    derives the answer from tool-history without persistence; we accept
    the bounded re-parse cost (<= 10 MiB rotated) for the WARN-only
    path rather than risk state corruption."""
    started_ts = entry.get("started_ts") or entry.get("started_ts_ns") or 0
    if not isinstance(started_ts, int) or started_ts <= 0:
        return
    history = _read_tool_history_since(root, started_ts)
    if not _is_first_src_edit(d, history):
        return

    todo_path, section = hm.parse_active_todo_section(entry)

    # --- Step 1 ---
    todo_reads = [
        r for r in history
        if r.get("tool_name") == "Read"
        and isinstance(r.get("target"), str)
        and r.get("target", "").startswith("todo/")
        and r.get("target", "").endswith(".md")
    ]
    if not todo_reads:
        ti = d.get("tool_input", {}) or {}
        target_file = ti.get("file_path", "") or ti.get("path", "") or "<unknown>"
        hm.emit_warn(
            root, 1,
            "todo-read-missing",
            "first src/ Edit on " + target_file + " but no todo/**/*.md "
            "Read recorded in this skill session. Implement-todo-section "
            "step 1: read the section before editing.",
            todo_path=todo_path, section=section,
        )

    # --- Step 2 ---
    if todo_reads:
        xref_targets = set()
        for r in todo_reads:
            tgt = r.get("target", "")
            if not tgt:
                continue
            try:
                full = os.path.join(root, tgt)
                if not os.path.exists(full):
                    continue
                # Codex M1 fix 2026-04-29: read up to 1 MiB so XREF/Inputs
                # blocks placed late in long TODO files (TODO-08 is ~205 KB)
                # are scanned. The previous 64 KiB cap caused a false-negative
                # on the exact file shape this heuristic targets.
                with open(full, "r", encoding="utf-8") as f:
                    body = f.read(1024 * 1024)
                for m in _XREF_RE.finditer(body):
                    xpath = m.group(1)
                    if not xpath.startswith("todo/"):
                        # Allow relative paths inside todo/<domain>/
                        cand = os.path.normpath(os.path.join(os.path.dirname(tgt), xpath))
                        if cand.startswith("todo/"):
                            xpath = cand
                        else:
                            continue
                    xref_targets.add(xpath)
            except Exception:
                continue
        read_paths = {r.get("target", "") for r in todo_reads}
        unread = sorted(t for t in xref_targets if t not in read_paths)
        if unread:
            hm.emit_warn(
                root, 2,
                "xref-unread",
                "first src/ Edit but " + str(len(unread)) + " XREF target(s) "
                "in the read TODO body not yet Read: " + ", ".join(unread[:3])
                + (" (+more)" if len(unread) > 3 else ""),
                todo_path=todo_path, section=section,
            )

    # --- Step 3 ---
    explore_count = sum(
        1 for r in history
        if r.get("tool_name") in _HEURISTIC_EXPLORE_TOOLS
    )
    if explore_count < _HEURISTIC_EXPLORE_MIN:
        hm.emit_warn(
            root, 3,
            "explore-thin",
            "first src/ Edit after only " + str(explore_count) + " explore "
            "querie(s) (Grep/Glob/lsp-bridge/todo-graph); "
            "implement-todo-section step 3 expects >= "
            + str(_HEURISTIC_EXPLORE_MIN) + ".",
            todo_path=todo_path, section=section,
        )


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0

    # §21 heuristics (steps 1/2/3) run on Edit/Write/MultiEdit before the
    # blocking-signature check. They never BLOCK -- emit_warn writes
    # stderr + miss-log and we still return 0 so the tool call proceeds.
    if d.get("tool_name") in ("Edit", "Write", "MultiEdit"):
        try:
            root = _repo_root()
            if root:
                state_path = os.path.join(root, _STATE_REL)
                with open(state_path, "r", encoding="utf-8") as f:
                    state = json.load(f)
                skill, entry = _select_active_skill(state, root, _cur_session(d))
                if skill == "implement-todo-section" and entry:
                    _heuristic_check_steps_1_2_3(d, root, entry)
        except Exception:
            pass
        return 0

    if not _is_blocking_signature(d):
        return 0

    root = _repo_root()
    if not root:
        return 0

    # Opt-out (Q4): both env vars required.
    # TODO-08 section-23: shared SKIP-env scanner -- inline + environ.
    _cmd_for_skip = ""
    if d.get("tool_name") == "Bash":
        _cmd_for_skip = (d.get("tool_input") or {}).get("command", "") or ""
    _skip_envs = hm  # placeholder; fixed below
    import _skip_env as _se
    _skip_envs = _se.read_skip_envs(
        _cmd_for_skip,
        keys=("SKIP_SKILL_STEP_BLOCK", "SKIP_SKILL_STEP_BLOCK_REASON"),
    )
    if _skip_envs.get("SKIP_SKILL_STEP_BLOCK", "") == "1":
        reason = _skip_envs.get("SKIP_SKILL_STEP_BLOCK_REASON", "")
        if len(reason) < 12:
            sys.stderr.write(
                "[skill-step-block] SKIP_SKILL_STEP_BLOCK=1 set but "
                "SKIP_SKILL_STEP_BLOCK_REASON missing or under 12 chars; "
                "the opt-out requires a plain-language reason >= 12 chars."
            )
            return 2
        # Will log on the way out below.

    state_path = os.path.join(root, _STATE_REL)
    try:
        with open(state_path, "r", encoding="utf-8") as f:
            state = json.load(f)
    except Exception:
        # No state -> nothing to enforce. The §4 section-commit gate is
        # the canonical guard for sections without an active skill flow.
        return 0

    skill, entry = _select_active_skill(state, root, _cur_session(d))
    if not skill or not entry:
        return 0

    observed = entry.get("steps_observed", [])
    missing = _missing_terminal_steps(skill, observed)
    if missing:
        # A kind this section has already CONVERGED on satisfies its step: the
        # convergence record is minted from a real dispatch, so it is evidence
        # the review happened, not an excuse for skipping it. Without this the
        # only exit was the blanket SKIP, which suppresses every other check too.
        conv = _converged_kinds(root, entry)
        if conv:
            try:
                from skill_step_map import SKILL_STEP_MAP
                sat = set()
                for step_n, tool, pattern in SKILL_STEP_MAP.get(skill, []):
                    if pattern.startswith("__REVIEW_KIND__:") and \
                            pattern.split(":", 1)[1] in conv:
                        sat.add(step_n)
                missing = [n for n in missing if n not in sat]
            except Exception:
                pass
    if not missing:
        return 0

    # Bootstrap: the terminal "commit + push" step (19 for implement,
    # 17 for review) is satisfied BY the very Bash call we're gating.
    # The PostToolUse observer cannot record it before this PreToolUse
    # fires, so without this branch every section-ship requires a
    # SKIP_SKILL_STEP_BLOCK on the first attempt. Match the in-flight
    # Bash signature against the step map; if it would credit one or
    # more of the missing terminal steps, drop those from `missing`.
    if d.get("tool_name") == "Bash":
        from skill_step_map import match_step  # noqa: E402
        sig = (d.get("tool_input") or {}).get("command", "") or ""
        in_flight_steps = set(match_step(skill, "Bash", sig))
        if in_flight_steps:
            missing = [m for m in missing if m not in in_flight_steps]
            if not missing:
                return 0

    if _skip_envs.get("SKIP_SKILL_STEP_BLOCK", "") == "1":
        _log_skip(root, _skip_envs.get("SKIP_SKILL_STEP_BLOCK_REASON", ""),
                  skill, missing)
        return 0

    sys.stderr.write(
        "[skill-step-block] BLOCK -- skill `" + skill + "` is at commit/"
        "review-todo-section gate but step(s) " + str(missing) + " were "
        "never observed by the skill-step observer. Run them or set "
        "SKIP_SKILL_STEP_BLOCK=1 SKIP_SKILL_STEP_BLOCK_REASON=\"<text>\" "
        "(both required, reason >= 12 chars). The required terminal "
        "step list for `" + skill + "` is "
        + str(REQUIRED_TERMINAL_STEPS.get(skill, [])) + ". Steps "
        "observed so far: " + str(sorted(o.get("n") for o in observed
                                          if isinstance(o, dict))) + ". "
        "Doctrine: feedback_skill_invocation_drift memory + TODO-08 §10."
    )
    return 2


if __name__ == "__main__":
    sys.exit(main())
