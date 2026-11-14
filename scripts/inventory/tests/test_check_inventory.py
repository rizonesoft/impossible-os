#!/usr/bin/env python3
"""Fixtures for scripts/inventory/check-inventory.py: one failing tree per rule
and passing trees, each run through the real script as a subprocess.

    python3 scripts/inventory/tests/test_check_inventory.py
"""
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = REPO / "scripts" / "inventory" / "check-inventory.py"
SCHEMA = REPO / "docs" / "design" / "inventory" / "schema.json"
B = "26300.9818"


def rec(rid, **kw):
    r = {"id": rid, "surface": "taskbar", "kind": "control", "label": rid.rsplit(".", 1)[1],
         "order": 0, "source": f"uia:{B}", "capturedFrom": B}
    r.update(kw)
    return r


def three():
    return [rec("win11.taskbar.bar", kind="surface", itemCount=2),
            rec("win11.taskbar.bar.start", parent="win11.taskbar.bar", order=0),
            rec("win11.taskbar.bar.search", parent="win11.taskbar.bar", order=1)]


class Tree:
    def __init__(self, files):
        self.dir = tempfile.mkdtemp()
        root = pathlib.Path(self.dir)
        shutil.copy(SCHEMA, root / "schema.json")
        for name, lines in files.items():
            p = root / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text("".join((l if isinstance(l, str) else json.dumps(l)) + "\n" for l in lines))

    def run(self, *args):
        r = subprocess.run([sys.executable, str(SCRIPT), "--root", self.dir, *args],
                           capture_output=True, text=True, timeout=60)
        shutil.rmtree(self.dir)
        return r


def run(files, *args):
    return Tree(files).run(*args)


class Passing(unittest.TestCase):
    def test_empty_tree(self):
        r = run({})
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("inventory: 0 records, 0 errors", r.stdout)

    def test_three_records(self):
        r = run({"taskbar/bar.json": three()})
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("inventory: 3 records, 0 errors", r.stdout)

    def test_typed_measurements(self):
        lines = three() + [
            rec("win11.taskbar.bar.height", kind="metric", parent="win11.taskbar.bar", order=2,
                measure={"value": 48, "unit": "px", "readBy": "taskbar height"}),
            rec("win11.taskbar.bar.open", kind="motion", parent="win11.taskbar.bar", order=3,
                motion={"durationMs": 167, "property": "position", "distancePx": 40, "curve": [0, 0, 0, 1]}),
            rec("win11.taskbar.bar.body", kind="state", parent="win11.taskbar.bar", order=4,
                textMetrics={"sizePx": 14, "ascent": 15, "descent": 4, "lineGap": 0, "advances": {"a": 7.1}},
                colours={"fill": {"light": "#f3f3f3", "dark": "#202020"}}),
        ]
        lines[0]["itemCount"] = 5
        r = run({"taskbar/bar.json": lines})
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_profile_observation_of_one_element_at_two_profiles(self):
        r = run({"taskbar/bar.json": three(),
                 "taskbar/bar.fhd-150.json": [{"id": "win11.taskbar.bar.start", "captureProfile": "fhd-150",
                                               "bounds": {"x": 0, "y": 0, "width": 60, "height": 60}, "capturedFrom": B}],
                 "taskbar/bar.fhd-125.json": [{"id": "win11.taskbar.bar.start", "captureProfile": "fhd-125",
                                               "bounds": {"x": 0, "y": 0, "width": 50, "height": 50}, "capturedFrom": B}]})
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("inventory: 3 records, 0 errors", r.stdout)

    def test_baseline_file_is_left_to_section_5(self):
        r = run({"settings/baseline.json": ['{"not": "a record"}']})
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_json_output(self):
        r = run({"taskbar/bar.json": three()}, "--json")
        self.assertEqual(json.loads(r.stdout), {"records": 3, "observations": 0, "errors": []})


