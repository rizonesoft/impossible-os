#!/usr/bin/env python3
"""Emit the KERNEL_TESTS "on_only" seam-symbol inventory (TODO-10 release proof).

For every kernel translation unit, compile it TWICE from its compile_commands.json
entry -- once with `-DKERNEL_TESTS` forced, once with `-UKERNEL_TESTS` forced,
identical in every other flag -- and diff the `llvm-nm --defined-only` symbol
sets. A symbol defined only in the `-D` object is a test-surface seam. The union
of those across all TUs is the seam inventory, printed one symbol per line.

Why this is sound and non-tautological: the per-TU objects are derived
INDEPENDENTLY of the release link, so a stale on-flavor object, an unpruned TU,
or a macro-generated seam is still surfaced -- unlike a test-map-minus-release-map
subtraction (absent from the release map by construction) or a `#ifdef` text
scanner (misses macro-generated / guarded-data definitions). Candidate TUs are
NOT filtered by a textual `KERNEL_TESTS` grep: every kernel TU is compiled both
ways, so a future seam whose guard lives only in an included header cannot slip
the inventory.

The caller (scripts/check-release-symbols.sh) intersects this union with the
defined symbols in build/kernel.map and FAILS if the intersection is non-empty.
"""

import argparse
import glob
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

# Basenames whose presence in compile_commands.json legitimately depends on the
# flavor the database was generated at, so they are exempt from the coverage
# check below: the three whole-file-guarded extra test TUs compile to nothing at
# KERNEL_TESTS=off (pruned), and the release-only provenance marker is linked
# ONLY at off. Everything else is an unconditional production TU.
FLAVOR_CONDITIONAL = {
    "ntfs_test.c", "ixfs_test.c", "test_threads.c", "provenance_release.c",
}


def check_db_coverage(cc, repo):
    """Fail-closed on a stale/incomplete compile database (F9): every
    unconditional production kernel .c on disk (src/kernel, src/libc,
    src/desktop; excluding test directories) MUST appear in compile_commands.json.
    A TU missing from a stale DB would silently drop its seams from the PART A
    inventory. Returns the list of uncovered files (empty == complete).

    src/libs is intentionally not checked: only vendored, exclusion-gated TUs
    live there (no KERNEL_TESTS seams), and replicating the Makefile's per-lib
    exclusion list would be fragile. Per-flag drift within a covered TU is out of
    scope (a signed database fingerprint is tracked in section 29)."""
    db_files = {os.path.realpath(e["file"]) for e in cc if e.get("file")}
    missing = []
    for base in ("kernel", "libc", "desktop"):
        root = os.path.join(repo, "src", base)
        for path in glob.glob(os.path.join(root, "**", "*.c"), recursive=True):
            if (os.sep + "test" + os.sep) in path:
                continue
            if os.path.basename(path) in FLAVOR_CONDITIONAL:
                continue
            if os.path.realpath(path) not in db_files:
                missing.append(path)
    return sorted(missing)


def is_kernel_tu(entry, repo):
    """A kernel TU: clang, --target=x86_64-elf, a .c under <repo>/src/, and NOT
    under src/kernel/test/. Test-directory TUs are pruned from the release link
    entirely (Makefile filter-out) and re-proven absent by the link-trace gate
    (check-release-symbols.sh Part B), so they need not enter the symbol diff."""
    args = entry.get("arguments") or []
    if not args:
        return False
    if "clang" not in os.path.basename(args[0]):
        return False
    if not any("x86_64-elf" in a for a in args):
        return False
    f = entry.get("file", "")
    if not f.startswith(os.path.join(repo, "src") + os.sep):
        return False
    if os.sep + os.path.join("src", "kernel", "test") + os.sep in f:
        return False
    return True


def build_cmd(args, out_obj, flavor_flag):
    """Copy the recorded argument vector, redirect its output object to out_obj,
    and append the flavor flag LAST so it wins over the -D/-U that the Makefile
    already baked into the recorded command (CFLAGS is last-wins)."""
    cmd = list(args)
    try:
        oidx = cmd.index("-o")
    except ValueError:
        return None
    if oidx + 1 >= len(cmd):
        return None
    cmd[oidx + 1] = out_obj
    cmd.append(flavor_flag)
    return cmd


