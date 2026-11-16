#!/usr/bin/env python3
"""Tests for the UI Automation capture (capture_core.py, capture-ui.py, capture.py),
on Linux with no Windows present: the walk runs over a recorded UIA tree.

    python3 scripts/inventory/tests/test_capture.py

fixtures/notepad-tree.json is Notepad on the reference VM, dumped by
capture-ui.py --dump on 2026-11-14 (build 26300.9711, fhd-100), structure only,
before the dump shared the walk's exclusions, so it still holds offscreen and
tooltip subtrees the walk must skip;
its one teaching tip's text was replaced with our own words (section 4).
fixtures/notepad-records.json is what the live capture wrote in the same run,
byte-identical across three VM sessions, so the walk over the fixture must give
exactly it.
"""
import contextlib
import copy
import importlib.util
import io
import json
import subprocess
import pathlib
import time
import signal
import os
import re
import shutil
import struct
import sys
import tempfile
import types
from unittest import mock
import unittest
import zlib

HERE = pathlib.Path(__file__).resolve().parent
INV = HERE.parent
FIXTURE = HERE / "fixtures" / "notepad-tree.json"
EXPECTED = HERE / "fixtures" / "notepad-records.json"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


core = load("capture_core", INV / "capture_core.py")
capture = load("capture_host", INV / "capture.py")
guest = load("capture_ui", INV / "capture-ui.py")
smoke = load("surface_notepad_smoke", INV / "surfaces" / "notepad-smoke.py")
PROFILES = json.loads((INV.parents[1] / "docs" / "design" / "inventory" / "profiles.json").read_text())
SEEN_OK = dict(PROFILES["fhd-100"], build="26300.9711")


def tree():
    return json.loads(FIXTURE.read_text(encoding="utf-8"))


def walk(t=None, **kw):
    args = dict(surface="notepad-smoke", build="26300.9711", profile="fhd-100", session="console", root_slug=smoke.ROOT_SLUG,
                root_label=smoke.ROOT_LABEL, environment_lists=smoke.ENVIRONMENT_LISTS, values=smoke.VALUES)
    args.update(kw)
    return core.walk(core.FixtureElement(t or tree()), **args)


def flat(t):
    yield t
    for k in t.get("children", []) + t.get("expands", []):
        yield from flat(k)


def node(name="", ctype="Button", aid="", cls="", bounds=(0, 0, 10, 10), **extra):
    return dict(name=name, controlType=ctype, automationId=aid, className=cls, bounds=list(bounds), **extra)


class Strict:
    """An element that fails on any read the walk is not allowed to make."""
    ALLOWED = {"name", "automation_id", "class_name", "control_type", "bounds", "offscreen", "accelerator",
               "enabled", "children", "can_expand", "expand", "collapse"}

    def __init__(self, inner):
        object.__setattr__(self, "_inner", inner)

    def __getattr__(self, attr):
        if attr not in Strict.ALLOWED:
            raise AssertionError(f"the walk read {attr}")
        value = getattr(self._inner, attr)
        if attr in ("children", "expand"):
            return lambda **kw: [Strict(k) for k in value(**kw)]
        return value


class Walk(unittest.TestCase):
    def test_the_fixture_walk_is_exactly_the_live_capture(self):
        self.assertEqual(walk().records, core.read_records(EXPECTED))

    def test_notepad_meets_the_checkpoint_and_the_checker(self):
        cap = walk()
        self.assertGreaterEqual(len(cap.records), 40)
        code, report = capture.check_scratch({"notepad-smoke/window.json": cap.records})
        self.assertEqual(code, 0, report)

    def test_ids_are_unique_and_follow_what_a_user_sees(self):
        recs = walk().records
        ids = [r["id"] for r in recs]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertIn("win11.notepad-smoke.window.menubar.file.save-as", ids)
        self.assertFalse([i for i in ids if "desktopchildsitebridge" in i or "inputsitewindowclass" in i])

    def test_hosting_panes_are_spliced_not_recorded(self):
        cap = walk()
        self.assertEqual(cap.skipped["hosting"], 7)
        by_id = {r["id"]: r for r in cap.records}
        self.assertEqual(by_id["win11.notepad-smoke.window.menubar"]["parent"], "win11.notepad-smoke.window")

    def test_root_is_the_surface_with_its_own_label_and_origin(self):
        root = walk().records[0]
        self.assertEqual((root["kind"], root["label"], root["order"]), ("surface", "Notepad", 0))
        self.assertEqual((root["bounds"]["x"], root["bounds"]["y"]), (0, 0))
        self.assertNotIn("parent", root)

    def test_bounds_are_relative_to_the_surface(self):
        t = tree()
        x0, y0 = t["bounds"][:2]
        recs = {r["id"]: r for r in walk(t).records}
        menubar = recs["win11.notepad-smoke.window.menubar"]["bounds"]
        raw = next(k for k in flat(t) if k.get("automationId") == "MenuBar")["bounds"]
        self.assertEqual((menubar["x"], menubar["y"]), (raw[0] - x0, raw[1] - y0))


    def test_one_level_of_menus_with_item_counts(self):
        recs = {r["id"]: r for r in walk().records}
        file_menu = recs["win11.notepad-smoke.window.menubar.file"]
        self.assertEqual(file_menu["itemCount"], 13)
        kids = [r for r in recs.values() if r.get("parent") == file_menu["id"]]
        self.assertEqual(len(kids), 13)
        self.assertTrue(all(k["kind"] == "menuItem" for k in kids))
        self.assertEqual(sorted(k["order"] for k in kids), list(range(13)))

    def test_shortcuts_are_recorded(self):
        recs = {r["id"]: r for r in walk().records}
        self.assertEqual(recs["win11.notepad-smoke.window.menubar.file.new-tab"]["shortcut"], "Ctrl+N")
        self.assertEqual(recs["win11.notepad-smoke.window.menubar.edit.find-previous"]["shortcut"], "Shift+F3")
        self.assertNotIn("shortcut", recs["win11.notepad-smoke.window.menubar.file.exit"])

    def test_every_expanded_opener_is_closed_again(self):
        opened = []
        real = core.FixtureElement.expand

        def spy(self, **kw):
            opened.append(self)
            return real(self, **kw)
        core.FixtureElement.expand = spy
        try:
            walk()
        finally:
            core.FixtureElement.expand = real
        self.assertGreater(len(opened), 0)
        self.assertFalse([e.name for e in opened if e.open])

    def test_labels_are_one_line_and_never_sentences(self):
        for r in walk().records:
            self.assertNotIn("\n", r["label"], r["id"])
            self.assertLessEqual(len(r["label"]), core.LABEL_MAX, r["id"])


class NeverData(unittest.TestCase):
    def test_the_walk_reads_structure_only(self):
        walk(core_root := tree())
        cap = core.walk(Strict(core.FixtureElement(core_root)), surface="notepad-smoke", build="26300.9711",
                        profile="fhd-100", session="console", root_slug="window", root_label="Notepad",
                        environment_lists=smoke.ENVIRONMENT_LISTS, values=smoke.VALUES)
        self.assertGreaterEqual(len(cap.records), 40)

    def test_environment_list_is_one_record_with_no_rows(self):
        recs = walk().records
        tabs = [r for r in recs if r["id"] == "win11.notepad-smoke.window.tabs"]
        self.assertEqual(len(tabs), 1)
        self.assertEqual(tabs[0]["controlType"], "list")
        self.assertFalse([r for r in recs if r.get("parent") == tabs[0]["id"]])
        self.assertFalse([r for r in recs if "Untitled" in r["label"]])

    def test_an_environment_list_in_a_popup_keeps_its_rows_out(self):
        devices = dict(node("Devices", "List"), popup=True, children=[node("Jo's USB stick", "ListItem")])
        t = node("Root", "Window", children=[node("Output", "ComboBox", expands=[devices])])
        args = dict(surface="s", build="1.2", profile="p", session="console", root_slug="r", root_label="R")
        cap = core.walk(core.FixtureElement(t), environment_lists=[{"controlType": "List"}], **args)
        self.assertNotIn("usb", json.dumps(cap.records).lower())
        box = [r for r in cap.records if r.get("label") == "Devices"]
        self.assertEqual((len(box), box[0]["controlType"]), (1, "list"))
        d = core.dump(core.FixtureElement(t), environment_lists=[{"controlType": "List"}])
        self.assertNotIn("usb", json.dumps(d).lower())
        core.confirm_dump(d, cap, environment_lists=[{"controlType": "List"}], **args)
        plain = core.walk(core.FixtureElement(t), **args)            # not marked: its rows are entries
        self.assertIn("Jo's USB stick", [r.get("label") for r in plain.records])

    def test_an_unnamed_environment_pane_is_never_opened(self):
        rows = dict(node("", "Pane", cls="DeviceRows"), children=[node("Jo's USB stick", "ListItem")])
        t = node("Root", "Window", children=[rows])
        args = dict(surface="s", build="1.2", profile="p", session="console", root_slug="r", root_label="R",
                    environment_lists=[{"className": "DeviceRows"}])
        read = []
        real = core.FixtureElement.children

        def spy(self, **kw):
            read.append(self.class_name)
            return real(self, **kw)
        core.FixtureElement.children = spy
        try:
            cap = core.walk(core.FixtureElement(t), **args)
        finally:
            core.FixtureElement.children = real
        self.assertNotIn("DeviceRows", read)
        self.assertNotIn("usb", json.dumps(cap.records).lower())
        core.confirm_dump(core.dump(core.FixtureElement(t), environment_lists=args["environment_lists"]), cap, **args)

    def test_live_values_are_withheld(self):
        cap = walk()
        labels = {r["label"] for r in cap.records}
        for value in ("0 characters", "Line 1, Column 1", "UTF-8", "Windows (CRLF)"):
            self.assertNotIn(value, labels)
        withheld = [u for u in cap.unnamed if u["why"] == "name is a live value"]
        self.assertEqual(len(withheld), 6)

    def test_an_unnamed_teaching_tip_pane_is_pruned_whole(self):
        tip = node("", "Pane", cls="Microsoft.UI.Xaml.Controls.TeachingTip", children=[node("Try it", "Text")])
        t = node("Root", "Window", children=[tip, node("OK", "Button")])
        cap = core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", session="console", root_slug="r", root_label="R")
        self.assertEqual([r["label"] for r in cap.records[1:]], ["OK"])
        d = core.dump(core.FixtureElement(t))
        core.confirm_dump(d, cap, surface="s", build="1.2", profile="p", session="console", root_slug="r", root_label="R")

    def test_a_wide_element_is_refused_before_its_children_are_made(self):
        made = []
        real = core.FixtureElement.__init__

        def count(self, d):
            made.append(1)
            real(self, d)
        wide = node("Root", "Window", children=[node("", "ToolTip") for _ in range(core.MAX_VISITS + 1)])
        core.FixtureElement.__init__ = count
        try:
            with self.assertRaises(core.Incomplete):
                core.walk(core.FixtureElement(wide), surface="s", build="1.2", profile="p", session="console", root_slug="r",
                          root_label="R")
        finally:
            core.FixtureElement.__init__ = real
        self.assertEqual(len(made), 1)                # the root only

    def test_reading_is_bounded_even_when_nothing_is_recorded(self):
        panes = [node("", "Pane") for _ in range(core.MAX_VISITS + 1)]
        t = node("Root", "Window", children=[node("", "Pane", children=panes)])
        with self.assertRaises(core.Incomplete):
            core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", session="console", root_slug="r", root_label="R")

    def test_tooltips_teaching_tips_and_prose_are_skipped(self):
        t = node("Root", "Window", bounds=(0, 0, 500, 500), children=[
            node("Help", "ToolTip"), node("", "Pane", "Tip", "Microsoft.UI.Xaml.Controls.TeachingTip"),
            node("x" * 81, "Text"), node("OK", "Button"), node("Hidden", "Button", offscreen=True)])
        cap = core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="fhd-100", session="console", root_slug="r",
                        root_label="Root")
        self.assertEqual([r["label"] for r in cap.records[1:]], ["OK"])
        self.assertEqual(cap.skipped, {"offscreen": 1, "tooltip": 2, "prose": 1, "hosting": 0})
        self.assertFalse([k for k in ("Help", "x" * 81) if k in json.dumps(cap.records)])

    def test_the_fixture_holds_no_help_text(self):
        names = [k.get("name", "") for k in flat(tree())]
        self.assertFalse([n for n in names if n.startswith("Local AI")])


