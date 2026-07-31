#!/usr/bin/env python3
"""Assemble build/test-results.json from the launcher's [UTEST-JSON] stream.

The kernel-side user-mode launcher emits one `[UTEST-JSON] {...}` record per
binary, one per synthetic skip block, and a single trailing summary record.
Individually they are valid JSON; together they are not a document. This is
the host-side assembly step, the JSON sibling of the JUnit XML
post-processor in scripts/test.sh step 6.

It is deliberately NOT a `grep | sed | jq` pipeline. The artifact is a gate,
not a convenience: a partial, duplicated, or internally inconsistent stream
must FAIL the run rather than publish a plausible-looking smaller result.
Validating that in shell would mean re-deriving JSON parsing from sed, and
the false-green class this closes is the one the ring-3 self-report and
reporting-honesty work already paid for.

The launcher emits four record kinds -- `binary`, `skip_block`, and the
trailing `run_report` (assertion dimension) and `run_meta` (completeness
dimension). Those last two are separate records only because every record
crosses the wire through klog, whose message field is 256 bytes: with all
of it nested inline the summary record measured 242 bytes and overflowed on
a larger suite. This assembler folds both back into `summary`, so a
consumer of the FILE sees one summary object carrying `reported`,
`aborted`, and `not_run` -- the transport constraint stops at the wire.

Fail-closed rules -- any of these refuses to publish a normal envelope:

  * no summary record, or more than one
  * no run_report or run_meta record, or more than one of either
  * a stream not ending run_report, summary, run_meta, in that order
  * `summary_error` (the launcher's own overflow marker)
  * any record that is not parseable JSON, or carries an unknown record_kind
  * binary-record count != summary.total
  * per-status binary counts != summary.passed/failed/skipped
  * skip-block record count != summary.reported.skip_records
  * reported + invalid + unreported != summary.total (they partition it)
  * a not-aborted run that nevertheless reports binaries as not run

An ABORTED run additionally gets a host-synthesized `suite-abort` record
appended to `testcases`, mirroring the `<testcase name="suite-abort">`
element the XML assembler injects (scripts/test.sh). Without it the abort
rides only as the boolean `summary.aborted`, and a generic consumer
walking `testcases[]` -- which is what the dashboards and trend tooling
this artifact names as its audience actually do -- reads a smaller but
internally consistent set as green, because the binaries that never ran
were never published as records.

That record is kept OUTSIDE the producer's accounting. It is appended only
after every producer-stream reconciliation has passed, it carries
`record_kind: "infrastructure"` and `synthetic: true`, and it moves NONE of
the producer counters (`total`, `passed`/`failed`/`skipped`, `errors`,
`reported.*`): those describe binaries the launcher accounted for, and one
infrastructure event standing for N omitted binaries is not an additional
binary. The published document states the host dimension explicitly
instead -- `summary.synthetic_errors` and `summary.testcase_total`, with
`testcase_total == total + synthetic_errors` -- so a consumer can tell a
producer fact from a host projection rather than having to trust that the
two were never mixed. Both fields are written on EVERY published run, not
only aborted ones, for the same reason the XML `<properties>` block is:
absent must mean "this artifact predates the dimension", never "zero".

That is why the schema identifier moved to `utest-json-v2`. The change is
observable in both directions -- two new `summary` fields on every run, and
a record in `testcases` whose `record_kind` is not `binary` -- so a document
carrying the shape while still claiming `v1` would be exactly the silent
contract drift this artifact exists to refuse. Nothing reads the identifier
today -- the user-mode test framework roadmap still tracks this schema as
pre-consumer, pending its evaluation against a published cross-tool format --
which is what makes moving it free; a strict reader written against v1 now
fails closed on an unknown version instead of misreading a population it does
not know about.

On refusal an explicit error envelope is written (never a zero-filled one
that reads as a clean empty run) and the exit code is nonzero.

Exit codes: 0 published, 1 refused (error envelope written), 2 usage/IO
error (nothing written), 3 no [UTEST-JSON] stream at all (error envelope
written).
"""

import json
import os
import re
import sys
import tempfile

