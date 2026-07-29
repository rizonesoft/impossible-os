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
import concurrent.futures
import itertools
import os
import re
import shutil
import subprocess
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
# BOTH operand orders are matched: `offsetof(...) == N` and `N == offsetof(...)`
# assert exactly the same thing, and recognising only one of them meant a field
# pinned the other way round was invisible to the coverage check -- it would
# then never reach TEB_LAYOUT/KUSD_LAYOUT, never be certified, and never enter
# the ABI fingerprint, so a binary built against the new layout still passed the
# handshake against the old one.
_OFFSETOF = (r'__builtin_offsetof\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*'
             r'([A-Za-z_][A-Za-z0-9_.\[\]]*)\s*\)')
_INTLIT = r'(0x[0-9A-Fa-f]+|[0-9]+)'
STATIC_ASSERT_RE = re.compile(
    r'_Static_assert\s*\(\s*' + _OFFSETOF + r'\s*==\s*' + _INTLIT)
STATIC_ASSERT_REVERSED_RE = re.compile(
    r'_Static_assert\s*\(\s*' + _INTLIT + r'\s*==\s*' + _OFFSETOF)


def parse_kernel_asserts(path: str, struct_name: str, text: str = None
                         ) -> list[tuple[str, int]]:
    """Return [(field, offset)] for every offset assertion on `struct_name`.
    Order preserved.

    `text` should be the PREPROCESSED translation unit. Scanning raw source
    counted assertions the compiler never sees -- one inside a comment, or in
    an inactive `#if` arm -- and missed one whose offset is written as a named
    constant, because the regex only recognises an integer literal. After
    preprocessing, comments and dead arms are gone and a named constant has
    already been expanded to its literal, so what is matched here is what the
    compiler actually compiled. The raw-file fallback exists only for callers
    that have no preprocessed text to offer."""
    if text is None:
        with open(path, 'r', encoding='utf-8') as f:
            text = f.read()
    out: list[tuple[str, int]] = []
    for m in STATIC_ASSERT_RE.finditer(text):
        if m.group(1) == struct_name:
            out.append((m.group(2), int(m.group(3), 0)))
    for m in STATIC_ASSERT_REVERSED_RE.finditer(text):
        if m.group(2) == struct_name:
            out.append((m.group(3), int(m.group(1), 0)))
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


# ---- Preprocessor-backed extraction -------------------------------------
#
# Every constant this generator emits is read through clang, in the SAME
# translation context the kernel is compiled in, and then CERTIFIED by clang
# before it is written. That two-step split is the whole design, and the split
# is what removes the bug class rather than narrowing it:
#
#   EXTRACT proposes a candidate value. It is allowed to be a heuristic.
#   CERTIFY compiles `_Static_assert(NAME == <candidate>)` against the real
#   headers, so a wrong candidate is a BUILD FAILURE, never a silent wrong
#   number.
#
# Before this, extraction WAS the answer: a hand-written scanner reimplemented
# C translation phases 2-4 in Python (splicing, comment removal, conditional
# tracking) and whatever it concluded went straight into both generated headers
# and the ABI hash. Five adversarial rounds each found a shape where that
# scanner and clang disagreed, and every disagreement had the same shape --
# both generated headers agree with each other, `make check-abi` passes, the
# crt0 fingerprint matches, and the running kernel uses a different number.
# Refusing the shapes we thought of is a weaker contract than being checked by
# the compiler, which is why the refusals are no longer the load-bearing part.
#
# Neither `-dM` nor `-E` hands back an evaluated value -- `-dM` prints the
# unexpanded body `(TASK_EXIT_REASON_BASE - 1)` and full `-E` expands that to
# the TEXT `((-1000) - 1)`, not to `-1001`. Any generator that stops at the
# preprocessor is therefore still doing C arithmetic in Python. `_Static_assert`
# is what makes the compiler compute the number.

CLANG_ENV = 'ABI_CLANG'
CLANG_DEFAULT = 'clang-19'

# Every preprocessor-visible build flavor axis, mirrored from the Makefile
# (KERNEL_TESTS / EXCEPT_TELEMETRY / BUILD_ALT_BOOT). The generated header pair
# is committed ONCE and shared by every flavor, so a constant whose value
# depends on which flavor the kernel was built with cannot be represented in it
# at all -- that is a refusal, not a value. The axes are named here but their
# MEANING (which -D each state implies) stays in the Makefile: this script asks
# `make print-abi-cppflags` per combination rather than assembling flags itself.
FLAVOR_AXES = (
    ('KERNEL_TESTS', ('on', 'off')),
    ('EXCEPT_TELEMETRY', ('on', 'off')),
    ('BUILD_ALT_BOOT', ('off', 'diagnostic', 'compatible')),
)

# -MMD/-MP/-MD/-M/-MM make the compiler WRITE dependency files as a side effect;
# they carry no translation semantics, and under -E/-fsyntax-only they would
# litter stray .d files next to the source. Dropped for that reason and that
# reason only -- anything that can change what the preprocessor sees is kept,
# including flags that look irrelevant to a header query (-O2, -mcmodel=kernel),
# because "obviously irrelevant" is the judgement call this design refuses to
# make on the compiler's behalf.
DEP_ONLY_FLAGS = frozenset({'-MMD', '-MP', '-MD', '-M', '-MM'})

