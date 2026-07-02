#!/usr/bin/env python3
# block-via: exit-2 (PreToolUse gate -- BLOCKs the first Edit/Write on
# src/ or include/ when the matching domain code-quality skill has not
# been walked yet).
"""TODO-08 §20 step-5 quality-gate walk presence (PreToolUse Edit/Write).

When `implement-todo-section` is the active skill (per skill-progress.json)
and the next tool call is an Edit/Write/MultiEdit on a file under src/ or
include/, BLOCK if `tool-history.jsonl` shows no prior
`Skill(<domain>-code-quality, ...)` invocation matching the path's domain.

Path-to-skill map mirrors CLAUDE.md "Mandatory Skill Triggers":
    src/boot/                    -> boot-code-quality
    src/kernel/, include/kernel/ -> kernel-code-quality
    src/desktop/                 -> desktop-code-quality
    src/shell/                   -> shell-code-quality
    user/, src/apps/             -> userland-code-quality

Closes the recurring "kernel-code-quality not walked" drift named in
`feedback_skill_invocation_drift`. Bootstrap-mode bypass active when
this hook's own file is in the staged diff.

Opt-out:
    SKIP_QUALITY_GATE_BLOCK=1 SKIP_QUALITY_GATE_BLOCK_REASON="<text >= 12 chars>"
"""
import json
import os
# TODO-08 section-23: shared SKIP-env scanner.
from pathlib import Path as _SkipPath
_SKIP_HOOK_DIR = _SkipPath(__file__).resolve().parent
import sys as _SkipSys
if str(_SKIP_HOOK_DIR) not in _SkipSys.path:
    _SkipSys.path.insert(0, str(_SKIP_HOOK_DIR))
import _skip_env as _se  # noqa: E402
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

_HOOK_DIR = Path(__file__).resolve().parent
if str(_HOOK_DIR) not in sys.path:
    sys.path.insert(0, str(_HOOK_DIR))

_STATE_REL = ".claude/state/skill-progress.json"
_TOOL_HISTORY_REL = ".claude/state/tool-history.jsonl"
_SKIP_LOG_REL = ".claude/state/skip-log.jsonl"
_SKIP_REASON_MIN_LEN = 12

# Path-to-skill map. Order matters: longer prefixes before shorter so
# `src/kernel/test/` is classified as kernel before `src/`. Tuple of
# (path_prefix, required_skill_name).
_PATH_SKILL_MAP = (
    ("src/boot/", "boot-code-quality"),
    ("include/kernel/", "kernel-code-quality"),
    ("src/kernel/", "kernel-code-quality"),
    ("src/desktop/", "desktop-code-quality"),
    ("src/shell/", "shell-code-quality"),
    ("src/apps/", "userland-code-quality"),
    ("user/", "userland-code-quality"),
)


def _repo_root() -> Optional[str]:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip() or None
    except Exception:
        return None


def _ts_ns() -> int:
    return time.time_ns() if hasattr(time, "time_ns") else int(time.time() * 1e9)


def _required_skill_for_path(p: str) -> str:
    """Return the required code-quality skill name for `p`, or empty
    string if `p` does not match any tracked code domain."""
    if not isinstance(p, str) or not p:
        return ""
    norm = p.replace("\\", "/")
    # Strip any leading repo-root prefix (Edit may pass absolute paths).
    if "/src/" in norm:
        norm = "src/" + norm.split("/src/", 1)[1]
    elif "/include/" in norm:
        norm = "include/" + norm.split("/include/", 1)[1]
    elif "/user/" in norm and not norm.startswith("user/"):
        norm = "user/" + norm.split("/user/", 1)[1]
    for prefix, skill in _PATH_SKILL_MAP:
        if norm.startswith(prefix):
            return skill
    return ""


