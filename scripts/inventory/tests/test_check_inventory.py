#!/usr/bin/env python3
"""Fixtures for scripts/inventory/check-inventory.py: one failing tree per rule,
cross-surface conflicts checked from each side, and passing trees, each run
through the real script as a subprocess.

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

    def test_downstream_fields(self):
        t = three() + [rec("win11.taskbar.bar.copy", kind="shortcut", parent="win11.taskbar.bar", order=2,
                           shortcut="Ctrl+C", shortcutScope="control", overridable=True, session="enhanced"),
                       rec("win11.taskbar.bar.hover", kind="state", parent="win11.taskbar.bar", order=3, state="selected-hover"),
                       rec("win11.taskbar.bar.pad", kind="metric", parent="win11.taskbar.bar", order=4,
                           measure={"value": 12, "unit": "px", "scaling": "snapped"})]
        t[0]["itemCount"] = 5
        obs = [{"id": "win11.taskbar.bar.start", "captureProfile": "fhd-150", "capturedFrom": B, "session": "enhanced",
                "bounds": {"x": 0, "y": 0, "width": 60, "height": 60},
                "textMetrics": {"sizePx": 21, "ascent": 22, "descent": 6, "lineGap": 0, "advances": {"a": 10.6}}}]
        r = run({"taskbar/bar.json": t, "taskbar/bar.fhd-150.json": obs})
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_scoped_check_skips_other_surfaces_observations(self):
        r = run({"taskbar/bar.json": three(), "start/menu.json": [rec("win11.start.x", surface="start")],
                 "start/menu.fhd-150.json": ["{not json"]}, "--surface", "taskbar")
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_scalar_and_font_observations_need_no_bounds(self):
        obs = [{"id": "win11.taskbar.bar.start", "captureProfile": "fhd-150", "capturedFrom": B,
                "measure": {"value": 18, "unit": "px", "scaling": "snapped"}},
               {"id": "win11.taskbar.bar.search", "captureProfile": "fhd-150", "capturedFrom": B,
                "textMetrics": {"sizePx": 21, "ascent": 22, "descent": 6, "lineGap": 0, "advances": {"a": 10.6}}}]
        r = run({"taskbar/bar.json": three(), "taskbar/bar.fhd-150.json": obs})
        self.assertEqual(r.returncode, 0, r.stdout)


    def test_json_output(self):
        r = run({"taskbar/bar.json": three()}, "--json")
        self.assertEqual(json.loads(r.stdout), {"records": 3, "observations": 0, "errors": []})


REF = {"reference.json": [{"edition": "Professional", "displayVersion": "26H2", "build": B}]}
SHA = "6de91feccadfbb0a796b71caedead5a2284f730e"


def bl(name, rows, **header):
    h = {"baseline": name, "owner": {"start-apps": "start", "controls": "system"}.get(name, name),
         "source": "test", "build": B}
    h.update(header)
    return [{k: v for k, v in h.items() if v is not None}] + [{"id": r} for r in rows]


def absent(*rows):
    body = "\n".join(f"| `{b}` | `{r}` | {why} |" for b, r, why in rows)
    return ["# Reference", "", "## Which baseline rows are absent?", "",
            "| Baseline | Row | Reason |", "| --- | --- | --- |", body, "", "## Next", ""]


def taskbar_with(**kw):
    r = three()
    r[1].update(kw)
    return r


class Baselines(unittest.TestCase):
    def report(self, files, *args):
        r = run({**REF, **files}, "--baselines", "--json", *args)
        return r, json.loads(r.stdout)

    def rules(self, files, *args):
        return [e["rule"] for e in self.report(files, *args)[1]["errors"]]

    def test_rows_are_counted_pending_until_the_owner_is_captured(self):
        r, out = self.report({"settings/baseline.json": bl("settings", ["a", "b"])})
        self.assertEqual(out["baselines"]["settings"],
                         {"owner": "settings", "rows": 2, "matched": 0, "absent": 0, "pending": 2, "unmatched": 0})

    def test_a_captured_owner_turns_its_open_rows_unmatched(self):
        rows = [rec("win11.settings.x", surface="settings", kind="surface", baselineId="settings:a")]
        _, out = self.report({"settings/baseline.json": bl("settings", ["a", "b", "c"]),
                              "settings/home.json": rows, "REFERENCE.md": absent(("settings", "c", "not in this edition"))})
        self.assertEqual(out["baselines"]["settings"],
                         {"owner": "settings", "rows": 3, "matched": 1, "absent": 1, "pending": 0, "unmatched": 1})

    def test_the_text_report_prints_one_line_per_baseline(self):
        r = run({**REF, "settings/baseline.json": bl("settings", ["a"]),
                 "controls/baseline.json": bl("controls", ["Button"], build=None,
                                              upstream={"repo": "o/r", "commit": SHA, "license": "MIT"})}, "--baselines")
        self.assertIn("baseline settings: 1 rows, 0 matched, 0 absent, 1 pending, 0 unmatched (owner settings)", r.stdout)
        self.assertIn("baseline controls: 1 rows", r.stdout)

    def test_every_known_baseline_must_exist_under_baselines(self):
        r, out = self.report({"settings/baseline.json": bl("settings", ["a"])})
        self.assertEqual(r.returncode, 1)
        self.assertEqual(sorted(e["file"] for e in out["errors"] if e["rule"] == "baseline-missing"),
                         ["classic-controls/baseline.json", "control-panel/baseline.json", "controls/baseline.json",
                          "known-folders/baseline.json", "shell-commands/baseline.json", "start-apps/baseline.json"])

    def test_a_dangling_baseline_id_is_refused(self):
        rows = [rec("win11.settings.x", surface="settings", kind="surface", baselineId="settings:zz")]
        self.assertIn("baseline-orphan", self.rules({"settings/baseline.json": bl("settings", ["a"]), "settings/home.json": rows}))
        rows[0]["baselineId"] = "nosuch:a"
        self.assertIn("baseline-orphan", self.rules({"settings/home.json": rows}))

    def test_an_old_capture_never_matches_a_newer_baseline(self):
        # The row id survives a build change; the capture of the old build must not count.
        old = rec("win11.settings.x", surface="settings", kind="surface", baselineId="settings:a",
                  source="uia:26300.9000", capturedFrom="26300.9000")
        r, out = self.report({"settings/baseline.json": bl("settings", ["a"]), "settings/home.json": [old]})
        self.assertIn("baseline-build", [e["rule"] for e in out["errors"]])
        self.assertEqual(out["baselines"]["settings"]["matched"], 0)

    def test_a_baseline_from_another_build_is_refused(self):
        self.assertIn("baseline-header", self.rules({"settings/baseline.json": bl("settings", ["a"], build="26300.1")}))

    def test_a_build_baseline_needs_reference_json(self):
        r = run({"settings/baseline.json": bl("settings", ["a"])}, "--json")
        self.assertIn("baseline-header", [e["rule"] for e in json.loads(r.stdout)["errors"]])

    def test_header_rules(self):
        for header, why in (({"baseline": "other"}, "name"), ({"owner": "explorer"}, "owner"),
                            ({"source": ""}, "source"), ({"upstream": {"repo": "o/r", "commit": SHA, "license": "MIT"}}, "both"),
                            ({"build": None}, "neither"), ({"extra": 1}, "extra"),
                            ({"files": [{"path": "x"}]}, "files")):
            with self.subTest(why):
                self.assertIn("baseline-header", self.rules({"settings/baseline.json": bl("settings", ["a"], **header)}))
        bad = {"build": None, "upstream": {"repo": "o/r", "commit": "main", "license": "MIT"}}
        self.assertIn("baseline-header", self.rules({"controls/baseline.json": bl("controls", ["a"], **bad)}))
        self.assertIn("baseline-header", self.rules({"nosuch/baseline.json": bl("nosuch", ["a"], owner="x")}))

    def test_row_rules(self):
        lines = bl("settings", ["a", "a"]) + ['{"name": "no id"}', ""]
        rules = self.rules({"settings/baseline.json": lines})
        for rule in ("baseline-duplicate", "baseline-row", "blank-line"):
            self.assertIn(rule, rules)
        self.assertIn("baseline-empty", self.rules({"settings/baseline.json": bl("settings", [])}))
        self.assertIn("link", self.rules({"settings/baseline.json": bl("settings", []) + [{"id": "a", "deepLink": "https://x"}]}))

    def test_only_baseline_json_is_a_baseline_name(self):
        self.assertIn("baseline-name", self.rules({"settings/baseline-old.json": bl("settings", ["a"])}))

    def test_absent_table_rules(self):
        base = {"settings/baseline.json": bl("settings", ["a", "b"])}
        self.assertIn("absent-orphan", self.rules({**base, "REFERENCE.md": absent(("settings", "zz", "why"))}))
        self.assertIn("absent-orphan", self.rules({**base, "REFERENCE.md": absent(("nosuch", "a", "why"))}))
        self.assertIn("absent-reason", self.rules({**base, "REFERENCE.md": absent(("settings", "a", " "))}))
        self.assertIn("absent-duplicate", self.rules({**base, "REFERENCE.md": absent(("settings", "a", "x"), ("settings", "a", "y"))}))
        rows = [rec("win11.settings.x", surface="settings", kind="surface", baselineId="settings:a")]
        self.assertIn("absent-matched", self.rules({**base, "settings/home.json": rows,
                                                    "REFERENCE.md": absent(("settings", "a", "gone"))}))

    def test_baseline_missing_and_baseline_id_together_are_refused(self):
        rows = [rec("win11.settings.x", surface="settings", kind="surface", baselineId="settings:a", baselineMissing=True)]
        self.assertIn("baseline-both", self.rules({"settings/baseline.json": bl("settings", ["a"]), "settings/home.json": rows}))

    def test_a_surface_check_reports_only_its_own_baseline(self):
        files = {"settings/baseline.json": bl("settings", ["a", "a"]), "taskbar/bar.json": three()}
        r = run({**REF, **files}, "--surface", "taskbar", "--json")
        self.assertEqual(json.loads(r.stdout)["errors"], [])
        r = run({**REF, **files}, "--surface", "settings", "--json")
        self.assertEqual([e["rule"] for e in json.loads(r.stdout)["errors"]], ["baseline-duplicate"])

    def test_the_published_baselines_are_clean(self):
        r = subprocess.run([sys.executable, str(SCRIPT), "--baselines"], capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        lines = [l for l in r.stdout.splitlines() if l.startswith("baseline ")]
        self.assertEqual(len(lines), 7, r.stdout)
        for line in lines:
            self.assertNotIn(": 0 rows", line)


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

    def test_malformed_json(self):
        self.fails({"taskbar/bar.json": ['{"id": "win11.taskbar.x",']}, "json")

    def test_nonfinite_numbers(self):
        for bad in ("NaN", "Infinity", "-Infinity", "1e999"):
            line = json.dumps(rec("win11.taskbar.h", kind="metric", measure={"value": 0, "unit": "px"})).replace('"value": 0', f'"value": {bad}')
            with self.subTest(bad=bad):
                self.fails({"taskbar/bar.json": [line]}, "non-finite")
                obs = '{"id": "win11.taskbar.bar.start", "captureProfile": "fhd-150", "capturedFrom": "%s", "bounds": {"x": 0, "y": 0, "width": 1, "height": 1}, "measure": {"value": %s, "unit": "px"}}' % (B, bad)
                self.fails({"taskbar/bar.json": three(), "taskbar/bar.fhd-150.json": [obs]}, "non-finite")

    def test_duplicate_observation(self):
        o = {"id": "win11.taskbar.bar.start", "captureProfile": "fhd-150",
             "bounds": {"x": 0, "y": 0, "width": 1, "height": 1}, "capturedFrom": B}
        self.fails({"taskbar/bar.json": three(), "taskbar/bar.fhd-150.json": [o],
                    "taskbar/more.fhd-150.json": [o]}, "duplicate-observation")

    def test_shortcut_scope_without_shortcut(self):
        t = three(); t[1]["shortcutScope"] = "global"
        self.fails({"taskbar/bar.json": t}, "schema")

    def test_cross_surface_conflicts_found_from_either_side(self):
        cases = {
            "duplicate-id": {"a/x.json": [rec("win11.a.dup", surface="a")], "b/x.json": [rec("win11.a.dup", surface="b")]},
            "parent-cycle": {"a/x.json": [rec("win11.a.p", surface="a", parent="win11.b.q")],
                             "b/x.json": [rec("win11.b.q", surface="b", parent="win11.a.p")]},
            "duplicate-order": {"a/x.json": [rec("win11.a.root", surface="a", kind="surface"),
                                             rec("win11.a.root.one", surface="a", parent="win11.a.root")],
                                "b/x.json": [rec("win11.b.two", surface="b", parent="win11.a.root")]},
        }
        for rule, files in cases.items():
            for side in ("a", "b"):
                with self.subTest(rule=rule, surface=side):
                    self.fails(files, rule, "--surface", side)
        r = run({**cases["duplicate-id"], "c/x.json": [rec("win11.c.ok", surface="c")]}, "--surface", "c")
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_observation_without_a_measurement(self):
        self.fails({"taskbar/bar.json": three(), "taskbar/bar.fhd-150.json": [
            {"id": "win11.taskbar.bar.start", "captureProfile": "fhd-150", "capturedFrom": B}]}, "schema")

    def test_surface_must_be_a_folder_name(self):
        for spelling in ("taskbar/", "./taskbar", "..", "taskbar/../start"):
            with self.subTest(spelling=spelling):
                r = run({"taskbar/bar.json": ["{not json"]}, "--surface", spelling)
                self.assertEqual(r.returncode, 2, r.stdout + r.stderr)

    def test_large_conflict_group_names_one_peer(self):
        lines = [rec("win11.taskbar.same", order=i) for i in range(300)]
        r = run({"taskbar/bar.json": lines}, "--json")
        self.assertEqual(r.returncode, 1, r.stderr)
        errs = json.loads(r.stdout)["errors"]
        self.assertTrue(all(e["rule"] == "duplicate-id" for e in errs))
        self.assertEqual(len(errs), 300)
        self.assertTrue(all(e["message"].endswith("and 298 more") for e in errs))

    def test_first_50_errors_are_printed(self):
        lines = [rec(f"win11.taskbar.x{i}", parent="win11.taskbar.none", order=i) for i in range(60)]
        r = self.fails({"taskbar/bar.json": lines}, "orphan-parent")
        self.assertEqual(sum(1 for l in r.stdout.splitlines() if ": orphan-parent:" in l), 50)
        self.assertIn("... 10 more error(s)", r.stdout)



NAMES = [{"reference": "OneDrive", "ours": "Cloud Files", "reason": "trade mark"}]


class Facts(Failing):
    """Section 4: the inventory holds facts, never Microsoft's text."""

    def test_label_of_80_characters_passes(self):
        t = three(); t[1]["label"] = "x" * 80
        self.assertEqual(run({"taskbar/bar.json": t}).returncode, 0)

    def test_label_of_120_characters_is_a_sentence(self):
        t = three(); t[1]["label"] = "Choose how the taskbar shows apps and which icons appear in the corner of it " * 2
        r = self.fails({"taskbar/bar.json": t[:1] + [dict(t[1], label=t[1]["label"][:120])] + t[2:]}, "label-sentence")
        self.assertIn("label is a sentence (120 characters", r.stdout)

    def test_link_anywhere_in_a_record(self):
        for field, value in (("behaviour", "see https://support.microsoft.com/x"),
                             ("uiaId", "HTTP://go.microsoft.com"), ("className", "a https://x b"),
                             ("measure", {"value": 1, "unit": "px", "readBy": "from https://learn.microsoft.com"})):
            with self.subTest(field=field):
                t = three(); t[1][field] = value
                r = run({"taskbar/bar.json": t})
                self.assertIn(": link: ", r.stdout)
                self.assertIn("link in record", r.stdout)

    def test_link_in_an_observation(self):
        o = {"id": "win11.taskbar.bar.start", "captureProfile": "fhd-150", "capturedFrom": B,
             "measure": {"value": 1, "unit": "px", "readBy": "per https://learn.microsoft.com"}}
        r = self.fails({"taskbar/bar.json": three(), "taskbar/bar.fhd-150.json": [o]}, "link")
        self.assertNotIn(": schema: ", r.stdout)

    def test_behaviour_that_repeats_the_label(self):
        t = three(); t[1]["behaviour"] = "  START "
        self.fails({"taskbar/bar.json": t}, "behaviour-is-label")

    def test_behaviour_in_our_words_passes(self):
        t = three(); t[1]["behaviour"] = "Opens the Start menu; pressing it again closes it."
        self.assertEqual(run({"taskbar/bar.json": t}).returncode, 0)

    def test_names_json_passes(self):
        self.assertEqual(run({"names.json": [json.dumps(NAMES)]}).returncode, 0)

    def test_names_json_faults(self):
        cases = {
            "duplicate": NAMES + [{"reference": "onedrive", "ours": "Drive", "reason": "trade mark"}],
            "missing field": [{"reference": "OneDrive", "ours": "Cloud Files"}],
            "empty value": [{"reference": "OneDrive", "ours": " ", "reason": "trade mark"}],
            "same name": [{"reference": "OneDrive", "ours": "onedrive", "reason": "trade mark"}],
            "not an array": {"reference": "OneDrive"},
            "empty": [],
        }
        for name, value in cases.items():
            with self.subTest(case=name):
                self.fails({"names.json": [json.dumps(value)]}, "names")
        self.fails({"names.json": ["[not json"]}, "names")

    def test_real_names_json_is_valid(self):
        r = subprocess.run([sys.executable, str(SCRIPT)], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


class Index(unittest.TestCase):
    """--index judges the staged bytes, not a later unstaged fix."""

    def setUp(self):
        self.repo = pathlib.Path(tempfile.mkdtemp())
        self.inv = self.repo / "docs" / "design" / "inventory"
        (self.inv / "taskbar").mkdir(parents=True)
        shutil.copy(SCHEMA, self.inv / "schema.json")
        self.git("init", "-q")

    def tearDown(self):
        shutil.rmtree(self.repo)

    def git(self, *args):
        subprocess.run(["git", "-C", str(self.repo), *args], check=True, capture_output=True, timeout=60)

    def write(self, records):
        (self.inv / "taskbar" / "bar.json").write_text("".join(json.dumps(r) + "\n" for r in records))

    def check(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.inv), *args],
                              capture_output=True, text=True, timeout=60)

    def test_staged_fault_with_unstaged_fix(self):
        bad = three(); bad[1]["label"] = "y" * 90
        self.write(bad); self.git("add", "-A")
        self.write(three())
        self.assertEqual(self.check().returncode, 0)
        r = self.check("--index")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("label is a sentence", r.stdout)

    def test_staged_fix_with_unstaged_fault(self):
        self.write(three()); self.git("add", "-A")
        bad = three(); bad[1]["behaviour"] = "start"
        self.write(bad)
        self.assertEqual(self.check().returncode, 1)
        self.assertEqual(self.check("--index").returncode, 0)

    def test_staged_deletion_is_not_checked(self):
        bad = three(); bad[1]["uiaId"] = "https://x"
        self.write(bad); self.git("add", "-A")
        self.git("rm", "-q", "--cached", "docs/design/inventory/taskbar/bar.json")
        r = self.check("--index")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("inventory: 0 records", r.stdout)

    def test_no_schema_staged(self):
        self.write(three())
        r = self.check("--index")
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=1)
