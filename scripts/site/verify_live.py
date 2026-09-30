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

Retained release docs trees (scripts/site/releases.py) are part of the site, so
the build is assembled exactly as a deploy assembles it: from the store branch as
it is now (`--releases remote`, the default), from the store commit a deploy
pinned (`--releases <sha>`), or without releases (`--releases none`).

Usage:
  python3 scripts/site/verify_live.py                  # build HEAD + releases, compare with project.json site_url
  python3 scripts/site/verify_live.py --built DIR      # compare an existing, already assembled build
  python3 scripts/site/verify_live.py --retries 6 --wait 30   # after a deploy, allow CDN propagation

Exit: 0 identical, 1 drift (every differing or missing path is listed), 2 usage/build error.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD_CMD = [sys.executable, str(REPO / "scripts/site/build.py")]
RELEASES_CMD = [sys.executable, str(REPO / "scripts/site/releases.py")]


def run_bounded(cmd: list[str], stop: float, tmp: Path) -> subprocess.CompletedProcess | None:
    """Run cmd in its own process group; None when it outlives the run deadline,
    after killing the whole group (git children included), so neither the build
    nor the release-store fetch and extraction can hold the run past `stop`.
    Its temporary files go under `tmp` (TMPDIR), which the caller owns and
    removes: a killed child never runs its own cleanup, and an extracted store
    can be hundreds of megabytes."""
    child_tmp = Path(tempfile.mkdtemp(prefix="child-", dir=tmp))
    proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            start_new_session=True, env=dict(os.environ, TMPDIR=str(child_tmp)))
    try:
        out, err = proc.communicate(timeout=max(1.0, stop - time.time()))
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.communicate()
        return None
    return subprocess.CompletedProcess(cmd, proc.returncode, out, err)


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


class DeadlinePassed(Exception):
    pass


