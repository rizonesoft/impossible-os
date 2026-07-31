#!/usr/bin/env python3
"""Correlate a `refused_<n>_<prefix>_<8hex>.exe` identity back to a filename.

The launcher never echoes the raw name of a refused test binary: a refused
name is untrusted bytes, and a control byte in it would split the very
record that reports it across physical serial lines. What it publishes
instead is a synthesized identity -- a run-unique ordinal, a sanitized and
truncated prefix, and an 8-hex FNV-1a digest of the entry's EXACT byte
span (`src/kernel/test/test_usermode.c`, u_build_refusal_id).

That leaves an operator holding a digest and no way to compute it, which
is what this tool exists for. It is a DIAGNOSTIC, not a gate: nothing in
the build or test path depends on it, so it mirrors the kernel's hashing
and sanitizing rules rather than any of its bounds. The two shared
constants -- the FNV-1a basis/prime and the accepted charset -- are pinned
by golden vectors asserted on BOTH sides (the kernel unit test
`UTEST: name digest covers bytes past a NUL` and the host regression in
scripts/test-tooling.sh), so a change to either side breaks a test rather
than silently producing digests that no longer correspond.

WHAT THE KERNEL HASHES, exactly, per discovery path:

  manifest  the entry's first whitespace-delimited token, measured against
            the LINE's end rather than by C-string scanning -- so an
            embedded NUL is INSIDE the span, and a name refused for that
            NUL hashes bytes a C string would never reach. Comment tails
            (` #...`), attribute tokens (`type=`, `expects_tasks=`) and
            the EOL are outside it.
  readdir   the directory entry's basename bytes, excluding the
            terminating NUL. A filename cannot contain a NUL, so this path
            never has bytes past one.

MATCHING is on the WHOLE identity, never the digest alone. A 32-bit
FNV-1a is not collision-resistant and the kernel says so: within a run,
uniqueness comes from the ordinal, not from hash strength. So a candidate
matches only when its digest AND its sanitized prefix both agree, every
candidate is reported, and more than one surviving candidate exits as
AMBIGUOUS rather than naming the first one.

AGGREGATE identities are a SECOND, structurally distinct shape:
`agg_<label>_<value>.exe`. The five fail-closed aggregates are not
refusals of a name -- no file produced them -- so there is nothing to
correlate back to, and `match` says so instead of searching. They are
recognized on the leading `agg_` token rather than by mirroring the
kernel's list of labels, so a newly added aggregate is classified
correctly by a tool that has never heard of it.

The value's unit is carried by the LABEL, because it is not one semantic
type across the five: `_pages` is a page count that was requested,
`_kept` is how many entries were RETAINED before a cap bit (the loss is
whatever came after, which was never counted), and the literal `unknown`
replaces the number entirely where it is not knowable -- a manifest that
would not parse leaves the number of lost entries underivable, and the
older shape's `0` there read as "nothing was lost".

Commands:
  digest   <input>            print the 8-hex digest of one candidate
  match    <identity> <input> report which candidates produce that identity,
                              or classify an aggregate identity (no candidate
                              inputs are needed for an aggregate)

Inputs (each may be repeated; `match` accepts any mix):
  --name NAME          a filename as an argv string (no NUL, no raw control
                       bytes -- use --hex for those)
  --hex HH[HH...]      raw bytes, the canonical form for arbitrary input
  --names-from FILE    one candidate per line, LF-separated, no trailing LF
                       needed (a filename cannot contain LF)
  --nul-from FILE      NUL-separated candidates, e.g. `find -print0`.
                       FILESYSTEM NAMES ONLY: the delimiter cannot represent
                       a name that itself contains a NUL, which is exactly
                       the manifest case, so use --manifest or --hex there.
  --manifest FILE      tokenize a test manifest the way the kernel
                       enumerator does and offer every entry's name span

Exit codes:
  0  digest printed / exactly one candidate matched
  2  usage error
  3  no candidate matched
  4  more than one candidate matched (AMBIGUOUS -- report lists them all)
  5  the identity is a fail-closed AGGREGATE: no filename produced it, so
     no correlation was attempted. Deliberately not 0 (nothing was
     matched) and not 3 (nothing was missing) -- a caller that treats
     "aggregate" as either would be drawing a conclusion the tool did not
     reach.
"""

