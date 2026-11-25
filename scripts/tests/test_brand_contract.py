#!/usr/bin/env python3
"""scripts/brand_contract.py: the icon and cursor contract Impossible Brand draws against.

Each test copies the contract's real inputs into a fixture tree, changes one thing,
and checks that the published contract follows it or that generation refuses it,
naming what is wrong. The validators are exercised with small synthetic files.
"""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("brand_contract", ROOT / "scripts" / "brand_contract.py")
bc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bc)

CURSORS = sorted(p for p in (ROOT / "resources" / "cursors").iterdir() if p.is_file() and p.name != "COPYING")


def make_tree(base: Path) -> Path:
    root = base / "repo"
    for rel in bc.INPUTS.values():
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(ROOT / rel, root / rel)
    shutil.copytree(ROOT / "resources" / "icons" / "src", root / "resources" / "icons" / "src")
    cursors = root / "resources" / "cursors"
    cursors.mkdir(parents=True)
    make = (ROOT / "Makefile").read_text()
    for src in bc._makefile_list(make, "CURSOR_SRCS"):
        shutil.copy(ROOT / src, root / src)
    return root


def edit(root: Path, rel: str, old: str, new: str, count: int = 1) -> None:
    path = root / rel
    text = path.read_text()
    assert old in text, (rel, old)
    path.write_text(text.replace(old, new, count))


def generate(root: Path) -> dict:
    return bc.generate(bc.Tree(root))


def refused(test: unittest.TestCase, root: Path, *needles: str) -> str:
    with test.assertRaises(bc.ContractError) as ctx:
        generate(root)
    message = str(ctx.exception)
    for needle in needles:
        test.assertIn(needle, message)
    return message


def entry(contract: dict, name: str, provider: str) -> dict:
    found = [i for i in contract["icons"] if i["name"] == name and i["provider"] == provider]
    assert len(found) == 1, (name, provider, found)
    return found[0]


def xcursor(frames: list, extra: bytes = b"", toc_type: int = 0xFFFD0002) -> bytes:
    """frames: [(nominal, width, height, xhot, yhot, chunk_type)]; pixels are zero."""
    header = 16 + 12 * len(frames)
    toc, body, pos = b"", b"", header
    for nominal, w, h, hx, hy, ctype in frames:
        toc += struct.pack("<III", toc_type, nominal, pos)
        chunk = struct.pack("<IIIIIIIII", 36, ctype, nominal, 1, w, h, hx, hy, 0) + b"\0" * (w * h * 4)
        body += chunk
        pos += len(chunk)
    return struct.pack("<IIII", 0x72756358, 16, 0x10000, len(frames)) + toc + body + extra


IMG = 0xFFFD0002


