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
sys.path.insert(0, str(REPO / "scripts"))  # review_pin, which reads each test repo's own pin
import section_commit_gate as gate  # noqa: E402

TODO = "todo/00-x/TODO-01-x.md"
BODY = "# X\n\n## 7. Seven\n\n- [x] item\n\n## 8. Eight\n\n- [ ] item\n"
STAMP = "> **Verified:** 2026-11-11 by the gate test.\n"
README = "# Project\n\nSuites: <!-- project:stat_x -->1<!-- /project --> so far.\n\nHand-written text.\n"
PASSES = ("plan", "round", "performance", "completion", "reconciliation")


def _pin():
    """The repository's own pin."""
    return (REPO / ".conclave/reviewers.toml").read_text()


EMPTY_TREE = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"


def _git(root, *args):
    done = subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                           "-c", "core.hooksPath=/dev/null", *args], capture_output=True, text=True, timeout=60)
    assert done.returncode == 0, done.stderr
    return done.stdout.strip()


def _history(tmp_path, source="src/kernel/a.c"):
    """plan-time commit, the build, the fix after the first round, a tooling change, a TODO-only commit; and an
    unrelated branch. `final` is the last commit that changes anything a pass reads."""
    root = tmp_path / "repo"
    root.mkdir()
    _git(root, "init", "-q", "-b", "main")
    (root / "README.md").write_text(README)
    (root / ".conclave").mkdir()
    (root / ".conclave/reviewers.toml").write_text(_pin())
    heads = {}
    for name, path, text in (("plan", TODO, BODY), ("build", source, "int a;\n"),
                             ("fix", source, "int a = 1;\n"), ("final", "scripts/x.sh", "echo x\n"),
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


# The dispatcher's digest of a candidate with nothing uncommitted (review_pin.CLEAN_WORKTREE).
CLEAN = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"


def _receipt(kind, head, order, section="7", **over):
    line = {"kind": kind, "todo": TODO, "section": int(section), "head": head, "jobId": f"direct:{order}",
            "route": "codex-dispatch.sh", "rc": 0, "pass_checked": True, "highest": "none",
            "worktree_sha256": CLEAN, "base": EMPTY_TREE}
    line.update(over)
    return line


def _judge(root, stamp_under="7", also_stage=None, highest=None, receipt=None, manifest_tail="",
           manifest_head="", stamp=STAMP, **stamps):
    """stamps: kind=(head, order) or (head, order, section). A pass kind becomes the dispatcher's receipt in the
    reviews manifest, oldest first by `order` (worst level `none` unless `highest` names another; `receipt` gives
    per-kind overrides). An older kind becomes a stamp. Stages a stamp under one section (or several,
    comma-separated), asks the gate about TODO, and puts the tree back."""
    now = time.time_ns()
    entry, receipts = {}, []
    for kind, spec in stamps.items():
        head, order = spec[0], spec[1]
        section = spec[2] if len(spec) > 2 else "7"
        kind = kind.replace("_", "-")
        if kind in PASSES:
            over = dict((receipt or {}).get(kind, {}))
            if kind in (highest or {}):
                over["highest"] = highest[kind]
            line = _receipt(kind, head, order, section)
            line.update(over)
            receipts.append((order, line))
        else:
            entry[kind] = now - (100 - order) * 1_000_000_000
            entry[f"{kind}_head"] = head
            entry[f"{kind}_section"] = section
    state = root / ".claude/state/last-review-stamps.json"
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text(json.dumps({TODO: entry} if entry else {}))
    manifest = root / gate._PASS_MANIFEST
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(manifest_head + "".join(json.dumps(r) + "\n" for _, r in sorted(receipts, key=lambda x: x[0]))
                        + manifest_tail)
    text = (root / TODO).read_text()
    for n in stamp_under.split(","):
        marker = {"7": "## 7. Seven\n\n- [x] item\n", "8": "## 8. Eight\n\n- [ ] item\n"}[n]
        text = text.replace(marker, marker + "\n" + stamp)
    (root / TODO).write_text(text)
    for path, body in (also_stage or {}).items():
        (root / path).parent.mkdir(parents=True, exist_ok=True)
        (root / path).write_text(body)
    _git(root, "add", "-A", "--", ":!.claude")
    gate._commit_diff_deadline.clear()      # each judgement is its own invocation of the hook
    gate._staged_paths(root, fresh=True)     # and reads the index it was given, as a detection does
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
    # Every level blocks in the repository pin (operator, 2026-11-21): a P3 holds the stamp.
    ok, why = _judge(root, highest={"round": "P3", "performance": "P3", "completion": "P3"}, **_good(h))
    assert not ok and "ended at P3" in why, why
    # A pin that blocks only P1 and P2 files a P3 instead: it does not hold the stamp. (The pin is stubbed, not
    # edited: an edited pin is staged with the stamp, which the gate rightly refuses as unreviewed.)
    real = gate._blocking_levels
    gate._blocking_levels = lambda _root: ("P1", "P2")
    try:
        assert _judge(root, highest={"round": "P3", "performance": "P3", "completion": "P3"}, **_good(h)) == (True, "")
    finally:
        gate._blocking_levels = real
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
        (_good(h, round=("", 2)), "round has no checked, complete receipt"),
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


def test_a_section_that_spent_the_pass_cap_needs_no_clean_round(tmp_path):
    """At pass_cap the round loop ends (the spec); the performance and last passes still read the candidate."""
    root, h = _history(tmp_path)
    cap = int(gate._pass_cap(root))
    def spent(n):  # n earlier checked rounds that read the build and ended at P2
        return "".join(json.dumps(_receipt("round", h["build"], 0, highest="P2")) + "\n" for _ in range(n))
    stuck = _good(h, round=(h["build"], 2))
    ok, why = _judge(root, highest={"round": "P2"}, manifest_head=spent(cap - 5), **stuck)
    assert not ok and "round" in why, why  # 4 passes short of the cap: the round still holds the stamp
    ok, why = _judge(root, highest={"round": "P2"}, manifest_head=spent(cap - 4), **stuck)
    assert ok, why
    for kind in ("performance", "completion"):  # neither is waived
        ok, why = _judge(root, highest={"round": "P2"}, manifest_head=spent(cap),
                         **_good(h, round=(h["build"], 2), **{kind: (h["build"], 3)}))
        assert not ok and f"{kind} is not the last word" in why, (kind, why)
    ok, why = _judge(root, highest={"round": "P2", "performance": "P2"}, manifest_head=spent(cap), **stuck)
    assert not ok and "performance ended at P2" in why, why


def test_refusal_only_a_checked_complete_receipt_from_the_dispatcher_counts(tmp_path):
    """A receipt is what scripts/codex-dispatch.sh writes once it has checked a pass. A line the broker writes
    when a dispatch starts, an incomplete pass, and a line from any other route are not receipts."""
    root, h = _history(tmp_path)
    for kind in ("round", "performance", "completion"):
        for bad in ({"pass_checked": False}, {"rc": 3}, {"route": "review-broker"}, {"head": ""}):
            ok, why = _judge(root, receipt={kind: bad}, **_good(h))
            assert not ok and f"{kind} has no checked, complete receipt" in why, (kind, bad, why)
    ok, why = _judge(root, receipt={"plan": {"rc": 3}}, **_good(h))
    assert not ok and "plan has no checked, complete receipt" in why, why
    # A pass dispatched again and still running: the newest line for it is the one that decides.
    running = json.dumps({"kind": "round", "todo": TODO, "section": 7, "head": h["final"], "jobId": "systemd-run:x"})
    ok, why = _judge(root, manifest_tail=running + "\n", **_good(h))
    assert not ok and "round has no checked, complete receipt" in why, why


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
    assert not ok and "missing: plan" in why and "missing: round" in why and "missing: performance" in why, why
    eight = {k: (v[0], v[1], "8") for k, v in _good(h).items()}
    assert _judge(root, stamp_under="8", **eight) == (True, "")
    ok, why = _judge(root, **_good(h, completion=(h["final"], 4, "8")))
    assert not ok and "missing: completion" in why, why
    ok, why = _judge(root, stamp_under="7,8", **_good(h))
    assert not ok and "one commit stamps one section" in why, why
    gate._staged_paths(root, fresh=True)
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


def test_refusal_one_pass_puts_the_section_on_the_new_rules(tmp_path):
    """The three older stamps cannot be topped up with a single pass, nor a pass set with older kinds."""
    root, h = _history(tmp_path)
    ok, why = _judge(root, adversarial=(h["fix"], 1), consistency=(h["fix"], 2), perf=(h["fix"], 3),
                     round=(h["final"], 4))
    assert not ok and "missing: plan" in why and "missing: completion" in why \
        and "missing: performance" in why, why
    # Passes for another section of the same TODO leave this one on the older rules it started with.
    assert _judge(root, adversarial=(h["fix"], 1), consistency=(h["fix"], 2), perf=(h["fix"], 3),
                  round=(h["final"], 4, "8")) == (True, "")


def test_a_fix_commit_in_a_reviewed_todo_is_not_held_for_a_re_adversarial(tmp_path):
    root, h = _history(tmp_path)
    _judge(root, plan=(h["plan"], 1))
    assert gate._pass_receipts(root, TODO, None)[0], "the plan receipt marks the TODO as reviewed under the pin"
    assert not gate._pass_receipts(root, "todo/other.md", None)[0]


def test_a_broken_pin_does_not_open_the_gate(tmp_path):
    root, h = _history(tmp_path)
    assert gate._blocking_levels(root) == ("P1", "P2", "P3")
    (root / ".conclave/reviewers.toml").write_text("schema_version = [")
    assert gate._blocking_levels(root) == gate._BLOCKING_FALLBACK
    ok, why = _judge(root, highest={"completion": "P2"}, **_good(h))
    assert not ok and "completion ended at P2" in why, why


def test_refusal_a_receipt_behind_an_unreadable_line_is_not_trusted(tmp_path):
    """Review finding: a newer blocking receipt cut short while it was written was skipped, and the older
    clean receipt behind it carried the stamp."""
    root, h = _history(tmp_path)
    assert _judge(root, **_good(h)) == (True, "")
    cut = '{"kind": "completion", "todo": "' + TODO + '", "section": 7, "head": "' + h["final"] + '", "rc": 0, "pas'
    ok, why = _judge(root, manifest_tail=cut, **_good(h))
    assert not ok and "cannot be read past a line" in why and "missing: completion" in why, why
    assert _judge(root, manifest_tail="\n  \n", **_good(h)) == (True, ""), "blank lines are not receipts"
    # Cut too short to say which TODO it was for: it may be this one's.
    ok, why = _judge(root, manifest_tail='{"ts": 17943', **_good(h))
    assert not ok and "cannot be read past a line" in why, why
    # An unreadable line that names another TODO is not this one's.
    other = '{"ts": 1795170000, "kind": "completion", "todo": "todo/00-x/TODO-02-other.md", "section": 7, "rc": 0, "pas'
    assert _judge(root, manifest_tail=other + "\n", **_good(h)) == (True, "")
    # Cut inside the todo value, or just before its closing quote: it may still be this one's (review 2026-11-20).
    for cut in ('{"kind": "completion", "todo": "' + TODO[:8], '{"kind": "completion", "todo": "' + TODO):
        ok, why = _judge(root, manifest_tail=cut, **_good(h))
        assert not ok and "cannot be read past a line" in why, (cut, why)
    # Another TODO's receipt torn before its newline, with this TODO's newer one glued on (review 2026-11-20).
    glued = other + '"}{"kind": "completion", "todo": "' + TODO + '", "section": 7, "rc": 0, "highest": "P2"}'
    ok, why = _judge(root, manifest_tail=glued, **_good(h))
    assert not ok and "cannot be read past a line" in why, why
    # This TODO's newer receipt torn inside its timestamp, another TODO's appended to it (review 2026-11-20).
    stamp_cut = '{"ts": 1795170000' + other.replace('"pas', '"pass_checked": true}')
    ok, why = _judge(root, manifest_tail=stamp_cut, **_good(h))
    assert not ok and "cannot be read past a line" in why, why
    # Another TODO's write cut after its section, this TODO's append cut inside the opening ts key.
    ok, why = _judge(root, manifest_tail=other.split(', "rc"')[0] + '{"ts', **_good(h))
    assert not ok and "cannot be read past a line" in why, why


def test_passes_dispatched_again_after_old_corruption_are_accepted(tmp_path):
    """Review finding: an unreadable line OLDER than every required receipt still refused the stamp, so
    dispatching the passes again, the remedy the message offered, never worked."""
    root, h = _history(tmp_path)
    assert _judge(root, manifest_head="{cut short\n", **_good(h)) == (True, "")
    # A pass that is NOT among the newer receipts would have to be looked for past the line.
    ok, why = _judge(root, manifest_head="{cut short\n", **_good(h, performance=None))
    assert not ok and "cannot be read past a line" in why and "missing: performance" in why, why
    # With no receipt of its own in the readable part, a TODO cannot be called an older-shape one either.
    ok, why = _judge(root, manifest_tail="{cut short", adversarial=(h["fix"], 1), consistency=(h["fix"], 2),
                     perf=(h["fix"], 3))
    assert not ok and "cannot be read past a line" in why, why


def test_refusal_a_manifest_that_exists_and_cannot_be_read_is_not_an_empty_one(tmp_path):
    """Review finding: any read error was taken for "no passes", which sent a pass-reviewed section to the
    older rules, where leftover stamps could carry it."""
    import os
    root, h = _history(tmp_path)
    legacy = dict(adversarial=(h["fix"], 1), consistency=(h["fix"], 2), perf=(h["fix"], 3))
    assert _judge(root, **legacy) == (True, "")
    manifest = root / gate._PASS_MANIFEST
    assert gate._pass_receipts(root, TODO, None) == ({}, True)
    manifest.unlink()
    assert gate._pass_receipts(root, TODO, None) == ({}, True), "a manifest that was never written"
    manifest.mkdir()                                  # exists, and reading it fails
    try:
        assert gate._pass_receipts(root, TODO, None) == ({}, False)
    finally:
        manifest.rmdir()
    manifest.write_bytes(b"\xff\xfe not text \xff\n")
    assert gate._pass_receipts(root, TODO, None) == ({}, False)
    if os.geteuid() != 0:
        manifest.write_text("")
        manifest.chmod(0)
        try:
            assert gate._pass_receipts(root, TODO, None) == ({}, False)
        finally:
            manifest.chmod(0o644)


def test_refusal_another_sections_receipt_does_not_lift_the_fix_commit_hold(tmp_path, monkeypatch):
    """Review finding: a plan receipt for section 7 released a locking fix in section 8, an older-shape
    section, from the re-adversarial hold, and nothing later made up for it."""
    root, h = _history(tmp_path)
    for name in ("_skill_entry_stale", "_entry_is_foreign"):
        monkeypatch.setattr(gate, name, lambda *a, **k: False)
    monkeypatch.setattr(gate, "_skill_entry_finished", lambda *a, **k: False)
    progress = root / ".claude/state/skill-progress.json"

    def active(section_args):
        progress.parent.mkdir(parents=True, exist_ok=True)
        progress.write_text(json.dumps({"review-todo-section": {"todo_path": TODO, "args": f"{TODO} {section_args}"}}))

    def hold():
        (root / "src/kernel/a.c").write_text("int a = 1;\nspinlock_t sa_lock;\n")
        _git(root, "add", "src/kernel/a.c")
        try:
            return gate._re_adversarial_trigger_check(root, ["src/kernel/a.c"], [])
        finally:
            _git(root, "reset", "-q", "--hard", "HEAD")

    manifest = root / gate._PASS_MANIFEST
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps(_receipt("plan", h["plan"], 1, "7")) + "\n")
    active("section 8")
    (root / ".claude/state/last-review-stamps.json").write_text("{}")
    assert gate._active_section(root) == (TODO, "8")
    ok, why = hold()
    assert not ok and "re-adversarial" in why, why
    active("section 7")
    assert hold() == (True, "")
    active("with no section named")
    assert gate._active_section(root) == (TODO, "")
    ok, _why = hold()
    assert not ok, "a section that cannot be identified keeps the hold"
    # Two identifiers that disagree identify nothing (the confirmation pass: section=7 with args naming 8
    # resolved to 7 and released section 8).
    progress.write_text(json.dumps({"review-todo-section": {"todo_path": TODO, "section": 7,
                                                             "args": f"{TODO} section 8"}}))
    assert gate._active_section(root) == (TODO, "")
    ok, _why = hold()
    assert not ok
    progress.write_text(json.dumps({"review-todo-section": {"todo_path": TODO, "section": "7",
                                                             "args": f"{TODO} section 7"}}))
    assert gate._active_section(root) == (TODO, "7") and hold() == (True, "")
    # A broker's registration of a job that was then refused is not a pass.
    manifest.write_text(json.dumps({"kind": "plan", "todo": TODO, "section": 7, "head": h["plan"],
                                    "jobId": "detached:codex-rev-plan-x"}) + "\n")
    ok, why = hold()
    assert not ok and "re-adversarial" in why, why


