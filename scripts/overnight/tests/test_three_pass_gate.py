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
BODY = "# X\n\n## 7. Seven\n\n- [x] item\n\n## 8. Eight\n\n- [ ] item\n"
STAMP = "> **Verified:** 2026-11-11 by the three-pass gate test.\n"
README = "# Project\n\nSuites: <!-- project:stat_x -->1<!-- /project --> so far.\n\nHand-written text.\n"


def _git(root, *args):
    done = subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                           "-c", "core.hooksPath=/dev/null", *args], capture_output=True, text=True, timeout=60)
    assert done.returncode == 0, done.stderr
    return done.stdout.strip()


def _history(tmp_path):
    """plan-time commit, the build, the fix after the round, a tooling change, a TODO-only commit; and an
    unrelated branch."""
    root = tmp_path / "repo"
    root.mkdir()
    _git(root, "init", "-q", "-b", "main")
    heads = {}
    (root / "README.md").write_text(README)
    for name, path, text in (("plan", TODO, BODY), ("build", "src/kernel/a.c", "int a;\n"),
                             ("fix", "src/kernel/a.c", "int a = 1;\n"), ("tool", "scripts/x.sh", "echo x\n"),
                             ("docs", TODO, BODY + "\n")):
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


def _judge(root, stamp_under="7", also_stage=None, **stamps):
    """stamps: kind=(head, order) or (head, order, section). Stages a stamp under one section (or several,
    comma-separated), writes the stamp file, asks the gate about TODO, and puts the tree back."""
    now = time.time_ns()
    entry = {}
    for kind, spec in stamps.items():
        head, order = spec[0], spec[1]
        kind = kind.replace("_", "-")
        entry[kind] = now - (100 - order) * 1_000_000_000
        entry[f"{kind}_head"] = head
        entry[f"{kind}_section"] = spec[2] if len(spec) > 2 else "7"
    state = root / ".claude/state/last-review-stamps.json"
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text(json.dumps({TODO: entry}))
    text = (root / TODO).read_text()
    for n in stamp_under.split(","):
        marker = {"7": "## 7. Seven\n\n- [x] item\n", "8": "## 8. Eight\n\n- [ ] item\n"}[n]
        text = text.replace(marker, marker + "\n" + STAMP)
    (root / TODO).write_text(text)
    for path, body in (also_stage or {}).items():
        (root / path).parent.mkdir(parents=True, exist_ok=True)
        (root / path).write_text(body)
    _git(root, "add", "-A", "--", ":!.claude")
    try:
        return gate._four_dispatch_evidence(root, [TODO])
    finally:
        _git(root, "reset", "-q", "--hard", "HEAD")
        for path in (also_stage or {}):
            if (root / path).exists() and not _git(root, "ls-files", "--", path):
                (root / path).unlink()


def test_a_three_pass_section_is_accepted_on_its_shape(tmp_path):
    root, h = _history(tmp_path)
    # plan before the code, the round on the build, completion on the final candidate; HEAD adds only TODO text.
    assert _judge(root, plan=(h["plan"], 1), round=(h["build"], 2), completion=(h["tool"], 3)) == (True, "")
    assert _judge(root, plan=(h["plan"], 1), round=(h["build"], 2), completion=(h["docs"], 3)) == (True, "")
    # A completion with gaps, the repair, then a reconciliation on the repaired candidate.
    assert _judge(root, plan=(h["plan"], 1), round=(h["build"], 2), completion=(h["build"], 3),
                  reconciliation=(h["tool"], 4)) == (True, "")
    # Generated files, the TODO itself, and README's generated regions may ride along with the stamp.
    regenerated = README.replace("<!-- project:stat_x -->1<!-- /project -->", "<!-- project:stat_x -->2<!-- /project -->")
    assert _judge(root, also_stage={"COUNT.md": "counts\n", "README.md": regenerated,
                                    "docs/test-coverage/coverage.md": "c\n", "todo/00-x/notes.md": "n\n"},
                  plan=(h["plan"], 1), round=(h["build"], 2), completion=(h["docs"], 3)) == (True, "")


def test_refusal_each_missing_or_outdated_pass_is_named(tmp_path):
    root, h = _history(tmp_path)
    cases = (
        (dict(round=(h["build"], 2), completion=(h["tool"], 3)), "missing: plan"),
        (dict(plan=(h["plan"], 1), completion=(h["tool"], 3)), "missing: round"),
        (dict(plan=(h["plan"], 1), round=(h["build"], 2)), "missing: completion"),
        # The completion read the build; the fix came after it and nothing has read the fix.
        (dict(plan=(h["plan"], 1), round=(h["build"], 2), completion=(h["build"], 3)),
         "completion is not the last word on this candidate"),
        # A reconciliation that is newer but itself older than the last fix.
        (dict(plan=(h["plan"], 1), round=(h["build"], 2), completion=(h["tool"], 3),
              reconciliation=(h["build"], 4)), "reconciliation is not the last word on this candidate"),
        (dict(plan=(h["elsewhere"], 1), round=(h["build"], 2), completion=(h["tool"], 3)), "not in this history"),
        (dict(plan=(h["plan"], 1), round=(h["elsewhere"], 2), completion=(h["tool"], 3)), "not in this history"),
        (dict(plan=(h["plan"], 1), round=(h["build"], 2), completion=(h["elsewhere"], 3)), "not in this history"),
        (dict(plan=(h["plan"], 1), round=("", 2), completion=(h["tool"], 3)), "round has no dispatch head recorded"),
        # A reconciliation alone is not a completion.
        (dict(plan=(h["plan"], 1), round=(h["build"], 2), reconciliation=(h["tool"], 4)), "missing: completion"),
    )
    for stamps, want in cases:
        ok, why = _judge(root, **stamps)
        assert not ok and want in why and "three-pass review incomplete" in why, (stamps, why)
        assert "adversarial|consistency|perf" not in why, "a three-pass refusal must not point at the older kinds"


