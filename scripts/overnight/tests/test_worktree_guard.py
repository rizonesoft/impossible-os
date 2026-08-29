#!/usr/bin/env python3
"""SEQ-WORKTREE adjudicates the INVOCATION, not the text (v17 close-out, 2026-08-29).

The guard used to shlex-split the whole command and match any `git` token
anywhere, so it blocked a heredoc PAYLOAD quoting the identity gate's own
`git worktree add` line, a commit MESSAGE describing that change, a `sed`
expression editing it -- five live blocks across TODO-06 sections 51-55, each
routed around in under a minute. This suite pins both directions through the
REAL hook process in a fixture repo:

  refusal direction -- every invocation shape that must still be BLOCKED,
  including the wrappers and chains a determined caller would reach for;
  allow direction   -- text that merely CONTAINS the phrase is not a mutation.
"""
import json
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_phase_guard_wait import _mk_fixture, _guard, HEADLESS  # noqa: E402

MUST_BLOCK = [
    "git worktree add ../x",
    "git -C /tmp/r worktree add wt",
    "cd /tmp && git worktree add wt",
    "env A=1 git worktree remove wt",
    "timeout 30 git worktree prune",
    'bash -c "git worktree add wt"',
    "sh -ec 'cd x; git worktree add wt'",
    "echo x | xargs git worktree add",
    "true; git worktree add x",
    "a&&git worktree add x",
    "git worktree add x <<EOF\nEOF",
    "python3 x.py && git worktree add wt",
    "git --git-dir=.git worktree move a b",
    "exec git worktree add y",
    "( cd /tmp/s && git worktree add wt )",
    "git worktree add x 2>&1 | tee log",
    "git worktree lock wt",
    # A scratch repo under /tmp is STILL blocked: the guard has no safe notion
    # of where a path resolves, and a wrong guess is a worktree in the run's tree.
    "git -C /tmp/scratch worktree add /tmp/scratch-wt",
]

MUST_ALLOW = [
    "git worktree list",
    "git -C /tmp/scratch worktree list",
    "python3 - <<'PY'\ns = \"git worktree add ../x\"\n"
    "import subprocess\nsubprocess.run(['git','worktree','add'])\nPY",
    'git commit -m "add git worktree add to identity gate" -- scripts/x.sh',
    "git commit -F /tmp/msg.txt",
    "echo git worktree add",
    "sed -i 's/git worktree add/x/' scripts/todo-graph/identity-gate.sh",
    'grep -n "git worktree add" scripts/todo-graph/identity-gate.sh',
    "printf '%s' \"git worktree add x\" > f",
    "cat <<'EOF' > f\ngit worktree add x\nEOF",
    "python3 -c \"print('git worktree add')\"",
    "git log --oneline | grep 'worktree add'",
]


def _bash(fx, cmd):
    return _guard(fx, "pretool", env_extra=HEADLESS,
                  stdin=json.dumps({"tool_name": "Bash",
                                    "tool_input": {"command": cmd}}))


def test_refusal_direction_every_invocation_shape_is_blocked():
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        (fx / ".claude/state/sequencer-armed").write_text("")
        missed = []
        for cmd in MUST_BLOCK:
            r = _bash(fx, cmd)
            if not (r.returncode == 2 and "SEQ-WORKTREE" in r.stderr):
                missed.append((cmd, r.returncode, r.stderr[-200:]))
        assert not missed, f"invocations that must be BLOCKED were allowed: {missed}"


def test_allow_direction_text_that_contains_the_phrase_is_not_a_mutation():
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        (fx / ".claude/state/sequencer-armed").write_text("")
        wrong = []
        for cmd in MUST_ALLOW:
            r = _bash(fx, cmd)
            if "SEQ-WORKTREE" in r.stderr:
                wrong.append((cmd, r.stderr[-200:]))
        assert not wrong, f"text-only mentions blocked as mutations: {wrong}"


def test_classifier_unit_level_matches_process_level():
    """The classifier is also importable; pin it directly so a regression is
    named at the function, not only at the hook's exit code."""
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "rpg", HERE.parent.parent.parent / ".claude/hooks/run_phase_guard.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    for cmd in MUST_BLOCK:
        assert mod._is_worktree_mutation(cmd), cmd
    for cmd in MUST_ALLOW:
        assert not mod._is_worktree_mutation(cmd), cmd
    assert not mod._is_worktree_mutation("")
    assert not mod._is_worktree_mutation(None)


if __name__ == "__main__":
    test_refusal_direction_every_invocation_shape_is_blocked()
    test_allow_direction_text_that_contains_the_phrase_is_not_a_mutation()
    test_classifier_unit_level_matches_process_level()
    print("PASS: worktree guard (command-position matching)")
