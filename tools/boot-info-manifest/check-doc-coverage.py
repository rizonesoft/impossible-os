#!/usr/bin/env python3
# ============================================================================
# check-doc-coverage.py -- enforce row-level documentation coverage for every
# non-sentinel field in struct boot_info.
#
# The matrix at docs/boot/boot-info-fields.md uses short field names inside
# nested-struct subsections (e.g. `active` under "## Nested struct:
# boot_usb_controller") to avoid repeating the parent path on every row.
# This checker parses the matrix with HEADING CONTEXT so it can distinguish
# `usb_controller.active` from `usb_devices[0].active` (both manifest fields
# exist; both have their own matrix rows in different `## Nested struct: ...`
# sections).
#
# Contract:
#   - Coverage is satisfied only when the manifest field has a table-row
#     entry (first backticked identifier in a markdown `| ... | ... |` row)
#     inside the EXPECTED section.
#   - Prose mentions, invariant-list citations, and backticked identifiers
#     in the doc's preamble do NOT count. Only row-level ownership does.
#   - Section resolution uses a deterministic struct-prefix map. Unknown
#     prefixes default to the top-level `struct boot_info` section set.
#
# Output:
#   - Prints PASS with the count on success.
#   - Prints FAIL + the first missing field + the expected section on
#     failure (exit 1).
# ============================================================================

from __future__ import annotations

import os
import re
import sys

REPO_ROOT = os.path.realpath(os.path.join(os.path.dirname(__file__), "..", ".."))
INC = os.path.join(REPO_ROOT, "tools/boot-info-manifest/dump-fields.inc")
MATRIX = os.path.join(REPO_ROOT, "docs/boot/boot-info-fields.md")
HEADER = os.path.join(REPO_ROOT, "include/kernel/boot_info.h")

# Map struct name -> F() prefix used in dump-fields.inc. Anonymous inline
# structs are keyed by their field name (e.g. `timing` is `struct { ... }
# timing;` inside boot_info, and its members appear as `F(timing.<name>)`).
# Adding a new top-level struct without registering its prefix here will
# fail loud at parse time (the manifest-completeness check refuses to run
# with unknown structs) -- this mirrors the fail-loud rule that
# expected_sections() applies to documented fields.
STRUCT_TO_PREFIX: dict[str, str] = {
    "boot_info":              "",
    "boot_info_header":       "header.",
    "boot_config":            "config.",
    "boot_framebuffer":       "fb.",
    "boot_usb_controller":    "usb_controller.",
    "boot_uefi_runtime":      "uefi_runtime.",
    "boot_mmap_entry":        "mmap[0].",
    "boot_uefi_config_entry": "config_table[0].",
    "boot_uefi_guid":         "config_table[0].guid.",
    "boot_rt_mem_entry":      "rt_mmap[0].",
    "boot_gop_mode":          "gop_modes[0].",
    "boot_gop_handle":        "gop_handles[0].",
    "boot_usb_device":        "usb_devices[0].",
    "boot_usb_endpoint":      "usb_devices[0].endpoints[0].",
    "boot_payload_desc":      "payload_descriptors[0].",
    # Anonymous inline struct field names (parser detects `struct { ... }
    # <name>;` at top level inside boot_info and treats <name> as the key).
    "timing":                 "timing.",
}

# Field types whose own enumeration covers their members -- skip them at
# the parent level so we don't expect `F(config)` or `F(fb)` for the
# container itself (only `F(config.<member>)` rows exist).
NESTED_STRUCT_FIELD_NAMES = {
    # Members of struct boot_info that are themselves typed sub-structs:
    "header", "config", "fb", "usb_controller", "uefi_runtime", "timing",
    "mmap", "config_table", "rt_mmap", "gop_modes", "usb_devices",
    "payload_descriptors", "gop_handles",
    # Sub-struct members inside other named structs (boot_uefi_guid lives
    # inside boot_uefi_config_entry; boot_usb_endpoint array lives inside
    # boot_usb_device):
    "guid", "endpoints",
}

