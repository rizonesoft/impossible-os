#!/usr/bin/env python3
"""Refuse an image, sound, font or cursor file copied from the Windows 11 reference VM.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 4.

The repository holds facts about the Windows 11 interface, never Microsoft's own
files. scripts/inventory/ms-hashes.txt lists the SHA-256 and size of every file
of the types in asset-types.txt on the reference VM (made by
make-ms-hashes.ps1, no bytes kept). A file here whose hash and size are on that
list is a copy, and is refused.

    python3 scripts/inventory/check-no-ms-assets.py --all     # tracked files, as on disk
    python3 scripts/inventory/check-no-ms-assets.py --index   # the staged contents and staged lists (commit gate)
    python3 scripts/inventory/check-no-ms-assets.py FILE...   # named files

Exit 0 when nothing matches, 1 when a file matches, 2 when the hash list or git
cannot be read: a check that cannot run never passes.
"""
import argparse
import hashlib
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
LINE = re.compile(r"^([0-9a-f]{64}) ([0-9]+)$")
GIT_TIMEOUT = 120  # deadline: the whole index of this repo is read in well under a second


class Unreadable(Exception):
    pass


def policy_text(path, repo, staged):
    """The text of a policy file: its staged blob under --index when it lives in the repository.

    A commit is judged by the policy it records too, so an unstaged edit to the
    hash list or the type list can neither clear nor fail a staged asset.
    """
    path = path.resolve()
    if staged:
        try:
            rel = path.relative_to(repo.resolve()).as_posix()
        except ValueError:
            rel = None
        if rel is not None:
            return git(repo, "cat-file", "blob", f":{rel}").decode("utf-8")
    return path.read_text(encoding="utf-8")


def asset_types(text, path):
    types = set()
    for line in text.splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            types.add(line.lower())
    if not types:
        raise Unreadable(f"{path} names no file types")
    return types


def load_hashes(text, path):
    known = set()
    for n, line in enumerate(text.splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        m = LINE.match(line)
        if not m:
            raise Unreadable(f"{path}:{n}: not '<sha256> <size>': {line[:80]!r}")
        known.add((m.group(1), int(m.group(2))))
    if not known:
        raise Unreadable(f"{path} holds no hashes")
    return known


def is_asset(name, types):
    suffix = pathlib.PurePosixPath(name).suffix
    return suffix[1:].lower() in types if suffix else False


def git(repo, *args, data=None):
    r = subprocess.run(["git", "-C", str(repo), *args], input=data, capture_output=True, timeout=GIT_TIMEOUT)
    if r.returncode != 0:
        raise Unreadable(f"git {' '.join(args)}: {r.stderr.decode(errors='replace').strip()}")
    return r.stdout


def from_index(repo, types):
    """Yield (path, bytes) for every staged regular file of an asset type."""
    entries = []
    for rec in git(repo, "ls-files", "-s", "-z").split(b"\0"):
        if not rec:
            continue
        meta, path = rec.split(b"\t", 1)
        mode, blob, _stage = meta.split(b" ")
        name = path.decode("utf-8", errors="surrogateescape")
        if mode in (b"100644", b"100755") and is_asset(name, types):
            entries.append((name, blob.decode()))
    if not entries:
        return
    out = git(repo, "cat-file", "--batch", data="".join(b + "\n" for _, b in entries).encode())
    pos = 0
    for name, blob in entries:
        end = out.index(b"\n", pos)
        header = out[pos:end].split(b" ")
        if len(header) != 3 or header[1] != b"blob":
            raise Unreadable(f"git cat-file: {name}: {out[pos:end].decode(errors='replace')}")
        size = int(header[2])
        yield name, out[end + 1:end + 1 + size]
        pos = end + 1 + size + 1


def from_tree(repo, types):
    """Yield (path, bytes) for every tracked file of an asset type, as on disk."""
    for raw in git(repo, "ls-files", "-z").split(b"\0"):
        name = raw.decode("utf-8", errors="surrogateescape")
        if name and is_asset(name, types):
            path = repo / name
            if path.is_file() and not path.is_symlink():
                yield name, path.read_bytes()


def from_paths(paths, types):
    for name in paths:
        path = pathlib.Path(name)
        if path.is_file() and is_asset(name, types):
            yield name, path.read_bytes()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--all", action="store_true", help="every tracked file, as on disk")
    group.add_argument("--index", action="store_true", help="every staged file, as staged")
    group.add_argument("paths", nargs="*", default=[], help="files to check")
    ap.add_argument("--hashes", default=str(HERE / "ms-hashes.txt"), help=argparse.SUPPRESS)
    ap.add_argument("--types", default=str(HERE / "asset-types.txt"), help=argparse.SUPPRESS)
    ap.add_argument("--repo", default=str(REPO), help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    repo = pathlib.Path(args.repo)
    try:
        types_path, hashes_path = pathlib.Path(args.types), pathlib.Path(args.hashes)
        types = asset_types(policy_text(types_path, repo, args.index), types_path)
        known = load_hashes(policy_text(hashes_path, repo, args.index), hashes_path)
        if args.index:
            files = from_index(repo, types)
        elif args.all:
            files = from_tree(repo, types)
        else:
            files = from_paths(args.paths, types)
        checked = 0
        copies = []
        for name, data in files:
            checked += 1
            digest = hashlib.sha256(data).hexdigest()
            if (digest, len(data)) in known:
                copies.append((name, digest, len(data)))
    except (OSError, UnicodeDecodeError, Unreadable, subprocess.TimeoutExpired) as exc:
        print(f"check-no-ms-assets: {exc}", file=sys.stderr)
        return 2
    for name, digest, size in copies:
        print(f"{name}: a file of the Windows 11 reference VM ({digest[:16]}, {size} bytes); "
              "Microsoft's files are never committed, draw our own")
    print(f"ms-assets: {checked} file(s) checked, {len(copies)} copied from the reference VM")
    return 1 if copies else 0


if __name__ == "__main__":
    sys.exit(main())
