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


class Ctx(types.SimpleNamespace, core.Polls):
    """A surface Context stand-in: the attributes given, plus the real poll deadlines."""
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
        self.assertEqual(cap.skipped, {"offscreen": 1, "tooltip": 2, "prose": 1, "hosting": 0, "elsewhere": 0, "caption": 0})
        self.assertFalse([k for k in ("Help", "x" * 81) if k in json.dumps(cap.records)])

    def test_a_text_repeating_its_parents_name_is_the_parents_caption(self):
        t = node("Root", "Window", bounds=(0, 0, 500, 500), children=[
            node("Lock", "MenuItem", children=[node("Lock", "Text")]),
            node("Sign out", "Button", children=[node("Signed in", "Text")])])
        cap = core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="fhd-100", session="console", root_slug="r",
                        root_label="Root")
        self.assertEqual([r["label"] for r in cap.records[1:]], ["Lock", "Sign out", "Signed in"])
        self.assertEqual(cap.skipped["caption"], 1)

    def test_a_menu_root_counts_its_entries(self):
        t = node("Power options", "Menu", bounds=(0, 0, 100, 100), children=[
            node("Lock", "MenuItem"), node("Shut down", "MenuItem"), node("Restart", "MenuItem")])
        args = dict(surface="s", build="1.2", profile="fhd-100", session="console", root_slug="m", root_label="Menu")
        self.assertEqual(core.walk(core.FixtureElement(t), root_kind="menu", **args).records[0]["itemCount"], 3)
        self.assertNotIn("itemCount", core.walk(core.FixtureElement(t), root_kind="region", **args).records[0])
        self.assertEqual(core.walk(core.FixtureElement(t), root_kind="region", count=True, **args).records[0]["itemCount"], 3)

    def test_a_scene_root_can_name_its_parent(self):
        t = node("General", "Pane", bounds=(0, 0, 100, 100), children=[node("Read-only", "CheckBox")])
        args = dict(surface="s", build="1.2", profile="fhd-100", session="console", root_slug="sheet-general",
                    root_label="General", root_kind="dialogTab")
        root = core.walk(core.FixtureElement(t), root_parent="sheet", **args).records[0]
        self.assertEqual(root["parent"], "win11.s.sheet")
        self.assertNotIn("parent", core.walk(core.FixtureElement(t), **args).records[0])
        noop = lambda ctx: None  # noqa: E731
        self.assertEqual(core.Scene(None, slug="a", label="A", open=noop, root=noop, close=noop, parent="sheet").parent,
                         "sheet")
        with self.assertRaises(ValueError):
            core.Scene(None, slug="a", label="A", open=noop, root=noop, close=noop, parent="Not a slug")

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

    def run_walk(self, t, identity=(), kinds=()):
        return core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", session="console", root_slug="r",
                         root_label="R", identity=identity, kinds=kinds)

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

    def test_a_label_and_its_field_are_told_apart_by_type(self):
        # A Win32 sheet names a field after the label before it (Size: and its value, 2026-11-20).
        t = node("Root", "Window", children=[node("Size:", "Text"), node("Size:", "Edit"),
                                             node("Read", "Text"), node("Read", "Image"), node("Read", "Image")])
        cap = self.run_walk(t)
        self.assertEqual([r["id"] for r in cap.records[1:]],
                         ["win11.s.r.size-text", "win11.s.r.size-edit", "win11.s.r.read-text",
                          "win11.s.r.read--1", "win11.s.r.read--2"])            # same-type twins stay positional
        self.assertEqual(cap.ambiguous, ["win11.s.r.read--1", "win11.s.r.read--2"])

    def test_a_typed_twin_never_takes_a_siblings_id(self):
        # Before typed twins, the button alone was "size-edit"; it keeps that id and its note (review, 2026-11-20).
        t = node("Root", "Window", children=[node("Size", "Text"), node("Size", "Edit"), node("Size edit", "Button")])
        ids = [r["id"] for r in self.run_walk(t).records[1:]]
        self.assertEqual(ids, ["win11.s.r.size-text", "win11.s.r.size", "win11.s.r.size-edit"])
        old = [{"id": "win11.s.r.size-edit", "label": "Size edit", "behaviour": "Opens the size editor."}]
        new = [{"id": i, "label": "x"} for i in ids]
        out, dropped = core.merge(old, new, [])
        self.assertEqual(dropped, [])
        self.assertEqual(out[2]["behaviour"], "Opens the size editor.")

    def test_nth_names_twins_whose_order_is_their_identity(self):
        # The traditional touch keyboard's two Control keys share an id and a name.
        keys = [node("Control", "Button", "switchCtl"), node("Space", "Button", "39"), node("Control", "Button", "switchCtl")]
        t = node("Root", "Window", children=keys)
        identity = [({"automationId": "switchCtl", "nth": 1}, "left-control"),
                    ({"automationId": "switchCtl", "nth": 2}, "right-control")]
        cap = self.run_walk(t, identity=identity)
        self.assertEqual([r["id"] for r in cap.records[1:]],
                         ["win11.s.r.left-control", "win11.s.r.space", "win11.s.r.right-control"])
        self.assertEqual(cap.ambiguous, [])
        # Without the rule they are positional; an nth past the matches names nothing.
        self.assertEqual(len(self.run_walk(t).ambiguous), 2)
        self.assertEqual(len(self.run_walk(t, identity=[({"automationId": "switchCtl", "nth": 3}, "x")]).ambiguous), 2)
        # A lone match is no twin: the layouts with one Control key keep "switchctl".
        lone = node("Root", "Window", children=[node("Control", "Button", "switchCtl")])
        self.assertEqual([r["id"] for r in self.run_walk(lone, identity=identity).records[1:]], ["win11.s.r.switchctl"])

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

    def test_a_name_prefix_tells_apart_buttons_with_live_names(self):
        # The tray's volume and bell share a class; only their names' first word is fixed.
        t = node("Root", "Window", children=[node("Volume No audio device is installed", "Button", "Icon", "Omni"),
                                             node("Notifications No new notifications", "Button", "Icon", "Omni")])
        cap = core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", session="console", root_slug="r",
                        root_label="R", values=[{"className": "Omni"}],
                        identity=[({"namePrefix": "Volume"}, "volume"), ({"namePrefix": "Notifications"}, "bell")])
        self.assertEqual([r["id"] for r in cap.records[1:]], ["win11.s.r.volume", "win11.s.r.bell"])
        self.assertNotIn("audio", json.dumps(cap.records))

    def test_a_name_suffix_picks_out_snap_zones_by_position_on_every_display(self):
        # A snap zone is named for its place and its share of the screen, and
        # a portrait display renames every zone (2026-11-18): the zones are
        # told apart by their order, and only the buttons whose name ends in
        # the share are zones, so a suggestion button beside them keeps its name.
        def layout(*names):
            return node("2 window split layout", "Group", "LayoutGrid",
                        children=[node(n, "Button", cls="Button") for n in names] +
                        [node("Suggested", "Button", cls="Button")])
        identity = [({"className": "Button", "nameSuffix": "% height", "nth": k}, f"zone-{k}") for k in (1, 2)]
        for names in (("Left half, 50% width, 100% height", "Right half, 50% width, 100% height"),
                      ("Top half, 100% width, 50% height", "Bottom half, 100% width, 50% height")):
            cap = core.walk(core.FixtureElement(node("Root", "Window", children=[layout(*names)])), surface="s",
                            build="1.2", profile="p", session="console", root_slug="r", root_label="R",
                            identity=identity)
            self.assertEqual([r["id"] for r in cap.records[2:]],
                             ["win11.s.r.layoutgrid.zone-1", "win11.s.r.layoutgrid.zone-2",
                              "win11.s.r.layoutgrid.suggested"])

    def test_a_class_prefix_matches_whole_leading_words(self):
        # Search's best match row gains state classes under the pointer; the
        # prefix is whole words, so "suggestion" never matches "suggestions".
        row = core.FixtureElement(node("x", "ListItem", cls="suggestion cf selectable sa_hv selected"))
        self.assertTrue(core.matches(row, {"classPrefix": "suggestion cf"}))
        self.assertFalse(core.matches(row, {"classPrefix": "suggestion selectable"}))
        self.assertFalse(core.matches(core.FixtureElement(node("x", "List", cls="suggestions")),
                                      {"classPrefix": "suggestion"}))
        self.assertFalse(core.matches(core.FixtureElement(node("x", "List")), {"classPrefix": "suggestion"}))

    def test_a_shot_box_is_clipped_to_the_screen(self):
        # The emoji panel's page reports scrolled-away rows far below the
        # screen; the shot keeps the panel and stops at the screen's edge.
        screen = (0, 0, 1920, 1080)
        panel, rows = (1559, 627, 360, 400), (1575, 808, 328, 15000)
        self.assertEqual(core.clip_box([panel, rows], screen), (1559, 627, 1919, 1080))
        self.assertEqual(core.clip_box([(10, 10, 20, 20)], screen), (10, 10, 30, 30))
        with self.assertRaises(core.Incomplete):
            core.clip_box([(2000, 0, 50, 50)], screen)

    def test_an_automation_id_prefix_matches_the_start_of_the_id(self):
        # The emoji panel's lists carry their category and a URL-encoded query
        # in the id; the rows' containers share the stem and are ListItems.
        grid = core.FixtureElement(node("Emoji", "List", "expanded-view-list-Emoji-Most%20frequently%20used"))
        spec = {"controlType": "List", "automationIdPrefix": "expanded-view-list-"}
        self.assertTrue(core.matches(grid, spec))
        self.assertFalse(core.matches(core.FixtureElement(
            node("Emoji", "ListItem", "expanded-view-list-container-Emoji")), spec))
        self.assertFalse(core.matches(core.FixtureElement(node("Emoji", "List", "view-list-Emoji")), spec))
        self.assertFalse(core.matches(core.FixtureElement(node("Emoji", "List")), spec))

    def test_kinds_set_the_record_kind_a_control_type_would_get_wrong(self):
        t = node("Actions", "List", children=[node("Open", "ListItem", "pp_S_Open"), node("Copy path", "ListItem")])
        cap = self.run_walk(t, kinds=[({"automationId": "pp_S_Open"}, "menuItem")])
        self.assertEqual([r["kind"] for r in cap.records[1:]], ["menuItem", "control"])
        self.assertEqual([r["kind"] for r in self.run_walk(t).records[1:]], ["control", "control"])


class RestartExplorer(unittest.TestCase):
    class Ctx:
        def __init__(self, fails):
            self.fails, self.scripts = fails, []

        def powershell(self, script):
            self.scripts.append(script)

        def find_window(self, className, timeout):
            return className

        def find(self, within, **spec):
            if self.fails:
                self.fails -= 1
                raise core.Incomplete("not yet")
            return (within, spec["automationId"])

    def test_it_waits_for_the_bar_to_come_back(self):
        ctx = self.Ctx(fails=2)
        core.restart_explorer(ctx, "Shell_SecondaryTrayWnd", clock=lambda: 0)
        self.assertEqual(ctx.fails, 0)
        self.assertIn("Stop-Process -Name explorer", ctx.scripts[0])

    def test_a_bar_that_never_returns_is_incomplete(self):
        now = iter([0, 50, 100])
        with self.assertRaisesRegex(core.Incomplete, "did not come back within 90 s"):
            core.restart_explorer(self.Ctx(fails=99), clock=lambda: next(now))


class ClockSeconds(unittest.TestCase):
    """The seconds scene checks the time for its seconds, never the clock's width,
    which follows whichever line is wider (2026-11-17)."""

    def setUp(self):
        sys.modules.setdefault("capture_core", core)
        self.tray = load("surface_tray", INV / "surfaces" / "tray.py")
        self.names = []
        self.tray.entry = lambda ctx, word: types.SimpleNamespace(name=self.names.pop(0) if len(self.names) > 1
                                                                  else self.names[0], bounds=(0, 0, 70, 48))
        self.tray.core = types.SimpleNamespace(restart_explorer=lambda ctx: None, Incomplete=core.Incomplete)
        self.tray.time = types.SimpleNamespace(sleep=lambda s: None, monotonic=time.monotonic)
        self.ctx = types.SimpleNamespace(powershell=lambda script: None)

    def test_seconds_in_the_time_pass_at_the_same_width(self):
        self.names = ["Clock 1:11 AM 11/17/2026", "Clock 1:11:52 AM 11/17/2026"]
        self.tray.open_seconds(self.ctx, clock=lambda: 0)

    def test_a_time_without_seconds_is_incomplete(self):
        self.names = ["Clock 1:11 AM 11/17/2026"]
        now = iter([0, 10, 40])
        with self.assertRaisesRegex(core.Incomplete, "showed no seconds"):
            self.tray.open_seconds(self.ctx, clock=lambda: next(now))


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


class PublishedRoundTrip(unittest.TestCase):
    """A published file merges onto an unchanged recapture of itself: a hand
    field on a positional id would refuse every later recapture (Start's account
    texts, 2026-11-17)."""

    def test_every_published_file_merges_with_itself(self):
        for path in sorted((INV.parents[1] / "docs" / "design" / "inventory").rglob("*.json")):
            lines = [ln for ln in path.read_text(encoding="utf-8").splitlines() if ln.strip()]
            try:
                recs = [json.loads(ln) for ln in lines]
            except ValueError:
                continue                    # a whole-file JSON document, not a record file
            if not recs or not all(isinstance(r, dict) and str(r.get("id", "")).startswith("win11.") for r in recs):
                continue
            with self.subTest(file=str(path.relative_to(INV.parents[1]))):
                out, dropped = core.merge(recs, copy.deepcopy(recs), [])
                self.assertEqual(dropped, [])
                self.assertEqual(out, recs)


