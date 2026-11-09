#!/usr/bin/env python3
"""A test fixture must not outlive the process that made it.

Measured 2026-11-06: /tmp on the dev host is a RAM-backed tmpfs, and
test_receipt_commit_binding.py left five full clones of this repo behind on every
suite run, about 1.1 GB each time. Nothing removed them until the VM restarted, so
a day of pushes took 7 GB of memory from everything else sharing the VM.

Each builder below runs in its own interpreter with TMPDIR pointed at an empty
directory. That directory must be empty again once the interpreter has exited.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent

BUILDERS = {
    "test_receipt_commit_binding": "m._clone()",
}

RUNNER = """
import importlib.util, sys
spec = importlib.util.spec_from_file_location("fixture_under_test", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
{call}
"""


def leftovers(module, call):
    with tempfile.TemporaryDirectory(prefix="fixture-cleanup.") as tmp:
        subprocess.run(
            [sys.executable, "-c", RUNNER.format(call=call),
             str(HERE / f"{module}.py")],
            check=True, capture_output=True,
            env={"PATH": "/usr/bin:/bin", "TMPDIR": tmp}, timeout=120)
        return sorted(p.name for p in pathlib.Path(tmp).iterdir())


def main() -> int:
    failures = 0
    for module, call in BUILDERS.items():
        left = leftovers(module, call)
        if left:
            failures += 1
            print(f"  FAIL {module}: left {left} behind after exit")
        else:
            print(f"  PASS {module}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
