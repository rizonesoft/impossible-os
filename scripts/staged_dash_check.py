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
                                             (by hand)
       staged_dash_check.py --push REMOTE   every ref pre-push's stdin names,
                                             each commit once (pre-push)
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
BINARY_PREFIX = 8000                                       # a NUL in this prefix makes a blob binary (as git decides)


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
    if b"\0" in blob[:BINARY_PREFIX]:
        return None
    return blob[3:] if blob.startswith(UTF8_BOM) else blob


def dash_count(text) -> int:
    return 0 if text is None else sum(text.count(d) for d in DASHES)


def dash_lines(text: bytes) -> list:
    """(line number, line) for each line holding a dash; a CR before the LF is
    not part of the line, so a change of line endings never names one new."""
    return [(n, ln.removesuffix(b"\r")) for n, ln in enumerate(text.split(b"\n"), 1)
            if any(d in ln for d in DASHES)]


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
            blob = self._read(oid, count_only=True)
            self.counts[oid] = 0 if blob is None else dash_count(as_text(blob))
        return self.counts[oid]

    def text(self, oid: str):
        return None if oid == NULL_OID else as_text(self._read(oid))

    def _read(self, oid: str, count_only: bool = False):
        """The blob's bytes; with count_only, None for a binary blob, whose
        prefix decides that, so the rest is drained without being kept."""
        self.proc.stdin.write(oid.encode() + b"\n")
        self.proc.stdin.flush()
        header = self.proc.stdout.readline().split(b" ")
        if len(header) < 3 or header[1] != b"blob":
            raise ValueError(f"cat-file did not return blob {oid}")
        size = int(header[2])
        head = self.proc.stdout.read(min(size, BINARY_PREFIX))
        if count_only and len(head) == min(size, BINARY_PREFIX) and head[:2] not in UTF16_BOMS and b"\0" in head:
            left = size - len(head)
            while left:
                chunk = self.proc.stdout.read(min(left, 1 << 16))
                if not chunk:
                    break
                left -= len(chunk)
            if left or self.proc.stdout.read(1) != b"\n":
                raise ValueError(f"cat-file returned blob {oid} short")
            return None
        data = head + self.proc.stdout.read(size - len(head))
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
    others = [{path: old for old, _new, path in changes} for changes in per_parent[1:]]
    found = []
    for old, new, path in per_parent[0]:
        if not all(path in index for index in others):
            continue
        n = reader.count(new)
        if not n:
            continue
        olds = [old] + [index[path] for index in others]
        gained = min(n - reader.count(o) for o in olds)
        if gained > 0:
            exact, copies = name_lines([reader.text(o) for o in dict.fromkeys(olds) if reader.count(o)],
                                       reader.text(new))
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


def merge_heads(path: str = "") -> list:
    """The other parents of a merge being committed (MERGE_HEAD, or the file
    the caller already resolved), or none."""
    path = path or _git("rev-parse", "--git-path", "MERGE_HEAD").decode().strip()
    try:
        with open(path, encoding="ascii") as f:
            return f.read().split()
    except FileNotFoundError:
        return []                                          # fail-direction: no merge is in progress


def staged(merging: bool = True, merge_head: str = "") -> list:
    """The staged change against HEAD and, while a merge is being committed,
    against each other parent too, so a resolution is judged as the push
    check judges the merge commit: on what it adds over every parent.
    `merging=False` is pre-commit's call, which has already seen no MERGE_HEAD;
    `merge_head` is commit-msg's, the MERGE_HEAD file it found."""
    diff = ("diff", "--cached", "--raw", "-z", "-M", "--no-abbrev", "--no-ext-diff")
    per_parent = [changes_from_raw(_git(*diff))]
    per_parent += [changes_from_raw(_git(*diff, parent)) for parent in (merge_heads(merge_head) if merging else [])]
    return [("", per_parent)]


def outgoing(updates: list, remote: str = "") -> list:
    """(label, per-parent changes) for every commit the (old, new) ref updates
    send, merges included, each commit once however many refs carry it: one
    rev-list per update, then one `diff-tree --stdin` fed one line per parent.
    A deletion (new is the null id) sends nothing. Every walk stops at every
    remote oid the push names, a deletion's included: the destination holds
    those commits. A new ref also stops at the destination's tracking refs
    when the destination is a configured remote; a push by URL has no
    tracking refs of its own, so only the named oids bound it. With no
    destination at all (a manual --range) every remote's tracking refs do."""
    if not any(new != NULL_OID for _old, new in updates):
        return []
    known = sorted({old for old, _new in updates if old != NULL_OID})
    if not remote:
        tracking = ["--remotes"]
    elif any(old == NULL_OID and new != NULL_OID for old, new in updates) and subprocess.run(
            ["git", "config", "--get", f"remote.{remote}.url"], capture_output=True, timeout=TIMEOUT).returncode == 0:
        tracking = [f"--remotes={remote}"]
    else:
        tracking = []
    pairs, seen = [], set()                               # (commit, parent or None), in input order
    for old, new in updates:
        if new == NULL_OID:
            continue
        span = ["--ignore-missing", new, "--not", *(tracking if old == NULL_OID else []), *known]
        for line in _git("rev-list", "--reverse", "--parents", *span).decode().splitlines():
            commit, *parents = line.split()
            if commit in seen:
                continue
            seen.add(commit)
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


def push_updates(lines: str) -> list:
    """(remote old, local new) per ref a push updates, from pre-push's stdin
    (`<local ref> <local oid> <remote ref> <remote oid>`). A deletion is kept,
    with the null id as its new: it sends nothing, but its remote oid is a
    commit the destination holds. git separates records by LF and fields by an
    ASCII space. The local ref is the source as typed and can hold a space
    (`HEAD@{0 seconds ago}`); the other three fields cannot, so they are
    taken from the right. Any other whitespace belongs to its field."""
    updates = []
    for line in lines.split("\n"):
        fields = line.rsplit(" ", 3) if line else []
        if len(fields) != 4:
            if fields:
                raise ValueError(f"pre-push gave an unreadable line: {line!r}")
            continue
        updates.append((fields[3], fields[1]))
    return updates


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
            found = judge(outgoing([(argv[1], argv[2])], *argv[3:]))
        elif argv[:1] == ["--push"] and len(argv) == 2:
            found = judge(outgoing(push_updates(sys.stdin.read()), argv[1]))
        elif not argv:
            found = judge(staged())
        elif argv == ["--no-merge"]:
            found = judge(staged(merging=False))
        elif argv[:1] == ["--merge-head"] and len(argv) == 2:
            found = judge(staged(merge_head=argv[1]))
        else:
            print("usage: staged_dash_check.py [--no-merge | --merge-head FILE | --range OLD NEW [REMOTE] | --push REMOTE]", file=sys.stderr)
            return 2
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        print(f"[dash-check] the changes could not be read: {exc}", file=sys.stderr)
        return 2
    report(found)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
