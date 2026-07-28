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
OUT_KERNEL_H = os.path.join(REPO_ROOT, 'include', 'kernel', 'abi_hash.h')

# TEB + KUSD offsets fed into the FNV-1a ABI hash. The entries must
# match the `_Static_assert(__builtin_offsetof(...) == <literal>)`
# lines in include/kernel/ob/teb.h + include/kernel/nt/kusd.h. Every
# offset that has a compile-time assertion on the kernel side belongs
# here so a silent drift in ANY asserted field changes the hash --
# otherwise an attacker-or-bug-driven reorder of a field OUTSIDE this
# list still passes the runtime handshake.
#
# Check: parse_kernel_asserts() below validates at generator runtime
# that every _Static_assert on the kernel side has a matching entry
# in these lists. Adding a new kernel-side assertion without updating
# this list fails the generator (and thus `make check-abi`) with a
# named error -- Codex HF 2026-04-22 "partial coverage" fix.
TEB_LAYOUT = [
    ('NtTib',                            0x0000),
    ('NtTib.Self',                       0x0030),
    ('EnvironmentPointer',               0x0038),
    ('ClientId',                         0x0040),
    ('ActiveRpcHandle',                  0x0050),
    ('ThreadLocalStoragePointer',        0x0058),
    ('ProcessEnvironmentBlock',          0x0060),
    ('LastErrorValue',                   0x0068),
    ('CountOfOwnedCriticalSections',     0x006C),
    ('TlsSlots',                         0x1480),
    ('TlsExpansionSlots',                0x1780),
]
KUSD_LAYOUT = [
    ('TickCountMultiplier',              0x004),
    ('InterruptTime',                    0x008),
    ('SystemTime',                       0x014),
    ('TimeZoneBias',                     0x020),
    ('NtSystemRoot',                     0x030),
    ('NtBuildNumber',                    0x260),
    ('NtProductType',                    0x264),
    ('NtMajorVersion',                   0x26C),
    ('NtMinorVersion',                   0x270),
    ('ProcessorFeatures',                0x274),
    ('NumberOfPhysicalPages',            0x2E8),
    ('QpcFrequency',                     0x300),
    ('SystemCall',                       0x308),
    ('TickCount',                        0x320),
    ('Cookie',                           0x330),
    ('AbiMagic',                         0x340),
    ('AbiVersion',                       0x344),
    ('AbiStructSize',                    0x346),
    ('AbiLayoutHash',                    0x348),
    ('AbiBuildTimestamp',                0x350),
]

TEB_HEADER = os.path.join(REPO_ROOT, 'include', 'kernel', 'ob', 'teb.h')
KUSD_HEADER = os.path.join(REPO_ROOT, 'include', 'kernel', 'nt', 'kusd.h')

# Extract `_Static_assert(__builtin_offsetof(STRUCT, FIELD) == LITERAL, ...)`
# so we can verify that every kernel assertion has a matching hash entry.
# The struct name is the first group-1 capture; the field (possibly with
# dotted subfield access) is group-2; the literal hex offset is group-3.
STATIC_ASSERT_RE = re.compile(
    r'_Static_assert\s*\(\s*__builtin_offsetof\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*'
    r'([A-Za-z_][A-Za-z0-9_.\[\]]*)\s*\)\s*==\s*(0x[0-9A-Fa-f]+|[0-9]+)'
)


def parse_kernel_asserts(path: str, struct_name: str) -> list[tuple[str, int]]:
    """Return [(field, offset)] for every _Static_assert in `path`
    whose struct name matches `struct_name`. Order preserved."""
    out: list[tuple[str, int]] = []
    with open(path, 'r', encoding='utf-8') as f:
        text = f.read()
    for m in STATIC_ASSERT_RE.finditer(text):
        if m.group(1) != struct_name:
            continue
        field = m.group(2)
        off = int(m.group(3), 0)
        out.append((field, off))
    return out


