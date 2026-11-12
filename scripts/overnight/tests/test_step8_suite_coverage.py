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
import re
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


RUNNER = "src/kernel/test/test_runner.c"
USER_MAKE = ("userland: $(SYSROOT)/test_libc_x.exe\n"
             "$(SYSROOT)/test_libc_x.exe: user/test/test_libc_x.c\n"
             "\t$(CC) $(USER_CFLAGS) -c user/test/test_libc_x.c -o $(BUILD_DIR)/user/test/test_libc_x.o\n"
             "\t$(LD) -o $(BUILD_DIR)/user/test/test_libc_x.exe \\\n"
             "\t\t$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/test/test_libc_x.o\n")
DEFAULT_RUNNER = "void boot_tests_run(void) {\n    test_register_x();\n}\n"


def _check(files: dict[str, str], refuse: bool = True, todo: dict | None = None,
           runner: str | None = DEFAULT_RUNNER):
    """(ok, message, stderr) for a commit staging `files`, in a repository whose
    index holds `runner` as the suite runner (None: no runner)."""
    saved = gate._step8_sections_stamped
    gate._step8_sections_stamped = lambda root, deadline=None: gate.STEP8_WARN_SECTIONS if refuse else 3
    err = io.StringIO()
    try:
        with tempfile.TemporaryDirectory() as d:
            repo = _repo(d)
            if runner is not None and RUNNER not in files:
                _stage(repo, {RUNNER: runner})
            staged = _stage(repo, files)
            todos = list(_stage(repo, todo)) if todo else []
            with contextlib.redirect_stderr(err):
                ok, msg = gate._step8_test_wiring_check(repo, staged, todos)
    finally:
        gate._step8_sections_stamped = saved
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


def test_for_the_first_ten_sections_it_warns_and_lets_the_commit_through():
    ok, msg, err = _check({"src/kernel/sched/dpc.c": DPC_C}, refuse=False)
    assert ok and "WARN (step-8, 3 of 10" in err and "dpc.c" in err, err


def test_the_warning_sections_are_sections_first_stamped_since_the_rule():
    with tempfile.TemporaryDirectory() as d:
        repo = _repo(d)

        def commit(files):
            _stage(repo, files)
            subprocess.run(["git", "-c", "commit.gpgsign=false", "commit", "-q", "--no-verify",
                            "-m", "x"], cwd=d, check=True, timeout=30)
            return subprocess.run(["git", "rev-parse", "HEAD"], cwd=d, capture_output=True,
                                  text=True, check=True, timeout=30).stdout.strip()
        base = commit({"todo/T.md": "## 1. Old\n> **Verified:** old\n## 2. Open\n"})
        for i in range(10):  # ten refreshes of one existing stamp add no section
            commit({"todo/T.md": f"## 1. Old\n> **Verified:** refresh {i}\n## 2. Open\n"})
        commit({"todo/T.md": "## 1. Old\n> **Verified:** x\n## 2. Open\n> **Verified:** a\n> **Verified:** b\n",
                "todo/U.md": "## 7. New\n> **Verified:** c\n",
                "src/x.c": "## 9. Not\n> **Verified:** not a TODO\n"})
        saved = gate.STEP8_WARN_SINCE
        gate.STEP8_WARN_SINCE = base
        try:
            assert gate._step8_sections_stamped(repo) == 2
            commit({**{f"todo/S{i}.md": "## 1. S\n> **Verified:** s\n" for i in range(8)}})
            assert gate._step8_sections_stamped(repo) == 10
            gate.STEP8_WARN_SINCE = "0" * 40
            assert gate._step8_sections_stamped(repo) is None
        finally:
            gate.STEP8_WARN_SINCE = saved


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

    def broken(root, paths, deadline=None):
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
        saved = gate._step8_sections_stamped
        gate._step8_sections_stamped = lambda root, deadline=None: gate.STEP8_WARN_SECTIONS
        try:
            ok, msg = gate._step8_test_wiring_check(repo, ["src/kernel/sched/dpc.c"], [])
        finally:
            gate._step8_sections_stamped = saved
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
    assert not ok, "no runner calls test_x_run yet"
    ok, msg, _ = _check({**files, "src/kernel/main/boot_tests.c": "/* tested-by: test_x.c */\nvoid boot_tests_run(void) { test_x_run(); }\n"})
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


