#!/usr/bin/env python3
"""Contract tests for section_slice.py -- the ONE section slicer shared by
section-manifest.py (SPLIT verdict) and section-pack.py (evidence bundle).

The bug these guard (found 2026-07-17 in the bare-metal-hardening TODO's
terminal section, recurred twice 2026-07-25 in kernel-resource-accounting-
quotas): both callers had private copies that terminated the slice only on
the next NUMBERED heading, so the LAST numbered section of every file sliced
to EOF and swallowed the file-level OS Comparison / Unit Tests / Verification
/ History blocks. Measured effect before the fix: 199 of 231 TODO files
over-counted their terminal section (3956 phantom items in total), and 28
files reported abi_impact=True purely from swallowed prose, which drives the
ABI arm of the SPLIT gate.

That made these DOCTRINE-gate corruption, not cosmetics: the sequencer is
required to split before implementing on a SPLIT-RECOMMENDED verdict.
"""
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from section_slice import section_block, SECTION_RE, SECTION_END_RE  # noqa: E402

REPO = HERE.parent.parent.parent

# A fixture shaped like a real TODO: numbered sections followed by the
# file-level trailing blocks that the bug used to swallow.
FIXTURE = "\n".join([
    "# TODO-99 Fixture",
    "",
    "## Inputs",
    "- src/kernel/preamble.c",
    "",
    "## 1. First section",
    "- [ ] one-alpha",
    "- [x] one-beta",
    "",
    "## 2. Terminal numbered section",
    "- [ ] two-alpha",
    "- [ ] two-beta",
    "### 2.1 Sub-heading belongs to section 2",
    "- [x] two-gamma",
    "",
    "## OS Comparison",
    "| feature | win11 |",
    "",
    "## Unit Tests",
    "- [ ] trailing-unit-test-item",
    "",
    "## Verification",
    "- [ ] trailing-verification-item",
    "The NTSTATUS and SSDT words here would forge abi_impact.",
    "",
    "## History",
    "- [x] trailing-history-item",
])


def _items(block):
    return [l.strip() for l in block.splitlines()
            if l.strip().startswith("- [")]


def test_terminal_section_excludes_trailing_unit_tests_block():
    """The regression the backlog item asks for by name."""
    b = section_block(FIXTURE, 2)
    assert "## Unit Tests" not in b, "terminal section swallowed Unit Tests"
    assert "trailing-unit-test-item" not in b, b
    assert "## OS Comparison" not in b, "terminal section swallowed OS Comparison"
    assert "## Verification" not in b, "terminal section swallowed Verification"
    assert "## History" not in b, "terminal section swallowed History"


def test_terminal_section_keeps_exactly_its_own_items():
    b = section_block(FIXTURE, 2)
    got = _items(b)
    assert len(got) == 3, f"expected 3 items in section 2, got {len(got)}: {got}"
    assert all("trailing-" not in g for g in got), got
    assert "- [ ] two-alpha" in got and "- [x] two-gamma" in got, got


def test_sub_headings_stay_inside_the_body():
    b = section_block(FIXTURE, 2)
    assert "### 2.1" in b, "a `### ` sub-heading must not terminate the slice"
    assert SECTION_END_RE.match("### 2.1 Sub") is None, \
        "SECTION_END_RE must not match a level-3 heading"


def test_non_terminal_section_unchanged():
    b = section_block(FIXTURE, 1)
    got = _items(b)
    assert len(got) == 2, got
    assert "two-alpha" not in b, "section 1 leaked into section 2"


def test_preamble_is_not_captured():
    b = section_block(FIXTURE, 1)
    assert "preamble.c" not in b, "preamble leaked into section 1"


def test_absent_section_returns_empty():
    assert section_block(FIXTURE, 77) == ""


def test_genuinely_final_section_still_reaches_eof():
    """When the last numbered section really IS the last block, slicing to
    EOF is correct and must not regress into truncation."""
    doc = "## 1. Only section\n- [ ] a\n- [ ] b\n"
    assert len(_items(section_block(doc, 1))) == 2


def test_unlisted_trailing_heading_terminates():
    """An allowlist of known trailing names (what scripts/lint.sh used) goes
    stale; a bare `## ` boundary does not."""
    doc = ("## 1. S\n- [ ] mine\n"
           "## Bare Metal Testing Plan\n- [ ] not-mine\n")
    b = section_block(doc, 1)
    assert "not-mine" not in b, b
    assert len(_items(b)) == 1, _items(b)


def test_abi_impact_not_forged_by_trailing_prose():
    """abi_impact drives the >=6-item ABI arm of the SPLIT gate, and the
    trailing Verification prose is full of ABI/SSDT/NTSTATUS words."""
    abi = re.compile(r"(?i)\b(ABI|NTSTATUS|SSDT|boot_info|struct offset|"
                     r"BOOT_INFO_VERSION|syscall number|PEB|TEB)\b")
    assert not abi.search(section_block(FIXTURE, 2)), \
        "trailing prose forged abi_impact on the terminal section"


def test_both_callers_use_the_shared_slicer():
    """Guards the oracle split-brain: neither caller may reintroduce a
    private section_block."""
    for name in ("section-manifest.py", "section-pack.py"):
        src = (REPO / "scripts" / "overnight" / name).read_text(encoding="utf-8")
        assert "from section_slice import" in src, f"{name} must import the shared slicer"
        assert not re.search(r"^def section_block", src, re.MULTILINE), \
            f"{name} reintroduced a private section_block (split-brain risk)"


def test_live_tree_terminal_sections_are_bounded():
    """End-to-end on the real todo/ tree: no terminal section may contain a
    file-level trailing heading."""
    todo_root = REPO / "todo"
    checked = 0
    for p in sorted(todo_root.rglob("TODO-*.md")):
        text = p.read_text(encoding="utf-8", errors="replace")
        nums = [int(m.group(1)) for m in re.finditer(r"^## (\d+)\.", text, re.M)]
        if not nums:
            continue
        checked += 1
        b = section_block(text, max(nums))
        for trailing in ("## Unit Tests", "## Verification", "## OS Comparison",
                         "## History"):
            assert trailing not in b, f"{p.name}: terminal section swallowed {trailing}"
    assert checked > 50, f"expected to check the live tree, only saw {checked} files"


if __name__ == "__main__":
    test_terminal_section_excludes_trailing_unit_tests_block()
    test_terminal_section_keeps_exactly_its_own_items()
    test_sub_headings_stay_inside_the_body()
    test_non_terminal_section_unchanged()
    test_preamble_is_not_captured()
    test_absent_section_returns_empty()
    test_genuinely_final_section_still_reaches_eof()
    test_unlisted_trailing_heading_terminates()
    test_abi_impact_not_forged_by_trailing_prose()
    test_both_callers_use_the_shared_slicer()
    test_live_tree_terminal_sections_are_bounded()
    print("PASS: section-slice")