class StartApps(unittest.TestCase):
    def setUp(self):
        self.start = load("surface_start", INV / "surfaces" / "start.py")

    def ids(self, apps):
        self.start._apps[:] = apps
        return {r["uiaId"]: r["id"] for r in self.start.app_records() if "uiaId" in r}

    A, B, C = ("P~Vendor.EditorA!App", "Editor", None), ("P~Vendor.EditorB!App", "Editor, New", None), \
        ("P~Vendor.Clock!App", "Clock", None)

    def test_apps_sharing_a_name_keep_their_ids_when_the_list_is_reordered(self):
        first, second = self.ids([self.A, self.B, self.C]), self.ids([self.C, self.B, self.A])
        self.assertEqual(first, second)
        self.assertNotEqual(first[self.A[0]], first[self.B[0]])
        self.assertNotIn("--", "".join(first.values()))

    def test_a_new_sibling_with_the_same_name_moves_no_id(self):
        # Round review 2026-11-17: a name-keyed id switched to the AppID only
        # once a second app took the name, dropping the first app's notes.
        alone, shared = self.ids([self.A, self.C]), self.ids([self.A, self.B, self.C])
        self.assertEqual(alone[self.A[0]], shared[self.A[0]])
        self.assertEqual(alone[self.C[0]], shared[self.C[0]])

    def test_appids_a_slug_would_merge_keep_their_own_ids(self):
        # Round review 2026-11-17: the slug drops punctuation and cuts at 60
        # characters, so these pairs shared an id.
        long = "P~{6D809377-6AF0-444B-8957-A3773F02200E}\\Windows Kits\\10\\Debuggers\\x64\\"
        pairs = [(("P~Vendor.Editor!App", "Editor", None), ("P~Vendor-Editor!App", "Editor", None)),
                 ((long + "windbg.exe", "WinDbg", None), (long + "cdb.exe", "Cdb", None))]
        for a, b in pairs:
            ids = self.ids([a, b])
            self.assertNotEqual(ids[a[0]], ids[b[0]])
            self.assertEqual(self.ids([a])[a[0]], ids[a[0]])

    def _folder(self, pages, percents, steps=5):
        # pages[i]: the folder's rows while the grid shows page i.
        at = [0]

        class Scroll:
            CurrentVerticallyScrollable = True
            CurrentVerticalScrollPercent = property(lambda self: percents[at[0]])

            def Scroll(self, h, v):
                at[0] = min(at[0] + 1, len(pages) - 1)
        sc = Scroll()
        ctx = types.SimpleNamespace(find=lambda **kw: "grid")
        row = lambda aid, off: types.SimpleNamespace(automation_id=aid, name=aid, offscreen=off,
                                                     control_type="ListItem")
        self.start._apps.clear()
        with mock.patch.object(self.start, "_walk_rows", lambda c, m: [row(*r) for r in pages[at[0]]]), \
                mock.patch.object(self.start, "_scroll", lambda c, g: sc), \
                mock.patch.object(self.start, "SCAN_STEPS", steps), mock.patch.object(self.start.time, "sleep", lambda s: None):
            self.start._read_folder(ctx, "modal", set(), {}, "Tools")
        return [a for a, _, _ in self.start._apps]

    def test_a_folder_longer_than_its_grid_is_read_to_its_end(self):
        # Round review 2026-11-17: a folder was read once, so apps below its
        # fold were left out without a word.
        pages = [[("P~A!App", False), ("P~B!App", True)], [("P~A!App", True), ("P~B!App", False)]]
        self.assertEqual(self._folder(pages, [0.0, 100.0]), ["P~A!App", "P~B!App"])

    def test_a_folder_that_never_reaches_its_end_fails_the_capture(self):
        with self.assertRaises(self.start.core.Incomplete):
            self._folder([[("P~A!App", False), ("P~B!App", True)]], [0.0])

    def test_the_panel_edges_come_from_the_screen_not_the_scene_root(self):
        # Round review 2026-11-17: the left scene is rooted at the panel, and a
        # walk measures from its root, so its x was always 0.
        self.start.PANEL_AT.clear()
        ctx = types.SimpleNamespace(powershell=lambda *a, **k: None)
        bounds = iter([(531, 142, 858, 890), (24, 142, 858, 890)])
        with mock.patch.object(self.start, "open_start", lambda c: None), \
                mock.patch.object(self.start, "panel", lambda c: types.SimpleNamespace(bounds=next(bounds))), \
                mock.patch.object(self.start.time, "sleep", lambda s: None):
            self.start.open_centre(ctx)
            self.start.open_left(ctx)
        self.assertEqual(self.start.panel_edges(), (531, 24))

    def test_an_unread_panel_edge_fails_the_capture(self):
        self.start.PANEL_AT.clear()
        with self.assertRaises(ValueError):
            self.start.panel_edges()

    def test_an_app_moved_out_of_its_folder_loses_the_folder_text(self):
        self.start._apps[:] = [("P~Vendor.Clock!App", "Clock", "Tools")]
        old = [r for r in self.start.app_records() if "uiaId" in r]
        self.start._apps[:] = [("P~Vendor.Clock!App", "Clock", None)]
        out, _ = core.merge(old, [r for r in self.start.app_records() if "uiaId" in r], [])
        self.assertNotIn("behaviour", out[0])

    def test_an_app_replaced_by_one_of_the_same_name_gets_a_new_id(self):
        # Otherwise merge carries the old app's baseline link onto the new one.
        self.assertNotEqual(self.ids([self.A])[self.A[0]], self.ids([self.B])[self.B[0]])


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

    def test_what_the_module_writes_wins_over_the_published_copy(self):
        # A surface module that writes a field (a hand record's behaviour, an
        # app's baseline link) is its author: the published copy of that field
        # is replaced, while fields only a person adds are still carried.
        # Reviews 2026-11-17: edited behaviour text never reached the published
        # file, and an app replaced by another kept the old app's baseline link.
        old = [{"id": "a.b", "behaviour": "Old text.", "baselineId": "start-apps:Old", "disposition": "build"}]
        new = [{"id": "a.b", "behaviour": "New text.", "baselineId": "start-apps:New"}]
        out, _ = core.merge(old, new, [])
        self.assertEqual(out, [{"id": "a.b", "behaviour": "New text.", "baselineId": "start-apps:New",
                                "disposition": "build"}])

    def test_a_field_the_capture_owns_without_a_value_is_removed(self):
        # Round review 2026-11-17: an app that leaves its folder has no folder
        # text; absence alone would bring the old text back.
        old = [{"id": "a.b", "behaviour": "In the Tools folder.", "disposition": "build"}]
        out, _ = core.merge(old, [{"id": "a.b", "behaviour": None}], [])
        self.assertEqual(out, [{"id": "a.b", "disposition": "build"}])

    def test_a_tiles_states_survive_a_recapture(self):
        # Quick Settings' tile states are measured by clicking, not walked (2026-11-17).
        old = [{"id": "q.night", "label": "Night light", "toggleStates": ["off", "on"]}]
        out, _ = core.merge(old, [{"id": "q.night", "label": "Night light", "order": 4}], [])
        self.assertEqual(out, [{"id": "q.night", "label": "Night light", "order": 4, "toggleStates": ["off", "on"]}])

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

    def _bound(self, old, new):
        with tempfile.TemporaryDirectory() as d, mock.patch.object(capture, "INVENTORY", pathlib.Path(d)):
            inv = pathlib.Path(d)
            (inv / "controls").mkdir()
            (inv / "controls" / "baseline.json").write_text('{"baseline": "controls"}\n{"id": "Button"}\n')
            (inv / "taskbar").mkdir()
            core.write_records(inv / "taskbar" / "bar.json", old)
            return capture.build_file(inv / "taskbar" / "bar.json", new, {"complete": True}, True)

    def test_a_capture_match_to_a_new_row_replaces_a_missing_flag(self):
        # Round review 2026-11-17: once the baseline gains the row, the
        # capture's match and the carried flag would both stand, and the
        # checker refuses that pair.
        out = self._bound([{"id": "a.b", "label": "B", "baselineMissing": True}],
                          [{"id": "a.b", "label": "B", "baselineId": "controls:Button"}])
        self.assertEqual(out, [{"id": "a.b", "label": "B", "baselineId": "controls:Button"}])

    def test_a_capture_match_to_an_absent_row_does_not_bring_back_the_old_match(self):
        # Round review 2026-11-17: the capture's match was removed before merge,
        # so merge refilled the old, superseded one.
        out = self._bound([{"id": "a.b", "label": "B", "baselineId": "controls:Button"}],
                          [{"id": "a.b", "label": "B", "baselineId": "controls:Gone"}])
        self.assertEqual(out, [{"id": "a.b", "label": "B"}])

    def test_a_capture_match_to_an_absent_row_keeps_the_missing_flag(self):
        out = self._bound([{"id": "a.b", "label": "B", "baselineMissing": True}],
                          [{"id": "a.b", "label": "B", "baselineId": "controls:Gone"}])
        self.assertEqual(out, [{"id": "a.b", "label": "B", "baselineMissing": True}])

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

    def test_a_failed_script_reports_what_it_printed(self):
        self.assertEqual(guest.script_reason("the file is not held\r\n", "\r\n"), "the file is not held")
        self.assertEqual(guest.script_reason("out", "Access is denied."), "Access is denied.")
        self.assertEqual(guest.script_reason("", ""), "it printed nothing")

    def test_a_chord_names_an_unknown_key(self):
        self.assertEqual(guest.chord_codes("shift+delete"), [0x10, 0x2E])
        self.assertEqual(guest.chord_codes("ctrl+c"), [0x11, ord("C")])
        with self.assertRaises(ValueError) as ex:
            guest.chord_codes("shift+del")
        self.assertIn("key 'del' in 'shift+del'", str(ex.exception))

    def test_a_defect_in_a_module_is_named_as_the_reason(self):
        class Com(Exception):
            pass
        try:
            None > 1                                   # noqa: B015 - the TypeError a module raised
        except TypeError as exc:
            reason, expected = guest.failure_reason(exc, Com)
        self.assertFalse(expected)
        self.assertTrue(reason.startswith("TypeError in the capture: "), reason)
        self.assertIn("test_capture.py", reason)
        reason, expected = guest.failure_reason(guest.core.Incomplete("no element"), Com)
        self.assertEqual((reason, expected), ("no element", True))

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

    def test_a_thumbnail_skips_hyper_vs_leading_bytes(self):
        # Hyper-V puts 4 bytes BEFORE the pixels at 1920 by 1080; reading from
        # byte 0 shifted every host shot 2 pixels right, and a guest screenshot
        # of the same desktop showed it (2026-11-18).
        shell, script = self._powershell(INV / "ref-common.ps1", windows=True)
        ps = ("$a = [System.Management.Automation.Language.Parser]::ParseFile('%s', [ref]$null, [ref]$null); "
              "$f = $a.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Save-Thumbnail' }, $true) | Select-Object -First 1; "
              "Invoke-Expression $f.Extent.Text; "
              "$png = Join-Path ([IO.Path]::GetTempPath()) ('ios-th-' + [Guid]::NewGuid().ToString('N') + '.png'); "
              "Save-Thumbnail ([byte[]](0x11, 0x22, 0x33, 0x44, 0x00, 0xF8, 0x1F, 0x00, 0xE0, 0x07, 0xFF, 0xFF)) $png 2 2; "
              "$b = [System.Drawing.Bitmap]::FromFile($png); "
              "foreach ($xy in @(0, 0), @(1, 0), @(0, 1), @(1, 1)) { $c = $b.GetPixel($xy[0], $xy[1]); '{0},{1},{2}' -f $c.R, $c.G, $c.B }; "
              "$b.Dispose(); Remove-Item -LiteralPath $png; "
              "try { Save-Thumbnail ([byte[]](0x11, 0x22, 0x00, 0xF8, 0x1F, 0x00)) $png 2 1; 'saved' } catch { 'refused' }") % script
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip()]
        # Rows cross a boundary at the third pixel; a length with another
        # prefix (2 bytes here) is a layout nobody checked, and is refused.
        self.assertEqual(lines, ["255,0,0", "0,0,255", "0,255,0", "255,255,255", "refused"], out.stderr)

    def test_a_shot_waits_out_hyper_vs_invalid_state_during_a_reset(self):
        # A burst frame taken as the VM reset after the stop error failed the
        # whole walk with "thumbnail failed: 32775" (2026-11-18).
        shell, script = self._powershell(INV / "ref-host.ps1", windows=True)
        ps = ("$a = [System.Management.Automation.Language.Parser]::ParseFile('%s', [ref]$null, [ref]$null); "
              "foreach ($fn in 'Get-ShotMode', 'Save-Shot', 'Get-ShotPixels') { "
              "$f = $a.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $fn }, $true) | Select-Object -First 1; "
              "Invoke-Expression $f.Extent.Text }; "
              "$script:now = [datetime]'2026-01-01'; function Get-Date { $script:now }; "
              "function Start-Sleep([int]$Milliseconds) { $script:now = $script:now.AddMilliseconds($Milliseconds) }; "
              "function Get-RefVideoMode { @(2, 1, 16) }; function Save-Thumbnail { }; "
              "$script:calls = 0; function Get-RefThumbnail { $script:calls++; if ($script:calls -le 2) { throw 'thumbnail failed: 32775' }; [byte[]](0) }; "
              "$Name = 'x'; $Out = [IO.Path]::GetTempPath(); $shots = New-Object System.Collections.ArrayList; "
              "Save-Shot 'a'; 'calls=' + $script:calls; 'exact=' + $shots[0].exact; "
              "$script:calls = -100; try { Save-Shot 'b'; 'saved' } catch { 'gave up' }; "
              # One deadline for the whole shot: 10 s waiting for the head, then
              # 9 s for the thumbnail, is past 15 s and gives up (review, 2026-11-18).
              "$script:heads = 0; function Get-RefVideoMode { $script:heads++; if ($script:heads -le 20) { throw 'no video head on x' }; @(2, 1, 16) }; "
              "$script:calls = -18; try { Save-Shot 'c'; 'saved' } catch { 'gave up' }") % script
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip() and not line.startswith("ref-host:")]
        self.assertEqual(lines, ["calls=3", "exact=True", "gave up", "gave up"], out.stderr)

    def _ps_functions(self, *pairs):
        """PowerShell that defines the named functions of each (script, names)."""
        out = []
        for path, names in pairs:
            script = self._powershell(path, windows=True)[1]
            out.append("$a = [System.Management.Automation.Language.Parser]::ParseFile('%s', [ref]$null, [ref]$null); "
                       "foreach ($fn in %s) { "
                       "$f = $a.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $fn }, $true) | Select-Object -First 1; "
                       "Invoke-Expression $f.Extent.Text }; " % (script, ", ".join(f"'{n}'" for n in names)))
        return "".join(out)

    def test_await_colour_waits_for_the_pixel_on_a_small_thumbnail(self):
        # The boot walk timed its menu by a burst's length, then by the mode,
        # which comes back while the spinner still shows; then a native-size
        # image per poll and a mode read that gave up a 90 s wait after 15 s
        # (2026-11-18). A 160 by 90 thumbnail, no mode read, its own deadline.
        shell = self._powershell(INV / "ref-host.ps1", windows=True)[0]
        ps = (self._ps_functions((INV / "ref-host.ps1", ["Test-ThumbnailColour", "Invoke-Step"]),
                                 (INV / "ref-common.ps1", ["Get-ThumbnailPixel"]))
              + "$script:now = [datetime]'2026-01-01'; function Get-Date { $script:now }; "
              "function Start-Sleep([int]$Milliseconds) { $script:now = $script:now.AddMilliseconds($Milliseconds) }; "
              "function Get-RefVideoMode { throw 'no video head on x' }; $Name = 'x'; $NoSession = $false; "
              "function Img([int]$lo, [int]$hi) { $b = New-Object byte[] (4 + 160 * 90 * 2); for ($i = 4; $i -lt $b.Length; $i += 2) { $b[$i] = $lo; $b[$i + 1] = $hi }; ,$b }; "
              # black, then 32775 (a reset), then blue (33, 101, 181) as RGB565 0x2336
              "$script:sizes = @(); $script:i = 0; function Get-RefThumbnail($n, $w, $h) { $script:sizes += \"$w x $h\"; $script:i++; "
              "if ($script:i -eq 1) { return (Img 0 0) }; if ($script:i -eq 2) { throw 'thumbnail failed: 32775' }; return (Img 0x36 0x23) }; "
              "(Get-ThumbnailPixel (Img 0x36 0x23) 160 90 159 89) -join ','; "
              "Invoke-Step ([pscustomobject]@{ op = 'awaitcolour'; x = 10; y = 10; rgb = @(33, 101, 181); timeout = 90 }); 'reads=' + $script:i; "
              "($script:sizes | Select-Object -Unique) -join ';'; "
              "function Get-RefThumbnail { Img 0 0 }; "
              "try { Invoke-Step ([pscustomobject]@{ op = 'awaitcolour'; x = 10; y = 10; rgb = @(33, 101, 181); timeout = 30 }); 'returned' } catch { 'timed out' }; "
              "'waited=' + ($script:now - [datetime]'2026-01-01').TotalSeconds; "
              # With a mode: blue at the display's mode is the page before; a
              # missing head is waited out; blue at the firmware's mode is the menu.
              "function Get-RefThumbnail($n, $w, $h) { $script:asked = \"$w x $h\"; $b = New-Object byte[] (4 + $w * $h * 2); for ($i = 4; $i -lt $b.Length; $i += 2) { $b[$i] = 0x36; $b[$i + 1] = 0x23 }; ,$b }; $script:m = 0; "
              "function Get-RefVideoMode { $script:m++; if ($script:m -eq 1) { return @(1920, 1080, 32) }; if ($script:m -eq 2) { throw 'no video head on x' }; @(1024, 768, 32) }; "
              "Invoke-Step ([pscustomobject]@{ op = 'awaitcolour'; x = 10; y = 10; rgb = @(33, 101, 181); width = 1024; height = 768; timeout = 30 }); 'modes=' + $script:m + ' ' + $script:asked")
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip()]
        self.assertEqual(lines[:4], ["33,101,181", "reads=3", "160 x 90", "timed out"], out.stderr)
        self.assertGreaterEqual(float(lines[4].split("=")[1]), 30.5, out.stderr)   # the whole 30 s, after the first wait
        self.assertEqual(lines[5], "modes=3 160 x 120", out.stderr)   # the 4:3 screen's own aspect

    def test_a_burst_with_until_ends_at_the_first_matching_frame(self):
        # A fixed burst could outrun the boot menu's 30 s countdown (review, 2026-11-18).
        shell = self._powershell(INV / "ref-host.ps1", windows=True)[0]
        ps = (self._ps_functions((INV / "ref-host.ps1", ["Test-ThumbnailColour", "Invoke-Step"]),
                                 (INV / "ref-common.ps1", ["Get-ThumbnailPixel"]))
              + "function Start-Sleep { }; $Name = 'x'; $NoSession = $false; $script:names = @(); "
              "function Img([int]$lo, [int]$hi) { $b = New-Object byte[] (4 + 4 * 2 * 2); for ($i = 4; $i -lt $b.Length; $i += 2) { $b[$i] = $lo; $b[$i + 1] = $hi }; ,$b }; "
              "function Save-Shot($n) { $script:names += $n; $c = if ($script:names.Count -ge 3) { Img 0x36 0x23 } else { Img 0 0 }; "
              "$script:LastShot = @{ Bytes = $c; Width = 4; Height = 2 } }; "
              "Invoke-Step ([pscustomobject]@{ op = 'burst'; name = 'boot'; count = 14; every = 0; until = [pscustomobject]@{ x = 10; y = 10; rgb = @(33, 101, 181) } }); "
              "$script:names -join ','; $script:names = @(); "
              "Invoke-Step ([pscustomobject]@{ op = 'burst'; name = 'stop'; count = 5; every = 0 }); $script:names.Count")
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip()]
        self.assertEqual(lines, ["boot-1,boot-2,boot-3", "5"], out.stderr)

    def test_a_burst_watches_for_its_seconds_whatever_a_shot_costs(self):
        # A frame count timed the boot burst by host speed (review, 2026-11-18).
        shell = self._powershell(INV / "ref-host.ps1", windows=True)[0]
        ps = (self._ps_functions((INV / "ref-host.ps1", ["Test-ThumbnailColour", "Invoke-Step"]),
                                 (INV / "ref-common.ps1", ["Get-ThumbnailPixel"]))
              + "$Name = 'x'; $NoSession = $false; $script:t = [datetime]'2026-01-01'; "
              "function Get-Date { $script:t }; function Start-Sleep([int]$Milliseconds) { $script:t = $script:t.AddMilliseconds($Milliseconds) }; "
              "function Save-Shot($n) { $script:names += $n; $script:t = $script:t.AddMilliseconds($script:cost); "
              "$script:LastShot = @{ Bytes = (New-Object byte[] 20); Width = 4; Height = 2 } }; "
              "$step = [pscustomobject]@{ op = 'burst'; name = 'boot'; count = 150; every = 0; cadence = 1; seconds = 30 }; "
              "foreach ($c in 200, 2500) { $script:cost = $c; $script:names = @(); $script:t = [datetime]'2026-01-01'; "
              "Invoke-Step $step; '' + $script:names.Count + ' ' + ($script:t - [datetime]'2026-01-01').TotalSeconds }")
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip()]
        # 0.2 s a shot: one a second for 30 s. 2.5 s a shot: back to back, 12 in 30 s.
        self.assertEqual(lines, ["30 30", "12 30"], out.stderr)

    def _cim_fakes(self):
        """The real shot, mode, thumbnail and wait helpers over fake CIM calls
        on a fake clock; each call logs when it starts (seconds from t0), costs
        (Cost) ms, and the video head is missing while (Gone) holds."""
        return (self._ps_functions((INV / "ref-host.ps1", ["Test-ThumbnailColour", "Invoke-Step", "Save-Shot", "Get-ShotMode", "Get-ShotPixels", "Save-FailureShot"]),
                                   (INV / "ref-common.ps1", ["Get-ThumbnailPixel", "Get-RefThumbnail", "Get-RefVideoMode", "Assert-RefBefore", "Get-ThumbnailHash"]))
                + "$Name = 'x'; $NoSession = $false; $Out = '.'; $script:Ns = 'n'; $shots = New-Object System.Collections.ArrayList; "
                "$t0 = [datetime]'2026-01-01'; $script:t = $t0; $script:calls = @(); $script:thumbs = 0; "
                "function Get-Date { $script:t }; function Start-Sleep([int]$Milliseconds) { $script:t = $script:t.AddMilliseconds($Milliseconds) }; "
                "function Write-Host { }; function Save-Thumbnail { }; "
                "function Call { $script:calls += ($script:t - $t0).TotalSeconds; $script:t = $script:t.AddMilliseconds((Cost)) }; "
                "function Get-RefSystem { Call; 'vm' }; function Get-CimInstance { Call; 'svc' }; "
                "function Get-CimAssociatedInstance($InputObject, $ResultClassName) { Call; "
                "if ($ResultClassName -eq 'Msvm_VideoHead') { if (Gone) { $null } else { [pscustomobject]@{ CurrentHorizontalResolution = 1024; CurrentVerticalResolution = 768; CurrentBitsPerPixel = 16 } } } "
                "else { [pscustomobject]@{ VirtualSystemType = 'Microsoft:Hyper-V:System:Realized' } } }; "
                "function Invoke-CimMethod { Call; $script:thumbs++; [pscustomobject]@{ ReturnValue = 0; ImageData = (New-Object byte[] 20) } }; ")

    def test_a_slow_shot_or_colour_wait_starts_no_host_call_past_its_deadline(self):
        # A standalone shot whose mode read ended after 15 s went on to the
        # thumbnail, and a colour wait's poll ran its CIM calls past the
        # wait's end (review, 2026-11-18).
        shell = self._powershell(INV / "ref-host.ps1", windows=True)[0]
        tail = ("'' + $script:thumbs + ' ' + (($script:calls | Measure-Object -Maximum).Maximum).ToString([cultureinfo]::InvariantCulture)")
        solo = ("function Cost { 16000 }; function Gone { $false }; "
                "try { Save-Shot 'solo'; 'saved' } catch { $_.Exception.Message }; " + tail)
        wait = ("function Cost { 400 }; function Gone { $false }; "
                "try { Invoke-Step ([pscustomobject]@{ op = 'awaitcolour'; x = 10; y = 10; rgb = @(33, 101, 181); timeout = 1 }); 'shown' } "
                "catch { $_.Exception.Message }; " + tail)
        out = {}
        for k, case in (("solo", solo), ("wait", wait)):
            r = subprocess.run([shell, "-NoProfile", "-Command", self._cim_fakes() + case], capture_output=True, text=True, timeout=120)
            out[k] = [line.strip() for line in r.stdout.splitlines() if line.strip()] or [r.stderr]
        # The mode read's first call took 16 s: its second call, the
        # thumbnail and the second mode read never start, and the shot fails.
        self.assertEqual(out["solo"][0], "shot solo: no frame within 15 s", out)
        self.assertEqual(out["solo"][1], "0 0", out)                      # one call, at 0 s
        # Calls at 0, 0.4 and 0.8 s; the fourth would start after 1 s.
        self.assertTrue(out["wait"][0].startswith("the pixel at (10, 10) did not show"), out)
        self.assertEqual(out["wait"][1], "0 0.8", out)

    def test_waits_stop_their_host_calls_and_sleeps_at_their_deadline(self):
        # A sized colour wait's mode read, its poll sleep and a settle poll
        # could each run past the wait's end (review, 2026-11-18).
        shell = self._powershell(INV / "ref-host.ps1", windows=True)[0]
        tail = ("'' + $script:calls.Count + ' ' + (($script:calls | Measure-Object -Maximum).Maximum).ToString([cultureinfo]::InvariantCulture) "
                "+ ' ' + ($script:t - $t0).TotalSeconds.ToString([cultureinfo]::InvariantCulture)")
        colour = "Invoke-Step ([pscustomobject]@{ op = 'awaitcolour'; x = 10; y = 10; rgb = @(33, 101, 181); width = 1920; height = 1080; timeout = 1 })"
        cases = {
            # The mode read's first call uses the whole second: the video head
            # lookup never starts.
            "first": "function Cost { 1200 }; function Gone { $false }; try { %s } catch { 'timed out' }; " % colour,
            # The mode never matches (1024x768): polls at 0, 0.35 and 0.7 s,
            # and the last sleep stops at 1 s, not 1.05.
            "mismatch": "function Cost { 50 }; function Gone { $false }; try { %s } catch { 'timed out' }; " % colour,
            # A settle whose thumbnail calls cross the end: calls at 0, 0.4
            # and 0.8 s, and none at 1.2.
            "settle": "function Cost { 400 }; function Gone { $false }; "
                      "try { Invoke-Step ([pscustomobject]@{ op = 'settle'; sec = 5; timeout = 1 }) } catch { 'timed out' }; ",
        }
        got = {}
        for k, case in cases.items():
            r = subprocess.run([shell, "-NoProfile", "-Command", self._cim_fakes() + case + tail], capture_output=True, text=True, timeout=120)
            got[k] = [line.strip() for line in r.stdout.splitlines() if line.strip()] or [r.stderr]
        self.assertEqual(got["first"], ["timed out", "1 0 1.2"], got)
        self.assertEqual(got["mismatch"], ["timed out", "6 0.75 1"], got)
        self.assertEqual(got["settle"], ["timed out", "3 0.8 1.2"], got)
        # A settle poll that gets its thumbnail at 0.8 s sleeps only the 0.2 s
        # left and times out at 1 s.
        settle = ("function Cost { 200 }; function Gone { $false }; "
                  "try { Invoke-Step ([pscustomobject]@{ op = 'settle'; sec = 5; timeout = 1 }) } catch { $_.Exception.Message }; ")
        r = subprocess.run([shell, "-NoProfile", "-Command", self._cim_fakes() + settle + tail], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in r.stdout.splitlines() if line.strip()]
        self.assertEqual(lines, ["the screen did not settle for 5 s within 1 s", "4 0.6 1"], (r.stdout, r.stderr))

    def test_a_burst_sleeps_neither_past_its_end_nor_after_its_last_frame(self):
        # every = 30 with 10 s left slept 30 s, and the repair burst slept 3 s
        # after its last frame (review, 2026-11-18).
        shell = self._powershell(INV / "ref-host.ps1", windows=True)[0]
        ps = (self._ps_functions((INV / "ref-host.ps1", ["Test-ThumbnailColour", "Invoke-Step"]),
                                 (INV / "ref-common.ps1", ["Get-ThumbnailPixel"]))
              + "$Name = 'x'; $NoSession = $false; $t0 = [datetime]'2026-01-01'; "
              "function Get-Date { $script:t }; function Start-Sleep([int]$Milliseconds) { $script:t = $script:t.AddMilliseconds($Milliseconds) }; "
              "function Save-Shot($n, $Until) { $script:t = $script:t.AddMilliseconds(100); $script:LastShot = @{ Bytes = (New-Object byte[] 20); Width = 4; Height = 2 } }; "
              "foreach ($s in @(@{ count = 5; every = 30; seconds = 10 }, @{ count = 1; every = 3 }, @{ count = 2; every = 3 })) { $script:t = $t0; "
              "Invoke-Step ([pscustomobject]@{ op = 'burst'; name = 'b'; count = $s.count; every = $s.every; seconds = $s.seconds }); "
              "($script:t - $t0).TotalSeconds.ToString([cultureinfo]::InvariantCulture) }")
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip()]
        self.assertEqual(lines, ["10", "0.1", "3.2"], (out.stdout, out.stderr))

    def test_a_still_burst_keeps_frames_until_the_screen_holds_still(self):
        # Startup Repair could start after a fixed burst's last frame, and the
        # settle after it kept none (review, 2026-11-18).
        shell = self._powershell(INV / "ref-host.ps1", windows=True)[0]
        ps = (self._ps_functions((INV / "ref-host.ps1", ["Test-ThumbnailColour", "Invoke-Step"]),
                                 (INV / "ref-common.ps1", ["Get-ThumbnailPixel", "Get-ThumbnailHash"]))
              + "$Name = 'x'; $NoSession = $false; $t0 = [datetime]'2026-01-01'; "
              "function Get-Date { $script:t }; function Start-Sleep([int]$Milliseconds) { $script:t = $script:t.AddMilliseconds($Milliseconds) }; "
              # The screen changes on every frame up to the switch, then holds.
              "function Save-Shot($n, $Until) { $script:k++; $script:t = $script:t.AddMilliseconds(100); "
              "$b = New-Object byte[] 20; if ($script:k -lt $script:switch) { $b[5] = $script:k }; $script:LastShot = @{ Bytes = $b; Width = 4; Height = 2 } }; "
              "foreach ($w in 20, 1000) { $script:switch = $w; $script:k = 0; $script:t = $t0; "
              "try { Invoke-Step ([pscustomobject]@{ op = 'burst'; name = 'r'; count = 80; every = 0; cadence = 3; seconds = 240; still = 5 }); 'held ' + $script:k } "
              "catch { $_.Exception.Message } }")
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip()]
        # Changing for 19 frames (57 s, past the old 12): held at frame 20 and
        # stopped 6 s later, at frame 22. A screen that never holds fails.
        self.assertEqual(lines, ["held 22", "burst r: the screen did not hold still for 5 s"], (out.stdout, out.stderr))

    def test_a_timed_burst_outlives_a_video_outage_longer_than_a_shot(self):
        # A 16 s outage during the repair restart failed a walk with minutes of
        # burst left (review, 2026-11-19). The real shot helpers run over fake
        # CIM calls; the head is gone from 2 s to 18 s.
        shell = self._powershell(INV / "ref-host.ps1", windows=True)[0]
        case = ("function Cost { 50 }; function Gone { $s = ($script:t - $t0).TotalSeconds; $s -ge 2 -and $s -lt 18 }; "
                "Invoke-Step ([pscustomobject]@{ op = 'burst'; name = 'repair'; count = 80; every = 0; cadence = 3; seconds = 40 }); "
                "($shots | ForEach-Object { $_.name }) -join ','")
        r = subprocess.run([shell, "-NoProfile", "-Command", self._cim_fakes() + case], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in r.stdout.splitlines() if line.strip()]
        # Frames at 0 s, then the outage holds frame 2 (tried at 3 s, again
        # after 15 s), then on until 40 s, numbered without a gap.
        self.assertTrue(lines, r.stderr)
        names = lines[-1].split(",")
        self.assertEqual(names, [f"repair-{i}" for i in range(1, len(names) + 1)], r.stdout)
        self.assertGreaterEqual(len(names), 8, names)

    def test_a_still_burst_restarts_its_hold_after_an_outage_and_an_empty_burst_fails(self):
        # An outage a shot waited out let the first frame after it end a still
        # burst, and a burst with no frame went on to the next steps (review,
        # 2026-11-19). Identical pixels throughout; the head is gone from 2 s
        # (from the start for the empty burst) to the given end.
        shell = self._powershell(INV / "ref-host.ps1", windows=True)[0]
        burst = "Invoke-Step ([pscustomobject]@{ op = 'burst'; name = 'repair'; count = 80; every = 0; cadence = 3; seconds = 60; still = 5 })"
        cases = {"short": (12, burst), "long": (20, burst),
                 "empty": (100, "Invoke-Step ([pscustomobject]@{ op = 'burst'; name = 'boot'; count = 10; every = 0; cadence = 1; seconds = 5 })")}
        got = {}
        for k, (back, step) in cases.items():
            gone_from = 0 if k == "empty" else 2      # the empty burst never sees the head
            case = ("function Cost { 50 }; function Gone { $s = ($script:t - $t0).TotalSeconds; $s -ge %d -and $s -lt %d }; "
                    "try { %s; 'ok ' + (($shots | ForEach-Object { $_.at }) -join ';') } catch { $_.Exception.Message }; "
                    "($script:t - $t0).TotalSeconds.ToString([cultureinfo]::InvariantCulture)") % (gone_from, back, step)
            r = subprocess.run([shell, "-NoProfile", "-Command", self._cim_fakes() + case], capture_output=True, text=True, timeout=120)
            got[k] = ([line.strip() for line in r.stdout.splitlines() if line.strip()] or [r.stderr])
        # The hold counts only from the first frame after the head returns:
        # the burst ends at least 5 s after it, never on that frame.
        for k, back in (("short", 12), ("long", 20)):
            self.assertTrue(got[k][0].startswith("ok"), got)
            self.assertGreaterEqual(float(got[k][-1]), back + 5, got)
        self.assertEqual(got["empty"][0], "burst boot: no frame within the burst", got)

    def test_pixels_from_before_an_outage_are_taken_again_and_a_failure_shot_waits_3_s(self):
        # The mode read after a thumbnail could wait out an outage and return
        # the pixels from before it, and the failure shot waited another 15 s
        # with no head (review, 2026-11-19).
        shell = self._powershell(INV / "ref-host.ps1", windows=True)[0]
        # Calls take 50 ms: the thumbnail ends at 0.1 s, the mode read after
        # it meets the outage (0.12 to 0.8 s), so the shot takes its pixels
        # again after 0.8 s.
        # Each thumbnail carries its own number in its first pixel (after the
        # four-byte header Save-Thumbnail skips), and the
        # saved and kept frames must be the one taken after the outage.
        again = ("function Cost { 50 }; function Gone { $s = ($script:t - $t0).TotalSeconds; $s -ge 0.12 -and $s -lt 0.8 }; $script:at = @(); "
                 "function Invoke-CimMethod { Call; $script:at += ($script:t - $t0).TotalSeconds.ToString([cultureinfo]::InvariantCulture); "
                 "$b = New-Object byte[] 20; $b[4] = $script:at.Count; [pscustomobject]@{ ReturnValue = 0; ImageData = $b } }; "
                 "function Save-Thumbnail($Bytes) { $script:saved = $Bytes[4] }; "
                 "Save-Shot 'x'; '' + $script:ShotInterrupted + ' ' + ($script:at -join ';') + ' ' + $script:saved + ' ' + $script:LastShot.Bytes[4]")
        fail = ("function Cost { 50 }; function Gone { $true }; function Write-Host($m) { $m }; Save-FailureShot 4; "
                "($script:t - $t0).TotalSeconds.ToString([cultureinfo]::InvariantCulture)")
        got = {}
        for k, case in (("again", again), ("fail", fail)):
            r = subprocess.run([shell, "-NoProfile", "-Command", self._cim_fakes() + case], capture_output=True, text=True, timeout=120)
            got[k] = [line.strip() for line in r.stdout.splitlines() if line.strip()] or [r.stderr]
        flag, thumbs, saved, kept = got["again"][-1].split(" ")
        self.assertEqual((saved, kept), ("2", "2"), got)
        self.assertEqual(flag, "True", got)
        self.assertEqual(len(thumbs.split(";")), 2, got)
        self.assertGreaterEqual(float(thumbs.split(";")[-1]), 0.8, got)
        self.assertTrue(got["fail"][0].startswith("ref-host: no shot of the failure either: "), got)
        self.assertLessEqual(float(got["fail"][-1]), 3.1, got)

    def test_a_settle_waits_out_an_invalid_state_and_counts_its_quiet_time_afresh(self):
        # One 32775 read ended a 60 s settle (review, 2026-11-19). Identical
        # frames on both sides of one failed read at the second poll.
        shell = self._powershell(INV / "ref-host.ps1", windows=True)[0]
        case = ("function Cost { 50 }; function Gone { $false }; $script:n = 0; "
                "function Invoke-CimMethod { Call; $script:n++; if ($script:n -eq 2) { return [pscustomobject]@{ ReturnValue = 32775; ImageData = $null } }; "
                "[pscustomobject]@{ ReturnValue = 0; ImageData = (New-Object byte[] 20) } }; "
                "try { Invoke-Step ([pscustomobject]@{ op = 'settle'; sec = 2; timeout = 60 }); 'settled' } catch { $_.Exception.Message }; "
                "($script:t - $t0).TotalSeconds.ToString([cultureinfo]::InvariantCulture)")
        r = subprocess.run([shell, "-NoProfile", "-Command", self._cim_fakes() + case], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in r.stdout.splitlines() if line.strip()]
        # Poll 1 at 0, the failed read at 0.55, then frames from 1.1 s: two
        # quiet seconds are counted from 1.1, so it settles at 3.3 s or later.
        self.assertEqual(lines[0], "settled", (r.stdout, r.stderr))
        self.assertGreaterEqual(float(lines[-1]), 3.1, lines)

    def test_every_identity_check_names_its_vm(self):
        # derive.ps1 left the name out, and a match noted the build on a VM
        # with an empty name (review, 2026-11-19).
        for f in INV.rglob("*.ps1"):
            for line in f.read_text(encoding="utf-8").splitlines():
                if "Test-RefIdentity " in line and "function Test-RefIdentity" not in line:
                    args = line.split("Test-RefIdentity ", 1)[1].split(")")[0].split()
                    self.assertEqual(len(args), 2, f"{f.name}: {line.strip()}")
        shell = self._powershell(INV / "ref-common.ps1", windows=True)[0]
        ps = (self._ps_functions((INV / "ref-common.ps1", ["Test-RefIdentity"]))
              + "try { Test-RefIdentity $null '' } catch { $_.Exception.Message }")
        r = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        self.assertEqual(r.stdout.strip(), "Test-RefIdentity: no VM name", (r.stdout, r.stderr))

    def test_a_burst_names_its_exact_until_frame_as_the_page_shot(self):
        # The boot menu was polled and shot again after the burst had its exact
        # frame, while its 30 s countdown ran (review, 2026-11-19).
        shell = self._powershell(INV / "ref-host.ps1", windows=True)[0]
        ps = (self._ps_functions((INV / "ref-host.ps1", ["Invoke-Step"]), (INV / "ref-common.ps1", ["Assert-RefBefore"]))
              + "$Name = 'x'; $NoSession = $false; $Out = 'o'; $shots = New-Object System.Collections.ArrayList; "
              "$script:t = [datetime]'2026-01-01'; function Get-Date { $script:t }; function Start-Sleep([int]$Milliseconds) { $script:t = $script:t.AddMilliseconds($Milliseconds) }; "
              "function Write-Host { }; function Get-RefThumbnail { New-Object byte[] 20 }; function Get-ThumbnailHash { 'same' }; "
              "function Copy-Item($LiteralPath, $Destination, [switch]$Force) { $script:ev += 'copy:' + (Split-Path $LiteralPath -Leaf) + '>' + (Split-Path $Destination -Leaf) }; "
              # The colour shows from look number $from on, at a burst frame or
              # a wait poll; every look and save is logged in order.
              "function Test-ThumbnailColour { $script:looks++; $r = $script:looks -ge $script:from; $script:ev += 'look:' + $r; $r }; "
              "function Save-Shot($n, $Until) { $script:t = $script:t.AddMilliseconds(100); $script:ev += 'save:' + $n; "
              "$script:LastShot = @{ Bytes = (New-Object byte[] 20); Width = 4; Height = 2 }; [void]$shots.Add([ordered]@{ name = $n; file = \"$n.png\"; exact = $script:exact; at = 'T' + $n }) }; "
              "$menu = [pscustomobject]@{ x = 1; y = 1; rgb = @(0, 0, 0) }; "
              "foreach ($c in @(@{ count = 5; exact = $true; from = 3 }, @{ count = 5; exact = $false; from = 3 }, @{ count = 2; exact = $true; from = 3 }, @{ count = 2; exact = $true; from = 99 })) { "
              "$shots.Clear(); $script:ev = @(); $script:looks = 0; $script:exact = $c.exact; $script:from = $c.from; "
              "try { Invoke-Step ([pscustomobject]@{ op = 'burst'; name = 'b'; count = $c.count; every = 0; cadence = 1; seconds = 30; until = $menu; as = 'page'; astimeout = 1 }) } catch { $script:ev += 'threw' }; "
              "($script:ev -join ',') + ' ' + ((@($shots)[-2..-1] | ForEach-Object { $_.Values -join ':' }) -join '|') }; "
              # A still burst names the frame that held still the same way.
              "$shots.Clear(); $script:ev = @(); $script:exact = $true; "
              "Invoke-Step ([pscustomobject]@{ op = 'burst'; name = 'r'; count = 9; every = 0; cadence = 1; seconds = 30; still = 2; as = 'still' }); "
              "($script:ev -join ',') + ' ' + ((@($shots)[-2..-1] | ForEach-Object { $_.Values -join ':' }) -join '|')")
        r = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in r.stdout.splitlines() if line.strip()]
        self.assertEqual(len(lines), 5, (r.stdout, r.stderr))
        exact, inexact, early, never, still = lines
        # The exact frame that met the colour is the page: no wait, no shot;
        # its copy is page.png and the page's entry keeps the frame's own
        # record (its time) under the page's name and file.
        # The burst frame's own entry stays as it was, before the page's.
        self.assertEqual(exact, "save:b-1,look:False,save:b-2,look:False,save:b-3,look:True,copy:b-3.png>page.png b-3:b-3.png:True:Tb-3|page:page.png:True:Tb-3")
        # Otherwise a wait poll that sees the colour comes after the burst's
        # last look and before the page is shot.
        self.assertEqual(inexact, "save:b-1,look:False,save:b-2,look:False,save:b-3,look:True,look:True,save:page b-3:b-3.png:False:Tb-3|page:page.png:False:Tpage")
        self.assertEqual(early, "save:b-1,look:False,save:b-2,look:False,look:True,save:page b-2:b-2.png:True:Tb-2|page:page.png:True:Tpage")
        # A colour that never shows times out and the page is never shot.
        self.assertTrue("threw" in never and "save:page" not in never and "page:" not in never, never)
        # Frames 1 s apart, all the same: held 2 s at the third.
        self.assertEqual(still, "save:r-1,save:r-2,save:r-3,copy:r-3.png>still.png r-3:r-3.png:True:Tr-3|still:still.png:True:Tr-3")

    def test_the_menus_credit_only_the_keys_the_walk_sends(self):
        # The boot menu's records claimed Up, Tab alone and Enter on a tile,
        # and every tile page the same arrow and Enter claim, walked or not
        # (review, 2026-11-19). The generator derives both from the steps.
        sc = load("host_session_boot", INV / "host" / "session-boot.py")
        D, T, E, R = sc.DOWN, sc.TAB, sc.ENTER, sc.RIGHT
        self.assertEqual([sc.keys_between(a, b) for _, a, b, _ in sc.BOOT_MENU_KEYS], [[D], [D, T], [E]])
        self.assertEqual({n: sc.sent_after(n) for n in ("boot-manager", "boot-options", "recovery", "recovery-reset", "recovery-more")},
                         {"boot-manager": [D], "boot-options": [D, D, E], "recovery": [R, E], "recovery-reset": [], "recovery-more": []})
        self.assertEqual(sc.tile_keys("boot-manager"), "Down moves the focus")
        self.assertEqual(sc.tile_keys("boot-options"), "Down moves the focus, Enter takes the focused tile")
        self.assertIsNone(sc.tile_keys("recovery-reset"))        # Backspace, then the next page's keys
        self.assertIsNone(sc.tile_keys("recovery-more"))         # the walk's last shot
        # A page's keys end at its Enter, which leaves it, and before a
        # Backspace, which goes back: later keys are another page's.
        with mock.patch.object(sc, "STEPS", [sc.shot("a"), sc.key(D), sc.key(E), sc.key(R), sc.shot("b"),
                                             sc.key(R), sc.key(sc.BACK), sc.key(D), sc.shot("c")]):
            self.assertEqual((sc.sent_after("a"), sc.sent_after("b")), ([D, E], [R]))
        # The published records are those the generator makes.
        records = {r["id"]: r for r in (json.loads(line) for line in (INV.parent.parent / "docs/design/inventory/session/boot.json")
                                       .read_text(encoding="utf-8").splitlines() if line.strip())}
        base = "win11.session.boot."
        self.assertEqual([records[f"{base}boot-manager.{lab.lower().replace(' ', '-')}"]["shortcut"] for lab, *_ in sc.BOOT_MENU_KEYS],
                         ["Down", "Down, Tab", "Enter"])
        self.assertNotIn("Enter", records[f"{base}boot-manager.choices"]["behaviour"])
        self.assertNotIn("focus", records[f"{base}recovery-reset.choices"]["behaviour"])
        self.assertIn("Down and then Tab from the second tile", records[f"{base}boot-manager.options"]["behaviour"])
        # Backspace on a back button only where the walk sends it.
        backs = sorted(r["id"] for r in records.values() if r["id"].endswith(".back") and "Backspace" in r.get("behaviour", ""))
        self.assertEqual(backs, [f"{base}recovery-reset.back"])
        self.assertEqual([n for n in ("boot-options", "recovery-troubleshoot", "recovery-reset", "recovery-advanced", "recovery-more")
                          if sc.goes_back(n)], ["recovery-reset"])
        with mock.patch.object(sc, "STEPS", [sc.shot("a"), sc.key(D), sc.key(E), sc.key(sc.BACK), sc.shot("b"), sc.key(sc.BACK)]):
            self.assertEqual((sc.goes_back("a"), sc.goes_back("b")), (False, True))    # an Enter left it first; the last page
        # No record credits a key or a click the walk does not send.
        for r in records.values():
            self.assertNotIn("function key", r.get("behaviour", ""), r["id"])
        self.assertNotIn("Enter or a click", records[f"{base}automatic-repair.go-on"]["behaviour"])

    def test_burst_frames_are_in_burst_order(self):
        # frames() reads the bursts through this; repair_phases relies on the
        # order (the spinner and repair test drives it).
        sc = load("host_session_boot", INV / "host" / "session-boot.py")
        names = ["repair-10", "repair-3", "repair-5", "boot-1", "repair-x", "repair-12"]
        self.assertEqual(sc.burst_frames(names, "repair"), ["repair-3", "repair-5", "repair-10", "repair-12"])
        self.assertEqual(sc.burst_frames(names, "repair", "repair-5"), ["repair-10", "repair-12"])

    def test_a_clock_step_cannot_stretch_a_burst_sleep(self):
        # The cadence sleep was the difference of two wall-clock readings, so a
        # host clock stepped back an hour asked for an hour (review, 2026-11-18).
        shell = self._powershell(INV / "ref-host.ps1", windows=True)[0]
        ps = (self._ps_functions((INV / "ref-host.ps1", ["Test-ThumbnailColour", "Invoke-Step"]),
                                 (INV / "ref-common.ps1", ["Get-ThumbnailPixel"]))
              + "$Name = 'x'; $NoSession = $false; $script:t = [datetime]'2026-01-01T12:00'; $script:longest = 0; $script:n = 0; "
              "function Get-Date { $script:t }; function Start-Sleep([int]$Milliseconds) { $script:longest = [Math]::Max($script:longest, $Milliseconds); $script:t = $script:t.AddMilliseconds($Milliseconds) }; "
              "function Save-Shot($n, $Until) { $script:n++; if ($script:n -eq 2) { $script:t = $script:t.AddHours(-1) } else { $script:t = $script:t.AddMilliseconds(100) }; "
              "$script:LastShot = @{ Bytes = (New-Object byte[] 20); Width = 4; Height = 2 } }; "
              "Invoke-Step ([pscustomobject]@{ op = 'burst'; name = 'b'; count = 5; every = 0; cadence = 1; seconds = 30 }); $script:longest")
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip()]
        self.assertTrue(lines and int(lines[-1]) <= 1000, (out.stdout, out.stderr))

    def test_a_timed_burst_starts_no_host_call_after_its_end(self):
        # A frame admitted near a burst's end could take its own 15 s, retry a
        # mode read or run a thumbnail's CIM calls after the end (review,
        # 2026-11-18). The real Save-Shot, mode and thumbnail helpers run here
        # over fake CIM calls, each logging when it starts.
        shell = self._powershell(INV / "ref-host.ps1", windows=True)[0]
        base = self._cim_fakes()
        burst = ("Invoke-Step ([pscustomobject]@{ op = 'burst'; name = 'stop'; count = 50; every = 0; cadence = 1; seconds = 5 }); "
                 "'' + $shots.Count + ' ' + $script:thumbs + ' ' + ($script:t - $t0).TotalSeconds.ToString([cultureinfo]::InvariantCulture) + ' ' + "
                 "(($script:calls | Measure-Object -Maximum).Maximum).ToString([cultureinfo]::InvariantCulture)")
        late = "(($script:t - $t0).TotalSeconds -ge 3.5)"
        cases = {
            # The video head goes away from 3.5 s; the frame begun at 4 s
            # retries mode reads only until the burst's end at 5 s.
            "reset": "function Cost { 50 }; function Gone { %s }; " % late,
            # Host calls turn slow from 3.5 s: the frame begun at 4 s crosses
            # the end inside its thumbnail's CIM calls.
            "slow": "function Cost { if (%s) { 300 } else { 50 } }; function Gone { $false }; " % late,
        }
        for k, fake in cases.items():
            out = subprocess.run([shell, "-NoProfile", "-Command", base + fake + burst], capture_output=True, text=True, timeout=120)
            last = out.stdout.strip().splitlines()[-1] if out.stdout.strip() else ""
            self.assertEqual(len(last.split()), 4, (k, out.stdout, out.stderr))
            n, thumbs, end, latest = last.split()
            self.assertEqual((n, thumbs), ("4", "4"), (k, last))         # frames at 0, 1, 2 and 3 s
            self.assertLess(float(latest), 5, (k, last))                  # no call starts after the end
            if k == "reset":
                self.assertEqual(float(end), 5, last)                     # the retry sleep stops at the end
            else:
                self.assertLessEqual(float(end), 5.3, (k, last))          # one call's overrun at most

    def test_a_burst_spaces_each_shot_from_the_one_before_and_ends_on_time(self):
        # A slow first shot let the next ones catch up on a schedule counted
        # from the burst's start, and a frame admitted late could wait 15 s
        # past the burst's end (review, 2026-11-18).
        shell = self._powershell(INV / "ref-host.ps1", windows=True)[0]
        ps = (self._ps_functions((INV / "ref-host.ps1", ["Test-ThumbnailColour", "Invoke-Step"]),
                                 (INV / "ref-common.ps1", ["Get-ThumbnailPixel"]))
              + "$Name = 'x'; $NoSession = $false; $t0 = [datetime]'2026-01-01'; $script:t = $t0; $script:starts = @(); "
              "function Get-Date { $script:t }; function Start-Sleep([int]$Milliseconds) { $script:t = $script:t.AddMilliseconds($Milliseconds) }; "
              "function Save-Shot($n, $Until) { $script:starts += ($script:t - $t0).TotalSeconds; "
              "if ($script:starts.Count -eq 1) { $script:t = $script:t.AddMilliseconds(2500) } "
              "elseif (($script:t - $t0).TotalSeconds -ge 8) { $script:t = $Until; throw 'no video head: resetting' } "
              "else { $script:t = $script:t.AddMilliseconds(200) }; "
              "$script:LastShot = @{ Bytes = (New-Object byte[] 20); Width = 4; Height = 2 } }; "
              "Invoke-Step ([pscustomobject]@{ op = 'burst'; name = 'boot'; count = 150; every = 0; cadence = 1; seconds = 10 }); "
              "(($script:starts | ForEach-Object { $_.ToString([cultureinfo]::InvariantCulture) }) -join ';'); ($script:t - $t0).TotalSeconds.ToString([cultureinfo]::InvariantCulture)")
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip()]
        self.assertEqual(len(lines), 2, out.stdout + out.stderr)
        starts = [float(x) for x in lines[0].split(";")]
        self.assertEqual(starts[:3], [0, 2.5, 3.5], starts)
        self.assertTrue(all(b - a >= 1 for a, b in zip(starts, starts[1:])), starts)
        self.assertEqual(float(lines[1]), 10, out.stderr)                 # ends at its budget, no error

    def test_a_walk_without_a_session_checks_the_build_recorded_on_the_vm(self):
        # A setup walk (no session) could restore an old pre-oobe and publish it
        # under a newer reference.json build (review, 2026-11-18).
        shell = self._powershell(INV / "ref-common.ps1", windows=True)[0]
        ps = (self._ps_functions((INV / "ref-common.ps1", ["Set-RefBuildNote", "Test-RefBuildNote"]))
              + "function Get-RefIdentity { [pscustomobject]@{ build = '26300.9974' } }; "
              "$script:notes = ''; $script:sets = 0; function Get-VM { [pscustomobject]@{ Notes = $script:notes } }; "
              "function Set-VM { param($Name, $Notes) $script:notes = $Notes; $script:sets++ }; "
              "@(Test-RefBuildNote 'x') -join '|'; "
              "Set-RefBuildNote 'x' '26300.9000'; @(Test-RefBuildNote 'x') -join '|'; "
              "Set-RefBuildNote 'x' '26300.9974'; Set-RefBuildNote 'x' '26300.9974'; 'sets=' + $script:sets; "
              "'problems=' + @(Test-RefBuildNote 'x').Count")
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip()]
        self.assertEqual(lines, ["x has no verified build in its notes; a capture from clean records it",
                                 "x was verified as build 26300.9000, reference.json says 26300.9974",
                                 "sets=2", "problems=0"], out.stderr)

    def test_a_stand_in_probe_is_judged_by_its_exit_code_not_its_stderr(self):
        # A wsl.exe warning on stderr ("Failed to translate" a dead PATH entry)
        # stopped the whole verify under Stop (2026-11-20).
        shell = self._powershell(INV / "ref-vm.ps1", windows=True)[0]
        ps = ("$ErrorActionPreference = 'Stop'; Push-Location -LiteralPath $env:SystemRoot; "
              + self._ps_functions((INV / "ref-vm.ps1", ["Test-StandinProfile"]))
              + "function Get-RefIdentity { [pscustomobject]@{ build = '1' } }; function Compare-RefProfile { @() }; "
              "$script:rc = 0; function wsl.exe { if ($args[1] -eq 'wslpath') { $global:LASTEXITCODE = 0; '/x'; return } "
              "cmd.exe /c \"echo wsl: Failed to translate R:\\gone 1>&2 & echo {\"\"remote\"\":false} & exit /b $script:rc\" }; "
              "try { 'ok=' + (@(Test-StandinProfile 'dual' $null)).Count } catch { 'threw: ' + $_ }; "
              "'seen=' + $script:SeenJson; "
              "$script:rc = 3; (@(Test-StandinProfile 'dual' $null)) -join '|'")
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip() and not line.startswith("ref-vm:")]
        self.assertEqual(lines, ["ok=0", 'seen={"remote":false}',
                                 'profile dual on the stand-in machine: wsl: Failed to translate R:\\gone / {"remote":false}'],
                         out.stdout + out.stderr)

    def test_a_new_checkpoint_is_renamed_through_its_object(self):
        # Rename-VMSnapshot by name straight after Checkpoint-VM found nothing
        # (derive 2026-11-16, clean 2026-11-19); only a promotion repair, which
        # renames a checkpoint that already existed, may look it up by name.
        by_name = []
        for path in sorted(INV.glob("**/*.ps1")):
            for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                if "Rename-VMSnapshot" in line and "-VMSnapshot" not in line.replace("Rename-VMSnapshot", ""):
                    by_name.append(f"{path.relative_to(INV)}:{n}")
        self.assertEqual(by_name, ["ref-common.ps1:390"])

    def test_a_restore_finds_media_the_store_move_left_behind(self):
        # The store moved from R:\11 to D:\11 (2026-11-19): every checkpoint
        # still attached R:\11\win11-26300.9974.iso, which lives in the store
        # root above the VM folder, so the VM-folder-only lookup missed it.
        shell = self._powershell(INV / "ref-common.ps1", windows=True)[0]
        ps = (self._ps_functions((INV / "ref-common.ps1", ["Find-RefMedia"]))
              + "$s = Join-Path ([IO.Path]::GetTempPath()) ('ios-media-' + [Guid]::NewGuid().ToString('N')); "
              "$vm = Join-Path $s 'ios-win11-ref'; New-Item -ItemType Directory -Path $vm | Out-Null; "
              "try { "
              "Set-Content -LiteralPath (Join-Path $s 'install.iso') -Value x; "
              "Set-Content -LiteralPath (Join-Path $vm 'both.iso') -Value x; Set-Content -LiteralPath (Join-Path $s 'both.iso') -Value x; "
              "New-Item -ItemType Directory -Path (Join-Path $s 'dir.iso') | Out-Null; "
              "(Find-RefMedia 'R:\\11\\install.iso' $vm).Substring($s.Length); "
              "(Find-RefMedia 'R:\\11\\ios-win11-ref-next\\both.iso' $vm).Substring($s.Length); "
              "'gone=' + ($null -eq (Find-RefMedia 'R:\\11\\gone.iso' $vm)); "
              "'dir=' + ($null -eq (Find-RefMedia 'R:\\11\\dir.iso' $vm)) "
              "} finally { Remove-Item -LiteralPath $s -Recurse -Force }")
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip()]
        self.assertEqual(lines, ["\\install.iso", "\\ios-win11-ref\\both.iso", "gone=True", "dir=True"], out.stderr)

    def test_the_build_note_keeps_the_operators_own_notes(self):
        # A match replaced the whole Notes field (review, 2026-11-19).
        shell = self._powershell(INV / "ref-common.ps1", windows=True)[0]
        ps = (self._ps_functions((INV / "ref-common.ps1", ["Set-RefBuildNote", "Test-RefBuildNote"]))
              + "function Get-RefIdentity { [pscustomobject]@{ build = '26300.9974' } }; "
              "$script:notes = \"reference VM`nkeep this\"; $script:sets = 0; function Get-VM { [pscustomobject]@{ Notes = $script:notes } }; "
              "function Set-VM { param($Name, $Notes) $script:notes = $Notes; $script:sets++ }; "
              "Set-RefBuildNote 'x' '26300.9000'; ($script:notes -split \"`n\") -join '|'; @(Test-RefBuildNote 'x') -join '|'; "
              "Set-RefBuildNote 'x' '26300.9974'; ($script:notes -split \"`n\") -join '|'; "
              "Set-RefBuildNote 'x' '26300.9974'; 'sets=' + $script:sets; 'problems=' + @(Test-RefBuildNote 'x').Count")
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip()]
        self.assertEqual(lines, ["ios-build=26300.9000|reference VM|keep this",
                                 "x was verified as build 26300.9000, reference.json says 26300.9974",
                                 "ios-build=26300.9974|reference VM|keep this", "sets=2", "problems=0"], out.stderr)

        # A stamp in the middle of CRLF notes with blank and trailing lines:
        # a match writes nothing, and a change touches only the value.
        ps = (self._ps_functions((INV / "ref-common.ps1", ["Set-RefBuildNote", "Test-RefBuildNote"]))
              + "function Get-RefIdentity { [pscustomobject]@{ build = '26300.9974' } }; "
              "$t = \"top`r`n`r`nios-build=26300.9974`r`nbottom`r`n\"; $script:notes = $t; $script:sets = 0; "
              "function Get-VM { [pscustomobject]@{ Notes = $script:notes } }; function Set-VM { param($Name, $Notes) $script:notes = $Notes; $script:sets++ }; "
              "Set-RefBuildNote 'x' '26300.9974'; 'sets=' + $script:sets + ' same=' + ($script:notes -ceq $t); "
              "Set-RefBuildNote 'x' '26300.9999'; 'sets=' + $script:sets + ' rest=' + ($script:notes -ceq $t.Replace('26300.9974', '26300.9999')); "
              "'problems=' + @(Test-RefBuildNote 'x').Count")
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip()]
        self.assertEqual(lines, ["sets=0 same=True", "sets=1 rest=True", "problems=1"], out.stderr)

    def test_a_teardown_restore_leaves_the_vm_not_running(self):
        # Restore-RefCheckpoint started the VM, so both teardowns released the
        # lock with the guest running (review, 2026-11-18). A checkpoint taken
        # while running restores Running: -NoStart turns it off.
        shell = self._powershell(INV / "ref-common.ps1", windows=True)[0]
        ps = (self._ps_functions((INV / "ref-common.ps1", ["Restore-RefCheckpoint", "Find-RefMedia"]))
              + "function Repair-RefPromotion { }; function Get-VMDvdDrive { @() }; "
              "function Get-VMSnapshot { @([pscustomobject]@{ Name = 'clean' }) }; "
              "$script:state = 'Running'; $script:after = 'Running'; "
              "function Get-VM { [pscustomobject]@{ State = $script:state; Id = 'id' } }; "
              "function Stop-VM { $script:state = 'Off' }; function Restore-VMSnapshot { $script:state = $script:after }; "
              "$script:starts = 0; function Start-VM { $script:starts++; $script:state = 'Running' }; "
              "$p = [IO.Path]::GetTempPath(); "
              "Restore-RefCheckpoint 'x' 'clean' $p -NoStart; 'running-checkpoint: ' + $script:state + ' starts=' + $script:starts; "
              "$script:after = 'Saved'; Restore-RefCheckpoint 'x' 'clean' $p -NoStart; 'saved-checkpoint: ' + $script:state + ' starts=' + $script:starts; "
              "Restore-RefCheckpoint 'x' 'clean' $p; 'capture: ' + $script:state + ' starts=' + $script:starts; "
              # A media repair that fails must not leave a running restore running.
              "$script:after = 'Running'; $script:state = 'Running'; "
              "function Get-VMDvdDrive { @([pscustomobject]@{ Path = 'D:\\x\\a.iso' }) }; function Test-Path { $true }; "
              "function Set-VMDvdDrive { throw 'media refused' }; "
              "try { Restore-RefCheckpoint 'x' 'clean' $p -NoStart; 'no error' } catch { 'error: ' + $_.Exception.Message + ' state=' + $script:state }")
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip()]
        self.assertEqual(lines, ["running-checkpoint: Off starts=0", "saved-checkpoint: Saved starts=0",
                                 "capture: Running starts=1", "error: media refused state=Off"], out.stderr)

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
                states=None, profile="fhd-100", file=None):
        capture.publish(target or self.target, list(records), "notepad-smoke", self.out, states or self.STATE,
                        self.manifest, self.store, profile=profile, journal=self.journal, file=file)

    def stored(self, surface="notepad-smoke", state="rest", profile="fhd-100"):
        """The store path of a published shot, from the manifest (shots are named by digest)."""
        e = next(e for e in json.loads(self.manifest.read_text())
                 if (e["surface"], e["state"], e["profile"]) == (surface, state, profile))
        return capture.shot_path(self.store, e)

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

    def test_refreshing_one_profile_leaves_the_others_byte_identical(self):
        def rec(rid, x, profile):
            return {"id": rid, "surface": "notepad-smoke", "kind": "region", "label": "Zone", "order": 0,
                    "source": "uia:1", "capturedFrom": "1", "captureProfile": profile, "session": "console",
                    "bounds": {"x": x, "y": 0, "width": 7, "height": 3}}
        self.publish([rec("win11.notepad-smoke.zone", 0, "fhd-100")])
        files = {}
        for profile, x in (("wide-100", 1), ("tall-100", 2)):
            files[profile] = self.target.with_name(f"window.{profile}.json")
            self.publish(core.observations([rec("win11.notepad-smoke.zone", x, profile)], profile),
                         target=files[profile], states=[self.shot(f"rest.{profile}.png")], profile=profile,
                         file="window")
        before = {p: f.read_bytes() for p, f in [("fhd-100", self.target)] + list(files.items())}
        self.publish(core.observations([rec("win11.notepad-smoke.zone", 5, "wide-100")], "wide-100"),
                     target=files["wide-100"], states=[self.shot("rest.wide-100.png")], profile="wide-100",
                     file="window")
        self.assertEqual(self.target.read_bytes(), before["fhd-100"])
        self.assertEqual(files["tall-100"].read_bytes(), before["tall-100"])
        self.assertEqual(json.loads(files["wide-100"].read_text())["bounds"]["x"], 5)
        owned = sorted((e["file"], e["profile"]) for e in json.loads(self.manifest.read_text())
                       if e["surface"] == "notepad-smoke")
        self.assertEqual(owned, [("window", "fhd-100"), ("window", "tall-100"), ("window", "wide-100")])

    def test_an_observation_keeps_only_measured_facts_of_its_profile(self):
        r = {"id": "win11.s.a", "surface": "s", "kind": "control", "label": "A", "order": 0, "source": "uia:1",
             "capturedFrom": "1", "captureProfile": "wide-100", "session": "console",
             "bounds": {"x": 0, "y": 0, "width": 1, "height": 1}, "disposition": "build"}
        bare = dict(r, id="win11.s.b")
        del bare["bounds"]
        self.assertEqual(core.observations([r, bare], "wide-100"),
                         [{"id": "win11.s.a", "captureProfile": "wide-100", "capturedFrom": "1",
                           "session": "console", "bounds": {"x": 0, "y": 0, "width": 1, "height": 1}}])
        with self.assertRaises(ValueError):
            core.observations([r], "tall-100")

    def test_a_hand_record_belongs_to_no_profile_and_is_no_observation(self):
        hand = {"id": "win11.s.keys", "surface": "s", "kind": "group", "label": "Keys", "order": 0,
                "source": "hand:1", "capturedFrom": "1", "disposition": "build"}
        self.assertEqual(core.observations([hand], "wide-100"), [])

    def test_a_shot_of_another_profile_publishes_nothing(self):
        with self.assertRaises(OSError):
            self.publish(profile="fhd-150")
        self.assertFalse(self.target.exists())

    def test_two_checkouts_publishing_one_state_keep_both_shots(self):
        # Checkout A publishes, then checkout B (its own manifest) publishes other
        # pixels for the same surface, state and profile: A's manifest must still
        # verify against the shared store (review, 2026-11-21).
        self.publish()
        a = json.loads(self.manifest.read_text())
        b_manifest = self.tmp / "b-shots.json"
        core.write_png(self.out / "shots" / "notepad-smoke" / "rest.fhd-100.png", 2, 1, b"\x00\x00\xff\x00\xff\x00")
        capture.publish(self.tmp / "b" / "window.json", [{"id": "win11.notepad-smoke.window", "label": "B"}],
                        "notepad-smoke", self.out, self.STATE, b_manifest, self.store, profile="fhd-100",
                        journal=self.tmp / "b.journal")
        b = json.loads(b_manifest.read_text())
        mine = lambda m: [e for e in m if e["surface"] == "notepad-smoke"]        # noqa: E731
        self.assertNotEqual(mine(a)[0]["sha256"], mine(b)[0]["sha256"])
        self.assertEqual(capture.verify_shots(mine(a), self.store), [])
        self.assertEqual(capture.verify_shots(mine(b), self.store), [])

    def test_the_store_moves_to_digest_names_once(self):
        self.publish()
        entry = next(e for e in json.loads(self.manifest.read_text()) if e["surface"] == "notepad-smoke")
        new, old = capture.shot_path(self.store, entry), capture.legacy_shot_path(self.store, entry)
        os.replace(new, old)                                    # a store written before digest names
        copied, missing = capture.migrate_store([entry], self.store)
        self.assertEqual((copied, missing), (1, []))
        self.assertTrue(new.is_file() and old.is_file())        # the old name stays for an older checkout
        self.assertEqual(capture.migrate_store([entry], self.store), (0, []))
        new.write_bytes(b"cut")                                  # a damaged copy beside a good original
        self.assertEqual(capture.migrate_store([entry], self.store), (1, []))
        self.assertEqual(capture.sha256(new), entry["sha256"])
        old.write_bytes(b"other")
        new.write_bytes(b"cut")
        self.assertEqual(capture.migrate_store([entry], self.store), (0, [str(old)]))   # neither holds the bytes

    def test_publish_then_verify(self):
        self.publish()
        manifest = json.loads(self.manifest.read_text())
        self.assertEqual([(e["surface"], e["state"], e["profile"]) for e in manifest],
                         [("notepad-smoke", "rest", "fhd-100"), ("other", "rest", "fhd-100")])
        other = capture.shot_path(self.store, manifest[1])
        self.assertEqual(other.name, "rest.fhd-100." + "0" * 16 + ".png")
        other.parent.mkdir()
        other.write_bytes(b"x")
        bad = capture.verify_shots(manifest, self.store)
        self.assertEqual(bad, [f"changed: {other}"])
        self.stored().unlink()
        self.assertEqual(len(capture.verify_shots(manifest, self.store)), 2)

    def test_a_host_shot_records_its_source_size_and_depth(self):
        # Section 9: each host shot records source size, output size and depth.
        state = {**self.STATE[0], "depth": 16, "sourceWidth": 2, "sourceHeight": 1, "exact": False, "extra": 1}
        self.publish(states=[state])
        entry = next(e for e in json.loads(self.manifest.read_text()) if e["surface"] == "notepad-smoke")
        self.assertEqual((entry["width"], entry["height"], entry["depth"], entry["sourceWidth"], entry["sourceHeight"], entry["exact"]),
                         (2, 1, 16, 2, 1, False))
        self.assertNotIn("extra", entry)

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
        before = {p: p.read_bytes() for p in (self.target, self.manifest, self.stored())}
        (self.out / "shots" / "notepad-smoke" / "rest.fhd-100.png").unlink()     # the second shot cannot be copied
        with self.assertRaises(OSError):
            self.publish([{"id": "win11.notepad-smoke.window", "label": "New"}])
        self.assertEqual({p: p.read_bytes() for p in before}, before)
        leftovers = [p.name for p in self.tmp.rglob("*") if p.name.endswith((".tmp", ".staged"))]
        self.assertEqual(leftovers, [])

    def test_a_failure_during_the_swap_puts_everything_back(self):
        self.publish([{"id": "win11.notepad-smoke.window", "label": "Notepad", "behaviour": "ours"}])
        before = {p: p.read_bytes() for p in (self.target, self.manifest, self.stored())}
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

    def test_a_state_another_file_published_is_not_overwritten(self):
        # session/secure and session/setup both published first-desktop: the
        # store path has no file in it, so setup's shot replaced secure's
        # while both kept their manifest entries (2026-11-18).
        self.publish()                                                     # window: rest
        before = {p: p.read_bytes() for p in (self.target, self.manifest, self.stored())}
        other = self.target.with_name("menu.json")
        with self.assertRaisesRegex(OSError, r"rest\.fhd-100\.png \(published for window\)"):
            self.publish([{"id": "win11.notepad-smoke.menu", "label": "Menu"}], target=other, states=[self.shot("rest.fhd-100.png")])
        self.assertEqual({p: p.read_bytes() for p in before}, before)
        self.assertFalse(other.exists())
        self.publish([{"id": "win11.notepad-smoke.menu", "label": "Menu"}], target=other,
                     states=[self.shot("menu.fhd-100.png")])                # its own state still publishes

    def test_verify_names_a_store_path_two_files_claim(self):
        self.publish()
        manifest = [e for e in json.loads(self.manifest.read_text()) if e["surface"] == "notepad-smoke"]
        manifest.append(dict(manifest[0], file="menu"))
        path = self.store / "notepad-smoke" / "rest.fhd-100.png"
        self.assertEqual(capture.verify_shots(manifest, self.store), [f"shared: {path} is claimed by menu, window"])

    def test_two_shots_with_one_file_name_publish_nothing(self):
        self.publish([{"id": "win11.notepad-smoke.window", "label": "Notepad", "behaviour": "ours"}])
        before = {p: p.read_bytes() for p in (self.target, self.manifest, self.stored())}
        with self.assertRaises(OSError):
            capture.publish(self.target, [{"id": "x"}], "notepad-smoke", self.out, self.STATE * 2,
                            self.manifest, self.store, profile="fhd-100", journal=self.journal)
        self.assertEqual({p: p.read_bytes() for p in before}, before)

    def test_a_kill_part_way_is_rolled_back_by_the_next_run(self):
        self.publish([{"id": "win11.notepad-smoke.window", "label": "Notepad", "behaviour": "ours"}])
        before = {p: p.read_bytes() for p in (self.target, self.manifest, self.stored())}
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

    def test_a_swap_killed_in_one_checkout_is_rolled_back_by_another(self):
        # Checkout A dies mid-swap, publishing with the default journal (beside
        # the shared store); checkout B's next run must find it and roll it back
        # before it publishes, so A has nothing left to undo later (review, 2026-11-21).
        self.publish([{"id": "win11.notepad-smoke.window", "label": "Notepad", "behaviour": "ours"}])
        shot = self.stored()
        before = {p: p.read_bytes() for p in (self.target, self.manifest, shot)}
        script = f"""
import importlib.util, os, pathlib
spec = importlib.util.spec_from_file_location("capture_host", {str(INV / "capture.py")!r})
capture = importlib.util.module_from_spec(spec); spec.loader.exec_module(capture)
real = os.replace
def dying(src, dst):
    if str(dst).endswith(".old") and "window.json" in str(dst):
        real(src, dst)
        os._exit(9)
    return real(src, dst)
capture.os.replace = dying
capture.JOURNAL = pathlib.Path({str(self.tmp / "a" / "publish.journal")!r})
t = pathlib.Path({str(self.tmp)!r})
capture.publish(t / "inventory" / "notepad-smoke" / "window.json", [{{"id": "win11.notepad-smoke.window", "label": "New"}}],
                "notepad-smoke", t / "out", {self.STATE!r}, t / "shots.json", t / "store", profile="fhd-100")
"""
        r = capture.subprocess.run([sys.executable, "-c", script], capture_output=True, timeout=60)
        self.assertEqual(r.returncode, 9, r.stderr)
        shared = capture.journal_for(self.store)
        self.assertTrue(shared.exists())
        self.assertFalse((self.tmp / "a" / "publish.journal").exists())        # nothing private to checkout A
        saved = capture.STORE, capture.JOURNAL
        capture.STORE, capture.JOURNAL = self.store, self.tmp / "b" / "publish.journal"   # checkout B's run
        try:
            self.assertTrue(capture.recover_pending())
            self.assertFalse(capture.recover_pending())
        finally:
            capture.STORE, capture.JOURNAL = saved
        self.assertEqual({p: p.read_bytes() for p in before}, before)
        self.assertFalse(shared.exists())

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
        for name in ("fhd-100-rtl", "wide-100", "tall-100", "qhd-200", "ime"):
            self.assertEqual(core.checkpoint_for(name, self.profiles), f"profile-{name}", name)

    def test_a_chord_is_held_before_it_is_released(self):
        # File Explorer's XAML reads the modifier state when it handles the
        # letter: a chord sent down and up in one batch reached it with Alt
        # and Ctrl already up (2026-11-19, Alt-D and Ctrl-T did nothing).
        batches = []
        with mock.patch.object(guest, "send_keys", lambda seq, what: batches.append(list(seq))), \
                mock.patch.object(guest.time, "sleep", lambda s: None):
            guest.keys("alt+d")
        alt, d = guest.chord_codes("alt+d")
        self.assertEqual(batches, [[(alt, 0), (d, 0)], [(d, 2), (alt, 2)]])

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

    def test_the_laptop_has_a_sound_output_that_plays_nowhere(self):
        cmd = " ".join(self.runner.qemu_cmd("o.qcow2", "v.fd", "a.aml", "j.img", {}, "q.sock"))
        self.assertIn("-audiodev none,id=snd", cmd)
        self.assertIn("hda-output,audiodev=snd,bus=hda.0", cmd)

    def test_the_guest_waits_for_room_before_it_boots(self):
        free = iter([3000, 4000, 5200])
        now = iter([0, 10, 20, 30])
        got = self.runner.wait_for_memory(5120, 600, available=lambda: next(free), clock=lambda: next(now),
                                          sleep=lambda s: None)
        self.assertEqual(got, (5200, True))

    def test_the_guest_is_not_booted_into_a_full_host(self):
        now = iter([0, 300, 600])
        got = self.runner.wait_for_memory(5120, 600, available=lambda: 3000, clock=lambda: next(now),
                                          sleep=lambda s: None)
        self.assertEqual(got, (3000, False))

    def test_the_memory_wait_covers_the_guest_it_boots(self):
        cmd = self.runner.qemu_cmd("o.qcow2", "v.fd", "a.aml", "j.img", {}, "q.sock")
        self.assertEqual(cmd[cmd.index("-m") + 1], str(self.runner.GUEST_MB))
        self.assertGreater(self.runner.available_mb(), 0)

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
        except (FileNotFoundError, ProcessLookupError):        # gone before the open, or during the read (ESRCH)
            return True

    def test_a_process_that_vanishes_during_the_read_is_gone(self):
        # The kernel answers ESRCH, not ENOENT, when it ends between the open and the read (CI, 2026-11-23).
        for gone in (FileNotFoundError(), ProcessLookupError()):
            with mock.patch.object(pathlib.Path, "read_text", side_effect=gone):
                self.assertTrue(self._gone(os.getpid()))
        self.assertFalse(self._gone(os.getpid()))

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

    def test_a_checkout_is_refused_while_another_holds_the_shot_store(self):
        # Two checkouts have two capture locks but one shot store: the store's
        # own lock is what keeps their publications apart.
        real = capture.LOCK, capture.STORE_LOCK
        capture.LOCK, capture.STORE_LOCK = self.tmp / "a" / "capture.lock", self.tmp / "ref-shots.lock"
        try:
            with capture.exclusive(capture.STORE_LOCK):          # the other checkout's run
                with self.assertRaisesRegex(BlockingIOError, "ref-shots.lock"):
                    with capture.held():
                        pass
                with capture.exclusive(capture.LOCK):             # this checkout's lock was let go
                    pass
            with capture.held():
                pass
        finally:
            capture.LOCK, capture.STORE_LOCK = real

    def test_the_guest_run_does_not_hold_the_shot_store(self):
        # Another checkout publishes while this one's guest runs (review, 2026-11-21);
        # publication then holds the store again.
        real = capture.LOCK, capture.STORE_LOCK
        capture.LOCK, capture.STORE_LOCK = self.tmp / "a" / "capture.lock", self.tmp / "ref-shots.lock"
        during = []

        def guest(*a, **k):
            with capture.exclusive(capture.STORE_LOCK):         # the other checkout's publication
                during.append("published")
            return self.tmp, 2                                  # rc 2: the capture stops here
        args = types.SimpleNamespace(surface="s", profile="fhd-100", timeout=1, fixture=None, prepare=None,
                                     standin=None, module=None)
        try:
            with mock.patch.object(capture, "run_guest", guest), mock.patch.object(capture, "recover_pending",
                                                                                     lambda: False):
                with capture.held(store=False):
                    self.assertEqual(capture.capture_and_publish(args), 2)
            self.assertEqual(during, ["published"])
            with capture.exclusive(capture.STORE_LOCK), self.assertRaises(BlockingIOError):
                with capture.store_held(wait=0):
                    pass
        finally:
            capture.LOCK, capture.STORE_LOCK = real

    def test_no_store_drive_means_only_the_checkout_lock(self):
        real = capture.LOCK, capture.STORE_LOCK
        capture.LOCK, capture.STORE_LOCK = self.tmp / "capture.lock", self.tmp / "absent" / "ref-shots.lock"
        try:
            with capture.held():
                self.assertFalse(capture.STORE_LOCK.parent.exists())
        finally:
            capture.LOCK, capture.STORE_LOCK = real

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
        self.assertEqual(calls[1][0][-3:], ["-Stop", "-Checkpoint", "clean"])
        self.assertIn("is off", out.getvalue())
        self.assertEqual([p for p in self.tmp.glob("*/in")], [])   # staging removed

    def test_two_runs_in_one_second_keep_their_own_inputs(self):
        # Two checkouts starting one label in the same second shared a staging
        # folder, and one's cleanup deleted the other's inputs (review, 2026-11-21).
        real = capture.subprocess.run, capture.WORK, capture.winpath, capture.time.strftime
        a, b = self.tmp / "a.py", self.tmp / "b.py"
        a.write_text("first"), b.write_text("second")
        seen = {}

        def run(cmd, **kw):
            stage = pathlib.Path(cmd[cmd.index("-In") + 1])
            if "outer" not in seen:
                seen["outer"] = stage
                capture.ref_run("notepad", {"x.py": b}, "inner", 60)
                seen["after"] = (stage / "x.py").read_text()
            return capture.subprocess.CompletedProcess(cmd, 0, "", "")
        capture.subprocess.run, capture.WORK, capture.winpath = run, self.tmp, str
        capture.time.strftime = lambda fmt, *a: "20261121-080000"
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                out, _ = capture.ref_run("notepad", {"x.py": a}, "outer", 60)
        finally:
            capture.subprocess.run, capture.WORK, capture.winpath, capture.time.strftime = real
        self.assertEqual(seen["after"], "first")
        self.assertEqual(out.parent.name.startswith("notepad-20261121-080000-"), True)
        self.assertEqual(len(list(self.tmp.glob("notepad-20261121-080000-*"))), 2)


    def test_a_failed_guest_run_reports_why(self):
        # ref-run's guest-command deadline throws, and PowerShell writes the
        # error to stderr; dropped, the capture said only "no meta.json"
        # (2026-11-18, a switching capture past the 900 s default).
        real_run, real_work, real_win = capture.subprocess.run, capture.WORK, capture.winpath

        def fake(cmd, **kw):
            return capture.subprocess.CompletedProcess(
                cmd, 1, "", "Wait-Until : timed out after 900 s waiting for the interactive command\r\n")
        capture.subprocess.run, capture.WORK, capture.winpath = fake, self.tmp, str
        try:
            with contextlib.redirect_stdout(io.StringIO()) as out:
                _, rc = capture.run_guest("notepad-smoke", "fhd-100", 900)
        finally:
            capture.subprocess.run, capture.WORK, capture.winpath = real_run, real_work, real_win
        self.assertEqual(rc, 1)
        self.assertIn("timed out after 900 s", out.getvalue())

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

    def test_an_interrupt_before_ref_run_starts_leaves_the_vm_alone(self):
        # An interrupt while staging or converting a path restored the
        # checkpoint of a VM no run had touched (review, 2026-11-19).
        for where in ("copy", "winpath"):
            calls = []
            real = capture.subprocess.run, capture.WORK, capture.winpath, capture.shutil.copy

            def run(cmd, **kw):
                calls.append(cmd)
                return capture.subprocess.CompletedProcess(cmd, 0, "", "")

            def stop(*_):
                raise KeyboardInterrupt
            win = (lambda p: stop() if pathlib.Path(p).name == "in" else str(p)) if where == "winpath" else str
            capture.subprocess.run, capture.WORK, capture.winpath = run, self.tmp, win
            if where == "copy":
                capture.shutil.copy = stop
            try:
                with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(KeyboardInterrupt):
                    capture.ref_run("x", {"a.txt": __file__}, "cmd", 10)
            finally:
                capture.subprocess.run, capture.WORK, capture.winpath, capture.shutil.copy = real
            self.assertEqual(calls, [], where)
            self.assertEqual(list(self.tmp.glob("x-*/in")), [], where)

    def test_a_machine_profile_run_is_restored_to_its_own_checkpoint(self):
        # Recovery restored clean after a profile-<id> run (review, 2026-11-18).
        calls = []
        real_run, real_work, real_win = capture.subprocess.run, capture.WORK, capture.winpath

        def fake(cmd, **kw):
            calls.append(cmd)
            if "-Stop" in cmd:
                return capture.subprocess.CompletedProcess(cmd, 0, "ref-run: restored", "")
            raise capture.subprocess.TimeoutExpired(cmd, kw["timeout"])
        capture.subprocess.run, capture.WORK, capture.winpath = fake, self.tmp, str
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                capture.ref_run("x", {}, "cmd", 10, checkpoint="profile-dual")
        finally:
            capture.subprocess.run, capture.WORK, capture.winpath = real_run, real_work, real_win
        self.assertEqual(calls[1][-3:], ["-Stop", "-Checkpoint", "profile-dual"])


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
        self.assertIn("differ at 'a' (label)", str(ex.exception))

    def test_the_failure_names_a_record_only_one_walk_has(self):
        self.assertEqual(core.first_difference([{"id": "a"}], [{"id": "a"}, {"id": "b"}]),
                         "'b', which only the last walk has")
        self.assertEqual(core.first_difference([{"id": "a"}, {"id": "b"}], [{"id": "a"}]),
                         "'b', which only the walk before has")
        self.assertEqual(core.first_difference([{"id": "a"}, {"id": "b"}], [{"id": "b"}, {"id": "a"}]),
                         "the order of their records")

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

    def test_a_measuring_scene_is_opened_and_closed_but_never_walked(self):
        m, log = self.mod()
        m.SCENES[0].update(keep=False, walk=False,
                           root=lambda ctx: (_ for _ in ()).throw(AssertionError("walked")))
        cap, passes = core.run_scenes(core.scenes_of(m), None, self.walk_scene)
        self.assertEqual(log, ["open desktop", "close desktop", "open menu", "close menu"])
        self.assertEqual(passes["desktop"], 0)
        self.assertEqual(cap.measured, [])
        self.assertEqual([r["id"] for r in cap.records if "parent" not in r], ["win11.scenetest.menu"])
        m.SCENES[0]["keep"] = True
        with self.assertRaisesRegex(ValueError, "walk=False needs keep=False"):
            core.scenes_of(m)

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

    def test_a_provider_timeout_is_a_transient_failure(self):
        class COMError(Exception):
            pass
        self.assertTrue(guest.transient(COMError(guest.UIA_E_TIMEOUT, None, None), COMError))
        self.assertTrue(guest.transient(COMError(guest.EVENT_E_ALL_SUBSCRIBERS_FAILED, None, None), COMError))
        self.assertFalse(guest.transient(COMError(-2147024891, None, None), COMError))   # access denied is not
        self.assertFalse(guest.transient(COMError(), COMError))
        self.assertFalse(guest.transient(ValueError(guest.UIA_E_TIMEOUT), COMError))

    def test_a_transient_failure_reopens_the_scene_a_bounded_number_of_times(self):
        class Dropped(Exception):
            pass
        m, log = self.mod()
        fails = [2]
        walk = m.SCENES[0]["root"]

        def flaky(ctx):
            if fails[0]:
                fails[0] -= 1
                raise Dropped()
            return walk(ctx)
        m.SCENES[0]["root"] = flaky
        cap, _ = core.run_scenes(core.scenes_of(m), None, self.walk_scene, transient=lambda e: isinstance(e, Dropped))
        self.assertEqual(log[:6], ["open desktop", "close desktop"] * 3)
        self.assertIn("win11.scenetest.desktop", [r["id"] for r in cap.records])
        # Past SCENE_TRIES opens it fails; and a failure not named transient is never retried.
        m, log = self.mod()
        m.SCENES[0]["root"] = lambda ctx: (_ for _ in ()).throw(Dropped())
        with self.assertRaises(Dropped):
            core.run_scenes(core.scenes_of(m), None, self.walk_scene, transient=lambda e: isinstance(e, Dropped))
        self.assertEqual(log.count("open desktop"), core.SCENE_TRIES)
        m, log = self.mod()
        m.SCENES[0]["root"] = lambda ctx: (_ for _ in ()).throw(Dropped())
        with self.assertRaises(Dropped):
            core.run_scenes(core.scenes_of(m), None, self.walk_scene)
        self.assertEqual(log, ["open desktop", "close desktop"])
        # A transient failure inside the open closes the scene before the retry.
        m, log = self.mod()
        opens = [1]
        logged = m.SCENES[0]["open"]

        def flaky_open(ctx):
            logged(ctx)
            if opens[0]:
                opens[0] -= 1
                raise Dropped()
        m.SCENES[0]["open"] = flaky_open
        core.run_scenes(core.scenes_of(m), None, self.walk_scene, transient=lambda e: isinstance(e, Dropped))
        self.assertEqual(log[:4], ["open desktop", "close desktop", "open desktop", "close desktop"])

    def test_a_failure_names_its_scene_and_step(self):
        def boom(ctx):
            raise core.Incomplete("gone")
        for key, step in (("open", "opening"), ("root", "walking"), ("close", "closing")):
            m, _ = self.mod()
            m.SCENES[1][key] = boom
            with self.assertRaises(core.Incomplete) as cm:
                core.run_scenes(core.scenes_of(m), None, self.walk_scene)
            self.assertEqual(cm.exception.__notes__, [f"scene menu: {step}"], key)

    def test_the_guest_report_says_where_a_failure_happened(self):
        m, _ = self.mod()

        def open_calendar(ctx):
            raise RuntimeError("provider gone")
        m.SCENES[1]["open"] = open_calendar
        try:          # not assertRaises: it drops the traceback the report reads
            core.run_scenes(core.scenes_of(m), None, self.walk_scene)
            self.fail("the scene did not fail")
        except RuntimeError as exc:
            got = guest.describe_failure(exc, True)
        self.assertTrue(got.startswith("UI Automation failed: provider gone (scene menu: opening; at test_capture.py:"),
                        got)
        self.assertTrue(got.endswith(" in open_calendar)"), got)

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

    def test_an_omitted_element_is_left_out_with_its_subtree(self):
        t = node("Taskbar", "Pane", bounds=(0, 0, 400, 48), children=[
            node("", "Pane", aid="TaskbarFrame", bounds=(0, 0, 300, 48), children=[node("Start", "Button")]),
            node("Show Hidden Icons", "Button", bounds=(300, 0, 32, 48))])
        cap = core.walk(core.FixtureElement(t), surface="taskbar", build="26300.9974", profile="fhd-100",
                        session="console", root_slug="tray", root_label="Tray", root_kind="region",
                        omit=[{"automationId": "TaskbarFrame"}])
        self.assertEqual([r["label"] for r in cap.records], ["Tray", "Show Hidden Icons"])
        self.assertEqual(cap.skipped["elsewhere"], 1)
        tree = core.dump(core.FixtureElement(t), omit=[{"automationId": "TaskbarFrame"}])
        self.assertNotIn("TaskbarFrame", json.dumps(tree))

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
    def test_measured_bounds_are_authored_by_metrics_never_by_hand(self):
        zone = {"id": "z", "kind": "region", "label": "Zone", "order": 0,
                "bounds": {"x": 0, "y": 0, "width": 960, "height": 1032}}
        (r,) = core.authored([zone], surface="window", build="1", source="uia", profile="fhd-100", session="console")
        self.assertEqual(r["bounds"]["width"], 960)
        with self.assertRaises(ValueError):
            core.authored([zone], surface="window", build="1", source="hand")

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

    def test_a_measured_colour_role_is_authored_and_checks(self):
        m, _ = Scenes().mod()
        cap, _ = core.run_scenes(core.scenes_of(m), None, Scenes().walk_scene)
        hover = core.authored([{"id": "desktop.close-hover", "parent": "desktop", "kind": "state", "label": "Close, hover",
                                "order": 3, "state": "hover", "colours": {"fill": {"light": "#c42b1c"}}}],
                              surface="scenetest", build="26300.9974", source="uia", profile="fhd-100", session="console")
        self.assertEqual(hover[0]["colours"], {"fill": {"light": "#c42b1c"}})
        code, report = capture.check_scratch({"scenetest/scenetest.json": cap.records + hover})
        self.assertEqual(code, 0, report)
        bad = [dict(hover[0], colours={"fill": {"light": "red"}})]
        code, _ = capture.check_scratch({"scenetest/scenetest.json": cap.records + bad})
        self.assertNotEqual(code, 0)

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

    def test_the_shared_helpers_are_staged_with_every_module(self):
        # A module may import a helper from surfaces/shared/; the guest has only what is staged.
        seen = {}
        real = capture.ref_run
        capture.ref_run = lambda label, files, cmd, timeout, checkpoint: seen.update(files=files) or ("out", 0)
        try:
            capture.run_guest("notepad-smoke", "fhd-100", 60)
        finally:
            capture.ref_run = real
        shared = sorted(p.name for p in (INV / "surfaces" / "shared").glob("*.py"))
        self.assertTrue(shared)
        for name in shared:
            self.assertEqual(seen["files"][f"surfaces/shared/{name}"], INV / "surfaces" / "shared" / name)

    def test_a_module_imports_its_shared_helper_the_way_the_guest_loads_it(self):
        m = guest.load_surface("explorer-frame")
        self.assertIs(m.ew, sys.modules["explorer_window"])

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
        # What the icon-size openers read from the screen (26300.9974).
        self.desk.MEASURED.update({"large": {"art": (72, 84), "label": (60, 12)},
                                   "medium": {"art": (36, 42), "label": (62, 12)},
                                   "small": {"art": (24, 28), "label": (62, 12)},
                                   "hover": (76, 70), "selected": (76, 70),
                                   "long": {"wrap": 70, "height": 85, "selected": 160}})

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

    def test_the_pixel_reads_are_published(self):
        got = {m["id"]: m["measure"]["value"] for m in self.desk.METRICS(self.cells())}
        self.assertEqual((got["desktop.art-width-large"], got["desktop.art-height-small"],
                          got["desktop.label-width-medium"], got["desktop.selected-width"],
                          got["desktop.hover-height"]), (72, 28, 62, 76, 70))

    def test_a_long_label_gives_its_wrap_width_and_line_counts(self):
        got = {m["id"]: m["measure"] for m in self.desk.METRICS(self.cells())}
        self.assertEqual(got["desktop.label-wrap-width"]["value"], 70)
        self.assertEqual(got["desktop.label-lines"], {"value": 2, "unit": "count", "scaling": "fixed"})
        self.assertEqual(got["desktop.label-lines-selected"]["value"], 7)     # (160 - 70) / 15 + 1

    def test_a_long_label_that_is_not_whole_lines_fails_the_capture(self):
        self.desk.MEASURED["long"]["selected"] = 161
        with self.assertRaises(ValueError):
            self.desk.METRICS(self.cells())

    def test_a_hung_explorer_fails_the_icon_toggle_instead_of_waiting(self):
        sent = []

        def send(*args):
            sent.append(args)
            return 0                                 # timed out, or the window is hung
        fake = types.SimpleNamespace(FindWindowExW=lambda *a: 7, SendMessageTimeoutW=send)
        ctx = types.SimpleNamespace()
        with mock.patch.object(self.desk, "user32", fake), \
                mock.patch.object(self.desk, "desktop", lambda c: types.SimpleNamespace(
                    el=types.SimpleNamespace(CurrentNativeWindowHandle=1))):
            with self.assertRaises(self.desk.core.Incomplete):
                self.desk.toggle_icons(ctx)
        self.assertEqual(sent[0][4:6], (self.desk.SMTO_ABORTIFHUNG, self.desk.SEND_TIMEOUT_MS))

    def test_a_missing_pixel_read_fails_the_capture(self):
        del self.desk.MEASURED["hover"]
        with self.assertRaises(ValueError):
            self.desk.METRICS(self.cells())

    def test_a_missing_measuring_scene_fails_the_capture(self):
        with self.assertRaises(ValueError):
            self.desk.METRICS([c for c in self.cells() if c["id"] != "win11.desktop.icons-large"])

    def test_hand_records_and_metrics_do_not_share_an_order(self):
        orders = [m["order"] for m in self.desk.METRICS(self.cells())] + [h["order"] for h in self.desk.HAND]
        self.assertEqual(len(orders), len(set(orders)))