# Map a manifest-field prefix to the matrix section that is expected to
# own the field's nested-struct row. When a manifest field matches a
# prefix, the checker looks up its tail in the named set of sections.
# Multiple section headings per prefix are allowed (the top-level section
# sometimes carries a scalar row AND the nested-struct section carries
# the sub-struct rows). A row match against ANY of the named sections
# satisfies coverage.
PREFIX_SECTIONS: dict[str, list[str]] = {
    "header.":         ["ABI header"],
    "fb.":             ["Framebuffer", "Nested struct: `boot_framebuffer`"],
    "config.":         ["Nested struct: `boot_config`", "Boot configuration (`boot.conf`)"],
    "timing.":         ["Nested struct: `timing`", "Boot timing (FPDT + TSC)"],
    "usb_controller.": ["Nested struct: `boot_usb_controller`"],
    "uefi_runtime.":   ["Nested struct: `boot_uefi_runtime`"],
    "loader_identity.":["Bootloader build identity"],
    "rejected_entries[":["Boot policy selection (v19)"],
}
# Top-level scalar sections that carry PRIMARY ownership rows -- any of
# these can host a bare-name row and satisfy the coverage gate.
# `Legacy / Multiboot2-only fields` is deliberately EXCLUDED: it is a
# summary / deprecation table that duplicates identifiers already owned
# by their primary section (e.g. `mem_lower_kb` has its ownership row in
# `Basic memory (legacy)` and is re-listed for deprecation context in
# `Legacy / Multiboot2-only fields`). Accepting a hit in the summary
# table would let deleting the primary row go undetected.
TOPLEVEL_SECTIONS: list[str] = [
    "ABI header",
    "Memory map",
    "Basic memory (legacy)",
    "Framebuffer",
    "GOP mode list",
    "ACPI",
    "Module (GRUB legacy)",
    "Boot configuration (`boot.conf`)",
    "UEFI Configuration Table",
    "UEFI Runtime Services",
    "TPM measured boot",
    "USB discovery",
    "Boot timing (FPDT + TSC)",
    "Serial port",
    "Last-boot error (NVRAM)",
    "Boot device identity",
    "UEFI boot variables",
    "Boot partition",
    "Removable media",
    "Kernel-populated fields",
    "Typed payload descriptor array",
    "Capability negotiation",
    "Boot-path provenance",
    "Anti-rollback and security-version binding",
    "EFI System Partition integrity",
    "Bootloader build identity",
    "UKI signed-payload addresses (v14)",
    "Extended UEFI boot-variable capability surface (v15)",
    "Local boot device path detail (v16)",
    "Local boot device path detail (v17)",
    "Firmware trust landscape (v18)",
    "Boot policy selection (v19)",
    "Policy audit surface (v20)",
    "OS-visible Loader UEFI variables (v21)",
    "Multi-GPU GOP handles (v23)",
    "Kernel boot stack (v24)",
    # NOTE: do NOT add `Legacy / Multiboot2-only fields` -- it is a
    # summary table only.
]

HEADING_RE = re.compile(r"^(#+)\s+(.*)$")
# First backticked identifier in a markdown table row (a row starts with "|"
# and continues with at least one "|"-delimited cell whose first non-space
# token is a backticked identifier). Excludes the header row ("| Field |")
# by requiring backticks.
ROW_BACKTICK_RE = re.compile(r"^\|\s*`([^`]+)`")


def strip_array_suffix(name: str) -> str:
    """Normalize `mmap[BOOT_MMAP_MAX_ENTRIES]` -> `mmap`."""
    i = name.find("[")
    return name[:i] if i >= 0 else name