# `#define NAME body` / `#undef NAME` as they appear in `clang -dD -E` output.
# -dD emits one line per DIRECTIVE PROCESSED, in order, so a name defined twice
# appears twice and an #undef is visible -- which is what lets ownership be
# checked. -dM (final state only) cannot see either.
DD_DEFINE_RE = re.compile(r'^#define\s+([A-Za-z_][A-Za-z0-9_]*)(\(|\s+|$)(.*)$')
DD_UNDEF_RE = re.compile(r'^#undef\s+([A-Za-z_][A-Za-z0-9_]*)\s*$')


def clang_binary() -> tuple:
    """Locate the compiler THE KERNEL IS BUILT WITH. Returns (path, None) or
    (None, reason).

    The name comes from `make print-abi-cc`, not from a constant here. The
    generator certifies ABI values by COMPILING assertions, so using a
    different binary than the kernel build would be its own silent-drift
    channel: `make CC=<other>` compiles the kernel with one compiler while the
    values are certified with another, compiler predefined macros can select
    different constants in each, and both generated headers plus every
    assertion still agree -- with the wrong compiler. `ABI_CLANG` overrides,
    for a host where the same compiler is installed under another name.

    Absence is an explicit, named REFUSAL rather than a skip. `make check-abi`
    needed no compiler before this change, so this is a real new requirement,
    and the alternative was worse: a silent skip turns the ABI drift gate OFF on
    exactly the hosts least likely to notice, and a fallback to the old scanner
    would re-open the class this function exists to close. Every host that can
    BUILD this repo already has clang-19 (setup.sh installs it as a required
    tool), so the requirement costs nothing on any host that could act on the
    answer anyway."""
    name = os.environ.get(CLANG_ENV)
    if not name:
        name, _axes, reason = abi_config()
        if reason:
            return (None, reason)
    path = shutil.which(name)
    if path:
        return (path, None)
    return (None, f'{name} not found on PATH. The generator reads kernel '
                  f'constants THROUGH the compiler the kernel is built with, '
                  f'so without it there is no certified answer to emit -- and '
                  f'skipping would silently disable the ABI drift gate. '
                  f'Install clang-19 (bash scripts/setup.sh), or set '
                  f'{CLANG_ENV}=<compiler> if yours is named differently.')


_CONFIG_CACHE: dict = {}


def abi_config() -> tuple:
    """The build's self-description: (cc, flavor_axes, None) or
    (None, None, reason). One `make print-abi-config` round-trip, cached for
    the process -- each `make` invocation is ~0.2s of pure startup and this
    runs inside check-abi on every build."""
    hit = _CONFIG_CACHE.get('config')
    if hit is not None:
        return hit
    lines, reason = make_query('print-abi-config')
    if reason:
        return (None, None, reason)
    cc, axes = None, []
    for line in lines:
        if line.startswith('CC='):
            cc = line[3:].strip()
        elif line.startswith('FLAVOR='):
            record = line[7:].strip()
            if '=' not in record:
                return (None, None, f'malformed axis record from make '
                                    f'print-abi-config: {record!r} '
                                    f'(expected AXIS=v1,v2)')
            axis, raw = record.split('=', 1)
            states = [v for v in raw.split(',') if v]
            if not axis.strip() or len(states) < 2:
                return (None, None, f'axis {axis!r} declares fewer than two '
                                    f'states, so it cannot be swept: '
                                    f'{record!r}')
            axes.append((axis.strip(), tuple(states)))
    if not cc:
        return (None, None, '`make print-abi-config` did not report a CC line; '
                            'the generator cannot know which compiler the '
                            'kernel is built with')
    if not axes:
        return (None, None, '`make print-abi-config` reported no FLAVOR axes; '
                            'the cross-flavor sweep would be vacuous')
    _CONFIG_CACHE['config'] = (cc, tuple(axes), None)
    return _CONFIG_CACHE['config']


def make_query(target: str, overrides: dict = None) -> tuple:
    """Run a `print-*` Makefile target and return (lines, None) or
    (None, reason).

    Output is parsed LINE by line, never by whitespace. Every one of these
    targets emits one record per line precisely so a value containing a space
    survives; `split()` would silently shred `-DX='a b'` into two arguments and
    hand the compiler a translation context the kernel never used. (The Make
    side cannot represent such a flag today either -- `printf '%s\n' $(CFLAGS)`
    is word-split by the shell before printf sees it -- so quoting it there is
    the other half of that fix if such a flag is ever added.)"""
    cmd = ['make', '-s', target]
    for var, value in sorted((overrides or {}).items()):
        cmd.append(f'{var}={value}')
    try:
        proc = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True,
                              text=True, check=False)
    except OSError as exc:
        return (None, f'cannot run `make {target}`: {exc}')
    if proc.returncode != 0:
        return (None, f'`{" ".join(cmd)}` failed (rc={proc.returncode}): '
                      f'{proc.stderr.strip()}')
    lines = [ln.strip() for ln in proc.stdout.splitlines() if ln.strip()]
    if not lines:
        return (None, f'`make {target}` produced no output; the Makefile '
                      f'target is missing or was emptied')
    return (lines, None)


