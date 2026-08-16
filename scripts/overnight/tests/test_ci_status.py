#!/usr/bin/env python3
"""Regression tests for ci_status supersession (2026-07-28).

The bug: both preflight.py and ci-check.py asked only "is a failed run's head
SHA an ancestor of local HEAD". Once ANY pushed commit went red that stayed true
forever, so the gate kept demanding a diagnosis of an already-fixed failure at
every section boundary for the rest of the run. Observed live: 051df5e0 broke
the tooling pack, eb2af727 fixed it, build.yml went green at HEAD 103a2db8, and
preflight still reported ours_red.

These tests pin the supersession half of the rule with fabricated run records
and a stubbed ancestry oracle -- no network, no gh, no repo state.
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import ci_status  # noqa: E402


def _anc_from_chain(chain):
    """Ancestry oracle for a linear history, oldest first."""
    order = {sha: i for i, sha in enumerate(chain)}

    def is_ancestor(a, b):
        if a not in order or b not in order:
            return False
        return order[a] <= order[b]
    return is_ancestor


# Oldest -> newest, matching the live history that exposed the bug.
CHAIN = ["797b7335", "051df5e0", "eb2af727", "aaf4a4c3", "103a2db8"]
ANC = _anc_from_chain(CHAIN)


def _rec(sha, conclusion, created="2026-07-27T23:00:00Z"):
    return {"headSha": sha, "conclusion": conclusion, "status": "completed",
            "createdAt": created}


class CancelledSupersessionTests(unittest.TestCase):
    """`cancel-in-progress` makes SUCCESS rare on a fast-moving branch.

    MEASURED 2026-08-06 across the last 100 build.yml runs: 75 cancelled, 5
    failure, 20 success -- and the newest success was roughly two days stale. An
    old failure therefore stayed `ours_red` forever, and the doctrine ("fix it
    before shipping the next section") re-charged that instruction at EVERY
    section boundary against a failure that had already been repaired.

    A cancellation is WEAK evidence: it proves the workflow started on that
    commit, not that it passed. So it is admitted only from a strict DESCENDANT
    -- work that came after the fix -- and never for the failing commit itself.
    """

    def test_a_cancelled_descendant_supersedes_when_no_green_exists(self):
        failure = _rec("051df5e0", "failure")
        cancelled = [_rec("103a2db8", "cancelled")]
        self.assertEqual(
            ci_status._superseded_by(failure, cancelled, ANC), "103a2db8")

    def test_a_cancelled_ANCESTOR_does_not_supersede(self):
        """Older work says nothing about a later failure."""
        failure = _rec("103a2db8", "failure")
        cancelled = [_rec("051df5e0", "cancelled")]
        self.assertEqual(ci_status._superseded_by(failure, cancelled, ANC), "")

    def test_a_green_is_still_preferred_over_a_cancellation(self):
        """The weak path must never displace real evidence."""
        failure = _rec("051df5e0", "failure")
        greens = [_rec("aaf4a4c3", "success")]
        self.assertEqual(
            ci_status._superseded_by(failure, greens, ANC), "aaf4a4c3")


class SupersessionTests(unittest.TestCase):
    def test_later_green_descendant_supersedes(self):
        """A green run on a DESCENDANT commit proves the failure was fixed."""
        failure = _rec("051df5e0", "failure")
        greens = [_rec("103a2db8", "success")]
        self.assertEqual(
            ci_status._superseded_by(failure, greens, ANC), "103a2db8")

    def test_earlier_green_does_not_supersede(self):
        """A green run BEFORE the break says nothing about the break."""
        failure = _rec("051df5e0", "failure")
        greens = [_rec("797b7335", "success")]
        self.assertEqual(ci_status._superseded_by(failure, greens, ANC), "")

    def test_same_sha_rerun_supersedes_only_when_later(self):
        """Re-running the same commit green clears it; an older green does not."""
        failure = _rec("051df5e0", "failure", "2026-07-27T23:29:47Z")
        later = [_rec("051df5e0", "success", "2026-07-27T23:45:00Z")]
        earlier = [_rec("051df5e0", "success", "2026-07-27T23:10:00Z")]
        self.assertEqual(
            ci_status._superseded_by(failure, later, ANC), "051df5e0")
        self.assertEqual(ci_status._superseded_by(failure, earlier, ANC), "")

    def test_no_greens_leaves_failure_standing(self):
        failure = _rec("051df5e0", "failure")
        self.assertEqual(ci_status._superseded_by(failure, [], ANC), "")

    def test_green_missing_sha_is_ignored(self):
        """A malformed record must not silently clear a real failure."""
        failure = _rec("051df5e0", "failure")
        self.assertEqual(
            ci_status._superseded_by(failure, [_rec("", "success")], ANC), "")


class CompletedDroughtTests(unittest.TestCase):
    """v14 close-out (2026-08-16): `cancelled` is neither red nor green, so a
    CI job killed by its own `timeout-minutes` on every push was invisible --
    18 hours and 6 pushes with no completed build leg while `ours_red` stayed
    False. The drought detector counts the leading streak of cancelled runs
    (newest first) before any verdict. NOTE-level by design: it must never set
    `ours_red` or halt an unattended run."""

    def test_observed_incident_shape_fires(self):
        # 6 cancelled pushes, then the stale greens further down the window --
        # the exact shape that read clean for 18 hours.
        recs = ([_rec(f"c{i}", "cancelled") for i in range(6)]
                + [_rec("g1", "success"), _rec("g2", "success")])
        self.assertEqual(ci_status.completed_drought(recs), 6)
        self.assertGreaterEqual(6, ci_status.DROUGHT_LIMIT)

    def test_refusal_a_green_head_stays_clean(self):
        # The 05:35Z green-run control: a verdict at the head means no drought,
        # however many cancellations sit behind it.
        recs = ([_rec("g1", "success")]
                + [_rec(f"c{i}", "cancelled") for i in range(20)])
        self.assertEqual(ci_status.completed_drought(recs), 0)

    def test_refusal_a_failure_head_is_the_red_paths_job(self):
        recs = [_rec("f1", "failure"), _rec("c1", "cancelled")]
        self.assertEqual(ci_status.completed_drought(recs), 0)

    def test_under_the_limit_does_not_fire(self):
        recs = ([_rec(f"c{i}", "cancelled") for i in range(4)]
                + [_rec("g1", "success")])
        self.assertLess(ci_status.completed_drought(recs),
                        ci_status.DROUGHT_LIMIT)

    def test_in_flight_runs_are_not_evidence_either_way(self):
        # A queued/in-progress run at the head resolves on its own; it neither
        # counts toward the streak nor ends it.
        recs = ([{"headSha": "p1", "conclusion": "",
                  "status": "in_progress", "createdAt": "2026-08-13T00:00:00Z"}]
                + [_rec(f"c{i}", "cancelled") for i in range(5)])
        self.assertEqual(ci_status.completed_drought(recs), 5)


class WindowTests(unittest.TestCase):
    def test_window_is_wide_enough_to_see_a_fix_land(self):
        """--limit 3 could not see the repair: the live fix was 3 pushes later.

        With a 3-run window the green run that proves the fix has already
        scrolled off, so the false alarm would come back. Pin the wider window
        so a future 'trim the query' change has to argue with this test.
        """
        self.assertGreaterEqual(ci_status.LIMIT, 10)


if __name__ == "__main__":
    unittest.main(verbosity=0)