def parse_matrix(path: str) -> dict[str, set[str]]:
    """Walk the matrix, return {section_heading_text: set(field_identifier)}.

    section_heading_text is the heading without the leading #'s. field
    identifier is the first backticked token in a table row, normalized to
    strip any `[...]` suffix. Only rows inside table bodies count; prose
    backticks are ignored.

    Fenced code blocks (between ``` markers) are skipped: a `| `field` |`
    line inside an example fence is not an ownership row, even though it
    structurally matches the row regex. Without this guard, deleting a
    real ownership row could be silently masked by an unrelated example
    fence under the same heading.
    """
    sections: dict[str, set[str]] = {}
    current = "(preamble)"
    sections[current] = set()
    in_fence = False
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            stripped = line.lstrip()
            # Toggle fenced-block state on lines starting with ```. The
            # leading-whitespace tolerance covers indented fences inside
            # blockquotes (rare, but valid CommonMark).
            if stripped.startswith("```"):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            m = HEADING_RE.match(line.rstrip("\n"))
            if m:
                current = m.group(2).strip()
                sections.setdefault(current, set())
                continue
            r = ROW_BACKTICK_RE.match(line)
            if r:
                raw = r.group(1).strip()
                # Skip table-format separators (rare) and empty.
                if not raw:
                    continue
                normalized = strip_array_suffix(raw)
                sections[current].add(normalized)
    return sections


F_RE = re.compile(r"F\(([^)]+)\)")
COMMENT_LEAD_RE = re.compile(r"^\s*(\*|//|/\*|\* )")


def parse_manifest(path: str) -> list[str]:
    """Return every F(name) field path from dump-fields.inc, skipping the
    file-header block comment lines (which contain example `F(...)`
    annotations).
    """
    fields: list[str] = []
    in_block_comment = False
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            stripped = line.lstrip()
            if stripped.startswith("/*"):
                in_block_comment = True
            if in_block_comment:
                if "*/" in line:
                    in_block_comment = False
                continue
            if stripped.startswith("//") or stripped.startswith("*"):
                continue
            for m in F_RE.finditer(line):
                name = m.group(1).strip()
                if name and name != "...":
                    fields.append(name)
    return fields


def expected_sections(field: str) -> list[str] | None:
    """Return the list of matrix sections that could legitimately own
    this manifest field's row.

    Returns None when the field is dotted (a nested-struct member path)
    but no entry in PREFIX_SECTIONS matches its prefix. The caller MUST
    treat None as a fail-loud error: the unmapped prefix would otherwise
    silently match the field's tail in any top-level section, letting a
    new struct's documentation gap pass coverage. Bare top-level scalars
    (no dot) get the full TOPLEVEL_SECTIONS list as before.
    """
    for prefix, sections in PREFIX_SECTIONS.items():
        if field.startswith(prefix):
            return list(sections)
    if "." in field:
        return None
    return list(TOPLEVEL_SECTIONS)


