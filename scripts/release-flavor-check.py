#!/usr/bin/env python3
"""Compile named sources as the RELEASE flavor builds them, syntax only.

    python3 scripts/release-flavor-check.py --db PATH --work ROOT FILE...
    python3 scripts/release-flavor-check.py --selftest

The compile database the analyzer reads keeps the TEST flavor's command for
every source both flavors compile, so a defect that exists only with
KERNEL_TESTS undefined (a variable that only test code initialises, a helper
only a test caller uses) passes analysis and fails the release build under
-Werror. On 2026-11-11 one did: a local read under `#ifdef KERNEL_TESTS` set
only there failed CI's KERNEL_TESTS=off job, and main stayed red until a fix
landed. This runs each named source's release command with -fsyntax-only, so
the pre-push gate refuses that commit instead.

--db is the release-flavor database (gen-compile-db.py --release-output). A
named source the release flavor does not compile is skipped: the generator
writes that database only when every such source is one the Makefile prunes
(its C_SRCS differs between the flavors), so an absent entry is a test translation unit
with no release build to break, never a command the parser could not read. Exit 0 every source compiles,
1 a source does not (its diagnostics are printed repo-relative), 2 the check
could not run.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

# Options that make the compiler write something, with the value they take.
WRITES_WITH_VALUE = {"-o", "-MF", "-MT", "-MQ"}
WRITES_BARE = {"-c", "-MMD", "-MD", "-MP", "-M", "-MM"}
DIAG = re.compile(r"^(?P<path>[^:\s][^:]*):(?P<rest>\d+:\d+: (?:warning|error|fatal error): .*)$")


def syntax_only(argv):
    """The same compile, writing nothing: outputs and dependency files dropped."""
    out, skip = [], False
    for a in argv:
        if skip:
            skip = False
            continue
        if a in WRITES_WITH_VALUE:
            skip = True
            continue
        # Joined forms too: -obuild/a.o, -MFa.d, -MF=a.d, -MTa.o.
        if a in WRITES_BARE or (a.startswith("-o") and len(a) > 2) \
                or any(a.startswith(o) and len(a) > len(o) for o in ("-MF", "-MT", "-MQ")):
            continue
        out.append(a)
    if skip:
        raise ValueError("an output option has no value")
    return out + ["-fsyntax-only"]


def check(db_path, work, files):
    try:
        db = json.load(open(db_path, encoding="utf-8"))
    except (OSError, ValueError) as e:
        print(f"release-flavor-check: cannot read {db_path}: {e}", file=sys.stderr)
        return 2
    by_file = {}
    for e in db:
        rel = os.path.relpath(os.path.join(e["directory"], e["file"]), work)
        by_file.setdefault(rel, e)
    failed = checked = 0
    for f in files:
        e = by_file.get(f)
        if e is None:
            continue
        try:
            argv = syntax_only(e["arguments"])
            # deadline: 300 s a source; the largest compiles in under ten.
            r = subprocess.run(argv, cwd=e["directory"], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, errors="replace", timeout=300)
        except (ValueError, OSError, subprocess.TimeoutExpired) as ex:
            print(f"release-flavor-check: could not compile {f}: {ex}", file=sys.stderr)
            return 2
        checked += 1
        if r.returncode == 0:
            continue
        failed += 1
        for line in (r.stderr or "").splitlines():
            m = DIAG.match(line)
            if m:
                p = os.path.normpath(os.path.join(e["directory"], m.group("path")))
                if p == work or p.startswith(work + os.sep):
                    p = os.path.relpath(p, work)
                line = f"{p}:{m.group('rest')}"
            print(line)
    if failed:
        print(f"release-flavor-check: {failed} of {checked} source(s) do not compile as the "
              f"release flavor (KERNEL_TESTS=off)")
        return 1
    print(f"release-flavor-check: {checked} source(s) compile as the release flavor")
    return 0


def selftest():
    cases = [
        (["clang-19", "-Wall", "-MMD", "-MP", "-c", "src/a.c", "-o", "build/a.o"],
         ["clang-19", "-Wall", "src/a.c", "-fsyntax-only"]),
        (["clang", "-MF", "a.d", "-MT", "a.o", "-c", "a.c", "-obuild/a.o"],
         ["clang", "a.c", "-fsyntax-only"]),
        (["clang", "-MF=a.d", "-DX=-o", "-I", "-c", "a.c"],
         ["clang", "-DX=-o", "-I", "a.c", "-fsyntax-only"]),
        (["clang", "-MFbuild/a.d", "-MTa.o", "-MQa.o", "-Werror", "-c", "a.c"],
         ["clang", "-Werror", "a.c", "-fsyntax-only"]),
    ]
    for argv, want in cases:
        got = syntax_only(argv)
        if got != want:
            print(f"release-flavor-check selftest FAIL: {argv} -> {got}, want {want}", file=sys.stderr)
            return 1
    try:
        syntax_only(["clang", "-c", "a.c", "-o"])
        print("release-flavor-check selftest FAIL: a dangling -o was accepted", file=sys.stderr)
        return 1
    except ValueError:
        pass
    # The real compiler under -Werror: what the transformation leaves must
    # compile a clean source with no unused-argument complaint.
    cc = next((c for c in ("clang-19", "clang") if shutil.which(c)), None)
    if cc is None:
        print("release-flavor-check selftest: 5 checks passed (no clang for the compiler case)")
        return 0
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "a.c")
        with open(src, "w", encoding="utf-8") as f:
            f.write("int a(void) { return 0; }\n")
        argv = syntax_only([cc, "-Wall", "-Werror", "-MMD", "-MP", "-MF" + os.path.join(d, "a.d"),
                            "-MT" + "a.o", "-c", src, "-o" + os.path.join(d, "a.o")])
        # deadline: 60 s for one trivial compile.
        r = subprocess.run(argv, cwd=d, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                           timeout=60)
        if r.returncode != 0 or os.listdir(d) != ["a.c"]:
            print(f"release-flavor-check selftest FAIL: {argv} -> rc {r.returncode}, files "
                  f"{os.listdir(d)}: {r.stderr.strip()}", file=sys.stderr)
            return 1
    print("release-flavor-check selftest: 6 checks passed")
    return 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    db = work = None
    files = []
    it = iter(argv)
    for a in it:
        if a == "--db":
            db = next(it, None)
        elif a == "--work":
            work = next(it, None)
        elif a.startswith("-"):
            print(f"release-flavor-check: unknown option {a}", file=sys.stderr)
            return 2
        else:
            files.append(a)
    if not db or not work or not os.path.isdir(work):
        print("release-flavor-check: usage: --db PATH --work ROOT FILE...", file=sys.stderr)
        return 2
    return check(db, os.path.realpath(work), files)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
