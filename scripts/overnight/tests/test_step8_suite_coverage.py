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
REGISTERED = ('static void suite(void) {{ {body} }}\n'
              'void test_register_x(void) {{\n    test_suite_register_cat("x",\n        suite, TEST_CAT_SCHED);\n}}\n')


def suite(body: str) -> str:
    """A test file whose registered suite runs `body`."""
    return REGISTERED.format(body=body)


SCHED_TEST = '#include "x.h"\n' + suite("dpc_queue(1);")
OTHER_TEST = suite("/* dpc_queue is named only here */ heap_alloc(1);")


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
                         "src/kernel/test/test_smp.c": suite("")})
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
                         "src/kernel/test/test_x.c": suite("helper();")})
    assert not ok and "dpc.c" in msg, msg


def test_refusal_a_name_in_a_string_is_not_a_call():
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C,
                         "src/kernel/test/test_x.c": 'static void t(void) { log("dpc_queue"); }\n'})
    assert not ok and "dpc.c" in msg, msg


def test_refusal_an_unreadable_staged_file_is_missing_evidence():
    saved = gate._staged_texts

    def broken(root, paths):
        raise gate._StagedReadError("git cat-file: timed out")
    gate._staged_texts = broken
    try:
        ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C,
                             "src/kernel/test/test_sched.c": SCHED_TEST}, refuse=False)
    finally:
        gate._staged_texts = saved
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



def test_refusal_a_declaration_or_an_unregistered_caller_is_not_a_call():
    decl = "extern int dpc_queue(int);\n" + suite("heap_alloc(1);")
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": decl})
    assert not ok and "dpc.c" in msg, msg
    unreg = "void stray(void) { dpc_queue(1); }\n" + suite("heap_alloc(1);")
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": unreg})
    assert not ok and "dpc.c" in msg, msg


def test_a_call_through_a_helper_the_suite_reaches_counts():
    reached = "static void helper(void) { dpc_queue(1); }\n" + suite("helper();")
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": reached})
    assert ok, msg


def test_alternative_if_branches_do_not_hide_the_next_definition():
    src = ("int dpc_queue(int x) {\n#if A\n    if (x) {\n#else\n    if (!x) {\n#endif\n"
           "        return 1;\n    }\n    return 0;\n}\nint dpc_flush(void) {\n    return 0;\n}\n")
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": src,
                         "src/kernel/test/test_x.c": suite("dpc_flush();")})
    assert ok, msg


def test_refusal_acpica_without_its_provenance_record():
    hdr = {"src/kernel/acpica/include/platform/acimpossible.h": "#define ACPI_SINGLE_THREADED\n"}
    ok, msg, _ = _check(hdr)
    assert not ok and "Local additions" in msg, msg
    ok, msg, _ = _check({"src/libs/PROVENANCE.md": "# no such heading\n", **hdr})
    assert not ok and "Local additions" in msg, msg


def test_hundreds_of_staged_files_are_read_in_two_git_calls():
    files = {f"src/kernel/x/f{i}.c": f"int f{i}(void) {{\n    return {i};\n}}\n" for i in range(300)}
    files["src/kernel/test/test_x.c"] = suite(" ".join(f"f{i}();" for i in range(300)))
    real, calls = gate.subprocess.run, []

    def counting(*a, **k):
        calls.append(a[0][:3])
        return real(*a, **k)
    with tempfile.TemporaryDirectory() as d:
        repo = _repo(d)
        staged = _stage(repo, files)
        gate.subprocess.run = counting
        try:
            texts = gate._staged_texts(repo, staged)
        finally:
            gate.subprocess.run = real
    assert len(calls) == 2 and texts["src/kernel/x/f7.c"].startswith("int f7"), calls


def test_refusal_a_file_that_registers_nothing_runs_nothing():
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C,
                         "src/kernel/test/test_x.c": "static void unused(void) { dpc_queue(1); }\n"})
    assert not ok and "dpc.c" in msg, msg


def test_an_entry_point_the_runner_calls_counts():
    files = {"src/kernel/sched/dpc.c": DPC_C,
             "src/kernel/test/test_x.c": "int test_x_run(void) {\n    return dpc_queue(1);\n}\n"}
    ok, msg, _ = _check(files)
    assert not ok, "no runner names test_x_run yet"
    ok, msg, _ = _check({**files, "src/kernel/main/boot_tests.c": "/* tested-by: test_x.c */\nvoid go(void) { test_x_run(); }\n"})
    assert ok, msg


def test_the_test_build_decides_which_if_branch_runs():
    disabled = suite("\n#if 0\n    dpc_queue(1);\n#endif\n")
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": disabled})
    assert not ok, "a call under #if 0 never runs"
    active = suite("\n#if 0\n    heap_alloc(1);\n#else\n    dpc_queue(1);\n#endif\n")
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": active})
    assert ok, msg
    flavour = suite("\n#ifndef KERNEL_TESTS\n    heap_alloc(1);\n#else\n    dpc_queue(1);\n#endif\n")
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": flavour})
    assert ok, msg


def test_refusal_a_local_named_like_the_function_is_not_a_use():
    shadow = suite('int dpc_queue = 0;\n    TEST_ASSERT_EQ(dpc_queue, 0, "x");')
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": shadow})
    assert not ok and "dpc.c" in msg, msg


def test_a_function_taken_by_address_after_a_block_prototype_counts():
    addr = suite("extern int dpc_queue(int);\n    probe((void *)dpc_queue);")
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": addr})
    assert ok, msg


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("OK")