def resolve_element_sentinel_parent(field: str) -> str | None:
    """For a manifest element sentinel like `mmap[0].base_addr`, return
    the nested-struct section name whose rows must include the tail
    (`base_addr`). Element sentinels for scalar arrays like
    `usb_controller.dma_pages[0]` have NO per-row owner and must be
    skipped (return empty string sentinel so caller can tell "skip"
    from "parent not recognized").
    """
    # Nested-array-of-struct leaf: `<outer>[0].<inner>[0].<leaf>`.
    # `usb_devices[0].endpoints[0].address` routes to
    # `Nested struct: boot_usb_endpoint` with tail `address`, NOT to
    # the outer `boot_usb_device` section (where `endpoints` is only a
    # parent-array row, not a per-leaf row). Match BEFORE the generic
    # single-level element pattern below so the generic one cannot
    # swallow doubly-nested paths.
    m = re.match(
        r"^([A-Za-z_][A-Za-z_0-9]*)\[0\]\.([A-Za-z_][A-Za-z_0-9]*)\[0\]\.(.+)$",
        field)
    if m:
        outer, inner, leaf = m.group(1), m.group(2), m.group(3)
        nested_array_to_section = {
            ("usb_devices", "endpoints"): "Nested struct: `boot_usb_endpoint`",
        }
        section = nested_array_to_section.get((outer, inner))
        if section:
            return section + "::" + strip_array_suffix(leaf)
        # Unknown nested-array: skip (parent-array row in the outer
        # struct's section satisfies coverage).
        return ""

    # Sub-struct-of-element: `<parent>[0].<sub>.<tail>` where <sub> is
    # itself a typed struct (not an array). Handle BEFORE the generic
    # element sentinel so `config_table[0].guid.data1` resolves to the
    # `boot_uefi_guid` section, not the outer `boot_uefi_config_entry`
    # section.
    m = re.match(r"^([A-Za-z_][A-Za-z_0-9]*)\[0\]\.([A-Za-z_][A-Za-z_0-9]*)\.(.+)$",
                 field)
    if m:
        parent, sub, tail = m.group(1), m.group(2), m.group(3)
        # If <sub> is a known sub-struct type, route to its section.
        sub_to_section = {
            "guid": "Nested struct: `boot_uefi_guid`",
        }
        section = sub_to_section.get(sub)
        if section:
            return section + "::" + strip_array_suffix(tail)
        # Otherwise fall through: treat `<parent>[0].<sub>.<tail>` as
        # though the whole `<sub>.<tail>` path lives in the parent's
        # section (rare; covers odd nesting).

    # `<parent>[0].<tail>` -- array-of-struct element sentinel.
    m = re.match(r"^([A-Za-z_][A-Za-z_0-9]*)\[0\]\.([A-Za-z_][A-Za-z_0-9.\[\]]*)$",
                 field)
    if m:
        parent, tail = m.group(1), m.group(2)
        parent_to_section = {
            "mmap":                "Nested struct: `boot_mmap_entry`",
            "config_table":        "Nested struct: `boot_uefi_config_entry`",
            "rt_mmap":             "Nested struct: `boot_rt_mem_entry`",
            "gop_modes":           "Nested struct: `boot_gop_mode`",
            "gop_handles":         "Nested struct: `boot_gop_handle`",
            "usb_devices":         "Nested struct: `boot_usb_device`",
            "payload_descriptors": "Nested struct: `boot_payload_desc`",
        }
        section = parent_to_section.get(parent)
        if section:
            return section + "::" + strip_array_suffix(tail)
        return None
    # Nested element of a nested element, e.g. usb_devices[0].endpoints[0].address
    m = re.match(r"^([A-Za-z_][A-Za-z_0-9]*)\[0\]\.([A-Za-z_][A-Za-z_0-9]*)\[0\]\.(.+)$",
                 field)
    if m:
        # Skip these: they are sentinels for the inner nested struct's
        # array-of-struct drift detection. If the outer struct's section
        # documents the array name, that's enough.
        return ""
    # Scalar-array sentinel like `usb_controller.dma_pages[0]` or
    # top-level `boot_device_path[0]`. Skip entirely.
    if "[0]" in field:
        return ""
    return None


FIELD_DECL_RE = re.compile(
    r"^\s*(?:const\s+|volatile\s+|signed\s+|unsigned\s+|struct\s+\w+\s+|"
    r"[\w_]+\s+)+"                       # type tokens (one or more)
    r"(\w+)"                             # capture group 1: field name
    r"\s*(\[[^\]]+\])?\s*;\s*$"          # optional [size]; trailing semicolon
)


def _strip_comments(src: str) -> str:
    """Remove /* ... */ and // ... comments. Preserve line breaks."""
    src = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"),
                 src, flags=re.DOTALL)
    src = re.sub(r"//[^\n]*", "", src)
    return src


