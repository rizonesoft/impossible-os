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

CLI: `utest-frame.py <log>` prints one JSON object (see parse()) and exits 0
when a complete, reconciled run was found, 1 when framing is present but no
run reconciles, and 2 when no frame was learned at all.
"""

import json
import re
import sys

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
ANNOUNCE_RE = re.compile(
    r"UTEST-([0-9a-f]{8}): \[UTEST-FRAME\] v=1 run=([0-9]{1,10})(?![0-9])")
END_RE = re.compile(
    r"UTEST-([0-9a-f]{8}): \[UTEST-FRAME-END\] run=([0-9]{1,10})(?![0-9])"
    r" records=([0-9]{1,10})(?![0-9])")

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


def strip_ansi(line):
    return ANSI_RE.sub("", line).rstrip("\n")


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
    nonce = None
    prefix = None
    foreign = set()
    conflict_overflow = [False]
    runs = []
    run_overflow = False
    unpaired = 0
    # A run that CLOSED but did not reconcile is fatal, and stays fatal: the
    # newest closed stream failed the mandatory count check, so falling back
    # to an older run that happened to reconcile would publish stale results
    # under a verdict the newest run never earned.
    bad_close = 0

    open_run = None       # ordinal of the currently open announcement
    open_count = 0        # framed lines since it, inclusive
    open_lines = []
    last_complete = None
    last_lines = []

    for raw in lines:
        line = raw if isinstance(raw, str) else str(raw)

        if CRASH_REPLAY_PREFIX in line:
            # Previous boot's evidence, not this boot's stream.
            continue

        announce = ANNOUNCE_RE.search(line)
        if announce:
            if nonce is None:
                nonce = announce.group(1)
                prefix = "UTEST-%s: " % nonce
            elif announce.group(1) != nonce:
                # An imitator announcing its own frame. Its records can never
                # match the learned prefix; record the conflict and move on.
                if len(foreign) < MAX_TRACKED_CONFLICTS:
                    foreign.add(announce.group(1))
                else:
                    conflict_overflow[0] = True
                continue

        if prefix is None or prefix not in line:
            continue

        # From here the line is framed with THIS boot's nonce.
        if announce is not None:
            if open_run is not None:
                # Re-opened before its terminator: the previous run lost its
                # tail and can never reconcile.
                unpaired += 1
            open_run = announce.group(2)
            open_count = 1
            open_lines = [line]
            continue

        if open_run is not None:
            open_count += 1
            open_lines.append(line)

        end = END_RE.search(line)
        if end and end.group(1) == nonce:
            if open_run is None or end.group(2) != open_run:
                unpaired += 1
                open_run = None
                open_lines = []
                continue
            declared = int(end.group(3))
            complete = (open_count == declared + 1)
            if len(runs) < MAX_TRACKED_RUNS:
                runs.append({
                    "run": int(open_run),
                    "declared": declared,
                    "actual": open_count,
                    "complete": complete,
                })
            else:
                run_overflow = True
            if complete:
                last_complete = {
                    "run": int(open_run),
                    "declared": declared,
                    "actual": open_count,
                    "complete": True,
                }
                last_lines = open_lines
            else:
                # Do NOT keep an older last_complete: the newest closed run
                # is the run, and it failed reconciliation.
                bad_close += 1
                last_complete = None
                last_lines = []
            open_run = None
            open_lines = []

    if open_run is not None:
        # A trailing announcement with no terminator. Never published -- an
        # artifact must describe a run that finished -- but NOT fatal on its
        # own: the run may simply have been cut off after an earlier one
        # completed cleanly.
        if len(runs) < MAX_TRACKED_RUNS:
            runs.append({
                "run": int(open_run),
                "declared": None,
                "actual": open_count,
                "complete": False,
            })
        else:
            run_overflow = True

    return {
        "nonce": nonce,
        "prefix": prefix,
        "runs": runs,
        "unpaired": unpaired,
        "bad_close": bad_close,
        "overflow": bool(run_overflow or conflict_overflow[0]),
        "conflicts": len(foreign) + (MAX_TRACKED_CONFLICTS
                                     if conflict_overflow[0] else 0),
        "last_complete": last_complete,
        "lines": last_lines,
        "ok": bool(nonce and unpaired == 0 and bad_close == 0 and not foreign
                   and not run_overflow and not conflict_overflow[0]
                   and last_complete is not None),
    }


def parse_file(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return parse(strip_ansi(line) for line in handle)


def main(argv):
    if len(argv) != 1:
        sys.stderr.write("usage: utest-frame.py <serial-log>\n")
        return 2
    try:
        result = parse_file(argv[0])
    except OSError as exc:
        sys.stderr.write("utest-frame: cannot read %s: %s\n" % (argv[0], exc))
        return 2

    # The framed line list is for in-process consumers; the CLI reports
    # structure only, so a caller does not pull a whole run through a pipe.
    emitted = dict(result)
    emitted["lines"] = len(result["lines"])
    sys.stdout.write(json.dumps(emitted) + "\n")

    if result["nonce"] is None:
        return 2
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