def _load_frame_parser():
    """Import scripts/utest-frame.py, whose name is not a Python identifier."""
    import importlib.util

    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "utest-frame.py")
    spec = importlib.util.spec_from_file_location("utest_frame", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SCHEMA = "utest-json-v2"
MARKER = "[UTEST-JSON] "
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

# A record is only believed when it is FRAMED. Ring-3 stdout shares the
# serial stream with the launcher -- sys_write(fd=1) copies caller bytes
# straight to serial_putchar -- so a bare `[UTEST-JSON] {...}` proves
# nothing about who wrote it, and used to be folded into the artifact
# verbatim. The kernel derives a per-boot nonce and logs every launcher
# record under the subsystem tag `UTEST-<8 hex>`, which ring 3 cannot
# reproduce because it never sees the value. The frame is learned from the
# launcher's own announcement, and a log with no announcement (or with two
# disagreeing ones) yields NO records rather than unframed ones.
def _write_atomic(path, payload):
    """Publish via temp file + rename.

    A consumer must never read a half-written artifact, and a run that dies
    mid-write must not leave one. The rename is atomic within a directory,
    so the file at `path` is always either the previous artifact or the
    complete new one.
    """
    directory = os.path.dirname(os.path.abspath(path)) or "."
    os.makedirs(directory, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=directory, prefix=".test-results-", suffix=".json")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=1, sort_keys=False)
            handle.write("\n")
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


# Run identity, read once from the file scripts/test.sh derives before the
# build. It is carried on EVERY envelope this module writes, refusals
# included: an artifact nobody can place is exactly the one a dashboard needs
# to place, and a refusal that cannot be attributed to a run is indistinguishable
# from a refusal belonging to some other run. Identity never softens a refusal
# -- `summary` stays null and `summary_error` stays set alongside it.
_IDENTITY = None


# Provenance is metadata, not payload: it is read under a hard byte cap and a
# regular-file check so a mistaken path (a directory, a multi-gigabyte log, a
# deeply nested document) degrades to `run_identity: null` instead of
# exhausting memory or killing the harvest with an uncaught RecursionError.
IDENTITY_MAX_BYTES = 64 * 1024


def _load_identity(path):
    """Return the identity object, or None when it is absent or unusable.

    A missing or malformed identity file is NOT a refusal: the artifact's own
    gates are about stream integrity, and failing the harvest because
    provenance is unavailable would turn a metadata gap into a false red.
    """
    if not path:
        return None

    def _decline(why):
        sys.stderr.write("utest-json-harvest: identity %s at %s "
                         "-- publishing without it\n" % (why, path))
        return None

    try:
        if not os.path.isfile(path):
            return _decline("is not a regular file")
        size = os.path.getsize(path)
        if size > IDENTITY_MAX_BYTES:
            return _decline("exceeds %d bytes" % IDENTITY_MAX_BYTES)
        with open(path, "r", encoding="utf-8") as handle:
            value = json.loads(handle.read(IDENTITY_MAX_BYTES + 1))
    except (OSError, ValueError, RecursionError):
        return _decline("is unreadable")
    if not isinstance(value, dict):
        return _decline("is not a JSON object")
    return value


# Captured per-binary output model, produced by scripts/utest-capture.py from
# the same canonical run slice the XML assembler consumes (section 36).
_CAPTURE = None

# The model is bounded at the source (a per-binary and a run-wide retention cap)
# but it is still read under a cap here, because this process must not be the
# one that turns a corrupted or mistaken path into an OOM.
CAPTURE_MAX_BYTES = 8 * 1024 * 1024


def _load_capture(path):
    """Return the capture model, or None when no capture was attached.

    UNLIKE identity, an unreadable model is NOT silently dropped when the caller
    asked for one: the model carries a fail-closed VERDICT about whether the
    run's captured bytes reconcile, so treating "cannot read it" as "nothing to
    attach" would convert a refusal into a clean artifact. A path that was
    passed but cannot be parsed therefore yields a synthetic refusal model.
    """
    if not path:
        return None

    def _unreadable(why):
        sys.stderr.write("utest-json-harvest: capture model %s at %s\n"
                         % (why, path))
        return {"schema": "utest-capture-v1", "ok": False,
                "refusal": {"reason": "capture_model_unreadable", "detail": why},
                "binaries": []}

    try:
        if not os.path.isfile(path):
            return _unreadable("is not a regular file")
        if os.path.getsize(path) > CAPTURE_MAX_BYTES:
            return _unreadable("exceeds %d bytes" % CAPTURE_MAX_BYTES)
        with open(path, "r", encoding="utf-8") as handle:
            value = json.loads(handle.read(CAPTURE_MAX_BYTES + 1))
    except (OSError, ValueError, RecursionError):
        return _unreadable("is unreadable")
    if not isinstance(value, dict):
        return _unreadable("is not a JSON object")
    return value


