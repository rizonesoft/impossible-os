#!/usr/bin/env python3
"""Reassemble per-binary ring-3 output from framed capture records.

The host half of the usermode test framework's per-binary output artifacts and
fail-closed byte reconciliation. The kernel's source-level output framing wraps
ring-3 stdout where it is PRODUCED and emits it as authenticated klog records:

    [UTEST-CAPTURE-EXPECT] owner=<pid> name=<binary.exe>
    [UTEST-CAPTURE-BEGIN] owner=<pid> chunk_max=<n> name=<binary.exe>
    [UTEST-CAPTURE] owner=<pid> wr=<id> seq=<n> len=<raw> final=<0|1> <escaped>
    [UTEST-CAPTURE-LOST] owner=<pid> len=unknown
    [UTEST-CAPTURE-PENDING] run=<n> pending=<n>
    [UTEST-CAPTURE-UNREAPED] owner=<pid> live=<n>

The last two are the RUN BOUNDARY's own reports, and both refuse the run they
appear in rather than annotating it. The launcher waits on one pid per binary
while fork() hands the same capture channel to every descendant, so at the end
of a run it seals capture admission, drains the claims already outstanding,
and reaps the descendant tree. PENDING says the drain ended with records
claimed but never emitted -- holes in an owner's sequence, named and charged
to the run that drew them instead of surfacing as corruption in whichever run
follows. UNREAPED says descendants outlived the reap's round cap; their future
records are already fenced by the owner's stop latch, so it is a resource
signal rather than a wire one, and still not a clean run.

`len` counts RAW (pre-escape) bytes. `<escaped>` rewrites `\\`, `[`, and every
byte < 0x20 or >= 0x7F as `\\xHH` (src/kernel/test/test_usermode.c, the
`u_capture_escape` comment block), so a payload can never forge a record
boundary and never survives the host's UTF-8 line decode mangled.

WHY THIS IS ONE MODULE AND NOT TWO CHECKS. Both artifacts -- the JUnit XML
assembled by scripts/test.sh and the JSON built by scripts/utest-json-harvest.py
-- need the same reassembled payload and the same verdict on whether it is
trustworthy. Two independent implementations would eventually disagree about
whether a run is corrupt, which is the one thing a fail-closed reconciliation
must never do. So this module parses ONCE, from ONE canonical run slice, and
publishes a model both renderers consume.

RECONCILIATION IS FAIL-CLOSED. With source framing the kernel's declared byte
count and the host's decoded byte count cover the identical byte range, so a
mismatch is corruption, not the ordinary interleaving that made the older
delimiter design unprovable. Per owner:

  * chunks are ordered by `seq`, NEVER by physical position in the stream --
    klog releases its ring lock before the serial write, so two concurrent
    writers' records can land on the wire in either order;
  * `seq` starts at 0 (every owner constructor runs TASK_UTEST_CAPTURE_RESET)
    and every subsequent value is exactly +1. Accepting the first OBSERVED seq
    as the base would let a lost seq=0 record pass as a complete payload;
  * `final=1` terminates ONE write() call, not the owner's lifetime, so a
    binary calling write() five times legitimately emits five final records.
    EVERY write is reconciled independently, keyed by the `wr` identity the
    producer stamps on each record (the sequence number of that write's first
    chunk, so `wr` is always its group's lowest seq). Checking only that the
    owner's HIGHEST record was final let a second writer sharing the owner --
    a fork descendant, or a second thread -- MASK an earlier truncated write:
    A emits `seq=0 final=0` and dies, B emits `seq=1 final=1`, and the owner
    looked contiguous and terminated. Now A's group has no final record and
    the run refuses;
  * a BEGIN with zero chunks is VALID: a binary that writes nothing is not a
    corrupt binary. (`u_capture_emit_chunk` returns before consuming a seq when
    raw_len == 0, so an empty write leaves no gap either.);
  * each chunk's decoded length must equal its declared `len`, and neither may
    exceed the `chunk_max` the producer ANNOUNCED on that owner's BEGIN. The
    bound is derived kernel-side (the record line cap, minus the record's own
    fixed literal cost, over the worst-case escape expansion), so mirroring
    today's value as a host constant would refuse EVERY run the day any of
    those three inputs moved -- a worse failure than the over-long chunk the
    check exists to reject. Every announcement in one run must agree;
  * an explicit [UTEST-CAPTURE-LOST] record for an owner is a producer-declared
    loss -- bytes went out unframed -- so the payload is known-incomplete.

CAPTURE MUST BE PROVEN ARMED, NOT ASSUMED. A BEGIN record is emitted by the
arming path itself, so a producer regression that stops arming capture removes
the binding too, leaving an EMPTY model that both artifacts happily read as a
run of ordinary output-free testcases -- and the frame record count still
reconciles, because those records were never produced. Inferring the
expectation host-side cannot close it either: that would refuse legitimate
launch-failure rows, which have no capture channel by construction.

So the LAUNCHER publishes [UTEST-CAPTURE-EXPECT] per spawned binary, and this
module requires every expected owner to have a BEGIN bound to the same name.
The expectation set is itself falsifiable: [UTEST-FRAME-END] carries the run's
own `spawned=` count, raised at a DIFFERENT call site than the one that emits
the records, and the two must agree exactly. Deleting either half alone is a
mismatch that refuses the run rather than a silently empty expected set.

BOUNDING IS SEPARATE FROM RECONCILIATION, AND HAPPENS AFTER IT. Reconciliation
validates the ENTIRE payload; only the artifact COPY is bounded, and never
silently -- a silent cut would make the artifact disagree with its own byte
count. Retention is streaming: the full payload is counted and validated but
only the bounded prefix is ever held, so a binary emitting a megabyte cannot
cost a megabyte of host memory per binary. Both a per-binary cap and a run-wide
aggregate cap apply -- the per-binary cap alone does not bound a run with
hundreds of binaries.

Truncation is reported in THREE representations, which are deliberately named
differently and must not be confused:

  * this module's MODEL (the JSON written by `model`), per binary:
    `total_bytes`, `retained_bytes`, `truncated`, `truncated_bytes`, plus
    `budget_stop` (and `budget_scope`/`budget_limit` when it is set);
  * the public JSON test artifact (scripts/utest-json-harvest.py), per testcase:
    `captured_output`, `captured_bytes` (= total), `captured_retained_bytes`,
    `captured_truncated`, `captured_truncated_bytes`, `captured_budget_stop`
    (and `captured_budget_scope`/`captured_budget_limit`);
  * the JUnit XML: `<system-out>` carries the TEXT ONLY -- byte-identical to
    `captured_output` -- and truncation rides as a SUITE-level property
    `capture.truncated` whose value is a comma-separated list of
    `<binary>:<retained>/<total>`, present only when something was truncated;
    a producer budget stop rides the separate `capture.budget_stop` property
    as `<binary>:<scope>@<limit>`.
    It is not on `<system-out>` because the Jenkins/xunit junit-10.xsd models
    that element as string-only, so attributes there can make a validating
    consumer reject the whole report.

This module bounds the HOST; the kernel emitter bounds the PRODUCER, and the
two are reported separately on purpose. When a binary exhausts its per-owner
record budget -- or the run exhausts the aggregate one -- the emitter publishes
exactly one authenticated [UTEST-CAPTURE-OVER] record and stops capturing for
that owner. That is a BOUNDED payload, not a corrupt one: without the marker
the stream would simply stop mid-write and reconcile as `capture_unterminated`,
turning a deliberate policy stop into a corruption verdict and a red run.

The marker is TRUSTED ONLY AS FAR AS IT CAN BE CHECKED, because it is a record
that suppresses a refusal and is therefore worth forging:

  * it must be the owner's HIGHEST sequence number, and the chunk sequences
    below it must be contiguous from 0 -- the producer draws the seq, the
    budget verdict and the run charge under one lock precisely so this holds;
  * a `scope=owner` marker must sit at EXACTLY the limit it declares, so an
    early stop cannot claim a budget it never reached;
  * a `scope=run` marker is proven against the RUN, in three equalities: every
    run-scope marker declares the same limit, each declares `charged` equal to
    it, and the run carries exactly that many chunk records across all owners.
    An owner stopped by the aggregate can be far below its own limit, so
    nothing about its own stream proves the run budget was reached. All three
    are equalities on purpose -- the producer freezes the aggregate at the
    limit under one lock, so nothing can be charged afterwards, and an earlier
    "at least" formulation of this rule admitted a forged early terminator.

`budget_stop` is reported as its OWN field rather than folded into
`truncated`. The producer cannot know how many bytes the binary went on to
write after the stop, and `truncated_bytes` is an exact contract
(`total_bytes - retained_bytes`); publishing a producer stop through it would
put a false exact number in a field consumers do arithmetic on.

Exit codes (both subcommands):
    0  model written / splice written -- capture is reconciled and trustworthy
    1  REFUSED: reconciliation failed; the model records the reason and the
       caller must publish a refusal artifact and fail the run
    2  usage or I/O error
"""
from __future__ import annotations

