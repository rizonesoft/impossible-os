#!/usr/bin/env python3
"""Write a compile database from a dry run of the build.

    python3 scripts/gen-compile-db.py [--root DIR] [--output PATH] [--selftest]

`make -n -B` prints every compile command the build would run without running
any, in about a second. That gives a database that is always current: it lists
a source the moment the Makefile compiles it, covers the loader (built by a
sub-make) and covers the release-only sources, none of which a database
recorded during the last clean build does.

The test flavor is read first and wins; the release flavor adds the sources
only it compiles. --root reads another tree (a snapshot of a commit) with
that tree's own Makefiles. Exit 0 written, 2 the build could not be read.
"""
import json
import os
import re
import shlex
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGETS = ["kernel", "uefi-boot"]
# (label, extra make arguments); the first flavor to compile a source wins.
FLAVORS = [("test", []), ("release", ["KERNEL_TESTS=off"])]
COMPILER = re.compile(r"^(clang|gcc|cc)(-[0-9]+)?$")
ENTER = re.compile(r"^make(\[\d+\])?: Entering directory '(.*)'$")
LEAVE = re.compile(r"^make(\[\d+\])?: Leaving directory '(.*)'$")
# Options whose next word is a value, never the source being compiled.
VALUE_OPTS = {"-o", "-MF", "-MT", "-MQ", "-include", "-I", "-isystem", "-D", "-x", "--target"}


def logical_lines(text):
    """Dry-run output with backslash continuations joined."""
    pending = ""
    for raw in text.splitlines():
        if raw.endswith("\\"):
            pending += raw[:-1] + " "
            continue
        yield pending + raw
        pending = ""
    if pending:
        yield pending


def parse(text, top):
    """Compile entries from dry-run output; `top` is where make was started."""
    stack = [top]
    out = []
    for line in logical_lines(text):
        m = ENTER.match(line)
        if m:
            stack.append(m.group(2))
            continue
        if LEAVE.match(line):
            if len(stack) > 1:
                stack.pop()
            continue
        if " -c " not in line:
            continue
        try:
            argv = shlex.split(line)
        except ValueError:
            continue
        if not argv or not COMPILER.match(os.path.basename(argv[0])) or "-c" not in argv:
            continue
        sources, output, skip = [], None, False
        for i, a in enumerate(argv[1:], 1):
            if skip:
                skip = False
                continue
            if a in VALUE_OPTS:
                if a == "-o" and i + 1 < len(argv):
                    output = argv[i + 1]
                skip = True
                continue
            if a.endswith(".c") and not a.startswith("-"):
                sources.append(a)
        if len(sources) != 1:
            continue
        cwd = stack[-1]
        entry = {"directory": cwd, "file": os.path.normpath(os.path.join(cwd, sources[0])),
                 "arguments": argv}
        if output:
            entry["output"] = output
        out.append(entry)
    return out


def dry_run(extra, root):
    env = dict(os.environ, LC_ALL="C")
    env.pop("MAKEFLAGS", None)
    env.pop("MFLAGS", None)
    try:
        r = subprocess.run(["make", "-n", "-B", "-w"] + extra + TARGETS, cwd=root, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                           timeout=120)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise SystemExit(f"gen-compile-db: could not run make: {e}") from e
    if r.returncode != 0:
        tail = "\n".join(r.stderr.strip().splitlines()[-5:])
        raise SystemExit(f"gen-compile-db: make -n failed ({r.returncode})\n{tail}")
    return parse(r.stdout, root)


def build(root, run=dry_run):
    """Both flavours merged: the first flavour to compile a source keeps it."""
    seen, db = set(), []
    for _label, extra in FLAVORS:
        for e in run(extra, root):
            if e["file"] not in seen:
                seen.add(e["file"])
                db.append(e)
    return db