class Popups(unittest.TestCase):
    def test_only_the_outermost_containers_are_read(self):
        # host > devices (marked list) > inner list; a separate menu beside it.
        parents = {"inner": "devices", "devices": "host", "menu": "host", "host": None}
        found = {"devices": "devices", "inner": "inner", "menu": "menu"}

        def parent_of(el):
            up = parents[el]
            return (up, up) if up else None
        self.assertEqual(core.outermost(found, parent_of), ["devices", "menu"])

    def test_an_unsettled_ancestry_fails_the_capture(self):
        with self.assertRaises(core.Incomplete):                 # a loop
            core.outermost({"a": "a"}, lambda el: ("z", "z"))
        chain = iter(range(10 ** 6))
        with self.assertRaises(core.Incomplete):                 # deeper than MAX_DEPTH
            core.outermost({"a": "a"}, lambda el: (next(chain), "x"))

        for parents in ({"bad": "bad", "good": None}, {"bad": "other", "other": "bad", "good": None}):
            found = {k: k for k in parents}
            with self.assertRaises(core.Incomplete):         # a loop through found containers
                core.outermost(found, lambda el, p=parents: (p[el], p[el]) if p[el] else None)

        def broken(el):
            raise core.Incomplete("parent unreadable")
        with self.assertRaises(core.Incomplete):                 # a read that fails
            core.outermost({"a": "a"}, broken)


class LiveHosts(unittest.TestCase):
    """guest.Live.hosts over a stub provider (no Windows): a top-level window
    that closes mid-read is skipped, and the rest are still searched."""

    def test_a_window_that_closes_mid_read_is_skipped(self):
        class Gone(Exception):
            pass

        def top(name, cls):
            return types.SimpleNamespace(el=name, offscreen=False, class_name=cls, control_type="Window")
        live = object.__new__(guest.Live)
        live.COMError = Gone
        live.uia = types.SimpleNamespace(GetRootElement=lambda: "root", CreatePropertyCondition=lambda *a: None)
        kids = {"root": [top("tip", "TooltipWindow"), top("main", "Notepad")],
                "main": [types.SimpleNamespace(el="host", class_name="PopupHost", offscreen=False)]}

        def children(el, condition=None, limit=None):
            if el == "tip":
                raise Gone("element not available")
            return kids[el]
        live.children = children
        self.assertEqual(live.hosts(1234), ["host"])


class LiveExpand(unittest.TestCase):
    """guest.LiveElement.expand over a stub provider: settling holds out for the
    whole popup set, re-discovered on every poll."""

    def entry(self, name):
        return types.SimpleNamespace(control_type="MenuItem", name=name, automation_id="", bounds=(0, 0, 1, 1))

    def element(self, discoveries):
        """discoveries: one (popups, complete) per call of popups(), the last repeated."""
        seq = list(discoveries)
        live = types.SimpleNamespace(discovery_complete=True)

        def popups(pid):
            found, live.discovery_complete = seq.pop(0) if len(seq) > 1 else seq[0]
            return dict(found)
        live.popups = popups
        live.wrap = lambda e: types.SimpleNamespace(control_type="Menu", name=e, automation_id="", class_name="")
        live.children = lambda e, **kw: [self.entry(f"{e}-item")]
        el = object.__new__(guest.LiveElement)
        el.live, el.pid, el.name, el.automation_id, el._opened = live, 1, "File", "", {}
        el._pattern = lambda: types.SimpleNamespace(Expand=lambda: None)
        el.children = lambda **kw: []
        return el

    def setUp(self):
        self.sleep = guest.time.sleep
        guest.time.sleep = lambda s: None

    def tearDown(self):
        guest.time.sleep = self.sleep

    def test_a_host_found_late_is_waited_for(self):
        a, ab = {("a",): "A"}, {("a",): "A", ("b",): "B"}
        el = self.element([({}, True), (a, True), (a, True), (ab, True), (ab, True), (ab, True)])
        names = [i.name for i in el.expand()]
        self.assertEqual(names, ["A-item", "B-item"])

    def test_a_failed_host_read_never_counts_as_settled(self):
        a, ab = {("a",): "A"}, {("a",): "A", ("b",): "B"}
        el = self.element([({}, True), (a, False), (a, False), (a, False), (ab, True), (ab, True), (ab, True)])
        self.assertEqual([i.name for i in el.expand()], ["A-item", "B-item"])

    def test_a_host_that_always_fails_ends_incomplete(self):
        real = guest.SETTLE_S
        guest.SETTLE_S = 0.05
        try:
            el = self.element([({}, True), ({("a",): "A"}, False)])
            with self.assertRaises(guest.core.Incomplete):
                el.expand()
        finally:
            guest.SETTLE_S = real


class Unnamed(unittest.TestCase):
    def test_unnamed_elements_fall_back_to_their_id_or_class_and_are_listed(self):
        cap = walk()
        recs = {r["id"]: r for r in cap.records}
        listed = {u["id"]: u for u in cap.unnamed}
        self.assertEqual(recs["win11.notepad-smoke.window.menubar"]["label"], "MenuBar")
        self.assertEqual(listed["win11.notepad-smoke.window.menubar"]["why"], "no name")
        self.assertNotIn("win11.notepad-smoke.window", listed)        # the root's label is the module's

    def test_number_only_automation_ids_do_not_name_an_id(self):
        t = node("Root", "Window", children=[node("Open", "MenuItem", "1001")])
        cap = core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", session="console", root_slug="r", root_label="R")
        self.assertEqual(cap.records[1]["id"], "win11.s.r.open")


class Identity(unittest.TestCase):
    def twins(self, *names):
        return node("Root", "Window", children=[node(n, "Button", "Twin") if n else node("", "Button", cls="Btn")
                                                  for n in names])

    def run_walk(self, t, identity=()):
        return core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", session="console", root_slug="r",
                         root_label="R", identity=identity)

    def test_siblings_sharing_a_segment_are_ambiguous(self):
        cap = self.run_walk(self.twins("", ""))
        self.assertEqual(cap.ambiguous, ["win11.s.r.btn--1", "win11.s.r.btn--2"])
        self.assertEqual([r["id"] for r in cap.records[1:]], cap.ambiguous)

    def test_no_name_can_make_a_positional_id(self):
        t = node("Root", "Window", children=[node("", "Button", "Twin"), node("", "Button", "Twin"),
                                             node("", "Button", "Twin--1"), node("Twin - 1", "Button")])
        # Names that spell "--" fold to one hyphen and so form their own pair.
        ids = [r["id"] for r in self.run_walk(t).records[1:]]
        self.assertEqual(ids, ["win11.s.r.twin--1", "win11.s.r.twin--2", "win11.s.r.twin-1--1", "win11.s.r.twin-1--2"])

    def test_identity_segments_must_be_slugs(self):
        t = node("Root", "Window", children=[node("A", "Button")])
        with self.assertRaises(ValueError):
            self.run_walk(t, identity=[({"name": "A"}, "first--1")])

    def test_a_sentence_never_names_an_id(self):
        prose = "Use this button to open the document you were working on most recently, in a new tab"
        t = node("Root", "Window", children=[node(prose, "Button", children=[node("OK", "Button")])])
        cap = self.run_walk(t)
        self.assertNotIn("recently", json.dumps(cap.records))

    def test_a_live_value_never_names_an_id(self):
        # A Text with no automation id is named by its name; when that name is a
        # live value the id would publish it and move with it.
        t = node("Root", "Window", children=[node("Private document.txt", "Text")])
        cap = core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", session="console", root_slug="r",
                        root_label="R", values=[{"controlType": "Text"}])
        self.assertNotIn("private", json.dumps(cap.records).lower())
        self.assertEqual(cap.records[1]["id"], "win11.s.r.text")

    def test_identity_names_an_element_its_tree_does_not(self):
        t = node("Root", "Window", children=[node("A", "Button", "Twin"), node("B", "Button", "Twin")])
        cap = self.run_walk(t, identity=[({"name": "A"}, "first"), ({"name": "B"}, "second")])
        self.assertEqual([r["id"] for r in cap.records[1:]], ["win11.s.r.first", "win11.s.r.second"])
        self.assertEqual(cap.ambiguous, [])


class Incomplete(unittest.TestCase):
    def test_a_flyout_that_never_settles_fails_the_capture(self):
        t = tree()
        t["children"].append(node("Broken", "MenuItem", expandFails=True))
        with self.assertRaises(core.Incomplete):
            walk(t)

    def test_an_incomplete_capture_publishes_and_removes_nothing(self):
        tmp = pathlib.Path(tempfile.mkdtemp())
        try:
            target = tmp / "window.json"
            core.write_records(target, [{"id": "win11.x.y", "behaviour": "ours"}])
            before = target.read_bytes()
            with contextlib.redirect_stdout(io.StringIO()) as out, self.assertRaises(SystemExit) as ex:
                capture.build_file(target, [], {"complete": False, "incomplete": "the flyout never settled"}, True)
            self.assertEqual(ex.exception.code, 3)
            self.assertIn("nothing published, nothing removed", out.getvalue())
            self.assertEqual(target.read_bytes(), before)
        finally:
            shutil.rmtree(tmp)

    def test_too_deep_or_too_many_fails(self):
        deep = node("Leaf", "Button")
        for _ in range(core.MAX_DEPTH + 2):
            deep = node("G", "Group", children=[deep])
        with self.assertRaises(core.Incomplete):
            core.walk(core.FixtureElement(deep), surface="s", build="1.2", profile="p", session="console", root_slug="r", root_label="R")


