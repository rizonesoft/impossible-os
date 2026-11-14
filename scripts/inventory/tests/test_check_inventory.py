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
         "order": 0, "source": f"uia:{B}", "capturedFrom": B, "disposition": "build"}
    r.update(kw)
    return {k: v for k, v in r.items() if v is not None}   # disposition=None: an undecided record


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


def absent(*rows, build=B):
    body = "\n".join(f"| `{b}` | `{r}` | {build} | {why} |" for b, r, why in rows)
    return ["# Reference", "", "## Which baseline rows are absent?", "",
            "| Baseline | Row | Build | Reason |", "| --- | --- | --- | --- |", body, "", "## Next", ""]


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
                         {"owner": "settings", "rows": 2, "matched": 0, "absent": 0, "pending": 2, "unmatched": 0, "stale": 0, "removed": []})

    def test_a_captured_owner_turns_its_open_rows_unmatched(self):
        rows = [rec("win11.settings.x", surface="settings", kind="surface", baselineId="settings:a")]
        _, out = self.report({"settings/baseline.json": bl("settings", ["a", "b", "c"]),
                              "settings/home.json": rows, "REFERENCE.md": absent(("settings", "c", "not in this edition"))})
        self.assertEqual(out["baselines"]["settings"],
                         {"owner": "settings", "rows": 3, "matched": 1, "absent": 1, "pending": 0, "unmatched": 1, "stale": 0, "removed": []})

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
        self.assertEqual(out["baselines"]["settings"],
                         {"owner": "settings", "rows": 1, "matched": 0, "absent": 0, "pending": 0, "unmatched": 1, "stale": 1, "removed": []})
        r = run({**REF, "settings/baseline.json": bl("settings", ["a"]), "settings/home.json": [old]}, "--baselines")
        self.assertIn("1 of the open rows have a match or an absence from another build", r.stdout)

    def test_an_old_capture_never_matches_the_imported_controls_baseline(self):
        up = {"repo": "o/r", "commit": SHA, "license": "MIT"}
        base = {"controls/baseline.json": bl("controls", ["Button"], build=None, upstream=up)}
        at = lambda b: [rec("win11.system.x", surface="system", kind="surface", baselineId="controls:Button",
                            source=f"uia:{b}", capturedFrom=b)]
        _, out = self.report({**base, "system/home.json": at("26300.1")})
        got = out["baselines"]["controls"]
        self.assertEqual((got["matched"], got["unmatched"], got["stale"]), (0, 1, 1))
        # ...so it cannot collide with an absence checked on the reference build.
        _, out = self.report({**base, "system/home.json": at("26300.1"), "REFERENCE.md": absent(("controls", "Button", "not shown"))})
        self.assertNotIn("absent-matched", [e["rule"] for e in out["errors"]])
        self.assertEqual(out["baselines"]["controls"]["absent"], 1)
        _, out = self.report({**base, "system/home.json": at(B)})
        self.assertEqual(out["baselines"]["controls"]["matched"], 1)

    def test_a_new_reference_build_is_reached_by_baselines_then_captures(self):
        A, up = "26300.9000", {"repo": "o/r", "commit": SHA, "license": "MIT"}
        ref = lambda b: {"reference.json": [{"edition": "Professional", "displayVersion": "26H2", "build": b}]}
        at = lambda b: [rec("win11.settings.x", surface="settings", kind="surface", baselineId="settings:a",
                            source=f"uia:{b}", capturedFrom=b)]
        errors = lambda files: [e["rule"] for e in json.loads(run(files, "--json").stdout)["errors"]]
        # Build A, matched.
        self.assertEqual(errors({**ref(A), "settings/baseline.json": bl("settings", ["a"], build=A), "settings/home.json": at(A)}), [])
        # reference.json moves to B: the old baselines are refused until they are read again...
        self.assertEqual(errors({**ref(B), "settings/baseline.json": bl("settings", ["a"], build=A), "settings/home.json": at(A)}),
                         ["baseline-header"])
        # ...the new baselines publish over the old records, which no longer count...
        step = {**ref(B), "settings/baseline.json": bl("settings", ["a"]), "settings/home.json": at(A)}
        self.assertEqual(errors(step), [])
        out = json.loads(run(step, "--baselines", "--json").stdout)["baselines"]["settings"]
        self.assertEqual((out["matched"], out["unmatched"], out["stale"]), (0, 1, 1))
        # A row B no longer has: the old record is stale, not an orphan, so the baselines still publish...
        gone = {**ref(B), "settings/baseline.json": bl("settings", ["b"]), "settings/home.json": at(A)}
        self.assertEqual(errors(gone), [])
        out = json.loads(run(gone, "--baselines", "--json").stdout)["baselines"]["settings"]
        self.assertEqual((out["rows"], out["matched"], out["unmatched"], out["stale"], out["removed"]), (1, 0, 1, 0, ["a"]))
        self.assertIn("names 1 row(s) this build no longer has: a", run(gone, "--baselines").stdout)
        # ...while a capture of B naming it is an orphan (capture.py unbinds it on --merge).
        self.assertEqual(errors({**gone, "settings/home.json": at(B)}), ["baseline-orphan"])
        # ...and the re-capture on B matches again.
        done = {**ref(B), "settings/baseline.json": bl("settings", ["a"]), "settings/home.json": at(B)}
        out = json.loads(run(done, "--baselines", "--json").stdout)["baselines"]["settings"]
        self.assertEqual((out["matched"], out["unmatched"], out["stale"]), (1, 0, 0))

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

    def test_a_machine_baseline_cannot_trade_its_build_for_an_upstream(self):
        up = {"repo": "o/r", "commit": SHA, "license": "MIT"}
        r, out = self.report({"settings/baseline.json": bl("settings", ["a"], build=None, upstream=up)})
        self.assertIn("baseline-header", [e["rule"] for e in out["errors"]])
        self.assertEqual(r.returncode, 1)
        self.assertIn("baseline-header", self.rules({"controls/baseline.json": bl("controls", ["a"])}))

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

    def test_an_unsound_absence_never_counts_and_is_reported_under_the_owner(self):
        base = {"known-folders/baseline.json": bl("known-folders", ["a", "b"], owner="explorer"),
                "explorer/x.json": [rec("win11.explorer.x", surface="explorer", kind="surface", baselineId="known-folders:a")],
                "taskbar/bar.json": three()}
        cases = {"absent-reason": absent(("known-folders", "b", " ")),
                 "absent-orphan": absent(("known-folders", "zz", "why")),
                 "absent-duplicate": absent(("known-folders", "b", "x"), ("known-folders", "b", "y")),
                 "absent-matched": absent(("known-folders", "a", "gone"))}
        for rule, table in cases.items():
            with self.subTest(rule):
                files = {**base, "REFERENCE.md": table}
                for scope in ((), ("--surface", "explorer"), ("--surface", "known-folders")):
                    r, out = self.report(files, *scope)
                    self.assertIn(rule, [e["rule"] for e in out["errors"]], scope)
                    c = out["baselines"]["known-folders"]
                    self.assertEqual(c["matched"] + c["absent"] + c["unmatched"], 2, c)
                    self.assertEqual((c["matched"], c["absent"]), (1, 1 if rule == "absent-duplicate" else 0), c)
                _, out = self.report(files, "--surface", "taskbar")
                self.assertNotIn(rule, [e["rule"] for e in out["errors"]])

    def test_a_broken_absent_table_is_reported_under_every_surface(self):
        table = ["## Which baseline rows are absent?", "", "| Baseline | Row | Reason |", "| --- | --- | --- |", "| `settings` | `a` | gone |"]
        files = {"settings/baseline.json": bl("settings", ["a"]), "taskbar/bar.json": three(), "REFERENCE.md": table}
        self.assertIn("absent-table", self.rules(files, "--surface", "taskbar"))

    def test_the_absent_section_is_found_and_ended_by_the_shared_heading_rule(self):
        # An indented heading is still a heading (CommonMark allows three spaces).
        base = {"settings/baseline.json": bl("settings", ["a", "b"]), "taskbar/bar.json": three()}
        table = ["  ## Which baseline rows are absent?", "", "| Baseline | Row | Build | Reason |", "| --- | --- | --- | --- |",
                 f"| `settings` | `a` | {B} | gone |", "", "   ## Next", "", "| `settings` | `b` | x | not part of the table |"]
        _, out = self.report({**base, "REFERENCE.md": table})
        self.assertEqual([e["rule"] for e in out["errors"] if e["rule"] != "baseline-missing"], [])
        self.assertEqual(out["baselines"]["settings"]["absent"], 1)

    def test_an_absent_table_without_its_delimiter_row_is_refused(self):
        base = {"settings/baseline.json": bl("settings", ["a", "b"]), "taskbar/bar.json": three()}
        head = ["## Which baseline rows are absent?", "", "| Baseline | Row | Build | Reason |"]
        rows = [f"| `settings` | `a` | {B} | gone |", f"| `settings` | `b` | {B} | gone |"]
        for table in (head + rows, head + ["| --- | --- | --- |"] + rows, head + ["| - | - | - | - |"] + rows, head):
            for scope in ((), ("--surface", "taskbar")):
                r, out = self.report({**base, "REFERENCE.md": table}, *scope)
                self.assertIn("absent-table", [e["rule"] for e in out["errors"]], table)
                self.assertEqual(out["baselines"]["settings"]["absent"], 0)
        _, out = self.report({**base, "REFERENCE.md": head + ["| :--- | ---: | :---: | --- |"] + rows})
        self.assertEqual(out["baselines"]["settings"]["absent"], 2)

    def test_an_absence_checked_on_another_build_is_stale_and_counts_nothing(self):
        # The row survived the build change; whether the new build shows it has not been checked.
        base = {"settings/baseline.json": bl("settings", ["a", "b"]),
                "settings/home.json": [rec("win11.settings.y", surface="settings", kind="surface")]}
        r, out = self.report({**base, "REFERENCE.md": absent(("settings", "a", "gated off"), build="26300.9000")})
        self.assertEqual([e["rule"] for e in out["errors"] if e["rule"] != "baseline-missing"], [])
        self.assertEqual(out["baselines"]["settings"],
                         {"owner": "settings", "rows": 2, "matched": 0, "absent": 0, "pending": 0, "unmatched": 2, "stale": 1, "removed": []})
        _, out = self.report({**base, "REFERENCE.md": absent(("settings", "a", "gated off"))})
        self.assertEqual((out["baselines"]["settings"]["absent"], out["baselines"]["settings"]["unmatched"],
                          out["baselines"]["settings"]["stale"]), (1, 1, 0))
        # A row the new build removed: the old entry is stale, not an orphan, so the new baselines publish.
        r, out = self.report({**base, "REFERENCE.md": absent(("settings", "gone", "gated off"), build="26300.9000")})
        self.assertEqual([e["rule"] for e in out["errors"] if e["rule"] != "baseline-missing"], [])
        got = out["baselines"]["settings"]
        self.assertEqual((got["absent"], got["stale"], got["removed"]), (0, 0, ["gone"]))
        self.assertIn("absent-orphan", self.rules({**base, "REFERENCE.md": absent(("settings", "gone", "gated off"))}))
        for bad in ("", "26H2", "build 9711"):
            self.assertIn("absent-build", self.rules({**base, "REFERENCE.md": absent(("settings", "a", "why"), build=bad)}))

    def test_a_record_of_another_surface_may_match_and_the_owner_answers_for_the_rest(self):
        # The settings index lists classic Control Panel entries, captured under control-panel.
        cp = [rec("win11.control-panel.x", surface="control-panel", kind="surface", baselineId="settings:Classic_a")]
        files = {"settings/baseline.json": bl("settings", ["Classic_a", "b"]), "control-panel/home.json": cp}
        r, out = self.report(files)
        self.assertNotIn("baseline-orphan", [e["rule"] for e in out["errors"]])
        self.assertEqual(out["baselines"]["settings"],
                         {"owner": "settings", "rows": 2, "matched": 1, "absent": 0, "pending": 1, "unmatched": 0, "stale": 0, "removed": []})
        files["settings/home.json"] = [rec("win11.settings.y", surface="settings", kind="surface")]
        _, out = self.report(files)
        self.assertEqual((out["baselines"]["settings"]["pending"], out["baselines"]["settings"]["unmatched"]), (0, 1))

    def test_a_match_to_a_baseline_with_no_file_is_refused_without_the_flag(self):
        # What capture.py's scratch check and lint run: a missing baseline cannot pass with matches naming it.
        files = {"taskbar/bar.json": [rec("win11.taskbar.bar", kind="surface", baselineId="settings:a")]}
        r = run({**REF, **files}, "--json")
        self.assertEqual(r.returncode, 1)
        self.assertIn("baseline-orphan", [e["rule"] for e in json.loads(r.stdout)["errors"]])

    def test_baseline_missing_and_baseline_id_together_are_refused(self):
        rows = [rec("win11.settings.x", surface="settings", kind="surface", baselineId="settings:a", baselineMissing=True)]
        self.assertIn("baseline-both", self.rules({"settings/baseline.json": bl("settings", ["a"]), "settings/home.json": rows}))

    def test_a_surface_check_reports_only_its_own_baseline(self):
        files = {"settings/baseline.json": bl("settings", ["a", "a"]), "taskbar/bar.json": three()}
        r = run({**REF, **files}, "--surface", "taskbar", "--json")
        self.assertEqual(json.loads(r.stdout)["errors"], [])
        r = run({**REF, **files}, "--surface", "settings", "--json")
        self.assertEqual([e["rule"] for e in json.loads(r.stdout)["errors"]], ["baseline-duplicate"])

    def test_a_baseline_is_checked_under_the_surface_that_owns_it(self):
        # known-folders and shell-commands are explorer's, start-apps is start's, the two control lists system's.
        for name, owner in (("known-folders", "explorer"), ("shell-commands", "explorer"), ("start-apps", "start"),
                            ("controls", "system"), ("classic-controls", "system")):
            with self.subTest(name):
                src = {"build": None, "upstream": {"repo": "o/r", "commit": SHA, "license": "MIT"}} if name == "controls" else {}
                dup = bl(name, ["a", "a"], owner=owner, **src)
                self.assertEqual(self.rules({f"{name}/baseline.json": dup, f"{owner}/x.json": []}, "--surface", owner)
                                 [:1], ["baseline-duplicate"])
                bad = bl(name, ["a"], owner=owner, extra=1, **src) + ['{"no": "id"}']
                rules = self.rules({f"{name}/baseline.json": bad, f"{owner}/x.json": []}, "--surface", owner)
                self.assertIn("baseline-header", rules)
                self.assertIn("baseline-row", rules)

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


