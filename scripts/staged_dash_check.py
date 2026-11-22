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
answer. A detected rename is compared with its source, as `git diff -M` pairs
them; a new copy whose original remains is a new file, compared with an empty
one; a dash line moved INTO another file counts as written there. Only on a
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
       staged_dash_check.py --range OLD NEW [URL]
                                             each commit in OLD..NEW; OLD all
                                             zeros means every commit of NEW
                                             the repository at URL does not
                                             advertise (without URL, that no
                                             remote-tracking ref holds; by hand)
       staged_dash_check.py --push URL      every ref pre-push's stdin names,
                                             each commit once, against what the
                                             destination URL advertises (pre-push)
Exit 0 when clean, 1 with the refused files, 2 when the changes cannot be
read. SKIP_DASH_CHECK=1 skips it.
"""
import codecs
import hashlib
import collections
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bounded_run                                         # the shared bounded runner beside this script

DASHES = (chr(0x2013).encode(), chr(0x2014).encode())
GENERATED = {"todo/implementation-plan.md", "COUNT.md"}
NULL_OID = "0" * 40
TIMEOUT = 60
BLOB_MODES = {"100644", "100755", "120000"}
UTF8_BOM = b"\xef\xbb\xbf"
UTF16_BOMS = (b"\xff\xfe", b"\xfe\xff")
BINARY_PREFIX = 8000                                       # a NUL in this prefix makes a blob binary (as git decides)
CHUNK = 1 << 16


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


def dash_count(text) -> int:
    return 0 if text is None else sum(text.count(d) for d in DASHES)


def _str_dashes(text: str) -> int:
    return text.count(chr(0x2013)) + text.count(chr(0x2014))


def line_key(line) -> bytes:
    """What two dash lines are compared by: a digest of the line's bytes."""
    return hashlib.sha256(line).digest()


def dash_lines(text: bytes) -> list:
    """(line number, line key) for each line holding a dash; a CR before the
    LF is not part of the line, so a change of line endings never names one
    new. Only dash-bearing lines are hashed, in place, so a large file with
    one dash copies no line at all."""
    # One forward pass: the next dash of each kind is found once and kept until the scan passes it, and after
    # a hit the scan resumes past that line's end, so no byte is searched more than once per dash kind.
    out, pos, line_no, counted_to = [], 0, 1, 0
    nxt = {d: text.find(d) for d in DASHES}
    while True:
        for d, at in nxt.items():
            if 0 <= at < pos:
                nxt[d] = text.find(d, pos)
        live = [at for at in nxt.values() if at >= 0]
        if not live:
            return out
        at = min(live)
        start = text.rfind(b"\n", pos, at) + 1 or pos
        line_no += text.count(b"\n", counted_to, start)
        counted_to = start
        end = text.find(b"\n", at)
        stop = len(text) if end < 0 else end
        stop -= text.endswith(b"\r", start, stop)
        out.append((line_no, line_key(memoryview(text)[start:stop])))
        if end < 0:
            return out
        pos = end + 1


class DashLineKeys:
    """dash_lines over text fed in pieces. Lines that end inside a piece are
    named as dash_lines names them; only the line still open at a piece's end
    is carried, and only as a running digest, so a long line is never held."""

    def __init__(self):
        self.out, self.no = [], 1
        self._open()

    def _open(self):
        self.h, self.dash, self.tail, self.cr = hashlib.sha256(), False, b"", False

    def _extend(self, seg: bytes):                         # more of the open line
        if not seg:
            return
        if not self.dash:
            self.dash = any(d in seg or d in self.tail + seg[:2] for d in DASHES)
        self.tail = seg[-2:] if len(seg) >= 2 else (self.tail + seg)[-2:]
        if self.cr:                                        # a CR held back was not the line's last byte
            self.h.update(b"\r")
        self.cr = seg.endswith(b"\r")
        self.h.update(memoryview(seg)[:-1] if self.cr else seg)

    def _close(self):                                      # the open line ends; a CR held back is dropped
        if self.dash:
            self.out.append((self.no, self.h.digest()))
        self.no += 1
        self._open()

    def feed(self, data: bytes):
        first = data.find(b"\n")
        if first < 0:
            self._extend(data)
            return
        self._extend(data[:first])
        self._close()
        last = data.rfind(b"\n")
        if last > first:
            self.out.extend((self.no + n - 1, k) for n, k in dash_lines(data[first + 1:last + 1]))
            self.no += data.count(b"\n", first + 1, last + 1)
        self._extend(data[last + 1:])

    def end(self) -> list:
        self._close()
        return self.out


