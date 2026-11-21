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
blob (a rename or copy pairs with its source), one streamed `git cat-file
--batch` reads them, and a dash line counts as added when the change's new
blobs hold more copies of that exact line than its old blobs did, so a line
moved within or between files is not new. Lines split on LF only; a binary
blob (a NUL in its first 8000 bytes, git's own test) holds no lines, and a
pair whose blob ids are equal is never read.

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
    """(old_oid, new_oid, path) per staged regular file or link from --raw -z.

    A deleted file keeps its old blob (its lines may move elsewhere) with a
    null new blob; a submodule has no blob and is left out."""
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
        if old_mode not in BLOB_MODES:
            old = NULL_OID
        if new_mode not in BLOB_MODES or status == "D":
            new = NULL_OID
        if old == new:
            continue                                       # identical content, or nothing to read
        out.append((old, new, path_raw.decode("utf-8", "surrogateescape")))
    return out


def is_binary(blob: bytes) -> bool:
    """Git's own test: a NUL in the first 8000 bytes."""
    return b"\0" in blob[:8000]


def dash_lines(blob: bytes) -> list:
    """(line number, line) for each LF-delimited text line holding a dash."""
    if is_binary(blob) or not any(d in blob for d in DASHES):
        return []
    return [(n, ln) for n, ln in enumerate(blob.split(b"\n"), 1) if any(d in ln for d in DASHES)]


class BlobReader:
    """One `git cat-file --batch` process, read one blob at a time so only the
    dash lines of each blob are kept."""

    def __init__(self):
        self.proc = subprocess.Popen(["git", "cat-file", "--batch"], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE)   # deadline: closed in close(), killed on error

    def read(self, oid: str) -> bytes:
        self.proc.stdin.write(oid.encode() + b"\n")
        self.proc.stdin.flush()
        header = self.proc.stdout.readline().split(b" ")
        if len(header) < 3:
            raise ValueError(f"cat-file did not return {oid}")
        size = int(header[2])
        data = self.proc.stdout.read(size)
        self.proc.stdout.read(1)                           # the LF after each object
        return data

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=TIMEOUT)
        except (OSError, subprocess.TimeoutExpired):
            self.proc.kill()


def added_dash_lines(changes: list, read) -> list:
    """(path, line) for each dash line the change adds.

    The budget is every dash line in the change's old blobs, so a line moved
    within a file, between files or out of a deleted file is not new; a line
    the new blobs hold more often than the old ones did is."""
    budget = collections.Counter()
    for old, _new, _path in changes:
        if old != NULL_OID:
            budget.update(ln for _n, ln in dash_lines(read(old)))
    found = []
    for _old, new, path in changes:
        if new == NULL_OID or path in GENERATED:
            continue
        for n, ln in dash_lines(read(new)):
            if budget[ln] > 0:
                budget[ln] -= 1
            else:
                found.append((path, n))
    return found


def hits() -> list:
    raw = subprocess.run(["git", "diff", "--cached", "--raw", "-z", "-M", "--no-abbrev", "--no-ext-diff"],
                         capture_output=True, timeout=TIMEOUT, check=True).stdout
    changes = staged_changes(raw)
    if not changes:
        return []
    reader = BlobReader()
    try:
        return added_dash_lines(changes, reader.read)
    finally:
        reader.close()


def main() -> int:
    if os.environ.get("SKIP_DASH_CHECK") == "1":
        return 0
    try:
        found = hits()
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        print(f"[dash-check] the staged changes could not be read: {exc}", file=sys.stderr)
        return 2
    for path, line in found:
        print(f"[dash-check] {path}:{line} adds a Unicode en or em dash; rewrite it with a comma, colon, "
              "parentheses or a full stop", file=sys.stderr)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