SHELL = "# Shell\n\n## Settings frame\n\ntext\n\n## One flow\n\ntext\n"


def entry(eid, *variants, kind="look", **kw):
    e = {"id": eid, "kind": kind, "question": "q?", "variants": list(variants), "rule": "one rule", "reason": "two answers"}
    e.update(kw)
    return e


def ledger(*entries):
    return [json.dumps(list(entries))]


def decide(files, *args):
    """Run with a design folder inside the tree, so a ledger design can resolve."""
    files = dict(files)
    files.setdefault("design/shell.md", [SHELL.rstrip("\n")])
    tree = Tree(files)
    return tree.run("--design-root", str(pathlib.Path(tree.dir) / "design"), *args)


def panel():
    """Settings (built) and a Control Panel page (replaced), one setting on each."""
    return {"settings/a.json": [
                rec("win11.settings.frame", surface="settings", kind="surface", itemCount=1, inconsistency="home"),
                rec("win11.settings.frame.sound", surface="settings", kind="page", parent="win11.settings.frame", itemCount=1),
                rec("win11.settings.frame.sound.volume", surface="settings", kind="setting", parent="win11.settings.frame.sound")],
            "control-panel/a.json": [
                rec("win11.control-panel.frame", surface="control-panel", kind="surface", itemCount=1,
                    disposition="replaced", inconsistency="home"),
                rec("win11.control-panel.frame.sound", surface="control-panel", kind="page", parent="win11.control-panel.frame",
                    itemCount=2, disposition="replaced", inconsistency="home", mergedInto="win11.settings.frame.sound"),
                rec("win11.control-panel.frame.sound.volume", surface="control-panel", kind="setting",
                    parent="win11.control-panel.frame.sound", order=0, disposition="replaced", inconsistency="home",
                    mergedInto="win11.settings.frame.sound.volume"),
                rec("win11.control-panel.frame.sound.scheme", surface="control-panel", kind="setting",
                    parent="win11.control-panel.frame.sound", order=1, placedUnder="win11.settings.frame.sound")],
            "consistency.json": ledger(entry("home", "win11.settings.frame", "win11.control-panel.frame",
                                             kind="home", design="shell.md#settings-frame"))}


