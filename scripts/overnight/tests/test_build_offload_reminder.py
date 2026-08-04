#!/usr/bin/env python3
"""Tests for build_offload_reminder.py -- run-artifact.sh exemption + P3.4 BLOCK.

Codex audit 2026-07-13 (verified): the matcher fired on the INNER
`bash scripts/build.sh` even when correctly wrapped as
`bash scripts/overnight/run-artifact.sh <label> -- bash scripts/build.sh`
(9x on one sequence). The OUTER-wrapper exemption (prereq 1) lets P3.4 promote
the reminder from WARN to BLOCK (exit 2) without deadlocking the sanctioned path:
a BARE build/test in the SECTIONS phase is rerouted; the wrapped form is exempt.
"""
import importlib.util
import io
import json
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
HOOK = HERE.parent.parent.parent / ".claude/hooks/build_offload_reminder.py"


def _load():
    # Production runs `python3 .claude/hooks/build_offload_reminder.py`, so the
    # hook dir is on sys.path and `import _offload_log` (the dedup log) resolves.
    if str(HOOK.parent) not in sys.path:
        sys.path.insert(0, str(HOOK.parent))
    spec = importlib.util.spec_from_file_location("build_offload_reminder", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _run_main(mod, cmd, root):
    # Force the gating predicates so the test exercises only the matcher +
    # exemption + block, not live sequencer state.
    mod._in_sections = lambda r: True
    mod._recent_checks_runner_dispatch = lambda r: False
    mod._repo_root = lambda: root
    payload = {"tool_name": "Bash", "tool_input": {"command": cmd}}
    old = (sys.stdin, sys.stdout, sys.stderr)
    sys.stdin, sys.stdout, sys.stderr = (io.StringIO(json.dumps(payload)),
                                         io.StringIO(), io.StringIO())
    try:
        rc = mod.main()
        out, err = sys.stdout.getvalue(), sys.stderr.getvalue()
    finally:
        sys.stdin, sys.stdout, sys.stderr = old
    return rc, out, err


def test_wrapped_command_is_exempt():
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        rc, out, err = _run_main(
            mod,
            "bash scripts/overnight/run-artifact.sh build1 -- bash scripts/build.sh",
            root)
        assert rc == 0 and out.strip() == "" and err.strip() == "", (rc, out, err)


def test_bare_command_blocks_with_reroute():
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        rc, out, err = _run_main(mod, "bash scripts/build.sh", root)
        assert rc == 2, "P3.4: bare build in SECTIONS must BLOCK (exit 2): " + repr(rc)
        assert "build-offload BLOCK" in err and "run-artifact.sh" in err, err


def test_wrapped_test_and_smoke_also_exempt():
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        for inner in ("bash scripts/test.sh SUITE=mm", "bash scripts/test-smoke.sh"):
            rc, out, err = _run_main(
                mod, f"bash scripts/overnight/run-artifact.sh lbl -- {inner}", root)
            assert rc == 0 and err.strip() == "", f"{inner!r} wrapped must be exempt"


def test_block_is_idempotent_but_log_dedups():
    # P3.4: every bare attempt keeps blocking (reroute), but the offload-events
    # log dedups so a repeat does not spam it (prereq 3 now gates logging only).
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        rc1, _, err1 = _run_main(mod, "bash scripts/build.sh", root)
        rc2, _, err2 = _run_main(mod, "bash scripts/build.sh", root)
        assert rc1 == 2 and rc2 == 2, (rc1, rc2)          # both block
        assert "build-offload BLOCK" in err1 and "build-offload BLOCK" in err2
        log = root / ".claude/state/offload-events.jsonl"
        entries = [json.loads(x) for x in log.read_text().splitlines() if x.strip()]
        builds = [e for e in entries if e.get("detail") == "bash scripts/build.sh"]
        assert len(builds) == 1, f"log must dedup the repeat: {entries}"
        # A different script blocks too and logs its own entry.
        rc3, _, err3 = _run_main(mod, "bash scripts/test.sh SUITE=mm", root)
        assert rc3 == 2 and "build-offload BLOCK" in err3


def test_message_has_no_checks_runner_text():
    # P3.4 prereq 2: the deprecated checks-runner routing text is removed.
    mod = _load()
    assert "checks-runner" not in mod._MSG, mod._MSG


def test_lint_is_exempt_from_block():
    # J2b: lint is cheap -> not in the BLOCK matcher, so a bare lint does NOT block.
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        rc, out, err = _run_main(mod, "bash scripts/lint.sh", root)
        assert rc == 0 and err.strip() == "", (rc, err)


def test_r2_bypass_shapes_block():
    # R2 (2026-07-19): absolute paths, ./-prefix, direct execution at a command
    # position, and make test-* shorthands all route like the literal form.
    mod = _load()
    blocked = [
        "bash /home/u/projects/impossible-os/scripts/test.sh",
        "bash ./scripts/test-smoke.sh",
        "./scripts/test.sh",
        "cd /repo && scripts/test.sh SUITE=mm",
        "make test-mm",
        "cd sub && make test-fs",
        "make test",
        "sh scripts/build.sh",
        # review 2026-07-19 (finders A/B/altitude): verified live bypasses
        "bash -x scripts/test.sh",                # interpreter flag
        'bash "scripts/test.sh"',                 # quoted path
        "make test-MM",                           # uppercase suite
        "make test-abi-foo",                      # multi-hyphen target
        "cd repo\nmake test-mm",                  # newline-separated
        "FOO=1\nscripts/test.sh",                 # newline + env prefix
        "QUIET=1 scripts/test.sh",                # env prefix at cmd position
        "(scripts/test.sh)",                      # subshell
    ]
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        for cmd in blocked:
            rc, _, err = _run_main(mod, cmd, root)
            assert rc == 2 and "build-offload BLOCK" in err, (cmd, rc, err)


def test_r2_non_invocations_pass():
    # Reading/grepping a suite script, or `make` targets that are not tests,
    # must NOT trip the matcher.
    mod = _load()
    clean = [
        "grep -n foo scripts/test.sh",
        "cat scripts/build.sh",
        "git log --oneline -- scripts/test.sh",
        "make -n help",
        "ls scripts/",
        "make TESTVAR=1 help",
        "make test=1 help",                       # variable assignment, no target
        "git commit -m 'docs: how make test-mm works'",  # quoted mention mid-string
    ]
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        for cmd in clean:
            rc, _, err = _run_main(mod, cmd, root)
            assert rc == 0 and err.strip() == "", (cmd, rc, err)


def test_r2_wrapped_route_logs_follow():
    # R2: the sanctioned run-artifact.sh reroute records a kind=follow event so
    # offload-report.py counts command-reroute compliance, not just Agent
    # dispatches (the 9%-follow misread).
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        rc, _, err = _run_main(
            mod,
            "bash scripts/overnight/run-artifact.sh t1 -- bash scripts/test.sh",
            root)
        assert rc == 0 and err.strip() == "", (rc, err)
        log = root / ".claude/state/offload-events.jsonl"
        entries = [json.loads(x) for x in log.read_text().splitlines() if x.strip()]
        follows = [e for e in entries
                   if e.get("kind") == "follow"
                   and e.get("hook") == "build_offload_reminder"]
        assert len(follows) == 1, entries


def test_two_script_paths_are_not_an_invocation():
    """2026-07-28: `\\b(?:bash|sh)\\s+` was satisfied by the DOT inside
    `build.sh` -- `.` is a non-word char, `s` is a word char -- so
    `scripts/build.sh scripts/test.sh` contained the substring
    `sh scripts/test.sh` and a READ-ONLY grep naming two suite scripts was
    BLOCKED as if it invoked one. The reroute advice is meaningless for a grep,
    so the only exits were rephrasing or giving up."""
    mod = _load()
    clean = [
        "grep -n 'check-abi' scripts/build.sh scripts/test.sh",
        "ls -la scripts/build.sh scripts/test.sh",
        "wc -l scripts/build.sh scripts/test.sh scripts/lint.sh",
        "git diff --stat scripts/build.sh scripts/test.sh",
    ]
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        for cmd in clean:
            rc, _, err = _run_main(mod, cmd, root)
            assert rc == 0, f"read-only command blocked: {cmd!r} -> {err}"


def test_real_interpreter_forms_still_block():
    """The fix must not buy its silence by going blind: every genuine
    invocation shape still has to match, including the two a plain
    command-position anchor would have broken."""
    mod = _load()
    for cmd in ("bash scripts/test.sh", "sh scripts/build.sh",
                "/bin/bash scripts/test.sh", "time bash scripts/test.sh"):
        assert any(r.search(cmd) for r in mod._SCRIPT_RES), f"stopped matching: {cmd!r}"


def test_gate_is_scoped_to_the_headless_run():
    """The gate keyed on the GLOBAL cursor, so while a run sat in SECTIONS every
    interactive operator session inherited its phase gates -- including a BLOCK
    whose rationale is explicitly about the RUN's context budget. Same
    discriminator run_phase_guard uses so an operator is never trapped."""
    import os
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        (root / ".claude/state/sequencer-run.json").write_text(
            json.dumps({"active": True, "phase": "SECTIONS"}))
        prev = os.environ.pop("OVERNIGHT_SEQUENCER_RUN", None)
        try:
            assert not mod._in_sections(root), "operator session inherited the run's gate"
            os.environ["OVERNIGHT_SEQUENCER_RUN"] = "1"
            assert mod._in_sections(root), "headless run must still be gated"
        finally:
            os.environ.pop("OVERNIGHT_SEQUENCER_RUN", None)
            if prev is not None:
                os.environ["OVERNIGHT_SEQUENCER_RUN"] = prev



def test_v04_quoted_data_is_not_a_command_position():
    """v04 runner item (2026-07-30). `_CMD_POS` treated `|` and `(` as command
    positions with no notion of quoting, so a suite path inside a quoted
    ARGUMENT read as an invocation. Two live hits 7 hours apart in the 3-day
    canary: `(scripts/test.sh` inside prose, and the ledger string
    `"high|scripts/test-tooling.sh:7883|..."`.

    The EVASION is why this became a rewrite rather than a fourth special case:
    after four consecutive blocked retries the run got a ledger write through by
    splitting a string literal (`TT="scripts/test-tooling"".sh:7883"`). A gate
    that teaches the model to obfuscate its own commands has stopped guarding
    and started shaping behaviour."""
    mod = _load()
    benign = [
        'python3 .claude/hooks/finding-ledger.py add "high|scripts/test-tooling.sh:7883|note"',
        'git commit -m "reworded the (scripts/test.sh mention) in prose"',
        'X="a|scripts/test.sh|b"',
        'TT="scripts/test-tooling"".sh:7883"',       # the evasion, now needless
        'echo "run (scripts/build.sh) later"',
        'git log --oneline -- scripts/test.sh',       # `--` is a pathspec here
    ]
    for cmd in benign:
        assert mod._match_suite_invocation(cmd) is None, cmd
    # Real invocations must still be caught, including through a pipeline and a
    # subshell -- the operators are only inert INSIDE quotes.
    for cmd in ('bash scripts/test.sh', 'echo hi | bash scripts/test.sh',
                '(bash scripts/test.sh)', 'BASH_ENV=x bash scripts/test-tooling.sh',
                'time bash scripts/test.sh', '/bin/bash scripts/test.sh',
                'bash "scripts/test.sh"', './scripts/test.sh',
                'scripts/test.sh SUITE=mm', 'make test', 'make test-mm'):
        assert mod._match_suite_invocation(cmd) is not None, cmd
    assert mod._match_suite_invocation('make test=1') is None


def test_v04_unparseable_is_split_by_which_risk_is_present():
    """The v04 item prescribed a blanket fail-OPEN on a parse error ("a cost gate
    that cannot parse the command must not be the thing that stops a run"). That
    is right for the FALSE-POSITIVE class it was written about, and WRONG for the
    bypass class -- the pre-existing tooling suite says so in as many words: "a
    false block costs a rephrase; a false allow runs the suite with nothing
    reporting it." Reconciled by which risk is actually present.

    Unparseable AND names a suite -> fail CLOSED (possible bypass; falls back to
    the legacy textual matcher, preserving hardening that suite earned).
    Unparseable and names none -> nothing to block, so the fail-open intent
    holds where it applies."""
    mod = _load()
    for cmd in ("echo 'unterminated", 'echo "unterminated',
                "git commit -m 'unclosed message"):
        assert mod._match_suite_invocation(cmd) is None, cmd
    for cmd in ("bash scripts/test.sh 'oops",
                "bash scripts/codex-dispatch.sh 'unbalanced && bash scripts/test.sh"):
        assert mod._match_suite_invocation(cmd) is not None, cmd
    assert mod._split_unquoted("echo 'x") is None


def test_v04_substitution_and_dash_c_bodies_still_run():
    """A substitution body and a `-c` string EXECUTE while being, syntactically,
    an argument -- so an otherwise-exempt outer command carried a bare suite run
    inside it. Pinned here because the structural rewrite had to re-earn these."""
    mod = _load()
    for cmd in (
            'bash scripts/codex-dispatch.sh "`bash scripts/test.sh QUIET=1`"',
            'bash scripts/codex-dispatch.sh "$(bash scripts/test.sh)"',
            'bash scripts/codex-dispatch.sh <(bash scripts/test.sh QUIET=1)',
            'bash scripts/codex-dispatch.sh >(bash scripts/test.sh QUIET=1)',
            'bash scripts/codex-dispatch.sh "$(echo `echo )`; bash scripts/test.sh)"',
            "bash -c 'bash scripts/test.sh QUIET=1' "
            "bash scripts/overnight/run-artifact.sh lbl -- true"):
        assert mod._match_suite_invocation(cmd) is not None, cmd



def test_v04_loop_and_conditional_bodies_still_caught():
    """Segments start after `;`/`&`/`|`, so a loop body arrives as
    `do bash scripts/test.sh`. Shell keywords must be transparent or every
    `for`/`while`/`if` wrapper is a bypass the old textual matcher did not have."""
    mod = _load()
    for cmd in ("for i in 1 2; do bash scripts/test.sh; done",
                "while true; do bash scripts/build.sh; done",
                "if true; then bash scripts/test.sh; fi",
                "until false; do scripts/test.sh; done"):
        assert mod._match_suite_invocation(cmd) is not None, cmd
    # ...while `done`/`fi` alone still end cleanly and prose stays free.
    assert mod._match_suite_invocation(
        "cat > f.md <<'EOF'\n- ran bash scripts/test.sh green\nEOF") is None

def _main():
    test_two_script_paths_are_not_an_invocation()
    test_real_interpreter_forms_still_block()
    test_gate_is_scoped_to_the_headless_run()
    test_wrapped_command_is_exempt()
    test_bare_command_blocks_with_reroute()
    test_wrapped_test_and_smoke_also_exempt()
    test_block_is_idempotent_but_log_dedups()
    test_message_has_no_checks_runner_text()
    test_lint_is_exempt_from_block()
    test_r2_bypass_shapes_block()
    test_r2_non_invocations_pass()
    test_v04_quoted_data_is_not_a_command_position()
    test_v04_unparseable_is_split_by_which_risk_is_present()
    test_v04_substitution_and_dash_c_bodies_still_run()
    test_v04_loop_and_conditional_bodies_still_caught()
    test_r2_wrapped_route_logs_follow()
    test_v08_non_executing_option_is_not_an_invocation()
    test_v08_message_prose_is_not_a_command()
    test_v08_wrapped_route_inside_a_loop_is_still_wrapped()
    print("PASS: build_offload_reminder exemption + P3.4 BLOCK + dedup + lint-exempt"
          " + R2 bypass shapes + follow log + v08 argv-attribution")


def test_v08_non_executing_option_is_not_an_invocation():
    """`bash -n` READS AND PARSES. It cannot run the suite, by definition.

    Filed THREE separate times in v08 (2026-08-03 11:34, 2026-08-04 05:12, and
    inside the composite wrapper item), because a syntax check is the normal
    inner loop for a 7,000-line shell harness and the gate refused it after
    every edit. The workaround actually reached for was hiding the path behind a
    shell variable -- an evasion that works for the wrong reason.

    The exemption is on the FLAG, not the script name: drop `-n` and the same
    command blocks again.
    """
    mod = _load()
    for cmd in ("bash -n scripts/test.sh",
                "/bin/bash -n scripts/test-tooling.sh",
                "bash --noexec scripts/test.sh",
                "bash -nx scripts/build.sh"):
        assert mod._blocking_match(cmd) is None, cmd
    # and the flag cannot smuggle a real run past the gate
    assert mod._blocking_match("bash -n scripts/lint.sh; bash scripts/test.sh")
    assert mod._blocking_match("bash scripts/test.sh")


def test_v08_message_prose_is_not_a_command():
    """Markdown code-quotes in a commit message are not a substitution.

    `git commit -m "...a plain \\`bash scripts/test.sh\\`..." -- <paths>` was
    BLOCKED while DESCRIBING the gate its own section had just fixed, and the
    answer was to reword the commit record. A gate that cannot read prose must
    not be editing it.

    The distinction is what bash actually executes: single quotes suppress every
    expansion and an escaped backtick is literal, so neither runs -- but an
    UNESCAPED backtick inside double quotes really does substitute and must
    still block. That control is the whole point of the fix being accuracy
    rather than an exemption.
    """
    mod = _load()
    assert mod._blocking_match(
        'git commit -m "a plain \\`bash scripts/test.sh\\` gate" -- a.md') is None
    assert mod._blocking_match(
        "git commit -m 'describes `bash scripts/test.sh` in prose' -- a.md") is None
    # LIVE substitutions still run, and still block
    assert mod._blocking_match('git commit -m "runs `bash scripts/test.sh`" -- a.md')
    assert mod._blocking_match('echo "x $(bash scripts/test.sh) y"')
    assert mod._blocking_match(
        "git commit -m 'prose `bash scripts/test.sh`' && bash scripts/test.sh")


def test_v08_wrapped_route_inside_a_loop_is_still_wrapped():
    """The exemption anchor missed a segment beginning with a shell keyword.

    A correctly-wrapped call was BLOCKED for sitting in a `for` loop: the
    splitter cuts on `;`, so the segment arrives as `do bash .../run-artifact.sh
    ... -- bash scripts/test-tooling.sh` and `^\\s*` no longer sat on the
    wrapper. NOTE the filed diagnosis blamed the loop's VARIABLE label; it is
    the `do` keyword -- a variable label outside a loop was never blocked.
    """
    mod = _load()
    assert mod._blocking_match(
        'for i in 1 2; do bash scripts/overnight/run-artifact.sh "stab$i" '
        '-- bash scripts/test-tooling.sh; done') is None
    assert mod._blocking_match(
        "if x; then bash scripts/overnight/run-artifact.sh l "
        "-- bash scripts/build.sh; fi") is None
    # a BARE suite in a loop is still caught
    assert mod._blocking_match("for i in 1 2; do bash scripts/test.sh; done")


if __name__ == "__main__":
    _main()