def check_coverage(layout: list[tuple[str, int]], asserts: list[tuple[str, int]],
                   struct_name: str) -> list[str]:
    """Every kernel-side assertion must map to a hash-input entry at
    the same offset. Name format differs slightly (kernel uses dotted
    subfield syntax `NtTib.Self`; hash list uses the same) so we
    compare by (field, offset) tuple. Returns list of human-readable
    errors; empty list means coverage is complete."""
    layout_map = {name: off for name, off in layout}
    errors: list[str] = []
    for field, off in asserts:
        if field not in layout_map:
            errors.append(f'{struct_name}.{field} (offset {off:#x}) has a kernel '
                          f'_Static_assert but no hash entry in gen-user-abi.py')
            continue
        if layout_map[field] != off:
            errors.append(f'{struct_name}.{field}: hash offset {layout_map[field]:#x} '
                          f'disagrees with kernel _Static_assert {off:#x}')
    return errors

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


def render(syscalls, ssdt, ntstatus, abi_hash: int) -> str:
    hash_block = (
        '/* ---- ABI fingerprint (FNV-1a 64-bit) --------------------------------------------------------- */\n'
        '/* Hash over the sorted tuple of (SYS_*, SSDT_*, FAULT_*, TEB offsets,\n'
        ' * KUSD offsets). Kernel emits the same hash via SYS_ABI_HANDSHAKE; user\n'
        ' * crt0 calls that syscall and aborts with EX_ABI_MISMATCH = 0x42 on\n'
        ' * disagreement. A kernel-side renumber that slipped through review\n'
        ' * but skipped this generator surfaces at process start, not at the\n'
        ' * first syscall with corrupted semantics. */\n'
        f'#define IMPOSSIBLE_OS_ABI_HASH   0x{abi_hash:016X}ULL\n'
        '#define EX_ABI_MISMATCH          0x42\n'
    )
    return HEADER_TEMPLATE.format(
        syscall_block=format_block('INT 0x80 syscall numbers (SYS_*, FAULT_*)', syscalls, 24),
        ssdt_block=format_block('SSDT service numbers (SSDT_*) -- fastpath probe', ssdt, 32),
        ntstatus_block=format_block('NTSTATUS return codes used by user probes', ntstatus, 32),
    ) + hash_block


# ---- FNV-1a 64-bit hash --------------------------------------------------

FNV_OFFSET_BASIS = 0xCBF29CE484222325
FNV_PRIME        = 0x00000100000001B3
FNV_MASK_64      = (1 << 64) - 1


def fnv1a_update(h: int, data: bytes) -> int:
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & FNV_MASK_64
    return h


def compute_abi_hash(syscalls, ssdt, teb_layout, kusd_layout) -> int:
    """Deterministic FNV-1a 64-bit fingerprint over the load-bearing
    ABI surface. Sorted tuples keep the hash stable across reorderings
    in the source headers -- only name/value changes shift the hash.

    Tuple shape: each input yields `name\\0<hex-value>\\n` bytes. SSDT
    entries use the FULL kernel allowlist of SSDT names (not the user
    allowlist), so a user-invisible SSDT renumber also flags the hash.
    """
    h = FNV_OFFSET_BASIS
    for name, _value, literal in sorted(syscalls, key=lambda e: e[0]):
        h = fnv1a_update(h, f'SYS\0{name}\0{literal}\n'.encode('ascii'))
    for name, _value, literal in sorted(ssdt, key=lambda e: e[0]):
        h = fnv1a_update(h, f'SSDT\0{name}\0{literal}\n'.encode('ascii'))
    for name, off in sorted(teb_layout, key=lambda e: e[0]):
        h = fnv1a_update(h, f'TEB\0{name}\0{off:#x}\n'.encode('ascii'))
    for name, off in sorted(kusd_layout, key=lambda e: e[0]):
        h = fnv1a_update(h, f'KUSD\0{name}\0{off:#x}\n'.encode('ascii'))
    return h


