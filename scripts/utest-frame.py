#!/usr/bin/env python3
"""utest-frame.py -- the canonical parser for the launcher's framed record stream.

Ring-3 stdout and the user-mode test launcher share one serial stream, so a
launcher record is only believable when it carries this boot's frame: the
kernel logs every launcher-owned record under the subsystem tag
`UTEST-<8 lowercase hex>`, announces that tag once, and closes each run with
a terminator stating how many framed records preceded it.

This module exists because that logic had been hand-copied into three
consumers -- scripts/test.sh, scripts/test-swtpm.sh and
scripts/utest-json-harvest.py -- and the copies had already drifted: only one
bound a terminator to its announcement, only one reconciled counts, and the
swtpm copy still described a wire field that no longer exists. A consumer
that validates the stream differently from the others is a consumer that
passes a stream the others would refuse.

The rules, in one place:

  * Learn the frame from the TAG of the FIRST announcement. The body carries
    no nonce copy (a body copy would survive the kernel's disk-sink alias and
    put the value back into a log ring 3 can open), and a guessed prefix must
    never be accepted -- so the prefix is learned, never pattern-matched.
  * FIRST announcement wins: the launcher announces before it creates any
    ring-3 task, so nothing of that run can have printed ahead of it. A later,
    different nonce is an imitator; its records never match the learned prefix.
  * Every terminator must CLOSE the open announcement with the SAME run
    ordinal. Without that binding, `BEGIN run=1 ... END run=2 records=N`
    reconciles, and two runs can compensate each other's count errors across
    the boundary.
  * Reconciliation is PER RUN, not boot-wide: a run is complete when the
    framed lines between its markers, inclusive, equal `records + 1`.
  * The artifact describes the LAST COMPLETE run. An unterminated trailing
    run is never published, and neither is an older run selected by position
    when a newer one closed cleanly.

Single pass, streaming: the log is a serial capture that reaches tens of MB,
and reading every line into a list to select one run made peak memory grow
with unrelated kernel-log volume. Only the candidate run's lines are held.

INCREMENTAL RESUME. The boot-completion poll in scripts/test.sh asks this
question once per second against a growing capture, and re-reading from byte
zero every time made the poll's cost scale with the whole log rather than with
what arrived since the last answer. `parse_file` therefore accepts a state
object from a previous call and resumes at its byte offset.

Resuming safely is the whole difficulty, because acceptance depends on far
more than an offset and a couple of counters: the learned nonce, the foreign
nonce set, the STICKY `unpaired` and `bad_close` failures, the run and
conflict overflow bounds, the open run's ordinal and line count, and the
newest complete selection all feed `ok`. A resume that dropped any earlier
conflict or bad close would let a later genuine terminator report green where
a full reparse stays red -- a false green, which is the exact failure class
this module exists to prevent. So the resumable state is the COMPLETE parser
FSM, not a summary of it, and there is only ONE implementation of that FSM
(`FrameParser`): `parse()` and the incremental path both drive it, so they
cannot drift the way the three hand-copies did.

Three further invariants make the resume equivalent to a full reparse:

  * The state is bound to the file's IDENTITY (device, inode) and to the
    offset it stopped at. A log that was rotated, recreated or truncated
    fails that binding and restarts from byte zero, so a stale nonce can
    never be carried across into a new capture's stream.
  * An offset is committed only just past a complete line terminator. Reading
    is done in BINARY mode and lines are assembled before they are decoded,
    so ANSI stripping and the `[CRASH-PREV] ` check only ever see a whole
    line -- a chunk boundary cannot split an escape sequence into two
    half-sequences that both fail to match.
  * A trailing bare CR is never consumed, because the next byte to arrive may
    be the LF that turns it into one CRLF terminator rather than two lines.
  * An UNTERMINATED trailing line is never fed to the FSM at all -- not even
    on a final parse. The text-mode reader this replaced would hand a
    half-written last record to the matcher as though it were whole, which is
    precisely how a capture cut mid-terminator could reconcile; refusing it
    costs nothing on a clean capture, where the last line ends in a newline.

CLI: `utest-frame.py <log>` prints one JSON object (see parse()) and exits 0
when a complete, reconciled run was found, 1 when framing is present but no
run reconciles, and 2 when no frame was learned at all.

  --state <path>     resume from (and update) a persisted state file
  --poll             structure only: do not retain the selected run's lines,
                     which keeps a once-per-second state file small
  --emit-run <path>  write the selected run's framed lines to a file, so a
                     caller never has to hold a run in a shell variable
"""

import hashlib
import json
import os
import re
import sys
import tempfile

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
ANNOUNCE_RE = re.compile(
    r"UTEST-([0-9a-f]{8}): \[UTEST-FRAME\] v=1 run=([0-9]{1,10})(?![0-9])")