class Merge(unittest.TestCase):
    OLD = [{"id": "a.b", "label": "Old", "behaviour": "Opens the file.", "disposition": "ship"},
           {"id": "a.gone", "label": "Gone", "behaviour": "Was here."},
           {"id": "a.plain", "label": "Plain"}]

    def test_hand_fields_are_carried_and_facts_refreshed(self):
        new = [{"id": "a.b", "label": "New", "order": 3}]
        out, dropped = core.merge(self.OLD, new, [])
        self.assertEqual(out, [{"id": "a.b", "label": "New", "order": 3, "behaviour": "Opens the file.",
                                "disposition": "ship"}])
        self.assertEqual(dropped, ["a.gone"])

    def test_the_baseline_match_survives_a_recapture(self):
        old = [{"id": "a.b", "label": "B", "baselineId": "settings:S1"}, {"id": "a.c", "label": "C", "baselineMissing": True},
               {"id": "a.gone", "label": "Gone", "baselineId": "settings:S2"}]
        out, dropped = core.merge(old, [{"id": "a.b", "label": "B2"}, {"id": "a.c", "label": "C2"}], [])
        self.assertEqual(out, [{"id": "a.b", "label": "B2", "baselineId": "settings:S1"},
                               {"id": "a.c", "label": "C2", "baselineMissing": True}])
        self.assertEqual(dropped, ["a.gone"])       # a lost match is named, never silent
        with self.assertRaises(ValueError):
            core.merge([{"id": "s.btn--2", "label": "Btn", "baselineId": "settings:S1"}],
                       [{"id": "s.btn--2", "label": "Btn"}], ["s.btn--2"])

    def test_a_match_to_a_row_the_build_dropped_is_unbound(self):
        recs = [{"id": "a.b", "baselineId": "settings:S1"}, {"id": "a.c", "baselineId": "settings:Gone", "behaviour": "x"},
                {"id": "a.d", "baselineMissing": True}, {"id": "a.e"}]
        out, unbound = core.unbind(recs, {"settings": {"S1"}})
        self.assertEqual(out, [{"id": "a.b", "baselineId": "settings:S1"}, {"id": "a.c", "behaviour": "x"},
                               {"id": "a.d", "baselineMissing": True}, {"id": "a.e"}])
        self.assertEqual(unbound, ["a.c"])
        # A baseline that is missing is not a build that dropped its rows: the match stays for the checker to refuse.
        out, unbound = core.unbind(recs, {"controls": {"Button"}})
        self.assertEqual((out, unbound), (recs, []))

    def test_a_missing_baseline_file_removes_no_match(self):
        with tempfile.TemporaryDirectory() as d, mock.patch.object(capture, "INVENTORY", pathlib.Path(d)):
            inv = pathlib.Path(d)
            (inv / "controls").mkdir()
            (inv / "controls" / "baseline.json").write_text('{"baseline": "controls"}\n{"id": "Button"}\n')
            (inv / "taskbar").mkdir()
            old = [{"id": "a.b", "label": "B", "baselineId": "settings:S1"}, {"id": "a.c", "label": "C", "baselineId": "controls:Gone"}]
            core.write_records(inv / "taskbar" / "bar.json", old)
            out = capture.build_file(inv / "taskbar" / "bar.json", [{"id": "a.b", "label": "B"}, {"id": "a.c", "label": "C"}],
                                     {"complete": True}, True)
        self.assertEqual(out, [{"id": "a.b", "label": "B", "baselineId": "settings:S1"}, {"id": "a.c", "label": "C"}])

    def test_a_note_never_lands_on_an_ambiguous_id(self):
        # An inserted twin shifts the positional suffix: the old note on btn-2 would
        # move to a different control, so the merge refuses.
        old = [{"id": "s.btn", "label": "Btn"}, {"id": "s.btn-2", "label": "Btn", "behaviour": "Deletes."}]
        new = [{"id": "s.btn", "label": "Btn"}, {"id": "s.btn-2", "label": "Btn"}, {"id": "s.btn-3", "label": "Btn"}]
        with self.assertRaises(ValueError) as ex:
            core.merge(old, new, ["s.btn", "s.btn-2", "s.btn-3"])
        self.assertIn("s.btn-2", str(ex.exception))
        out, _ = core.merge([{"id": "s.btn", "label": "Btn"}], new, ["s.btn", "s.btn-2", "s.btn-3"])
        self.assertEqual(len(out), 3)            # unannotated twins merge freely

    def test_a_note_never_lands_beneath_an_ambiguous_id(self):
        # Two look-alike groups reorder: each group's apply button swaps ids with
        # the other's, so a note on twin.apply would move to the other group.
        t = node("Root", "Window", children=[node("", "Group", "Twin", children=[node("Apply", "Button")]),
                                             node("", "Group", "Twin", children=[node("Apply", "Button")])])
        cap = core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", session="console", root_slug="r", root_label="R")
        old = [dict(r, behaviour="Saves.") if r["id"] == "win11.s.r.twin--1.apply" else r for r in cap.records]
        with self.assertRaises(ValueError) as ex:
            core.merge(old, cap.records, cap.ambiguous)
        self.assertIn("win11.s.r.twin--1.apply", str(ex.exception))

    def test_a_group_that_shrinks_to_one_drops_its_note(self):
        # Delete and Save share an automation id; Delete carries a note. Delete
        # is removed: Save must not inherit "Deletes the document."
        def cap_of(*names):
            t = node("Root", "Window", children=[node(n, "Button", "Twin") for n in names])
            return core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", session="console", root_slug="r",
                             root_label="R")
        before = cap_of("Delete", "Save")
        old = [dict(r, behaviour="Deletes the document.") if r.get("label") == "Delete" else r for r in before.records]
        after = cap_of("Save")
        out, dropped = core.merge(old, after.records, after.ambiguous)
        self.assertNotIn("Deletes the document.", json.dumps(out))
        self.assertEqual(dropped, ["win11.s.r.twin--1"])

    def test_a_positional_note_never_passes_to_a_real_name(self):
        # Two look-alikes, the first annotated; next capture holds one control
        # genuinely named Twin-1, with a child. Neither may inherit the note.
        t = node("Root", "Window", children=[node("", "Group", "Twin", children=[node("Apply", "Button")]),
                                             node("", "Group", "Twin")])
        before = core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", session="console", root_slug="r", root_label="R")
        old = [dict(r, behaviour="Saves.") if r["id"].startswith("win11.s.r.twin--1") else r for r in before.records]
        t2 = node("Root", "Window", children=[node("", "Group", "Twin-1", children=[node("Apply", "Button")])])
        after = core.walk(core.FixtureElement(t2), surface="s", build="1.2", profile="p", session="console", root_slug="r", root_label="R")
        out, dropped = core.merge(old, after.records, after.ambiguous)
        self.assertNotIn("Saves.", json.dumps(out))
        self.assertEqual(sorted(dropped), ["win11.s.r.twin--1", "win11.s.r.twin--1.apply"])

    def test_an_existing_file_needs_merge(self):
        tmp = pathlib.Path(tempfile.mkdtemp())
        try:
            target = tmp / "window.json"
            core.write_records(target, self.OLD)
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as ex:
                capture.build_file(target, [{"id": "a.b"}], {"complete": True}, False)
            self.assertEqual(ex.exception.code, 2)
            with contextlib.redirect_stdout(io.StringIO()) as out:
                recs = capture.build_file(target, [{"id": "a.b"}], {"complete": True}, True)
            self.assertEqual(recs[0]["behaviour"], "Opens the file.")
            self.assertIn("dropped a.gone", out.getvalue())
        finally:
            shutil.rmtree(tmp)

    def test_records_round_trip(self):
        tmp = pathlib.Path(tempfile.mkdtemp())
        try:
            recs = walk().records
            core.write_records(tmp / "r.json", recs)
            self.assertEqual(core.read_records(tmp / "r.json"), recs)
        finally:
            shutil.rmtree(tmp)


