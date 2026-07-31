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

Commands:
  digest   <input>            print the 8-hex digest of one candidate
  match    <identity> <input> report which candidates produce that identity

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
        raise ValueError(
            "not a refusal identity: expected "
            "refused_<ordinal>_<prefix>_<8 hex digits>.exe, got %r" % (text,))
    prefix = match.group("prefix").encode("utf-8", "surrogateescape")
    if not prefix:
        raise ValueError(
            "refusal identity carries an EMPTY prefix, which u_build_refusal_id "
            "never emits (its static assert reserves a readable minimum): %r"
            % (text,))
    if sanitize(prefix) != prefix:
        raise ValueError(
            "refusal identity prefix carries a byte outside the accepted "
            "charset, which u_build_refusal_id renders `_`: %r" % (text,))
    return (int(match.group("ordinal")), prefix,
            int(match.group("digest"), 16))


def candidate_matches(raw, prefix, digest):
    """How `raw` matches this identity: None, "exact", or "truncated".

    Two independent checks, because neither is sufficient alone: the digest
    is 32 bits and explicitly not collision-resistant, and the prefix is
    sanitized and truncated to whatever the ordinal left. The prefix is
    compared over its OWN length -- it is a truncation of the candidate,
    not the whole of it -- and the caller is told WHICH of the two it was,
    because only an exact prefix accounts for the whole candidate.
    """
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
                              "the artifact")
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
