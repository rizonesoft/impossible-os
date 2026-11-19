"""operator-capture.py: records from a Settings page's tree, and what is blanked."""
import importlib.util
import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("operator_capture", HERE.parent / "operator-capture.py")
oc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(oc)

MON = [-5760, 0, 1920, 1080]


def el(ctype, name="", aid="", cls="", bounds=(-5000, 100, 100, 20), kids=()):
    return {"controlType": ctype, "name": name, "automationId": aid, "className": cls, "offscreen": False,
            "bounds": list(bounds), "children": list(kids)}


def card(title, aid=""):
    return el("Group", f"Connected device {title}, category keyboard", aid, "NamedContainerAutomationPeer",
              kids=[el("Text", ""), el("Text", title)])


def window(*content, account=("Pat Doe", "pat@example.com")):
    head = el("Button", " ".join(account), "UserProfileControlButton", bounds=(-5755, 49, 290, 75),
              kids=[el("Text", account[0], "UserName", bounds=(-5672, 70, 128, 19)),
                    el("Text", account[1], "UserAccount", bounds=(-5672, 89, 128, 16))])
    crumb = el("Button", "Devices", cls="Microsoft.UI.Xaml.Controls.BreadcrumbBarItem")
    return el("Window", "Settings", kids=[head, crumb, el("Pane", aid=oc.CONTENT_ID, kids=list(content))])


def device_list(*titles):
    return el("List", aid="Devices_InputDeviceList", kids=[el("Group", kids=[
        card(t, f"\\\\?\\USB#VID_{i}#{{guid}}") for i, t in enumerate(titles)])])


