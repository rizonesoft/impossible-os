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
blobs hold more copies of that exact line than its old blobs did: each file
is matched against its own old blob first, then leftover old lines cover
lines moved between files. Lines split on LF only; a binary
blob (a NUL in its first 8000 bytes, git's own test) holds no lines, and a
pair whose blob ids are equal is never read.

Generated files are exempt on both sides: their text is copied from sources
this check has already judged (todo/implementation-plan.md copies TODO
titles), so a change to one neither adds lines nor lends them.

Exit 0 when clean, 1 with one line per hit, 2 when the staged changes cannot
be read. SKIP_DASH_CHECK=1 skips it.
"""
import collections
import os
import subprocess
import sys
import threading

DASHES = (chr(0x2013).encode(), chr(0x2014).encode())
GENERATED = {"todo/implementation-plan.md", "COUNT.md"}
NULL_OID = "0" * 40
TIMEOUT = 60
BLOB_MODES = {"100644", "100755", "120000"}


def staged_changes(raw: bytes) -> list:
    """(old_oid, new_oid, path) per staged regular file or link from --raw -z.

    A deleted file keeps its old blob (its lines may move elsewhere) with a
    null new blob; a submodule has no blob. A change touching a generated
    file on either side is left out whole, so it neither adds nor lends."""
    out, parts, i = [], raw.split(b"\0"), 0
    while i < len(parts):
        head = parts[i].decode("ascii", "replace")
        if not head.startswith(":"):
            i += 1
            continue
        fields = head[1:].split(" ")
        status = fields[4][:1] if len(fields) > 4 else ""
        two = status in ("R", "C")
        src = parts[i + 1].decode("utf-8", "surrogateescape")
        path = parts[i + 2].decode("utf-8", "surrogateescape") if two else src
        i += 3 if two else 2
        if src in GENERATED or path in GENERATED:
            continue
        old_mode, new_mode, old, new = fields[0], fields[1], fields[2], fields[3]
        if old_mode not in BLOB_MODES:
            old = NULL_OID
        if new_mode not in BLOB_MODES or status == "D":
            new = NULL_OID
        if old == new:
            continue                                       # identical content, or nothing to read
        out.append((old, new, path))
    return out


def is_binary(blob: bytes) -> bool:
    """Git's own test: a NUL in the first 8000 bytes."""
    return b"\0" in blob[:8000]


def dash_lines(blob: bytes) -> list:
    """(line number, line) for each LF-delimited text line holding a dash.

    Only the lines that hold a dash are copied: the blob is searched for the
    dash bytes and each hit's line is cut out by its LF bounds."""
    if is_binary(blob):
        return []
    starts = set()
    for d in DASHES:
        at = blob.find(d)
        while at != -1:
            starts.add(blob.rfind(b"\n", 0, at) + 1)
            at = blob.find(d, at + 1)
    out, line_no, pos = [], 1, 0
    for start in sorted(starts):
        line_no += blob.count(b"\n", pos, start)
        pos = start
        end = blob.find(b"\n", start)
        out.append((line_no, blob[start:] if end == -1 else blob[start:end]))
    return out


class BlobReader:
    """One `git cat-file --batch` process, read one blob at a time so only the
    dash lines of each blob are kept. A timer kills the process at the
    deadline, so a stalled read ends in a short read, which is an error."""

    def __init__(self):
        self.proc = subprocess.Popen(["git", "cat-file", "--batch"], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE)   # deadline: the timer below
        self.timer = threading.Timer(TIMEOUT, self.proc.kill)
        self.timer.start()
        self.cache = {}

    def dash_lines(self, oid: str) -> list:
        if oid not in self.cache:
            self.cache[oid] = dash_lines(self._read(oid))
        return self.cache[oid]

    def _read(self, oid: str) -> bytes:
        self.proc.stdin.write(oid.encode() + b"\n")
        self.proc.stdin.flush()
        header = self.proc.stdout.readline().split(b" ")
        if len(header) < 3 or header[1] != b"blob":
            raise ValueError(f"cat-file did not return blob {oid}")
        size = int(header[2])
        data = self.proc.stdout.read(size)
        if len(data) != size or self.proc.stdout.read(1) != b"\n":
            raise ValueError(f"cat-file returned blob {oid} short")
        return data

    def close(self) -> int:
        try:
            self.proc.stdin.close()
        except OSError:
            pass                                           # fail-direction: wait() below reports the exit
        try:
            rc = self.proc.wait(timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            rc = self.proc.wait()
        self.timer.cancel()
        return rc


def added_dash_lines(changes: list, lines_of) -> list:
    """(path, line) for each dash line the change adds.

    Each file is matched against its own old blob first, so an untouched line
    is never blamed for a copy elsewhere. Lines left over in old blobs (moved
    out, or out of a deleted file) then cover the same lines appearing in
    other files; whatever is still unmatched is new."""
    pool, pending = collections.Counter(), []
    for old, new, path in changes:
        keep = collections.Counter(ln for _n, ln in lines_of(old)) if old != NULL_OID else collections.Counter()
        if new != NULL_OID:
            for n, ln in lines_of(new):
                if keep[ln] > 0:
                    keep[ln] -= 1
                else:
                    pending.append((path, n, ln))
        pool.update(keep)
    found = []
    for path, n, ln in pending:
        if pool[ln] > 0:
            pool[ln] -= 1
        else:
            found.append((path, n))
    return found


def hits() -> list:
    raw = subprocess.run(["git", "diff", "--cached", "--raw", "-z", "-M", "--no-abbrev", "--no-ext-diff"],
                         capture_output=True, timeout=TIMEOUT, check=True).stdout
    changes = staged_changes(raw)
    if not any(new != NULL_OID for _old, new, _path in changes):
        return []                                          # only deletions: nothing can be added
    reader = BlobReader()
    try:
        found = added_dash_lines(changes, reader.dash_lines)
    finally:
        rc = reader.close()
    if rc != 0:
        raise ValueError(f"cat-file exited {rc}")
    return found


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