class ContractGeneration(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="brand-contract-"))
        self.root = make_tree(self.tmp)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def literal(self, name: str) -> None:
        (self.root / "src" / "apps").mkdir(parents=True, exist_ok=True)
        (self.root / "src" / "apps" / "fixture.c").write_text(f'void f(void) {{ icon_get_by_name("{name}"); }}\n')

    def test_two_runs_are_byte_identical_and_the_repository_contract_is_current(self):
        self.assertEqual(bc.dumps(generate(self.root)), bc.dumps(generate(self.root)))
        # The committed contract against the whole repository (every caller under src/ and user/),
        # not the fixture, which copies only the fixed inputs.
        self.assertEqual(bc.dumps(generate(ROOT)), (ROOT / bc.CONTRACT).read_text())

    def test_a_literal_request_turns_wired_once_registered(self):
        self.literal("printer")
        printer = entry(generate(self.root), "printer", "colour")
        self.assertEqual(printer["missing"], ["irespack", "kernel_table"])
        self.assertFalse(printer["wired"])
        shutil.copy(self.root / "resources/icons/src/computer.svg", self.root / "resources/icons/src/printer.svg")
        edit(self.root, "include/icon_store.h", "    ICON_CONTROL_PANEL,\n", "    ICON_CONTROL_PANEL,\n    ICON_PRINTER,\n")
        edit(self.root, "src/kernel/icon_store.c", '"control_panel"\n', '"control_panel", "printer"\n')
        edit(self.root, "tools/irespack.c", '"control_panel",\n', '"control_panel", "printer",\n')
        printer = entry(generate(self.root), "printer", "colour")
        self.assertTrue(printer["wired"])
        self.assertEqual(printer["missing"], [])

    def test_glyph_names_are_glyphs_and_the_settings_pair_stays_two_entries(self):
        contract = generate(self.root)
        self.assertEqual(entry(contract, "cut", "glyph")["sources"], ["kernel_table"])
        colour = entry(contract, "settings", "colour")
        self.assertEqual(colour["used_for"], "The Settings app")
        self.assertNotIn("used_for", entry(contract, "settings", "glyph"))
        edit(self.root, "include/icon_store.h", "    ICON_CONTROL_PANEL,\n", "    ICON_CONTROL_PANEL,\n    ICON_SETTINGS_APP,\n")
        edit(self.root, "src/kernel/icon_store.c", '"control_panel"\n', '"control_panel", "settings"\n')
        edit(self.root, "tools/irespack.c", '"control_panel",\n', '"control_panel", "settings",\n')
        colour = entry(generate(self.root), "settings", "colour")
        self.assertEqual(colour["missing"], ["lookup"])
        self.assertEqual(colour["lookup"], "glyph")

    def test_a_spec_only_row_is_a_colour_entry_from_spec(self):
        self.assertEqual(entry(generate(self.root), "folder_downloads", "colour")["sources"], ["spec"])

    def test_an_enum_slot_without_a_name_is_refused(self):
        edit(self.root, "include/icon_store.h", "    ICON_CONTROL_PANEL,\n", "    ICON_CONTROL_PANEL,\n    ICON_SCANNER,\n")
        refused(self, self.root, "ICON_SCANNER", "zero-filled")

    def test_a_name_repeated_within_one_half_is_refused_naming_both(self):
        edit(self.root, "include/icon_store.h", "    ICON_CONTROL_PANEL,\n", "    ICON_CONTROL_PANEL,\n    ICON_AGAIN,\n")
        edit(self.root, "src/kernel/icon_store.c", '"control_panel"\n', '"control_panel", "folder_open"\n')
        refused(self, self.root, "ICON_FOLDER_OPEN", "ICON_AGAIN")

    def test_a_glyph_without_a_codepoint_is_refused(self):
        edit(self.root, "include/icon_store.h", "    ICON_FULLSCREEN,\n", "    ICON_FULLSCREEN,\n    ICON_RULER,\n")
        edit(self.root, "src/kernel/icon_store.c", '"pin", "clipboard", "fullscreen",', '"pin", "clipboard", "fullscreen", "ruler",')
        refused(self, self.root, "60 codepoints for 61 glyph slots")

    def test_colour_names_longer_than_the_packer_keeps_are_refused(self):
        self.literal("n" * 63)
        generate(self.root)
        self.literal("n" * 64)
        refused(self, self.root, "64 bytes")

    def test_size_count_limits(self):
        sixteen = ", ".join(str(s) for s in range(1, 17))
        edit(self.root, "tools/irespack.c", "16, 24, 32, 48, 64, 72, 96, 128, 256", sixteen)
        self.assertEqual(len(generate(self.root)["sizes"]), 16)
        edit(self.root, "tools/irespack.c", sixteen, sixteen + ", 17")
        refused(self, self.root, "supported_sizes holds 17")

    def test_the_wire_format_caps_sizes_at_255(self):
        for rel, old in (("tools/irespack.c", "#define MAX_SIZES       16"), ("src/kernel/icon_store.c", "#define IRES_MAX_SIZES      16")):
            edit(self.root, rel, old, old.replace("16", "256"))
        edit(self.root, "tools/irespack.c", '"folder_closed", "folder_open",\n    "file_default", "exe_default",\n    "dll_default", "text_file",\n'
             '    "computer", "recycle_bin_empty", "recycle_bin_full",\n    "control_panel",\n', '"folder_closed",\n')
        sizes = ", ".join(str(s) for s in range(1, 256))
        edit(self.root, "tools/irespack.c", "16, 24, 32, 48, 64, 72, 96, 128, 256", sizes)
        self.assertEqual(generate(self.root)["limits"]["ires_sizes"], 255)
        edit(self.root, "tools/irespack.c", sizes, sizes + ", 256")
        refused(self, self.root, "1 to 255 fit")

    def test_zero_and_unsorted_sizes_are_refused(self):
        edit(self.root, "tools/irespack.c", "16, 24, 32,", "0, 24, 32,")
        refused(self, self.root, "supported_sizes")

    def test_more_accepted_names_than_max_icons_are_refused(self):
        names = ", ".join(f'"extra_{i}"' for i in range(55))
        edit(self.root, "tools/irespack.c", '"control_panel",\n', f'"control_panel", {names},\n')
        refused(self, self.root, "MAX_ICONS")

    def test_the_name_table_buffer_binds_before_the_offset_limit(self):
        edit(self.root, "tools/irespack.c", "#define MAX_NAME_LEN    64", "#define MAX_NAME_LEN    65")
        names = ", ".join(f'"{chr(97 + i // 26)}{chr(97 + i % 26)}' + "x" * 62 + '"' for i in range(64))
        text = (self.root / "tools/irespack.c").read_text()
        text = re.sub(r"(accepted_icons\[\]\s*=\s*\{).*?(NULL)", lambda m: f"{m.group(1)}\n    {names},\n    {m.group(2)}", text, flags=re.S)
        (self.root / "tools/irespack.c").write_text(text)
        refused(self, self.root, "4160 name-table bytes; 4096 fit")

    def test_a_full_pack_over_the_loader_limit_is_refused(self):
        edit(self.root, "tools/irespack.c", "96, 128, 256", "96, 128, 256, 1024, 2048")
        refused(self, self.root, "the loader accepts")

    def test_limits_are_read_from_their_sources(self):
        edit(self.root, "tools/irespack.c", "#define MAX_ICONS       64", "#define MAX_ICONS       70")
        edit(self.root, "tools/mkfs-ixfs.c", "#define IXFS_MAX_NAME        252", "#define IXFS_MAX_NAME        200")
        limits = generate(self.root)["limits"]
        self.assertEqual((limits["pack_icons"], limits["guest_name_bytes"]), (70, 199))

    def test_a_token_missing_from_one_theme_is_refused(self):
        tokens = json.loads((self.root / "docs/design/tokens.json").read_text())
        del tokens["color"]["themes"]["light"]["card_bg"]
        (self.root / "docs/design/tokens.json").write_text(json.dumps(tokens))
        refused(self, self.root, "'light'", "card_bg")

    def test_token_values_are_published_in_svg_order(self):
        values = generate(self.root)["tokens"]["values"]["card_bg"]
        self.assertEqual(values["dark"], "#FFFFFF0D")

    def test_cursor_shapes_must_map_to_installed_files(self):
        edit(self.root, "include/cursor.h", "    CURSOR_COUNT", "    CURSOR_ZOOM,\n    CURSOR_COUNT")
        refused(self, self.root, "CURSOR_ZOOM")

    def test_a_mapped_cursor_file_missing_from_cursor_srcs_is_refused(self):
        edit(self.root, "Makefile", " \\\n               resources/cursors/not-allowed", "")
        refused(self, self.root, "CURSOR_FORBIDDEN")

    def test_a_kernel_only_directory_change_is_refused(self):
        edit(self.root, "src/kernel/drivers/cursor.c", '"C:\\\\Impossible\\\\System\\\\Cursors\\\\text"',
             '"C:\\\\Impossible\\\\Cursors\\\\text"')
        refused(self, self.root, "CURSOR_TEXT", "CURSOR_GUEST_DIR")

    def cursor_src(self, name: str) -> None:
        shutil.copy(ROOT / "resources/cursors/default", self.root / "resources/cursors" / name)
        edit(self.root, "Makefile", "resources/cursors/not-allowed", f"resources/cursors/not-allowed resources/cursors/{name}")

    def test_case_only_cursor_names_collide(self):
        self.cursor_src("Pointer")
        refused(self, self.root, "Pointer", "same guest file")

    def test_a_backslash_cursor_name_is_refused(self):
        self.cursor_src("arrow\\\\left")
        refused(self, self.root, "one guest path component")

    def test_guest_name_length(self):
        self.cursor_src("c" * 251)
        generate(self.root)
        self.cursor_src("d" * 252)
        refused(self, self.root, "252-byte name")

    def test_a_shipped_icon_outside_the_profile_is_refused(self):
        edit(self.root, "resources/icons/src/computer.svg", "</svg>", "<mask id=\"m\"/></svg>")
        refused(self, self.root, "computer.svg", "mask")

    def test_the_packer_list_must_end_in_one_null(self):
        edit(self.root, "tools/irespack.c", '    "control_panel",\n    NULL', '    NULL,\n    "control_panel"')
        refused(self, self.root, "exactly one NULL")

    def test_a_changed_frame_selection_refuses_until_the_rule_follows(self):
        driver = "src/kernel/drivers/cursor.c"
        text = (self.root / driver).read_text()
        parse = text.index("static int xcur_parse(")
        head, tail = text[:parse], text[parse:]
        (self.root / driver).write_text(head + tail.replace("if (diff < best_diff)", "if (diff <= best_diff)", 1))
        refused(self, self.root, "xcur_parse() changed")
        # A change that keeps every comparison but alters the winner (stop at the first eligible frame).
        (self.root / driver).write_text(head + tail.replace("found_any = 1;", "found_any = 1;\n            break;", 1))
        refused(self, self.root, "xcur_parse() changed")
        (self.root / driver).write_text(text.replace("#define PIXELS_PER_CURSOR (XCUR_TARGET_SIZE * XCUR_TARGET_SIZE)",
                                                     "#define PIXELS_PER_CURSOR (32 * 32)"))
        refused(self, self.root, "cursor_init() changed")
        (self.root / driver).write_text(text.replace("#define BYTES_PER_CURSOR  (PIXELS_PER_CURSOR * 4)",
                                                     "#define BYTES_PER_CURSOR  (PIXELS_PER_CURSOR * 2)"))
        refused(self, self.root, "cursor_init() changed")

    def test_loader_and_packer_behaviour_is_pinned(self):
        store = (self.root / "src/kernel/icon_store.c").read_text()
        edit(self.root, "src/kernel/icon_store.c", "size > 32 * 1024 * 1024", "size >= 32 * 1024 * 1024")
        refused(self, self.root, "ires_load() changed")
        (self.root / "src/kernel/icon_store.c").write_text(store)
        edit(self.root, "tools/irespack.c", "#define SIZE_COUNT (sizeof(supported_sizes) / sizeof(supported_sizes[0]))",
             "#define SIZE_COUNT 8")
        refused(self, self.root, "SIZE_COUNT must be the length")

    def test_wire_constants_and_the_size_count_macro(self):
        edit(self.root, "tools/irespack.c", "#define IRES_VERSION    1", "#define IRES_VERSION    2")
        refused(self, self.root, "disagree on IRES_VERSION")
        edit(self.root, "tools/irespack.c", "#define IRES_VERSION    2", "#define IRES_VERSION    1")
        edit(self.root, "tools/irespack.c", "sizeof(supported_sizes[0]))", "sizeof(supported_sizes[0])) - 1")
        refused(self, self.root, "SIZE_COUNT must be the length")

    def test_forms_the_reader_does_not_model_are_refused(self):
        cases = [
            ("tools/irespack.c", "    uint32_t magic;         /* 0x53455249 = \"IRES\" */\n",
             "    uint32_t magic;         /* 0x53455249 = \"IRES\" */\n    uint16_t extra;\n", "define ires_header_t differently"),
            ("tools/irespack.c", "supported_sizes[] = {", "supported_sizes[10] = {", "supported_sizes[] must be declared"),
            ("Makefile", "CURSOR_GUEST_DIR := Impossible/System/Cursors\n",
             "CURSOR_GUEST_DIR := Impossible/System/Cursors\nCURSOR_SRCS += resources/cursors/COPYING\n", "CURSOR_SRCS must be assigned"),
        ]
        for rel, old, new, needle in cases:
            original = (self.root / rel).read_text()
            edit(self.root, rel, old, new)
            refused(self, self.root, needle)
            (self.root / rel).write_text(original)
        generate(self.root)

    def test_preprocessor_and_make_forms_are_refused(self):
        cases = [
            ("include/cursor.h", "#define CURSOR_MAX_SIZE  48", "#define CURSOR_MAX_SIZE  48\n#undef CURSOR_MAX_SIZE\n#define CURSOR_MAX_SIZE 16",
             "CURSOR_MAX_SIZE must be #defined exactly once"),
            ("include/cursor.h", "#define CURSOR_MAX_SIZE  48", "#ifdef SMALL\n#define CURSOR_MAX_SIZE  48\n#endif",
             "inside a conditional block"),
            ("Makefile", "## sysroot-cursors:", "sysroot-cursors: CURSOR_SRCS := resources/cursors/default\n## sysroot-cursors:",
             "CURSOR_SRCS must be assigned"),
            ("src/kernel/icon_store.c", "system_icon_t icon_get_by_name(const char *name)\n{",
             "system_icon_t icon_get_by_name(const char *name)\n{\n    (void)0;", "icon_get_by_name() changed"),
        ]
        for rel, old, new, needle in cases:
            original = (self.root / rel).read_text()
            edit(self.root, rel, old, new)
            refused(self, self.root, needle)
            (self.root / rel).write_text(original)
        generate(self.root)

    def test_arithmetic_is_limited_to_forms_c_and_python_agree_on(self):
        cases = [
            ("src/kernel/drivers/cursor.c", "#define XCUR_TARGET_SIZE 24", "#define XCUR_TARGET_SIZE 12 + 12",
             "fully parenthesised"),
            ("tools/irespack.c", "    16, 24, 32,", "    (16 + 0), 24, 32,", "plain decimal literal"),
            ("src/kernel/icon_store.c", "#define IRES_COLOR_COUNT    (ICON_COLOR_COUNT - ICON_MONO_COUNT)",
             "#define IRES_COLOR_COUNT    (ICON_COLOR_COUNT - ICON_MONO_COUNT - 1)", "IRES_COLOR_COUNT must be"),
            ("tools/irespack.c", "#define SIZE_COUNT (sizeof(supported_sizes) / sizeof(supported_sizes[0]))",
             "#if 0\n#define SIZE_COUNT (sizeof(supported_sizes) / sizeof(supported_sizes[0]))\n#endif", "SIZE_COUNT"),
            ("Makefile", "CURSOR_GUEST_DIR := Impossible/System/Cursors",
             "ifeq ($(NEVER),yes)\nCURSOR_GUEST_DIR := Impossible/System/Cursors\nendif", "inside a Make conditional"),
        ]
        for rel, old, new, needle in cases:
            original = (self.root / rel).read_text()
            edit(self.root, rel, old, new)
            refused(self, self.root, needle)
            (self.root / rel).write_text(original)
        generate(self.root)
        with self.assertRaises(bc.ContractError):
            bc.int_expr("(16 + (-1 / 2))", "test")

    def test_extents_widths_and_spellings_c_reads_differently(self):
        cases = [
            ("src/kernel/icon_store.c", "static uint16_t     ires_sizes[IRES_MAX_SIZES];", "static uint16_t     ires_sizes[1];", "ires_sizes"),
            ("tools/irespack.c", "    } sizes[MAX_SIZES];", "    } sizes[1];", "icon_data_t changed"),
            ("tools/irespack.c", "    16, 24, 32,", "    016, 24, 32,", "plain decimal literal"),
            ("src/kernel/drivers/cursor.c", "#define XCUR_MAX_FILE    (512 * 1024)",
             "#define XCUR_MAX_FILE    (0x10000 * 0x10000 + 524288)", "XCUR_MAX_FILE"),
            ("tools/irespack.c", '    "folder_closed", "folder_open",', '    // note \\\n    "folder_closed", "folder_open",',
             "line continuation"),
        ]
        for rel, old, new, needle in cases:
            original = (self.root / rel).read_text()
            edit(self.root, rel, old, new)
            refused(self, self.root, needle)
            (self.root / rel).write_text(original)
        for rel, old in (("tools/irespack.c", "#define IRES_VERSION    1"), ("src/kernel/icon_store.c", "#define IRES_VERSION        1")):
            edit(self.root, rel, old, old.replace("1", "65536"))
        refused(self, self.root, "IRES_VERSION does not fit")

    def test_shipped_sources_codepoints_and_conditional_calls(self):
        source = (self.root / "resources/icons/src/computer.svg").read_text()
        (self.root / "resources/icons/src/computer.svg").write_text(
            re.sub(r'fill="#[0-9A-Fa-f]+"', 'fill="var(--accent)"', source, count=1))
        refused(self, self.root, "computer.svg", "literal colours only")        # the OS converter renders it as written
        (self.root / "resources/icons/src/computer.svg").write_text(source)
        codepoints = (self.root / "include/generated/fluent_codepoints.h").read_text()
        edit(self.root, "include/generated/fluent_codepoints.h", "0xf33a,", "0x100000000,")
        refused(self, self.root, "fits its uint32_t slot")
        (self.root / "include/generated/fluent_codepoints.h").write_text(codepoints)
        (self.root / "src/apps").mkdir(parents=True, exist_ok=True)
        (self.root / "src/apps/disabled.c").write_text('#if 0\nvoid f(void) { icon_get_by_name("printer"); }\n#endif\n')
        refused(self, self.root, "conditional block")

    def test_a_padded_make_value_is_refused(self):
        edit(self.root, "Makefile", "CURSOR_GUEST_DIR := Impossible/System/Cursors\n",
             "CURSOR_GUEST_DIR := Impossible/System/Cursors  \n")
        refused(self, self.root, "CURSOR_GUEST_DIR must be one word")

    def test_missing_brand_artifacts_fail_the_gate_and_the_dispatch(self):
        (self.root / "scripts").mkdir(exist_ok=True)
        shutil.copy(ROOT / "scripts/brand_contract.py", self.root / "scripts/brand_contract.py")
        shutil.copy(ROOT / "scripts/brand_contract_gate.sh", self.root / "scripts/brand_contract_gate.sh")
        bc.main(["--root", str(self.root)])
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        subprocess.run(["git", "-C", str(self.root), "add", "-A"], check=True)
        self.assertIn("ERRORS=0", self.run_dispatch(staged=True))
        # Gate and checker kept, both data files dropped: the gate fails.
        subprocess.run(["git", "-C", str(self.root), "rm", "-q", "--cached", "docs/design/brand-contract.json",
                        "docs/design/svg-profile.json"], check=True)
        self.assertIn("ERRORS=1", self.run_dispatch(staged=True))
        # Only the gate kept (checker and data files gone): the gate itself fails (it used to pass).
        subprocess.run(["git", "-C", str(self.root), "rm", "-q", "--cached", "scripts/brand_contract.py"], check=True)
        out = self.run_dispatch(staged=True)
        self.assertIn("ERRORS=1", out)
        self.assertIn("lacks scripts/brand_contract.py", out)
        # Only the checker kept (no gate, no data files): the dispatch fails.
        subprocess.run(["git", "-C", str(self.root), "add", "scripts/brand_contract.py"], check=True)
        subprocess.run(["git", "-C", str(self.root), "rm", "-q", "--cached", "scripts/brand_contract_gate.sh"], check=True)
        out = self.run_dispatch(staged=True)
        self.assertIn("ERRORS=1", out)
        self.assertIn("not scripts/brand_contract_gate.sh", out)

    def test_the_fallback_theme_must_exist(self):
        tokens = json.loads((self.root / "docs/design/tokens.json").read_text())
        tokens["color"]["default_theme"] = "absent"
        (self.root / "docs/design/tokens.json").write_text(json.dumps(tokens))
        refused(self, self.root, "default_theme 'absent'")

    def test_literal_arguments_must_be_one_literal(self):
        (self.root / "src/apps").mkdir(parents=True, exist_ok=True)
        (self.root / "src/apps/split.c").write_text('void f(void) { icon_get_by_name("folder_" "closed"); }\n')
        refused(self, self.root, "exactly one plain string literal")
        for arg in ('("printer")', '"\\160rinter"'):
            (self.root / "src/apps/split.c").write_text(f"void f(void) {{ icon_get_by_name({arg}); }}\n")
            refused(self, self.root, "icon_get_by_name")
        (self.root / "src/apps/split.c").unlink()
        edit(self.root, "src/kernel/icon_store.c", "/* ---- Internal: load IRES file from VFS ---- */",
             'static void probe(void) { (void)icon_get_by_name("scanner"); }\n/* ---- Internal: load IRES file from VFS ---- */')
        self.assertEqual(entry(generate(self.root), "scanner", "colour")["sources"], ["literal"])

    def test_icon_enum_aliases_and_holes_are_refused(self):
        header = (self.root / "include/icon_store.h").read_text()
        edit(self.root, "include/icon_store.h", "    ICON_COPY,", "    ICON_COPY = ICON_CUT,")
        refused(self, self.root, "ICON_COPY", "shares a slot")
        (self.root / "include/icon_store.h").write_text(header.replace("    ICON_PASTE,", "    ICON_PASTE = 3,"))
        refused(self, self.root, "without a member")

    def test_crlf_sources_generate_the_same_contract(self):
        lf = bc.dumps(generate(self.root))
        for rel in ("Makefile", "src/kernel/icon_store.c", "tools/irespack.c", "include/cursor.h"):
            path = self.root / rel
            path.write_bytes(path.read_bytes().replace(b"\n", b"\r\n"))
        self.assertEqual(bc.dumps(generate(self.root)), lf)

    def test_concurrent_writers_each_publish_a_whole_file(self):
        (self.root / "docs/design").mkdir(parents=True, exist_ok=True)
        cmd = ["python3", str(ROOT / "scripts/brand_contract.py"), "--root", str(self.root)]
        procs = [subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, start_new_session=True)
                 for _ in range(4)]
        try:
            results = [(p.wait(timeout=300), p.stderr.read()) for p in procs]
        finally:
            for p in procs:
                if p.poll() is None:
                    os.killpg(p.pid, 9)
        self.assertEqual([rc for rc, _ in results], [0, 0, 0, 0], results)
        self.assertEqual(bc.main(["--root", str(self.root), "--check"]), 0)
        self.assertEqual(list((self.root / "docs/design").glob(".brand-contract-*")), [])
        published = self.root / bc.CONTRACT
        os.chmod(published, 0o644)
        bc.main(["--root", str(self.root)])
        self.assertEqual(published.stat().st_mode & 0o777, 0o644)          # regeneration keeps the mode

    def test_check_compares_bytes(self):
        (self.root / "docs/design").mkdir(parents=True, exist_ok=True)
        bc.main(["--root", str(self.root)])
        path = self.root / bc.CONTRACT
        data = json.loads(path.read_text())
        path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")          # same values, other formatting
        with _capture_stderr() as err:
            self.assertEqual(bc.main(["--root", str(self.root), "--check"]), 1)
        self.assertIn("differs from the generated bytes", err.getvalue())

    def run_dispatch(self, staged: bool) -> tuple:
        """Run lint.sh's own brand-contract dispatch block against the fixture repository."""
        lint = (ROOT / "scripts/lint.sh").read_text()
        block = lint[lint.index("# BEGIN brand-contract dispatch"):lint.index("# END brand-contract dispatch")]
        script = ('RED=""; NC=""; ERRORS=0; REPO_ROOT="$1"; LINT_GATE_SCOPE_STAGED="$2"\n'
                  + block + '\necho "ERRORS=$ERRORS"\n')
        proc = subprocess.run(["bash", "-c", script, "dispatch", str(self.root), "1" if staged else "0"],
                              capture_output=True, text=True, timeout=120)
        return proc.stdout + proc.stderr

    def test_lint_dispatch_judges_one_snapshot(self):
        (self.root / "scripts").mkdir(exist_ok=True)
        shutil.copy(ROOT / "scripts/brand_contract.py", self.root / "scripts/brand_contract.py")
        bc.main(["--root", str(self.root)])
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        subprocess.run(["git", "-C", str(self.root), "add", "-A"], check=True)
        self.assertIn("ERRORS=1", self.run_dispatch(staged=True))          # contract staged, no gate anywhere
        shutil.copy(ROOT / "scripts/brand_contract_gate.sh", self.root / "scripts/brand_contract_gate.sh")
        out = self.run_dispatch(staged=True)                               # gate only in the working tree
        self.assertIn("ERRORS=1", out)
        self.assertIn("not scripts/brand_contract_gate.sh", out)
        subprocess.run(["git", "-C", str(self.root), "add", "-A"], check=True)
        self.assertIn("ERRORS=0", self.run_dispatch(staged=True))
        edit(self.root, "tools/irespack.c", "96, 128, 256", "96, 128")      # a stale contract staged
        subprocess.run(["git", "-C", str(self.root), "add", "-A"], check=True)
        self.assertIn("ERRORS=1", self.run_dispatch(staged=True))

    def test_cursor_enum_aliases_and_gaps_are_refused(self):
        header = (self.root / "include/cursor.h").read_text()
        edit(self.root, "include/cursor.h", "CURSOR_HAND,", "CURSOR_HAND = CURSOR_ARROW,")
        refused(self, self.root, "CURSOR_HAND", "shares a slot")
        (self.root / "include/cursor.h").write_text(header.replace("CURSOR_HAND,", "CURSOR_HAND = 2,"))
        refused(self, self.root, "slot(s) [1]")

    def test_a_huge_declared_count_is_refused_without_building_it(self):
        import time
        start = time.monotonic()
        edit(self.root, "include/icon_store.h", "ICON_TOTAL_COUNT = ICON_COLOR_COUNT", "ICON_TOTAL_COUNT = 2000000000")
        refused(self, self.root, "system_icon_t")
        self.assertLess(time.monotonic() - start, 5)

    def test_a_missing_comma_in_the_name_table_is_refused(self):
        edit(self.root, "src/kernel/icon_store.c", '"pin", "clipboard"', '"pin" "clipboard"')
        refused(self, self.root, "missing comma")

    def test_a_required_source_must_be_a_direct_child(self):
        (self.root / "resources/icons/src/archive").mkdir()
        (self.root / "resources/icons/src/computer.svg").rename(self.root / "resources/icons/src/archive/computer.svg")
        refused(self, self.root, "'computer' has no source")

    def test_the_checker_runs_from_a_copy_against_the_index(self):
        bc.main(["--root", str(self.root)])
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        subprocess.run(["git", "-C", str(self.root), "add", "-A"], check=True)
        copy = self.tmp / "checker-copy.py"
        shutil.copy(ROOT / "scripts/brand_contract.py", copy)       # what Check 30 runs under the gate
        proc = subprocess.run(["python3", str(copy), "--root", str(self.root), "--check", "--staged"],
                              capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def gate(self, *scope):
        proc = subprocess.run(["bash", str(ROOT / "scripts/brand_contract_gate.sh"), str(self.root), *scope],
                              capture_output=True, text=True, timeout=120)
        return proc.returncode, proc.stdout + proc.stderr

    def test_the_gate_never_passes_by_absence(self):
        (self.root / "scripts").mkdir(exist_ok=True)
        shutil.copy(ROOT / "scripts/brand_contract.py", self.root / "scripts/brand_contract.py")
        bc.main(["--root", str(self.root)])
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        subprocess.run(["git", "-C", str(self.root), "add", "-A"], check=True)
        self.assertEqual(self.gate("--staged")[0], 0)
        (self.root / "scripts/brand_contract.py").unlink()                     # working copy gone, index intact
        self.assertEqual(self.gate("--staged")[0], 0)
        subprocess.run(["git", "-C", str(self.root), "rm", "-q", "--cached", "scripts/brand_contract.py"], check=True)
        rc, out = self.gate("--staged")                                        # contract staged, checker not
        self.assertEqual(rc, 1, out)
        self.assertIn("lacks scripts/brand_contract.py", out)
        edit(self.root, "tools/irespack.c", "96, 128, 256", "96, 128")
        shutil.copy(ROOT / "scripts/brand_contract.py", self.root / "scripts/brand_contract.py")
        subprocess.run(["git", "-C", str(self.root), "add", "-A"], check=True)
        self.assertEqual(self.gate("--staged")[0], 1)                          # stale contract staged

    def test_the_cursor_install_follows_the_guest_directory_variable(self):
        build = self.tmp / "build"
        def make(*extra):
            proc = subprocess.Popen(["make", "-s", "sysroot-cursors", f"BUILD_DIR={build}", *extra], cwd=ROOT,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
            try:
                out, err = proc.communicate(timeout=300)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, 9)
                raise
            self.assertEqual(proc.returncode, 0, (out + err).decode(errors="replace"))
        expected = sorted([f["name"] for f in generate(ROOT)["cursors"]["files"]] + [".stamp"])
        make()
        self.assertEqual(sorted(os.listdir(build / "sysroot/Impossible/System/Cursors")), expected)
        make("CURSOR_GUEST_DIR=Impossible/Cursors2")       # existing sysroot, old stamp present
        self.assertEqual(sorted(os.listdir(build / "sysroot/Impossible/Cursors2")), expected)

    def test_a_missing_input_names_its_path(self):
        (self.root / "tools/irespack.c").unlink()
        refused(self, self.root, "tools/irespack.c")

    def test_check_names_the_first_stale_path(self):
        (self.root / "docs/design").mkdir(parents=True, exist_ok=True)
        self.assertEqual(bc.main(["--root", str(self.root)]), 0)
        self.assertEqual(bc.main(["--root", str(self.root), "--check"]), 0)
        edit(self.root, "tools/irespack.c", "96, 128, 256", "96, 128")
        with _capture_stderr() as err:
            self.assertEqual(bc.main(["--root", str(self.root), "--check"]), 1)
        self.assertIn("stale at $.sizes", err.getvalue())

    def test_staged_ignores_an_unstaged_edit(self):
        bc.main(["--root", str(self.root)])
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        subprocess.run(["git", "-C", str(self.root), "add", "-A"], check=True)
        edit(self.root, "tools/irespack.c", "96, 128, 256", "96, 128")
        self.assertEqual(bc.main(["--root", str(self.root), "--check", "--staged"]), 0)
        with _capture_stderr():
            self.assertEqual(bc.main(["--root", str(self.root), "--check"]), 1)


class _capture_stderr:
    def __enter__(self):
        import io
        import sys
        self._old, self._buf = sys.stderr, io.StringIO()
        sys.stderr = self._buf
        return self._buf

    def __exit__(self, *exc):
        import sys
        sys.stderr = self._old
        return False


CONTRACT = json.loads((ROOT / bc.CONTRACT).read_text())
SVG_HEAD = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48" width="48" height="48">'


def svg(body: str, head: str = "") -> str:
    return head + SVG_HEAD + body + "</svg>"


class SvgProfile(unittest.TestCase):
    def ok(self, text):
        bc.validate_svg(text, CONTRACT)

    def bad(self, text, needle):
        with self.assertRaises(bc.ContractError) as ctx:
            bc.validate_svg(text, CONTRACT)
        self.assertIn(needle, str(ctx.exception))

    def test_local_references_reuse_and_clips_are_accepted(self):
        self.ok(svg('<defs><symbol id="a"><path d="M0 0L4 4"/></symbol><clipPath id="c"><rect width="4" height="4"/></clipPath>'
                    '<linearGradient id="g"><stop offset="0" stop-color="#fff"/><stop offset="1" stop-color="var(--accent)"/></linearGradient></defs>'
                    '<use href="#a"/><use href="#a"/><g clip-path="url(#c)"><rect width="4" height="4" fill="url(#g)"/></g>'
                    '<rect width="1" height="1" fill="var(--card_bg)"/>'))

    def test_references_and_paint_are_checked(self):
        self.bad(svg('<use href="other.svg#a"/>'), "same-document")
        self.bad(svg('<g clip-path="red"/>'), "clip-path")
        self.bad(svg('<defs><linearGradient id="g"/></defs><g clip-path="url(#g)"/>'), "not clipPath")
        self.bad(svg('<rect fill="url(#nope)"/>'), "does not resolve")
        self.bad(svg('<rect fill="var(--no_such_token)"/>'), "unknown token")
        self.bad(svg('<defs><linearGradient id="g"/></defs><use href="#g"/>'), "resolves to <linearGradient>")
        self.bad(svg('<defs><linearGradient id="g" href="#p"/></defs><path id="p" d="M0 0"/>'), "'href' is outside")

    def test_duplicate_ids_are_refused_in_either_order(self):
        self.bad(svg('<defs><linearGradient id="x"/></defs><path id="x" d="M0 0"/>'), "duplicate id 'x'")
        self.bad(svg('<path id="x" d="M0 0"/><defs><linearGradient id="x"/></defs>'), "duplicate id 'x'")

    def test_cycles_through_use_and_clip_are_refused(self):
        self.bad(svg('<g id="a"><use href="#a"/></g>'), "cycle")
        self.bad(svg('<defs><clipPath id="c"><use href="#g"/></clipPath></defs><g id="g" clip-path="url(#c)"><path d="M0 0"/></g>'), "cycle")
        self.ok(svg('<defs><clipPath id="c"><rect width="2" height="2"/></clipPath></defs>'
                    '<g clip-path="url(#c)"/><g clip-path="url(#c)"/>'))

    def test_use_depth(self):
        chain = '<defs><path id="u0" d="M0 0"/>' + "".join(f'<g id="u{i}"><use href="#u{i - 1}"/></g>' for i in range(1, 9)) + "</defs>"
        self.ok(svg(chain + '<use href="#u7"/>'))
        self.bad(svg(chain + '<use href="#u8"/>'), "nest deeper than 8")

    def test_path_commands_count_implicit_repetitions(self):
        self.assertEqual(bc.path_commands("M0 0 L1 1 L2 2"), bc.path_commands("M0 0 1 1 2 2"))
        self.assertEqual(bc.path_commands("M0 0a1 1 0 01 1 1"), 2)
        explicit = "M0 0" + " L1 1" * 65535
        implicit = "M0 0" + " 1 1" * 65535
        self.ok(svg(f'<path d="{explicit}"/>'))
        self.ok(svg(f'<path d="{implicit}"/>'))
        self.bad(svg(f'<path d="{explicit} L2 2"/>'), "65536 geometry commands")
        self.bad(svg(f'<path d="{implicit} 2 2"/>'), "65536 geometry commands")
        points = " ".join(["1,1"] * 65536)
        self.ok(svg(f'<polyline points="{points}"/>'))
        self.bad(svg(f'<polyline points="{points} 2,2"/>'), "geometry commands")

    def test_use_instances_and_clip_applications_are_charged(self):
        clip = "M0 0" + " L1 1" * 1023

        def groups(n):
            return '<g clip-path="url(#c)"/>' * n
        head = f'<defs><clipPath id="c"><path d="{clip}"/></clipPath></defs>'
        self.ok(svg(head + groups(64)))
        self.bad(svg(head + groups(65)), "geometry commands")
        sym = f'<defs><symbol id="s"><path d="{clip}"/></symbol></defs>'
        self.bad(svg(sym + '<use href="#s"/>' * 65), "geometry commands")

    def test_the_prolog_admits_only_an_xml_declaration(self):
        self.ok('<?xml version="1.0" encoding="UTF-8"?>' + svg('<rect fill="#f00"/>'))
        self.bad('<?xml version="1.0" encoding="UTF-16"?>' + svg('<rect fill="#f00"/>'), "encoding 'UTF-16'")
        self.bad('<?xml-stylesheet type="text/css" href="data:text/css,rect{fill:green}"?>' + svg('<rect fill="#f00"/>'), "prolog")
        self.bad('<!DOCTYPE svg [<!ENTITY x "y">]>' + svg('<rect/>'), "prolog")

    def test_referenced_gradients_and_their_stops_are_charged(self):
        def doc(stops):
            return svg('<defs><linearGradient id="g">' + '<stop offset="0" stop-color="#fff"/>' * stops +
                       '</linearGradient></defs><rect width="4" height="4" fill="url(#g)"/>')
        self.ok(doc(4093))                     # svg + rect + gradient + 4093 stops = 4096
        self.bad(doc(4094), "4096 elements")

    def test_inherited_gradient_paint_is_charged_per_shape(self):
        grad = '<defs><linearGradient id="g">' + '<stop offset="0" stop-color="#fff"/>' * 100 + '</linearGradient></defs>'
        rects = '<rect width="1" height="1"/>' * 40
        self.ok(svg(grad + f'<g fill="url(#g)">{rects}</g>'))                    # svg + g + 40 * (rect + gradient + 100 stops) = 4082
        self.bad(svg(grad + f'<g fill="url(#g)">{rects}<rect/></g>'), "4096 elements")   # 4184
        self.ok(svg(grad + f'<g fill="url(#g)">{rects * 3}</g>'.replace("<rect ", '<rect fill="#000" ')))  # overridden

    def test_cdata_is_refused_and_comments_are_never_resolved(self):
        self.bad(svg('<![CDATA[ fill="var(--unknown)" ]]><rect/>'), "CDATA")
        # A comment cannot hold `--` (XML), so the attribute-shaped text is the part to keep untouched.
        text = svg('<!-- sample: fill="#fff" stroke="none" --><rect fill="var(--accent)"/>')
        self.ok(text)
        out = bc.resolve_paint(text, CONTRACT)
        self.assertIn('<!-- sample: fill="#fff" stroke="none" -->', out)
        self.assertIn('fill="#60CDFF"', out)

    def test_resolve_paint_leaves_other_attribute_values_alone(self):
        text = svg("""<g id="fill='var(--accent)'"><rect fill="var(--accent)"/></g>""")
        out = bc.resolve_paint(text, CONTRACT)
        self.assertIn("""id="fill='var(--accent)'\"""", out)
        self.assertIn('fill="#60CDFF"', out)

    def test_geometry_scanners_are_linear_and_positional(self):
        import time
        start = time.monotonic()
        self.bad(svg('<polygon points="' + "1" * 5000 + 'x"/>'), "points must")
        self.bad(svg('<polygon points="' + "1 " * 20000 + '1,"/>'), "points must")
        self.assertLess(time.monotonic() - start, 5)
        for d in ("M,0,,0", "M0,,0", "M0 0,", "M0 0, L1 1"):
            self.bad(svg(f'<path d="{d}"/>'), "comma")
        self.ok(svg('<path d="M0,0 1,1L2-2"/>'))
        self.bad(svg('<defs><path d="L1 1"/></defs>'), "moveto")       # unused definitions are checked too

    def test_long_clip_chains_do_not_exhaust_the_stack(self):
        n = 1500
        clips = [f'<clipPath id="c{i}"><rect width="1" height="1" clip-path="url(#c{i + 1})"/></clipPath>' for i in range(n)]
        clips.append(f'<clipPath id="c{n}"><rect width="1" height="1"/></clipPath>')
        for order in (clips, list(reversed(clips))):
            self.ok(svg("<defs>" + "".join(order) + '</defs><g clip-path="url(#c0)"/>'))

    def test_raw_scanners_cover_every_start_tag(self):
        self.bad(svg('<rect xmlns:a·b="urn:x" fill="var(--accent)"/>'), "ASCII")
        self.bad(svg('<path d="M٠ ٠"/>'), "path data")
        self.bad(svg('<polygon points="1.2.3 4"/>'), "points must")
        self.ok(svg('<!-- example: <example:shape/> <![CDATA[ x ]]> --><rect width="1" height="1"/>'))

    def test_hostile_svg_stays_linear(self):
        import time
        start = time.monotonic()
        body = '<defs><g id="s">' + '<rect width="1" height="1"/>' * 50 + "</g></defs>"
        uses = "".join(f'<use href="#s" fill="#{i:06x}"/>' for i in range(78))        # 78 distinct fills: 1 + 78 * 52 = 4057
        self.ok(svg(body + uses))
        self.bad(svg(body + uses + '<use href="#s" fill="#123456"/>'), "4096 elements")
        self.bad("<!--" * 20000 + svg("<rect/>"), "unterminated")
        self.assertLess(time.monotonic() - start, 5)
        # Malformed markup: 40,000 `<` in a row is rejected in well under a second (measured: the
        # linear scan takes under 1 ms; the earlier quadratic one took about 3 s here).
        begin = time.monotonic()
        with self.assertRaises(bc.ContractError):
            bc.validate_svg(svg("<" * 40000), CONTRACT)
        self.assertLess(time.monotonic() - begin, 1.0)

    def test_prefixed_elements_are_refused(self):
        self.bad(svg('<x:rect xmlns:x="http://www.w3.org/2000/svg" fill="var(--accent)"/>'), "namespace prefixes")

    def test_geometry_must_parse_completely(self):
        self.bad(svg('<path d="L 1 1"/>'), "moveto")
        self.bad(svg('<polygon points="foo 1,2 bar 3,4"/>'), "points must be numbers")
        self.ok(svg('<polygon points="1,2 3,4 5 6"/>'))

    def test_entity_checks_read_only_real_paint_values(self):
        self.ok(svg('<!-- sample fill="&#35;fff" --><rect fill="#fff"/>'))
        self.ok(svg('<rect id="fill&#61;x" fill="#fff"/>'))
        self.bad(svg('<rect fill="&#35;fff"/>'), "character and entity references")

    def test_padded_paint_is_refused(self):
        self.bad(svg('<rect fill=" var(--accent) "/>'), "no leading or trailing spaces")

    def test_geometry_costs_come_from_the_profile(self):
        rects = svg('<rect width="1" height="1"/>' * 3000)
        light = json.loads(json.dumps(CONTRACT))
        light["profile"]["limits"]["elements"] = 100000          # only the command budget is in play
        bc.validate_svg(rects, light)                            # 3000 * 5 = 15000 commands
        heavy = json.loads(json.dumps(light))
        heavy["profile"]["geometry_cost"]["rect"] = 25
        with self.assertRaises(bc.ContractError):
            bc.validate_svg(rects, heavy)                        # 3000 * 25 = 75000 > 65536
        bad = json.loads(json.dumps(CONTRACT["profile"]))
        bad["geometry_cost"]["path"] = "explicit letters only"
        with self.assertRaises(bc.ContractError):
            bc.check_geometry_costs(bad)

    def test_both_href_spellings_together_are_refused(self):
        self.bad(svg('<defs><symbol id="a"><path d="M0 0"/></symbol></defs>'
                     '<use xmlns:xlink="http://www.w3.org/1999/xlink" href="#a" xlink:href="other.svg#a"/>'), "both href")

    def test_the_root_must_be_svg(self):
        self.bad('<rect xmlns="http://www.w3.org/2000/svg" width="4" height="4"/>', "not <svg>")

    def test_references_in_paint_values_are_refused(self):
        self.bad(svg('<rect fill="var(--&#97;ccent)"/>'), "character and entity references")
        self.bad(svg('<rect fill="&#35;fff"/>'), "character and entity references")

    def test_forbidden_elements_and_attributes(self):
        self.bad(svg('<text>hi</text>'), "<text>")
        self.bad(svg('<rect style="fill:red"/>'), "'style'")

    def test_resolve_paint_uses_svg_order(self):
        out = bc.resolve_paint(svg('<rect width="4" height="4" fill="var(--card_bg)"/>'), CONTRACT)
        self.assertIn('fill="#FFFFFF0D"', out)

    def test_resolve_paint_touches_only_paint_values(self):
        text = svg('<!-- var(--no_such_token) --><path id="var(--accent)" d="M0 0"/><rect fill="var(--accent)"/>'
                   '<defs><linearGradient id="g"><stop offset="0" stop-color="var(--accent)"/></linearGradient></defs>')
        out = bc.resolve_paint(text, CONTRACT)
        self.assertIn('id="var(--accent)"', out)
        self.assertIn("<!-- var(--no_such_token) -->", out)
        self.assertIn('fill="#60CDFF"', out)
        self.assertIn('stop-color="#60CDFF"', out)


class Xcursor(unittest.TestCase):
    rules = CONTRACT["cursors"]["rules"]

    def bad(self, data, needle, rules=None):
        with self.assertRaises(bc.ContractError) as ctx:
            bc.validate_xcursor(data, rules or self.rules)
        self.assertIn(needle, str(ctx.exception))

    def test_every_shipped_cursor_passes(self):
        for path in CURSORS:
            if path.name in {f["name"] for f in CONTRACT["cursors"]["files"]}:
                bc.validate_xcursor(path.read_bytes(), self.rules)

    def test_selection_mirrors_the_current_loader(self):
        self.assertEqual(bc.validate_xcursor(xcursor([(24, 24, 24, 1, 1, IMG)]), self.rules)["width"], 24)
        self.bad(xcursor([(32, 32, 32, 1, 1, IMG)]), "does not fit")
        self.bad(xcursor([(24, 32, 32, 1, 1, IMG), (32, 24, 24, 1, 1, IMG)]), "does not fit")
        first = bc.validate_xcursor(xcursor([(24, 20, 20, 1, 1, IMG), (24, 24, 24, 1, 1, IMG)]), self.rules)
        self.assertEqual(first["width"], 20)
        self.bad(xcursor([(24, 257, 1, 0, 0, IMG)]), "per axis")
        self.bad(xcursor([(10023, 24, 24, 1, 1, IMG)]), "selects no frame")
        self.bad(xcursor([(24, 24, 24, 1, 1, IMG), (0x80000010, 24, 24, 1, 1, IMG)]), "signed")
        self.bad(xcursor([(24, 24, 24, 1, 1, 0x12345)]), "not an image")

    def test_hotspots_and_all_frame_rules(self):
        self.bad(xcursor([(24, 24, 24, 24, 1, IMG)]), "hotspot")
        self.bad(xcursor([(24, 24, 24, 1, 1, IMG), (48, 48, 48, 60, 1, IMG)]), "hotspot")
        self.bad(xcursor([(24, 24, 24, 1, 1, IMG), (512, 512, 512, 1, 1, IMG)]), "per axis")

    def test_the_selected_frame_must_lie_in_the_read_prefix(self):
        data = xcursor([(48, 48, 48, 1, 1, IMG), (24, 24, 24, 1, 1, IMG)])
        self.bad(data, "the bytes the loader reads", dict(self.rules, read_prefix_bytes=len(data) - 100))
        self.bad(data, "the bytes the loader reads", dict(self.rules, read_prefix_bytes=len(data) - 24 * 24 * 4 - 20))

    def test_both_kernel_buffers_bound_the_frame(self):
        rules = dict(self.rules, target_size=64, frame_pixels=min(64 * 64, 48 * 48), save_under=48)
        self.bad(xcursor([(64, 64, 64, 1, 1, IMG)]), "does not fit", rules)


if __name__ == "__main__":
    unittest.main()