def test_one_comparison_of_two_commits_is_run_once_and_given_a_short_wait(tmp_path, monkeypatch):
    root, h = _history(tmp_path)
    gate._commit_diffs.clear()
    real, seen = subprocess.check_output, []

    def counting(cmd, **kw):
        if cmd[:2] == ["git", "diff"] and "--cached" not in cmd:
            seen.append((tuple(cmd), kw.get("timeout")))
        return real(cmd, **kw)

    monkeypatch.setattr(gate._proc, "check_output", counting)
    # Three routed passes at one commit before HEAD: one list of changed files serves all of them.
    assert _judge(root, **_good(h)) == (True, "")
    commands = [cmd for cmd, _ in seen]
    assert commands and len(commands) == len(set(commands)), "a comparison of the same two commits ran twice"
    assert all(wait == gate._COMMIT_DIFF_TIMEOUT for _, wait in seen)
    # A comparison that cannot be made is said so; it is not a pass.
    gate._commit_diffs.clear()
    monkeypatch.setattr(gate, "_commit_diff", lambda *a, **k: None)
    ok, why = _judge(root, **_good(h))
    assert not ok and "could not list what changed" in why, why


def test_the_comparisons_share_one_budget_and_stop_when_it_is_spent(tmp_path, monkeypatch):
    root, h = _history(tmp_path)
    # Every routed pass at a different commit, and every comparison timing out: three of them at three seconds
    # each would outlast the hook.
    apart = _good(h, round=(h["build"], 2), performance=(h["fix"], 3), completion=(h["final"], 4))
    gate._commit_diffs.clear()
    gate._commit_diff_deadline.clear()
    clock, waits = [100.0], []

    def slow(_root, *args, timeout=10):
        waits.append(timeout)
        clock[0] += timeout          # it used its whole wait and gave nothing
        return None

    monkeypatch.setattr(gate.time, "monotonic", lambda: clock[0])
    monkeypatch.setattr(gate, "_git_lines", lambda root, *args, timeout=10: (
        slow(root, *args, timeout=timeout) if args[:1] == ("diff",) and "--cached" not in args else
        gate.subprocess.check_output(["git", *args], cwd=str(root), text=True).splitlines()))
    ok, why = _judge(root, **apart)
    assert not ok and "could not" in why, why
    assert sum(waits) <= gate._COMMIT_DIFF_BUDGET and waits == [3, 3], waits
    gate._commit_diffs.clear()
    gate._commit_diff_deadline.clear()


