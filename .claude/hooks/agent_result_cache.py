#!/usr/bin/env python3
"""Content-addressed cache for read-only analyst agent reports.

An analyst dispatch over byte-identical inputs returns the same report; paying
Sonnet (and the main session's wait) twice for it is pure waste -- measured
2026-07-10: repeated parity/evidence dispatches across relaunches and review
rounds re-derived unchanged answers. The cache key is
sha256(agent_type + normalized prompt + content fingerprint), where the
fingerprint is the git state of the scopes that agent reads (source tree,
todo/ tree, or both) -- so ANY relevant edit invalidates and staleness is
impossible by construction (same model as scripts/overnight/receipts.py).

Modes (argv[1]):
  post   PostToolUse on Task/Agent: store the agent's report under the key.
  pre    PreToolUse on Task/Agent: on a key hit, BLOCK the dispatch (exit 2)
         and hand the cached report back in the block message -- the model
         reuses it instead of re-paying for the dispatch.

To force a fresh run, change the prompt (e.g. append "fresh run: <why>") --
any prompt change changes the key. Operator kill-switch:
AGENT_RESULT_CACHE_DISABLE=1. Only agents in CACHE_SCOPES participate; every
other subagent_type passes through untouched. Fail-open everywhere.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

# Which git scopes feed each cacheable agent's fingerprint. "src" = the build
# input surface (mirrors receipts.BUILD_INPUT_PATHS), "todo" = the TODO tree.
# Agents whose answers depend on the web (researchers) still cache: their
# INPUTS here are the repo questions; a deliberate refresh is a prompt tweak.
SRC_PATHS = ["src", "include", "user", "resources", "tools",
             "Makefile", "scripts/build.sh", "boot.conf"]
TODO_PATHS = ["todo"]
CACHE_SCOPES = {
    "kernel-explorer": ("src",),
    "section-context-mapper": ("src", "todo"),
    "concurrency-evidence-mapper": ("src",),
    "review-evidence-mapper": ("src", "todo"),
    "test-coverage-mapper": ("src", "todo"),
    "xref-dependency-mapper": ("src", "todo"),
    "parity-research-analyst": ("todo",),
}

CACHE_DIR_REL = ".claude/state/agent-cache"
MAX_ENTRIES = 64
MAX_REPORT_BYTES = 32 * 1024
PRUNE_AGE_S = 7 * 24 * 3600  # hygiene only; validity is content-bound


def _repo_root() -> Path | None:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True, timeout=3, stderr=subprocess.DEVNULL).strip()
        return Path(out) if out else None
    except Exception:
        return None


def _git(root: Path, *args: str) -> str | None:
    try:
        r = subprocess.run(["git", "-C", str(root), *args],
                           capture_output=True, timeout=60)
        return r.stdout.decode("utf-8", "replace") if r.returncode == 0 else None
    except Exception:
        return None


def _scope_key(root: Path, paths: list) -> str | None:
    """Content fingerprint of the given paths: HEAD tree + worktree diff +
    untracked names (names suffice here -- an analyst re-reads the worktree,
    and new untracked content shows up in the diff-of-names churn; the heavy
    hash-object pass is reserved for build receipts)."""
    tree = _git(root, "ls-tree", "-r", "HEAD", "--", *paths)
    diff = _git(root, "diff", "HEAD", "--", *paths)
    untracked = _git(root, "ls-files", "-o", "--exclude-standard", "--", *paths)
    if tree is None or diff is None or untracked is None:
        return None
    h = hashlib.sha256()
    h.update(tree.encode())
    h.update(diff.encode())
    h.update(untracked.encode())
    return h.hexdigest()


def _cache_key(root: Path, stype: str, prompt: str) -> str | None:
    scopes = CACHE_SCOPES.get(stype)
    if not scopes:
        return None
    h = hashlib.sha256()
    h.update(stype.encode())
    h.update(b"\0")
    h.update(re.sub(r"\s+", " ", (prompt or "").strip()).encode())
    for s in scopes:
        sk = _scope_key(root, SRC_PATHS if s == "src" else TODO_PATHS)
        if sk is None:
            return None  # git unavailable -> no caching (fail-open)
        h.update(b"\0")
        h.update(sk.encode())
    return h.hexdigest()


def _extract_report(resp) -> str:
    """Best-effort text extraction from a PostToolUse tool_response."""
    if isinstance(resp, str):
        return resp
    if isinstance(resp, dict):
        content = resp.get("content")
        if isinstance(content, str):
            return content
        if isinstance(content, list):
            parts = [b.get("text", "") for b in content
                     if isinstance(b, dict) and b.get("type") == "text"]
            if parts:
                return "\n".join(parts)
        for k in ("result", "text", "output"):
            if isinstance(resp.get(k), str):
                return resp[k]
    return ""


def _prune(cache_dir: Path) -> None:
    try:
        entries = sorted(cache_dir.glob("*.json"),
                         key=lambda p: p.stat().st_mtime, reverse=True)
        now = time.time()
        for i, p in enumerate(entries):
            if i >= MAX_ENTRIES or now - p.stat().st_mtime > PRUNE_AGE_S:
                p.unlink(missing_ok=True)
    except Exception:
        pass


def main(mode: str) -> int:
    if os.environ.get("AGENT_RESULT_CACHE_DISABLE") == "1":
        return 0
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if d.get("tool_name") not in ("Task", "Agent"):
        return 0
    ti = d.get("tool_input") or {}
    stype = ti.get("subagent_type") or ""
    if stype not in CACHE_SCOPES:
        return 0
    root = _repo_root()
    if root is None:
        return 0
    key = _cache_key(root, stype, ti.get("prompt") or "")
    if key is None:
        return 0
    cache_dir = root / CACHE_DIR_REL
    entry = cache_dir / f"{key}.json"

    if mode == "post":
        report = _extract_report(d.get("tool_response"))[:MAX_REPORT_BYTES]
        if len(report) < 80:
            return 0  # too small to be a real analyst report; don't cache
        try:
            cache_dir.mkdir(parents=True, exist_ok=True)
            entry.write_text(json.dumps({
                "agent": stype, "stored_epoch": int(time.time()),
                "report": report}), encoding="utf-8")
            _prune(cache_dir)
        except Exception:
            pass
        return 0

    # pre: block the dispatch on a hit and return the cached report.
    try:
        rec = json.loads(entry.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return 0
    age_min = (time.time() - rec.get("stored_epoch", 0)) / 60
    sys.stderr.write(
        f"[agent-cache] HIT: an identical {stype} dispatch already ran over "
        f"content-identical inputs ({age_min:.0f} min ago; every file that "
        f"agent reads is unchanged since). REUSE the report below instead of "
        f"re-dispatching -- verify load-bearing file:line claims yourself as "
        f"usual. To force a fresh run, change the prompt (e.g. append "
        f"'fresh run: <why>').\n\n--- cached {stype} report ---\n"
        f"{rec.get('report', '')}\n--- end cached report ---\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "pre"))