def _refuse(out_path, reason, detail=None):
    """Write the error envelope and report the reason on stderr.

    `summary` is explicitly null rather than a zero-filled object: a
    consumer that keys on the summary must be unable to read a refused
    artifact as a clean run that happened to have no tests.
    """
    envelope = {
        "schema": SCHEMA,
        "testcases": [],
        "skip_blocks": [],
        "summary": None,
        "summary_error": reason,
    }
    if detail:
        envelope["detail"] = detail
    envelope["run_identity"] = _IDENTITY
    _write_atomic(out_path, envelope)
    sys.stderr.write("utest-json-harvest: refused -- %s%s\n"
                     % (reason, (": " + detail) if detail else ""))


# Status vocabulary of a binary record, mapped to the summary counter it must
# agree with. A record carrying anything else is a producer bug, not a new
# outcome to tolerate.
#
# ERROR is a binary that never RAN -- a refused name, or a launch that
# produced no task. It maps to `failed` because the producer counts it there
# too: `errors` is an overlapping SUBSET of `failed`, not a fourth disjoint
# bucket, so `passed + failed + skipped` stays the run total and a consumer
# that only ever read `failed` sees exactly the number it always did. The
# subset itself is reconciled separately, against summary.errors.
STATUS_TO_COUNTER = {"PASS": "passed", "FAIL": "failed", "SKIP": "skipped",
                     "ERROR": "failed"}

# The statuses that mean "this binary never executed". Kept as its own set
# rather than inferred from STATUS_TO_COUNTER, which cannot express it: FAIL
# and ERROR share a counter and differ only here.
NEVER_RAN_STATUSES = frozenset(("ERROR",))

# The per-binary report triple. A binary that never ran submitted no report,
# so a record carrying any of these alongside a never-ran status describes
# assertions that no execution could have produced. The producer already
# makes that unrepresentable -- u_format_json_testcase emits the triple only
# for verdict != 3 (src/kernel/test/test_usermode.c), which is the
# precondition UTEST_FIXED_JSON_NEVER_RAN is derived from -- so the check
# here is the host half of a two-sided invariant, not defensive padding.
REPORT_TRIPLE = ("asserts_passed", "asserts_failed", "skip_blocks")

# Framing is parsed by the CANONICAL parser (scripts/utest-frame.py), not by
# a copy living here. Ring-3 stdout shares the serial stream with the
# launcher, so a bare `[UTEST-JSON]` line proves nothing about who wrote it;
# the parser learns this boot's nonce from the launcher's own announcement,
# binds each terminator to its announcement by run ordinal, reconciles the
# record count per run, and hands back only the LAST COMPLETE run's lines.
# This file used to carry its own copy of those rules and had already drifted
# from the other two consumers -- it never consumed the terminator's declared
# count at all.
_FRAME = _load_frame_parser()


def _extract_records(log_path):
    """Return the payload strings following each FRAMED [UTEST-JSON] marker.

    Only records belonging to the last complete, reconciled run are returned;
    an unframed record, a wrong-nonce record, a record from a run whose
    markers do not pair, and a record from a run whose count does not
    reconcile are all excluded. An empty result therefore means "nothing
    attributable", which the caller turns into an explicit refusal rather
    than an empty-but-valid artifact.
    """
    result = _FRAME.parse_file(log_path)
    if not result["ok"]:
        return []

    framed_marker = result["prefix"] + MARKER
    records = []
    for line in result["lines"]:
        idx = line.find(framed_marker)
        if idx < 0:
            continue
        records.append(line[idx + len(framed_marker):].strip())
    return records


