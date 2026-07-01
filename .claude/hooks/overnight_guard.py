#!/usr/bin/env python3
"""Overnight TODO-run guard.

Marks an active overnight implementation run, injects per-tool flow
reminders, and BLOCKS the Stop event (exit 2) so Claude does not
final-answer between section ships. Cleared explicitly by the user
("pause the overnight"), by running the script with `clear`, or
automatically when the active TODO file's Implementation Order is fully [x].

Hook modes (argv[0]):
  prompt      -- UserPromptSubmit; activate or pause based on phrasing.
  post-tool   -- PostToolUse; emit reminder while state is active.
  stop        -- Stop; emit sentinel + exit 2 while state is active.

CLI modes (manual):
  start <todo-path>
  clear "<reason>"
  status
"""

from __future__ import annotations

import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
STATE_DIR = REPO_ROOT / ".claude" / "state"
STATE_FILE = STATE_DIR / "overnight-run.json"


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def hook_stdin() -> str:
    try:
        return sys.stdin.read()
    except OSError:
        return ""


def flatten_strings(value: Any) -> list[str]:
    if isinstance(value, str):
        return [value]
    if isinstance(value, dict):
        out: list[str] = []
        for v in value.values():
            out.extend(flatten_strings(v))
        return out
    if isinstance(value, list):
        out = []
        for v in value:
            out.extend(flatten_strings(v))
        return out
    return []


def decode_payload(raw: str) -> str:
    if not raw.strip():
        return ""
    try:
        return "\n".join(flatten_strings(json.loads(raw)))
    except json.JSONDecodeError:
        return raw


TODO_PATTERNS = [
    r"(/home/[^\s`'\"\)]+/todo/\d{2}-[a-z0-9-]+/TODO-\d{2}-[^\s`'\"\)]+\.md)",
    r"(\\\\wsl\.localhost\\Ubuntu\\home\\[^\s`'\"\)]+\\todo\\\d{2}-[a-z0-9-]+\\TODO-\d{2}-[^\s`'\"\)]+\.md)",
    r"(todo/\d{2}-[a-z0-9-]+/TODO-\d{2}-[a-zA-Z0-9._-]+\.md)",
]


def extract_todo_file(text: str) -> str | None:
    files = extract_todo_files(text)
    return files[0] if files else None


def normalize_todo_path(path: str) -> str:
    """Strip absolute / WSL UNC prefixes so the path is repo-relative."""
    p = path.replace("\\", "/")
    marker = "/todo/"
    idx = p.find(marker)
    if idx >= 0:
        return p[idx + 1:]
    return p


def extract_todo_files(text: str) -> list[str]:
    """Return all TODO paths in `text`, in order of first appearance, de-duped."""
    found: list[tuple[int, str]] = []
    for pat in TODO_PATTERNS:
        for m in re.finditer(pat, text):
            found.append((m.start(), m.group(1)))
    seen: set[str] = set()
    out: list[str] = []
    for _, raw in sorted(found, key=lambda x: x[0]):
        norm = normalize_todo_path(raw)
        if norm in seen:
            continue
        seen.add(norm)
        out.append(norm)
    return out


ACTIVATION_PHRASES = (
    "start overnight",
    "start the overnight",
    "begin overnight",
    "begin the overnight",
    "run overnight",
    "run the overnight",
    "run an overnight",
    "kick off overnight",
    "kick off the overnight",
    "launch overnight",
    "launch the overnight",
    "resume overnight",
    "resume the overnight",
    "carry on overnight",
    "carry on with overnight",
)

META_MARKERS = (
    "[overnight-guard",
    "[overnight-guard:",
    "overnight-guard.py",
    "stop hook feedback",
    "session-resume]",
    "stop sentinel",
)


def activates_overnight(text: str) -> bool:
    """Activate ONLY on explicit imperative phrasing; ignore pasted hook output / docs.

    Rationale: words "overnight" + "todo" appear too easily in meta-discussion --
    quoting hook output, asking about the feature, drafting docs, etc. Earlier the
    user pasted a Stop-hook output that contained those words verbatim and the
    hook re-activated state behind their back. Activation must require a concrete
    imperative phrase the user wouldn't type by accident.
    """
    low = text.lower()
    # Bail if the text is clearly pasted hook output / docs, not a user request.
    for marker in META_MARKERS:
        if marker in low:
            return False
    return any(phrase in low for phrase in ACTIVATION_PHRASES)


def pauses_overnight(text: str) -> bool:
    low = text.lower()
    for phrase in (
        "pause the overnight",
        "stop the overnight",
        "cancel the overnight",
        "end the overnight",
        "pause overnight",
        "stop overnight",
        "cancel overnight",
        "end overnight",
    ):
        if phrase in low:
            return True
    return False