import argparse
import os
import re
import stat
import sys

# The published FNV-1a 32-bit basis and prime, mirroring UTEST_FNV1A_BASIS
# and UTEST_FNV1A_PRIME.
FNV1A_BASIS = 0x811C9DC5
FNV1A_PRIME = 0x01000193
MASK32 = 0xFFFFFFFF

# Input bounds. This tool reads operator-supplied files, so it states its
# own limits rather than inheriting the host's memory as one.
MAX_INPUT_BYTES = 16 * 1024 * 1024
MAX_CANDIDATES = 100000
# Per-FILE and per-COUNT limits do not bound the total: repeating a 16 MiB
# single-line file a hundred times stays inside both while retaining ~1.6
# GiB of candidates. These two bound what is actually held.
MAX_CANDIDATE_BYTES = 64 * 1024
MAX_TOTAL_CANDIDATE_BYTES = 64 * 1024 * 1024
# Used only when the kernel source cannot be read (this tool copied out of
# the tree); the live value is parsed from UTEST_MANIFEST_ARENA_BYTES.
MANIFEST_CAP_FALLBACK = 8192

# The accepted charset, mirroring u_name_char_ok. Every byte outside it is
# rendered `_` in the identity's prefix -- including the very byte that
# caused a charset refusal, which is why the prefix alone can never
# identify a name and the digest exists.
_NAME_CHAR_OK = re.compile(rb"[A-Za-z0-9._-]")

# `refused_<ordinal>_<prefix>_<8hex>.exe`. The prefix may itself contain
# `_`, so the ordinal is taken from the FRONT and the digest from the BACK;
# a greedy middle is the only correct reading.
_IDENTITY_RE = re.compile(
    r"^refused_(?P<ordinal>\d+)_(?P<prefix>.*)_(?P<digest>[0-9a-f]{8})\.exe$")

# `agg_<label>_<value>.exe`, the fail-closed aggregate shape. The label may
# contain `_`, so the value is taken from the BACK and the label is the
# greedy middle -- the same reading the refusal shape needs. The value is a
# decimal, or the literal `unknown` where the kernel cannot derive one.
#
# Matched on the SHAPE, never against a mirrored list of kernel labels: an
# aggregate added to the launcher tomorrow is classified correctly by a copy
# of this tool that predates it, which a label allowlist could not do.
#
# The value grammar is CANONICAL ASCII uint32, not `\d+`. Python's `\d` is
# Unicode-aware and `int()` is unbounded, so a permissive reading would
# authenticate `agg_plan_kept_0007.exe`, `agg_plan_kept_4294967296.exe` and
# identities built from non-ASCII decimal digits as genuine launcher
# records -- none of which u_append_uint can emit from a uint32_t. Because
# aggregate recognition short-circuits correlation entirely, accepting one
# would misclassify a corrupted identity as a launcher record rather than
# reporting it as unparseable.
_AGGREGATE_VALUE_UNKNOWN = "unknown"
_AGGREGATE_VALUE_MAX = 0xFFFFFFFF
_AGGREGATE_RE = re.compile(
    r"^agg_(?P<label>.*)_(?P<value>0|[1-9][0-9]*|" +
    _AGGREGATE_VALUE_UNKNOWN + r")\.exe$", re.ASCII)


def fnv1a32(data):
    """FNV-1a over an exact byte span -- not over a C string."""
    h = FNV1A_BASIS
    for byte in data:
        h ^= byte
        h = (h * FNV1A_PRIME) & MASK32
    return h


def sanitize(data):
    """Render bytes the way u_build_refusal_id builds its prefix."""
    return b"".join(c if _NAME_CHAR_OK.match(c) else b"_"
                    for c in (data[i:i + 1] for i in range(len(data))))