import json
import os
import re
import sys
import tempfile
import xml.etree.ElementTree as ET

SCHEMA = "utest-capture-v1"

# Retention caps. Per-binary matches the 64 KiB bound utest-json-harvest.py
# already uses for the identity document; the aggregate cap keeps a run with
# many chatty binaries from turning the artifact into the log it replaces.
PER_BINARY_CAP = 64 * 1024
RUN_AGGREGATE_CAP = 1024 * 1024

# A run slice is bounded upstream (utest-frame.py caps a physical line at 1 MiB
# and refuses a run whose line overflows), but this module reads the slice
# directly, so it carries its own file bound.
SLICE_MAX_BYTES = 256 * 1024 * 1024

# INPUT bounds, enforced while scanning -- distinct from the RETENTION caps
# above, which only bound what reaches the artifact. Retention alone does not
# bound this PROCESS: every record's escaped payload and seq is held before
# anything can be capped, because out-of-order records mean the prefix to keep
# is unknown until the whole owner has been seen. A binary emitting enough
# valid sub-256-byte records could therefore drive host memory well past the
# slice size. Past these bounds the run REFUSES rather than degrades: a payload
# this large is abusive rather than chatty, and silently dropping it would
# publish an artifact whose byte counts nobody validated.
MAX_RECORDS = 200000
MAX_ESCAPED_BYTES = 64 * 1024 * 1024

# Distinct owners, bounded separately. BEGIN and LOST records carry no payload,
# so they cost nothing against MAX_ESCAPED_BYTES, but each DISTINCT pid retains
# an _Owner -- measured at ~127 MiB RSS for 400,000 BEGIN records, and the
# shortest accepted BEGIN fits ~4.7M times under SLICE_MAX_BYTES. A run cannot
# legitimately spawn anything close to this: the launcher's own binary count is
# two orders of magnitude smaller.
MAX_OWNERS = 4096

# ANCHORED and EXACT: these are matched with fullmatch() against the record body
# (everything after the authenticated frame prefix), so they are the precise
# inverse of the producer's printf formats in test_usermode.c -- no leading
# garbage, no optional separator, no missing suffix.
#
# The separator before the payload is MANDATORY (the producer's format string is
# "... final=%u %s"). Leaving it optional accepted `final=1abc`, a record shape
# the kernel cannot emit, and quietly reinterpreted the first payload byte as
# part of the field. Likewise [UTEST-CAPTURE-LOST] always carries " len=unknown";
# matching without it would accept a truncated or version-skewed loss record.
# Numeric fields are CANONICAL ASCII uint32, matching the producer's `%u`:
# `[0-9]` rather than `\d` (which also matches Unicode digit characters), no
# leading zeros, and a range check below. `\d+` plus int() silently NORMALIZED
# non-producer forms -- `owner=07 seq=00 len=01` reconciled as a trustworthy
# one-byte capture -- which let a malformed authenticated record slip past the
# malformed-family detector by parsing successfully.
_U32 = r"(0|[1-9][0-9]{0,9})"
# The same canonical shape for a field the producer only ever emits NONZERO.
# It is a grammar rather than a range check on purpose: a zero there is a
# record the kernel cannot have written, which is a SHAPE fault, and shape
# faults outrank semantic ones in this module's refusal precedence.
#
# What this deliberately does NOT do is bound the value by a kernel constant
# (the run record budget, TASK_MAX). Mirroring a producer bound host-side is
# the drift this module refuses everywhere else -- the host learns the
# producer's bounds from records like `chunk_max=`, it does not hardcode
# them -- so a count above what the kernel can currently reach is left to the
# uint32 range check alongside every other numeric field.
_U32_POS = r"([1-9][0-9]{0,9})"
_BEGIN_RE = re.compile(
    r"\[UTEST-CAPTURE-BEGIN\] owner=" + _U32 + r" chunk_max=" + _U32
    + r" name=(\S+)"
)
# The launcher's spawn announcement. Same owner+name pair BEGIN carries, from a
# different producer call site on purpose -- see the module docstring.
_EXPECT_RE = re.compile(
    r"\[UTEST-CAPTURE-EXPECT\] owner=" + _U32 + r" name=(\S+)")
_CHUNK_RE = re.compile(
    r"\[UTEST-CAPTURE\] owner=" + _U32 + r" wr=" + _U32 + r" seq=" + _U32
    + r" len=" + _U32 + r" final=([01]) (.*)"
)
_LOST_RE = re.compile(r"\[UTEST-CAPTURE-LOST\] owner=" + _U32 + r" len=unknown")
# The run terminator, read ONLY for its `spawned=` count. Everything else about
# framing belongs to scripts/utest-frame.py and is not re-derived here.
_FRAME_END_RE = re.compile(
    r"\[UTEST-FRAME-END\] run=" + _U32 + r" records=" + _U32
    + r" spawned=" + _U32
)
# A terminator that does not carry the census MUST refuse rather than read as
# "no terminator". Matching only the strict form left the two states
# indistinguishable, so an old-form or field-stripped terminator silently
# dropped `spawned` to None -- and with no expectation records surviving
# either, the whole census evaporated and the stream reconciled green. This is
# the shape that recognises the record kind, so its absence of the field is
# reportable instead of invisible.
_FRAME_END_ANY = "[UTEST-FRAME-END]"
# The producer's emission-budget terminator. `scope` is an exact alternation
# rather than \w+: an unknown scope is a version skew the host must refuse, not
# a value to pass through into an artifact field consumers branch on.
_OVER_RE = re.compile(
    r"\[UTEST-CAPTURE-OVER\] owner=" + _U32 + r" seq=" + _U32
    + r" scope=(owner|run) limit=" + _U32 + r" charged=" + _U32
)
# One write that the owner's budget stop cut mid-stream. The OVER marker above
# says a stop happened; this says WHICH write it ended, and the distinction is
# the whole point: the producer latches the whole OWNER, so a single marker
# used to exempt every unterminated write that owner had open -- including one
# truncated for an unrelated reason, which is the masking `wr` was added to
# catch. Each cut write now names itself.
_CUT_RE = re.compile(
    r"\[UTEST-CAPTURE-CUT\] owner=" + _U32 + r" wr=" + _U32
)
# A write whose thread stopped existing while it was still open. `haswr=0` is
# the case this record exists for: a write killed before its first chunk ever
# reached the wire leaves no chunk, so no `wr` group exists for the host to
# find open, and without this record the artifact would claim byte-exactness
# for an owner whose payload is simply gone.
#
# `reason` is an exact alternation for the same reason `scope` is above: an
# unknown reason is version skew the host must refuse, not a value to pass
# through into a verdict consumers branch on. `killed` is a genuine loss and
# REFUSES; `fenced` is the launcher deliberately tearing down a descendant
# that outlived its binary, and exempts the write exactly as the reap
# behaviour always has.
#
# There is deliberately NO `sealed` reason. A write stopped by the run
# boundary sealing admission cannot produce a record at all -- the producer's
# terminal claim refuses once sealed -- so accepting the shape would put a
# vocabulary in this grammar that only a forgery could ever emit, and hand it
# an exemption. The boundary reports its own losses through the pending
# record instead.
_ABANDON_RE = re.compile(
    r"\[UTEST-CAPTURE-ABANDON\] owner=" + _U32 + r" task=" + _U32
    + r" thr=" + _U32 + r" haswr=([01]) wr=" + _U32
    + r" reason=(killed|fenced)"
)
# The run spent its whole terminal-record allowance. Bounding that wire cost
# must never become hiding the loss, so the producer emits this once and the
# host refuses the run: after it, an unknown number of writes went unexplained.
_TERMINAL_OVER_RE = re.compile(
    r"\[UTEST-CAPTURE-TERMINAL-OVER\] limit=" + _U32
)
# The run boundary's report of capture records it could not deliver. The
# producer seals admission, drains the claims already outstanding, and emits
# this INSIDE the originating run when any of them never reached the wire --
# so a hole in an owner's sequence has a named cause attributed to the run
# that drew it, instead of surfacing as unexplained corruption in whichever
# run happens to follow.
# `pending` uses the POSITIVE grammar, not the general one: the producer emits
# this record only when the close wrote something off (`if (pending)`), so
# `pending=0` is a shape it cannot produce. Accepting it would classify an
# impossible authenticated record as a semantic run failure instead of the
# malformed producer output it is -- the same inverse-of-the-printf rule the
# `_U32` block above exists to state.
_PENDING_RE = re.compile(
    r"\[UTEST-CAPTURE-PENDING\] run=" + _U32 + r" pending=" + _U32_POS)