def test_a_section_built_during_an_outage_is_judged_from_its_recorded_base(tmp_path):
    root, h = _history(tmp_path)
    # Built while the reviewer was down, so its plan pass came late: at `final`, after the code.
    late = _good(h, plan=(h["final"], 1), round=(h["docs"], 2), performance=(h["docs"], 3), completion=(h["docs"], 4))
    assert _judge(root, **late) == (True, "")
    owed = root / ".claude/state/review-owed.json"
    owed.write_text(json.dumps([{"todo": TODO, "section": 7, "base": h["plan"], "since": 1}]))
    shown = {k: {"base": h["plan"]} for k in ("round", "performance", "completion")}
    try:
        # The routed reviewer's own passes were based at the late plan pass: they read nothing of the section.
        ok, why = _judge(root, receipt={k: {"base": h["final"]} for k in shown}, **late)
        assert not ok and "round was shown this section from" in why and "performance was shown" in why, why
        # And a receipt that names no base at all is not evidence either.
        ok, why = _judge(root, **late)
        assert not ok and "completion was shown this section from" in why, why
        assert _judge(root, receipt=shown, **late) == (True, "")
        # A record that cannot be read is said so; it is not read as nothing owed.
        owed.write_text("{")
        ok, why = _judge(root, receipt=shown, **late)
        assert not ok and "where this section started could not be read" in why, why
    finally:
        owed.unlink()


