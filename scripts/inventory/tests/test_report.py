#!/usr/bin/env python3
"""Tests for scripts/inventory/inventory-report.py, the inventory coverage report.

Run: python3 scripts/inventory/tests/test_report.py
"""
import importlib.util
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import types
import unittest

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE.parent / "inventory-report.py"
SCHEMA = HERE.parents[2] / "docs" / "design" / "inventory" / "schema.json"
TIMEOUT = 120  # deadline: one report over a handful of fixture files


def rec(rid, **kw):
    r = {"id": rid, "surface": "taskbar", "kind": "control", "label": rid.rsplit(".", 1)[1],
         "order": 0, "source": "uia:26300.9711", "capturedFrom": "26300.9711", "disposition": "build"}
    r.update(kw)
    return r


def section(n, *lines):
    return [f"## {n}. Fixture section {n}", "", *lines, ""]


class Tree:
    def __init__(self, records, todo_sections, ledger=None, files=None):
        self.dir = pathlib.Path(tempfile.mkdtemp(prefix="inventory-report-test-"))
        inv = self.dir / "docs" / "design" / "inventory"
        (inv / "taskbar").mkdir(parents=True)
        shutil.copy(SCHEMA, inv / "schema.json")
        (inv / "taskbar" / "fixture.json").write_text("".join(json.dumps(r) + "\n" for r in records), encoding="utf-8")
        if ledger is not None:
            (inv / "consistency.json").write_text(json.dumps(ledger), encoding="utf-8")
        todo = self.dir / "todo" / "00-fixture"
        todo.mkdir(parents=True)
        body = ["# TODO-01 Fixture", ""]
        for s in todo_sections:
            body += s
        (todo / "TODO-01-fixture.md").write_text("\n".join(body) + "\n", encoding="utf-8")
        for rel, content in (files or {}).items():
            path = self.dir / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")

    def run(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.dir), *args],
                              capture_output=True, text=True, timeout=TIMEOUT)

    def close(self):
        shutil.rmtree(self.dir, ignore_errors=True)


class Owners(unittest.TestCase):
    def check(self, tree):
        try:
            return tree.run("--owners", "--check")
        finally:
            tree.close()

    def test_checkpoint_three_records(self):
        out = self.check(Tree([rec("win11.fixture.a"), rec("win11.fixture.b"), rec("win11.fixture.c")],
                              [section(1, "**Inventory:** win11.fixture.a"),
                               section(2, "**Inventory:** `win11.fixture.c`"),
                               section(3, "**Inventory:** win11.fixture.c")]))
        self.assertEqual(out.returncode, 1, out.stderr)
        self.assertEqual(out.stdout.splitlines(), ["no owner: win11.fixture.b", "two owners: win11.fixture.c"])

    def test_longest_prefix_wins(self):
        out = self.check(Tree([rec("win11.fixture.a"), rec("win11.fixture.b")],
                              [section(1, "**Inventory:** win11.fixture"),
                               section(2, "**Inventory:** win11.fixture.a")]))
        self.assertEqual((out.returncode, out.stdout), (0, ""), out.stderr)

    def test_replaced_excluded_and_catalogue_need_no_owner(self):
        out = self.check(Tree([rec("win11.fixture.r", disposition="replaced", inconsistency="x"),
                               rec("win11.fixture.e", disposition="excluded", reason="not ours"),
                               {k: v for k, v in rec("win11.fixture.k", kind="shortcut").items() if k != "disposition"}],
                              [section(1, "Nothing owned here.")]))
        self.assertEqual((out.returncode, out.stdout), (0, ""), out.stderr)

    def test_prose_line_owns_nothing_and_a_dead_token_is_named(self):
        out = self.check(Tree([rec("win11.fixture.a")],
                              [section(1, "**Inventory:** writes the files under `docs/design/inventory/taskbar/`"),
                               section(2, "**Inventory:** win11.fixture.a, win11.fixture.gone")]))
        self.assertEqual(out.returncode, 1)
        self.assertEqual(out.stdout.splitlines(), ["owns nothing: win11.fixture.gone (todo/00-fixture/TODO-01-fixture.md section 2)"])

    def test_flow_and_home_ledger_entries_need_an_owner(self):
        ledger = [{"id": "one-flow", "kind": "flow"}, {"id": "a-look", "kind": "look"}, {"id": "a-home", "kind": "home"}]
        out = self.check(Tree([rec("win11.fixture.a")],
                              [section(1, "**Inventory:** win11.fixture.a ledger:a-home")], ledger=ledger))
        self.assertEqual(out.stdout.splitlines(), ["no owner: ledger:one-flow"])

    def test_nested_heading_is_its_own_section(self):
        out = self.check(Tree([rec("win11.fixture.a")],
                              [section(1, "**Inventory:** win11.fixture.a"),
                               ["- ## 2. Nested", "", "**Inventory:** win11.fixture.a", ""]]))
        self.assertEqual(out.stdout.splitlines(), ["two owners: win11.fixture.a"])

    def test_owner_line_in_a_code_fence_owns_nothing(self):
        out = self.check(Tree([rec("win11.fixture.a")],
                              [section(1, "```", "**Inventory:** win11.fixture.a", "```")]))
        self.assertEqual(out.stdout.splitlines(), ["no owner: win11.fixture.a"])