class BlobReader:
    """One `git cat-file --batch` process, run as a process group bounded in
    time (bounded_run.Stream): at the deadline, or on Ctrl-C or SIGTERM, the
    whole group ends, so a stalled read ends in a short read, which is an
    error. Each blob's dash count is cached by id and its text is not kept; a
    refused file's blobs are read again, as a stream, to name lines."""

    def __init__(self):
        self.stream = bounded_run.Stream(["git", "cat-file", "--batch"], "git cat-file", timeout=TIMEOUT,
                                         env=_git_env())   # deadline: the stream's own timer
        self.proc = self.stream.proc
        self.counts = {}

    def count(self, oid: str) -> int:
        if oid == NULL_OID:
            return 0
        if oid not in self.counts:
            self.counts[oid] = self._count(oid)
        return self.counts[oid]

    def _header(self, oid: str) -> int:
        self.proc.stdin.write(oid.encode() + b"\n")
        self.proc.stdin.flush()
        header = self.proc.stdout.readline().split(b" ")
        if len(header) < 3 or header[1] != b"blob":
            raise ValueError(f"cat-file did not return blob {oid}")
        return int(header[2])

    def _end(self, oid: str, left: int) -> None:
        if left or self.proc.stdout.read(1) != b"\n":
            raise ValueError(f"cat-file returned blob {oid} short")

    def _chunks(self, left: int):
        while left:
            chunk = self.proc.stdout.read(min(left, CHUNK))
            if not chunk:
                return
            left -= len(chunk)
            yield chunk

    def _count(self, oid: str) -> int:
        """The blob's dash count, read in chunks and never held whole: a NUL in
        its prefix makes it binary (0), a UTF-16 byte-order mark is decoded as
        it streams, anything else is counted as UTF-8 bytes."""
        size = self._header(oid)
        head = self.proc.stdout.read(min(size, BINARY_PREFIX))
        left = size - len(head)
        if len(head) < min(size, BINARY_PREFIX):
            raise ValueError(f"cat-file returned blob {oid} short")
        if head[:2] in UTF16_BOMS:
            decoder = codecs.getincrementaldecoder("utf-16")()
            try:
                n = _str_dashes(decoder.decode(head))
                for chunk in self._chunks(left):
                    left -= len(chunk)
                    n += _str_dashes(decoder.decode(chunk))
                n += _str_dashes(decoder.decode(b"", final=True))
            except UnicodeDecodeError as exc:
                raise ValueError(f"a UTF-16 file could not be decoded ({exc.reason})") from exc
        elif b"\0" in head:
            n = 0
            for chunk in self._chunks(left):
                left -= len(chunk)
        else:
            n, tail = dash_count(head), head[-2:]
            for chunk in self._chunks(left):
                left -= len(chunk)
                # A dash is 3 bytes: none fits in the carried 2 or in chunk[:2], so the 4 bytes across the
                # boundary hold only dashes that cross it, and the chunk itself is counted in place.
                n += dash_count(chunk) + dash_count(tail + chunk[:2])
                tail = (tail + chunk)[-2:] if len(chunk) < 2 else chunk[-2:]
        self._end(oid, left)
        return n

    def lines(self, oid: str) -> list:
        """dash_lines of the blob as text, read in chunks like the count: only
        the line being read is carried from one chunk to the next, so the blob
        is never held whole. A UTF-16 byte-order mark is decoded as it streams
        (a file that cannot be decoded is a read error, never clean), a UTF-8
        one is dropped, and a binary blob (a NUL in its prefix) has none."""
        if oid == NULL_OID:
            return []
        size = self._header(oid)
        head = self.proc.stdout.read(min(size, BINARY_PREFIX))
        left = size - len(head)
        if len(head) < min(size, BINARY_PREFIX):
            raise ValueError(f"cat-file returned blob {oid} short")
        keys = DashLineKeys()
        feed = keys.feed
        if head[:2] in UTF16_BOMS:
            decoder = codecs.getincrementaldecoder("utf-16")()
            try:
                feed(decoder.decode(head).encode("utf-8"))
                for chunk in self._chunks(left):
                    left -= len(chunk)
                    feed(decoder.decode(chunk).encode("utf-8"))
                feed(decoder.decode(b"", final=True).encode("utf-8"))
            except UnicodeDecodeError as exc:
                raise ValueError(f"a UTF-16 file could not be decoded ({exc.reason})") from exc
        elif b"\0" in head:
            for chunk in self._chunks(left):
                left -= len(chunk)
        else:
            feed(head[3:] if head.startswith(UTF8_BOM) else head)
            for chunk in self._chunks(left):
                left -= len(chunk)
                feed(chunk)
        self._end(oid, left)
        return keys.end()

    def abort(self) -> None:
        self.stream.abort()

    def close(self) -> int:
        return self.stream.close()