def flavor_combinations() -> tuple:
    """Every supported build-flavor combination, as {VAR: value} dicts, with
    the axis matrix read from the MAKEFILE rather than mirrored here.

    A hand-maintained copy of the axes is the same hazard as a hand-maintained
    copy of the flags: adding an axis to the build and forgetting this list
    would silently NARROW the invariance sweep while every check stayed green,
    and the sweep is the only thing standing between a flavor-dependent
    constant and a header shared by every flavor."""
    _cc, axes, reason = abi_config()
    if reason:
        return (None, reason)
    names = [a for a, _ in axes]
    values = [list(v) for _, v in axes]
    combos = [dict(zip(names, combo)) for combo in itertools.product(*values)]
    return (combos, None)


# One `make` round-trip is ~0.13s of pure process startup, and both the flavor
# sweep and the fixture suite ask for the same vectors repeatedly within a
# single process. The Makefile cannot change underneath a running generator, so
# the answer is cached per flavor for the life of the process.
#
# Unlocked on purpose. The flavor sweep reads this from several threads, but
# each key maps to a deterministic value, so the only race is two threads
# recomputing the SAME vector and storing identical results; `dict` insertion is
# atomic under the GIL, so no partially-built entry is ever observable. A lock
# here would serialize the sweep it exists to speed up.
_FLAGS_CACHE: dict = {}


def kernel_cpp_flags(flavor: dict = None) -> tuple:
    """Ask the Makefile for the authoritative flag vector. Returns
    (flags, None) or (None, reason).

    The generator deliberately does NOT assemble this list. A hand-picked
    subset is how the context drifts: the real compile line carries five
    ordered -I paths and three flavor -D flags, and a subset that merely LOOKS
    equivalent can select a different definition while every generated artifact
    stays self-consistent."""
    key = tuple(sorted((flavor or {}).items()))
    hit = _FLAGS_CACHE.get(key)
    if hit is not None:
        return hit
    lines, reason = make_query('print-abi-cppflags', flavor)
    if reason:
        return (None, reason)
    flags = [tok for tok in lines if tok not in DEP_ONLY_FLAGS]
    if not flags:
        return (None, '`make print-abi-cppflags` emitted only dependency-file '
                      'flags; the Makefile target is missing or was emptied')
    _FLAGS_CACHE[key] = (flags, None)
    return (flags, None)


class _Events(list):
    """A plain list of preprocessor events that also carries the preprocessed
    text it was derived from. A list subclass so every existing consumer keeps
    iterating it unchanged."""
    text = ''


def _probe_tu(headers) -> str:
    """A translation unit that includes the allowlisted headers and nothing
    else, so what the query sees is what the kernel sees."""
    return ''.join(f'#include "{h}"\n' for h in headers)


def run_preprocessor(headers, flags, clang) -> tuple:
    """Run `clang -dD -E` over `headers`. Returns (events, final, None) or
    (None, None, reason), where events is an ordered [(kind, name, body)] of
    every #define/#undef the preprocessor PROCESSED and final maps each name to
    the body still in effect at the end.

    The raw preprocessed text is attached as `events.text` so a caller can scan
    what the compiler ACTUALLY saw (comments stripped, dead arms gone, named
    constants expanded) instead of re-reading the source file."""
    fd, tu = tempfile.mkstemp(suffix='.c', prefix='abi-probe.')
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as f:
            f.write(_probe_tu(headers))
        cmd = [clang] + list(flags) + ['-dD', '-E', tu]
        try:
            proc = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True,
                                  text=True, check=False)
        except OSError as exc:
            return (None, None, f'cannot run the preprocessor: {exc}')
        if proc.returncode != 0:
            detail = proc.stderr.strip().splitlines()
            head = '; '.join(detail[:3]) if detail else 'no diagnostic'
            return (None, None, f'the preprocessor REFUSED these headers '
                                f'(rc={proc.returncode}): {head}')
    finally:
        os.unlink(tu)
    events = []
    final = {}
    for line in proc.stdout.splitlines():
        m = DD_UNDEF_RE.match(line)
        if m:
            events.append(('undef', m.group(1), None))
            final.pop(m.group(1), None)
            continue
        m = DD_DEFINE_RE.match(line)
        if not m:
            continue
        name, sep, rest = m.group(1), m.group(2), m.group(3)
        if sep == '(':
            # Function-like macro: never an ABI number, and its body is not a
            # constant expression. Recorded as an event so a name that is both
            # object-like and function-like still trips the ownership check.
            events.append(('define', name, None))
            final[name] = None
            continue
        body = rest.strip()
        events.append(('define', name, body))
        final[name] = body
    events = _Events(events)
    events.text = proc.stdout
    return (events, final, None)


