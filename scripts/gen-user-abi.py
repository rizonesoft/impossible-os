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
#   include/kernel/sched/task.h         -- allowlisted TASK_EXIT_* exit-status
#                                           constants a ring-3 test asserts on
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
import tempfile

# ---- Paths --------------------------------------------------------------

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYSCALL_H = os.path.join(REPO_ROOT, 'include', 'kernel', 'sched', 'syscall.h')
SSDT_H    = os.path.join(REPO_ROOT, 'include', 'kernel', 'nt', 'service_numbers.h')
NTSTATUS_H = os.path.join(REPO_ROOT, 'include', 'kernel', 'nt', 'ntstatus.h')
TASK_H    = os.path.join(REPO_ROOT, 'include', 'kernel', 'sched', 'task.h')
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


# ---- Signed / expression-valued constants -------------------------------
#
# parse_defines() above extracts the first UNSIGNED integer literal out of a
# define body, which is correct for every table it reads (SYS_*, FAULT_*,
# SSDT_*, STATUS_* are all non-negative). It is actively WRONG for the kernel
# exit-status block: `(-1000)` would resolve to +1000 and
# `(TASK_EXIT_REASON_BASE - 1)` to +1 -- silently incorrect values rather than
# omissions, which is a worse failure than the hand-copy drift this exists to
# close. Exit statuses therefore get their own parser, their own restricted
# expression grammar, and a fail-closed contract.

# `#define NAME BODY` over a COMMENT-STRIPPED line, so BODY is the whole
# logical body. Matching the raw line and stopping at the first `/*` would
# truncate `(BASE - 1) /* why */ + 2` to `(BASE - 1)` and resolve it to -1001
# while the kernel compiles -999 -- and both generated headers plus the ABI
# hash would then agree on a value ring 0 never uses. Function-like macros are
# rejected by the `(?!\()` guard, same as DEFINE_NAME_RE.
EXIT_DEFINE_RE = re.compile(
    r'^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)(?!\()\s+(.*)$'
)

# Exit statuses travel as int32_t (`void task_exit(int32_t status)`), so a
# value the resolver can compute in Python's unbounded integers but C would
# truncate on the way to ring 3 is a wrong answer wearing a certificate.
INT32_MIN = -(1 << 31)
INT32_MAX = (1 << 31) - 1

# The ENTIRE expression grammar the generator will resolve: a symbol, a symbol
# with one integer offset, or a signed integer literal. Anything else is a
# build failure by design -- a generator that guesses at C expressions would
# reintroduce the silent-wrong-value class this block exists to prevent.
EXIT_EXPR_RE = re.compile(
    r'^([A-Za-z_][A-Za-z0-9_]*|[+-]?(?:0x[0-9A-Fa-f]+|[0-9]+))'
    r'(?:\s*([+-])\s*(0x[0-9A-Fa-f]+|[0-9]+))?$'
)

COND_OPEN_RE  = re.compile(r'^\s*#\s*(?:if|ifdef|ifndef)\b')
COND_CLOSE_RE = re.compile(r'^\s*#\s*endif\b')


def splice_logical_lines(text: str) -> list:
    """Join backslash-newline continuations into LOGICAL lines, as C
    translation phase 2 does. Returns [(lineno, logical_line)] where lineno is
    the physical line the logical line STARTS on, so diagnostics still point
    somewhere useful.

    This runs BEFORE comment stripping and BEFORE directive matching, and the
    order is load-bearing rather than cosmetic. C splices first, so

        // trailing comment \\
        #endif

    puts that `#endif` INSIDE the comment; a scanner working on physical lines
    sees a real directive and closes a conditional that is actually still open,
    then certifies the enclosed define as unconditional. Verified against
    clang-19 -E: for exactly that shape the compiler leaves the constant
    UNDEFINED while a physical-line scanner resolved it to a concrete value.
    The same phase also makes `X_STA\\` + `TUS` one identifier, which a
    physical-line scanner never sees as a definition at all."""
    out = []
    buf: list = []
    start = None
    for lineno, raw in enumerate(text.splitlines(), 1):
        if start is None:
            start = lineno
        # clang splices a backslash followed by TRAILING WHITESPACE too (it
        # warns, but it splices), so matching only a FINAL `\` reopens the very
        # bypass this function exists to close: `// c \   ` still absorbs the
        # next line's `#endif`.
        trimmed = raw.rstrip()
        if trimmed.endswith('\\'):
            # The backslash-newline pair is DELETED, not replaced by a space --
            # that is what lets a split identifier rejoin.
            buf.append(trimmed[:-1])
            continue
        buf.append(raw)
        out.append((start, ''.join(buf)))
        buf = []
        start = None
    if buf:
        out.append((start, ''.join(buf)))
    return out


