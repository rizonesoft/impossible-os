#!/usr/bin/env python3
"""skill_step_observer does not revive a FINISHED skill from the transcript
after a compaction (2026-11-11).

Observed live: a review-todo-section of the TPM roadmap's event-log
section finished (its commit landed), but every later compaction orphaned
the state entries and the observer's transcript fallback found that same
Skill call as the most recent one and started it again. Four revivals in
one afternoon; the last made section_commit_gate read the next section's
first commit as the finished review's fix loop and refuse it.
"""
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / ".claude/hooks"))
import skill_step_observer as sso  # noqa: E402

ARGS = "todo/00-x/TODO-01-x.md first-section Harden"


def _orphan(steps, args=ARGS):
    return {"args": args, "compaction_orphaned": True,
            "steps_observed": [{"n": n} for n in steps]}


def test_finished_across_orphans_is_not_revived():
    """The terminal steps were split over several orphans; together they
    cover the skill, so the invocation is done."""
    state = {"review-todo-section.orphan.1": _orphan([5, 8]),
             "review-todo-section.orphan.2": _orphan([4, 17])}
    assert sso._invocation_finished(state, "review-todo-section", ARGS)


def test_unarchived_orphan_counts():
    """PreCompact flags the entry in place; archiving happens on next use."""
    state = {"review-todo-section": _orphan([4, 5, 8]),
             "review-todo-section.orphan.1": _orphan([17])}
    assert sso._invocation_finished(state, "review-todo-section", ARGS)


def test_unfinished_invocation_is_still_revived():
    """Refusal direction: a review interrupted mid-flight still resumes."""
    state = {"review-todo-section.orphan.1": _orphan([4, 5])}
    assert not sso._invocation_finished(state, "review-todo-section", ARGS)
    assert not sso._invocation_finished({}, "review-todo-section", ARGS)


def test_other_invocations_do_not_count():
    """Steps recorded for another section's review prove nothing here."""
    state = {"review-todo-section.orphan.1": _orphan([4, 5, 8, 17],
                                                      args="todo/00-x/TODO-01-x.md other-section")}
    assert not sso._invocation_finished(state, "review-todo-section", ARGS)


def test_live_entry_steps_do_not_count():
    """Only orphans are history; a live entry is the current attempt."""
    state = {"review-todo-section": {"args": ARGS,
                                     "steps_observed": [{"n": n} for n in (4, 5, 8, 17)]}}
    assert not sso._invocation_finished(state, "review-todo-section", ARGS)


def _run_main(state, transcript_lines, tool_input):
    """Drive the observer's real entry point against a scratch repo."""
    import io, json, os, subprocess, tempfile
    with tempfile.TemporaryDirectory() as d:
        subprocess.run(["git", "init", "-q", d], check=True)
        os.makedirs(os.path.join(d, ".claude/state"))
        sp = os.path.join(d, sso._STATE_REL)
        with open(sp, "w") as f:
            json.dump(state, f)
        tp = os.path.join(d, "t.jsonl")
        with open(tp, "w") as f:
            for ln in transcript_lines:
                f.write(json.dumps(ln) + "\n")
        saved = (sso._repo_root, sys.stdin)
        sso._repo_root = lambda: d
        sys.stdin = io.StringIO(json.dumps({"tool_name": "Bash", "tool_input": tool_input,
                                            "transcript_path": tp}))
        try:
            sso.main()
        finally:
            sso._repo_root, sys.stdin = saved
        with open(sp) as f:
            return json.load(f)


def test_main_does_not_revive_finished_review():
    call = {"message": {"content": [{"type": "tool_use", "name": "Skill",
            "input": {"skill": "review-todo-section", "args": ARGS}}]}}
    build = {"command": "bash scripts/build.sh"}
    done = {"review-todo-section.orphan.1": _orphan([4, 5, 8, 17])}
    out = _run_main(done, [call], build)
    assert "review-todo-section" not in out, "a finished review was revived"
    half = {"review-todo-section.orphan.1": _orphan([5, 8])}
    out = _run_main(half, [call], build)
    assert out.get("review-todo-section", {}).get("args") == ARGS, \
        "an interrupted review must still resume"


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    print("ok")