def test_refusal_a_registration_no_runner_calls():
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": SCHED_TEST},
                        runner="void boot_tests_run(void) {\n    test_register_other();\n}\n")
    assert not ok and "dpc.c" in msg, msg


def test_refusal_a_runner_that_declares_or_disables_the_call():
    files = {"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": SCHED_TEST}
    ok, msg, _ = _check(files, runner="extern void test_register_x(void);\nvoid boot_tests_run(void) {}\n")
    assert not ok, "a declaration starts nothing"
    ok, msg, _ = _check(files, runner="void boot_tests_run(void) {\n#if 0\n    test_register_x();\n#endif\n}\n")
    assert not ok, "a call under #if 0 starts nothing"


def test_conditions_are_evaluated_with_the_files_own_macros():
    parens = suite("\n#if (0)\n    dpc_queue(1);\n#endif\n")
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": parens})
    assert not ok, "#if (0) never compiles"
    local = "#define ENABLE_PROBE 1\n" + suite("\n#ifdef ENABLE_PROBE\n    dpc_queue(1);\n#endif\n")
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": local})
    assert ok, msg
    header_value = suite("\n#if HEAP_INIT_ON_ALLOC\n    dpc_queue(1);\n#endif\n")
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": header_value})
    assert not ok, "a branch on a header macro's value may not compile, so it earns no credit"


def test_the_test_build_defines_match_the_makefile():
    import _test_coverage_by_symbol as tcs
    makefile = (HOOKS.parents[1] / "Makefile").read_text()
    for name, value in tcs.TEST_BUILD_DEFINES.items():
        if name != "__x86_64__":  # the compiler predefines it for --target=x86_64-elf
            assert re.search(rf"-D{name}(=|\b)", makefile), f"-D{name} is not in the Makefile"


def test_if_conditions_follow_c_precedence():
    import _test_coverage_by_symbol as tcs
    env = dict(tcs.TEST_BUILD_DEFINES)
    for expr, want in (("!2 == 1", False), ("2 == 2 == 1", True), ("(2 && 3) == 1", True),
                       ("1 + 2 * 3 == 7", True), ("1 ? 0 : 1", False), ("(0)", False),
                       ("HEADER_VALUE", None), ("0 && HEADER_VALUE", False)):
        assert tcs._condition("if", expr, env) is want, expr


def test_macros_changed_where_compilation_is_unknown_become_unknown():
    import _test_coverage_by_symbol as tcs
    code = ("#if HEADER_FLAG\n#define SKIP_TEST 1\n#endif\n#ifndef SKIP_TEST\nCALL_A\n#endif\n"
            "#define F 1\n#undef F\n#if F\nX\n#else\nCALL_B\n#endif\n")
    assert tcs._first_branches(code).split() == ["CALL_B"]


def test_refusal_a_registration_in_an_uncalled_runner_helper():
    runner = ("void boot_tests_run(void) {\n}\n"
              "static void forgotten(void) {\n    test_register_x();\n}\n")
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": SCHED_TEST},
                        runner=runner)
    assert not ok and "dpc.c" in msg, msg


def test_refusal_a_registration_in_an_uncalled_exported_runner_function():
    runner = ("void boot_tests_run(void) {\n}\n"
              "void forgotten(void) {\n    test_register_x();\n}\n")
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": SCHED_TEST},
                        runner=runner)
    assert not ok and "dpc.c" in msg, msg


def test_the_runner_root_is_what_the_boot_calls():
    import _test_coverage_by_symbol as tcs
    boot = (HOOKS.parents[1] / "src/kernel/main/boot_desktop.c").read_text()
    assert re.search(rf"\b{tcs.RUNNER_ROOT}\s*\(\s*\)\s*;", boot)


def test_refusal_a_macro_a_header_may_define_is_not_known_absent():
    for guard in ("#ifndef TEST_MAX_SUITES", "#if !defined(TEST_MAX_SUITES)", "#ifdef TEST_MAX_SUITES"):
        code = suite(f"\n{guard}\n    dpc_queue(1);\n#endif\n")
        ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": code})
        assert not ok, guard
    guarded = "#ifndef TEST_X_H\n#define TEST_X_H\n" + SCHED_TEST + "#endif\n"
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": guarded})
    assert ok, "an include guard compiles on first inclusion: " + msg