def parse_identity(text):
    """Split a refusal identity into (ordinal, prefix bytes, digest int).

    The prefix is validated as something the kernel could actually have
    emitted: non-empty, and built only from the accepted charset (every
    other byte is rendered `_` by u_build_refusal_id, so a prefix carrying
    one was not produced by it). An EMPTY prefix in particular would make
    the prefix half of the match vacuous, leaving a 32-bit digest -- which
    the kernel explicitly does not treat as unique -- as the whole test.

    What is deliberately NOT validated is the prefix's LENGTH against the
    ordinal's budget. That budget derives from UTEST_MAX_BINARY_NAME, which
    is build-specific and changed (37 -> 36) in the same section that added
    this tool; a host copy of it would mis-refuse every identity produced by
    a kernel whose bound differed, which is the mirrored-constant failure
    the capture reconciler already refused to repeat.
    """
    match = _IDENTITY_RE.match(text)
    if not match:
        # Name BOTH shapes. An identity that begins `agg_` but failed the
        # aggregate grammar reaches here, and telling its operator only
        # about the refusal shape would send them looking for the wrong
        # defect -- the launcher publishes canonical ASCII decimals from a
        # uint32, so `agg_plan_kept_007.exe` is corruption, not a refusal.
        raise ValueError(
            "not a launcher identity: expected "
            "refused_<ordinal>_<prefix>_<8 hex digits>.exe, or "
            "agg_<label>_<uint32 or `unknown`>.exe, got %r" % (text,))
    prefix = match.group("prefix").encode("utf-8", "surrogateescape")
    # An EMPTY prefix is legal, and refusing it used to reject a shape the
    # kernel demonstrably emits. A manifest token whose FIRST byte is NUL
    # classifies REFUSE_NUL (the span carries a NUL, so len != span_len --
    # src/kernel/test/test_usermode.c), and its stored C string is empty, so
    # u_build_refusal_id's readable-prefix loop stops at raw[0] and produces
    # `refused_<ordinal>__<digest>.exe`. That is precisely the embedded-NUL
    # shape this tool promises to correlate, so the digest carries the whole
    # identity for it: every candidate whose digest matches is reported, and
    # the caller's existing ambiguity handling decides what that means.
    if prefix and sanitize(prefix) != prefix:
        raise ValueError(
            "refusal identity prefix carries a byte outside the accepted "
            "charset, which u_build_refusal_id renders `_`: %r" % (text,))
    return (int(match.group("ordinal")), prefix,
            int(match.group("digest"), 16))


