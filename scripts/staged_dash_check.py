#!/usr/bin/env python3
"""Refuse a commit that adds a Unicode en or em dash.

The dash rule (CLAUDE.md "Code Style: ASCII Dashes") was enforced only by
Claude's edit hook, which binds no other lane. This check runs from
.githooks/pre-commit on the staged change and from .githooks/pre-push on
every outgoing commit (a rebase that resolves a conflict commits without
pre-commit), so it binds every lane. The 2026-11-21 tree still holds dashes
in about 225 older files, which are not swept.

The decision is one count per file: a file the change touches is refused
when its new version holds more en or em dashes than its old version did.
No line matching decides anything, so line order, a byte-order mark, line
endings, diff settings, pathspec modes and file names cannot change the
answer. A rename or copy is compared with its source, as `git diff -M` pairs
them; a dash line moved INTO another file counts as written there. Only on a
refusal are lines matched, to name the new or edited lines (or, when the
gain is an extra copy of an identical line, every copy).

One `git diff --raw -z -M` (or, for a push, one `git diff-tree --stdin`
over every outgoing commit and parent) names each file with its old and new
blob, and one streamed `git cat-file --batch` reads them; only dash counts
are kept. A file's old blob is read only when its new blob holds a dash. A
merge is judged too: a file is refused when it gains over every parent.
The rule is the UTF-8 encoding of U+2013 and U+2014 (the bytes Windows
serial and CMD garble), read from UTF-8 text and BOM-marked UTF-16 text; a
binary blob (a NUL in its first 8000 bytes, git's own test) holds none.

Generated files are exempt: their text is copied from sources this check has
already judged (todo/implementation-plan.md copies TODO titles).

Usage: staged_dash_check.py                 the staged change (pre-commit)
       staged_dash_check.py --range OLD NEW [REMOTE]
                                             each commit in OLD..NEW; OLD all
                                             zeros means every commit of NEW
                                             that REMOTE's tracking refs (or,
                                             without any, no remote's) hold
                                             (pre-push)
Exit 0 when clean, 1 with the refused files, 2 when the changes cannot be
read. SKIP_DASH_CHECK=1 skips it.
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
UTF8_BOM = b"\xef\xbb\xbf"
UTF16_BOMS = (b"\xff\xfe", b"\xfe\xff")


def changes_from_raw(raw: bytes) -> list:
    """(old_oid, new_oid, path) per changed regular file or link in --raw -z.

    A deleted file, a submodule and a generated destination have no new blob
    to judge and are left out; a generated or non-blob source compares as
    empty. Equal blob ids cannot gain anything and are left out."""
    return changes_by_header(raw)[0][1] if raw else []


def changes_by_header(raw: bytes) -> list:
    """[(header, changes)] from raw -z output. A part that does not start
    with ':' where an entry is expected is a header (`diff-tree --stdin`
    prints the commit id before each pair's entries); output without headers
    is one block with header None."""
    blocks, parts, i = [], raw.split(b"\0"), 0
    while i < len(parts):
        part = parts[i]
        if not part.startswith(b":"):
            if part:
                blocks.append((part.decode("ascii", "replace"), []))
            i += 1
            continue
        if not blocks:
            blocks.append((None, []))
        fields = part[1:].decode("ascii", "replace").split(" ")
        status = fields[4][:1] if len(fields) > 4 else ""
        two = status in ("R", "C")
        src = parts[i + 1].decode("utf-8", "surrogateescape")
        path = parts[i + 2].decode("utf-8", "surrogateescape") if two else src
        i += 3 if two else 2
        old_mode, new_mode, old, new = fields[0], fields[1], fields[2], fields[3]
        if new_mode not in BLOB_MODES or status == "D" or path in GENERATED:
            continue
        if old_mode not in BLOB_MODES or src in GENERATED:
            old = NULL_OID
        if old != new:
            blocks[-1][1].append((old, new, path))
    return blocks


def as_text(blob: bytes):
    """The blob as UTF-8 text bytes without a byte-order mark, or None for a
    binary blob. A BOM-marked UTF-16 file is decoded; one that cannot be
    decoded is a read error, never clean."""
    if blob[:2] in UTF16_BOMS:
        try:
            return blob.decode("utf-16").encode("utf-8")
        except UnicodeDecodeError as exc:
            raise ValueError(f"a UTF-16 file could not be decoded ({exc.reason})") from exc
    if b"\0" in blob[:8000]:
        return None
    return blob[3:] if blob.startswith(UTF8_BOM) else blob


def dash_count(text) -> int:
    return 0 if text is None else sum(text.count(d) for d in DASHES)


def dash_lines(text: bytes) -> list:
    """(line number, line) for each LF-delimited line holding a dash."""
    return [(n, ln) for n, ln in enumerate(text.split(b"\n"), 1) if any(d in ln for d in DASHES)]


class BlobReader:
    """One `git cat-file --batch` process. Each blob's dash count is cached by
    id and its text is not kept; a refused file's two blobs are read again to
    name lines. A timer kills the process at the deadline, so a stalled read
    ends in a short read, which is an error."""

    def __init__(self):
        self.proc = subprocess.Popen(["git", "cat-file", "--batch"], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE)   # deadline: the timer below
        self.timer = threading.Timer(TIMEOUT, self.proc.kill)
        self.timer.start()
        self.counts = {}

    def count(self, oid: str) -> int:
        if oid == NULL_OID:
            return 0
        if oid not in self.counts:
            self.counts[oid] = dash_count(as_text(self._read(oid)))
        return self.counts[oid]

    def text(self, oid: str):
        return None if oid == NULL_OID else as_text(self._read(oid))

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


def name_lines(old_texts, new_text):
    """(exact, copies): dash lines of the new text that no parent holds, and
    every copy of an identical line that now appears more often than in any
    parent. A merge's line that one parent already had is never named new."""
    new_lines = dash_lines(new_text)
    old_seen = collections.Counter()
    for text in old_texts:
        if text:
            for ln, k in collections.Counter(ln for _n, ln in dash_lines(text)).items():
                old_seen[ln] = max(old_seen[ln], k)
    new_seen = collections.Counter(ln for _n, ln in new_lines)
    exact = [n for n, ln in new_lines if not old_seen[ln]]
    copies = [n for n, ln in new_lines if old_seen[ln] and new_seen[ln] > old_seen[ln]]
    return exact, copies


def refused(per_parent: list, reader) -> list:
    """(path, gained, exact lines, candidate copies) for each file that holds
    more dashes than before. `per_parent` holds one change list per parent
    (one for a commit or the staged change, several for a merge); a file is
    refused only when it gains over every parent, so a merge is judged on
    what the merge itself added. Only a path changed against every parent is
    read, and a new blob with no dash is never compared."""
    if not per_parent:
        return []
    by_path = [{path: old for old, _new, path in changes} for changes in per_parent]
    found = []
    for old, new, path in per_parent[0]:
        if not all(path in olds for olds in by_path[1:]):
            continue
        n = reader.count(new)
        if not n:
            continue
        olds = [olds[path] for olds in by_path]
        gained = min(n - reader.count(o) for o in olds)
        if gained > 0:
            exact, copies = name_lines([reader.text(o) for o in olds], reader.text(new))
            found.append((path, gained, exact, copies))
    return found


def _git(*args, stdin=None) -> bytes:
    return subprocess.run(["git", *args], input=stdin, capture_output=True, timeout=TIMEOUT, check=True).stdout


def judge(commits: list) -> list:
    """(label, refused files) for each (label, per-parent changes), one blob reader for all."""
    if not any(changes for _label, per_parent in commits for changes in per_parent):
        return []
    reader = BlobReader()
    try:
        out = [(label, hits) for label, per_parent in commits if (hits := refused(per_parent, reader))]
    finally:
        rc = reader.close()
    if rc != 0:
        raise ValueError(f"cat-file exited {rc}")
    return out


def merge_heads() -> list:
    """The other parents of a merge being committed (MERGE_HEAD), or none."""
    path = _git("rev-parse", "--git-path", "MERGE_HEAD").decode().strip()
    try:
        with open(path, encoding="ascii") as f:
            return f.read().split()
    except FileNotFoundError:
        return []                                          # fail-direction: no merge is in progress


def staged() -> list:
    """The staged change against HEAD and, while a merge is being committed,
    against each other parent too, so a resolution is judged as the push
    check judges the merge commit: on what it adds over every parent."""
    diff = ("diff", "--cached", "--raw", "-z", "-M", "--no-abbrev", "--no-ext-diff")
    per_parent = [changes_from_raw(_git(*diff))]
    per_parent += [changes_from_raw(_git(*diff, parent)) for parent in merge_heads()]
    return [("", per_parent)]


def outgoing(old: str, new: str, remote: str = "") -> list:
    """(label, per-parent changes) for every outgoing commit, merges included,
    from one rev-list and one `diff-tree --stdin` fed one line per parent. A
    new ref takes every commit the destination remote's tracking refs do not
    hold; a push by URL, or to a remote with none, falls back to all remotes."""
    if old != NULL_OID:
        span = [f"{old}..{new}"]
    elif remote and _git("for-each-ref", "--count=1", f"refs/remotes/{remote}/").strip():
        span = [new, "--not", f"--remotes={remote}"]
    else:
        span = [new, "--not", "--remotes"]
    pairs = []                                            # (commit, parent or None), in input order
    for line in _git("rev-list", "--reverse", "--parents", *span).decode().splitlines():
        commit, *parents = line.split()
        if parents:
            pairs.extend((commit, p) for p in parents)
        else:
            pairs.append((commit, None))
    if not pairs:
        return []
    feed = "".join(f"{c} {p}\n" if p else f"{c}\n" for c, p in pairs).encode()
    blocks = changes_by_header(_git("diff-tree", "--stdin", "--always", "-r", "-z", "-M", "--no-abbrev", "--root",
                                    stdin=feed))
    if len(blocks) != len(pairs) or any(h != c for (h, _ch), (c, _p) in zip(blocks, pairs)):
        raise ValueError("diff-tree returned a different set of commits than it was given")
    commits = {}
    for (commit, _p), (_h, changes) in zip(pairs, blocks):
        commits.setdefault(commit, []).append(changes)
    return [(f"commit {c[:12]}: ", per_parent) for c, per_parent in commits.items()]


def report(found: list) -> None:
    fix = "rewrite it with a comma, colon, parentheses or a full stop"
    for label, hits in found:
        for path, gained, exact, copies in hits:
            print(f"[dash-check] {label}{path} gains {gained} Unicode en or em dash(es); {fix}", file=sys.stderr)
            for n in exact:
                print(f"[dash-check]   {path}:{n} is a new or edited line holding one", file=sys.stderr)
            if copies:
                where = ", ".join(f"{path}:{n}" for n in copies)
                print(f"[dash-check]   an identical line now appears more often; the new copy is one of {where}",
                      file=sys.stderr)


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if os.environ.get("SKIP_DASH_CHECK") == "1":
        return 0
    try:
        if argv[:1] == ["--range"] and len(argv) in (3, 4):
            found = judge(outgoing(*argv[1:]))
        elif not argv:
            found = judge(staged())
        else:
            print("usage: staged_dash_check.py [--range OLD NEW [REMOTE]]", file=sys.stderr)
            return 2
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        print(f"[dash-check] the changes could not be read: {exc}", file=sys.stderr)
        return 2
    report(found)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
