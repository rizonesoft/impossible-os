#!/usr/bin/env python3
"""Print the toolchain setup `build.yml` runs, as one shell script.

The container tier reads the install steps out of the workflow, so it cannot drift
from what CI installs. A step this cannot find is an error, never a skip: it means
the workflow changed shape and the tier would otherwise test a different host.

Usage: ci_container_setup.py [path/to/build.yml]
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

import yaml

STEP_NAMES = ("Use GNU coreutils", "Install LLVM", "Install dependencies")


def setup_script(workflow_text: str) -> str:
    wf = yaml.safe_load(workflow_text)
    env = wf.get("env") or {}
    steps = wf["jobs"]["build"]["steps"]
    parts = ["set -euxo pipefail"]
    for name in STEP_NAMES:
        hit = [s for s in steps if str(s.get("name", "")).startswith(name)]
        if len(hit) != 1 or "run" not in hit[0]:
            raise SystemExit(f"ci_container_setup: step {name!r} not found exactly "
                             "once in the build job; the workflow changed shape")
        body = hit[0]["run"]
        for key, value in env.items():
            body = re.sub(r"\$\{\{\s*env\." + re.escape(key) + r"\s*\}\}", str(value), body)
        if "${{" in body:
            raise SystemExit(f"ci_container_setup: unresolved expression in {name!r}")
        parts.append(body)
    return "\n".join(parts) + "\n"


def main(argv: list) -> int:
    path = Path(argv[0] if argv else ".github/workflows/build.yml")
    sys.stdout.write(setup_script(path.read_text(encoding="utf-8")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
