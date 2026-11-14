#!/usr/bin/env python3
"""Tests for the completeness baselines: the row builders in baseline_core.py
over sources shaped like the reference machine's (written here, never copied
from it), and baselines.py's acceptance, assembly and publication.

    python3 scripts/inventory/tests/test_baselines.py
"""
import importlib.util
import json
import pathlib
import shutil
import tempfile
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent.parent


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, HERE / file)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


baselines = load("baselines", "baselines.py")
core = baselines.core   # the module baselines.py raises from
B = "26300.9711"


def setting(fid, pages=(), deep=None, cond=None, page_id=None, **attrs):
    nodes = "".join(f'<Node Type="Page">{p}</Node><Node Type="Group">g</Node>' for p in pages)
    a = "".join(f' {k}="{v}"' for k, v in attrs.items())
    return (f"<SearchableContent{a}><Filename>{fid}</Filename>"
            f"<ApplicationInformation>{f'<DeepLink>{deep}</DeepLink>' if deep else ''}</ApplicationInformation>"
            f"<SettingIdentity><SettingPaths><Path>{nodes}</Path></SettingPaths>"
            f"{f'<PageID>{page_id}</PageID>' if page_id else ''}{f'<Condition>{cond}</Condition>' if cond else ''}"
            f"</SettingIdentity><SettingInformation><Description>@res,-1</Description></SettingInformation>"
            f"</SearchableContent>")


def xml(*items):
    return ("<PCSettings>" + "".join(items) + "</PCSettings>").encode()


class Settings(unittest.TestCase):
    def test_a_row_holds_the_id_pages_link_condition_and_flags(self):
        rows = core.settings_rows([("a.xml", xml(setting("S1", pages=["P1", "P1", "P2"], deep="ms-settings:x",
                                                          cond="shcond://v1#X;0", IncludeWithFeature="F")))])
        self.assertEqual(rows, [{"id": "S1", "pages": ["P1", "P2"], "deepLink": "ms-settings:x",
                                 "condition": "shcond://v1#X;0", "includeWithFeature": "F"}])

    def test_description_text_is_never_read(self):
        rows = core.settings_rows([("a.xml", xml(setting("S1")))])
        self.assertNotIn("@res", json.dumps(rows))

    def test_page_id_stands_in_when_there_is_no_page_path(self):
        self.assertEqual(core.settings_rows([("a.xml", xml(setting("S1", page_id="P9")))])[0]["pages"], ["P9"])

    def test_the_largest_file_supplies_a_setting_named_twice(self):
        small = ("b.xml", xml(setting("S1", pages=["old"])))
        big = ("a.xml", xml(setting("S1", pages=["new"]), setting("S2")))
        for order in ([small, big], [big, small]):
            rows = core.settings_rows(order)
            self.assertEqual([r["id"] for r in rows], ["S1", "S2"])
            self.assertEqual(rows[0]["pages"], ["new"])

    def test_a_malformed_source_is_refused(self):
        for files in ([("a.xml", b"<PCSettings>")], [("a.xml", b"<Other/>")], [("a.xml", xml("<SearchableContent/>"))],
                      [("a.xml", xml())]):
            with self.assertRaises(core.SourceError):
                core.settings_rows(files)


G = "{025A5937-A6BE-4686-A844-36FE4BEC8B6D}"
G2 = "{3EB685DB-65F9-4CF6-A03A-E3EF65729F3D}"


class ControlPanel(unittest.TestCase):
    def test_clsids_and_cpl_files(self):
        rows = core.control_panel_rows([(G, "Microsoft.PowerOptions"), (G2, None), ("DelegateFolders", None)],
                                       ["Main.cpl"])
        self.assertEqual(rows, [{"id": "clsid:" + G.lower(), "kind": "namespace", "name": "Microsoft.PowerOptions"},
                                {"id": "clsid:" + G2.lower(), "kind": "namespace"},
                                {"id": "cpl:main.cpl", "kind": "cpl", "file": "Main.cpl"}])

    def test_refusals(self):
        for ns, cpls in (([], ["x.dll"]), ([(G, None), (G.lower(), None)], []), ([], [])):
            with self.assertRaises(core.SourceError):
                core.control_panel_rows(ns, cpls)


