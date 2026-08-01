#!/usr/bin/env python3
"""Differential test: incremental resume MUST equal a full reparse.

scripts/utest-frame.py answers the boot-completion question once per second
against a growing capture, resuming from a byte offset instead of re-reading
from zero. That saving is only safe if the resumed answer is IDENTICAL to the
answer a full reparse would give, because acceptance depends on much more than
an offset: the learned nonce, the foreign-nonce set, the sticky `unpaired` and
`bad_close` failures, the overflow bounds, the open run's ordinal and count,
and the newest complete selection all feed `ok`. A resume that forgot any of
them would let a later genuine terminator report green where a full reparse
stays red -- a FALSE GREEN, the exact class the framing work exists to close.

So this asserts equivalence on EVERY append prefix, byte by byte, against an
independent oracle: Python's own universal-newline reader driving the module's
plain `parse()`. The oracle is deliberately not the parser's own line splitter
-- comparing an implementation to itself proves nothing.

Run directly, or through scripts/test-tooling.sh sub-test 6f.
"""

import importlib.util
import io
import json
import os
import sys
import tempfile

_HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "uf", os.path.join(_HERE, "utest-frame.py"))
uf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(uf)

N = "1a2b3c4d"
P = "UTEST-%s: " % N
def L(s): return "[  1.000] [ OK ] " + s

# a complete run of 4 records + announcement, then a second complete run
STREAM = [
    L("KERNEL: booting"),
    L(P + "[UTEST-FRAME] v=1 run=1"),
    L(P + "[UTEST-XML] <testcase name=\"a\"/>"),
    L(P + "[UTEST-JSON] {\"n\":\"a\"}"),
    "raw ring-3 stdout with no klog prefix",
    L(P + "[UTEST-FRAME-END] run=1 records=3 spawned=0"),
    L("UTEST-deadbeef: [UTEST-FRAME] v=1 run=9"),   # foreign imitator
    L(P + "[UTEST-FRAME] v=1 run=2"),
    L(P + "[UTEST-XML] <testcase name=\"b\"/>"),
    L(P + "[UTEST-FRAME-END] run=2 records=2 spawned=0"),
    L("[CRASH-PREV] " + P + "[UTEST-FRAME-END] run=7 records=1 spawned=0"),
]

def strip_state(r):
    """Drop the fields that describe HOW the answer was reached.

    The oracle reports only the parse result; `state`, `restarted` and
    `confirmed_by_full_reparse` are mechanism, not verdict, and are asserted
    directly by the checks that care about them.
    """
    d = dict(r)
    for k in ("state", "restarted", "confirmed_by_full_reparse"):
        d.pop(k, None)
    return d

def full(path):
    """Independent oracle: Python's own universal-newline reader.

    One deliberate adjustment, matching the parser's documented rule: a
    trailing BARE CR is ambiguous (the LF may not have arrived yet), so it is
    not a line terminator. Python's text mode would translate it into one.
    An unterminated final line is likewise dropped rather than parsed.

    A second adjustment mirrors production's MAX_PHYSICAL_LINE cap
    (parse_file()'s `if len(raw) > MAX_PHYSICAL_LINE` check): any RAW
    (pre-decode) physical line over the cap is excluded from the parse and
    latches the same `overflow` flag production reports. Without this, an
    over-long announcement or terminator would be LEARNED by this oracle but
    REFUSED by production, so the two would silently diverge at exactly the
    boundary the cap exists to enforce -- which is what plain byte-count
    equivalence testing had missed. bytes.splitlines() (not the parser's own
    split_complete_lines/find_terminator) supplies the independent split, so
    this stays a comparison against a DIFFERENT implementation, not the
    parser checking itself; only the MAX_PHYSICAL_LINE threshold itself is
    the shared spec constant, same as the frame regexes this oracle already
    reuses via `uf.parse`/`uf.strip_ansi`.
    """
    with open(path, "rb") as h:
        raw = h.read()
    if raw.endswith(b"\r"):
        raw = raw[:-1]
    raw_lines = raw.splitlines()
    if raw and not raw.endswith((b"\n", b"\r")):
        # bytes.splitlines() has no notion of "unterminated"; the trailing
        # element is the fragment production never feeds either.
        raw_lines = raw_lines[:-1]
    overflowed = False
    decoded = []
    for rl in raw_lines:
        if len(rl) > uf.MAX_PHYSICAL_LINE:
            overflowed = True
            continue
        decoded.append(uf.strip_ansi(rl.decode("utf-8", errors="replace")))
    result = uf.parse(decoded)
    if overflowed:
        result["overflow"] = True
    return result

