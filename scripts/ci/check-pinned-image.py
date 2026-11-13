#!/usr/bin/env python3
"""Every CI job that checks out the tree runs in the pinned CI image, and none installs a package.

The image (scripts/ci/image/Dockerfile, published by .github/workflows/ci-image.yml)
holds the whole toolchain, so a job that installs a package at run time depends on
the package mirror again: on 2026-11-11 nine Build runs in a row failed in their
install step and no project code ran. In each workflow this checks, it refuses:

  * a run step that installs a package (apt, apt-get, apt-install.sh, pip, pipx);
  * a job that checks out the tree and has no `container.image`;
  * a container image not pinned by `@sha256:` digest;
  * two different digests, since then two toolchains are tested;
  * a migrated workflow whose run steps do not default to bash (a container
    step otherwise runs under sh, and `[[ ... =~ ... ]]` stops parsing);
  * a container that is not `--init` as uid 1001: GitHub starts the container
    with `tail -f /dev/null` as PID 1, which never reaps an orphan, so the
    LSP bridge's process-reaping tests saw zombies as live (2026-11-13).

Usage: check-pinned-image.py [WORKFLOWS_DIR]
       check-pinned-image.py --selftest [WORKFLOWS_DIR]
"""
from __future__ import annotations

import copy
import re
import sys
from pathlib import Path

import yaml

WORKFLOWS = ("build.yml", "release.yml", "visual-regression.yml", "todo-graph.yml")
# A package manager, then its install or update verb anywhere later in the same
# command (options and their values come between: `apt-get -o X=1 install`,
# `python3 -m pip --quiet install`). Line continuations are joined first. The
# threat is an honest edit re-adding an install step, not a crafted evasion.
INSTALL_RE = re.compile(r"\b(?:apt-get|apt|pipx|pip3?)\b[^\n;&|]*?\b(?:install|update)\b|apt-install\.sh")


def installs(run: str) -> bool:
    return bool(INSTALL_RE.search(run.replace("\\\n", " ")))
DIGEST_RE = re.compile(r"^[a-z0-9./_-]+@sha256:[0-9a-f]{64}$")


def _checks_out(job: dict) -> bool:
    return any(str(s.get("uses", "")).startswith("actions/checkout@") for s in job.get("steps") or [])


def _image(job: dict) -> str | None:
    c = job.get("container")
    if isinstance(c, str):
        return c
    if isinstance(c, dict):
        return c.get("image")
    return None


def problems(workflows: dict) -> list:
    """workflows: {name: parsed YAML}. Returns one message per broken rule."""
    out, digests = [], {}
    for name, wf in workflows.items():
        jobs = (wf or {}).get("jobs") or {}
        shell = (((wf or {}).get("defaults") or {}).get("run") or {}).get("shell")
        for jname, job in jobs.items():
            for i, step in enumerate(job.get("steps") or []):
                run = step.get("run")
                if isinstance(run, str) and installs(run):
                    out.append(f"{name}: job {jname} step {step.get('name', i)!r} installs a package at run time")
            if not _checks_out(job):
                continue
            image = _image(job)
            if not image:
                out.append(f"{name}: job {jname} checks out the tree but has no container image")
                continue
            if not DIGEST_RE.match(image):
                out.append(f"{name}: job {jname} image {image!r} is not pinned by @sha256 digest")
                continue
            digests.setdefault(image, []).append(f"{name}:{jname}")
            opts = str((job.get("container") or {}).get("options", "")).split()
            if "--init" not in opts or "--user" not in opts or opts[opts.index("--user") + 1:][:1] != ["1001:1001"]:
                out.append(f"{name}: job {jname} container options must include --init and --user 1001:1001")
            if shell != "bash":
                out.append(f"{name}: run steps of a container job must default to bash (defaults.run.shell)")
    if len(digests) > 1:
        out.append("more than one CI image digest: " + "; ".join(f"{d} ({', '.join(j)})" for d, j in digests.items()))
    return sorted(set(out))


def load(root: Path) -> dict:
    return {w: yaml.safe_load((root / w).read_text(encoding="utf-8")) for w in WORKFLOWS if (root / w).is_file()}


def selftest(root: Path) -> int:
    base = load(root)
    if problems(base):
        print("check-pinned-image selftest: the live workflows already fail; fix them first")
        return 1
    name = "build.yml"
    job = next(j for j, v in base[name]["jobs"].items() if _checks_out(v))
    breaks = {
        "installs a package": lambda w: w[name]["jobs"][job]["steps"].append({"name": "x", "run": "sudo apt-get install -y zip"}),
        "has no container image": lambda w: w[name]["jobs"][job].pop("container"),
        "not pinned by @sha256": lambda w: w[name]["jobs"][job].__setitem__("container", {"image": "ghcr.io/x/y:latest"}),
        "more than one CI image digest": lambda w: w[name]["jobs"][job].__setitem__("container", {"image": "ghcr.io/x/y@sha256:" + "0" * 64}),
        "must default to bash": lambda w: w[name].pop("defaults"),
        "must include --init": lambda w: w[name]["jobs"][job]["container"].__setitem__("options", "--user 1001:1001"),
        "must include --init (user)": lambda w: w[name]["jobs"][job]["container"].__setitem__("options", "--init"),
    }
    for i, run in enumerate(("apt-get -o Acquire::Retries=3 install -y zip",
                             "python3 -m pip --quiet install PyYAML",
                             "sudo apt-get \\\n  install zip", "pipx install ruff==0.17.4")):
        breaks[f"installs a package ({i})"] = (
            lambda w, run=run: w[name]["jobs"][job]["steps"].append({"name": "x", "run": run}))
    fails = []
    for want, mutate in breaks.items():
        w = copy.deepcopy(base)
        mutate(w)
        got = problems(w)
        if not any(want.split(" (")[0] in m for m in got):
            fails.append(f"{want}: not refused ({got})")
    print(f"check-pinned-image selftest: {fails or 'OK'}")
    return 1 if fails else 0


def main(argv: list) -> int:
    test = "--selftest" in argv
    rest = [a for a in argv if a != "--selftest"]
    root = Path(rest[0] if rest else ".github/workflows")
    if test:
        return selftest(root)
    found = problems(load(root))
    for m in found:
        print(f"check-pinned-image: {m}")
    if not found:
        print("check-pinned-image: every checkout job runs in one pinned image and installs nothing")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