def test_if_integers_follow_c_signedness_and_division():
    import _test_coverage_by_symbol as tcs
    env = dict(tcs.TEST_BUILD_DEFINES)
    for expr, want in (("-1 < 1U", False), ("18446744073709551615ULL / 3 == 6148914691236517205ULL", True),
                       ("9223372036854775807 / 3 == 3074457345618258602", True), ("-7 / 2 == -3", True),
                       ("-7 % 2 == -1", True), ("(1 ? -1 : 0u) > 0", True),
                       ("9223372036854775807 + 1 > 0", None)):
        assert tcs._condition("if", expr, env) is want, expr


def test_a_user_mode_test_starts_at_main():
    libc = "int strlen_x(const char *s) {\n    return 0;\n}\n"
    test = "int main(void) {\n    return strlen_x(\"a\");\n}\n"
    make = USER_MAKE
    ok, msg, _ = _check({"user/lib/string_x.c": libc, "user/test/test_libc_x.c": test, "Makefile": make})
    assert ok, msg
    ok, msg, _ = _check({"user/lib/string_x.c": libc, "Makefile": make,
                         "user/test/test_libc_x.c": test.replace("int main", "int not_main")})
    assert not ok and "string_x.c" in msg, msg
    for unbuilt in ("all:\n", "# $(CC) -c user/test/test_libc_x.c\n",
                    "userland: $(SYSROOT)/test_libc_x.exe\nall: user/test/test_libc_x.c\n\t@true\n",
                    "ifeq (0,1)\n" + USER_MAKE + "endif\n",
                    USER_MAKE.replace("$(LD)", "@echo"),
                    USER_MAKE.replace("userland: $(SYSROOT)/test_libc_x.exe", "userland:"),
                    USER_MAKE.replace("$(SYSROOT)/test_libc_x.exe: user", "unused: user")
                    + "$(SYSROOT)/test_libc_x.exe:\n\t@true\n",
                    USER_MAKE + "$(SYSROOT)/test_libc_x.exe:\n\t@true\n",
                    USER_MAKE + "$(SYSROOT)/test_libc_x.exe: ; @true\n",
                    USER_MAKE + "$(SYSROOT)/test_libc_x.exe: ;\n",
                    USER_MAKE.replace("\t$(CC)", "\t@echo $(CC)").replace("\t$(LD)", "\t@echo $(LD)"),
                    "userland: $(SYSROOT)/test_libc_x.exe\n$(SYSROOT)/test_libc_x.exe:\n"
                    "\t@echo 'preview; $(CC) -c user/test/test_libc_x.c -o x.o; "
                    "$(LD) -o x.exe crt0.o user/test/test_libc_x.o'\n",
                    USER_MAKE.replace("\t$(CC)", "\ttrue || $(CC)").replace("\t$(LD)", "\ttrue || $(LD)"),
                    USER_MAKE.replace("\t$(CC)", "\tfalse && $(CC)").replace("\t$(LD)", "\tfalse && $(LD)")):
        ok, msg, _ = _check({"user/lib/string_x.c": libc, "user/test/test_libc_x.c": test, "Makefile": unbuilt})
        assert not ok and "string_x.c" in msg, "a test the Makefile never builds never runs"


def test_refusal_a_fallback_define_is_not_an_include_guard():
    fallback = ('#include "kernel/test/test.h"\n' + suite(
        "\n#ifndef TEST_MAX_SUITES\n#define TEST_MAX_SUITES 4\n    dpc_queue(1);\n#endif\n"))
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": fallback})
    assert not ok, "a header may already define the name, so the block may not compile"
    late = "#ifndef TEST_X_H\n#define TEST_X_H\n" + SCHED_TEST + "#endif\nint trailer;\n"
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": late})
    assert not ok, "code after the #endif means the block is not a whole-file guard"


def test_refusal_a_runner_static_shadows_a_test_function():
    runner = ("static void test_register_x(void) {\n}\n"
              "void boot_tests_run(void) {\n    test_register_x();\n}\n")
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": SCHED_TEST},
                        runner=runner)
    assert not ok and "dpc.c" in msg, "the runner's call binds to its own static function"


def test_step8_io_shares_one_deadline():
    import time
    with tempfile.TemporaryDirectory() as d:
        repo = _repo(d)
        _stage(repo, {"todo/T.md": "## 1. A\n"})
        spent = time.monotonic() - 1
        try:
            gate._staged_texts(repo, ["todo/T.md"], spent)
            raise AssertionError("a spent deadline must not read")
        except gate._StagedReadError:
            pass
        assert gate._step8_sections_stamped(repo, spent) is None