# `spawned=` is REQUIRED, not optional. The producer publishes the run's own
# count of capture-armed spawns on this line (test_usermode.c u_frame_end), and
# scripts/utest-capture.py reconciles the expectation set against it. Stopping
# at `records=` accepted the obsolete two-field form as a COMPLETE run, so a
# truncated or version-skewed terminator satisfied the completion poll -- and in
# a TAP-only run, where the capture model never runs, nothing downstream applied
# the stricter check either. A terminator the current producer cannot emit is
# not a run this parser should certify.
#
# The terminator's grammar is CANONICAL and ANCHORED: every field is the
# producer's `%u` shape (no leading zeros), bounded to uint32 below, and the
# record must END after the census. A trailing-garbage or out-of-range
# terminator is a shape the producer cannot emit, and accepting one certified
# the run complete on the TAP-only path where nothing downstream re-checks it
# -- `spawned=4294967296`, `spawned=0junk` and `spawned=0 extra=1` all passed.
_END_U32 = r"(0|[1-9][0-9]{0,9})"
END_RE = re.compile(
    r"UTEST-([0-9a-f]{8}): \[UTEST-FRAME-END\] run=" + _END_U32
    + r" records=" + _END_U32
    + r" spawned=" + _END_U32 + r"\s*$")
# 10 digits still admits values above 2^32 that the producer's uint32 cannot
# hold, so the width bound alone is not a range bound.
END_UINT32_MAX = 0xFFFFFFFF

# A recovered crash log replays the PREVIOUS boot's entries to serial,
# subsystem tag and all -- see klog_crash_recover() in src/kernel/klog.c. That
# tag is a real frame from a boot that is over, so a recovered complete run
# would otherwise be learned as THIS boot's frame and satisfy the completion
# gate before the current launcher has run a single binary.
CRASH_REPLAY_PREFIX = "[CRASH-PREV] "

# Attacker-controlled input must not grow parser state without bound: the
# conflict set and the run list are both fed by lines ring 3 can print.
MAX_TRACKED_RUNS = 256
MAX_TRACKED_CONFLICTS = 64

# Bumped whenever the persisted FSM shape changes. A state file written by an
# older parser is DISCARDED rather than adapted: guessing at a missing field
# is how a resume silently stops being equivalent to a full reparse.
STATE_VERSION = 2

# How much of the appended region is read at a time. Only complete lines are
# handed to the FSM, so this bounds peak memory rather than the answer.
READ_CHUNK = 1 << 20

# Hard cap on ONE physical line. Ring 3 controls raw serial output and nothing
# obliges it to ever emit a newline, so an unbounded line assembler is a
# memory-exhaustion primitive reachable from a test binary: the remainder
# buffer would grow by every chunk, and because the committed offset stays at
# the last COMPLETE line, every later poll would re-read the whole tail again
# (quadratic in the length of the abuse). Over this bound the parser stops
# buffering, discards to the next terminator, and latches a sticky failure --
# a run that did this is refused rather than silently parsed from fragments.
# Records are capped at 256 bytes on the kernel side, so a megabyte is orders
# of magnitude above anything legitimate.
MAX_PHYSICAL_LINE = 1 << 20

# Hard cap on the persisted STATE FILE itself, checked via fstat before a
# single byte is read. The legitimate content (a poll's FSM snapshot with
# retain_lines=False, so no run text) is a few KB; this is headroom, not a
# tight fit. Rejecting above it only costs the resume optimization -- see
# _load_state().
STATE_FILE_MAX = 1 << 20

# Bytes immediately before the committed offset whose digest is stored with
# the state and re-read on resume. This is what makes the resume safe against
# a log that was truncated and regrown, or an inode that was reused: device
# and inode equality plus `size >= offset` prove neither that the prefix is
# unchanged nor that it is the same stream.
ANCHOR_BYTES = 4096


def strip_ansi(line):
    return ANSI_RE.sub("", line).rstrip("\n")


_LINE_TERM_RE = re.compile(rb"\r\n|\r|\n")


def split_complete_lines(buf):
    """Split `buf` (bytes) into complete lines plus the unconsumed remainder.

    Mirrors Python's universal-newline handling -- CRLF, bare LF and bare CR
    all terminate a line -- because that is what the text-mode reader this
    replaced did, and a resume that split lines differently from a full
    reparse would not be equivalent to one.

    Returns (lines, consumed). A trailing CR at the very end of `buf` is NOT
    consumed: the next byte may be the LF that makes it a single CRLF, and
    committing it early would turn one line into two on the next call.

    Delimiter search runs through `re.finditer` rather than a per-byte Python
    loop: measured 2026-07-29 at ~9x slower than the FULL reparse it resumes
    (25 MiB synthetic capture, 1.306s vs 0.146s) when this was a manual
    byte-at-a-time scan, since every byte paid Python bytecode dispatch
    instead of the regex engine's C loop. The alternation order (`\r\n`
    before lone `\r`) matches the manual version's CRLF-vs-lone-CR
    precedence exactly, so this changes performance only, no semantic.
    """
    lines = []
    start = 0
    consumed = 0
    n = len(buf)
    for m in _LINE_TERM_RE.finditer(buf):
        if m.group() == b"\r" and m.end() == n:
            # Ambiguous: could be the CR of a CRLF that has not arrived yet.
            # This can only be the LAST match (it consumes the final byte of
            # `buf`), so stopping here never skips a later real terminator.
            break
        lines.append(buf[start:m.start()])
        start = m.end()
        consumed = m.end()
    return lines, consumed


