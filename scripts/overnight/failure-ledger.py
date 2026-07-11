#!/usr/bin/env python3
"""Failure fingerprint ledger -- never rediscover the same failure twice.

Normalizes a failure log (strip timestamps, addresses, pids, temp paths, ANSI)
and hashes it together with the failure kind + the relevant content
fingerprint (build inputs for build/test failures). On `check`, an identical
recurring failure returns the STORED diagnosis so the session reuses it and
backs off instead of spending fresh turns re-deriving it.

  failure-ledger.py check  <kind> < log      exit 0 + stored record on hit
  failure-ledger.py record <kind> --diagnosis "<text>" < log
  failure-ledger.py note   <kind> < log      hit count only (for backoff)

Ledger: .claude/state/failure-ledger.jsonl (ring, newest 200 kept).
"""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
import time
from pathlib import Path

LEDGER_REL = ".claude/state/failure-ledger.jsonl"
MAX_ENTRIES = 200
_NORM = [
    (re.compile(r"\x1b\[[0-9;]*m"), ""),
    (re.compile(r"\b\d{2}:\d{2}:\d{2}(?:\.\d+)?\b"), "TS"),
    (re.compile(r"\b20\d{2}-\d{2}-\d{2}\b"), "DATE"),
    (re.compile(r"\b0x[0-9a-fA-F]{6,}\b"), "ADDR"),
    (re.compile(r"\bpid[= ]?\d+\b", re.I), "PID"),
    (re.compile(r"/tmp/[^\s'\"]+"), "TMPPATH"),
    (re.compile(r"\b\d+\.\d+s\b"), "DUR"),
]


def normalize(text: str) -> str:
    for rx, rep in _NORM:
        text = rx.sub(rep, text)
    # keep only lines that look failure-relevant, bounded
    keep = [ln.strip() for ln in text.splitlines()
            if re.search(r"(?i)\b(error|fail|fatal|panic|assert|block)\b", ln)]
    return "\n".join(keep[:80])


def fingerprint(root: Path, kind: str, log: str) -> str:
    h = hashlib.sha256()
    h.update(kind.encode())
    h.update(b"\0")
    h.update(normalize(log).encode())
    if kind in ("build", "test", "smoke"):
        try:
            r = subprocess.run(
                ["git", "-C", str(root), "diff", "HEAD", "--",
                 "src", "include", "user", "Makefile"],
                capture_output=True, timeout=60)
            h.update(hashlib.sha256(r.stdout).digest())
        except Exception:
            pass
    return h.hexdigest()


def load(root: Path) -> list:
    p = root / LEDGER_REL
    out = []
    try:
        for ln in p.read_text(encoding="utf-8").splitlines():
            try:
                out.append(json.loads(ln))
            except ValueError:
                pass
    except OSError:
        pass
    return out


def save(root: Path, entries: list) -> None:
    p = root / LEDGER_REL
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text("\n".join(json.dumps(e) for e in entries[-MAX_ENTRIES:]) + "\n",
                 encoding="utf-8")


def main(argv) -> int:
    if len(argv) < 2:
        print("usage: failure-ledger.py check|record|note <kind> "
              "[--diagnosis TEXT] < log", file=sys.stderr)
        return 2
    verb, kind = argv[0], argv[1]
    root = Path(".").resolve()
    log = sys.stdin.read()
    fp = fingerprint(root, kind, log)
    entries = load(root)
    hit = next((e for e in reversed(entries) if e.get("fp") == fp), None)

    if verb == "record":
        diag = argv[argv.index("--diagnosis") + 1] if "--diagnosis" in argv else ""
        if hit:
            hit["count"] = hit.get("count", 1) + 1
            hit["last_epoch"] = int(time.time())
            if diag:
                hit["diagnosis"] = diag[:2000]
        else:
            entries.append({"fp": fp, "kind": kind, "count": 1,
                            "first_epoch": int(time.time()),
                            "last_epoch": int(time.time()),
                            "diagnosis": diag[:2000],
                            "sample": normalize(log)[:600]})
        save(root, entries)
        print(json.dumps({"fp": fp[:16], "recorded": True}))
        return 0

    if hit:
        hit["count"] = hit.get("count", 1) + 1
        hit["last_epoch"] = int(time.time())
        save(root, entries)
        print(json.dumps({
            "hit": True, "fp": fp[:16], "count": hit["count"],
            "kind": hit.get("kind"),
            "diagnosis": hit.get("diagnosis") or "(none stored)",
            "advice": ("IDENTICAL failure over identical inputs recurred "
                       f"{hit['count']}x -- reuse the stored diagnosis; do "
                       "not re-derive. 3+ recurrences = defer-and-escalate "
                       "per doctrine.")}, indent=1))
        return 0
    print(json.dumps({"hit": False, "fp": fp[:16]}))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
