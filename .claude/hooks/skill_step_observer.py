#!/usr/bin/env python3
# block-via: warning-only (STATE hook -- writes the skill-progress state
# file, never blocks; the BLOCK partner is skill_step_block.py which reads
# this hook's output).
"""PostToolUse skill-step observer (TODO-08 §10).

Walks the session transcript backward to find the most recent invocation
of one of the multi-step skills tracked by skill_step_map. If the current
tool call's signature matches a known step-evidence rule for that skill,
appends a `steps_observed` entry to .claude/state/skill-progress.json.

State file shape (per-skill, latest invocation wins):

    {
      "<skill_name>": {
        "started_ts": <int ns>,
        "started_head_sha": "<sha>",
        "session_id": "<from transcript SessionStart event if available>",
        "steps_observed": [
          {"n": <int>, "observed_ts": <int ns>,
           "evidence_tool": "Bash|Edit|Write|MultiEdit|Skill",
           "evidence_token": "<sha or path>"}
        ]
      },
      ...
    }

Per Codex design review 2026-04-28 Q1: state invalidation is keyed on
`started_head_sha` (HEAD at the moment the skill was invoked), NOT on
current HEAD. Repeated git commits within one skill flow keep the state
valid; only a fresh top-level Skill invocation rotates the entry.

Hook category: STATE. Never blocks. The blocker partner reads this file.
Wrap.sh-eligible: NO -- the observer needs to walk transcript on every
tool call to record the observed step on the right skill's state.
"""

import contextlib
import json
import os
import re
import subprocess
import sys
import time
from typing import Dict, List, Optional, Tuple
from pathlib import Path

try:
    import fcntl  # type: ignore[import]
    _HAVE_FLOCK = True
except ImportError:
    fcntl = None  # type: ignore[assignment]
    _HAVE_FLOCK = False


_HOOK_DIR = Path(__file__).resolve().parent
if str(_HOOK_DIR) not in sys.path:
    sys.path.insert(0, str(_HOOK_DIR))

# pylint: disable=wrong-import-position
from skill_step_map import (  # noqa: E402
    MULTI_STEP_SKILLS,
    SKILL_STEP_MAP,
    match_step,
)


_STATE_REL = ".claude/state/skill-progress.json"


def _repo_root() -> Optional[str]:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
        return out or None
    except Exception:
        return None


def _head_sha(root: str) -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"],
            cwd=root, text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return ""


def _scan_transcript_for_active_skill(
    path: str,
) -> Tuple[Optional[str], Optional[str], Optional[str]]:
    """Walk the transcript JSONL to find the most-recent Skill invocation
    that names a multi-step skill. Return (skill_name, args, session_id).
    session_id is best-effort (transcripts include it on some events).
    """
    if not path or not os.path.exists(path):
        return (None, None, None)
    try:
        f = open(path, encoding="utf-8")
    except OSError:
        return (None, None, None)
    last_skill: Optional[str] = None
    last_args: Optional[str] = None
    last_session: Optional[str] = None
    with f:
        for line in f:
            try:
                ev = json.loads(line)
            except Exception:
                continue
            sid = ev.get("session_id") or ev.get("sessionId")
            if isinstance(sid, str) and sid:
                last_session = sid
            msg = ev.get("message")
            if not isinstance(msg, dict):
                continue
            content = msg.get("content")
            if not isinstance(content, list):
                continue
            for c in content:
                if not isinstance(c, dict):
                    continue
                if c.get("type") != "tool_use":
                    continue
                name = c.get("name") or ""
                inp = c.get("input") or {}
                if name != "Skill":
                    continue
                skill = inp.get("skill") or inp.get("name") or ""
                if skill in MULTI_STEP_SKILLS:
                    last_skill = skill
                    last_args = inp.get("args", "")
    return (last_skill, last_args, last_session)


def _signature_for_tool(tool_name: str, ti: Dict) -> str:
    """Extract the signature string the step map regexes match against."""
    if tool_name == "Bash":
        return (ti.get("command") or "")
    if tool_name in ("Edit", "Write", "MultiEdit"):
        return (ti.get("file_path") or ti.get("path") or "")
    if tool_name == "Skill":
        return (ti.get("skill") or ti.get("name") or "")
    return ""