# The run boundary's report of capture-owning descendants it could not reap
# within its round cap -- a producer forking faster than the launcher kills.
# Their FUTURE records are already fenced by the owner's stop latch, so this
# is a resource-leak signal rather than a wire-integrity one, but it is still
# a refusal: a run that leaves live tasks behind is not a clean run.
_UNREAPED_RE = re.compile(
    r"\[UTEST-CAPTURE-UNREAPED\] owner=" + _U32 + r" live=" + _U32_POS)

_UINT32_MAX = 0xFFFFFFFF

# Escape decoder. The producer emits `\xHH` and nothing else, so a bare
# backslash or a raw `[` in a payload is corruption by construction.
#
# LOWERCASE ONLY, and only for bytes the producer actually escapes. The kernel
# escaper indexes UTEST_HEX_DIGITS = "0123456789abcdef" (test_usermode.c) and
# takes the escape branch for exactly `\`, `[`, c < 0x20 and c >= 0x7F. A
# decoder that also accepted `\x41` or `\X0A` would NORMALIZE wire shapes the
# producer can never emit -- turning a corrupted record into clean text instead
# of reporting it.
_ESCAPE_RE = re.compile(r"\\x([0-9a-f]{2})")

# Any line carrying the authenticated frame prefix whose body starts with this
# belongs to the capture family and MUST parse. See _scan.
_CAPTURE_FAMILY = "[UTEST-CAPTURE"


def _producer_escapes(byte: int) -> bool:
    """True when the kernel escaper would have emitted \\xHH for this byte."""
    return byte in (0x5C, 0x5B) or byte < 0x20 or byte >= 0x7F


class Refusal(Exception):
    """A reconciliation failure. Carries the named reason the artifacts publish."""

    def __init__(self, reason: str, detail: str) -> None:
        super().__init__(f"{reason}: {detail}")
        self.reason = reason
        self.detail = detail


def _decode_payload(escaped: str, owner: int, seq: int) -> bytes:
    """Decode one chunk's escaped payload back to raw bytes.

    Rejects anything the producer could not have emitted rather than guessing:
    a lone backslash, a malformed \\xHH, or an unescaped `[`.
    """
    out = bytearray()
    i = 0
    n = len(escaped)
    while i < n:
        ch = escaped[i]
        if ch == "[":
            raise Refusal(
                "capture_unescaped_marker",
                f"owner={owner} seq={seq} payload contains an unescaped '[' "
                "-- the producer always escapes it as \\x5B",
            )
        if ch != "\\":
            # The producer escapes everything outside printable ASCII, so any
            # surviving character is a single-byte ASCII codepoint.
            code = ord(ch)
            if code < 0x20 or code >= 0x7F:
                raise Refusal(
                    "capture_unescaped_byte",
                    f"owner={owner} seq={seq} payload contains a raw 0x{code:02X} "
                    "byte the producer would have escaped",
                )
            out.append(code)
            i += 1
            continue
        m = _ESCAPE_RE.match(escaped, i)
        if not m:
            raise Refusal(
                "capture_bad_escape",
                f"owner={owner} seq={seq} has a backslash that does not begin a "
                "lowercase \\xHH escape",
            )
        value = int(m.group(1), 16)
        if not _producer_escapes(value):
            raise Refusal(
                "capture_noncanonical_escape",
                f"owner={owner} seq={seq} escapes 0x{value:02X}, which the "
                "producer emits literally -- the record is not in the shape the "
                "kernel escaper can produce",
            )
        out.append(value)
        i = m.end()
    return bytes(out)


def _artifact_text(payload: bytes) -> str:
    """Render raw captured bytes as artifact-safe text.

    Tab and LF survive as themselves because they carry the output's shape. CR
    does NOT: an XML parser normalizes a literal CR to LF (XML 1.0 section 2.11),
    so a payload containing CR would read back differently than it was captured.
    Everything else outside printable ASCII is re-escaped, which keeps the JSON
    string and the XML text node byte-identical to each other.
    """
    out = []
    for b in payload:
        if b in (0x09, 0x0A):
            out.append(chr(b))
        elif 0x20 <= b < 0x7F:
            out.append(chr(b))
        else:
            # Lowercase, matching the producer's own escape rendering, so a
            # payload read out of an artifact looks like the payload on serial.
            out.append(f"\\x{b:02x}")
    return "".join(out)


class _Owner:
    __slots__ = ("pid", "name", "chunks", "lost", "declared", "records", "over",
                 "chunk_max", "expect_name", "cuts", "abandons")

    def __init__(self, pid: int) -> None:
        self.pid = pid
        self.name = None
        self.chunks = {}  # seq -> (declared_len, final, escaped_payload, wr)
        self.lost = False
        self.declared = 0
        self.records = 0
        self.over = None  # (seq, scope, limit, charged) once terminated
        self.chunk_max = None    # raw-byte bound this owner's BEGIN announced
        self.expect_name = None  # name the launcher said it spawned
        self.cuts = set()        # wr values a budget stop cut mid-stream
        self.abandons = []       # (task, thr, haswr, wr, reason) per abandoned write


