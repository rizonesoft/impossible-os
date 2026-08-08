#!/usr/bin/env python3
"""Content-bound receipt for `scripts/test-tooling.sh`.

WHY THIS EXISTS. `.githooks/pre-push` runs the ~6-minute tooling suite whenever
a push touches what the suite covers. That is correct and it is also duplicated
work in the common case: the operator (or the run) executes the suite, sees it
green, commits, and pushes -- and the gate runs the identical 1300 tests over
the identical bytes. Measured 2026-08-08: the second run blew the 10-minute
tool wall and killed the push call outright; the backgrounded retry then paid
the six minutes again.

WHY IT IS DANGEROUS, AND WHAT THAT DICTATES. A gate that can be satisfied by a
receipt is a gate that is not running. So:

  * the key binds to CONTENT, not to a commit, a timestamp or a flag -- every
    tracked and untracked file under the tooling surface, hashed;
  * the enumeration is recomputed on every call, so a NEW untracked file
    changes the key (hashing a fixed list would let one appear for free);
  * the receipt is written ONLY by a suite that finished with zero failures,
    and only when the key computed BEFORE the run still matches the key after
    it -- a file edited mid-run means the suite tested bytes that no longer
    exist, and that receipt is refused rather than written;
  * `check` fails CLOSED on anything unexpected: no receipt, unreadable
    receipt, missing field, key mismatch. Every failure path means "run the
    suite", never "assume it passed".

The surface is deliberately WIDER than the gate's trigger paths. The trigger
asks "did this push touch tooling?"; the receipt has to answer "could anything
have changed what the suite would say?", and the suite reads wrappers, hooks,
workflows and doctrine files far outside the four trigger prefixes.
"""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path

# Every path prefix whose bytes can change the suite's verdict.
SURFACE = (
    "scripts",
    ".claude/hooks",
    ".claude/settings.json",
    ".githooks",
    ".github/workflows",
    "CLAUDE.md",
    "README.md",
    "AGENTS.md",
    "Makefile",
)

RECEIPT_REL = ".claude/state/tooling-receipt.json"


def _git(root: Path, *args: str) -> str:
    try:
        r = subprocess.run(("git", "-C", str(root), *args),
                           capture_output=True, text=True, timeout=60)
        return r.stdout if r.returncode == 0 else ""
    except Exception:
        return ""


def _surface_files(root: Path) -> list[str]:
    """Tracked + untracked (gitignore-honouring) files under the surface.

    Enumerated live on every call: a receipt whose key came from a FIXED list
    would stay valid when a brand-new hook appeared beside the ones it hashed.
    """
    tracked = _git(root, "ls-files", "--", *SURFACE).splitlines()
    others = _git(root, "ls-files", "--others", "--exclude-standard",
                  "--", *SURFACE).splitlines()
    return sorted({p for p in (*tracked, *others) if p.strip()})


def surface_key(root: Path) -> str:
    items = []
    for rel in _surface_files(root):
        try:
            h = hashlib.sha256((root / rel).read_bytes()).hexdigest()
        except OSError:
            h = "absent"
        items.append((rel, h))
    return hashlib.sha256(json.dumps(items).encode()).hexdigest()[:16]


def cmd_key(root: Path) -> int:
    print(surface_key(root))
    return 0


def cmd_write(root: Path, argv: list[str]) -> int:
    """Write the receipt. `--expect <key>` is the pre-run key: when it does not
    match the key now, the suite measured bytes that have since changed and no
    receipt is written."""
    expect = None
    if "--expect" in argv:
        expect = argv[argv.index("--expect") + 1]
    now = surface_key(root)
    if expect and expect != now:
        sys.stderr.write(
            "tooling-receipt: NOT written -- the tooling surface changed while "
            f"the suite ran ({expect} -> {now}); its result describes bytes "
            "that are no longer on disk\n")
        return 1
    p = root / RECEIPT_REL
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps({"key": now, "suite": "scripts/test-tooling.sh"}))
    return 0


def cmd_check(root: Path) -> int:
    """0 = a green suite already ran on exactly these bytes. Non-zero for every
    other state, including every error state."""
    p = root / RECEIPT_REL
    try:
        rec = json.loads(p.read_text())
    except (OSError, ValueError) as exc:
        sys.stderr.write(f"tooling-receipt: no usable receipt ({exc})\n")
        return 1
    key = rec.get("key")
    if not isinstance(key, str) or not key:
        sys.stderr.write("tooling-receipt: receipt has no key\n")
        return 1
    now = surface_key(root)
    if key != now:
        sys.stderr.write(
            f"tooling-receipt: STALE -- tooling surface is {now}, receipt "
            f"describes {key}\n")
        return 1
    return 0


def main(argv: list[str]) -> int:
    root = Path(".").resolve()
    if "--project" in argv:
        i = argv.index("--project")
        root = Path(argv[i + 1]).resolve()
        argv = argv[:i] + argv[i + 2:]
    verb = argv[0] if argv else ""
    if verb == "key":
        return cmd_key(root)
    if verb == "write":
        return cmd_write(root, argv[1:])
    if verb == "check":
        return cmd_check(root)
    sys.stderr.write("usage: tooling-receipt.py {key|write [--expect K]|check}"
                     " [--project DIR]\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