def strip_c_comments(text):
    """Blank out C comments, preserving offsets and line structure.

    A structural claim read off raw C text is a claim about the SOURCE
    FILE, not about what the compiler builds: a commented-out table row or
    publication site still matches a regex. Replacing comment bytes with
    spaces (and keeping newlines) removes them from every later match
    without disturbing line numbers.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        two = text[i:i + 2]
        if two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        elif two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text[i] in "\"'":
            quote, j = text[i], i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(text[i:j])
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def parse_aggregate(text):
    """Split an aggregate identity into (label, value) or return None.

    `value` is an int, or None where the identity carries the `unknown`
    literal instead of a number. Returning None for a non-aggregate is what
    lets the caller fall through to the refusal shape: the two are disjoint
    by construction, since a refusal identity always begins `refused_`.

    The label is validated as something the kernel could have emitted --
    non-empty and inside the accepted charset -- for the same reason the
    refusal prefix is. Its LENGTH is deliberately not checked against the
    kernel's derived name bound: that bound is build-specific, and a host
    copy of it would mis-refuse identities from a kernel whose bound
    differed, which is the mirrored-constant failure this tool exists
    without.
    """
    match = _AGGREGATE_RE.match(text)
    if not match:
        return None
    label = match.group("label").encode("utf-8", "surrogateescape")
    if not label or sanitize(label) != label:
        raise ValueError(
            "aggregate identity label carries a byte outside the accepted "
            "charset, which the launcher cannot emit: %r" % (text,))
    raw_value = match.group("value")
    if raw_value == _AGGREGATE_VALUE_UNKNOWN:
        return (label, None)
    value = int(raw_value)
    if value > _AGGREGATE_VALUE_MAX:
        raise ValueError(
            "aggregate identity value %s is past the uint32 the launcher "
            "publishes from, so no run produced it: %r" % (raw_value, text))
    return (label, value)


def describe_aggregate_value(label, value):
    """Say what the number means, from the UNIT the label carries.

    The launcher deliberately puts the unit in the label rather than
    publishing every aggregate's number as "the count of what was lost" --
    which is what the previous shape did, and it was wrong for four of the
    five paths. This reads the unit back off the suffix, and says plainly
    when it does not recognize one, rather than inventing a reading.
    """
    if value is None:
        return ("the launcher could not derive this number -- it published "
                "`unknown` rather than a zero that would read as 'nothing "
                "was lost'")
    text = label.decode("utf-8", "surrogateescape")
    if text.endswith("_pages"):
        return "%d page(s) requested" % (value,)
    if text.endswith("_kept"):
        return ("%d entr(y/ies) RETAINED before the cap bit -- what was lost "
                "came after this point and was never counted" % (value,))
    return ("%d -- this label carries no unit suffix this tool recognizes; "
            "read the launcher's aggregate table for its meaning" % (value,))


def candidate_matches(raw, prefix, digest):
    """How `raw` matches this identity: None, "exact", or "truncated".

    Two independent checks, because neither is sufficient alone: the digest
    is 32 bits and explicitly not collision-resistant, and the prefix is
    sanitized and truncated to whatever the ordinal left. The prefix is
    compared over its OWN length -- it is a truncation of the candidate,
    not the whole of it -- and the caller is told WHICH of the two it was,
    because only an exact prefix accounts for the whole candidate.
    """
    # An EMPTY prefix is not a wildcard, it is a fact about the candidate.
    # u_build_refusal_id's prefix loop writes nothing only when it stops at
    # raw[0], and that is the ONLY way to reach an empty prefix: a NULL name
    # is excluded by the caller, and the guard above the loop returns before
    # `budget` can reach 0. So an empty-prefix identity was produced by bytes
    # beginning with a NUL, and no ordinary filename could have produced it
    # whatever its digest says. Without this the prefix half of the match
    # goes vacuous -- `sanitized[:0] == b""` is true for everything -- and a
    # 32-bit collision would be reported as a confident attribution.
    if not prefix and raw[:1] != b"\x00":
        return None
    if fnv1a32(raw) != digest:
        return None
    sanitized = sanitize(raw)
    if sanitized == prefix:
        return "exact"
    if sanitized[:len(prefix)] == prefix:
        return "truncated"
    return None


def _decode_hex(text):
    stripped = "".join(text.split())
    try:
        return bytes.fromhex(stripped)
    except ValueError:
        raise ValueError("--hex takes an even number of hex digits, got %r"
                         % (text,))


def manifest_name_spans(data):
    """Yield each manifest entry's exact name-token bytes, in file order.

    A byte-exact mirror of u_manifest_parse's line/token walk: leading
    whitespace (including CR) is skipped, a `#` introduces a comment only at
    line start or after whitespace, the payload ends at EOL or that comment,
    trailing spaces and tabs are stripped, and the name is the first
    space/tab-delimited token measured against the LINE's end -- so an
    embedded NUL is part of the span rather than a terminator.
    """
    pos, end = 0, len(data)
    while pos < end:
        while pos < end and data[pos:pos + 1] in (b" ", b"\t", b"\r"):
            pos += 1
        if pos >= end:
            break
        if data[pos:pos + 1] == b"\n":
            pos += 1
            continue
        if data[pos:pos + 1] == b"#":
            while pos < end and data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        line_start = pos
        while pos < end and data[pos:pos + 1] not in (b"\n", b"\r"):
            if (data[pos:pos + 1] == b"#" and pos > line_start
                    and data[pos - 1:pos] in (b" ", b"\t")):
                break
            pos += 1
        line_end = pos
        while (line_end > line_start
               and data[line_end - 1:line_end] in (b" ", b"\t")):
            line_end -= 1
        while pos < end and data[pos:pos + 1] != b"\n":
            pos += 1
        if pos < end:
            pos += 1
        span_end = line_start
        while (span_end < line_end
               and data[span_end:span_end + 1] not in (b" ", b"\t")):
            span_end += 1
        if span_end > line_start:
            yield data[line_start:span_end]


def read_bounded(path, limit=MAX_INPUT_BYTES):
    """Read a REGULAR file whole, refusing anything past `limit`.

    A diagnostic tool pointed at a corrupt or enormous file should say so
    rather than exhaust the host it is helping debug; a directory or device
    is a usage error, not an input.

    Everything is decided on the DESCRIPTOR, never on a path stat taken
    beforehand: a stat-then-open pair can disagree, and a caller that treats
    "the path looked missing a moment ago" as a fact ends up degrading on a
    file that is present and unusable. `O_NONBLOCK` is what makes opening
    safe before the type is known -- a FIFO with no writer would otherwise
    block in the open itself, which is the hang the type check exists to
    prevent. ABSENCE surfaces distinctly, as FileNotFoundError.
    """
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NONBLOCK", 0))
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode):
            raise ValueError("%s is not a regular file" % (path,))
        if info.st_size > limit:
            raise ValueError("%s is %d bytes, past this tool's %d-byte input "
                             "limit" % (path, info.st_size, limit))
        handle = os.fdopen(fd, "rb")
        fd = None       # ownership moved to the file object
        with handle:
            data = handle.read(limit + 1)
    finally:
        if fd is not None:
            os.close(fd)
    if len(data) > limit:
        # It grew between the fstat and the read. Refusing is the only
        # honest answer: the bytes examined are not the whole file.
        raise ValueError("%s grew past this tool's %d-byte input limit while "
                         "it was being read" % (path, limit))
    return data


def kernel_manifest_cap(source=None):
    """The manifest size at or above which the KERNEL discards the file.

    Read from the kernel source rather than mirrored, because a hardcoded
    copy would silently disagree the day the arena is resized -- and here
    that disagreement would make the tool attribute a refusal to a manifest
    the launcher never parsed. Returns (cap, provenance) so the caller can
    say which number it used when the source is not on hand.
    """
    path = source or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "..", "src", "kernel", "test",
                                  "test_usermode.c")
    # Through the SAME bounded reader as every other input: this path is
    # operator-supplied when `--kernel-source` is given, so a FIFO or
    # /dev/zero here would otherwise block or exhaust the host through the
    # one door the input bounds left open.
    #
    # The fallback is for ABSENCE ONLY -- this script copied out of the
    # tree, where there is no kernel source to consult. A source that is
    # PRESENT but unreadable, oversized, non-regular, or missing the macro
    # is an ERROR, because degrading there would silently reconcile against
    # a cap that is not the kernel's and attribute (or refuse) a manifest on
    # a number nobody chose.
    #
    # Absence is decided by the OPEN ATTEMPT, not by a preceding
    # `os.path.exists`: that snapshot can disagree with the open that
    # follows it, and it reports a dangling symlink -- a directory entry
    # that is present and unusable -- as missing.
    try:
        text = read_bounded(path).decode("utf-8", "replace")
    except FileNotFoundError:
        if source:
            raise ValueError("--kernel-source %s does not exist" % (path,))
        # A dangling symlink opens as ENOENT but is a directory entry that
        # EXISTS and does not work -- someone pointed it somewhere. Treating
        # that as "no kernel source here" would degrade on a tree that has
        # one, which is the whole failure this branch is narrowed against.
        if os.path.lexists(path):
            raise ValueError(
                "kernel source at %s is a broken link; repair it or pass "
                "--kernel-source rather than reconciling against a cap the "
                "kernel did not state" % (path,))
        return (MANIFEST_CAP_FALLBACK, "documented default (no kernel source "
                                       "at %s)" % (path,))
    except (ValueError, OSError) as exc:
        if source:
            raise ValueError("--kernel-source %s" % (exc,))
        raise ValueError(
            "kernel source at %s is present but unusable (%s); pass a "
            "readable one with --kernel-source rather than reconciling "
            "against a cap the kernel did not state" % (path, exc))
    match = re.search(r"#\s*define\s+UTEST_MANIFEST_ARENA_BYTES\s+(\d+)", text)
    if not match:
        raise ValueError(
            "kernel source at %s declares no UTEST_MANIFEST_ARENA_BYTES; the "
            "manifest cap cannot be derived from it" % (path,))
    return (int(match.group(1)), "read from %s" % (path,))


def kernel_aggregate_kinds(source=None):
    """The launcher's aggregate table as {label: publishes_a_number}.

    READ from the kernel source, never mirrored -- the same rule (and the
    same reader) as kernel_manifest_cap above. A hardcoded copy would be
    the mirrored-constant failure this whole identity kind exists to avoid,
    and it is what lets this tool reject a value FORM the table forbids
    without ever knowing the labels in advance.

    Returns (mapping, provenance). The mapping is None when there is
    genuinely no kernel source to consult (this script copied out of the
    tree), in which case the caller reports the identity as aggregate-
    SHAPED but unverified rather than authenticating it.
    """
    path = source or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "..", "src", "kernel", "test",
                                  "test_usermode.c")
    try:
        text = read_bounded(path).decode("utf-8", "replace")
    except FileNotFoundError:
        if source:
            raise ValueError("--kernel-source %s does not exist" % (path,))
        if os.path.lexists(path):
            raise ValueError(
                "kernel source at %s is a broken link; repair it or pass "
                "--kernel-source rather than reporting an aggregate whose "
                "value form nothing checked" % (path,))
        return (None, "no kernel source at %s" % (path,))
    except (ValueError, OSError) as exc:
        if source:
            raise ValueError("--kernel-source %s" % (exc,))
        raise ValueError(
            "kernel source at %s is present but unusable (%s); pass a "
            "readable one with --kernel-source" % (path, exc))
    block = re.search(r"#define UTEST_AGG_KINDS\(X\)(.*?)\n\n", text, re.S)
    if not block:
        raise ValueError(
            "kernel source at %s declares no UTEST_AGG_KINDS table; an "
            "aggregate's value form cannot be checked against it" % (path,))
    body = strip_c_comments(block.group(1))
    rows = re.findall(
        r'X\(\s*[A-Z0-9_]+\s*,\s*"([A-Za-z0-9._-]+)"\s*,\s*([01])\s*\)', body)
    declared = len(re.findall(r"\bX\(", body))
    if not rows or len(rows) != declared:
        raise ValueError(
            "kernel source at %s has a UTEST_AGG_KINDS table this tool "
            "parsed only %d of %d rows from; refusing to check a value form "
            "against a partial reading" % (path, len(rows), declared))
    return ({label: has_value == "1" for label, has_value in rows},
            "read from %s" % (path,))


def collect_candidates(args):
    """Build the ordered (source, bytes) candidate list from the CLI."""
    candidates = []
    total = [0]

    def add(source, raw):
        if len(candidates) >= MAX_CANDIDATES:
            raise ValueError("more than %d candidates offered; narrow the "
                             "input rather than scanning everything"
                             % MAX_CANDIDATES)
        # A candidate is a NAME. One longer than this is not a filename any
        # discovery path can produce, so refusing it costs nothing and stops
        # a single pathological line from being retained whole.
        if len(raw) > MAX_CANDIDATE_BYTES:
            raise ValueError("a candidate from %s is %d bytes; no discovery "
                             "path produces a name past %d"
                             % (source, len(raw), MAX_CANDIDATE_BYTES))
        total[0] += len(raw)
        if total[0] > MAX_TOTAL_CANDIDATE_BYTES:
            raise ValueError("candidate inputs total more than %d bytes; the "
                             "per-file limit does not bound a repeated or "
                             "expanded input set" % MAX_TOTAL_CANDIDATE_BYTES)
        candidates.append((source, raw))

    for name in args.name or ():
        add("--name", name.encode("utf-8", "surrogateescape"))
    for text in args.hex or ():
        add("--hex", _decode_hex(text))
    for path in args.names_from or ():
        for line in read_bounded(path).split(b"\n"):
            if line:
                add("%s (line)" % path, line)
    for path in args.nul_from or ():
        for item in read_bounded(path).split(b"\0"):
            if item:
                add("%s (nul)" % path, item)
    for path in args.manifest or ():
        body = read_bounded(path)
        # The kernel DISCARDS an oversized manifest whole -- it does not
        # truncate and parse the head -- so no refusal identity in any
        # artifact can have come from a file this large. Tokenizing it
        # anyway would let this tool name a "match" the launcher provably
        # never derived, which is worse than refusing to guess.
        cap, provenance = kernel_manifest_cap(args.kernel_source)
        # Say so whenever the cap is NOT the live one. Otherwise a tool run
        # outside the tree silently reconciles against a constant that may
        # no longer be the kernel's, and the only way to notice would be a
        # refusal that never came.
        if not provenance.startswith("read from"):
            sys.stderr.write("utest-refusal-id: manifest cap %d taken from "
                             "the %s\n" % (cap, provenance))
        if len(body) >= cap:
            raise ValueError(
                "%s is %d bytes; the launcher discards a manifest at or above "
                "%d (%s) without parsing any entry, so no refusal identity "
                "can have come from it"
                % (path, len(body), cap, provenance))
        for span in manifest_name_spans(body):
            add("%s (manifest span)" % path, span)
    return candidates


def _render(raw):
    """A printable rendering of candidate bytes -- never the raw bytes.

    Same reason the kernel does not log them: these are untrusted, and a
    control byte would corrupt the line reporting it.
    """
    return repr(raw)[1:]        # drop Python's `b` prefix, keep the quoting


def cmd_digest(args):
    candidates = collect_candidates(args)
    if not candidates:
        sys.stderr.write("utest-refusal-id: digest needs one input\n")
        return 2
    for source, raw in candidates:
        print("%08x  %s  %s" % (fnv1a32(raw), _render(raw), source))
    return 0


def cmd_match(args):
    # The aggregate shape is checked FIRST and short-circuits: no filename
    # produced one, so searching candidates could only ever be misleading.
    # Under the old shape an aggregate borrowed the refusal identity with a
    # COUNT in the digest slot, and a candidate whose FNV-1a happened to
    # equal that count matched for real -- the all-candidates rule could not
    # flag it, because the digest genuinely agreed.
    try:
        aggregate = parse_aggregate(args.identity)
    except ValueError as exc:
        sys.stderr.write("utest-refusal-id: %s\n" % (exc,))
        return 2
    if aggregate is not None:
        label, value = aggregate
        # The grammar alone does not prove the launcher could emit THIS
        # identity: whether a kind publishes a number is a column in the
        # kernel's table, so `agg_plan_kept_unknown.exe` and
        # `agg_manifest_bad_0.exe` are both well-formed and both
        # impossible. Check the form against the table rather than
        # authenticating a corrupted or ABI-skewed artifact.
        try:
            kinds, provenance = kernel_aggregate_kinds(args.kernel_source)
        except ValueError as exc:
            sys.stderr.write("utest-refusal-id: %s\n" % (exc,))
            return 2
        text_label = label.decode("utf-8", "surrogateescape")
        if kinds is not None:
            if text_label not in kinds:
                sys.stderr.write(
                    "utest-refusal-id: no aggregate kind named %r in the "
                    "launcher's table (%s); this identity was not published "
                    "by it\n" % (text_label, provenance))
                return 2
            if kinds[text_label] != (value is not None):
                wanted = "a number" if kinds[text_label] else "`unknown`"
                sys.stderr.write(
                    "utest-refusal-id: aggregate %r publishes %s, so this "
                    "identity's value form is one the launcher cannot emit "
                    "(%s)\n" % (text_label, wanted, provenance))
                return 2
        print("identity: %s" % args.identity)
        print("  AGGREGATE: a fail-closed aggregate published by the "
              "launcher, not a refused filename")
        print("  label %s" % _render(label))
        print("  value %s" % describe_aggregate_value(label, value))
        if kinds is None:
            # Aggregate-SHAPED, but nothing checked it against the table.
            # Saying so is the difference between a classification and an
            # authentication.
            print("  NOTE: %s, so the label and value form were NOT checked "
                  "against the launcher's table -- this is an "
                  "aggregate-SHAPED identity, not a verified one"
                  % (provenance,))
        print("  no correlation attempted: no file produced this record, so "
              "there is nothing to match it back to")
        return 5

    try:
        ordinal, prefix, digest = parse_identity(args.identity)
    except ValueError as exc:
        sys.stderr.write("utest-refusal-id: %s\n" % (exc,))
        return 2
    candidates = collect_candidates(args)
    if not candidates:
        sys.stderr.write("utest-refusal-id: match needs at least one "
                         "candidate input\n")
        return 2
    hits = []
    for source, raw in candidates:
        kind = candidate_matches(raw, prefix, digest)
        if kind:
            hits.append((source, raw, kind))
    print("identity: %s" % args.identity)
    print("  ordinal %d (unique within its run), prefix %s, digest %08x"
          % (ordinal, _render(prefix), digest))
    print("  %d candidate(s) examined, %d matched" % (len(candidates),
                                                      len(hits)))
    if not prefix:
        # The leading-NUL shape: the kernel's readable-prefix loop stopped at
        # raw[0], so the identity carries no prefix and the 32-bit digest --
        # which the kernel explicitly does not treat as unique -- is the whole
        # of the match. Said out loud rather than folded into the exit code:
        # refusing the identity outright would break the embedded-NUL case
        # this tool promises to correlate, but letting a digest-only hit read
        # like a prefix-confirmed one would overstate what was proved.
        print("  NOTE: this identity carries NO prefix (its raw bytes begin "
              "with a NUL), so the 32-bit digest alone decided every match "
              "above -- confirm the candidate independently")
    for source, raw, kind in hits:
        # `truncated` means the identity's prefix is a proper prefix of this
        # candidate's sanitization -- normal for a name longer than the
        # budget the ordinal left, but worth naming, because it is the one
        # case where the prefix does not account for the whole candidate.
        print("  MATCH (%s prefix) %s  %s" % (kind, _render(raw), source))
    if not hits:
        print("  no candidate reproduces this identity -- the refused entry "
              "may have been removed, or its exact bytes differ from every "
              "candidate offered (try --manifest, or --hex for a name "
              "carrying a NUL or a control byte)")
        return 3
    if len(hits) > 1:
        # A 32-bit digest cannot promise a unique reverse mapping, and the
        # prefix is truncated, so naming one of these would be a guess.
        print("  AMBIGUOUS: more than one candidate reproduces this identity")
        return 4
    return 0


def add_input_args(parser):
    parser.add_argument("--name", action="append",
                        help="a candidate filename as an argv string")
    parser.add_argument("--hex", action="append",
                        help="candidate bytes as hex (canonical for "
                             "arbitrary bytes)")
    parser.add_argument("--names-from", action="append", metavar="FILE",
                        help="LF-separated candidate names")
    parser.add_argument("--nul-from", action="append", metavar="FILE",
                        help="NUL-separated candidate names (filesystem "
                             "names only)")
    parser.add_argument("--manifest", action="append", metavar="FILE",
                        help="tokenize a test manifest and offer each "
                             "entry's exact name span")
    parser.add_argument("--kernel-source", metavar="FILE",
                        help="where to read the launcher's manifest size cap "
                             "from (default: src/kernel/test/test_usermode.c "
                             "beside this script)")


def main(argv):
    parser = argparse.ArgumentParser(
        prog="utest-refusal-id",
        description="Correlate a refused_<n>_<prefix>_<8hex>.exe identity "
                    "back to the filename that produced it.")
    sub = parser.add_subparsers(dest="command", required=True)

    p_digest = sub.add_parser("digest",
                              help="print the 8-hex digest of each input")
    add_input_args(p_digest)
    p_digest.set_defaults(func=cmd_digest)

    p_match = sub.add_parser("match",
                             help="report which candidates produce an identity")
    p_match.add_argument("identity",
                         help="the refused_<n>_<prefix>_<8hex>.exe name from "
                              "the artifact, or an agg_<label>_<value>.exe "
                              "aggregate (classified, not correlated -- "
                              "exit 5, no candidate inputs needed)")
    add_input_args(p_match)
    p_match.set_defaults(func=cmd_match)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except ValueError as exc:
        sys.stderr.write("utest-refusal-id: %s\n" % (exc,))
        return 2
    except OSError as exc:
        sys.stderr.write("utest-refusal-id: %s\n" % (exc,))
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