class Failing(unittest.TestCase):
    def fails(self, files, rule, *args):
        r = run(files, *args)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn(f": {rule}:", r.stdout)
        return r

    def test_duplicate_id(self):
        t = three(); t.append(rec("win11.taskbar.bar.start", parent="win11.taskbar.bar", order=5))
        self.fails({"taskbar/bar.json": t}, "duplicate-id")

    def test_orphan_parent(self):
        self.fails({"taskbar/bar.json": [rec("win11.taskbar.x", parent="win11.taskbar.none")]}, "orphan-parent")

    def test_parent_cycle(self):
        self.fails({"taskbar/bar.json": [rec("win11.taskbar.a", parent="win11.taskbar.b"),
                                         rec("win11.taskbar.b", parent="win11.taskbar.a")]}, "parent-cycle")

    def test_duplicate_order(self):
        t = three(); t[2]["order"] = 0
        self.fails({"taskbar/bar.json": t}, "duplicate-order")

    def test_item_count(self):
        t = three(); t[0]["itemCount"] = 3
        self.fails({"taskbar/bar.json": t}, "item-count")

    def test_unknown_source(self):
        t = three(); t[1]["source"] = "guess:1.0"
        self.fails({"taskbar/bar.json": t}, "schema")

    def test_bounds_without_profile(self):
        t = three(); t[1]["bounds"] = {"x": 0, "y": 0, "width": 1, "height": 1}
        self.fails({"taskbar/bar.json": t}, "schema")

    def test_menu_item_without_parent(self):
        self.fails({"taskbar/bar.json": [rec("win11.taskbar.m", kind="menuItem")]}, "schema")

    def test_metric_without_measure(self):
        self.fails({"taskbar/bar.json": [rec("win11.taskbar.h", kind="metric")]}, "schema")

    def test_motion_without_curve(self):
        self.fails({"taskbar/bar.json": [rec("win11.taskbar.o", kind="motion",
                                             motion={"durationMs": 100, "property": "opacity"})]}, "schema")

    def test_unknown_field(self):
        t = three(); t[1]["colour"] = "#ffffff"
        self.fails({"taskbar/bar.json": t}, "schema")

    def test_blank_line(self):
        self.fails({"taskbar/bar.json": [three()[0], ""]}, "blank-line")

    def test_observation_of_missing_record(self):
        self.fails({"taskbar/bar.json": three(),
                    "taskbar/bar.fhd-150.json": [{"id": "win11.taskbar.bar.gone", "captureProfile": "fhd-150",
                                                  "bounds": {"x": 0, "y": 0, "width": 1, "height": 1}, "capturedFrom": B}]},
                   "observation-orphan")

    def test_observation_profile_differs_from_file(self):
        self.fails({"taskbar/bar.json": three(),
                    "taskbar/bar.fhd-150.json": [{"id": "win11.taskbar.bar.start", "captureProfile": "fhd-125",
                                                  "bounds": {"x": 0, "y": 0, "width": 1, "height": 1}, "capturedFrom": B}]},
                   "observation-profile")

    def test_parent_resolves_across_surfaces_but_only_scope_reports(self):
        files = {"taskbar/bar.json": three(),
                 "start/menu.json": [rec("win11.start.pin", surface="start", parent="win11.taskbar.bar.start")],
                 "tray/x.json": [rec("win11.tray.x", surface="tray", parent="win11.tray.none")]}
        r = run(files, "--surface", "start")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("inventory: 1 records, 0 errors", r.stdout)

    def test_first_50_errors_are_printed(self):
        lines = [rec(f"win11.taskbar.x{i}", parent="win11.taskbar.none", order=i) for i in range(60)]
        r = self.fails({"taskbar/bar.json": lines}, "orphan-parent")
        self.assertEqual(sum(1 for l in r.stdout.splitlines() if ": orphan-parent:" in l), 50)
        self.assertIn("... 10 more error(s)", r.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=1)