def find_terminator(buf):
    """Offset just past the first line terminator in `buf`, or -1.

    Used to resynchronize after an over-long physical line has been discarded.
    A trailing bare CR is not treated as a terminator, for the same
    CRLF-ambiguity reason as above.
    """
    n = len(buf)
    i = 0
    while i < n:
        c = buf[i]
        if c == 0x0A:
            return i + 1
        if c == 0x0D:
            if i + 1 >= n:
                return -1
            return i + 2 if buf[i + 1] == 0x0A else i + 1
        i += 1
    return -1


def decode_line(raw):
    """Decode one COMPLETE raw line the way the text-mode reader did."""
    return strip_ansi(raw.decode("utf-8", errors="replace"))


class FrameParser:
    """The single implementation of the framing FSM.

    Full parses and incremental resumes both drive this object, so the two
    paths cannot disagree about whether a stream is acceptable.
    """

    def __init__(self):
        self.nonce = None
        self.prefix = None
        self.foreign = set()
        self.conflict_overflow = False
        self.runs = []
        self.run_overflow = False
        self.unpaired = 0
        # A run that CLOSED but did not reconcile is fatal, and stays fatal:
        # the newest closed stream failed the mandatory count check, so
        # falling back to an older run that happened to reconcile would
        # publish stale results under a verdict the newest run never earned.
        self.bad_close = 0
        self.open_run = None       # ordinal of the currently open announcement
        self.open_count = 0        # framed lines since it, inclusive
        self.open_lines = []
        self.last_complete = None
        self.last_lines = []
        # Latched when a single physical line exceeded MAX_PHYSICAL_LINE. The
        # bytes are discarded rather than buffered, so the stream can no
        # longer be reconstructed faithfully -- the run is refused.
        self.line_overflow = False
        # When False the selected run's text is not retained. The poll only
        # needs structure, and holding a run's lines in a once-per-second
        # state file is pure cost.
        self.retain_lines = True

    # -- the FSM itself ------------------------------------------------
    def feed(self, line):
        if CRASH_REPLAY_PREFIX in line:
            # Previous boot's evidence, not this boot's stream.
            return

        announce = ANNOUNCE_RE.search(line)
        if announce:
            if self.nonce is None:
                self.nonce = announce.group(1)
                self.prefix = "UTEST-%s: " % self.nonce
            elif announce.group(1) != self.nonce:
                # An imitator announcing its own frame. Its records can never
                # match the learned prefix; record the conflict and move on.
                if len(self.foreign) < MAX_TRACKED_CONFLICTS:
                    self.foreign.add(announce.group(1))
                else:
                    self.conflict_overflow = True
                return

        if self.prefix is None or self.prefix not in line:
            return

        # From here the line is framed with THIS boot's nonce.
        if announce is not None:
            if self.open_run is not None:
                # Re-opened before its terminator: the previous run lost its
                # tail and can never reconcile.
                self.unpaired += 1
            self.open_run = announce.group(2)
            self.open_count = 1
            self.open_lines = [line] if self.retain_lines else []
            return

        if self.open_run is not None:
            self.open_count += 1
            if self.retain_lines:
                self.open_lines.append(line)

        end = END_RE.search(line)
        if end and any(int(end.group(i)) > END_UINT32_MAX for i in (2, 3, 4)):
            # Wider than the producer's own field. Treated as NOT a terminator
            # rather than as a valid one: the open run stays open and fails to
            # reconcile, which is the fail-closed direction.
            end = None
        if end and end.group(1) == self.nonce:
            if self.open_run is None or end.group(2) != self.open_run:
                self.unpaired += 1
                self.open_run = None
                self.open_lines = []
                return
            declared = int(end.group(3))
            complete = (self.open_count == declared + 1)
            if len(self.runs) < MAX_TRACKED_RUNS:
                self.runs.append({
                    "run": int(self.open_run),
                    "declared": declared,
                    "actual": self.open_count,
                    "complete": complete,
                })
            else:
                self.run_overflow = True
            if complete:
                self.last_complete = {
                    "run": int(self.open_run),
                    "declared": declared,
                    "actual": self.open_count,
                    "complete": True,
                }
                self.last_lines = self.open_lines
            else:
                # Do NOT keep an older last_complete: the newest closed run
                # is the run, and it failed reconciliation.
                self.bad_close += 1
                self.last_complete = None
                self.last_lines = []
            self.open_run = None
            self.open_lines = []

    def clone(self):
        return FrameParser.restore(self.snapshot())

    # -- results -------------------------------------------------------
    def result(self, fragment=None):
        """The public dict. Identical in shape to the pre-incremental parser.

        The trailing-announcement entry is computed here rather than mutated
        into `runs`, because an incremental caller may ask for a result while
        that announcement is still open and then feed more lines: folding it
        in destructively would double-count it on the next call.

        `fragment` is an UNTERMINATED trailing line, which is never committed
        to the FSM -- a half-written record must not be able to complete a
        run. It is not simply discarded either: the old text-mode reader
        delivered it as a whole line, so dropping it outright would LOSE
        negative evidence and turn a real refusal into a false green. The
        concrete case is a complete run followed by an unterminated FOREIGN
        announcement: the old parser reported conflicts=1 and ok=false, and a
        parser that ignored the fragment would report ok=true -- silently
        discarding the ring-3 forgery signal the framing exists to catch. So
        the fragment is replayed on a THROWAWAY clone and only its NEGATIVE
        effects (conflicts, unpaired, bad closes, overflow) are honoured.
        """
        speculative = None
        if fragment:
            speculative = self.clone()
            speculative.feed(fragment)

        runs = list(self.runs)
        run_overflow = self.run_overflow
        if self.open_run is not None:
            # A trailing announcement with no terminator. Never published --
            # an artifact must describe a run that finished -- but NOT fatal
            # on its own: the run may simply have been cut off after an
            # earlier one completed cleanly.
            if len(runs) < MAX_TRACKED_RUNS:
                runs.append({
                    "run": int(self.open_run),
                    "declared": None,
                    "actual": self.open_count,
                    "complete": False,
                })
            else:
                run_overflow = True

        # Negative evidence only: counts may rise from the uncommitted
        # fragment, never fall, and the fragment can never contribute a
        # `last_complete` or extend the selected run.
        unpaired = self.unpaired
        bad_close = self.bad_close
        foreign_n = len(self.foreign)
        conflict_overflow = self.conflict_overflow
        if speculative is not None:
            unpaired = max(unpaired, speculative.unpaired)
            bad_close = max(bad_close, speculative.bad_close)
            foreign_n = max(foreign_n, len(speculative.foreign))
            conflict_overflow = conflict_overflow or speculative.conflict_overflow
            run_overflow = run_overflow or speculative.run_overflow

        conflicts = foreign_n + (MAX_TRACKED_CONFLICTS if conflict_overflow else 0)

        return {
            "nonce": self.nonce,
            "prefix": self.prefix,
            "runs": runs,
            "unpaired": unpaired,
            "bad_close": bad_close,
            "overflow": bool(run_overflow or conflict_overflow
                             or self.line_overflow),
            "conflicts": conflicts,
            "last_complete": self.last_complete,
            "lines": list(self.last_lines),
            "ok": bool(self.nonce and unpaired == 0
                       and bad_close == 0 and foreign_n == 0
                       and not run_overflow and not conflict_overflow
                       and not self.line_overflow
                       and self.last_complete is not None),
        }

    # -- persistence ---------------------------------------------------
    def snapshot(self):
        return {
            "nonce": self.nonce,
            "prefix": self.prefix,
            "foreign": sorted(self.foreign),
            "conflict_overflow": self.conflict_overflow,
            "runs": self.runs,
            "run_overflow": self.run_overflow,
            "unpaired": self.unpaired,
            "bad_close": self.bad_close,
            "open_run": self.open_run,
            "open_count": self.open_count,
            "open_lines": self.open_lines,
            "last_complete": self.last_complete,
            "last_lines": self.last_lines,
            "line_overflow": self.line_overflow,
            "retain_lines": self.retain_lines,
        }

    @classmethod
    def restore(cls, snap):
        """Rebuild from a snapshot, validating STRICTLY.

        The state file is ordinary on-disk data next to a log ring 3 can open,
        so it is treated as untrusted input rather than as something this
        process wrote: a malformed or hostile snapshot must degrade to a full
        reparse, never to a half-initialized FSM that silently accepts. Any
        type or shape violation raises, and the caller restarts from zero.
        """
        if not isinstance(snap, dict):
            raise ValueError("snapshot is not an object")

        def _str_or_none(key):
            v = snap[key]
            if v is not None and not isinstance(v, str):
                raise ValueError("%s must be a string or null" % key)
            return v

        def _nonneg_int(key):
            v = snap[key]
            if not isinstance(v, int) or isinstance(v, bool) or v < 0:
                raise ValueError("%s must be a non-negative integer" % key)
            return v

        def _str_list(key):
            v = snap[key]
            if not isinstance(v, list) or any(not isinstance(x, str) for x in v):
                raise ValueError("%s must be a list of strings" % key)
            return list(v)

        self = cls()
        self.nonce = _str_or_none("nonce")
        self.prefix = _str_or_none("prefix")
        # A learned nonce must still look like one, and the prefix must be the
        # one derived from it -- otherwise a crafted state file could make the
        # parser accept records under an attacker-chosen prefix.
        if self.nonce is not None:
            if not re.fullmatch(r"[0-9a-f]{8}", self.nonce):
                raise ValueError("nonce is not 8 lowercase hex digits")
            if self.prefix != "UTEST-%s: " % self.nonce:
                raise ValueError("prefix does not match the learned nonce")
        elif self.prefix is not None:
            raise ValueError("prefix without a nonce")

        def _exact_bool(key, default=None):
            v = snap.get(key, default)
            if not isinstance(v, bool):
                raise ValueError("%s must be a boolean" % key)
            return v

        def _run_entry(entry, allow_open):
            """Validate one run record, field by field.

            A run entry feeds `runs` and `last_complete`, and `last_complete`
            being non-null is what makes `ok` true -- so an unvalidated dict
            here is a forged verdict. Cross-field consistency is required too:
            a COMPLETE run is by definition one whose framed-line count
            reconciles against its declared count, so a record claiming
            completeness without that relationship is rejected outright.
            """
            if not isinstance(entry, dict):
                raise ValueError("run entry is not an object")
            if set(entry) != {"run", "declared", "actual", "complete"}:
                raise ValueError("run entry has unexpected fields")
            run = entry["run"]
            if not isinstance(run, int) or isinstance(run, bool) or run < 0:
                raise ValueError("run ordinal is invalid")
            actual = entry["actual"]
            if not isinstance(actual, int) or isinstance(actual, bool) or actual < 0:
                raise ValueError("run actual count is invalid")
            declared = entry["declared"]
            if declared is None:
                if not allow_open:
                    raise ValueError("declared may not be null here")
            elif (not isinstance(declared, int) or isinstance(declared, bool)
                  or declared < 0):
                raise ValueError("run declared count is invalid")
            complete = entry["complete"]
            if not isinstance(complete, bool):
                raise ValueError("run complete flag is invalid")
            if complete != (declared is not None and actual == declared + 1):
                raise ValueError("run completeness contradicts its counts")
            return entry

        self.foreign = set(_str_list("foreign"))
        for tag in self.foreign:
            if not re.fullmatch(r"[0-9a-f]{8}", tag):
                raise ValueError("foreign nonce is not 8 lowercase hex digits")
        if len(self.foreign) > MAX_TRACKED_CONFLICTS:
            raise ValueError("foreign set exceeds its tracked bound")
        self.conflict_overflow = _exact_bool("conflict_overflow")
        runs = snap["runs"]
        if not isinstance(runs, list):
            raise ValueError("runs must be a list")
        if len(runs) > MAX_TRACKED_RUNS:
            raise ValueError("runs exceeds its tracked bound")
        self.runs = [_run_entry(r, allow_open=False) for r in runs]
        self.run_overflow = _exact_bool("run_overflow")
        self.unpaired = _nonneg_int("unpaired")
        self.bad_close = _nonneg_int("bad_close")
        self.open_run = _str_or_none("open_run")
        if self.open_run is not None and not re.fullmatch(r"[0-9]{1,10}",
                                                          self.open_run):
            raise ValueError("open_run is not a run ordinal")
        self.open_count = _nonneg_int("open_count")
        self.open_lines = _str_list("open_lines")
        if self.open_run is None and self.open_lines:
            raise ValueError("open lines without an open run")
        lc = snap["last_complete"]
        if lc is not None:
            _run_entry(lc, allow_open=False)
            if not lc["complete"]:
                raise ValueError("last_complete is not complete")
        self.last_complete = lc
        self.last_lines = _str_list("last_lines")
        if lc is None and self.last_lines:
            raise ValueError("selected lines without a selected run")
        # No default: snapshot() ALWAYS writes this field, so a state file
        # missing it is malformed, not "no overflow happened". Defaulting to
        # False would let a version-skewed or truncated state silently erase
        # a recorded overflow instead of failing the restore and falling
        # back to a cold parse -- the same false-green class every other
        # field here is validated to prevent.
        self.line_overflow = _exact_bool("line_overflow")
        self.retain_lines = _exact_bool("retain_lines")
        return self