def _evidence_token(tool_name: str, ti: Dict) -> str:
    """Short token captured for traceability. Truncated to 200 chars."""
    if tool_name == "Bash":
        cmd = ti.get("command") or ""
        return cmd[:200]
    if tool_name in ("Edit", "Write", "MultiEdit"):
        return (ti.get("file_path") or ti.get("path") or "")[:200]
    if tool_name == "Skill":
        return (ti.get("skill") or ti.get("name") or "")[:200]
    return ""


def _load_state(state_path: str) -> Dict:
    try:
        with open(state_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            if isinstance(data, dict):
                return data
    except Exception:
        pass
    return {}


def _save_state(state_path: str, data: Dict) -> None:
    """Atomic write of skill-progress.json. TODO-08 §16 Codex H1 fix
    (2026-04-28): per-process tmp filename via os.getpid() + ns
    timestamp prevents two concurrent observers from clobbering each
    other's tmp file. Combined with _with_state_lock() in main() this
    closes the parallel-PostToolUse race that could lose post-
    compaction step evidence.
    """
    os.makedirs(os.path.dirname(state_path), exist_ok=True)
    tmp = state_path + ".tmp." + str(os.getpid()) + "." + str(_ts_ns())
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, sort_keys=True)
        os.replace(tmp, state_path)
    except Exception:
        try:
            os.unlink(tmp)
        except Exception:
            pass


@contextlib.contextmanager
def _with_state_lock(state_path: str):
    """TODO-08 §16 Codex H1 fix (2026-04-28): hold an exclusive
    advisory lock on `<state_path>.lock` while reading-modifying-
    writing skill-progress.json. Two concurrent PostToolUse observers
    used to race here -- both archived the same orphaned entry, both
    created a fresh entry, last write wins, evidence loss. The lock
    serializes the entire RMW span; per-process tmp filename in
    _save_state covers the file-system-visible portion.

    Falls open on platforms without fcntl (Windows hosts running
    Claude Code natively): the lock is a no-op and the per-process
    tmp filename is the only safety net. Acceptable because:
    Windows-side parallel hooks are rare (the harness serializes
    most tool calls), and the tmp filename uniqueness still prevents
    cross-process tmp clobbering.
    """
    if not _HAVE_FLOCK:
        yield
        return
    os.makedirs(os.path.dirname(state_path), exist_ok=True)
    lock_path = state_path + ".lock"
    fd = None
    try:
        fd = os.open(
            lock_path,
            os.O_RDWR | os.O_CREAT,
            0o600,
        )
        fcntl.flock(fd, fcntl.LOCK_EX)
        yield
    finally:
        if fd is not None:
            try:
                fcntl.flock(fd, fcntl.LOCK_UN)
            except Exception:
                pass
            try:
                os.close(fd)
            except Exception:
                pass


def _archive_orphan_in_place(state: Dict, skill: str) -> None:
    """TODO-08 §16 Codex M3 fix (2026-04-28): if state[skill] is
    a dict with `compaction_orphaned: true`, move it to
    `<skill>.orphan.<orphan_ts_ns>` (collision-safe with a counter
    suffix if the archive key already exists). Caller can then
    safely write a fresh entry under `state[skill]`.

    Used by both Branch A (fresh Skill invocation) and Branch B
    (lazy entry init when an active flow needs to record steps).
    Without Branch A coverage, a fresh implement-todo-section
    invocation post-compaction would clobber the orphaned entry
    before either the blocker skip log or the observer archive
    path could preserve it -- audit-trail loss exactly when
    compaction resilience is being exercised.
    """
    existing = state.get(skill)
    if not isinstance(existing, dict):
        return
    if existing.get("compaction_orphaned") is not True:
        return
    base = skill + ".orphan." + str(existing.get("orphan_ts_ns", _ts_ns()))
    key = base
    suffix = 0
    while key in state:
        suffix += 1
        key = base + "." + str(suffix)
    state[key] = existing


def _ts_ns() -> int:
    return time.time_ns() if hasattr(time, "time_ns") else int(time.time() * 1e9)


def _is_skill_invocation_event(ev: Dict, skill: str) -> bool:
    msg = ev.get("message")
    if not isinstance(msg, dict):
        return False
    content = msg.get("content")
    if not isinstance(content, list):
        return False
    for c in content:
        if not isinstance(c, dict) or c.get("type") != "tool_use":
            continue
        if c.get("name") == "Skill":
            inp = c.get("input") or {}
            if (inp.get("skill") or inp.get("name") or "") == skill:
                return True
    return False


