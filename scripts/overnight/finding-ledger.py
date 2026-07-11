#!/usr/bin/env python3
"""Codex finding-triage ledger -- decisions survive compaction and rollover.

Every Codex finding gets a stable ID = sha256(review kind + reviewed blob
SHAs + severity + file:line + normalized title). The session records its
Fix/Reject/Accept decision + evidence once; a woken/rolled-over/compacted
session looks the finding up instead of re-classifying it. A lookup MISS
means the finding (or the content it targets) is genuinely new.

  finding-ledger.py id     <kind> <severity> <file:line> <title...>
  finding-ledger.py record <kind> <severity> <file:line> --decision fix|reject|accept
                     --evidence "<text>" <title...>
  finding-ledger.py lookup <kind> <severity> <file:line> <title...>
  finding-ledger.py list [--todo PATH]

Blob binding: the ID includes the CURRENT staged/worktree blob of the target
file, so the same words against changed content is a different finding (it
must be re-triaged -- the code moved).
Ledger: .claude/state/finding-triage.jsonl (ring, newest 500 kept).
"""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
import time
from pathlib import Path

LEDGER_REL = ".claude/state/finding-triage.jsonl"
MAX_ENTRIES = 500


def _blob(root: Path, path: str) -> str:
    try:
        r = subprocess.run(["git", "-C", str(root), "hash-object", path],
                           capture_output=True, text=True, timeout=15)
        return r.stdout.strip() if r.returncode == 0 else "absent"
    except Exception:
        return "absent"


def finding_id(root: Path, kind: str, severity: str, loc: str, title: str) -> str:
    path = loc.split(":", 1)[0]
    h = hashlib.sha256()
    for part in (kind.lower(), severity.lower(), loc,
                 re.sub(r"\s+", " ", title.lower()).strip(),
                 _blob(root, path)):
        h.update(part.encode())
        h.update(b"\0")
    return h.hexdigest()[:20]


def load(root: Path) -> list:
    try:
        return [json.loads(ln) for ln in
                (root / LEDGER_REL).read_text(encoding="utf-8").splitlines()
                if ln.strip()]
    except (OSError, ValueError):
        return []


def save(root: Path, entries: list) -> None:
    p = root / LEDGER_REL
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text("\n".join(json.dumps(e) for e in entries[-MAX_ENTRIES:]) + "\n",
                 encoding="utf-8")


def main(argv) -> int:
    if not argv:
        print(__doc__, file=sys.stderr)
        return 2
    verb = argv[0]
    root = Path(".").resolve()
    entries = load(root)

    if verb == "list":
        todo = argv[argv.index("--todo") + 1] if "--todo" in argv else None
        for e in entries:
            if todo and todo not in (e.get("todo") or ""):
                continue
            print(json.dumps(e))
        return 0

    if len(argv) < 5:
        print("usage: finding-ledger.py id|record|lookup <kind> <severity> "
              "<file:line> [flags] <title...>", file=sys.stderr)
        return 2
    kind, severity, loc = argv[1], argv[2], argv[3]
    rest = argv[4:]
    decision = evidence = None
    if "--decision" in rest:
        i = rest.index("--decision")
        decision = rest[i + 1]
        rest = rest[:i] + rest[i + 2:]
    if "--evidence" in rest:
        i = rest.index("--evidence")
        evidence = rest[i + 1]
        rest = rest[:i] + rest[i + 2:]
    title = " ".join(rest)
    fid = finding_id(root, kind, severity, loc, title)

    if verb == "id":
        print(fid)
        return 0
    if verb == "record":
        if decision not in ("fix", "reject", "accept"):
            print("record needs --decision fix|reject|accept", file=sys.stderr)
            return 2
        entries = [e for e in entries if e.get("id") != fid]
        entries.append({"id": fid, "kind": kind, "severity": severity,
                        "loc": loc, "title": title[:300],
                        "decision": decision,
                        "evidence": (evidence or "")[:1000],
                        "epoch": int(time.time())})
        save(root, entries)
        print(json.dumps({"id": fid, "recorded": decision}))
        return 0
    if verb == "lookup":
        hit = next((e for e in reversed(entries) if e.get("id") == fid), None)
        if hit:
            print(json.dumps({"hit": True, **hit}, indent=1))
            return 0
        print(json.dumps({"hit": False, "id": fid,
                          "note": "new finding OR the target content changed "
                                  "since the last triage -- classify it"}))
        return 1
    print(f"unknown verb: {verb}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
