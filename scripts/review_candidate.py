#!/usr/bin/env python3
"""The candidate a review pass reads: one fingerprint of the working tree against HEAD, and the files the pass
covers. Each is read whole or refused (exit 2), never returned partly read.

    python3 scripts/review_candidate.py snapshot [--exclude DIR]
    python3 scripts/review_candidate.py prepare --base OID --head OID [--exclude DIR] --out FILE --untracked-out FILE

`snapshot` prints a sha256 over the tracked changes (a binary patch against HEAD, or the empty tree in a
repository with no commit yet), every status record, and each
untracked entry read without following links: a symlink by its target text, a regular file by its mode and the
bytes it held when the read began, anything else by its type alone, never opened (a FIFO would block the read).
A clean tree hashes nothing, so its digest is the sha256 of empty input, which review_pin.py checks a
reconciliation start against.

`prepare` is the dispatcher's one read before a pass: it prints the same fingerprint and, from the same status
walk, writes the files the pass covers to FILE, NUL-separated (the section's change BASE..HEAD and the working
tree's against HEAD, both sides of every rename, every untracked entry), and lists the untracked entries for the
reviewer's prompt, one per line, a link shown with its target and a non-file with its type, since neither diff
shows them.

DIR (the dispatcher's own reviews directory) is left out of every read when it sits inside the tree, matched
literally: its transcript appears before the reviewer starts and grows while it runs. Pathspec settings from the
environment (GIT_LITERAL_PATHSPECS and its siblings) are dropped, so the exclusion and every path read the same
everywhere.
"""
import argparse
import hashlib
import json
import os
import stat
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bounded_run                                         # the shared bounded runner beside this script

TIMEOUT = 120
CHUNK = 1 << 20
EMPTY_TREE = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"
PATHSPEC_ENV = ("GIT_LITERAL_PATHSPECS", "GIT_GLOB_PATHSPECS", "GIT_NOGLOB_PATHSPECS", "GIT_ICASE_PATHSPECS")
KINDS = {stat.S_IFIFO: "named pipe", stat.S_IFSOCK: "socket", stat.S_IFCHR: "character device",
         stat.S_IFBLK: "block device", stat.S_IFDIR: "directory"}


class Refused(Exception):
    """The candidate could not be read whole; the dispatcher refuses the pass."""


def _env():
    return {k: v for k, v in os.environ.items() if k not in PATHSPEC_ENV}


def _run(top, args, sink=None):
    """(exit status, stdout, stderr) of one git command, bounded as a process group (scripts/bounded_run.py):
    a helper git starts (a clean filter, a monitor hook) holding its pipes past the deadline is ended with it."""
    cmd = ["git", "--no-optional-locks", *(["-C", top] if top else []), *args]
    try:
        return bounded_run.run(cmd, f"git {args[0]}", timeout=TIMEOUT, env=_env(), sink=sink, keep_stderr=True)
    except (OSError, ValueError) as exc:
        raise Refused(f"git {args[0]} could not run: {exc}") from None


def _git(top, *args, sink=None):
    rc, out, err = _run(top, args, sink)
    if rc != 0:
        raise Refused(f"git {args[0]} exited {rc}: {err.decode(errors='replace').strip()}")
    return out


def _top():
    rc, out, _err = _run("", ("rev-parse", "--show-toplevel"))
    # git ends the path with one LF; a directory name can itself end in LF, so exactly that one is taken off.
    if rc != 0 or not out.endswith(b"\n") or len(out) < 2:
        raise Refused("not inside a git work tree")
    return os.path.realpath(os.fsdecode(out[:-1]))


def _scope(top, exclude):
    """The pathspec for the whole tree, less the reviews directory (matched literally) when it sits inside."""
    spec = [":(top)"]
    if exclude:
        rel = os.path.relpath(os.path.realpath(exclude), top)
        if rel != "." and rel != ".." and not rel.startswith(".." + os.sep):
            spec.append(f":(top,exclude,literal){rel}")
    return ["--", *spec]


def _status(top, scope):
    """(raw status bytes, untracked paths as bytes, whether any tracked path changed)."""
    raw = _git(top, "status", "--porcelain=v1", "-z", "--untracked-files=all", "--no-renames", *scope)
    records = [rec for rec in raw.split(b"\0") if rec]
    untracked = [rec[3:] for rec in records if rec[:3] == b"?? "]
    return raw, untracked, len(untracked) < len(records)


def _parent(top_fd, rel):
    """(descriptor of the directory holding `rel`, its last name), reached one component at a time from the
    checkout's own descriptor and never through a link: a directory replaced by a link after the status walk
    refuses instead of leading the read outside the checkout."""
    *dirs, name = rel.split(b"/")
    fd = os.dup(top_fd)
    try:
        for part in dirs:
            step = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=fd)
            os.close(fd)
            fd = step
    except BaseException:
        os.close(fd)
        raise
    return fd, name


def _entry(top_fd, rel):
    """(record, note) for one untracked entry, read without following a link (the entry's own, or any
    directory above it) and without opening anything but a regular file. A regular file is read to the size it
    had when opened and refused if it changed meanwhile, so a file still being written can neither stall the
    read nor be half-read. `note` describes a non-file for the reviewer, or is None."""
    try:
        pfd, name = _parent(top_fd, rel)
    except OSError as exc:
        raise Refused(f"the untracked entry {os.fsdecode(rel)} could not be reached: {exc.strerror}") from None
    try:
        return _entry_in(pfd, name, rel)
    finally:
        os.close(pfd)