class PollDeadlines(unittest.TestCase):
    def test_a_helper_wait_ends_with_the_poll_it_runs_in(self):
        ctx = Ctx()
        self.assertEqual(ctx.capped(15), 15)                     # no poll: the helper's own wait
        self.assertFalse(ctx.overdue())
        with ctx.within(2) as end:
            self.assertLessEqual(ctx.capped(15), 2)
            with ctx.within(60) as inner:
                self.assertEqual(inner, end)                     # an inner poll never outlives its outer one
            self.assertEqual(ctx.capped(1), 1)
        with ctx.within(0):
            time.sleep(0.01)
            self.assertTrue(ctx.overdue())
            with self.assertRaises(core.Incomplete):            # an ended poll is no single whole look
                ctx.capped(15)
        self.assertFalse(ctx.overdue())

    def test_a_find_inside_a_poll_ends_with_the_poll(self):
        live = types.SimpleNamespace(children=lambda el: [], uia=types.SimpleNamespace(GetRootElement=lambda: None),
                                     COMError=OSError)
        ctx = guest.Context(live)
        start = time.monotonic()
        with ctx.within(0.3):
            with self.assertRaises(guest.core.Incomplete):
                ctx.find(timeout=15, name="Never")
        self.assertLess(time.monotonic() - start, 2)          # not the helper's own 15 s

    def test_an_ended_poll_fetches_nothing(self):
        # An expired poll made find() search a whole tree and return a late match (review, 2026-11-21).
        fetched = []
        target = types.SimpleNamespace(name="Target", offscreen=False, control_type="Window", automation_id="",
                                       class_name="Target", children=lambda: fetched.append(1) or [])

        def children(el, cond=None):
            fetched.append(1)
            return [target]
        live = types.SimpleNamespace(children=children, uia=types.SimpleNamespace(
            GetRootElement=lambda: None, CreateTrueCondition=lambda: None), COMError=OSError)
        ctx = guest.Context(live)
        with ctx.within(0):
            time.sleep(0.01)
            for look in (lambda: ctx.find(name="Target"), lambda: ctx.find_window(name="Target")):
                with self.assertRaises(guest.core.Incomplete):
                    look()
        self.assertEqual(fetched, [])
        self.assertIs(ctx.find(timeout=0, name="Target"), target)   # an explicit single look still runs