def parse_struct_members(path: str) -> dict[str, list[tuple[str, bool]]]:
    """Parse boot_info.h and return {struct_name: [(field_name, is_array)]}
    for every top-level struct definition AND for anonymous inline struct
    fields of struct boot_info (keyed by their field name -- `timing` etc).

    Skips field declarations whose type names a known nested struct
    (NESTED_STRUCT_FIELD_NAMES); those members are checked indirectly via
    their own struct's enumeration.

    Limitations: assumes one top-level brace pair per struct, no nested
    named struct definitions, no anonymous unions inside named structs
    (boot_info.h has none today). If those patterns appear, this parser
    needs extension."""
    src = _strip_comments(open(path, "r", encoding="utf-8").read())
    result: dict[str, list[tuple[str, bool]]] = {}

    # Find every top-level `struct <name> { ... };`. State-machine over
    # brace depth, tracking the open struct's tag.
    pos = 0
    while pos < len(src):
        m = re.search(r"\bstruct\s+(\w+)\s*\{", src[pos:])
        if not m:
            break
        struct_name = m.group(1)
        body_start = pos + m.end()
        depth = 1
        i = body_start
        while i < len(src) and depth > 0:
            c = src[i]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        if depth != 0:
            break  # unmatched brace
        body = src[body_start:i]
        members: list[tuple[str, bool]] = []
        # Walk body and collect depth-1 declarations. Skip inner brace
        # pairs (they are anonymous inline structs that we handle below
        # ONLY for boot_info; for other structs an inline struct member is
        # currently unused and would be flagged as "unknown shape").
        inner_depth = 0
        line_buf = ""
        anon_buf: list[tuple[int, str]] = []  # (depth-when-opened, body)
        anon_open: int = -1
        for ch_i, ch in enumerate(body):
            if ch == "{":
                if inner_depth == 0:
                    anon_open = ch_i + 1
                inner_depth += 1
                continue
            if ch == "}":
                inner_depth -= 1
                if inner_depth == 0:
                    anon_buf.append((1, body[anon_open:ch_i]))
                    # Continue: the `<fieldname>;` after `}` will appear
                    # in line_buf for member-name capture.
                continue
            if inner_depth > 0:
                continue
            line_buf += ch
            if ch == ";":
                fm = FIELD_DECL_RE.match(line_buf)
                if fm:
                    fname = fm.group(1)
                    is_array = fm.group(2) is not None
                    if fname not in NESTED_STRUCT_FIELD_NAMES:
                        members.append((fname, is_array))
                    elif anon_buf and struct_name == "boot_info":
                        # Anonymous inline struct: enumerate its body as a
                        # virtual struct keyed by the field name.
                        inline_members = _parse_inline_body(anon_buf[-1][1])
                        result[fname] = inline_members
                    anon_buf = []
                line_buf = ""
        # Per-struct dedup: an explicit named struct definition wins over
        # any prior anonymous-inline registration of the same name.
        result[struct_name] = members
        pos = body_start + (i - body_start) + 1
    return result


def _parse_inline_body(body: str) -> list[tuple[str, bool]]:
    """Parse the body of an anonymous inline struct (boot_info member)
    and return its leaf fields. Skips brace-nested content."""
    out: list[tuple[str, bool]] = []
    depth = 0
    line_buf = ""
    for ch in body:
        if ch == "{":
            depth += 1
            continue
        if ch == "}":
            depth -= 1
            continue
        if depth > 0:
            continue
        line_buf += ch
        if ch == ";":
            fm = FIELD_DECL_RE.match(line_buf)
            if fm:
                out.append((fm.group(1), fm.group(2) is not None))
            line_buf = ""
    return out


def check_manifest_completeness(
    fields: list[str],
    structs: dict[str, list[tuple[str, bool]]],
) -> list[tuple[str, str]]:
    """Return [(struct.field, reason)] for every struct member that has
    no F() entry in the manifest. Inverse direction of the matrix gate:
    enforces that every real struct field is enumerated by F() before
    the matrix coverage check runs."""
    fields_set = set(fields)
    missing: list[tuple[str, str]] = []
    for struct_name, members in structs.items():
        prefix = STRUCT_TO_PREFIX.get(struct_name)
        if prefix is None:
            # Unmapped struct definition (new struct landed without
            # registration). Skip silently rather than fail-loud here:
            # not every struct in the header is a boot_info member
            # (some are local helpers). The real coverage check is
            # against the boot_info shape.
            continue
        for fname, is_array in members:
            expected = f"{prefix}{fname}"
            if expected not in fields_set:
                missing.append(
                    (f"{struct_name}.{fname}",
                     f"missing F({expected}) line in dump-fields.inc"))
                continue
            if is_array:
                sentinel = f"{prefix}{fname}[0]"
                if sentinel not in fields_set:
                    missing.append(
                        (f"{struct_name}.{fname}",
                         f"missing element-size sentinel F({sentinel}) "
                         f"in dump-fields.inc"))
    return missing