def strip_c_comments(line: str, in_block: bool) -> tuple:
    """Remove C comments from ONE physical line, given whether the line starts
    inside a block comment. Returns (clean_line, still_in_block).

    This has to run before anything else looks at the line, because C strips
    comments BEFORE it recognises preprocessing directives. Three shapes
    otherwise slip past the scanner, and all three end with the generated
    headers agreeing with each other about a value the kernel does not use:

      - `/* gate */ #ifdef KERNEL_TESTS` is a real conditional opener that a
        directive regex anchored at `^\\s*#` never sees
      - an `#endif` spelled inside a block comment would wrongly close a
        conditional that is still open
      - `#define X (BASE - 1) /* why */ + 2` has LIVE tokens after the comment

    A comment is replaced by a space, as the C preprocessor does, so tokens on
    either side of it stay separate."""
    out = []
    i, n = 0, len(line)
    quote = ''
    while i < n:
        ch = line[i]
        if in_block:
            end = line.find('*/', i)
            if end < 0:
                break
            in_block = False
            out.append(' ')
            i = end + 2
            continue
        if quote:
            # Inside a string or char literal, `/*` is just two characters.
            # Without this state a `"/*"` opened a synthetic block comment that
            # swallowed the ACTIVE arm of an #if/#else and left the inactive one
            # standing -- verified against clang-19, which expanded the constant
            # to a different value than the resolver certified.
            out.append(ch)
            if ch == '\\' and i + 1 < n:
                out.append(line[i + 1])
                i += 2
                continue
            if ch == quote:
                quote = ''
            i += 1
            continue
        if ch in '"\'':
            quote = ch
            out.append(ch)
            i += 1
            continue
        if line.startswith('//', i):
            break
        if line.startswith('/*', i):
            in_block = True
            i += 2
            continue
        out.append(ch)
        i += 1
    return (''.join(out), in_block)


def parse_int_literal(text: str) -> tuple:
    """Parse a decimal or hex integer literal, with an explicit sign.

    Not `int(text, 0)`: that raises on `010` -- a shape C reads as OCTAL 8 --
    so the generator would die on a traceback instead of a named error. Octal
    is refused outright rather than honoured, because an exit status written
    with a leading zero is far more likely a typo than a deliberate base-8
    constant, and guessing is the failure mode this resolver exists to remove.
    Returns (value, None) or (None, reason)."""
    sign = -1 if text.startswith('-') else 1
    body = text[1:] if text[:1] in '+-' else text
    if body[:2].lower() == '0x':
        try:
            return (sign * int(body, 16), None)
        except ValueError:
            return (None, f'malformed hex literal {text!r}')
    if len(body) > 1 and body.startswith('0'):
        return (None, f'{text!r} is an octal literal in C; write it in decimal '
                      f'or hex so the value is unambiguous')
    try:
        return (sign * int(body, 10), None)
    except ValueError:
        return (None, f'malformed integer literal {text!r}')


def strip_enclosing_parens(text: str):
    """Peel fully-enclosing parentheses off `text`. Returns None when the
    parentheses are unbalanced -- a body the generator must refuse rather than
    guess at. `(A) + (B)` is returned UNCHANGED (its leading paren closes
    early, so it does not enclose), and then fails the grammar match, which is
    the intended outcome."""
    s = text.strip()
    while s.startswith('(') and s.endswith(')'):
        depth = 0
        encloses = True
        for i, ch in enumerate(s):
            if ch == '(':
                depth += 1
            elif ch == ')':
                depth -= 1
                if depth == 0 and i != len(s) - 1:
                    encloses = False
                    break
        if depth != 0:
            return None
        if not encloses:
            return s
        s = s[1:-1].strip()
    return s