class Campaign(unittest.TestCase):
    ROWS = [{"surface": "taskbar", "state": "rest", "profile": "fhd-100", "needs": ["records", "shot"]},
            {"surface": "taskbar", "state": "rest", "profile": "fhd-150", "needs": ["shot"]},
            {"surface": "start", "state": "open", "profile": "fhd-100", "needs": ["records"]},
            {"surface": "taskbar", "state": "peek", "profile": "fhd-100", "needs": ["recording"]},
            {"surface": "battery", "state": "low", "profile": "fhd-100", "needs": ["records"], "source": "absent"}]
    SHOTS = [{"surface": "taskbar", "state": "rest", "profile": "fhd-100", "file": "taskbar/rest.fhd-100.png"}]

    PROFILES = {"fhd-100": {}, "fhd-150": {}}

    def run_campaign(self, rows=ROWS, shots=SHOTS, unnamed=None, records=None, profiles=PROFILES, fixtures=None,
                     stand_ins=None, variants=None):
        inv = "docs/design/inventory/"
        files = {inv + "shots.json": json.dumps(shots), inv + "profiles.json": json.dumps(profiles)}
        if stand_ins is not None:
            files[inv + "stand-ins.json"] = json.dumps(stand_ins)
        if variants is not None:
            files["scripts/inventory/standin/variants.json"] = json.dumps(variants)
        for name, text in (fixtures or {}).items():
            files[f"scripts/inventory/fixtures/{name}.ps1"] = text
        if rows is not None:
            files[inv + "expected.json"] = json.dumps(rows)
        if unnamed is not None:
            files["build/inventory/taskbar-unnamed.json"] = json.dumps(unnamed)
        tree = Tree(records or [rec("win11.taskbar.a")], [section(1, "**Inventory:** win11.taskbar")], files=files)
        try:
            return tree.run("--campaign", "--check")
        finally:
            tree.close()

    def test_missing_matrix_fails(self):
        out = self.run_campaign(rows=None)
        self.assertEqual(out.returncode, 1)
        self.assertIn("no capture matrix", out.stdout)

    def test_each_missing_evidence_is_named(self):
        out = self.run_campaign()
        self.assertEqual(out.returncode, 1, out.stderr)
        self.assertEqual(out.stdout.splitlines(), ["no shot: taskbar/rest/fhd-150", "no records: start/open/fhd-100",
                                                   "no recording: taskbar/peek/fhd-100", "absent without a reason: battery/low/fhd-100"])

    def test_complete_matrix_passes(self):
        rows = [{"surface": "taskbar", "state": "rest", "profile": "fhd-100", "needs": ["records", "shot"]},
                {"surface": "taskbar", "state": "peek", "profile": "fhd-100", "needs": ["recording"]},
                {"surface": "battery", "state": "low", "profile": "fhd-100", "needs": ["records"], "source": "absent",
                 "reason": "no stand-in; designed as ours", "sibling": "taskbar/rest"}]
        shots = self.SHOTS + [{"surface": "taskbar", "state": "peek", "profile": "fhd-100", "file": "taskbar/peek.fhd-100.webm"}]
        out = self.run_campaign(rows=rows, shots=shots)
        self.assertEqual((out.returncode, out.stdout), (0, ""), out.stderr)

    def test_unknown_profile_is_named(self):
        out = self.run_campaign(rows=[{"surface": "taskbar", "state": "rest", "profile": "fhd-999", "needs": ["shot"]}])
        self.assertEqual(out.stdout.splitlines(), ["unknown profile: taskbar/rest/fhd-999 (not in docs/design/inventory/profiles.json)"])

    def test_checkpoint_deleting_a_surface_or_a_state_names_the_row(self):
        rows = [{"surface": "taskbar", "state": "rest", "profile": "fhd-100", "needs": ["records", "shot"]},
                {"surface": "taskbar", "state": "rest", "profile": "fhd-150", "needs": ["shot"]}]
        shots = self.SHOTS + [{"surface": "taskbar", "state": "rest", "profile": "fhd-150", "file": "taskbar/rest.fhd-150.png"}]
        whole = self.run_campaign(rows=rows, shots=shots)
        self.assertEqual((whole.returncode, whole.stdout), (0, ""), whole.stderr)
        no_surface = self.run_campaign(rows=rows, shots=shots, records=[rec("win11.start.a", surface="start")])
        self.assertEqual(no_surface.stdout.splitlines(), ["no records: taskbar/rest/fhd-100"])
        no_state = self.run_campaign(rows=rows, shots=self.SHOTS)
        self.assertEqual(no_state.stdout.splitlines(), ["no shot: taskbar/rest/fhd-150"])

    def test_a_row_waits_for_its_fixture_script_and_variant(self):
        rows = [{"surface": "taskbar", "state": "rest", "profile": "fhd-100", "needs": ["records"], "fixture": f}
                for f in ("lists:populated", "lists:huge", "services:update-failed")]
        rows = [dict(r, state=f"rest{i}") for i, r in enumerate(rows)]
        out = self.run_campaign(rows=rows, fixtures={"lists": "# Variants: empty, populated\n"})
        self.assertEqual(out.stdout.splitlines(), [
            "no fixture: taskbar/rest1/fhd-100 (fixture 'lists:huge': lists.ps1 has no variant 'huge' (it has: empty, populated))",
            "no fixture: taskbar/rest2/fhd-100 (fixture 'services:update-failed': no script services.ps1 in scripts/inventory/fixtures/)"])

    def test_absent_reason_must_be_text(self):
        for reason in (None, "   ", 7, ["x"]):
            row = {"surface": "battery", "state": "low", "profile": "fhd-100", "needs": ["records"], "source": "absent",
                   "reason": reason}
            out = self.run_campaign(rows=[row])
            self.assertEqual(out.stdout.splitlines(), ["absent without a reason: battery/low/fhd-100"], reason)

    def test_absent_row_names_a_captured_sibling(self):
        base = {"surface": "battery", "state": "low", "profile": "fhd-100", "needs": ["records"], "source": "absent",
                "reason": "no stand-in"}
        other_absent = dict(base, state="face", sibling="taskbar/rest")
        for sibling in (None, "", "taskbar/nowhere", "battery/face"):
            out = self.run_campaign(rows=[dict(base, sibling=sibling), other_absent])
            self.assertEqual(out.stdout.splitlines(), [
                "absent without a sibling: battery/low/fhd-100 (sibling names the surface/state of a captured row)",
                "absent without a sibling: battery/face/fhd-100 (sibling names the surface/state of a captured row)"],
                sibling)

    def test_a_stand_in_row_needs_a_proven_stand_in(self):
        row = {"surface": "taskbar", "state": "rest", "profile": "fhd-100", "needs": ["records", "shot"], "source": "stand-in"}
        good = {"variant": "charging", "proof": {"file": "stand-ins/battery.png", "sha256": "a" * 64}}
        variants = {"charging": {}}
        cases = [
            (None, {"battery": good}, "the row names no standin"),
            ("lid", {"battery": good}, "lid is not in docs/design/inventory/stand-ins.json"),
            ("battery", {"battery": dict(good, variant="flat")},
             "battery: variant 'flat' is not in scripts/inventory/standin/variants.json"),
            ("battery", {"battery": {"variant": "charging"}}, "battery has no proof screenshot (standin/prove.py records one)"),
            ("battery", {"battery": dict(good, proof={"file": "x.png", "sha256": "not-a-hash"})},
             "battery has no proof screenshot (standin/prove.py records one)"),
        ]
        for sid, stand_ins, why in cases:
            out = self.run_campaign(rows=[dict(row, standin=sid)], stand_ins=stand_ins, variants=variants)
            self.assertEqual(out.stdout.splitlines(), [f"unproven stand-in: taskbar/rest/fhd-100 ({why})"], sid)
        proven = self.run_campaign(rows=[dict(row, standin="battery")], stand_ins={"battery": good}, variants=variants)
        self.assertEqual((proven.returncode, proven.stdout), (0, ""), proven.stderr)

    def test_unlabelled_capture_is_named(self):
        rows = [{"surface": "taskbar", "state": "rest", "profile": "fhd-100", "needs": ["records", "shot"]}]
        unnamed = [{"id": "win11.taskbar.a", "uiaId": "", "className": "Btn", "controlType": "Button", "why": "no name"},
                   {"id": "win11.taskbar.b", "uiaId": "", "className": "Btn", "controlType": "Button", "why": "no name"}]
        records = [rec("win11.taskbar.a", label="Btn"), rec("win11.taskbar.b", label="Widgets")]
        out = self.run_campaign(rows=rows, unnamed=unnamed, records=records)
        self.assertEqual(out.stdout.splitlines(), ["no label: win11.taskbar.a"])


