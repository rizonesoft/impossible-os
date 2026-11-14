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
import pathlib
import shutil
import struct
import sys
import tempfile
import types
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
    args = dict(surface="notepad-smoke", build="26300.9711", profile="fhd-100", root_slug=smoke.ROOT_SLUG,
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
                        profile="fhd-100", root_slug="window", root_label="Notepad",
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
        args = dict(surface="s", build="1.2", profile="p", root_slug="r", root_label="R")
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
        args = dict(surface="s", build="1.2", profile="p", root_slug="r", root_label="R",
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
        cap = core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", root_slug="r", root_label="R")
        self.assertEqual([r["label"] for r in cap.records[1:]], ["OK"])
        d = core.dump(core.FixtureElement(t))
        core.confirm_dump(d, cap, surface="s", build="1.2", profile="p", root_slug="r", root_label="R")

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
                core.walk(core.FixtureElement(wide), surface="s", build="1.2", profile="p", root_slug="r",
                          root_label="R")
        finally:
            core.FixtureElement.__init__ = real
        self.assertEqual(len(made), 1)                # the root only

    def test_reading_is_bounded_even_when_nothing_is_recorded(self):
        panes = [node("", "Pane") for _ in range(core.MAX_VISITS + 1)]
        t = node("Root", "Window", children=[node("", "Pane", children=panes)])
        with self.assertRaises(core.Incomplete):
            core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", root_slug="r", root_label="R")

    def test_tooltips_teaching_tips_and_prose_are_skipped(self):
        t = node("Root", "Window", bounds=(0, 0, 500, 500), children=[
            node("Help", "ToolTip"), node("", "Pane", "Tip", "Microsoft.UI.Xaml.Controls.TeachingTip"),
            node("x" * 81, "Text"), node("OK", "Button"), node("Hidden", "Button", offscreen=True)])
        cap = core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="fhd-100", root_slug="r",
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
        cap = core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", root_slug="r", root_label="R")
        self.assertEqual(cap.records[1]["id"], "win11.s.r.open")


class Identity(unittest.TestCase):
    def twins(self, *names):
        return node("Root", "Window", children=[node(n, "Button", "Twin") if n else node("", "Button", cls="Btn")
                                                  for n in names])

    def run_walk(self, t, identity=()):
        return core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", root_slug="r",
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
        cap = core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", root_slug="r",
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
            core.walk(core.FixtureElement(deep), surface="s", build="1.2", profile="p", root_slug="r", root_label="R")


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
        cap = core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", root_slug="r", root_label="R")
        old = [dict(r, behaviour="Saves.") if r["id"] == "win11.s.r.twin--1.apply" else r for r in cap.records]
        with self.assertRaises(ValueError) as ex:
            core.merge(old, cap.records, cap.ambiguous)
        self.assertIn("win11.s.r.twin--1.apply", str(ex.exception))

    def test_a_group_that_shrinks_to_one_drops_its_note(self):
        # Delete and Save share an automation id; Delete carries a note. Delete
        # is removed: Save must not inherit "Deletes the document."
        def cap_of(*names):
            t = node("Root", "Window", children=[node(n, "Button", "Twin") for n in names])
            return core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", root_slug="r",
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
        before = core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", root_slug="r", root_label="R")
        old = [dict(r, behaviour="Saves.") if r["id"].startswith("win11.s.r.twin--1") else r for r in before.records]
        t2 = node("Root", "Window", children=[node("", "Group", "Twin-1", children=[node("Apply", "Button")])])
        after = core.walk(core.FixtureElement(t2), surface="s", build="1.2", profile="p", root_slug="r", root_label="R")
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
        real = guest.read_profile
        guest.read_profile = lambda: dict(SEEN_OK, dpi=120)
        try:
            with contextlib.redirect_stdout(io.StringIO()) as out:
                code = guest.main(["--surface", "no-such-surface", "--out", "x.json",
                                   "--profiles", str(INV.parents[1] / "docs/design/inventory/profiles.json")])
        finally:
            guest.read_profile = real
        self.assertEqual(code, 2)
        self.assertEqual(out.getvalue().strip(), "profile fhd-100: scaling is 125, expected 100")
        self.assertIsNone(guest.user32)          # nothing was opened

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
        core.walk(core.FixtureElement(t), surface="s", build="1.2", profile="p", root_slug="r", root_label="R",
                  shot=lambda state, el, items: seen.append(state) or {"file": f"{state}.p.png"})
        self.assertEqual(sorted(seen), ["r", "r.a-b-open", "r.a.b-open"])


class Dump(unittest.TestCase):
    def test_the_dump_walks_to_the_same_records(self):
        cap = walk()
        d = core.dump(core.FixtureElement(tree()), environment_lists=smoke.ENVIRONMENT_LISTS)
        core.confirm_dump(d, cap, surface="notepad-smoke", build="26300.9711", profile="fhd-100",
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
        return dict(surface="notepad-smoke", build="26300.9711", profile="fhd-100", root_slug="window",
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


if __name__ == "__main__":
    unittest.main(verbosity=1)