def defined_symbols(nm, obj):
    """Return the set of defined symbol names in obj (llvm-nm --defined-only:
    every listed line is a definition; the symbol name is the final field)."""
    res = subprocess.run(
        [nm, "--defined-only", obj],
        capture_output=True, text=True,
    )
    if res.returncode != 0:
        raise RuntimeError(f"nm failed on {obj}: {res.stderr.strip()}")
    names = set()
    for line in res.stdout.splitlines():
        parts = line.split()
        if not parts:
            continue
        name = parts[-1]
        # Skip assembler-temporary local labels (`.L...`): they are excluded
        # from the final symbol table by construction, so they can never appear
        # in kernel.map, and their on/off difference is pure string-literal
        # renumbering noise -- never a real seam. Real seams are named globals
        # or file statics, which are retained here.
        if name.startswith(".L"):
            continue
        names.add(name)
    return names


def process_tu(entry, idx, repo, workdir, nm):
    """Compile one TU on/off and return (on_only_set, error_or_None)."""
    args = entry["arguments"]
    directory = entry.get("directory", repo)
    on_obj = os.path.join(workdir, f"tu{idx}.on.o")
    off_obj = os.path.join(workdir, f"tu{idx}.off.o")
    on_cmd = build_cmd(args, on_obj, "-DKERNEL_TESTS")
    off_cmd = build_cmd(args, off_obj, "-UKERNEL_TESTS")
    if on_cmd is None or off_cmd is None:
        return set(), f"{entry['file']}: no -o in recorded compile command"
    for flavor, cmd in (("on", on_cmd), ("off", off_cmd)):
        res = subprocess.run(cmd, cwd=directory, capture_output=True, text=True)
        if res.returncode != 0:
            return set(), (f"{entry['file']}: compile failed at KERNEL_TESTS={flavor}"
                           f"\n{res.stderr.strip()}")
    try:
        on_defs = defined_symbols(nm, on_obj)
        off_defs = defined_symbols(nm, off_obj)
    except RuntimeError as e:
        return set(), str(e)
    finally:
        for o in (on_obj, off_obj):
            try:
                os.unlink(o)
            except OSError:
                pass
    return on_defs - off_defs, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", required=True, help="compile_commands.json path")
    ap.add_argument("--repo", required=True, help="repository root (absolute)")
    ap.add_argument("--nm", default="llvm-nm-19")
    ap.add_argument("--workdir", required=True, help="scratch dir for temp objects")
    ap.add_argument("--out", required=True, help="write seam inventory here (one symbol/line)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    with open(args.cc) as fh:
        cc = json.load(fh)

    # F9: refuse a stale/incomplete compile database before deriving anything --
    # a missing production TU would silently drop its seams from the inventory.
    uncovered = check_db_coverage(cc, args.repo)
    if uncovered:
        print(f"[seam-inventory] STALE/INCOMPLETE compile_commands.json: "
              f"{len(uncovered)} production TU(s) missing (rebuild clean):",
              file=sys.stderr)
        for f in uncovered[:20]:
            print("  " + f, file=sys.stderr)
        return 4

    tus = [e for e in cc if is_kernel_tu(e, args.repo)]
    if not tus:
        print("[seam-inventory] no kernel TUs found in compile_commands.json",
              file=sys.stderr)
        return 2
    print(f"[seam-inventory] compiling {len(tus)} kernel TUs twice "
          f"(-D/-U KERNEL_TESTS), {args.jobs} jobs", file=sys.stderr)

    on_only = set()
    errors = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(process_tu, e, i, args.repo, args.workdir, args.nm)
                   for i, e in enumerate(tus)]
        for fut in futures:
            syms, err = fut.result()
            if err:
                errors.append(err)
            else:
                on_only |= syms

    if errors:
        print(f"[seam-inventory] {len(errors)} TU(s) failed to compile:",
              file=sys.stderr)
        for e in errors[:20]:
            print("  " + e, file=sys.stderr)
        return 3

    with open(args.out, "w") as fh:
        for sym in sorted(on_only):
            fh.write(sym + "\n")
    print(f"[seam-inventory] {len(on_only)} seam symbols in on_only inventory",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