def name_lines(old_texts, new_text):
    """(exact, copies): dash lines of the new text that no parent holds, and
    every copy of an identical line that now appears more often than in any
    parent. A merge's line that one parent already had is never named new."""
    return _name(((dash_lines(t) if t else []) for t in old_texts), dash_lines(new_text))


def _name(old_line_lists, new_lines):
    """name_lines over dash lines already extracted, one list per parent."""
    old_seen = collections.Counter()
    for found in old_line_lists:
        for ln, k in collections.Counter(ln for _n, ln in found).items():
            old_seen[ln] = max(old_seen[ln], k)
    new_seen = collections.Counter(ln for _n, ln in new_lines)
    exact = [n for n, ln in new_lines if not old_seen[ln]]
    copies = [n for n, ln in new_lines if old_seen[ln] and new_seen[ln] > old_seen[ln]]
    return exact, copies


def refused(per_parent: list, reader, lines=None) -> list:
    """(path, gained, exact lines, candidate copies) for each file that holds
    more dashes than before. `per_parent` holds one change list per parent
    (one for a commit or the staged change, several for a merge); a file is
    refused only when it gains over every parent, so a merge is judged on
    what the merge itself added. Only a path changed against every parent is
    read, and a new blob with no dash is never compared. `lines` caches the
    named lines by (new blob, dash-bearing parent blobs), which alone decide
    them, so the same refused change at many paths is diagnosed once, and
    each blob's dash lines once, so a series of refused edits to one file
    reads each version once."""
    lines = {} if lines is None else lines

    def blob(oid):                                         # a blob's dash lines, whichever side it is on
        if ("blob", oid) not in lines:
            lines["blob", oid] = reader.lines(oid)
        return lines["blob", oid]

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
        gained = n
        for o in olds:                                         # a parent holding as many decides it: stop
            gained = min(gained, n - reader.count(o))
            if gained <= 0:
                break
        if gained > 0:
            parents = tuple(o for o in dict.fromkeys(olds) if reader.count(o))
            if (new, parents) not in lines:
                lines[new, parents] = _name(map(blob, parents), blob(new))
            exact, copies = lines[new, parents]
            found.append((path, gained, exact, copies))
    return found


# Every diff this checker reads finds renames the same way, whatever the caller's config: with no limit on
# inexact detection, a renamed and edited file is compared with its source, never with an empty file.
DIFF_CONFIG = ("-c", "diff.renameLimit=0", "-c", "diff.renames=true")