class ExplorerDescendants(unittest.TestCase):
    """shared/explorer_window.py descendants(): one element budget for the look."""

    def setUp(self):
        guest.load_surface("explorer-frame")
        self.ew = sys.modules["explorer_window"]

    def test_wide_branches_together_are_refused(self):
        # Two branches, each within the budget, that together pass it (review, 2026-11-21).
        leaf = lambda: types.SimpleNamespace(class_name="Leaf", name="x", el=[])   # noqa: E731
        branch = types.SimpleNamespace(class_name="Group", name="g", el=[leaf(), leaf()])
        root = types.SimpleNamespace(class_name="Root", name="r", el=[branch, branch])
        asked = []

        def children(e, cond, limit):
            asked.append(limit)
            if len(e) > limit:
                raise self.ew.core.Incomplete("over")
            return e
        ctx = Ctx(live=types.SimpleNamespace(
            children=children, uia=types.SimpleNamespace(CreateTrueCondition=lambda: None)))
        saved = self.ew.core.MAX_RECORDS
        self.ew.core.MAX_RECORDS = 5
        try:
            with self.assertRaises(self.ew.core.Incomplete):
                self.ew.descendants(ctx, root, 3)
            self.assertEqual(asked, [5, 3, 1])         # what was left, after each fetch was charged
            self.assertEqual(len(self.ew.descendants(ctx, root, 1)), 2)
            skipped = self.ew.descendants(ctx, root, 2, skip=("Group",))
            self.assertEqual(skipped, [])               # a skipped parent's children are never fetched
        finally:
            self.ew.core.MAX_RECORDS = saved

    def ctx(self):
        def children(e, cond, limit):
            if len(e) > limit:
                raise self.ew.core.Incomplete("over")
            return e
        return Ctx(live=types.SimpleNamespace(children=children,
                                              uia=types.SimpleNamespace(CreateTrueCondition=lambda: None)))

    def test_a_tree_exactly_at_the_limit_is_read_whole(self):
        leaf = lambda: types.SimpleNamespace(class_name="Leaf", name="x", el=[])   # noqa: E731
        root = types.SimpleNamespace(class_name="Root", name="r", el=[leaf(), leaf()])
        saved = self.ew.core.MAX_RECORDS
        self.ew.core.MAX_RECORDS = 2
        try:
            self.assertEqual(len(self.ew.descendants(self.ctx(), root, 2)), 2)     # leaves have no children
            root.el[0].el.append(leaf())                                          # one more is one too many
            with self.assertRaises(self.ew.core.Incomplete):
                self.ew.descendants(self.ctx(), root, 2)
        finally:
            self.ew.core.MAX_RECORDS = saved

    def test_a_look_stops_at_what_it_waits_for_and_at_its_poll_deadline(self):
        leaf = lambda n: types.SimpleNamespace(class_name="Leaf", name=n, el=[])  # noqa: E731
        deep = types.SimpleNamespace(class_name="Group", name="g", el=[leaf("deep")])
        root = types.SimpleNamespace(class_name="Root", name="r", el=[leaf("want"), deep])
        found = self.ew.descendants(self.ctx(), root, 3, stop=lambda k: k.name == "want")
        self.assertNotIn("deep", [k.name for k in found])                         # no level past the match
        ctx = self.ctx()
        with ctx.within(0):
            time.sleep(0.01)
            with self.assertRaisesRegex(self.ew.core.Incomplete, "deadline"):
                self.ew.descendants(ctx, root, 3)