def _active_implement_skill_started_ts(root: str) -> int:
    """Return started_ts for the active implement-todo-section entry, or 0
    when none active or compaction-orphaned. Skipping orphan entries
    matches §16 / skill_step_block discipline."""
    state_path = os.path.join(root, _STATE_REL)
    try:
        with open(state_path, "r", encoding="utf-8") as f:
            state = json.load(f)
    except Exception:
        return 0
    if not isinstance(state, dict):
        return 0
    entry = state.get("implement-todo-section")
    if not isinstance(entry, dict):
        return 0
    if entry.get("compaction_orphaned") is True:
        return 0
    ts = entry.get("started_ts", 0)
    return ts if isinstance(ts, int) and ts > 0 else 0


def _active_implement_skill_entry(root: str) -> dict:
    state_path = os.path.join(root, _STATE_REL)
    try:
        with open(state_path, "r", encoding="utf-8") as f:
            state = json.load(f)
    except Exception:
        return {}
    if not isinstance(state, dict):
        return {}
    entry = state.get("implement-todo-section")
    if not isinstance(entry, dict) or entry.get("compaction_orphaned") is True:
        return {}
    ts = entry.get("started_ts", 0)
    if not isinstance(ts, int) or ts <= 0:
        return {}
    return entry


def _normalize_todo_path(path: str) -> str:
    norm = path.replace("\\", "/")
    if "todo/" in norm and not norm.startswith("todo/"):
        norm = "todo/" + norm.split("todo/", 1)[1]
    return norm


def _section_from_entry(entry: dict) -> str:
    raw = entry.get("section")
    if isinstance(raw, int):
        return str(raw)
    if isinstance(raw, str) and re.fullmatch(r"\d+", raw.strip()):
        return raw.strip()
    args = entry.get("args") or ""
    if not isinstance(args, str):
        return ""
    patterns = (
        r"--section\s+['\"]?(\d+)['\"]?",
        r"(?:section|§)\s*['\"]?(\d+)['\"]?",
        r"##\s*(\d+)\.",
    )
    for pattern in patterns:
        m = re.search(pattern, args, re.IGNORECASE)
        if m:
            return m.group(1)
    return ""


def _todo_from_entry(entry: dict) -> str:
    raw = entry.get("todo_path")
    if isinstance(raw, str) and raw:
        return _normalize_todo_path(raw)
    args = entry.get("args") or ""
    if isinstance(args, str):
        m = re.search(r"todo/[\w./-]+\.md", args)
        if m:
            return _normalize_todo_path(m.group(0))
    return ""


def _ai_workflow_state_dir(root: str) -> Path:
    env = os.environ.get("AI_WORKFLOW_STATE_DIR", "")
    if env:
        p = Path(env)
        if not p.is_absolute():
            p = Path(root) / p
        return p
    return Path(root) / ".ai-workflow"


def _auto_acquire_lease(root: str, todo: str, section: str, run_id: str) -> bool:
    """Acquire (or same-run renew) the shared lease for this session. lease.py
    refuses when another live holder owns the section, so this cannot steal."""
    try:
        rc = subprocess.run(
            [
                sys.executable, os.path.join(root, "scripts/ai-workflow/lease.py"),
                "acquire", "--todo", todo, "--section", section,
                "--driver", "claude", "--run-id", run_id,
            ],
            cwd=root, capture_output=True, timeout=15,
        ).returncode
    except Exception:
        return False
    return rc == 0