def _scan(lines, prefix):
    """Collect capture records from the canonical run slice.

    When `prefix` is given (the framed "UTEST-<nonce>: " marker), a line must
    carry it to be considered: the nonce is what makes a record non-forgeable,
    and this module must not weaken that by matching on the marker alone.

    Returns (owners, spawned), where `spawned` is the run terminator's own
    count of launched binaries, or None when the slice carries no terminator
    (an aborted run: there is then nothing to reconcile the expectation set
    against, and inventing a count would be worse than not checking one).
    """
    owners = {}
    records = 0
    escaped_bytes = 0
    spawned = None

    def _own(pid):
        """Resolve an owner slot under the distinct-owner bound.

        Every record kind goes through here. An earlier version allocated the
        slot with a bare setdefault() from each branch, so only CHUNK records
        were counted -- a flood of distinct BEGIN or LOST pids grew memory with
        nothing bounding it.
        """
        if pid > _UINT32_MAX:
            raise Refusal(
                "capture_field_out_of_range",
                f"owner={pid} is wider than the producer's uint32 field",
            )
        owner = owners.get(pid)
        if owner is None:
            if len(owners) >= MAX_OWNERS:
                raise Refusal(
                    "capture_owner_flood",
                    f"more than {MAX_OWNERS} distinct capture owners in one run",
                )
            owner = _Owner(pid)
            owners[pid] = owner
        return owner

    def _count(payload_len):
        """Count one authenticated capture-family record against the bounds."""
        nonlocal records, escaped_bytes
        records += 1
        escaped_bytes += payload_len
        if records > MAX_RECORDS:
            raise Refusal(
                "capture_record_flood",
                f"more than {MAX_RECORDS} capture records in one run",
            )
        if escaped_bytes > MAX_ESCAPED_BYTES:
            raise Refusal(
                "capture_volume_exceeded",
                f"more than {MAX_ESCAPED_BYTES} escaped payload bytes in one run",
            )

    for line in lines:
        if prefix:
            at = line.find(prefix)
            if at < 0:
                continue
            body = line[at + len(prefix):]
        else:
            body = line
        body = body.rstrip("\r\n")

        m = _CHUNK_RE.fullmatch(body)
        if m:
            pid, wr, seq, dlen = (int(m.group(i)) for i in (1, 2, 3, 4))
            final = int(m.group(5))
            payload = m.group(6)
            _count(len(payload))
            # The regex bounds each field to 10 digits; this rejects the values
            # in [10^9, 2^32) that 10 digits still admits but a uint32 cannot
            # hold, so no field can arrive wider than the producer's own type.
            if seq > _UINT32_MAX or dlen > _UINT32_MAX or wr > _UINT32_MAX:
                raise Refusal(
                    "capture_field_out_of_range",
                    f"owner={pid} wr={wr} seq={seq} len={dlen} carries a value "
                    "wider than the producer's uint32 fields",
                )
            # `wr` is the seq of its own write's first chunk, so a record can
            # never name a write that starts after it. Checked at scan time
            # because it is a property of the single record, not of the group.
            if wr > seq:
                raise Refusal(
                    "capture_write_identity",
                    f"owner={pid} seq={seq} claims write wr={wr}, which is "
                    "above its own sequence number",
                )
            # The producer returns BEFORE consuming a seq when a write stages no
            # bytes (test_usermode.c: "if (raw_len == 0) return"), so a len=0
            # record cannot come from the kernel escaper.
            if dlen == 0:
                raise Refusal(
                    "capture_empty_chunk",
                    f"owner={pid} seq={seq} declares len=0, which the producer "
                    "never emits",
                )
            owner = _own(pid)
            if seq in owner.chunks:
                raise Refusal(
                    "capture_duplicate_seq",
                    f"owner={pid} emitted seq={seq} twice",
                )
            owner.chunks[seq] = (dlen, final, payload, wr)
            owner.declared += dlen
            owner.records += 1
            continue

        m = _BEGIN_RE.fullmatch(body)
        if m:
            pid, chunk_max, name = int(m.group(1)), int(m.group(2)), m.group(3)
            _count(0)
            if chunk_max > _UINT32_MAX:
                raise Refusal(
                    "capture_field_out_of_range",
                    f"owner={pid} announces chunk_max={chunk_max}, wider than "
                    "the producer's uint32 field",
                )
            # A zero bound admits nothing, so no derived producer value can be
            # one -- and accepting it would make every chunk oversize, turning
            # a version skew into a flood of misattributed refusals.
            if chunk_max == 0:
                raise Refusal(
                    "capture_chunk_max_invalid",
                    f"owner={pid} announces chunk_max=0, which no derived "
                    "producer bound can be",
                )
            owner = _own(pid)
            # ANY second binding for one owner is refused, not just one that
            # renames it. The producer emits exactly one BEGIN per owner, at
            # task construction, and pids are monotonic within a boot and never
            # reused -- a fork descendant inherits ownership without a binding
            # of its own -- so a duplicate cannot come from the kernel at all.
            # Accepting a same-name repeat let the LAST record win the owner's
            # announced chunk bound: `chunk_max=1` then `chunk_max=42` followed
            # by a two-byte chunk reconciled green, with the bound check seeing
            # only the value the stream chose to leave last.
            if owner.name is not None:
                detail = (
                    f"owner={pid} bound to '{owner.name}' then to '{name}'"
                    if owner.name != name else
                    f"owner={pid} ('{name}') was bound twice, announcing "
                    f"chunk_max={owner.chunk_max} then chunk_max={chunk_max}"
                )
                raise Refusal("capture_owner_rebound", detail)
            owner.name = name
            owner.chunk_max = chunk_max
            continue

        m = _EXPECT_RE.fullmatch(body)
        if m:
            pid, name = int(m.group(1)), m.group(2)
            _count(0)
            owner = _own(pid)
            # One spawn, one announcement. A second one would inflate the
            # expectation set against the run's own spawn count, so it is a
            # producer contradiction rather than a harmless repeat.
            if owner.expect_name is not None:
                raise Refusal(
                    "capture_duplicate_expect",
                    f"owner={pid} was announced as spawned twice "
                    f"('{owner.expect_name}' then '{name}')",
                )
            owner.expect_name = name
            continue

        m = _LOST_RE.fullmatch(body)
        if m:
            pid = int(m.group(1))
            _count(0)
            _own(pid).lost = True
            continue

        m = _OVER_RE.fullmatch(body)
        if m:
            pid, seq = int(m.group(1)), int(m.group(2))
            scope = m.group(3)
            limit, charged = int(m.group(4)), int(m.group(5))
            _count(0)
            if seq > _UINT32_MAX or limit > _UINT32_MAX or charged > _UINT32_MAX:
                raise Refusal(
                    "capture_field_out_of_range",
                    f"owner={pid} overflow marker carries a value wider than "
                    "the producer's uint32 fields",
                )
            owner = _own(pid)
            # The producer latches the owner before emitting, so a second
            # marker cannot come from the kernel -- and accepting one would
            # let a later, weaker marker overwrite the checks the first must
            # satisfy.
            if owner.over is not None:
                raise Refusal(
                    "capture_duplicate_over",
                    f"owner={pid} emitted two overflow markers "
                    f"(seq={owner.over[0]} then seq={seq})",
                )
            owner.over = (seq, scope, limit, charged)
            continue

        m = _CUT_RE.fullmatch(body)
        if m:
            pid, wr = int(m.group(1)), int(m.group(2))
            _count(0)
            if wr > _UINT32_MAX:
                raise Refusal(
                    "capture_field_out_of_range",
                    f"owner={pid} cut record carries a wr wider than the "
                    "producer's uint32 field",
                )
            # A duplicate is not merely redundant: the producer settles a
            # write's evidence exactly once, so two cuts naming one write mean
            # either a forgery or that the settle-once invariant broke, and the
            # second would exempt a group the first already accounted for.
            owner = _own(pid)
            if wr in owner.cuts:
                raise Refusal(
                    "capture_duplicate_cut",
                    f"owner={pid} emitted two cut records for write wr={wr}",
                )
            owner.cuts.add(wr)
            continue

        m = _ABANDON_RE.fullmatch(body)
        if m:
            pid, task, thr = int(m.group(1)), int(m.group(2)), int(m.group(3))
            haswr, wr, reason = int(m.group(4)), int(m.group(5)), m.group(6)
            _count(0)
            if task > _UINT32_MAX or thr > _UINT32_MAX or wr > _UINT32_MAX:
                raise Refusal(
                    "capture_field_out_of_range",
                    f"owner={pid} abandon record carries a value wider than "
                    "the producer's uint32 fields",
                )
            # A write with no identity cannot also name one: `haswr=0` means
            # nothing reached the wire under this write, so a non-zero `wr`
            # beside it is a contradiction, and a zero one must not be read as
            # naming the group that legitimately holds wr=0.
            if not haswr and wr != 0:
                raise Refusal(
                    "capture_abandon_identity",
                    f"owner={pid} abandon record declares haswr=0 but names "
                    f"wr={wr}",
                )
            owner = _own(pid)
            # The producer settles each write exactly once, through a
            # compare-exchange on the thread's own evidence, so two abandons
            # for one thread cannot come from the kernel -- and a second could
            # exempt a group the first did not.
            if any(a[0] == task and a[1] == thr for a in owner.abandons):
                raise Refusal(
                    "capture_duplicate_abandon",
                    f"owner={pid} emitted two abandon records for "
                    f"task={task} thread={thr}",
                )
            owner.abandons.append((task, thr, haswr, wr, reason))
            continue

        m = _TERMINAL_OVER_RE.fullmatch(body)
        if m:
            _count(0)
            raise Refusal(
                "capture_terminal_budget_exhausted",
                f"the run spent its whole terminal-record allowance "
                f"(limit={int(m.group(1))}), so an unknown number of "
                "abandoned or cut writes went unexplained",
            )

        m = _PENDING_RE.fullmatch(body)
        if m:
            run, count = int(m.group(1)), int(m.group(2))
            _count(0)
            # Both fields, not just the interesting one: the grammar admits 10
            # digits, which reaches past a producer uint32, and an over-range
            # RUN would name a run ordinal the producer cannot have emitted
            # just as surely as an over-range count would.
            if run > _UINT32_MAX or count > _UINT32_MAX:
                raise Refusal(
                    "capture_field_out_of_range",
                    f"the pending report declares run={run} pending={count}, "
                    "wider than the producer's uint32 fields",
                )
            # Refused rather than recorded, and refused against THIS run --
            # which is the whole reason the producer emits it here instead of
            # letting the stale records surface in the next run. Each pending
            # record is a sequence number drawn for a chunk that never
            # arrived, so this owner's stream has holes; accepting it would
            # publish a capture artifact that silently omits output the
            # binary produced.
            raise Refusal(
                "capture_pending_records",
                f"run={run} sealed with {count} capture record(s) claimed but "
                "never emitted -- the run's capture streams are incomplete",
            )

        m = _UNREAPED_RE.fullmatch(body)
        if m:
            pid, live = int(m.group(1)), int(m.group(2))
            _count(0)
            if pid > _UINT32_MAX or live > _UINT32_MAX:
                raise Refusal(
                    "capture_field_out_of_range",
                    f"the unreaped report declares owner={pid} live={live}, "
                    "wider than the producer's uint32 fields",
                )
            raise Refusal(
                "capture_unreaped_descendants",
                f"owner={pid} left {live} capture-owning descendant(s) alive "
                "at the run boundary",
            )

        # The run terminator is NOT capture-family, so a slice without one is
        # not corruption here -- it simply leaves `spawned` unknown. A
        # terminator that IS present must carry the census, though: without
        # this branch an old-form or field-stripped one parsed as silence.
        if _FRAME_END_ANY in body and not _FRAME_END_RE.fullmatch(body):
            raise Refusal(
                "capture_spawn_count_missing",
                "the run terminator carries no spawn census: "
                f"{body[:120]!r}",
            )

        m = _FRAME_END_RE.fullmatch(body)
        if m:
            count = int(m.group(3))
            if count > _UINT32_MAX:
                raise Refusal(
                    "capture_field_out_of_range",
                    f"the run terminator declares spawned={count}, wider than "
                    "the producer's uint32 field",
                )
            # utest-frame.py already refuses a run that re-announces without
            # terminating, so two terminators in one slice cannot both belong
            # to this run -- and a second, smaller count would be exactly the
            # forgery the expectation check exists to catch.
            if spawned is not None and spawned != count:
                raise Refusal(
                    "capture_spawn_count_conflict",
                    f"the run slice carries two terminators declaring "
                    f"spawned={spawned} and spawned={count}",
                )
            spawned = count
            continue

        # An AUTHENTICATED capture-family line that parses as none of the above
        # is corruption, and it must never be skipped. Skipping it made a
        # damaged record indistinguishable from silence: a BEGIN followed only
        # by an unparseable chunk (say `final=2`) reconciled as a valid binary
        # that wrote nothing. Frame-level record counting does not catch it
        # either -- the line still carries the nonce, so it counts as a record.
        #
        # The marker is looked for ANYWHERE in the body, not just at the start,
        # so a record with leading garbage is refused rather than ignored. That
        # is safe precisely because the producer escapes `[` as \x5b in every
        # payload: a well-formed framed line cannot contain this marker unless
        # it IS one of these records. Verified against a live 1518-record slice,
        # where "contains" and "starts with" select the identical set.
        if _CAPTURE_FAMILY in body:
            raise Refusal(
                "capture_malformed_record",
                f"authenticated capture-family record does not parse: {body[:120]!r}",
            )
    return owners, spawned


