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

import json
import os
import re
import subprocess
import sys
import time
from typing import Dict, List, Optional, Tuple
from pathlib import Path


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
    os.makedirs(os.path.dirname(state_path), exist_ok=True)
    tmp = state_path + ".tmp"
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, sort_keys=True)
        os.replace(tmp, state_path)
    except Exception:
        try:
            os.unlink(tmp)
        except Exception:
            pass


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
    state = _load_state(state_path)

    # Branch A: this tool call IS a multi-step Skill invocation -- start
    # (or restart) the state entry for that skill. Per Q1+Q2: latest
    # invocation wins; the started_head_sha pins the invalidation key.
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
        state[fresh_skill] = {
            "started_ts": _ts_ns(),
            "started_head_sha": head,
            "session_id": sid,
            "steps_observed": [],
            "args": (d.get("tool_input") or {}).get("args", ""),
        }
        _save_state(state_path, state)
        return 0

    # Branch B: the tool call is something else -- check whether it is a
    # step-evidence event for the most-recent active multi-step skill.
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
    # observed-from-payload, e.g. observer started mid-session).
    entry = state.get(skill)
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

    _save_state(state_path, state)
    return 0


if __name__ == "__main__":
    sys.exit(main())