def harvest(log_path, out_path):
    try:
        raw_records = _extract_records(log_path)
    except OSError as exc:
        sys.stderr.write("utest-json-harvest: cannot read %s: %s\n" % (log_path, exc))
        return 2

    if not raw_records:
        _refuse(out_path, "missing_stream",
                "no [UTEST-JSON] records on serial")
        return 3

    testcases = []
    skip_blocks = []
    summary = None
    summary_index = -1
    run_meta = None
    run_meta_index = -1
    run_report = None
    run_report_index = -1

    for i, raw in enumerate(raw_records):
        try:
            record = json.loads(raw)
        except ValueError as exc:
            _refuse(out_path, "malformed_record",
                    "record %d is not valid JSON (%s)" % (i + 1, exc))
            return 1
        if not isinstance(record, dict):
            _refuse(out_path, "malformed_record",
                    "record %d is not a JSON object" % (i + 1))
            return 1

        # The launcher's own overflow marker. It is syntactically valid JSON
        # precisely so it cannot be mistaken for a run summary -- honour that.
        if "summary_error" in record:
            _refuse(out_path, "producer_overflow",
                    "launcher reported %r" % (record["summary_error"],))
            return 1

        if "summary" in record:
            if summary is not None:
                _refuse(out_path, "duplicate_summary",
                        "a second summary record appeared at position %d" % (i + 1))
                return 1
            summary = record["summary"]
            summary_index = i
            continue

        kind = record.get("record_kind")
        if kind == "binary":
            testcases.append(record)
        elif kind == "skip_block":
            skip_blocks.append(record)
        elif kind == "run_meta":
            if run_meta is not None:
                _refuse(out_path, "duplicate_run_meta",
                        "a second run_meta record appeared at position %d"
                        % (i + 1))
                return 1
            run_meta = record
            run_meta_index = i
        elif kind == "run_report":
            if run_report is not None:
                _refuse(out_path, "duplicate_run_report",
                        "a second run_report record appeared at position %d"
                        % (i + 1))
                return 1
            run_report = record
            run_report_index = i
        else:
            _refuse(out_path, "unknown_record_kind",
                    "record %d carries record_kind=%r" % (i + 1, kind))
            return 1

    if summary is None:
        _refuse(out_path, "missing_summary",
                "%d record(s) on serial but no summary -- stream truncated"
                % len(raw_records))
        return 1
    if not isinstance(summary, dict):
        _refuse(out_path, "malformed_summary", "summary is not an object")
        return 1
    # The launcher emits summary then run_meta, in that order, as the last two
    # records. Anything after them means the stream interleaved two runs or
    # was cut and resumed, and the counts below would be checked against the
    # wrong population.
    if run_meta is None:
        _refuse(out_path, "missing_completeness",
                "no run_meta record -- cannot tell a complete run from an "
                "aborted one")
        return 1
    if run_report is None:
        _refuse(out_path, "missing_report",
                "no run_report record -- the assertion dimension is absent")
        return 1
    if (run_meta_index != len(raw_records) - 1
            or summary_index != run_meta_index - 1
            or run_report_index != summary_index - 1):
        _refuse(out_path, "records_after_summary",
                "expected the stream to end run_report, summary, run_meta; "
                "found them at %d, %d, %d of %d"
                % (run_report_index + 1, summary_index + 1,
                   run_meta_index + 1, len(raw_records)))
        return 1

    # The assertion-level dimension travels as its own record for the same
    # transport reason as run_meta, and is nested back under
    # `summary.reported` here so the artifact keeps the documented shape.
    if run_report is None:
        _refuse(out_path, "missing_report",
                "no run_report record -- the assertion dimension is absent")
        return 1
    reported = {k: v for k, v in run_report.items() if k != "record_kind"}
    REPORT_FIELDS = ("asserts_passed", "asserts_failed", "skip_blocks",
                     "skip_records", "binaries_reported", "binaries_invalid",
                     "binaries_unreported")
    for field in REPORT_FIELDS:
        value = reported.get(field)
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            _refuse(out_path, "malformed_report",
                    "run_report.%s is %r, expected a non-negative integer"
                    % (field, value))
            return 1
    # The three outcome counters PARTITION the executed binaries: the launcher
    # increments exactly one of reported/invalid/unreported for every binary
    # it accounts for, including the ones whose task_create failed. Without
    # this reconciliation a stream could report one passing binary and zero in
    # all three counters and still satisfy every other check -- publishing an
    # artifact whose assertion dimension silently describes nobody.
    partition = (reported["binaries_reported"] + reported["binaries_invalid"]
                 + reported["binaries_unreported"])
    if partition != summary.get("total"):
        _refuse(out_path, "report_partition_mismatch",
                "reported+invalid+unreported=%d but summary.total=%r"
                % (partition, summary.get("total")))
        return 1
    summary["reported"] = reported

    # Cross-checks. The stream and the summary are produced by different code
    # paths in the launcher, so agreement between them is real evidence that
    # neither counter drifted -- the same fail-closed stance scripts/test.sh
    # takes when it recounts per-binary verdicts against the summary line.
    if len(testcases) != summary.get("total"):
        _refuse(out_path, "count_mismatch",
                "%d binary record(s) but summary.total=%r"
                % (len(testcases), summary.get("total")))
        return 1

    observed = {"passed": 0, "failed": 0, "skipped": 0}
    observed_errors = 0
    # The per-binary side of the assertion dimension. The launcher emits the
    # report triple on a record under exactly one condition -- an ACCEPTED
    # report (u_format_json_testcase: verdict != 3 and state == VALID) -- and
    # increments binaries_reported plus the three running sums under that
    # same condition (src/kernel/test/test_usermode.c). So the records
    # carrying a complete triple ARE binaries_reported, and their sums ARE
    # the run_report totals. Reconciling both here is what stops a stream
    # from claiming an accepted report that no testcase carries.
    observed_reported = 0
    triple_sums = {f: 0 for f in REPORT_TRIPLE}
    for record in testcases:
        status = record.get("status")
        counter = STATUS_TO_COUNTER.get(status)
        if counter is None:
            _refuse(out_path, "unknown_status",
                    "binary record %r carries status=%r"
                    % (record.get("name"), status))
            return 1
        observed[counter] += 1
        if status in NEVER_RAN_STATUSES:
            observed_errors += 1
            # A never-ran binary cannot have self-reported. Refusing the
            # record here rather than dropping the fields keeps the two
            # sides honest: the producer cannot emit this shape, so a
            # stream that carries it was not produced by the launcher
            # this harvester is reconciling, and publishing it would put
            # assertion counts on a testcase the same artifact declares
            # never executed.
            carried = [f for f in REPORT_TRIPLE if f in record]
            if carried:
                _refuse(out_path, "error_self_report",
                        "binary record %r has status=%r but carries %s"
                        % (record.get("name"), status, ", ".join(carried)))
                return 1
        else:
            # All-or-none, because the producer writes the three fields in
            # one block or not at all. A PARTIAL triple is a record whose
            # assertion dimension cannot be summed, and summing it as if the
            # absent fields were zero is how a truncated record would read
            # as a binary that simply asserted nothing.
            carried = [f for f in REPORT_TRIPLE if f in record]
            if carried and len(carried) != len(REPORT_TRIPLE):
                _refuse(out_path, "partial_report",
                        "binary record %r carries %s but not %s"
                        % (record.get("name"), ", ".join(carried),
                           ", ".join(f for f in REPORT_TRIPLE
                                     if f not in carried)))
                return 1
            if carried:
                for field in REPORT_TRIPLE:
                    value = record.get(field)
                    if (isinstance(value, bool)
                            or not isinstance(value, int) or value < 0):
                        _refuse(out_path, "malformed_report",
                                "binary record %r has %s=%r, expected a "
                                "non-negative integer"
                                % (record.get("name"), field, value))
                        return 1
                    triple_sums[field] += value
                observed_reported += 1
    for counter, seen in observed.items():
        if seen != summary.get(counter):
            _refuse(out_path, "count_mismatch",
                    "%d record(s) with status for %s but summary.%s=%r"
                    % (seen, counter, counter, summary.get(counter)))
            return 1

    # The never-ran subset, reconciled exactly like the three counters above
    # and for the same reason: the field and the records it describes are
    # produced by different code paths, so agreement between them is the
    # evidence. Absent rather than zero is a REFUSAL -- an artifact whose
    # producer predates the dimension cannot state that no binary was
    # refused, and silently reading it as "none" is how a refusal-carrying
    # run would publish as an ordinary set of assertion failures.
    errors = summary.get("errors")
    if not isinstance(errors, int) or isinstance(errors, bool) or errors < 0:
        _refuse(out_path, "missing_errors",
                "summary carries no usable errors count (got %r)" % (errors,))
        return 1
    if observed_errors != errors:
        _refuse(out_path, "count_mismatch",
                "%d record(s) with a never-ran status but summary.errors=%r"
                % (observed_errors, errors))
        return 1
    # A subset can never exceed the population it is drawn from. Checked
    # explicitly because both numbers come from the same producer: a counter
    # bug that inflated only `errors` would otherwise reconcile against its
    # own records and publish a JUnit projection with negative failures.
    if errors > summary.get("failed", 0):
        _refuse(out_path, "count_mismatch",
                "summary.errors=%r exceeds summary.failed=%r"
                % (errors, summary.get("failed")))
        return 1
    # The never-ran subset is also a subset of the UNREPORTED partition:
    # every launcher path that counts a binary as never-ran increments
    # rt->unreported in the same breath (src/kernel/test/test_usermode.c --
    # the refusal path and the task_create-failure path), because a binary
    # that never ran had nothing to report. Without this the two dimensions
    # never meet: a stream could carry one ERROR record and simultaneously
    # claim binaries_reported=1/binaries_unreported=0, satisfying the
    # partition check, the status counters and the errors<=failed bound
    # while publishing an artifact that says the same binary both never ran
    # and submitted an accepted report.
    if errors > reported["binaries_unreported"]:
        _refuse(out_path, "count_mismatch",
                "summary.errors=%r exceeds "
                "summary.reported.binaries_unreported=%r"
                % (errors, reported["binaries_unreported"]))
        return 1
    # The accepted-report population, reconciled from both directions: how
    # many records carry a triple, and what those triples sum to. Checking
    # only the count would accept a record whose numbers were rewritten;
    # checking only the sums would accept the totals spread over the wrong
    # number of binaries.
    if observed_reported != reported["binaries_reported"]:
        _refuse(out_path, "count_mismatch",
                "%d record(s) carry an accepted report but "
                "summary.reported.binaries_reported=%r"
                % (observed_reported, reported["binaries_reported"]))
        return 1
    for field in REPORT_TRIPLE:
        if triple_sums[field] != reported[field]:
            _refuse(out_path, "count_mismatch",
                    "binary records sum to %s=%d but "
                    "summary.reported.%s=%r"
                    % (field, triple_sums[field], field, reported[field]))
            return 1

    if len(skip_blocks) != reported.get("skip_records"):
        _refuse(out_path, "count_mismatch",
                "%d skip_block record(s) but summary.reported.skip_records=%r"
                % (len(skip_blocks), reported.get("skip_records")))
        return 1

    # The run-completeness dimension travels as its own record because the
    # summary record is already at klog's 256-byte message limit, but it is
    # folded into `summary` here: a consumer of the FILE sees one summary
    # object carrying `aborted` and `not_run`, and never has to know the
    # transport constraint that split them on the wire.
    aborted = run_meta.get("aborted")
    if not isinstance(aborted, bool):
        _refuse(out_path, "missing_completeness",
                "run_meta.aborted is %r, expected a boolean" % (aborted,))
        return 1
    # `bool` is a subclass of `int` in Python, so the isinstance check alone
    # would accept `"not_run": true` as a count.
    not_run = run_meta.get("not_run")
    if isinstance(not_run, bool) or not isinstance(not_run, int):
        _refuse(out_path, "missing_completeness",
                "run_meta.not_run is %r, expected an integer" % (not_run,))
        return 1
    # A completed run that claims binaries did not run, or an aborted one
    # that claims none were skipped, is self-contradicting -- exactly the
    # internally-consistent-but-wrong artifact this gate exists to reject.
    if not aborted and not_run != 0:
        _refuse(out_path, "inconsistent_completeness",
                "run reports not aborted but not_run=%d" % not_run)
        return 1

    summary["aborted"] = aborted
    summary["not_run"] = not_run

    # Captured per-binary stdout (section 36). The model is built ONCE by
    # scripts/utest-capture.py from the canonical run slice and shared with the
    # XML assembler, so both artifacts carry the same bytes and the same verdict
    # -- a second reconciliation here could disagree with the XML side about
    # whether the run is corrupt, which is the one thing a fail-closed check
    # must never do.
    if _CAPTURE is not None:
        if not _CAPTURE.get("ok"):
            refusal = _CAPTURE.get("refusal") or {}
            _refuse(out_path, "capture_reconciliation",
                    "%s: %s" % (refusal.get("reason", "unknown"),
                                refusal.get("detail", "")))
            return 1
        by_name = {}
        for entry in _CAPTURE.get("binaries", []):
            if entry.get("name"):
                by_name[entry["name"]] = entry
        # Identical population policy to the XML splicer: a captured binary with
        # no testcase means a verdict went missing between two views of ONE run,
        # and publishing anyway would put a plausible artifact over a
        # known-incomplete result set. The reverse (a testcase with no capture)
        # is legal -- skip blocks and binaries that never spawned have none.
        present = set()
        for case in testcases:
            if case.get("name"):
                present.add(case["name"])
        orphans = sorted(n for n in by_name if n not in present)
        if orphans:
            _refuse(out_path, "capture_population_drift",
                    "capture model names binaries with no testcase: %s"
                    % ", ".join(orphans))
            return 1
        for case in testcases:
            entry = by_name.get(case.get("name"))
            if entry is None:
                continue
            case["captured_output"] = entry.get("text", "")
            case["captured_bytes"] = entry.get("total_bytes", 0)
            case["captured_retained_bytes"] = entry.get("retained_bytes", 0)
            case["captured_truncated"] = bool(entry.get("truncated"))
            case["captured_truncated_bytes"] = entry.get("truncated_bytes", 0)
            # The producer's own emission budget, carried as its own pair of
            # fields rather than folded into captured_truncated: that flag is
            # paired with an EXACT captured_truncated_bytes count, and a binary
            # the kernel stopped wrote an unknown number of further bytes that
            # never reached this host at all. Both artifacts must describe the
            # same event the same way, so this mirrors the XML's
            # capture.budget_stop property rather than inventing a third shape.
            case["captured_budget_stop"] = bool(entry.get("budget_stop"))
            if entry.get("budget_stop"):
                case["captured_budget_scope"] = entry.get("budget_scope", "")
                case["captured_budget_limit"] = entry.get("budget_limit", 0)

    # The host-synthesized abort record, appended LAST -- after the capture
    # model's orphan reconciliation, which compares captured binaries against
    # the testcase population. Appending earlier would enter `suite-abort`
    # into that matching as a binary with no captured bytes; the reverse
    # direction is already legal (a testcase with no capture), so keeping it
    # out of the comparison entirely is what makes the two views agree.
    #
    # Producer counters are deliberately untouched: `summary.total` and the
    # `reported` partition describe binaries the LAUNCHER accounted for, and
    # this record is one infrastructure event standing for `not_run` omitted
    # binaries. Publishing it as a binary would put a nonexistent unreported
    # binary into the producer's own assertion dimension.
    synthetic_errors = 0
    if aborted:
        testcases.append({
            "record_kind": "infrastructure",
            "name": "suite-abort",
            "classname": "infrastructure",
            "status": "ERROR",
            "synthetic": True,
            "time_ms": 0,
            "reason": "smoke gate aborted the suite; %d binaries never ran"
                      % not_run,
        })
        synthetic_errors = 1
    summary["synthetic_errors"] = synthetic_errors
    summary["testcase_total"] = summary["total"] + synthetic_errors

    _write_atomic(out_path, {
        "schema": SCHEMA,
        "testcases": testcases,
        "skip_blocks": skip_blocks,
        "summary": summary,
        "run_identity": _IDENTITY,
    })
    return 0


def main(argv):
    global _IDENTITY, _CAPTURE
    identity_path = None
    capture_path = None
    positional = []
    i = 0
    while i < len(argv):
        if argv[i] == "--identity":
            if i + 1 >= len(argv):
                sys.stderr.write("utest-json-harvest: --identity needs a path\n")
                return 2
            identity_path = argv[i + 1]
            i += 2
            continue
        if argv[i] == "--capture":
            if i + 1 >= len(argv):
                sys.stderr.write("utest-json-harvest: --capture needs a path\n")
                return 2
            capture_path = argv[i + 1]
            i += 2
            continue
        positional.append(argv[i])
        i += 1
    if len(positional) != 2:
        sys.stderr.write("usage: utest-json-harvest.py <test-log> <out.json> "
                         "[--identity <identity.json>] [--capture <capture.json>]\n")
        return 2
    _IDENTITY = _load_identity(identity_path)
    _CAPTURE = _load_capture(capture_path)
    return harvest(positional[0], positional[1])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
