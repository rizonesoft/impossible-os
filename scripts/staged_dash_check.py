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

The rule is the UTF-8 encoding of U+2013 and U+2014 (the bytes Windows serial
and CMD garble), read from UTF-8 text and from BOM-marked UTF-16 text; other
source encodings are outside it, and the tree holds none.

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
    null new blob; a submodule has no blob. The generated-file exemption
    applies to each side alone: a generated source lends no lines and a
    generated destination adds none, so a rename out of one is still read."""
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
        old_mode, new_mode, old, new = fields[0], fields[1], fields[2], fields[3]
        if old_mode not in BLOB_MODES or src in GENERATED:
            old = NULL_OID                                 # a generated source lends nothing
        if new_mode not in BLOB_MODES or status == "D" or path in GENERATED:
            new = NULL_OID                                 # a generated destination adds nothing
        if old == new:
            continue                                       # identical content, or nothing to read
        out.append((old, new, path))
    return out


UTF16_BOMS = (b"\xff\xfe", b"\xfe\xff")


def as_text(blob: bytes):
    """The blob as UTF-8 text bytes, or None for a binary blob.

    A BOM-marked UTF-16 file (a PowerShell script saved by Windows) is text
    and is decoded; otherwise git's own test applies: a NUL in the first 8000
    bytes means binary."""
    if blob[:2] in UTF16_BOMS:
        try:
            return blob.decode("utf-16").encode("utf-8")
        except UnicodeDecodeError as exc:
            raise ValueError(f"a UTF-16 file could not be decoded ({exc.reason})") from exc
    return None if b"\0" in blob[:8000] else blob


class _DashFinder:
    """The next dash at or after a position. Each dash's next hit is cached
    and searched for again only once the position passes it, so the whole
    text is scanned once per dash."""

    def __init__(self, text: bytes):
        self.text, self.next = text, [text.find(d) for d in DASHES]

    def at_or_after(self, pos: int) -> int:
        for i, d in enumerate(DASHES):
            if self.next[i] != -1 and self.next[i] < pos:
                self.next[i] = self.text.find(d, pos)
        live = [n for n in self.next if n != -1]
        return min(live) if live else -1


def dash_lines(blob: bytes) -> list:
    """(line number, line) for each LF-delimited text line holding a dash.

    Only the lines that hold a dash are copied. The search jumps from a hit
    to the end of its line, so a line with many dashes is scanned once."""
    text = as_text(blob)
    if text is None:
        return []
    finder = _DashFinder(text)
    out, line_no, pos, at = [], 1, 0, finder.at_or_after(0)
    while at != -1:
        nl = text.rfind(b"\n", pos, at)
        start = nl + 1 if nl != -1 else pos
        line_no += text.count(b"\n", pos, start)
        end = text.find(b"\n", at)
        out.append((line_no, text[start:] if end == -1 else text[start:end]))
        if end == -1:
            break
        pos, line_no = end + 1, line_no + 1
        at = finder.at_or_after(pos)
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
    """(path, candidate line numbers, count) for each run of dash lines the change adds.

    Each file is matched against its own old blob first, so an untouched line
    is never blamed for a copy elsewhere. Lines left over in the change's old
    blobs (moved out, or out of a deleted file) then cover the same lines
    appearing in other files. When identical lines make the new copy
    ambiguous, every candidate is named. Old blobs are read only for files
    whose new blob holds a dash, and the rest only if something is unmatched."""
    pending, matched = [], set()
    for i, (old, new, path) in enumerate(changes):
        new_lines = lines_of(new) if new != NULL_OID else []
        if not new_lines:
            continue
        matched.add(i)
        kept = collections.Counter(ln for _n, ln in lines_of(old)) if old != NULL_OID else collections.Counter()
        where = collections.defaultdict(list)
        for n, ln in new_lines:
            where[ln].append(n)
        for ln, numbers in where.items():
            excess = len(numbers) - kept[ln]
            if excess > 0:
                pending.append([path, ln, numbers, excess, kept[ln] > 0])
    if not pending:
        return []
    pool = collections.Counter()
    for i, (old, new, _path) in enumerate(changes):
        if old != NULL_OID:
            lines = [ln for _n, ln in lines_of(old)]
            if i in matched:
                lines = (collections.Counter(lines) - collections.Counter(ln for _n, ln in lines_of(new))).elements()
            pool.update(lines)
    found = []
    for path, ln, numbers, excess, _shared in pending:
        take = min(pool[ln], excess)
        pool[ln] -= take
        if excess - take > 0:
            found.append((path, tuple(numbers), excess - take))
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
    fix = "rewrite it with a comma, colon, parentheses or a full stop"
    for path, numbers, count in found:
        if count == len(numbers):
            for n in numbers:
                print(f"[dash-check] {path}:{n} adds a Unicode en or em dash; {fix}", file=sys.stderr)
        else:
            where = ", ".join(f"{path}:{n}" for n in numbers)
            print(f"[dash-check] {path} adds {count} line(s) with a Unicode en or em dash; identical lines make "
                  f"the new copy one of {where}; {fix}", file=sys.stderr)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