class Records(unittest.TestCase):
    def page(self, top, end=None):
        dump = {"monitor": MON, "uri": "ms-settings:bluetooth", "top": top, "views": [end] if end else []}
        personal = oc.personal_strings(top, ["PAT-PC"])
        return oc.page_records("bluetooth", dump, "26300.9974", personal)

    def test_a_list_keeps_its_first_entry_as_the_template_in_every_tree(self):
        top = window(device_list("Keyboard A", "Mouse B", "Headset C"))
        # Scrolled, the realized entries start elsewhere: none of them is a record.
        end = window(device_list("Mouse B", "Headset C"))
        recs = self.page(top, end)
        lst = next(r for r in recs if r.get("controlType") == "list")
        entries = [r for r in recs if r.get("parent") == lst["id"]]
        self.assertEqual([r["label"] for r in entries], ["Entry"])
        self.assertFalse([r["label"] for r in recs if r["label"] in ("Mouse B", "Headset C")],
                         "an entry past the template is this machine's, under any parent")
        self.assertIn("(3 seen on the operator's machine)", lst["behaviour"])
        self.assertNotIn("uiaId", entries[0], "a device path is this machine's, not the design's")

    def test_a_card_is_labelled_by_its_shown_title_not_a_glyph_or_its_sentence(self):
        recs = self.page(window(card("Add a network camera", "Camera_Add")))
        self.assertEqual([r["label"] for r in recs if r["kind"] == "setting"], ["Add a network camera"])

    def test_the_machine_name_never_reaches_a_label(self):
        recs = self.page(window(el("Text", "Discoverable as “PAT-PC”", "Device_DiscoverableAs_TextBlock")))
        labels = [r["label"] for r in recs]
        self.assertIn("Discoverable as “<personal>”", labels)
        self.assertFalse(any("PAT-PC" in str(r) for r in recs))


    def test_a_list_first_realized_after_scrolling_keeps_its_template_and_count(self):
        recs = self.page(window(device_list()), window(device_list("Keyboard A", "Mouse B")))
        lst = next(r for r in recs if r.get("controlType") == "list")
        self.assertEqual([r["label"] for r in recs if r.get("parent") == lst["id"]], ["Entry"])
        self.assertIn("(2 seen on the operator's machine)", lst["behaviour"])

    def test_a_renamed_device_never_reaches_a_record(self):
        recs = self.page(window(device_list("Pat's headset", "Pat's phone")))
        self.assertFalse([r["id"] for r in recs if "Pat" in str(r)])

    def test_the_leak_check_reads_unescaped_text(self):
        self.assertEqual(oc.leaks([{"id": "a", "kind": "control", "uiaId": "Card", "label": "Jos\u00e9's card"}],
                                  ["JOS\u00c9"]), ["a"])

    def test_a_network_named_like_a_page_does_not_block_the_fixed_records(self):
        top = window(el("Group", "Help with WiFi", aid="SystemSettings_Connections_WiFi_Help"),
                     el("Button", "WiFi properties", bounds=(-5100, 500, 300, 40)))
        dump = {"monitor": MON, "uri": "ms-settings:network-wifi", "top": top}
        personal = oc.personal_strings(top, ["PAT-PC", "WiFi"])
        recs = oc.records({"wifi": dump}, "26300.9974", personal)
        self.assertEqual(oc.leaks(recs, personal), [])
        self.assertIn("win11.settings.hardware.wifi", [r["id"] for r in recs])
        self.assertFalse([r for r in recs if "wifi properties" in r.get("label", "").casefold()])

    def test_a_label_derived_id_is_checked(self):
        self.assertEqual(oc.leaks([{"id": "p.pat-home-5g", "kind": "control", "label": "x"}], ["Pat Home 5G"]),
                         ["p.pat-home-5g"])

    def test_a_page_read_at_another_build_is_stale(self):
        self.assertEqual(oc.stale_pages({"wifi": {"build": "26300.9818"}, "camera": {}}, "26300.9974"),
                         {"wifi": "26300.9818", "camera": None})


    def test_a_control_repeating_the_template_title_does_not_carry_it(self):
        radio = el("Button", "Workshop adapter", aid="RadioToggle")
        entry = el("Group", "Workshop adapter", cls="NamedContainerAutomationPeer",
                   kids=[el("Text", "Workshop adapter"), radio])
        recs = self.page(window(el("List", aid="Adapters", kids=[entry])))
        self.assertFalse([r["id"] for r in recs if "workshop" in str(r).casefold()])

    def test_a_template_title_holding_a_personal_string_is_replaced_whole(self):
        radio = el("Button", "PAT-PC Studio adapter", aid="RadioToggle")
        entry = el("Group", "PAT-PC Studio adapter", cls="NamedContainerAutomationPeer",
                   kids=[el("Text", "PAT-PC Studio adapter"), radio])
        recs = self.page(window(el("List", aid="Adapters", kids=[entry])))
        self.assertFalse([r["id"] for r in recs if "studio" in str(r).casefold()])

    def test_entries_seen_in_different_views_are_all_counted(self):
        recs = self.page(window(device_list("A", "Monitor", "Monitor")), window(device_list("Monitor", "C", "D")))
        lst = next(r for r in recs if r.get("controlType") == "list")
        self.assertIn("(5 seen on the operator's machine)", lst["behaviour"])

    def test_one_title_at_two_places_on_the_page_is_two_entries(self):
        def monitor(y):
            c = card("Monitor", "m")
            c["bounds"] = [-5149, y, 1000, 69]
            return c
        top = window(el("List", aid="Displays", kids=[el("Group", kids=[card("A", "a"), monitor(900)])]))
        end = el("Pane", aid=oc.CONTENT_ID, kids=[el("List", aid="Displays", kids=[el("Group", kids=[monitor(300)])])])
        end["scrolled"] = 1200                 # this Monitor sits 600 px below the first on the page
        recs = oc.page_records("bluetooth", {"monitor": MON, "uri": "ms-settings:bluetooth", "top": top,
                                             "views": [end]}, "26300.9974", oc.personal_strings(top, []))
        lst = next(r for r in recs if r.get("controlType") == "list")
        self.assertIn("(3 seen on the operator's machine)", lst["behaviour"])

    def test_a_control_is_a_child_of_its_card_and_an_expander_of_its_template(self):
        add = el("Button", "Add desktop app", aid="AddDesktopApp")
        cardn = el("Group", "Custom settings for applications", cls="NamedContainerAutomationPeer",
                   kids=[el("Text", "Custom settings for applications"), add])
        exp = el("Button", "Show more settings", aid="EntityItemButton", cls="ExpanderToggleButton")
        entry = el("Group", "Pat's app", cls="NamedContainerAutomationPeer", kids=[el("Text", "Pat's app"), exp])
        recs = self.page(window(cardn, el("List", aid="Apps", kids=[entry])))
        by = {r["label"]: r for r in recs}
        self.assertEqual(by["Add desktop app"]["parent"], by["Custom settings for applications"]["id"])
        self.assertEqual(by["Show more settings"]["parent"], by["Entry"]["id"])

    def test_the_content_scroll_bar_is_not_a_page_record(self):
        bar = el("ScrollBar", "", cls="ScrollBar", kids=[el("Button", "Vertical Small Decrease")])
        recs = self.page(window(el("Button", "Add device", aid="AddDevice"), bar))
        self.assertNotIn("Vertical Small Decrease", [r["label"] for r in recs])
        self.assertIn("Add device", [r["label"] for r in recs])

    def test_each_template_keeps_its_expander_once(self):
        def entry(title):
            exp = el("Button", "Show more settings", aid="EntityItemButton", cls="ExpanderToggleButton",
                     kids=[el("Button", "Show more settings", aid="EntityItemButton", cls="ExpanderToggleButton")])
            own = el("Button", title, aid="EntityItemButton", cls="Button")
            return el("Group", title, cls="NamedContainerAutomationPeer", kids=[el("Text", title), own, exp])
        a = el("List", aid="Input", kids=[entry("Keyboard A")])
        b = el("List", aid="Audio", kids=[entry("Headset C"), entry("Speaker D")])
        recs = self.page(window(a, b))
        shown = [r["label"] for r in recs if r.get("kind") == "control" and r.get("controlType") == "button"]
        self.assertEqual(shown, ["Show more settings", "Show more settings"])

    def test_a_nested_peer_of_one_field_is_one_record(self):
        box = el("Edit", "Search this list", aid="AppListSearch", cls="TextBox", bounds=(-5450, 520, 220, 32),
                 kids=[el("Edit", "Search this list", bounds=(-5450, 520, 220, 32))])
        recs = self.page(window(box))
        self.assertEqual([r["label"] for r in recs if r.get("controlType") == "edit"], ["Search this list"])

    def test_bounds_first_measured_after_scrolling_are_kept(self):
        hidden = el("Button", "Sound", aid="Sound_Link", bounds=(-5100, 2000, 72, 40))
        hidden["offscreen"] = True
        shown_ = el("Button", "Sound", aid="Sound_Link", bounds=(-5100, 600, 72, 40))
        recs = self.page(window(hidden), window(shown_))
        self.assertEqual(next(r for r in recs if r["label"] == "Sound")["bounds"],
                         {"x": 660, "y": 600, "width": 72, "height": 40})

    def test_a_control_cut_by_the_viewport_takes_its_whole_size_from_a_later_view(self):
        def tree(y, h):
            exp = el("Button", "Show more settings", aid="EntityItemButton", cls="ExpanderToggleButton",
                     bounds=(-4190, y, 32, h))
            entry = el("Group", "Pat's phone", cls="NamedContainerAutomationPeer", kids=[el("Text", "Pat's phone"), exp])
            pane = el("Pane", aid=oc.CONTENT_ID, bounds=(-5760, 0, 1920, 1032),
                      kids=[el("List", aid="Other", kids=[entry])])
            return el("Window", "Settings", kids=[pane])
        recs = self.page(tree(1019, 13), tree(500, 32))
        exp = next(r for r in recs if r["label"] == "Show more settings")
        self.assertEqual((exp["bounds"]["y"], exp["bounds"]["height"]), (500, 32))

    def test_a_scrolled_view_of_the_content_pane_alone_is_read(self):
        pane = el("Pane", aid=oc.CONTENT_ID, kids=[el("Button", "Sound", aid="Sound_Link", bounds=(-5100, 600, 72, 40))])
        recs = self.page(window(), pane)
        self.assertIn("Sound", [r["label"] for r in recs])

    def test_every_list_on_a_page_keeps_its_own_template(self):
        a = el("List", aid="Input", kids=[card("Keyboard A")])
        b = el("List", aid="Audio", kids=[card("Headset C"), card("Speaker D")])
        recs = self.page(window(a, b))
        lists = [r["id"] for r in recs if r.get("controlType") == "list"]
        self.assertEqual([[r["label"] for r in recs if r.get("parent") == lid] for lid in lists], [["Entry"], ["Entry"]])

    def test_a_tree_of_another_page_is_refused(self):
        self.assertEqual(oc.wrong_pages({"wifi": {"top": window()}}), {"wifi": "Devices"})
        crumb = el("Button", "Wi-Fi", cls="Microsoft.UI.Xaml.Controls.BreadcrumbBarItem")
        ok = el("Window", "Settings", kids=[crumb, el("Pane", aid=oc.CONTENT_ID)])
        self.assertEqual(oc.wrong_pages({"wifi": {"top": ok, "monitor": MON, "dpi": 96}}), {})

    def test_a_page_read_at_another_scale_is_refused(self):
        crumb = el("Button", "Wi-Fi", cls="Microsoft.UI.Xaml.Controls.BreadcrumbBarItem")
        ok = el("Window", "Settings", kids=[crumb, el("Pane", aid=oc.CONTENT_ID)])
        self.assertEqual(oc.wrong_pages({"wifi": {"top": ok, "monitor": MON, "dpi": 120}}),
                         {"wifi": "1920x1080 at 120 dpi"})
        self.assertEqual(oc.wrong_pages({"wifi": {"top": ok, "monitor": MON}}), {"wifi": "1920x1080 at None dpi"})
        self.assertEqual(oc.wrong_pages({"wifi": {"top": ok, "monitor": [0, 0, 2560, 1440], "dpi": 96}}),
                         {"wifi": "2560x1440 at 96 dpi"})

    def test_a_run_without_its_identity_cannot_be_redacted(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(oc.core.Incomplete):
                oc.identity(pathlib.Path(d))
            (pathlib.Path(d) / "identity.json").write_text('{"computer": "PAT-PC", "user": "pat", "profile": ""}')
            with self.assertRaises(oc.core.Incomplete):
                oc.identity(pathlib.Path(d))
            # Runs from before the host read the connected SSIDs cannot be redacted either.
            for old in ('{"computer": "PAT-PC", "user": "pat", "profile": "C:\\\\Users\\\\pat"}',
                        '{"computer": "PAT-PC", "user": "pat", "profile": "C:\\\\Users\\\\pat", "networks": ["Home"]}',
                        '{"computer": "PAT-PC", "user": "pat", "profile": "C:\\\\Users\\\\pat",'
                        ' "ssids": [], "profiles": ["Home"]}',
                        '{"version": 2, "computer": "PAT-PC", "user": "pat", "profile": "C:\\\\Users\\\\pat",'
                        ' "ssids": [], "profiles": ["Home"]}',
                        '{"version": 3, "computer": "PAT-PC", "user": "pat", "profile": "C:\\\\Users\\\\pat",'
                        ' "ssids": [], "profiles": ["Home"]}',
                        '{"version": 4, "computer": "PAT-PC", "user": "pat", "profile": "C:\\\\Users\\\\pat",'
                        ' "ssids": [], "profiles": ["Home"]}',
                        '{"version": 5, "computer": "PAT-PC", "user": "pat", "profile": "C:\\\\Users\\\\pat",'
                        ' "ssids": [], "profiles": ["Home"]}',
                        '{"version": 6, "computer": "PAT-PC", "user": "pat", "profile": "C:\\\\Users\\\\pat",'
                        ' "ssids": [], "profiles": ["Home"]}',
                        '{"version": 7, "computer": "PAT-PC", "user": "pat", "profile": "C:\\\\Users\\\\pat",'
                        ' "ssids": [], "profiles": ["Home"]}'):
                (pathlib.Path(d) / "identity.json").write_text(old)
                with self.assertRaises(oc.core.Incomplete):
                    oc.identity(pathlib.Path(d))


class Host(unittest.TestCase):
    def test_an_interrupted_host_call_still_restores_the_theme(self):
        calls = []

        def fake_run(cmd, **kw):
            calls.append(cmd)
            if "-Pages" in cmd:
                raise KeyboardInterrupt
            return subprocess.CompletedProcess(cmd, 0, "", "")
        with tempfile.TemporaryDirectory() as work:
            (pathlib.Path(work) / oc.PENDING).write_text("[]")    # the host wrote it before its theme change
            with mock.patch.object(oc.capture, "WORK", pathlib.Path(work)), \
                    mock.patch.object(oc.capture, "winpath", lambda p: str(p)), \
                    mock.patch.object(oc.subprocess, "run", fake_run):
                with self.assertRaises(KeyboardInterrupt):
                    oc.run_host(["wifi"])
        self.assertEqual([("-Restore" in c) for c in calls], [False, True])

    def test_nothing_pending_launches_nothing(self):
        with tempfile.TemporaryDirectory() as work, \
                mock.patch.object(oc.subprocess, "run", side_effect=AssertionError("launched")):
            oc.restore(pathlib.Path(work) / "operator-x")

    def test_saved_network_names_are_personal(self):
        with tempfile.TemporaryDirectory() as d:
            (pathlib.Path(d) / "identity.json").write_text(
                '{"version": 8, "computer": "PAT-PC", "user": "pat", "profile": "C:\\\\Users\\\\pat",'
                ' "ssids": "Pat Home 5G", "profiles": ["Pat Home"]}')
            names = oc.identity(pathlib.Path(d))
        self.assertIn("Pat Home 5G", names)
        dump = {"monitor": MON, "uri": "ms-settings:network-wifi",
                "top": window(el("Button", "Pat Home 5G properties", aid="Props"))}
        recs = oc.page_records("wifi", dump, "26300.9974", oc.personal_strings(dump["top"], names))
        self.assertFalse([r["id"] for r in recs if "home 5g" in str(r).casefold()])


class Main(unittest.TestCase):
    def test_a_replay_does_not_query_the_live_build(self):
        with tempfile.TemporaryDirectory() as d, \
                mock.patch.object(oc, "restore"), \
                mock.patch.object(oc, "host_build", side_effect=AssertionError("queried")):
            args = oc.argparse.Namespace(src=d, dry_run=True)
            self.assertEqual(oc.measure(args, ["wifi"]), pathlib.Path(d))

    def test_a_busy_inventory_keeps_the_measured_run(self):
        with tempfile.TemporaryDirectory() as d:
            locks = pathlib.Path(d)
            with mock.patch.object(oc, "HOST_LOCK", locks / "host.lock"), \
                    mock.patch.object(oc.capture, "LOCK", locks / "capture.lock"), \
                    mock.patch.object(oc, "measure", return_value=locks / "run") as measured, \
                    mock.patch.object(oc, "check_and_publish", side_effect=AssertionError("published")):
                with oc.capture.exclusive(locks / "capture.lock"):       # a VM capture holds the inventory
                    self.assertEqual(oc.main(["--dry-run"]), 4)
        measured.assert_called_once()

    def test_the_host_journal_root_is_the_capture_root(self):
        # operator-host.ps1 keeps the restore journal in a fixed root; it must be
        # the folder Python puts runs in and looks for the journal in.
        text = oc.HOST_SCRIPT.read_text(encoding="utf-8")
        root = next(line.split("'")[1] for line in text.splitlines() if line.startswith("$Captures = "))
        drive, rest = oc.capture.WORK.parts[2], oc.capture.WORK.parts[3:]
        self.assertEqual(root, "\\".join([f"{drive.upper()}:", *rest]))

    def test_the_host_lock_is_shared_by_every_checkout(self):
        # Beside the pending-restore file, not under this checkout's build/.
        self.assertEqual(oc.HOST_LOCK.parent, oc.capture.WORK)
        with tempfile.TemporaryDirectory() as d:
            lock = pathlib.Path(d) / "operator-host.lock"
            with mock.patch.object(oc, "HOST_LOCK", lock), \
                    mock.patch.object(oc, "measure", side_effect=AssertionError("measured")):
                with oc.capture.exclusive(lock):                         # another checkout's capture
                    self.assertEqual(oc.main(["--dry-run"]), 4)

    def test_a_subset_of_pages_publishes_nothing(self):
        with self.assertRaises(SystemExit) as e:
            oc.main(["--pages", "wifi"])
        self.assertEqual(e.exception.code, 2)


class Blanks(unittest.TestCase):
    def test_the_account_header_the_taskbar_and_a_personal_text_are_blanked(self):
        top = window(el("Text", "Connect a new device to PAT-PC", bounds=(-5100, 200, 300, 20)))
        dump = {"monitor": MON, "top": top}
        rects = oc.blanks(dump, oc.personal_strings(top, ["PAT-PC"]))
        self.assertIn((0, 1080 - oc.TASKBAR, 1920, oc.TASKBAR), rects)
        self.assertIn((5, 49, 290, 75), rects)                 # the account header, monitor-relative
        self.assertIn((660, 200, 300, 20), rects)

    def test_a_control_painting_a_personal_name_is_blanked(self):
        top = window(el("Button", "Pat Home 5G properties", bounds=(-5100, 500, 300, 40)))
        rects = oc.blanks({"monitor": MON, "top": top}, ["Pat Home 5G"])
        self.assertIn((660, 500, 300, 40), rects)

    def test_a_list_that_moved_while_the_page_was_copied_is_refused(self):
        def tree(y):
            entry = el("Group", "App", cls="NamedContainerAutomationPeer",
                       kids=[el("Text", "Pat's app", bounds=(-5090, y, 200, 19))])
            return window(el("List", aid="Apps", kids=[entry]))
        self.assertFalse(oc.moved({"monitor": MON, "before": tree(612), "top": tree(612)}, []))
        self.assertTrue(oc.moved({"monitor": MON, "before": tree(612), "top": tree(684)}, []))
        self.assertTrue(oc.moved({"monitor": MON, "top": tree(612)}, []))

    def test_redaction_never_matches_its_own_placeholder(self):
        names = ["personal", *"personal", *"PERSONAL"]
        self.assertEqual(oc.scrub("x personal zz", names), "x <personal> zz")
        self.assertEqual(oc.scrub("Pat Doe's phone", ["Pat", "Pat Doe"]), "<personal>'s phone")

    def test_an_entry_icon_is_blanked(self):
        icon = el("Image", "", bounds=(-5130, 300, 24, 24))
        entry = el("Group", "App", cls="NamedContainerAutomationPeer", kids=[icon, el("Text", "Pat's app")])
        rects = oc.blanks({"monitor": MON, "top": window(el("List", aid="Apps", kids=[entry]))}, [])
        self.assertIn((630, 300, 24, 24), rects)

    def test_an_entry_icon_missing_from_the_tree_is_blanked(self):
        entry = el("Group", "App", cls="NamedContainerAutomationPeer", bounds=(-5149, 600, 1000, 69),
                   kids=[el("Text", "Pat's app", bounds=(-5090, 612, 200, 19))])
        rects = oc.blanks({"monitor": MON, "top": window(el("List", aid="Apps", kids=[entry]))}, [])
        self.assertIn((611, 600, 59, 69), rects)

    def test_every_entry_title_is_blanked_and_windows_own_cards_are_not(self):
        nested = el("Group", "Show available networks", cls="NamedContainerAutomationPeer",
                    kids=[el("Text", "Show available networks", bounds=(-5100, 400, 200, 20))])
        entry = el("Group", "Adapter", cls="NamedContainerAutomationPeer",
                   kids=[el("Text", "Pat's adapter", bounds=(-5100, 300, 200, 20)), nested])
        lst = el("List", aid="Adapters", kids=[entry, card("Pat's headset")])
        rects = oc.blanks({"monitor": MON, "top": window(lst)}, [])
        self.assertIn((660, 300, 200, 20), rects)
        self.assertNotIn((660, 400, 200, 20), rects)
        self.assertEqual(sum(1 for r in rects if r == (760, 100, 100, 20)), 2)   # the second entry's title and glyph


class Shown(unittest.TestCase):
    def test_a_glyph_is_not_text(self):
        self.assertFalse(oc.shown(""))
        self.assertFalse(oc.shown(" "))
        self.assertTrue(oc.shown("Wi-Fi"))


if __name__ == "__main__":
    unittest.main()
