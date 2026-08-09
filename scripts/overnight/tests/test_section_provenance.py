#!/usr/bin/env python3
"""todo-staged-check's two SECTION-HYGIENE refusals (2026-08-09).

Both exist because a TODO grew from 9 sections to 35 in seven days while every
sensor read healthy:

  * PROVENANCE -- the spawn-chain sensor can only count sections that declare
    where they came from, and nothing required the declaration. Measured: 20
    markers across 2,410 sections, all in the one file the run happened to be
    in when the sensor shipped. An undeclared section is a root at depth 0, so
    a review-spawn cascade never reaches the limit and never justifies itself.
  * PARK-INTO-A-SHIPPING-SECTION -- a `- [/]` park is how you file into a
    section that is ALREADY closed. Parking into one you are closing strands it
    by construction: `stranded_deferrals` reports 60 items in DONE-parked
    sections that "will NOT flip naturally", three of them already finished
    with nobody left to tick the box.

Both are REFUSALS, so every test here asserts the allowed direction too -- a
gate that only ever says no gets switched off.
"""
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
CHECK = REPO / "scripts/todo-staged-check.py"

BASE = ("# T\n\n## Implementation Order\n\n| x | 1 | s1 | d | -- | [x] |\n\n"
        "## 1. One\n\n- [x] a\n")


def _repo(d):
    """A throwaway git repo with one committed TODO carrying one section."""
    root = pathlib.Path(d)
    (root / "todo/00-infrastructure").mkdir(parents=True)
    (root / "todo/00-infrastructure/TODO-01-x.md").write_text(BASE, encoding="utf-8")
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
    for cmd in (["git", "init", "-q", "."],
                ["git", "add", "todo/00-infrastructure/TODO-01-x.md"],
                ["git", "commit", "-qm", "init"]):
        subprocess.run(cmd, cwd=str(root), env=env, capture_output=True, check=True)
    return root, env


def _stage(root, env, rel, body):
    (root / rel).write_text(body, encoding="utf-8")
    subprocess.run(["git", "add", rel], cwd=str(root), env=env,
                   capture_output=True, check=True)


def _run(root, env):
    r = subprocess.run([sys.executable, str(CHECK)], cwd=str(root), env=env,
                       capture_output=True, text=True)
    return r.returncode, r.stderr


def test_new_section_without_provenance_is_refused():
    with tempfile.TemporaryDirectory() as d:
        root, env = _repo(d)
        _stage(root, env, "todo/00-infrastructure/TODO-01-x.md",
               BASE + "\n## 2. Two\n\n- [ ] work\n")
        rc, err = _run(root, env)
        assert rc == 1, (rc, err)
        assert "no provenance" in err, err
        # the message must be ACTIONABLE -- all three legal forms, verbatim
        for form in ("Spawned-by:** root", "(split)", "(review)"):
            assert form in err, (form, err)


def test_each_declared_form_is_accepted():
    """The `(review)` case carries a user-impact line because a review-spawn now
    requires one -- see test_review_spawned_section_must_state_user_impact. A
    fixture without it would be refused for the RIGHT reason and would stop
    testing provenance, which is what this case is about."""
    for marker in ("> **Spawned-by:** root",
                   "> **Spawned-by:** section 1 (split)",
                   "> **Spawned-by:** section 1 (review)\n"
                   "> **User impact:** a stale cache certifies a moved corpus"):
        with tempfile.TemporaryDirectory() as d:
            root, env = _repo(d)
            _stage(root, env, "todo/00-infrastructure/TODO-01-x.md",
                   BASE + f"\n## 2. Two\n\n{marker}\n\n- [ ] work\n")
            rc, err = _run(root, env)
            assert rc == 0, (marker, rc, err)


def test_preexisting_sections_are_not_judged():
    """Additive by construction: only sections this commit ADDS are checked, so
    editing an old section never trips it. Without this the gate would refuse
    every commit touching any of the 2,390 undeclared sections."""
    with tempfile.TemporaryDirectory() as d:
        root, env = _repo(d)
        _stage(root, env, "todo/00-infrastructure/TODO-01-x.md",
               BASE + "- [ ] another item on the OLD section\n")
        rc, err = _run(root, env)
        assert rc == 0, (rc, err)


def test_a_brand_new_todo_file_is_exempt():
    """Every section in a file this commit CREATES is a root by construction;
    stamping ten identical `root` lines on a scaffold is ceremony."""
    with tempfile.TemporaryDirectory() as d:
        root, env = _repo(d)
        _stage(root, env, "todo/00-infrastructure/TODO-02-new.md",
               "# N\n\n## Implementation Order\n\n| x | 1 | s1 | d | -- | [ ] |\n\n"
               "## 1. Fresh\n\n- [ ] work\n\n## 2. Also fresh\n\n- [ ] work\n")
        rc, err = _run(root, env)
        assert rc == 0, (rc, err)