def check_single_ownership(events, names) -> list:
    """Refuse any allowlisted name the preprocessor saw defined more than once,
    including a redefinition that is IDENTICAL and a #undef followed by a new
    definition.

    `-Werror=macro-redefined` does not cover this: clang accepts an identical
    redefinition silently, and accepts #undef + redefine silently, reporting
    only the surviving value. Both shapes exit 0. The property worth keeping is
    not "clang did not complain" but "exactly one place in the kernel owns this
    number" -- a constant with two owners is one refactor away from the two
    disagreeing, and the generator would faithfully certify whichever one won
    the include race."""
    # Indexed once rather than rescanned per name: main() checks every published
    # family (650+ names) against the full event stream, and the naive nested
    # scan is quadratic over an input that grows with every kernel header.
    wanted = set(names)
    defines: dict = {}
    undeffed = set()
    for kind, name, body in events:
        if name not in wanted:
            continue
        if kind == 'undef':
            undeffed.add(name)
        else:
            defines.setdefault(name, []).append(body)
    errors = []
    for name in names:
        bodies = defines.get(name, [])
        if len(bodies) <= 1:
            continue
        shape = ('#undef followed by a new definition' if name in undeffed else
                 'defined again without an intervening #undef')
        distinct = {b for b in bodies if b is not None}
        same = ' (the definitions are IDENTICAL, which clang accepts silently)' \
            if len(distinct) <= 1 else ''
        errors.append(f'{name} has {len(bodies)} definitions in the kernel '
                      f'translation context -- {shape}{same}. Exactly one '
                      f'place must own an ABI number; collapse them or drop '
                      f'the name from the allowlist')
    return errors


# Each emitted family states how the number it publishes must compare against
# the kernel constant. The comparison TYPE is part of the claim: STATUS_* is a
# 32-bit bit pattern published as unsigned hex while the kernel spells it
# `((NTSTATUS)0xC0000008)` (a negative int32), so a bare `==` would compare a
# negative int against a large unsigned and prove the wrong thing.
CERTIFY_UNSIGNED32 = 'unsigned32'
CERTIFY_SIGNED32 = 'signed32'


UINT32_MAX = (1 << 32) - 1


def certify_range_error(name: str, value: int, kind: str, tag: str):
    """Refuse a proposed value that does not FIT the type it will be published
    and compared as. Returns a reason, or None.

    Without this the certification could pass while the header lied. Both sides
    of an unsigned assertion are 32-bit -- the kernel expression is cast to
    `unsigned int` and the proposal would be masked -- so a malformed
    `((NTSTATUS)0x1C0000008)` certifies as `0xC0000008`, while `format_block`
    emits the ORIGINAL unmasked `0x1C0000008` into the user header. The
    generator would then have certified one number and published a different
    one, which is worse than either failing or being wrong consistently."""
    if kind == CERTIFY_UNSIGNED32:
        if not 0 <= value <= UINT32_MAX:
            return (f'{tag}: {name} = {value:#x} does not fit uint32; it would '
                    f'be certified truncated to {value & 0xFFFFFFFF:#x} while '
                    f'the header published the untruncated literal')
    elif not INT32_MIN <= value <= INT32_MAX:
        return (f'{tag}: {name} = {value} does not fit int32; C would truncate '
                f'it on the way to ring 3, so the generated header would not '
                f'match the running kernel')
    return None


def _assert_line(name: str, value: int, kind: str, tag: str) -> str:
    # Callers must have run certify_range_error() first: the masks below are
    # then identities, not silent truncation.
    if kind == CERTIFY_UNSIGNED32:
        lhs = f'((unsigned int)({name}))'
        rhs = f'{value & 0xFFFFFFFF}u'
    else:
        lhs = f'((int)({name}))'
        rhs = f'({value})'
    return (f'_Static_assert({lhs} == {rhs}, '
            f'"{tag}: {name} does not match the generated header");')


def _offset_assert_line(struct: str, field: str, off: int) -> str:
    return (f'_Static_assert(__builtin_offsetof({struct}, {field}) == {off}, '
            f'"{struct}.{field} offset does not match the ABI hash input");')