def _check_over(owner):
    """Validate one owner's producer-budget terminator.

    This record SUPPRESSES the `capture_unterminated` refusal, so it is the one
    record kind a forger gains something by emitting. Authentication (the frame
    nonce) proves it came from the kernel; these checks prove it came from the
    kernel's BUDGET path, at the point that path can actually be reached.
    """
    seq, scope, limit, charged = owner.over
    highest_chunk = max(owner.chunks) if owner.chunks else None

    # Both derived budgets are necessarily positive (each is a ceiling division
    # of a non-zero raw allowance), so a zero limit cannot come from the
    # producer at all. Checked FIRST because every rule below is relative to
    # the limit: with limit=0 an owner-scope marker at seq=0 satisfies the
    # equality vacuously, and a BEGIN plus a bare marker reconciled green as a
    # bounded stop with an empty payload -- a malformed or version-skewed
    # producer suppressing capture_unterminated on a budget nothing reached.
    if limit == 0:
        raise Refusal(
            "capture_budget_unreached",
            f"owner={owner.pid} ({owner.name}) declares a {scope}-scope stop "
            "at limit=0, which no derived producer budget can be",
        )

    # The producer draws the seq, the verdict and the run charge under one
    # lock, so the terminator is always the owner's highest drawn number and
    # the chunks below it are contiguous from 0. A marker sitting below a
    # chunk means either a forgery or that invariant breaking; both refuse.
    if highest_chunk is not None and seq <= highest_chunk:
        raise Refusal(
            "capture_over_not_highest",
            f"owner={owner.pid} ({owner.name}) overflow marker at seq={seq} is "
            f"not above its highest chunk seq={highest_chunk}",
        )
    expected = 0 if highest_chunk is None else highest_chunk + 1
    if seq != expected:
        raise Refusal(
            "capture_over_seq_gap",
            f"owner={owner.pid} ({owner.name}) overflow marker at seq={seq} "
            f"leaves a hole: the chunks end at seq={expected - 1}",
        )
    # An owner-scope stop is drawn at exactly the budget, never above it: the
    # latch fires on the first claim that finds the counter AT the limit.
    # Equality is what makes "the budget was really reached" checkable rather
    # than merely asserted by the record that benefits from the claim.
    if scope == "owner" and seq != limit:
        raise Refusal(
            "capture_budget_unreached",
            f"owner={owner.pid} ({owner.name}) declares an owner-scope stop at "
            f"limit={limit} but terminated at seq={seq}",
        )
    # `charged` is the RUN's aggregate at the moment this owner stopped, so it
    # has a feasible floor even for an owner-scope stop: this owner's own
    # chunks were each charged against that aggregate before the stop, so the
    # run cannot have charged fewer records than this one binary emitted. A
    # marker below its own floor is contradicting itself.
    if charged < len(owner.chunks):
        raise Refusal(
            "capture_budget_unreached",
            f"owner={owner.pid} ({owner.name}) declares charged={charged} for "
            f"the run but emitted {len(owner.chunks)} chunk(s) of its own, "
            "each of which was charged against that same aggregate",
        )


def _check_writes(owner, seqs):
    """Reconcile each write() of one owner independently.

    Chunks are grouped by the `wr` identity the producer stamps on them, and
    every group must be a whole write: exactly one `final=1` record, and that
    record the group's LAST. Checking only the owner's highest record (which
    the caller still does, for its more precise message) let a concurrent
    writer sharing the owner mask an earlier truncated write.

    An unterminated write is exempt only when the producer NAMED it: a
    [UTEST-CAPTURE-CUT] for a write the owner's budget stop ended, or an
    [UTEST-CAPTURE-ABANDON] whose reason is a deliberate harness teardown.

    That per-write evidence replaces an owner-wide exemption on the
    [UTEST-CAPTURE-OVER] marker. The marker carries no `wr`, so once any
    budget stop fired for an owner it silenced EVERY unterminated write that
    owner had open -- including one truncated for an entirely unrelated
    reason. A captured fork descendant killed mid-write left an open group,
    and a later owner-budget stop retroactively excused it: exactly the
    masking that per-write identity was introduced to catch, reintroduced by
    the exemption. Naming each cut write keeps the deliberate policy stop from
    reading as corruption without extending its cover to anything else.

    An abandon with reason=killed is NOT an exemption -- it is the loss being
    reported -- and is refused by the caller whether or not the write it names
    ever reached the wire.
    """
    writes = {}
    for seq in seqs:
        writes.setdefault(owner.chunks[seq][3], []).append(seq)

    # An exemption must PROVE its cause, not merely assert it.
    #
    # A CUT says "the owner's budget stop ended this write", so the owner must
    # actually carry a validated [UTEST-CAPTURE-OVER]: without that check a
    # single standalone CUT -- one line -- suppresses the integrity failure it
    # claims to explain, and a stream with no budget stop at all reconciles
    # green. The marker is validated by _check_over before this runs.
    #
    # A CUT must also name a write that EXISTS and is genuinely open. One
    # naming a closed group is contradicting the final record beside it, and
    # one naming no group at all is an orphan whose only effect could be to
    # excuse something later.
    cut_wr = set()
    for wr in sorted(owner.cuts):
        if wr not in writes:
            raise Refusal(
                "capture_cut_orphan",
                f"owner={owner.pid} ({owner.name}) declares write wr={wr} cut, "
                "but no chunk carries that write identity",
            )
        cut_wr.add(wr)

    # Writes an abandon record explains as a deliberate teardown rather than a
    # loss. `haswr=0` entries name no group (the caller reports them), and a
    # named one must correspond to a real write for the same reason a CUT must.
    exempt_wr = set()
    for (_task, _thr, haswr, wr, reason) in owner.abandons:
        if not haswr or reason != "fenced":
            continue
        if wr not in writes:
            raise Refusal(
                "capture_abandon_orphan",
                f"owner={owner.pid} ({owner.name}) declares write wr={wr} "
                "abandoned, but no chunk carries that write identity",
            )
        exempt_wr.add(wr)

    for wr in sorted(writes):
        members = writes[wr]
        # `wr` is the seq of the write's own first chunk, so it is always the
        # group's lowest member. Without this a stream could label unrelated
        # records into one group and present a single trailing final as
        # closure for all of them.
        if wr != members[0]:
            raise Refusal(
                "capture_write_identity",
                f"owner={owner.pid} ({owner.name}) write wr={wr} starts at "
                f"seq={members[0]} -- the identity must be the write's own "
                "first chunk",
            )
        finals = [seq for seq in members if owner.chunks[seq][1] == 1]
        if len(finals) > 1:
            raise Refusal(
                "capture_write_multi_final",
                f"owner={owner.pid} ({owner.name}) write wr={wr} carries "
                f"{len(finals)} final records (seq="
                + ", ".join(str(s) for s in finals) + ")",
            )
        if finals and finals[0] != members[-1]:
            raise Refusal(
                "capture_write_multi_final",
                f"owner={owner.pid} ({owner.name}) write wr={wr} terminates at "
                f"seq={finals[0]} but continues to seq={members[-1]}",
            )
        if not finals and wr not in cut_wr and wr not in exempt_wr:
            raise Refusal(
                "capture_unterminated",
                f"owner={owner.pid} ({owner.name}) write wr={wr} ends at "
                f"seq={members[-1]} with no final record and no record "
                "explaining what stopped it -- that write's tail is missing",
            )