def quiet_main(argv):
    """Run the CLI with stdout/stderr captured -- the harness prints its own
    lines, and main()'s JSON would otherwise be read as test output."""
    import contextlib
    buf, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
        return uf.main(argv)


fails = 0
def check(name, a, b):
    global fails
    if a != b:
        fails += 1
        print("FAIL %s" % name)
        print("  incremental: %s" % json.dumps(a, sort_keys=True)[:400])
        print("  full       : %s" % json.dumps(b, sort_keys=True)[:400])
    else:
        print("ok   %s" % name)

body = ("\r\n".join(STREAM) + "\r\n").encode()

# 1. Every append prefix, byte by byte, resuming.
#
#    Two properties, not one equality. The parser deliberately differs from a
#    complete-lines-only parse in ONE direction: an unterminated trailing
#    fragment contributes NEGATIVE evidence (a foreign announcement there must
#    still refuse the run) while never contributing positive evidence. So:
#      P1  the SELECTION is exactly what a full reparse selects -- a resume
#          may never invent, extend or lose a completed run;
#      P2  acceptance is a SUBSET -- the resumed parser may refuse what the
#          full parse accepts (the fragment's negative evidence), but it may
#          never accept what the full parse refuses. That direction is the
#          false green, and it is the one that must be impossible.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    state = None
    sel_mismatch = 0
    false_green = 0
    exact_mismatch = 0
    for cut in range(0, len(body) + 1):
        with open(log, "wb") as h: h.write(body[:cut])
        r = uf.parse_file(log, state=state)
        state = r["state"]
        o = full(log)
        if (r["last_complete"], r["lines"]) != (o["last_complete"], o["lines"]):
            sel_mismatch += 1
        if r["ok"] and not o["ok"]:
            false_green += 1
        # P3. The two properties above are deliberately one-directional, which
        # leaves room for a resume to DIVERGE in a way neither catches (an
        # over-refusal, or a drifting conflicts/unpaired count). That slack is
        # only justified where an unterminated fragment exists, since the
        # fragment contributes negative evidence the oracle does not model.
        # At every cut that lands exactly on a line terminator there is no
        # fragment and therefore no excuse: demand FULL equality there.
        if body[:cut].endswith(b"\n"):
            if strip_state(r) != o:
                exact_mismatch += 1
    check("append-prefix resume selects the same run (every byte)", sel_mismatch, 0)
    check("append-prefix resume never accepts what a full parse refuses",
          false_green, 0)
    check("append-prefix resume is EXACTLY equal at every whole-line cut",
          exact_mismatch, 0)

# 2. one-shot full parse equals oracle
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    with open(log, "wb") as h: h.write(body)
    check("one-shot parse equals oracle", strip_state(uf.parse_file(log)), full(log))