def cross_flavor_names(headers, clang, prefixes, flavors=None) -> tuple:
    """Preprocess `headers` in EVERY flavor and return (names_by_flavor,
    errors), refusing any prefixed name whose PRESENCE depends on the flavor.

    Extracting once in the default flavor is not enough, and the gap is subtle:
    the flavor sweep can only assert names the extraction pass discovered, so a
    constant defined only under (say) `KERNEL_TESTS=off` is absent from the
    candidates, the skip list, the ownership check, the generated header, the
    FNV hash AND every certification unit -- and all 12 units still compile,
    because nothing ever asks about it. A cross-flavor guarantee is only worth
    as much as the INVENTORY behind it, so the inventory is taken per flavor
    too."""
    if flavors is None:
        combos, reason = flavor_combinations()
        if reason:
            return ({}, None, None, [reason])
    else:
        combos = flavors
    if not combos:
        # ThreadPoolExecutor(max_workers=0) raises, and an empty sweep would
        # "prove" flavor-invariance by checking nothing at all.
        return ({}, None, None,
                ['the build-flavor matrix is empty, so the cross-flavor '
                 'inventory would check nothing'])

    # The DEFAULT flavor is one of the combinations, so extraction reads its
    # events/macros back out of this sweep instead of paying a separate,
    # unoverlapped make+clang pass before it (~0.23s of every build).
    # combos[0] IS the default because `make print-abi-flavors` lists each
    # axis's default state first and itertools.product varies the last axis
    # fastest -- a contract stated on the Makefile target, not an accident of
    # ordering here.
    default = combos[0] if combos else None
    default_events = default_macros = None

    def probe(flavor):
        flags, reason = kernel_cpp_flags(flavor)
        if reason:
            return (flavor, None, None, reason)
        events, macros, reason = run_preprocessor(headers, flags, clang)
        return (flavor, events, macros, reason)

    with concurrent.futures.ThreadPoolExecutor(max_workers=len(combos)) as pool:
        results = list(pool.map(probe, combos))

    seen: dict = {}
    every = set()
    for flavor, events, macros, reason in results:
        if reason:
            return ({}, None, None, [reason])
        desc = ' '.join(f'{k}={v}' for k, v in sorted(flavor.items()))
        every.add(desc)
        if flavor == default:
            default_events, default_macros = events, macros
        for name in macros:
            if name.startswith(prefixes):
                seen.setdefault(name, set()).add(desc)
    errors = []
    for name in sorted(seen):
        missing = every - seen[name]
        if missing:
            errors.append(f'{name} carries an ABI prefix but is defined in only '
                          f'{len(seen[name])} of {len(every)} build flavors '
                          f'(absent under: {"; ".join(sorted(missing))}). The '
                          f'generated header pair is shared by every flavor, so '
                          f'a name present in only some of them has nothing to '
                          f'publish -- make it unconditional or drop the prefix')
    return (seen, default_events, default_macros, errors)


def certify_values(headers, families, clang, flavors=None,
                   offset_families=None) -> list:
    """Compile `_Static_assert(NAME == <emitted value>)` for every constant the
    generator is about to publish, once per build flavor. Returns [] when the
    compiler certifies every value in every flavor.

    This is the step that makes the COMPILER the authority. Extraction upstream
    only has to PROPOSE a number; if it proposes a wrong one -- because a body
    was spliced, hidden behind a comment, wrapped in a cast, or built from an
    expression the proposer read badly -- the assert fails and the build stops.

    Running it across every flavor also answers a question a single query
    cannot: the header PAIR is committed once and included by every build, so a
    constant that is 5 under one flavor and 6 under another has no single
    correct value to publish. Such a constant fails here rather than being
    silently frozen at whichever flavor happened to generate it. A name that is
    undefined in some flavor fails the same way, as an unknown identifier.

    `families` is [(tag, kind, [(name, value, literal)])]."""
    asserts = []
    range_errors = []
    for tag, kind, entries in families:
        for name, value, _literal in entries:
            reason = certify_range_error(name, value, kind, tag)
            if reason:
                range_errors.append(reason)
                continue
            asserts.append(_assert_line(name, value, kind, tag))
    # Struct offsets are hash inputs too, so they get the same treatment. Before
    # this they were verified by REGEX over raw header text, which accepts an
    # assertion sitting in a comment or in an inactive #if arm and never notices
    # a kernel-side assertion that was deleted -- so a field could move while a
    # stale textual assertion held the fingerprint constant, and the kernel and
    # user headers would agree with each other all the way through the runtime
    # handshake. __builtin_offsetof compiled in the real translation context
    # cannot be fooled by either.
    for tag, struct, entries in (offset_families or []):
        for field, off in entries:
            asserts.append(_offset_assert_line(struct, field, off))
    if range_errors:
        return range_errors
    if not asserts:
        # An empty assert set would compile trivially and report success, so a
        # caller that lost its tables would be told the ABI is certified. There
        # is no legitimate call with nothing to certify.
        return ['nothing to certify: the constant tables are empty, so a '
                'successful compile would prove nothing about the ABI']
    source = _probe_tu(headers) + '\n'.join(asserts) + '\n'
    if flavors is None:
        combos, reason = flavor_combinations()
        if reason:
            return [reason]
    else:
        combos = flavors

    # The flavor sweep is a dozen independent subprocess round-trips and this
    # runs inside `check-abi`, on the critical path of every build. Serially it
    # is ~2.2s, of which the `make` queries alone are ~1.5s -- pure process
    # startup, not work. They share nothing, so they run concurrently; threads
    # are the right tool because every one of them is blocked in wait(2).
    def certify_one(flavor):
        desc = ' '.join(f'{k}={v}' for k, v in sorted(flavor.items()))
        flags, reason = kernel_cpp_flags(flavor)
        if reason:
            return [reason]
        fd, tu = tempfile.mkstemp(suffix='.c', prefix='abi-certify.')
        try:
            with os.fdopen(fd, 'w', encoding='utf-8') as f:
                f.write(source)
            try:
                proc = subprocess.run([clang] + flags + ['-fsyntax-only', tu],
                                      cwd=REPO_ROOT, capture_output=True,
                                      text=True, check=False)
            except OSError as exc:
                return [f'cannot run the compiler for certification: {exc}']
        finally:
            os.unlink(tu)
        if proc.returncode == 0:
            return []
        found = [f'[{desc}] {line.strip()}' for line in proc.stderr.splitlines()
                 if 'static assertion failed' in line or 'error:' in line]
        return found or [f'[{desc}] the certification unit failed to compile '
                         f'(rc={proc.returncode}); the emitted values cannot '
                         f'be confirmed against the kernel in this flavor']

    with concurrent.futures.ThreadPoolExecutor(max_workers=len(combos)) as pool:
        # Ordered map, so the reported failures do not shuffle between runs.
        results = list(pool.map(certify_one, combos))
    return [e for group in results for e in group]