def resolve_exit_expr(body: str, values: dict, known: tuple):
    """Resolve one restricted constant expression. Returns (value, None) on
    success, (None, 'pending') when it references an allowlisted symbol that is
    not resolved YET, and (None, <reason>) on a refusal that must fail the
    build."""
    stripped = strip_enclosing_parens(body)
    if stripped is None:
        return (None, 'unbalanced parentheses')
    m = EXIT_EXPR_RE.match(stripped)
    if not m:
        return (None, 'not a supported constant expression (allowed: SYMBOL, '
                      'SYMBOL +/- N, or a signed integer literal)')
    head, op, offset = m.group(1), m.group(2), m.group(3)
    if head[0].isalpha() or head[0] == '_':
        if head not in known:
            return (None, f'references {head}, which is not in the exit-status '
                          f'allowlist -- add it there or inline the value')
        if head not in values:
            return (None, 'pending')
        value = values[head]
    else:
        value, reason = parse_int_literal(head)
        if reason:
            return (None, reason)
    if op:
        delta, reason = parse_int_literal(offset)
        if reason:
            return (None, reason)
        value = value + delta if op == '+' else value - delta
    if not INT32_MIN <= value <= INT32_MAX:
        return (None, f'resolves to {value}, outside int32_t -- C would '
                      f'truncate it on the way to ring 3, so the generated '
                      f'header would not match the running kernel')
    return (value, None)


def parse_exit_status_defines(path: str, names: tuple) -> tuple:
    """Collect the raw body of `#define <NAME>` for every NAME in `names`,
    refusing every shape the generator cannot certify against the kernel the
    build actually compiles:

      - defined inside ANY preprocessor conditional. The generator does not
        evaluate the build configuration, so with `#if`/`#else` arms it would
        pick a physical definition rather than the ACTIVE one -- and both
        generated headers would then agree with each other while the kernel ran
        a different value, defeating check-abi and the crt0 handshake at once.
        Not hypothetical: task.h already carries 14 `#ifdef KERNEL_TESTS`
        blocks, one of them directly below the exit-status block.
      - defined more than once, for the same reason.
      - absent entirely. A kernel-side rename must fail the generator loudly,
        not silently drop the constant out of the user header.

    Returns (bodies_by_name, errors)."""
    bodies: dict = {}
    errors: list = []
    flagged: set = set()
    depth = 0
    in_block = False
    with open(path, 'r', encoding='utf-8') as f:
        source = f.read()
    # Phase order mirrors the C preprocessor: splice continuations, THEN strip
    # comments, THEN recognise directives. Any other order lets a directive
    # hide behind a comment, or a comment swallow a directive.
    for lineno, raw in splice_logical_lines(source):
        line, in_block = strip_c_comments(raw, in_block)
        if COND_OPEN_RE.match(line):
            depth += 1
            continue
        if COND_CLOSE_RE.match(line):
            if depth == 0:
                # A valid C header cannot have a stray #endif, so seeing one
                # means THIS scanner's model of the file diverged from the
                # compiler's. Fail closed rather than clamp to zero and carry
                # on certifying values from a file we are misreading.
                errors.append(f'unmatched #endif at {path}:{lineno}; the '
                              f'generator is misreading this file and will '
                              f'not certify a value from it')
                break
            depth -= 1
            continue
        m = EXIT_DEFINE_RE.match(line)
        if not m or m.group(1) not in names:
            continue
        name = m.group(1)
        if depth > 0:
            flagged.add(name)
            errors.append(f'{name} ({path}:{lineno}) is defined inside a '
                          f'preprocessor conditional; the generator cannot '
                          f'know which arm the kernel compiles')
            continue
        if name in bodies:
            flagged.add(name)
            errors.append(f'{name} ({path}:{lineno}) is defined more than '
                          f'once; the generator cannot know which '
                          f'definition the kernel compiles')
            continue
        bodies[name] = m.group(2).strip()
    for name in names:
        if name not in bodies and name not in flagged:
            errors.append(f'{name} is not defined in {path} -- it was renamed '
                          f'or removed; update the exit-status allowlist')
    return (bodies, errors)