class Profile(unittest.TestCase):
    def test_125_percent_is_refused_with_the_checkpoint_message(self):
        seen = dict(SEEN_OK, dpi=120)
        self.assertEqual(core.compare_profile(seen, PROFILES["fhd-100"], "fhd-100"),
                         ["profile fhd-100: scaling is 125, expected 100"])

    def test_every_other_value_is_named(self):
        seen = dict(SEEN_OK, width=1280, accent="FF000000")
        self.assertEqual(core.compare_profile(seen, PROFILES["fhd-100"], "fhd-100"),
                         ["profile fhd-100: width is 1280, expected 1920",
                          "profile fhd-100: accent is FF000000, expected FFD47800"])
        self.assertEqual(core.compare_profile(SEEN_OK, PROFILES["fhd-100"], "fhd-100"), [])

    def test_the_tool_exits_2_before_opening_anything(self):
        real, real_apply = guest.read_profile, guest.apply_profile
        guest.read_profile = lambda: dict(SEEN_OK, dpi=120)
        guest.apply_profile = lambda name, profiles: []
        try:
            with contextlib.redirect_stdout(io.StringIO()) as out:
                code = guest.main(["--surface", "no-such-surface", "--out", "x.json",
                                   "--profiles", str(INV.parents[1] / "docs/design/inventory/profiles.json")])
        finally:
            guest.read_profile, guest.apply_profile = real, real_apply
        self.assertEqual(code, 2)
        self.assertEqual(out.getvalue().strip(), "profile fhd-100: scaling is 125, expected 100")
        self.assertIsNone(guest.user32)          # nothing was opened

    def test_a_slow_profile_application_is_a_named_failure(self):
        real = guest.subprocess.run
        def slow(*a, **kw):
            self.assertEqual(kw["timeout"], guest.APPLY_TIMEOUT_S)
            raise guest.subprocess.TimeoutExpired(a[0], kw["timeout"])
        guest.subprocess.run = slow
        try:
            self.assertEqual(guest.apply_profile("fhd-100-text150", "p.json"),
                             [f"profile fhd-100-text150: set-profile did not finish within {guest.APPLY_TIMEOUT_S} s"])
        finally:
            guest.subprocess.run = real

    def test_ref_vm_compares_every_profile_value(self):
        shell = shutil.which("pwsh") or shutil.which("powershell.exe")
        if not shell:
            self.skipTest("no PowerShell on this host")
        script = INV / "ref-vm.ps1"
        if shell.endswith(".exe"):
            script = subprocess.run(["wslpath", "-w", str(script)], capture_output=True, text=True, check=True).stdout.strip()
        ps = ("$a = [System.Management.Automation.Language.Parser]::ParseFile('%s', [ref]$null, [ref]$null); "
              "$f = $a.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Compare-RefProfile' }, $true) | Select-Object -First 1; "
              "Invoke-Expression $f.Extent.Text; "
              "$id = [pscustomobject]@{ edition = 'Professional'; displayVersion = '26H2'; build = '1.1' }; "
              "$want = [pscustomobject]@{ dpi = 144; textScale = 150; highContrast = 'Dusk' }; "
              "$seen = [pscustomobject]@{ edition = 'Professional'; displayVersion = '26H2'; build = '1.1'; dpi = 96; textScale = 100; highContrast = 'none'; foreground = 'Progman'; session = 2 }; "
              "Compare-RefProfile $seen $id $want @('Progman') | ForEach-Object { $_ }; 'ok-run'; "
              "$seen.dpi = 144; $seen.textScale = 150; $seen.highContrast = 'Dusk'; "
              "(@(Compare-RefProfile $seen $id $want @('Progman'))).Count") % script
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip()]
        self.assertEqual(lines, ["dpi is 96, want 144", "textScale is 100, want 150", "highContrast is none, want Dusk",
                                 "ok-run", "0"], out.stderr)

    def test_a_rebuild_removes_the_disks_the_vm_uses_and_only_those(self):
        # A VM moved with Move-VMStorage keeps its disks under "Virtual Hard
        # Disks"; the cleanup removed only disk.vhdx and left them (2026-11-16).
        # Hyper-V cmdlets are stubs; the disk files are real.
        shell = shutil.which("pwsh") or shutil.which("powershell.exe")
        if not shell:
            self.skipTest("no PowerShell on this host")
        script = INV / "ref-common.ps1"
        if shell.endswith(".exe"):
            script = subprocess.run(["wslpath", "-w", str(script)], capture_output=True, text=True, check=True).stdout.strip()
        ps = ("$a = [System.Management.Automation.Language.Parser]::ParseFile('%s', [ref]$null, [ref]$null); "
              "$f = $a.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Remove-RefVm' }, $true) | Select-Object -First 1; "
              "Invoke-Expression $f.Extent.Text; "
              "$root = Join-Path ([IO.Path]::GetTempPath()) ('ios-rm-' + [Guid]::NewGuid().ToString('N')); "
              "$vm = Join-Path $root 'vm'; $vhd = Join-Path $vm 'Virtual Hard Disks'; "
              "New-Item -ItemType Directory -Force $vhd, (Join-Path $vm 'Snapshots') | Out-Null; "
              "$base = Join-Path $vhd 'base.vhdx'; $cur = Join-Path $vhd 'cur.avhdx'; $snap = Join-Path $vhd 'snap.avhdx'; $data = Join-Path $root 'data.vhdx'; "
              "foreach ($p in $base, $cur, $snap, $data) { Set-Content -LiteralPath $p -Value x }; "
              "$parent = @{ $cur = $base; $snap = $base; $base = $null; $data = $null }; "
              "function Get-VM { [pscustomobject]@{ State = 'Off' } }; function Stop-VM { throw 'stopped an Off VM' }; "
              "function Get-VMHardDiskDrive { [pscustomobject]@{ Path = $cur }, [pscustomobject]@{ Path = $data } }; "
              "function Get-VMSnapshot { [pscustomobject]@{ HardDrives = @([pscustomobject]@{ Path = $snap }) } }; "
              "function Get-VHD([string]$Path) { [pscustomobject]@{ ParentPath = $parent[$Path] } }; "
              "$script:removed = 0; function Remove-VM { $script:removed++ }; "
              "Remove-RefVm 'ios-test' $vm; "
              "'vm=' + $script:removed; 'disks=' + (Test-Path $vhd); 'snapshots=' + (Test-Path (Join-Path $vm 'Snapshots')); 'data=' + (Test-Path $data); "
              "Remove-Item -LiteralPath $root -Recurse -Force") % script
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip()]
        self.assertEqual(lines, ["vm=1", "disks=False", "snapshots=False", "data=True"], out.stderr)

    def _powershell(self, path, windows=False):
        # windows: the test starts a Windows child and shares a Windows mutex
        # with it, so Linux pwsh will not do.
        shell = shutil.which("powershell.exe") if windows else (shutil.which("pwsh") or shutil.which("powershell.exe"))
        if not shell:
            self.skipTest("no PowerShell on this host")
        if shell.endswith(".exe"):
            path = subprocess.run(["wslpath", "-w", str(path)], capture_output=True, text=True, check=True).stdout.strip()
        return shell, str(path)

    def test_update_iso_reuses_only_a_finished_servicing(self):
        # A packaging failure after the parts came out redid the whole
        # servicing on the retry (review 2026-11-16). Real files, no VM.
        shell, script = self._powershell(INV / "update-iso.ps1")
        ps = ("$a = [System.Management.Automation.Language.Parser]::ParseFile('%s', [ref]$null, [ref]$null); "
              "foreach ($n in 'Test-UpdateParts', 'Write-UpdateParts') { Invoke-Expression ($a.FindAll({ param($x) $x -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $x.Name -eq $n }, $true) | Select-Object -First 1).Extent.Text }; "
              "$d = Join-Path ([IO.Path]::GetTempPath()) ('ios-parts-' + [Guid]::NewGuid().ToString('N')); New-Item -ItemType Directory $d | Out-Null; "
              "$key = [ordered]@{ iso = 'R:\\x.iso'; isoSize = 5; index = 6; package = 'AB' }; "
              "'none=' + (Test-UpdateParts $d $key); "
              "Set-Content -LiteralPath (Join-Path $d 'install.swm') -Value 'aaaa'; Set-Content -LiteralPath (Join-Path $d 'install2.swm') -Value 'bb'; "
              "'unwritten=' + (Test-UpdateParts $d $key); "
              "Write-UpdateParts $d $key 'build 1'; 'written=' + (Test-UpdateParts $d $key); "
              "$other = [ordered]@{ iso = 'R:\\x.iso'; isoSize = 5; index = 6; package = 'CD' }; 'otherpackage=' + (Test-UpdateParts $d $other); "
              "Set-Content -LiteralPath (Join-Path $d 'install2.swm') -Value 'b'; 'truncated=' + (Test-UpdateParts $d $key); "
              "Remove-Item -LiteralPath $d -Recurse -Force") % script
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip()]
        self.assertEqual(lines, ["none=False", "unwritten=False", "written=True", "otherpackage=False", "truncated=False"], out.stderr)

    def test_update_iso_reads_the_pvd_label_not_the_diagnostic(self):
        # With xorriso's stderr folded in, the quoted diagnostic line came
        # first and became the label (review 2026-11-16). Lines as xorriso 1.5.6
        # prints them for win11.iso, and a 32-character label.
        shell, script = self._powershell(INV / "update-iso.ps1")
        ps = ("$a = [System.Management.Automation.Language.Parser]::ParseFile('%s', [ref]$null, [ref]$null); "
              "Invoke-Expression ($a.FindAll({ param($x) $x -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $x.Name -eq 'Get-PvdLabel' }, $true) | Select-Object -First 1).Extent.Text; "
              "$l32 = 'A' * 32; "
              "'esd=' + (Get-PvdLabel @(\"xorriso 1.5.6 : RockRidge filesystem manipulator\", \"Volume id    : 'ESD_ISO'\", 'Drive current: -indev x', 'Volume Id    : ESD_ISO', 'Volume Set Id: ')); "
              "'long=' + (Get-PvdLabel @(\"Volume id    : '$l32'\", \"Volume Id    : $l32\")).Length; "
              "'none=' + ($null -eq (Get-PvdLabel @(\"Volume id    : 'ESD_ISO'\"))); "
              "'twice=' + ($null -eq (Get-PvdLabel @('Volume Id    : A', 'Volume Id    : B')))") % script
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip()]
        self.assertEqual(lines, ["esd=ESD_ISO", "long=32", "none=True", "twice=True"], out.stderr)

    def test_a_second_update_iso_run_touches_nothing(self):
        # A rejected second run deleted the first run's staging before it took
        # the lock (review 2026-11-16). The lock is held here; the real script
        # must exit 4 with -Work as it found it.
        shell, script = self._powershell(INV / "update-iso.ps1", windows=True)
        ps = ("$w = Join-Path ([IO.Path]::GetTempPath()) ('ios-uiso-' + [Guid]::NewGuid().ToString('N')); "
              "New-Item -ItemType Directory (Join-Path $w 'iso'), (Join-Path $w 'parts') | Out-Null; "
              "Set-Content -LiteralPath (Join-Path $w 'parts\\install.swm') -Value keep; "
              "$pkgDir = Join-Path $w 'pkg'; New-Item -ItemType Directory $pkgDir | Out-Null; $tmp = Join-Path $pkgDir 'p'; Set-Content -LiteralPath $tmp -Value x; "
              "$pkg = Join-Path $pkgDir ('windows11.0-kb1-x64_' + (Get-FileHash -Algorithm SHA1 -LiteralPath $tmp).Hash.ToLower() + '.msu'); Move-Item $tmp $pkg; "
              "$full = [IO.Path]::GetFullPath($w).TrimEnd('\\'); "
              "$k = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($full.ToLowerInvariant()))).Replace('-', '').Substring(0, 16); "
              "$m = New-Object Threading.Mutex($true, \"Global\\ios-update-iso-$k\"); "
              "$p = Start-Process powershell.exe -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', '%s', '-Package', $pkg, '-Out', (Join-Path $w 'out.iso'), '-Work', $w, '-Iso', (Join-Path $w 'none.iso'), '-Name', 'ios-test-none' -Wait -PassThru -NoNewWindow; "
              "$m.ReleaseMutex(); 'exit=' + $p.ExitCode; 'iso=' + (Test-Path (Join-Path $w 'iso')); 'part=' + (Test-Path (Join-Path $w 'parts\\install.swm')); "
              "Remove-Item -LiteralPath $w -Recurse -Force") % script
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip() and not line.startswith("update-iso:")]
        self.assertEqual(lines, ["exit=4", "iso=True", "part=True"], out.stdout + out.stderr)

    def test_the_two_contrast_tables_agree(self):
        import re
        guest_dir = INV / "guest"
        setter = (guest_dir / "set-profile.ps1").read_text()
        probe = (guest_dir / "probe-profile.ps1").read_text()
        set_pairs = {m.group(1): m.group(2) for m in re.finditer(r"'([^']+)' = @\('[^']+', '([^']+)'\)", setter)}
        probe_pairs = {v: k for k, v in re.findall(r"'([^']+)' = '([^']+)'", probe.split("$ContrastNames", 1)[1].split("\n", 1)[0])}
        self.assertEqual(len(set_pairs), 4)
        self.assertEqual(set_pairs, probe_pairs)
        profiles = json.loads((INV.parents[1] / "docs/design/inventory/profiles.json").read_text())
        for name, p in profiles.items():
            self.assertIn(p.get("highContrast", "none"), ["none", *set_pairs], name)
            self.assertIn(round(p["dpi"] * 100 / 96), [100, 125, 150, 175, 200, 225, 250, 300], name)

    def test_ref_vm_reads_the_same_profile(self):
        text = (INV / "ref-vm.ps1").read_text()
        self.assertIn("docs\\design\\inventory\\profiles.json", text)
        self.assertNotIn("width = 1920", text)


