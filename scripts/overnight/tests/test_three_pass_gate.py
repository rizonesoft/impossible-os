#!/usr/bin/env python3
"""The commit gate judges a three-pass section by its shape, and an older section as before.

docs/specs/2026-11-11-three-pass-review-design.md: plan before the code, one round after the build,
completion on the final candidate, reconciliation after a completion with gaps.
"""
import json
import pathlib
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / ".claude/hooks"))
import section_commit_gate as gate  # noqa: E402

TODO = "todo/00-x/TODO-01-x.md"


def _git(root, *args):
    done = subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                           "-c", "core.hooksPath=/dev/null", *args], capture_output=True, text=True, timeout=60)
    assert done.returncode == 0, done.stderr
    return done.stdout.strip()


def _history(tmp_path):
    """plan-time commit, the build, the fix after the round, a TODO-only commit; and an unrelated branch."""
    root = tmp_path / "repo"
    root.mkdir()
    _git(root, "init", "-q", "-b", "main")
    heads = {}
    for name, path, text in (("plan", "todo/00-x/TODO-01-x.md", "plan\n"), ("build", "src/kernel/a.c", "int a;\n"),
                             ("fix", "src/kernel/a.c", "int a = 1;\n"), ("docs", "todo/00-x/TODO-01-x.md", "stamped\n")):
        f = root / path
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_text(text)
        _git(root, "add", "-A")
        _git(root, "commit", "-q", "-m", name)
        heads[name] = _git(root, "rev-parse", "HEAD")
    _git(root, "checkout", "-q", "--orphan", "elsewhere")
    _git(root, "commit", "-q", "--allow-empty", "-m", "elsewhere")
    heads["elsewhere"] = _git(root, "rev-parse", "HEAD")
    _git(root, "checkout", "-q", "main")
    return root, heads


def _judge(root, **stamps):
    """stamps: kind=(head, order). Writes the stamp file and asks the gate about TODO."""
    now = time.time_ns()
    entry = {}
    for kind, (head, order) in stamps.items():
        kind = kind.replace("_", "-")
        entry[kind] = now - (100 - order) * 1_000_000_000
        entry[f"{kind}_head"] = head
    state = root / ".claude/state/last-review-stamps.json"
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text(json.dumps({TODO: entry}))
    return gate._four_dispatch_evidence(root, [TODO])


def test_a_three_pass_section_is_accepted_on_its_shape(tmp_path):
    root, h = _history(tmp_path)
    # plan before the code, the round on the build, completion on the fixed candidate; HEAD adds only the stamp.
    assert _judge(root, plan=(h["plan"], 1), round=(h["build"], 2), completion=(h["fix"], 3)) == (True, "")
    assert _judge(root, plan=(h["plan"], 1), round=(h["build"], 2), completion=(h["docs"], 3)) == (True, "")
    # A completion with gaps, the repair, then a reconciliation on the repaired candidate.
    assert _judge(root, plan=(h["plan"], 1), round=(h["build"], 2), completion=(h["build"], 3),
                  reconciliation=(h["fix"], 4)) == (True, "")


def test_refusal_each_missing_or_outdated_pass_is_named(tmp_path):
    root, h = _history(tmp_path)
    cases = (
        (dict(round=(h["build"], 2), completion=(h["fix"], 3)), "missing: plan"),
        (dict(plan=(h["plan"], 1), completion=(h["fix"], 3)), "missing: round"),
        (dict(plan=(h["plan"], 1), round=(h["build"], 2)), "missing: completion"),
        # The completion read the build; the fix came after it and nothing has read the fix.
        (dict(plan=(h["plan"], 1), round=(h["build"], 2), completion=(h["build"], 3)),
         "completion is not the last word on this candidate"),
        # A reconciliation that is newer but itself older than the last fix.
        (dict(plan=(h["plan"], 1), round=(h["build"], 2), completion=(h["fix"], 3),
              reconciliation=(h["build"], 4)), "reconciliation is not the last word on this candidate"),
        (dict(plan=(h["elsewhere"], 1), round=(h["build"], 2), completion=(h["fix"], 3)), "not in this history"),
        (dict(plan=(h["plan"], 1), round=(h["elsewhere"], 2), completion=(h["fix"], 3)), "not in this history"),
        (dict(plan=(h["plan"], 1), round=("", 2), completion=(h["fix"], 3)), "round has no dispatch head recorded"),
        # A reconciliation alone is not a completion.
        (dict(plan=(h["plan"], 1), round=(h["build"], 2), reconciliation=(h["fix"], 4)), "missing: completion"),
    )
    for stamps, want in cases:
        ok, why = _judge(root, **stamps)
        assert not ok and want in why and "three-pass review incomplete" in why, (stamps, why)


def test_an_older_section_is_judged_exactly_as_before(tmp_path):
    root, h = _history(tmp_path)
    assert _judge(root, adversarial=(h["fix"], 1), consistency=(h["fix"], 2), perf=(h["fix"], 3)) == (True, "")
    ok, why = _judge(root, adversarial=(h["fix"], 1), consistency=(h["fix"], 2))
    assert not ok and "missing: perf" in why and "three-pass" not in why, why
    ok, why = _judge(root, adversarial=(h["build"], 1), consistency=(h["fix"], 2), perf=(h["fix"], 3))
    assert not ok and "source changed since dispatch" in why, why


def test_refusal_one_pass_stamp_puts_the_section_on_the_new_rules(tmp_path):
    """The three older stamps cannot be topped up with a single pass, nor a pass set with older kinds."""
    root, h = _history(tmp_path)
    ok, why = _judge(root, adversarial=(h["fix"], 1), consistency=(h["fix"], 2), perf=(h["fix"], 3),
                     round=(h["build"], 4))
    assert not ok and "missing: plan" in why and "missing: completion" in why, why
    assert gate._three_pass_shape({"plan": 5}) and not gate._three_pass_shape({"adversarial": 5, "plan": 0})
    assert not gate._three_pass_shape({"plan": "5"}) and not gate._three_pass_shape({})