class KnownFolders(unittest.TestCase):
    def test_a_row_keeps_facts_and_drops_the_localized_name(self):
        rows = core.known_folder_rows([(G, {"Name": "AccountPictures", "Category": 4, "RelativePath": "A\\B",
                                            "ParentFolder": G2, "LocalizedName": "@shell32.dll,-1"})])
        self.assertEqual(rows, [{"id": G.lower(), "name": "AccountPictures", "category": 4,
                                 "relativePath": "A\\B", "parent": G2.lower()}])

    def test_refusals(self):
        for folders in ([("notaguid", {"Name": "x"})], [(G, {})]):
            with self.assertRaises(core.SourceError):
                core.known_folder_rows(folders)


class StartApps(unittest.TestCase):
    def test_kind_follows_the_app_id(self):
        rows = core.start_app_rows([{"AppID": "Pkg_x!App", "Name": "A"}, {"AppID": "{G}\\x.exe", "Name": "B"},
                                    {"AppID": "Pkg_x!App", "Name": "A again"}])
        self.assertEqual(rows, [{"id": "Pkg_x!App", "name": "A", "kind": "packaged"},
                                {"id": "{G}\\x.exe", "name": "B", "kind": "desktop"}])

    def test_an_entry_without_an_id_is_refused(self):
        with self.assertRaises(core.SourceError):
            core.start_app_rows([{"Name": "A"}])


GALLERY = {"Groups": [{"UniqueId": "Fundamentals", "IsSpecialSection": True, "Items": [{"UniqueId": "Binding", "Title": "B"}]},
                      {"UniqueId": "BasicInput", "Items": [{"UniqueId": "Button", "Title": "Button", "Description": "prose"}]}]}


class Controls(unittest.TestCase):
    def test_special_sections_and_prose_are_left_out(self):
        self.assertEqual(core.control_rows(GALLERY), [{"id": "Button", "name": "Button", "category": "BasicInput"}])

    def test_refusals(self):
        for data in ([], {"Groups": [{"UniqueId": "G", "Items": [{"Title": "x"}]}]},
                     {"Groups": [{"UniqueId": "G", "Items": [{"UniqueId": "a", "Title": "a"}]},
                                 {"UniqueId": "H", "Items": [{"UniqueId": "a", "Title": "a"}]}]}):
            with self.assertRaises(core.SourceError):
                core.control_rows(data)

    def test_the_fetch_is_pinned_and_bounded(self):
        seen = []
        rows = baselines.gallery_rows(lambda u: seen.append(u) or json.dumps(GALLERY).encode())
        self.assertEqual(len(rows), 1)
        self.assertIn(core.GALLERY["commit"], seen[0])
        with self.assertRaises(core.SourceError):
            baselines.gallery_rows(lambda u: b"not json")


class ClassicControls(unittest.TestCase):
    def test_only_names_under_the_version_prefix_count(self):
        names = ["6.0.26100.9711!Button", "6.0.26100.9711!SysLink", "Button", "6.0.1!Edit", "6.0.26100.9711!"]
        self.assertEqual(core.classic_control_rows(names, "6.0.26100.9711"), [{"id": "Button"}, {"id": "SysLink"}])

    def test_no_classes_is_refused(self):
        with self.assertRaises(core.SourceError):
            core.classic_control_rows(["Button"], "6.0.26100.9711")


class ShellCommands(unittest.TestCase):
    def test_folders_and_verbs(self):
        verbs = {"*": ["pintohomefile"], "Directory": ["cmd"], "Directory\\Background": ["cmd"], "Drive": []}
        ids = [r["id"] for r in core.shell_command_rows(["Desktop"], verbs)]
        self.assertEqual(ids, ["folder:Desktop", "verb:*/pintohomefile", "verb:Directory/cmd",
                               "verb:Directory\\Background/cmd"])

    def test_every_verb_class_must_be_read(self):
        with self.assertRaises(core.SourceError):
            core.shell_command_rows(["Desktop"], {"*": []})