def _git_env() -> dict:
    """The environment of every git this checker starts. A push sends the stored objects along their stored
    parents, so neither a local replacement ref (GIT_NO_REPLACE_OBJECTS) nor a graft file (GIT_GRAFT_FILE,
    pointed at an empty file) may stand in for them: either would make the check judge other bytes, or other
    parents, than the ones the destination receives."""
    return {**os.environ, "GIT_NO_REPLACE_OBJECTS": "1", "GIT_GRAFT_FILE": os.devnull}


def _git(*args, stdin=None) -> bytes:
    """stdout of one git command, bounded as a process group (scripts/bounded_run.py); a non-zero exit is a
    CalledProcessError and a timeout a ValueError, both of which refuse the check."""
    rc, out, err = bounded_run.run(["git", *DIFF_CONFIG, *args], f"git {args[0]}", timeout=TIMEOUT,
                                   env=_git_env(), stdin=stdin, keep_stderr=True)
    if rc != 0:
        raise subprocess.CalledProcessError(rc, ["git", args[0]], out, err)
    return out


def judge(commits: list) -> list:
    """(label, refused files) for each (label, per-parent changes), one blob reader for all."""
    if not any(changes for _label, per_parent in commits for changes in per_parent):
        return []
    reader, lines = BlobReader(), {}
    try:
        out = [(label, hits) for label, per_parent in commits if (hits := refused(per_parent, reader, lines))]
    except BaseException:
        reader.abort()
        raise
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
    # --no-relative: from a subdirectory with diff.relative set, git would list only the paths below it.
    diff = ("diff", "--cached", "--raw", "-z", "-M", "--no-abbrev", "--no-ext-diff", "--no-relative")
    per_parent = [changes_from_raw(_git(*diff))]
    if not merging:
        return [("", per_parent)]                          # one parent: nothing to intersect
    common = {path for _old, _new, path in per_parent[0]}
    if not common:
        return [("", [[]])]                                # nothing changed against HEAD: MERGE_HEAD is not read
    for parent in merge_heads(merge_head):
        if not common:
            return [("", [[]])]                            # a refused path changes against every parent: none can now
        per_parent.append(changes_from_raw(_git(*diff, parent)))
        common &= {path for _old, _new, path in per_parent[-1]}
    return [("", per_parent if common or len(per_parent) == 1 else [[]])]


def _bounded(cmd: list, what: str) -> tuple:
    """(exit status, stdout) of `cmd`, bounded as one process group (scripts/bounded_run.py): a stalled
    transport (ssh, a credential helper) is a child of git and would outlive an end of git alone. An error
    names `what`, never the command: the URL in it can carry a token."""
    rc, out, _err = bounded_run.run(cmd, what, timeout=TIMEOUT, env=_git_env())
    return rc, out


NO_PROMPT = ("it cannot prompt on the terminal, so authenticate without a prompt (an ssh agent, a credential "
             "helper, or SSH_ASKPASS)")


def destination_oids(dest: str) -> set:
    """Every object id the push destination advertises (`git ls-remote`: its
    branches, tags, peeled tags and HEAD). `dest` is the URL git pushes to,
    which pre-push receives as its second argument, so a pushurl, a remote
    given by URL and a remote whose tracking refs are stale all answer for
    the repository actually written."""
    if dest.startswith("-"):
        raise ValueError("the push destination reads as an option")
    # pre-push hands over the URL after git's rewrites; ls-remote applies url.*.insteadOf again, and a second
    # matching rule would list another repository's refs. That is refused, never followed. git ends the URL it
    # prints with one LF; a local path can itself end in LF, so the comparison takes exactly that one.
    rc, again = _bounded(["git", "ls-remote", "--get-url", "--end-of-options", dest],
                         "the push URL check (git ls-remote --get-url)")
    if rc != 0 or again != os.fsencode(dest) + b"\n":
        raise ValueError("a url.*.insteadOf rule rewrites the push URL again, so the destination's refs cannot "
                         "be listed safely; make the rewrite rules not chain")
    # The lookup runs in its own process group with no terminal, so it cannot ask for a passphrase or a
    # password the way an attended push can (decided: the bounded lookup authenticates without a prompt). A
    # helper waiting for an answer that never comes ends at the deadline, so both failures say so.
    try:
        rc, out = _bounded(["git", "ls-remote", "--end-of-options", dest], "the destination (git ls-remote)")
    except ValueError as exc:
        raise ValueError(f"{exc}; {NO_PROMPT}") from None
    if rc != 0:
        raise ValueError(f"the destination's refs could not be listed (git ls-remote exit {rc}); {NO_PROMPT}")
    return {line.split("\t", 1)[0] for line in out.decode("ascii", "replace").splitlines() if line.strip()}


