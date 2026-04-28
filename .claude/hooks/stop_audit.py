#!/usr/bin/env python3
# block-via: warning-only (Stop hook -- writes to log, never blocks)
"""Stop hook -- end-of-turn audit (TODO-08 §11).

Two checks at session-stop time:

  Check A (acknowledged-but-skipped): walks the transcript JSONL
  backward up to the last 30 turns looking for a promise-shape phrase
  ("I will run X", "I should invoke X", etc.) where X is one of the
  enforcement-relevant skill / hook names. If a matching tool call did
  NOT fire within the next 5 turns of the promise, append a WARN line
  to .claude/state/acknowledged-but-skipped.log.

  Check B (verification-before-completion gate): if any todo/**/*.md
  edit flipped `[ ]` to `[x]` in this session AND no
  Skill(superpowers:verification-before-completion) ran in the same
  session, append a WARN line to the same log. Reminder, not block;
  the user may legitimately stop the session intentionally with
  verification owed to the next.

Per design Q3: the promise pattern is anchored to specific enforcement
skill / hook names so general narrative ("I will read the file") does
not false-positive.
"""
import json
import os
import re
import subprocess
import sys
import time
from typing import List, Optional


_SKIP_LOG_REL = ".claude/state/acknowledged-but-skipped.log"

_TURN_WINDOW = 30
_MATCH_WINDOW = 5

# Promise pattern: "I('| w)ill|I should" + verb + skill-name. Anchored to
# enforcement-relevant names only.
_PROMISE_RE = re.compile(
    r"I(?:'ll|\s+will|\s+should)\s+(?:run|invoke|call|dispatch|fire|now)\s+"
    r"(?:the\s+|/)?"
    r"(review-todo-section|verify-todo-section|implement-todo-section|"
    r"complete-todo-file|quality-review-section|"
    r"codex-(?:adversarial|consistency|design|fix|impact|perf|review|test)-?\w*|"
    r"(?:boot|kernel|desktop|shell|userland)-code-quality|"
    r"superpowers:(?:receiving-code-review|verification-before-completion|"
    r"systematic-debugging|test-driven-development))",
    re.I,
)

# Skills that satisfy the "I'll run kernel-code-quality" / "I'll invoke
# review-todo-section" promise types: any subsequent Skill or Bash that
# name-matches the promise target counts.

_VFC_SKILL = "superpowers:verification-before-completion"


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


def _load_transcript(path: str) -> List[dict]:
    """Materializes ALL events. Used by _check_acknowledged_but_skipped
    where the bounded scan still needs the last _TURN_WINDOW events
    plus _MATCH_WINDOW lookahead. Codex perf review 2026-04-28 M2:
    callers that only need the tail should use _load_transcript_tail
    or _check_vfc_streaming which avoid the full materialization."""
    if not path or not os.path.exists(path):
        return []
    out = []
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                try:
                    out.append(json.loads(line))
                except Exception:
                    continue
    except Exception:
        return []
    return out


def _check_vfc_streaming(path: str) -> Optional[str]:
    """Stream the transcript and short-circuit as soon as both
    todo_flip and vfc_seen are known. Codex perf review 2026-04-28 M2:
    avoids materializing the full event list when VFC state can be
    determined from a partial scan.
    """
    if not path or not os.path.exists(path):
        return None
    todo_flip = False
    vfc_seen = False
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                try:
                    ev = json.loads(line)
                except Exception:
                    continue
                for tu in _tool_uses(ev):
                    name = tu.get("name") or ""
                    inp = tu.get("input") or {}
                    if name == "Skill":
                        sk = (inp.get("skill") or inp.get("name") or "").lower()
                        if (sk == _VFC_SKILL.lower()
                                or sk.endswith(":verification-before-completion")):
                            vfc_seen = True
                    elif name in ("Edit", "MultiEdit"):
                        fp = inp.get("file_path") or inp.get("path") or ""
                        if "/todo/" not in fp.replace("\\", "/"):
                            continue
                        ns = inp.get("new_string", "")
                        if isinstance(ns, str) and "[x]" in ns:
                            todo_flip = True
                        edits = inp.get("edits", [])
                        if isinstance(edits, list):
                            for e in edits:
                                if isinstance(e, dict):
                                    es = e.get("new_string", "")
                                    if isinstance(es, str) and "[x]" in es:
                                        todo_flip = True
                # Early-exit: VFC owed only when todo_flip AND not vfc_seen.
                # If we have BOTH (vfc satisfied), no warning. Stop scanning.
                # If we have todo_flip but no vfc YET, we must keep scanning
                # because VFC may appear later; can't short-circuit early.
                # If we have vfc_seen, we're DONE (no warning regardless of
                # any later todo flips).
                if vfc_seen:
                    return None
    except Exception:
        return None
    if todo_flip and not vfc_seen:
        return ("{ts} VFC-OWED todo edits flipped [x] but "
                "Skill(superpowers:verification-before-completion) never ran"
                .format(ts=_ts_ns()))
    return None


def _load_transcript_tail(path: str, max_events: int) -> List[dict]:
    """Read the last max_events JSONL events from path. Used by the
    promise-scan check which is bounded by _TURN_WINDOW + _MATCH_WINDOW.
    Avoids materializing 10K+ events when only ~35 are needed.
    """
    if not path or not os.path.exists(path):
        return []
    # Simple ring-buffer over the file: parse all lines, keep the last
    # max_events. The file format is one JSON per line so we can avoid
    # holding all parsed dicts in memory simultaneously by keeping a
    # bounded deque of the raw line texts and parsing only at the end.
    from collections import deque
    tail_lines = deque(maxlen=max_events)
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                tail_lines.append(line)
    except Exception:
        return []
    out = []
    for line in tail_lines:
        try:
            out.append(json.loads(line))
        except Exception:
            continue
    return out


