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
  pre           PreToolUse on Task/Agent: on a key hit, BLOCK the dispatch
                (exit 2) and hand the cached report back in the block message --
                the model reuses it instead of re-paying for the dispatch.
  subagentstop  SubagentStop: store the agent's report under the key. This is
                the ONLY reliable store point -- the Agent tool runs subagents
                in the BACKGROUND by default, so the Task PostToolUse fires at
                DISPATCH time with the ack, not the report (empirically verified
                2026-07-11). The SubagentStop payload carries agent_type +
                last_assistant_message (the report) + agent_transcript_path
                (whose first user message is the exact dispatched prompt), so
                the same content-bound key can be recomputed and stored.
  post          PostToolUse on Task/Agent: legacy store point, still fires for
                FOREGROUND (synchronous) dispatches whose PostToolUse carries the
                report; a no-op for background (report absent). Kept as
                belt-and-suspenders; subagentstop is the primary path.

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
    untracked file CONTENTS. Untracked contents must be hashed (same as
    receipts.build_input_key): an edit to an existing untracked file changes
    neither the name list nor the diff, and a name-only hash would serve a
    stale cached report over changed content."""
    tree = _git(root, "ls-tree", "-r", "HEAD", "--", *paths)
    diff = _git(root, "diff", "HEAD", "--", *paths)
    untracked = _git(root, "ls-files", "-o", "--exclude-standard", "--", *paths)
    if tree is None or diff is None or untracked is None:
        return None
    h = hashlib.sha256()
    h.update(tree.encode())
    h.update(diff.encode())
    names = [n for n in untracked.splitlines() if n.strip()]
    h.update("\n".join(names).encode())
    if names:
        try:
            r = subprocess.run(
                ["git", "-C", str(root), "hash-object", "--stdin-paths"],
                input="\n".join(names).encode(), capture_output=True,
                timeout=120)
            if r.returncode != 0:
                return None  # fail toward no-cache, never a stale hit
            h.update(r.stdout)
        except Exception:
            return None
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


def _first_user_message(transcript_path: str) -> str:
    """The first user message in a subagent's transcript IS the exact prompt the
    parent dispatched (empirically verified 2026-07-11) -- so the content-bound
    key recomputed here matches the one `pre` computed from tool_input.prompt."""
    try:
        with open(transcript_path, encoding="utf-8") as f:
            for line in f:
                try:
                    ev = json.loads(line)
                except Exception:
                    continue
                m = ev.get("message")
                if not isinstance(m, dict) or m.get("role") != "user":
                    continue
                c = m.get("content")
                if isinstance(c, str):
                    t = c
                elif isinstance(c, list):
                    t = "".join(b.get("text", "") for b in c
                                if isinstance(b, dict) and b.get("type") == "text")
                else:
                    t = ""
                if t.strip():
                    return t
    except OSError:
        return ""
    return ""


def _store(root: Path, stype: str, prompt: str, report: str) -> None:
    key = _cache_key(root, stype, prompt)
    if key is None:
        return
    cache_dir = root / CACHE_DIR_REL
    try:
        cache_dir.mkdir(parents=True, exist_ok=True)
        (cache_dir / f"{key}.json").write_text(json.dumps({
            "agent": stype, "stored_epoch": int(time.time()),
            "report": report}), encoding="utf-8")
        _prune(cache_dir)
    except Exception:
        return
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import _offload_log
        _offload_log.log_event(root, "cache-store", "agent_result_cache",
                               f"{stype} report cached ({len(report)} bytes)")
    except Exception:
        pass


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
    root = _repo_root()
    if root is None:
        return 0

    # SubagentStop: the primary store point. Payload has no tool_name/tool_input;
    # it carries agent_type + last_assistant_message (report) + the leaf
    # agent_transcript_path (first user message == the dispatched prompt).
    if mode == "subagentstop":
        stype = d.get("agent_type") or d.get("subagent_type") or ""
        if stype not in CACHE_SCOPES:
            return 0
        report = _extract_report(d.get("last_assistant_message"))[:MAX_REPORT_BYTES]
        if len(report) < 80:
            return 0  # too small to be a real analyst report; don't cache
        prompt = _first_user_message(d.get("agent_transcript_path") or "")
        if not prompt:
            return 0
        _store(root, stype, prompt, report)
        return 0

    # pre / post operate on the Task/Agent tool call payload.
    if d.get("tool_name") not in ("Task", "Agent"):
        return 0
    ti = d.get("tool_input") or {}
    stype = ti.get("subagent_type") or ""
    if stype not in CACHE_SCOPES:
        return 0
    key = _cache_key(root, stype, ti.get("prompt") or "")
    if key is None:
        return 0
    cache_dir = root / CACHE_DIR_REL
    entry = cache_dir / f"{key}.json"

    if mode == "post":
        # Legacy path: only fires usefully for FOREGROUND dispatches (background
        # PostToolUse has the dispatch ack, not the report). subagentstop is the
        # primary store; this is belt-and-suspenders for synchronous dispatches.
        report = _extract_report(d.get("tool_response"))[:MAX_REPORT_BYTES]
        if len(report) < 80:
            return 0
        _store(root, stype, ti.get("prompt") or "", report)
        return 0

    # pre: block the dispatch on a hit and return the cached report.
    try:
        rec = json.loads(entry.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return 0
    age_min = (time.time() - rec.get("stored_epoch", 0)) / 60
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import _offload_log
        # ~4 chars/token on the report the dispatch would have re-derived,
        # plus the agent's own reading -- a floor estimate, logged for the
        # net-savings scorecard.
        est_tokens = max(500, len(rec.get("report", "")) // 4)
        _offload_log.log_event(root, "cache-hit", "agent_result_cache",
                               f"{stype} ~{est_tokens} sidechain tokens avoided")
    except Exception:
        pass
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