def test_guard_detection_scales_linearly():
    import time
    import _test_coverage_by_symbol as tcs
    header = "".join(f"#ifndef M{i}\n#define M{i} 1\n#endif\n" for i in range(40000)) + "int f(void);\n"
    t0 = time.monotonic()
    tcs.public_symbols("include/kernel/x.h", header)
    assert time.monotonic() - t0 < 2.0, "40,000 conditional defines took the hook's budget"


def test_refusal_a_test_stub_named_like_the_source_function():
    stub = "static int dpc_queue(int x) {\n    return x;\n}\n" + SCHED_TEST
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": stub})
    assert not ok and "dpc.c" in msg, "the call reaches the stub, not dpc.c"


def test_a_user_test_is_read_with_the_user_build_flags():
    import _test_coverage_by_symbol as tcs
    libc = "int strlen_x(const char *s) {\n    return 0;\n}\n"
    test = "int main(void) {\n#ifdef KERNEL_TESTS\n    return strlen_x(\"a\");\n#endif\n    return 0;\n}\n"
    ok, msg, _ = _check({"user/lib/string_x.c": libc, "user/test/test_libc_x.c": test, "Makefile": USER_MAKE})
    assert not ok and "string_x.c" in msg, "the user build does not define KERNEL_TESTS"
    makefile = (HOOKS.parents[1] / "Makefile").read_text()
    flags = re.search(r"^USER_CFLAGS\s*:=((?:.*\\\n)*.*)", makefile, re.M).group(1)
    assert set(re.findall(r"-D(\w+)", flags)) <= set(tcs.USER_BUILD_DEFINES)


def test_a_one_step_user_build_enters_where_it_says():
    import _test_coverage_by_symbol as tcs
    make = ("userland: $(SYSROOT)/test_pe_x.exe\n"
            "$(SYSROOT)/test_pe_x.exe: user/test/test_pe_x.c\n"
            "\tclang-19 -Wl,/entry:_start -o $(BUILD_DIR)/user/test/test_pe_x.exe \\\n"
            "\t\tuser/test/test_pe_x.c\n")
    assert tcs.user_built_tests(make) == {"user/test/test_pe_x.c": "_start"}
    unreached = make.replace("$(SYSROOT)/test_pe_x.exe: user", "unused: user")
    assert tcs.user_built_tests(unreached) == {}, "a recipe nothing in userland reaches never runs"
    printed = make.replace("\tclang-19", "\t@printf '%s\\n' clang-19")
    assert tcs.user_built_tests(printed) == {}, "a printed compiler command builds nothing"
    real = tcs.user_built_tests((HOOKS.parents[1] / "Makefile").read_text())
    assert real.get("user/test/test_loader_pe.c") == "_start" and real.get("user/test/test_libc.c") == "main"


def test_refusal_a_parameter_or_local_named_like_the_function():
    helper = ("static void check(int dpc_queue) {\n    (void)dpc_queue;\n}\n"
              "static void suite(void) {\n    int x = 0, dpc_queue = 0;\n    check(x + dpc_queue);\n}\n"
              "void test_register_x(void) {\n    test_suite_register_cat(\"x\", suite, TEST_CAT_SCHED);\n}\n")
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": helper})
    assert not ok and "dpc.c" in msg, "a parameter or a second declarator is not the function"
    for body in ("int (*dpc_queue)(void) = stub;\n    dpc_queue();",
                 "for (int x = 0, dpc_queue = 0; x < 1; ++x) { (void)dpc_queue; }",
                 "int x[] = {0}, dpc_queue = 0;\n    (void)dpc_queue; (void)x;",
                 "int a = (int){0}, dpc_queue = 0;\n    (void)dpc_queue; (void)a;",
                 "static int (*dpc_queue)(void) = stub;\n    dpc_queue();",
                 "unsigned int (*dpc_queue)(void) = (unsigned int (*)(void))stub;\n    dpc_queue();",
                 "int (dpc_queue) = 0;\n    (void)dpc_queue;",
                 "enum { A, dpc_queue = 1 };\n    (void)dpc_queue;",
                 "struct s v = { .dpc_queue = 1 };\n    v.dpc_queue = 2;\n    (&v)->dpc_queue = 3;",
                 "void helper(int dpc_queue);",
                 "void (*helper)(int dpc_queue);",
                 "typedef int dpc_queue;\n    (void)sizeof(dpc_queue);",
                 "dpc_t (dpc_queue) = 0;\n    (void)dpc_queue;"):
        test = ("typedef int dpc_t;\nstatic int stub(void) { return 0; }\n"
                f"static void suite(void) {{\n    {body}\n}}\n"
                "void test_register_x(void) {\n    test_suite_register_cat(\"x\", suite, TEST_CAT_SCHED);\n}\n")
        ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": test})
        assert not ok and "dpc.c" in msg, f"a local declared as {body!r} is not the function"