def _check_expectations(owners, spawned):
    """Prove the run's capture channels were armed, not merely absent.

    Two independent producer facts have to agree: the per-spawn
    [UTEST-CAPTURE-EXPECT] records, and the run terminator's own `spawned=`
    count, which is raised at a different call site. A regression that stops
    arming capture takes the BEGIN binding with it and would otherwise leave
    an empty, perfectly reconcilable model.
    """
    expected = [o for o in owners.values() if o.expect_name is not None]

    if spawned is None:
        # No terminator in the slice. In production this is not a state a
        # stream can choose: test.sh only builds a model for a run
        # utest-frame.py already certified COMPLETE, and a run missing its
        # terminator is incomplete and fails upstream -- so the census cannot
        # be evaded by dropping the LINE, only by dropping the FIELD, which
        # _scan refuses. What is left here is the fixture and aborted-slice
        # case: with no count the population cannot be reconciled, so an
        # expectation set that cannot be checked is refused rather than
        # assumed whole.
        if expected:
            raise Refusal(
                "capture_spawn_count_missing",
                f"{len(expected)} binaries announced a capture channel but the "
                "run terminator declares no spawn count to reconcile them "
                "against",
            )
        return
    if len(expected) != spawned:
        raise Refusal(
            "capture_spawn_count_mismatch",
            f"the run declares spawned={spawned} but {len(expected)} binaries "
            "announced a capture channel",
        )


def _check_owner_expectation(owner, spawned):
    """The per-owner half of the expectation contract.

    Called from _reconcile at its established precedence -- AFTER the
    producer-declared loss check, which is the stronger signal about the same
    owner -- so adding these verdicts cannot demote an older, more precise
    diagnosis to a newer, vaguer one.
    """
    if owner.expect_name is not None:
        if owner.name is None:
            raise Refusal(
                "capture_channel_missing",
                f"owner={owner.pid} was spawned as '{owner.expect_name}' but "
                "no [UTEST-CAPTURE-BEGIN] armed a capture channel for it",
            )
        if owner.name != owner.expect_name:
            raise Refusal(
                "capture_expect_mismatch",
                f"owner={owner.pid} was spawned as '{owner.expect_name}' but "
                f"its capture channel is bound to '{owner.name}'",
            )
        return

    # The REVERSE invariant. EXPECT -> BEGIN alone is fail-open: a regression
    # that drops an owner's expectation AND its spawn increment together
    # leaves a SELF-CONSISTENT census -- `spawned=0` beside one bound owner
    # still publishing its capture as trustworthy -- which is population drift
    # no count can see. Gated on the census being present, because a slice
    # with no terminator carries no population statement to check a binding
    # against and refusing there would refuse the aborted-run shape too.
    if spawned is not None and owner.name is not None:
        raise Refusal(
            "capture_unexpected_channel",
            f"owner={owner.pid} ('{owner.name}') armed a capture channel that "
            "the run never announced as spawned",
        )


def _check_chunk_max_agreement(owners):
    """One run, one derivation.

    Every BEGIN in a slice carries the same compile-time constant, so a
    disagreement is a mixed or forged stream. Run-level, and evaluated after
    every owner's own reconciliation, so it cannot pre-empt a per-owner loss
    or corruption verdict.
    """
    announced = {o.chunk_max for o in owners.values() if o.chunk_max is not None}
    if len(announced) > 1:
        raise Refusal(
            "capture_chunk_max_conflict",
            "capture channels announce different chunk bounds: "
            + ", ".join(str(v) for v in sorted(announced)),
        )


def _check_owner_chunk_bound(owner):
    """Enforce the raw-byte chunk bound this owner's BEGIN announced."""
    if owner.chunk_max is None:
        # No BEGIN: _reconcile already refused the owner as unbound, which is
        # the more precise verdict. Nothing to bound here.
        return
    for seq in sorted(owner.chunks):
        declared = owner.chunks[seq][0]
        if declared > owner.chunk_max:
            raise Refusal(
                "capture_chunk_oversize",
                f"owner={owner.pid} ({owner.name}) seq={seq} declares "
                f"{declared} raw byte(s), above the {owner.chunk_max} the "
                "producer announced",
            )