def test_refusal_a_stamp_needs_a_pin_that_loads(tmp_path):
    """The blocking levels and the pass cap fall back when the pin cannot be read; the stamp must not."""
    root, h = _history(tmp_path)
    pin = root / ".conclave/reviewers.toml"
    good = pin.read_text()
    cases = (
        (None, "could not be read"),
        ("schema_version = [", "could not be read"),
        # The second reviewer was removed: a pin that still names one is refused, never quietly half-read.
        (good + '\n[second]\nreviewer = "carbon"\n', "could not be read"),
    )
    try:
        for text, want in cases:
            if text is None:
                pin.unlink()
            else:
                pin.write_text(text)
            _git(root, "add", "-A", "--", ".conclave")
            _git(root, "commit", "-q", "--allow-empty", "-m", "pin")
            head = _git(root, "rev-parse", "HEAD")
            ok, why = _judge(root, **_good(h, round=(head, 2), performance=(head, 3), completion=(head, 4)))
            assert not ok and want in why and "reviewer pin" in why, (text, why)
            _git(root, "reset", "-q", "--hard", "HEAD~1")
    finally:
        pin.write_text(good)
    assert _judge(root, **_good(h)) == (True, "")




def test_refusal_a_pass_that_read_uncommitted_changes_does_not_count(tmp_path):
    """Those changes can be discarded before the stamp is staged, so the pass reviewed bytes it never carries."""
    root, h = _history(tmp_path)
    for kind in ("round", "performance", "completion"):
        ok, why = _judge(root, receipt={kind: {"worktree_sha256": "f" * 64}}, **_good(h))
        assert not ok and f"{kind} read uncommitted changes" in why, (kind, why)