class Decisions(unittest.TestCase):
    """Section 6: dispositions and the consistency ledger."""

    def fails(self, files, needle, *args):
        r = decide(files, *args)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn(needle, r.stdout)
        return r

    def test_a_record_without_a_disposition_fails(self):
        lines = three(); lines[1] = rec("win11.taskbar.bar.start", parent="win11.taskbar.bar", disposition=None)
        r = self.fails({"taskbar/bar.json": lines}, "disposition-missing: win11.taskbar.bar.start")
        self.assertIn("inventory: 3 records, 1 errors", r.stdout)

    def test_a_fresh_capture_may_be_undecided_and_a_commit_may_not(self):
        lines = [rec(r["id"], **{**r, "disposition": None}) for r in three()]
        self.assertEqual(decide({"taskbar/bar.json": lines}, "--allow-undecided").returncode, 0)
        self.assertEqual(decide({"taskbar/bar.json": lines}).returncode, 1)

    def test_a_catalogue_record_needs_no_disposition(self):
        lines = three() + [rec("win11.taskbar.bar.height", kind="metric", parent="win11.taskbar.bar", order=2,
                               measure={"value": 48, "unit": "px"}, disposition=None)]
        lines[0]["itemCount"] = 3
        r = decide({"taskbar/bar.json": lines})
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_excluded_needs_a_reason_and_only_excluded_has_one(self):
        lines = three(); lines[1]["disposition"] = "excluded"
        self.fails({"taskbar/bar.json": lines}, "disposition-fields: win11.taskbar.bar.start: an excluded record gives its reason")
        lines = three(); lines[1]["reason"] = "advertising"
        self.fails({"taskbar/bar.json": lines}, "only an excluded record has one")
        lines = three(); lines[1].update(disposition="excluded", reason="advertising")
        self.assertEqual(decide({"taskbar/bar.json": lines}).returncode, 0)

    def test_an_inconsistency_names_a_ledger_entry(self):
        lines = three(); lines[1]["inconsistency"] = "no-such"
        self.fails({"taskbar/bar.json": lines}, "inconsistency-undefined: win11.taskbar.bar.start: inconsistency no-such")
        r = decide({"taskbar/bar.json": lines, "consistency.json": ledger(entry("no-such"))})
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_a_ledger_variant_names_a_record(self):
        self.fails({"taskbar/bar.json": three(), "consistency.json": ledger(entry("bars", "win11.taskbar.gone"))},
                   "ledger-variant: bars: variant win11.taskbar.gone names no record")

    def test_the_whole_replacement_passes(self):
        r = decide(panel())
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("inventory: 7 records, 0 errors", r.stdout)

    def test_replaced_needs_two_captured_variants(self):
        files = panel()
        files["consistency.json"] = ledger(entry("home", "win11.control-panel.frame", kind="home", design="shell.md#settings-frame"))
        r = self.fails(files, "replaced-evidence: win11.control-panel.frame: replaced under home, which names 1 captured variant(s)")
        # A variant that names no record is no evidence either.
        files["consistency.json"] = ledger(entry("home", "win11.control-panel.frame", "win11.settings.gone",
                                                 kind="home", design="shell.md#settings-frame"))
        self.fails(files, "which names 1 captured variant(s)")

    def test_replaced_must_be_a_variant_or_sit_under_one(self):
        files = panel()
        files["taskbar/bar.json"] = three()
        files["taskbar/bar.json"][1].update(disposition="replaced", inconsistency="home")
        self.fails(files, "replaced-evidence: win11.taskbar.bar.start: replaced under home, but it is not a variant")

    def test_replaced_without_an_inconsistency_or_a_target_is_refused(self):
        files = panel(); del files["control-panel/a.json"][0]["inconsistency"]
        self.fails(files, "disposition-fields: win11.control-panel.frame: a replaced record names its ledger entry")
        files = panel(); del files["control-panel/a.json"][2]["mergedInto"]
        self.fails(files, "a replaced setting names the record that holds it in mergedInto")
        files = panel(); files["settings/a.json"][2]["mergedInto"] = "win11.settings.frame.sound"
        self.fails(files, "only a replaced record is mergedInto another")

    def test_many_settings_cannot_merge_into_a_page(self):
        files = panel()
        files["control-panel/a.json"][2]["mergedInto"] = "win11.settings.frame.sound"
        self.fails(files, "merged-target: win11.control-panel.frame.sound.volume: a replaced setting is held by a setting, "
                          "and win11.settings.frame.sound is a page")

    def test_a_merge_target_exists_and_is_built(self):
        files = panel(); files["control-panel/a.json"][2]["mergedInto"] = "win11.settings.frame.gone"
        self.fails(files, "merged-target: win11.control-panel.frame.sound.volume: mergedInto win11.settings.frame.gone names no record")
        files = panel(); files["settings/a.json"][2].update(disposition="excluded", reason="cloud")
        self.fails(files, "which is itself excluded; nothing holds the option")
        files = panel(); files["settings/a.json"][0].update(disposition="excluded", reason="cloud")
        r = self.fails(files, "merged-target: win11.control-panel.frame.sound.volume: mergedInto win11.settings.frame.sound.volume, "
                              "which sits under a replaced or excluded record")
        self.assertIn("placed-target: win11.control-panel.frame.sound.scheme: placedUnder win11.settings.frame.sound, "
                      "which sits under a replaced or excluded record", r.stdout)
        self.assertIn("unplaced: win11.settings.frame.sound:", r.stdout)
        files = panel(); files["control-panel/a.json"][2]["mergedInto"] = "win11.control-panel.frame.sound.volume"
        self.fails(files, "which is itself replaced")

    def test_a_built_record_under_a_replaced_home_is_placed(self):
        files = panel(); del files["control-panel/a.json"][3]["placedUnder"]
        self.fails(files, "unplaced: win11.control-panel.frame.sound.scheme: built, but its captured home is replaced")

    def test_what_sits_under_a_placed_record_needs_no_place_of_its_own(self):
        files = panel()
        files["control-panel/a.json"].append(rec("win11.control-panel.frame.sound.scheme.list", surface="control-panel",
                                                 parent="win11.control-panel.frame.sound.scheme"))
        r = decide(files)
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_a_place_is_a_built_page_or_group(self):
        files = panel(); files["control-panel/a.json"][3]["placedUnder"] = "win11.settings.frame.gone"
        self.fails(files, "placed-target: win11.control-panel.frame.sound.scheme: placedUnder win11.settings.frame.gone names no record")
        files = panel(); files["control-panel/a.json"][3]["placedUnder"] = "win11.settings.frame.sound.volume"
        self.fails(files, "placedUnder names a page or group, and win11.settings.frame.sound.volume is a setting")
        files = panel(); files["control-panel/a.json"][3]["placedUnder"] = "win11.control-panel.frame.sound"
        self.fails(files, "which is itself replaced; a place is built")
        files = panel(); files["control-panel/a.json"][2].update(placedUnder="win11.settings.frame.sound")
        self.fails(files, "a replaced record is not built, so it has no placedUnder")

    def test_a_place_never_leads_back_to_itself(self):
        scheme = "win11.control-panel.frame.sound.scheme"
        files = panel(); files["control-panel/a.json"][3].update(kind="group", placedUnder=scheme)
        self.fails(files, f"placed-target: {scheme}: placedUnder {scheme} leads back to itself")
        # Under its own captured child.
        files = panel(); files["control-panel/a.json"][3].update(kind="group", placedUnder=scheme + ".more")
        files["control-panel/a.json"].append(rec(scheme + ".more", surface="control-panel", kind="group", parent=scheme))
        self.fails(files, "leads back to itself")
        # Two records placed under each other.
        files = panel(); files["control-panel/a.json"][3].update(kind="group", placedUnder=scheme + "2")
        files["control-panel/a.json"][1]["itemCount"] = 3
        files["control-panel/a.json"].append(rec(scheme + "2", surface="control-panel", kind="group", order=2,
                                                 parent="win11.control-panel.frame.sound", placedUnder=scheme))
        r = self.fails(files, f"placed-target: {scheme}: placedUnder {scheme}2 leads back to itself")
        self.assertIn(f"placed-target: {scheme}2: placedUnder {scheme} leads back to itself", r.stdout)

    def test_a_place_under_a_placed_record_is_followed_to_its_end(self):
        scheme = "win11.control-panel.frame.sound.scheme"
        files = panel(); files["control-panel/a.json"][3].update(kind="group")
        files["control-panel/a.json"][1]["itemCount"] = 3
        files["control-panel/a.json"].append(rec(scheme + "2", surface="control-panel", kind="setting", order=2,
                                                 parent="win11.control-panel.frame.sound", placedUnder=scheme))
        r = decide(files)
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_a_merge_target_under_a_replaced_home_is_found_from_the_source_surface(self):
        files = panel()
        files["settings/a.json"][0].update(disposition="replaced")
        for args in ((), ("--surface", "control-panel")):
            with self.subTest(args=args):
                r = self.fails(files, "merged-target: win11.control-panel.frame.sound.volume: mergedInto "
                                      "win11.settings.frame.sound.volume, which sits under a replaced or excluded record", *args)
                self.assertIn("placed-target: win11.control-panel.frame.sound.scheme", r.stdout)

    def test_a_destination_chain_that_names_no_record_is_found_from_the_source_surface(self):
        files = panel()
        files["settings/a.json"][1]["placedUnder"] = "win11.settings.frame.typo"
        for args in ((), ("--surface", "control-panel")):
            with self.subTest(args=args):
                r = self.fails(files, "merged-target: win11.control-panel.frame.sound.volume: mergedInto "
                                      "win11.settings.frame.sound.volume, whose place or parent names no record", *args)
                self.assertIn("placed-target: win11.control-panel.frame.sound.scheme: placedUnder win11.settings.frame.sound, "
                              "whose place or parent names no record", r.stdout)

    def test_a_placement_maps_a_record_to_a_built_page(self):
        def with_placement(source, page):
            files = panel()
            files["consistency.json"] = ledger(entry("home", "win11.settings.frame", "win11.control-panel.frame", kind="home",
                                                     design="shell.md#settings-frame", placement={source: page}))
            return files
        r = decide(with_placement("win11.control-panel.frame.sound", "win11.settings.frame.sound"))
        self.assertEqual(r.returncode, 0, r.stdout)
        self.fails(with_placement("win11.control-panel.frame.gone", "win11.settings.frame.sound"),
                   "ledger-placement: home: placement names win11.control-panel.frame.gone, which is no record")
        self.fails(with_placement("win11.control-panel.frame.sound", "win11.settings.frame.sound.volume"),
                   "which is not a built page")
        self.fails(with_placement("win11.control-panel.frame.sound", "win11.control-panel.frame.sound"),
                   "which is not a built page")
        self.fails(with_placement("win11.control-panel.frame.sound", "not an id"), "ledger: home: placement")

    def test_a_long_chain_is_walked_in_linear_time(self):
        import time
        lines = [rec("win11.taskbar.deep", kind="surface", itemCount=1, disposition="replaced", inconsistency="deep")]
        for i in range(4000):
            lines.append(rec(f"win11.taskbar.d{i}", kind="group",
                             parent=lines[-1]["id"], itemCount=1, disposition="replaced", inconsistency="deep"))
        lines[-1].pop("itemCount")
        files = {"taskbar/deep.json": lines, "taskbar/bar.json": three(),
                 "consistency.json": ledger(entry("deep", "win11.taskbar.deep", "win11.taskbar.bar"))}
        start = time.monotonic()
        r = decide(files)
        self.assertEqual(r.returncode, 0, r.stdout[:2000])
        self.assertLess(time.monotonic() - start, 20)

    def test_ledger_faults(self):
        base = {"taskbar/bar.json": three()}
        for text, needle in (
                ("{", "ledger: not JSON"),
                ("{}", "consistency.json is an array"),
                (json.dumps([entry("a"), entry("a")]), "ledger: a is defined twice"),
                (json.dumps([entry("a", kind="flow")]), "'design' is a required property"),
                (json.dumps([entry("a", kind="shape")]), "ledger: a: kind"),
                (json.dumps([entry("a", extra=1)]), "ledger: a: (entry)"),
                (json.dumps([entry("a", "win11.taskbar.bar", "win11.taskbar.bar")]), "ledger: a: variants"),
                (json.dumps([entry("a", kind="flow", design="shell.md#no-such")]), "design names #no-such, which shell.md does not have"),
                (json.dumps([entry("a", kind="home", design="gone.md#x")]), "design names gone.md, which is not a page"),
                (json.dumps([entry("a", kind="home", design="../x.md#y")]), "ledger: a: design")):
            with self.subTest(text=text):
                self.fails({**base, "consistency.json": [text]}, needle)

    def test_a_design_resolves_to_a_heading(self):
        r = decide({"taskbar/bar.json": three(),
                    "consistency.json": ledger(entry("a", kind="flow", design="shell.md#one-flow"))})
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_a_scratch_copy_without_design_pages_names_the_design_root(self):
        files = {"taskbar/bar.json": three(), "consistency.json": ledger(entry("a", kind="flow", design="shell.md#one-flow"))}
        r = run(files)   # the design root defaults to the parent of the inventory, which holds no shell.md
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("which is not a page", r.stdout)

    def test_a_surface_check_reports_its_records_and_not_the_ledger(self):
        files = panel()
        files["consistency.json"] = ledger(entry("home", "win11.control-panel.frame", "win11.settings.gone",
                                                 kind="home", design="shell.md#settings-frame"))
        r = decide(files, "--surface", "settings")
        self.assertEqual(r.returncode, 0, r.stdout)
        r = decide(files, "--surface", "control-panel")
        self.assertNotIn("ledger-variant", r.stdout)
        self.assertIn("replaced-evidence", r.stdout)

    def test_the_published_ledger_is_seeded_and_clean(self):
        r = subprocess.run([sys.executable, str(SCRIPT)], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        entries = json.loads((REPO / "docs" / "design" / "inventory" / "consistency.json").read_text())
        ids = {e["id"] for e in entries}
        self.assertGreaterEqual(len(ids), 9)
        self.assertLessEqual({"settings-home", "hello-enrolment", "tool-frame", "context-menus"}, ids)
        self.assertEqual({e["kind"] for e in entries}, {"look", "flow", "home"})


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

    def test_a_design_is_judged_by_the_staged_page(self):
        design = self.repo / "docs" / "design" / "shell.md"
        (self.inv / "consistency.json").write_text(json.dumps([entry("a", kind="flow", design="shell.md#one-flow")]))
        self.write(three())
        design.write_text("# Shell\n\n## Another flow\n")
        self.git("add", "-A")
        design.write_text(SHELL)   # the unstaged fix
        self.assertEqual(self.check().returncode, 0)
        r = self.check("--index")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("design names #one-flow", r.stdout)
        self.git("add", "-A")
        design.write_text("# Shell\n")   # the unstaged fault
        self.assertEqual(self.check().returncode, 1)
        self.assertEqual(self.check("--index").returncode, 0)

    def test_index_takes_no_design_root(self):
        self.write(three()); self.git("add", "-A")
        r = self.check("--index", "--design-root", str(self.repo))
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("takes no --design-root", r.stderr)

    def test_no_schema_staged(self):
        self.write(three())
        r = self.check("--index")
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=1)
