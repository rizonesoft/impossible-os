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


GOTCHA_BRIEF_CAP = 6          # cards injected into a brief
GOTCHA_BYTE_CAP = 2400        # hard byte ceiling on the injected block


def _gotcha_context(root: Path) -> str:
    """Deterministic relevance context: cursor file + phase + its basename
    stem (a cheap symbol proxy). No embeddings, no model."""
    st = _state(root)
    f = (st.get("file") or "")
    bits = [f, st.get("phase") or ""]
    if f:
        import os as _os
        bits.append(_os.path.basename(f).rsplit(".", 1)[0])
    return " ".join(bits).lower()


def gotchas(root: Path, relevant_only: bool = False) -> list[str]:
    """Unexpired gotcha cards. With relevant_only, keep global cards (no
    scope tag) plus cards whose deterministic tags -- `[path:...]`,
    `[phase:...]`, `[todo:...]`, `[sym:...]` -- match the current cursor
    context, capped by count and bytes. No model / embedding involved."""
    p = root / ".claude" / "state" / "live-gotchas.md"
    try:
        lines = p.read_text(encoding="utf-8").splitlines()
    except Exception:
        return []
    today = date.today().isoformat()
    ctx = _gotcha_context(root) if relevant_only else ""
    tag_re = re.compile(r"\[(path|phase|todo|sym):([^\]]+)\]", re.I)
    # SELECTION ORDER, not file order. The cap is 6 cards / 2400 bytes against a
    # registry that reached 85 cards, and the old walk consumed it in FILE order
    # -- so a newly-appended card was the LEAST likely to be delivered, which is
    # the exact inverse of what a "live" registry needs. Measured 2026-08-05: an
    # operator card tagged for the cursor file sat at position 85 behind 52 KB of
    # older global cards and never reached the run at all.
    #
    # Priority: cards whose tag MATCHES the current cursor beat global ones (a
    # card about the file you are on is worth more than a generic one from June),
    # and within each group the NEWEST wins. Untagged cards are still global and
    # still delivered -- they just no longer crowd out targeted guidance.
    cards: list[tuple[int, str]] = []          # (priority, card)
    for ln in lines:
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        m = re.search(r"\(expires (\d{4}-\d{2}-\d{2})\)", ln)
        if m and m.group(1) < today:
            continue
        card = ln.lstrip("- ").strip()
        if not relevant_only:
            cards.append((1, card))
            continue
        tags = tag_re.findall(card)
        if tags:  # tagged card: keep only when a tag hits the context
            if not any(val.strip().lower() in ctx for _, val in tags):
                continue
            cards.append((0, card))            # targeted -> highest priority
        else:
            cards.append((1, card))            # global
    if not relevant_only:
        return [c for _, c in cards]
    # stable within priority, newest (latest in file) first
    ordered = [card for _, (_prio, card) in sorted(
        enumerate(cards), key=lambda kv: (kv[1][0], -kv[0]))]
    out: list[str] = []
    budget = GOTCHA_BYTE_CAP
    for card in ordered:
        if len(out) >= GOTCHA_BRIEF_CAP or budget - len(card) < 0:
            continue
        budget -= len(card)
        out.append(card)
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


def anchor_line(root: Path) -> str:
    st = _state(root)
    return (f"cursor {st.get('file', '(none)')} | phase {st.get('phase', '(none)')} "
            f"| obligations:{len(obligations(root))} | gotchas:{len(gotchas(root))}")


def full_brief(root: Path) -> str:
    obl = obligations(root)
    # Relevance-filtered: only gotchas matching the cursor context (path /
    # phase / todo / symbol tags) plus untagged globals, capped by count +
    # bytes -- the brief carries only facts that can affect the next action.
    got = gotchas(root, relevant_only=True)
    total = len(gotchas(root))
    dec = recent_decisions(root)
    parts = [where(root), git_state(root)]
    parts.append("OBLIGATIONS: " + ("none" if not obl else ""))
    parts += [f"  - {o}" for o in obl]
    suppressed = total - len(got)
    hdr = "GOTCHAS: " + ("none" if not got else "")
    if suppressed > 0:
        hdr += f"  ({suppressed} off-context suppressed; `runner_status.py --all-gotchas`)"
    parts.append(hdr)
    parts += [f"  - {g}" for g in got]
    parts.append("RECENT DECISIONS: " + ("none" if not dec else ""))
    parts += [f"  {d}" for d in dec]
    return "\n".join(p for p in parts if p is not None)


def all_gotchas_brief(root: Path) -> str:
    got = gotchas(root)
    return "ALL GOTCHAS:\n" + "\n".join(f"  - {g}" for g in got) if got \
        else "ALL GOTCHAS: none"


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
        elif "--all-gotchas" in argv:
            print(all_gotchas_brief(root))
        else:
            print(full_brief(root))
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
