#!/usr/bin/env python3
"""GitHub repository metadata (description, homepage, topics) from project.json.

The About box on GitHub is published text like the README, but it lives in the
repository settings, not the tree, so no file check can see it. This keeps it
derived from project.json the same way everything else published is:

    python3 scripts/site/repo_meta.py --check   # compare the live repo (needs gh)
    python3 scripts/site/repo_meta.py --apply   # push project.json values to GitHub

`validate()` is the offline half: build.py --check (lint Check 30) runs it so a
bad value is refused at commit time, before anyone applies it. The live
comparison runs in .github/workflows/repo-metadata.yml on change and daily, so
an edit made in the GitHub UI turns the workflow red instead of drifting.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOPIC_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,49}$")   # GitHub's own topic rule
MAX_TOPICS = 20
MAX_DESCRIPTION = 350
DASHES = (chr(0x2014), chr(0x2013))                    # em and en dash


def expected(project: dict) -> dict:
    """The metadata GitHub should show, derived from project.json."""
    return {
        "description": project["tagline"],
        "homepage": project["site_url"].rstrip("/") + "/",
        "topics": sorted(project.get("topics", [])),
    }


def validate(project: dict) -> list[str]:
    """Offline checks on the values themselves (no network). Shape first: a string
    `topics` would otherwise iterate as one-letter topics and `--apply` would
    replace every real topic with them."""
    errors = []
    for key in ("tagline", "site_url"):
        if not isinstance(project.get(key), str) or not project[key].strip():
            errors.append(f"project.json: {key} must be a non-empty string")
    topics = project.get("topics")
    if not isinstance(topics, list) or not all(isinstance(t, str) for t in topics):
        errors.append("project.json: topics must be a list of strings")
    if errors:
        return errors
    want = expected(project)
    desc = want["description"]
    if len(desc) > MAX_DESCRIPTION:
        errors.append(f"project.json: tagline is {len(desc)} chars; GitHub's description limit is {MAX_DESCRIPTION}")
    if any(d in desc for d in DASHES):
        errors.append("project.json: tagline contains an em or en dash")
    if not want["homepage"].startswith("https://"):
        errors.append(f"project.json: site_url must be https (got {project['site_url']})")
    if not topics:
        errors.append("project.json: topics list is missing or empty")
    if len(topics) > MAX_TOPICS:
        errors.append(f"project.json: {len(topics)} topics; GitHub allows {MAX_TOPICS}")
    if len({str(t).lower() for t in topics}) != len(topics):   # GitHub folds topic case
        errors.append("project.json: duplicate topics")
    for t in topics:
        if not TOPIC_RE.match(t):
            errors.append(f"project.json: topic {t!r} is not a valid GitHub topic (lowercase letters, digits, hyphens)")
    return errors


def normalise(live: dict) -> dict:
    """The comparable subset of `gh api repos/{owner}/{repo}` output."""
    home = live.get("homepage") or ""
    return {
        "description": live.get("description") or "",
        "homepage": home.rstrip("/") + "/" if home else "",
        "topics": sorted(live.get("topics") or []),
    }


def diff(want: dict, have: dict) -> list[str]:
    out = []
    for key in ("description", "homepage"):
        if want[key] != have[key]:
            out.append(f"{key}: GitHub has {have[key]!r}, project.json says {want[key]!r}")
    missing = sorted(set(want["topics"]) - set(have["topics"]))
    extra = sorted(set(have["topics"]) - set(want["topics"]))
    if missing:
        out.append(f"topics missing on GitHub: {', '.join(missing)}")
    if extra:
        out.append(f"topics on GitHub but not in project.json: {', '.join(extra)}")
    return out


def gh(*args: str) -> str:
    try:
        r = subprocess.run(["gh", *args], capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        raise SystemExit(f"repo_meta: gh {' '.join(args[:2])} did not answer within 120 s") from None
    if r.returncode != 0:
        raise SystemExit(f"repo_meta: gh {' '.join(args[:2])} failed: {r.stderr.strip()}")
    return r.stdout


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="compare the live repository with project.json")
    mode.add_argument("--apply", action="store_true", help="write project.json values to the repository")
    args = ap.parse_args()

    project = json.loads((REPO / "project.json").read_text(encoding="utf-8"))
    errors = validate(project)
    if errors:
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1
    slug = f"{project['owner']}/{project['repo']}"
    want = expected(project)
    have = normalise(json.loads(gh("api", f"repos/{slug}")))
    drift = diff(want, have)

    if args.check:
        for d in drift:
            print(f"DRIFT: {d}", file=sys.stderr)
        if drift:
            print("repo-meta: run `python3 scripts/site/repo_meta.py --apply` (edit project.json, never the GitHub UI)",
                  file=sys.stderr)
            return 1
        print(f"repo-meta: OK ({slug} matches project.json)")
        return 0

    if not drift:
        print(f"repo-meta: {slug} already matches project.json")
        return 0
    cmd = ["repo", "edit", slug, "--description", want["description"], "--homepage", want["homepage"]]
    for t in sorted(set(want["topics"]) - set(have["topics"])):
        cmd += ["--add-topic", t]
    for t in sorted(set(have["topics"]) - set(want["topics"])):
        cmd += ["--remove-topic", t]
    gh(*cmd)
    after = diff(want, normalise(json.loads(gh("api", f"repos/{slug}"))))
    if after:
        for d in after:
            print(f"DRIFT after apply: {d}", file=sys.stderr)
        return 1
    print(f"repo-meta: applied {len(drift)} change(s) to {slug}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
