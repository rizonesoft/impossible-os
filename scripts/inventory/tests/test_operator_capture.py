"""operator-capture.py: records from a Settings page's tree, and what is blanked."""
import contextlib
import importlib.util
import io
import json
import pathlib
import re
import shutil
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

    def test_a_second_entry_of_one_title_adds_nothing_to_the_template(self):
        def monitor(y, button):
            c = card("Monitor", "m")
            c["bounds"] = [-5149, y, 1000, 69]
            c["children"].append(el("Button", button, aid=button.replace(" ", "")))
            return c
        top = window(el("List", aid="Displays", kids=[el("Group", kids=[monitor(900, "Identify")])]))
        end = el("Pane", aid=oc.CONTENT_ID, kids=[el("List", aid="Displays",
                                                       kids=[el("Group", kids=[monitor(300, "Calibrate")])])])
        end["scrolled"] = 1200                 # another Monitor, 600 px below the first
        recs = oc.page_records("bluetooth", {"monitor": MON, "uri": "ms-settings:bluetooth", "top": top,
                                             "views": [end]}, "26300.9974", oc.personal_strings(top, []))
        labels = [r["label"] for r in recs]
        self.assertIn("Identify", labels)
        self.assertNotIn("Calibrate", labels)

    def test_an_entry_without_bounds_is_not_taken_for_the_template_by_title(self):
        def monitor(y, button):
            c = card("Monitor", "m")
            c["bounds"] = [-5149, y, 1000, 69]
            c["children"].append(el("Button", button, aid=button.replace(" ", "")))
            return c
        top = window(el("List", aid="Displays", kids=[el("Group", kids=[monitor(900, "Identify")])]))
        later = monitor(300, "Calibrate")
        later["offscreen"] = True
        later["bounds"] = None
        end = el("Pane", aid=oc.CONTENT_ID, kids=[el("List", aid="Displays", kids=[el("Group", kids=[later])])])
        end["scrolled"] = 1200
        recs = oc.page_records("bluetooth", {"monitor": MON, "uri": "ms-settings:bluetooth", "top": top,
                                             "views": [end]}, "26300.9974", oc.personal_strings(top, []))
        self.assertNotIn("Calibrate", [r["label"] for r in recs])

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

    def test_a_control_in_a_card_is_named_for_its_card(self):
        def cardn(title):
            return el("Group", title, cls="NamedContainerAutomationPeer",
                      kids=[el("Text", title), el("Button", "Show more settings", aid="EntityItemButton",
                                                  cls="ExpanderToggleButton")])
        def expander_ids(*titles):
            recs = self.page(window(*[cardn(x) for x in titles]))
            return {r["parent"].rsplit(".", 1)[-1]: r["id"] for r in recs if r.get("uiaId") == "EntityItemButton"}
        both = expander_ids("Advanced graphics settings", "Help from the web")
        help_only = expander_ids("Help from the web")
        self.assertEqual(help_only["help-from-the-web"], both["help-from-the-web"])
        self.assertNotIn(both["advanced-graphics-settings"], help_only.values())

    def test_a_control_in_a_card_inside_a_template_is_named_for_that_card(self):
        def inner(title):
            return el("Group", title, cls="NamedContainerAutomationPeer",
                      kids=[el("Text", title), el("Button", "Show more settings", aid="EntityItemButton",
                                                  cls="ExpanderToggleButton")])
        def expander_ids(*titles):
            entry = el("Group", "Pat's adapter", cls="NamedContainerAutomationPeer",
                       kids=[el("Text", "Pat's adapter")] + [inner(x) for x in titles])
            recs = self.page(window(el("List", aid="Adapters", kids=[entry])))
            return {r["parent"].rsplit(".", 1)[-1]: r["id"] for r in recs if r["label"] == "Show more settings"}
        both = expander_ids("Show available networks", "Hardware properties")
        one = expander_ids("Hardware properties")
        self.assertEqual(one["adapters-entry-hardware-properties"], both["adapters-entry-hardware-properties"])
        self.assertNotIn(both["adapters-entry-show-available-networks"], one.values())

    def test_template_ids_are_named_for_their_list_not_their_place(self):
        def entry(title):
            return el("Group", title, cls="NamedContainerAutomationPeer",
                      kids=[el("Text", title), el("Button", "Remove", aid="RemoveButton")])
        def ids(*lists):
            recs = self.page(window(*[el("List", aid=aid, kids=[entry(t)] if t else []) for aid, t in lists]))
            return {r["label"]: r["id"] for r in recs if r["label"] in ("Entry", "Remove")}, recs
        _, both = ids(("Input", "Pat's mouse"), ("Audio", "Pat's speaker"))
        audio_only, recs = ids(("Input", None), ("Audio", "Pat's speaker"))
        self.assertTrue(audio_only["Entry"].endswith(".audio-entry"))
        self.assertTrue(audio_only["Remove"].endswith(".audio-entry-remove"))
        self.assertIn(audio_only["Entry"], [r["id"] for r in both])
        self.assertIn(audio_only["Remove"], [r["id"] for r in both])

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
        self.assertEqual(oc.wrong_pages({"wifi": {"top": ok, "monitor": MON, "dpi": 96, "textScale": 100, "highContrast": "none"}}), {})

    def test_a_page_read_at_another_scale_text_size_or_contrast_is_refused(self):
        crumb = el("Button", "Wi-Fi", cls="Microsoft.UI.Xaml.Controls.BreadcrumbBarItem")
        ok = el("Window", "Settings", kids=[crumb, el("Pane", aid=oc.CONTENT_ID)])
        good = {"top": ok, "monitor": MON, "dpi": 96, "textScale": 100, "highContrast": "none"}
        for change, why in (({"dpi": 120}, "dpi 120"), ({"dpi": None}, "dpi None"),
                            ({"monitor": [0, 0, 2560, 1440]}, "width 2560, height 1440"),
                            ({"textScale": 150}, "textScale 150"), ({"textScale": None}, "textScale None"),
                            ({"highContrast": "High Contrast Black"}, "highContrast High Contrast Black")):
            self.assertEqual(oc.wrong_pages({"wifi": good | change}), {"wifi": why})

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
                        ' "ssids": [], "profiles": ["Home"]}',
                        '{"version": 8, "computer": "PAT-PC", "user": "pat", "profile": "C:\\\\Users\\\\pat",'
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

    def test_shot_is_the_window_own_pixels(self):
        # A screen copy holds whatever shows over Settings (a notification, an
        # overlay) and no check around it can be atomic: the shot is the window's
        # own capture, compiled from the helper beside the host script.
        host = oc.HOST_SCRIPT.read_text(encoding="utf-8")
        self.assertNotIn("CopyFromScreen", host)
        self.assertIn("[WindowCapture]::Capture(", host)
        helper = (oc.HERE / "window-capture.cs").read_text(encoding="utf-8")
        self.assertIn("public static byte[] Capture(", helper)
        # A frame taken mid-resize is refused (its masks would be off), and a
        # copy past its deadline is cancelled with its frame released.
        self.assertIn("frame.ContentSize", helper)
        self.assertIn("op.Cancel()", helper)
        self.assertLess(helper.index("using (frame)"), helper.index("CreateCopyFromSurfaceAsync"))

    def test_pages_open_after_mouse_input(self):
        # A window opened while the keyboard was the last input draws a keyboard
        # focus ring the reference does not have; placing the cursor is no input.
        host = oc.HOST_SCRIPT.read_text(encoding="utf-8")
        self.assertNotIn("SetCursorPos", host)
        for opened in [m.start() for m in re.finditer(r"Start-Process \$Uri\[\$page\]", host)]:
            self.assertIn("[W]::PointerTo(", host[max(0, opened - 200):opened])

    def test_saved_network_names_are_personal(self):
        with tempfile.TemporaryDirectory() as d:
            (pathlib.Path(d) / "identity.json").write_text(
                '{"version": %d, "computer": "PAT-PC", "user": "pat", "profile": "C:\\\\Users\\\\pat",'
                ' "ssids": "Pat Home 5G", "profiles": ["Pat Home"]}' % oc.IDENTITY_VERSION)
            names = oc.identity(pathlib.Path(d))
        self.assertIn("Pat Home 5G", names)
        dump = {"monitor": MON, "uri": "ms-settings:network-wifi",
                "top": window(el("Button", "Pat Home 5G properties", aid="Props"))}
        recs = oc.page_records("wifi", dump, "26300.9974", oc.personal_strings(dump["top"], names))
        self.assertFalse([r["id"] for r in recs if "home 5g" in str(r).casefold()])

    def test_a_list_prefix_named_like_a_network_is_not_a_leak(self):
        # A saved network called "WiFi": the adapter list's automation ID holds
        # "wifi", and its template and controls carry that ID as their prefix.
        entry = el("Group", "Pat's adapter", cls="NamedContainerAutomationPeer",
                   kids=[el("Text", "Pat's adapter"), el("Button", "Hardware properties", aid="HwProps")])
        top = window(el("List", aid="SystemSettings_Connections_Wifi_Adapter_List_ItemsControl", kids=[entry]))
        dump = {"monitor": MON, "uri": "ms-settings:network-wifi", "top": top}
        names = ["WiFi"]
        recs = oc.page_records("wifi", dump, "26300.9974", oc.personal_strings(top, names))
        self.assertTrue(any(r["id"].endswith("-wifi-adapter-list-itemscontrol-entry-hardware-properties") for r in recs))
        self.assertEqual(oc.leaks(recs, names), [])
        # A control below another control of the template carries the template's prefix.
        child = next(r for r in recs if r["id"].endswith("-entry-hardware-properties"))
        deep = {**child, "id": child["id"] + "-more", "label": "More", "parent": child["id"]}
        deep["id"] = child["id"].replace("hardware-properties", "more")
        self.assertEqual(oc.leaks(recs + [deep], names), [])
        # What the record itself adds is still read: a control named for the network is caught.
        child = next(r for r in recs if r["id"].endswith("-entry-hardware-properties"))
        bad = {**child, "id": child["id"].replace("hardware-properties", "wifi-properties"), "label": "Properties"}
        self.assertEqual(oc.leaks(recs + [bad], names), [bad["id"]])


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

    def test_the_host_refuses_what_the_profile_does_not_allow(self):
        # operator-host.ps1 refuses a run before the theme changes: its fixed
        # profile must be the one Python publishes under.
        text = oc.HOST_SCRIPT.read_text(encoding="utf-8")
        line = next(x for x in text.splitlines() if x.startswith("$Width = "))
        host = {k.strip().lstrip("$"): v.strip().strip("'") for k, v in (a.split("=") for a in line.split(";"))}
        prof = oc.profile()
        self.assertEqual((int(host["Width"]), int(host["Height"]), int(host["Dpi"]), int(host["TextScale"]),
                          host["Contrast"]),
                         (prof["width"], prof["height"], prof["dpi"], prof["textScale"], prof["highContrast"]))

    def test_the_host_lock_is_shared_by_every_checkout(self):
        # Beside the pending-restore file, not under this checkout's build/.
        self.assertEqual(oc.HOST_LOCK.parent, oc.capture.WORK)
        with tempfile.TemporaryDirectory() as d:
            lock = pathlib.Path(d) / "operator-host.lock"
            with mock.patch.object(oc, "HOST_LOCK", lock), \
                    mock.patch.object(oc, "measure", side_effect=AssertionError("measured")):
                with oc.capture.exclusive(lock):                         # another checkout's capture
                    self.assertEqual(oc.main(["--dry-run"]), 4)

    def test_a_recapture_keeps_what_a_person_wrote(self):
        rid = "win11.settings.hardware.bluetooth.bluetooth"
        with tempfile.TemporaryDirectory() as d:
            target = pathlib.Path(d) / "host.json"
            old = {"id": rid, "kind": "control", "label": "Bluetooth", "behaviour": "old text",
                   "disposition": "build", "inconsistency": "a note", "baselineId": "nosuchbaseline:row"}
            target.write_text(json.dumps(old) + "\n", encoding="utf-8")
            new = {"id": rid, "kind": "control", "label": "Bluetooth", "behaviour": "new text", "disposition": "build"}
            with contextlib.redirect_stdout(io.StringIO()):
                out = oc.carry_hand_fields(target, [new])
        self.assertEqual(out, [{**new, "inconsistency": "a note", "baselineId": "nosuchbaseline:row"}])

    def test_a_recapture_keeps_a_persons_disposition(self):
        rid = "win11.settings.hardware.bluetooth.bluetooth"
        for kept in ({"disposition": "equivalent"}, {"disposition": "excluded", "reason": "not on this OS"}):
            with tempfile.TemporaryDirectory() as d:
                target = pathlib.Path(d) / "host.json"
                target.write_text(json.dumps({"id": rid, "kind": "control", "label": "Bluetooth", **kept}) + "\n",
                                  encoding="utf-8")
                new = {"id": rid, "kind": "control", "label": "Bluetooth", "disposition": "build"}
                fresh = {"id": "win11.settings.hardware.bluetooth.new", "kind": "control", "label": "New",
                         "disposition": "build"}
                with contextlib.redirect_stdout(io.StringIO()):
                    out = oc.carry_hand_fields(target, [new, fresh])
            self.assertEqual(out, [{**new, **kept}, fresh])

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

    def test_the_bluetooth_radio_name_is_personal_apart_from_the_computer_name(self):
        shown = el("Text", "Discoverable as \u201cPat's Laptop\u201d", aid=oc.DISCOVERABLE_ID,
                   bounds=(-5100, 300, 300, 20))
        top = window(shown)
        personal = oc.personal_strings(top, ["PAT-PC"])
        self.assertIn("Pat's Laptop", personal)
        recs = oc.page_records("bluetooth", {"monitor": MON, "uri": "ms-settings:bluetooth", "top": top},
                               "26300.9974", personal)
        self.assertFalse([r["id"] for r in recs if "laptop" in str(r).casefold()])
        self.assertEqual(oc.leaks(recs, personal), [])
        self.assertIn((660, 300, 300, 20), oc.blanks({"monitor": MON, "top": top}, personal))

    def test_an_ssid_starting_with_the_adapter_name_is_replaced_whole(self):
        entry = el("Group", "Wi-Fi", cls="NamedContainerAutomationPeer",
                   kids=[el("Text", "Wi-Fi"), el("Button", "Wi-Fi Studio Loft properties", aid="Props")])
        top = window(el("List", aid="Adapters", kids=[entry]))
        personal = oc.personal_strings(top, ["Wi-Fi Studio Loft"])
        recs = oc.page_records("wifi", {"monitor": MON, "uri": "ms-settings:network-wifi", "top": top},
                               "26300.9974", personal)
        self.assertFalse([r["id"] for r in recs if "loft" in str(r).casefold()])
        self.assertIn("<personal> properties", [r["label"] for r in recs])

    def test_a_radio_name_holding_quotes_is_read_whole(self):
        shown = el("Text", 'Discoverable as \u201cStudio "Juniper" Laptop\u201d', aid=oc.DISCOVERABLE_ID)
        top = window(shown, el("Button", 'Rename Studio "Juniper" Laptop', aid="Rename"))
        personal = oc.personal_strings(top, ["PAT-PC"])
        self.assertIn('Studio "Juniper" Laptop', personal)
        recs = oc.page_records("bluetooth", {"monitor": MON, "uri": "ms-settings:bluetooth", "top": top},
                               "26300.9974", personal)
        self.assertFalse([r["id"] for r in recs if "juniper" in str(r).casefold()])

    def test_an_unreadable_radio_caption_refuses_the_run(self):
        top = window(el("Text", "Visible to nearby devices", aid=oc.DISCOVERABLE_ID))
        with self.assertRaises(oc.core.Incomplete):
            oc.personal_strings(top, [])

    def test_a_control_painting_a_personal_name_is_blanked(self):
        top = window(el("Button", "Pat Home 5G properties", bounds=(-5100, 500, 300, 40)))
        rects = oc.blanks({"monitor": MON, "top": top}, ["Pat Home 5G"])
        self.assertIn((660, 500, 300, 40), rects)

    def test_a_personal_text_child_off_screen_does_not_spare_its_control(self):
        # The child is never masked (no placed bounds), so the control painting
        # the same caption must be (review, 2026-11-20).
        for child in (dict(el("Text", "Pat Home 5G"), offscreen=True), dict(el("Text", "Pat Home 5G"), bounds=None)):
            top = window(el("Button", "Pat Home 5G properties", bounds=(-5100, 500, 300, 40), kids=[child]))
            self.assertIn((660, 500, 300, 40), oc.blanks({"monitor": MON, "top": top}, ["Pat Home 5G"]))
        placed = el("Text", "Pat Home 5G", bounds=(-5090, 510, 120, 19))
        top = window(el("Button", "Pat Home 5G properties", bounds=(-5100, 500, 300, 40), kids=[placed]))
        rects = oc.blanks({"monitor": MON, "top": top}, ["Pat Home 5G"])
        self.assertIn((670, 510, 120, 19), rects)
        self.assertNotIn((660, 500, 300, 40), rects)

    def test_a_shot_must_be_the_one_saved_with_its_tree(self):
        # A replayed folder whose wifi.png was swapped for another page's would
        # be masked by the Wi-Fi tree (review, 2026-11-20).
        with tempfile.TemporaryDirectory() as d:
            wifi, camera = pathlib.Path(d) / "wifi.png", pathlib.Path(d) / "camera.png"
            wifi.write_bytes(b"wifi pixels")
            camera.write_bytes(b"camera pixels")
            dump = {"png": oc.capture.sha256(wifi)}
            self.assertTrue(oc.shot_is_bound(dump, wifi))
            self.assertFalse(oc.shot_is_bound(dump, camera))
            self.assertFalse(oc.shot_is_bound({}, wifi))                          # a run from before the digest
            self.assertFalse(oc.shot_is_bound(dump, pathlib.Path(d) / "gone.png"))
        self.assertIn("png = $png", oc.HOST_SCRIPT.read_text(encoding="utf-8"))   # the host writes it

    def test_a_scroll_that_does_not_move_stops_the_page(self):
        shell = shutil.which("powershell.exe")
        if not shell:
            self.skipTest("no Windows PowerShell on this host")
        script = subprocess.run(["wslpath", "-w", str(oc.HOST_SCRIPT)], capture_output=True, text=True, check=True).stdout.strip()
        ps = ("$a = [System.Management.Automation.Language.Parser]::ParseFile('%s', [ref]$null, [ref]$null); "
              "$f = $a.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Views' }, $true) | Select-Object -First 1; "
              "Invoke-Expression $f.Extent.Text; function Start-Sleep { }; function Snapshot { @{} }; function Scrolled { 0 }; $MaxViews = 20; "
              "$script:sets = 0; $p = [pscustomobject]@{ Current = [pscustomobject]@{ VerticallyScrollable = $true; VerticalScrollPercent = 40.0; VerticalViewSize = 50.0 } }; "
              "$p | Add-Member ScriptMethod SetScrollPercent { param($h, $v) $script:sets++ }; "
              "try { Views ([pscustomobject]@{ pattern = $p; element = $null; pane = $null }) | Out-Null; 'no error' } catch { 'error: ' + $_.Exception.Message }; 'sets=' + $script:sets") % script
        out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
        self.assertEqual([x.strip() for x in out.stdout.splitlines() if x.strip()],
                         ["error: the page did not scroll from 40 percent", "sets=1"], out.stderr)

    def test_a_page_too_long_for_the_steps_is_refused_before_it_is_read(self):
        shell = shutil.which("powershell.exe")
        if not shell:
            self.skipTest("no Windows PowerShell on this host")
        script = subprocess.run(["wslpath", "-w", str(oc.HOST_SCRIPT)], capture_output=True, text=True, check=True).stdout.strip()

        def run(view):
            # Each request moves the page as asked, so only the size decides.
            ps = ("$a = [System.Management.Automation.Language.Parser]::ParseFile('%s', [ref]$null, [ref]$null); "
                  "$f = $a.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Views' }, $true) | Select-Object -First 1; "
                  "Invoke-Expression $f.Extent.Text; function Start-Sleep { }; function Snapshot { @{} }; function Scrolled { 0 }; $MaxViews = 20; "
                  "$cur = [pscustomobject]@{ VerticallyScrollable = $true; VerticalScrollPercent = 0.0; VerticalViewSize = %s }; "
                  "$p = [pscustomobject]@{ Current = $cur }; $p | Add-Member ScriptMethod SetScrollPercent { param($h, $v) $this.Current.VerticalScrollPercent = [double]$v }; "
                  "try { $r = Views ([pscustomobject]@{ pattern = $p; element = $null; pane = $null }); 'views=' + $r.Count } catch { 'error: ' + $_.Exception.Message }") % (script, view)
            out = subprocess.run([shell, "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=120)
            return [x.strip() for x in out.stdout.splitlines() if x.strip()], out.stderr
        lines, err = run("8.0")       # 23 steps of 4.35 percent
        self.assertEqual(lines, ["error: the page needs 23 half-viewport steps to reach its end; 20 are left"], err)
        lines, err = run("9.0907")    # a page that ends on step 20
        self.assertEqual(lines, ["views=20"], err)

    def test_the_wifi_identity_is_read_after_the_theme_is_restored(self):
        # Its WLAN calls have no deadline: a stalled service must not hold the
        # capture theme on the operator's machine (review, 2026-11-20).
        lines = oc.HOST_SCRIPT.read_text(encoding="utf-8").splitlines()
        restore = next(i for i, x in enumerate(lines) if x.strip().startswith("Restore-Values $saved"))
        write = [i for i, x in enumerate(lines) if "Write-Identity $ssids" in x]
        self.assertEqual(len(write), 1)
        self.assertGreater(write[0], restore)
        self.assertIn("if ($captured) { Write-Identity $ssids }", lines[write[0]])

    def test_a_list_that_moved_while_the_page_was_copied_is_refused(self):
        def tree(y):
            entry = el("Group", "App", cls="NamedContainerAutomationPeer",
                       kids=[el("Text", "Pat's app", bounds=(-5090, y, 200, 19))])
            return window(el("List", aid="Apps", kids=[entry]))
        self.assertFalse(oc.moved({"monitor": MON, "before": tree(612), "top": tree(612)}, []))
        self.assertTrue(oc.moved({"monitor": MON, "before": tree(612), "top": tree(684)}, []))
        self.assertTrue(oc.moved({"monitor": MON, "top": tree(612)}, []))

    def test_the_leak_check_ignores_its_own_placeholder(self):
        names = ["Al", "WORKSTATION"]
        label = oc.scrub("Discoverable as “WORKSTATION”", names)
        rec = {"id": "win11.settings.hardware.bluetooth.discoverable-as-personal", "kind": "control", "label": label}
        self.assertEqual(oc.leaks([rec], names), [])
        kept = {"id": "win11.settings.hardware.bluetooth.al-s-phone", "kind": "control", "label": "Al's phone"}
        self.assertEqual(oc.leaks([kept], names), [kept["id"]])

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

    def test_an_entry_cut_by_the_viewport_without_a_placed_title_is_blanked_whole(self):
        entry = el("Group", "App", cls="NamedContainerAutomationPeer", bounds=(-5149, 1020, 1000, 12),
                   kids=[el("Text", "Pat's app", bounds=(0, 0, 0, 0))])
        entry["children"][0]["bounds"] = None
        rects = oc.blanks({"monitor": MON, "top": window(el("List", aid="Apps", kids=[entry]))}, [])
        self.assertIn((611, 1020, 1000, 12), rects)

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
