#!/usr/bin/env python3
"""Decide whether a weekly demo may be built from one commit of main.

    demo_gate.py --repo OWNER/REPO --sha COMMIT --tag demo-YY.M.D [--wait SECONDS] [--dry-run]

release.yml runs it before a scheduled demo. It prints `proceed=true` or
`proceed=false` (and appends it to $GITHUB_OUTPUT), and refuses with exit 1.

  proceed=true   every workflow the push started on this commit finished clean,
                 every workflow in REQUIRED among them
  proceed=false  a demo already exists for this commit, so nothing is new
                 (--dry-run publishes nothing, so it does not ask)
  exit 1         a run failed, was cancelled, is missing or did not finish in
                 time, or the tag is taken by another commit

A demo is published unattended, so anything unclear is a refusal: the failed
run is the report, and last week's demo stays the newest.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time

# Workflows every push to main starts. One with a path filter may rightly have no run.
REQUIRED = {"Build", "GitHub Pages", "Visual Regression"}
CLEAN = {"success", "skipped", "neutral"}
POLL_SECONDS = 60
API_TIMEOUT = 120


class Refused(Exception):
    pass


def judge(runs: list[dict]) -> str:
    """`wait` while the verdict can still change, `clean` when every run is.
    Raises Refused for a run that ended any other way."""
    bad = [r for r in runs if r["status"] == "completed" and r["conclusion"] not in CLEAN]
    if bad:
        raise Refused("not green: " + ", ".join(f"{r['name']} {r['conclusion']}" for r in bad))
    if any(r["status"] != "completed" for r in runs):
        return "wait"
    if not REQUIRED <= {r["name"] for r in runs if r["conclusion"] == "success"}:
        return "wait"   # not started yet, or it never ran: the deadline turns this into a refusal
    return "clean"


def already_published(tags: dict[str, str], tag: str, sha: str) -> bool:
    """tags: {demo tag: commit}. True when this commit already has a demo."""
    if sha in tags.values():
        return True
    if tag in tags:
        raise Refused(f"{tag} already exists for {tags[tag][:12]}")
    return False


def api(path: str, jq: str) -> list[dict]:
    """One object per line: the CI image's gh 2.46 has no `--slurp` (2.48)."""
    out = subprocess.run(["gh", "api", "--paginate", "--jq", jq, path], check=True, capture_output=True,
                         text=True, timeout=API_TIMEOUT).stdout
    return [json.loads(line) for line in out.splitlines() if line]


def demo_tags(repo: str) -> dict[str, str]:
    refs = api(f"repos/{repo}/git/matching-refs/tags/demo-", ".[] | {ref, sha: .object.sha}")
    return {r["ref"].removeprefix("refs/tags/"): r["sha"] for r in refs}


def push_runs(repo: str, sha: str) -> list[dict]:
    return api(f"repos/{repo}/actions/runs?head_sha={sha}&event=push&branch=main&per_page=100",
               ".workflow_runs[] | {name, status, conclusion}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--sha", required=True)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--wait", type=int, default=3600)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    try:
        if not a.dry_run and already_published(demo_tags(a.repo), a.tag, a.sha):
            verdict = "false"
            print(f"demo gate: {a.sha[:12]} already has a demo; nothing new to publish")
        else:
            deadline = time.monotonic() + a.wait
            while judge(push_runs(a.repo, a.sha)) != "clean":
                if time.monotonic() >= deadline:
                    raise Refused(f"no finished verdict from all of {', '.join(sorted(REQUIRED))} for {a.sha[:12]} after {a.wait} s")
                time.sleep(POLL_SECONDS)
            verdict = "true"
            print(f"demo gate: {a.sha[:12]} is green")
    except Refused as e:
        print(f"::error::demo refused: {e}")
        return 1
    print(f"proceed={verdict}")
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a") as fh:
            fh.write(f"proceed={verdict}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