KERNEL_HEADER_TEMPLATE = '''\
/* ============================================================================
 * abi_hash.h -- Kernel ABI fingerprint (GENERATED)
 *
 * Regenerate via: python3 scripts/gen-user-abi.py
 * Verified via:   make check-abi
 *
 * DO NOT EDIT BY HAND. Derived from the same sources as
 * user/include/abi_numbers.h -- the kernel-side copy is needed so
 * SYS_ABI_HANDSHAKE can return the same 64-bit hash the user libc
 * has compiled in. A drift between the two headers is a build error,
 * not a runtime surprise.
 * ============================================================================ */

#pragma once

#define IMPOSSIBLE_OS_ABI_HASH   0x{abi_hash:016X}ULL
'''


def render_kernel_hash_header(abi_hash: int) -> str:
    return KERNEL_HEADER_TEMPLATE.format(abi_hash=abi_hash)


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
                    help='verify committed abi headers match kernel source; exit 1 on drift')
    args = ap.parse_args()

    syscalls = parse_defines(SYSCALL_H, ('SYS_', 'FAULT_'))
    ssdt_all = parse_defines(SSDT_H, 'SSDT_')
    ssdt = [e for e in ssdt_all if e[0] in SSDT_USER_ALLOWLIST]
    ntstatus_all = parse_defines(NTSTATUS_H, 'STATUS_')
    ntstatus = [e for e in ntstatus_all if e[0] in NTSTATUS_USER_ALLOWLIST]

    # Coverage check: every kernel-side _Static_assert on TEB/KUSD
    # offsets must have a matching entry in TEB_LAYOUT/KUSD_LAYOUT.
    # Closes the "hand-picked subset" gap Codex flagged 2026-04-22:
    # without this, an offset outside the hand-coded lists could
    # silently drift and the runtime handshake would still succeed.
    teb_errs = check_coverage(TEB_LAYOUT,
                              parse_kernel_asserts(TEB_HEADER, 'TEB'), 'TEB')
    kusd_errs = check_coverage(KUSD_LAYOUT,
                               parse_kernel_asserts(KUSD_HEADER, 'KUSER_SHARED_DATA'),
                               'KUSER_SHARED_DATA')
    if teb_errs or kusd_errs:
        for e in teb_errs + kusd_errs:
            print(f'gen-user-abi: coverage: {e}', file=sys.stderr)
        print('               a kernel _Static_assert on offset without a matching',
              file=sys.stderr)
        print('               entry in TEB_LAYOUT/KUSD_LAYOUT would hash-hole that',
              file=sys.stderr)
        print('               field. Add the missing entries.',
              file=sys.stderr)
        return 1

    # Hash over the FULL SSDT registry (not just the user allowlist) so
    # any renumber in the kernel table bumps the fingerprint even when
    # user-visible names are unchanged.
    abi_hash = compute_abi_hash(syscalls, ssdt_all, TEB_LAYOUT, KUSD_LAYOUT)

    rendered_user = render(syscalls, ssdt, ntstatus, abi_hash)
    rendered_kernel = render_kernel_hash_header(abi_hash)

    if args.check:
        drift = False
        for path, rendered in ((OUT_H, rendered_user), (OUT_KERNEL_H, rendered_kernel)):
            try:
                with open(path, 'r', encoding='utf-8') as f:
                    existing = f.read()
            except FileNotFoundError:
                print(f'gen-user-abi: {path} missing -- run `python3 scripts/gen-user-abi.py`',
                      file=sys.stderr)
                drift = True
                continue
            if existing != rendered:
                print(f'gen-user-abi: {path} is stale vs kernel source',
                      file=sys.stderr)
                drift = True
        if drift:
            print('                run `python3 scripts/gen-user-abi.py` to regenerate',
                  file=sys.stderr)
            return 1
        return 0

    for path, rendered in ((OUT_H, rendered_user), (OUT_KERNEL_H, rendered_kernel)):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w', encoding='utf-8') as f:
            f.write(rendered)
    print(f'gen-user-abi: wrote {OUT_H} + {OUT_KERNEL_H} '
          f'(syscalls={len(syscalls)} ssdt={len(ssdt)} ntstatus={len(ntstatus)} '
          f'hash=0x{abi_hash:016X})')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