def source_define_order(path: str) -> list:
    """Names in the order they are `#define`d in the SOURCE file.

    Used for PRESENTATION only -- the generated header stays readable and
    diffable against the kernel header it mirrors. Values never come from here:
    `-dD` output is emitted in include-processing order across the whole
    translation unit, which is not the order a human reads one header in."""
    order = []
    seen = set()
    with open(path, 'r', encoding='utf-8') as f:
        for line in f:
            m = re.match(r'^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)', line)
            if m and m.group(1) not in seen:
                seen.add(m.group(1))
                order.append(m.group(1))
    return order


def propose_defines(macros: dict, path: str, name_prefix) -> list:
    """Propose (name, value, literal) for every allowlisted-prefix name the
    PREPROCESSOR reported, in source order.

    This is a candidate proposal, not a verdict: `certify_values` compiles each
    number against the kernel headers afterwards, so a body this function reads
    badly fails the build instead of being published. That is why it is allowed
    to stay a simple single-literal rule -- the rule no longer has to be right
    about C, it only has to be right often enough to be useful, and wrong is
    caught rather than shipped.

    Bodies come from the preprocessor rather than from a regex over the file, so
    conditional arms, splices, comments and include order are already resolved
    the way the kernel build resolves them."""
    prefixes = (name_prefix,) if isinstance(name_prefix, str) else tuple(name_prefix)
    order = source_define_order(path)
    rank = {name: i for i, name in enumerate(order)}
    candidates = []
    skipped = []
    for name, body in macros.items():
        if not name.startswith(prefixes):
            continue
        if body is None:
            # Function-like or empty: never a publishable ABI number. Reported
            # rather than dropped -- see the skip contract below.
            skipped.append((name, '<function-like or empty>'))
            continue
        lits = INT_LITERAL_RE.findall(body)
        # Exactly one integer literal keeps the generated header a flat integer
        # table: `(BAR + 1)` (a symbol plus a literal) and `(A | B)` (no
        # literal) are deliberately NOT published rather than guessed at.
        if len(lits) != 1:
            skipped.append((name, body))
            continue
        literal = lits[0]
        try:
            value = int(literal, 0)
        except ValueError:
            skipped.append((name, body))
            continue
        candidates.append((name, value, literal))
    # Names absent from this header (reached through an include) sort after the
    # ones a reader can find in it, alphabetically so the output is stable.
    candidates.sort(key=lambda e: (rank.get(e[0], len(order)), e[0]))
    skipped.sort()
    # The SKIP list is returned, never swallowed. A name carrying an ABI prefix
    # that this rule cannot read is not a harmless omission: main() feeds these
    # tables to BOTH the generated header and the FNV fingerprint, so a dropped
    # name silently leaves the hash as well -- `#define SSDT_NtFoo SSDT_NtBar`
    # would vanish from the contract while every artifact stayed self-consistent,
    # which is the exact failure shape this section exists to remove. Nothing is
    # skipped in the tree today (72/72 SYS_+FAULT_, 479/479 SSDT_, 103/103
    # STATUS_), so refusing on skip costs nothing now and catches the first one.
    return (candidates, skipped)


# ---- Signed / expression-valued constants -------------------------------
#
# propose_defines() above extracts the first UNSIGNED integer literal out of a
# define body, which is correct for every table it reads (SYS_*, FAULT_*,
# SSDT_*, STATUS_* are all non-negative). It is actively WRONG for the kernel
# exit-status block: `(-1000)` would resolve to +1000 and
# `(TASK_EXIT_REASON_BASE - 1)` to +1 -- silently incorrect values rather than
# omissions, which is a worse failure than the hand-copy drift this exists to
# close. Exit statuses therefore keep their own restricted expression grammar
# and their own fail-closed contract on top of the preprocessor's answer.
#
# What used to live here as well -- splice_logical_lines() and
# strip_c_comments(), a Python reimplementation of C translation phases 2 and 3
# -- is GONE. Each was correct, each was paid for by an adversarial round that
# found a shape it got wrong, and each was still only as good as the shapes
# anyone had thought of. clang performs those phases now, so keeping a second
# implementation around would preserve exactly the drift risk this section
# exists to delete.


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


