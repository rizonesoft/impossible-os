#!/usr/bin/env python3
"""Nested-suite scoping in `scripts/test-tooling.sh` (2026-08-10).

WHY IT EXISTS. Two NESTED regression suites ran unconditionally inside the
tooling pack and together were 60% of its wall-clock -- `test_build.sh` at 252s
and `test_bridge.sh` at 219s against ~320s for the other ~740 tests. Since
`.githooks/pre-push` runs the pack whenever a push touches `.claude/hooks/`, a
hooks-only push paid 471s for two suites it could not possibly affect. That is
what took the pre-push stack past the 10-minute tool wall and killed two
consecutive pushes.

THE SAFETY PROPERTY IS THE DEFAULT, not the scoping. With
TEST_TOOLING_CHANGED_PATHS unset -- which is how CI invokes it -- everything
runs. Every uncertain state (empty file, unreadable file, no parseable paths)
also runs everything. A scoping bug must cost time, never coverage, and these
tests assert that direction far more than they assert the saving.

The regexes are read FROM THE SCRIPT rather than restated here. A copy would let
the two drift, and a test that pins a stale copy of the rule reports on nothing.
"""
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
TT = REPO / "scripts/test-tooling.sh"
PREPUSH = REPO / ".githooks/pre-push"


def _src():
    return TT.read_text()


def _trigger(name):
    m = re.search(rf"{name}='([^']+)'", _src())
    assert m, f"{name} not found in test-tooling.sh -- the rule moved or was renamed"
    return m.group(1)


def test_triggers_cover_everything_each_suite_reads():
    """REFUSAL CONTROL, and the one that matters most. Each trigger must fire
    for every path class its suite actually reads -- derived by extracting the
    repo paths each harness references, not guessed. Too WIDE only costs
    runtime; too NARROW skips a suite that mattered, and only that direction is
    silent."""
    tg = _trigger("_TT_TRIG_TODO_GRAPH")
    lsp = _trigger("_TT_TRIG_LSP_MCP")
    must_run_todo_graph = [
        "scripts/todo-graph/build.py",
        "scripts/todo-graph/tests/test_build.sh",
        "scripts/lint/check_bucket_emission.py",
        "scripts/lint.sh",
        "scripts/todo-reachability.py",
        "scripts/setup-deps.sh",
        "todo/00-infrastructure/TODO-06-todo-metadata-layer.md",
        "tools/firmware-tables-decode.c",
        "docs/infrastructure/todo-graph.md",
        ".githooks/pre-push",          # section 30 commit-tick tests
        ".githooks/post-commit",
        "scripts/test-tooling.sh",     # the pack invoking it
    ]
    must_run_lsp = [
        "scripts/lsp-mcp/bridge.py",
        "scripts/lsp-mcp/tests/test_bridge.sh",
        "scripts/machines/run-qemu.ps1",
        "scripts/test-tooling.sh",
    ]
    for p in must_run_todo_graph:
        assert re.search(tg, p), f"todo-graph trigger MISSES {p} -- it would be skipped"
    for p in must_run_lsp:
        assert re.search(lsp, p), f"lsp-mcp trigger MISSES {p} -- it would be skipped"


def test_unreachable_paths_are_actually_skipped():
    """The saving direction. If this fails the scoping does nothing, which is a
    performance bug, not a correctness one."""
    tg, lsp = _trigger("_TT_TRIG_TODO_GRAPH"), _trigger("_TT_TRIG_LSP_MCP")
    for p in [".claude/hooks/optout_env_prefix_block.py",
              ".claude/settings.json",
              "src/kernel/mm/pmm.c"]:
        assert not re.search(tg, p), f"todo-graph would run for unrelated {p}"
        assert not re.search(lsp, p), f"lsp-mcp would run for unrelated {p}"


def test_default_and_every_uncertain_state_runs_everything():
    """REFUSAL CONTROL. `_tt_suite_needed` returns 0 (run) whenever the changed
    set is empty -- unset var, empty file, unreadable file. This is what keeps
    CI at full coverage, since CI passes nothing."""
    src = _src()
    fn = src[src.index("_tt_suite_needed() {"):]
    fn = fn[:fn.index("\n}\n") + 3]
    assert '[ -n "$_TT_CHANGED" ] || return 0' in fn, \
        "the empty-changed-set fast path must RUN the suite (return 0), not skip it"
    # And the variable is only populated from a readable file.
    assert '[ -r "$_TT_CHANGED_FILE" ]' in src, \
        "an unreadable changed-paths file must leave the set empty -> run everything"


def test_every_skip_is_announced():
    """NO SILENT CAPS. A skipped suite reads exactly like a passing one unless
    the summary says otherwise, and the pre-push caller uses --quiet -- which is
    precisely where the reduced coverage matters, so the notice must not be
    behind the quiet check."""
    src = _src()
    assert "_TT_SKIPPED+=(" in src, "skips must be recorded"
    i = src.index("nested suite(s) skipped")
    # Walk back to the enclosing condition; it must not be gated on QUIET.
    head = src[max(0, i - 400):i]
    assert 'if [ "${#_TT_SKIPPED[@]}" -gt 0 ]' in head, \
        "the skip notice must be emitted whenever anything was skipped"
    assert 'QUIET' not in head.split('if [ "${#_TT_SKIPPED[@]}" -gt 0 ]')[-1], \
        "the skip notice must NOT be suppressed by --quiet"


def test_lsp_skip_names_both_harnesses():
    """The else-branch runs test_bridge.sh AND test_boundary.sh. A single skip
    line under-reports by one suite and drops the total by one -- caught live
    when the first scoped run reported 1319/1320 against a full run's 1320."""
    src = _src()
    blk = src[src.index("elif ! _tt_suite_needed \"lsp-mcp"):]
    blk = blk[:blk.index("\nelse\n")]
    assert "test_bridge.sh SKIPPED" in blk and "test_boundary.sh SKIPPED" in blk, \
        "both lsp-mcp harnesses must be named when the group is skipped"


def test_prepush_passes_paths_and_cleans_up():
    """The caller side: pre-push must hand over the pushed paths, and must not
    leak the temp file on the success path (it originally removed it only in the
    failure branch)."""
    src = PREPUSH.read_text()
    assert 'TEST_TOOLING_CHANGED_PATHS="$TT_CHANGED"' in src, \
        "pre-push must pass the changed-paths file to the suite"
    assert src.count('rm -f "$TT_CHANGED"') >= 2, \
        "the temp file must be removed on the success path too, not only on failure"
    # A mktemp failure must not produce an empty path that reads as 'scoped'.
    assert 'if [ -n "$TT_CHANGED" ]; then' in src, \
        "a failed mktemp must not be treated as a usable changed-paths file"


if __name__ == "__main__":
    fails = []
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"  PASS {name}")
            except Exception as exc:
                fails.append(name)
                print(f"  FAIL {name}: {exc}")
    sys.exit(1 if fails else 0)
