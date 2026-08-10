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

# MACHINE-REWRITTEN REGIONS ARE EXCLUDED FROM THE HASH, and this is not a
# convenience: without it the receipt is useless for the exact workflow it
# exists for. `.githooks/post-commit` rewrites README.md's lines-of-code badge
# on EVERY commit, so `run suite -> commit -> push` invalidated the receipt
# between the run and the push, every time. Found on the first end-to-end use,
# by the push paying the six minutes the receipt was built to save.
#
# Excluding the FILE would be wrong -- the suite asserts that README.md names
# the wrappers it should, so a real README edit must still invalidate. Only the
# delimited auto-generated span is dropped, so everything a human writes in that
# file still counts. Keep this list to regions a HOOK regenerates; anything else
# belongs in the hash.
GENERATED_SPANS = {
    "README.md": ("COUNT-BADGE:START", "COUNT-BADGE:END"),
}


def _hashable_bytes(rel: str, data: bytes) -> bytes:
    span = GENERATED_SPANS.get(rel)
    if not span:
        return data
    start, end = span
    out, skipping = [], False
    for line in data.split(b"\n"):
        if start.encode() in line:
            skipping = True
        if not skipping:
            out.append(line)
        if end.encode() in line:
            skipping = False
    return b"\n".join(out)


def _git(root: Path, *args: str) -> str:
    try:
        r = subprocess.run(("git", "-C", str(root), *args),
                           capture_output=True, text=True, timeout=60)
        return r.stdout if r.returncode == 0 else ""
    except Exception:
        return ""


def _git_bytes(root: Path, *args: str):
    """Raw stdout, or None on ANY failure.

    Distinct from `_git`, which collapses failure to "". That collapse is safe
    where an empty result and a failed result mean the same thing (no files),
    and unsafe in `surface_key_at_commit`, where it would turn "git could not
    read this commit" into "this commit has an empty surface" -- and an empty
    surface hashes to a stable value that could then MATCH another empty one.
    """
    try:
        r = subprocess.run(("git", "-C", str(root), *args),
                           capture_output=True, timeout=60)
        return r.stdout if r.returncode == 0 else None
    except Exception:
        return None


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
            h = hashlib.sha256(
                _hashable_bytes(rel, (root / rel).read_bytes())).hexdigest()
        except OSError:
            h = "absent"
        items.append((rel, h))
    return hashlib.sha256(json.dumps(items).encode()).hexdigest()[:16]


def surface_key_at_commit(root: Path, sha: str) -> str | None:
    """The same key, computed over the surface AS COMMITTED at `sha`.

    WHY THIS EXISTS (v13 carry, 2026-08-10). `surface_key` hashes the LIVE
    WORKING TREE, which is correct for "may I skip the suite right now?" and
    exactly wrong for "may I skip the suite for the commit I am pushing?". The
    run backgrounds its ship push and keeps working; every later edit to any
    surface file moves the worktree key, so a receipt that validly attests the
    PUSHED bytes is refused and the ~6-minute suite runs inside the push --
    the very cost the receipt exists to avoid, reintroduced by unrelated work.

    The filed item read this as "the pre-push receipt is not content-bound; bind
    it like tooling-receipt does". That premise does not hold: this receipt was
    already content-bound, and content-binding TO THE WORKTREE is precisely what
    produces the symptom. The missing piece is a second question, not a second
    mechanism.

    SOUNDNESS. A pass here means the bytes the suite verified are byte-identical
    to the bytes in `sha` -- not similar, not a superset. Untracked files make it
    fail rather than pass: they are part of `surface_key` and cannot be part of a
    commit, so a suite run with an untracked surface file present can never match
    a commit key, and the caller falls through to running the suite. That is the
    conservative direction and it is the only one available.

    Returns None on any git failure -- the caller must treat that as "cannot
    attest", never as a match.
    """
    raw = _git_bytes(root, "ls-tree", "-r", "--name-only", "-z", sha,
                     "--", *SURFACE)
    if raw is None:
        return None
    names = raw.decode("utf-8", "replace")
    rels = sorted(p for p in names.split("\0") if p.strip())
    if not rels:
        return None
    items = []
    for rel in rels:
        blob = _git_bytes(root, "show", f"{sha}:{rel}")
        if blob is None:
            return None
        items.append((rel, hashlib.sha256(
            _hashable_bytes(rel, blob)).hexdigest()))
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


def cmd_check(root: Path, commit: str | None = None) -> int:
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
        # SECOND QUESTION, asked only when the first one fails. The worktree has
        # moved -- but if the receipt describes exactly the surface AS COMMITTED
        # at the commit being pushed, the suite has already run on the bytes
        # about to leave this machine, and a later unrelated edit is not a reason
        # to re-run it inside the push.
        if commit:
            at = surface_key_at_commit(root, commit)
            if at is not None and at == key:
                sys.stderr.write(
                    f"tooling-receipt: worktree has moved ({now} != {key}), but "
                    f"the receipt describes the surface AS COMMITTED at "
                    f"{commit[:12]} exactly -- the suite already ran on the bytes "
                    f"being pushed.\n")
                return 0
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
        # The commit whose bytes are actually being pushed. Optional: without it
        # `check` keeps its original worktree-only semantics exactly.
        commit = None
        if "--commit" in argv:
            i = argv.index("--commit")
            commit = argv[i + 1] if i + 1 < len(argv) else None
        return cmd_check(root, commit)
    sys.stderr.write("usage: tooling-receipt.py {key|write [--expect K]|check [--commit SHA]}"
                     " [--project DIR]\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