def parse(lines):
    """Walk a framed stream once and return its structure.

    `lines` is any iterable of already-ANSI-stripped strings.

    Returns a dict:
      nonce            the learned 8-hex nonce, or None
      prefix           "UTEST-<nonce>: ", or None
      runs             [{run, declared, actual, complete}] per announcement
      unpaired         markers that did not pair (a terminator closing no open
                       announcement, a mismatched ordinal, or an announcement
                       re-opened before its terminator)
      bad_close        runs that CLOSED but did not reconcile -- fatal, and it
                       clears any older selection: the newest closed run is
                       the run, and it failed the mandatory count check
      overflow         True when tracked runs or conflicts hit their bound
                       (attacker-controlled input must not grow state freely)
      conflicts        DISTINCT foreign nonces that announced a frame
      last_complete    the newest run that closed AND reconciled, or None
      lines            that run's framed lines, in order (empty when none)
      ok               True iff a frame was learned, nothing is unpaired, no
                       run closed unreconciled, no foreign announcement
                       appeared, nothing overflowed, and a complete run exists
    """
    parser = FrameParser()
    for raw in lines:
        parser.feed(raw if isinstance(raw, str) else str(raw))
    return parser.result()


def _anchor_digest(handle, offset):
    """Digest of the bytes immediately before `offset`, or None at offset 0.

    This is the content half of the resume binding. Device and inode equality
    prove only that the PATH still names the same file, and `size >= offset`
    proves only that it is long enough -- neither survives the two cases that
    matter: a log truncated to nothing and regrown past the old offset between
    polls, and an inode reused after a delete. In both, the stale FSM would be
    resumed over unrelated prefix bytes, and a state holding an open run could
    consume a matching terminator from the REPLACEMENT stream and report a
    reconciled run that a full parse never sees.
    """
    if offset <= 0:
        return None
    start = max(0, offset - ANCHOR_BYTES)
    handle.seek(start)
    data = handle.read(offset - start)
    if len(data) != offset - start:
        return None
    return hashlib.sha256(data).hexdigest()