def guest_output(tmp, **over):
    vm = {n: {"source": "s", "rows": [{"id": "r"}]} for n in core.VM_BASELINES}
    got = {"complete": True, "build": B, "baselines": vm}
    got.update(over)
    (tmp / "baselines.json").write_text(json.dumps(got), encoding="utf-8")


class Received(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_a_complete_read_of_the_reference_build_is_taken(self):
        guest_output(self.tmp)
        self.assertEqual(set(baselines.received(self.tmp, 0, B)), set(core.VM_BASELINES))

    def test_refusals(self):
        cases = {"missing": None, "incomplete": {"complete": False, "incomplete": "x"},
                 "other build": {"build": "26300.1"}, "short": {"baselines": {"settings": {"source": "s", "rows": []}}}}
        for why, over in cases.items():
            with self.subTest(why):
                (self.tmp / "baselines.json").unlink(missing_ok=True)
                if over is not None:
                    guest_output(self.tmp, **over)
                with self.assertRaises(core.SourceError):
                    baselines.received(self.tmp, 0, B)
        guest_output(self.tmp)
        with self.assertRaises(core.SourceError):
            baselines.received(self.tmp, 1, B)


class Assemble(unittest.TestCase):
    def test_headers_name_build_or_upstream(self):
        vm = {n: {"source": "s", "rows": [{"id": "r"}]} for n in core.VM_BASELINES}
        files = baselines.assemble(vm, B, [{"id": "Button"}])
        self.assertEqual(len(files), 7)
        self.assertEqual(files["settings/baseline.json"][0],
                         {"baseline": "settings", "owner": "settings", "source": "s", "build": B})
        head = files["controls/baseline.json"][0]
        self.assertNotIn("build", head)
        self.assertEqual(head["upstream"]["commit"], core.GALLERY["commit"])

    def test_the_assembled_set_passes_the_checker(self):
        vm = {n: {"source": "s", "rows": [{"id": "r"}]} for n in core.VM_BASELINES}
        code, report = baselines.capture.check_scratch(baselines.assemble(vm, json.loads(
            (baselines.INVENTORY / "reference.json").read_text())["build"], [{"id": "Button"}]))
        self.assertEqual(code, 0, report)


class Publish(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.patch = mock.patch.object(baselines, "INVENTORY", self.tmp)
        self.patch.start()

    def tearDown(self):
        self.patch.stop()
        shutil.rmtree(self.tmp)

    def files(self, tag):
        return {f"{n}/baseline.json": [{"baseline": n}, {"id": tag}] for n in ("settings", "controls")}

    def test_all_files_land_together(self):
        baselines.publish(self.files("one"), self.tmp / "journal")
        baselines.publish(self.files("two"), self.tmp / "journal")
        for n in ("settings", "controls"):
            self.assertIn('"two"', (self.tmp / n / "baseline.json").read_text())
        self.assertEqual(sorted(p.name for p in self.tmp.rglob("*") if p.is_file()), ["baseline.json", "baseline.json"])

    def test_a_failure_at_any_rename_leaves_the_previous_set(self):
        # Renames: two staged writes, the journal, then aside + into place per file.
        real = baselines.capture.os.replace
        for fail_at in range(1, 8):
            with self.subTest(fail_at=fail_at):
                shutil.rmtree(self.tmp)
                self.tmp.mkdir()
                baselines.publish(self.files("one"), self.tmp / "journal")
                calls = []

                def flaky(a, b):
                    calls.append(a)
                    if len(calls) == fail_at:
                        raise OSError("disk full")
                    return real(a, b)
                with mock.patch.object(baselines.capture.os, "replace", flaky):
                    with self.assertRaises(OSError):
                        baselines.publish(self.files("two"), self.tmp / "journal")
                for n in ("settings", "controls"):
                    self.assertIn('"one"', (self.tmp / n / "baseline.json").read_text())
                left = sorted(str(p.relative_to(self.tmp)) for p in self.tmp.rglob("*") if p.is_file())
                self.assertEqual(left, ["controls/baseline.json", "settings/baseline.json"])

if __name__ == "__main__":
    unittest.main()
