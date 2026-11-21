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
import subprocess
import sys
import threading

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


def _cmd(top, args):
    return ["git", "--no-optional-locks", "-C", top, *args]


def _git(top, *args):
    try:
        done = subprocess.run(_cmd(top, args), capture_output=True, timeout=TIMEOUT, env=_env())
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise Refused(f"git {args[0]} could not run: {exc}") from None
    if done.returncode != 0:
        raise Refused(f"git {args[0]} exited {done.returncode}: {done.stderr.decode(errors='replace').strip()}")
    return done.stdout


def _git_into(top, h, *args):
    """Stream a git command's output into the hash `h`, never holding it whole; a timer ends it at the
    deadline, which is then an error."""
    try:
        proc = subprocess.Popen(_cmd(top, args), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                stdin=subprocess.DEVNULL, env=_env())   # deadline: the timer below
    except OSError as exc:
        raise Refused(f"git {args[0]} could not run: {exc}") from None
    timer = threading.Timer(TIMEOUT, proc.kill)
    timer.start()
    err = []
    drain = threading.Thread(target=lambda: err.append(proc.stderr.read()), daemon=True)
    drain.start()
    try:
        while chunk := proc.stdout.read(CHUNK):
            h.update(chunk)
        rc = proc.wait()
        drain.join()
    finally:
        timer.cancel()
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    if rc != 0:
        raise Refused(f"git {args[0]} exited {rc}: {b''.join(err).decode(errors='replace').strip()}")


def _top():
    try:
        done = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True,
                              timeout=TIMEOUT, env=_env())
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise Refused(f"the work tree could not be found: {exc}") from None
    if done.returncode != 0 or not done.stdout.strip():
        raise Refused("not inside a git work tree")
    return os.path.realpath(done.stdout.rstrip("\n"))


def _scope(top, exclude):
    """The pathspec for the whole tree, less the reviews directory (matched literally) when it sits inside."""
    spec = [":(top)"]
    if exclude:
        rel = os.path.relpath(os.path.realpath(exclude), top)
        if rel != "." and rel != ".." and not rel.startswith(".." + os.sep):
            spec.append(f":(top,exclude,literal){rel}")
    return ["--", *spec]


def _status(top, scope):
    """(raw status bytes, untracked paths as bytes)."""
    raw = _git(top, "status", "--porcelain=v1", "-z", "--untracked-files=all", "--no-renames", *scope)
    return raw, [rec[3:] for rec in raw.split(b"\0") if rec[:3] == b"?? "]


def _entry(top, rel):
    """(record, note) for one untracked entry, read without following a link and without opening anything but
    a regular file. A regular file is read to the size it had when opened and refused if it changed meanwhile,
    so a file still being written can neither stall the read nor be half-read. `note` describes a non-file for
    the reviewer, or is None."""
    path = os.path.join(os.fsencode(top), rel)
    try:
        st = os.lstat(path)
        if stat.S_ISLNK(st.st_mode):
            target = os.readlink(path)
            return b"L\0" + target, f"a symbolic link to {shown(target)}: review the link text, not what it points at"
        if not stat.S_ISREG(st.st_mode):
            kind = KINDS.get(stat.S_IFMT(st.st_mode), "special file")
            return b"T\0" + str(stat.S_IFMT(st.st_mode)).encode(), f"a {kind}, not a file: nothing to read"
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
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
    """What the tracked changes are taken against: HEAD, or the empty tree while HEAD names a branch with no
    commit yet (a new repository). A HEAD that names nothing readable refuses."""
    for args, answer in ((("rev-parse", "-q", "--verify", "HEAD^{commit}"), "HEAD"),
                         (("symbolic-ref", "-q", "HEAD"), EMPTY_TREE)):
        try:
            done = subprocess.run(_cmd(top, args), capture_output=True, timeout=TIMEOUT, env=_env())
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise Refused(f"HEAD could not be read: {exc}") from None
        if done.returncode == 0:
            return answer
    raise Refused("HEAD names no commit and no branch")


def _fingerprint(top, scope, status):
    """(digest, [(path, note)] for the untracked entries)."""
    h = hashlib.sha256()
    _git_into(top, h, "diff", _head(top), "--binary", "--no-ext-diff", "--no-textconv", "--no-renames", *scope)
    raw, untracked = status
    h.update(raw)
    notes = []
    for rel in untracked:
        record, note = _entry(top, rel)
        h.update(rel + b"\0" + record + b"\0")
        notes.append((rel, note))
    return h.hexdigest(), notes


def snapshot(exclude=""):
    top = _top()
    scope = _scope(top, exclude)
    return _fingerprint(top, scope, _status(top, scope))[0]


def prepare(base, head, exclude=""):
    """(digest, sorted covered paths, [(untracked path, note)]) from one status walk."""
    top = _top()
    scope = _scope(top, exclude)
    status = _status(top, scope)
    digest, notes = _fingerprint(top, scope, status)
    found = {rel for rel, _note in notes}
    for args in (("diff", "--name-only", "-z", "--no-renames", base, head),
                 ("diff", "--name-only", "-z", "--no-renames", head)):
        found.update(p for p in _git(top, *args, *scope).split(b"\0") if p)
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
