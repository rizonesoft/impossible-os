#!/usr/bin/env python3
"""The weekly demo: what the gate accepts, and the order release.yml publishes in.

The March 2026 demo was never booted before it was published and hung on every
machine with two or more CPUs (GitHub issue 6), so the workflow half of this
file requires the boots to come before the publish step, on the packaged image.
"""
import sys
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts/release"))
import demo_gate  # noqa: E402
import demo_prune  # noqa: E402


def run(name, conclusion="success", status="completed"):
    return {"name": name, "status": status, "conclusion": conclusion}


GREEN = [run("Build"), run("GitHub Pages"), run("Visual Regression")]


class Judge(unittest.TestCase):
    def test_clean(self):
        self.assertEqual(demo_gate.judge([*GREEN, run("TODO graph", "skipped")]), "clean")

    def test_any_unclean_run_refuses(self):
        for c in ("failure", "cancelled", "timed_out", "startup_failure", "action_required", "stale", None):
            with self.assertRaises(demo_gate.Refused):
                demo_gate.judge([run("Build"), run("Visual Regression", c)])

    def test_unfinished_waits(self):
        self.assertEqual(demo_gate.judge([run("Build", None, "in_progress")]), "wait")
        self.assertEqual(demo_gate.judge([run("Build"), run("GitHub Pages", None, "queued")]), "wait")

    def test_a_failure_refuses_without_waiting(self):
        with self.assertRaises(demo_gate.Refused):
            demo_gate.judge([run("Build", None, "in_progress"), run("TODO graph", "failure")])

    def test_a_missing_required_workflow_is_never_clean(self):
        self.assertEqual(demo_gate.judge([]), "wait")
        for i, r in enumerate(GREEN):
            rest = GREEN[:i] + GREEN[i + 1:]
            self.assertEqual(demo_gate.judge(rest), "wait")
            self.assertEqual(demo_gate.judge(rest + [run(r["name"], "skipped")]), "wait")

    def test_required_is_every_workflow_each_push_to_main_starts(self):
        unfiltered = set()
        for path in (REPO / ".github/workflows").glob("*.yml"):
            wf = yaml.safe_load(path.read_text())
            push = (wf[True] or {}).get("push") if isinstance(wf[True], dict) else None
            if push and "main" in (push.get("branches") or []) and "paths" not in push:
                unfiltered.add(wf["name"])
        self.assertEqual(unfiltered, demo_gate.REQUIRED)


class Published(unittest.TestCase):
    def test_same_commit_is_not_published_twice(self):
        self.assertTrue(demo_gate.already_published({"demo-26.11.22": "a" * 40}, "demo-26.11.29", "a" * 40))

    def test_new_commit(self):
        self.assertFalse(demo_gate.already_published({"demo-26.11.22": "a" * 40}, "demo-26.11.29", "b" * 40))

    def test_taken_tag_is_refused(self):
        with self.assertRaises(demo_gate.Refused):
            demo_gate.already_published({"demo-26.11.29": "a" * 40}, "demo-26.11.29", "b" * 40)


def release(tag, prerelease=True, draft=False):
    return {"tag_name": tag, "prerelease": prerelease, "draft": draft}


class Prune(unittest.TestCase):
    def test_keeps_the_newest_five_by_date_not_by_text(self):
        demos = [release(f"demo-{d}") for d in ("26.9.27", "26.10.4", "26.11.29", "26.12.6", "27.1.3", "26.9.6", "26.12.27")]
        self.assertEqual(demo_prune.to_delete(demos), ["demo-26.9.27", "demo-26.9.6"])

    def test_five_or_fewer_delete_nothing(self):
        self.assertEqual(demo_prune.to_delete([release(f"demo-26.11.{d}") for d in range(1, 6)]), [])

    def test_nothing_but_a_published_demo_is_ever_deleted(self):
        keep = [release(f"demo-27.1.{d}") for d in range(1, 6)]
        others = [release("v26.3.18-alpha.821"), release("v26.3.18", prerelease=False), release("demo-26.1.1", draft=True),
                  release("demo-26.1.2", prerelease=False), release("demo-26.1.3-fixed"), release("my-demo-26.1.4"),
                  release("demo-26.2.31"), release("demo-26.01.01"), release("demo-2\uff16.1.1")]
        self.assertEqual(demo_prune.to_delete(keep + others), [])


class Workflow(unittest.TestCase):
    def setUp(self):
        self.wf = yaml.safe_load((REPO / ".github/workflows/release.yml").read_text())
        self.on = self.wf[True]   # YAML 1.1 reads `on` as a boolean
        self.steps = self.wf["jobs"]["release"]["steps"]

    def at(self, name):
        return next(i for i, s in enumerate(self.steps) if s["name"] == name)

    def test_sunday_midnight_in_south_africa(self):
        # 00:00 Sunday SAST (UTC+2, no daylight saving) is 22:00 Saturday UTC.
        self.assertEqual(self.on["schedule"], [{"cron": "0 22 * * 6"}])

    def test_release_waits_for_the_gate(self):
        self.assertEqual(self.wf["jobs"]["release"]["needs"], "gate")
        self.assertIn("needs.gate.outputs.proceed == 'true'", self.wf["jobs"]["release"]["if"])

    def test_packaged_image_boots_before_it_is_published(self):
        boot = self.steps[self.at("Boot the packaged image")]
        self.assertLess(self.at("Package release archive"), self.at("Boot the packaged image"))
        self.assertLess(self.at("Boot the packaged image"), self.at("Create GitHub Release"))
        self.assertIn("build/release/system-disk.img", boot["run"])
        self.assertIn("for cpus in 1 2 4", boot["run"])
        self.assertIn("SMOKE_DESKTOP=1", boot["run"])
        self.assertNotIn("continue-on-error", boot)

    def test_old_demos_go_only_after_the_new_one_is_out(self):
        prune = self.steps[self.at("Delete older demos")]
        self.assertLess(self.at("Create GitHub Release"), self.at("Delete older demos"))
        self.assertIn("inputs.publish", prune["if"])
        self.assertIn("startsWith(steps.version.outputs.tag, 'demo-')", prune["if"])

    def test_a_stable_release_leaves_the_demos(self):
        script = self.steps[self.at("Clean up old pre-releases")]["with"]["script"]
        self.assertIn("r.prerelease && !r.draft && !r.tag_name.startsWith('demo-')", script)

    def test_no_gh_flag_newer_than_the_ci_image(self):
        for f in ("demo_gate.py", "demo_prune.py"):
            self.assertNotIn('"--slurp"', (REPO / "scripts/release" / f).read_text())

    def test_publishing_never_overwrites_a_release(self):
        publish = self.steps[self.at("Create GitHub Release")]
        self.assertNotIn("uses", publish)
        self.assertIn("gh release create", publish["run"])
        self.assertNotIn("--clobber", publish["run"])

    def test_a_dry_run_never_shares_a_group_with_a_release(self):
        group = self.wf["concurrency"]["group"]
        self.assertIn("github.event_name == 'push' && github.ref", group)
        self.assertIn("!inputs.publish && github.run_id", group)

    def test_a_manual_run_publishes_only_when_asked(self):
        self.assertFalse(self.on["workflow_dispatch"]["inputs"]["publish"]["default"])
        self.assertIn("inputs.publish", self.steps[self.at("Create GitHub Release")]["if"])


if __name__ == "__main__":
    unittest.main(verbosity=1)
