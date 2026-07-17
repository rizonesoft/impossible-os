#!/usr/bin/env python3
"""Advisory audit: symbols referenced EXCLUSIVELY by pruned test objects that
still ship in the release kernel.map (release test-surface exclusion).

check-release-symbols.sh PART A proves that no `#ifdef KERNEL_TESTS`-gated seam
leaks, by diffing each production TU compiled -D vs -U KERNEL_TESTS. That diff
is BLIND to a test-only helper defined UNCONDITIONALLY in a production TU
(present in BOTH flavors), because such a symbol never differs between the two
compiles. Section 29 guards the known such helpers so PART A's flavor-diff
covers them going forward; this auditor is the automated net that surfaces a
FUTURE unguarded one -- with no hand-maintained symbol list.

Method (compiler-derived, not a name glob):
  test_refs = union of UNDEFINED symbols across the pruned test objects
              (src/kernel/test/*.o + the extra out-of-tree test TUs), compiled
              as recorded (KERNEL_TESTS=on -- the off compile DB excludes them).
  prod_refs = union of UNDEFINED symbols across every production TU compiled
              -UKERNEL_TESTS (the release flavor).
  candidates = (test_refs - prod_refs) INTERSECT <defined symbols in the
              release kernel.map>.
A candidate is a symbol that (a) some test object references, (b) NO production
TU references, and (c) is nonetheless DEFINED in the shipped image -- i.e. dead
test-support weight, the exact shape section 29's guards remove.

WHY THIS IS ADVISORY, NOT A HARD EMPTY-ASSERT GATE (Codex design review
2026-07-17): an undefined-reference set describes object-file boundaries, not
production intent. It over-reports (a genuine public API that is address-taken
only within its own defining TU -- e.g. an SSDT handler registered in-file -- or
is exported by numeric slot / referenced only by a not-yet-written production
caller shows up as test-only-referenced) and can under-report (a helper reached
only indirectly, or defined-but-unreferenced with no test caller at all, such as
the guarded tpm_evlog_fail_offset). Asserting the raw set empty would therefore
reject legitimate release APIs while still missing some leaks. So this prints a
CANDIDATE inventory for human audit and, by default, does NOT change the release
exit status. Use --strict to make a non-empty inventory fail (only meaningful
once a tree is known-clean and the caller accepts the false-positive risk).
"""

import argparse
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

# Out-of-tree test translation units (pruned at KERNEL_TESTS=off by the Makefile
# filter-out; they live outside src/kernel/test/ under ordinary filenames). This
# is the fallback ONLY: the live set is parsed from the Makefile's
# KERNEL_TESTS_EXTRA_TUS (the single source of truth) at runtime so a fourth
# out-of-tree test TU added there cannot silently drift into `prod_refs` and mask
# a leak (Codex adversarial 2026-07-17 F1).
_FALLBACK_EXTRA_TEST_BASENAMES = frozenset({"ntfs_test.c", "ixfs_test.c",
                                            "test_threads.c"})


def read_extra_test_basenames(repo):
    """Parse KERNEL_TESTS_EXTRA_TUS from the Makefile so the pruned out-of-tree
    test-TU list is single-sourced. Returns the set of basenames; falls back to
    the pinned constant (with a stderr warning) if the Makefile is unreadable or
    the variable is absent, and warns loudly if the parsed set drifts from the
    fallback so a stale fallback is visible."""
    mk = os.path.join(repo, "Makefile")
    try:
        with open(mk) as fh:
            text = fh.read()
    except OSError:
        print(f"[test-ref-audit] WARNING: cannot read {mk}; using pinned "
              f"extra-test-TU fallback {sorted(_FALLBACK_EXTRA_TEST_BASENAMES)}",
              file=sys.stderr)
        return set(_FALLBACK_EXTRA_TEST_BASENAMES)
    # KERNEL_TESTS_EXTRA_TUS := <path> <path> ... (single logical line here; the
    # Makefile defines it on one line -- no continuation handling needed).
    names = set()
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("KERNEL_TESTS_EXTRA_TUS"):
            _, _, rhs = stripped.partition(":=")
            for tok in rhs.split():
                if tok.endswith(".c"):
                    names.add(os.path.basename(tok))
            break
    if not names:
        print("[test-ref-audit] WARNING: KERNEL_TESTS_EXTRA_TUS not found in "
              f"Makefile; using pinned fallback "
              f"{sorted(_FALLBACK_EXTRA_TEST_BASENAMES)}", file=sys.stderr)
        return set(_FALLBACK_EXTRA_TEST_BASENAMES)
    if names != set(_FALLBACK_EXTRA_TEST_BASENAMES):
        print(f"[test-ref-audit] NOTE: Makefile KERNEL_TESTS_EXTRA_TUS = "
              f"{sorted(names)} (pinned fallback {sorted(_FALLBACK_EXTRA_TEST_BASENAMES)}); "
              f"using the Makefile set.", file=sys.stderr)
    return names