def _resolve(rev: str) -> str:
    """A --range endpoint as the object id it names; one this clone cannot resolve is a read error, never an
    empty range."""
    if rev == NULL_OID:
        return rev
    rc, out = _bounded(["git", "rev-parse", "--verify", "--quiet", "--end-of-options", rev + "^{object}"],
                       "git rev-parse")
    if rc != 0:
        raise ValueError(f"{rev!r} names no object in this repository")
    return out.decode("ascii", "replace").strip()


def _object_types(oids: list) -> dict:
    """oid -> type ("commit", "tag", ..., or "missing") in one batch-check."""
    if not oids:
        return {}
    out = _git("cat-file", "--batch-check=%(objectname) %(objecttype)", stdin="".join(o + "\n" for o in oids).encode())
    types = {}
    for line in out.decode("ascii", "replace").splitlines():
        oid, _sep, kind = line.partition(" ")
        types[oid] = "missing" if kind.endswith("missing") or not kind else kind
    return types


def outgoing(updates: list, dest: str = "") -> tuple:
    """(blocks, missing): (label, per-parent changes) for every commit the
    ref updates send, merges included, each commit once, and the number of
    destination commits this clone lacks.

    The commits sent are exactly those reachable from a pushed tip and from
    nothing the destination holds: one `rev-list --stdin` over every tip at
    once, stopped at every remote oid the push names (a deletion's
    included) and every oid the destination itself advertises. With no
    destination, a range from nothing is bounded by every remote's
    tracking refs and a range between two commits is exactly that range. A pushed tip this clone lacks is a read error, never clean. A
    destination commit this clone lacks cannot bound the walk; it is
    counted, so a refusal can say to fetch first."""
    tips = list(dict.fromkeys(new for _old, new in updates if new != NULL_OID))
    if not tips:
        return [], 0
    held = {old for old, _new in updates if old != NULL_OID}
    if dest:
        held |= destination_oids(dest)
    elif any(old == NULL_OID for old, _new in updates):
        # A manual range from nothing: what some remote already tracks stands in for a destination. A range
        # between two named commits is exactly that range.
        held |= set(_git("for-each-ref", "--format=%(objectname)", "refs/remotes").decode().split())
    types = _object_types(sorted(set(tips) | held))
    for tip in tips:
        if types.get(tip, "missing") == "missing":
            raise ValueError(f"the pushed object {tip} is not in this repository")
    bounds = [o for o in held if types.get(o) in ("commit", "tag")]
    missing = sum(1 for o in held if types.get(o, "missing") == "missing")
    if all(tip in held for tip in tips):
        return [], missing                                 # every tip is already there: nothing is sent
    feed = "".join(t + "\n" for t in tips) + "".join("^" + o + "\n" for o in bounds)
    walked = [line.split() for line in
              _git("rev-list", "--reverse", "--parents", "--stdin", stdin=feed.encode()).decode().splitlines()]
    # rev-list shows a shallow boundary as a root. Its stored parents decide: a boundary whose parents the
    # destination already has (advertised, or reachable from what it advertises) and this clone holds is judged
    # against them; any other means outgoing history this clone cannot judge.
    stored = _stored_parents([c for c, *ps in walked if not ps])
    unjudged = _unjudged_parents({p for ps in stored.values() for p in ps}, held, bounds)
    pairs = []                                                     # (commit, parent or None), oldest first
    for commit, *parents in walked:
        if not parents and stored.get(commit):
            if any(p in unjudged for p in stored[commit]):
                raise ValueError(f"the history is shallow at commit {commit[:12]}: commits before it are not in this "
                                 "clone, so they cannot be judged; deepen it (git fetch --deepen=N, or --unshallow) "
                                 "and push again")
            parents = stored[commit]
        if parents:
            pairs.extend((commit, p) for p in parents)
        else:
            pairs.append((commit, None))
    if not pairs:
        return [], missing
    feed = "".join(f"{c} {p}\n" if p else f"{c}\n" for c, p in pairs).encode()
    blocks = changes_by_header(_git("diff-tree", "--stdin", "--always", "-r", "-z", "-M", "--no-abbrev", "--root",
                                    stdin=feed))
    if len(blocks) != len(pairs) or any(h != c for (h, _ch), (c, _p) in zip(blocks, pairs)):
        raise ValueError("diff-tree returned a different set of commits than it was given")
    commits = {}
    for (commit, _p), (_h, changes) in zip(pairs, blocks):
        commits.setdefault(commit, []).append(changes)
    return [(f"commit {c[:12]}: ", per_parent) for c, per_parent in commits.items()], missing