def test_the_userland_walk_scales_linearly():
    import time
    import _test_coverage_by_symbol as tcs
    make = "userland: " + " ".join(f"t{i}" for i in range(20000)) + "\n" + "".join(f"t{i}:\n" for i in range(20000))
    t0 = time.monotonic()
    tcs.user_built_tests(make)
    assert time.monotonic() - t0 < 2.0, "20,000 reachable targets took the hook's budget"


def test_a_call_through_a_dereference_is_still_a_use():
    for body in ("int r = (*dpc_queue)(1);\n    (void)r;", "(void)(*dpc_queue)(1);",
                 "int r = (int){dpc_queue(1)};\n    (void)r;",
                 "int scale = 2;\n    int r = scale * dpc_queue(1);\n    (void)r;",
                 "int (*dpc_queue(int));\n    (void)dpc_queue(1);",
                 "int scale = 2;\n    scale * dpc_queue(1);",
                 "int (dpc_queue)(int);\n    dpc_queue(1);"):
        test = (f"static void suite(void) {{\n    {body}\n}}\n"
                "void test_register_x(void) {\n    test_suite_register_cat(\"x\", suite, TEST_CAT_SCHED);\n}\n")
        ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": test})
        assert ok, f"{body!r} calls the function: {msg}"


def test_local_declarations_parse_in_linear_time():
    import time
    import _test_coverage_by_symbol as tcs
    body = "() { int " + ", ".join(f"a{i}[] = {{0}}" for i in range(22000)) + "; }"
    t0 = time.monotonic()
    tcs._body_uses(body)
    assert time.monotonic() - t0 < 2.0, "22,000 initialised declarators took the hook's budget"


def test_refusal_a_prototype_parameter_in_the_runner_starts_nothing():
    for decl in ("void helper(int test_register_x);", "void (*helper)(int test_register_x);",
                 "typedef int test_register_x;"):
        runner = f"void boot_tests_run(void) {{\n    {decl}\n}}\n"
        ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": SCHED_TEST}, runner=runner)
        assert not ok and "dpc.c" in msg, f"a parameter name in {decl!r} calls nothing"


def test_refusal_a_registered_name_a_local_shadows_is_not_the_function():
    test = ("static void unused(void) {\n    dpc_queue(1);\n}\n"
            "static void empty(void) {\n}\n"
            "void test_register_x(void) {\n    void (*unused)(void) = empty;\n"
            "    test_suite_register_cat(\"x\", unused, TEST_CAT_SCHED);\n}\n")
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": test})
    assert not ok and "dpc.c" in msg, "the registered callback is the local pointer, which calls empty()"


def test_a_typedef_out_of_scope_or_not_compiled_hides_no_use():
    test = ("/* typedef int consume; */\n#if 0\ntypedef int consume;\n#endif\n"
            "static void other(void) {\n    typedef int consume;\n}\n"
            "static void suite(void) {\n    { typedef int consume; }\n    consume(dpc_queue);\n}\n"
            "void test_register_x(void) {\n    test_suite_register_cat(\"x\", suite, TEST_CAT_SCHED);\n}\n")
    ok, msg, _ = _check({"src/kernel/sched/dpc.c": DPC_C, "src/kernel/test/test_x.c": test})
    assert ok, msg


def test_many_locals_read_in_linear_time():
    import time
    import _test_coverage_by_symbol as tcs
    body = "() {" + "".join(f"int a{i} = 0; a{i}++;" for i in range(20000)) + "}"
    t0 = time.monotonic()
    tcs._body_uses(body)
    assert time.monotonic() - t0 < 2.0, "20,000 locals took the hook's budget"


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("OK")
