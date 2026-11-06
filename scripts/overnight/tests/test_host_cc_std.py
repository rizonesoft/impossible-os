#!/usr/bin/env python3
"""Host tools that include kernel headers must not follow the compiler's default C
standard. GCC 15 defaults to C23, where `bool` is a keyword, so a clean build failed
in dump-kernel.c while every incremental build, which never rebuilt it, stayed green.
"""
import pathlib
import shlex
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
OUT = "build/tools/dump-boot-info-kernel"
SRC = "tools/boot-info-manifest/dump-kernel.c"


def recipe() -> str:
    r = subprocess.run(["make", "-n", "-B", OUT], cwd=REPO, capture_output=True,
                       text=True, timeout=120)
    lines = [l for l in r.stdout.splitlines() if SRC in l and " -o " in l]
    assert lines, f"no compile line for {SRC}: {r.stderr[:300]}"
    return lines[-1]


def test_recipe_pins_the_kernel_standard():
    line = recipe()
    assert "-std=gnu11" in line, line


def test_the_tool_compiles_with_that_recipe():
    """Control against a flag that is present but ineffective: run the real command."""
    argv = shlex.split(recipe())
    at = argv.index("-o") + 1
    with tempfile.TemporaryDirectory() as d:
        argv[at] = f"{d}/out"
        r = subprocess.run(argv, cwd=REPO, capture_output=True, text=True, timeout=120)
        assert r.returncode == 0, r.stderr[:500]
        assert pathlib.Path(d, "out").is_file(), "the output did not go to the temp path"


if __name__ == "__main__":
    test_recipe_pins_the_kernel_standard()
    test_the_tool_compiles_with_that_recipe()
    print("PASS test_host_cc_std")
    sys.exit(0)
