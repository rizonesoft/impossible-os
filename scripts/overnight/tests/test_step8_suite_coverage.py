#!/usr/bin/env python3
"""The commit gate's step-8 rule finds a kernel source's tests by what they call.

It bound `foo.c` to a test named `test_foo*.c`; suites are registered per subsystem, so
23 kernel section commits that staged their test opted out of it. Each case runs the
gate on a scratch repository's index.
"""
from __future__ import annotations

import contextlib
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import time

HOOKS = pathlib.Path(__file__).resolve().parents[3] / ".claude/hooks"
sys.path.insert(0, str(HOOKS))
import section_commit_gate as gate  # noqa: E402

DPC_C = "int dpc_queue(int x) {\n    return x;\n}\n\nstatic int helper(void) { return 0; }\n"
SCHED_TEST = '#include "x.h"\nstatic void t(void) { dpc_queue(1); }\n'
OTHER_TEST = "static void t(void) { /* dpc_queue is named only here */ heap_alloc(1); }\n"


def _repo(d: str) -> pathlib.Path:
    repo = pathlib.Path(d)
    for args in (["init", "-q", "-b", "main"], ["config", "user.email", "t@x"],
                 ["config", "user.name", "t"]):
        subprocess.run(["git", *args], cwd=d, check=True, timeout=30)
    (repo / ".claude/state").mkdir(parents=True)
    (repo / ".claude/state/skill-progress.json").write_text(json.dumps(
        {"implement-todo-section": {"started_ts": time.time_ns(), "args": "t"}}))
    return repo


def _stage(repo: pathlib.Path, files: dict[str, str]) -> list[str]:
    for path, text in files.items():
        (repo / path).parent.mkdir(parents=True, exist_ok=True)
        (repo / path).write_text(text)
    subprocess.run(["git", "add", *files], cwd=str(repo), check=True, timeout=30)
    return list(files)


def _check(files: dict[str, str], refuse: bool = True, todo: dict | None = None):
    """(ok, message, stderr) for a commit staging `files`."""
    saved = getattr(gate, "STEP8_REFUSE_FROM", None)
    gate.STEP8_REFUSE_FROM = "2000-01-01" if refuse else "2999-01-01"
    err = io.StringIO()
    try:
        with tempfile.TemporaryDirectory() as d:
            repo = _repo(d)
            staged = _stage(repo, files)
            todos = list(_stage(repo, todo)) if todo else []
            with contextlib.redirect_stderr(err):
                ok, msg = gate._step8_test_wiring_check(repo, staged, todos)
    finally:
        gate.STEP8_REFUSE_FROM = saved
    return ok, msg, err.getvalue()


def test_a_source_is_covered_by_the_suite_that_calls_it():
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C,
                         "src/kernel/test/test_sched.c": SCHED_TEST})
    assert ok, msg


def test_refusal_a_source_no_staged_test_calls():
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C,
                         "src/kernel/test/test_heap.c": OTHER_TEST})
    assert not ok and "dpc.c" in msg, msg
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C})
    assert not ok, "a test at HEAD is not this commit's test"


def test_refusal_every_source_needs_its_test_not_one_of_them():
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C,
                         "src/kernel/sched/apc.c": "int apc_queue(void) {\n    return 0;\n}\n",
                         "src/kernel/test/test_dpc.c": SCHED_TEST})
    assert not ok and "apc.c" in msg and "dpc.c" not in msg, msg


def test_a_test_only_commit_needs_nothing():
    ok, msg, _ = _check({"src/kernel/test/test_sched.c": SCHED_TEST})
    assert ok, msg


def test_refusal_code_no_test_can_call_needs_a_tested_by_line():
    flat = "bits 16\nstart:\n    cli\n"
    ok, msg, _ = _check({"src/kernel/smp/ap_trampoline.asm": flat,
                         "src/kernel/test/test_heap.c": OTHER_TEST})
    assert not ok, "an unrelated staged test does not cover a flat binary"
    ok, msg, _ = _check({"src/kernel/smp/ap_trampoline.asm": "; tested-by: test_smp.c\n" + flat,
                         "src/kernel/test/test_smp.c": "static void t(void) {}\n"})
    assert ok, msg


def test_upstream_acpica_is_not_ours_but_its_local_additions_are():
    prov = "# x\n\n## Local additions (NOT upstream)\n\n- `src/kernel/acpica/include/platform/acimpossible.h` -- ours\n\n## Other\n"
    ok, msg, _ = _check({"src/libs/PROVENANCE.md": prov,
                         "src/kernel/acpica/components/tables/tbxface.c": "int AcpiX(void) {\n}\n"})
    assert ok, msg
    ok, msg, _ = _check({"src/libs/PROVENANCE.md": prov,
                         "src/kernel/acpica/include/platform/acimpossible.h": "#define ACPI_SINGLE_THREADED\n"})
    assert not ok and "acimpossible.h" in msg, msg


def test_before_the_switch_date_it_warns_and_lets_the_commit_through():
    ok, msg, err = _check({"src/kernel/sched/dpc.c": DPC_C}, refuse=False)
    assert ok and "WARN (step-8" in err and "dpc.c" in err, err


def test_the_no_test_surface_note_still_exempts():
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C},
                        todo={"todo/TODO-01-x.md": "**Note:** No kernel test surface -- serial only.\n"})
    assert ok, msg


def test_definitions_the_line_rules_missed_are_found():
    import _test_coverage_by_symbol as tcs
    assert tcs.public_symbols("a.c", "int foo(void) { return 1; }") == {"foo"}
    assert tcs.public_symbols("a.c", "void first(void) {}\nvoid second(void) {}\n") == {"first", "second"}
    assert tcs.public_symbols("a.c", "int pub(\n    int a,\n    int b)\n{\n    return a;\n}\n") == {"pub"}


def test_refusal_a_static_helper_on_a_split_line_is_not_public():
    src = "static\nint helper(void)\n{\n    return 0;\n}\nint dpc_queue(int x) {\n    return x;\n}\n"
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": src,
                         "src/kernel/test/test_x.c": "static void t(void) { helper(); }\n"})
    assert not ok and "dpc.c" in msg, msg


def test_refusal_a_name_in_a_string_is_not_a_call():
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C,
                         "src/kernel/test/test_x.c": 'static void t(void) { log("dpc_queue"); }\n'})
    assert not ok and "dpc.c" in msg, msg


def test_refusal_an_unreadable_staged_file_is_missing_evidence():
    saved = gate._staged_text

    def broken(root, path):
        raise gate._StagedReadError(f"{path}: timed out")
    gate._staged_text = broken
    try:
        ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C,
                             "src/kernel/test/test_sched.c": SCHED_TEST}, refuse=False)
    finally:
        gate._staged_text = saved
    assert not ok and "could not read" in msg, msg


def test_a_deleted_source_needs_no_test():
    with tempfile.TemporaryDirectory() as d:
        repo = _repo(d)
        _stage(repo, {"src/kernel/sched/dpc.c": DPC_C})
        subprocess.run(["git", "rm", "-q", "--cached", "src/kernel/sched/dpc.c"], cwd=d,
                       check=True, timeout=30)
        saved, gate.STEP8_REFUSE_FROM = gate.STEP8_REFUSE_FROM, "2000-01-01"
        try:
            ok, msg = gate._step8_test_wiring_check(repo, ["src/kernel/sched/dpc.c"], [])
        finally:
            gate.STEP8_REFUSE_FROM = saved
    assert ok, msg


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("OK")
