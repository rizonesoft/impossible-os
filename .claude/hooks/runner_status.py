#!/usr/bin/env python3
"""Situational-awareness brief for the overnight loop. Read-only, fail-open.

Aggregates existing scattered run-state (guard cursor/phase, gate state-files,
git, the doctrine Run Log) plus the curated live-gotchas registry into a compact
brief. `full_brief` is the pull; `anchor_line` is the one-line push the guard
emits at phase transitions. Stdlib only; ASCII.

CLI: runner_status.py [--root PATH] [--anchor]
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
import time
from datetime import date
from pathlib import Path

_CODEX_TTL_NS = 3600 * 1_000_000_000
DOCTRINE_REL = "todo/TODO-Claude-Overnight-Runner.md"


def _clip(s: str, n: int = 160) -> str:
    s = " ".join(s.split())
    return s if len(s) <= n else s[: n - 3] + "..."


def _read_json(p: Path):
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return None


def _state(root: Path) -> dict:
    return _read_json(root / ".claude" / "state" / "sequencer-run.json") or {}


def where(root: Path) -> str:
    st = _state(root)
    if not st.get("active"):
        return "WHERE: (no active run)"
    return (f"WHERE: cursor {st.get('file', '(none)')} | phase "
            f"{st.get('phase', '(none)')} | pass {st.get('pass_no', '?')}")


def git_state(root: Path) -> str:
    try:
        head = subprocess.check_output(
            ["git", "log", "-1", "--oneline"], cwd=str(root), text=True,
            timeout=3, stderr=subprocess.DEVNULL).strip()
        dirty = subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=str(root), text=True,
            timeout=3, stderr=subprocess.DEVNULL).splitlines()
        return f"GIT: {head}  ({len(dirty)} dirty)"
    except Exception:
        return "GIT: (unknown)"


def obligations(root: Path) -> list[str]:
    out: list[str] = []
    cx = _read_json(root / ".claude" / "state" / "last-codex-review.json")
    if isinstance(cx, dict) and cx.get("received") is False:
        ts = cx.get("timestamp_ns")
        if isinstance(ts, int) and (time.time_ns() - ts) <= _CODEX_TTL_NS:
            out.append("Codex review unreceived -- receiving_review gate will "
                       "block edits until Skill(superpowers:receiving-code-review)")
    try:
        diff = subprocess.check_output(
            ["git", "show", "--format=", "HEAD"], cwd=str(root), text=True,
            timeout=4, stderr=subprocess.DEVNULL).splitlines()
        flipped = any(re.match(r"\+.*\|\s*\[x\]\s*\|", ln) for ln in diff)
        stamped = any(ln.startswith("+") and "**Verified:**" in ln for ln in diff)
        if flipped and not stamped:
            out.append("HEAD flipped an IO row to [x] without a Verified stamp "
                       "-- review-todo-section due [best-effort]")
    except Exception:
        pass
    return out


def gotchas(root: Path) -> list[str]:
    p = root / ".claude" / "state" / "live-gotchas.md"
    try:
        lines = p.read_text(encoding="utf-8").splitlines()
    except Exception:
        return []
    today = date.today().isoformat()
    out = []
    for ln in lines:
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        m = re.search(r"\(expires (\d{4}-\d{2}-\d{2})\)", ln)
        if m and m.group(1) < today:
            continue
        out.append(ln.lstrip("- ").strip())
    return out


def recent_decisions(root: Path, n: int = 3) -> list[str]:
    # Run Log archived out of the doctrine file 2026-07-11 (it was 64 KB of
    # append-only history re-read on every compaction). Archive first; the
    # doctrine split remains as fallback for pre-archive checkouts.
    try:
        text = (root / "docs/overnight/run-log.md").read_text(encoding="utf-8")
    except Exception:
        try:
            text = (root / DOCTRINE_REL).read_text(encoding="utf-8")
            text = text.split("## Run Log", 1)[1] if "## Run Log" in text else ""
        except Exception:
            return []
    entries = [ln.strip() for ln in text.splitlines()
               if ln.strip().startswith("- 20")]
    return [_clip(e) for e in entries[-n:]]


def _woke_line(root: Path) -> str | None:
    """One-shot wake note left by `run_phase_guard.py wake`: the fresh session
    must read the awaited artifact(s) and receive the verdict FIRST."""
    st = _state(root)
    w = st.get("woke_from_wait")
    if not isinstance(w, dict):
        return None
    kind = "READY" if w.get("ready") else ("EXPIRED" if w.get("expired") else "?")
    arts = ", ".join(w.get("artifacts") or [])
    return (f"WOKE FROM WAIT [{kind}] ({w.get('reason', '')}): read + receive "
            f"the verdict(s) FIRST: {arts}")


def anchor_line(root: Path) -> str:
    st = _state(root)
    woke = " | WOKE-FROM-WAIT" if st.get("woke_from_wait") else ""
    return (f"cursor {st.get('file', '(none)')} | phase {st.get('phase', '(none)')} "
            f"| obligations:{len(obligations(root))} | gotchas:{len(gotchas(root))}"
            f"{woke}")


def full_brief(root: Path) -> str:
    obl = obligations(root)
    got = gotchas(root)
    dec = recent_decisions(root)
    parts = [where(root), git_state(root)]
    woke = _woke_line(root)
    if woke:
        parts.append(woke)
    parts.append("OBLIGATIONS: " + ("none" if not obl else ""))
    parts += [f"  - {o}" for o in obl]
    parts.append("GOTCHAS: " + ("none" if not got else ""))
    parts += [f"  - {g}" for g in got]
    parts.append("RECENT DECISIONS: " + ("none" if not dec else ""))
    parts += [f"  {d}" for d in dec]
    return "\n".join(p for p in parts if p is not None)


def _resolve_root(argv: list[str]) -> Path:
    if "--root" in argv:
        return Path(argv[argv.index("--root") + 1])
    try:
        top = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"], text=True,
            timeout=3, stderr=subprocess.DEVNULL).strip()
        return Path(top) if top else Path.cwd()
    except Exception:
        return Path.cwd()


def main(argv: list[str]) -> int:
    root = _resolve_root(argv)
    try:
        if "--anchor" in argv:
            print(anchor_line(root))
        else:
            print(full_brief(root))
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
