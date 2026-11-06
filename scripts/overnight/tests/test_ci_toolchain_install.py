#!/usr/bin/env python3
"""CI installs LLVM from the Ubuntu archive. ubuntu-latest moved to 26.04, which has
no apt-key, so the old apt.llvm.org setup failed every workflow before it built
anything. Control: each workflow must still install the pinned toolchain.
"""
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parents[3]
FLOWS = ["build.yml", "release.yml", "visual-regression.yml"]


def test_no_removed_tooling_and_the_toolchain_is_still_installed():
    for name in FLOWS:
        text = (REPO / ".github/workflows" / name).read_text()
        assert "apt-key" not in text, name
        for pkg in ("clang-", "lld-"):
            assert pkg in text, (name, pkg)
        assert "apt-get install" in text, name
        step = re.search(r"- name: Install LLVM.*?(?=\n\s*- name:|\n\s*#\s*──|\Z)", text, re.S).group(0)
        assert re.search(r"^\s*timeout-minutes: \d+$", step, re.M), (name, "a hang must fail fast")
        assert "-qq" not in step.split("apt-get install")[1].split("\\")[0], (name, "keep apt output in the log")


if __name__ == "__main__":
    test_no_removed_tooling_and_the_toolchain_is_still_installed()
    print("PASS test_ci_toolchain_install")
    sys.exit(0)