def _is_skill_invocation_in_payload(d: Dict, skill_set: frozenset) -> Optional[str]:
    """Check if the CURRENT tool call IS the multi-step Skill invocation
    that should reset/start a fresh state entry."""
    if d.get("tool_name") != "Skill":
        return None
    inp = d.get("tool_input") or {}
    name = inp.get("skill") or inp.get("name") or ""
    return name if name in skill_set else None


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0

    root = _repo_root()
    if not root:
        return 0

    state_path = os.path.join(root, _STATE_REL)
    # TODO-08 §16 Codex H1 fix (2026-04-28): hold the state lock for
    # the entire read-modify-write span so two concurrent observers
    # cannot race and lose post-compaction step evidence.
    with _with_state_lock(state_path):
        state = _load_state(state_path)

        # Branch A: this tool call IS a multi-step Skill invocation --
        # start (or restart) the state entry for that skill. Per Q1+Q2:
        # latest invocation wins; the started_head_sha pins the
        # invalidation key.
        fresh_skill = _is_skill_invocation_in_payload(d, MULTI_STEP_SKILLS)
        if fresh_skill:
            head = _head_sha(root)
            sid = ""
            # Best-effort session_id from transcript.
            tp = d.get("transcript_path", "")
            try:
                if tp and os.path.exists(tp):
                    with open(tp, "r", encoding="utf-8") as f:
                        for line in f:
                            try:
                                ev = json.loads(line)
                            except Exception:
                                continue
                            s = ev.get("session_id") or ev.get("sessionId")
                            if isinstance(s, str) and s:
                                sid = s
            except Exception:
                pass
            # Codex consistency M3 fix (2026-04-28): preserve any
            # orphaned entry under the same skill name before the
            # fresh write -- the unconditional state[fresh_skill]=
            # {...} on line ~245 used to clobber post-compaction
            # audit state. Use the same archive-and-rotate path as
            # Branch B's lazy init.
            _archive_orphan_in_place(state, fresh_skill)
            args_str = (d.get("tool_input") or {}).get("args", "")
            # TODO-08 §17 deferred-XREF M4 fix: record structured
            # todo_path so _attribute_review_todo prefers it over a
            # loose regex of `args`.
            todo_path = ""
            if isinstance(args_str, str) and args_str:
                m = re.search(r"todo/[\w./-]+TODO-\d[\w./-]*\.md", args_str)
                if m:
                    todo_path = m.group(0)
            # TODO-08 §22 partial retraction (2026-04-29): RISK_TIER
            # doctrine removed -- every section runs the full pipeline
            # (design + adversarial + consistency + perf + conditional
            # re-adversarial + fix loop). The tier classifier removed
            # value from the implement-todo-section skill enforcement.
            # Spiral check retained with uniform 60-min budget; bootstrap-
            # mode and state-file diagnostics retained.
            state[fresh_skill] = {
                "started_ts": _ts_ns(),
                "started_head_sha": head,
                "session_id": sid,
                "steps_observed": [],
                "args": args_str,
                "todo_path": todo_path,
                "spiral_check_emitted": 0,  # escalation count: 0->2x->4x of 60 min
            }
            _save_state(state_path, state)
            return 0

        # Branch B: the tool call is something else -- check whether it is a
        # step-evidence event for the most-recent active multi-step skill.
        #
        # Codex perf review 2026-04-28 H1: avoid walking the transcript on
        # every PostToolUse. The active skill state lives in skill-progress
        # .json (written by Branch A above when a fresh Skill invocation
        # fires). Pick the most-recently-started entry there instead of
        # rescanning N MiB of JSONL on every tool call. Transcript walk is
        # ONLY used as a fallback when the state file has no recent entry
        # (observer started mid-session, state cleared by SessionStart, etc).
        skill = None
        args = ""
        sid = ""
        if isinstance(state, dict) and state:
            best_ts = -1
            for name, entry in state.items():
                if not isinstance(entry, dict):
                    continue
                if name not in MULTI_STEP_SKILLS:
                    continue
                # TODO-08 §16: skip orphaned entries. Without this skip the
                # observer would keep recording post-compaction step evidence
                # into an orphaned entry while the §10 step-block hook (which
                # also skips orphans) ignores it -- creating an enforcement
                # bypass where post-compaction commits proceed without any
                # active-skill gate. Symmetric with skill_step_block.py.
                if entry.get("compaction_orphaned") is True:
                    continue
                ts = entry.get("started_ts", 0)
                if isinstance(ts, int) and ts > best_ts:
                    best_ts = ts
                    skill = name
                    args = entry.get("args", "")
                    sid = entry.get("session_id", "")
        if not skill:
            # Fallback: no state entry (or all entries orphaned). One
            # transcript walk to bootstrap from the resumed flow's most-
            # recent Skill(implement-todo-section/...) invocation.
            transcript = d.get("transcript_path", "")
            skill, args, sid = _scan_transcript_for_active_skill(transcript)
        if not skill:
            return 0
        tn = d.get("tool_name", "")
        ti = d.get("tool_input", {}) or {}
        sig = _signature_for_tool(tn, ti)
        if not sig:
            return 0

        matched = match_step(skill, tn, sig)
        if not matched:
            return 0

        # Lazy state init for the skill if missing (transcript-walked but not
        # observed-from-payload, e.g. observer started mid-session). TODO-08
        # §16: if the existing entry under this skill name is orphaned,
        # archive it via the collision-safe helper (Codex re-adversarial
        # M4 fix 2026-04-28: was an inline assignment that could
        # silently overwrite a prior archive with the same orphan_ts_ns;
        # _archive_orphan_in_place uses a counter suffix). Without this
        # rotation the observer would write new step evidence into the
        # orphaned entry, which §10 + §16 selector skips ignore -- the
        # resumed flow would have NO active gate.
        _archive_orphan_in_place(state, skill)
        entry = state.get(skill)
        if isinstance(entry, dict) and entry.get("compaction_orphaned") is True:
            # Helper only archives if the entry is orphaned; if it
            # remained, force fresh-entry creation below.
            entry = None
        if not isinstance(entry, dict):
            entry = {
                "started_ts": _ts_ns(),
                "started_head_sha": _head_sha(root),
                "session_id": sid or "",
                "steps_observed": [],
                "args": args or "",
            }
            state[skill] = entry

        so = entry.setdefault("steps_observed", [])
        if not isinstance(so, list):
            so = []
            entry["steps_observed"] = so
        token = _evidence_token(tn, ti)
        now = _ts_ns()
        seen = {x.get("n") for x in so if isinstance(x, dict)}
        for n in matched:
            if n in seen:
                continue
            so.append({
                "n": n,
                "observed_ts": now,
                "evidence_tool": tn,
                "evidence_token": token,
            })
            seen.add(n)

        # TODO-08 §22 #5 spiral check: compare elapsed wall-clock vs
        # tier budget. Per Codex design pointer 3, persist an
        # escalation counter so the reminder fires at 2x and 4x but
        # not on every PostToolUse in between (chat flood).
        _spiral_check(entry, skill)

        _save_state(state_path, state)
        return 0


