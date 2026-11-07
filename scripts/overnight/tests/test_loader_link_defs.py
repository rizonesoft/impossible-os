#!/usr/bin/env python3
"""BOOTX64 links with -z defs, so an undefined symbol fails the build rather than a
boot: `memcpy` once linked through a GLOB_DAT slot holding 0 (v22, 2026-11-07)."""
import pathlib
import re
import shutil
import subprocess
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
MAKEFILE = REPO / "src/boot/uefi/Makefile"


def _link_flags():
    text = MAKEFILE.read_text()
    m = re.search(r"bootx64\.so:.*\n\t\$\(LD\) ([^\\\n]*)\\", text)
    assert m, "the bootx64.so link rule moved"
    return m.group(1).split()


def test_rule_carries_the_flag():
    flags = _link_flags()
    assert "-z" in flags and flags[flags.index("-z") + 1] == "defs", flags


def test_refusal_those_flags_reject_an_undefined_symbol():
    clang, lld = shutil.which("clang-19"), shutil.which("ld.lld-19")
    if not clang or not lld:
        print("skip: toolchain absent")
        return
    with tempfile.TemporaryDirectory() as d:
        src = pathlib.Path(d) / "a.c"
        # Not memcpy itself: clang inlines a small constant-size copy, which leaves
        # no undefined symbol and makes this control prove nothing.
        src.write_text("extern void loader_missing_symbol(void);\n"
                       "void f(void) { loader_missing_symbol(); }\n")
        obj = pathlib.Path(d) / "a.o"
        subprocess.run([clang, "--target=x86_64-elf", "-fPIC", "-c", str(src),
                        "-o", str(obj)], check=True)
        out = pathlib.Path(d) / "a.so"
        bad = subprocess.run([lld, *_link_flags(), "-o", str(out), str(obj)],
                             capture_output=True, text=True)
        assert bad.returncode != 0 and "loader_missing_symbol" in bad.stderr, bad
        # The control: the same link without the flag is what used to succeed.
        loose = [f for f in _link_flags() if f not in ("-z", "defs")]
        ok = subprocess.run([lld, *loose, "-o", str(out), str(obj)],
                            capture_output=True, text=True)
        assert ok.returncode == 0, ok


if __name__ == "__main__":
    test_rule_carries_the_flag()
    test_refusal_those_flags_reject_an_undefined_symbol()
    print("OK")
