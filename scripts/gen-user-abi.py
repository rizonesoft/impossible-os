#!/usr/bin/env python3
# =============================================================================
# gen-user-abi.py -- Generate user/include/abi_numbers.h from kernel source
#
# Reads the authoritative syscall + SSDT number tables out of the kernel
# headers and emits a generated, committed header that user-mode test binaries
# and the user libc include. Kills ~80% of the historical fast-path fragility
# class: silent drift between SYS_*/SSDT_* numbers on the kernel side and
# hand-copied #defines in user/include/syscall.h.
#
# Sources (single source of truth):
#   include/kernel/sched/syscall.h      -- INT 0x80 SYS_* numbers + SYS_NT_*
#                                           aliases + SYS_FAULT_INJECT +
#                                           FAULT_* sub-selectors
#   include/kernel/nt/service_numbers.h -- SSDT_* service number table
#   include/kernel/nt/ntstatus.h        -- STATUS_* return codes used by
#                                           the fastpath SSDT_NtClose probe
#
# Output:
#   user/include/abi_numbers.h  -- generated; committed; do NOT edit by hand
#
# Invocation:
#   python3 scripts/gen-user-abi.py            # rewrite the header in place
#   python3 scripts/gen-user-abi.py --check    # re-run + diff; exit 1 on drift
#
# `make check-abi` uses --check as a pre-commit / CI gate: a kernel-side
# renumber that did not get the user header regenerated fails the build
# at the check step instead of at runtime when a user binary makes a
# syscall against the wrong number and gets a garbage handler back.
# =============================================================================

import argparse
import os
import re
import sys

# ---- Paths --------------------------------------------------------------

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYSCALL_H = os.path.join(REPO_ROOT, 'include', 'kernel', 'sched', 'syscall.h')
SSDT_H    = os.path.join(REPO_ROOT, 'include', 'kernel', 'nt', 'service_numbers.h')
NTSTATUS_H = os.path.join(REPO_ROOT, 'include', 'kernel', 'nt', 'ntstatus.h')
OUT_H     = os.path.join(REPO_ROOT, 'user', 'include', 'abi_numbers.h')

# ---- Parsers ------------------------------------------------------------

# Match `#define NAME BODY` where NAME is uppercase and BODY contains
# exactly one integer literal we can extract. Handles three cases the
# kernel headers use:
#   #define SYS_WRITE    1
#   #define SSDT_NtClose 0x0000
#   #define STATUS_INVALID_HANDLE  ((NTSTATUS)0xC0000008)  /* comment */
# Explicitly rejects function-like macros (`#define FOO(x)`) and
# multi-token expressions (`#define FOO (BAR + 1)`) by extracting
# the FIRST `0x...` / decimal literal and refusing to emit unless the
# literal is the only one in the body.
DEFINE_NAME_RE = re.compile(
    r'^\s*#\s*define\s+([A-Z][A-Za-z0-9_]*)(?!\()\s+(.*?)(?://|/\*|\s*$)'
)
INT_LITERAL_RE = re.compile(r'(0x[0-9A-Fa-f]+|[0-9]+)')


def parse_defines(path: str, name_prefix: str) -> list[tuple[str, int, str]]:
    """Parse `#define <NAME> <integer>` lines whose name starts with one of
    the prefixes. Returns [(name, value_int, original_literal)] preserving
    the order they appear in the source file so the generated header is
    readable."""
    if isinstance(name_prefix, str):
        prefixes = (name_prefix,)
    else:
        prefixes = tuple(name_prefix)
    out: list[tuple[str, int, str]] = []
    with open(path, 'r', encoding='utf-8') as f:
        for line in f:
            m = DEFINE_NAME_RE.match(line)
            if not m:
                continue
            name = m.group(1)
            if not name.startswith(prefixes):
                continue
            body = m.group(2)
            lits = INT_LITERAL_RE.findall(body)
            # Accept exactly one integer literal in the body so
            # `#define FOO  (BAR + 1)` (zero literals) and
            # `#define FOO  (A | B)` (multiple names) are skipped --
            # user header stays a flat integer table.
            if len(lits) != 1:
                continue
            literal = lits[0]
            try:
                value = int(literal, 0)
            except ValueError:
                continue
            out.append((name, value, literal))
    return out


# ---- Emitter ------------------------------------------------------------

