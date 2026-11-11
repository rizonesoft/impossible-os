#!/usr/bin/env python3
"""The commit gate judges a section reviewed under the reviewer pin by that shape, and an older one as before.

docs/specs/2026-11-11-three-pass-review-design.md: plan before the code; then a round, a performance pass and a
completion pass, each of which must have read the candidate being stamped and ended below a blocking level.
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
STAMP = "> **Verified:** 2026-11-11 by the gate test.\n"
README = "# Project\n\nSuites: <!-- project:stat_x -->1<!-- /project --> so far.\n\nHand-written text.\n"
CHECKED = ("round", "performance", "completion", "reconciliation")


def _git(root, *args):
    done = subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                           "-c", "core.hooksPath=/dev/null", *args], capture_output=True, text=True, timeout=60)
    assert done.returncode == 0, done.stderr
    return done.stdout.strip()


def _history(tmp_path):
    """plan-time commit, the build, the fix after the first round, a tooling change, a TODO-only commit; and an
    unrelated branch. `final` is the last commit that changes anything a pass reads."""
    root = tmp_path / "repo"
    root.mkdir()
    _git(root, "init", "-q", "-b", "main")
    (root / "README.md").write_text(README)
    (root / ".conclave").mkdir()
    (root / ".conclave/reviewers.toml").write_text((REPO / ".conclave/reviewers.toml").read_text())
    heads = {}
    for name, path, text in (("plan", TODO, BODY), ("build", "src/kernel/a.c", "int a;\n"),
                             ("fix", "src/kernel/a.c", "int a = 1;\n"), ("final", "scripts/x.sh", "echo x\n"),
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


def _judge(root, stamp_under="7", also_stage=None, highest=None, receipts=True, **stamps):
    """stamps: kind=(head, order) or (head, order, section). Writes the stamp file and, for every checked pass,
    the dispatcher's receipt (worst level `none` unless `highest` names another); stages a stamp under one
    section (or several, comma-separated); asks the gate about TODO; puts the tree back."""
    now = time.time_ns()
    entry, lines = {}, []
    for kind, spec in stamps.items():
        head, order = spec[0], spec[1]
        section = spec[2] if len(spec) > 2 else "7"
        kind = kind.replace("_", "-")
        entry[kind] = now - (100 - order) * 1_000_000_000
        entry[f"{kind}_head"] = head
        entry[f"{kind}_section"] = section
        if kind in CHECKED and receipts:
            lines.append(json.dumps({"kind": kind, "todo": TODO, "section": int(section), "head": head,
                                     "jobId": f"direct:{order}", "route": "codex-dispatch.sh", "rc": 0,
                                     "pass_checked": True, "highest": (highest or {}).get(kind, "none")}))
    state = root / ".claude/state/last-review-stamps.json"
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text(json.dumps({TODO: entry}))
    manifest = root / gate._PASS_MANIFEST
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text("\n".join(lines) + ("\n" if lines else ""))
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


def _good(h, **over):
    """A section whose plan preceded the code and whose three checked passes read the final candidate."""
    passes = dict(plan=(h["plan"], 1), round=(h["final"], 2), performance=(h["final"], 3), completion=(h["final"], 4))
    passes.update(over)
    return {k: v for k, v in passes.items() if v is not None}


def test_a_section_is_accepted_when_every_pass_read_the_candidate_and_ended_clean(tmp_path):
    root, h = _history(tmp_path)
    assert _judge(root, **_good(h)) == (True, "")
    # Dispatched at HEAD itself, which adds only TODO text over `final`.
    assert _judge(root, **_good(h, completion=(h["docs"], 4))) == (True, "")
    # A completion with gaps, the repair, then a clean reconciliation on the repaired candidate.
    assert _judge(root, highest={"completion": "P2"},
                  **_good(h, completion=(h["build"], 4), reconciliation=(h["final"], 5))) == (True, "")
    # A finding below the blocking levels is filed, not fixed: it does not hold the stamp.
    assert _judge(root, highest={"round": "P3", "performance": "P3", "completion": "P3"}, **_good(h)) == (True, "")
    # Generated files, the TODO itself, and README's generated regions may ride along with the stamp.
    regenerated = README.replace("<!-- project:stat_x -->1<!-- /project -->", "<!-- project:stat_x -->2<!-- /project -->")
    assert _judge(root, also_stage={"COUNT.md": "counts\n", "README.md": regenerated,
                                    "docs/test-coverage/coverage.md": "c\n", "todo/00-x/notes.md": "n\n"},
                  **_good(h)) == (True, "")


def test_refusal_each_missing_or_outdated_pass_is_named(tmp_path):
    root, h = _history(tmp_path)
    cases = (
        (_good(h, plan=None), "missing: plan"),
        (_good(h, round=None), "missing: round"),
        (_good(h, performance=None), "missing: performance"),
        (_good(h, completion=None), "missing: completion"),
        # A pass that read the build; fixes came after it and that pass has not read them.
        (_good(h, round=(h["build"], 2)), "round is not the last word on this candidate"),
        (_good(h, performance=(h["build"], 3)), "performance is not the last word on this candidate"),
        (_good(h, completion=(h["build"], 4)), "completion is not the last word on this candidate"),
        # A reconciliation that is newer but itself older than the last fix.
        (_good(h, reconciliation=(h["build"], 5)), "reconciliation is not the last word on this candidate"),
        (_good(h, plan=(h["elsewhere"], 1)), "not in this history"),
        (_good(h, round=(h["elsewhere"], 2)), "not in this history"),
        (_good(h, completion=(h["elsewhere"], 4)), "not in this history"),
        (_good(h, round=("", 2)), "round has no dispatch head recorded"),
        # A reconciliation alone is not a completion.
        (_good(h, completion=None, reconciliation=(h["final"], 5)), "missing: completion"),
    )
    for stamps, want in cases:
        ok, why = _judge(root, **stamps)
        assert not ok and want in why and "three-pass review incomplete" in why, (stamps, why)
        assert "adversarial|consistency|perf" not in why, "a refusal here must not point at the older kinds"


def test_refusal_a_pass_that_ended_at_a_blocking_level_does_not_count(tmp_path):
    """A completion that returned gaps, or a round with a P1 still open, is not the last word."""
    root, h = _history(tmp_path)
    for kind in ("round", "performance", "completion"):
        for level in ("P1", "P2"):
            ok, why = _judge(root, highest={kind: level}, **_good(h))
            assert not ok and f"{kind} ended at {level}" in why, (kind, level, why)
    ok, why = _judge(root, highest={"reconciliation": "P1"}, **_good(h, reconciliation=(h["final"], 5)))
    assert not ok and "reconciliation ended at P1" in why, why
    ok, why = _judge(root, highest={"completion": "sort of fine"}, **_good(h))
    assert not ok and "records no HIGHEST level" in why, why


def test_refusal_a_pass_without_the_dispatchers_receipt_does_not_count(tmp_path):
    """A stamp says a dispatch was recorded. Only the receipt says the pass was checked and how it ended."""
    root, h = _history(tmp_path)
    ok, why = _judge(root, receipts=False, **_good(h))
    assert not ok and "round has no checked, complete receipt" in why \
        and "performance has no checked, complete receipt" in why \
        and "completion has no checked, complete receipt" in why, why
    # A receipt for another dispatch head is not this pass's receipt.
    _judge(root, **_good(h))
    manifest = root / gate._PASS_MANIFEST
    manifest.write_text(manifest.read_text().replace(h["final"], h["build"]))
    state = json.loads((root / ".claude/state/last-review-stamps.json").read_text())
    assert gate._pass_receipt(root, TODO, "7", "round", h["final"]) is None
    assert gate._pass_receipt(root, TODO, "7", "round", h["build"])["kind"] == "round"
    assert state[TODO]["round_head"] == h["final"]


def test_refusal_a_tooling_change_after_a_pass_is_unread(tmp_path):
    """Review finding: freshness looked only at src/, include/, user/ and tools/, so a hook or script changed
    after the completion pass left it valid and a tooling section could be stamped over unreviewed changes."""
    root, h = _history(tmp_path)
    ok, why = _judge(root, **_good(h, completion=(h["fix"], 4)))
    assert not ok and "changed since it ran: ['scripts/x.sh']" in why, why


def test_refusal_a_pass_for_another_section_does_not_count(tmp_path):
    """Review finding: the stamp entry is per TODO and keeps earlier passes, so section 8 could reuse the
    plan and round of section 7 with only a fresh completion of its own."""
    root, h = _history(tmp_path)
    ok, why = _judge(root, stamp_under="8", **_good(h, completion=(h["final"], 4, "8")))
    assert not ok and "plan was dispatched for section '7', not section 8" in why \
        and "round was dispatched for section '7', not section 8" in why, why
    eight = {k: (v[0], v[1], "8") for k, v in _good(h).items()}
    assert _judge(root, stamp_under="8", **eight) == (True, "")
    ok, why = _judge(root, **_good(h, completion=(h["final"], 4, "8")))
    assert not ok and "completion was dispatched for section '8', not section 7" in why, why
    ok, why = _judge(root, stamp_under="7,8", **_good(h))
    assert not ok and "one commit stamps one section" in why, why
    assert gate._stamped_sections(root, TODO) is None, "nothing staged: no section to read"


def test_refusal_a_fix_staged_with_the_stamp_is_read_by_no_pass(tmp_path):
    """Review finding: a completion at HEAD was accepted without looking at the index, so a source fix
    staged together with the stamp was committed unreviewed."""
    root, h = _history(tmp_path)
    for path in ("src/kernel/a.c", "scripts/x.sh", ".githooks/pre-commit", "docs/new-page.md"):
        ok, why = _judge(root, also_stage={path: "changed with the stamp\n"}, **_good(h))
        assert not ok and f"staged with the stamp but read by no pass: ['{path}']" in why, (path, why)


def test_refusal_an_exemption_covers_generated_content_only(tmp_path):
    """Review finding: README.md and everything under todo/ were exempt whole, so a hand edit to the README, or
    a script renamed into todo/, changed reviewed content with no pass reading it."""
    root, h = _history(tmp_path)
    ok, why = _judge(root, also_stage={"README.md": README.replace("Hand-written text.", "Changed by hand.")},
                     **_good(h))
    assert not ok and "staged with the stamp but read by no pass: ['README.md']" in why, why
    ok, why = _judge(root, also_stage={"todo/00-x/run-me.sh": "echo hi\n"}, **_good(h))
    assert not ok and "'todo/00-x/run-me.sh'" in why, why
    # The same, committed after the passes instead of staged.
    (root / "README.md").write_text(README.replace("Hand-written text.", "Changed by hand."))
    _git(root, "add", "README.md")
    _git(root, "commit", "-q", "-m", "hand edit")
    ok, why = _judge(root, **_good(h))
    assert not ok and "changed since it ran: ['README.md']" in why, why
    # A reviewed script renamed into todo/: its removal from where it ran is listed.
    before = _git(root, "rev-parse", "HEAD")
    _git(root, "mv", "scripts/x.sh", "todo/00-x/x.md")
    _git(root, "commit", "-q", "-m", "moved")
    ok, why = _judge(root, **_good(h, round=(before, 2), performance=(before, 3), completion=(before, 4)))
    assert not ok and "'scripts/x.sh'" in why, why


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
                     round=(h["final"], 4))
    assert not ok and "missing: plan" in why and "missing: completion" in why \
        and "missing: performance" in why, why
    assert gate._three_pass_shape({"plan": 5}) and gate._three_pass_shape({"performance": 5})
    assert not gate._three_pass_shape({"adversarial": 5, "plan": 0})
    assert not gate._three_pass_shape({"plan": "5"}) and not gate._three_pass_shape({})


def test_a_broken_pin_does_not_open_the_gate(tmp_path):
    root, h = _history(tmp_path)
    assert gate._blocking_levels(root) == ("P1", "P2")
    (root / ".conclave/reviewers.toml").write_text("schema_version = [")
    assert gate._blocking_levels(root) == gate._BLOCKING_FALLBACK
    ok, why = _judge(root, highest={"completion": "P2"}, **_good(h))
    assert not ok and "completion ended at P2" in why, why