def _active_lease_allows_implement_edit(
    root: str, entry: dict, run_id: str
) -> tuple[bool, str]:
    """OWNERSHIP consult: the active lease must target this todo/section AND
    belong to this session's run-id. When no live lease exists and the session
    has an identity, auto-acquire it (TODO-10 lease-ownership item)."""
    todo = _todo_from_entry(entry)
    section = _section_from_entry(entry)
    if not todo or not section:
        return (
            False,
            "active implement-todo-section state lacks structured todo_path/section "
            "needed for driver-lease lookup",
        )
    lease_path = _ai_workflow_state_dir(root) / "active-lease.json"
    try:
        lease = json.loads(lease_path.read_text(encoding="utf-8"))
    except Exception:
        lease = {}
    live = (
        isinstance(lease, dict)
        and lease
        and int(lease.get("expires_at_ns") or 0) >= _ts_ns()
    )
    if not live:
        if run_id and _auto_acquire_lease(root, todo, section, run_id):
            return True, ""
        if isinstance(lease, dict) and lease:
            return False, f"active driver lease for {todo} section {section} is expired"
        return False, f"no active driver lease for {todo} section {section}"
    if lease.get("todo_path") != todo or str(lease.get("section")) != section:
        return (
            False,
            "active driver lease targets "
            f"{lease.get('todo_path', '<none>')} section {lease.get('section', '<none>')}, "
            f"not {todo} section {section}",
        )
    holder = str(lease.get("driver_run_id") or "")
    if run_id and holder != run_id:
        return (
            False,
            f"active driver lease for {todo} section {section} is OWNED by "
            f"{holder}, not this session ({run_id}); a second session must not "
            "edit under another holder's lease",
        )
    return True, ""


def _shared_gate_enforcement_enabled() -> bool:
    return os.environ.get("AI_WORKFLOW_ENFORCE_SHARED_GATES", "") == "1"


def _quality_skill_walked(root: str, started_ts: int, required_skill: str) -> bool:
    """Walk tool-history.jsonl forward from `started_ts` looking for any
    Skill(<required_skill>) event. Returns True on first match."""
    history_path = os.path.join(root, _TOOL_HISTORY_REL)
    if not os.path.exists(history_path):
        return False
    try:
        with open(history_path, "r", encoding="utf-8") as f:
            for line in f:
                try:
                    ev = json.loads(line)
                except Exception:
                    continue
                ts_ns = ev.get("ts_ns", 0)
                if not isinstance(ts_ns, int) or ts_ns < started_ts:
                    continue
                if ev.get("tool_name") != "Skill":
                    continue
                # tool-history.jsonl does not record tool_input fields.
                # Walk the transcript instead via transcript_path provided
                # in the hook payload (caller passes it through).
                # See main() -- this fast-path returns False when only
                # ts_ns metadata is present.
                pass
    except Exception:
        pass
    return False


def _quality_skill_walked_in_transcript(
    transcript_path: str, started_ts: int, required_skill: str
) -> bool:
    """Walk transcript JSONL forward from `started_ts`; True on first
    Skill tool_use whose `skill` or `name` equals `required_skill`."""
    if not transcript_path or not os.path.exists(transcript_path):
        return False
    try:
        with open(transcript_path, "r", encoding="utf-8") as f:
            for line in f:
                try:
                    ev = json.loads(line)
                except Exception:
                    continue
                # Best-effort timestamp; if absent, count as post-start
                # (the transcript stops being kept across compactions, so
                # everything in this file is likely current-session).
                ev_ts = ev.get("ts_ns")
                if isinstance(ev_ts, int) and ev_ts < started_ts:
                    continue
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
                    if c.get("name") != "Skill":
                        continue
                    raw = c.get("input")
                    inp = raw if isinstance(raw, dict) else {}
                    name = inp.get("skill") or inp.get("name") or ""
                    if name == required_skill:
                        return True
    except Exception:
        pass
    return False