def _unjudged_parents(parents: set, held: set, bounds: list) -> set:
    """The parents of shallow boundaries that cannot stand as a comparison: missing from this clone, or not
    held by the destination (neither advertised nor reachable from a commit it advertises)."""
    if not parents:
        return set()
    types = _object_types(sorted(parents))
    absent = {p for p in parents if types.get(p) != "commit"}
    loose = sorted(p for p in parents - absent if p not in held)
    if not loose or not bounds:
        return absent | set(loose)
    feed = "".join(p + "\n" for p in loose) + "--not\n" + "".join(b + "\n" for b in bounds)
    beyond = set(_git("rev-list", "--stdin", stdin=feed.encode()).decode().split())
    return absent | {p for p in loose if p in beyond}


def _stored_parents(commits: list) -> dict:
    """{commit: [parent, ...]} as each commit object stores them, for the few commits rev-list showed as
    roots: a shallow boundary is shown parentless though its object names its parents."""
    if not commits:
        return {}
    out = _git("cat-file", "--batch", stdin="".join(c + "\n" for c in commits).encode())
    found, pos = {}, 0
    for c in commits:
        nl = out.index(b"\n", pos)
        header = out[pos:nl].split()
        if len(header) != 3 or header[1] != b"commit":
            raise ValueError(f"commit {c} could not be read")
        size = int(header[2])
        head = out[nl + 1:nl + 1 + size].split(b"\n\n", 1)[0]
        found[c] = [ln[7:].decode() for ln in head.split(b"\n") if ln.startswith(b"parent ")]
        pos = nl + 1 + size + 1
    return found


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
        missing = 0
        if argv[:1] == ["--range"] and len(argv) in (3, 4):
            old, new = _resolve(argv[1]), _resolve(argv[2])
            # Two named commits are exactly that range; only a range from nothing asks the URL what it holds.
            blocks, missing = outgoing([(old, new)], argv[3] if old == NULL_OID and argv[3:] else "")
            found = judge(blocks)
        elif argv[:1] == ["--push"] and len(argv) == 2:
            blocks, missing = outgoing(push_updates(sys.stdin.read()), argv[1])
            found = judge(blocks)
        elif not argv:
            found = judge(staged())
        elif argv == ["--no-merge"]:
            found = judge(staged(merging=False))
        elif argv[:1] == ["--merge-head"] and len(argv) == 2:
            found = judge(staged(merge_head=argv[1]))
        else:
            print("usage: staged_dash_check.py [--no-merge | --merge-head FILE | --range OLD NEW [URL] | --push URL]", file=sys.stderr)
            return 2
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        print(f"[dash-check] the changes could not be read: {exc}", file=sys.stderr)
        return 2
    report(found)
    if found and missing:
        print(f"[dash-check] {missing} commit(s) the destination holds are not in this clone, so they could not "
              "bound the walk; if a named commit is already there, fetch the destination's refs from the push "
              "URL itself (git fetch <push-url> 'refs/*:refs/dash-check/*') and push again", file=sys.stderr)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
