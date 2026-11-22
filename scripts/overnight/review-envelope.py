#!/usr/bin/env python3
"""Combine broker-dispatched Codex review legs into ONE findings envelope.

The full review transcripts stay on disk; the envelope carries everything
decision-relevant -- per-kind status, every severity-marked finding line,
artifact path + sha256 + size -- so the main session ingests one bounded JSON
instead of N multi-thousand-line transcripts. Nothing is lost: the envelope
names the artifact for any finding that needs its full surrounding context
(slice-read the artifact at need).

Usage: review-envelope.py [PROJECT_DIR] [--kinds adversarial,consistency,perf]
                          [--todo <todo-path>] [--section <n>] [--since <epoch>]
Reads the newest manifest entry PER KIND from
.claude/overnight/reviews/manifest.jsonl; exits 1 if any requested leg is
missing or incomplete (envelope still printed, with the gap named).

E3 SCOPING: the manifest ACCUMULATES across sections and runs (append-only), so
"newest per kind" would otherwise pull a PRIOR section's stale review (measured:
a TODO-22 self-check aggregated stale TODO-12 morning reviews). Pass `--todo
<path>` to restrict to the section under review (manifest carries `todo` per
entry) and/or `--since <epoch>` to restrict to this dispatch round. Both default
off for back-compat, but the sequencer/skills SHOULD pass `--todo`.
"""
from __future__ import annotations

import collections
import hashlib
import json
import os
import re
import stat
import sys
from pathlib import Path

# The broker (review-broker-codex-dispatch.sh) appends "Turn completed (rc=N)"
# where N is the Codex process exit code. rc != 0 means the leg CRASHED (the
# recurring "codex app-server exited unexpectedly, rc=1" is the live case) and
# produced no usable verdict -- it must be RE-DISPATCHED, but ONLY that leg,
# not the whole 3-leg bundle. Without parsing rc, leg_summary marked a crashed
# leg `complete: True` (the marker is present regardless of rc), so the runner
# could not tell a crash from a clean verdict and re-ran every leg. Measured
# 2026-07-12: 16 rc=1 crash lines / 5+ full re-dispatches in one section.
RC_RE = re.compile(r"Turn completed \(rc=(\d+)\)")
CRASH_SIGNATURES = ("app-server exited unexpectedly", "exited unexpectedly, rc=")
FINDING_RE = re.compile(
    r"(?i)^\s*(?:[-*\d.\s]*)?\[?(critical|high|medium|low|C\d|H\d|M\d|L\d)\b")
MAX_FINDING_LINES = 120
TAIL_LINES = 30


LINE_KEEP = 4096          # bytes of one line kept for its finding and tail entry (both cut to 400 characters)
SCAN_OVERLAP = 128        # bytes carried between chunks, longer than any completion or crash marker


def _scan(path: Path):
    """One streamed pass over a regular file, or None: its size, sha256, the last completion code, whether a
    crash signature appears, the finding lines (at most MAX_FINDING_LINES kept, all counted) and the last
    TAIL_LINES lines. Opened without blocking, so a FIFO or a device never hangs it, and read a chunk at a
    time, so memory is bounded by the chunk and LINE_KEEP whatever the transcript's size."""
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_CLOEXEC", 0))
    except OSError:
        return None
    digest, size, last_rc, crash = hashlib.sha256(), 0, None, False
    findings, found, tail = [], 0, collections.deque(maxlen=TAIL_LINES)
    line, carry = b"", b""

    def finish(raw):
        nonlocal found
        for ln in raw.decode("utf-8", "replace").splitlines() or [""]:
            if FINDING_RE.match(ln):
                found += 1
                if len(findings) < MAX_FINDING_LINES:
                    findings.append(ln.strip()[:400])
            tail.append(ln[:400])
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            return None
        while chunk := os.read(fd, 1 << 20):
            digest.update(chunk)
            size += len(chunk)
            window = (carry + chunk).decode("utf-8", "replace")
            for m in RC_RE.finditer(window):
                last_rc = int(m.group(1))
            crash = crash or any(sig in window for sig in CRASH_SIGNATURES)
            carry = chunk[-SCAN_OVERLAP:]
            parts = chunk.split(b"\n")
            for part in parts[:-1]:
                finish((line + part)[:LINE_KEEP])
                line = b""
            line = (line + parts[-1])[:LINE_KEEP]
        if line:
            finish(line)
    except OSError:
        return None
    finally:
        os.close(fd)
    return {"bytes": size, "sha256": digest.hexdigest(), "rc": last_rc, "crash": crash,
            "findings": findings, "found": found, "tail": list(tail)}