def test_refusal_a_replacement_ref_does_not_put_a_pass_in_this_history(tmp_path):
    """A replace ref that grafts another branch under this history: the gate walks stored objects, as the
    candidate reader does, so a pass from that branch is still not in this history."""
    root, h = _history(tmp_path)
    _git(root, "replace", "--graft", h["plan"], h["elsewhere"])
    ok, why = _judge(root, **_good(h, plan=(h["elsewhere"], 1)))
    assert not ok and "not in this history" in why, why


def test_refusal_a_graft_file_refuses_the_stamp(tmp_path):
    root, h = _history(tmp_path)
    (root / ".git/info").mkdir(exist_ok=True)
    (root / ".git/info/grafts").write_text(f"{h['plan']} {h['elsewhere']}\n")
    ok, why = _judge(root, **_good(h))
    assert not ok and "graft file" in why, why


def test_refusal_a_pass_shown_the_section_from_past_its_start(tmp_path):
    """A caller's REVIEW_BASE at the section's second commit: the reviewer saw build..final and never the build
    commit itself, so the pass does not cover the section even though its receipt names the candidate."""
    root, h = _history(tmp_path)
    late = {k: {"base": h["build"]} for k in ("round", "performance", "completion")}
    ok, why = _judge(root, receipt=late, **_good(h))
    assert not ok and "not at or before where it starts" in why, why
    early = {k: {"base": h["plan"]} for k in ("round", "performance", "completion")}
    assert _judge(root, receipt=early, **_good(h)) == (True, "")
    none = {"round": {"base": None}}
    ok, why = _judge(root, receipt=none, **_good(h))
    assert not ok and "no recorded base" in why, why