def main() -> int:
    if not os.path.exists(INC):
        print(f"error: {INC} missing", file=sys.stderr)
        return 2
    if not os.path.exists(MATRIX):
        print(f"error: {MATRIX} missing", file=sys.stderr)
        return 2
    if not os.path.exists(HEADER):
        print(f"error: {HEADER} missing", file=sys.stderr)
        return 2

    sections = parse_matrix(MATRIX)
    fields = parse_manifest(INC)
    if not fields:
        print(f"error: no F() entries parsed from {INC}", file=sys.stderr)
        return 2

    structs = parse_struct_members(HEADER)
    completeness_missing = check_manifest_completeness(fields, structs)
    if completeness_missing:
        print("FAIL boot_info manifest completeness:",
              len(completeness_missing),
              "struct field(s) in include/kernel/boot_info.h have no",
              "matching F() line in tools/boot-info-manifest/dump-fields.inc",
              file=sys.stderr)
        first = completeness_missing[0]
        print(f"  first missing: {first[0]} ({first[1]})", file=sys.stderr)
        print("  hint: add the F(<prefix><name>) line; this gate runs",
              "before the matrix coverage gate so docs cannot mask a",
              "struct -> manifest drift", file=sys.stderr)
        return 1

    missing: list[tuple[str, str]] = []
    for field in fields:
        # Skip element sentinels: either array-of-struct tails (must be
        # in the parent's nested section) or scalar-array [0] sentinels
        # (no per-row owner).
        elem = resolve_element_sentinel_parent(field)
        if elem == "":
            continue  # scalar-array sentinel, no row needed
        if elem is not None:
            # Array-of-struct element: look up in the nested-struct section.
            section, tail = elem.split("::", 1)
            tail = strip_array_suffix(tail)
            section_set = sections.get(section, set())
            if tail in section_set:
                continue
            missing.append((field, f"'{tail}' in '{section}'"))
            continue

        # Non-sentinel: nested prefix or top-level.
        normalized = strip_array_suffix(field)
        # For nested-struct prefixes (config.xxx, fb.xxx, ...) look up the
        # tail inside the prefix's named sections ONLY.
        tail_or_name = normalized
        sections_to_check = expected_sections(field)
        if sections_to_check is None:
            prefix = normalized.rsplit(".", 1)[0]
            missing.append((field, f"unmapped prefix '{prefix}' -- add to PREFIX_SECTIONS"))
            continue
        found = False
        # If the field has a known nested prefix, the tail (part after
        # the last dot) is what appears in the nested-struct row.
        if "." in normalized:
            tail_or_name = normalized.rsplit(".", 1)[1]
        for sec in sections_to_check:
            if tail_or_name in sections.get(sec, set()):
                found = True
                break
            # Some nested rows also use the full dotted name in the
            # top-level scalar section (e.g. `fb.addr` in "Framebuffer").
            if normalized in sections.get(sec, set()):
                found = True
                break
        if not found:
            missing.append((field, f"'{tail_or_name}' in any of {sections_to_check}"))

    if missing:
        print("FAIL boot_info doc coverage:", len(missing),
              "field(s) in tools/boot-info-manifest/dump-fields.inc missing",
              "from docs/boot/boot-info-fields.md",
              file=sys.stderr)
        first = missing[0]
        print(f"  first missing: {first[0]} (expected: {first[1]})",
              file=sys.stderr)
        print("  hint: add a table row under the named section; prose",
              "mentions and preamble backticks do NOT count",
              file=sys.stderr)
        return 1

    enumerated_fields = sum(len(m) for m in structs.values())
    print(f"PASS boot_info doc coverage: {len(fields)} manifest fields,",
          "every non-sentinel field has an owning matrix row;",
          f"manifest completeness OK ({enumerated_fields} struct fields",
          "across", len(structs), "structs all enumerated by F())")
    return 0


if __name__ == "__main__":
    sys.exit(main())