def _state_shape_ok(state, identity):
    """Cheap structural checks before any I/O against the file."""
    if not isinstance(state, dict):
        return False
    if state.get("v") != STATE_VERSION:
        return False
    ident = state.get("identity")
    if not isinstance(ident, dict):
        return False
    if ident.get("dev") != identity["dev"] or ident.get("ino") != identity["ino"]:
        return False
    offset = state.get("offset")
    if not isinstance(offset, int) or isinstance(offset, bool) or offset < 0:
        return False
    if identity["size"] < offset:
        return False
    anchor = state.get("anchor")
    if anchor is not None and not isinstance(anchor, str):
        return False
    return True


def parse_file(path, state=None, retain_lines=True):
    """Parse `path`, optionally resuming from `state`.

    Returns the same dict `parse()` returns, plus a `state` key holding the
    object to hand back on the next call. When `state` is absent, malformed,
    or cannot be bound to this file's identity AND content, the whole file is
    parsed from byte zero -- the resume is an optimization and every doubt
    resolves to the slow, always-correct path.

    `retain_lines=False` drops the selected run's text, which is what the
    once-per-second completion poll wants: it asks only whether a run closed.
    """
    with open(path, "rb") as handle:
        # fstat the DESCRIPTOR, not the path: stat()ing the name and then
        # opening it is a two-step that a rename or recreate can land between,
        # so the identity checked would not be the identity read.
        st = os.fstat(handle.fileno())
        identity = {"dev": st.st_dev, "ino": st.st_ino, "size": st.st_size}

        parser = None
        offset = 0
        restarted = state is not None
        # Whether persisted state actually supplied the FSM. Tracked
        # independently of the offset: a forged state may legitimately carry
        # offset 0 (with a null anchor, which matches trivially), so keying
        # the confirmation below on `offset > 0` would let exactly that shape
        # through -- reproduced against an empty log, where a schema-valid
        # forged FSM returned ok=true without ever being confirmed.
        resumed_from_state = False
        if state is not None and _state_shape_ok(state, identity):
            try:
                candidate = FrameParser.restore(state["fsm"])
            except (KeyError, TypeError, ValueError):
                candidate = None
            if candidate is not None:
                want = state.get("anchor")
                got = _anchor_digest(handle, state["offset"])
                # A poll that stops retaining lines mid-stream would produce a
                # truncated run slice, so only WIDENING is safe on a resume.
                widening = retain_lines and not candidate.retain_lines
                if want == got and not widening:
                    parser = candidate
                    offset = state["offset"]
                    restarted = False
                    resumed_from_state = True

        if parser is None:
            parser = FrameParser()
            offset = 0
        parser.retain_lines = retain_lines

        pending = b""
        # True while the tail of an over-long physical line is being thrown
        # away; bytes are discarded until the next terminator resynchronizes.
        skipping = False
        handle.seek(offset)
        while True:
            chunk = handle.read(READ_CHUNK)
            if not chunk:
                break
            pending += chunk

            while True:
                if skipping:
                    cut = find_terminator(pending)
                    if cut < 0:
                        # Still inside the over-long line: keep nothing.
                        offset += len(pending)
                        pending = b""
                        break
                    offset += cut
                    pending = pending[cut:]
                    skipping = False
                    continue

                lines, consumed = split_complete_lines(pending)
                for raw in lines:
                    # The bound applies to a COMPLETE line too, not only to an
                    # unterminated remainder. A line that eventually ends is
                    # still assembled in memory first, so an abuser who does
                    # emit a terminator -- after a megabyte -- would otherwise
                    # walk straight past the check below.
                    if len(raw) > MAX_PHYSICAL_LINE:
                        parser.line_overflow = True
                        continue
                    parser.feed(decode_line(raw))
                offset += consumed
                pending = pending[consumed:]

                if len(pending) > MAX_PHYSICAL_LINE:
                    # Unbounded newline-free output. Refuse the run rather
                    # than buffer it: ring 3 chooses this length.
                    parser.line_overflow = True
                    skipping = True
                    continue
                break

        # Whatever is left is an UNTERMINATED trailing line. It is never
        # committed, but its negative evidence still counts (see result()).
        fragment = decode_line(pending) if (pending and not skipping) else None

        result = parser.result(fragment=fragment)
        # The outgoing anchor MUST be taken from the descriptor that was just
        # parsed, before it closes. Re-opening the path to digest it would
        # reintroduce exactly the race the fstat above removes: an in-place
        # replacement in that window would bind this FSM to a digest of bytes
        # it never read, and the NEXT resume would then accept it.
        anchor = _anchor_digest(handle, offset)

    result["state"] = {
        "v": STATE_VERSION,
        "identity": identity,
        "offset": offset,
        "anchor": anchor,
        "fsm": parser.snapshot(),
    }
    result["restarted"] = restarted

    # PERSISTED STATE MAY NEVER ESTABLISH A POSITIVE RESULT.
    #
    # The state file is unauthenticated data on disk, next to a log ring 3 can
    # open. Every attempt to make a resumed POSITIVE verdict trustworthy by
    # validating harder -- identity, size, a tail digest, strict field types --
    # closes one bypass and leaves the class open: a 4096-byte tail anchor
    # cannot prove the whole committed prefix is unchanged (a truncate-regrow
    # may reproduce that suffix), and no amount of shape-checking makes a
    # forged `last_complete` false. Both were demonstrated: a state carrying a
    # fabricated `last_complete` reported ok=true against a log with no frame
    # in it at all.
    #
    # And the positive verdict is precisely the dangerous one -- it is what
    # stops QEMU and declares the run complete. So the optimization is
    # confined to what it is actually for. A resumed parse may cheaply report
    # "not yet", which is the answer 59 polls out of 60 give; the moment it
    # would report a COMPLETED run, that answer is re-derived by a full parse
    # from byte zero, which no state file can influence. The saving is kept
    # and the false green becomes structurally impossible rather than guarded.
    # The predicate is "state was ACCEPTED", not "state had a useful offset".
    # A forged state legitimately carries offset 0 and a null anchor -- which
    # matches trivially, since there are no bytes before offset 0 to digest --
    # so an offset-based guard would wave through precisely the shape it is
    # meant to stop. The recursive call passes state=None, so the confirming
    # parse can never itself be resumed and the recursion is one deep.
    if resumed_from_state and result["last_complete"] is not None:
        confirmed = parse_file(path, state=None, retain_lines=retain_lines)
        confirmed["restarted"] = restarted
        confirmed["confirmed_by_full_reparse"] = True
        return confirmed
    result["confirmed_by_full_reparse"] = False
    return result