def fetch(url: str, timeout: float = 30.0, deadline: float | None = None) -> bytes | None:
    """The body at URL, None on 404. Read in chunks so a body that trickles in
    forever stops at `deadline` instead of holding the run past it."""
    req = urllib.request.Request(url, headers={"User-Agent": "impossible-os-verify-live",
                                               "Cache-Control": "no-cache"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            chunks = []
            while True:
                if deadline is not None and time.time() > deadline:
                    raise DeadlinePassed()
                chunk = r.read1(65536)   # what has arrived, so the deadline is checked as bytes trickle in
                if not chunk:
                    return b"".join(chunks)
                chunks.append(chunk)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise


SLOT_SECS = 6 * 3600   # the rotation step: site-live.yml's schedule, so each run takes a new slice


def select_files(built: Path, sample: int, slot: int) -> list[str]:
    """What one run fetches: every file of the main site, and of each retained
    release tree (named by docs/versions.json) its index.html plus a rotating
    1/stride slice of the rest, stride = ceil(files / sample). Consecutive
    six-hour slots take consecutive slices, so every release file is checked
    within `stride` slots (about 3.5 days for a 330-file tree at the default 25).
    This is SAMPLED verification of the releases: a deploy that drops or alters a
    file outside the current slice is caught only when its slice comes round.
    Fetching every one of tens of thousands of release files on every run would
    exceed Pages' bandwidth. sample 0 checks every file."""
    files = sorted(p.relative_to(built).as_posix() for p in built.rglob("*") if p.is_file())
    if sample <= 0:
        return files
    try:
        versions = json.loads((built / "docs" / "versions.json").read_text(encoding="utf-8"))["versions"]
        roots = [f"docs/{v['path']}" for v in versions if v.get("path")]
    except (OSError, ValueError, KeyError, TypeError):
        roots = []
    keep = [f for f in files if not any(f.startswith(r) for r in roots)]
    for r in roots:
        tree = [f for f in files if f.startswith(r)]
        stride = -(-len(tree) // sample)
        keep += [f for i, f in enumerate(tree) if f == r + "index.html" or (i + slot) % stride == 0]
    return sorted(keep)


def failed_paths(problems: list[str]) -> list[str]:
    """The site paths named by compare()'s report lines (`KIND        path[: detail]`)."""
    return [p[12:].split(": ", 1)[0] for p in problems if len(p) > 12 and p[11] == " "]


WORKERS = 8


def compare(built: Path, base: str, bust: str, only: list[str] | None = None,
            deadline: float | None = None) -> list[str]:
    """Report lines for every checked file that is not byte-identical live.

    Every file gets exactly one verdict: its own result, or UNVERIFIED when
    `deadline` (a time.time() value) passes first. Results are recorded under a
    lock and the deadline takes a snapshot under the same lock, so a request that
    finishes at the deadline is either counted or reported, never lost. The
    workers are daemon threads, so one stuck in a slow read cannot keep the
    process alive after the verdict (an executor's workers are joined at exit)."""
    files = ([built / f for f in only] if only is not None
             else sorted(p for p in built.rglob("*") if p.is_file()))
    rels = [p.relative_to(built).as_posix() for p in files]

    def one(p: Path) -> str | None:
        rel = p.relative_to(built).as_posix()
        want = hashlib.sha256(p.read_bytes()).hexdigest()
        try:
            got = fetch(f"{base}{live_path(rel)}?v={bust}", deadline=deadline)
        except DeadlinePassed:
            return f"UNVERIFIED  {rel}: run deadline passed"
        except Exception as e:  # network error is a failure to verify, never a pass
            return f"UNREACHABLE {rel}: {e}"
        if got is None:
            return f"MISSING     {rel}"
        if hashlib.sha256(got).hexdigest() != want:
            return f"DIFFERS     {rel}"
        return None

    lock = threading.Condition()
    verdicts: dict[str, str | None] = {}
    queue = list(zip(rels, files))
    closed = False

    def worker() -> None:
        while True:
            with lock:
                if closed or not queue:
                    return
                rel, p = queue.pop()
            try:
                r = one(p)
            except Exception as e:  # reading the built file failed: still a verdict
                r = f"UNREACHABLE {rel}: {e}"
            with lock:
                if not closed:
                    verdicts[rel] = r
                    lock.notify_all()

    for _ in range(min(WORKERS, len(files))):
        threading.Thread(target=worker, daemon=True).start()
    with lock:
        while len(verdicts) < len(rels):
            remaining = None if deadline is None else deadline - time.time()
            if remaining is not None and remaining <= 0:
                break
            lock.wait(remaining)
        closed = True
        snapshot = dict(verdicts)
    problems = [snapshot[r] if r in snapshot else f"UNVERIFIED  {r}: run deadline passed" for r in rels]
    problems = [x for x in problems if x]
    problems.sort(key=lambda line: line[12:])   # path order, whatever order requests finished in
    if not files:
        problems.append("EMPTY BUILD: nothing to compare")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--built", type=Path, help="compare this build directory instead of building HEAD")
    ap.add_argument("--base", help="site URL (default: project.json site_url)")
    ap.add_argument("--retries", type=int, default=1, help="attempts before reporting drift")
    ap.add_argument("--wait", type=float, default=30.0, help="seconds between attempts")
    ap.add_argument("--release-sample", type=int, default=25,
                    help="files per retained release tree per run, rotating every six hours (0 = every file)")
    ap.add_argument("--deadline", type=float, default=1200.0,
                    help="seconds for the whole run, retries included; unfinished files report UNVERIFIED")
    ap.add_argument("--releases", default="remote",
                    help="retained release trees to add to a fresh build: remote (default), none, or a store commit")
    a = ap.parse_args()
    if a.built is not None and a.releases != "remote":
        ap.error("--releases applies to a fresh build; --built is compared as it is")
    if not a.releases:
        # An empty value is a lost workflow output, never "no releases": guessing
        # would compare against the wrong set of trees.
        ap.error("--releases is empty; pass remote, none or a store commit")

    stop = time.time() + a.deadline   # the WHOLE run: build, store fetch and comparison
    base = (a.base or site_url()).rstrip("/") + "/"
    with tempfile.TemporaryDirectory() as tmp:
        built = a.built
        if built is None:
            built = Path(tmp) / "site"
            r = run_bounded(BUILD_CMD + ["--out", str(built), "--quiet"], stop, Path(tmp))
            if r is None:
                print("verify-live: the build did not finish before the run deadline", file=sys.stderr)
                return 2
            if r.returncode != 0:
                print(f"verify-live: build failed (rc {r.returncode})\n{r.stderr}", file=sys.stderr)
                return 2
            receipt = Path(tmp) / "store"
            r = run_bounded(RELEASES_CMD + ["assemble", "--site", str(built), "--releases", a.releases,
                                            "--github-output", str(receipt)], stop, Path(tmp))
            if r is None:
                print("verify-live: assembling the retained releases did not finish before the run deadline",
                      file=sys.stderr)
                return 2
            if r.returncode != 0 or not receipt.is_file():
                print(f"verify-live: cannot assemble the retained releases (rc {r.returncode}):\n{r.stderr}",
                      file=sys.stderr)
                return 2
            print(f"verify-live: release {receipt.read_text(encoding='utf-8').strip()}")
        n = sum(1 for p in built.rglob("*") if p.is_file())
        todo = select_files(built, a.release_sample, int(time.time() // SLOT_SECS))
        checked = len(todo)
        if checked < n:
            print(f"verify-live: checking {len(todo)} of {n} file(s): all of main, a rotating slice of each release")
        problems: list[str] = []
        for attempt in range(1, max(1, a.retries) + 1):
            # A retry refetches only what failed: CDN propagation, not the whole site.
            problems = compare(built, base, f"{int(time.time())}-{attempt}", todo, stop)
            todo = failed_paths(problems) or todo
            if not problems:
                print(f"verify-live: OK -- {checked} of {n} file(s) at {base} checked, all byte-identical to the build")
                return 0
            if attempt < a.retries and time.time() + a.wait < stop:
                print(f"verify-live: attempt {attempt}: {len(problems)} difference(s); retrying in {a.wait:.0f}s")
                time.sleep(a.wait)
            else:
                break
    print(f"verify-live: DRIFT -- {len(problems)} of {n} file(s) at {base} do not match the build "
          f"or could not be verified:")
    for p in problems[:50]:
        print("  " + p)
    if len(problems) > 50:
        print(f"  ... and {len(problems) - 50} more")
    print("Repair: redeploy with `gh workflow run pages.yml` (the scheduled site-live workflow does this itself).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