class Shots(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.out = self.tmp / "out"
        shots = self.out / "shots" / "notepad-smoke"
        shots.mkdir(parents=True)
        core.write_png(shots / "rest.fhd-100.png", 2, 1, b"\xff\x00\x00\x00\xff\x00")
        self.store = self.tmp / "store"
        self.manifest = self.tmp / "shots.json"
        self.manifest.write_text(json.dumps([{"surface": "other", "state": "rest", "profile": "fhd-100",
                                              "width": 1, "height": 1, "size": 1, "sha256": "0" * 64}]))
        self.target = self.tmp / "inventory" / "notepad-smoke" / "window.json"   # its folder does not exist yet
        self.journal = self.tmp / "publish.journal"

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_png_is_valid(self):
        data = (self.out / "shots" / "notepad-smoke" / "rest.fhd-100.png").read_bytes()
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
        width, height, depth, colour = struct.unpack(">IIBB", data[16:26])
        self.assertEqual((width, height, depth, colour), (2, 1, 8, 2))
        idat = data.index(b"IDAT")
        length = struct.unpack(">I", data[idat - 4:idat])[0]
        self.assertEqual(zlib.decompress(data[idat + 4:idat + 4 + length]), b"\0\xff\x00\x00\x00\xff\x00")

    STATE = [{"state": "rest", "file": "rest.fhd-100.png", "width": 2, "height": 1}]

    def publish(self, records=({"id": "win11.notepad-smoke.window", "label": "Notepad"},), target=None,
                states=None, profile="fhd-100"):
        capture.publish(target or self.target, list(records), "notepad-smoke", self.out, states or self.STATE,
                        self.manifest, self.store, profile=profile, journal=self.journal)

    def shot(self, name):
        core.write_png(self.out / "shots" / "notepad-smoke" / name, 2, 1, b"\xff\x00\x00\x00\xff\x00")
        return {"state": name.rsplit(".", 2)[0], "file": name, "width": 2, "height": 1}

    def test_each_file_and_profile_keeps_its_own_screenshots(self):
        self.publish()                                                     # window at fhd-100
        menu = self.shot("menu.fhd-100.png")
        self.publish([{"id": "win11.notepad-smoke.menu", "label": "Menu"}],
                     target=self.target.with_name("menu.json"), states=[menu])
        self.publish(states=[self.shot("rest.fhd-150.png")], profile="fhd-150")
        owned = lambda: sorted((e.get("file"), e["state"], e["profile"])         # noqa: E731
                               for e in json.loads(self.manifest.read_text()) if e["surface"] == "notepad-smoke")
        self.assertEqual(owned(), [("menu", "menu", "fhd-100"), ("window", "rest", "fhd-100"),
                                   ("window", "rest", "fhd-150")])
        self.publish(states=[self.shot("other-open.fhd-100.png")])          # window at fhd-100 again, new states
        self.assertEqual(owned(), [("menu", "menu", "fhd-100"), ("window", "other-open", "fhd-100"),
                                   ("window", "rest", "fhd-150")])
        mine = [e for e in json.loads(self.manifest.read_text()) if e["surface"] == "notepad-smoke"]
        self.assertEqual(capture.verify_shots(mine, self.store), [])

    def test_a_shot_of_another_profile_publishes_nothing(self):
        with self.assertRaises(OSError):
            self.publish(profile="fhd-150")
        self.assertFalse(self.target.exists())

    def test_publish_then_verify(self):
        self.publish()
        manifest = json.loads(self.manifest.read_text())
        self.assertEqual([(e["surface"], e["state"], e["profile"]) for e in manifest],
                         [("notepad-smoke", "rest", "fhd-100"), ("other", "rest", "fhd-100")])
        (self.store / "other").mkdir()
        (self.store / "other" / "rest.fhd-100.png").write_bytes(b"x")
        bad = capture.verify_shots(manifest, self.store)
        self.assertEqual(bad, [f"changed: {self.store / 'other' / 'rest.fhd-100.png'}"])
        (self.store / "notepad-smoke" / "rest.fhd-100.png").unlink()
        self.assertEqual(len(capture.verify_shots(manifest, self.store)), 2)

    def test_a_recapture_replaces_its_surface_entries_only(self):
        self.publish()
        self.publish()
        manifest = json.loads(self.manifest.read_text())
        self.assertEqual(len(manifest), 2)

    def test_a_first_publication_creates_its_folder(self):
        self.publish()
        self.assertEqual(core.read_records(self.target), [{"id": "win11.notepad-smoke.window", "label": "Notepad"}])

    def test_a_failed_publication_leaves_the_previous_one_whole(self):
        self.publish([{"id": "win11.notepad-smoke.window", "label": "Notepad", "behaviour": "ours"}])
        before = {p: p.read_bytes() for p in (self.target, self.manifest, self.store / "notepad-smoke" / "rest.fhd-100.png")}
        (self.out / "shots" / "notepad-smoke" / "rest.fhd-100.png").unlink()     # the second shot cannot be copied
        with self.assertRaises(OSError):
            self.publish([{"id": "win11.notepad-smoke.window", "label": "New"}])
        self.assertEqual({p: p.read_bytes() for p in before}, before)
        leftovers = [p.name for p in self.tmp.rglob("*") if p.name.endswith((".tmp", ".staged"))]
        self.assertEqual(leftovers, [])

    def test_a_failure_during_the_swap_puts_everything_back(self):
        self.publish([{"id": "win11.notepad-smoke.window", "label": "Notepad", "behaviour": "ours"}])
        before = {p: p.read_bytes() for p in (self.target, self.manifest, self.store / "notepad-smoke" / "rest.fhd-100.png")}
        real = capture.os.replace
        calls = []

        def flaky(src, dst):
            calls.append(dst)
            if str(src).endswith(".staged"):          # the records, swapped last
                raise OSError("disk gone")
            return real(src, dst)
        capture.os.replace = flaky
        try:
            with self.assertRaises(OSError):
                self.publish([{"id": "win11.notepad-smoke.window", "label": "New"}])
        finally:
            capture.os.replace = real
        self.assertEqual({p: p.read_bytes() for p in before}, before)
        leftovers = [p.name for p in self.tmp.rglob("*") if p.name.endswith((".tmp", ".staged", ".old"))]
        self.assertEqual(leftovers, [])

    def test_two_shots_with_one_file_name_publish_nothing(self):
        self.publish([{"id": "win11.notepad-smoke.window", "label": "Notepad", "behaviour": "ours"}])
        before = {p: p.read_bytes() for p in (self.target, self.manifest, self.store / "notepad-smoke" / "rest.fhd-100.png")}
        with self.assertRaises(OSError):
            capture.publish(self.target, [{"id": "x"}], "notepad-smoke", self.out, self.STATE * 2,
                            self.manifest, self.store, profile="fhd-100", journal=self.journal)
        self.assertEqual({p: p.read_bytes() for p in before}, before)

    def test_a_kill_part_way_is_rolled_back_by_the_next_run(self):
        self.publish([{"id": "win11.notepad-smoke.window", "label": "Notepad", "behaviour": "ours"}])
        before = {p: p.read_bytes() for p in (self.target, self.manifest, self.store / "notepad-smoke" / "rest.fhd-100.png")}
        # A child process publishes and dies (os._exit: no handler runs) as it
        # swaps the records, after the screenshot and the manifest are swapped.
        script = f"""
import importlib.util, os, pathlib, sys
spec = importlib.util.spec_from_file_location("capture_host", {str(INV / "capture.py")!r})
capture = importlib.util.module_from_spec(spec); spec.loader.exec_module(capture)
real = os.replace
def dying(src, dst):
    if str(dst).endswith(".old") and "window.json" in str(dst):
        real(src, dst)
        os._exit(9)
    return real(src, dst)
capture.os.replace = dying
t = pathlib.Path({str(self.tmp)!r})
capture.publish(t / "inventory" / "notepad-smoke" / "window.json", [{{"id": "win11.notepad-smoke.window", "label": "New"}}],
                "notepad-smoke", t / "out", {self.STATE!r}, t / "shots.json", t / "store", profile="fhd-100",
                journal=t / "publish.journal")
"""
        r = capture.subprocess.run([sys.executable, "-c", script], capture_output=True, timeout=60)
        self.assertEqual(r.returncode, 9, r.stderr)
        self.assertFalse(self.target.exists())          # killed with the records moved aside
        self.assertTrue(capture.recover(self.journal))
        self.assertEqual({p: p.read_bytes() for p in before}, before)
        self.assertFalse(self.journal.exists())
        leftovers = [p.name for p in self.tmp.rglob("*") if p.name.endswith((".tmp", ".staged", ".old"))]
        self.assertEqual(leftovers, [])
        self.assertFalse(capture.recover(self.journal))   # nothing left to do

    def test_a_failed_record_write_leaves_the_file_whole(self):
        core.write_records(self.tmp / "r.json", [{"id": "a"}])
        real = json.dumps

        def boom(obj, **kw):
            if obj.get("id") == "b":
                raise OSError("disk full")
            return real(obj, **kw)
        core.json.dumps = boom
        try:
            with self.assertRaises(OSError):
                core.write_records(self.tmp / "r.json", [{"id": "c"}, {"id": "b"}])
        finally:
            core.json.dumps = real
        self.assertEqual(core.read_records(self.tmp / "r.json"), [{"id": "a"}])


class Received(unittest.TestCase):
    """The host takes a capture only whole: exit 0, complete, every counted record."""

    def setUp(self):
        self.out = pathlib.Path(tempfile.mkdtemp())
        self.meta = {"complete": True, "records": 2}
        self.recs = [{"id": "win11.s.r"}, {"id": "win11.s.r.a"}]

    def tearDown(self):
        shutil.rmtree(self.out)

    def write(self, meta=True, recs=True):
        if meta:
            (self.out / "meta.json").write_text(json.dumps(self.meta))
        if recs:
            core.write_records(self.out / "records.json", self.recs)

    def test_a_whole_capture_is_taken(self):
        self.write()
        self.assertEqual(capture.received(self.out, 0), (self.meta, self.recs))

    def test_a_failed_transfer_is_refused(self):
        self.write()
        with self.assertRaises(capture.core.Incomplete):
            capture.received(self.out, 1)

    def test_missing_records_are_refused(self):
        self.write(recs=False)
        with self.assertRaises(capture.core.Incomplete):
            capture.received(self.out, 0)

    def test_a_short_but_valid_file_is_refused(self):
        self.recs = self.recs[:1]
        self.write()
        with self.assertRaises(capture.core.Incomplete):
            capture.received(self.out, 0)

    def test_a_cut_line_is_refused(self):
        self.write()
        data = (self.out / "records.json").read_bytes()
        (self.out / "records.json").write_bytes(data[:-5])
        with self.assertRaises(capture.core.Incomplete):
            capture.received(self.out, 0)

    def test_an_empty_capture_is_refused(self):
        self.meta, self.recs = {"complete": True, "records": 0}, []
        self.write()
        with self.assertRaises(capture.core.Incomplete):
            capture.received(self.out, 0)


class Arguments(unittest.TestCase):
    def test_a_path_is_refused_before_anything_runs(self):
        real = capture.run_guest
        capture.run_guest = lambda *a: self.fail("the guest ran")
        try:
            for bad in ("../../etc/x", "/tmp/x", "a/b", "Window", "baseline", "baseline-capture"):
                with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as ex:
                    capture.main(["--surface", "notepad-smoke", "--file", bad, "--dry-run"])
                self.assertEqual(ex.exception.code, 2)
        finally:
            capture.run_guest = real


    def test_an_unknown_surface_or_profile_is_refused_by_name(self):
        real = capture.run_guest
        capture.run_guest = lambda *a: self.fail("the guest ran")
        try:
            for argv, said in ((["--surface", "no-such-walker", "--file", "x"], "has no walker"),
                               (["--surface", "notepad-smoke", "--file", "x", "--profile", "fhd-999"], "is not in")):
                err = io.StringIO()
                with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as ex:
                    capture.main(argv + ["--dry-run"])
                self.assertEqual(ex.exception.code, 2)
                self.assertIn(said, err.getvalue())
        finally:
            capture.run_guest = real


class Fixtures(unittest.TestCase):
    def setUp(self):
        self.dir = pathlib.Path(tempfile.mkdtemp())
        (self.dir / "lists.ps1").write_text("# Variants: empty, populated\n")
        (self.dir / "plain.ps1").write_text("# no variants\n")

    def tearDown(self):
        shutil.rmtree(self.dir)

    def test_a_spec_names_a_script_and_a_declared_variant(self):
        p = lambda spec: core.fixture_problem(self.dir, spec)
        self.assertIsNone(p("lists:empty"))
        self.assertIsNone(p("plain"))
        self.assertEqual(p("lists"), "fixture 'lists': name one of its variants (empty, populated)")
        self.assertEqual(p("lists:huge"), "fixture 'lists:huge': lists.ps1 has no variant 'huge' (it has: empty, populated)")
        self.assertEqual(p("nope:x"), "fixture 'nope:x': no script nope.ps1 in scripts/inventory/fixtures/")
        self.assertEqual(p("../x:y"), "fixture '../x:y': the name must be a slug")

    def test_every_real_fixture_declares_its_variants(self):
        for path in sorted((INV / "fixtures").glob("*.ps1")):
            m = core.FIXTURE_VARIANTS.search(path.read_text())
            self.assertIsNotNone(m, path.name)
            declared = [v.strip() for v in m.group(1).split(",")]
            self.assertIn("[ValidateSet(%s)]" % ", ".join(f"'{v}'" for v in declared), path.read_text(), path.name)

    def test_prepare_runs_the_fixture_before_the_walk(self):
        seen = {}
        real = capture.ref_run
        capture.ref_run = lambda label, files, cmd, timeout, checkpoint: seen.update(files=files, cmd=cmd, cp=checkpoint) or ("out", 0)
        try:
            capture.run_guest("notepad-smoke", "fhd-100", 60, prepare="lists:populated")
        finally:
            capture.ref_run = real
        self.assertEqual(seen["cp"], "clean")
        self.assertIn("fixtures/lists.ps1", seen["files"])
        first, _, rest = seen["cmd"].partition(" && ")
        self.assertTrue(first.endswith("C:\\ios\\in\\fixtures\\lists.ps1 -Variant populated"), first)
        self.assertTrue(rest.startswith("python C:\\ios\\in\\capture-ui.py"), rest)

    def test_a_machine_profile_is_captured_on_its_checkpoint(self):
        seen = {}
        real = capture.ref_run
        capture.ref_run = lambda label, files, cmd, timeout, checkpoint: seen.update(cp=checkpoint, cmd=cmd) or ("out", 0)
        try:
            capture.run_guest("notepad-smoke", "fhd-100-rtl", 60)
        finally:
            capture.ref_run = real
        self.assertEqual(seen["cp"], "profile-fhd-100-rtl")
        self.assertIn("--profile fhd-100-rtl", seen["cmd"])

    def test_an_unknown_fixture_is_refused_before_the_vm(self):
        real = capture.run_guest
        capture.run_guest = lambda *a: self.fail("the guest ran")
        try:
            err = io.StringIO()
            with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as ex:
                capture.main(["--surface", "notepad-smoke", "--file", "x", "--prepare", "lists:huge", "--dry-run"])
            self.assertEqual(ex.exception.code, 2)
            self.assertIn("has no variant 'huge'", err.getvalue())
        finally:
            capture.run_guest = real

    def test_the_runner_compares_both_states(self):
        run = load("fixture_run", INV / "fixtures" / "run.py")
        def verdict(states, spec="lists:populated"):
            for i, st in enumerate(states, 1):
                (self.dir / f"state{i}.json").write_text("\ufeff" + json.dumps(st), encoding="utf-8")
            return run.verdict(self.dir, spec)
        same = {"files": ["a"], "apps": []}
        self.assertEqual(verdict([same, same]), ("lists:populated: same state twice", True))
        self.assertEqual(verdict([same, {"files": ["a", "b"], "apps": []}]),
                         ("lists:populated: the second run left a different state", False))
        self.assertEqual(verdict([{"files": []}, {"files": []}]), ("lists:populated: the fixture left nothing", False))
        # An empty variant still owns state (the empty folder); all-empty means it did nothing.
        self.assertEqual(verdict([{"files": []}, {"files": []}], "lists:empty"), ("lists:empty: the fixture left nothing", False))
        self.assertEqual(verdict([{"folder": True, "files": []}] * 2, "lists:empty"), ("lists:empty: same state twice", True))
        (self.dir / "state2.json").unlink()
        self.assertEqual(run.verdict(self.dir, "lists:empty"), ("lists:empty: no state after round 2", False))


class Displays(unittest.TestCase):
    """Two-display profiles on the stand-in machine (section 22)."""

    runner = load("standin_run_displays", INV / "standin" / "run.py")
    probe = load("standin_probe", INV / "standin" / "probe.py")
    variants = json.loads((INV / "standin" / "variants.json").read_text())
    profiles = json.loads((INV.parents[1] / "docs" / "design" / "inventory" / "profiles.json").read_text())

    def test_two_heads_replace_the_basic_display_and_drop_the_laptop(self):
        cmd = self.runner.qemu_cmd("o.qcow2", "v.fd", None, "j.img", self.variants["dual"], "q.sock")
        line = " ".join(cmd)
        self.assertIn("virtio-vga,xres=1920,yres=1080,edid=on,addr=0x3", line)
        self.assertIn("virtio-gpu-pci,xres=1920,yres=1080,edid=on,addr=0x5", line)
        self.assertNotIn("VGA,xres", line.replace("virtio-vga,xres", ""))
        self.assertNotIn("-acpitable", cmd)
        one = self.runner.qemu_cmd("o.qcow2", "v.fd", "a.aml", "j.img", self.variants["charging"], "q.sock")
        self.assertIn("-acpitable", one)
        self.assertIn("VGA,xres=1920,yres=1080,edid=on,addr=0x3", one)

    def test_a_head_count_the_machine_lacks_is_refused(self):
        with self.assertRaises(ValueError):
            self.runner.heads({"displays": 3})

    def test_a_two_display_profile_picks_the_two_head_variant(self):
        v, why = core.standin_variant("dual-100-150", self.profiles["dual-100-150"], self.variants)
        self.assertEqual((v, why), ("dual", None))
        self.assertEqual(core.standin_variant("fhd-100", self.profiles["fhd-100"], self.variants), (None, None))
        v, why = core.standin_variant("dual-150-100", self.profiles["dual-150-100"], self.variants, "charging")
        self.assertIsNone(v)
        self.assertIn("needs 2", why)
        v, why = core.standin_variant("fhd-100", self.profiles["fhd-100"], self.variants, "dual")
        self.assertIsNone(v)
        self.assertIn("--standin dual has 2", why)
        v, why = core.standin_variant("x", {"monitors": 3}, self.variants)
        self.assertIsNone(v)
        self.assertIn("0 stand-in variants", why)

    def test_every_two_display_profile_names_its_second_display(self):
        live = re.search(r"\$LiveKeys = (.*)", (INV / "ref-vm.ps1").read_text()).group(1)
        for name, p in self.profiles.items():
            if p.get("monitors", 1) == 2:
                self.assertIn(p.get("secondSide"), ("left", "right"), name)
                self.assertIn(round(p["secondDpi"] * 100 / 96), (100, 125, 150, 175, 200), name)
                self.assertNotEqual(p["secondDpi"], p["dpi"], name)   # two displays at different scales
            else:
                self.assertNotIn("secondDpi", p, name)
                self.assertNotIn("secondSide", p, name)
        for key in ("'secondDpi'", "'secondSide'"):
            self.assertIn(key, live)       # set-profile applies them in the session

    def test_the_primary_swap_is_the_second_profile(self):
        a, b = self.profiles["dual-100-150"], self.profiles["dual-150-100"]
        self.assertEqual((a["dpi"], a["secondDpi"]), (b["secondDpi"], b["dpi"]))
        self.assertEqual({a["secondSide"], b["secondSide"]}, {"left", "right"})

    def test_a_machine_profile_restores_its_own_checkpoint(self):
        self.assertEqual(core.checkpoint_for("fhd-100", self.profiles), "clean")
        self.assertEqual(core.checkpoint_for("fhd-150", self.profiles), "clean")          # live: applied in clean
        for name in ("fhd-100-rtl", "wide-100", "tall-100", "qhd-200"):
            self.assertEqual(core.checkpoint_for(name, self.profiles), f"profile-{name}", name)

    def test_the_live_keys_are_the_same_in_both_tools(self):
        live = re.search(r"\$LiveKeys = (.*)", (INV / "ref-vm.ps1").read_text()).group(1)
        self.assertEqual(tuple(re.findall(r"'([A-Za-z]+)'", live)), core.LIVE_KEYS)

    def test_a_remote_session_is_never_recorded_as_console(self):
        self.assertEqual(core.session_type({"remote": False}), "console")
        self.assertEqual(core.session_type({}), "console")
        self.assertIsNone(core.session_type({"remote": True}))

    def test_the_probe_tells_a_refusal_from_a_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp)
            (out / "seen.json").write_text('{"monitors":2}\n', encoding="utf-8")
            self.assertEqual(self.probe.result(0, out), (0, '{"monitors":2}'))
            (out / "seen.json").unlink()
            (out / "stdout.txt").write_text("set-profile: second-display scaling 175 is not offered", encoding="utf-8")
            self.assertEqual(self.probe.result(1, out)[0], 1)          # no refusal marker: a failure
            (out / "refused.txt").write_text("refused", encoding="utf-8")
            self.assertEqual(self.probe.result(1, out), (2, "set-profile: second-display scaling 175 is not offered"))
            (out / "refused.txt").unlink()
            (out / "stdout.txt").unlink()
            self.assertEqual(self.probe.result(3, out), (3, "probe: the job left no probe line"))