def test_refusal_a_tooling_change_after_the_last_pass_is_unread(tmp_path):
    """Review finding: freshness looked only at src/, include/, user/ and tools/, so a hook or script changed
    after the completion pass left it valid and a tooling section could be stamped over unreviewed changes."""
    root, h = _history(tmp_path)
    ok, why = _judge(root, plan=(h["plan"], 1), round=(h["build"], 2), completion=(h["fix"], 3))
    assert not ok and "changed since it ran: ['scripts/x.sh']" in why, why


def test_refusal_a_pass_for_another_section_does_not_count(tmp_path):
    """Review finding: the stamp entry is per TODO and keeps earlier passes, so section 8 could reuse the
    plan and round of section 7 with only a fresh completion of its own."""
    root, h = _history(tmp_path)
    ok, why = _judge(root, stamp_under="8", plan=(h["plan"], 1, "7"), round=(h["build"], 2, "7"),
                     completion=(h["tool"], 3, "8"))
    assert not ok and "plan was dispatched for section '7', not section 8" in why \
        and "round was dispatched for section '7', not section 8" in why, why
    assert _judge(root, stamp_under="8", plan=(h["plan"], 1, "8"), round=(h["build"], 2, "8"),
                  completion=(h["tool"], 3, "8")) == (True, "")
    ok, why = _judge(root, plan=(h["plan"], 1), round=(h["build"], 2), completion=(h["tool"], 3, "8"))
    assert not ok and "completion was dispatched for section '8', not section 7" in why, why
    ok, why = _judge(root, stamp_under="7,8", plan=(h["plan"], 1), round=(h["build"], 2), completion=(h["tool"], 3))
    assert not ok and "one commit stamps one section" in why, why
    assert gate._stamped_sections(root, TODO) is None, "nothing staged: no section to read"


def test_refusal_a_fix_staged_with_the_stamp_is_read_by_no_pass(tmp_path):
    """Review finding: a completion at HEAD was accepted without looking at the index, so a source fix
    staged together with the stamp was committed unreviewed."""
    root, h = _history(tmp_path)
    for path in ("src/kernel/a.c", "scripts/x.sh", ".githooks/pre-commit", "docs/new-page.md"):
        ok, why = _judge(root, also_stage={path: "changed with the stamp\n"},
                         plan=(h["plan"], 1), round=(h["build"], 2), completion=(h["docs"], 3))
        assert not ok and f"staged with the stamp but read by no pass: ['{path}']" in why, (path, why)


def test_an_older_section_is_judged_exactly_as_before(tmp_path):
    root, h = _history(tmp_path)
    assert _judge(root, adversarial=(h["fix"], 1), consistency=(h["fix"], 2), perf=(h["fix"], 3)) == (True, "")
    # The older rule reads only kernel-shaped paths and does not look at the index; both are unchanged.
    assert _judge(root, also_stage={"scripts/y.sh": "x\n"},
                  adversarial=(h["fix"], 1), consistency=(h["fix"], 2), perf=(h["fix"], 3)) == (True, "")
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


def test_refusal_an_exemption_covers_generated_content_only(tmp_path):
    """Review finding: README.md and everything under todo/ were exempt whole, so a hand edit to the README, or
    a script renamed into todo/, changed reviewed content with no pass reading it."""
    root, h = _history(tmp_path)
    good = dict(plan=(h["plan"], 1), round=(h["build"], 2), completion=(h["docs"], 3))
    # A hand edit to README.md staged with the stamp.
    ok, why = _judge(root, also_stage={"README.md": README.replace("Hand-written text.", "Changed by hand.")}, **good)
    assert not ok and "staged with the stamp but read by no pass: ['README.md']" in why, why
    # Something that is not Markdown placed under todo/.
    ok, why = _judge(root, also_stage={"todo/00-x/run-me.sh": "echo hi\n"}, **good)
    assert not ok and "'todo/00-x/run-me.sh'" in why, why
    # The same two, committed after the completion pass instead of staged.
    (root / "README.md").write_text(README.replace("Hand-written text.", "Changed by hand."))
    _git(root, "add", "README.md")
    _git(root, "commit", "-q", "-m", "hand edit")
    ok, why = _judge(root, **good)
    assert not ok and "changed since it ran: ['README.md']" in why, why
    # A reviewed script renamed into todo/: its removal from where it ran is listed.
    _git(root, "mv", "scripts/x.sh", "todo/00-x/x.md")
    _git(root, "commit", "-q", "-m", "moved")
    ok, why = _judge(root, plan=(h["plan"], 1), round=(h["build"], 2),
                     completion=(_git(root, "rev-parse", "HEAD~1"), 3))
    assert not ok and "'scripts/x.sh'" in why, why
