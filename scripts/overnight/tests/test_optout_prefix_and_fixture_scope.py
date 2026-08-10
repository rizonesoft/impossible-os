#!/usr/bin/env python3
"""Two v13 carries, resolved 2026-08-10 in the v14 cycle.

1. `optout_env_prefix_block` -- an opt-out env var on a NON-FIRST command in a
   chain is silently dropped by `_skip_env._scan_inline` (which stops at the
   first real token), so the gate blocks while its message tells the caller to
   set the variable they just set.

2. `skill_step_block` -- a `git commit` into a THROWAWAY /tmp fixture repo was
   judged as a section commit, forcing a standing opt-out on every fixture
   commit.

EVERY TEST HERE COMES IN A PAIR. The pass-direction case is satisfiable by
deleting the gate; only the REFUSAL-direction cases establish that the thing
still blocked is still blocked. A canary run exercises the happy path at scale
and structurally cannot exercise the refusal path -- which is the entire purpose
of a gate -- so the refusal cases are the load-bearing half.
"""
import importlib.util
import json
import os
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
PREFIX_HOOK = REPO / ".claude/hooks/optout_env_prefix_block.py"


def _rc(cmd):
    r = subprocess.run(
        [sys.executable, str(PREFIX_HOOK)],
        input=json.dumps({"tool_name": "Bash", "tool_input": {"command": cmd}}),
        text=True, capture_output=True)
    return r.returncode


# ---------------------------------------------------------------------------
# 1. optout_env_prefix_block
# ---------------------------------------------------------------------------

def test_losing_shape_blocks():
    """The exact shape that recurred on 2026-08-10, plus its near relatives."""
    for cmd in [
        'git add path && SKIP_REVIEW_HOOK=1 git commit -m "msg"',
        'echo hi; SKIP_SKILL_STEP_BLOCK=1 git commit -m z',
        'a && b && SKIP_CI_PARITY=1 git push',
        'foo & SKIP_HOOK_AUDIT=1 git commit -m q',
    ]:
        assert _rc(cmd) == 2, f"should have blocked: {cmd!r}"


def test_correct_shape_passes():
    """REFUSAL CONTROL: a prefix that WILL be seen must never be flagged."""
    for cmd in [
        'SKIP_REVIEW_HOOK=1 git commit -m "msg"',
        'env SKIP_REVIEW_HOOK=1 git commit -m "msg"',
        'SKIP_CI_PARITY=1 git push origin main',
        # Present up front AND repeated later: works regardless.
        'SKIP_REVIEW_HOOK=1 git add x && SKIP_REVIEW_HOOK=1 git commit -m y',
    ]:
        assert _rc(cmd) == 0, f"should have passed: {cmd!r}"


def test_no_false_positives():
    """REFUSAL CONTROL: the gate must not fire on a MENTION, an unrelated env
    var, or a chain with no opt-out at all. A gate that guesses is worse than
    the quirk it replaces -- it would make every ordinary chained command
    suspect."""
    for cmd in [
        'git add x && echo "set SKIP_REVIEW_HOOK=1 next time"',
        'git add x && FOO=1 git commit -m y',
        'git add x && git commit -m y',
        'grep -rn SKIP_REVIEW_HOOK .claude/ && echo done',
    ]:
        assert _rc(cmd) == 0, f"false positive on: {cmd!r}"


def test_override_is_itself_a_leading_prefix():
    """The override obeys the rule the hook teaches: leading position only.
    Honouring it from anywhere would make this hook the one thing in the repo
    that reads an opt-out the scanners cannot -- the exact inconsistency the
    gate exists to surface."""
    assert _rc('OPTOUT_PREFIX_OVERRIDE=1 git add x '
               '&& SKIP_REVIEW_HOOK=1 git commit -m y') == 0
    # REFUSAL CONTROL: the override in a losing position does NOT clear it.
    assert _rc('git add x && OPTOUT_PREFIX_OVERRIDE=1 SKIP_REVIEW_HOOK=1 '
               'git commit -m y') == 2


def test_agrees_with_the_scanner_it_models():
    """The hook reproduces `_skip_env._scan_inline`'s stop rule rather than
    approximating it. Two definitions that can disagree would make this gate
    fire on calls that actually work -- so assert they agree on the same input
    rather than trusting the comment."""
    sys.path.insert(0, str(REPO / ".claude/hooks"))
    import _skip_env
    losing = 'git add x && SKIP_REVIEW_HOOK=1 git commit -m y'
    winning = 'SKIP_REVIEW_HOOK=1 git commit -m y'
    # The scanner genuinely cannot see the losing one...
    assert _skip_env.read_skip_envs(
        losing, keys=("SKIP_REVIEW_HOOK",), fallback_to_environ=False) == {}
    # ...and genuinely sees the winning one.
    assert _skip_env.read_skip_envs(
        winning, keys=("SKIP_REVIEW_HOOK",),
        fallback_to_environ=False).get("SKIP_REVIEW_HOOK") == "1"
    # ...which is exactly the split the hook enforces.
    assert _rc(losing) == 2 and _rc(winning) == 0


# ---------------------------------------------------------------------------
# 2. skill_step_block fixture scoping
# ---------------------------------------------------------------------------

def _ssb():
    os.environ["CLAUDE_PROJECT_DIR"] = str(REPO)
    sys.path.insert(0, str(REPO / ".claude/hooks"))
    spec = importlib.util.spec_from_file_location(
        "ssb_mod", REPO / ".claude/hooks/skill_step_block.py")
    m = importlib.util.module_from_spec(spec)
    sys.modules["ssb_mod"] = m
    spec.loader.exec_module(m)
    return m


def test_throwaway_repo_is_not_a_section_commit(tmp_path=None):
    m = _ssb()
    fixture = pathlib.Path("/tmp/ssb-scope-fixture")
    subprocess.run(["rm", "-rf", str(fixture)], check=False)
    fixture.mkdir(parents=True, exist_ok=True)
    subprocess.run(["git", "-C", str(fixture), "init", "-q"], check=True)
    try:
        assert not m._commit_is_in_project(
            f'git -C {fixture} commit -m "fixture"')
        assert not m._commit_is_in_project(
            f'cd {fixture} && git commit -m "fixture"')
    finally:
        subprocess.run(["rm", "-rf", str(fixture)], check=False)


def test_project_commits_still_gated():
    """REFUSAL CONTROL: this is the whole point of the gate. A section ship in
    the project repo -- bare, chained, or explicitly `-C`'d at the project --
    must still be judged."""
    m = _ssb()
    for cmd in [
        'git commit -m "section ship"',
        'git add -A && git commit -m "section ship"',
        f'git -C {REPO} commit -m "section ship"',
    ]:
        assert m._commit_is_in_project(cmd), f"stopped gating: {cmd!r}"


def test_undeterminable_target_fails_closed():
    """REFUSAL CONTROL: a destination that cannot be READ must be treated as
    the project. A section commit slipping the gate is the failure the gate
    exists to prevent; a fixture commit tripping it costs one opt-out."""
    m = _ssb()
    for cmd in [
        'git -C "$SOMEVAR" commit -m x',
        'cd /nonexistent-path-xyz && git commit -m x',
        'git -C /tmp/*/glob commit -m x',
    ]:
        assert m._commit_is_in_project(cmd), f"failed open on: {cmd!r}"


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