def parse_exit_status_defines(path: str, names: tuple, macros: dict = None,
                              events: list = None) -> tuple:
    """Collect the body of `#define <NAME>` for every NAME in `names` as the
    PREPROCESSOR resolved it, refusing what the generator cannot certify:

      - defined more than once in the translation context (including an
        identical redefinition, and #undef followed by a new definition).
        Exactly one place must own an ABI number.
      - absent entirely. A kernel-side rename must fail the generator loudly,
        not silently drop the constant out of the user header.

    Conditional definitions are no longer refused HERE, and that is an upgrade
    rather than a relaxation. The old scanner refused anything inside `#if`
    because it could not evaluate the build configuration and would otherwise
    have picked a physical arm rather than the ACTIVE one. clang picks the arm
    the kernel actually compiles, so a conditional definition now resolves
    CORRECTLY -- and a definition whose value genuinely differs between build
    flavors is caught downstream by `certify_values`, which compiles the emitted
    number against every flavor. Net: the shapes that used to be refused because
    they were unreadable are now read correctly, and the shapes that are
    genuinely unpublishable still fail the build.

    Returns (bodies_by_name, errors)."""
    if macros is None:
        clang, reason = clang_binary()
        if reason:
            return ({}, [reason])
        flags, reason = kernel_cpp_flags()
        if reason:
            return ({}, [reason])
        events, macros, reason = run_preprocessor([path], flags, clang)
        if reason:
            return ({}, [reason])
    ownership = check_single_ownership(events or [], names)
    errors = list(ownership)
    # Names already reported for ownership must not ALSO be reported as absent.
    # Matched on the exact leading token rather than a substring: `X in <name>`
    # would let X_STATUS inherit a diagnostic raised for X_STATUS_EXTRA.
    flagged = {e.split(' ', 1)[0] for e in ownership}
    bodies = {}
    for name in names:
        body = macros.get(name)
        if body is None:
            if name not in flagged:
                errors.append(f'{name} is not defined in the translation '
                              f'context rooted at {path} -- it was renamed, '
                              f'removed, or compiled out; update the '
                              f'exit-status allowlist')
            continue
        bodies[name] = body.strip()
    return (bodies, errors)


