#!/usr/bin/env python3
"""Caller-integrity contract for `.githooks/pre-push` -> identity-gate.sh, and the
tooling pack's nested-suite failure DETAIL (v17 close-out, 2026-08-29).

Two caller defects were found in one section and neither was in the gate:
  (1) the block was conditioned on `-x identity-gate.sh`, so a push that
      flipped the mode to 100644 alongside a real closure change SKIPPED the
      whole gate silently -- indistinguishable from "no closure change";
  (2) the gate was invoked with no `--head`, so mid-rebase it adjudicated the
      detached checkout instead of the ref being pushed.
And the pack reported a nested suite's TALLY only, discarding the `[FAIL]`
lines that name the assertion, so a pack-only refusal cost a full re-run to
learn nothing (measured ~4,500s on one section).

The hook cannot be exercised end to end without a remote, so its contract is
pinned on the hook TEXT the way test_tooling_scoped_suites pins the pack's
regexes; the nested-detail helper IS exercised, extracted from the pack by
its function boundaries so no test seam is added to a control-plane file.
"""
import os
import pathlib
import re
import subprocess
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
HOOK = REPO / ".githooks/pre-push"
TT = REPO / "scripts/test-tooling.sh"


def _hook():
    return HOOK.read_text()


def test_identity_block_is_not_gated_on_the_executable_bit():
    src = _hook()
    assert not re.search(r'\[ -x "\$REPO_ROOT/scripts/todo-graph/identity-gate\.sh" \]', src), \
        "the identity block is conditioned on -x again: a mode-only change would skip it silently"
    # The gate is always invoked through bash, mode-independent, like CI.
    assert re.search(r'bash "\$IDENTITY_GATE_SH"', src), "gate must be invoked via bash"


def test_missing_gate_refuses_instead_of_declining_quietly():
    src = _hook()
    m = re.search(r'if \[ ! -f "\$IDENTITY_GATE_SH" \]; then(.*?)\n  fi', src, re.S)
    assert m, "no explicit missing-file branch"
    assert "exit 1" in m.group(1), "a gate that cannot run must REFUSE, not skip"


def test_gate_is_told_which_commit_is_being_pushed():
    src = _hook()
    assert 'IDENTITY_HEAD="${IDENTITY_HEAD:-$loid}"' in src, "local oid of the pushed ref not captured"
    assert '--head "$IDENTITY_HEAD"' in src, "gate invoked without --head (detached-HEAD hazard)"
    # Refusal direction: the failure branch still blocks the push.
    m = re.search(r'Resolver identity gate failed -- push blocked(.*?)exit 1', src, re.S)
    assert m, "gate failure no longer exits 1"


def _extract_helper():
    src = TT.read_text()
    start = src.index('_TT_NESTED_LOG_DIR="${TEST_TOOLING_NESTED_LOG_DIR')
    end = src.index("\n}\n", start) + 3
    return src[start:end]


def test_nested_detail_names_the_failing_assertion_and_keeps_the_log():
    helper = _extract_helper()
    with tempfile.TemporaryDirectory() as d:
        out = ("[test_build] 827/828 passed, 1 failed\n"
               "OK   something fine\n"
               "[FAIL] 22f1: token B leaked into the diagnostic\n"
               "  detail line\n")
        script = (helper +
                  "\n_tt_nested_fail_detail test_build \"$1\"\n")
        r = subprocess.run(["bash", "-c", script, "_", out], capture_output=True,
                           text=True, env={**os.environ, "REPO_ROOT": str(REPO),
                                           "TEST_TOOLING_NESTED_LOG_DIR": d})
        assert r.returncode == 0, r.stderr
        assert "[FAIL] 22f1" in r.stdout, r.stdout
        assert "full nested output:" in r.stdout, r.stdout
        logs = list(pathlib.Path(d).glob("nested-test_build-*.log"))
        assert len(logs) == 1 and "[FAIL] 22f1" in logs[0].read_text()
        # The tally line is NOT repeated as a detail line (it is already in t_fail).
        assert "| [test_build] 827/828" not in r.stdout


def test_nested_detail_is_wired_after_every_nested_t_fail_and_never_a_pass():
    src = TT.read_text()
    for name in ("test_build", "test_query_bounds", "test_todo_fence",
                 "test_alias_staleness", "test_format_md_tables"):
        assert f'_tt_nested_fail_detail "{name}"' in src, f"{name} failure has no detail call"
    # Refusal direction: the helper is only ever reached on the t_fail path, so
    # it can add a diagnostic but never turn a failure into a pass.
    for m in re.finditer(r'_tt_nested_fail_detail "[a-z_]+" "\$[A-Z_]+"', src):
        preceding = src[max(0, m.start() - 400):m.start()]
        assert "t_fail" in preceding.rsplit("t_pass", 1)[-1], \
            "detail call not on a t_fail path"


if __name__ == "__main__":
    test_identity_block_is_not_gated_on_the_executable_bit()
    test_missing_gate_refuses_instead_of_declining_quietly()
    test_gate_is_told_which_commit_is_being_pushed()
    test_nested_detail_names_the_failing_assertion_and_keeps_the_log()
    test_nested_detail_is_wired_after_every_nested_t_fail_and_never_a_pass()
    print("PASS: pre-push identity caller + nested-suite failure detail")