class StandIn(unittest.TestCase):
    """The stand-in machine (section 21): its ACPI devices, its runner and its proofs."""

    runner = load("standin_run", INV / "standin" / "run.py")
    prove = load("standin_prove", INV / "standin" / "prove.py")

    def test_every_variant_fills_every_placeholder(self):
        asl = (INV / "standin" / "standin.asl").read_text()
        variants = json.loads((INV / "standin" / "variants.json").read_text())
        for name, v in variants.items():
            if not v.get("laptop", True):
                continue          # no laptop table on this variant: nothing is filled
            filled = self.runner.fill(asl, v)
            # Only the comment's own "@NAME@" remains; every value is filled.
            self.assertEqual(re.findall(r"@[A-Z_]+@", filled), ["@NAME@"], name)
            self.assertIn(f"Return ({v['ac']})", filled, name)

    def test_the_backlight_node_is_the_slot_the_display_is_pinned_to(self):
        # QEMU names the device at PCI slot N "S" + hex(N * 8): slot 3 is S18.
        cmd = " ".join(self.runner.qemu_cmd("o.qcow2", "v.fd", "a.aml", "j.img", {}, "q.sock"))
        self.assertIn("VGA,xres=1920,yres=1080,edid=on,addr=0x3", cmd)
        self.assertIn("External (\\_SB.PCI0.S18, DeviceObj)", (INV / "standin" / "standin.asl").read_text())
        # Windows mounts a partitionless FAT volume only on removable media.
        self.assertIn("usb-storage,drive=job,bus=xhci.0,removable=on", cmd)
        self.assertIn("-no-reboot", cmd)

    def test_the_command_job_runs_the_command_as_ref_run_does(self):
        job = self.runner.COMMAND_JOB.format(command='python C:\\ios\\in\\a.py --x "y"'.replace('"', '""'))
        self.assertIn('& cmd.exe /c "python C:\\ios\\in\\a.py --x ""y"" > C:\\ios\\out\\stdout.txt 2>&1"', job)
        self.assertIn("Set-Location -LiteralPath 'C:\\ios'", job)

    def test_a_proof_needs_every_expectation(self):
        unmet = self.prove.unmet
        power = {"power": {"ac": 0, "percent": 15, "saver": 1}}
        self.assertEqual(unmet(power, {"power": {"ac": 0, "percent": 15, "saver": 1}}), [])
        self.assertEqual(unmet(power, {"power": {"ac": 0, "percent": 15, "saver": 0}}), ["saver is 0, want 1"])
        self.assertEqual(unmet(power, {}), ["ac is None, want 0", "percent is None, want 15", "saver is None, want 1"])
        lid = {"device": "ACPI\\PNP0C0D\\*"}
        self.assertEqual(unmet(lid, {"devices": ["ACPI\\PNP0C0D\\2&DABA3FF&0"]}), [])
        self.assertEqual(unmet(lid, {"devices": ["ACPI\\PNP0C0A\\1"]}), ["no working device ACPI\\PNP0C0D\\*"])
        self.assertEqual(unmet({"brightness": True}, {"brightness": False}), ["brightness reported is False, want True"])
        self.assertEqual(unmet({"colour": 1}, {}), ["unknown expectation 'colour'"])

    def test_each_page_is_opened_once_and_shared(self):
        entries = {"a": {"page": "ms-settings:batterysaver", "marker": "Power Mode"},
                   "b": {"page": "ms-settings:display", "marker": "Brightness"},
                   "c": {"page": "ms-settings:batterysaver", "marker": "Power Mode"}}
        job = self.prove.job_for(["a", "b", "c"], entries)
        self.assertEqual(job.count("$pages += ,@("), 2)
        self.assertIn("$pages += ,@('page0', 'ms-settings:batterysaver', 'Power Mode')", job)
        self.assertIn("$pages += ,@('page1', 'ms-settings:display', 'Brightness')", job)
        self.assertNotIn("PAGES", job)
        self.assertNotIn("Start-Sleep 60", job)

    def test_a_page_that_never_showed_is_not_proven(self):
        entries = {"a": {"page": "ms-settings:display", "marker": "Brightness", "expect": {"brightness": True}}}
        facts = {"brightness": True, "ready": []}
        self.assertEqual(self.prove.verdicts(["a"], entries, 0, facts),
                         {"a": ["ms-settings:display never showed 'Brightness'"]})
        self.assertEqual(self.prove.verdicts(["a"], entries, 0, dict(facts, ready=["page0"])), {"a": []})
        self.assertEqual(self.prove.verdicts(["a"], entries, 3, None), {"a": ["the stand-in job failed"]})

    def test_a_later_failure_keeps_the_earlier_invalidation_on_disk(self):
        tmp = pathlib.Path(tempfile.mkdtemp())
        reg = tmp / "stand-ins.json"
        old = {"variant": "charging", "page": "p", "marker": "m", "expect": {}, "proof": {"sha256": "a" * 64}}
        reg.write_text(json.dumps({"x": dict(old), "y": dict(old, variant="saver")}))
        calls = []

        def fake(run, variant, group, entries, timeout, failed):
            calls.append(variant)
            if variant == "charging":
                failed.append("x")
                entries["x"].pop("proof")
                return
            raise OSError("disk full")
        real = (self.prove.STAND_INS, self.prove.prove_variant, self.prove._load)
        self.prove.STAND_INS, self.prove.prove_variant, self.prove._load = reg, fake, lambda *a: None
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                rc = self.prove.main([])
        finally:
            self.prove.STAND_INS, self.prove.prove_variant, self.prove._load = real
            got = json.loads(reg.read_text())
            shutil.rmtree(tmp)
        self.assertEqual((rc, calls), (1, ["charging", "saver"]))
        self.assertNotIn("proof", got["x"])
        self.assertNotIn("proof", got["y"])

    def _sleeper(self, ignore_term=False):
        # A child that sleeps at most 30 s; with ignore_term it survives SIGTERM.
        code = "import signal,time\n" + ("signal.signal(signal.SIGTERM, signal.SIG_IGN)\n" if ignore_term else "")
        code += "print('up', flush=True)\ntime.sleep(30)"
        p = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.PIPE, text=True, start_new_session=True)
        p.stdout.readline()
        return p

    def test_stop_escalates_to_sigkill_and_reaps(self):
        p = self._sleeper(ignore_term=True)
        try:
            self.runner.stop(p, grace=0.5)
            self.assertEqual(p.returncode, -9)
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()

    def test_stop_on_an_exited_guest_is_harmless(self):
        p = self._sleeper()
        p.terminate()
        p.wait()
        self.runner.stop(p, grace=0.5)

    @staticmethod
    def _gone(pid):
        try:
            return pathlib.Path(f"/proc/{pid}/stat").read_text().split()[2] == "Z"
        except FileNotFoundError:
            return True

    def test_qemu_dies_with_a_killed_runner(self):
        # The runner starts a child with _die_with_parent, prints its pid, then is
        # SIGKILLed. The child ignores SIGTERM, as a hung QEMU can.
        child_code = ("import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                      "print('ready', flush=True); time.sleep(30)")
        code = ("import importlib.util,subprocess,sys,time\n"
                f"spec=importlib.util.spec_from_file_location('r', {str(INV / 'standin' / 'run.py')!r})\n"
                "m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)\n"
                f"c=subprocess.Popen([sys.executable,'-c',{child_code!r}],preexec_fn=m._die_with_parent)\n"
                "print(c.pid, flush=True); time.sleep(30)")
        runner = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.PIPE, text=True, start_new_session=True)
        # Both write to one pipe: the child's pid, and its "ready" once SIGTERM is ignored.
        lines = [runner.stdout.readline().strip() for _ in range(2)]
        child = int(next(x for x in lines if x.isdigit()))
        try:
            runner.kill()
            runner.wait()
            for _ in range(50):
                if self._gone(child):
                    break
                time.sleep(0.1)
            else:
                self.fail("the child outlived its killed runner")
        finally:
            with contextlib.suppress(ProcessLookupError):
                os.kill(child, signal.SIGKILL)

    def test_stale_overlays_are_cleared_and_live_ones_kept(self):
        tmp = pathlib.Path(tempfile.mkdtemp())
        try:
            disk = tmp / "standin.qcow2"
            dead = subprocess.Popen([sys.executable, "-c", "pass"])
            dead.wait()
            for pid in (dead.pid, os.getpid()):
                (tmp / f"run-{pid}.qcow2").write_text("x")
            self.runner.clear_stale_overlays(disk)
            self.assertEqual(sorted(f.name for f in tmp.iterdir()), [f"run-{os.getpid()}.qcow2"])
        finally:
            shutil.rmtree(tmp)

    def test_a_cancelled_capture_stops_the_runner(self):
        p = self._sleeper(ignore_term=True)
        try:
            capture.stop_runner(p, grace=0.5)
            self.assertEqual(p.returncode, -9)
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()

    def test_the_registry_and_the_matrix_agree(self):
        entries = json.loads((INV.parents[1] / "docs" / "design" / "inventory" / "stand-ins.json").read_text())
        variants = json.loads((INV / "standin" / "variants.json").read_text())
        rows = json.loads((INV.parents[1] / "docs" / "design" / "inventory" / "expected.json").read_text())
        named = {r["standin"] for r in rows if r.get("source") == "stand-in"}
        self.assertEqual(named, set(entries))
        for sid, e in entries.items():
            self.assertIn(e["variant"], variants, sid)
            self.assertTrue(e["page"].startswith("ms-settings:"), sid)

    def test_capture_routes_a_stand_in_capture_to_the_stand_in_machine(self):
        seen = {}
        real_ref, real_si = capture.ref_run, capture.standin_run
        capture.ref_run = lambda *a, **k: self.fail("the reference VM ran")
        capture.standin_run = lambda label, files, cmd, timeout, variant: seen.update(v=variant, cmd=cmd) or ("out", 0)
        try:
            capture.run_guest("notepad-smoke", "fhd-100", 60, standin="saver")
        finally:
            capture.ref_run, capture.standin_run = real_ref, real_si
        self.assertEqual(seen["v"], "saver")
        self.assertTrue(seen["cmd"].startswith("python C:\\ios\\in\\capture-ui.py"), seen["cmd"])

    def test_an_unknown_stand_in_variant_is_refused_before_any_machine(self):
        real = capture.run_guest
        capture.run_guest = lambda *a: self.fail("the guest ran")
        try:
            err = io.StringIO()
            with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as ex:
                capture.main(["--surface", "notepad-smoke", "--file", "x", "--standin", "flat", "--dry-run"])
            self.assertEqual(ex.exception.code, 2)
            self.assertIn("--standin 'flat' is not in scripts/inventory/standin/variants.json", err.getvalue())
        finally:
            capture.run_guest = real