def _is_clang_kernel_c(entry, repo):
    args = entry.get("arguments") or []
    if not args or "clang" not in os.path.basename(args[0]):
        return False
    if not any("x86_64-elf" in a for a in args):
        return False
    f = entry.get("file", "")
    return f.startswith(os.path.join(repo, "src") + os.sep)


def is_test_tu(entry, repo, extra_basenames):
    """A pruned test object: under src/kernel/test/, or one of the out-of-tree
    extra test TUs named by the Makefile's KERNEL_TESTS_EXTRA_TUS."""
    if not _is_clang_kernel_c(entry, repo):
        return False
    f = entry.get("file", "")
    if os.sep + os.path.join("src", "kernel", "test") + os.sep in f:
        return True
    return os.path.basename(f) in extra_basenames


def is_production_tu(entry, repo, extra_basenames):
    """A production kernel TU: a clang x86_64-elf .c under src/, NOT a test TU."""
    return (_is_clang_kernel_c(entry, repo)
            and not is_test_tu(entry, repo, extra_basenames))


def _build_cmd(args, out_obj, extra_flag=None):
    cmd = list(args)
    try:
        oidx = cmd.index("-o")
    except ValueError:
        return None
    if oidx + 1 >= len(cmd):
        return None
    cmd[oidx + 1] = out_obj
    if extra_flag:
        cmd.append(extra_flag)  # last-wins over the recorded -D/-U
    return cmd


def _nm_names(nm, obj, mode):
    """mode: '--undefined-only' or '--defined-only'. Returns the set of symbol
    names (last field of each nm line), dropping assembler-local .L labels."""
    res = subprocess.run([nm, mode, obj], capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"nm {mode} failed on {obj}: {res.stderr.strip()}")
    names = set()
    for line in res.stdout.splitlines():
        parts = line.split()
        if not parts:
            continue
        name = parts[-1]
        if name.startswith(".L"):
            continue
        names.add(name)
    return names


def _undef_of(entry, idx, workdir, nm, off):
    """Compile one TU and return (undefined_syms, error_or_None). off=True forces
    -UKERNEL_TESTS (release flavor); off=False compiles as recorded (test flavor)."""
    obj = os.path.join(workdir, f"tu{idx}.o")
    cmd = _build_cmd(entry["arguments"], obj, "-UKERNEL_TESTS" if off else None)
    if cmd is None:
        return set(), f"{entry['file']}: no -o in recorded compile command"
    res = subprocess.run(cmd, cwd=entry.get("directory", "."),
                         capture_output=True, text=True)
    if res.returncode != 0:
        return set(), f"{entry['file']}: compile failed\n{res.stderr.strip()}"
    try:
        return _nm_names(nm, obj, "--undefined-only"), None
    except RuntimeError as e:
        return set(), str(e)
    finally:
        try:
            os.unlink(obj)
        except OSError:
            pass