class Index(unittest.TestCase):
    """--index runs the report the index holds, on the tree the index holds."""

    STUB = "import sys\nprint('staged', ' '.join(sys.argv[1:]))\n"

    def setUp(self):
        self.dir = pathlib.Path(tempfile.mkdtemp(prefix="inventory-report-index-test-"))
        self.addCleanup(shutil.rmtree, self.dir, True)
        stub = self.dir / "scripts" / "inventory" / "inventory-report.py"
        stub.parent.mkdir(parents=True)
        stub.write_text(self.STUB, encoding="utf-8")
        git = ["git", "-C", str(self.dir)]
        subprocess.run(git + ["init", "-q"], check=True, timeout=TIMEOUT)
        subprocess.run(git + ["add", "scripts"], check=True, timeout=TIMEOUT)
        self.stub = stub

    def index_check(self):
        return subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.dir), "--index", "--check"],
                              capture_output=True, text=True, timeout=TIMEOUT)

    def test_staged_copy_runs_on_the_snapshot(self):
        self.stub.write_text("print('working tree')\n", encoding="utf-8")
        out = self.index_check()
        self.assertEqual(out.returncode, 0, out.stderr)
        words = out.stdout.split()
        self.assertEqual((words[0], words[1], words[3]), ("staged", "--root", "--check"))
        self.assertNotEqual(words[2], str(self.dir))

    def test_unstaged_deletion_still_runs_the_staged_copy(self):
        self.stub.unlink()
        out = self.index_check()
        self.assertTrue(out.stdout.startswith("staged "), out.stdout + out.stderr)


class Implemented(unittest.TestCase):
    """A section done in the plan counts only with both shipping stamps."""

    def test_deferred_done_section_is_not_implemented(self):
        spec = importlib.util.spec_from_file_location("inventory_report", SCRIPT)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        sections = {"a": types.SimpleNamespace(path="todo/x.md", n=1, box="x"),
                    "b": types.SimpleNamespace(path="todo/x.md", n=2, box="x"),
                    "c": types.SimpleNamespace(path="todo/x.md", n=3, box=" ")}
        plan = types.SimpleNamespace(load_sections=lambda root: sections, PlanError=Exception)
        triage = types.SimpleNamespace(section_stamps=lambda path: {1: {"V", "Q"}, 2: {"D"}, 3: {"V", "Q"}})
        mod._module = lambda name, path: plan if name == "todo_plan" else triage
        self.assertEqual(mod.implemented_sections(pathlib.Path("/nonexistent")), {("todo/x.md", 1)})


if __name__ == "__main__":
    unittest.main(verbosity=1)