def test_refusal_a_round_at_the_pass_cap_is_still_held_to_the_section_start(tmp_path):
    """At the pass cap the round's level stops holding the stamp; where it was shown the section from does not."""
    root, h = _history(tmp_path)
    real = gate._pass_cap
    gate._pass_cap = lambda _root: 1
    try:
        late = {"round": {"base": h["build"]}}
        ok, why = _judge(root, receipt=late, highest={"round": "P2"}, **_good(h))
        assert not ok and "round was shown this section from" in why, why
        assert _judge(root, highest={"round": "P2"}, **_good(h)) == (True, "")
    finally:
        gate._pass_cap = real


def test_the_graft_lookup_is_made_once_and_briefly(tmp_path):
    root, _h = _history(tmp_path)
    calls = []
    real = gate._git_lines
    gate._git_lines = lambda r, *a, **k: (calls.append(k.get("timeout")), real(r, *a, **k))[1]
    gate._graft_issues.clear()
    try:
        assert gate._graft_issue(root) == "" and gate._graft_issue(root) == ""
    finally:
        gate._git_lines = real
        gate._graft_issues.clear()
    assert calls == [gate._GRAFT_LOOKUP_TIMEOUT] and gate._GRAFT_LOOKUP_TIMEOUT <= 2, calls


