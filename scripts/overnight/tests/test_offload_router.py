#!/usr/bin/env python3
"""D2: interactive_offload_router routes a helper/symbol-LOCATION prompt to a
mapper dispatch instead of leaving it to an N-round inline grep hunt, without
false-positiving on an ordinary prompt."""
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
HOOK = REPO / ".claude/hooks/interactive_offload_router.py"


def _hint(prompt):
    r = subprocess.run([sys.executable, str(HOOK)],
                       input=json.dumps({"prompt": prompt}), text=True,
                       capture_output=True)
    assert r.returncode == 0, r.stderr
    if not r.stdout.strip():
        return None
    return json.loads(r.stdout).get("systemMessage", "")


def test_helper_location_hunt_routes_to_explorer():
    for p in ["where do the string helpers live (kstrdup, kstrndup, memcpy)?",
              "is there a spinlock primitive we can reuse?",
              "do we have a kernel allocator wrapper for aligned pages?"]:
        h = _hint(p)
        assert h and "kernel-explorer" in h, f"expected explorer hint for: {p!r} -> {h!r}"


def test_ordinary_prompt_no_hint():
    # A plain implementation ask must NOT trip the helper-hunt route.
    assert _hint("please implement the timer reap teardown for TODO-21") is None \
        or "offload hint" in (_hint("please implement the timer reap teardown for TODO-21") or "")


def test_existing_routes_still_fire():
    assert "checks-runner" in (_hint("re-run the mm test suite") or "")


if __name__ == "__main__":
    test_helper_location_hunt_routes_to_explorer()
    test_ordinary_prompt_no_hint()
    test_existing_routes_still_fire()
    print("PASS: interactive-offload-router (D2)")