def _log_skip(root: str, reason: str, required_skill: str) -> None:
    p = os.path.join(root, _SKIP_LOG_REL)
    try:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        rec = {
            "ts_ns": _ts_ns(),
            "kind": "SKIP_QUALITY_GATE_BLOCK",
            "reason": reason[:240],
            "required_skill": required_skill,
        }
        with open(p, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec) + "\n")
    except Exception:
        pass


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0

    tn = d.get("tool_name", "")
    if tn not in ("Edit", "Write", "MultiEdit"):
        return 0
    ti = d.get("tool_input") or {}
    target = ti.get("file_path") or ti.get("path") or ""
    required_skill = _required_skill_for_path(target)
    if not required_skill:
        return 0

    root = _repo_root()
    if not root:
        return 0

    # Bootstrap-mode: shipping this hook itself? downgrade to WARN.
    try:
        from _bootstrap_mode import is_bootstrap_commit
        if is_bootstrap_commit(__file__):
            sys.stderr.write(
                "[step5-quality-gate] WARN (bootstrap-mode) -- staged "
                "diff includes this hook's own file; gate downgraded to "
                "WARN for the commit that ships it.\n"
            )
            return 0
    except Exception:
        pass

    # Opt-out check.
    # TODO-08 section-23: shared SKIP-env scanner -- environ-only here
    # because step5_quality_gate is an Edit/Write hook (no Bash cmd).
    _skip_envs = _se.read_skip_envs(
        cmd="",
        keys=("SKIP_QUALITY_GATE_BLOCK", "SKIP_QUALITY_GATE_BLOCK_REASON"),
    )
    skip_req = _skip_envs.get("SKIP_QUALITY_GATE_BLOCK", "") == "1"
    skip_reason = _skip_envs.get("SKIP_QUALITY_GATE_BLOCK_REASON", "")
    if skip_req and len(skip_reason) < _SKIP_REASON_MIN_LEN:
        sys.stderr.write(
            "[step5-quality-gate] BLOCK -- opt-out malformed: "
            "SKIP_QUALITY_GATE_BLOCK=1 set but SKIP_QUALITY_GATE_BLOCK_REASON "
            f"missing or under {_SKIP_REASON_MIN_LEN} chars.\n"
        )
        return 2

    entry = _active_implement_skill_entry(root)
    started_ts = entry.get("started_ts", 0) if entry else 0
    if started_ts == 0:
        # No active implement-todo-section flow; gate not applicable.
        return 0

    session_id = str(d.get("session_id") or "")
    session_run_id = f"claude-{session_id}" if session_id else ""
    lease_ok, lease_err = _active_lease_allows_implement_edit(root, entry, session_run_id)
    if not lease_ok:
        if _shared_gate_enforcement_enabled():
            sys.stderr.write(
                "[step5-quality-gate] BLOCK -- active implement-todo-section "
                f"has no matching driver lease: {lease_err}.\n"
                "[step5-quality-gate]   Acquire the shared lease first, e.g. "
                "`python3 scripts/ai-workflow/lease.py acquire --todo <todo> "
                "--section <n> --driver claude --run-id <run>`.\n"
                "[step5-quality-gate]   Set only by repo-owned drivers; do not "
                "freehand stamps or bypass the lease.\n"
            )
            return 2
        sys.stderr.write(
            "[step5-quality-gate] WARN -- active implement-todo-section "
            f"has no matching driver lease: {lease_err}. Shared lease "
            "blocking is not default-on until Claude lease acquisition ships.\n"
        )

    transcript_path = d.get("transcript_path", "")
    if _quality_skill_walked_in_transcript(transcript_path, started_ts, required_skill):
        return 0

    if skip_req:
        _log_skip(root, skip_reason, required_skill)
        sys.stderr.write(
            f"[step5-quality-gate] SKIP allowed -- reason: {skip_reason}\n"
        )
        return 0

    sys.stderr.write(
        f"[step5-quality-gate] BLOCK -- editing {target} requires "
        f"prior `Skill(name=\"{required_skill}\")` invocation per "
        f"implement-todo-section step 5 (domain code-quality gate "
        f"walk). No matching Skill event recorded since the active "
        f"implement-todo-section started.\n"
        f"[step5-quality-gate]   Run `Skill(name=\"{required_skill}\")` "
        f"and walk all gates against the planned change before retrying "
        f"the Edit.\n"
        f"[step5-quality-gate]   Opt-out (rare): "
        f"SKIP_QUALITY_GATE_BLOCK=1 "
        f"SKIP_QUALITY_GATE_BLOCK_REASON=\"<text >= 12 chars>\".\n"
        f"[step5-quality-gate]   Doctrine: feedback_skill_invocation_drift.\n"
    )
    return 2


if __name__ == "__main__":
    sys.exit(main())
