#!/usr/bin/env python3
"""A section parked on a question is open work again once the question has an answer.

Until 2026-11-11 nothing read todo/answers.md back into the classifier: every one of
its 37 questions had an answer and 36 sections still read BLOCKED on them.
"""
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2] / ".claude/hooks"))
import sequencer_triage as st  # noqa: E402

ANSWERS = """# Answers

## Q1 (a): first

A: (operator, 2026-11-07) do the first thing.

## Q2 (b): second

A: (run decision, 2026-11-08) do the second thing.

## Q3 (c): third, nobody has answered
"""


def classes(stamp, answers=ANSWERS):
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        domain = root / "todo" / "01-x"
        domain.mkdir(parents=True)
        if answers is not None:
            (root / "todo" / "answers.md").write_text(answers)
        md = domain / "TODO-01-x.md"
        md.write_text(f"## 1. Parked\n\n- [/] an item\n\n{stamp}\n")
        stamps = st.section_stamps(str(md))
        return stamps[1], st.classify_section({"n": 1, "status": "/"}, stamps)


def test_an_answered_question_reopens_the_section():
    for stamp in ("> **Deferred:** awaiting-answer Q1 (`todo/answers.md`). needs a decision",
                  "> **Deferred:** [M] awaiting-answer Q2: a run decision counts as an answer",
                  "> **Deferred:** awaiting-answer Q1 and Q2 (`todo/answers.md`)."):
        kinds, cls = classes(stamp)
        assert kinds == set() and cls == st.NEEDS_WORK, (stamp, kinds, cls)


def test_a_park_that_still_waits_holds():
    for stamp in ("> **Deferred:** awaiting-answer Q3 (`todo/answers.md`).",          # unanswered
                  "> **Deferred:** awaiting-answer Q1 and Q3.",                         # one of two unanswered
                  "> **Deferred:** awaiting-answer, no question cited",                 # names no question
                  "> **Deferred:** [awaiting-operator] needs a physical machine, see Q1",  # not waiting on an answer
                  "> **Deferred:** awaiting-answer Q1, and awaiting-hardware"):         # waits on something else too
        kinds, cls = classes(stamp)
        assert kinds == {"D", "DR"} and cls == st.BLOCKED, (stamp, kinds, cls)


def test_a_terminal_park_and_a_missing_answers_file_are_unchanged():
    kinds, cls = classes("> **Deferred:** superseded by the owner, see Q1")
    assert kinds == {"D"} and cls == st.DONE
    kinds, cls = classes("> **Deferred:** awaiting-answer Q1.", answers=None)
    assert kinds == {"D", "DR"} and cls == st.BLOCKED


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    for t in tests:
        t()
    print(f"PASS: answered parks ({len(tests)} tests)")