def _load_state(path):
    try:
        with open(path, "rb") as handle:
            # fstat the DESCRIPTOR before reading a byte, and reject an
            # oversized file BEFORE json.load() ever runs. `_state_shape_ok`
            # and `FrameParser.restore()` reject a malformed body field by
            # field, but both run AFTER the parse -- so an untrusted or
            # racing writer could make json.load() itself pay unbounded
            # memory/CPU parsing a huge or deeply-nested document before
            # either check gets a chance to refuse it. The legitimate state
            # this file holds is the once-per-second POLL's FSM snapshot
            # (retain_lines=False, so no run text is retained) plus at most
            # MAX_TRACKED_RUNS run records and MAX_TRACKED_CONFLICTS nonces --
            # a few KB. STATE_FILE_MAX is generous headroom over that, not a
            # tight fit, and rejecting above it only costs the OPTIMIZATION:
            # the caller falls back to a cold parse from byte zero, same as
            # any other malformed state.
            if os.fstat(handle.fileno()).st_size > STATE_FILE_MAX:
                return None
            raw = handle.read(STATE_FILE_MAX + 1)
        if len(raw) > STATE_FILE_MAX:
            return None
        return json.loads(raw.decode("utf-8"))
    except (OSError, ValueError):
        return None
    except RecursionError:
        # A document deeply nested enough to blow the interpreter's
        # recursion limit can be well under STATE_FILE_MAX in bytes (a few
        # thousand levels of "[" costs one byte each) -- the size cap above
        # bounds memory, not nesting depth. json.loads() raises
        # RecursionError, which is neither OSError nor ValueError, so
        # without this it would escape _load_state() entirely instead of
        # degrading to the same cold-reparse fallback every other malformed
        # state takes.
        return None