def test_one_ancestry_question_is_asked_once(tmp_path):
    """Three passes sharing one base ask git once, inside the budget the commit comparisons share."""
    root, h = _history(tmp_path)
    calls = []
    real = gate._proc.run
    gate._proc.run = lambda cmd, *a, **k: (calls.append(cmd), real(cmd, *a, **k))[1]
    gate._ancestry.clear()
    try:
        for _ in range(3):
            assert gate._git_is_ancestor(root, h["plan"], h["build"])
    finally:
        gate._proc.run = real
        gate._ancestry.clear()
    assert sum("--is-ancestor" in c for c in calls) == 1, calls


def test_refusal_a_round_with_no_verdict_holds_the_stamp_at_the_pass_cap(tmp_path):
    """The cap waives the round's level, never its verdict: a round that ended incomplete (rc 3, a lens it
    did not report) still holds the stamp, beside a completed round with blocking findings, which does not."""
    root, h = _history(tmp_path)
    real = gate._pass_cap
    gate._pass_cap = lambda _root: 1
    try:
        ok, why = _judge(root, receipt={"round": {"rc": 3}}, **_good(h))
        assert not ok and "round has no checked, complete receipt" in why, why
        assert _judge(root, highest={"round": "P1"}, **_good(h)) == (True, "")
    finally:
        gate._pass_cap = real