def _entry_in(pfd, name, rel):
    try:
        st = os.stat(name, dir_fd=pfd, follow_symlinks=False)
        if stat.S_ISLNK(st.st_mode):
            target = os.readlink(name, dir_fd=pfd)
            return b"L\0" + target, f"a symbolic link to {shown(target)}: review the link text, not what it points at"
        if not stat.S_ISREG(st.st_mode):
            kind = KINDS.get(stat.S_IFMT(st.st_mode), "special file")
            return b"T\0" + str(stat.S_IFMT(st.st_mode)).encode(), f"a {kind}, not a file: nothing to read"
        fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC, dir_fd=pfd)
        try:
            opened = os.fstat(fd)
            if not stat.S_ISREG(opened.st_mode) or (opened.st_dev, opened.st_ino) != (st.st_dev, st.st_ino):
                raise Refused(f"{os.fsdecode(rel)} was replaced while it was read")
            h, left = hashlib.sha256(), opened.st_size
            while left:
                chunk = os.read(fd, min(left, CHUNK))
                if not chunk:
                    break
                h.update(chunk)
                left -= len(chunk)
            after = os.fstat(fd)
        finally:
            os.close(fd)
        if left or (after.st_size, after.st_mtime_ns) != (opened.st_size, opened.st_mtime_ns):
            raise Refused(f"{os.fsdecode(rel)} changed while it was read")
        return b"F\0" + (b"x" if opened.st_mode & 0o111 else b"-") + b"\0" + h.hexdigest().encode(), None
    except OSError as exc:
        raise Refused(f"the untracked entry {os.fsdecode(rel)} could not be read: {exc.strerror}") from None


def _head(top):
    """The commit the tracked changes are taken against: HEAD's object id, or the empty tree while HEAD names
    a branch with no commit yet (a new repository). A HEAD that names nothing readable refuses."""
    rc, out, _err = _run(top, ("rev-parse", "-q", "--verify", "HEAD^{commit}"))
    if rc == 0:
        return out.decode("ascii", "replace").strip()
    rc, _out, _err = _run(top, ("symbolic-ref", "-q", "HEAD"))
    if rc == 0:
        return EMPTY_TREE
    raise Refused("HEAD names no commit and no branch")


def _fingerprint(top, scope, status):
    """(digest, HEAD's object id, [(path, note)] for the untracked entries). A status with no tracked change
    has no patch to take."""
    h = hashlib.sha256()
    head = _head(top)
    raw, untracked, tracked = status
    if tracked:
        _git(top, "diff", head, "--binary", "--no-ext-diff", "--no-textconv", "--no-renames", *scope,
             sink=h.update)
    h.update(raw)
    notes = []
    if untracked:
        top_fd = os.open(top, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        try:
            for rel in untracked:
                record, note = _entry(top_fd, rel)
                h.update(rel + b"\0" + record + b"\0")
                notes.append((rel, note))
        finally:
            os.close(top_fd)
    return h.hexdigest(), head, notes


def snapshot(exclude=""):
    top = _top()
    scope = _scope(top, exclude)
    return _fingerprint(top, scope, _status(top, scope))[0]


def launch(exclude=""):
    """(HEAD, index tree, digest) of one candidate, for a launch row: HEAD and the index are read before and
    after the digest, and a commit or an index change meanwhile refuses, so the three name one candidate.
    HEAD is "unknown" before the first commit, and the index "unknown" while it holds a conflict."""
    top = _top()
    scope = _scope(top, exclude)

    def index():
        rc, out, _err = _run(top, ("write-tree",))
        return out.decode("ascii", "replace").strip() if rc == 0 else "unknown"
    before = index()
    digest, head, _notes = _fingerprint(top, scope, _status(top, scope))
    if index() != before or _head(top) != head:
        raise Refused("the candidate changed while it was read (a commit, or a change to the index); run again")
    return ("unknown" if head == EMPTY_TREE else head), before, digest


def prepare(base, head, exclude=""):
    """(digest, sorted covered paths, [(untracked path, note)]) from one status walk. The working tree's own
    changes are listed against `head` (the candidate); when that is HEAD and the status is clean there are
    none to list."""
    top = _top()
    scope = _scope(top, exclude)
    status = _status(top, scope)
    digest, current, notes = _fingerprint(top, scope, status)
    found = {rel for rel, _note in notes}
    found.update(p for p in _git(top, "diff", "--name-only", "-z", "--no-renames", base, head, *scope).split(b"\0")
                 if p)
    if status[2] or head != current:
        found.update(p for p in _git(top, "diff", "--name-only", "-z", "--no-renames", head, *scope).split(b"\0")
                     if p)
    return digest, sorted(found), sorted(notes)


def shown(path: bytes) -> str:
    """A path for a person to read: as it is when plain, JSON-quoted when it holds a byte a line would hide."""
    text = path.decode("utf-8", "surrogateescape")
    return text if text.isprintable() and text.strip() == text and '"' not in text else json.dumps(text)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("snapshot")
    s.add_argument("--exclude", default="")
    s = sub.add_parser("launch")
    s.add_argument("--exclude", default="")
    p = sub.add_parser("prepare")
    p.add_argument("--base", required=True)
    p.add_argument("--head", required=True)
    p.add_argument("--exclude", default="")
    p.add_argument("--out", required=True)
    p.add_argument("--untracked-out", required=True)
    args = parser.parse_args(argv)
    try:
        if args.cmd == "snapshot":
            print(snapshot(args.exclude))
            return 0
        if args.cmd == "launch":
            print(*launch(args.exclude))
            return 0
        digest, covered, notes = prepare(args.base, args.head, args.exclude)
        with open(args.out, "wb") as f:
            f.write(b"".join(c + b"\0" for c in covered))
        with open(args.untracked_out, "w", encoding="utf-8") as f:
            f.write("".join(shown(rel) + (f" ({note})" if note else "") + "\n" for rel, note in notes))
        print(digest)
        return 0
    except (Refused, OSError) as exc:
        print(f"review_candidate: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
