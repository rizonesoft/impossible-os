#!/usr/bin/env python3
"""Refuse a push whose new commits carry a test fixture's identity.

A fixture's `git config user.name` once landed in the real .git/config because the
suite inherited GIT_DIR from a hook, and 674 commits went to main under that name
before anyone read the author field: every other gate looks at content, not author.

Usage: check-push-identity.py LOCAL_OID REMOTE_OID. Exit 1 refuses. Commits already on
the remote are never judged, so the mapped history (see .mailmap) cannot block a push.
A git failure is exit 0 with a note: this gate must not stop a push it cannot read.
"""
import re
import subprocess
import sys

ZERO = re.compile(r"^0+$")
BAD_NAME = re.compile(r"\bfixture\b", re.I)
BAD_EMAIL = re.compile(r"(@|\.)(example\.(invalid|com|org|net)|test\.invalid|invalid|test|localhost)$", re.I)


def bad(name: str, email: str) -> bool:
    return bool(BAD_NAME.search(name) or BAD_EMAIL.search(email))


def rev_args(local: str, remote: str) -> list:
    if ZERO.match(remote):
        return [local, "--not", "--remotes"]
    return [f"{remote}..{local}"]


def main(argv) -> int:
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    local, remote = argv[1], argv[2]
    if ZERO.match(local):
        return 0                       # a delete pushes no commits
    r = subprocess.run(["git", "log", "--format=%h%x00%an%x00%ae%x00%cn%x00%ce", *rev_args(local, remote)],
                       capture_output=True, text=True, timeout=60)
    if r.returncode:
        print(f"check-push-identity: could not read the range: {r.stderr.strip()[:200]}")
        return 0
    hits = []
    for line in r.stdout.splitlines():
        sha, an, ae, cn, ce = line.split("\0")
        if bad(an, ae) or bad(cn, ce):
            hits.append(f"{sha} author {an} <{ae}>, committer {cn} <{ce}>")
    if not hits:
        return 0
    print(f"[pre-push] REFUSED: {len(hits)} commit(s) carry a test-fixture identity:")
    for h in hits[:5]:
        print("  " + h)
    print("  Check `git config --show-origin --get-all user.name`, remove the bad [user]\n"
          "  block from .git/config, and re-author these commits before pushing.\n"
          "  Opt-out: SKIP_PUSH_IDENTITY_CHECK=1")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
