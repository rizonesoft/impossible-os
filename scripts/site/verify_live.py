#!/usr/bin/env python3
"""Verify the DEPLOYED site is byte-identical to a fresh build of the tree.

scripts/site/build.py --check proves the SOURCES agree with project.json and the
docs; nothing proved the site people actually load is the one those sources
build. A failed deploy, a deploy of an older commit, or a stale CDN edge serves
old facts (a replaced donate link, a moved repo URL) while every local check is
green. The build is byte-reproducible (two builds of one commit are identical,
and CI's build matches a local one), so the check is a straight comparison:
build the tree, fetch every built file from the live site with a cache-busting
query, and compare SHA-256.

Usage:
  python3 scripts/site/verify_live.py                  # build HEAD, compare with project.json site_url
  python3 scripts/site/verify_live.py --built DIR      # compare an existing build
  python3 scripts/site/verify_live.py --retries 6 --wait 30   # after a deploy, allow CDN propagation

Exit: 0 identical, 1 drift (every differing or missing path is listed), 2 usage/build error.
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import hashlib
import json
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def site_url() -> str:
    url = json.loads((REPO / "project.json").read_text())["site_url"]
    return url.rstrip("/") + "/"


def live_path(rel: str) -> str:
    """The URL path Pages serves a built file at (dir/index.html -> dir/)."""
    if rel == "index.html":
        return ""
    if rel.endswith("/index.html"):
        return rel[: -len("index.html")]
    return rel


def fetch(url: str, timeout: float = 30.0) -> bytes | None:
    req = urllib.request.Request(url, headers={"User-Agent": "impossible-os-verify-live",
                                               "Cache-Control": "no-cache"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise


def compare(built: Path, base: str, bust: str) -> list[str]:
    files = sorted(p for p in built.rglob("*") if p.is_file())
    problems: list[str] = []

    def one(p: Path) -> str | None:
        rel = p.relative_to(built).as_posix()
        want = hashlib.sha256(p.read_bytes()).hexdigest()
        try:
            got = fetch(f"{base}{live_path(rel)}?v={bust}")
        except Exception as e:  # network error is a failure to verify, never a pass
            return f"UNREACHABLE {rel}: {e}"
        if got is None:
            return f"MISSING     {rel}"
        if hashlib.sha256(got).hexdigest() != want:
            return f"DIFFERS     {rel}"
        return None

    with cf.ThreadPoolExecutor(max_workers=8) as ex:
        for r in ex.map(one, files):
            if r:
                problems.append(r)
    if not files:
        problems.append("EMPTY BUILD: nothing to compare")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--built", type=Path, help="compare this build directory instead of building HEAD")
    ap.add_argument("--base", help="site URL (default: project.json site_url)")
    ap.add_argument("--retries", type=int, default=1, help="attempts before reporting drift")
    ap.add_argument("--wait", type=float, default=30.0, help="seconds between attempts")
    a = ap.parse_args()

    base = (a.base or site_url()).rstrip("/") + "/"
    with tempfile.TemporaryDirectory() as tmp:
        built = a.built
        if built is None:
            built = Path(tmp) / "site"
            r = subprocess.run([sys.executable, str(REPO / "scripts/site/build.py"), "--out", str(built), "--quiet"],
                               cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
            if r.returncode != 0:
                print(f"verify-live: build failed (rc {r.returncode})\n{r.stderr}", file=sys.stderr)
                return 2
        n = sum(1 for p in built.rglob("*") if p.is_file())
        problems: list[str] = []
        for attempt in range(1, max(1, a.retries) + 1):
            problems = compare(built, base, f"{int(time.time())}-{attempt}")
            if not problems:
                print(f"verify-live: OK -- {n} file(s) at {base} are byte-identical to the build")
                return 0
            if attempt < a.retries:
                print(f"verify-live: attempt {attempt}: {len(problems)} difference(s); retrying in {a.wait:.0f}s")
                time.sleep(a.wait)
    print(f"verify-live: DRIFT -- {len(problems)} of {n} file(s) at {base} do not match the build:")
    for p in problems[:50]:
        print("  " + p)
    if len(problems) > 50:
        print(f"  ... and {len(problems) - 50} more")
    print("Repair: redeploy with `gh workflow run pages.yml` (the scheduled site-live workflow does this itself).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