def _reconcile(owner, spawned):
    """Validate one owner's records and return (retained_prefix, total_bytes).

    Streaming by construction: the full payload is decoded and counted chunk by
    chunk, but only the bounded prefix is retained.

    The ORDER of the checks below is the diagnosis contract, not an accident:
    a producer-declared loss outranks the expectation contract, then
    ownership, then the budget terminator, then sequence integrity, then
    per-write closure, then the announced chunk bound, and only then the
    payload decode. Verdicts added later slot into that order rather than in
    front of it -- a newer, vaguer reason must never displace an older, more
    precise one on the same owner.

    That contract governs RECONCILIATION, and deliberately does not extend
    backwards over _scan. A record whose SHAPE the producer cannot emit is
    rejected while reading, before any owner-level reasoning exists to have a
    precedence -- which is why `capture_malformed_record`, `capture_
    empty_chunk`, `capture_duplicate_seq`, `capture_field_out_of_range` and
    `capture_owner_rebound` have always outranked an owner's own loss record,
    and why the shape checks added since (an out-of-range `wr`, a zero
    `chunk_max`, a repeated binding or expectation, a terminator with no
    census) sit beside them rather than here. Naming the one record that
    cannot have come from the kernel is the more precise diagnosis, and
    splitting that class across two phases would make the module report two
    identical malformed-record faults at two different precedences.
    """
    if owner.lost:
        raise Refusal(
            "capture_lost",
            f"owner={owner.pid} ({owner.name or 'unbound'}) emitted an explicit "
            "loss record -- part of its output went to serial unframed",
        )
    # A write whose thread was KILLED while it was still open. Checked before
    # the payload reconciliation below because it is a fact ABOUT the payload:
    # every check that follows reasons over the records that reached the wire,
    # and this record's whole purpose is to report bytes that never did. A
    # write killed before its first chunk flushed leaves no group, no hole and
    # no unterminated tail, so without this the stream reconciles clean and the
    # artifact claims byte-exactness for an owner whose output is simply gone.
    #
    # `fenced` and `sealed` are excluded on purpose: those are the launcher's
    # end-of-binary descendant reap and the run boundary sealing admission,
    # both deliberate, and both already reported by the mechanisms that own
    # them.
    killed = [a for a in owner.abandons if a[4] == "killed"]
    if killed:
        task, thr, haswr, wr, _reason = killed[0]
        where = f"write wr={wr}" if haswr else "a write that never reached the wire"
        raise Refusal(
            "capture_abandoned_write",
            f"owner={owner.pid} ({owner.name or 'unbound'}) abandoned "
            f"{where}: task={task} thread={thr} died with the write still "
            f"open ({len(killed)} abandoned write(s) in total)",
        )
    _check_owner_expectation(owner, spawned)
    if owner.name is None:
        raise Refusal(
            "capture_unbound_owner",
            f"owner={owner.pid} produced {owner.records} capture record(s) with "
            "no [UTEST-CAPTURE-BEGIN] binding it to a binary",
        )
    if owner.over is not None:
        _check_over(owner)
    # A CUT asserts "this owner's budget stop ended that write", so the owner
    # must actually carry the validated marker saying a stop happened. Checked
    # HERE, ahead of every payload rule below, because an unbacked cut is a
    # record that cannot have come from the producer at all -- the same class
    # as a malformed record, and a more precise finding than the unterminated
    # write it would otherwise be reported as.
    if owner.cuts and owner.over is None:
        raise Refusal(
            "capture_cut_unbacked",
            f"owner={owner.pid} ({owner.name}) declares write "
            f"wr={min(owner.cuts)} cut by a budget stop, but emitted no "
            "overflow marker",
        )
    if not owner.chunks:
        return b"", 0

    seqs = sorted(owner.chunks)
    if seqs[0] != 0:
        raise Refusal(
            "capture_missing_head",
            f"owner={owner.pid} ({owner.name}) starts at seq={seqs[0]}, not 0 -- "
            "the leading record(s) never reached the wire",
        )
    for prev, cur in zip(seqs, seqs[1:]):
        if cur != prev + 1:
            raise Refusal(
                "capture_seq_gap",
                f"owner={owner.pid} ({owner.name}) jumps seq={prev} -> seq={cur}",
            )
    _last_wr = owner.chunks[seqs[-1]][3]
    _last_explained = (
        (_last_wr in owner.cuts and owner.over is not None)
        or any(haswr and wr == _last_wr and reason == "fenced"
               for (_t, _h, haswr, wr, reason) in owner.abandons)
    )
    if owner.over is None and not _last_explained \
            and owner.chunks[seqs[-1]][1] != 1:
        # Only meaningful for a stream the producer did NOT terminate itself:
        # a budget stop legitimately cuts a write mid-sequence, and the marker
        # (already validated above) is that stream's terminator. A per-write
        # cut or a deliberate-teardown abandon terminates this owner's LAST
        # write just as surely, and without them a fenced descendant's final
        # partial write would refuse a run that was torn down exactly as
        # designed.
        raise Refusal(
            "capture_unterminated",
            f"owner={owner.pid} ({owner.name}) highest record seq={seqs[-1]} is "
            "not final -- the last write's tail is missing",
        )
    _check_writes(owner, seqs)
    _check_owner_chunk_bound(owner)

    retained = bytearray()
    total = 0
    for seq in seqs:
        declared, _final, escaped, _wr = owner.chunks[seq]
        raw = _decode_payload(escaped, owner.pid, seq)
        if len(raw) != declared:
            raise Refusal(
                "capture_byte_mismatch",
                f"owner={owner.pid} ({owner.name}) seq={seq} declared {declared} "
                f"byte(s) but decoded {len(raw)}",
            )
        total += len(raw)
        room = PER_BINARY_CAP - len(retained)
        if room > 0:
            retained.extend(raw[:room])
    return bytes(retained), total


def _refusal_model(exc):
    return {
        "schema": SCHEMA,
        "ok": False,
        "refusal": {"reason": exc.reason, "detail": exc.detail},
        "binaries": [],
        "aggregate_bytes": 0,
        "aggregate_truncated": False,
    }


def _check_run_budget(owners):
    """Prove a run-scope stop against the whole run, not one owner's stream.

    An owner stopped by the AGGREGATE budget can be arbitrarily far below its
    own limit, so nothing in its own sequence shows the run budget was reached
    -- `_check_over`'s equality test does not apply and cannot. The proof has
    to come from the run: every run-scope marker must agree on the limit, must
    declare a `charged` count equal to it, and the run must carry exactly that
    many chunk records across all owners. An early or forged run stop then
    fails on a count no single binary controls.

    All three are EQUALITIES, deliberately. The producer charges the aggregate
    under one lock and latches at exactly the limit, so a real run stop has
    `charged == limit` and leaves exactly `limit` chunks on the wire; nothing
    can be charged afterwards, because the latch is what the next claim reads.
    An earlier version of this check accepted `observed >= limit` and ignored
    `charged` entirely, which let two chunks plus a `limit=1 charged=0` marker
    reconcile green -- a terminator that suppresses `capture_unterminated` on
    a budget the run never reached is exactly the forgery this exists to stop.
    """
    run_overs = [o.over for o in owners.values()
                 if o.over is not None and o.over[1] == "run"]
    if not run_overs:
        return
    limits = {over[2] for over in run_overs}
    if len(limits) > 1:
        raise Refusal(
            "capture_run_limit_conflict",
            "run-scope overflow markers disagree on the aggregate limit: "
            + ", ".join(str(v) for v in sorted(limits)),
        )
    limit = limits.pop()
    for seq, _scope, _limit, charged in run_overs:
        if charged != limit:
            raise Refusal(
                "capture_budget_unreached",
                f"a run-scope stop at seq={seq} declares limit={limit} but "
                f"charged={charged} -- the producer latches at exactly the "
                "limit, so the two cannot differ",
            )
    observed = sum(len(o.chunks) for o in owners.values())
    if observed != limit:
        raise Refusal(
            "capture_budget_unreached",
            f"a run-scope stop declares limit={limit} but the run carries "
            f"{observed} capture chunk record(s)",
        )


def build_model(lines, prefix):
    """Parse + reconcile the whole run. Never raises Refusal; records it."""
    try:
        owners, spawned = _scan(lines, prefix)
    except Refusal as exc:
        return _refusal_model(exc)

    binaries = []
    aggregate = 0
    aggregate_truncated = False
    try:
        _check_run_budget(owners)
        for pid in sorted(owners):
            owner = owners[pid]
            retained, total = _reconcile(owner, spawned)
            # The aggregate cap bounds the ARTIFACT, never the verdict: the
            # payload above was fully validated before anything was dropped.
            #
            # `aggregate_truncated` reports whether bytes were actually DROPPED,
            # not whether the cap was reached. Keying it on `room <= 0` alone
            # marked a run truncated when the budget landed exactly full and the
            # next owner was a silent binary with nothing to drop.
            room = RUN_AGGREGATE_CAP - aggregate
            kept = retained if len(retained) <= room else retained[:max(room, 0)]
            if len(kept) < len(retained):
                aggregate_truncated = True
            aggregate += len(kept)
            record = {
                "name": owner.name,
                "owner_pid": owner.pid,
                "records": owner.records,
                "total_bytes": total,
                "retained_bytes": len(kept),
                # HOST-side retention truncation only. A producer budget stop
                # is reported through budget_stop below, never here: the
                # producer cannot know how many bytes the binary wrote after
                # it stopped listening, and this pair is an exact contract.
                "truncated": len(kept) < total,
                "truncated_bytes": total - len(kept),
                "budget_stop": owner.over is not None,
                "text": _artifact_text(kept),
            }
            if owner.over is not None:
                record["budget_scope"] = owner.over[1]
                record["budget_limit"] = owner.over[2]
            binaries.append(record)
        # Run-level, and deliberately LAST: both statements are about the run
        # as a whole, so letting either pre-empt a per-owner loss or
        # corruption verdict would replace a precise diagnosis with a vaguer
        # one about a different subject.
        _check_chunk_max_agreement(owners)
        _check_expectations(owners, spawned)
    except Refusal as exc:
        return _refusal_model(exc)

    return {
        "schema": SCHEMA,
        "ok": True,
        "refusal": None,
        "binaries": binaries,
        "aggregate_bytes": aggregate,
        "aggregate_truncated": aggregate_truncated,
    }