class ExplorerMenu(unittest.TestCase):
    """shared/explorer_window.py menu(): the open menu, never a closed one's."""

    def setUp(self):
        guest.load_surface("explorer-frame")
        self.ew = sys.modules["explorer_window"]

    def run_menu(self, tree):
        el = lambda **kw: types.SimpleNamespace(**{"control_type": "Pane", "offscreen": False, **kw})
        focus = el(control_type="ListItem")
        live = types.SimpleNamespace(uia=types.SimpleNamespace(GetFocusedElement=lambda: focus),
                                     wrap=lambda e: e, _parent=lambda e: None)
        ctx = Ctx(live=live)
        with mock.patch.object(self.ew, "window", lambda c, wait=None: None), \
             mock.patch.object(self.ew, "descendants", lambda c, root, depth, skip=(): tree(el)), \
             mock.patch.object(self.ew, "focused", lambda c: {}), mock.patch.object(self.ew.time, "sleep", lambda s: None):
            return self.ew.menu(ctx, wait=0)

    def test_the_menu_on_screen_is_taken_over_a_closed_one(self):
        got = self.run_menu(lambda el: [el(control_type="Menu", offscreen=True, name="stale"),
                                        el(control_type="Menu", name="open")])
        self.assertEqual(got.name, "open")

    def test_the_window_lookup_is_one_look_inside_the_menu_poll(self):
        # A lookup with its own 20 s wait ran inside a 5 s menu poll (review, 2026-11-21).
        waits = []
        focus = types.SimpleNamespace(control_type="ListItem", offscreen=False)
        live = types.SimpleNamespace(uia=types.SimpleNamespace(GetFocusedElement=lambda: focus),
                                     wrap=lambda e: e, _parent=lambda e: None)

        def gone(c, wait=None):
            waits.append(wait)
            raise self.ew.core.Incomplete("no File Explorer window")
        start = time.monotonic()
        with mock.patch.object(self.ew, "window", gone), mock.patch.object(self.ew, "focused", lambda c: {}), \
                self.assertRaisesRegex(self.ew.core.Incomplete, "holds the focus within 0.3 s"):
            self.ew.menu(Ctx(live=live), wait=0.3)
        self.assertLess(time.monotonic() - start, 1.5)
        self.assertTrue(waits and set(waits) == {0}, waits)

    def test_only_a_closed_menu_is_no_menu(self):
        with self.assertRaises(guest.core.Incomplete):
            self.run_menu(lambda el: [el(control_type="Menu", offscreen=True, name="stale")])


class ExplorerViews(unittest.TestCase):
    """The views module's records from what its measured scenes saw."""

    def setUp(self):
        self.m = guest.load_surface("explorer-views")

    def found(self):
        one, long_ = self.m.ONE_LINE, self.m.LONG
        # Two cells in a row and one below: the gaps are 7 and 9 px.
        icons = {one: {"box": [422, 282, 105, 116], "label": [441, 380, 68, 16]},
                 "Other": {"box": [534, 282, 105, 116], "label": [551, 380, 68, 16]},
                 long_: {"box": [422, 407, 105, 131], "label": [441, 505, 68, 31], "icon": [446, 420, 36, 48]}}
        rows = {one: {"box": [422, 307, 598, 24], "label": [444, 310, 236, 19]},
                long_: {"box": [422, 335, 598, 24], "label": [444, 338, 236, 19], "icon": [426, 339, 12, 16]}}
        return {slug: (icons if slug in self.m.ICON_MODES else rows) for slug, _l, _c in self.m.MODES}

    def test_each_mode_gets_its_sizes(self):
        got = {r["id"]: r for r in self.m.mode_metrics(self.found(), "normal")}
        self.assertEqual(got["views.large-icons.icon-slot"]["measure"]["value"], 98)         # label top less cell top
        self.assertEqual(got["views.large-icons.label-lines"]["measure"]["value"], 2)
        self.assertEqual(got["views.large-icons.column-gap"]["measure"]["value"], 7)
        self.assertEqual(got["views.large-icons.row-gap"]["measure"]["value"], 9)
        self.assertEqual(got["views.details.icon-slot"]["measure"]["value"], 22)             # label left less cell left
        self.assertEqual(got["views.details.row-gap"]["measure"]["value"], 4)
        self.assertEqual(got["views.large-icons.icon-height"]["measure"]["value"], 48)
        self.assertEqual(got["views.details.icon-width"]["measure"]["value"], 12)
        self.assertEqual(sum(1 for r in got.values() if r["kind"] == "state"), 8)
        compact = {r["id"] for r in self.m.mode_metrics(self.found(), "compact")}
        self.assertIn("views.details-compact.cell-height", compact)

    def test_a_mode_whose_items_were_not_read_stops_the_capture(self):
        found = self.found()
        found["tiles"] = {}
        with self.assertRaises(ValueError):
            self.m.mode_metrics(found, "normal")
        found = self.found()
        found["tiles"] = {k: {kk: vv for kk, vv in v.items() if kk != "icon"} for k, v in found["tiles"].items()}
        with self.assertRaises(ValueError):                          # no icon measured
            self.m.mode_metrics(found, "normal")

    def test_an_icon_is_the_artwork_in_its_slot(self):
        bg, ink = (255, 255, 255), (40, 90, 200)
        rows = [[ink if 3 <= x < 9 and 2 <= y < 12 else bg for x in range(14)] for y in range(16)]
        self.assertEqual(self.m.icon_bounds(rows), (3, 2, 6, 10))
        self.assertIsNone(self.m.icon_bounds([[bg] * 4] * 4))
        ruled = [[ink if (y in (0, 15) and x >= 1) or (3 <= x < 9 and 2 <= y < 12) else bg for x in range(14)]
                 for y in range(16)]                                           # the content view's row lines
        self.assertEqual(self.m.icon_bounds(ruled), (3, 2, 6, 10))
        ctx = types.SimpleNamespace(pixels=lambda x, y, w, h: rows)
        entry = {"box": [100, 50, 14, 16], "label": [114, 50, 40, 16]}
        self.assertEqual(self.m.read_icon(ctx, entry, "details"), [103, 52, 6, 10])
        cut = [[ink if x < 9 else bg for x in range(14)] for y in range(16)]   # artwork running off the slot
        with self.assertRaises(self.m.core.Incomplete):
            self.m.read_icon(types.SimpleNamespace(pixels=lambda x, y, w, h: cut), entry, "details")
        self.assertEqual(self.m.icon_slot({"box": [0, 0, 105, 131], "label": [19, 98, 68, 31]}, "large-icons"),
                         (0, 0, 105, 98))

    def test_the_new_folder_names_are_checked(self):
        self.assertEqual(self.m.new_name_records(["New folder", "New folder (2)"])[0]["shortcut"], "Ctrl+Shift+N")
        with self.assertRaises(ValueError):
            self.m.new_name_records(["New folder"])

    def test_the_info_tip_rows_come_from_the_tip(self):
        r = self.m.tip_records(["Type: Text Document", "Size: 16 bytes", "Date modified: 11/19/2026 7:45 AM"])
        self.assertTrue(r[0]["behaviour"].endswith("Type, Size, Date modified."))
        with self.assertRaises(ValueError):
            self.m.tip_records(["Sample document"])

    def test_unnamed_elements_are_labelled_by_what_they_are(self):
        for aid, label in (("GroupCheckbox", "Select all"), ("PART_ScrollPresenter", "Scroll area")):
            el = types.SimpleNamespace(control_type="CheckBox", automation_id=aid, class_name="", name="")
            given = next((lab for spec, lab in self.m.LABELS if core.matches(el, spec)), None)
            self.assertEqual(given, label)

    def test_an_owner_lookup_is_one_look(self):
        # The info tip and owned-window polls have their own deadlines (review, 2026-11-21).
        waits = []

        def window(ctx, wait=20):
            waits.append(wait)
            raise self.m.core.Incomplete("no File Explorer window")
        with mock.patch.object(self.m, "window", window):
            self.assertEqual(self.m.owned_windows(None), [])
        self.assertEqual(waits, [0])

    def test_a_tooltip_already_on_the_desktop_is_not_the_info_tip(self):
        # An unrelated named tooltip present before the hover is never read as the
        # file's properties (review, 2026-11-21); the one the hover brings up is.
        stale = types.SimpleNamespace(control_type="ToolTip", name="Other: tip", bounds=(5, 5, 9, 9), offscreen=False,
                                      pid=1)
        fresh_tip = types.SimpleNamespace(control_type="ToolTip", name="Type: Text Document\nSize: 1 KB",
                                          bounds=(50, 60, 90, 40), offscreen=False, pid=1)
        tops = [stale]
        ctx = Ctx(live=types.SimpleNamespace(children=lambda el: list(tops),
                                             uia=types.SimpleNamespace(GetRootElement=lambda: None)),
                  hover=lambda el, settle=0: tops.append(fresh_tip), hover_point=lambda x, y: None)
        with mock.patch.multiple(self.m, fresh=lambda ctx: None, set_mode=lambda ctx, keys: None,
                                 window=lambda ctx, wait=20: types.SimpleNamespace(pid=1),
                                 item=lambda ctx, name: None, owned_windows=lambda ctx: []):
            self.m.measure_tip(ctx)
        self.assertEqual(self.m._m["tip"], ["Type: Text Document", "Size: 1 KB"])
        tops[:] = [stale]
        with mock.patch.multiple(self.m, fresh=lambda ctx: None, set_mode=lambda ctx, keys: None, TIP_WAIT=0,
                                 window=lambda ctx, wait=20: types.SimpleNamespace(pid=1),
                                 item=lambda ctx, name: None, owned_windows=lambda ctx: []):
            ctx.hover = lambda el, settle=0: None
            with self.assertRaises(self.m.core.Incomplete):
                self.m.measure_tip(ctx)

    def test_a_stale_owned_tooltip_is_not_the_info_tip(self):
        stale = types.SimpleNamespace(control_type="ToolTip", name="Unrelated: previous", bounds=(1, 1, 9, 9),
                                      offscreen=False, pid=1)
        tip = types.SimpleNamespace(control_type="ToolTip", name="Type: Text Document", bounds=(50, 60, 90, 40),
                                    offscreen=False, pid=1)
        owned = [stale]
        ctx = Ctx(live=types.SimpleNamespace(children=lambda el: [], uia=types.SimpleNamespace(GetRootElement=lambda: None)),
                  hover=lambda el, settle=0: owned.append(tip), hover_point=lambda x, y: None)
        with mock.patch.multiple(self.m, fresh=lambda ctx: None, set_mode=lambda ctx, keys: None,
                                 window=lambda ctx, wait=20: types.SimpleNamespace(pid=1),
                                 item=lambda ctx, name: None, owned_windows=lambda ctx: list(owned)):
            self.m.measure_tip(ctx)
            self.assertEqual(self.m._m["tip"], ["Type: Text Document"])
            owned[:] = [stale]
            ctx.hover = lambda el, settle=0: None
            with mock.patch.object(self.m, "TIP_WAIT", 0), self.assertRaises(self.m.core.Incomplete):
                self.m.measure_tip(ctx)                                  # no new tip: the stale one is not read

    def test_another_programs_tooltip_is_not_the_info_tip(self):
        other = types.SimpleNamespace(control_type="ToolTip", name="Other: tip", bounds=(5, 5, 9, 9), offscreen=False,
                                      pid=2)
        tip = types.SimpleNamespace(control_type="ToolTip", name="Type: Text Document", bounds=(50, 60, 90, 40),
                                    offscreen=False, pid=1)
        tops = []
        ctx = Ctx(live=types.SimpleNamespace(children=lambda el: list(tops),
                                             uia=types.SimpleNamespace(GetRootElement=lambda: None)),
                  hover=lambda el, settle=0: tops.extend([other, tip]), hover_point=lambda x, y: None)
        patch = dict(fresh=lambda ctx: None, set_mode=lambda ctx, keys: None, item=lambda ctx, name: None,
                     owned_windows=lambda ctx: [], window=lambda ctx, wait=20: types.SimpleNamespace(pid=1))
        with mock.patch.multiple(self.m, **patch):
            self.m.measure_tip(ctx)                                     # the other program's tip comes first
            self.assertEqual(self.m._m["tip"], ["Type: Text Document"])
            tops.clear()
            twin = types.SimpleNamespace(**dict(vars(tip), name="Size: 1 KB", bounds=(0, 0, 1, 1)))
            ctx.hover = lambda el, settle=0: tops.extend([tip, twin])
            with self.assertRaisesRegex(self.m.core.Incomplete, "2 tooltips"):
                self.m.measure_tip(ctx)

    def test_only_the_measured_items_have_their_labels_read(self):
        label = types.SimpleNamespace(automation_id="System.ItemNameDisplay", bounds=(1, 2, 3, 4))
        read = []

        def item_el(name):
            return types.SimpleNamespace(control_type="ListItem", name=name, bounds=(0, 0, 9, 9),
                                         children=lambda: read.append(name) or [label])
        view = types.SimpleNamespace(children=lambda: [item_el("a"), item_el(self.m.ONE_LINE)])
        with mock.patch.object(self.m, "items", lambda ctx: view):
            out = self.m.read_items(None, (self.m.ONE_LINE,))
        self.assertEqual(read, [self.m.ONE_LINE])
        self.assertEqual((out["a"]["label"], out[self.m.ONE_LINE]["label"]), (None, [1, 2, 3, 4]))

    def test_a_measured_item_off_screen_is_brought_into_view(self):
        shown = {self.m.ONE_LINE: {"box": [0, 0, 1, 1], "label": [0, 0, 1, 1]}}
        brought = []

        def bring(container, name):
            brought.append((container, name))
            shown[name] = {"box": [5, 5, 1, 1], "label": [5, 5, 1, 1]}
        ctx = types.SimpleNamespace(bring_into_view=bring)
        with mock.patch.object(self.m, "read_items", lambda c, labels=(): dict(shown)), \
                mock.patch.object(self.m, "items", lambda c: "list"):
            now = self.m.read_measured(ctx)
        self.assertEqual(brought, [("list", self.m.LONG)])
        self.assertIn(self.m.LONG, now)

    def test_ids_are_unique_across_metrics_and_hand_records(self):
        recs = (self.m.mode_metrics(self.found(), "normal") + self.m.mode_metrics(self.found(), "compact")
                + self.m.mode_key_records() + self.m.HAND)
        ids = [r["id"] for r in recs]
        self.assertEqual(len(ids), len(set(ids)))


class Invoke(unittest.TestCase):
    def test_an_element_without_the_invoke_pattern_is_named(self):
        ctx = guest.Context.__new__(guest.Context)
        el = types.SimpleNamespace(name="Compact view", automation_id="",
                                   el=types.SimpleNamespace(GetCurrentPattern=lambda pid: None))
        with self.assertRaisesRegex(guest.core.Incomplete, "Compact view: has no Invoke pattern"):
            ctx.invoke(el)


class BringIntoView(unittest.TestCase):
    def ctx(self):
        ctx = guest.Context.__new__(guest.Context)
        ctx.live = types.SimpleNamespace(COMError=OSError, UIA=types.SimpleNamespace(
            IUIAutomationItemContainerPattern="container", IUIAutomationVirtualizedItemPattern="virtual",
            IUIAutomationScrollItemPattern="scroll"))
        return ctx

    def pattern(self, calls, kind, **methods):
        return types.SimpleNamespace(QueryInterface=lambda iface: (calls.append((kind, iface)), types.SimpleNamespace(**methods))[1])

    def test_an_off_screen_item_is_found_realized_and_scrolled(self):
        calls = []
        item = types.SimpleNamespace(GetCurrentPattern=lambda pid: {
            guest.VIRTUALIZED_ITEM: self.pattern(calls, "v", Realize=lambda: calls.append("realize")),
            guest.SCROLL_ITEM: self.pattern(calls, "s", ScrollIntoView=lambda: calls.append("scroll"))}.get(pid))
        found = []
        container = types.SimpleNamespace(name="Items View", automation_id="", el=types.SimpleNamespace(
            GetCurrentPattern=lambda pid: self.pattern(calls, "c", FindItemByProperty=lambda start, prop, value: (
                found.append((start, prop, value)), item)[1]) if pid == guest.ITEM_CONTAINER else None))
        with mock.patch.object(guest.time, "sleep"):
            self.ctx().bring_into_view(container, "Long name")
        self.assertEqual(found, [(None, guest.PROP_NAME, "Long name")])
        self.assertEqual([c for c in calls if isinstance(c, str)], ["realize", "scroll"])

    def test_a_list_without_the_item_or_the_pattern_is_named(self):
        bare = types.SimpleNamespace(name="Items View", automation_id="", el=types.SimpleNamespace(GetCurrentPattern=lambda pid: None))
        with self.assertRaisesRegex(guest.core.Incomplete, "Items View: has no ItemContainer pattern"):
            self.ctx().bring_into_view(bare, "x")
        calls = []
        empty = types.SimpleNamespace(name="Items View", automation_id="", el=types.SimpleNamespace(
            GetCurrentPattern=lambda pid: self.pattern(calls, "c", FindItemByProperty=lambda *a: None)))
        with self.assertRaisesRegex(guest.core.Incomplete, "no item 'x'"):
            self.ctx().bring_into_view(empty, "x")


class ScenePairs(unittest.TestCase):
    def test_a_module_constant_of_the_wrong_shape_is_refused_by_name(self):
        mod = types.SimpleNamespace(KINDS=[("general", "General items")])
        with self.assertRaisesRegex(ValueError, "kinds holds"):
            core.Scene(mod, slug="s", label="S", open=None, root=None, close=None)
        ok = types.SimpleNamespace(KINDS=[({"controlType": "Pane"}, "region")])
        self.assertEqual(core.Scene(ok, slug="s", label="S", open=None, root=None, close=None).kinds[0][1], "region")


class ExplorerMenus(unittest.TestCase):
    """The File Explorer context-menu module: classic menu texts and the verb table."""

    def setUp(self):
        self.m = guest.load_surface("explorer-menus")

    def test_entry_text_gives_label_access_key_and_shortcut(self):
        self.assertEqual(self.m.parse_entry("&Open\tCtrl+O"), ("Open", "O", "Ctrl+O"))
        self.assertEqual(self.m.parse_entry("Cop&y as path"), ("Copy as path", "Y", None))
        self.assertEqual(self.m.parse_entry("Save && close"), ("Save & close", None, None))
        self.assertEqual(self.m.parse_entry("Cu&t"), ("Cut", "T", None))

    def test_verb_table_merges_menus_and_marks_the_default(self):
        entry = lambda t, d=False: {"text": t, "default": d, "submenu": False, "enabled": True}  # noqa: E731
        classic = {"document": [entry("&Open", True), {"separator": True}, entry("Cu&t\tCtrl+X")],
                   "folder": [entry("&Open", True), entry("Cu&t\tCtrl+X")],
                   "send-to-parent": [entry("Ignored")]}
        out = {r["id"]: r for r in self.m.verb_records(classic)}
        self.assertNotIn("menus.verbs.ignored", out)
        self.assertEqual(out["menus.verbs.cut"]["shortcut"], "Ctrl+X")
        self.assertIn("default (bold) entry on document, folder", out["menus.verbs.open"]["behaviour"])
        self.assertIn("Access key T", out["menus.verbs.cut"]["behaviour"])

    def test_one_verb_written_two_ways_is_one_record(self):
        entry = lambda t: {"text": t, "default": False, "submenu": False, "enabled": True}  # noqa: E731
        out = [r for r in self.m.verb_records({"a": [entry("Open wit&h")], "b": [entry("Open wit&h...")]})
               if r["kind"] == "menuItem"]
        self.assertEqual([r["id"] for r in out], ["menus.verbs.open-with"])
        self.assertIn("on different menus", out[0]["behaviour"])

    def test_two_shortcuts_for_one_verb_are_refused(self):
        entry = lambda t: {"text": t, "default": False, "submenu": False, "enabled": True}  # noqa: E731
        with self.assertRaises(ValueError):
            self.m.verb_records({"a": [entry("Cu&t\tCtrl+X")], "b": [entry("Cu&t\tShift+Del")]})

    def test_registered_verbs_match_their_baseline_rows(self):
        entry = {"text": "&Pin to Quick access", "default": False, "submenu": False, "enabled": True}
        verbs = [{"class": "Drive", "verb": "pintohome", "label": "Pin to Quick access", "default": None},
                 {"class": "*", "verb": "removeproperties", "label": None, "default": "Remove properties",
                  "programmatic": True},
                 {"class": "Directory", "verb": "Powershell", "label": "Open in Terminal", "default": None,
                  "velocity": "12345"}]
        verbs.append({"class": "Directory", "verb": "pinfolder", "label": "Pin to Quick access", "default": None,
                      "appliesTo": "System.IsFolder"})
        out = {r["id"]: r for r in self.m.registered_records(verbs, {"drive": [entry], "send-to-parent": [entry]})}
        # The drive's menu shows its own label; a folder verb of the same name is not credited with it.
        self.assertEqual(out["menus.registered.folder.pinfolder"]["kind"], "state")
        self.assertIn("no item read met that condition", out["menus.registered.folder.pinfolder"]["behaviour"])
        self.assertEqual(out["menus.registered.drive.pintohome"]["behaviour"], "Shown on drive.")
        pin = out["menus.registered.drive.pintohome"]
        self.assertEqual((pin["kind"], pin["baselineId"]), ("menuItem", "shell-commands:verb:Drive/pintohome"))
        self.assertIn("ProgrammaticAccessOnly", out["menus.registered.all-files.removeproperties"]["behaviour"])
        self.assertIn("12345", out["menus.registered.folder.powershell"]["behaviour"])
        with self.assertRaises(ValueError):
            self.m.registered_records([], {})

    def test_one_entry_is_credited_to_one_of_two_verbs_sharing_its_label(self):
        entry = {"text": "Turn on &BitLocker", "default": False, "submenu": False, "enabled": True}
        verbs = [{"class": "Drive", "verb": "encrypt-bde", "label": "Turn on &BitLocker", "default": None,
                  "appliesTo": "RequiresAdmin False"},
                 {"class": "Drive", "verb": "encrypt-bde-elev", "label": "Turn on &BitLocker", "default": None,
                  "appliesTo": "RequiresAdmin True"}]
        modern = [{"id": "win11.explorer.drive-menu.encrypt-bde-elev", "kind": "menuItem"}]
        out = {r["id"]: r for r in self.m.registered_records(verbs, {"drive": [entry]}, modern)}
        self.assertEqual(out["menus.registered.drive.encrypt-bde-elev"]["kind"], "menuItem")
        self.assertEqual(out["menus.registered.drive.encrypt-bde"]["kind"], "state")
        self.assertIn("no item read met that condition", out["menus.registered.drive.encrypt-bde"]["behaviour"])
        out = {r["id"]: r for r in self.m.registered_records(verbs, {"drive": [entry]}, [])}
        for v in ("encrypt-bde", "encrypt-bde-elev"):                  # no modern item: neither is credited alone
            self.assertIn("do not say which", out[f"menus.registered.drive.{v}"]["behaviour"])
        # The drive menu's modern item tells nothing about the removable menu (review, 2026-11-21).
        out = {r["id"]: r for r in self.m.registered_records(verbs, {"drive": [entry], "removable": [entry]}, modern)}
        elev = out["menus.registered.drive.encrypt-bde-elev"]["behaviour"]
        self.assertTrue(elev.startswith("Shown on drive; on removable it shares its label with encrypt-bde"), elev)
        self.assertNotIn("Shown on drive, removable", elev)
        self.assertTrue(out["menus.registered.drive.encrypt-bde"]["behaviour"].startswith(
            "On removable it shares its label with encrypt-bde-elev"))
        self.assertEqual(out["menus.registered.drive.encrypt-bde"]["kind"], "state")

    def test_icon_row_lists_the_triggers_that_show_each_button(self):
        row = {"height": 48}
        records = [{"id": "win11.explorer.document-menu.cut", "parent": "win11.explorer.document-menu", "bounds": row},
                   {"id": "win11.explorer.folder-menu.cut", "parent": "win11.explorer.folder-menu", "bounds": row},
                   {"id": "win11.explorer.classic-folder-menu.cut", "parent": "win11.explorer.classic-folder-menu",
                    "bounds": row},
                   {"id": "win11.explorer.document-menu.open", "parent": "win11.explorer.document-menu",
                    "bounds": {"height": 32}}]
        out = {r["id"]: r["behaviour"] for r in self.m.icon_row_records(records)}
        self.assertEqual(out["menus.icon-row.cut"], "Shown on: document, folder.")
        self.assertEqual(out["menus.icon-row.paste"], "Shown on none of the triggers.")