def _write_atomic(path, write_body):
    """Stage through an O_EXCL temporary in the destination directory, then
    os.replace onto `path`.

    The staging file is created by mkstemp, NOT at a predictable `path.tmp`.
    A predictable staging name is itself an attack surface: if something has
    pre-placed a hard link to the input capture at that name, opening it "w"
    truncates the shared inode and destroys the very log being judged -- and a
    path-based alias check cannot see it, because realpath compares NAMES
    after symlink resolution, not inode identity. mkstemp uses O_EXCL, so it
    never opens an existing file at all, which removes the class rather than
    guarding one instance of it.

    Atomicity matters on its own: a poll killed partway through a write would
    otherwise leave a truncated document behind under the real name.
    """
    directory = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(dir=directory, prefix=".utest-frame-",
                               suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            write_body(handle)
        os.replace(tmp, path)
    except OSError:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def _store_state(path, state):
    try:
        _write_atomic(path, lambda handle: json.dump(state, handle))
    except OSError:
        # A state file that cannot be written costs the OPTIMIZATION, never
        # the answer: the next poll simply parses from byte zero.
        pass


def main(argv):
    log = None
    state_path = None
    emit_run = None
    poll = False

    def _value(flag, idx):
        """Take the argument after `flag`, refusing another option token.

        `--state --poll log` would otherwise silently take "--poll" as the
        state PATH and then write JSON to a file called "--poll", which is
        both a wrong answer and a stray file.
        """
        if idx + 1 >= len(argv) or argv[idx + 1].startswith("--"):
            sys.stderr.write("utest-frame: %s needs a path\n" % flag)
            return None
        return argv[idx + 1]

    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--state":
            state_path = _value(arg, i)
            if state_path is None:
                return 2
            i += 2
        elif arg == "--emit-run":
            emit_run = _value(arg, i)
            if emit_run is None:
                return 2
            i += 2
        elif arg == "--poll":
            poll = True
            i += 1
        elif arg.startswith("--"):
            sys.stderr.write("utest-frame: unknown option %r\n" % arg)
            return 2
        elif log is None:
            log = arg
            i += 1
        else:
            sys.stderr.write("utest-frame: unexpected argument %r\n" % arg)
            return 2

    if log is None:
        sys.stderr.write(
            "usage: utest-frame.py [--state F] [--poll] [--emit-run F] <log>\n")
        return 2

    # No output path may alias the input or the other output. `--state LOG LOG`
    # would replace the capture with state JSON and `--emit-run LOG LOG` would
    # truncate it -- destroying the evidence the run is being judged on, before
    # anything is even parsed. Compared by resolved path so a symlink or a
    # `./` spelling cannot slip past.
    def _key(p):
        return os.path.realpath(p)

    # Staging is an O_EXCL mkstemp file in the destination directory (see
    # _write_atomic), so there is no predictable derived path to defend and no
    # way to truncate a pre-existing file through it. What remains to check is
    # the DESTINATIONS: os.replace onto a name that is the input log would
    # clobber the capture, and two outputs sharing a name would clobber each
    # other. Identity is compared by inode where the file exists -- a hard
    # link has a different name for the same bytes, which a name comparison
    # cannot see -- and by resolved path where it does not.
    def _same(a, b):
        try:
            return os.path.samefile(a, b)
        except OSError:
            return _key(a) == _key(b)

    outputs = [(flag, t) for flag, t in
               (("--state", state_path), ("--emit-run", emit_run))
               if t is not None]

    for flag, target in outputs:
        if _same(target, log):
            sys.stderr.write(
                "utest-frame: %s path aliases the input log (%s)\n"
                % (flag, log))
            return 2
    for i, (flag_a, a) in enumerate(outputs):
        for flag_b, b in outputs[i + 1:]:
            if _same(a, b):
                sys.stderr.write("utest-frame: %s aliases %s\n"
                                 % (flag_b, flag_a))
                return 2

    # The run slice is the artifact's payload; retaining lines is required to
    # produce it, so --emit-run overrides --poll rather than silently emitting
    # an empty file.
    retain = (not poll) or (emit_run is not None)

    prior = _load_state(state_path) if state_path else None
    try:
        result = parse_file(log, state=prior, retain_lines=retain)
    except OSError as exc:
        sys.stderr.write("utest-frame: cannot read %s: %s\n" % (log, exc))
        return 2

    if state_path:
        _store_state(state_path, result["state"])

    if emit_run is not None:
        # Written atomically. A mid-write failure used to leave a NON-EMPTY
        # partial slice, and the host harness treats any non-empty slice as
        # usable -- so a truncated run would have been assembled into the
        # artifact as though it were whole.
        def _emit(handle):
            for line in result["lines"]:
                handle.write(line + "\n")

        try:
            _write_atomic(emit_run, _emit)
        except OSError as exc:
            sys.stderr.write("utest-frame: cannot write %s: %s\n"
                             % (emit_run, exc))
            return 2

    # The framed line list is for in-process consumers; the CLI reports
    # structure only, so a caller does not pull a whole run through a pipe.
    # The resume state is likewise not stdout material -- it goes to --state.
    emitted = dict(result)
    emitted["lines"] = len(result["lines"])
    emitted.pop("state", None)
    sys.stdout.write(json.dumps(emitted) + "\n")

    if result["nonce"] is None:
        return 2
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