# Uniform skill wall-clock budget (60 min) per TODO-08 §22 partial
# retraction (2026-04-29). The earlier RISK_TIER doctrine right-sized
# Codex pipelines per-tier and was retracted because the classifier
# undermined implement-todo-section enforcement; spiral check retained
# at a single uniform budget so the skill flags long-running flows
# without right-sizing reviews away.
_SPIRAL_BUDGET_NS = 60 * 60 * 1_000_000_000


def _spiral_check(entry: Dict, skill: str) -> None:
    """Emit a one-shot stderr `systemMessage` when elapsed wall-clock
    exceeds 2x or 4x the uniform 60-min budget. Pure advisory; never
    blocks. Persists escalation counter to dedup re-emissions.
    """
    started_ts = entry.get("started_ts", 0)
    if not isinstance(started_ts, int) or started_ts <= 0:
        return
    budget = _SPIRAL_BUDGET_NS
    elapsed = _ts_ns() - started_ts
    emitted = entry.get("spiral_check_emitted", 0)
    if not isinstance(emitted, int):
        emitted = 0
    new_level = emitted
    if elapsed > 4 * budget and emitted < 2:
        new_level = 2
        threshold = "4x"
    elif elapsed > 2 * budget and emitted < 1:
        new_level = 1
        threshold = "2x"
    else:
        return
    elapsed_min = elapsed / 60_000_000_000
    budget_min = budget / 60_000_000_000
    sys.stderr.write(
        f"[skill-step-observer] SPIRAL CHECK -- skill `{skill}` "
        f"elapsed {elapsed_min:.0f} min, {threshold} of {budget_min:.0f} "
        f"min budget. Consider: declare the section done, split it, or "
        f"document why the work is genuinely larger than 60 min. Pure "
        f"advisory; this does not block.\n"
    )
    entry["spiral_check_emitted"] = new_level


if __name__ == "__main__":
    sys.exit(main())