class ExplorerDialogs(unittest.TestCase):
    """The File Explorer dialogs module: sheet views, scene coverage and fixture scripts."""

    def setUp(self):
        self.m = guest.load_surface("explorer-dialogs")

    def test_the_address_path_names_no_id(self):
        m = guest.load_surface("dialogs-common")
        band = types.SimpleNamespace(name="Address: C:\\Users\\ref\\Documents", automation_id="1001",
                                     class_name="ToolbarWindow32", control_type="ToolBar")
        self.assertEqual(core._segment(band, m.IDENTITY, m.VALUES), "toolbarwindow32")
        refresh = types.SimpleNamespace(name='Refresh "Documents" (F5)', automation_id="", class_name="",
                                        control_type="Button")
        version = types.SimpleNamespace(name="Version 26H2 (OS Build 26300.9974)", automation_id="",
                                        class_name="Static", control_type="Text")
        self.assertEqual(core._segment(refresh, m.IDENTITY, m.VALUES), "button")
        self.assertEqual(core._segment(version, m.IDENTITY, m.VALUES), "static")

    def test_a_property_value_names_no_id(self):
        # The Details tab's value cells carry the machine's name and dates (2026-11-20).
        cell = types.SimpleNamespace(name="IOS-REF (this PC)", automation_id="", class_name="", control_type="Text")
        label = types.SimpleNamespace(name="Computer:", automation_id="", class_name="Static", control_type="Text")
        self.assertEqual(core._segment(cell, self.m.IDENTITY, self.m.VALUES), "text")
        self.assertEqual(core._segment(label, self.m.IDENTITY, self.m.VALUES), "computer")
        free = types.SimpleNamespace(name="65,572,872,192 bytes", automation_id="14407", class_name="Static",
                                     control_type="Text")
        item = types.SimpleNamespace(name="Type: Text Document Size: 16 bytes", automation_id="", class_name="Static",
                                     control_type="Text")
        self.assertEqual(core._segment(free, self.m.IDENTITY, self.m.VALUES), "free-space-bytes")
        self.assertEqual(core._segment(item, self.m.IDENTITY, self.m.VALUES), "item-details")
        self.assertTrue(any(core.matches(free, v) for v in self.m.VALUES))   # its amount labels nothing
        progress = types.SimpleNamespace(name="Paused - 32% complete", automation_id="eltSummary", class_name="Element",
                                         control_type="Text")
        self.assertTrue(any(core.matches(progress, v) for v in self.m.VALUES))
        tile = types.SimpleNamespace(name="Copying 1 item from A to B - Paused - 36% complete", automation_id="eltTile1",
                                     class_name="Element", control_type="Group")
        self.assertTrue(any(core.matches(tile, v) for v in self.m.VALUES))
        # The installed apps and the pinned folders are frames with no rows (review, 2026-11-20).
        group = types.SimpleNamespace(name="Default app", automation_id="", class_name="ListViewHeaderItem",
                                      control_type="Group")
        tree = types.SimpleNamespace(name="Navigation Pane", automation_id="100", class_name="SysTreeView32",
                                     control_type="Tree")
        self.assertTrue(all(any(core.matches(x, v) for v in self.m.ENVIRONMENT_LISTS) for x in (group, tree)))
        # The Up button names the parent, a wizard page its run-time GUID, the
        # estimate its time (review, 2026-11-20): none of them may reach an id.
        up = types.SimpleNamespace(name='Up to "Explorer sample" (Alt + Up Arrow)', automation_id="", class_name="",
                                   control_type="Button")
        page = types.SimpleNamespace(name="{2E505C65-F784-4044-A4D5-10DC3DFF2F5F}",
                                     automation_id="{2E505C65-F784-4044-A4D5-10DC3DFF2F5F}",
                                     class_name="Win32PropSheetPageHost", control_type="Pane")
        other = types.SimpleNamespace(name="{1C7014FA-5CC2-4FAF-ABA4-BA677E6650A2}",
                                      automation_id="{1C7014FA-5CC2-4FAF-ABA4-BA677E6650A2}",
                                      class_name="Win32PropSheetPageHost", control_type="Pane")
        eta = types.SimpleNamespace(name="About 2 minutes", automation_id="eltTimeRemaining", class_name="Element",
                                    control_type="Text")
        self.assertEqual(core._segment(page, self.m.IDENTITY, self.m.VALUES), "page")
        self.assertEqual(core._segment(other, self.m.IDENTITY, self.m.VALUES), "page")
        self.assertNotIn("explorer", core._segment(up, self.m.IDENTITY, self.m.VALUES))
        self.assertTrue(all(any(core.matches(x, v) for v in self.m.VALUES) for x in (up, page, eta)))
        made = types.SimpleNamespace(name="Date created: 11/20/2026 9:51 PM", automation_id="", class_name="",
                                     control_type="Text")
        ask = types.SimpleNamespace(name="Do you still want to merge the folder created on 11/20/2026 9:51 PM with this one?",
                                    automation_id="", class_name="", control_type="Text")
        self.assertEqual([core._segment(x, self.m.IDENTITY, self.m.VALUES) for x in (made, ask)],
                         ["date-created", "merge-question"])

    def view_records(self):
        p = "win11.explorer.folder-options-view"
        tree = f"{p}.advanced-settings-tree"
        rec = lambda rid, parent, kind, order, ct: {"id": rid, "parent": parent, "kind": kind,  # noqa: E731
                                                     "order": order, "controlType": ct}
        return tree, [rec(tree, p, "control", 3, "tree"),
                      rec(f"{tree}.nonclientverticalscrollbar", tree, "control", 0, "scrollbar"),
                      rec(f"{tree}.files-and-folders", tree, "setting", 1, "treeitem"),
                      rec(f"{tree}.files-and-folders.always-show-icons", f"{tree}.files-and-folders", "setting", 0,
                          "treeitem")]

    def test_closing_looks_again_after_its_last_pass(self):
        dw = sys.modules["dialog_window"]
        box = (101, "#32770", "Inventory sample")
        looks = iter([[box]] * dw.CLOSE_PASSES + [[]])     # the last pass's button closed it
        saved = dw.top_windows, dw.user32, dw.dismiss, dw.time.sleep, dw.describe
        fake = types.SimpleNamespace(PostMessageW=lambda *a: 0, SetForegroundWindow=lambda *a: 0,
                                     GetWindow=lambda *a: 0, GetForegroundWindow=lambda: 999)
        keys = []
        ctx = types.SimpleNamespace(keys=keys.append)
        dw.top_windows, dw.user32, dw.dismiss = (lambda c: set(next(looks))), fake, (lambda c, h: None)
        dw.time.sleep = lambda s: None
        dw.describe = lambda w: w[1:]
        dw._base["windows"] = set()
        try:
            dw.close_new_windows(ctx)                          # no raise: the window went
            self.assertEqual(keys, [])                         # never a key to a window not in front
            looks = iter([[box]] * (dw.CLOSE_PASSES + 1))
            dw.dismiss = lambda c, h: ["Yes"]
            with self.assertRaisesRegex(guest.core.Incomplete, r"no leave button among \{'Inventory sample': \['Yes'\]\}"):
                dw.close_new_windows(ctx)
        finally:
            dw.top_windows, dw.user32, dw.dismiss, dw.time.sleep, dw.describe = saved
            dw._base.clear()

    def tooltip_ctx(self, trees):
        """A ctx whose UIA tree is `trees`: {handle: (control_type, [children])}."""
        def el(ct, kids=()):
            return types.SimpleNamespace(control_type=ct, el=list(kids))
        def build(spec):
            ct, kids = spec
            return el(ct, [build(k) for k in kids])
        roots = {h: build(spec) for h, spec in trees.items()}
        live = types.SimpleNamespace(wrap=lambda h: roots[h], uia=types.SimpleNamespace(
            ElementFromHandle=lambda h: h, CreateTrueCondition=lambda: None),
            children=self.children_within, COMError=OSError)
        return Ctx(live=live)

    @staticmethod
    def children_within(e, cond, limit=None):
        if limit is not None and len(e) > limit:
            raise sys.modules["dialog_window"].core.Incomplete("over")
        return e

    def test_a_tooltip_host_look_is_bounded_and_ends_with_its_poll(self):
        dw = sys.modules["dialog_window"]
        tip = ("ToolTip", [("Text", [])])
        wide = ("Window", [("Pane", [tip] + [("Text", [])] * 40)] * 3)      # a tooltip in 123 elements
        self.assertTrue(dw.tooltip_only(self.tooltip_ctx({1: ("Window", [("Pane", [tip] + [("Text", [])] * 40)])}),
                                        (1, "Xaml_WindowedPopupClass", "PopupHost")))  # within the budget
        ctx = self.tooltip_ctx({1: wide})
        self.assertFalse(dw.tooltip_only(ctx, (1, "Xaml_WindowedPopupClass", "PopupHost")))   # past the budget: real
        with ctx.within(0):
            time.sleep(0.01)
            with self.assertRaises(dw.core.Incomplete):
                dw.tooltip_only(ctx, (1, "Xaml_WindowedPopupClass", "PopupHost"))

    def test_a_win32_only_list_never_asks_ui_automation(self):
        dw = sys.modules["dialog_window"]

        def hung(el, cond):
            raise AssertionError("UI Automation was asked while a window hangs")
        live = types.SimpleNamespace(children=hung, uia=types.SimpleNamespace(GetRootElement=lambda: None,
                                                                              CreateTrueCondition=lambda: None))
        with mock.patch.object(dw, "win32_windows", lambda: [(5, "#32770", "Python")]):
            self.assertEqual(dw.top_windows(Ctx(live=live), uia=False), {(5, "#32770", "Python")})

    def test_a_known_window_is_not_read_for_a_tooltip(self):
        dw = sys.modules["dialog_window"]
        asked = []
        live = types.SimpleNamespace(children=lambda el, cond: [], uia=types.SimpleNamespace(
            GetRootElement=lambda: None, CreateTrueCondition=lambda: None))
        with mock.patch.object(dw, "win32_windows", lambda: [(1, "Xaml_WindowedPopupClass", "a"),
                                                              (2, "Xaml_WindowedPopupClass", "b")]), \
                mock.patch.object(dw, "tooltip_only", lambda ctx, w: asked.append(w[0]) or False):
            self.assertEqual(len(dw.top_windows(Ctx(live=live), known={1})), 2)
        self.assertEqual(asked, [2])

    def test_a_window_that_only_holds_a_tooltip_is_no_scene_window(self):
        dw = sys.modules["dialog_window"]
        tip = ("ToolTip", [("Text", [])])
        ctx = self.tooltip_ctx({
            1: ("Window", [tip, ("Text", [])]),                                    # the Start button's tooltip (2026-11-20)
            2: ("Window", [("MenuItem", []), ("Text", [])]),                       # a flyout menu is a real window
            3: ("Window", [tip, ("Pane", [("Pane", [("Button", [])])])]),         # a button two panes down (review)
            4: ("Window", [("Pane", [("Pane", [("Pane", [("Pane", [("Pane", [("Pane", [("Pane", [("Pane", [tip])])])])])])])])]),
            5: ("Window", [("Text", [])])})                                        # no tooltip at all
        self.assertTrue(dw.tooltip_only(ctx, (1, "Xaml_WindowedPopupClass", "PopupHost")))
        self.assertFalse(dw.tooltip_only(ctx, (2, "Xaml_WindowedPopupClass", "PopupHost")))
        self.assertFalse(dw.tooltip_only(ctx, (3, "Xaml_WindowedPopupClass", "PopupHost")))
        self.assertFalse(dw.tooltip_only(ctx, (4, "Xaml_WindowedPopupClass", "PopupHost")))  # past TOOLTIP_DEPTH
        self.assertFalse(dw.tooltip_only(ctx, (5, "Xaml_WindowedPopupClass", "PopupHost")))
        self.assertFalse(dw.tooltip_only(ctx, (1, "#32770", "Inventory sample")))       # not a host class

    def test_a_settling_look_is_bounded(self):
        dw = sys.modules["dialog_window"]
        # Two branches, each within the budget, that together pass it (review, 2026-11-20).
        kid = types.SimpleNamespace(control_type="ListItem", name="App", bounds=(0, 0, 1, 1), el=[])
        branch = types.SimpleNamespace(control_type="Group", name="Apps", bounds=(0, 0, 1, 1), el=[kid])
        el = types.SimpleNamespace(name="Open with", class_name="", el=[branch, branch])
        asked = []

        def children(e, cond, limit):
            asked.append(limit)
            if len(e) > limit:
                raise dw.core.Incomplete("over")
            return e
        live = types.SimpleNamespace(children=children, uia=types.SimpleNamespace(CreateTrueCondition=lambda: None))
        saved = dw.core.MAX_RECORDS
        dw.core.MAX_RECORDS = 2
        try:
            with self.assertRaises(dw.core.Incomplete):
                dw.quiet(Ctx(live=live), el)
        finally:
            dw.core.MAX_RECORDS = saved
        self.assertEqual(asked, [2, 0])                         # the next fetch has nothing left, and finds a child

    def test_a_window_seen_after_the_deadline_is_not_taken(self):
        dw = sys.modules["dialog_window"]
        live = types.SimpleNamespace(wrap=lambda h: ("wrapped", h), uia=types.SimpleNamespace(ElementFromHandle=lambda h: h))

        def slow(ctx, known=(), uia=True):
            time.sleep(0.15)                                  # the look ends past the 0.1 s wait
            return {(12, "#32770", "Late")}
        with mock.patch.object(dw, "top_windows", slow), self.assertRaises(dw.core.Incomplete):
            dw.appeared(Ctx(live=live), set(), wait=0.1)

    def test_no_scan_starts_once_the_wait_has_run_out(self):
        dw = sys.modules["dialog_window"]
        looks = []

        def none_yet(ctx, known=(), uia=True):
            looks.append(time.monotonic())
            return set()
        start = time.monotonic()
        with mock.patch.object(dw, "top_windows", none_yet), self.assertRaises(dw.core.Incomplete):
            dw.appeared(Ctx(live=None), set(), wait=0.3)
        self.assertTrue(all(t < start + 0.3 for t in looks), [t - start for t in looks])
        with mock.patch.object(dw, "top_windows", none_yet), self.assertRaises(dw.core.Incomplete):
            looks.clear()
            dw.appeared(Ctx(live=None), set(), wait=0)
        self.assertEqual(len(looks), 1)                           # wait=0 is the one look

    def test_a_renamed_window_is_not_a_new_one(self):
        dw = sys.modules["dialog_window"]
        before = {(10, "CabinetWClass", "Explorer sample"), (11, "Notepad", "Untitled - Notepad")}
        now = {(10, "CabinetWClass", "Explorer sample"), (11, "Notepad", "*Untitled - Notepad"), (12, "#32770", "Save As")}
        live = types.SimpleNamespace(wrap=lambda h: ("wrapped", h), uia=types.SimpleNamespace(ElementFromHandle=lambda h: h))
        saved = dw.top_windows
        dw.top_windows = lambda ctx, known=(), uia=True: now
        try:
            self.assertEqual(dw.appeared(Ctx(live=live), before, wait=0), ("wrapped", 12))
        finally:
            dw.top_windows = saved

    def test_two_lists_naming_one_window_give_one_entry(self):
        dw = sys.modules["dialog_window"]
        uia_w = types.SimpleNamespace(offscreen=False, class_name="#32770", name="Open (UIA)", hwnd=7,
                                      el=types.SimpleNamespace())        # a live handle read would fail here
        live = types.SimpleNamespace(children=lambda root, cond: [uia_w], uia=types.SimpleNamespace(
            GetRootElement=lambda: None, CreateTrueCondition=lambda: None))
        saved = dw.win32_windows, dw.tooltip_only
        dw.win32_windows, dw.tooltip_only = (lambda: {(7, "#32770", "Open")}), (lambda ctx, w: False)
        try:
            self.assertEqual(dw.top_windows(Ctx(live=live)), {(7, "#32770", "Open")})
        finally:
            dw.win32_windows, dw.tooltip_only = saved

    def test_dismiss_presses_an_enabled_button_only(self):
        dw = sys.modules["dialog_window"]
        pressed = []
        btn = lambda name, enabled: types.SimpleNamespace(name=name, control_type="Button", enabled=enabled)  # noqa: E731
        kids = [btn("Close", False), btn("Yes", True), btn("No", True)]
        live = types.SimpleNamespace(wrap=lambda e: e, uia=types.SimpleNamespace(ElementFromHandle=lambda h: h),
                                     COMError=OSError)
        ctx = Ctx(live=live, invoke=lambda el: pressed.append(el.name))
        saved = dw.descendants
        dw.descendants = lambda c, el, depth: kids
        try:
            self.assertIsNone(dw.dismiss(ctx, 1))
            self.assertEqual(pressed, ["No"])                  # not the disabled title bar Close
            kids[:] = [btn("Close", False), btn("Yes", True)]
            self.assertEqual(dw.dismiss(ctx, 1), ["Yes"])
        finally:
            dw.descendants = saved

    def test_metrics_write_the_view_tree_rows_the_walk_could_not_see(self):
        tree, records = self.view_records()
        self.m._view_tree[:] = [(("Files and folders",), "TreeItem"),
                                (("Files and folders", "Always show icons"), "TreeItem"),
                                (("Files and folders", "Hidden files"), "TreeItem"),
                                (("Files and folders", "Hidden files", "Show hidden files"), "RadioButton"),
                                (("Navigation pane",), "TreeItem"),
                                (("Navigation pane", "Show libraries"), "TreeItem")]
        try:
            out = {r["id"]: r for r in self.m.METRICS(records)}
        finally:
            self.m._view_tree.clear()
        t = tree[len("win11.explorer."):]
        self.assertEqual(sorted(out), sorted([f"{t}.files-and-folders.hidden-files",
                                              f"{t}.files-and-folders.hidden-files.show-hidden-files",
                                              f"{t}.navigation-pane", f"{t}.navigation-pane.show-libraries"]))
        self.assertEqual(out[f"{t}.files-and-folders.hidden-files"]["order"], 1)   # after the walked row
        self.assertEqual(out[f"{t}.navigation-pane"]["order"], 2)                  # after the scrollbar and first group
        self.assertEqual(out[f"{t}.files-and-folders.hidden-files.show-hidden-files"]["kind"], "control")
        self.assertEqual(out[f"{t}.navigation-pane.show-libraries"]["kind"], "setting")
        guest.core.authored(list(out.values()), surface="explorer", build="1", source="uia")

    def test_the_merge_setting_is_put_back_as_it_was(self):
        # Saved once (absent, 0 or 1) before the override, restored exactly (review, 2026-11-21).
        show, hide = self.m.SHOW_MERGE, self.m.HIDE_MERGE
        self.assertLess(show.index("Test-Path -LiteralPath $saved"), show.index("-Value 0"))
        self.assertIn("'absent'", show)
        self.assertIn("$ErrorActionPreference = 'Stop'", show)
        self.assertIn("$ErrorActionPreference = 'Stop'", hide)
        self.assertLess(hide.index("throw"), hide.index("Remove-Item -LiteralPath $saved"))   # deleted only once verified

    @unittest.skipUnless(shutil.which("powershell.exe"), "needs Windows PowerShell for the registry")
    def test_the_merge_setting_round_trips_through_the_registry(self):
        # The real scripts against a throwaway key: absent and 1 come back as they
        # were, and a restore that fails keeps its record for a retry.
        key = "HKCU:\\Software\\ImpossibleOsInventoryTest%d" % os.getpid()
        saved = "$env:TEMP\\inventory-merge-test-%d.txt" % os.getpid()
        show = self.m.SHOW_MERGE.replace(self.m.MERGE_KEY, key).replace(self.m.MERGE_SAVED, saved)
        hide = self.m.HIDE_MERGE.replace(self.m.MERGE_KEY, key).replace(self.m.MERGE_SAVED, saved)

        def ps(script):
            r = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
                               capture_output=True, text=True, timeout=60)
            return r.returncode, r.stdout.strip()
        state = ("$p = Get-ItemProperty -Path '%s'; if ($p.PSObject.Properties.Name -contains 'HideMergeConflicts') "
                 "{ [string]$p.HideMergeConflicts } else { 'absent' }; Test-Path -LiteralPath \"%s\"" % (key, saved))
        ps("New-Item -Path '%s' -Force | Out-Null" % key)
        try:
            self.assertEqual(ps(show)[0], 0)
            self.assertEqual(ps(state)[1].split(), ["0", "True"])
            self.assertEqual(ps(hide)[0], 0)
            self.assertEqual(ps(state)[1].split(), ["absent", "False"])        # absent comes back absent
            ps("Set-ItemProperty -Path '%s' -Name HideMergeConflicts -Value 1 -Type DWord" % key)
            self.assertEqual(ps(show)[0], 0)
            self.assertEqual(ps(show)[0], 0)                                    # a rerun keeps the first saved value
            ps("Set-Content -LiteralPath \"%s\" -Value nonsense" % saved)       # the restore cannot apply this
            self.assertNotEqual(ps(hide)[0], 0)
            self.assertEqual(ps(state)[1].split()[1], "True")                   # its record stays for a retry
            ps("Set-Content -LiteralPath \"%s\" -Value 1" % saved)
            self.assertEqual(ps(hide)[0], 0)
            self.assertEqual(ps(state)[1].split(), ["1", "False"])
            # An empty record (a save cut short before this design) refuses the
            # override and leaves the setting alone; a leftover temp file does not count.
            ps("New-Item -ItemType File -Path \"%s\" -Force | Out-Null" % saved)
            self.assertNotEqual(ps(show)[0], 0)
            self.assertEqual(ps(state)[1].split()[0], "1")
            ps("Remove-Item -LiteralPath \"%s\" -Force; Set-Content -LiteralPath \"%s.tmp\" -Value ''" % (saved, saved))
            self.assertEqual(ps(show)[0], 0)
            self.assertEqual(ps(hide)[0], 0)
            self.assertEqual(ps(state)[1].split(), ["1", "False"])
        finally:
            ps("Remove-Item -Path '%s' -Recurse -Force; Remove-Item -LiteralPath \"%s\", \"%s.tmp\" -Force "
               "-ErrorAction Ignore" % (key, saved, saved))

    def test_a_failed_merge_opener_puts_the_setting_back(self):
        ran = []
        ctx = types.SimpleNamespace(powershell=lambda script: ran.append(script))
        patch = dict(work=lambda ctx, script: None, copy_into=lambda *a: set(),
                     operation_window=lambda ctx, before: types.SimpleNamespace(),
                     descendants=lambda ctx, el, depth: [types.SimpleNamespace(name="Copying")],
                     close_operation=lambda ctx: ran.append("closed"), SETTLE=0)
        with mock.patch.multiple(self.m, **patch), self.assertRaises(self.m.core.Incomplete):
            self.m.open_folder_merge(ctx)
        self.assertEqual(ran, ["closed", self.m.HIDE_MERGE])

    def test_a_merge_setup_that_fails_after_the_override_puts_it_back(self):
        ran = []
        ctx = types.SimpleNamespace(powershell=lambda script: ran.append(script))

        def work(ctx, script):
            raise self.m.core.Incomplete("Set-Content: the fixture folder is read-only")
        patch = dict(work=work, close_operation=lambda ctx: ran.append("closed"))
        with mock.patch.multiple(self.m, **patch), self.assertRaises(self.m.core.Incomplete):
            self.m.open_folder_merge(ctx)
        self.assertEqual(ran, ["closed", self.m.HIDE_MERGE])
        self.assertIsNone(self.m._root["el"])

    def test_the_folder_merge_message_is_recorded_without_its_time(self):
        merge = {"id": "win11.explorer.folder-merge", "kind": "dialog", "label": "x", "order": 0}
        self.assertEqual(self.m.merge_message([]), [])                         # no merge scene, no message
        with self.assertRaises(ValueError):                                     # the scene ran, its message was not read
            self.m.merge_message([merge])
        # One element, three paragraphs, the time wrapped in direction marks (the 2026-11-20 capture).
        self.m._merge_text[:] = ["This destination already contains a folder named 'General items'.\r\n\r\n"
                                 "If any files have the same names, you will be asked if you want to replace those files."
                                 "\r\n\r\nDo you still want to merge the folder created on "
                                 "\u200e11/\u200e20/\u200e2026 \u200f\u200e11:00 PM with this one?"]
        try:
            rec = self.m.merge_message([merge])[0]
        finally:
            self.m._merge_text.clear()
        self.assertEqual(rec["parent"], "folder-merge")
        self.assertEqual(rec["behaviour"], "This destination already contains a folder named 'General items'. If any "
                         "files have the same names, you will be asked if you want to replace those files. Do you still "
                         "want to merge the folder created on (the folder's creation time) with this one?")
        self.m._merge_text[:] = ["This destination already contains a folder named 'General items'. If any files "
                                 "have the same names. Do you still want to merge the folder created on 2026-11-20?"]
        try:
            with self.assertRaises(ValueError):                                 # a time the pattern did not know
                self.m.merge_message([merge])
        finally:
            self.m._merge_text.clear()
        guest.core.authored([rec], surface="explorer", build="1", source="uia")

    def test_metrics_refuse_a_walked_row_away_from_its_computed_id(self):
        _, records = self.view_records()
        self.m._view_tree[:] = [(("Files and folders",), "TreeItem"), (("Files and folders", "Other"), "TreeItem")]
        try:
            with self.assertRaises(ValueError):
                self.m.METRICS(records)
        finally:
            self.m._view_tree.clear()
        with self.assertRaises(ValueError):           # a tree that was never read
            self.m.METRICS(records)

    class El:
        def __init__(self, name, cls, bounds, kids=(), control_type=None):
            self.name, self.class_name, self.bounds, self._kids = name, cls, bounds, list(kids)
            self.control_type = control_type or cls

        def children(self, limit=None):
            return self._kids

    def test_a_sheet_splits_into_its_page_and_its_frame_at_the_tab_control(self):
        El = self.El
        tab = El("", "SysTabControl32", (10, 30, 300, 400))
        sheet = El("Notes Properties", "#32770", (0, 0, 320, 480), [
            tab, El("General", "TabItem", (12, 32, 60, 20)), El("Read-only", "Button", (20, 300, 80, 20)),
            El("OK", "Button", (100, 440, 70, 24)), El("", "TitleBar", (0, 0, 320, 30))])
        page = [k.name for k in self.m.View(sheet, page=True).children()]
        frame = [k.name for k in self.m.View(sheet, page=False).children()]
        self.assertEqual(page, ["Read-only"])
        self.assertEqual(frame, ["", "General", "OK", ""])
        self.assertEqual(self.m.View(sheet, page=True).bounds, tab.bounds)

    def test_a_sheet_without_one_tab_control_is_refused(self):
        with self.assertRaises(guest.core.Incomplete):
            self.m.View(self.El("x", "#32770", (0, 0, 1, 1)), page=True)

    def test_every_tab_of_every_sheet_is_a_counted_scene(self):
        scenes = {s["slug"]: s for s in self.m.SCENES}
        self.assertEqual(len(scenes), len(self.m.SCENES))
        tabs = [(slug, tab) for slug, _, _, _, tabs in self.m.SHEETS for tab in tabs]
        tabs += [("folder-options", tab) for tab in self.m.FOLDER_OPTIONS_TABS]
        for slug, tab in tabs:
            base = slug if slug == "folder-options" else f"{slug}-properties"
            scene = scenes[f"{base}-{guest.core.slug(tab)}"]
            self.assertEqual((scene["kind"], scene.get("count")), ("dialogTab", True), scene["slug"])
            # The schema requires a tab's parent: its own tab in the frame scene's strip.
            self.assertEqual(scene["parent"], f"{base}.systabcontrol32.{guest.core.slug(tab)}", scene["slug"])
        view = scenes["folder-options-view"]
        self.assertIn(({"controlType": "TreeItem"}, "setting"), view["kinds"])

    def test_the_hold_is_proven_and_the_cleanup_spares_itself(self):
        script = self.m.hold_open("C:\\x\\Held file.txt")
        self.assertIn("'-File','" + self.m.HOLDER + "'", script)
        self.assertIn("'the file is not held'; exit 1", script)
        self.assertTrue(script.rstrip().endswith("exit 0"), script[-80:])
        self.assertIn("$_.ProcessId -ne $PID", self.m.REMOVE_WORK)
        self.assertIn("inventory-hold", self.m.HOLDER)

    def test_hand_records_are_authored_cleanly(self):
        out = guest.core.authored(self.m.HAND, surface="explorer", build="1", source="hand")
        self.assertEqual({r["parent"] for r in out} - {"win11.explorer.frame"}, {"win11.explorer.dialogs"})