def load_state() -> dict[str, Any] | None:
    if not STATE_FILE.exists():
        return None
    try:
        st = json.loads(STATE_FILE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return st if st.get("active") else None


def save_state(
    todo_file: str | None,
    source: str,
    queue: list[str] | None = None,
) -> dict[str, Any]:
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    existing = load_state()
    if existing is None:
        state = {
            "active": True,
            "todo_file": todo_file or "",
            "queue": queue or [],
            "completed": [],
            "source": source,
            "started_at": now_iso(),
            "updated_at": now_iso(),
        }
    else:
        state = dict(existing)
        state.setdefault("queue", [])
        state.setdefault("completed", [])
        if todo_file:
            state["todo_file"] = todo_file
        if queue is not None:
            # Append new queue entries that aren't already current/queued/completed.
            current = {state.get("todo_file", ""), *state["queue"], *state["completed"]}
            for q in queue:
                if q and q not in current:
                    state["queue"].append(q)
                    current.add(q)
        state["source"] = source
        state["updated_at"] = now_iso()
    STATE_FILE.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    return state


def advance_queue(state: dict[str, Any]) -> dict[str, Any] | None:
    """Promote next queued TODO to todo_file. Return new state, or None if queue empty.

    Skips queue entries that are already exhausted on disk.
    """
    current = state.get("todo_file", "")
    queue = list(state.get("queue", []))
    completed = list(state.get("completed", []))
    if current and current not in completed:
        completed.append(current)
    while queue:
        nxt = queue.pop(0)
        if nxt == current or nxt in completed:
            continue
        # If the next TODO is already shipped, mark it completed and keep advancing.
        if file_is_exhausted(nxt):
            completed.append(nxt)
            continue
        new_state = dict(state)
        new_state["todo_file"] = nxt
        new_state["queue"] = queue
        new_state["completed"] = completed
        new_state["updated_at"] = now_iso()
        STATE_FILE.write_text(json.dumps(new_state, indent=2) + "\n", encoding="utf-8")
        return new_state
    return None


def clear_state(reason: str) -> None:
    if STATE_FILE.exists():
        STATE_FILE.unlink()
    print(f"[overnight-guard] cleared: {reason}", file=sys.stderr)


def file_is_exhausted(todo_file: str) -> bool:
    """Return True when the TODO's Implementation Order table has no remaining [ ] rows."""
    path = REPO_ROOT / todo_file
    if not path.exists():
        return False
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return False
    in_table = False
    saw_row = False
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("## Implementation Order"):
            in_table = True
            continue
        if in_table:
            if s.startswith("## "):
                break
            if s.startswith("|") and "[ ]" in s:
                return False
            if s.startswith("|") and "[x]" in s:
                saw_row = True
    return saw_row


QUEUE_REMINDER_TEMPLATE = "\nQueue: {n} more TODO file(s) after this:\n  - {paths}"
COMPLETED_REMINDER_TEMPLATE = "\nCompleted earlier in this run: {n} file(s)."


def queue_footer(state: dict[str, Any]) -> str:
    parts = []
    queue = state.get("queue") or []
    completed = state.get("completed") or []
    if queue:
        parts.append(QUEUE_REMINDER_TEMPLATE.format(n=len(queue), paths="\n  - ".join(queue)))
    if completed:
        parts.append(COMPLETED_REMINDER_TEMPLATE.format(n=len(completed)))
    return "".join(parts)


REMINDER_TEMPLATE = """[overnight-guard] active TODO run: {todo}{footer}
One slice = one section ship through /implement-todo-section.

Per-section loop:
  1. Skill(implement-todo-section) on the next [ ] row in Implementation Order
     (Codex design + adversarial + consistency + perf, fix loop, build,
      smoke test, tests, validate, commit, push -- section_commit_gate enforces).
  2. Skill(review-todo-section) immediately after the section-ship commit
     (mandatory per CLAUDE.md; emits the Verified: stamp).
  3. Reread the TODO file -- gap-audit XREFs may have grown it.
  4. Continue to the next unblocked section in Implementation Order.

Stop ONLY when:
  - Every section in Implementation Order is [x] AND the queue is empty
    (current file exhausted -- run Skill(complete-todo-file) for loose-end
    sweep + closure commit, then the guard auto-advances to the next queued
    TODO if any), OR
  - The user explicitly paused/stopped the run, OR
  - Every remaining [ ] section is concretely blocked (Inputs missing,
    prerequisite TODO not shipped, hardware unavailable) AND each is filed
    Deferred with a named XREF target before stopping.

Never final-answer during an active overnight run while unblocked work remains."""


def reminder_for(state: dict[str, Any]) -> str:
    todo = state.get("todo_file") or "the active TODO file"
    return REMINDER_TEMPLATE.format(todo=todo, footer=queue_footer(state))


def maybe_advance(state: dict[str, Any]) -> dict[str, Any] | None:
    """If current TODO is exhausted, advance to next queued. Return current state or None if run finished.

    Never auto-advances while the run is paused on error -- the agent must
    diagnose and either fix or explicitly resume / clear before progress.
    """
    if state.get("paused"):
        return state
    todo = state.get("todo_file") or ""
    if not todo or not file_is_exhausted(todo):
        return state
    nxt = advance_queue(state)
    if nxt is not None:
        print(
            f"[overnight-guard] {todo} exhausted; advancing to {nxt['todo_file']}",
            file=sys.stderr,
        )
        return nxt
    clear_state(f"queue exhausted (last file: {todo})")
    return None


def pause_state(reason: str) -> dict[str, Any] | None:
    """Mark the active run paused-on-error. Stop hook stops blocking; agent must resume or clear."""
    state = load_state()
    if state is None:
        print("[overnight-guard] no active run to pause", file=sys.stderr)
        return None
    state["paused"] = True
    state["pause_reason"] = reason or "unspecified"
    state["paused_at"] = now_iso()
    state["updated_at"] = now_iso()
    STATE_FILE.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    print(f"[overnight-guard] paused: {state['pause_reason']}", file=sys.stderr)
    return state


def resume_state() -> dict[str, Any] | None:
    """Clear the paused flag and reactivate the loop."""
    state = load_state()
    if state is None:
        print("[overnight-guard] no active run to resume", file=sys.stderr)
        return None
    state.pop("paused", None)
    state.pop("pause_reason", None)
    state.pop("paused_at", None)
    state["updated_at"] = now_iso()
    STATE_FILE.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    print("[overnight-guard] resumed", file=sys.stderr)
    return state


def handle_prompt() -> int:
    text = decode_payload(hook_stdin())
    if pauses_overnight(text):
        clear_state("user paused overnight run via prompt")
        return 0
    if activates_overnight(text):
        files = extract_todo_files(text)
        if not files:
            # Bare activation phrase with NO concrete TODO file named -- this is
            # interactive discussion of overnight runs ("if I start the overnight
            # run..."), not a real start. Do not arm: arming with an empty todo_file
            # is what false-traps the session at Stop. A real start names a TODO file
            # (or uses the CLI `start` mode / the sequencer's arm-sequencer.sh).
            return 0
        first = files[0]
        rest = files[1:] if len(files) > 1 else []
        state = save_state(first, "UserPromptSubmit", queue=rest)
        print(reminder_for(state), file=sys.stderr)
    return 0


def handle_post_tool() -> int:
    state = load_state()
    if state is None:
        text = decode_payload(hook_stdin())
        if activates_overnight(text):
            files = extract_todo_files(text)
            first = files[0] if files else None
            rest = files[1:] if len(files) > 1 else []
            state = save_state(first, "PostToolUse", queue=rest)
    if state is None:
        return 0
    state = maybe_advance(state)
    if state is None:
        return 0
    print(reminder_for(state), file=sys.stderr)
    return 0


def handle_stop() -> int:
    state = load_state()
    if state is None:
        return 0
    state = maybe_advance(state)
    if state is None:
        return 0
    # Paused-on-error: do NOT block. Surface the pause so the agent can final-answer with the diagnosis.
    if state.get("paused"):
        print(
            f"[overnight-guard] paused-on-error: {state.get('pause_reason', 'unspecified')}\n"
            f"Active TODO: {state.get('todo_file', '')}\n"
            "Stop not blocked. Final-answer with the diagnosis (what failed, where, and what the "
            "next step is). Resume only after the root cause is fixed:\n"
            "  python3 .claude/hooks/overnight_guard.py resume",
            file=sys.stderr,
        )
        return 0
    # False-arm guard: a real overnight run always targets a concrete TODO file.
    # An empty todo_file means the state was armed by a bare activation phrase in an
    # interactive prompt (e.g. discussing overnight runs), not an actual run -- clear
    # it and do NOT trap the session. This is the documented interactive false-trap.
    if not (state.get("todo_file") or "").strip():
        clear_state("false-arm: no concrete todo_file (armed by a prompt keyword, not a real run)")
        return 0
    todo = state.get("todo_file") or "the active TODO file"
    msg = reminder_for(state) + (
        "\n\n[overnight-guard: Stop sentinel]\n"
        f"Found active overnight state at {STATE_FILE}.\n"
        "If this Stop is between section ships, do not final-answer.\n"
        f"Reread {todo} and start the next [ ] section.\n"
        "\n"
        "HALT-ON-ERROR: if you reached this Stop because a build, unit test, smoke test,\n"
        "Codex review fix loop, or section_commit_gate FAILED -- do NOT advance to the\n"
        "next section. Pause the run and final-answer with the diagnosis:\n"
        "  python3 .claude/hooks/overnight_guard.py pause \"smoke test crashed in TODO-09 section 4\"\n"
        "\n"
        "To intentionally end the run, clear the marker:\n"
        "  python3 .claude/hooks/overnight_guard.py clear \"reason\"\n"
        "  -- or say 'pause the overnight run' in your next prompt."
    )
    print(msg, file=sys.stderr)
    return 2


def handle_session_start() -> int:
    """Emit a resume reminder on a fresh session if a run is active.

    Lets a new Claude Code session (started manually or by cron after a usage-limit
    reset) pick up an overnight run without the user having to re-prompt.
    """
    state = load_state()
    if state is None:
        return 0
    state = maybe_advance(state)
    if state is None:
        return 0
    todo = state.get("todo_file") or "the active TODO file"
    started = state.get("started_at", "")
    if state.get("paused"):
        print(
            f"""[overnight-guard: session-resume -- PAUSED ON ERROR]
Active overnight run detected (started {started}).
Current TODO: {todo}
Pause reason: {state.get('pause_reason', 'unspecified')}
Paused at: {state.get('paused_at', '')}

The previous session paused the run because of an error. Before continuing:
  1. Read the pause reason above and the last commit + logs in this repo.
  2. Diagnose the root cause -- do NOT advance, do NOT skip the failing step.
  3. Fix the root cause and verify (rerun the failing build/test/smoke check).
  4. Only then resume:
       python3 .claude/hooks/overnight_guard.py resume

If the failure is unrecoverable in this session, leave the run paused and
final-answer with the diagnosis. The state file will keep the pause across
sessions; do not clear it unless you are abandoning the run intentionally.""",
            file=sys.stderr,
        )
        return 0
    print(
        f"""[overnight-guard: session-resume]
Active overnight run detected (started {started}).
Current TODO: {todo}{queue_footer(state)}

This session is resuming an overnight run. Reread {todo}, find the next
[ ] section in Implementation Order, and continue the loop:
  Skill(implement-todo-section) -> Skill(review-todo-section)
  -> reread -> next [ ] section -> (file exhausted) Skill(complete-todo-file)
  -> guard auto-advances to the next queued TODO.

HALT-ON-ERROR: any build / unit test / smoke test / Codex fix-loop / commit-gate
failure must pause the run, not skip past it:
  python3 .claude/hooks/overnight_guard.py pause "what failed"

To cancel the run entirely:
  python3 .claude/hooks/overnight_guard.py clear "reason"
  -- or say 'pause the overnight run' in your next prompt.""",
        file=sys.stderr,
    )
    return 0


def handle_status() -> int:
    state = load_state()
    if state is None:
        print("[overnight-guard] inactive")
        return 0
    print(json.dumps(state, indent=2))
    return 0


def main(argv: list[str]) -> int:
    cmd = argv[0] if argv else "post-tool"
    if cmd == "prompt":
        return handle_prompt()
    if cmd == "post-tool":
        return handle_post_tool()
    if cmd == "stop":
        return handle_stop()
    if cmd == "session-start":
        return handle_session_start()
    if cmd == "start":
        # Usage: start <todo1> [todo2 todo3 ...]
        paths = [normalize_todo_path(p) for p in argv[1:]] if len(argv) > 1 else []
        first = paths[0] if paths else None
        rest = paths[1:] if len(paths) > 1 else []
        state = save_state(first, "manual", queue=rest)
        print(reminder_for(state))
        return 0
    if cmd == "enqueue":
        # Usage: enqueue <todo1> [todo2 ...]
        if len(argv) < 2:
            print("[overnight-guard] enqueue needs at least one path", file=sys.stderr)
            return 64
        existing = load_state()
        if existing is None:
            print("[overnight-guard] no active run; use `start` instead", file=sys.stderr)
            return 64
        paths = [normalize_todo_path(p) for p in argv[1:]]
        state = save_state(None, existing.get("source", "manual"), queue=paths)
        print(reminder_for(state))
        return 0
    if cmd == "pause":
        reason = " ".join(argv[1:]) or "unspecified"
        return 0 if pause_state(reason) is not None else 64
    if cmd == "resume":
        return 0 if resume_state() is not None else 64
    if cmd == "clear":
        clear_state(" ".join(argv[1:]) or "manual clear")
        return 0
    if cmd == "status":
        return handle_status()
    print(f"[overnight-guard] unknown command: {cmd}", file=sys.stderr)
    return 64


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
