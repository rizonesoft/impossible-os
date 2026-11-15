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

The contract is pinned on the hook TEXT, and since 2026-11-15 also end to end:
a copy of the real hook pushes an explicit SHA that is not HEAD to a bare
remote, against a stub gate that records its arguments. The nested-detail helper IS exercised, extracted from the pack by
its function boundaries so no test seam is added to a control-plane file.
"""
import os
import pathlib
import re
import signal
import subprocess
import time
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
    for name in ("test_query_bounds", "test_todo_fence",
                 "test_alias_staleness", "test_format_md_tables"):
        assert f'_tt_nested_fail_detail "{name}"' in src, f"{name} failure has no detail call"
    # The todo-graph build suite is four shards judged in one loop, so its
    # detail call names the loop variable; every shard must be in that loop.
    shards = re.search(r'^_TT_TB_SHARDS="([a-z_ ]+)"$', src, re.M)
    assert shards, "_TT_TB_SHARDS list not found"
    assert set(shards.group(1).split()) == {
        "test_build", "test_build_gate", "test_build_sections_a", "test_build_sections_b"}
    assert '_tt_nested_fail_detail "$_tt_tb" "$TG_OUT"' in src, "shard failure has no detail call"
    # Refusal direction: the helper is only ever reached on the t_fail path, so
    # it can add a diagnostic but never turn a failure into a pass.
    for m in re.finditer(r'_tt_nested_fail_detail "(?:[a-z_]+|\$_tt_tb)" "\$[A-Z_]+"', src):
        preceding = src[max(0, m.start() - 400):m.start()]
        assert "t_fail" in preceding.rsplit("t_pass", 1)[-1], \
            "detail call not on a t_fail path"


STUB_GATE = """#!/usr/bin/env bash
printf 'base=%s args=%s\\n' "${IDENTITY_GATE_LAST_GATED_SHA:-}" "$*" >> "$GATE_LOG"
"""


def _run(cmd, timeout=60, env=None):
    """Run cmd in its own session; on timeout, and always at the end, signal the whole group.

    subprocess.run(timeout=) kills only the direct child, and a push starts the hook, the gate and git's own
    helpers under it: any of them could outlive the test and the temporary repository it ran in.
    """
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, env=env, start_new_session=True)
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        _kill_group(proc.pid)
        proc.communicate()
        raise
    finally:
        _kill_group(proc.pid)
    return subprocess.CompletedProcess(cmd, proc.returncode, out, err)


def _kill_group(pgid):
    try:
        os.killpg(pgid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


def _git(root, *args):
    r = _run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@example.invalid", *args])
    assert r.returncode == 0, r.stderr
    return r.stdout.strip()


def _push_fixture(d):
    """A clone of a bare remote carrying the real hook and a stub gate that records its arguments.

    History: `base` (on the remote), `pushed` (a resolver-closure change), `later` (HEAD, not pushed).
    """
    d = pathlib.Path(d)
    remote, root = d / "remote.git", d / "work"
    for cmd in (["git", "init", "-q", "--bare", "-b", "main", str(remote)],
                ["git", "clone", "-q", str(remote), str(root)]):
        r = _run(cmd)
        assert r.returncode == 0, r.stderr
    (root / ".githooks/lib").mkdir(parents=True)
    for rel in (".githooks/pre-push", ".githooks/lib/isolate-git-env.sh"):
        (root / rel).write_text((REPO / rel).read_text())
    (root / ".githooks/pre-push").chmod(0o755)
    # The hook refuses when a sibling gate's script is missing; those gates are not under test.
    (root / "scripts/hooks").mkdir(parents=True)
    (root / "scripts/hooks/static-analysis-gate.sh").write_text("exit 0\n")
    (root / "scripts/hooks/tooling-pack-reads.sh").write_text("exit 0\n")
    gate = root / "scripts/todo-graph/identity-gate.sh"
    gate.parent.mkdir(parents=True)
    gate.write_text(STUB_GATE)
    gate.chmod(0o755)
    heads = {}
    for name, path in (("base", "README"), ("pushed", "scripts/todo-graph/build.py"), ("later", "NOTES")):
        (root / path).write_text(name + "\n")
        _git(root, "add", "-A")
        _git(root, "commit", "-q", "-m", name)
        heads[name] = _git(root, "rev-parse", "HEAD")
        if name == "base":
            _git(root, "push", "-q", "origin", "main")
    _git(root, "config", "core.hooksPath", ".githooks")
    return root, gate, heads


def _push(root, sha, log, timeout=120):
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    # Only the identity block is under test: the other gates are switched off by their own opt-outs.
    env.update(GATE_LOG=str(log), SKIP_PUSH_IDENTITY_CHECK="1", SKIP_HOST_GATES="1",
               SKIP_TOOLING_SUITE="1", SKIP_RUNNER_SUITE="1", SKIP_CI_PARITY="1",
               SKIP_CI_PARITY_REASON="fixture", SKIP_STATIC_ANALYSIS="1")
    return _run(["git", "-C", str(root), "push", "-q", "origin", f"{sha}:refs/heads/main"],
                timeout=timeout, env=env)


def test_a_push_that_is_not_head_is_gated_on_the_pushed_commit():
    with tempfile.TemporaryDirectory() as d:
        root, _gate, h = _push_fixture(d)
        log = pathlib.Path(d) / "gate.log"
        assert _git(root, "rev-parse", "HEAD") == h["later"] != h["pushed"]
        r = _push(root, h["pushed"], log)
        assert r.returncode == 0, r.stdout + r.stderr
        lines = log.read_text().splitlines()
        assert lines == [f"base={h['base']} args=--head {h['pushed']}"], lines


def test_an_uncommitted_mode_change_does_not_skip_the_gate():
    with tempfile.TemporaryDirectory() as d:
        root, gate, h = _push_fixture(d)
        log = pathlib.Path(d) / "gate.log"
        gate.chmod(0o644)
        r = _push(root, h["pushed"], log)
        assert r.returncode == 0, r.stdout + r.stderr
        assert log.exists() and f"--head {h['pushed']}" in log.read_text(), r.stdout


def test_a_failing_gate_blocks_the_push_of_a_commit_that_is_not_head():
    with tempfile.TemporaryDirectory() as d:
        root, gate, h = _push_fixture(d)
        log = pathlib.Path(d) / "gate.log"
        gate.write_text(STUB_GATE + "exit 1\n")
        r = _push(root, h["pushed"], log)
        assert r.returncode != 0 and "push blocked" in r.stdout + r.stderr, r.stdout + r.stderr
        assert _git(root, "ls-remote", "origin", "main").split()[0] == h["base"]


def _gone(pid, within=5.0):
    """True once pid has exited (absent, or a zombie waiting for its reaper)."""
    deadline = time.monotonic() + within
    while time.monotonic() < deadline:
        try:
            state = pathlib.Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0]
        except (FileNotFoundError, ProcessLookupError):
            return True
        if state == "Z":
            return True
        time.sleep(0.05)
    return False


def test_a_stalled_hook_is_stopped_with_its_whole_tree():
    with tempfile.TemporaryDirectory() as d:
        root, gate, h = _push_fixture(d)
        log = pathlib.Path(d) / "gate.log"
        pidfile = pathlib.Path(d) / "gate.pid"
        # A grandchild of git: git -> the hook -> the gate, which becomes a bounded sleep.
        gate.write_text(STUB_GATE + f'echo $$ > "{pidfile}"\nexec sleep 30\n')
        try:
            _push(root, h["pushed"], log, timeout=3)
        except subprocess.TimeoutExpired:
            pass
        else:
            raise AssertionError("a gate that sleeps 30s finished inside a 3s timeout")
        assert pidfile.exists(), "the stalling gate never started"
        assert _gone(int(pidfile.read_text())), "the gate outlived the timed-out push"


if __name__ == "__main__":
    test_identity_block_is_not_gated_on_the_executable_bit()
    test_missing_gate_refuses_instead_of_declining_quietly()
    test_gate_is_told_which_commit_is_being_pushed()
    test_nested_detail_names_the_failing_assertion_and_keeps_the_log()
    test_nested_detail_is_wired_after_every_nested_t_fail_and_never_a_pass()
    test_a_push_that_is_not_head_is_gated_on_the_pushed_commit()
    test_an_uncommitted_mode_change_does_not_skip_the_gate()
    test_a_failing_gate_blocks_the_push_of_a_commit_that_is_not_head()
    test_a_stalled_hook_is_stopped_with_its_whole_tree()
    print("PASS: pre-push identity caller + nested-suite failure detail")