class DialogsCommon(unittest.TestCase):
    """The common dialogs module: its scenes and the records it computes."""

    def setUp(self):
        self.m = guest.load_surface("dialogs-common")

    def test_the_seven_button_sets_each_have_a_scene(self):
        slugs = [s["slug"] for s in self.m.SCENES]
        self.assertEqual(len(slugs), len(set(slugs)))
        sets = [slug for slug in slugs if slug.startswith("message-box-") and "-icon-" not in slug
                and slug not in ("message-box-default-second", "message-box-long-text")]
        self.assertEqual(sets, [f"message-box-{s}" for s in ("ok", "ok-cancel", "yes-no", "yes-no-cancel",
                                                              "retry-cancel", "abort-retry-ignore",
                                                              "cancel-try-continue")])
        self.assertTrue(all(s["kind"] == "dialog" for s in self.m.SCENES))

    def metrics_input(self):
        box = lambda w: {"bounds": {"x": 0, "y": 0, "width": w, "height": 150}}  # noqa: E731
        return [dict(id="win11.dialogs.message-box-ok", **box(240)),
                dict(id="win11.dialogs.message-box-long-text", **box(548))] + \
            [{"id": f"win11.dialogs.{i}"} for i in self.m.REQUIRED]

    def test_metrics_refuse_a_scene_published_without_its_controls(self):
        # Readiness saw the controls; the published records must hold them too (review, 2026-11-21).
        records = [r for r in self.metrics_input() if r["id"] != "win11.dialogs.save-as-encoding.encoding-combobox"]
        with self.assertRaisesRegex(ValueError, "encoding-combobox"):
            self.m.METRICS(records)

    def test_metrics_give_the_long_text_width_and_each_icon_sound(self):
        self.m._m["sounds"] = {"SystemHand": "Windows Foreground.wav", "SystemQuestion": None,
                               "SystemExclamation": "Windows Background.wav", "SystemAsterisk": "Windows Background.wav"}
        self.m._m["crash"] = {"windows": [], "running": 0, "exit": 0xC0000602, "HKLM": None, "HKCU": 0}
        self.m._m["print"] = {"className": "#32770", "title": "Print", "controls": list(self.m.CLASSIC_PRINT_CONTROLS)}
        try:
            out = {r["id"]: r for r in self.m.METRICS(self.metrics_input())}
        finally:
            self.m._m.clear()
        self.assertEqual(out["message-box-long-text.width"]["measure"]["value"], 548)
        self.assertIn("240 px", out["message-box-long-text.width"]["behaviour"])
        self.assertEqual(out["message-box-icon-error.sound"]["behaviour"],
                         "Plays Windows Foreground.wav, the SystemHand event of the current scheme.")
        self.assertIn("Plays nothing", out["message-box-icon-question.sound"]["behaviour"])
        self.assertIn("DontShowUI is not set for the machine and 0 for the user", out["crash-report"]["behaviour"])
        self.assertIn("opens the classic Print dialog", out["print-modern"]["behaviour"])
        guest.core.authored(list(out.values()), surface="dialogs", build="1", source="uia:1")

    def test_metrics_refuse_a_missing_box_or_scheme(self):
        with self.assertRaises(ValueError):
            self.m.METRICS(self.metrics_input()[:1])
        with self.assertRaises(ValueError):
            self.m.METRICS(self.metrics_input())

    def test_metrics_refuse_a_crash_that_showed_a_window_or_stayed(self):
        self.m._m["sounds"] = {e: None for e in self.m.ICON_EVENTS.values()}
        try:
            for crash in ({"windows": [("#32770", "x")], "running": 0, "HKLM": None, "HKCU": None},
                          {"windows": [], "running": 1, "HKLM": None, "HKCU": None},
                          # a host that died of a Python error before it raised (review, 2026-11-21)
                          {"windows": [], "running": 0, "exit": 1, "HKLM": None, "HKCU": None}):
                self.m._m["crash"] = crash
                with self.assertRaises(ValueError):
                    self.m.METRICS(self.metrics_input())
        finally:
            self.m._m.clear()

    def test_metrics_refuse_a_print_window_that_is_not_the_classic_dialog(self):
        # A modern Print window must be walked as a scene, not summarised (review, 2026-11-20).
        self.m._m["sounds"] = {e: None for e in self.m.ICON_EVENTS.values()}
        self.m._m["crash"] = {"windows": [], "running": 0, "exit": 0xC0000602, "HKLM": None, "HKCU": None}
        try:
            classic = [list(c) for c in self.m.CLASSIC_PRINT_CONTROLS]
            for shown in (None, {"className": "Microsoft.UI.Content.DesktopChildSiteBridge", "title": "Print", "controls": []},
                          {"className": "#32770", "title": "Notepad", "controls": [["Button", "OK"]]},   # an error box
                          {"className": "#32770", "title": "Print", "controls": classic[:2]}):
                self.m._m.pop("print", None)
                if shown:
                    self.m._m["print"] = shown
                with self.assertRaises(ValueError):
                    self.m.METRICS(self.metrics_input())
        finally:
            self.m._m.clear()
        self.assertNotIn("print-modern", [s["slug"] for s in self.m.SCENES])

    def test_the_credential_prompt_waits_for_its_fields(self):
        kids = [types.SimpleNamespace(automation_id=a, offscreen=False) for a in ("CloseButton", "CancelButton")]
        saved_d, saved_w = self.m.descendants, self.m.DIALOG_WAIT
        seen = []

        def look(ctx, el, depth, stop=None):
            for k in kids:
                seen.append(k)
                if stop is not None and stop(k):
                    break
            return kids
        self.m.descendants, self.m.DIALOG_WAIT = look, 0
        try:
            with self.assertRaises(self.m.core.Incomplete):           # the partial frame of 2026-11-20
                self.m.credential_ready(Ctx(), None)
            hidden = [types.SimpleNamespace(automation_id=a, offscreen=True) for a in self.m.CREDENTIAL_FIELDS]
            kids.extend(hidden)
            with self.assertRaises(self.m.core.Incomplete):           # present but offscreen: the walk keeps none
                self.m.credential_ready(Ctx(), None)
            for k in hidden:
                k.offscreen = False
            kids.append(types.SimpleNamespace(automation_id="Unrelated", offscreen=False))
            seen.clear()
            self.m.credential_ready(Ctx(), None)
            self.assertNotIn("Unrelated", [k.automation_id for k in seen])   # the look stopped at the last field
        finally:
            self.m.descendants, self.m.DIALOG_WAIT = saved_d, saved_w

    def test_the_encoding_slot_is_waited_for(self):
        kids = [types.SimpleNamespace(control_type="Edit", name="File name:", offscreen=False)]
        saved_d, saved_w = self.m.descendants, self.m.DIALOG_WAIT
        self.m.descendants, self.m.DIALOG_WAIT = (lambda ctx, el, depth, stop=None: kids), 0
        try:
            with self.assertRaises(self.m.core.Incomplete):           # an uncustomised dialog
                self.m.encoding_ready(Ctx(), None)
            combo = types.SimpleNamespace(control_type="ComboBox", name="Encoding:", offscreen=True)
            kids.append(combo)
            with self.assertRaises(self.m.core.Incomplete):           # present but offscreen
                self.m.encoding_ready(Ctx(), None)
            combo.offscreen = False
            self.m.encoding_ready(Ctx(), None)
        finally:
            self.m.descendants, self.m.DIALOG_WAIT = saved_d, saved_w
        self.assertIn("save-as-encoding", [sc["slug"] for sc in self.m.SCENES])

    def test_hosts_are_ended_when_a_window_will_not_close(self):
        ran = []
        ctx = types.SimpleNamespace(powershell=lambda script: ran.append(script))

        def stuck(ctx):
            raise self.m.core.Incomplete("a window refused to close")
        with mock.patch.object(self.m, "close_new_windows", stuck):
            self.m._hosts[:] = [types.SimpleNamespace(poll=lambda: None)]
            with self.assertRaisesRegex(self.m.core.Incomplete, "refused"):
                self.m.close_scene(ctx)
        self.assertEqual(ran, [self.m.END_HOSTS])
        self.assertEqual(self.m._hosts, [])

    def test_the_hung_fixture_is_ended_before_the_window_scan(self):
        order = []

        class Host:
            code = None

            def poll(self):
                return self.code

            def kill(self):
                order.append("kill")
                self.code = -9
        ctx = types.SimpleNamespace(powershell=lambda script: order.append("END_HOSTS"))
        with mock.patch.object(self.m, "close_new_windows", lambda ctx: order.append("scan")):
            self.m._hosts[:] = [Host()]
            self.m.close_hung(ctx)
        self.assertEqual(order, ["kill", "scan"])
        self.assertEqual(self.m._hosts, [])
        by = {s["slug"]: s for s in self.m.SCENES}
        self.assertIs(by["not-responding"]["close"], self.m.close_hung)

    def test_a_host_that_exited_needs_no_process_query(self):
        ran = []
        ctx = types.SimpleNamespace(powershell=lambda script: ran.append(script))
        saved = self.m.close_new_windows
        self.m.close_new_windows = lambda ctx: None
        try:
            self.m._hosts[:] = [types.SimpleNamespace(poll=lambda: 0)]
            self.m.close_scene(ctx)
            self.assertEqual(ran, [])
            self.m._hosts[:] = []                                   # a scene that started no host through host()
            self.m.close_scene(ctx)
            self.assertEqual(ran, [self.m.END_HOSTS])
        finally:
            self.m.close_new_windows = saved
            self.m._hosts.clear()

    def test_the_about_frame_refuses_an_owner_it_would_publish(self):
        self.assertIn("exit 1", self.m.CHECK_OWNER)
        self.assertIn('["ref", "", "Windows User", "user"]', self.m.CHECK_OWNER)
        self.assertNotIn("Write-Output $v", self.m.CHECK_OWNER)


class ExplorerFrame(unittest.TestCase):
    """The File Explorer frame module's records from what its measured scenes saw."""

    def setUp(self):
        self.m = guest.load_surface("explorer-frame")

    @staticmethod
    def keys():
        edit = {"name": "Address Bar", "controlType": "Edit"}
        regions = ["item list", "navigation pane", "tab row", "item list"]
        return {"alt+d": {"region": "address bar", "focus": edit, "list": False},
                "ctrl+l": {"region": "address bar", "focus": edit, "list": False},
                "ctrl+e": {"region": "search box", "focus": {"controlType": "Edit"}, "list": False},
                "f4": {"region": "address bar", "focus": edit, "list": True},
                "alt+p": {"opens": "preview", "then": None}, "alt+shift+p": {"opens": "details", "then": None},
                "ctrl+t": {"tabs": [1, 2], "new": "Home"}, "ctrl+w": {"tabs": [2, 1]},
                "swap": {"details-then-alt+p": "preview"},
                "ctrl+n": {"windows": [1, 2]},
                "alt+left": {"from": "General items", "to": "Explorer sample", "start": "Explorer sample"},
                "alt+right": {"from": "Explorer sample", "to": "General items", "inside": "General items"},
                "alt+up": {"from": "General items", "to": "Explorer sample", "start": "Explorer sample"},
                "f6": {"stops": regions}}

    def test_live_values_name_no_id(self):
        def el(name):
            return types.SimpleNamespace(control_type="Text", automation_id="", class_name="TextBlock", name=name)
        for name, seg in (("Explorer sample (13 items)", "folder-summary"), ("\u200e11/\u200e19/\u200e2026 7:23 AM", "modified-date")):
            self.assertEqual(core._segment(el(name), self.m.IDENTITY, self.m.VALUES), seg)
            self.assertTrue(any(core.matches(el(name), v) for v in self.m.VALUES))

    def test_a_shown_date_is_a_live_value_and_its_label_is_not(self):
        el = lambda name: types.SimpleNamespace(control_type="Text", automation_id="", class_name="TextBlock", name=name)
        date = el("\u200e11/\u200e19/\u200e2026 \u200f\u200e7:45 AM")
        self.assertEqual(core._segment(date, values=self.m.VALUES), "textblock")      # not 11-19-2026-7-45-am
        self.assertEqual(core._segment(el("Date modified"), values=self.m.VALUES), "date-modified")

    def test_a_chord_is_named_the_way_the_records_write_keys(self):
        self.assertEqual([self.m.chord_name(c) for c in ("alt+shift+p", "f4", "ctrl+t")], ["Alt+Shift+P", "F4", "Ctrl+T"])

    def test_the_keyboard_map_is_written_from_the_measurement(self):
        got = {r["shortcut"]: r for r in self.m.key_records(self.keys())}
        self.assertEqual(len(got), 13)
        self.assertIn("on Home", got["Ctrl+T"]["behaviour"])
        self.assertTrue(got["F6"]["behaviour"].endswith("item list, navigation pane, tab row."), got["F6"]["behaviour"])

    def test_a_key_that_did_not_do_its_job_stops_the_capture(self):
        # The control: the same measurement with Alt+D leaving the focus in the list.
        k = self.keys()
        k["alt+d"]["region"] = "item list"
        with self.assertRaises(ValueError):
            self.m.key_records(k)
        k = self.keys()
        k["f6"]["stops"] = ["item list", "tab row", "navigation pane"]        # never came round
        with self.assertRaises(ValueError):
            self.m.key_records(k)

    def test_each_opened_known_folder_matches_both_baselines(self):
        known = [{"id": "{A}", "name": "Downloads"}, {"id": "{b}", "name": "Screenshots"}]     # the registry's own case
        ns = [{"name": "Downloads", "opens": True, "title": "Downloads", "path": "C:\\Users\\ref\\Downloads"},
              {"name": "Screenshots", "opens": False}]
        got = {r["id"]: r for r in self.m.folder_records(ns, known)}
        page = got["frame.known-folders.downloads"]
        self.assertEqual(page["baselineId"], "known-folders:{a}")
        self.assertEqual(page["behaviour"], "Opens %USERPROFILE%\\Downloads.")
        self.assertEqual(got["frame.known-folders.downloads.shell-name"]["baselineId"], "shell-commands:folder:Downloads")
        self.assertEqual(len(got), 3)                     # the group, the page and its name; Screenshots has none
        with self.assertRaises(ValueError):
            self.m.folder_records([], known)

    def test_a_tab_drag_that_did_not_tear_out_stops_the_capture(self):
        good = {"windows": [1, 2], "left": ["Explorer sample"]}
        self.assertEqual(len(self.m.tab_drag_records(good)), 1)
        for bad in ({**good, "windows": [1, 1]}, {**good, "left": ["Explorer sample", "Home"]}):
            with self.assertRaises(ValueError):
                self.m.tab_drag_records(bad)

    def test_hand_records_and_measured_records_do_not_share_an_order(self):
        orders = [r["order"] for r in self.m.key_records(self.keys())] + [h["order"] for h in self.m.HAND]
        orders += [r["order"] for r in self.m.tab_drag_records({"windows": [1, 2], "left": ["a"]})]
        self.assertEqual(len(orders), len(set(orders)))


class GuestImports(unittest.TestCase):
    def test_a_surface_module_imports_the_core_the_tool_loaded(self):
        # The guest's embeddable Python leaves the script folder off sys.path; so
        # does this test, so only the tool's own registration can satisfy the import.
        inv = str(INV)
        saved_path, saved_core = list(sys.path), sys.modules.pop("capture_core", None)
        sys.path[:] = [p for p in sys.path if os.path.abspath(p or ".") != inv]
        try:
            ui = load("capture_ui_isolated", INV / "capture-ui.py")
            with tempfile.TemporaryDirectory() as tmp:
                mod = pathlib.Path(tmp) / "m.py"
                mod.write_text("import capture_core as core\nSEEN = core\n")
                self.assertIs(load("surface_m", mod).SEEN, ui.core)
        finally:
            sys.path[:] = saved_path
            if saved_core is not None:
                sys.modules["capture_core"] = saved_core


class GuestOutput(unittest.TestCase):
    def test_a_name_the_console_code_page_lacks_still_prints(self):
        code = (f"import importlib.util,sys\n"
                f"spec=importlib.util.spec_from_file_location('g', {str(INV / 'capture-ui.py')!r})\n"
                "g=importlib.util.module_from_spec(spec); spec.loader.exec_module(g)\n"
                "g.utf8_output(); print('\\u200eNovember\\u200e 2026')\n")
        env = dict(os.environ, PYTHONIOENCODING="cp1252", PYTHONPATH=str(INV))
        r = subprocess.run([sys.executable, "-c", code], capture_output=True, env=env, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr.decode(errors="replace"))
        self.assertEqual(r.stdout.decode("utf-8").strip(), "\u200eNovember\u200e 2026")


class DesktopPixels(unittest.TestCase):
    """The desktop's pixel reads: an icon's artwork and label are the two runs of
    rows that change when the icons are hidden."""

    def setUp(self):
        self.desktop = load("surface_desktop", INV / "surfaces" / "desktop.py")

    def test_artwork_and_label_are_told_apart_by_the_gap_between_them(self):
        bg, ink = (10, 60, 20), (240, 240, 240)
        a = [[bg] * 20 for _ in range(12)]
        b = copy.deepcopy(a)
        for r in range(1, 5):                   # artwork: rows 1-4, columns 6-13
            for c in range(6, 14):
                a[r][c] = ink
        for r in (7, 8):                        # label: rows 7-8, columns 2-17
            for c in range(2, 18):
                a[r][c] = ink
        parts = self.desktop.blocks(self.desktop.changed(a, b, "test"))
        self.assertEqual(len(parts), 2)
        self.assertEqual(self.desktop.extent(parts[0]), (8, 4))
        self.assertEqual(self.desktop.extent(parts[1]), (16, 2))

    def test_a_small_colour_difference_is_not_a_change(self):
        a = [[(10, 10, 10)] * 4]
        with self.assertRaises(self.desktop.core.Incomplete):
            self.desktop.changed(a, [[(30, 30, 30)] * 4], "noise")


class Find(unittest.TestCase):
    """Context.find reads no level it will not search and keeps its deadline on
    a tree that is slow to read (performance review 2026-11-17)."""

    class El:
        def __init__(self, name, kids=(), log=None):
            self.name, self.automation_id, self.class_name, self.control_type = name, "", "", "Pane"
            self.kids, self.log = list(kids), log

        def children(self):
            self.log.append(self.name)
            return self.kids

    def context(self):
        uia = types.SimpleNamespace(GetRootElement=lambda: "root")
        return guest.Context(types.SimpleNamespace(uia=uia, COMError=OSError))

    def test_the_last_level_is_not_read(self):
        log = []
        leaf = self.El("leaf", log=log)
        root = self.El("root", [self.El("mid", [leaf], log)], log)
        with self.assertRaises(guest.core.Incomplete):
            self.context().find(within=root, timeout=0, depth=2, name="nothing")
        self.assertEqual(log, ["root"])          # mid is the last level searched: matched, never opened

    def test_a_slow_first_pass_stops_at_the_deadline(self):
        # Reading children is what costs time on a broad UIA tree, so the clock
        # moves inside the first pass and the deadline must hold there too
        # (round and performance reviews 2026-11-17: 30 one-second reads ran 31 s
        # under timeout=1).
        log, now = [], [0.0]

        class Slow(self.El):
            def children(self):
                now[0] += 1.0
                return super().children()
        root = Slow("root", [Slow(f"k{i}", [Slow("x", log=log)], log) for i in range(30)], log)
        with mock.patch.object(guest.time, "monotonic", lambda: now[0]), \
                mock.patch.object(guest.time, "sleep", lambda s: None):
            with self.assertRaises(guest.core.Incomplete):
                self.context().find(within=root, timeout=1, depth=3, name="nothing")
        self.assertLessEqual(len(log), 2)          # root and one child read, then the deadline

    def test_a_zero_timeout_still_searches_the_whole_tree_once(self):
        log = []
        target = self.El("target", log=log)
        root = self.El("root", [self.El("a", [self.El("b", [target], log)], log)], log)
        found = self.context().find(within=root, timeout=0, depth=4, name="target")
        self.assertIs(found, target)


class FindWindow(unittest.TestCase):
    def test_a_zero_timeout_looks_once(self):
        win = types.SimpleNamespace(name="", automation_id="", class_name="Popup", control_type="Window", offscreen=False)
        uia = types.SimpleNamespace(GetRootElement=lambda: "root", CreateTrueCondition=lambda: "all")
        live = types.SimpleNamespace(uia=uia, children=lambda el, cond=None: [win])
        self.assertIs(guest.Context(live).find_window(timeout=0, className="Popup"), win)
        with self.assertRaises(guest.core.Incomplete):
            guest.Context(live).find_window(timeout=0, className="Other")

    def test_a_missing_window_names_what_is_on_screen(self):
        # A window that went missing mid-capture is explained by what took its
        # place; offscreen windows are not named.
        shown = types.SimpleNamespace(name="Taskbar", automation_id="", class_name="Shell_TrayWnd", control_type="Pane",
                                      offscreen=False)
        hidden = types.SimpleNamespace(name="Gone", automation_id="", class_name="Hidden", control_type="Pane",
                                       offscreen=True)
        uia = types.SimpleNamespace(GetRootElement=lambda: "root", CreateTrueCondition=lambda: "all")
        live = types.SimpleNamespace(uia=uia, children=lambda el, cond=None: [shown, hidden], COMError=OSError)
        with self.assertRaisesRegex(guest.core.Incomplete, r"on screen: Shell_TrayWnd:'Taskbar'$"):
            guest.Context(live).find_window(timeout=0, className="Search")

    def test_a_light_dismissed_flyout_is_never_brought_forward(self):
        raised = []
        fake = types.SimpleNamespace(SetForegroundWindow=raised.append)
        el = types.SimpleNamespace(CurrentNativeWindowHandle=7)
        uia = types.SimpleNamespace(GetRootElement=lambda: "root", CreateTrueCondition=lambda: "all")
        real = guest.user32
        guest.user32 = fake
        try:
            for cls in ("Shell_InputSwitchTopLevelWindow", "Notepad"):
                win = types.SimpleNamespace(name="", automation_id="", class_name=cls, control_type="Window",
                                            offscreen=False, el=el)
                live = types.SimpleNamespace(uia=uia, children=lambda e, cond=None, w=win: [w])
                guest.Context(live).find_window(timeout=0, className=cls)
        finally:
            guest.user32 = real
        self.assertEqual(raised, [7])          # Notepad only: the switcher would close


