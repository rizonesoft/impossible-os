#!/usr/bin/env python3
"""A new park that waits on the operator is refused unless its section declares a Need."""
import importlib.util
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("tsc", HERE.parents[1] / "todo-staged-check.py")
tsc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tsc)


def found(body, needs=None):
    lines = ["## 1. Earlier", "", "**Needs:** operator", "", "## 2. Subject", ""]
    if needs:
        lines += [f"**Needs:** {needs}", ""]
    start = len(lines)
    lines += body + ["", "## 3. Later", "", "**Needs:** bare-metal"]
    added = [(start + i + 1, text) for i, text in enumerate(body)]
    return [text for _, text in tsc._parks_on_a_person(lines, lines, added)]


def test_a_nested_section_does_not_borrow_the_need_above_it():
    lines = ["## 1. Earlier", "", "**Needs:** operator", "", "- ## 2. Nested", "  - BLOCKED: awaiting the operator"]
    heads = list(lines)
    heads[4] = "## 2. Nested"  # what the shared projection shows for a heading inside a list item
    added = [(6, lines[5])]
    assert tsc._parks_on_a_person(lines, heads, added) == added
    assert tsc._parks_on_a_person(lines, lines, added) == []  # the control: physical lines alone miss it


PARKS = ["- [/] Pick the wire encoding: operator-gated ABI decision",
         "> **Deferred:** [H] awaiting-answer Q9 (`todo/answers.md`)",
         "  - BLOCKED: awaiting the operator",
         "> **Deferred:** [M] awaiting-operator design decision"]


def test_a_park_on_a_person_is_refused_without_a_need():
    assert found(PARKS) == PARKS  # the Needs lines of the sections around it do not count


def test_a_declared_need_allows_the_park():
    assert found(PARKS, "operator") == []
    assert found(PARKS, "bare-metal, attended") == []
    assert found(PARKS, "none") == PARKS


def test_only_park_lines_are_read():
    assert found(["- [ ] Decide the wire encoding and record it; this was operator-gated until 2026-11-11",
                  "> **Notes:** Reopened, was Deferred: awaiting-answer Q9",
                  "- [/] Parked on `05-storage-filesystems/TODO-04` section 6, which owns the flush",
                  "The operator decision of 2026-11-10 applies here."]) == []


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    for t in tests:
        t()
    print(f"PASS: a park names its need ({len(tests)} tests)")