class Serial(unittest.TestCase):
    """One capture at a time, and a host deadline that never strands the VM."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_a_second_capture_is_refused_while_one_runs(self):
        lock = self.tmp / "capture.lock"
        with capture.exclusive(lock):
            with self.assertRaises(BlockingIOError):
                with capture.exclusive(lock):
                    pass
        with capture.exclusive(lock):              # released again
            pass

    def test_the_host_deadline_covers_the_guest_and_stops_the_vm_after(self):
        calls = []
        real_run, real_work, real_win = capture.subprocess.run, capture.WORK, capture.winpath

        def fake(cmd, **kw):
            calls.append((cmd, kw.get("timeout")))
            if "-Stop" in cmd:
                return capture.subprocess.CompletedProcess(cmd, 0, "ref-run: ios-win11-ref is off\r\n", "")
            raise capture.subprocess.TimeoutExpired(cmd, kw["timeout"])
        capture.subprocess.run, capture.WORK, capture.winpath = fake, self.tmp, str
        try:
            with contextlib.redirect_stdout(io.StringIO()) as out:
                _, rc = capture.run_guest("notepad-smoke", "fhd-100", 900)
        finally:
            capture.subprocess.run, capture.WORK, capture.winpath = real_run, real_work, real_win
        self.assertEqual(rc, 3)
        self.assertEqual(calls[0][1], 900 + capture.RUN_MARGIN)
        self.assertIn("-Stop", calls[1][0])
        self.assertIn("is off", out.getvalue())
        self.assertEqual([p.name for p in self.tmp.iterdir() if p.name.endswith("-in")], [])   # staging removed


    def test_an_interrupted_capture_stops_the_vm(self):
        calls = []
        real_run, real_work, real_win = capture.subprocess.run, capture.WORK, capture.winpath

        def fake(cmd, **kw):
            calls.append(cmd)
            if "-Stop" in cmd:
                return capture.subprocess.CompletedProcess(cmd, 0, "ref-run: ios-win11-ref is off", "")
            raise KeyboardInterrupt
        capture.subprocess.run, capture.WORK, capture.winpath = fake, self.tmp, str
        try:
            with contextlib.redirect_stdout(io.StringIO()) as out, self.assertRaises(KeyboardInterrupt):
                capture.run_guest("notepad-smoke", "fhd-100", 900)
        finally:
            capture.subprocess.run, capture.WORK, capture.winpath = real_run, real_work, real_win
        self.assertEqual(sum("-Stop" in c for c in calls), 1)
        self.assertIn("interrupted; ref-run: ios-win11-ref is off", out.getvalue())


class States(unittest.TestCase):
    def test_state_names_follow_ids_one_to_one(self):
        t = node("Root", "Window", children=[node("", "MenuItem", "a-b", expands=[node("X", "MenuItem")]),
                                             node("", "Group", "a", children=[
                                                 node("", "MenuItem", "b", expands=[node("Y", "MenuItem")])])])
        seen = []
        core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", session="console", root_slug="r", root_label="R",
                  shot=lambda state, el, items, *_: seen.append(state) or {"file": f"{state}.p.png"})
        self.assertEqual(sorted(seen), ["r", "r.a-b-open", "r.a.b-open"])


class Dump(unittest.TestCase):
    def test_the_dump_walks_to_the_same_records(self):
        cap = walk()
        d = core.dump(core.FixtureElement(tree()), environment_lists=smoke.ENVIRONMENT_LISTS)
        core.confirm_dump(d, cap, surface="notepad-smoke", build="26300.9711", profile="fhd-100", session="console",
                          root_slug=smoke.ROOT_SLUG, root_label=smoke.ROOT_LABEL,
                          environment_lists=smoke.ENVIRONMENT_LISTS, values=smoke.VALUES)
        self.assertEqual(core.dump(core.FixtureElement(copy.deepcopy(d)), environment_lists=smoke.ENVIRONMENT_LISTS), d)

    def test_the_dump_visits_only_what_the_walk_visits(self):
        rows = [node(f"row {i}", "ListItem") for i in range(500)]
        t = node("Root", "Window", children=[node("Files", "List", children=rows),
                                             node("Tip", "Pane", offscreen=True, children=[node("Deep", "Text")])])
        visited = []
        real = core.FixtureElement.children

        def spy(self):
            visited.append(self.name)
            return real(self)
        core.FixtureElement.children = spy
        try:
            d = core.dump(core.FixtureElement(t), environment_lists=[{"controlType": "List"}])
        finally:
            core.FixtureElement.children = real
        self.assertEqual(visited, ["Root"])
        self.assertEqual([k["name"] for k in d["children"]], ["Files"])
        self.assertNotIn("children", d["children"][0])

    def test_the_dump_is_bounded(self):
        wide = node("Root", "Window", children=[node(f"b{i}", "Button") for i in range(core.MAX_RECORDS + 1)])
        with self.assertRaises(core.Incomplete):
            core.dump(core.FixtureElement(wide))

    def test_entries_are_dumped_flat_and_never_opened(self):
        t = node("Root", "Window", children=[node("File", "MenuItem", expands=[
            node("Recent", "MenuItem", expands=[node("Nested", "MenuItem")], children=[node("Kid", "Text")])])])
        opened = []
        real = core.FixtureElement.expand

        def spy(self, **kw):
            opened.append(self.name)
            return real(self, **kw)
        core.FixtureElement.expand = spy
        try:
            d = core.dump(core.FixtureElement(t))
        finally:
            core.FixtureElement.expand = real
        self.assertEqual(opened, ["File"])
        entry = d["children"][0]["expands"][0]
        self.assertEqual(entry["name"], "Recent")
        self.assertNotIn("expands", entry)
        self.assertNotIn("children", entry)


class Settle(unittest.TestCase):
    def caps(self, *labels):
        out = []
        for label in labels:
            c = core.Capture()
            c.records = [{"id": "a", "label": label}]
            out.append(c)
        return iter(out)

    def test_the_first_repeat_is_taken(self):
        seq = self.caps("first open", "B", "B")
        cap, passes = core.settle(lambda: next(seq))
        self.assertEqual((cap.records[0]["label"], passes), ("B", 3))

    def test_a_surface_that_never_repeats_is_incomplete(self):
        seq = self.caps("A", "B", "C", "D", "E")
        with self.assertRaises(core.Incomplete) as ex:
            core.settle(lambda: next(seq))
        self.assertIn(f"in {core.MAX_PASSES} walks", str(ex.exception))

    def walk_args(self):
        return dict(surface="notepad-smoke", build="26300.9711", profile="fhd-100", session="console", root_slug="window",
                    root_label="Notepad", environment_lists=smoke.ENVIRONMENT_LISTS, values=smoke.VALUES)

    def test_a_dump_that_matches_its_walk_is_accepted(self):
        cap = walk()
        core.confirm_dump(tree(), cap, **self.walk_args())

    def test_a_dump_that_moved_is_refused(self):
        cap = walk()
        t = tree()
        t["bounds"][2] += 18                     # the window grew between the walk and the dump
        with self.assertRaises(core.Incomplete):
            core.confirm_dump(t, cap, **self.walk_args())


class Scenes(unittest.TestCase):
    """A module walks several roots into one file (section 8: the desktop and
    each of its context menus)."""

    def mod(self, **kw):
        log = []
        menu = node("", "Window", "OverflowPopup", bounds=(0, 0, 0, 0),
                    children=[node("View", "MenuItem", "view", bounds=(500, 300, 170, 32)),
                              node("Refresh", "MenuItem", "refresh", bounds=(500, 332, 170, 32))])
        desk = node("Desktop", "Pane", "", "Progman", bounds=(0, 0, 1920, 1080),
                    children=[node("Recycle Bin", "ListItem", bounds=(0, 5, 76, 70))])

        def scene(slug, label, t, kind="surface"):
            return dict(slug=slug, label=label, kind=kind, open=lambda ctx: log.append(f"open {slug}"),
                        root=lambda ctx: core.FixtureElement(t), close=lambda ctx: log.append(f"close {slug}"))
        m = types.SimpleNamespace(SURFACE="scenetest", SCENES=[scene("desktop", "Desktop", desk),
                                                              scene("menu", "Desktop menu", menu, "contextMenu")], **kw)
        return m, log

    def walk_scene(self, sc):
        return core.walk(sc.root(None), surface="scenetest", build="26300.9974", profile="fhd-100", session="console",
                         root_slug=sc.slug, root_label=sc.label, root_kind=sc.kind, identity=sc.identity)

    def test_every_scene_is_walked_in_order_into_one_capture(self):
        m, log = self.mod()
        cap, passes = core.run_scenes(core.scenes_of(m), None, self.walk_scene)
        roots = [r for r in cap.records if "parent" not in r]
        self.assertEqual([(r["id"], r["kind"], r["order"]) for r in roots],
                         [("win11.scenetest.desktop", "surface", 0), ("win11.scenetest.menu", "contextMenu", 1)])
        self.assertEqual(log, ["open desktop", "close desktop", "open menu", "close menu"])
        self.assertEqual(passes, {"desktop": 2, "menu": 2})
        code, report = capture.check_scratch({"scenetest/scenetest.json": cap.records})
        self.assertEqual(code, 0, report)

    def test_a_root_with_no_area_takes_the_box_around_its_entries(self):
        # A WinUI menu's popup window reports an empty box (2026-11-16).
        m, _ = self.mod()
        cap, _ = core.run_scenes(core.scenes_of(m), None, self.walk_scene)
        by = {r["id"]: r for r in cap.records}
        self.assertEqual(by["win11.scenetest.menu"]["bounds"], {"x": 0, "y": 0, "width": 170, "height": 64})
        self.assertEqual(by["win11.scenetest.menu.refresh"]["bounds"], {"x": 0, "y": 32, "width": 170, "height": 32})
        empty = core.FixtureElement(node("", "Window", bounds=(0, 0, 0, 0)))
        with self.assertRaises(core.Incomplete):
            core.extent(empty)

    def test_a_failed_scene_is_closed_and_ends_the_capture(self):
        m, log = self.mod()
        m.SCENES[0]["root"] = lambda ctx: (_ for _ in ()).throw(core.Incomplete("gone"))
        with self.assertRaises(core.Incomplete):
            core.run_scenes(core.scenes_of(m), None, self.walk_scene)
        self.assertEqual(log, ["open desktop", "close desktop"])

    def test_a_measuring_scene_is_not_published(self):
        m, _ = self.mod()
        m.SCENES[1]["keep"] = False
        cap, _ = core.run_scenes(core.scenes_of(m), None, self.walk_scene)
        self.assertFalse([r for r in cap.records if r["id"].startswith("win11.scenetest.menu")])
        self.assertEqual(cap.measured[0]["id"], "win11.scenetest.menu")
        self.assertEqual([s["state"] for s in cap.states], [])

    def test_a_scene_that_does_not_expand_records_its_entries_only(self):
        t = node("", "Menu", bounds=(0, 0, 100, 60),
                 children=[node("View", "MenuItem", "view", expands=[node("Large icons", "MenuItem")])])
        args = dict(surface="scenetest", build="26300.9974", profile="fhd-100", session="console", root_slug="m",
                    root_label="M", root_kind="contextMenu")
        opened = core.walk(core.FixtureElement(t), **args)
        flat = core.walk(core.FixtureElement(t), expand=False, **args)
        self.assertIn("win11.scenetest.m.view.large-icons", [r["id"] for r in opened.records])
        self.assertEqual([r["id"] for r in flat.records], ["win11.scenetest.m", "win11.scenetest.m.view"])
        self.assertNotIn("itemCount", flat.records[1])
        self.assertNotIn("expands", core.dump(core.FixtureElement(t), expand=False)["children"][0])

    def test_a_win32_entry_keeps_its_shortcut_out_of_its_name(self):
        t = node("", "Menu", bounds=(0, 0, 100, 60), children=[node("Undo Copy\tCtrl+Z", "MenuItem")])
        cap = core.walk(core.FixtureElement(t), surface="scenetest", build="26300.9974", profile="fhd-100",
                        session="console", root_slug="m", root_label="M", root_kind="contextMenu")
        entry = cap.records[1]
        self.assertEqual((entry["id"], entry["label"], entry["shortcut"]), ("win11.scenetest.m.undo-copy", "Undo Copy", "Ctrl+Z"))

    def test_scene_slugs_are_slugs_and_distinct(self):
        m, _ = self.mod()
        m.SCENES[1]["slug"] = "desktop"
        with self.assertRaises(ValueError):
            core.scenes_of(m)
        m.SCENES[1]["slug"] = "Menu"
        with self.assertRaises(ValueError):
            core.scenes_of(m)
        m.SCENES[1]["slug"], m.SCENES[1]["colour"] = "menu", "red"
        with self.assertRaises(ValueError):
            core.scenes_of(m)

    def test_a_module_without_scenes_is_one_scene(self):
        (sc,) = core.scenes_of(smoke)
        self.assertEqual((sc.slug, sc.label, sc.kind), (smoke.ROOT_SLUG, smoke.ROOT_LABEL, getattr(smoke, "ROOT_KIND", "surface")))
        self.assertEqual(sc.environment_lists, tuple(smoke.ENVIRONMENT_LISTS))


class Authored(unittest.TestCase):
    def test_hand_and_metric_records_check_beside_the_capture(self):
        m, _ = Scenes().mod()
        cap, _ = core.run_scenes(core.scenes_of(m), None, Scenes().walk_scene)
        hand = core.authored([{"id": "desktop.rubber-band", "parent": "desktop", "kind": "gesture", "label": "Rubber-band selection",
                               "order": 0, "behaviour": "Drag on the empty area selects the icons the rectangle touches."},
                              {"id": "desktop.selected", "parent": "desktop", "kind": "state", "label": "Selected", "order": 1}],
                             surface="scenetest", build="26300.9974", source="hand")
        metric = core.authored([{"id": "desktop.cell-width", "parent": "desktop", "kind": "metric", "label": "Icon cell width",
                                 "order": 2, "measure": {"value": 76, "unit": "px"}}],
                               surface="scenetest", build="26300.9974", source="uia", profile="fhd-100", session="console")
        self.assertEqual(hand[0]["source"], "hand:26300.9974")
        self.assertNotIn("captureProfile", hand[0])
        self.assertEqual((metric[0]["source"], metric[0]["captureProfile"]), ("uia:26300.9974", "fhd-100"))
        code, report = capture.check_scratch({"scenetest/scenetest.json": cap.records + hand + metric})
        self.assertEqual(code, 0, report)

    def test_a_field_outside_the_list_is_refused(self):
        with self.assertRaises(ValueError):
            core.authored([{"id": "a.b", "kind": "state", "label": "x", "order": 0, "source": "uia:1.2"}],
                          surface="scenetest", build="26300.9974", source="hand")


class Modules(unittest.TestCase):
    def test_the_module_is_staged_and_named_to_the_guest(self):
        seen = {}
        real = capture.ref_run
        capture.ref_run = lambda label, files, cmd, timeout, checkpoint: seen.update(files=files, cmd=cmd) or ("out", 0)
        try:
            capture.run_guest("notepad-smoke", "fhd-100", 60, module="other")
        finally:
            capture.ref_run = real
        self.assertIn("surfaces/other.py", seen["files"])
        self.assertNotIn("surfaces/notepad-smoke.py", seen["files"])
        self.assertIn("--surface notepad-smoke --module other ", seen["cmd"])

    def test_a_missing_module_is_refused_before_the_vm(self):
        real = capture.run_guest
        capture.run_guest = lambda *a, **k: self.fail("the guest ran")
        try:
            err = io.StringIO()
            with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as ex:
                capture.main(["--surface", "notepad-smoke", "--module", "no-such-walker", "--file", "x", "--dry-run"])
            self.assertEqual(ex.exception.code, 2)
            self.assertIn("--module 'no-such-walker' has no walker", err.getvalue())
        finally:
            capture.run_guest = real

    def test_a_file_whose_ids_another_file_of_the_surface_holds_is_refused(self):
        # Two modules of one surface folder (taskbar and tray) must not write one id.
        m, _ = Scenes().mod()
        cap, _ = core.run_scenes(core.scenes_of(m), None, Scenes().walk_scene)
        code, report = capture.check_scratch({"scenetest/scenetest.json": cap.records, "scenetest/other.json": cap.records[:1]})
        self.assertEqual(code, 1)
        self.assertIn("duplicate-id", report)


class DesktopMetrics(unittest.TestCase):
    def setUp(self):
        self.desk = load("surface_desktop", INV / "surfaces" / "desktop.py")

    def cells(self):
        def cell(name, y, h):
            return {"id": f"win11.desktop.desktop.desktop.{name}", "parent": "win11.desktop.desktop.desktop",
                    "controlType": "listitem", "bounds": {"x": 0, "y": y, "width": 76, "height": h}}
        out = [cell("recycle-bin", 5, 70), cell("edge", 105, 85), cell("folder", 205, 70)]
        out += [{"id": f"win11.desktop.icons-{k}", "bounds": {"x": 0, "y": 0, "width": w, "height": h}}
                for k, w, h in (("large", 106, 118), ("medium", 76, 70), ("small", 76, 54))]
        return out

    def test_the_grid_is_measured_from_the_cells(self):
        got = {m["id"]: m["measure"]["value"] for m in self.desk.METRICS(self.cells())}
        self.assertEqual((got["desktop.cell-width"], got["desktop.cell-height-one-line"], got["desktop.label-line-height"],
                          got["desktop.row-pitch"], got["desktop.margin-top"], got["desktop.cell-height-small"]),
                         (76, 70, 15, 100, 5, 54))

    def test_a_missing_measuring_scene_fails_the_capture(self):
        with self.assertRaises(ValueError):
            self.desk.METRICS([c for c in self.cells() if c["id"] != "win11.desktop.icons-large"])

    def test_hand_records_and_metrics_do_not_share_an_order(self):
        orders = [m["order"] for m in self.desk.METRICS(self.cells())] + [h["order"] for h in self.desk.HAND]
        self.assertEqual(len(orders), len(set(orders)))


if __name__ == "__main__":
    unittest.main(verbosity=1)
