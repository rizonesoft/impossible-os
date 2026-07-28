#!/usr/bin/env python3
"""T4-1 cost summary: reconciliation, scoping honesty, and fail-open.

The report's whole value is being trustworthy enough to redirect decisions --
the backlog optimized the ~10% output slice for want of a number showing
cache-read is ~81%. So the tests that matter are: the figures RECONCILE with
the raw metrics, and a figure that cannot be scoped to this run is OMITTED
rather than silently reported as an all-time count.
"""
import json, pathlib, subprocess, sys, tempfile, time

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
SCRIPT = REPO / "scripts/overnight/cost-summary.py"
FAILS = []


def check(name, cond):
    if not cond:
        FAILS.append(name)


def _metrics(d, name="run-20260101-120000.jsonl"):
    p = pathlib.Path(d) / name
    rows = [
        {"turns": 100, "cache_read_input_tokens": 30_000_000,
         "cache_creation_input_tokens": 200_000, "output_tokens": 50_000,
         "input_tokens": 100, "sidechain_cache_read_input_tokens": 5_000_000,
         "sidechain_cache_creation_input_tokens": 100_000,
         "sidechain_output_tokens": 900, "agent_dispatches": 3},
        {"turns": 50, "cache_read_input_tokens": 20_000_000,
         "cache_creation_input_tokens": 100_000, "output_tokens": 25_000,
         "input_tokens": 50, "sidechain_cache_read_input_tokens": 0,
         "sidechain_cache_creation_input_tokens": 0,
         "sidechain_output_tokens": 0, "agent_dispatches": 1},
    ]
    p.write_text("\n".join(json.dumps(r) for r in rows) + "\n")
    return p


def _run(p, project):
    return subprocess.run([sys.executable, str(SCRIPT), str(p), "--project", str(project)],
                          capture_output=True, text=True, timeout=120).stdout


def test_totals_reconcile_with_the_raw_metrics():
    with tempfile.TemporaryDirectory() as d:
        p = _metrics(d)
        out = _run(p, d)
        # 30M + 20M cache-read; 150 turns; avg 333,333; peak 400,000
        check("cache-read total", "50,000,000" in out)
        check("turns", "turns: 150" in out)
        check("avg context/turn", "333,333" in out)
        check("peak context/turn", "400,000" in out)
        check("agent dispatches summed", "agent dispatches: 4" in out)
        # 50,000,000 * 1.50 / 1e6 = $75.00
        check("cache-read cost", "$75.00" in out)


def test_cache_read_is_reported_as_the_dominant_bucket():
    """The finding the report exists to surface must be visible at a glance."""
    with tempfile.TemporaryDirectory() as d:
        out = _run(_metrics(d), d)
        first = [l for l in out.splitlines() if "%" in l and "$" in l][0]
        check("cache-read ranked first", "cache-read" in first)


def test_unscopable_run_omits_rather_than_reporting_all_time():
    """A per-run table that quietly includes an all-time count is worse than no
    table -- it reads as authoritative. Without a run stamp in the filename the
    cross-run logs cannot be scoped, so those rows must be withheld."""
    with tempfile.TemporaryDirectory() as d:
        p = _metrics(d, name="metrics-no-stamp.jsonl")
        out = _run(p, d)
        check("omission is stated", "could not be scoped" in out)
        check("no re-read row", "re-reads:" not in out)
        check("no hook-fire row", "hook fires:" not in out)
        check("token rows still present", "50,000,000" in out)


def test_avg_context_is_labelled_as_length_confounded():
    """The 2026-07-28 misdiagnosis: a rising `avg context/turn` across segments
    was read as "rollover is not resetting context" and filed as the top cost
    item. It was length confound -- context grows monotonically and is never
    trimmed, so avg ~= (start+end)/2 scales with how long the section ran.
    Length-matched, the three segments agreed to 4.5% and every one of them
    started at ~57.5K. The report must carry that caveat and the
    length-invariant figures beside the average, or it invites the same wrong
    call again."""
    with tempfile.TemporaryDirectory() as d:
        out = _run(_metrics(d), d)
        check("turn count shown beside avg", "over 150 turns" in out)
        check("terminal context labelled", "end-of-segment 400,000" in out)
        check("length caveat stated", "SCALES WITH SEGMENT LENGTH" in out)
        check("quadratic relation stated", "QUADRATIC in segment length" in out)
        check("accumulation rate shown", "tok/turn" in out)
        check("split estimate offered", "split across 2 sections" in out)


def test_split_estimate_is_omitted_when_it_cannot_be_derived():
    """One checkpoint record means end == avg, so the linear model has no slope
    to fit. Emitting a 'split this and save X%' line from no evidence would be
    the same invented-authority failure the scoping tests guard against."""
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d) / "run-20260101-120000.jsonl"
        p.write_text(json.dumps({"turns": 100, "cache_read_input_tokens": 10_000_000}) + "\n")
        out = _run(p, d)
        check("avg still reported", "avg 100,000" in out)
        check("no split estimate", "split across" not in out)
        check("no invented rate", "accumulation ~" not in out)


def test_fail_open_on_missing_and_malformed():
    with tempfile.TemporaryDirectory() as d:
        check("missing file", "nothing to report" in _run(pathlib.Path(d) / "nope.jsonl", d))
        bad = pathlib.Path(d) / "run-20260101-120000.jsonl"
        bad.write_text("{not json\n")
        check("malformed file", "nothing to report" in _run(bad, d))


if __name__ == "__main__":
    test_totals_reconcile_with_the_raw_metrics()
    test_cache_read_is_reported_as_the_dominant_bucket()
    test_unscopable_run_omits_rather_than_reporting_all_time()
    test_avg_context_is_labelled_as_length_confounded()
    test_split_estimate_is_omitted_when_it_cannot_be_derived()
    test_fail_open_on_missing_and_malformed()
    if FAILS:
        for f in FAILS:
            print("FAIL:", f)
        raise SystemExit(1)
    print("test_cost_summary OK (reconciliation + scoping honesty + fail-open)")