def leg_summary(entry: dict) -> dict:
    path = Path(entry.get("logFile") or "")
    out = {"kind": entry.get("kind"), "jobId": entry.get("jobId"),
           "artifact": str(path), "complete": False, "crashed": False,
           "rc": None, "findings": [], "sha256": None, "bytes": 0, "tail": []}
    # The dispatcher's <log>.done (its exit status, written after the review
    # process exited and after its receipt) is read BEFORE the transcript, so a
    # snapshot taken afterwards is never older than the status it is paired with.
    # Both are opened without blocking and read only as regular files, so a path replaced by a FIFO or a
    # device reads as no status (or an unreadable artifact), never as a hang.
    # Through the one status parser, keeping its read error: a status that is there but could not be read
    # is unknown, read again later, never an unfinished pass to dispatch again.
    status, error = _review_pin()._review_result().read_status_file(str(path) + ".done")
    if error is not None:
        out["unreadable"] = f"its status could not be read ({error.strerror})"
        return out
    done_rc = int(status) if status is not None and status.isdigit() else None  # fail-direction: unfinished
    scan = _scan(path)
    if scan is None:
        out["error"] = "artifact unreadable"
        return out
    out["bytes"] = scan["bytes"]
    out["sha256"] = scan["sha256"]
    # Finished means the dispatcher said so: its <log>.done, or (an older log
    # without one) a "Turn completed (rc=N)" line it appended, the LAST one,
    # since a review can quote an earlier one. Log text such as the companion's
    # mid-run "[codex] Turn completed." never counts. With a status in hand the
    # exit code alone decides a crash: a review can quote a crash diagnostic.
    if done_rc is not None:
        out["complete"] = True
        out["rc"] = done_rc
        out["crashed"] = done_rc != 0
    elif entry.get("done_file") is True:
        pass  # its dispatcher writes <log>.done; without it the review is still running
    else:
        if scan["rc"] is not None:
            out["complete"] = True
            out["rc"] = scan["rc"]
        if out["rc"] is not None:
            out["crashed"] = out["rc"] != 0
        elif scan["crash"]:
            out["crashed"] = True
    if scan["found"] > MAX_FINDING_LINES:
        out["findings_truncated"] = scan["found"] - MAX_FINDING_LINES
    out["findings"] = scan["findings"]
    out["tail"] = scan["tail"]
    # A pass of the reviewer pin (.conclave/reviewers.toml) carries its result
    # on the receipt scripts/codex-dispatch.sh wrote: whether every lens gave a
    # verdict, and the review's worst level. rc 3 there is an INCOMPLETE pass
    # (a lens or the HIGHEST line is missing), not a crash: the reviewer ran,
    # and the same pass is dispatched again.
    # The receipt, not the transcript, says how a pass ended: a review can
    # quote a "Turn completed (rc=0)" line, and the first such line in the
    # text would otherwise decide.
    # A receipt from a dispatcher that publishes <log>.done is credited only
    # once that status is published (it is written after the receipt).
    if entry.get("pass_checked") is True and not (entry.get("done_file") is True and done_rc is None):
        # The status the dispatcher published is its final word and decides: it can follow a receipt that
        # said 0 with a failure (rc 3 when the manifest helper was lost, as codex-dispatch.sh specifies).
        published = entry.get("done_file") is True and done_rc is not None
        rc, highest = (done_rc if published else entry.get("rc")), entry.get("highest") or None
        out["pass"] = {"rc": rc, "highest": highest}
        out["rc"] = rc
        out["complete"] = True
        if rc == 0 and highest in ("P1", "P2", "P3", "none"):
            out["crashed"] = False
        elif rc == 3 or rc == 0:
            out["crashed"] = False
            out["incomplete"] = True          # a lens or the HIGHEST line is missing
        else:
            out["crashed"] = True             # the dispatcher itself failed
    # A pass the review broker launched is the broker's attempt, read as review_pin.receipt_complete (the
    # commit gate's predicate) reads it: its own status can be 0 while the broker published the attempt as
    # ended by its limit (124). Until the broker publishes, the pass is pending; any status but 0 is a
    # failed attempt, dispatched again, even when the pass's own status never came (the broker ended it
    # between its receipt and its status).
    if entry.get("parent"):
        broker_status, error = _broker_status(entry)
        if error is not None:
            out["complete"] = False
            out["unreadable"] = f"its broker's status could not be read ({error.strerror})"
        elif broker_status is None:
            out["complete"] = False
        elif broker_status != "0":
            out["complete"] = True
            out["crashed"] = True
            out["rc"] = int(broker_status) if broker_status.isdigit() else out["rc"]
    return out