def _extract_text(ev: dict) -> str:
    msg = ev.get("message")
    if not isinstance(msg, dict):
        return ""
    parts = msg.get("content")
    if isinstance(parts, str):
        return parts
    if not isinstance(parts, list):
        return ""
    out = []
    for p in parts:
        if isinstance(p, dict):
            if p.get("type") == "text":
                t = p.get("text", "")
                if isinstance(t, str):
                    out.append(t)
    return "\n".join(out)


def _tool_uses(ev: dict) -> List[dict]:
    msg = ev.get("message")
    if not isinstance(msg, dict):
        return []
    parts = msg.get("content")
    if not isinstance(parts, list):
        return []
    out = []
    for p in parts:
        if isinstance(p, dict) and p.get("type") == "tool_use":
            out.append(p)
    return out


def _tool_signature(tu: dict) -> str:
    """Return a flattened signature string used to match against promise targets."""
    name = tu.get("name") or ""
    inp = tu.get("input") or {}
    bits = [name]
    if name == "Skill":
        bits.append(inp.get("skill") or inp.get("name") or "")
    elif name == "Bash":
        bits.append(inp.get("command") or "")
    elif name in ("Edit", "Write", "MultiEdit"):
        bits.append(inp.get("file_path") or inp.get("path") or "")
    return " ".join(b for b in bits if isinstance(b, str))


def _check_acknowledged_but_skipped(events: List[dict]) -> List[str]:
    """Return list of WARN lines for promises that never had a matching tool call."""
    warns = []
    n = len(events)
    if n == 0:
        return warns

    # Walk last N events looking for promise text.
    start = max(0, n - _TURN_WINDOW)
    for i in range(start, n):
        text = _extract_text(events[i])
        if not text:
            continue
        for m in _PROMISE_RE.finditer(text):
            promised = (m.group(1) or "").lower()
            # Look forward up to _MATCH_WINDOW events for a matching tool use.
            satisfied = False
            for j in range(i, min(n, i + _MATCH_WINDOW + 1)):
                for tu in _tool_uses(events[j]):
                    sig = _tool_signature(tu).lower()
                    if promised in sig:
                        satisfied = True
                        break
                if satisfied:
                    break
            if not satisfied:
                excerpt = m.group(0)[:120]
                warns.append(
                    "{ts} ACKNOWLEDGED-BUT-SKIPPED promised={promised!r} excerpt={excerpt!r}"
                    .format(ts=_ts_ns(), promised=promised, excerpt=excerpt)
                )
    return warns


def _check_vfc_owed(events: List[dict]) -> Optional[str]:
    """If any todo/*.md edit flipped [ ] -> [x] in this session AND
    Skill(superpowers:verification-before-completion) was never invoked,
    return a WARN line. Otherwise None."""
    todo_flip = False
    vfc_seen = False
    for ev in events:
        for tu in _tool_uses(ev):
            name = tu.get("name") or ""
            inp = tu.get("input") or {}
            if name == "Skill":
                sk = (inp.get("skill") or inp.get("name") or "").lower()
                if sk == _VFC_SKILL.lower() or sk.endswith(":verification-before-completion"):
                    vfc_seen = True
            elif name in ("Edit", "MultiEdit"):
                # Heuristic: file_path under todo/ AND new_string contains "[x]"
                fp = inp.get("file_path") or inp.get("path") or ""
                if "/todo/" not in fp.replace("\\", "/"):
                    continue
                ns = inp.get("new_string", "")
                if isinstance(ns, str) and "[x]" in ns:
                    todo_flip = True
                edits = inp.get("edits", [])
                if isinstance(edits, list):
                    for e in edits:
                        if isinstance(e, dict):
                            es = e.get("new_string", "")
                            if isinstance(es, str) and "[x]" in es:
                                todo_flip = True
    if todo_flip and not vfc_seen:
        return ("{ts} VFC-OWED todo edits flipped [x] but "
                "Skill(superpowers:verification-before-completion) never ran"
                .format(ts=_ts_ns()))
    return None


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict):
        return 0

    root = _repo_root()
    if not root:
        return 0

    transcript = d.get("transcript_path", "")

    # Codex perf review 2026-04-28 M2: bound the promise scan to a
    # tail of (TURN_WINDOW + MATCH_WINDOW) events, and use the
    # streaming VFC check that early-exits as soon as a VFC invocation
    # is observed. Avoids materializing 10K+ event transcripts at end
    # of every turn.
    tail_events = _load_transcript_tail(transcript, _TURN_WINDOW + _MATCH_WINDOW)
    warns = _check_acknowledged_but_skipped(tail_events)
    vfc_warn = _check_vfc_streaming(transcript)
    if vfc_warn:
        warns.append(vfc_warn)

    if not warns:
        return 0

    log_path = os.path.join(root, _SKIP_LOG_REL)
    try:
        os.makedirs(os.path.dirname(log_path), exist_ok=True)
        with open(log_path, "a", encoding="utf-8") as f:
            for w in warns:
                f.write(w + "\n")
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