def test_refusal_a_capped_round_from_another_branch_holds_the_stamp(tmp_path):
    """At the cap a round from an abandoned branch can share the plan's base; it read other code, so it
    does not stand in for this candidate's round."""
    root, h = _history(tmp_path)
    real = gate._pass_cap
    gate._pass_cap = lambda _root: 1
    try:
        ok, why = _judge(root, receipt={"round": {"head": h["elsewhere"]}}, **_good(h))
        assert not ok and "not in this history" in why, why
        ok, why = _judge(root, receipt={"round": {"worktree_sha256": "0" * 64}}, **_good(h))
        assert not ok and "uncommitted" in why, why
    finally:
        gate._pass_cap = real


def test_the_stamp_path_reads_share_the_hooks_deadline(tmp_path):
    """Reads that each fit their own wait cannot together outlast the hook: once its deadline is spent, every
    further git read is refused (None), so the hook still answers in time, with a refusal."""
    root, _h = _history(tmp_path)
    saved = list(gate._hook_deadline)
    gate._hook_deadline[:] = [time.monotonic() - 1]
    try:
        began = time.monotonic()
        assert gate._git_lines(root, "rev-parse", "HEAD") is None
        assert gate._readme_only_regenerated(root, "HEAD:README.md", "HEAD:README.md") is False
        assert time.monotonic() - began < 0.5
    finally:
        gate._hook_deadline[:] = saved
    assert gate._git_lines(root, "rev-parse", "HEAD")


def test_the_pass_count_stops_at_the_cap(tmp_path, monkeypatch):
    """Asked only whether a section has reached its cap, the count stops there: receipts past the cap (each
    one costs its status reads) are never checked."""
    manifest = tmp_path / gate._PASS_MANIFEST
    manifest.parent.mkdir(parents=True)
    rows = [json.dumps({"todo": "todo/x.md", "section": 7, "kind": "round"}) for _ in range(10)]
    manifest.write_text("\n".join(rows) + "\n")
    checked = []
    monkeypatch.setattr(gate, "_receipt_complete", lambda e: checked.append(e) or True)
    assert gate._spent_passes(tmp_path, "todo/x.md", "7", 3) == 3 and len(checked) == 3
    checked.clear()
    assert gate._spent_passes(tmp_path, "todo/x.md", "7") == 10 and len(checked) == 10


def test_a_verdict_reached_after_the_deadline_is_a_refusal():
    """A check that stalls past the hook's budget and then returns success inside the watchdog's grace: the
    hook refuses, as the watchdog would have, rather than approve late."""
    def late():
        time.sleep(0.09)
        return 0
    assert gate._with_deadline(0.02, "harness", late) == 2
    assert gate._with_deadline(5.0, "harness", lambda: 0) == 0


def test_the_pass_count_reads_the_manifest_newest_first(tmp_path, monkeypatch):
    """A long history of other sections, then this section's passes: the count reads from the end and stops
    at the cap, so the rows before it are never parsed."""
    manifest = tmp_path / gate._PASS_MANIFEST
    manifest.parent.mkdir(parents=True)
    other = [json.dumps({"todo": "todo/y.md", "section": 1, "kind": "round"}) for _ in range(2000)]
    mine = [json.dumps({"todo": "todo/x.md", "section": 7, "kind": "round"}) for _ in range(3)]
    manifest.write_text("\n".join(other + mine) + "\n")
    monkeypatch.setattr(gate, "_receipt_complete", lambda e: True)
    parsed = []
    real = gate.json.loads
    monkeypatch.setattr(gate.json, "loads", lambda s, *a, **k: parsed.append(s) or real(s, *a, **k))
    assert gate._spent_passes(tmp_path, "todo/x.md", "7", 3) == 3 and len(parsed) <= 3, len(parsed)