def resolve_exit_statuses(path: str, export: tuple, resolve_only: tuple) -> tuple:
    """Resolve the allowlisted exit-status constants. Returns
    (entries, errors) where entries is [(name, value, literal)] in `export`
    order; `resolve_only` names feed the arithmetic and are never emitted.

    Resolution is a FIXPOINT, not a source-order walk: a constant defined above
    the symbol it references is legal C (macros expand at use), so file order
    must not decide whether the generator can resolve it. A round that resolves
    nothing means the remainder is cyclic or unresolvable, and says so."""
    known = tuple(export) + tuple(resolve_only)
    bodies, errors = parse_exit_status_defines(path, known)
    if errors:
        return ([], errors)
    values: dict = {}
    pending = dict(bodies)
    while pending:
        progressed = False
        for name in list(pending):
            value, reason = resolve_exit_expr(pending[name], values, known)
            if reason == 'pending':
                continue
            progressed = True
            body = pending.pop(name)
            if reason:
                errors.append(f'{name}: {reason} (body: {body!r})')
            else:
                values[name] = value
        if not progressed:
            for name in sorted(pending):
                errors.append(f'{name}: unresolvable -- its expression is '
                              f'cyclic (body: {pending[name]!r})')
            break
    if errors:
        return ([], errors)
    # Emit the RESOLVED value, not the source expression: the point is to
    # freeze the number ring 3 compares against, so an equivalent rewrite of
    # the kernel-side expression is correctly NOT drift, while any change to
    # the value is. Parenthesized because the values are negative.
    return ([(n, values[n], f'({values[n]})') for n in export], [])


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
 *   include/kernel/sched/task.h          (TASK_EXIT_* exit statuses)
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
{exit_status_block}
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


def render(syscalls, ssdt, ntstatus, exit_status, abi_hash: int) -> str:
    hash_block = (
        '/* ---- ABI fingerprint (FNV-1a 64-bit) --------------------------------------------------------- */\n'
        '/* Hash over the sorted tuple of (SYS_*, SSDT_*, FAULT_*, exported\n'
        ' * TASK_EXIT_* statuses, TEB offsets, KUSD offsets). The STATUS_* codes\n'
        ' * emitted above are NOT fingerprinted -- they mirror a stable external\n'
        ' * contract rather than a number this kernel assigns. Changing an\n'
        ' * exported exit status therefore invalidates existing user binaries at\n'
        ' * crt0, which is deliberate: a stale binary would otherwise compare a\n'
        ' * waitpid result against a number the kernel no longer produces.\n'
        ' * Kernel emits the same hash via SYS_ABI_HANDSHAKE; user\n'
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
        exit_status_block=format_block(
            'Kernel exit statuses (TASK_EXIT_*) -- resolved, signed', exit_status, 32),
    ) + hash_block


# ---- FNV-1a 64-bit hash --------------------------------------------------

FNV_OFFSET_BASIS = 0xCBF29CE484222325
FNV_PRIME        = 0x00000100000001B3
FNV_MASK_64      = (1 << 64) - 1


def fnv1a_update(h: int, data: bytes) -> int:
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & FNV_MASK_64
    return h


def compute_abi_hash(syscalls, ssdt, exit_status, teb_layout, kusd_layout) -> int:
    """Deterministic FNV-1a 64-bit fingerprint over the load-bearing
    ABI surface. Sorted tuples keep the hash stable across reorderings
    in the source headers -- only name/value changes shift the hash.

    Tuple shape: each input yields `name\\0<hex-value>\\n` bytes. SSDT
    entries use the FULL kernel allowlist of SSDT names (not the user
    allowlist), so a user-invisible SSDT renumber also flags the hash.
    Exit statuses use the EXPORTED list only -- a resolve-only helper
    symbol is not a ring-3 contract and must not version-skew binaries.
    """
    h = FNV_OFFSET_BASIS
    for name, _value, literal in sorted(syscalls, key=lambda e: e[0]):
        h = fnv1a_update(h, f'SYS\0{name}\0{literal}\n'.encode('ascii'))
    for name, _value, literal in sorted(ssdt, key=lambda e: e[0]):
        h = fnv1a_update(h, f'SSDT\0{name}\0{literal}\n'.encode('ascii'))
    for name, _value, literal in sorted(exit_status, key=lambda e: e[0]):
        h = fnv1a_update(h, f'EXIT\0{name}\0{literal}\n'.encode('ascii'))
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

