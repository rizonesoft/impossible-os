#!/usr/bin/env python3
"""Delete every published weekly demo except the newest KEEP, with its tag.

    demo_prune.py --repo OWNER/REPO [--dry-run]

Only a release whose tag is exactly demo-YY.M.D is ever deleted: a version
(v*), a draft and any other release are left alone. Age is the date in the tag.
release.yml runs it after a demo is published, so a week without a demo
deletes nothing.
"""
from __future__ import annotations

import argparse
import datetime
import json
import re
import subprocess
import sys

KEEP = 5
DEMO_RE = re.compile(r"demo-([1-9][0-9])\.([1-9][0-9]?)\.([1-9][0-9]?)")   # what release.yml writes: no zero padding
GH_TIMEOUT = 120


def demo_date(tag: str) -> datetime.date | None:
    m = DEMO_RE.fullmatch(tag)
    try:
        return datetime.date(2000 + int(m[1]), int(m[2]), int(m[3])) if m else None
    except ValueError:   # not a real date, so not a demo release.yml published
        return None


def to_delete(releases: list[dict], keep: int = KEEP) -> list[str]:
    demos = []
    for r in releases:
        date = demo_date(r["tag_name"])
        if date and r["prerelease"] and not r["draft"]:
            demos.append((date, r["tag_name"]))
    return [tag for _, tag in sorted(demos, reverse=True)[keep:]]


def gh(*args: str) -> str:
    return subprocess.run(["gh", *args], check=True, capture_output=True, text=True, timeout=GH_TIMEOUT).stdout


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    # One object per line: the CI image's gh 2.46 has no `--slurp` (2.48).
    out = gh("api", "--paginate", "--jq", ".[] | {tag_name, prerelease, draft}", f"repos/{a.repo}/releases?per_page=100")
    old = to_delete([json.loads(line) for line in out.splitlines() if line])
    for tag in old:
        print(f"{'would delete' if a.dry_run else 'deleting'} {tag}")
        if not a.dry_run:
            gh("release", "delete", tag, "--repo", a.repo, "--yes", "--cleanup-tag")
    print(f"demo prune: {len(old)} deleted, newest {KEEP} kept")
    return 0


if __name__ == "__main__":
    sys.exit(main())