def _write_atomic(path, text):
    directory = os.path.dirname(os.path.abspath(path)) or "."
    fd, tmp = tempfile.mkstemp(dir=directory, prefix=".utest-capture.")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def _iter_lines(path):
    """Yield slice lines one at a time.

    Streaming rather than read-then-splitlines: the previous shape held the
    whole slice as bytes, again as decoded text, and a third time as a list of
    lines, so peak memory was several times the file before a single bound
    applied. The line iterator holds one line.
    """
    size = os.path.getsize(path)
    if size > SLICE_MAX_BYTES:
        raise Refusal(
            "capture_slice_too_large",
            f"{path} is {size} bytes, over the {SLICE_MAX_BYTES}-byte bound",
        )
    # Same decode the rest of the host parsers use; the producer escapes every
    # non-ASCII byte, so a replacement char here is itself evidence the line was
    # not a capture record.
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            yield line


def cmd_model(argv):
    if len(argv) < 2:
        sys.stderr.write(
            "usage: utest-capture.py model <slice> <out.json> [--prefix P]\n"
        )
        return 2
    slice_path, out_path = argv[0], argv[1]
    prefix = None
    rest = argv[2:]
    if rest:
        if rest[0] != "--prefix" or len(rest) < 2:
            sys.stderr.write(
                "usage: utest-capture.py model <slice> <out.json> [--prefix P]\n"
            )
            return 2
        prefix = rest[1] or None

    # build_model() consumes the iterator, so both the slice bound (a Refusal)
    # and a read error surface HERE rather than at _iter_lines().
    try:
        model = build_model(_iter_lines(slice_path), prefix)
    except OSError as exc:
        sys.stderr.write(f"utest-capture: cannot read {slice_path}: {exc}\n")
        return 2

    # One write path for both the reconciled and the refusal model. The refusal
    # branch used to write OUTSIDE this handler, so an I/O fault while recording
    # a refusal escaped as an unhandled exception -- Python exits 1, which the
    # caller reads as "reconciliation failed" rather than the documented I/O
    # exit 2, blaming the run's bytes for a disk problem.
    try:
        _write_atomic(out_path, json.dumps(model, indent=1) + "\n")
    except OSError as exc:
        sys.stderr.write(f"utest-capture: cannot write {out_path}: {exc}\n")
        return 2
    if not model["ok"]:
        sys.stderr.write(
            "utest-capture: REFUSED {0}: {1}\n".format(
                model["refusal"]["reason"], model["refusal"]["detail"]
            )
        )
        return 1
    return 0


# Testcases the launcher did not spawn: the host's own abort/refusal documents
# and the synthetic skip-block records. They have no owner pid and must never
# be matched by name against a captured payload.
_SYNTHETIC_CLASSNAMES = ("infrastructure",)


def cmd_splice_xml(argv):
    """Add <system-out>/<system-err> children to real binary testcases.

    Structural, not textual: the assembled document is parsed and re-serialized,
    so self-closing testcases become containers correctly and every payload is
    escaped by the XML writer rather than by hand. Textual splicing cannot work
    here -- the assembler keeps only physical lines beginning with `<testcase`,
    so any multi-line insertion would be dropped by the very next run.
    """
    if len(argv) != 3:
        sys.stderr.write(
            "usage: utest-capture.py splice-xml <in.xml> <model.json> <out.xml>\n"
        )
        return 2
    in_path, model_path, out_path = argv

    try:
        with open(model_path, "r", encoding="utf-8") as handle:
            model = json.load(handle)
    except (OSError, ValueError) as exc:
        sys.stderr.write(f"utest-capture: cannot read model {model_path}: {exc}\n")
        return 2
    if not model.get("ok"):
        sys.stderr.write("utest-capture: refusing to splice an unreconciled model\n")
        return 1

    by_name = {}
    for entry in model.get("binaries", []):
        if entry.get("name"):
            by_name[entry["name"]] = entry

    try:
        tree = ET.parse(in_path)
    except (OSError, ET.ParseError) as exc:
        sys.stderr.write(f"utest-capture: cannot parse {in_path}: {exc}\n")
        return 2

    root = tree.getroot()

    # Both populations come from ONE canonical run, so a captured binary with no
    # testcase means a verdict went missing between the two. Silently dropping
    # it would publish a plausible artifact over a known-incomplete result set.
    # The reverse is legal and common: infrastructure testcases, synthetic skip
    # blocks and binaries that never spawned have no captured output.
    present = {case.get("name") for case in root.iter("testcase")}
    orphans = sorted(n for n in by_name if n not in present)
    if orphans:
        sys.stderr.write(
            "utest-capture: capture model names binaries with no testcase: %s\n"
            % ", ".join(orphans)
        )
        return 1

    truncated = []
    budget_stopped = []
    for case in root.iter("testcase"):
        if case.get("classname") in _SYNTHETIC_CLASSNAMES:
            continue
        name = case.get("name")

        # <system-err> carries the launcher's own kernel-observed diagnostic for
        # this binary -- the reason string already on the <failure>/<skipped>
        # element. Promoting it to system-err gives the element real meaning on
        # a system whose only descriptor is STDOUT_FD, without inventing a
        # second source of truth for the verdict.
        for child in list(case):
            if child.tag in ("failure", "error", "skipped"):
                message = child.get("message")
                if message:
                    err = ET.SubElement(case, "system-err")
                    err.text = message
                break

        record = by_name.get(name)
        if record is None:
            continue
        out = ET.SubElement(case, "system-out")
        # TEXT ONLY, and exactly the model text -- byte-identical to the JSON
        # `captured_output` for the same binary. Two earlier shapes were wrong:
        # appending a human-readable "[capture truncated: ...]" notice made the
        # artifacts disagree and attributed bytes to stdout the binary never
        # wrote; carrying the counts as ATTRIBUTES on this element made the
        # document nonstandard -- the Jenkins/xunit junit-10.xsd models
        # system-out as a string-only element, so a consumer that validates
        # before ingesting would reject the whole report and lose every result.
        # Truncation metadata lives in the JSON artifact and in the suite-level
        # <properties> block, which is the representation this document already
        # uses for run identity.
        out.text = record.get("text", "")
        if record.get("truncated"):
            truncated.append(
                "%s:%d/%d"
                % (name, record.get("retained_bytes", 0), record.get("total_bytes", 0))
            )
        if record.get("budget_stop"):
            budget_stopped.append(
                "%s:%s@%d"
                % (name, record.get("budget_scope", "unknown"),
                   record.get("budget_limit", 0))
            )

    # A truncated payload must never be a SILENT cut in either artifact. The JSON
    # record carries the per-binary counts; the XML says so here, at SUITE level,
    # where <properties> is schema-legal and where this document already reports
    # aborted/not_run and run identity. Absent when nothing was truncated, so a
    # normal run's document is exactly what it was before.
    #
    # A producer budget stop rides its OWN property for the same reason it has
    # its own model field: `capture.truncated` states an exact retained/total
    # pair, and a budget-stopped binary's totals are the bytes that REACHED the
    # host, not the bytes the binary wrote. Folding the two together would put
    # a number in that list which does not mean what every other entry means.
    if truncated or budget_stopped:
        props = root.find("properties")
        if props is None:
            props = ET.Element("properties")
            root.insert(0, props)
        if truncated:
            prop = ET.SubElement(props, "property")
            prop.set("name", "capture.truncated")
            prop.set("value", ",".join(truncated))
        if budget_stopped:
            prop = ET.SubElement(props, "property")
            prop.set("name", "capture.budget_stop")
            prop.set("value", ",".join(budget_stopped))

    try:
        body = ET.tostring(root, encoding="unicode")
        _write_atomic(
            out_path, '<?xml version="1.0" encoding="UTF-8"?>\n' + body + "\n"
        )
    except (OSError, ValueError) as exc:
        sys.stderr.write(f"utest-capture: cannot write {out_path}: {exc}\n")
        return 2
    return 0


def main(argv):
    if not argv:
        sys.stderr.write(__doc__ or "")
        return 2
    if argv[0] == "model":
        return cmd_model(argv[1:])
    if argv[0] == "splice-xml":
        return cmd_splice_xml(argv[1:])
    sys.stderr.write(f"utest-capture: unknown subcommand '{argv[0]}'\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
