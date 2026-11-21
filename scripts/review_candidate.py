#!/usr/bin/env python3
"""The candidate a review pass reads: one fingerprint of the working tree against HEAD, and the files the pass
covers. Each is read whole or refused (exit 2), never returned partly read.

    python3 scripts/review_candidate.py snapshot [--exclude DIR]
    python3 scripts/review_candidate.py paths --base OID --head OID [--exclude DIR] --out FILE [--untracked-out FILE]

`snapshot` prints a sha256 over the tracked changes (a binary patch against HEAD), every status record, and each
untracked entry read without following links: a symlink by its target text, a regular file by its mode and
bytes, anything else by its type alone, never opened (a FIFO would block the read). A clean tree hashes nothing,
so its digest is the sha256 of empty input, which review_pin.py checks a reconciliation start against.

`paths` writes the files the pass covers to FILE, NUL-separated: the section's change BASE..HEAD and the
working tree's against HEAD, both sides of every rename, and every untracked file. --untracked-out lists the
untracked files one per line for the reviewer's prompt, since neither diff shows them.

DIR (the dispatcher's own reviews directory) is left out of both when it sits inside the tree: its transcript
appears before the reviewer starts and grows while it runs. Pathspec settings from the environment
(GIT_LITERAL_PATHSPECS and its siblings) are dropped, so the exclusion and every path read the same everywhere.
"""
import argparse
import hashlib
import json
import os
import stat
import subprocess
import sys

TIMEOUT = 120
CHUNK = 1 << 20
PATHSPEC_ENV = ("GIT_LITERAL_PATHSPECS", "GIT_GLOB_PATHSPECS", "GIT_NOGLOB_PATHSPECS", "GIT_ICASE_PATHSPECS")


class Refused(Exception):
    pass


def _env():
    return {k: v for k, v in os.environ.items() if k not in PATHSPEC_ENV}


def _git(top, *args):
    try:
        done = subprocess.run(["git", "--no-optional-locks", "-C", top, *args], capture_output=True,
                              timeout=TIMEOUT, env=_env())
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise Refused(f"git {args[0]} could not run: {exc}") from None
    if done.returncode != 0:
        raise Refused(f"git {args[0]} exited {done.returncode}: {done.stderr.decode(errors='replace').strip()}")
    return done.stdout


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
    """The pathspec for the whole tree, less the reviews directory when it sits inside it."""
    spec = [":(top)"]
    if exclude:
        rdir = os.path.realpath(exclude)
        rel = os.path.relpath(rdir, top)
        if rel != "." and not rel.startswith(".." + os.sep) and rel != "..":
            spec.append(f":(top,exclude){rel}")
    return ["--", *spec]


def _status(top, scope):
    """(raw status bytes, untracked paths as bytes)."""
    raw = _git(top, "status", "--porcelain=v1", "-z", "--untracked-files=all", "--no-renames", *scope)
    untracked = [rec[3:] for rec in raw.split(b"\0") if rec[:3] == b"?? "]
    return raw, untracked


def _entry(top, rel):
    """One untracked entry's record, read without following a link and without opening anything but a
    regular file."""
    path = os.path.join(os.fsencode(top), rel)
    try:
        st = os.lstat(path)
        if stat.S_ISLNK(st.st_mode):
            return b"L\0" + os.readlink(path)
        if not stat.S_ISREG(st.st_mode):
            return b"T\0" + str(stat.S_IFMT(st.st_mode)).encode()
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
        try:
            opened = os.fstat(fd)
            if not stat.S_ISREG(opened.st_mode) or (opened.st_dev, opened.st_ino) != (st.st_dev, st.st_ino):
                raise Refused(f"{os.fsdecode(rel)} was replaced while it was read")
            h = hashlib.sha256()
            while chunk := os.read(fd, CHUNK):
                h.update(chunk)
        finally:
            os.close(fd)
        return b"F\0" + (b"x" if opened.st_mode & 0o111 else b"-") + b"\0" + h.hexdigest().encode()
    except OSError as exc:
        raise Refused(f"the untracked entry {os.fsdecode(rel)} could not be read: {exc.strerror}") from None


def snapshot(exclude=""):
    top = _top()
    scope = _scope(top, exclude)
    h = hashlib.sha256()
    h.update(_git(top, "diff", "HEAD", "--binary", "--no-ext-diff", "--no-textconv", "--no-renames", *scope))
    raw, untracked = _status(top, scope)
    h.update(raw)
    for rel in untracked:
        h.update(rel + b"\0" + _entry(top, rel) + b"\0")
    return h.hexdigest()


def paths(base, head, exclude=""):
    """(sorted covered paths, untracked paths), both as bytes."""
    top = _top()
    scope = _scope(top, exclude)
    found = set()
    for args in (("diff", "--name-only", "-z", "--no-renames", base, head),
                 ("diff", "--name-only", "-z", "--no-renames", head)):
        found.update(p for p in _git(top, *args, *scope).split(b"\0") if p)
    _raw, untracked = _status(top, scope)
    found.update(untracked)
    return sorted(found), sorted(untracked)


def shown(path: bytes) -> str:
    """A path for a person to read: as it is when plain, JSON-quoted when it holds a byte a line would hide."""
    text = path.decode("utf-8", "surrogateescape")
    return text if text.isprintable() and text.strip() == text and '"' not in text else json.dumps(text)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("snapshot")
    s.add_argument("--exclude", default="")
    p = sub.add_parser("paths")
    p.add_argument("--base", required=True)
    p.add_argument("--head", required=True)
    p.add_argument("--exclude", default="")
    p.add_argument("--out", required=True)
    p.add_argument("--untracked-out")
    args = parser.parse_args(argv)
    try:
        if args.cmd == "snapshot":
            print(snapshot(args.exclude))
            return 0
        covered, untracked = paths(args.base, args.head, args.exclude)
        with open(args.out, "wb") as f:
            f.write(b"".join(c + b"\0" for c in covered))
        if args.untracked_out:
            with open(args.untracked_out, "w", encoding="utf-8") as f:
                f.write("".join(shown(u) + "\n" for u in untracked))
        return 0
    except (Refused, OSError) as exc:
        print(f"review_candidate: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