def _review_pin():
    scripts = str(Path(__file__).resolve().parents[1])
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    import review_pin
    return review_pin


def _broker_status(entry: dict):
    """(status, error) the broker that launched this pass published: (None, None) when there is none yet,
    the read error when it is there but could not be read."""
    pin = _review_pin()
    broker = pin.broker_log(entry)
    return (None, None) if broker is None else pin._review_result().read_status_file(broker + ".done")


PASS_KINDS = ("plan", "round", "performance", "completion", "reconciliation")


def blocking_levels(project: Path) -> tuple:
    """The levels that hold a stamp, from the reviewer pin; every level when it cannot be read, as the
    commit gate falls back (section_commit_gate._blocking_levels), so the two never disagree."""
    try:
        review_pin = _review_pin()
        return tuple(review_pin.load_pin(project / ".conclave" / "reviewers.toml")["blocking"])
    except Exception:  # noqa: BLE001 -- a broken pin must not make a blocking pass read as clear
        return ("P1", "P2", "P3")


def main(argv) -> int:
    project = Path(argv[0]) if argv and not argv[0].startswith("-") else Path(".")
    kinds = ["adversarial", "consistency", "perf"]
    if "--kinds" in argv:
        kinds = [k.strip() for k in argv[argv.index("--kinds") + 1].split(",")]
    # E3: scope filters so "newest per kind" cannot pull a prior section's review.
    want_todo = None
    if "--todo" in argv:
        want_todo = str(argv[argv.index("--todo") + 1]).replace("\\", "/").lstrip("./")
    # v14 close-out (2026-08-16): FILE scope alone silently ingested an
    # earlier section's legs from the same TODO (observed: a section-26 wave
    # returned section-25 consistency+perf legs, whose findings were about
    # code the change never touched). With --section, an entry that carries a
    # DIFFERENT section is skipped; an entry with NO section field is skipped
    # too -- fail toward redispatch, which costs one dispatch, never a wrong
    # verdict ingested as this section's.
    want_section = None
    if "--section" in argv:
        try:
            want_section = int(argv[argv.index("--section") + 1])
        except (ValueError, IndexError):
            want_section = None
    since = None
    if "--since" in argv:
        try:
            since = int(argv[argv.index("--since") + 1])
        except (ValueError, IndexError):
            since = None
    manifest = project / ".claude/overnight/reviews/manifest.jsonl"
    latest: dict = {}
    unknown: list = []
    pin = _review_pin()
    if not os.path.lexists(manifest):
        print(json.dumps({"error": "no broker manifest", "kinds": {}}))
        return 1
    # The manifest is read as the commit gate reads it (review_pin._newest_first: newest first, a regular
    # file opened without blocking) and by its rule: a line that cannot be read and may be one of these
    # passes stops the read, and every kind not found before it is unknown, never credited from an older row.
    try:
        for ln in pin._newest_first(manifest):
            if not ln.strip():
                continue
            try:
                e = json.loads(ln)
            except ValueError:
                if want_todo is None or pin.unreadable_line_may_be(ln, want_todo):
                    unknown = [k for k in kinds if k not in latest]
                    break
                continue
            if not isinstance(e, dict):
                continue
            if want_todo is not None:
                etodo = str(e.get("todo") or "").replace("\\", "/").lstrip("./")
                if etodo != want_todo:
                    continue  # a different TODO's review -- not ours
            if want_section is not None:
                esec = e.get("section")
                if not isinstance(esec, int) or esec != want_section:
                    continue  # different or unattributed section -- not ours
            if since is not None:
                ets = e.get("ts")
                if not isinstance(ets, int) or ets < since:
                    continue
            latest.setdefault(e.get("kind"), e)  # read newest first: the first row of a kind wins
            if all(k in latest for k in kinds):
                break
    except (OSError, pin.Refused) as exc:
        print(json.dumps({"error": f"the broker manifest cannot be read ({exc})", "kinds": {}}))
        return 1
    # all_complete: every leg produced a completion marker (back-compat).
    # all_clean:    every leg completed AND did not crash (rc==0) -- the real
    #               "the review is usable" signal.
    # needs_redispatch: exactly the legs to re-run (missing OR crashed). The
    #               runner re-dispatches ONLY these, never the whole bundle;
    #               a completed clean leg is reused as-is.
    env = {"kinds": {}, "all_complete": True, "all_clean": True,
           "missing": [], "crashed": [], "incomplete": [], "blocking": [],
           "unreadable": [], "needs_redispatch": []}
    levels = blocking_levels(project)
    for k in kinds:
        if k in unknown:
            # Its newest row may be the torn line: unknown, read again, not dispatched again.
            env["unreadable"].append(k)
            env["all_complete"] = env["all_clean"] = False
            continue
        if k not in latest:
            env["missing"].append(k)
            env["all_complete"] = False
            env["all_clean"] = False
            env["needs_redispatch"].append(k)
            continue
        leg = leg_summary(latest[k])
        env["kinds"][k] = leg
        if leg.get("unreadable"):
            env["unreadable"].append(k)
            env["all_complete"] = env["all_clean"] = False
        elif not leg["complete"]:
            env["all_complete"] = False
            env["all_clean"] = False
            env["needs_redispatch"].append(k)
        elif leg.get("incomplete"):
            # A pass whose review skipped a lens or its HIGHEST line. Not a
            # crash and not a verdict: the same pass is dispatched again.
            env["incomplete"].append(k)
            env["all_clean"] = False
            if k not in env["needs_redispatch"]:
                env["needs_redispatch"].append(k)
        elif leg["crashed"]:
            # Completed-but-crashed: has a marker, no usable verdict. Re-run
            # this leg only; do NOT let it greenlight the review.
            env["crashed"].append(k)
            env["all_clean"] = False
            if k not in env["needs_redispatch"]:
                env["needs_redispatch"].append(k)
        elif k in PASS_KINDS and "pass" not in leg:
            # A pass kind with no checked receipt: still running under the
            # broker, or dispatched some way that checks nothing.
            env["all_clean"] = False
            if k not in env["needs_redispatch"]:
                env["needs_redispatch"].append(k)
        elif leg.get("pass", {}).get("highest") in levels:
            # Usable, and it ended at a level that holds the stamp. The commit
            # gate will refuse the stamp until this pass is dispatched again
            # on the fixed candidate and ends below that level.
            env["blocking"].append(k)
    # THE NEXT STEP, stated where the verdicts are read (2026-09-28). The most
    # common refused call across 21 run logs (85 of 309) was an edit made right
    # after reading a wave's verdicts, before receiving it; 47 of those were
    # followed immediately by the receive. Saying it here costs one line.
    if env["unreadable"]:
        env["next"] = ("read the envelope again for " + ", ".join(env["unreadable"])
                       + " (a status or receipt could not be read; do NOT dispatch it again)"
                       + ("; re-dispatch ONLY " + ", ".join(env["needs_redispatch"]) if env["needs_redispatch"] else ""))
    elif env["needs_redispatch"]:
        env["next"] = ("re-dispatch ONLY " + ", ".join(env["needs_redispatch"])
                       + "; receive the wave once all legs are clean")
    else:
        env["next"] = ("Skill(superpowers:receiving-code-review) over ALL legs of this wave "
                       "BEFORE any Edit, Write or fix")
        if env["blocking"]:
            env["next"] += ("; then fix the findings of " + ", ".join(env["blocking"])
                            + ", commit, and dispatch " + ("it" if len(env["blocking"]) == 1 else "them")
                            + " again: a pass that ended at " + " or ".join(levels) + " does not carry a stamp")
    print(json.dumps(env, indent=1))
    # Exit 0 only when the review is genuinely usable (all legs clean). A
    # crashed leg used to slip through on all_complete; gating on all_clean
    # stops the runner proceeding on a non-verdict.
    return 0 if env["all_clean"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