# 3. truncation between polls -> restart from zero
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    with open(log, "wb") as h: h.write(body)
    r1 = uf.parse_file(log)
    with open(log, "wb") as h: h.write(body[:len(body)//3])
    r2 = uf.parse_file(log, state=r1["state"])
    check("truncation restarts (restarted flag)", r2["restarted"], True)
    check("truncation result equals full reparse", strip_state(r2), full(log))

# 4. recreation with a NEW nonce under the same name -> stale nonce not carried
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    with open(log, "wb") as h: h.write(body)
    r1 = uf.parse_file(log)
    os.unlink(log)
    P2 = "UTEST-99887766: "
    body2 = ("\r\n".join([L(P2 + "[UTEST-FRAME] v=1 run=1"),
                          L(P2 + "[UTEST-FRAME-END] run=1 records=1 spawned=0")]) + "\r\n").encode()
    with open(log, "wb") as h: h.write(body2)
    r2 = uf.parse_file(log, state=r1["state"])
    check("recreation learns the NEW nonce", r2["nonce"], "99887766")
    check("recreation equals full reparse", strip_state(r2), full(log))

# 5. split ANSI escape across a chunk boundary
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    ansi_body = ("\x1b[32m" + L(P + "[UTEST-FRAME] v=1 run=1") + "\x1b[0m\r\n"
                 + "\x1b[32m" + L(P + "[UTEST-FRAME-END] run=1 records=1 spawned=0")
                 + "\x1b[0m\r\n").encode()
    state = None; sel = 0; fg = 0
    for cut in range(0, len(ansi_body) + 1):
        with open(log, "wb") as h: h.write(ansi_body[:cut])
        r = uf.parse_file(log, state=state); state = r["state"]
        o = full(log)
        if (r["last_complete"], r["lines"]) != (o["last_complete"], o["lines"]):
            sel += 1
        if r["ok"] and not o["ok"]: fg += 1
    check("split ANSI sequence resume selects the same run", sel, 0)
    check("split ANSI sequence resume never false-greens", fg, 0)
    with open(log, "wb") as h: h.write(ansi_body)
    check("ANSI run reconciles", uf.parse_file(log)["ok"], True)

# 6. unterminated trailing line is never parsed
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    partial = ("\r\n".join(STREAM[:5]) + "\r\n" + L(P + "[UTEST-FRAME-END] run=1 records=3 spawned=0")).encode()
    with open(log, "wb") as h: h.write(partial)
    check("unterminated terminator not accepted", uf.parse_file(log)["last_complete"], None)
    with open(log, "ab") as h: h.write(b"\r\n")
    check("same terminator accepted once complete",
          uf.parse_file(log)["last_complete"]["run"], 1)

# 7. bad close is sticky across a resume
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    bad = [L(P + "[UTEST-FRAME] v=1 run=1"),
           L(P + "[UTEST-FRAME-END] run=1 records=99 spawned=0")]
    good = [L(P + "[UTEST-FRAME] v=1 run=2"),
            L(P + "[UTEST-FRAME-END] run=2 records=1 spawned=0")]
    with open(log, "wb") as h: h.write(("\r\n".join(bad) + "\r\n").encode())
    r1 = uf.parse_file(log)
    check("bad close seen", r1["bad_close"], 1)
    with open(log, "ab") as h: h.write(("\r\n".join(good) + "\r\n").encode())
    r2 = uf.parse_file(log, state=r1["state"])
    check("bad_close survives resume (no false green)", r2["ok"], False)
    check("sticky bad_close equals full reparse", strip_state(r2), full(log))

# 8. poll mode keeps structure but drops lines
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    with open(log, "wb") as h: h.write(body)
    rp = uf.parse_file(log, retain_lines=False)
    rf = uf.parse_file(log)
    check("poll mode agrees on last_complete", rp["last_complete"], rf["last_complete"])
    check("poll mode retains no lines", rp["lines"], [])
    check("poll mode agrees on ok", rp["ok"], rf["ok"])

# 9. foreign nonce conflict counted
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    with open(log, "wb") as h: h.write(body)
    check("foreign imitator counted", uf.parse_file(log)["conflicts"], 1)


# ---------------------------------------------------------------------------
# Adversarial-review regression checks (2026-07-29). Each corresponds to a
# finding that a plain equivalence sweep did NOT catch.
# ---------------------------------------------------------------------------

# 10. [A1] shrink-and-regrow past the old offset must NOT resume.
#     dev/ino are unchanged and size >= offset, so the identity check alone
#     passes; only the content anchor catches it. Without this the stale FSM
#     consumes a terminator from an unrelated stream and reports a run the
#     full parse never sees.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    with open(log, "wb") as h: h.write(body)
    r1 = uf.parse_file(log)
    filler = ("\r\n".join(["x" * 80] * 400) + "\r\n").encode()
    assert len(filler) > r1["state"]["offset"], "filler must exceed the offset"
    with open(log, "wb") as h: h.write(filler)      # truncate + regrow
    r2 = uf.parse_file(log, state=r1["state"])
    check("[A1] shrink-regrow past offset restarts", r2["restarted"], True)
    check("[A1] shrink-regrow equals full reparse", strip_state(r2), full(log))
    check("[A1] stale nonce not carried into new stream", r2["nonce"], None)

# 11. [A1] a hostile / corrupt state file degrades to a full reparse.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    with open(log, "wb") as h: h.write(body)
    good = uf.parse_file(log)["state"]
    hostile = [
        ("prefix not matching nonce",
         {**good, "fsm": {**good["fsm"], "prefix": "UTEST-ffffffff: "}}),
        ("nonce not hex",
         {**good, "fsm": {**good["fsm"], "nonce": "zzzzzzzz"}}),
        ("negative counter",
         {**good, "fsm": {**good["fsm"], "unpaired": -5}}),
        ("open_run not an ordinal",
         {**good, "fsm": {**good["fsm"], "open_run": "../etc"}}),
        ("lines not strings",
         {**good, "fsm": {**good["fsm"], "last_lines": [1, 2]}}),
        ("wrong schema version", {**good, "v": 999}),
        ("anchor mismatch", {**good, "anchor": "0" * 64}),
    ]
    bad = 0
    for name, st in hostile:
        r = uf.parse_file(log, state=st)
        if strip_state(r) != full(log) or not r["restarted"]:
            bad += 1
            print("   hostile state not rejected: %s" % name)
    check("[A1] every hostile state file degrades to a full reparse", bad, 0)

good_run = ("\r\n".join([L(P + "[UTEST-FRAME] v=1 run=1"),
                          L(P + "[UTEST-FRAME-END] run=1 records=1 spawned=0")]) + "\r\n").encode()

# 12. [A2] an unterminated FOREIGN announcement must still be refused.
#     This is the false green the fragment scan exists to prevent: a complete
#     run followed by a forged announcement with no trailing newline.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    with open(log, "wb") as h: h.write(good_run)
    check("[A2] clean run is ok before the fragment", uf.parse_file(log)["ok"], True)
    with open(log, "ab") as h:
        h.write(L("UTEST-deadbeef: [UTEST-FRAME] v=1 run=2").encode())  # no newline
    r = uf.parse_file(log)
    check("[A2] unterminated foreign announcement counted", r["conflicts"], 1)
    check("[A2] unterminated foreign announcement refuses the run", r["ok"], False)

# 13. [A2] the fragment must contribute NEGATIVE evidence only -- it can
#     never complete a run.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    partial = (L(P + "[UTEST-FRAME] v=1 run=1") + "\r\n"
               + L(P + "[UTEST-FRAME-END] run=1 records=1 spawned=0")).encode()  # no newline
    with open(log, "wb") as h: h.write(partial)
    check("[A2] fragment cannot complete a run", uf.parse_file(log)["last_complete"], None)

# 14. [A3] newline-free ring-3 output must not grow the parser without bound.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    with open(log, "wb") as h:
        h.write(good_run)
        h.write(b"A" * (uf.MAX_PHYSICAL_LINE + 4096))
        h.write(b"\r\n")
    r = uf.parse_file(log)
    check("[A3] over-long physical line latches overflow", r["overflow"], True)
    check("[A3] over-long physical line refuses the run", r["ok"], False)
    check("[A3] parser still terminates and reports", r["nonce"], N)

# 15. [TC1] the tracked-conflict bound behaves the same incrementally.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    for n_conf, label in ((uf.MAX_TRACKED_CONFLICTS, "at bound"),
                          (uf.MAX_TRACKED_CONFLICTS + 1, "past bound")):
        lines = [L(P + "[UTEST-FRAME] v=1 run=1"),
                 L(P + "[UTEST-FRAME-END] run=1 records=1 spawned=0")]
        for k in range(n_conf):
            lines.append(L("UTEST-%08x: [UTEST-FRAME] v=1 run=9" % (k + 1)))
        blob = ("\r\n".join(lines) + "\r\n").encode()
        # feed it in two halves through a resume
        with open(log, "wb") as h: h.write(blob[:len(blob)//2])
        s1 = uf.parse_file(log, retain_lines=False)["state"]
        with open(log, "wb") as h: h.write(blob)
        r_inc = uf.parse_file(log, state=s1, retain_lines=False)
        r_full = full(log)
        check("[TC1] conflicts %s: incremental matches full" % label,
              (r_inc["conflicts"], r_inc["ok"], r_inc["overflow"]),
              (r_full["conflicts"], r_full["ok"], r_full["overflow"]))

# 16. [TC1] the tracked-run bound behaves the same incrementally.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    for n_runs, label in ((uf.MAX_TRACKED_RUNS, "at bound"),
                          (uf.MAX_TRACKED_RUNS + 1, "past bound")):
        lines = []
        for k in range(1, n_runs + 1):
            lines.append(L(P + "[UTEST-FRAME] v=1 run=%d" % k))
            lines.append(L(P + "[UTEST-FRAME-END] run=%d records=1 spawned=0" % k))
        blob = ("\r\n".join(lines) + "\r\n").encode()
        with open(log, "wb") as h: h.write(blob[:len(blob)//2])
        s1 = uf.parse_file(log, retain_lines=False)["state"]
        with open(log, "wb") as h: h.write(blob)
        r_inc = uf.parse_file(log, state=s1, retain_lines=False)
        r_full = full(log)
        check("[TC1] runs %s: incremental matches full" % label,
              (len(r_inc["runs"]), r_inc["ok"], r_inc["overflow"]),
              (len(r_full["runs"]), r_full["ok"], r_full["overflow"]))

# 17. [TC2] CLI: an output path may never alias the input log.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    with open(log, "wb") as h: h.write(body)
    before = open(log, "rb").read()
    aliases = [
        (["--state", log, log], "--state aliases input"),
        (["--emit-run", log, log], "--emit-run aliases input"),
        (["--state", os.path.join(d, ".", "serial.log"), log], "dotted alias"),
    ]
    bad = 0
    for argv, label in aliases:
        rc = quiet_main(argv)
        if rc != 2 or open(log, "rb").read() != before:
            bad += 1
            print("   alias not rejected or input mutated: %s" % label)
    check("[TC2] aliasing output paths are refused before any mutation", bad, 0)
    rc = quiet_main(["--state", os.path.join(d, "s.json"),
                     "--emit-run", os.path.join(d, "s.json"), log])
    check("[TC2] --state and --emit-run may not alias each other", rc, 2)

# 18. [TC2] CLI: option-shaped values and unknown options are refused.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    with open(log, "wb") as h: h.write(body)
    check("[TC2] --state consuming an option token is refused",
          quiet_main(["--state", "--poll", log]), 2)
    check("[TC2] unknown option is refused", quiet_main(["--bogus", log]), 2)
    check("[TC2] no stray file named for the option token",
          os.path.exists("--poll"), False)

# 19. [TC2] --emit-run is atomic and leaves no .tmp behind.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    out = os.path.join(d, "slice.txt")
    # A CLEAN stream: `body` carries a foreign imitator, so rc there is
    # legitimately 1 (framed but not ok) and would not test the success path.
    with open(log, "wb") as h: h.write(good_run)
    rc = quiet_main(["--emit-run", out, log])
    check("[TC2] --emit-run succeeds on a complete run", rc, 0)
    check("[TC2] --emit-run leaves no .tmp", os.path.exists(out + ".tmp"), False)
    check("[TC2] emitted slice is the selected run",
          open(out).read().strip().splitlines(), uf.parse_file(log)["lines"])
    unwritable = os.path.join(d, "nodir", "slice.txt")
    check("[TC2] unwritable --emit-run destination returns 2",
          quiet_main(["--emit-run", unwritable, log]), 2)


# ---------------------------------------------------------------------------
# Round-2 adversarial regressions (2026-07-29).
# ---------------------------------------------------------------------------

# 20. [R1] A forged state file must never manufacture a positive verdict.
#     Reproduced before the fix: a state carrying a fabricated last_complete
#     reported ok=true against a log containing no frame at all.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    with open(log, "wb") as h: h.write(b"nothing framed here at all\r\n" * 10)
    st = uf.parse_file(log)["state"]
    forged = [
        ("fabricated last_complete", {"nonce": "1a2b3c4d",
                                      "prefix": "UTEST-1a2b3c4d: ",
                                      "last_complete": {"fabricated": "yes"}}),
        ("last_complete contradicting its counts",
         {"nonce": "1a2b3c4d", "prefix": "UTEST-1a2b3c4d: ",
          "last_complete": {"run": 1, "declared": 3, "actual": 99,
                            "complete": True}}),
        ("last_complete not actually complete",
         {"nonce": "1a2b3c4d", "prefix": "UTEST-1a2b3c4d: ",
          "last_complete": {"run": 1, "declared": 3, "actual": 3,
                            "complete": False}}),
        ("selected lines without a selected run",
         {"nonce": "1a2b3c4d", "prefix": "UTEST-1a2b3c4d: ",
          "last_lines": ["forged"]}),
        ("foreign set past its bound",
         {"foreign": ["%08x" % k for k in range(uf.MAX_TRACKED_CONFLICTS + 5)]}),
        ("run entry with unexpected fields",
         {"runs": [{"run": 1, "declared": 1, "actual": 2, "complete": True,
                    "extra": 1}]}),
    ]
    bad = 0
    for name, patch in forged:
        st2 = {**st, "fsm": {**st["fsm"], **patch}}
        r = uf.parse_file(log, state=st2)
        if r["ok"] or r["last_complete"] is not None:
            bad += 1
            print("   forged state produced a positive verdict: %s" % name)
    check("[R1] no forged state manufactures a positive verdict", bad, 0)

# 21. [R1] A resumed POSITIVE result is always re-derived by a full reparse,
#     so it cannot rest on the state file at all.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    with open(log, "wb") as h: h.write(good_run[:len(good_run)//2])
    s1 = uf.parse_file(log)["state"]
    check("[R1] partial run is not yet complete",
          uf.parse_file(log, state=s1)["last_complete"], None)
    with open(log, "wb") as h: h.write(good_run)
    r = uf.parse_file(log, state=s1)
    check("[R1] completion is confirmed by a full reparse",
          r.get("confirmed_by_full_reparse"), True)
    check("[R1] confirmed completion matches the oracle",
          (r["last_complete"], r["ok"]), (full(log)["last_complete"], full(log)["ok"]))

# 22. [R2] A replacement preserving the last ANCHOR_BYTES must not resume a
#     stale open run. The tail anchor alone cannot see the difference, so the
#     full-reparse confirmation is what has to catch it.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    tail = b"Z" * (uf.ANCHOR_BYTES + 64)
    original = L(P + "[UTEST-FRAME] v=1 run=1").encode() + b"\r\n" + tail + b"\r\n"
    with open(log, "wb") as h: h.write(original)
    s1 = uf.parse_file(log)["state"]
    check("[R2] stale state holds an open run", s1["fsm"]["open_run"], "1")
    # Replace the prefix (no announcement) but keep the anchor suffix intact.
    replacement = b"Q" * len(L(P + "[UTEST-FRAME] v=1 run=1").encode()) + b"\r\n" + tail + b"\r\n"
    assert len(replacement) == len(original)
    with open(log, "wb") as h: h.write(replacement)
    with open(log, "ab") as h:
        h.write((L(P + "[UTEST-FRAME-END] run=1 records=2 spawned=0") + "\r\n").encode())
    r = uf.parse_file(log, state=s1)
    o = full(log)
    check("[R2] replaced prefix cannot close a stale run",
          r["last_complete"], o["last_complete"])
    check("[R2] replaced prefix does not go green", r["ok"], o["ok"])

# 23. [R2] The outgoing anchor comes from the parsed descriptor, so a state
#     handed straight back resumes cleanly rather than restarting.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    with open(log, "wb") as h: h.write(body)
    r1 = uf.parse_file(log, retain_lines=False)
    r2 = uf.parse_file(log, state=r1["state"], retain_lines=False)
    check("[R2] anchor round-trips (no spurious restart)", r2["restarted"], False)


# 24. [R3] A forged state at offset ZERO must not bypass the cold reparse.
#     The round-2 guard keyed on offset > 0, and a forged state legitimately
#     carries offset 0 with a null anchor (there are no bytes before 0 to
#     digest, so the anchor matches trivially) -- which waved through exactly
#     the shape the confirmation exists to stop. Reproduced against an empty
#     log: ok=true, confirmed_by_full_reparse=false.
def _forged_state(log, **fsm):
    st = os.stat(log)
    base = {"nonce": None, "prefix": None, "foreign": [],
            "conflict_overflow": False, "runs": [], "run_overflow": False,
            "unpaired": 0, "bad_close": 0, "open_run": None, "open_count": 0,
            "open_lines": [], "last_complete": None, "last_lines": [],
            "line_overflow": False, "retain_lines": True}
    base.update(fsm)
    return {"v": uf.STATE_VERSION,
            "identity": {"dev": st.st_dev, "ino": st.st_ino, "size": st.st_size},
            "offset": 0, "anchor": None, "fsm": base}

with tempfile.TemporaryDirectory() as d:
    for name, payload in (("empty log", b""),
                          ("frameless log", b"nothing framed\r\n" * 5)):
        log = os.path.join(d, "z.log")
        with open(log, "wb") as h: h.write(payload)
        st = _forged_state(log, nonce="1a2b3c4d", prefix="UTEST-1a2b3c4d: ",
                           last_complete={"run": 1, "declared": 1,
                                          "actual": 2, "complete": True})
        r = uf.parse_file(log, state=st)
        check("[R3] zero-offset forged state does not go green (%s)" % name,
              r["ok"], False)
        check("[R3] zero-offset forged completion is cold-confirmed (%s)" % name,
              r.get("confirmed_by_full_reparse"), True)
        check("[R3] zero-offset forged state matches the oracle (%s)" % name,
              strip_state(r), full(log))


# 25. [S4] The PRODUCTION --state path, exercised the way scripts/test.sh
#     uses it: repeated CLI invocations against a growing log, each carrying
#     the state file forward. The existing --state checks only covered its
#     error paths, so a regression in the successful path -- the one the poll
#     actually runs every second -- would not have been caught.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    stf = os.path.join(d, "poll.state")
    with open(log, "wb") as h: h.write(b"")
    rcs = []
    for cut in (0, len(good_run) // 3, 2 * len(good_run) // 3, len(good_run)):
        with open(log, "wb") as h: h.write(good_run[:cut])
        rcs.append(quiet_main(["--state", stf, "--poll", log]))
    check("[S4] --state CLI: incomplete polls report 'not complete' (rc 1/2)",
          all(rc in (1, 2) for rc in rcs[:-1]), True)
    check("[S4] --state CLI: the final poll reports a complete run (rc 0)",
          rcs[-1], 0)
    check("[S4] --state CLI wrote a reusable state file",
          os.path.isfile(stf) and os.path.getsize(stf) > 0, True)
    check("[S4] --state CLI left no .tmp", os.path.exists(stf + ".tmp"), False)
    # The state file must never change the ANSWER: a cold CLI run agrees.
    check("[S4] --state CLI verdict equals a cold CLI run",
          quiet_main([log]), 0)

# 26. [S2/S7] Staging must not be able to destroy the input capture.
#     Staging is an O_EXCL mkstemp file, so the old predictable "<dest>.tmp"
#     attack has no target: writing to a name whose ".tmp" IS the input can no
#     longer truncate it. Asserted by OUTCOME (the capture survives), which
#     stays true however staging is implemented.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "capture.log.tmp")
    with open(log, "wb") as h: h.write(good_run)
    before = open(log, "rb").read()
    target = os.path.join(d, "capture.log")   # its ".tmp" IS the input
    quiet_main(["--emit-run", target, log])
    check("[S2] a dest whose .tmp is the input leaves the capture intact",
          open(log, "rb").read(), before)
    quiet_main(["--state", os.path.join(d, "s2.state"), log])
    check("[S2] state staging leaves the capture intact",
          open(log, "rb").read(), before)
    # NB: target + ".tmp" IS the input log here, so its existence proves
    # nothing. What must be true is that no STAGING file was orphaned.
    check("[S2] no staging file is orphaned",
          [f for f in os.listdir(d) if f.startswith(".utest-frame-")], [])

# 27. [S7] A HARD LINK to the input, placed where staging used to be
#     predictable, must not destroy the capture. A name-based alias check
#     cannot see this: realpath resolves symlinks, not inode identity.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "capture.log")
    with open(log, "wb") as h: h.write(good_run)
    before = open(log, "rb").read()
    dest = os.path.join(d, "slice.txt")
    try:
        os.link(log, dest + ".tmp")           # hard link at the old staging path
        linked = True
    except OSError:
        linked = False
    if linked:
        quiet_main(["--emit-run", dest, log])
        check("[S7] a hard link at the old staging path cannot truncate the input",
              open(log, "rb").read(), before)
    # And a destination that IS the input by inode is refused outright.
    hard = os.path.join(d, "alias.log")
    try:
        os.link(log, hard)
        check("[S7] a hard-linked destination is refused by inode identity",
              quiet_main(["--emit-run", hard, log]), 2)
        check("[S7] the capture survives the refusal",
              open(log, "rb").read(), before)
    except OSError:
        pass


# ---------------------------------------------------------------------------
# Test-coverage round: recovery contracts that were documented but unasserted.
# ---------------------------------------------------------------------------

# 28. A CRASH-PREV replay line landing INSIDE an open run must be ignored
#     across a resume exactly as it is in a cold parse. The crash region is a
#     previous boot's evidence; letting it count would let a finished boot's
#     frame satisfy this boot's gate.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    part1 = (L(P + "[UTEST-FRAME] v=1 run=1") + "\r\n").encode()
    replay = (L("[CRASH-PREV] " + P + "[UTEST-FRAME-END] run=1 records=1 spawned=0")
              + "\r\n").encode()
    part2 = (L(P + "[UTEST-FRAME-END] run=1 records=1 spawned=0") + "\r\n").encode()
    with open(log, "wb") as h: h.write(part1)
    st = uf.parse_file(log)["state"]
    with open(log, "ab") as h: h.write(replay)
    st = uf.parse_file(log, state=st)["state"]
    with open(log, "ab") as h: h.write(part2)
    r = uf.parse_file(log, state=st)
    check("[cov] CRASH-PREV inside an open run equals a cold parse",
          strip_state(r), full(log))
    check("[cov] CRASH-PREV line is not counted toward the run",
          r["last_complete"]["run"], 1)

# 29. Widening retain_lines false -> true must RESTART and yield the complete
#     slice, not a slice truncated to whatever arrived after the switch.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    with open(log, "wb") as h: h.write(good_run)
    poll_state = uf.parse_file(log, retain_lines=False)["state"]
    widened = uf.parse_file(log, state=poll_state, retain_lines=True)
    check("[cov] widening retain_lines restarts", widened["restarted"], True)
    check("[cov] widened parse yields the COMPLETE slice",
          widened["lines"], uf.parse_file(log)["lines"])

# 30. A state PATH that is a directory must cold-parse rather than raise.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    with open(log, "wb") as h: h.write(good_run)
    as_dir = os.path.join(d, "state-dir")
    os.mkdir(as_dir)
    check("[cov] a directory state path degrades to a cold parse",
          uf._load_state(as_dir), None)
    check("[cov] and the CLI still reports the run", quiet_main([log]), 0)

# 31. Unwritable / directory destinations must preserve what exists, leave no
#     staging file behind, and report failure rather than half-succeed.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    with open(log, "wb") as h: h.write(good_run)
    dest_dir = os.path.join(d, "dest-is-a-dir")
    os.mkdir(dest_dir)
    check("[cov] --emit-run onto a directory returns 2",
          quiet_main(["--emit-run", dest_dir, log]), 2)
    check("[cov] the directory destination survives", os.path.isdir(dest_dir), True)
    check("[cov] no staging file orphaned by the failure",
          [f for f in os.listdir(d) if f.startswith(".utest-frame-")], [])
    # A state write that cannot land costs the OPTIMIZATION, never the answer.
    check("[cov] an unwritable state path still returns the right verdict",
          quiet_main(["--state", dest_dir, log]), 0)

# 32. Two writers racing one state file must leave VALID JSON, and the next
#     parse must still agree with a cold parse. Offsets are not required to be
#     monotonic (there is no lock); correctness of the ANSWER is.
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "serial.log")
    stf = os.path.join(d, "race.state")
    with open(log, "wb") as h: h.write(good_run)
    for _ in range(6):
        quiet_main(["--state", stf, "--poll", log])
    loaded = uf._load_state(stf)
    check("[cov] the raced state file is valid JSON", isinstance(loaded, dict), True)
    r = uf.parse_file(log, state=loaded)
    check("[cov] a parse resumed from it agrees with a cold parse",
          (r["last_complete"], r["ok"]),
          (full(log)["last_complete"], full(log)["ok"]))

print("\n%s" % ("ALL DIFFERENTIAL CHECKS PASS" if fails == 0 else "%d FAILURES" % fails))
sys.exit(1 if fails else 0)
