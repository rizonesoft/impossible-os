#!/usr/bin/env python3
"""Refuse a commit that adds a line holding a Unicode en or em dash.

The dash rule (CLAUDE.md "Code Style: ASCII Dashes") was enforced only by
Claude's edit hook, which binds no other lane. This check runs from
.githooks/pre-commit, so it binds every lane that commits. It refuses only
lines the commit ADDS: the 2026-11-21 tree still holds dashes in about 225
older files, which are not swept, and an untouched, moved or renamed line is
never refused.

No diff text is read, so no diff setting (prefixes, context, inter-hunk
context, textconv), pathspec mode or file name can change the answer. One
`git diff --cached --raw -z -M` names each staged file with its old and new
blob (a rename or copy pairs with its source), one `git cat-file --batch`
reads the blobs, and a dash line counts as added when the new blob holds more
copies of that exact line than the old one did. Lines split on LF only.

Generated files are exempt: their text is copied from sources this check has
already judged (todo/implementation-plan.md copies TODO titles).

Exit 0 when clean, 1 with one line per hit, 2 when the staged changes cannot
be read. SKIP_DASH_CHECK=1 skips it.
"""
import collections
import os
import subprocess
import sys

DASHES = (chr(0x2013).encode(), chr(0x2014).encode())
GENERATED = {"todo/implementation-plan.md", "COUNT.md"}
NULL_OID = "0" * 40
TIMEOUT = 60
BLOB_MODES = {"100644", "100755", "120000"}


def staged_changes(raw: bytes) -> list:
    """(old_oid, new_oid, path) per staged text-or-link file from --raw -z."""
    out, parts, i = [], raw.split(b"\0"), 0
    while i < len(parts):
        head = parts[i].decode("ascii", "replace")
        if not head.startswith(":"):
            i += 1
            continue
        fields = head[1:].split(" ")
        status = fields[4][:1] if len(fields) > 4 else ""
        two = status in ("R", "C")
        path_raw = parts[i + 2] if two else parts[i + 1]
        i += 3 if two else 2
        old_mode, new_mode, old, new = fields[0], fields[1], fields[2], fields[3]
        if status == "D" or new_mode not in BLOB_MODES:
            continue                                       # deleted, or a submodule
        if old_mode not in BLOB_MODES:
            old = NULL_OID                                 # was nothing, or a submodule
        out.append((old, new, path_raw.decode("utf-8", "surrogateescape")))
    return out


def read_blobs(oids: list) -> dict:
    want = sorted({o for o in oids if o != NULL_OID})
    if not want:
        return {}
    res = subprocess.run(["git", "cat-file", "--batch"], input=("\n".join(want) + "\n").encode(),
                         capture_output=True, timeout=TIMEOUT, check=True).stdout
    blobs, pos = {}, 0
    for oid in want:
        nl = res.index(b"\n", pos)
        header = res[pos:nl].split(b" ")
        size = int(header[2])
        blobs[oid] = res[nl + 1:nl + 1 + size]
        pos = nl + 1 + size + 1
    return blobs


def added_dash_lines(old: bytes, new: bytes) -> list:
    """1-based line numbers in `new` holding a dash line `old` did not have as often."""
    budget = collections.Counter(ln for ln in old.split(b"\n") if any(d in ln for d in DASHES))
    out = []
    for n, ln in enumerate(new.split(b"\n"), 1):
        if any(d in ln for d in DASHES):
            if budget[ln] > 0:
                budget[ln] -= 1
            else:
                out.append(n)
    return out


def hits() -> list:
    raw = subprocess.run(["git", "diff", "--cached", "--raw", "-z", "-M", "--no-abbrev", "--no-ext-diff"],
                         capture_output=True, timeout=TIMEOUT, check=True).stdout
    changes = [c for c in staged_changes(raw) if c[2] not in GENERATED]
    blobs = read_blobs([o for c in changes for o in c[:2]])
    found = []
    for old, new, path in changes:
        for n in added_dash_lines(blobs.get(old, b""), blobs.get(new, b"")):
            found.append((path, n))
    return found


def main() -> int:
    if os.environ.get("SKIP_DASH_CHECK") == "1":
        return 0
    try:
        found = hits()
    except (OSError, subprocess.SubprocessError, ValueError, IndexError) as exc:
        print(f"[dash-check] the staged changes could not be read: {exc}", file=sys.stderr)
        return 2
    for path, line in found:
        print(f"[dash-check] {path}:{line} adds a Unicode en or em dash; rewrite it with a comma, colon, "
              "parentheses or a full stop", file=sys.stderr)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
