#!/usr/bin/env python3
# =============================================================================
# gen-user-abi.py -- Generate the ABI contract (abi/generated/abi_contract.h)
#                    from kernel source
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
# Output -- exactly ONE generated file:
#   abi/generated/abi_contract.h  -- generated; committed; do NOT edit by hand
#
# The two headers everyone actually includes are STATIC FACADES over it, not
# generated output, and regenerating does not touch them:
#   user/include/abi_numbers.h    -- ring-3 facade (defines ABI_CONTRACT_WANT_NUMBERS)
#   include/kernel/abi_hash.h     -- kernel facade (fingerprint only)
# Repair a damaged facade with --write-shims; regenerating will not.
#
# Invocation:
#   python3 scripts/gen-user-abi.py               # republish the contract
#   python3 scripts/gen-user-abi.py --check       # verify; exit 1 on drift
#   python3 scripts/gen-user-abi.py --write-shims # restore the two facades
#
# `make check-abi` uses --check as a pre-commit / CI gate: a kernel-side
# renumber that did not get the contract regenerated fails the build at the
# check step instead of at runtime when a user binary makes a syscall against
# the wrong number and gets a garbage handler back. --check also refuses a
# facade that is not its canonical text.
# =============================================================================

import argparse
import concurrent.futures
import itertools
import os
import re
import shutil
import subprocess
import sys
import stat
import tempfile

# ---- Paths --------------------------------------------------------------

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYSCALL_H = os.path.join(REPO_ROOT, 'include', 'kernel', 'sched', 'syscall.h')
SSDT_H    = os.path.join(REPO_ROOT, 'include', 'kernel', 'nt', 'service_numbers.h')
NTSTATUS_H = os.path.join(REPO_ROOT, 'include', 'kernel', 'nt', 'ntstatus.h')
TASK_H    = os.path.join(REPO_ROOT, 'include', 'kernel', 'sched', 'task.h')
# The SINGLE generated destination. Both former destinations
# (user/include/abi_numbers.h, include/kernel/abi_hash.h) are now static
# committed shims over this file -- see HEADER_TEMPLATE for why one file is
# the whole point: POSIX rename is atomic for one path, so one generated
# artifact means publication has no split-pair window to crash inside.
OUT_CONTRACT = os.path.join(REPO_ROOT, 'abi', 'generated', 'abi_contract.h')

