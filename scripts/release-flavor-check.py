#!/usr/bin/env python3
"""Compile named sources as the RELEASE flavor builds them, to a throwaway object.

    python3 scripts/release-flavor-check.py --db PATH --work ROOT FILE...
    python3 scripts/release-flavor-check.py --selftest

The compile database the analyzer reads keeps the TEST flavor's command for
every source both flavors compile, so a defect that exists only with
KERNEL_TESTS undefined (a variable that only test code initialises, a helper
only a test caller uses) passes analysis and fails the release build under
-Werror. On 2026-11-11 one did: a local read under `#ifdef KERNEL_TESTS` set
only there failed CI's KERNEL_TESTS=off job, and main stayed red until a fix
landed. This runs each named source's release command, writing its object to a
temporary directory, so the pre-push gate refuses that commit instead. It is a
real compile and not -fsyntax-only: the backend rejects what the parser accepts
(an invalid inline-assembly instruction, for one), and such a source would
pass a syntax check and still fail the release build.

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
from concurrent.futures import ThreadPoolExecutor

# Options that make the compiler write something, with the value they take.
WRITES_WITH_VALUE = {"-o", "-MF", "-MT", "-MQ"}
WRITES_BARE = {"-MMD", "-MD", "-MP", "-M", "-MM"}
HEADER_SUFFIXES = (".h", ".inc")
DIAG = re.compile(r"^(?P<path>[^:\s][^:]*):(?P<rest>\d+:\d+: (?:warning|error|fatal error): .*)$")


def object_compile(argv, obj):
    """The same compile, writing only `obj`: its own output and dependency files dropped."""
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
    if "-c" not in out:
        raise ValueError("not a compile command (no -c)")
    return out + ["-o", obj]


def dep_command(argv):
    """The compile's preprocessor dependency listing on stdout: -M lists every
    header the translation unit includes under these flags. No -MG: a missing
    header would be listed by its include spelling, not its path, so a deleted
    one would match nothing. Without it the listing fails, and dependents()
    compiles that unit, whose real compile names the missing header."""
    out = object_compile(argv, "unused.o")[:-2]
    return [a for a in out if a != "-c"] + ["-M"]


def parse_deps(text, directory, work):
    """Repo-relative paths from make-syntax dependency output."""
    body = text.replace("\\\n", " ")
    _, _, body = body.partition(": ")
    paths, cur = set(), ""
    i = 0
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body) and body[i + 1] == " ":
            cur += " "
            i += 2
            continue
        if c.isspace():
            if cur:
                paths.add(cur)
            cur = ""
        else:
            cur += c
        i += 1
    if cur:
        paths.add(cur)
    rel = set()
    for pth in paths:
        full = os.path.normpath(os.path.join(directory, pth))
        if full.startswith(work + os.sep):
            rel.add(os.path.relpath(full, work))
    return rel


def dependents(entries, work, headers, jobs):
    """{source} of every translation unit that includes one of `headers`, plus
    every unit whose header listing fails (it includes a header that does not
    exist, a deleted one among them): failing closed compiles it. Returns
    (sources, error)."""
    want = set(headers)

    def one(item):
        rel, e = item
        # deadline: 120 s a preprocessor run; the whole database takes seconds.
        r = subprocess.run(dep_command(e["arguments"]), cwd=e["directory"],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                           errors="replace", timeout=120)
        if r.returncode != 0:
            return rel, None, True
        return rel, parse_deps(r.stdout, e["directory"], work), False

    hits = set()
    try:
        with ThreadPoolExecutor(jobs) as ex:
            for rel, deps, failed in ex.map(one, entries.items()):
                if failed or deps & want:
                    hits.add(rel)
    except (ValueError, OSError, subprocess.TimeoutExpired) as ex:
        return None, f"cannot list headers: {ex}"
    return hits, None


def check(db_path, work, files, jobs=None):
    try:
        db = json.load(open(db_path, encoding="utf-8"))
    except (OSError, ValueError) as e:
        print(f"release-flavor-check: cannot read {db_path}: {e}", file=sys.stderr)
        return 2
    by_file = {}
    for e in db:
        rel = os.path.relpath(os.path.join(e["directory"], e["file"]), work)
        by_file.setdefault(rel, e)
    jobs = jobs or os.cpu_count() or 4
    # A changed header is checked through every release translation unit that
    # includes it, found under the release flags: a header only release code
    # includes (ACPICA's), or a block in a shared header the test seams hide,
    # breaks no test-flavor compile.
    headers = [f for f in files if f.endswith(HEADER_SUFFIXES)]
    sources = [f for f in files if not f.endswith(HEADER_SUFFIXES)]
    via_headers = 0
    if headers:
        hits, err = dependents(by_file, work, headers, jobs)
        if hits is None:
            print(f"release-flavor-check: {err}", file=sys.stderr)
            return 2
        extra = sorted(hits - set(sources))
        via_headers = len(extra)
        sources += extra
    todo = [(f, by_file[f]) for f in dict.fromkeys(sources) if f in by_file]
    objs = tempfile.TemporaryDirectory(prefix="release-flavor-check-")

    def compile_one(item):
        n, (f, e) = item
        argv = object_compile(e["arguments"], os.path.join(objs.name, f"{n}.o"))
        # deadline: 300 s a source; the largest compiles in under ten.
        return f, e, subprocess.run(argv, cwd=e["directory"], stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True, errors="replace",
                                    timeout=300)

    try:
        with ThreadPoolExecutor(jobs) as ex:
            results = list(ex.map(compile_one, enumerate(todo)))
    except (ValueError, OSError, subprocess.TimeoutExpired) as ex:
        print(f"release-flavor-check: could not compile: {ex}", file=sys.stderr)
        return 2
    failed = 0
    checked = len(results)
    for f, e, r in results:
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
    print(f"release-flavor-check: {checked} source(s) compile as the release flavor"
          + (f" ({via_headers} through {len(headers)} changed header(s))" if headers else ""))
    return 0


def selftest():
    cases = [
        (["clang-19", "-Wall", "-MMD", "-MP", "-c", "src/a.c", "-o", "build/a.o"],
         ["clang-19", "-Wall", "-c", "src/a.c", "-o", "/t/x.o"]),
        (["clang", "-MF", "a.d", "-MT", "a.o", "-c", "a.c", "-obuild/a.o"],
         ["clang", "-c", "a.c", "-o", "/t/x.o"]),
        (["clang", "-MF=a.d", "-DX=-o", "-I", "-c", "a.c"],
         ["clang", "-DX=-o", "-I", "-c", "a.c", "-o", "/t/x.o"]),
        (["clang", "-MFbuild/a.d", "-MTa.o", "-MQa.o", "-Werror", "-c", "a.c"],
         ["clang", "-Werror", "-c", "a.c", "-o", "/t/x.o"]),
    ]
    for argv, want in cases:
        got = object_compile(argv, "/t/x.o")
        if got != want:
            print(f"release-flavor-check selftest FAIL: {argv} -> {got}, want {want}", file=sys.stderr)
            return 1
    for bad, why in ((["clang", "-c", "a.c", "-o"], "a dangling -o"), (["clang", "-E", "a.c"], "a command with no -c")):
        try:
            object_compile(bad, "/t/x.o")
            print(f"release-flavor-check selftest FAIL: {why} was accepted", file=sys.stderr)
            return 1
        except ValueError:
            pass
    deps = parse_deps("a.o: src/a.c include/x.h \\\n  inc/my\\ file.h ../out.h /abs.h\n", "/w/src", "/w")
    if deps != {"src/src/a.c", "src/include/x.h", "src/inc/my file.h", "out.h"}:
        print(f"release-flavor-check selftest FAIL: dependency parse gave {sorted(deps)}", file=sys.stderr)
        return 1
    if [a for a in dep_command(["cc", "-MMD", "-c", "a.c", "-o", "a.o"]) if a in ("-c", "-o", "-MMD", "-MG")]:
        print("release-flavor-check selftest FAIL: the dependency command still compiles", file=sys.stderr)
        return 1
    # The real compiler under -Werror: what the transformation leaves must
    # compile a clean source with no unused-argument complaint.
    cc = next((c for c in ("clang-19", "clang") if shutil.which(c)), None)
    if cc is None:
        print("release-flavor-check selftest: 8 checks passed (no clang for the compiler cases)")
        return 0
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "a.c")
        with open(src, "w", encoding="utf-8") as f:
            f.write("int a(void) { return 0; }\n")
        out = os.path.join(d, "out")
        os.mkdir(out)
        argv = object_compile([cc, "-Wall", "-Werror", "-MMD", "-MP", "-MF" + os.path.join(d, "a.d"),
                               "-MT" + "a.o", "-c", src, "-o" + os.path.join(d, "a.o")],
                              os.path.join(out, "x.o"))
        # deadline: 60 s for one trivial compile.
        r = subprocess.run(argv, cwd=d, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                           timeout=60)
        if r.returncode != 0 or sorted(os.listdir(d)) != ["a.c", "out"] or os.listdir(out) != ["x.o"]:
            print(f"release-flavor-check selftest FAIL: {argv} -> rc {r.returncode}, files "
                  f"{os.listdir(d)}: {r.stderr.strip()}", file=sys.stderr)
            return 1
        # What only code generation rejects: the parser accepts any inline-assembly text.
        with open(src, "w", encoding="utf-8") as f:
            f.write('void a(void) { __asm__ volatile("release_flavor_no_such_insn"); }\n')
        # deadline: 60 s for one trivial compile, each.
        syntax = subprocess.run([cc, "-fsyntax-only", src], cwd=d, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, timeout=60)
        real = subprocess.run(object_compile([cc, "-c", src], os.path.join(out, "y.o")), cwd=d,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=60)
        if syntax.returncode != 0 or real.returncode == 0:
            print(f"release-flavor-check selftest FAIL: an invalid instruction gave syntax rc "
                  f"{syntax.returncode} and compile rc {real.returncode}", file=sys.stderr)
            return 1
    print("release-flavor-check selftest: 10 checks passed")
    return 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    db = work = jobs = None
    files = []
    it = iter(argv)
    for a in it:
        if a == "--db":
            db = next(it, None)
        elif a == "--work":
            work = next(it, None)
        elif a == "--jobs":
            v = next(it, "")
            if not v.isdigit() or int(v) < 1:
                print(f"release-flavor-check: --jobs needs a positive number, not {v!r}", file=sys.stderr)
                return 2
            jobs = int(v)
        elif a.startswith("-"):
            print(f"release-flavor-check: unknown option {a}", file=sys.stderr)
            return 2
        else:
            files.append(a)
    if not db or not work or not os.path.isdir(work):
        print("release-flavor-check: usage: --db PATH --work ROOT FILE...", file=sys.stderr)
        return 2
    return check(db, os.path.realpath(work), files, jobs)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