def selftest():
    text = "\n".join([
        "make: Entering directory '/r'",
        "mkdir -p build/kernel",
        "clang-19 -Iinclude -DNAME='\"a b\"' -c src/kernel/a.c -o build/kernel/a.o",
        "clang-19 -Iinclude -I src/x \\",
        "      -D__X__ -c src/kernel/b.c -o build/kernel/b.o",
        "echo \"[CC] src/kernel/a.c\"",
        "nasm -f elf64 src/kernel/c.asm -o build/kernel/c.o",
        "ld.lld-19 -o build/kernel.exe build/kernel/a.o",
        "make[1]: Entering directory '/r/src/boot/uefi'",
        "clang-19 -I. -I../../../build -MMD -MP -c bootx64.c -o bootx64.o",
        "make[1]: Leaving directory '/r/src/boot/uefi'",
        "/usr/bin/gcc -c -O2 -I include -o build/tools/t tools/t/t.c",
        "clang-19 -c one.c two.c",
        "echo unterminated ' -c x.c",
        "make: Leaving directory '/r'",
    ])
    got = [(e["directory"], e["file"], e.get("output")) for e in parse(text, "/r")]
    want = [
        ("/r", "/r/src/kernel/a.c", "build/kernel/a.o"),
        ("/r", "/r/src/kernel/b.c", "build/kernel/b.o"),
        ("/r/src/boot/uefi", "/r/src/boot/uefi/bootx64.c", "bootx64.o"),
        ("/r", "/r/tools/t/t.c", "build/tools/t"),
    ]
    if got != want:
        print("gen-compile-db selftest FAIL", file=sys.stderr)
        for g in got:
            print("  got ", g, file=sys.stderr)
        return 1
    args = parse(text, "/r")[0]["arguments"]
    if "-DNAME=\"a b\"" not in args:
        print("gen-compile-db selftest FAIL: quoted define lost", file=sys.stderr)
        return 1
    # The merge: a source both flavours compile keeps the first flavour's
    # command, and a source only one flavour compiles is still present.
    first = FLAVORS[0][1]

    def fake(extra, _root):
        tag = "test" if extra == first else "release"
        only = "/r/t_only.c" if tag == "test" else "/r/r_only.c"
        return [{"directory": "/r", "file": "/r/shared.c", "arguments": [tag]},
                {"directory": "/r", "file": only, "arguments": [tag]}]

    merged = [(e["file"], e["arguments"][0]) for e in build("/r", fake)]
    if merged != [("/r/shared.c", "test"), ("/r/t_only.c", "test"), ("/r/r_only.c", "release")]:
        print(f"gen-compile-db selftest FAIL: flavour merge {merged}", file=sys.stderr)
        return 1
    print("gen-compile-db selftest: 6 checks passed")
    return 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    root = REPO
    if "--root" in argv:
        i = argv.index("--root")
        if i + 1 >= len(argv) or not os.path.isdir(argv[i + 1]):
            print("gen-compile-db: --root needs a directory", file=sys.stderr)
            return 2
        root = os.path.realpath(argv[i + 1])
    out = os.path.join(root, "build", "static-analysis", "compile_commands.json")
    if "--output" in argv:
        i = argv.index("--output")
        if i + 1 >= len(argv):
            print("gen-compile-db: --output needs a path", file=sys.stderr)
            return 2
        out = argv[i + 1]
    db = build(root)
    files = {e["file"] for e in db}
    # A database with no kernel or no loader source is a misread build, not a result.
    for must in ("src/kernel/mm/pmm.c", "src/boot/uefi/bootx64.c"):
        if os.path.join(root, must) not in files:
            print(f"gen-compile-db: the dry run did not compile {must}; refusing to write",
                  file=sys.stderr)
            return 2
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    tmp = out + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(db, f, indent=1)
    os.replace(tmp, out)
    print(f"gen-compile-db: {len(db)} compile commands -> {out}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except SystemExit as e:
        if isinstance(e.code, str):
            print(e.code, file=sys.stderr)
            sys.exit(2)
        raise