def test_review_spawned_section_must_state_user_impact():
    """Provenance made the cascade countable; this makes each link answer for
    itself when the answer is cheap. The worked example: a section created for a
    real section-parser defect that has ZERO live occurrences in 232 TODO files,
    surfaced only by another section's fixture."""
    with tempfile.TemporaryDirectory() as d:
        root, env = _repo(d)
        _stage(root, env, "todo/00-infrastructure/TODO-01-x.md",
               BASE + "\n## 2. Two\n\n> **Spawned-by:** section 1 (review)\n\n- [ ] work\n")
        rc, err = _run(root, env)
        assert rc == 1, (rc, err)
        assert "what a user hits" in err, err
        # the message must say the honest negative answer is allowed, or it
        # teaches people to invent impact -- which is worse than no rule
        assert "Nothing today" in err, err


def test_user_impact_is_required_only_of_review_spawns():
    """A root is a capability someone set out to build; a split is justified
    work being partitioned. Neither is the runaway shape, and demanding the line
    of them would be ceremony that gets the whole check switched off."""
    for marker in ("> **Spawned-by:** root",
                   "> **Spawned-by:** section 1 (split)"):
        with tempfile.TemporaryDirectory() as d:
            root, env = _repo(d)
            _stage(root, env, "todo/00-infrastructure/TODO-01-x.md",
                   BASE + f"\n## 2. Two\n\n{marker}\n\n- [ ] work\n")
            rc, err = _run(root, env)
            assert rc == 0, (marker, rc, err)


def test_an_honest_negative_user_impact_is_accepted():
    """The content is NOT judged. "Nothing today" is the answer that talks an
    author out of the section, so it has to be sayable -- a check that rejected
    it would produce invented impact instead of honest ones."""
    with tempfile.TemporaryDirectory() as d:
        root, env = _repo(d)
        _stage(root, env, "todo/00-infrastructure/TODO-01-x.md",
               BASE + "\n## 2. Two\n\n> **Spawned-by:** section 1 (review)\n"
                      "> **User impact:** nothing today; only if a TODO ever fences a heading\n\n"
                      "- [ ] work\n")
        rc, err = _run(root, env)
        assert rc == 0, (rc, err)


def test_park_into_a_section_this_commit_stamps_is_refused():
    with tempfile.TemporaryDirectory() as d:
        root, env = _repo(d)
        _stage(root, env, "todo/00-infrastructure/TODO-01-x.md",
               BASE + "- [/] leftover from the review\n\n"
                      "> **Verified:** 2026-08-09 | commit `abc1234` | 1/1 items | build OK\n")
        rc, err = _run(root, env)
        assert rc == 1, (rc, err)
        assert "STAMPING" in err, err
        # and it must name the three reachable alternatives
        assert "- [x]" in err and "OPEN section" in err and "user-impact" in err, err


def test_park_into_an_already_closed_section_still_works():
    """The SANCTIONED cross-section park. Refusing this would break the only
    documented way to file into a section that has already shipped."""
    with tempfile.TemporaryDirectory() as d:
        root, env = _repo(d)
        stamped = BASE + ("\n> **Verified:** 2026-08-09 | commit `abc1234` "
                          "| 1/1 items | build OK\n")
        _stage(root, env, "todo/00-infrastructure/TODO-01-x.md", stamped)
        subprocess.run(["git", "commit", "-qm", "stamp", "--",
                        "todo/00-infrastructure/TODO-01-x.md"],
                       cwd=str(root), env=env, capture_output=True, check=True)
        _stage(root, env, "todo/00-infrastructure/TODO-01-x.md",
               stamped + "- [/] a LATER park into the already-closed section\n")
        rc, err = _run(root, env)
        assert rc == 0, (rc, err)


def test_stamping_with_named_fixes_is_never_blocked():
    """The disposition rule's DEFAULT -- fix it and name it as `- [x]` -- must
    sail through, or the gate would push authors back toward parking."""
    with tempfile.TemporaryDirectory() as d:
        root, env = _repo(d)
        _stage(root, env, "todo/00-infrastructure/TODO-01-x.md",
               BASE + "- [x] Route the request error after the generation check "
                      "(re-adversarial round 7)\n\n"
                      "> **Verified:** 2026-08-09 | commit `abc1234` | 2/2 items | build OK\n")
        rc, err = _run(root, env)
        assert rc == 0, (rc, err)


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok {name}")
    print("PASS: section provenance + park disposition")