def _map_defined(map_path):
    """Defined symbol names in an llvm-nm -n kernel.map (3 fields, type != U)."""
    defined = set()
    with open(map_path) as fh:
        for line in fh:
            parts = line.split()
            if len(parts) == 3 and parts[1] not in ("U", "u"):
                defined.add(parts[2])
    return defined


def _union_undef(tus, workdir, nm, off, jobs):
    acc, errors = set(), []
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        futs = [pool.submit(_undef_of, e, i, workdir, nm, off)
                for i, e in enumerate(tus)]
        for fut in futs:
            syms, err = fut.result()
            if err:
                errors.append(err)
            else:
                acc |= syms
    return acc, errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", required=True,
                    help="ON-flavor compile_commands.json (must include test TUs)")
    ap.add_argument("--map", required=True, help="release (off-flavor) kernel.map")
    ap.add_argument("--repo", required=True, help="repository root (absolute)")
    ap.add_argument("--nm", default="llvm-nm-19")
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--out", help="write the candidate inventory here (one/line)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 when the candidate inventory is non-empty "
                         "(default: advisory, exit 0)")
    args = ap.parse_args()

    with open(args.cc) as fh:
        cc = json.load(fh)
    repo = os.path.abspath(args.repo)
    extra = read_extra_test_basenames(repo)

    test_tus = [e for e in cc if is_test_tu(e, repo, extra)]
    prod_tus = [e for e in cc if is_production_tu(e, repo, extra)]
    if not test_tus:
        print("[test-ref-audit] ADVISORY SKIP: the compile DB carries no test "
              "objects -- pass an ON-flavor (KERNEL_TESTS=on) compile_commands.json "
              "(the release/off DB prunes them).", file=sys.stderr)
        return 0
    if not prod_tus:
        print("[test-ref-audit] no production TUs in the compile DB", file=sys.stderr)
        return 2
    if not os.path.isfile(args.map):
        print(f"[test-ref-audit] missing release map: {args.map}", file=sys.stderr)
        return 2

    print(f"[test-ref-audit] {len(test_tus)} test TUs (as-recorded) + "
          f"{len(prod_tus)} production TUs (-UKERNEL_TESTS), {args.jobs} jobs",
          file=sys.stderr)

    test_refs, terr = _union_undef(test_tus, args.workdir, args.nm, False, args.jobs)
    prod_refs, perr = _union_undef(prod_tus, args.workdir, args.nm, True, args.jobs)
    errors = terr + perr
    if errors:
        print(f"[test-ref-audit] {len(errors)} TU(s) failed to compile:",
              file=sys.stderr)
        for e in errors[:20]:
            print("  " + e, file=sys.stderr)
        return 3

    shipped = _map_defined(args.map)
    if not shipped:
        print(f"[test-ref-audit] no defined symbols parsed from {args.map}",
              file=sys.stderr)
        return 2

    candidates = sorted((test_refs - prod_refs) & shipped)

    if args.out:
        with open(args.out, "w") as fh:
            for s in candidates:
                fh.write(s + "\n")

    if not candidates:
        print("[test-ref-audit] PASS: no shipped symbol is referenced only by "
              "pruned test objects (no unguarded test-only helper leaked).",
              file=sys.stderr)
        return 0

    where = f" (full list: {args.out})" if args.out else ""
    print(f"[test-ref-audit] ADVISORY: {len(candidates)} shipped symbol(s) are "
          f"referenced only by pruned test objects{where}. Audit each; guard a "
          f"genuine test-only helper with #ifdef KERNEL_TESTS. This set OVER-reports "
          f"by design -- a legitimate public API address-taken only in its own TU, "
          f"exported by numeric slot, or awaiting a production caller looks "
          f"test-only -- so it is never a release-blocking gate. Sample:",
          file=sys.stderr)
    for s in candidates[:12]:
        print("    " + s, file=sys.stderr)
    if len(candidates) > 12:
        print(f"    ... and {len(candidates) - 12} more", file=sys.stderr)
    return 1 if args.strict else 0


if __name__ == "__main__":
    sys.exit(main())