def resolve_exit_statuses(path: str, export: tuple, resolve_only: tuple,
                          macros: dict = None, events: list = None) -> tuple:
    """Resolve the allowlisted exit-status constants. Returns
    (entries, errors) where entries is [(name, value, literal)] in `export`
    order; `resolve_only` names feed the arithmetic and are never emitted.

    Resolution is a FIXPOINT, not a source-order walk: a constant defined above
    the symbol it references is legal C (macros expand at use), so file order
    must not decide whether the generator can resolve it. A round that resolves
    nothing means the remainder is cyclic or unresolvable, and says so.

    The value this returns is still a CANDIDATE. `certify_values` compiles it
    against the kernel headers afterwards, so the grammar below can refuse an
    expression it does not understand (fail loud) but can no longer certify one
    it understands WRONGLY (fail silent). Passing `macros`/`events` reuses an
    existing preprocessor query instead of spawning another."""
    known = tuple(export) + tuple(resolve_only)
    bodies, errors = parse_exit_status_defines(path, known, macros, events)
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

    # One preprocessor query over ALL the allowlisted headers, in the kernel's
    # own translation context. Reading them together rather than one at a time
    # is deliberate: include order is part of that context, and a name the
    # kernel reaches through an include chain must resolve here the same way.
    clang, reason = clang_binary()
    if reason:
        print(f'gen-user-abi: {reason}', file=sys.stderr)
        return 1
    headers = [SYSCALL_H, SSDT_H, NTSTATUS_H, TASK_H]

    # The INVENTORY is taken per flavor, not just the values: a name defined
    # only under a non-default flavor would be invisible to every check below
    # AND to certification, because nothing generates an assertion for a name
    # nobody discovered. Extraction then reuses the DEFAULT flavor's pass out of
    # this same sweep rather than preprocessing that context a second time.
    ABI_PREFIXES = ('SYS_', 'FAULT_', 'SSDT_', 'STATUS_')
    _names, events, macros, flavor_errs = cross_flavor_names(
        headers, clang, ABI_PREFIXES)
    if flavor_errs:
        for e in flavor_errs:
            print(f'gen-user-abi: flavor: {e}', file=sys.stderr)
        return 1
    if events is None or macros is None:
        print('gen-user-abi: the flavor sweep returned no default-flavor '
              'extraction; the axis matrix does not list the build default '
              'first', file=sys.stderr)
        return 1

    syscalls, sys_skipped = propose_defines(macros, SYSCALL_H, ('SYS_', 'FAULT_'))
    ssdt_all, ssdt_skipped = propose_defines(macros, SSDT_H, 'SSDT_')
    ssdt = [e for e in ssdt_all if e[0] in SSDT_USER_ALLOWLIST]
    ntstatus_all, nts_skipped = propose_defines(macros, NTSTATUS_H, 'STATUS_')
    ntstatus = [e for e in ntstatus_all if e[0] in NTSTATUS_USER_ALLOWLIST]

    # A prefixed name the single-literal rule cannot read must FAIL, not vanish.
    # These tables feed the FNV fingerprint as well as the header, so a silent
    # skip drops the name out of the ABI contract while every artifact stays
    # self-consistent -- the failure shape this section exists to remove.
    inventory_errs = [f'{name} carries an ABI prefix but its body is not a '
                      f'single integer literal (body: {body!r}); publish it as '
                      f'a plain integer, or remove the prefix'
                      for name, body in sys_skipped + ssdt_skipped + nts_skipped]

    # Every allowlisted name must actually be present. Filtering a name that no
    # longer exists yields a SHORTER table, not an error, so without this a
    # kernel-side rename silently drops a constant ring 3 still expects.
    for label, allow, got in (('SSDT', SSDT_USER_ALLOWLIST, ssdt),
                              ('NTSTATUS', NTSTATUS_USER_ALLOWLIST, ntstatus)):
        missing = sorted(set(allow) - {n for n, _v, _l in got})
        if missing:
            inventory_errs.append(f'{label} user allowlist names absent from '
                                  f'the kernel translation context: '
                                  f'{", ".join(missing)} -- renamed, removed, '
                                  f'or no longer a plain integer')

    # Ownership, for EVERY published family and not just the exit statuses.
    # -Wmacro-redefined (fatal under the kernel's -Werror) already stops an
    # incompatible redefinition; this covers the two shapes clang accepts
    # silently -- an identical redefinition, and #undef followed by redefine.
    inventory_errs += check_single_ownership(
        events, [n for n, _v, _l in syscalls + ssdt_all + ntstatus_all])

    if inventory_errs:
        for e in inventory_errs:
            print(f'gen-user-abi: inventory: {e}', file=sys.stderr)
        print('               an ABI name that cannot be published must break '
              'the build here;', file=sys.stderr)
        print('               dropping it would remove it from the generated '
              'header AND the ABI', file=sys.stderr)
        print('               fingerprint while every artifact stayed '
              'self-consistent.', file=sys.stderr)
        return 1

    # Exit statuses are signed expressions, so they go through the restricted
    # grammar rather than the single-literal rule. Any refusal is fatal by
    # design: a constant the generator cannot resolve must break the build here,
    # not vanish from the user header and resurface as a stale hand-copy.
    exit_status, exit_errs = resolve_exit_statuses(
        TASK_H, EXIT_STATUS_EXPORT, EXIT_STATUS_RESOLVE_ONLY, macros, events)
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
    #
    # The assertions are read out of the PREPROCESSED layout headers, not the
    # raw files. On raw text a commented-out or dead-arm assertion still counts
    # while one whose offset is written as a named constant does not -- so a
    # newly pinned field could stay out of the layout lists, out of certification
    # and out of the fingerprint, and a binary built against the new layout would
    # still pass the handshake against the old one.
    layout_flags, reason = kernel_cpp_flags()
    if reason:
        print(f'gen-user-abi: {reason}', file=sys.stderr)
        return 1
    layout_events, _layout_macros, reason = run_preprocessor(
        [TEB_HEADER, KUSD_HEADER], layout_flags, clang)
    if reason:
        print(f'gen-user-abi: layout: {reason}', file=sys.stderr)
        return 1
    layout_text = layout_events.text
    teb_errs = check_coverage(
        TEB_LAYOUT, parse_kernel_asserts(TEB_HEADER, 'TEB', layout_text), 'TEB')
    kusd_errs = check_coverage(
        KUSD_LAYOUT,
        parse_kernel_asserts(KUSD_HEADER, 'KUSER_SHARED_DATA', layout_text),
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

    # THE gate: have the compiler certify every number about to be published,
    # in every build flavor. Everything above only PROPOSED values -- a
    # single-literal rule for the flat tables and a restricted grammar for the
    # exit statuses, both of them Python reading C. This is where clang gets the
    # last word, so a proposal that is wrong fails the build instead of being
    # frozen into two mutually-agreeing headers and an ABI hash.
    #
    # Both name lists are certified, not just the emitted subset: ssdt_all and
    # ntstatus_all feed the fingerprint and the allowlist filter respectively,
    # so a wrong value there is just as load-bearing as one in the header.
    families = [
        ('syscall', CERTIFY_SIGNED32, syscalls),
        ('ssdt', CERTIFY_SIGNED32, ssdt_all),
        ('ntstatus', CERTIFY_UNSIGNED32, ntstatus_all),
        ('exit-status', CERTIFY_SIGNED32, exit_status),
    ]
    # TEB/KUSD offsets are hashed, so they are certified alongside the constants
    # rather than trusted to the raw-text assertion scan above.
    certify_errs = certify_values(
        headers + [TEB_HEADER, KUSD_HEADER], families, clang,
        offset_families=[('teb', 'TEB', TEB_LAYOUT),
                         ('kusd', 'KUSER_SHARED_DATA', KUSD_LAYOUT)])
    if certify_errs:
        for e in certify_errs:
            print(f'gen-user-abi: certify: {e}', file=sys.stderr)
        print('               the compiler disagrees with a value this '
              'generator was about to', file=sys.stderr)
        print('               publish. A number that differs per build flavor '
              'cannot be put in a', file=sys.stderr)
        print('               header every flavor shares; a number that '
              'differs at all means the', file=sys.stderr)
        print('               extraction misread the kernel. Fix the kernel '
              'definition or the', file=sys.stderr)
        print('               allowlist -- do NOT hand-edit the generated '
              'header.', file=sys.stderr)
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