class HostShots(unittest.TestCase):
    """Host shots are measured from their pixels: read_png and ink_box."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_a_png_reads_back_the_pixels_it_was_written_with(self):
        w, h = 7, 5
        rgb = bytes((x * 30 + y) % 256 for y in range(h) for x in range(w) for _ in range(3))
        core.write_png(self.tmp / "a.png", w, h, rgb)
        self.assertEqual(core.read_png(self.tmp / "a.png"), (w, h, rgb))

    def test_every_png_row_filter_decodes(self):
        # write_png uses filter 0 only; Windows' encoder uses the others, so each
        # is written by hand here (rows of a 2x1 RGB image, one filter per row).
        w = 2
        rows = [bytes([10, 20, 30, 40, 50, 60]), bytes([11, 21, 31, 41, 51, 61]),
                bytes([12, 22, 32, 42, 52, 62]), bytes([13, 23, 33, 43, 53, 63]), bytes([14, 24, 34, 44, 54, 64])]

        def enc(f, cur, prev):
            out = bytearray()
            for i, v in enumerate(cur):
                a = cur[i - 3] if i >= 3 else 0
                b = prev[i]
                c = prev[i - 3] if i >= 3 else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = [0, a, b, (a + b) // 2, a if pa <= pb and pa <= pc else b if pb <= pc else c][f]
                out.append((v - pred) & 255)
            return bytes([f]) + bytes(out)
        raw, prev = b"", bytes(6)
        for f, row in enumerate(rows):
            raw += enc(f, row, prev)
            prev = row
        import struct
        import zlib

        def chunk(tag, data):
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        (self.tmp / "f.png").write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, 5, 8, 2, 0, 0, 0))
                                         + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
        self.assertEqual(core.read_png(self.tmp / "f.png"), (w, 5, b"".join(rows)))

    def test_ink_box_bounds_only_the_ink_inside_the_region(self):
        w, h = 10, 8
        px = bytearray(w * h * 3)
        for x, y in ((2, 3), (5, 6), (9, 0)):          # (9, 0) lies outside the region
            px[(y * w + x) * 3:(y * w + x) * 3 + 3] = b"\xff\xff\xff"
        img = (w, h, bytes(px))
        white = lambda p: p == (255, 255, 255)          # noqa: E731
        self.assertEqual(core.ink_box(img, (0, 2, 8, 6), white), (2, 3, 4, 4))
        self.assertIsNone(core.ink_box(img, (0, 0, 2, 2), white))

    def test_ink_boxes_splits_pieces_further_apart_than_the_gap(self):
        w, h = 40, 10
        px = bytearray(w * h * 3)
        for x, y in ((1, 1), (3, 2), (30, 5), (31, 8)):
            px[(y * w + x) * 3:(y * w + x) * 3 + 3] = b"\xff\xff\xff"
        img = (w, h, bytes(px))
        white = lambda p: p == (255, 255, 255)          # noqa: E731
        self.assertEqual(core.ink_boxes(img, (0, 0, w, h), white, gap=4), [(1, 1, 3, 2), (30, 5, 2, 4)])
        self.assertEqual(core.ink_boxes(img, (0, 0, w, h), white, gap=40), [(1, 1, 31, 8)])

    def test_ink_boxes_folds_a_piece_lying_inside_another(self):
        # A ring with a dot in its middle further than the gap from the ring.
        w = h = 30
        px = bytearray(w * h * 3)
        for i in range(2, 28):
            for x, y in ((i, 2), (i, 27), (2, i), (27, i)):
                px[(y * w + x) * 3:(y * w + x) * 3 + 3] = b"\xff\xff\xff"
        px[(15 * w + 15) * 3:(15 * w + 15) * 3 + 3] = b"\xff\xff\xff"
        white = lambda p: p == (255, 255, 255)          # noqa: E731
        self.assertEqual(core.ink_boxes((w, h, bytes(px)), (0, 0, w, h), white, gap=4), [(2, 2, 26, 26)])


class HostCapture(unittest.TestCase):
    """host-capture.py refuses what it cannot publish exactly."""

    @classmethod
    def setUpClass(cls):
        cls.hc = load("host_capture", INV / "host-capture.py")

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def index(self, shots, failed=None):
        (self.tmp / "shots.json").write_text(json.dumps({"checkpoint": "clean", "thumbnail": self.hc.THUMBNAIL, "shots": shots, "failed": failed}),
                                             encoding="utf-8")

    def shot(self, name, exact=True):
        return {"name": name, "file": f"{name}.png", "width": 1920, "height": 1080, "exact": exact}

    def test_every_wanted_shot_taken_exactly_is_returned(self):
        self.index([self.shot("a"), self.shot("b"), self.shot("extra")])
        self.assertEqual(sorted(self.hc.taken(self.tmp, 0, ["a", "b"])), ["a", "b"])

    def test_refuses_a_failed_step_a_missing_shot_and_a_mode_change(self):
        cases = [
            ([self.shot("a")], "step 3 (click): boom", 1, ["a"], "stopped"),
            ([self.shot("a")], None, 0, ["a", "b"], "no shot of b"),
            ([self.shot("a", exact=False)], None, 0, ["a"], "video mode changed"),
            ([self.shot("a")], None, 1, ["a"], "exit 1"),
        ]
        for shots, failed, rc, wanted, why in cases:
            self.index(shots, failed)
            with self.assertRaisesRegex(self.hc.core.Incomplete, why):
                self.hc.taken(self.tmp, rc, wanted)

    def test_a_mode_change_is_kept_only_inside_a_burst_across_a_restart(self):
        self.index([self.shot("boot-3", exact=False), self.shot("menu", exact=False)])
        self.assertFalse(self.hc.taken(self.tmp, 0, ["boot-3"], ("boot-",))["boot-3"]["exact"])
        with self.assertRaisesRegex(self.hc.core.Incomplete, "video mode changed during menu"):
            self.hc.taken(self.tmp, 0, ["boot-3", "menu"], ("boot-",))
        # A still shot that only shares the burst's prefix must hold its mode.
        self.index([self.shot("boot-manager", exact=False)])
        with self.assertRaisesRegex(self.hc.core.Incomplete, "video mode changed during boot-manager"):
            self.hc.taken(self.tmp, 0, ["boot-manager"], ("boot-",))

    def test_a_burst_that_ended_early_keeps_only_the_frames_it_took(self):
        # The boot walk's bursts end at the boot menu (review, 2026-11-18).
        wanted = ["boot-1", "boot-2", "boot-3", "boot-4", "menu"]
        self.index([self.shot("boot-1"), self.shot("boot-2"), self.shot("menu")])
        self.assertEqual(sorted(self.hc.taken(self.tmp, 0, wanted, ("boot-",), ("boot-",))), ["boot-1", "boot-2", "menu"])
        with self.assertRaisesRegex(self.hc.core.Incomplete, "no shot of boot-3, boot-4"):
            self.hc.taken(self.tmp, 0, wanted, ("boot-",))          # not an early burst
        self.index([self.shot("boot-2"), self.shot("menu")])
        with self.assertRaisesRegex(self.hc.core.Incomplete, "no shot of boot-1"):
            self.hc.taken(self.tmp, 0, wanted, ("boot-",), ("boot-",))

    def test_shots_read_by_an_older_thumbnail_revision_are_refused(self):
        # Revision 1 put Hyper-V's 4 leading bytes into the first two pixels;
        # a --from replay of such a run must not republish (2026-11-18).
        (self.tmp / "shots.json").write_text(json.dumps({"checkpoint": "clean", "shots": [self.shot("a")]}), encoding="utf-8")
        with self.assertRaisesRegex(self.hc.core.Incomplete, "thumbnail revision 1, not 2"):
            self.hc.taken(self.tmp, 0, ["a"])

    def test_a_failed_proof_publishes_nothing(self):
        core = self.hc.core
        self.index([{**self.shot("desktop"), "width": 2, "height": 1}])
        core.write_png(self.tmp / "desktop.png", 2, 1, bytes([200, 100, 50, 0, 0, 0]))
        core.write_png(self.tmp / "guest.png", 2, 1, bytes([200, 100, 50, 255, 255, 255]))

        class Sc:
            SHOTS, PROOF = ["desktop"], ("desktop", "guest.png")

            def records(*_):
                raise AssertionError("measured after a failed proof")
        (self.tmp / "build.json").write_text(json.dumps({"build": "1"}), encoding="utf-8")
        said = io.StringIO()
        with mock.patch.object(self.hc.capture, "publish") as wrote, mock.patch.object(self.hc, "build", lambda: "1"), \
                contextlib.redirect_stdout(said):
            self.assertEqual(self.hc.publish(Sc, self.tmp, 0, False), 3)
        wrote.assert_not_called()
        self.assertIn("not the guest's screen 1:1", said.getvalue())

    def test_a_burst_that_ended_early_is_published_with_the_frames_it_took(self):
        # publish() indexed every scenario name and raised KeyError on the
        # frames an early burst skipped (review, 2026-11-18).
        core = self.hc.core
        self.index([self.shot("boot-1"), self.shot("menu")])
        for n in ("boot-1", "menu"):
            core.write_png(self.tmp / f"{n}.png", 1, 1, bytes(3))
        (self.tmp / "build.json").write_text(json.dumps({"build": "1"}), encoding="utf-8")
        got = {}

        class Sc:
            SHOTS, MOVING, EARLY, SURFACE, FILE = ["boot-1", "boot-2", "boot-3", "menu"], ("boot-",), ("boot-",), "session", "x"

            def records(images, at, shots):
                got["images"], got["shots"] = sorted(images), sorted(shots)
                return []
        with mock.patch.object(self.hc, "build", lambda: "1"), mock.patch.object(self.hc, "checked", lambda r, *_: r), \
                mock.patch.object(self.hc.capture, "check_scratch", return_value=(0, "")), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(self.hc.publish(Sc, self.tmp, 0, True), 0)
        self.assertEqual(got, {"images": ["boot-1", "menu"], "shots": ["boot-1", "menu"]})

    def test_a_replay_of_another_builds_shots_is_refused(self):
        # A --from replay after the reference build moved on stamped the old
        # shots with the new build (review, 2026-11-18).
        with mock.patch.object(self.hc, "build", lambda: "2"):
            with self.assertRaisesRegex(self.hc.core.Incomplete, "no build.json"):
                self.hc.captured_build(self.tmp)
            (self.tmp / "build.json").write_text(json.dumps({"build": "1"}), encoding="utf-8")
            with self.assertRaisesRegex(self.hc.core.Incomplete, "shots are of build 1, the inventory is of 2"):
                self.hc.captured_build(self.tmp)
            (self.tmp / "build.json").write_text(json.dumps({"build": "2"}), encoding="utf-8")
            self.assertEqual(self.hc.captured_build(self.tmp), "2")

    def test_the_host_shot_must_be_the_guests_screen_after_16_bit_rounding(self):
        guest = (2, 2, bytes([200, 100, 50] * 4))
        rounded = (2, 2, bytes([207, 103, 56] * 4))
        self.assertEqual(self.hc.mapping(rounded, guest), (1.0, 7))
        moved = (2, 2, bytes([200, 100, 50] * 3 + [0, 0, 0]))
        self.assertEqual(self.hc.mapping(moved, guest)[0], 0.75)
        with self.assertRaisesRegex(self.hc.core.Incomplete, "2x2, the guest's screenshot 2x1"):
            self.hc.mapping(guest, (2, 1, bytes(6)))

    def test_a_proof_that_does_not_match_publishes_nothing(self):
        core = self.hc.core
        core.write_png(self.tmp / "desktop.png", 2, 1, bytes([200, 100, 50, 0, 0, 0]))
        core.write_png(self.tmp / "guest.png", 2, 1, bytes([200, 100, 50, 255, 255, 255]))

        class Sc:
            PROOF = ("desktop", "guest.png")
        with self.assertRaisesRegex(core.Incomplete, "not the guest's screen 1:1"):
            self.hc.proven(Sc, self.tmp, {"desktop": {"file": "desktop.png"}})
        core.write_png(self.tmp / "guest.png", 2, 1, bytes([200, 100, 50, 0, 0, 0]))
        self.hc.proven(Sc, self.tmp, {"desktop": {"file": "desktop.png"}})

    def test_the_boot_scenario_proves_its_shots_and_walks_known_key_paths(self):
        sc = load("host_session_boot", INV / "host" / "session-boot.py")
        self.assertEqual(sc.PROOF[0], "desktop")
        self.assertIn({"op": "fetch", "path": "C:\\ios\\out\\guest-desktop.png", "name": sc.PROOF[1]}, sc.STEPS)
        ops = [s["op"] for s in sc.STEPS]
        # The stop error comes before Startup Repair: after a failed repair the
        # recovery pages change their layout, and a key path from the first
        # pages turned the VM off (2026-11-18).
        takes = [s.get("as") or s.get("name") for s in sc.STEPS]          # a burst's as is the shot it takes
        self.assertLess(ops.index("nmi"), takes.index("automatic-repair"))

    def test_the_host_scenarios_of_one_surface_publish_disjoint_shots(self):
        # secure and setup both took first-desktop and network, so one store
        # path held either file's shot (2026-11-18); publish now refuses that.
        owners = {}
        for path in sorted((INV / "host").glob("*.py")):
            sc = load("host_" + path.stem.replace("-", "_"), path)
            for name in sc.SHOTS:
                owners.setdefault((sc.SURFACE, name), []).append(sc.FILE)
        self.assertEqual({k: v for k, v in owners.items() if len(v) > 1}, {})
        self.assertEqual({f for (s, _), fs in owners.items() for f in fs if s == "session"}, {"boot", "secure", "setup"})

    def test_a_spinner_screen_is_measured_from_the_first_frame_with_its_dots(self):
        # The rebuilt VM's restarting shot, one second after the click, held
        # the label alone (2026-11-20); the burst's later frames have the dots.
        sc = load("host_session_secure", INV / "host" / "session-secure.py")
        w, h = 1920, 1080

        def frame(*boxes):
            data = bytearray(bytes(w * h * 3))
            for x, y, bw, bh in boxes:
                for yy in range(y, y + bh):
                    data[(yy * w + x) * 3:(yy * w + x + bw) * 3] = b"\xff" * (bw * 3)
            return (w, h, bytes(data))
        word, dot = (907, 585, 106, 23), (950, 520, 8, 8)
        self.assertEqual(sorted(sc.spinner_frame([frame(word), frame(word, dot), frame()], "restarting")), sorted([word, dot]))
        with self.assertRaisesRegex(sc.core.Incomplete, r"restarting: no frame shows a spinner and its text \(pieces per frame: \[1, 0\]\)"):
            sc.spinner_frame([frame(word), frame()], "restarting")
        self.assertEqual(sc.SHOTS[-sc.SPIN_BURST:], [f"restarting-{i}" for i in range(1, sc.SPIN_BURST + 1)])

    def test_the_first_sign_in_text_skips_the_desktop_and_spans_every_later_line(self):
        sc = load("host_session_secure", INV / "host" / "session-secure.py")
        w, h = 400, 40

        def frame(bg, *boxes):
            px = bytearray(bytes(bg) * (w * h))
            for x0, y0, bw, bh in boxes:
                for y in range(y0, y0 + bh):
                    for x in range(x0, x0 + bw):
                        px[(y * w + x) * 3:(y * w + x) * 3 + 3] = b"\xff\xff\xff"
            return (w, h, bytes(px))
        black = (0, 0, 0)
        frames = [frame(black), frame(black, (190, 20, 20, 8)), frame(black, (100, 18, 250, 10)),
                  frame((189, 199, 222)), frame(black, (150, 20, 120, 8))]
        self.assertEqual(sc.first_sign_in_text(frames, (0, 0, w, h)), ((190, 20, 20, 8), (100, 18, 250, 10)))
        with self.assertRaisesRegex(sc.core.Incomplete, "no line after the greeting"):
            sc.first_sign_in_text(frames[:2] + [frame((189, 199, 222))], (0, 0, w, h))
        # A greeting caught twice is still only the greeting.
        with self.assertRaisesRegex(sc.core.Incomplete, "no line after the greeting"):
            sc.first_sign_in_text(frames[:2] + [frame(black, (190, 20, 20, 8)), frame((189, 199, 222))], (0, 0, w, h))

    def test_refuses_a_run_that_wrote_no_index(self):
        with self.assertRaisesRegex(self.hc.core.Incomplete, "no shots.json"):
            self.hc.taken(self.tmp, 3, ["a"])

    def test_refuses_a_record_not_marked_as_a_hand_record_of_this_build(self):
        good = {"id": "win11.session.x", "surface": "session", "source": "hand:1.2", "capturedFrom": "1.2"}
        self.assertEqual(self.hc.checked([good], "session", "1.2"), [good])
        for bad in ({**good, "source": "uia:1.2"}, {**good, "capturedFrom": "1.3"}, {**good, "surface": "window"}):
            with self.assertRaises(ValueError):
                self.hc.checked([bad], "session", "1.2")

    def test_a_scenario_measures_with_the_front_ends_own_module(self):
        # Its Incomplete must be the class publish() catches, or a refused
        # measurement escapes as a traceback (2026-11-18).
        for path in sorted((INV / "host").glob("*.py")):
            self.assertIs(self.hc.scenario(path.stem).core, self.hc.core, path.name)

    def test_every_scenario_names_what_the_front_end_reads(self):
        for path in sorted((INV / "host").glob("*.py")):
            sc = load(f"host_{path.stem.replace('-', '_')}", path)
            for name in ("SURFACE", "FILE", "PROFILE", "CHECKPOINT", "TIMEOUT", "STEPS", "SHOTS", "records"):
                self.assertTrue(hasattr(sc, name), f"{path.name} has no {name}")
            shot_names = {s["name"] for s in sc.STEPS if s.get("op") == "shot"}
            shot_names |= {f"{s['name']}-{i}" for s in sc.STEPS if s.get("op") == "burst" for i in range(1, int(s["count"]) + 1)}
            shot_names |= {s["as"] for s in sc.STEPS if s.get("op") == "burst" and s.get("as")}   # the page a burst ends on
            self.assertEqual(set(sc.SHOTS) - shot_names, set(), f"{path.name} publishes shots its steps never take")
            if not getattr(sc, "SESSION", True):
                # ref-host refuses these without a session, so the run would stop at the first.
                needs = [s["op"] for s in sc.STEPS if s.get("op") in ("guest", "admin", "fetch", "user", "reconnect")]
                self.assertEqual(needs, [], f"{path.name} runs without a session but has steps that need one")

    def test_a_scenario_without_a_session_runs_ref_host_with_no_session(self):
        seen = []

        class Done:
            returncode, stdout, stderr = 0, "", ""

        def run(argv, **_):
            seen.append(argv)
            return Done()
        with mock.patch.object(self.hc.subprocess, "run", run), mock.patch.object(self.hc.capture, "WORK", self.tmp), \
                mock.patch.object(self.hc.capture, "winpath", str):
            self.hc.run_host("x", [], "pre-oobe", 10, session=False)
            self.hc.run_host("x", [], "clean", 10)
        self.assertEqual(seen[0][-1], "-NoSession")
        self.assertNotIn("-NoSession", seen[1])

    def test_a_host_run_never_stamps_a_folder_another_run_filled(self):
        # ref-host refused a folder that was not empty and run_host then wrote
        # the current build over that run's build.json (review, 2026-11-19).
        seen = []

        class Done:
            returncode, stdout, stderr = 0, "", ""

        def run(argv, **_):
            seen.append(argv)
            return Done()
        with mock.patch.object(self.hc.subprocess, "run", run), mock.patch.object(self.hc.capture, "WORK", self.tmp), \
                mock.patch.object(self.hc.capture, "winpath", str), mock.patch.object(self.hc.time, "strftime", lambda _f: "20260101-000000"), \
                mock.patch.object(self.hc, "build", lambda: "26300.9999"):
            old = self.tmp / "x-20260101-000000"
            old.mkdir()
            (old / "build.json").write_text(json.dumps({"build": "26300.1"}), encoding="utf-8")
            out, rc = self.hc.run_host("x", [], "clean", 10)
        # A run in the same second has a folder of its own (review, 2026-11-21).
        self.assertEqual(rc, 0)
        self.assertNotEqual(out, old)
        self.assertTrue(out.parent.name.startswith("x-20260101-000000-"), out)
        self.assertEqual(len(seen), 1)
        self.assertEqual(json.loads((old / "build.json").read_text(encoding="utf-8")), {"build": "26300.1"})

    def test_an_interrupted_host_walk_restores_its_checkpoint(self):
        # Ctrl+C killed ref-host before its finally, and only a deadline ran
        # the recovery (review, 2026-11-18).
        def run(cmd, **kw):
            raise KeyboardInterrupt
        with mock.patch.object(self.hc.subprocess, "run", run), mock.patch.object(self.hc.capture, "WORK", self.tmp), \
                mock.patch.object(self.hc.capture, "winpath", str), \
                mock.patch.object(self.hc.capture, "stop_vm", return_value="restored") as stop, \
                contextlib.redirect_stdout(io.StringIO()) as said, self.assertRaises(KeyboardInterrupt):
            self.hc.run_host("x", [], "pre-oobe", 10, session=False)
        self.assertEqual(stop.call_args[0][1], "pre-oobe")
        self.assertIn("interrupted; restored", said.getvalue())
        self.assertEqual(list(self.tmp.glob("*-steps.json")), [])

    def test_a_scenario_publishes_its_canonical_shot_under_its_files_state(self):
        # The capture matrix's session/boot row had no shot: the boot walk
        # names none "boot" (review, 2026-11-19).
        sc = types.SimpleNamespace(FILE="boot", PROFILE="fhd-100", CANONICAL="menu")
        out, lay = self.tmp / "out", self.tmp / "lay"
        out.mkdir()
        lay.mkdir()
        shots, images = {}, {}
        for n, px in (("menu", b"m"), ("logo", b"l")):
            (out / f"{n}.png").write_bytes(px)
            shots[n] = {"file": f"{n}.png", "depth": 16, "modeBefore": [1920, 1080], "exact": True}
            images[n] = (1920, 1080, b"")
        states = self.hc.lay_out(sc, shots, images, out, lay)
        self.assertEqual([s["file"] for s in states], ["menu.fhd-100.png", "logo.fhd-100.png", "boot.fhd-100.png"])
        self.assertEqual((lay / "boot.fhd-100.png").read_bytes(), b"m")
        self.assertEqual(states[2]["exact"], True)
        # A walk with a shot of that name publishes it once.
        shots["boot"], images["boot"] = dict(shots["logo"], file="logo.png"), images["logo"]
        self.assertEqual([s["file"] for s in self.hc.lay_out(sc, shots, images, out, lay)].count("boot.fhd-100.png"), 1)
        # The boot scenario names a shot it takes, for its file's matrix row.
        boot = load("host_session_boot", INV / "host" / "session-boot.py")
        self.assertIn(boot.CANONICAL, boot.SHOTS)
        rows = json.loads((INV.parent.parent / "docs/design/inventory/expected.json").read_text(encoding="utf-8"))
        self.assertIn((boot.SURFACE, boot.FILE), {(r["surface"], r["state"]) for r in rows})

    def test_an_interrupt_before_ref_host_starts_leaves_the_vm_alone(self):
        # An interrupt or timeout while wslpath converted a path ran the
        # checkpoint recovery for a walk that never started (review, 2026-11-19).
        for exc in (KeyboardInterrupt(), self.hc.subprocess.TimeoutExpired("wslpath", 1)):
            seen = []

            def win(p, exc=exc, seen=seen):       # only the first conversion fails
                seen.append(p)
                if len(seen) == 1:
                    raise exc
                return str(p)
            with mock.patch.object(self.hc.subprocess, "run") as run, mock.patch.object(self.hc.capture, "WORK", self.tmp), \
                    mock.patch.object(self.hc.capture, "winpath", win), \
                    mock.patch.object(self.hc.capture, "stop_vm", return_value="restored") as stop, \
                    contextlib.redirect_stdout(io.StringIO()), self.assertRaises(type(exc)):
                self.hc.run_host("x", [], "pre-oobe", 10, session=False)
            self.assertEqual((run.call_count, stop.call_count), (0, 0), exc)
            self.assertEqual(list(self.tmp.glob("*-steps.json")), [])

    def test_the_stop_error_is_timed_to_the_first_exact_frame_without_it(self):
        # An inexact frame still showing the stop error was taken as its end,
        # and the end was searched after the last stop frame (review, 2026-11-18).
        sc = load("host_session_boot", INV / "host" / "session-boot.py")
        stop, clear = (1, 1, b"s"), (1, 1, b"c")
        is_stop = lambda i: i[2] == b"s"

        def span(*frames):
            names = [f"stop-{i}" for i in range(1, len(frames) + 1)]
            shots = {n: (stop if f[0] == "s" else clear) for n, f in zip(names, frames)}
            taken = {n: {"exact": f[1] == "x"} for n, f in zip(names, frames)}
            return sc.stop_span(names, taken, shots, is_stop)
        self.assertEqual(span("cx", "sx", "sx", "s~", "c~", "cx"), ("stop-2", "stop-6"))   # inexact frames are passed over
        self.assertEqual(span("sx", "cx", "cx"), ("stop-1", "stop-2"))
        self.assertEqual(span("sx", "sx"), ("stop-1", None))
        self.assertEqual(span("cx", "cx"), (None, None))
        with self.assertRaises(sc.core.Incomplete):
            span("sx", "cx", "sx", "cx")                                  # it came back: no single span

    def test_the_stop_progress_compares_only_frames_of_one_mode(self):
        # Two stop frames of different modes were counted as two compared
        # frames (review, 2026-11-18).
        sc = load("host_session_boot", INV / "host" / "session-boot.py")
        a, b = (2, 1, bytes(6)), (2, 1, bytes([255] * 6))
        other = (1, 1, bytes(3))
        box = (0, 0, 2, 1)
        self.assertEqual(sc.band_change([a, other], box), "one frame caught it, so this walk shows no change")
        self.assertEqual(sc.band_change([a, a, other], box), "its text stayed the same in the 2 frames compared")
        self.assertEqual(sc.band_change([a, b, other], box), "its text changed across the 2 frames compared")

    def test_the_spinner_and_the_repair_phase_are_read_only_from_frames_that_show_them(self):
        # The blue boot menu read as the spinner alone, and the repair record
        # claimed a second phase no frame showed (review, 2026-11-18).
        sc = load("host_session_boot", INV / "host" / "session-boot.py")
        w, h = 1920, 1080

        def frame(bg, *boxes):
            data = bytearray(bytes(bg) * (w * h))
            for x, y, bw, bh in boxes:
                for yy in range(y, y + bh):
                    data[(yy * w + x) * 3:(yy * w + x + bw) * 3] = b"\xff" * (bw * 3)
            return (w, h, bytes(data))
        black = (0, 0, 0)
        self.assertEqual(sc.spinner_alone(frame(black, (940, 820, 40, 40))), (940, 820, 40, 40))
        self.assertIsNone(sc.spinner_alone(frame(sc.BLUE)))                        # the menu
        self.assertIsNone(sc.spinner_alone(frame(black, (600, 400, 700, 30))))     # a line of text
        self.assertIsNone(sc.spinner_alone(frame(black, (868, 391, 183, 45))))     # the firmware logo alone
        # Restarting: the dots move, so each frame shows one phase; the
        # spinner is their union above the one label (boot-3, 5 and 6 of the
        # 2026-11-19 walk).
        word = (906, 585, 108, 23)
        third = frame(black, (942, 492, 18, 10), (972, 496, 7, 7), (928, 524, 6, 6), (985, 528, 6, 7), (936, 541, 7, 7), word)
        arc = frame(black, (928, 521, 13, 25), word)                        # a partial arc
        self.assertEqual(sc.restarting_parts(arc), ([(928, 521, 13, 25)], word))
        self.assertIsNone(sc.restarting_parts(frame(black, word)))           # the label alone
        self.assertIsNone(sc.restarting_parts(frame(black, (900, 300, 200, 200), word)))   # not a spinner
        self.assertIsNone(sc.restarting_parts(frame(sc.BLUE, (942, 492, 18, 10), word)))  # not on black
        self.assertEqual(sc.restarting_spinner([third, arc, frame(black, (941, 492, 18, 11), (986, 521, 6, 6), word)]),
                         ((928, 492, 64, 56), word, 3))
        # A wide arc (46 by 15) is shaped like text; the frames of the
        # 2026-11-19 00:14 walk, boot-3 to boot-6, span 58 px together.
        walk = [frame(black, (936, 492, 46, 15), (933, 538, 7, 6), (975, 543, 7, 7), word),
                frame(black, (963, 492, 6, 7), (986, 518, 6, 6), word),
                frame(black, (928, 492, 60, 31), word), frame(black, (933, 492, 51, 19), word)]
        self.assertEqual(sc.restarting_spinner(walk), ((928, 492, 64, 58), word, 4))
        # A frame whose label sits 3 px off is another screen: its dots stay
        # out; 2 px off is the same label.
        moved = frame(black, (900, 470, 10, 10), (909, 588, 108, 23))
        near = frame(black, (930, 500, 8, 8), (908, 585, 108, 23))
        self.assertEqual(sc.restarting_spinner([third, moved]), ((928, 492, 63, 56), word, 1))
        self.assertEqual(sc.restarting_spinner([third, near]), ((928, 492, 63, 56), word, 2))
        # The boot logo's spinner: the union of the compact dots under the
        # logo, in frames that show the logo.
        sw, sh = 1024, 768

        def fw(*boxes):
            data = bytearray(sw * sh * 3)
            for x, y, bw, bh in boxes:
                for yy in range(y, y + bh):
                    data[(yy * sw + x) * 3:(yy * sw + x + bw) * 3] = b"\xff" * (bw * 3)
            return (sw, sh, bytes(data))
        blogo = (420, 361, 184, 45)
        self.assertEqual(sc.logo_spinner([fw(blogo, (504, 610, 17, 6)), fw(blogo, (498, 598, 29, 18)), fw(blogo), fw((10, 10, 50, 50), (498, 598, 29, 18))], blogo),
                         ((498, 598, 29, 18), 2))
        # Startup Repair's first frame, in the firmware's 1024x768: the logo
        # with its label, with a spinner between them too, or with a spinner
        # alone (shapes from the saved walk, 2026-11-18).
        def small(*boxes):
            sw, sh = 1024, 768
            data = bytearray(sw * sh * 3)
            for x, y, bw, bh in boxes:
                for yy in range(y, y + bh):
                    data[(yy * sw + x) * 3:(yy * sw + x + bw) * 3] = b"\xff" * (bw * 3)
            return (sw, sh, bytes(data))
        logo, label = (420, 361, 183, 45), (451, 636, 124, 21)
        self.assertEqual(sc.repair_label(small(logo, label), logo), (logo, label))
        self.assertEqual(sc.repair_label(small(logo, (497, 592, 25, 24), label), logo), (logo, label))
        self.assertIsNone(sc.repair_label(small(logo, (497, 592, 13, 10)), logo))
        self.assertIsNone(sc.repair_label(frame(black, logo, label), logo))      # the display's mode is not the first phase
        self.assertIsNone(sc.repair_label(small((497, 592, 25, 24), label), logo))   # a spinner is not the logo
        self.assertIsNone(sc.repair_label(small(logo, (497, 640, 12, 4)), logo))     # a fragment is not the label
        mark = (868, 391, 183, 45)
        self.assertTrue(sc.diagnosing(frame(black, mark, (931, 826, 40, 40), (785, 896, 294, 38)), mark))
        self.assertFalse(sc.diagnosing(frame(black, (619, 394, 691, 27), (662, 494, 606, 27), (776, 653, 379, 41)), mark))   # the failure page
        self.assertFalse(sc.diagnosing(frame(black, mark), mark))
        self.assertFalse(sc.diagnosing(frame(black, mark, (785, 896, 60, 38), (900, 896, 60, 38)), mark))   # two words, no spinner
        # The options link's focus is a dotted rectangle around its text; the
        # same menu without it, and a blank frame, are not focus (review,
        # 2026-11-19). The geometry is the walks' (172, 647, 786, 41).
        text = (181, 660, 458, 26)

        def dotted(bg, rect, *boxes):
            img = frame(bg, *boxes)
            data = bytearray(img[2])
            x, y, rw, rh = rect
            for xx in range(x, x + rw, 2):
                for yy in (y, y + rh - 1):
                    data[(yy * w + xx) * 3:(yy * w + xx) * 3 + 3] = b"\xff\xff\xff"
            for yy in range(y, y + rh, 2):
                for xx in (x, x + rw - 1):
                    data[(yy * w + xx) * 3:(yy * w + xx) * 3 + 3] = b"\xff\xff\xff"
            return (w, h, bytes(data))
        self.assertEqual(sc.link_focus(dotted(sc.BLUE, (172, 647, 786, 41), text), text), (172, 647, 786, 41))
        self.assertIsNone(sc.link_focus(frame(sc.BLUE, text), text))                   # no rectangle
        self.assertIsNone(sc.link_focus(frame(black), text))                           # a blank frame
        self.assertIsNone(sc.link_focus(frame(sc.BLUE, (172, 647, 786, 41)), text))   # a solid block, not dotted
        # The phases in the order the burst saw them: a diagnosing frame
        # before the first phase is not its second phase (review, 2026-11-19).
        first, diag = small(logo, label), frame(black, mark, (931, 826, 40, 40), (785, 896, 294, 38))
        self.assertEqual(sc.repair_phases([diag, first], logo)[1:], (logo, label, False))
        self.assertEqual(sc.repair_phases([first, diag], logo)[1:], (logo, label, True))
        self.assertIsNone(sc.repair_phases([diag], logo))
        # The failure page is two wide lines and the details under them; the
        # diagnosing screen's logo, spinner and label are three rows as well.
        # The details' two lines are those of the 02:22 walk's page.
        reason, path = (855, 653, 221, 12), (776, 678, 379, 16)
        rows = [(619, 394, 691, 27), (662, 494, 606, 27), (776, 653, 379, 41)]
        self.assertEqual(sc.failure_rows(frame(black, rows[0], rows[1], reason, path), logo), rows)
        self.assertIsNone(sc.failure_rows(diag, logo))
        self.assertIsNone(sc.failure_rows(frame(black, rows[0], rows[1]), logo))               # no details
        self.assertIsNone(sc.failure_rows(frame(black, rows[0], rows[1], reason), logo))       # no log path
        self.assertIsNone(sc.failure_rows(frame(black, rows[0], rows[1], path), logo))         # no reason
        # A detail line is 8 to 20 px tall; the gap between the two is the
        # walk's 13 px at every height.
        for tall, ok in ((7, False), (8, True), (20, True), (21, False)):
            got = sc.failure_rows(frame(black, rows[0], rows[1], (855, 653, 221, tall), (776, 653 + tall + 13, 379, 16)), logo)
            self.assertEqual(got is not None, ok, f"reason {tall} px")
            got = sc.failure_rows(frame(black, rows[0], rows[1], (855, 653, 221, 12), (776, 678, 379, tall)), logo)
            self.assertEqual(got is not None, ok, f"log path {tall} px")
        self.assertIsNone(sc.failure_rows(frame(black, rows[0], (662, 494, 300, 27), reason, path), logo))   # a short line

    def test_corner_marks_and_tile_descriptions_read_only_their_own_text(self):
        # Desktop icons under a Safe Mode mark, the focus border beside a tile
        # name and a wrapped name all read as text (review, 2026-11-18).
        sc = load("host_session_boot", INV / "host" / "session-boot.py")
        w, h = 400, 300

        def frame(*boxes):
            data = bytearray(b"\x00" * (w * h * 3))
            for x, y, bw, bh in boxes:
                for yy in range(y, y + bh):
                    data[(yy * w + x) * 3:(yy * w + x + bw) * 3] = b"\xff" * (bw * 3)
            return (w, h, bytes(data))
        # The mark in rows 3 to 11 (a gap inside it), an icon from row 14.
        img = frame((2, 3, 54, 2), (2, 6, 54, 6), (2, 14, 40, 6))
        self.assertEqual(sc.band(img, (0, 1, 120, 16), sc.white), (0, 3, 120, 9))
        self.assertEqual(sc.one(img, sc.band(img, (0, 1, 120, 16), sc.white), "mark", sc.white, 6), (2, 3, 54, 9))
        self.assertEqual(sc.band(frame(), (0, 1, 120, 16), sc.white), (0, 1, 120, 16))
        # The production crop on a focused tile with only a name: the focus
        # rectangle's right border must fall outside the text region.
        tile = (40, 40, 108, 108)
        ring = (28, 28, 404 - 400 + 300, 134)              # ring_width 304 fits the frame
        rw = ring[2]
        focused = frame(tile, (161, 40, 100, 20), (ring[0] + rw - 2, ring[1], 2, ring[3]))
        x, y, tw, th = sc.tile_text(tile, rw)
        self.assertLess(x + tw, ring[0] + rw - 2)
        self.assertEqual(sc.rows_in(focused, sc.tile_text(tile, rw), sc.white, 10), [(161, 40, 100, 20)])
        self.assertFalse(sc.has_description(focused, sc.tile_text(tile, rw)))
        name = (20, 10, 120, 20)
        self.assertFalse(sc.has_description(frame(name), (0, 0, 300, 120)))
        self.assertTrue(sc.has_description(frame(name, (20, 44, 160, 11)), (0, 0, 300, 120)))
        self.assertFalse(sc.has_description(frame(name, (20, 41, 130, 25)), (0, 0, 300, 120)))      # a wrapped name
        self.assertTrue(sc.has_description(frame(name, (20, 41, 130, 25), (20, 74, 150, 11)), (0, 0, 300, 120)))

    def test_the_setup_scenario_walks_the_local_account_path(self):
        sc = load("host_session_setup", INV / "host" / "session-setup.py")
        self.assertEqual(sc.PAGES, ["region", "keyboard", "second-keyboard", "network", "local-account",
                                    "getting-ready", "privacy", "preparing"])
        self.assertFalse(sc.SESSION)

    def test_the_setup_panel_is_found_by_its_edge_on_a_light_backdrop(self):
        # A landscape behind the privacy page left the backdrop as light as the
        # acrylic panel (247, 247, 255 beside 255), and the colour test broke
        # (2026-11-18); the edge's shadow step still tells them apart.
        sc = load("host_session_setup", INV / "host" / "session-setup.py")
        w, h, box = 200, 160, (40, 30, 120, 100)
        data = bytearray()
        for y in range(h):
            for x in range(w):
                inside = box[0] <= x < box[0] + box[2] and box[1] <= y < box[1] + box[3]
                near = box[0] - 3 <= x < box[0] + box[2] + 3 and box[1] - 3 <= y < box[1] + box[3] + 3
                data += bytes((255, 255, 255) if inside else (231, 231, 239) if near else (247, 247, 255))
        img = (w, h, bytes(data))
        sc.panel_at(img, box, "privacy")
        for moved in ((41, 30, 120, 100), (40, 30, 119, 100), (40, 29, 120, 100)):
            with self.assertRaisesRegex(self.hc.core.Incomplete, "no panel edge"):
                sc.panel_at(img, moved, "privacy")
        flat = (w, h, bytes((247, 247, 255)) * (w * h))
        with self.assertRaisesRegex(self.hc.core.Incomplete, "no panel edge at the left"):
            sc.panel_at(flat, box, "privacy")


if __name__ == "__main__":
    unittest.main(verbosity=1)