# The static shims. NOT generated and NOT written by this script; listed so
# --check can confirm they still route to the generated contract. A shim that
# was edited to hand-define an ABI constant would reintroduce exactly the
# hand-copy drift this generator exists to eliminate.
SHIM_USER_H   = os.path.join(REPO_ROOT, 'user', 'include', 'abi_numbers.h')
SHIM_KERNEL_H = os.path.join(REPO_ROOT, 'include', 'kernel', 'abi_hash.h')

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
# tracking) and whatever it concluded went straight into the generated output
# and the ABI hash. Five adversarial rounds each found a shape where that
# scanner and clang disagreed, and every disagreement had the same shape --
# the generated output was self-consistent (a PAIR of headers at the time;
# one contract since the single-commit-point publication), `make check-abi`
# passed, the crt0 fingerprint
# matched, and the running kernel used a different number.
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
# (KERNEL_TESTS / EXCEPT_TELEMETRY / BUILD_ALT_BOOT). The generated contract
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
    different constants in each, and the generated contract plus every
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
    the other half of that fix if such a flag is ever added.)

    `--no-print-directory` is explicit, not redundant with `-s`. When this
    generator runs from INSIDE a recursive build (the ABI check is itself a
    Makefile prerequisite, so a top-level `make` that recurses into a
    subdirectory build exports MAKEFLAGS with `w` set), the child `make`
    started here inherits that MAKEFLAGS and prints `make[N]: Entering
    directory '...'` / `Leaving directory '...'` to STDOUT regardless of the
    `-s` on this command line -- confirmed against GNU Make: `-s` silences
    recipe echo, it does not imply `--no-print-directory`, and an inherited
    `w` in MAKEFLAGS wins over nothing. Those announcement lines then land
    INSIDE this function's return value and get handed to clang as filenames
    the same way a stray $(info) did (CI run 30531605290, 2026-07-30,
    `stale-abi-fixtures` job: `make[2]: Entering directory ...` reached
    gen-user-abi as a bogus header path). An explicit `--no-print-directory`
    on argv overrides the inherited environment flag for this invocation only
    (GNU Make: command-line flags override MAKEFLAGS-derived ones).

    `--no-print-directory` alone only neutralizes `w`. MAKEFLAGS/MFLAGS can
    carry ANY GNU Make option, and several others corrupt this same stdout
    channel worse than `w` did -- confirmed: an inherited `-p` (database dump)
    or `-d` (debug trace) makes the child print 16,705 / 246,428 extra lines
    ahead of the real flag vector; an inherited `-n` (dry-run) makes it print
    the recipe's literal `printf '%s\n' -Wall ...` source line instead of
    running it, which passes the "nonempty" check below but is not a flag at
    all. There is no complete allow-list of make options to strip one by one
    (adversarial round 2, 2026-07-30) -- the fix is to give this invocation NO
    inherited make state at all: every value the query needs already reaches
    the child through explicit argv (the target name, `-s`,
    `--no-print-directory`, and `overrides` as `VAR=value` tokens), so
    MAKEFLAGS/MFLAGS from the parent process carry nothing this call
    legitimately needs and everything it can be corrupted by. Scrub both
    before the child ever sees them, rather than reacting flag-by-flag to
    each one someone happens to export."""
    cmd = ['make', '-s', '--no-print-directory', target]
    for var, value in sorted((overrides or {}).items()):
        cmd.append(f'{var}={value}')
    child_env = dict(os.environ)
    child_env.pop('MAKEFLAGS', None)
    child_env.pop('MFLAGS', None)
    try:
        proc = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True,
                              text=True, check=False, env=child_env)
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
                          f'generated contract is shared by every flavor, so '
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
    cannot: the CONTRACT is committed once and included by every build, so a
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
 * abi_contract.h -- Kernel/ring-3 ABI contract (GENERATED, SINGLE SOURCE)
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
 * SINGLE COMMIT POINT. This file is the ONLY generated ABI artifact. It
 * used to be a PAIR -- user/include/abi_numbers.h plus
 * include/kernel/abi_hash.h -- published by two back-to-back os.replace
 * calls. POSIX rename is atomic for ONE path, so a SIGKILL between those
 * calls left a mismatched pair on disk, and crt0 aborts every ring-3
 * binary at SYS_ABI_HANDSHAKE when the two disagree. One generated file
 * means one rename: no reader can observe half a generation. Both former
 * destinations are now STATIC committed shims over this file; a shim
 * carries no generated data, so it cannot desynchronize from the source.
 *
 * TWO INDEPENDENT GUARDS, deliberately. The fingerprint is always visible;
 * the number tables appear only under ABI_CONTRACT_WANT_NUMBERS, which the
 * ring-3 shim defines and the kernel shim does not -- the kernel already
 * defines SYS_* in include/kernel/sched/syscall.h and would collide. The
 * guards are separate rather than one `#pragma once` so INCLUSION ORDER
 * cannot matter: a translation unit that already saw this header without
 * the macro still gets the tables when a later include defines it.
 *
 * Layout contract: flat `#define` block per category. No enums (user libc
 * is freestanding and may be compiled without -std=c11+), no function-like
 * macros (keeps parse surface minimal for future tooling).
 * ============================================================================ */

#ifndef IMPOSSIBLE_OS_ABI_CONTRACT_H
#define IMPOSSIBLE_OS_ABI_CONTRACT_H

{hash_block}
#endif /* IMPOSSIBLE_OS_ABI_CONTRACT_H */

#if defined(ABI_CONTRACT_WANT_NUMBERS) && !defined(IMPOSSIBLE_OS_ABI_NUMBERS_H)
#define IMPOSSIBLE_OS_ABI_NUMBERS_H

{syscall_block}
{ssdt_block}
{ntstatus_block}
{exit_status_block}
{mismatch_block}
#endif /* ABI_CONTRACT_WANT_NUMBERS */
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
        ' * in the number block are NOT fingerprinted -- they mirror a stable external\n'
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
    )
    # Ring-3 semantics: the exit status crt0 uses when the handshake fails.
    # It lives under ABI_CONTRACT_WANT_NUMBERS rather than beside the hash
    # because only user/lib/crt_init.c consumes it -- the kernel needs the
    # fingerprint alone, and a kernel TU has no use for a ring-3 exit code.
    mismatch_block = (
        '/* ---- crt0 handshake failure exit status --------------------------------------------------- */\n'
        '#define EX_ABI_MISMATCH          0x42\n'
    )
    return HEADER_TEMPLATE.format(
        hash_block=hash_block,
        syscall_block=format_block('INT 0x80 syscall numbers (SYS_*, FAULT_*)', syscalls, 24),
        ssdt_block=format_block('SSDT service numbers (SSDT_*) -- fastpath probe', ssdt, 32),
        ntstatus_block=format_block('NTSTATUS return codes used by user probes', ntstatus, 32),
        exit_status_block=format_block(
            'Kernel exit statuses (TASK_EXIT_*) -- resolved, signed', exit_status, 32),
        mismatch_block=mismatch_block,
    )


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


# Constant families the generator owns. Any macro whose name starts with one of
# these is an ABI number that must come from the generated contract and from
# nowhere else.
SHIM_OWNED_PREFIXES = ('SYS_', 'SSDT_', 'STATUS_', 'FAULT_', 'TASK_EXIT_',
                       'IMPOSSIBLE_OS_ABI_HASH', 'EX_ABI_MISMATCH')

def user_cpp_flags() -> tuple:
    """Ask the Makefile for the authoritative RING-3 flag vector, the way
    kernel_cpp_flags() does for the kernel. Returns (flags, None) or
    (None, reason).

    A shim must be validated under the flags it is really compiled with. A
    hand-picked minimal vector was tried and was fail-open: production carries
    -O2, which defines __OPTIMIZE__, so a shim could hide a wrong ABI value
    behind `#ifdef __OPTIMIZE__`, satisfy the gate, and still reach every
    optimized user binary -- with the fingerprint untouched, so the crt0
    handshake would agree and nothing would ever report the drift."""
    hit = _FLAGS_CACHE.get('user')
    if hit is not None:
        return hit
    lines, reason = make_query('print-user-cflags')
    if reason:
        return (None, reason)
    flags = [tok for tok in lines if tok not in DEP_ONLY_FLAGS]
    if not flags:
        return (None, '`make print-user-cflags` emitted only dependency-file '
                      'flags; the Makefile target is missing or was emptied')
    _FLAGS_CACHE['user'] = (flags, None)
    return (flags, None)


def _owned_macros(final: dict) -> dict:
    """The subset of a preprocessed macro map that the generator owns."""
    return {n: b for n, b in final.items() if n.startswith(SHIM_OWNED_PREFIXES)}


SHIM_INCLUDE_SPELLING = '"../../abi/generated/abi_contract.h"'

# The facades, pinned to exact canonical TEXT (newline-normalized).
#
# Everything about these two files is fixed: they carry no ABI data and never
# vary with the ABI, so there is nothing for a human to legitimately tune in
# them. Pinning their exact text is therefore both the simplest check and the
# strongest one, and it is the reason no C parsing happens here any more.
#
# Four successive parsing designs were tried and each was bypassed, because
# each was an approximation of what a compiler does with a file:
#   1. a `#define `-prefix text scan          -- missed `# define` and `#undef`
#   2. the same, under minimal flags          -- missed __OPTIMIZE__/flavor guards
#   3. per-vector semantic value comparison   -- missed a guard on NULL, which
#      `user/include/syscall.h` supplies by including types.h first
#   4. a directive allowlist over stripped comments -- missed line splicing,
#      then `// /*` hiding directives from a block-first regex, then `/*` inside
#      an #include header-name collapsing to the required spelling
# Every one of those had the same shape: Python guessing at C lexing, one
# corner at a time. A text comparison has no corners. It cannot be fooled by a
# splice, a comment, a header-name, or a preceding header, because it never
# interprets anything -- every difference is a refusal EXCEPT line endings,
# which are normalized on read because a CRLF checkout is compiler-equivalent
# and .gitattributes already pins LF on the repository side.
#
# check_shim() below is NOT a second production layer: `--check` calls only
# check_shim_form(). Once a facade matches this canonical text and the contract
# matches a fresh render, the macros that facade resolves are DETERMINED, so
# preprocessing it per vector could not discover a new state. check_shim() is
# the tooling-suite CANARY, exercised against deliberately non-canonical
# facades -- the inputs where its answer is not already known.
#
# To change a facade: edit the constant here, then copy it into the file (or
# run `python3 scripts/gen-user-abi.py --write-shims`).
CANONICAL_SHIM_KERNEL = '''\
/* ============================================================================
 * abi_hash.h -- Kernel ABI fingerprint (STATIC FACADE, text-pinned)
 *
 * NOT generated, and NOT free-form: `make check-abi` compares this file
 * against CANONICAL_SHIM_KERNEL in scripts/gen-user-abi.py: newline-normalized,
 * so a CRLF checkout is fine, but every other byte must match exactly.
 * Change it there, or the build fails. The rationale for every line below --
 * why one generated artifact, why a relative include, why this side omits
 * ABI_CONTRACT_WANT_NUMBERS -- lives in that script's HEADER_TEMPLATE and
 * check_shim_form() docstrings, so it is stated once rather than twice.
 * ============================================================================ */

#pragma once

#include "../../abi/generated/abi_contract.h"
'''

CANONICAL_SHIM_USER = '''\
/* ============================================================================
 * abi_numbers.h -- User-mode ABI numbers (STATIC FACADE, text-pinned)
 *
 * NOT generated, and NOT free-form: `make check-abi` compares this file
 * against CANONICAL_SHIM_USER in scripts/gen-user-abi.py: newline-normalized,
 * so a CRLF checkout is fine, but every other byte must match exactly.
 * Change it there, or the build fails. The rationale for every line below --
 * why one generated artifact, why a relative include, why this side defines
 * ABI_CONTRACT_WANT_NUMBERS -- lives in that script's HEADER_TEMPLATE and
 * check_shim_form() docstrings, so it is stated once rather than twice.
 * ============================================================================ */

#pragma once

#define ABI_CONTRACT_WANT_NUMBERS
#include "../../abi/generated/abi_contract.h"
'''


def check_shim_form(path: str, want_numbers: bool) -> list[str]:
    """Compare a facade against its pinned canonical TEXT. No parsing, by design.

    Equality is NEWLINE-NORMALIZED, not raw-byte: the file is read with
    universal newlines, so a CRLF working copy compares equal. That is
    deliberate -- a line ending cannot change which directives are active, and
    failing a checkout clang compiles identically would be a false alarm
    (.gitattributes keeps the repository side LF). Every OTHER byte difference,
    including whitespace inside a line, is refused.

    See CANONICAL_SHIM_* above for the four parsing designs this replaced and
    why each was bypassable. Returns human-readable reasons; empty means the
    file matches its canonical text, modulo line endings."""
    rel = os.path.relpath(path, REPO_ROOT)
    want = CANONICAL_SHIM_USER if want_numbers else CANONICAL_SHIM_KERNEL
    # A facade must be a REGULAR FILE. Comparing content alone accepted a
    # symlink whose target happened to hold the canonical bytes -- the bytes
    # would be right today and follow someone else's file tomorrow, entirely
    # outside this gate's view.
    try:
        st = os.lstat(path)
    except OSError:
        st = None
    if st is not None and not stat.S_ISREG(st.st_mode):
        if stat.S_ISDIR(st.st_mode):
            # --write-shims cannot fix this: os.replace of a regular file onto a
            # directory raises IsADirectoryError, so pointing at it would name a
            # remedy that cannot work. Say what actually resolves it.
            return [f'{rel} is a DIRECTORY where a facade belongs; remove or '
                    f'move it first, then restore the facade with `python3 '
                    f'scripts/gen-user-abi.py --write-shims`']
        kind = 'a symlink' if stat.S_ISLNK(st.st_mode) else 'not a regular file'
        return [f'{rel} is {kind}; a facade must be a regular file so its bytes '
                f'cannot be redirected. Restore it with `python3 '
                f'scripts/gen-user-abi.py --write-shims`']
    try:
        # Universal newlines ON PURPOSE: a CRLF working copy (a Windows
        # checkout, or core.autocrlf=true -- the repo pins LF in .gitattributes
        # but a stale clone predates it) is byte-different and SEMANTICALLY
        # IDENTICAL, since a line ending cannot change which directives are
        # active. Comparing raw bytes there failed every --check on a file
        # clang was perfectly happy with, and, because splitlines() strips both
        # endings, the per-line loop below then found no difference and fell
        # through to a self-contradicting "expected 15 lines, found 15".
        with open(path, 'r', encoding='utf-8') as f:
            got = f.read()
    except FileNotFoundError:
        return [f'{rel} missing -- the facade over the generated contract was '
                f'deleted']
    except (UnicodeDecodeError, OSError) as exc:
        return [f'{rel} is not readable as UTF-8 text: {exc}']
    if got == want:
        return []
    fix = ('restore it with `python3 scripts/gen-user-abi.py --write-shims`, '
           'or edit CANONICAL_SHIM_* in scripts/gen-user-abi.py if the change '
           'is intended')
    # Name the first differing line: the whole point is that a reader can see
    # what moved without diffing by hand.
    got_lines, want_lines = got.splitlines(), want.splitlines()
    for n, (g, w) in enumerate(zip(got_lines, want_lines), 1):
        if g != w:
            return [f'{rel}:{n} differs from the pinned facade -- expected '
                    f'{w!r}, found {g!r}. A facade carries no ABI data and is '
                    f'text-pinned (canonical text, modulo line endings); {fix}']
    if len(got_lines) != len(want_lines):
        return [f'{rel} differs from the pinned facade in length: expected '
                f'{len(want_lines)} lines, found {len(got_lines)}. A facade is '
                f'text-pinned (canonical text, modulo line endings); {fix}']
    # Equal line-by-line and equal in count, yet unequal overall: the only
    # remaining difference is invisible to splitlines(), i.e. a trailing
    # newline or a stray leading BOM. Say so rather than emitting a
    # contradiction.
    return [f'{rel} differs from the pinned facade in characters splitlines() '
            f'does not show -- a missing or extra trailing newline, or a byte-'
            f'order mark. {fix}']





def _user_shim_vectors() -> tuple:
    """[(flags, label)] for validating the ring-3 shim. Returns (vectors, err).

    One vector: ring-3 has no flavor axis (USER_CFLAGS is a single definition
    with no per-build overrides), unlike the kernel."""
    flags, reason = user_cpp_flags()
    if reason:
        return ([], f'cannot obtain the ring-3 flag vector: {reason}')
    return ([(flags, 'user')], None)


def _kernel_shim_vectors() -> tuple:
    """[(flags, label)] for validating the kernel shim, ONE PER FLAVOR.

    Returns (vectors, err). The kernel is built under several flavors
    (KERNEL_TESTS, EXCEPT_TELEMETRY, BUILD_ALT_BOOT), each with its own -D set.
    A shim that hid a wrong ABI value behind one of those macros would be
    invisible to a single-vector check, so every flavor gets its own pass --
    the same rule the constant extraction already follows."""
    combos, reason = flavor_combinations()
    if reason:
        return ([], f'cannot enumerate build flavors: {reason}')
    vectors = []
    for flavor in combos:
        flags, err = kernel_cpp_flags(flavor)
        if err:
            label = ','.join(f'{k}={v}' for k, v in sorted(flavor.items()))
            return ([], f'cannot obtain the kernel flag vector for '
                        f'[{label or "default"}]: {err}')
        label = ','.join(f'{k}={v}' for k, v in sorted(flavor.items()))
        vectors.append((flags, label or 'default'))
    return (vectors, None)


def check_shim(path: str, want_numbers: bool, clang: str,
               flags=None, context: str = '') -> list[str]:
    """Confirm a static shim resolves EXACTLY the ABI macros the contract
    publishes for its side, with the same values.

    The shims are ordinary committed source, not generated output, so they
    cannot be diffed against a rendering. Validation is therefore SEMANTIC: the
    shim and the contract are each preprocessed, and the owned macros the shim
    actually resolves are compared name-by-name and value-by-value against what
    the contract yields for that visibility.

    `flags` MUST be the authoritative vector for this shim's side -- the kernel
    TU vector for the kernel shim (per flavor), the ring-3 vector for the user
    shim. Validating under a hand-picked minimal vector is fail-open, because
    production carries -O2 (hence __OPTIMIZE__) and the kernel adds flavor
    macros: a value guarded behind either would pass here and still reach the
    real binaries. `context` names the vector in any message, so a failure that
    reproduces under only one flavor says which one.

    A raw-text scan was tried first and was fail-open too. It matched only
    lines beginning with the exact token `#define `, so a shim could include
    the contract and then `#undef SYS_WRITE` followed by `# define SYS_WRITE
    999` (note the space) and pass -- ring 3 would compile against a wrong
    syscall number while the fingerprint stayed correct, so SYS_ABI_HANDSHAKE
    would agree and nothing would ever report the drift. Directives inside an
    inactive `#if 0` arm fooled it in the other direction. Asking the
    preprocessor closes both: it reports what the compiler will actually see.

    Returns a list of human-readable reasons; empty means the shim is sound."""
    rel = os.path.relpath(path, REPO_ROOT)
    if context:
        rel = f'{rel} [{context}]'
    if flags is None:
        return [f'{rel}: no authoritative flag vector supplied -- refusing to '
                f'validate a shim under a guessed preprocessing context']
    if not os.path.exists(path):
        return [f'{rel} missing -- the static shim over the generated contract '
                f'was deleted']
    if not os.path.exists(OUT_CONTRACT):
        return [f'{os.path.relpath(OUT_CONTRACT, REPO_ROOT)} missing -- cannot '
                f'validate {rel} against it']

    # What the contract publishes for this side, asked of the compiler rather
    # than assumed from the template -- and under the SAME vector as the shim,
    # so the comparison isolates what the shim did rather than a flag delta.
    contract_flags = list(flags)
    if want_numbers:
        contract_flags.append('-DABI_CONTRACT_WANT_NUMBERS')
    _ev, contract_final, err = run_preprocessor([OUT_CONTRACT], contract_flags,
                                                clang)
    if err:
        return [f'cannot preprocess the generated contract for {rel}: {err}']

    _ev, shim_final, err = run_preprocessor([path], list(flags), clang)
    if err:
        return [f'cannot preprocess {rel}: {err}']

    expected = _owned_macros(contract_final)
    got = _owned_macros(shim_final)
    problems = []

    missing = sorted(set(expected) - set(got))
    if missing:
        head = ', '.join(missing[:5]) + (', ...' if len(missing) > 5 else '')
        problems.append(f'{rel} does not resolve {len(missing)} ABI macro(s) the '
                        f'contract publishes for it ({head}) -- it stopped '
                        f'including the contract, guarded the include out, or '
                        f'undefined them')
    extra = sorted(set(got) - set(expected))
    if extra:
        head = ', '.join(extra[:5]) + (', ...' if len(extra) > 5 else '')
        problems.append(f'{rel} resolves {len(extra)} ABI macro(s) the contract '
                        f'does NOT publish for it ({head}) -- either a hand-'
                        f'written constant, or the wrong ABI_CONTRACT_WANT_'
                        f'NUMBERS visibility for this side')
    for name in sorted(set(expected) & set(got)):
        if expected[name] != got[name]:
            problems.append(f'{rel} resolves {name} to {got[name]!r}, but the '
                            f'contract publishes {expected[name]!r} -- a shim '
                            f'must never restate an ABI value')
    return problems


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
# a waitpid result against. These are emitted into the contract and folded
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


def _contained_dir(path: str) -> tuple:
    """Resolve the directory a publication may write into, refusing to leave
    the repository. Returns (dirname, reason).

    Every component is checked, not just the final one. Protecting only the
    destination file left the same escape one level up: if `user/include` were
    a symlink to an external directory, staging and replacement would both
    resolve through it and overwrite a file outside the tree. This REFUSES
    instead of writing, which is the right answer for a repair command -- it is
    not a defense against a concurrent attacker racing the check (that would
    need dir_fd + O_NOFOLLOW plumbing throughout, which is disproportionate for
    a build script, and a tree whose directories have been swapped for links is
    already compiling from somewhere unintended)."""
    dirname = os.path.dirname(path)
    root = os.path.realpath(REPO_ROOT)
    probe = dirname
    while True:
        # Test for the trust anchor BEFORE testing for a link. The checkout
        # itself may legitimately be reached through a symlink -- a worktree
        # under a symlinked home, or an absolute invocation via such a path --
        # and refusing that rejected every publication in a perfectly normal
        # tree. The anchor is the RESOLVED root; only its descendants are
        # required to be link-free, which is what actually stops an escape.
        if os.path.realpath(probe) == root:
            break
        if os.path.islink(probe):
            return (None, f'{os.path.relpath(probe, REPO_ROOT)} is a symlink; '
                          f'refusing to publish through it')
        parent = os.path.dirname(probe)
        if parent == probe:
            return (None, f'{dirname} is not inside the repository')
        probe = parent
    if not os.path.realpath(dirname).startswith(root + os.sep):
        return (None, f'{dirname} resolves outside the repository')
    return (dirname, None)


def _fsync_dir(dirname: str) -> None:
    """fsync a directory so a rename entry within it survives power loss."""
    dirfd = os.open(dirname, os.O_RDONLY)
    try:
        os.fsync(dirfd)
    finally:
        os.close(dirfd)


def publish_atomically(dest: str, content: str, before_replace=None,
                       after_replace=None) -> bool:
    """THE publication protocol: stage, fsync, ONE os.replace, fsync the parent.

    Used by the generator for the contract and by --write-shims for a facade,
    and invoked directly by the crash-window fixture through the two hooks --
    which is the point of it being one function. The fixture used to re-declare
    its own copy of this protocol, so it certified a transcription rather than
    the shipping code: reordering the replace before the write, or replacing the
    wrong path, would not have failed it.

    os.replace is atomic for ONE path, so a single artifact has no window in
    which a reader can observe half a generation. The parent fsync makes the
    swap survive power loss, not merely a process death.

    The destination MODE is preserved when it already exists, and otherwise set
    to 0644 honouring umask: mkstemp creates 0600, so publishing straight from
    it left the ABI headers private to the invoking user and unreadable by
    anyone else sharing the checkout."""
    dirname, reason = _contained_dir(dest)
    if reason:
        raise RuntimeError(f'refusing to publish {dest}: {reason}')
    # Unchanged input, nothing observable. Replacing unconditionally swapped the
    # inode and moved ctime/mtime on every regeneration, which is exactly what
    # this section promised NOT to do: a re-run over unchanged sources would
    # have re-triggered every make rule and file watcher downstream of the
    # contract for no ABI change at all.
    #
    # The shortcut is gated on lstat, NOT on open(). open() follows symlinks, so
    # a facade replaced by a link whose TARGET already held the canonical bytes
    # compared equal and short-circuited -- leaving the abnormal symlink in
    # place and making --write-shims silently decline to repair the very thing
    # it exists to repair. A FIFO or device at the destination would also have
    # been read, blocking indefinitely or streaming without bound. Only a
    # regular, non-symlink file may take the shortcut; anything else is forced
    # down the replacement path, which swaps the link or special file for a
    # real file.
    try:
        st = os.lstat(dest)
    except OSError:
        st = None
    if st is not None and stat.S_ISREG(st.st_mode):
        try:
            with open(dest, 'r', encoding='utf-8') as f:
                if f.read() == content:
                    # Still complete the durability barrier. os.replace may have
                    # committed on a PREVIOUS attempt whose parent fsync then
                    # failed; returning here without the fsync would turn that
                    # reported failure plus this reported success into a
                    # publication no barrier ever covered.
                    _fsync_dir(dirname)
                    return False
        except (OSError, UnicodeDecodeError):
            pass
    os.makedirs(dirname, exist_ok=True)
    # Preserve the mode ONLY of an existing regular file. os.stat follows
    # links, so replacing an abnormal symlink imported its TARGET's mode onto
    # the new facade -- a link to a mode-000 file published an unreadable
    # facade and reported success, which contradicts the whole point of not
    # following the link.
    if st is not None and stat.S_ISREG(st.st_mode):
        mode = stat.S_IMODE(st.st_mode)
    else:
        umask = os.umask(0)
        os.umask(umask)
        mode = 0o644 & ~umask
    fd, tmp = tempfile.mkstemp(dir=dirname,
                               prefix=os.path.basename(dest) + '.',
                               suffix='.tmp')
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as f:
            f.write(content)
            # fchmod on the OPEN descriptor, BEFORE the fsync -- the mode is
            # part of what has to survive a power loss. Applying it after the
            # fsync left the metadata change outside the durability barrier
            # (the later parent fsync covers the rename entry, not this inode's
            # mode), so recovery could surface the contract at mkstemp's 0600
            # while publication claimed the mode was preserved.
            os.fchmod(f.fileno(), mode)
            f.flush()
            os.fsync(f.fileno())
        if before_replace is not None:
            before_replace(tmp, dest)
        os.replace(tmp, dest)
        if after_replace is not None:
            after_replace(tmp, dest)
    except Exception:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    _fsync_dir(dirname)
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true',
                    help='verify the generated contract matches kernel source and the two facades match their canonical text; exit 1 on drift')
    ap.add_argument('--write-shims', action='store_true',
                    help='rewrite the two text-pinned facades from CANONICAL_SHIM_*')
    args = ap.parse_args()

    if args.write_shims:
        # A facade has one canonical text, so restoring it is a copy, not an edit.
        # Offered as a command so the --check failure message names a fix
        # instead of asking someone to hand-transcribe a constant.
        #
        # Staged + os.replace, NOT open(path, 'w'): this command exists to
        # repair an ABNORMAL facade, and `open` follows symlinks. A facade that
        # had been replaced by a link would have had its TARGET truncated and
        # overwritten -- verified to clobber a file outside the repository while
        # leaving the link in place -- turning a repair into data loss. replace
        # swaps the link itself, and is atomic besides.
        for path, want in ((SHIM_USER_H, CANONICAL_SHIM_USER),
                           (SHIM_KERNEL_H, CANONICAL_SHIM_KERNEL)):
            wrote = publish_atomically(path, want)
            print(f'gen-user-abi: {"wrote" if wrote else "unchanged"} '
                  f'{os.path.relpath(path, REPO_ROOT)}')
        return 0

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
    # frozen into the generated contract and an ABI hash.
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

    rendered_contract = render(syscalls, ssdt, ntstatus, exit_status, abi_hash)

    if args.check:
        drift = False
        # The contract gets the SAME regular-file guard as the facades. It was
        # only guarded there, which left the identical hole on the new single
        # source of truth: a FIFO here blocks this read forever -- and build.sh
        # invokes the gate with no timeout, so it would hang every canonical
        # build -- while a symlink would let the gate bless a mutable external
        # target as the ABI.
        try:
            cst = os.lstat(OUT_CONTRACT)
        except OSError:
            cst = None
        # A bad contract also DISABLES the facade checks below. Those invoke
        # clang on the facades, and every facade #includes the contract -- so
        # with a FIFO contract the guard here reported the problem and the very
        # next stage handed the same FIFO to the compiler and blocked anyway.
        contract_ok = False
        if cst is None:
            print(f'gen-user-abi: {OUT_CONTRACT} missing -- run '
                  '`python3 scripts/gen-user-abi.py`', file=sys.stderr)
            drift = True
        elif not stat.S_ISREG(cst.st_mode):
            kind = 'a symlink' if stat.S_ISLNK(cst.st_mode) else 'not a regular file'
            print(f'gen-user-abi: {OUT_CONTRACT} is {kind}; the generated '
                  f'contract must be a regular file so its bytes cannot be '
                  f'redirected. Remove it and run '
                  f'`python3 scripts/gen-user-abi.py`', file=sys.stderr)
            drift = True
        else:
            with open(OUT_CONTRACT, 'r', encoding='utf-8') as f:
                existing = f.read()
            if existing != rendered_contract:
                print(f'gen-user-abi: {OUT_CONTRACT} is stale vs kernel source',
                      file=sys.stderr)
                drift = True
            else:
                # Byte-equality, not merely "it is a regular file". A stale but
                # regular contract is still arbitrary content: one containing an
                # #include of a FIFO would be reported stale here and then hand
                # that FIFO to clang in the facade stage below, reopening the
                # very hang this guard exists to close. Only a contract that
                # matches what we just rendered is safe to compile against.
                contract_ok = True
        # The shims carry no generated data, so they cannot be "stale" -- but a
        # shim that stopped including the contract, or grew a hand-written ABI
        # constant, silently detaches its side of the build from the generator.
        # That is the drift class this whole generator exists to prevent, so
        # --check refuses it rather than trusting review to catch a one-line edit.
        # Each side is validated under ITS OWN authoritative vector, and the
        # kernel side under every flavor, because a value hidden behind
        # __OPTIMIZE__ or a flavor macro is invisible to any other context.
        # The FORM check is the whole build-time facade gate, deliberately.
        #
        # Once the facade matches its canonical text AND the contract matches
        # the rendering we just computed, the macros that facade resolves are
        # fully DETERMINED -- there is no third state left for preprocessing to
        # discover. Running check_shim() here anyway meant 1 ring-3 vector plus
        # 12 kernel flavors, each preprocessing both the contract and the
        # facade: 26 clang subprocesses per build whose result was decided
        # before they started. `--check` runs on EVERY build, so that was pure
        # recurring cost.
        #
        # check_shim() is not deleted: it stays the semantic canary in
        # scripts/test-tooling.sh, where it is exercised against deliberately
        # non-canonical facades -- exactly the inputs where its answer is NOT
        # already known, and where sweeping every flavor is the point.
        facade_drift = False
        for shim, want_numbers in ((SHIM_USER_H, True), (SHIM_KERNEL_H, False)):
            for reason in check_shim_form(shim, want_numbers):
                print(f'gen-user-abi: {reason}', file=sys.stderr)
                drift = facade_drift = True
        if drift:
            # Name the RIGHT remedy: regenerating rewrites the contract and does
            # nothing to a facade, so pointing a reader at it for a facade
            # failure sends them somewhere that cannot fix their problem.
            print('                run `python3 scripts/gen-user-abi.py` to regenerate',
                  file=sys.stderr)
            if facade_drift:
                print('                and `python3 scripts/gen-user-abi.py '
                      '--write-shims` to restore a facade', file=sys.stderr)
            return 1
        return 0

    wrote = publish_atomically(OUT_CONTRACT, rendered_contract)
    print(f'gen-user-abi: {"wrote" if wrote else "unchanged"} {OUT_CONTRACT} '
          f'(syscalls={len(syscalls)} ssdt={len(ssdt)} ntstatus={len(ntstatus)} '
          f'exit={len(exit_status)} hash=0x{abi_hash:016X})')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
