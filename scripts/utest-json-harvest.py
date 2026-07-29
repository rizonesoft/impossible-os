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

SCHEMA = "utest-json-v1"
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
FRAME_ANNOUNCE_RE = re.compile(
    r"UTEST-([0-9a-f]{8}): \[UTEST-FRAME\] v=1 run=([0-9]+)")
FRAME_END_RE = re.compile(
    r"UTEST-([0-9a-f]{8}): \[UTEST-FRAME-END\] run=([0-9]+) records=([0-9]+)")

# Status vocabulary of a binary record, mapped to the summary counter it must
# agree with. A record carrying anything else is a producer bug, not a new
# outcome to tolerate.
STATUS_TO_COUNTER = {"PASS": "passed", "FAIL": "failed", "SKIP": "skipped"}


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
    _write_atomic(out_path, envelope)
    sys.stderr.write("utest-json-harvest: refused -- %s%s\n"
                     % (reason, (": " + detail) if detail else ""))


def _learn_frame(lines):
    """Return this boot's launcher frame prefix, or None.

    The nonce lives in the TAG and nowhere else: the announcement body does
    not restate it, because a body copy would survive the kernel's disk-sink
    alias and put the value back into the one log ring 3 can read. The FIRST
    announcement wins -- the launcher announces before it creates any ring-3
    task, so nothing of that run can have printed ahead of it. A later,
    different nonce is ring-3 output imitating the frame; its records simply
    never match the learned prefix. Reporting that conflict is
    scripts/test.sh's job -- this harvester's contract is to assemble only
    records it can attribute.
    """
    for line in lines:
        match = FRAME_ANNOUNCE_RE.search(line)
        if match:
            return "UTEST-%s: " % match.group(1)
    return None


def _last_run_slice(lines, frame):
    """Narrow `lines` to the LAST complete framed run.

    test_usermode_run() is documented safe to call repeatedly and the nonce
    is deliberately per-BOOT, not per-run, so a boot with two launcher runs
    puts two full record streams under the same prefix. Collecting both
    yields duplicate summary / run_report / run_meta records, which this
    harvester correctly refuses as a cut-and-resumed stream -- turning a
    legitimate double run into a failed artifact.

    Each run brackets itself with `[UTEST-FRAME] ... run=N` and
    `[UTEST-FRAME-END] run=N`, so the last closed bracket pair is the run to
    publish. An unterminated trailing run is NOT chosen: an artifact must
    describe a run that finished.
    """
    begin = None
    best = None
    for index, line in enumerate(lines):
        match = FRAME_ANNOUNCE_RE.search(line)
        if match and line.find(frame) >= 0:
            begin = (index, match.group(2))
            continue
        match = FRAME_END_RE.search(line)
        if match and line.find(frame) >= 0 and begin is not None:
            if match.group(2) == begin[1]:
                best = (begin[0], index)
                begin = None
    if best is None:
        return None
    return lines[best[0]:best[1] + 1]


def _extract_records(log_path):
    """Return the payload strings following each FRAMED [UTEST-JSON] marker."""
    with open(log_path, "r", encoding="utf-8", errors="replace") as handle:
        lines = [ANSI_RE.sub("", line).rstrip("\n") for line in handle]

    frame = _learn_frame(lines)
    if frame is None:
        return []

    lines = _last_run_slice(lines, frame)
    if lines is None:
        return []
    framed_marker = frame + MARKER
    records = []
    for line in lines:
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
    for record in testcases:
        counter = STATUS_TO_COUNTER.get(record.get("status"))
        if counter is None:
            _refuse(out_path, "unknown_status",
                    "binary record %r carries status=%r"
                    % (record.get("name"), record.get("status")))
            return 1
        observed[counter] += 1
    for counter, seen in observed.items():
        if seen != summary.get(counter):
            _refuse(out_path, "count_mismatch",
                    "%d record(s) with status for %s but summary.%s=%r"
                    % (seen, counter, counter, summary.get(counter)))
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

    _write_atomic(out_path, {
        "schema": SCHEMA,
        "testcases": testcases,
        "skip_blocks": skip_blocks,
        "summary": summary,
    })
    return 0


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: utest-json-harvest.py <test-log> <out.json>\n")
        return 2
    return harvest(argv[0], argv[1])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