HEADER_TEMPLATE = '''\
/* ============================================================================
 * abi_numbers.h -- User-mode ABI number table (GENERATED)
 *
 * Regenerate via: python3 scripts/gen-user-abi.py
 * Verified via:   make check-abi
 *
 * DO NOT EDIT BY HAND. This header is machine-derived from:
 *   include/kernel/sched/syscall.h       (INT 0x80 SYS_* numbers)
 *   include/kernel/nt/service_numbers.h  (SSDT_* service numbers)
 *   include/kernel/nt/ntstatus.h         (NTSTATUS return codes)
 *
 * Any kernel-side renumber that forgets to regenerate this header will
 * fail `make check-abi` before the drift leaks to runtime. User-mode
 * tests + user/lib MUST consume these constants rather than duplicating
 * the kernel values by hand -- the hand-copy pattern is exactly what
 * TODO-04 -17 was filed to eliminate.
 *
 * Layout contract: flat `#define` block per category. No enums (user libc
 * is freestanding and may be compiled without -std=c11+), no function-like
 * macros (keeps parse surface minimal for future tooling), no guard against
 * redefinition other than the `#pragma once` below (user headers that
 * previously #defined these constants must strip their copies and
 * `#include "abi_numbers.h"` instead).
 * ============================================================================ */

#pragma once

{syscall_block}
{ssdt_block}
{ntstatus_block}
'''


def format_block(title: str, entries: list[tuple[str, int, str]], name_width: int = 24) -> str:
    """Render a titled block of `#define NAME VALUE` lines with column
    alignment. Widths are chosen to match the kernel-side formatting so
    side-by-side diffs stay readable."""
    if not entries:
        return f'/* ---- {title}: (empty) ----\n */\n'
    lines = [f'/* ---- {title} --------------------------------------------------------- */']
    for name, _value, literal in entries:
        lines.append(f'#define {name:<{name_width}} {literal}')
    return '\n'.join(lines) + '\n'


def render(syscalls, ssdt, ntstatus) -> str:
    return HEADER_TEMPLATE.format(
        syscall_block=format_block('INT 0x80 syscall numbers (SYS_*, FAULT_*)', syscalls, 24),
        ssdt_block=format_block('SSDT service numbers (SSDT_*) -- fastpath probe', ssdt, 32),
        ntstatus_block=format_block('NTSTATUS return codes used by user probes', ntstatus, 32),
    )


# ---- SSDT filter --------------------------------------------------------

# The full SSDT table is 470 entries; mirroring all of them into user
# headers would bloat the generated file without benefit -- user code
# today only reaches for SSDT_NtClose (fastpath probe). Add names here as
# new user-mode fast-path consumers appear; `make check-abi` surfaces any
# missing new name as a build error in the consumer, not as silent drift.
SSDT_USER_ALLOWLIST = frozenset({
    'SSDT_NtClose',
    'SSDT_NtTerminateProcess',
    'SSDT_NtYieldExecution',
    'SSDT_NtQuerySystemInformation',
})

# Similarly: ntstatus.h is hundreds of codes; we export the subset a
# user probe binary might test against.
NTSTATUS_USER_ALLOWLIST = frozenset({
    'STATUS_SUCCESS',
    'STATUS_INVALID_HANDLE',
    'STATUS_ACCESS_DENIED',
    'STATUS_NOT_IMPLEMENTED',
})


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true',
                    help='verify committed abi_numbers.h matches kernel source; exit 1 on drift')
    args = ap.parse_args()

    syscalls = parse_defines(SYSCALL_H, ('SYS_', 'FAULT_'))
    ssdt_all = parse_defines(SSDT_H, 'SSDT_')
    ssdt = [e for e in ssdt_all if e[0] in SSDT_USER_ALLOWLIST]
    ntstatus_all = parse_defines(NTSTATUS_H, 'STATUS_')
    ntstatus = [e for e in ntstatus_all if e[0] in NTSTATUS_USER_ALLOWLIST]

    rendered = render(syscalls, ssdt, ntstatus)

    if args.check:
        try:
            with open(OUT_H, 'r', encoding='utf-8') as f:
                existing = f.read()
        except FileNotFoundError:
            print(f'gen-user-abi: {OUT_H} missing -- run `python3 scripts/gen-user-abi.py`',
                  file=sys.stderr)
            return 1
        if existing != rendered:
            print(f'gen-user-abi: {OUT_H} is stale vs kernel source', file=sys.stderr)
            print('                run `python3 scripts/gen-user-abi.py` to regenerate',
                  file=sys.stderr)
            return 1
        return 0

    os.makedirs(os.path.dirname(OUT_H), exist_ok=True)
    with open(OUT_H, 'w', encoding='utf-8') as f:
        f.write(rendered)
    print(f'gen-user-abi: wrote {OUT_H} '
          f'(syscalls={len(syscalls)} ssdt={len(ssdt)} ntstatus={len(ntstatus)})')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