# Kernel exit-status constants, split into two lists on purpose.
#
# EXIT_STATUS_EXPORT is the ring-3 CONTRACT: the values a user binary compares
# a waitpid result against. These are emitted into abi_numbers.h and folded
# into the ABI fingerprint.
#
# EXIT_STATUS_RESOLVE_ONLY names the symbols an exported expression is allowed
# to reference. They are resolved internally and NEVER emitted or hashed --
# crt0 aborts every process on a fingerprint change (EX_ABI_MISMATCH), so
# hashing an internal allocation base would reject every existing binary over a
# refactor that left the exported status untouched. Only load-bearing values
# belong in the handshake (design review 2026-07-28).
#
# Tuples, not frozensets: the export order decides the emitted block order, and
# a generated header must not reshuffle itself between runs.
EXIT_STATUS_EXPORT = ('TASK_EXIT_EXEC_IMAGE_DESTROYED',)
EXIT_STATUS_RESOLVE_ONLY = ('TASK_EXIT_REASON_BASE',)


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

    # Exit statuses are signed expressions, so they get the fail-closed
    # resolver rather than parse_defines. Any refusal is fatal by design: a
    # constant the generator cannot certify must break the build here, not
    # vanish from the user header and resurface as a stale hand-copy.
    exit_status, exit_errs = resolve_exit_statuses(
        TASK_H, EXIT_STATUS_EXPORT, EXIT_STATUS_RESOLVE_ONLY)
    if exit_errs:
        for e in exit_errs:
            print(f'gen-user-abi: exit-status: {e}', file=sys.stderr)
        print('               the exit-status allowlist must resolve to a '
              'certain value;', file=sys.stderr)
        print('               omitting it would silently restore the '
              'hand-copied literal', file=sys.stderr)
        print('               it replaced. Fix the definition or the '
              'allowlist.', file=sys.stderr)
        return 1

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
    abi_hash = compute_abi_hash(syscalls, ssdt_all, exit_status,
                                TEB_LAYOUT, KUSD_LAYOUT)

    rendered_user = render(syscalls, ssdt, ntstatus, exit_status, abi_hash)
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

    # Stage BOTH headers, fsync them, then swap both in back to back.
    #
    # What this DOES guarantee: each header goes from old content to new content
    # atomically (os.replace within a filesystem), never through a truncated or
    # half-written state; a failure during GENERATION leaves both destinations
    # untouched; and unique temp names mean two concurrent generator runs cannot
    # clobber each other's staging file.
    #
    # What it deliberately does NOT guarantee: PAIRWISE atomicity. POSIX rename
    # covers one path, so a SIGKILL or power loss between the two os.replace
    # calls still leaves one new header beside one old one, with no handler
    # running to undo it. That is worth naming precisely, because crt0 compares
    # the user header's hash against the kernel's and a mismatched pair aborts
    # every ring-3 binary at SYS_ABI_HANDSHAKE.
    #
    # The mitigation is the drift gate, not the write protocol: --check
    # re-renders and compares BOTH destinations, and the build wrapper runs it
    # before any compilation, so a split pair fails the next build with a named
    # error instead of reaching a kernel. Closing the window itself needs one
    # atomic commit point for the artifact PAIR, which changes how generated
    # headers are laid out and included repo-wide -- tracked by the generator
    # hardening work in the user-mode test framework TODO.
    staged = []
    try:
        for path, rendered in ((OUT_H, rendered_user),
                               (OUT_KERNEL_H, rendered_kernel)):
            os.makedirs(os.path.dirname(path), exist_ok=True)
            fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path),
                                       prefix=os.path.basename(path) + '.',
                                       suffix='.tmp')
            with os.fdopen(fd, 'w', encoding='utf-8') as f:
                f.write(rendered)
                f.flush()
                os.fsync(f.fileno())
            staged.append((tmp, path))
        while staged:
            tmp, path = staged.pop(0)
            os.replace(tmp, path)
    except Exception:
        for tmp, _path in staged:
            try:
                os.unlink(tmp)
            except OSError:
                pass
        raise
    print(f'gen-user-abi: wrote {OUT_H} + {OUT_KERNEL_H} '
          f'(syscalls={len(syscalls)} ssdt={len(ssdt)} ntstatus={len(ntstatus)} '
          f'exit={len(exit_status)} hash=0x{abi_hash:016X})')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
