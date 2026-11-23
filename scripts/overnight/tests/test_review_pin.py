#!/usr/bin/env python3
"""The reviewer pin loads strictly, and the repository's own pin holds."""
import pathlib
import sys

import pytest

REPO = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "scripts"))
import review_pin as rp  # noqa: E402

GOOD = (REPO / ".conclave/reviewers.toml").read_text()


@pytest.fixture(autouse=True)
def _own_git_config(monkeypatch):
    """Every git these tests start, directly or through the dispatchers, reads no config of the operator's:
    a global commit.gpgSign or a hook path must not reach a fixture (a locked signing key waits at pinentry)."""
    import os
    for key in [k for k in os.environ if k.startswith("GIT_")]:
        monkeypatch.delenv(key)
    monkeypatch.setenv("GIT_CONFIG_GLOBAL", os.devnull)
    monkeypatch.setenv("GIT_CONFIG_NOSYSTEM", "1")


def _pin(text=GOOD):
    return rp.load_pin(data=text.encode())


def test_the_repository_pin_holds():
    pin = _pin()
    assert (pin["reviewer"], pin["cli"], pin["model"]) == ("astra", "codex", "gpt-6.2-astra")
    # No stall cap (owner, 2026-11-20): a loop ends clean or at pass_cap.
    assert (pin["pass_cap"], pin["no_progress_cap"], pin["deep_paths"]) == (55, None, None)
    assert pin["lenses"]["plan"] == ("plan",) and pin["lenses"]["completion"] == ("plan-delivery", "record")
    # Every reconciliation checks the named gaps and attacks the newest repair; kernel and boot add the full lenses.
    assert pin["lenses"]["reconciliation"] == ("plan-delivery", "record", "repair-adversarial", "repair-integration")
    assert rp.lenses_for(pin, "reconciliation", ["src/kernel/a.c"])[4:] == ("adversarial", "integration",
                                                                          "concurrency", "bare-metal")
    assert rp.lenses_for(pin, "reconciliation", ["src/desktop/a.c"]) == pin["lenses"]["reconciliation"]
    assert pin["blocking"] == ("P1", "P2", "P3")   # operator, 2026-11-21
    assert not set(pin["lenses"]["round"]) & set(pin["lenses"]["completion"]), "one owner per lens"
    assert {"adversarial", "consistency", "test-coverage"} <= set(pin["lenses"]["round"])
    # Performance is its own pass, never a lens of the round (the 2026-11-11 replay).
    assert pin["lenses"]["performance"] == ("optimization",) and "optimization" not in pin["lenses"]["round"]
    assert rp.kinds(pin) == ("plan", "round", "performance", "completion", "reconciliation", "gap-audit")


@pytest.mark.parametrize("old, new", [
    ("schema_version = 1", "schema_version = 2"),
    ('claude = ["astra"]', 'claude = ["astra", "other"]'),
    ('claude = ["astra"]', 'claude = ["astra"]\ncodex = ["astra"]'),
    ('claude = ["astra"]', 'claude = ["nobody"]'),
    ('cli = "codex"', 'cli = "agy"'),
    ('model = "gpt-6.2-astra"', 'model = "gpt-6.2-astra"\neffort = "high"'),
    ('policy = "four-pass"', 'policy = "three-pass"'),
    ("pass_cap = 55", "pass_cap = 3"),
    ('[schedule.performance]\nlenses = ["optimization"]\n', ""),
    ('[schedule.performance]\nlenses = ["optimization"]', '[schedule.performance]\nlenses = ["adversarial"]'),
    ("pass_cap = 55", 'pass_cap = "33"'),
    ("pass_cap = 55", "pass_cap = true"),
    ("pass_cap = 55\n", ""),
    ("pass_cap = 55\n", "pass_cap = 55\nno_progress_cap = 56\n"),
    ("pass_cap = 55\n", 'pass_cap = 55\nno_progress_cap = 3\ndeep_paths = ["/src/boot/"]\n'),
    ("pass_cap = 55\n", 'pass_cap = 55\nno_progress_cap = 3\ndeep_paths = ["src/kernel/", "src/kernel/"]\n'),
    ("pass_cap = 55\n", 'pass_cap = 55\ndeep_paths = ["src/kernel/"]\n'),
    ("pass_cap = 55\n", "pass_cap = 55\nseverity_stop_after = 2\n"),
    ("[schedule.lenses]\n", '[schedule.lenses]\nadversarial = "Redefined?"\n'),
    ("[schedule.lenses]\n", '[schedule.lenses]\nunused-lens = "Nobody asks this?"\n'),
    ("[schedule.lenses]\n", '[schedule.lenses]\nBad_Name = "Asked?"\n'),
    ("[schedule.lenses]\n", "[schedule.lenses]\nquoted = \"Is it 'quoted'?\"\n"),
    ("[schedule.lenses]\n", '[schedule.lenses]\nflagged = "Does --force break it?"\n'),
    ("[schedule.lenses]\n", '[schedule.lenses]\nempty = "  "\n'),
    ('lenses = ["plan"]', 'lenses = ["plan", "plan"]'),
    ('lenses = ["plan"]', 'lenses = ["made-up"]'),
    ('lenses = ["plan"]', "lenses = []"),
    ('lenses = ["plan-delivery", "record"]\n', ""),
    ('lenses = ["plan-delivery", "record"]', 'lenses = ["plan-delivery", "record", "adversarial"]'),
    ('blocking = ["P1", "P2", "P3"]', 'blocking = ["P2"]'),
    ('blocking = ["P1", "P2", "P3"]', 'blocking = ["P1", "P4"]'),
    ('blocking = ["P1", "P2", "P3"]', "blocking = []"),
    ('blocking = ["P1", "P2", "P3"]\n', ""),
    ('passes = ["plan", "round"]', 'passes = ["plan", "review"]'),
    ('paths = ["src/kernel/", "include/kernel/", "src/boot/"]', 'paths = ["../elsewhere/"]'),
    ('paths = ["src/kernel/", "include/kernel/", "src/boot/"]', 'paths = ["/abs/"]'),
    ('lenses = ["concurrency", "bare-metal"]', 'lenses = ["concurrency", "adversarial"]'),
    ('lenses = ["concurrency", "bare-metal"]', 'lenses = ["concurrency"]\nwhen = "always"'),
    ("[schedule.reconciliation]", "[[schedule.rounds]]\nlenses = [\"adversarial\"]\n\n[schedule.reconciliation]"),
])
def test_refusal_one_wrong_key_refuses_the_pin(old, new):
    assert old in GOOD
    with pytest.raises(rp.Refused):
        _pin(GOOD.replace(old, new, 1))


def test_refusal_a_missing_or_broken_pin_never_falls_back(tmp_path):
    with pytest.raises(rp.Refused):
        rp.load_pin(tmp_path / "absent.toml")
    with pytest.raises(rp.Refused):
        rp.load_pin(data=b"schema_version = [")
    assert rp.main(["--kinds", "--pin", str(tmp_path / "absent.toml")]) == 2


def test_a_round_owes_one_verdict_per_lens():
    pin = _pin()
    block = rp.lens_block(pin, "round")
    for name in pin["lenses"]["round"]:
        assert f"- {name}: " in block
    assert "- record: " in rp.lens_block(pin, "completion")
    full = "\n".join(f"LENS {name}: clean" for name in pin["lenses"]["round"])
    assert rp.missing_lens_verdicts(pin, "round", full) == ()
    skimmed = full.replace("LENS quality: clean", "quality looked fine")
    assert rp.missing_lens_verdicts(pin, "round", skimmed) == ("quality",)
    assert rp.missing_lens_verdicts(pin, "round", full.replace("consistency: clean", "consistency: maybe")) \
        == ("consistency",)
    assert rp.missing_lens_verdicts(pin, "round", full.replace("LENS adversarial: clean", "`LENS adversarial: findings`")) \
        == ()


def test_the_codex_config_must_name_the_pinned_model(tmp_path):
    pin, config = _pin(), tmp_path / "config.toml"
    config.write_text('model = "gpt-6.2-astra"\nmodel_reasoning_effort = "low"\n')
    rp.check_codex_model(pin, config)
    for other in ("gpt-6-astra", "another-model"):
        config.write_text(f'model = "{other}"\n')
        with pytest.raises(rp.Refused, match="change the config, not the call"):
            rp.check_codex_model(pin, config)
    config.write_text('model = "another-model"\n')
    with pytest.raises(rp.Refused, match="change the config, not the call"):
        rp.check_codex_model(pin, config)
    with pytest.raises(rp.Refused):
        rp.check_codex_model(pin, tmp_path / "absent.toml")


def test_conditional_lenses_follow_the_changed_paths():
    pin = _pin()
    base = rp.lenses_for(pin, "round")
    assert "concurrency" not in base and "design" not in base
    kernel = rp.lenses_for(pin, "round", ["docs/a.md", "src/kernel/sched/dpc.c"])
    assert kernel == base + ("concurrency", "bare-metal")
    assert rp.lenses_for(pin, "round", ["src/desktop/x.c", "src/boot/uefi/b.c"]) \
        == base + ("concurrency", "bare-metal", "design")
    assert rp.lenses_for(pin, "completion", ["src/kernel/a.c"]) == pin["lenses"]["completion"]
    assert "design" in rp.lenses_for(pin, "plan", ["src/desktop/x.c"])
    assert rp.lenses_for(pin, "round", ["src/kernelx/a.c", "tools/src/kernel/a.c"]) == base
    full = "\n".join(f"LENS {name}: clean" for name in base)
    assert rp.missing_lens_verdicts(pin, "round", full) == ()
    assert rp.missing_lens_verdicts(pin, "round", full, ["src/kernel/a.c"]) == ("concurrency", "bare-metal")
    assert "- bare-metal: " in rp.lens_block(pin, "round", ["src/boot/uefi/b.c"])


def test_only_the_blocking_levels_hold_the_stamp():
    pin = _pin()
    assert rp.blocks(pin, "P1") and rp.blocks(pin, "P2") and rp.blocks(pin, "P3")   # every level (operator, 2026-11-21)
    assert not rp.blocks(pin, "none")
    narrowed = _pin(GOOD.replace('blocking = ["P1", "P2", "P3"]', 'blocking = ["P1", "P2"]'))
    assert rp.blocks(narrowed, "P2") and not rp.blocks(narrowed, "P3")
    with pytest.raises(rp.Refused):
        rp.blocks(pin, "high")
    assert "HIGHEST: none" in rp.lens_block(pin, "completion")


def test_the_recorder_and_the_other_hooks_know_the_four_passes():
    """Until 2026-11-11 the recorder spelled its kinds out twice and dropped an unknown one with no error."""
    sys.path.insert(0, str(REPO / ".claude/hooks"))
    import codex_review_completed as crc
    import review_convergence as conv
    import skill_step_observer as obs
    assert crc.SECTION_PASS_KINDS == rp.SECTION_KINDS
    for kind in rp.SECTION_KINDS:
        assert crc._detect_review_kind("", f"[review-kind: {kind}] todo/x.md section 1\nbody") == kind
        assert kind in obs.CANONICAL_REVIEW_KINDS and kind in conv.KIND_SCOPES, kind
    for kind in crc.LEGACY_REVIEW_KINDS:
        assert crc._detect_review_kind("", f"[review-kind: {kind}] todo/x.md") == kind


def test_refusal_an_unknown_kind_is_said_out_loud(capsys):
    sys.path.insert(0, str(REPO / ".claude/hooks"))
    import codex_review_completed as crc
    # Built in two parts: the repository-wide scan for review kinds must not read this misspelling as one.
    marker = "[review-" + "kind: roundd]"
    assert crc._detect_review_kind("", marker + " todo/x.md section 1") == ""
    err = capsys.readouterr().err
    assert "unknown review kind 'roundd'" in err and "NOT recorded" in err, err
    assert crc._detect_review_kind("", "no marker at all") == ""
    assert capsys.readouterr().err == ""


def test_every_pass_carries_the_meaning_of_each_level():
    """The reviewer graded on its own scale until the levels were defined in the prompt (2026-11-21)."""
    pin = _pin()
    for kind in rp.SECTION_KINDS:
        block = rp.lens_block(pin, kind)
        for level in rp.SEVERITIES:
            assert f"- {level}: {rp.SEVERITY_TEXTS[level]}" in block, (kind, level)
        assert rp.SEVERITY_RULE in block, kind
        assert "A finding with no reachable trigger is P3." in block, kind
        assert "legal CPU and interrupt orderings" in block, kind      # a rare race is never graded down
    assert set(rp.SEVERITY_TEXTS) == set(rp.SEVERITIES)


def test_a_lens_block_survives_the_companions_argument_split():
    """codex-companion re-splits the prompt on whitespace and reads quotes and bare flags as syntax."""
    pin = _pin()
    for kind in rp.SECTION_KINDS:
        block = rp.lens_block(pin, kind, ["src/kernel/a.c", "src/desktop/b.c"])
        assert "'" not in block and '"' not in block, kind
        assert not any(word.startswith("--") for word in block.split()), kind


FAKE_COMPANION = """
const fs = require('fs');
const tail = process.argv.slice(3);
const prompt = tail[0] === '--' ? tail.slice(1).join(' ') : tail.join(' ');
fs.writeFileSync(process.env.FAKE_PROMPT_OUT, prompt);
fs.writeFileSync(process.env.FAKE_PROMPT_OUT + '.argv', JSON.stringify(tail));
// As the real companion: progress first (here, the echoed prompt), then the heading, then the response.
process.stdout.write('[codex] prompt: ' + prompt + '\\n# Codex Adversarial Review\\n\\n');
process.stdout.write(fs.readFileSync(process.env.FAKE_REVIEW, 'utf8'));
"""


def _fixture_repo(tmp_path):
    """A small history for the dispatcher to read, made once per test: four commits after the first touching a
    kernel source, a document, a TODO and a script, so a range from any of them sets every conditional lens.
    The operator's own checkout is never read or written by these tests."""
    import subprocess
    repo = tmp_path / "fx"
    if not repo.exists():
        repo.mkdir()

        def git(*args):
            subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                            "-c", "core.hooksPath=/dev/null", *args], check=True, capture_output=True, timeout=60)
        git("init", "-q")
        git("commit", "-q", "--allow-empty", "-m", "zero")
        for rel in ("src/kernel/a.c", "docs/b.md", "todo/x.md", "scripts/y.py"):
            (repo / rel).parent.mkdir(parents=True, exist_ok=True)
            (repo / rel).write_text(rel + "\n")
            git("add", rel)
            git("commit", "-q", "-m", rel)
    return repo


def _rev(tmp_path, rev):
    import subprocess
    return subprocess.run(["git", "rev-parse", rev], capture_output=True, text=True, cwd=str(_fixture_repo(tmp_path)),
                          check=True, timeout=30).stdout.strip()


def _dispatch(tmp_path, kind, review, base="HEAD", marker=None, body="Review it.", reviews_dir=None):
    import os
    import subprocess
    fake = tmp_path / "companion.js"
    fake.write_text(FAKE_COMPANION)
    (tmp_path / "review.txt").write_text(review)
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_COMPANION_ACCEPTS_EFFORT": "1",
           "CODEX_REVIEWS_DIR": str(reviews_dir or tmp_path / "reviews"),
           "FAKE_PROMPT_OUT": str(tmp_path / "prompt.txt"), "FAKE_REVIEW": str(tmp_path / "review.txt"),
           "REVIEW_BASE": base}
    done = subprocess.run(["bash", str(REPO / "scripts/codex-dispatch.sh"),
                           (marker or f"[review-kind: {kind}]")
                           + f" todo/00-infrastructure/TODO-01-developer-tooling-stack.md section 99\n{body}"],
                          capture_output=True, text=True, timeout=120, cwd=str(_fixture_repo(tmp_path)), env=env)
    prompt = (tmp_path / "prompt.txt").read_text() if (tmp_path / "prompt.txt").exists() else ""
    return done, prompt


def test_the_dispatcher_appends_the_lenses_and_passes_a_review_with_every_verdict(tmp_path):
    pin = _pin()
    full = "findings...\n" + "\n".join(f"LENS {n}: clean" for n in pin["lenses"]["completion"]) + "\nHIGHEST: none\n"
    done, prompt = _dispatch(tmp_path, "completion", full)
    assert done.returncode == 0, done.stderr
    assert "- plan-delivery: " in prompt and "- record: " in prompt and "HIGHEST: none" in prompt
    log = next((tmp_path / "reviews").glob("*-completion-direct.out")).read_text()
    assert log.rstrip().endswith("Turn completed (rc=0)")


def test_refusal_a_review_that_skips_a_lens_is_incomplete_not_clean(tmp_path):
    done, _prompt = _dispatch(tmp_path, "completion", "looks fine to me\nLENS record: clean\nHIGHEST: none\n")
    assert done.returncode == 3, (done.returncode, done.stderr)
    assert "INCOMPLETE -- the review gave no verdict line for: plan-delivery" in done.stderr
    log = next((tmp_path / "reviews").glob("*-completion-direct.out")).read_text()
    assert log.rstrip().endswith("Turn completed (rc=3)"), log[-200:]


def test_a_flag_word_in_the_prompt_is_never_read_as_an_option(tmp_path):
    """Live 2026-11-12: `git -C after` in a plan body became --cwd after, and the pass died "git is not installed"."""
    import json
    done, prompt = _dispatch(tmp_path, "adversarial", "no lens lines at all\n", body="see git -C after a separator, `git diff --cached` and --base x")
    assert done.returncode == 0, done.stderr
    argv = json.loads((tmp_path / "prompt.txt.argv").read_text())
    assert argv[0] == "--" and len(argv) == 2 and argv[1].endswith("git -C after a separator, `git diff --cached` and --base x"), argv


def test_an_older_kind_is_dispatched_exactly_as_before(tmp_path):
    done, prompt = _dispatch(tmp_path, "adversarial", "no lens lines at all\n")
    assert done.returncode == 0, done.stderr
    assert "LENS" not in prompt and prompt.endswith("Review it.")


def test_a_pin_with_a_stall_cap_and_no_deep_paths_leaves_them_unknown():
    """Codex review 2026-11-19: an absent key read as an empty list put every kernel section on the shorter rule."""
    pin = _pin(GOOD.replace("pass_cap = 55\n", "pass_cap = 55\nno_progress_cap = 3\n", 1))
    assert (pin["no_progress_cap"], pin["deep_paths"]) == (3, None)


def test_a_pin_lens_is_asked_like_a_built_in_one():
    pin = _pin()
    block = rp.lens_block(pin, "reconciliation", ["src/apps/a.c"])
    assert "- repair-adversarial: Starting with the newest repair" in block and "- record: " in block
    assert "across the whole section" in block
    done = "\n".join(f"LENS {n}: clean" for n in pin["lenses"]["reconciliation"]) + "\nHIGHEST: none\n"
    assert rp.missing_lens_verdicts(pin, "reconciliation", done, ["src/apps/a.c"]) == ()
    assert rp.missing_lens_verdicts(pin, "reconciliation", "LENS record: clean\n", ["src/apps/a.c"]) == (
        "plan-delivery", "repair-adversarial", "repair-integration")


def test_the_caps_name_no_stall_cap_when_the_pin_has_none(capsys):
    assert rp.main(["--caps"]) == 0
    assert capsys.readouterr().out == "pass_cap 55\n"


def _receipt(kind, head, section=99, checked=True, rc=0, todo="todo/00-infrastructure/TODO-01-developer-tooling-stack.md",
             worktree=rp.CLEAN_WORKTREE, base=None):
    import json
    # The dispatcher's field order (manifest-line.py): ts, kind, todo first.
    return json.dumps({"ts": 1795170000, "kind": kind, "todo": todo, "section": section, "head": head,
                       "base": head if base is None else base,
                       "pass_checked": checked, "rc": rc, "worktree_sha256": worktree, "route": "codex-dispatch.sh"})


def test_a_reconciliation_starts_from_the_newest_checked_completion_or_reconciliation(tmp_path):
    todo = "todo/00-infrastructure/TODO-01-developer-tooling-stack.md"
    manifest = tmp_path / "manifest.jsonl"
    assert rp.repair_base(manifest, todo, 99) == ""                         # no manifest yet
    manifest.write_text("\n".join([
        _receipt("completion", "aaaaaaaa"), _receipt("round", "bbbbbbbb"),             # a round is not where a repair starts
        _receipt("reconciliation", "cccccccc"), _receipt("reconciliation", "dddddddd", checked=False),
        _receipt("completion", "eeeeeeee", rc=3), _receipt("completion", "ffffffff", section=98),
        '{"ts": 1, "kind": "completion", "todo": "todo/other.md", "not json']) + "\n")
    assert rp.repair_base(manifest, todo, 99) == "cccccccc"
    assert rp.repair_base(manifest, todo, 97) == ""
    # Review 2026-11-20: a newer line that cannot be read and names this TODO may be a newer pass of the section.
    manifest.write_text(_receipt("completion", "aaaaaaaa") + "\n" + _receipt("reconciliation", "bbbbbbbb")[:-30] + "\n")
    with pytest.raises(rp.Refused, match="unreadable line"):
        rp.repair_base(manifest, todo, 99)
    # The dispatcher writes kind before todo: a newer line cut before its todo field may be this TODO's.
    manifest.write_text(_receipt("completion", "aaaaaaaa") + "\n" + '{"ts": 1, "kind": "reconciliation", "to' + "\n")
    with pytest.raises(rp.Refused, match="unreadable line"):
        rp.repair_base(manifest, todo, 99)
    # Review 2026-11-20: a cut inside the todo value, or just before its closing quote, may be this TODO too.
    for cut in (f'{{"ts": 1, "kind": "reconciliation", "todo": "{todo[:12]}',
                f'{{"ts": 1, "kind": "reconciliation", "todo": "{todo}'):
        manifest.write_text(_receipt("completion", "aaaaaaaa") + "\n" + cut + "\n")
        with pytest.raises(rp.Refused, match="unreadable line"):
            rp.repair_base(manifest, todo, 99)
    assert rp.unreadable_line_may_be('{"ts": 1, "kind": "completion", "todo": "todo/other.md", "sec', todo) is False
    assert rp.unreadable_line_may_be(f'{{"ts": 1, "kind": "completion", "todo": "{todo}", "sec', todo) is True
    # Review 2026-11-20: another TODO's receipt that lost its newline, with this TODO's appended to it.
    glued = _receipt("completion", "zzz", todo="todo/other.md")[:-1] + _receipt("completion", "bbbbbbbb")
    manifest.write_text(_receipt("completion", "aaaaaaaa") + "\n" + glued + "\n")
    with pytest.raises(rp.Refused, match="unreadable line"):
        rp.repair_base(manifest, todo, 99)
    torn = _receipt("completion", "zzz", todo="todo/other.md")[:-1] + '{"ts": 2, "kind": "completion"'
    assert rp.unreadable_line_may_be(torn, todo) is True
    # This TODO's receipt torn inside its timestamp, another TODO's complete receipt appended to it.
    stamp_cut = '{"ts": 1795170000' + _receipt("completion", "zzz", todo="todo/other.md")
    manifest.write_text(_receipt("completion", "aaaaaaaa") + "\n" + stamp_cut + "\n")
    with pytest.raises(rp.Refused, match="unreadable line"):
        rp.repair_base(manifest, todo, 99)
    # Another TODO's write cut after its section, then this TODO's append cut inside the opening ts key.
    short = _receipt("completion", "zzz", todo="todo/other.md").split(', "head"')[0] + '{"ts'
    manifest.write_text(_receipt("completion", "aaaaaaaa") + "\n" + short + "\n")
    with pytest.raises(rp.Refused, match="unreadable line"):
        rp.repair_base(manifest, todo, 99)
    manifest.write_text(_receipt("completion", "aaaaaaaa") + "\n" + '{"ts": 1, "kind": "completion", "todo": "todo/other.md", "sec' + "\n")
    assert rp.repair_base(manifest, todo, 99) == "aaaaaaaa"                        # another TODO's broken line
    # A completion that read uncommitted changes: `git diff <head>` cannot show what its repair removed.
    manifest.write_text(_receipt("completion", "aaaaaaaa", worktree="0" * 64) + "\n")
    with pytest.raises(rp.Refused, match="uncommitted changes"):
        rp.repair_base(manifest, todo, 99)


def test_a_reconciliation_names_its_repair_and_refuses_without_one(tmp_path):
    import subprocess
    reviews = tmp_path / "reviews"
    reviews.mkdir()
    pin = _pin()
    full = "\n".join(f"LENS {n}: clean" for n in pin["lenses"]["reconciliation"]) + "\nHIGHEST: none\n"
    done, prompt = _dispatch(tmp_path, "reconciliation", full, reviews_dir=reviews)
    assert done.returncode == 2 and "a reconciliation follows a completion" in done.stderr, done.stderr
    start, last, head = _rev(tmp_path, "HEAD~2"), _rev(tmp_path, "HEAD~1"), _rev(tmp_path, "HEAD")
    (reviews / "manifest.jsonl").write_text(_receipt("completion", last, base=start) + "\n")
    # No REVIEW_BASE: the section range is the one the completion reviewed, never HEAD~1.
    done, prompt = _dispatch(tmp_path, "reconciliation", full, base="", reviews_dir=reviews)
    assert done.returncode == 0, done.stderr
    named = f"The newest repair runs from the last reviewed candidate, {last}, to the repaired candidate, HEAD {head}"
    assert named in prompt and prompt.index(named) < prompt.index("- repair-adversarial: ")
    assert f"then review the whole section: git diff {start} HEAD" in prompt     # the whole section, not the repair
    # A staged change can undo the committed repair, so the repair, the index and the working files are each shown.
    assert f"the committed repair (git diff {last} {head})" in prompt and f"git diff --cached {head}" in prompt
    receipt = _receipt("completion", last, base=start) + "\n"
    (reviews / "manifest.jsonl").write_text(receipt)                  # the pass above appended its own record
    done, _ = _dispatch(tmp_path, "reconciliation", full, base=last, reviews_dir=reviews)
    assert done.returncode == 2 and "REVIEW_BASE names another commit" in done.stderr, done.stderr
    (reviews / "manifest.jsonl").write_text(receipt)
    done, _ = _dispatch(tmp_path, "reconciliation", full, base=start, reviews_dir=reviews)
    assert done.returncode == 0, done.stderr
    (reviews / "manifest.jsonl").write_text(_receipt("completion", last, base="") + "\n")
    done, _ = _dispatch(tmp_path, "reconciliation", full, base="", reviews_dir=reviews)
    assert done.returncode == 2 and "records no section base" in done.stderr, done.stderr
    (reviews / "manifest.jsonl").write_text(_receipt("completion", "0" * 40, base=start) + "\n")
    done, _ = _dispatch(tmp_path, "reconciliation", full, base="", reviews_dir=reviews)
    assert done.returncode == 2 and "is not a commit in this history" in done.stderr, done.stderr
    # Review 2026-11-20: a commit that exists but was left behind by a reset or rebase is no start either.
    off = subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit-tree",
                          "HEAD^{tree}", "-m", "off-history fixture"], capture_output=True, text=True,
                         cwd=str(_fixture_repo(tmp_path)), check=True, timeout=30).stdout.strip()
    (reviews / "manifest.jsonl").write_text(_receipt("completion", off, base=start) + "\n")
    done, _ = _dispatch(tmp_path, "reconciliation", full, base="", reviews_dir=reviews)
    assert done.returncode == 2 and "is not a commit in this history" in done.stderr, done.stderr



def test_a_reconciliation_after_a_first_commit_takes_the_empty_tree_base(tmp_path):
    import subprocess
    reviews = tmp_path / "reviews"
    reviews.mkdir()
    lenses = ["plan-delivery", "record", "repair-adversarial", "repair-integration", "adversarial", "integration",
              "concurrency", "bare-metal", "design"]                    # from the empty tree every path changed
    full = "\n".join(f"LENS {n}: clean" for n in lenses) + "\nHIGHEST: none\n"
    last = _rev(tmp_path, "HEAD~1")
    empty = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"
    (reviews / "manifest.jsonl").write_text(_receipt("completion", last, base=empty) + "\n")
    done, prompt = _dispatch(tmp_path, "reconciliation", full, base="", reviews_dir=reviews)
    assert done.returncode == 0, done.stderr
    assert f"then review the whole section: git diff {empty} HEAD" in prompt
    # Given explicitly, the recorded sentinel is the same base, not "another commit".
    (reviews / "manifest.jsonl").write_text(_receipt("completion", last, base=empty) + "\n")
    done, prompt = _dispatch(tmp_path, "reconciliation", full, base=empty, reviews_dir=reviews)
    assert done.returncode == 0, done.stderr
    assert f"then review the whole section: git diff {empty} HEAD" in prompt


def test_a_later_pass_with_no_base_starts_where_the_section_did(tmp_path):
    """With REVIEW_BASE unset a round reviews from the plan pass's candidate, never HEAD~1; with no plan on
    record it is refused."""
    import json
    import subprocess
    reviews = tmp_path / "reviews"
    reviews.mkdir()
    start = _rev(tmp_path, "HEAD~3")
    done, _prompt = _dispatch(tmp_path, "round", "LENS adversarial: clean\nHIGHEST: none\n", base="",
                              reviews_dir=reviews)
    assert done.returncode == 2 and "start is not on record" in done.stderr, done.stderr
    (reviews / "manifest.jsonl").write_text(_receipt("plan", start) + "\n")
    full = "\n".join(f"LENS {n}: clean" for n in ("adversarial", "integration", "source-defect", "consistency",
                                                  "quality", "test-coverage", "design", "concurrency",
                                                  "bare-metal")) + "\nHIGHEST: none\n"
    done, prompt = _dispatch(tmp_path, "round", full, base="", reviews_dir=reviews)
    receipt = json.loads((reviews / "manifest.jsonl").read_text().splitlines()[-1])
    assert receipt["kind"] == "round" and receipt["base"] == start, (receipt, done.stderr)
    assert f"This pass reviews the whole section: git diff {start} {receipt['head']}" in prompt, prompt[-400:]


def test_refusal_a_candidate_that_moves_while_the_pass_is_prepared_is_not_dispatched(tmp_path):
    """The lenses come from the candidate pinned first; an edit before the reviewer starts refuses the pass."""
    import os
    import subprocess
    import shutil as sh
    repo = tmp_path / "repo"
    repo.mkdir()
    for args in (["init", "-q"], ["commit", "-q", "--allow-empty", "-m", "one"]):
        subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                        "-c", "core.hooksPath=/dev/null", *args], check=True, capture_output=True, timeout=60)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    real = sh.which("python3")
    (bindir / "python3").write_text(f'#!/bin/sh\ncase " $* " in *" --lenses "*) echo moved > "{repo}/new.txt" ;; esac\n'
                                    f'exec "{real}" "$@"\n')
    (bindir / "python3").chmod(0o755)
    fake = tmp_path / "companion.js"
    fake.write_text(FAKE_COMPANION)
    (tmp_path / "review.txt").write_text("HIGHEST: none\n")
    env = {**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}", "CODEX_COMPANION_PATH": str(fake),
           "CODEX_REVIEWS_DIR": str(tmp_path / "reviews"), "FAKE_PROMPT_OUT": str(tmp_path / "prompt.txt"),
           "FAKE_REVIEW": str(tmp_path / "review.txt"), "REVIEW_BASE": "HEAD"}
    done = subprocess.run(["bash", str(REPO / "scripts/codex-dispatch.sh"),
                           "[review-kind: round] todo/x.md section 9\nReview it."],
                          capture_output=True, text=True, timeout=120, cwd=str(repo), env=env)
    assert done.returncode == 2 and "candidate changed while the round pass was prepared" in done.stderr, done.stderr
    assert not (tmp_path / "prompt.txt").exists()                          # the reviewer never started


def test_the_section_start_is_read_from_the_dispatchers_manifest(tmp_path):
    todo = "todo/00-infrastructure/TODO-01-developer-tooling-stack.md"
    manifest = tmp_path / "manifest.jsonl"
    assert rp.section_start(manifest, todo, 99) == ""                        # no manifest yet
    manifest.write_text(_receipt("plan", "a" * 40) + "\n" + _receipt("plan", "b" * 40, checked=False) + "\n"
                        + _receipt("round", "c" * 40) + "\n")
    assert rp.section_start(manifest, todo, 99) == "a" * 40


def test_the_repair_range_comes_from_one_receipt(tmp_path):
    todo = "todo/00-infrastructure/TODO-01-developer-tooling-stack.md"
    manifest = tmp_path / "manifest.jsonl"
    manifest.write_text(_receipt("completion", "b" * 40, base="a" * 40) + "\n")
    assert rp.repair_base(manifest, todo, 99, field="range") == f"{'a' * 40} {'b' * 40}"

def test_the_round_guard_takes_its_caps_from_the_pin(tmp_path, monkeypatch, capsys):
    sys.path.insert(0, str(REPO / ".claude/hooks"))
    import review_round_guard as guard
    monkeypatch.delenv("OVERNIGHT_REVIEW_ROUND_CEILING", raising=False)
    monkeypatch.delenv("OVERNIGHT_REVIEW_NOPROGRESS_CAP", raising=False)
    assert (guard._noprogress_cap(), guard._ceiling()) == (None, 55)       # no stall cap (owner, 2026-11-20)
    other = tmp_path / "reviewers.toml"
    other.write_text(GOOD.replace("pass_cap = 55\n", "pass_cap = 40\nno_progress_cap = 2\n"))
    assert guard._pin_caps(other) == (2, 40)
    # With no stall cap a chain of equal-severity passes runs on; only pass_cap ends it.
    state = tmp_path / "rounds"
    for _ in range(10):
        _c, _s, capped, _r = guard.bump(state, "S", False, None, 55, "P2")
        assert not capped
    assert guard.bump(tmp_path / "r2", "S", False, None, 1, "P2")[2]
    # One call can still override, and a broken pin keeps the backstop alive and says so.
    monkeypatch.setenv("OVERNIGHT_REVIEW_ROUND_CEILING", "7")
    assert guard._ceiling() == 7
    capsys.readouterr()
    assert guard._pin_caps(tmp_path / "absent.toml") == (guard.BUILTIN_NOPROGRESS_CAP, guard.BUILTIN_CEILING)
    assert "could not be read" in capsys.readouterr().err


def test_a_verdict_rendered_as_a_list_item_counts():
    """The first real round (2026-11-11) came back with `- LENS adversarial: findings` and was called incomplete."""
    pin = _pin()
    names = pin["lenses"]["completion"]
    text = f"Next steps:\n- LENS {names[0]}: findings\nLENS {names[1]}: clean\n"
    assert rp.missing_lens_verdicts(pin, "completion", text) == ()
    assert rp.missing_lens_verdicts(pin, "completion", f"-LENS {names[0]}: clean\nLENS {names[1]}: clean") == (names[0],)


def test_refusal_verdicts_in_the_echoed_prompt_do_not_count(tmp_path):
    """Review finding: the whole transcript was scanned, so example verdict lines in a prompt passed the check."""
    done, _prompt = _dispatch(tmp_path, "completion", "No verdict lines in the response.\n",
                              body="Example of a good ending:\nLENS plan-delivery: clean\nLENS record: clean")
    assert done.returncode == 3, (done.returncode, done.stderr)
    assert "no verdict line for: plan-delivery record" in done.stderr
    transcript = "LENS plan-delivery: clean\nLENS record: clean\n# Codex Adversarial Review\nall good\n"
    assert rp.final_response(transcript) == "all good"
    assert rp.final_response("LENS record: clean\nno heading at all") == ""


def test_refusal_a_base_that_is_not_a_commit_refuses_the_pass(tmp_path):
    """Review finding: an unreadable base gave an empty file list and silently dropped the conditional lenses."""
    done, prompt = _dispatch(tmp_path, "round", "x", base="no-such-commit-anywhere")
    assert done.returncode == 2 and "REVIEW_BASE is not a commit here" in done.stderr, done.stderr
    assert prompt == "", "nothing may be dispatched"


def test_refusal_a_pass_with_nowhere_to_keep_its_transcript_is_not_dispatched(tmp_path):
    """Review finding: with no reviews directory the wrapper ran the review unchecked and returned 0."""
    blocker = tmp_path / "a-file"
    blocker.write_text("")
    done, prompt = _dispatch(tmp_path, "completion", "x", reviews_dir=blocker / "reviews")
    assert done.returncode == 2 and "no writable reviews directory" in done.stderr, done.stderr
    assert prompt == ""


def test_refusal_any_marked_review_with_nowhere_to_keep_its_receipt_is_not_dispatched(tmp_path):
    blocker = tmp_path / "a-file"
    blocker.write_text("")
    done, prompt = _dispatch(tmp_path, "adversarial", "x", reviews_dir=blocker / "reviews")
    assert done.returncode == 2 and "no writable reviews directory" in done.stderr, done.stderr
    assert prompt == ""


def test_a_first_line_larger_than_a_pipe_is_attributed(tmp_path):
    """The first line itself can be a large inventory: path and section are read in memory."""
    import json
    pin = _pin()
    full = "\n".join(f"LENS {n}: clean" for n in pin["lenses"]["completion"]) + "\nHIGHEST: none\n"
    done, _ = _dispatch(tmp_path, "completion", full, body=" also " + "todo/y.md " * 9000)
    assert done.returncode == 0, (done.returncode, done.stderr[-400:])
    e = json.loads((tmp_path / "reviews/manifest.jsonl").read_text().splitlines()[-1])
    assert e["todo"] == "todo/00-infrastructure/TODO-01-developer-tooling-stack.md" and e["section"] == 99


def test_refusal_a_marked_review_in_a_directory_it_cannot_write_is_not_dispatched(tmp_path):
    import os
    reviews = tmp_path / "ro-reviews"
    reviews.mkdir()
    os.chmod(reviews, 0o555)
    try:
        if os.access(reviews, os.W_OK):
            pytest.skip("running as a user who can write a read-only directory")
        done, prompt = _dispatch(tmp_path, "adversarial", "x", reviews_dir=reviews)
        assert done.returncode == 2 and "no writable reviews directory" in done.stderr, done.stderr
        assert prompt == "", "the reviewer must not have started"
    finally:
        os.chmod(reviews, 0o755)


def test_the_first_line_is_found_in_one_pass_after_a_blank_prefix():
    import subprocess
    import time
    lib = REPO / "scripts/lib/review-prompt.sh"
    prompt = "\n" * 4000 + "  [review-kind: round] todo/a.md section 3\n" + "x" * 90000
    t = time.time()
    r = subprocess.run(["bash", "-c", f". {lib}; review_prompt_first_line \"$1\"", "_", prompt],
                       capture_output=True, text=True, timeout=30)
    assert r.stdout == "  [review-kind: round] todo/a.md section 3" and time.time() - t < 2, r.stdout[:80]


def test_refusal_a_marker_only_the_recorder_would_read_is_refused(tmp_path):
    """Review finding: ` [review-kind: completion]` and `[review_kind: completion]` skipped the pin and lens
    checks here and were still recorded as a completion pass."""
    sys.path.insert(0, str(REPO / ".claude/hooks"))
    import codex_review_completed as crc
    for marker in (" [review-kind: completion]", "[review_kind: completion]", "[Review-Kind: completion]",
                   "[ review-kind : completion ]", "see [review-kind: completion]",
                   "[review-kind: adversarial] and [review-kind: completion]"):
        assert crc._detect_review_kind("", marker + " todo/x.md section 1") in ("completion", ""), marker
        done, prompt = _dispatch(tmp_path, "completion", "x", marker=marker)
        assert done.returncode == 2 and "write the marker once" in done.stderr, (marker, done.stderr)
        assert prompt == "", marker


def test_the_recorder_writes_no_stamp_for_a_pass():
    """The commit gate judges a pass from the dispatcher's receipt. A stamp written when a dispatch starts,
    which for the background broker is before there is anything to judge, would say nothing it may use."""
    text = (REPO / ".claude/hooks/codex_review_completed.py").read_text()
    assert "review_kind not in SECTION_PASS_KINDS" in text and "_k in SECTION_PASS_KINDS" in text
    assert "_pass_receipt_ok" not in text and "pass-receipts-credited" not in text


def test_the_wrapper_writes_the_receipt_the_gate_reads(tmp_path):
    import json
    pin = _pin()
    full = "\n".join(f"LENS {n}: clean" for n in pin["lenses"]["completion"]) + "\nHIGHEST: none\n"
    _dispatch(tmp_path, "completion", full)
    _dispatch(tmp_path, "completion", "LENS record: clean\n")
    _dispatch(tmp_path, "adversarial", "anything\n")
    lines = [json.loads(ln) for ln in (tmp_path / "reviews/manifest.jsonl").read_text().splitlines()]
    assert [(e["kind"], e["rc"], e["pass_checked"]) for e in lines] == \
        [("completion", 0, True), ("completion", 3, True), ("adversarial", 0, False)]
    assert len({e["jobId"] for e in lines}) == 3
    assert all(e["route"] == "codex-dispatch.sh" and e["section"] == 99 for e in lines)


def test_the_wrapper_publishes_its_done_file_after_the_receipt(tmp_path):
    """A waiter ends on <log>.done, never on log text, and reads the receipt next."""
    import json
    pin = _pin()
    full = "\n".join(f"LENS {n}: clean" for n in pin["lenses"]["completion"]) + "\nHIGHEST: none\n"
    done, _ = _dispatch(tmp_path, "completion", full)
    assert done.returncode == 0, done.stderr
    entry = json.loads((tmp_path / "reviews/manifest.jsonl").read_text().splitlines()[-1])
    assert entry["done_file"] is True
    assert pathlib.Path(entry["logFile"] + ".done").read_text().strip() == "0"


def test_a_pass_whose_receipt_cannot_be_written_is_not_published_as_success(tmp_path):
    """No receipt, no credit: the pass reports rc 3 in its exit and its done file."""
    pin = _pin()
    full = "\n".join(f"LENS {n}: clean" for n in pin["lenses"]["completion"]) + "\nHIGHEST: none\n"
    (tmp_path / "reviews").mkdir()
    manifest = tmp_path / "reviews/manifest.jsonl"
    manifest.write_text("")
    manifest.chmod(0o444)                                  # readable, so the start is read; the append fails
    try:
        done, _ = _dispatch(tmp_path, "completion", full)
    finally:
        manifest.chmod(0o644)
    assert done.returncode == 3, (done.returncode, done.stderr)
    dones = list((tmp_path / "reviews").glob("*.out.done"))
    assert len(dones) == 1 and dones[0].read_text().strip() == "3"


def test_a_prompt_larger_than_a_pipe_reaches_dispatch(tmp_path):
    """The first line is read without a pipe, so a whole-file inventory cannot SIGPIPE the wrapper."""
    pin = _pin()
    full = "\n".join(f"LENS {n}: clean" for n in pin["lenses"]["completion"]) + "\nHIGHEST: none\n"
    done, prompt = _dispatch(tmp_path, "completion", full, body="inventory line\n" * 8000)
    assert done.returncode == 0, (done.returncode, done.stderr[-400:])
    assert prompt.count("inventory line") == 8000


def test_refusal_a_pass_names_its_todo_and_section(tmp_path):
    for first in ("[review-kind: round] just some words", "[review-kind: round] todo/x.md with no section"):
        done, prompt = _dispatch(tmp_path, "round", "x", marker=first.split("]")[0] + "]",
                                 body="")
        assert done.returncode in (0, 3), "the helper's own first line names both"
    import os
    import subprocess
    fake = tmp_path / "companion.js"
    for first in ("[review-kind: round] just some words", "[review-kind: round] todo/x.md with no section number"):
        done = subprocess.run(["bash", str(REPO / "scripts/codex-dispatch.sh"), first + "\nReview it."],
                              capture_output=True, text=True, timeout=60, cwd=str(_fixture_repo(tmp_path)),
                              env={**os.environ, "CODEX_COMPANION_PATH": str(fake),
                                   "CODEX_REVIEWS_DIR": str(tmp_path / "reviews2")})
        assert done.returncode == 2 and "names its TODO and section on the first line" in done.stderr, done.stderr


SELF_CONTAINED_COMPANION = """
const fs = require('fs');
const path = require('path');
fs.writeFileSync(path.join(__dirname, 'prompt.txt'), (a => a[0] === '--' ? a.slice(1).join(' ') : a.join(' '))(process.argv.slice(3)));
process.stdout.write('# Codex Adversarial Review\\n\\n' + fs.readFileSync(path.join(__dirname, 'review.txt'), 'utf8'));
"""


def test_the_broker_runs_a_pass_through_the_checking_wrapper(tmp_path):
    """The run dispatches in the background through the broker. A pass sent that way is run by the wrapper
    inside the detached job, so it is checked and receipted exactly as a foreground one."""
    import json
    import os
    import subprocess
    import time
    repo = tmp_path / "repo"
    repo.mkdir()

    def git(*args):
        subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                        "-c", "core.hooksPath=/dev/null", *args], capture_output=True, text=True, timeout=60, check=True)

    git("init", "-q")
    (repo / ".gitignore").write_text(".claude/\n")
    git("add", "-A")
    git("commit", "-q", "-m", "candidate")
    # The detached job has a fresh environment, so this companion finds its files beside itself. It is the
    # only reviewer this test can reach: the broker hands the wrapper the companion path it was given.
    fake = tmp_path / "companion.js"
    fake.write_text(SELF_CONTAINED_COMPANION)
    pin = _pin()
    (tmp_path / "review.txt").write_text(
        "\n".join(f"LENS {n}: clean" for n in pin["lenses"]["completion"]) + "\nHIGHEST: none\n")
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "REVIEW_BASE": "HEAD", "CODEX_OUTAGE_OVERRIDE": "1"}
    done = subprocess.run(["bash", str(REPO / "scripts/overnight/review-broker-codex-dispatch.sh"),
                           "[review-kind: completion] todo/00-x/TODO-01-x.md section 7\nReview it."],
                          capture_output=True, text=True, timeout=60, cwd=str(repo), env=env)
    assert done.returncode == 0, done.stderr
    log = pathlib.Path(json.loads(done.stdout.strip().splitlines()[-1])["logFile"])
    deadline = time.time() + 60
    done_file = pathlib.Path(str(log) + ".done")
    while time.time() < deadline and not done_file.exists():   # what the waiter ends on
        time.sleep(0.2)
    assert done_file.read_text().strip() == "0", log.read_text()[-400:]
    assert "Turn completed (rc=0)" in log.read_text(), log.read_text()[-400:]
    assert "[codex]" not in log.read_text(), "the real reviewer must not have been reached"
    assert "- record: " in (tmp_path / "prompt.txt").read_text(), "the lenses were appended"
    lines = [json.loads(ln) for ln in (repo / ".claude/overnight/reviews/manifest.jsonl").read_text().splitlines()]
    receipts = [e for e in lines if e.get("pass_checked") is True]
    assert len(receipts) == 1 and receipts[0]["kind"] == "completion" and receipts[0]["section"] == 7 \
        and receipts[0]["rc"] == 0 and receipts[0]["highest"] == "none", lines
    # The broker registers the job BEFORE launching it, so the finished receipt is the newest line.
    assert "pass_checked" not in lines[0] and lines[0]["jobId"].startswith("detached:codex-rev-completion-")
    assert lines[-1] is not lines[0] and lines[-1].get("pass_checked") is True, lines


def test_the_broker_survives_an_apostrophe_and_a_space_in_every_value(tmp_path):
    """Review finding: paths and REVIEW_BASE were placed inside hand-written single quotes in the detached
    command, so an apostrophe (a valid ref is refs/heads/owner's-fix) ended the quoting."""
    import json
    import os
    import subprocess
    import time
    home = tmp_path / "it's a dir"
    repo = home / "the repo"
    repo.mkdir(parents=True)

    def git(*args):
        subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                        "-c", "core.hooksPath=/dev/null", *args], capture_output=True, text=True, timeout=60, check=True)

    git("init", "-q")
    (repo / ".gitignore").write_text(".claude/\n")
    git("add", "-A")
    git("commit", "-q", "-m", "candidate")
    git("branch", "owner's-fix")
    fake = home / "companion.js"
    fake.write_text(SELF_CONTAINED_COMPANION)
    pin = _pin()
    (home / "review.txt").write_text(
        "\n".join(f"LENS {n}: clean" for n in pin["lenses"]["completion"]) + "\nHIGHEST: none\n")
    marker = tmp_path / "injected"
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_OUTAGE_OVERRIDE": "1",
           "REVIEW_BASE": "refs/heads/owner's-fix"}
    done = subprocess.run(["bash", str(REPO / "scripts/overnight/review-broker-codex-dispatch.sh"),
                           f"[review-kind: completion] todo/00-x/TODO-01-x.md section 7\nIt's fine; $(touch {marker})"],
                          capture_output=True, text=True, timeout=60, cwd=str(repo), env=env)
    assert done.returncode == 0, done.stderr
    log = pathlib.Path(json.loads(done.stdout.strip().splitlines()[-1])["logFile"])
    deadline = time.time() + 60
    while time.time() < deadline and not (log.exists() and "Turn completed (rc=" in log.read_text()):
        time.sleep(0.2)
    assert "Turn completed (rc=0)" in log.read_text(), log.read_text()[-400:]
    assert "It's fine; $(touch" in (home / "prompt.txt").read_text(), "the prompt arrived as written"
    assert not marker.exists(), "nothing in a value or the prompt was run as a command"


def test_two_passes_of_one_kind_in_the_same_second_keep_their_own_transcripts(tmp_path):
    """Review finding: the transcript name was the second and the kind, so two same-kind passes started
    together wrote one file and each was checked against the other's verdict."""
    import os
    import subprocess
    fake = tmp_path / "companion.js"
    fake.write_text(FAKE_COMPANION)
    pin = _pin()
    clean = "\n".join(f"LENS {n}: clean" for n in pin["lenses"]["completion"]) + "\nHIGHEST: none\n"
    procs = []
    for n, review in enumerate((clean, "no verdict lines here\n")):
        (tmp_path / f"review{n}.txt").write_text(review)
        env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_REVIEWS_DIR": str(tmp_path / "reviews"),
               "FAKE_PROMPT_OUT": str(tmp_path / f"prompt{n}.txt"), "FAKE_REVIEW": str(tmp_path / f"review{n}.txt"),
               "REVIEW_BASE": "HEAD"}
        procs.append(subprocess.Popen(
            ["bash", str(REPO / "scripts/codex-dispatch.sh"),
             f"[review-kind: completion] todo/00-x/TODO-01-x.md section {n + 1}\nReview it."],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=str(_fixture_repo(tmp_path)), env=env))
    codes = []
    for proc in procs:
        # deadline: 120 s for a fake reviewer that answers at once.
        out = proc.communicate(timeout=120)
        codes.append((proc.returncode, out[1][-600:]))
    assert [c for c, _e in codes] == [0, 3], codes
    assert len(list((tmp_path / "reviews").glob("*-completion-direct.out"))) == 2


def test_the_worst_level_of_a_review_is_read_from_its_last_highest_line():
    assert rp.highest("LENS record: clean\nHIGHEST: none\n") == "none"
    assert rp.highest("- HIGHEST: P2\n") == "P2" and rp.highest("`HIGHEST: P1`") == "P1"
    assert rp.highest("HIGHEST: P2\nlater text\nHIGHEST: P2\n") == "P2"
    for bad in ("", "HIGHEST: P4", "HIGHEST P1", "the highest is P1", "HIGHEST: none at all"):
        assert rp.highest(bad) == "", bad


def test_the_receipt_carries_the_worst_level_and_a_review_without_one_is_incomplete(tmp_path):
    import json
    pin = _pin()
    verdicts = "\n".join(f"LENS {n}: findings" for n in pin["lenses"]["performance"])
    done, prompt = _dispatch(tmp_path, "performance", verdicts + "\nHIGHEST: P2\n")
    assert done.returncode == 0 and "- optimization: " in prompt, done.stderr
    done, _ = _dispatch(tmp_path, "performance", verdicts + "\n")
    assert done.returncode == 3 and "no HIGHEST: line" in done.stderr, done.stderr
    lines = [json.loads(ln) for ln in (tmp_path / "reviews/manifest.jsonl").read_text().splitlines()]
    assert [(e["kind"], e["rc"], e["highest"]) for e in lines] == [("performance", 0, "P2"), ("performance", 3, "")]


def test_refusal_a_verdict_inside_an_example_or_contradicted_is_no_verdict():
    """Review finding: a fenced `HIGHEST: none` after a real `HIGHEST: P1` made the review read as clean, and
    an example alone read as a verdict."""
    fence = "```"
    assert rp.highest(f"HIGHEST: P1\n{fence}\nHIGHEST: none\n{fence}\n") == "P1"
    assert rp.highest(f"A good ending looks like:\n{fence}\nHIGHEST: none\n{fence}\n") == ""
    assert rp.highest("HIGHEST: P1\nHIGHEST: none\n") == "", "two different levels: no verdict"
    pin = _pin()
    a, b = pin["lenses"]["completion"]
    assert rp.missing_lens_verdicts(pin, "completion", f"{fence}\nLENS {a}: clean\nLENS {b}: clean\n{fence}\n") == (a, b)
    assert rp.missing_lens_verdicts(pin, "completion", f"LENS {a}: clean\nLENS {a}: findings\nLENS {b}: clean") == (a,)
    assert rp.missing_lens_verdicts(pin, "completion", f"LENS {a}: clean\nLENS {a}: clean\nLENS {b}: clean") == ()


MOVING_COMPANION = """
const fs = require('fs');
const { execSync } = require('child_process');
fs.writeFileSync(process.env.FAKE_PROMPT_OUT, (a => a[0] === '--' ? a.slice(1).join(' ') : a.join(' '))(process.argv.slice(3)));
execSync(process.env.FAKE_DURING, { stdio: 'ignore' });
process.stdout.write('# Codex Adversarial Review\\n\\n' + fs.readFileSync(process.env.FAKE_REVIEW, 'utf8'));
"""


def test_refusal_a_candidate_that_moved_during_the_review_is_not_the_one_reviewed(tmp_path):
    """Review finding: the receipt named the HEAD read after the reviewer exited, so a commit made by another
    caller during the review was recorded as the candidate reviewed."""
    import json
    import os
    import subprocess

    def git(*args):
        return subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                               "-c", "core.hooksPath=/dev/null", *args], capture_output=True, text=True,
                              timeout=60, check=True).stdout.strip()

    repo = tmp_path / "repo"
    repo.mkdir()
    git("init", "-q")
    (repo / "a.txt").write_text("one\n")
    git("add", "-A")
    git("commit", "-q", "-m", "reviewed")
    reviewed = git("rev-parse", "HEAD")
    fake = tmp_path / "companion.js"
    fake.write_text(MOVING_COMPANION)
    pin = _pin()
    (tmp_path / "review.txt").write_text(
        "\n".join(f"LENS {n}: clean" for n in pin["lenses"]["completion"]) + "\nHIGHEST: none\n")
    moved = ("git -c user.name=t -c user.email=t@example.invalid -c core.hooksPath=/dev/null "
             "commit -q --allow-empty -m during")
    for during, want_rc in ((moved, 3), ("true", 0)):
        env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_REVIEWS_DIR": str(tmp_path / "reviews"),
               "FAKE_PROMPT_OUT": str(tmp_path / "prompt.txt"), "FAKE_REVIEW": str(tmp_path / "review.txt"),
               "FAKE_DURING": during, "REVIEW_BASE": "HEAD"}
        start = git("rev-parse", "HEAD")
        done = subprocess.run(["bash", str(REPO / "scripts/codex-dispatch.sh"),
                               "[review-kind: completion] todo/x.md section 9\nReview it."],
                              capture_output=True, text=True, timeout=120, cwd=str(repo), env=env)
        assert done.returncode == want_rc, (during, done.returncode, done.stderr)
        receipt = json.loads((tmp_path / "reviews/manifest.jsonl").read_text().splitlines()[-1])
        assert receipt["head"] == start and receipt["rc"] == want_rc, receipt
        if want_rc == 3:
            assert receipt["head"] == reviewed and "candidate changed while it was being reviewed" in done.stderr


def test_refusal_every_kind_of_fence_hides_its_contents():
    """Review finding: only triple backticks were tracked, so a tilde block, or a short fence inside a longer
    one, still passed an example off as the verdict."""
    pin = _pin()
    a, b = pin["lenses"]["completion"]
    body = f"LENS {a}: clean\nLENS {b}: clean\nHIGHEST: none\n"
    for opening, closing in (("~~~", "~~~"), ("~~~~", "~~~~"), ("```text", "```"), ("````", "````"),
                             ("~~~ example", "~~~")):
        text = f"{opening}\n{body}{closing}\n"
        assert rp.highest(text) == "" and rp.missing_lens_verdicts(pin, "completion", text) == (a, b), opening
    # A shorter or different fence inside the block does not close it.
    for inner in ("```", "~~~", "```` trailing"):
        text = f"````\n{inner}\n{body}````\n"
        assert rp.highest(text) == "", inner
    # A block left open runs to the end.
    assert rp.highest(f"```\n{body}") == "" and rp.highest(f"~~~\n{body}") == ""
    # What is outside still counts, before and after a block, and in inline code.
    assert rp.highest(f"HIGHEST: P2\n~~~\nHIGHEST: none\n~~~\n") == "P2"
    assert rp.highest(f"~~~\nHIGHEST: none\n~~~\n`HIGHEST: P1`\n") == "P1"
    assert rp.missing_lens_verdicts(pin, "completion", f"```\nexample\n```\n- LENS {a}: clean\n`LENS {b}: findings`") == ()


def test_manifest_lines_are_serialized_not_formatted(tmp_path):
    """Review finding: manifest lines were built with printf, so a path holding a quote, a backslash or a
    newline produced a line no reader could parse, and every later dispatch added another."""
    import json
    import subprocess
    tool = str(REPO / "scripts/overnight/manifest-line.py")
    out = tmp_path / "m.jsonl"
    awkward = 'a "quoted"\\path\nwith a second line'
    for args in ([f"logFile={awkward}", "int:section=7", "bool:pass_checked=true", "opt:int:rc=", "highest="],
                 ["kind=round", "opt:int:section=12"]):
        done = subprocess.run([sys.executable, tool, "--append", str(out), *args], capture_output=True, text=True,
                              timeout=30)
        assert done.returncode == 0, done.stderr
    lines = [json.loads(ln) for ln in out.read_text().splitlines()]
    assert lines == [{"logFile": awkward, "section": 7, "pass_checked": True, "highest": ""},
                     {"kind": "round", "section": 12}]
    printed = subprocess.run([sys.executable, tool, "kind=plan"], capture_output=True, text=True, timeout=30)
    assert json.loads(printed.stdout) == {"kind": "plan"}
    for bad in (["no-equals-sign"], ["int:section=seven"], ["bool:pass_checked=yes"], ["--append"],
                ["--append", str(tmp_path / "no-such-dir" / "m.jsonl"), "kind=x"]):
        assert subprocess.run([sys.executable, tool, *bad], capture_output=True, timeout=30).returncode == 2, bad


def test_a_receipt_survives_an_awkward_reviews_directory(tmp_path):
    import json
    pin = _pin()
    awkward = tmp_path / 'a "quoted" dir\nsecond line'
    awkward.mkdir()
    full = "\n".join(f"LENS {n}: clean" for n in pin["lenses"]["completion"]) + "\nHIGHEST: none\n"
    done, _prompt = _dispatch(tmp_path, "completion", full, reviews_dir=awkward)
    assert done.returncode == 0, done.stderr
    receipt = json.loads((awkward / "manifest.jsonl").read_text())
    assert receipt["pass_checked"] is True and receipt["rc"] == 0 and receipt["section"] == 99 \
        and receipt["logFile"].startswith(str(awkward)), receipt


def test_a_section_is_told_which_review_rules_it_is_on(tmp_path):
    """The skills name the older kinds. A section in flight under them must finish on them, and a section that
    starts now must use the passes, so each one asks before its first dispatch."""
    import json
    import subprocess
    todo = "todo/00-x/TODO-01-x.md"
    assert rp.section_status(tmp_path, todo, 7) == ("new", "")
    stamps = tmp_path / rp.STAMPS
    stamps.parent.mkdir(parents=True)
    stamps.write_text(json.dumps({todo: {"adversarial": 5, "adversarial_section": "7", "perf": 6, "perf_section": "6"}}))
    assert rp.section_status(tmp_path, todo, 7) == ("older", "")
    assert rp.section_status(tmp_path, todo, "6") == ("older", "")
    assert rp.section_status(tmp_path, todo, 8) == ("new", ""), "another section of the same TODO starts on the passes"
    assert rp.section_status(tmp_path, "todo/00-x/TODO-02-y.md", 7) == ("new", "")
    manifest = tmp_path / rp.MANIFEST
    manifest.parent.mkdir(parents=True)
    lines = [{"kind": "adversarial", "todo": todo, "section": 8, "head": "a" * 40},
             {"kind": "plan", "todo": todo, "section": 8, "head": "b" * 40, "rc": 3, "pass_checked": True},
             {"kind": "plan", "todo": todo, "section": 9, "head": "c" * 40, "rc": 0, "pass_checked": True,
              "route": "codex-dispatch.sh"}]
    # A torn last line that provably names another TODO; one that may be this TODO's refuses (tested below).
    torn = '{"ts": 1795170000, "kind": "plan", "todo": "todo/00-x/TODO-02-y.md", "sec'
    manifest.write_text("\n".join(json.dumps(e) for e in lines) + "\n" + torn + "\n")
    assert rp.section_status(tmp_path, todo, 8) == ("passes", ""), "an incomplete plan is no base"
    assert rp.section_status(tmp_path, todo, 9) == ("passes", "c" * 40)
    assert rp.section_status(tmp_path, todo, 7) == ("older", ""), "its own receipts decide, not another section's"
    done = subprocess.run([sys.executable, str(REPO / "scripts/review_pin.py"), "--root", str(tmp_path),
                           "--section", todo, "9"], capture_output=True, text=True, timeout=30)
    assert done.returncode == 0 and done.stdout.strip() == "passes REVIEW_BASE=" + "c" * 40, done
    for skill in ("implement-todo-section", "review-todo-section"):
        text = (REPO / ".claude/skills" / skill / "SKILL.md").read_text()
        assert "python3 scripts/review_pin.py --section <todo-path> <N>" in text and "three-pass-review.md" in text, skill
    howto = (REPO / ".claude/skills/three-pass-review.md").read_text()
    for kind in rp.SECTION_KINDS:
        assert f"`{kind}`" in howto, kind
    assert "--kinds round,performance,completion" in howto and "REVIEW_BASE=<sha>" in howto


def test_refusal_an_older_section_whose_stamp_was_overwritten_is_still_older(tmp_path):
    """Review finding: section_status read the older kinds only from the stamp file, which keeps one entry per
    kind per TODO, so reviewing section 8 made section 7 read as new and sent it for a plan after its code."""
    import json
    todo = "todo/00-x/TODO-01-x.md"
    (tmp_path / rp.STAMPS).parent.mkdir(parents=True)
    (tmp_path / rp.STAMPS).write_text(json.dumps({todo: {"adversarial": 9, "adversarial_section": "8"}}))
    manifest = tmp_path / rp.MANIFEST
    manifest.parent.mkdir(parents=True)
    manifest.write_text(json.dumps({"kind": "adversarial", "todo": todo, "section": 7, "head": "a" * 40}) + "\n")
    assert rp.section_status(tmp_path, todo, 7) == ("older", "")
    assert rp.section_status(tmp_path, todo, 8) == ("older", "")
    assert rp.section_status(tmp_path, todo, 9) == ("new", "")
    # Both kinds of evidence for one section: a pass decides.
    with manifest.open("a") as f:
        f.write(json.dumps({"kind": "plan", "todo": todo, "section": 7, "head": "b" * 40, "rc": 0,
                            "pass_checked": True, "route": "codex-dispatch.sh"}) + "\n")
    assert rp.section_status(tmp_path, todo, 7) == ("passes", "b" * 40)


def test_a_pass_earns_a_step_only_through_a_dispatcher_that_checks_it():
    """Review finding: any command carrying the marker earned the step, so a direct companion call marked
    `round`, which checks nothing and writes no receipt, counted as the mandatory review."""
    sys.path.insert(0, str(REPO / ".claude/hooks"))
    import skill_step_map as m
    prompt = "'[review-kind: round] todo/x.md section 1\nbody; with && shell | characters'"
    wrapper = "REVIEW_BASE=abc bash scripts/codex-dispatch.sh " + prompt
    broker = "bash scripts/overnight/review-broker-codex-dispatch.sh '[review-kind: completion] todo/x.md section 1'"
    direct = "node /x/codex-companion.mjs adversarial-review " + prompt
    quoting = direct[:-1] + " as scripts/codex-dispatch.sh would'"
    assert m.match_step("implement-todo-section", "Bash", wrapper) == [13]
    assert m.match_step("review-todo-section", "Bash", wrapper) == [5]
    assert m.match_step("review-todo-section", "Bash", broker) == [8]
    # Review finding: route and kind were matched apart, over the whole line. They belong to one command.
    # One dispatcher name per source line: lint Check 15 reads two on a line as a bundled dispatch.
    route = "scripts/codex-dispatch.sh"
    credited = (f'bash "{route}" ' + prompt,
                f"REVIEW_BASE='abc' bash {route} " + prompt,
                f"bash /home/x/{route} " + prompt,
                f"cd /x && bash {route} " + prompt)
    for cmd in credited:
        assert m.match_step("implement-todo-section", "Bash", cmd) == [13], cmd
    uncredited = (f"echo {route} ; " + direct,
                  f"bash {route} 'see [review-kind: round] x'",
                  f"bash {route} '[review-kind: round] todo/x.md")
    for cmd in uncredited:
        assert m.match_step("implement-todo-section", "Bash", cmd) == [], cmd
    for cmd in (direct, quoting):
        assert m.match_step("implement-todo-section", "Bash", cmd) == [], cmd
        assert m.match_step("review-todo-section", "Bash", cmd) == [], cmd
    older = "node /x/codex-companion.mjs adversarial-review '[review-kind: adversarial] todo/x.md'"
    assert m.match_step("review-todo-section", "Bash", older) == [5], "the older kinds are bound as before"
    assert set(rp.SECTION_KINDS) <= set(m.bound_review_kinds("implement-todo-section"))


# The second reviewer was removed on 2026-11-15 (the automation-hardening roadmap's removal section). A pin
# that still carries its table is refused, never half-read: the gate would otherwise judge a stamp against a
# schedule nobody wrote.
def test_refusal_a_leftover_second_table_is_refused():
    text = GOOD + '\n[second]\nreviewer = "carbon"\npaths = ["src/kernel/"]\n'
    with pytest.raises(rp.Refused):
        _pin(text)


def test_refusal_a_reviewer_entry_nothing_routes_to_is_refused():
    text = GOOD.replace('[routes]', '[reviewers.carbon]\ncli = "agy"\nmodel = "m"\n\n[routes]')
    with pytest.raises(rp.Refused):
        _pin(text)


def test_a_raw_transcript_is_read_whole_for_its_worst_level(tmp_path, capsys):
    review = tmp_path / "review.out"
    review.write_text("P2 a.c:1 wrong.\nHIGHEST: P2\n")
    assert rp.main(["--highest", "--raw", "--transcript", str(review)]) == 0
    assert capsys.readouterr().out.strip() == "P2"
    assert rp.main(["--highest", "--transcript", str(review)]) == 1     # no Codex heading: no response to read


def _tmp_repo(tmp_path):
    import subprocess
    repo = tmp_path / "repo"
    repo.mkdir()

    def git(*args):
        return subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                               "-c", "core.hooksPath=/dev/null", *args], capture_output=True, text=True,
                              timeout=60, check=True).stdout.strip()
    git("init", "-q")
    git("commit", "-q", "--allow-empty", "-m", "one")
    return repo, git


def _dispatch_in(repo, tmp_path, kind, review, base, extra_env=None):
    import os
    import subprocess
    fake = tmp_path / "companion.js"
    fake.write_text(FAKE_COMPANION)
    (tmp_path / "review.txt").write_text(review)
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_REVIEWS_DIR": str(tmp_path / "reviews"),
           "FAKE_PROMPT_OUT": str(tmp_path / "prompt.txt"), "FAKE_REVIEW": str(tmp_path / "review.txt"),
           "REVIEW_BASE": base, **(extra_env or {})}
    done = subprocess.run(["bash", str(REPO / "scripts/codex-dispatch.sh"),
                           f"[review-kind: {kind}] todo/x.md section 9\nReview it."],
                          capture_output=True, text=True, timeout=120, cwd=str(repo), env=env)
    prompt = (tmp_path / "prompt.txt").read_text() if (tmp_path / "prompt.txt").exists() else ""
    return done, prompt


def test_refusal_a_plan_left_behind_by_a_rewrite_is_not_the_section_start(tmp_path):
    repo, git = _tmp_repo(tmp_path)
    git("checkout", "-q", "-b", "side")
    git("commit", "-q", "--allow-empty", "-m", "side")
    stale = git("rev-parse", "HEAD")
    git("checkout", "-q", "-")
    reviews = tmp_path / "reviews"
    reviews.mkdir()
    (reviews / "manifest.jsonl").write_text(_receipt("plan", stale, todo="todo/x.md", section=9) + "\n")
    done, prompt = _dispatch_in(repo, tmp_path, "round", "HIGHEST: none\n", "")
    assert done.returncode == 2 and "not on the candidate's history" in done.stderr, done.stderr
    assert prompt == ""


def test_refusal_an_edit_to_an_untracked_file_while_the_pass_is_prepared_is_seen(tmp_path):
    """The candidate fingerprint reads file contents: an untracked file edited in place changes no status."""
    import os
    import shutil as sh
    repo, _git = _tmp_repo(tmp_path)
    (repo / "notes.txt").write_text("one\n")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    (bindir / "python3").write_text(f'#!/bin/sh\ncase " $* " in *" --lenses "*) echo two >> "{repo}/notes.txt" ;; esac\n'
                                    f'exec "{sh.which("python3")}" "$@"\n')
    (bindir / "python3").chmod(0o755)
    done, prompt = _dispatch_in(repo, tmp_path, "round", "HIGHEST: none\n", "HEAD",
                                {"PATH": f"{bindir}:{os.environ['PATH']}"})
    assert done.returncode == 2 and "candidate changed while the round pass was prepared" in done.stderr, done.stderr


def test_an_untracked_kernel_source_decides_the_lenses(tmp_path):
    repo, _git = _tmp_repo(tmp_path)
    (repo / "src/kernel").mkdir(parents=True)
    (repo / "src/kernel/new.c").write_text("int x;\n")
    _done, prompt = _dispatch_in(repo, tmp_path, "round", "HIGHEST: none\n", "HEAD")
    assert "- concurrency: " in prompt and "- bare-metal: " in prompt, prompt[-600:]


def test_refusal_an_unreadable_line_after_the_newest_plan_leaves_the_start_unknown(tmp_path):
    todo = "todo/00-infrastructure/TODO-01-developer-tooling-stack.md"
    manifest = tmp_path / "manifest.jsonl"
    torn = _receipt("plan", "b" * 40)[:60]
    manifest.write_text(_receipt("plan", "a" * 40) + "\n" + torn + "\n")
    with pytest.raises(rp.Refused):
        rp.section_start(manifest, todo, 99)
    manifest.write_text(_receipt("plan", "a" * 40) + "\n" + torn + "\n" + _receipt("plan", "c" * 40) + "\n")
    assert rp.section_start(manifest, todo, 99) == "c" * 40


def test_the_manifest_is_read_from_its_end_in_blocks(tmp_path):
    manifest = tmp_path / "m.jsonl"
    rows = [f"row {i} " + "x" * (i % 5) for i in range(40)]
    manifest.write_text("\n".join(rows) + "\n")
    assert list(rp._newest_first(manifest, block=7)) == rows[::-1]


def _snapshot(repo, extra_env=None, timeout=30):
    import os
    import subprocess
    return subprocess.run([sys.executable, str(REPO / "scripts/review_candidate.py"), "snapshot"], cwd=str(repo),
                          capture_output=True, text=True, timeout=timeout, env={**os.environ, **(extra_env or {})})


def test_a_link_to_a_fifo_is_fingerprinted_without_opening_it(tmp_path):
    import os
    repo, _git = _tmp_repo(tmp_path)
    os.mkfifo(tmp_path / "pipe")
    before = _snapshot(repo).stdout
    os.symlink(tmp_path / "pipe", repo / "link")
    done = _snapshot(repo)
    assert done.returncode == 0 and len(done.stdout.strip()) == 64 and done.stdout != before, done


def test_a_special_file_is_recorded_by_its_type_and_never_opened(tmp_path):
    """git status does not list a standalone FIFO, so the reader's type-only record is checked directly."""
    import os
    rc = _rc_module()
    os.mkfifo(tmp_path / "own-pipe")
    top = os.open(tmp_path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        record, note = rc._entry(top, b"own-pipe")
    finally:
        os.close(top)
    assert record.startswith(b"T\0") and "named pipe" in note, (record, note)


def test_a_retargeted_link_changes_the_fingerprint(tmp_path):
    import os
    repo, _git = _tmp_repo(tmp_path)
    (tmp_path / "a").write_text("same\n")
    (tmp_path / "b").write_text("same\n")
    os.symlink(tmp_path / "a", repo / "link")
    first = _snapshot(repo).stdout
    os.unlink(repo / "link")
    os.symlink(tmp_path / "b", repo / "link")
    assert _snapshot(repo).stdout != first


def test_refusal_an_unreadable_untracked_file_refuses_the_fingerprint(tmp_path):
    import os
    repo, _git = _tmp_repo(tmp_path)
    (repo / "a.txt").write_text("one\n")
    (repo / "b.txt").write_text("two\n")
    os.chmod(repo / "a.txt", 0)
    try:
        if os.access(repo / "a.txt", os.R_OK):
            pytest.skip("running with privileges that read any file")
        done = _snapshot(repo)
    finally:
        os.chmod(repo / "a.txt", 0o644)
    assert done.returncode == 2 and "a.txt" in done.stderr, done


def test_literal_pathspecs_from_the_environment_change_nothing(tmp_path):
    repo, _git = _tmp_repo(tmp_path)
    (repo / "src/kernel").mkdir(parents=True)
    (repo / "src/kernel/new.c").write_text("int x;\n")
    plain = _snapshot(repo).stdout
    assert _snapshot(repo, {"GIT_LITERAL_PATHSPECS": "1"}).stdout == plain
    _done, prompt = _dispatch_in(repo, tmp_path, "round", "HIGHEST: none\n", "HEAD", {"GIT_LITERAL_PATHSPECS": "1"})
    assert "- concurrency: " in prompt and "src/kernel/new.c" in prompt, prompt[-600:]


def test_a_non_ascii_name_and_a_rename_out_of_the_kernel_keep_the_kernel_lenses(tmp_path):
    repo, git = _tmp_repo(tmp_path)
    (repo / "src/kernel").mkdir(parents=True)
    (repo / "src/kernel/old.c").write_text("int x;\n")
    git("add", "-A")
    git("commit", "-q", "-m", "kernel file")
    base = git("rev-parse", "HEAD")
    git("mv", "src/kernel/old.c", "elsewhere.c")
    git("commit", "-q", "-m", "moved out")
    _done, prompt = _dispatch_in(repo, tmp_path, "round", "HIGHEST: none\n", base)
    assert "- concurrency: " in prompt and "- bare-metal: " in prompt, prompt[-600:]
    repo2 = tmp_path / "second"
    repo2.mkdir()
    import shutil
    shutil.move(str(repo), str(repo2 / "repo"))
    (repo2 / "repo/src/kernel").mkdir(parents=True, exist_ok=True)
    (repo2 / "repo/src/kernel/caf\u00e9.c").write_text("int y;\n")
    _done, prompt = _dispatch_in(repo2 / "repo", repo2, "round", "HIGHEST: none\n", "HEAD")
    assert "- concurrency: " in prompt, prompt[-600:]


def test_an_untracked_file_is_named_to_the_reviewer(tmp_path):
    repo, _git = _tmp_repo(tmp_path)
    (repo / "fresh.py").write_text("x = 1\n")
    _done, prompt = _dispatch_in(repo, tmp_path, "round", "HIGHEST: none\n", "HEAD")
    assert "appear in no diff; read each one:\nfresh.py" in prompt, prompt[-600:]


def test_a_reviews_directory_name_is_matched_literally(tmp_path):
    """A reviews directory named src/* is that one directory: read as a pattern it would hide every source."""
    repo, _git = _tmp_repo(tmp_path)
    (repo / "src/kernel").mkdir(parents=True)
    (repo / "src/kernel/new.c").write_text("int x;\n")
    odd = repo / "src/*"
    odd.mkdir()
    (odd / "transcript.out").write_text("grows\n")
    _done, prompt = _dispatch_in(repo, tmp_path, "round", "HIGHEST: none\n", "HEAD", {"CODEX_REVIEWS_DIR": str(odd)})
    listed = prompt.split("appear in no diff", 1)[-1]
    assert "src/kernel/new.c" in listed and "transcript.out" not in listed, prompt[-600:]
    assert "- concurrency: " in prompt, prompt[-600:]


def test_a_link_is_shown_with_its_target_not_as_a_file_to_read(tmp_path):
    import os
    repo, _git = _tmp_repo(tmp_path)
    (tmp_path / "outside.txt").write_text("secret\n")
    os.symlink(tmp_path / "outside.txt", repo / "link")
    _done, prompt = _dispatch_in(repo, tmp_path, "round", "HIGHEST: none\n", "HEAD")
    assert f"link (a symbolic link to {tmp_path / 'outside.txt'}: review the link text" in prompt, prompt[-600:]


def test_a_long_untracked_list_is_kept_beside_the_transcript(tmp_path):
    repo, _git = _tmp_repo(tmp_path)
    for i in range(60):
        (repo / f"new-{i:02}.txt").write_text(f"{i}\n")
    _done, prompt = _dispatch_in(repo, tmp_path, "round", "HIGHEST: none\n", "HEAD")
    kept = prompt.split("read each one listed in ", 1)[1].split("\n", 1)[0].strip()
    assert "60 untracked entries" in prompt and "new-07.txt" not in prompt, prompt[-600:]
    assert len(open(kept).read().splitlines()) == 60


def test_refusal_a_plan_base_from_another_branch(tmp_path):
    repo, git = _tmp_repo(tmp_path)
    git("checkout", "-q", "-b", "side")
    git("commit", "-q", "--allow-empty", "-m", "side")
    stale = git("rev-parse", "HEAD")
    git("checkout", "-q", "-")
    done, prompt = _dispatch_in(repo, tmp_path, "plan", "HIGHEST: none\n", stale)
    assert done.returncode == 2 and "not on the candidate's history" in done.stderr and prompt == "", done.stderr


def test_refusal_a_resume_and_a_dispatch_read_one_section_start(tmp_path):
    todo = "todo/00-infrastructure/TODO-01-developer-tooling-stack.md"
    manifest = tmp_path / rp.MANIFEST
    manifest.parent.mkdir(parents=True)
    manifest.write_text(_receipt("plan", "a" * 40) + "\n" + _receipt("plan", "b" * 40)[:60] + "\n")
    with pytest.raises(rp.Refused):
        rp.section_status(tmp_path, todo, 99)


def test_the_review_dispatch_code_is_in_the_control_plane():
    import subprocess
    for rel in ("scripts/review_candidate.py", "scripts/codex-dispatch.sh", "scripts/review_pin.py"):
        r = subprocess.run(["bash", str(REPO / "scripts/overnight/control-plane-match.sh"), rel],
                           capture_output=True, text=True, timeout=30)
        assert r.returncode == 0 and r.stdout.strip() == rel, (rel, r)


def _rc_module():
    import importlib.util
    spec = importlib.util.spec_from_file_location("rc", REPO / "scripts/review_candidate.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_refusal_a_file_that_grows_while_it_is_read(tmp_path, monkeypatch):
    import os
    rc = _rc_module()
    repo, _git = _tmp_repo(tmp_path)
    (repo / "log.txt").write_text("one\n")
    real, grown = rc.os.read, []

    def read(fd, n):
        chunk = real(fd, n)
        if len(grown) < 3:                                              # a writer still appending, then done
            grown.append(1)
            with open(repo / "log.txt", "a") as f:
                f.write("more\n")
        return chunk
    monkeypatch.setattr(rc.os, "read", read)
    with pytest.raises(rc.Refused, match="changed while it was read"):
        rc._entry(os.open(repo, os.O_RDONLY | os.O_DIRECTORY), b"log.txt")


def test_the_fingerprint_is_the_hash_of_exactly_what_it_names(tmp_path, monkeypatch):
    import hashlib
    import subprocess
    rc = _rc_module()
    repo, git = _tmp_repo(tmp_path)
    (repo / "t.txt").write_text("one\n")
    git("add", "-A")
    git("commit", "-q", "-m", "t")
    (repo / "t.txt").write_text("two\n")
    monkeypatch.chdir(repo)
    patch = subprocess.run(["git", "diff", "HEAD", "--binary", "--no-ext-diff", "--no-textconv", "--no-renames"],
                           capture_output=True, check=True, timeout=30).stdout
    status = subprocess.run(["git", "status", "--porcelain=v1", "-z", "--untracked-files=all", "--no-renames"],
                            capture_output=True, check=True, timeout=30).stdout
    assert rc.snapshot() == hashlib.sha256(patch + status).hexdigest()


def test_preparing_a_pass_walks_the_status_once(tmp_path, monkeypatch):
    rc = _rc_module()
    repo, git = _tmp_repo(tmp_path)
    (repo / "u.txt").write_text("u\n")
    monkeypatch.chdir(repo)
    calls = []
    real = rc._status
    monkeypatch.setattr(rc, "_status", lambda *a: calls.append(a) or real(*a))
    head = git("rev-parse", "HEAD")
    _digest, covered, notes = rc.prepare(head, head)
    assert len(calls) == 1 and covered == [b"u.txt"] and notes == [(b"u.txt", None)]


def test_a_repository_with_no_commit_yet_has_a_fingerprint(tmp_path):
    import subprocess
    repo = tmp_path / "fresh"
    repo.mkdir()
    subprocess.run(["git", "init", "-q", str(repo)], check=True, capture_output=True, timeout=30)
    empty = _snapshot(repo)
    assert empty.returncode == 0 and empty.stdout.strip() == rp.CLEAN_WORKTREE, empty
    (repo / "a.txt").write_text("a\n")
    subprocess.run(["git", "-C", str(repo), "add", "a.txt"], check=True, timeout=30)
    staged = _snapshot(repo)
    assert staged.returncode == 0 and staged.stdout != empty.stdout, staged


def test_a_helper_holding_the_reader_s_pipe_is_ended_at_the_deadline(tmp_path, monkeypatch):
    """git exits but a helper it started keeps stderr open: the read ends at its deadline, helper and all."""
    import os
    import time
    rc = _rc_module()
    bindir = tmp_path / "bin"
    bindir.mkdir()
    pidfile = tmp_path / "helper.pid"
    (bindir / "git").write_text(f"#!/bin/sh\nsh -c 'echo $$ > \"{pidfile}\"; exec sleep 61' &\nexit 0\n")
    (bindir / "git").chmod(0o755)
    monkeypatch.setenv("PATH", f"{bindir}:{os.environ['PATH']}")
    monkeypatch.setattr(rc, "TIMEOUT", 1)
    began = time.monotonic()
    try:
        with pytest.raises(rc.Refused, match="did not answer within 1 s"):
            rc._git(str(tmp_path), "status")
        assert time.monotonic() - began < 4
        pid = int(pidfile.read_text())
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and _proc_alive(pid):
            time.sleep(0.05)
        assert not _proc_alive(pid), "the helper outlived the read"
    finally:
        _reap_pid(pidfile)


def _proc_alive(pid):
    import pathlib
    try:
        return pathlib.Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0] not in ("Z", "X")
    except (FileNotFoundError, ProcessLookupError):        # gone before the open, or during the read (ESRCH)
        return False


def _reap_pid(pidfile):
    import os
    import signal
    try:
        os.kill(int(pidfile.read_text()), signal.SIGKILL)
    except (FileNotFoundError, ValueError, ProcessLookupError):   # never started, or already gone
        pass


def test_refusal_a_directory_replaced_by_a_link_is_not_followed(tmp_path):
    import os
    rc = _rc_module()
    repo = tmp_path / "repo"
    (tmp_path / "outside").mkdir()
    (tmp_path / "outside/f").write_text("outside\n")
    repo.mkdir()
    os.symlink(tmp_path / "outside", repo / "d")              # was a directory when status listed d/f
    top_fd = os.open(repo, os.O_RDONLY | os.O_DIRECTORY)
    try:
        with pytest.raises(rc.Refused, match="could not be reached"):
            rc._entry(top_fd, b"d/f")
    finally:
        os.close(top_fd)


def test_both_paths_file_forms_set_the_kernel_lenses(tmp_path):
    import subprocess
    lines = tmp_path / "paths.txt"
    lines.write_text("docs/example.md\nsrc/kernel/example.c\n")
    nul = tmp_path / "paths.bin"
    nul.write_bytes(b"docs/example.md\0src/kernel/example.c\0")
    for flag, path in (("--paths-file", lines), ("--paths-file0", nul)):
        done = subprocess.run([sys.executable, str(REPO / "scripts/review_pin.py"), "--lenses", "round", flag, str(path)],
                              capture_output=True, text=True, timeout=30)
        assert done.returncode == 0 and "- concurrency: " in done.stdout and "- bare-metal: " in done.stdout, (flag, done)


def test_a_clean_candidate_takes_no_patch_and_no_tree_name_diff(tmp_path, monkeypatch):
    rc = _rc_module()
    repo, git = _tmp_repo(tmp_path)
    monkeypatch.chdir(repo)
    calls = []
    real = rc._git
    monkeypatch.setattr(rc, "_git", lambda top, *a, **k: calls.append(a) or real(top, *a, **k))
    head = git("rev-parse", "HEAD")
    digest, covered, notes = rc.prepare(head, head)
    assert digest == rp.CLEAN_WORKTREE and covered == [] and notes == []
    assert not any("--binary" in a for a in calls), calls
    assert sum(1 for a in calls if a[:2] == ("diff", "--name-only")) == 1, calls   # only base..head


def test_the_section_status_reads_the_manifest_once(tmp_path, monkeypatch):
    todo = "todo/00-x/TODO-01-x.md"
    manifest = tmp_path / rp.MANIFEST
    manifest.parent.mkdir(parents=True)
    manifest.write_text(_receipt("plan", "c" * 40, section=9, todo=todo) + "\n")
    reads = []
    real, real_text = rp._newest_first, rp.pathlib.Path.read_text
    monkeypatch.setattr(rp, "_newest_first", lambda *a, **k: reads.append(a) or real(*a, **k))
    monkeypatch.setattr(rp.pathlib.Path, "read_text",
                        lambda self, *a, **k: (reads.append(self) if self == manifest else None) or real_text(self, *a, **k))
    assert rp.section_status(tmp_path, todo, 9) == ("passes", "c" * 40)
    assert len(reads) == 1, reads


def test_the_broker_and_its_reviewer_fingerprint_one_scope(tmp_path):
    """A pinned pass through the broker: an inherited CODEX_REVIEWS_DIR naming another directory changes
    neither digest, and the launch row and the wrapper's receipt name one candidate."""
    import json
    import os
    import subprocess
    import time
    repo, _git = _tmp_repo(tmp_path)
    (repo / "other").mkdir()
    (repo / "other/new.txt").write_text("part of the candidate\n")
    fake = tmp_path / "companion.js"                     # the detached job gets no test variables: self-contained
    fake.write_text("process.stdout.write('[codex] working\\n# Codex Adversarial Review\\n\\nHIGHEST: none\\n');\n")
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_OUTAGE_OVERRIDE": "1", "REVIEW_BASE": "HEAD",
           "CODEX_REVIEWS_DIR": str(repo / "other")}
    done = subprocess.run(["bash", str(REPO / "scripts/overnight/review-broker-codex-dispatch.sh"),
                           "[review-kind: round] todo/x.md section 9\nReview it."],
                          capture_output=True, text=True, timeout=60, cwd=str(repo), env=env)
    assert done.returncode == 0, done.stderr
    log = pathlib.Path(json.loads(done.stdout.strip().splitlines()[-1])["logFile"])
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline and not pathlib.Path(f"{log}.done").exists():
        time.sleep(0.1)
    assert pathlib.Path(f"{log}.done").exists(), "the detached pass never finished"   # its verdict is not the point
    rows = [json.loads(line) for line in (repo / ".claude/overnight/reviews/manifest.jsonl").read_text().splitlines()]
    launch = next(r for r in rows if r.get("route") != "codex-dispatch.sh")
    receipt = next(r for r in rows if r.get("route") == "codex-dispatch.sh")
    assert launch["worktree_sha256"] == receipt["worktree_sha256"] != rp.CLEAN_WORKTREE, (launch, receipt)
    assert launch["head"] == receipt["head"] and launch["index_tree"] == receipt["index_tree"], (launch, receipt)


def test_refusal_a_commit_while_the_launch_identity_is_read(tmp_path, monkeypatch):
    rc = _rc_module()
    repo, git = _tmp_repo(tmp_path)
    monkeypatch.chdir(repo)
    first = git("rev-parse", "HEAD")
    heads = iter([first, "f" * 40])
    monkeypatch.setattr(rc, "_head", lambda top: next(heads))
    with pytest.raises(rc.Refused, match="changed while it was read"):
        rc.launch()


def test_the_launch_identity_names_head_index_and_digest(tmp_path, monkeypatch):
    rc = _rc_module()
    repo, git = _tmp_repo(tmp_path)
    monkeypatch.chdir(repo)
    (repo / "n.txt").write_text("x\n")
    head, index, digest = rc.launch()
    import hashlib
    import subprocess
    staged = subprocess.run(["git", "-C", str(repo), "ls-files", "-s", "-z"], capture_output=True, check=True,
                            timeout=30).stdout
    assert head == git("rev-parse", "HEAD") and digest == rc.snapshot()
    assert index == "staged-" + hashlib.sha256(staged).hexdigest()


def test_a_checkout_whose_name_ends_in_a_newline_is_read_as_itself(tmp_path, monkeypatch):
    import subprocess
    rc = _rc_module()
    for name, text in (("a", "sibling\n"), ("a\n", "this one\n")):
        root = tmp_path / name
        root.mkdir()
        subprocess.run(["git", "init", "-q", str(root)], check=True, capture_output=True, timeout=30)
        (root / "f.txt").write_text(text)
    monkeypatch.chdir(tmp_path / "a\n")
    assert rc._top() == str(tmp_path / "a\n")
    assert rc.prepare(rc.EMPTY_TREE, rc.EMPTY_TREE)[2] == [(b"f.txt", None)]
    monkeypatch.chdir(tmp_path / "a")
    assert rc.snapshot() != (monkeypatch.chdir(tmp_path / "a\n") or rc.snapshot())


def test_an_untracked_only_candidate_takes_no_tracked_diff(tmp_path, monkeypatch):
    rc = _rc_module()
    repo, git = _tmp_repo(tmp_path)
    monkeypatch.chdir(repo)
    (repo / "new.c").write_text("int x;\n")
    calls = []
    real = rc._git
    monkeypatch.setattr(rc, "_git", lambda top, *a, **k: calls.append(a) or real(top, *a, **k))
    head = git("rev-parse", "HEAD")
    digest, covered, _notes = rc.prepare(head, head)
    assert digest != rp.CLEAN_WORKTREE and covered == [b"new.c"]
    assert not any("--binary" in a for a in calls), calls
    assert sum(1 for a in calls if a[:2] == ("diff", "--name-only")) == 1, calls   # only base..head


def test_the_fixtures_read_no_git_config_of_the_operators(tmp_path, monkeypatch):
    """A global config that signs every commit with a program that cannot sign does not reach a fixture."""
    home = tmp_path / "home"
    home.mkdir()
    (home / ".gitconfig").write_text("[commit]\n\tgpgSign = true\n[gpg]\n\tprogram = false\n")
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.setenv("XDG_CONFIG_HOME", str(home))
    repo, git = _tmp_repo(tmp_path)
    assert git("rev-parse", "HEAD")


def test_refusal_a_broker_that_cannot_read_the_tree_leaves_no_prompt_behind(tmp_path):
    import os
    import subprocess
    repo, _git = _tmp_repo(tmp_path)
    (repo / "locked.txt").write_text("x\n")
    (repo / "locked.txt").chmod(0)
    temp = tmp_path / "tmp"
    temp.mkdir()
    fake = tmp_path / "companion.js"
    fake.write_text("process.stdout.write('fine');\n")
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_OUTAGE_OVERRIDE": "1", "TMPDIR": str(temp)}
    try:
        if os.access(repo / "locked.txt", os.R_OK):
            pytest.skip("this user reads a mode-0 file")          # before the broker, so it launches nothing
        done = subprocess.run(["bash", str(REPO / "scripts/overnight/review-broker-codex-dispatch.sh"),
                               "[review-kind: adversarial] todo/x.md section 9\nReview it."],
                              capture_output=True, text=True, timeout=60, cwd=str(repo), env=env)
    finally:
        (repo / "locked.txt").chmod(0o644)
    assert done.returncode == 2 and "could not be read" in done.stderr, done.stderr
    assert list(temp.iterdir()) == []


def test_refusal_an_untracked_file_saved_over_while_it_is_read(tmp_path, monkeypatch):
    import os
    rc = _rc_module()
    repo, _git = _tmp_repo(tmp_path)
    monkeypatch.chdir(repo)
    (repo / "notes.txt").write_text("old\n")
    (repo / "replacement").write_text("new\n")
    real, done = rc.os.read, []

    def read(fd, n):
        if not done and os.readlink(f"/proc/self/fd/{fd}").endswith("/notes.txt"):
            done.append(1)
            os.replace(repo / "replacement", repo / "notes.txt")   # an editor's atomic save
        return real(fd, n)
    monkeypatch.setattr(rc.os, "read", read)
    with pytest.raises(rc.Refused, match="notes.txt was replaced while it was read"):
        rc.snapshot()


def test_an_index_that_cannot_be_read_refuses_the_launch(tmp_path, monkeypatch):
    rc = _rc_module()
    repo, _git = _tmp_repo(tmp_path)
    monkeypatch.chdir(repo)
    real_run = rc._run
    monkeypatch.setattr(rc, "_run", lambda top, args, sink=None: (128, b"", b"index file corrupt")
                        if args[0] == "ls-files" else real_run(top, args, sink))
    with pytest.raises(rc.Refused, match="the index could not be read"):
        rc.launch()


def test_two_launch_reads_at_once_do_not_refuse_each_other(tmp_path):
    """git write-tree locked the index: three passes of one wave, launched together, refused each other."""
    import subprocess
    repo, git = _tmp_repo(tmp_path)
    (repo / "a.txt").write_text("1\n")
    git("add", "a.txt")
    import time
    procs, outs = [], []
    deadline = time.monotonic() + 60                   # one deadline for the cohort, not one per reader
    try:
        for _ in range(8):
            procs.append(subprocess.Popen([sys.executable, str(REPO / "scripts/review_candidate.py"), "launch"],
                                          cwd=str(repo), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True))
        for p in procs:
            outs.append(p.communicate(timeout=max(0.1, deadline - time.monotonic())))
    finally:
        for p in procs:                                # TERM first: a reader ends its own git group on it
            if p.poll() is None:
                p.terminate()
        for p in procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
    assert all(p.returncode == 0 for p in procs) and len({o for o, _e in outs}) == 1, outs


def test_refusal_a_wrapper_handed_another_candidate_than_its_own(tmp_path):
    repo, _git = _tmp_repo(tmp_path)
    done, prompt = _dispatch_in(repo, tmp_path, "round", "HIGHEST: none\n", "HEAD",
                                {"CODEX_EXPECT_CANDIDATE": "a b c"})
    assert done.returncode == 2 and "changed between the broker's launch" in done.stderr, done.stderr
    assert prompt == ""                                                    # the reviewer never started


def test_an_index_change_during_the_review_makes_the_pass_incomplete(tmp_path):
    """Only the staged hunks change: the working tree and its status (MM) stay as they were."""
    import os
    import subprocess
    repo, git = _tmp_repo(tmp_path)
    (repo / "a.txt").write_text("1\n2\n3\n")
    git("add", "a.txt")
    git("commit", "-q", "-m", "a")

    def stage(text):
        blob = subprocess.run(["git", "-C", str(repo), "hash-object", "-w", "--stdin"], input=text, text=True,
                              capture_output=True, check=True, timeout=30).stdout.strip()
        return ["git", "-C", str(repo), "update-index", "--cacheinfo", f"100644,{blob},a.txt"]
    subprocess.run(stage("X\n2\n3\n"), check=True, timeout=30)
    (repo / "a.txt").write_text("X\n2\nY\n")
    swap = " ".join(f"'{a}'" for a in stage("1\n2\nY\n"))
    fake = tmp_path / "companion.js"
    fake.write_text("require('child_process').execSync(process.env.FAKE_DURING);\n"
                    "process.stdout.write('# Codex Adversarial Review\\n\\nHIGHEST: none\\n');\n")
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_REVIEWS_DIR": str(tmp_path / "reviews"),
           "REVIEW_BASE": "HEAD", "FAKE_DURING": swap}
    done = subprocess.run(["bash", str(REPO / "scripts/codex-dispatch.sh"),
                           "[review-kind: round] todo/x.md section 9\nReview it."],
                          capture_output=True, text=True, timeout=120, cwd=str(repo), env=env)
    assert "the candidate changed while it was being reviewed" in done.stderr, done.stderr


def test_refusal_a_candidate_that_moves_before_the_detached_pass_starts(tmp_path):
    """The broker registers one candidate; a change before its detached wrapper starts refuses the pass."""
    import json
    import os
    import subprocess
    import time
    repo, _git = _tmp_repo(tmp_path)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    # Stands in for the scheduler: the tree moves after registration, then the setsid route launches the job.
    (bindir / "systemd-run").write_text(f'#!/bin/sh\necho moved > "{repo}/late.txt"\nexit 1\n')
    (bindir / "systemd-run").chmod(0o755)
    fake = tmp_path / "companion.js"
    fake.write_text("process.stdout.write('# Codex Adversarial Review\\n\\nHIGHEST: none\\n');\n")
    env = {**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}", "CODEX_COMPANION_PATH": str(fake),
           "CODEX_OUTAGE_OVERRIDE": "1", "REVIEW_BASE": "HEAD"}
    done = subprocess.run(["bash", str(REPO / "scripts/overnight/review-broker-codex-dispatch.sh"),
                           "[review-kind: round] todo/x.md section 9\nReview it."],
                          capture_output=True, text=True, timeout=60, cwd=str(repo), env=env)
    assert done.returncode == 0, done.stderr
    log = pathlib.Path(json.loads(done.stdout.strip().splitlines()[-1])["logFile"])
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline and not pathlib.Path(f"{log}.done").exists():
        time.sleep(0.1)
    assert pathlib.Path(f"{log}.done").read_text().strip() == "2", log.read_text()
    assert "changed between the broker's launch" in log.read_text()


def test_refusal_a_directory_git_status_cannot_open(tmp_path, monkeypatch):
    import os
    rc = _rc_module()
    repo, _git = _tmp_repo(tmp_path)
    monkeypatch.chdir(repo)
    (repo / "d").mkdir()
    (repo / "d/hidden.c").write_text("int x;\n")
    (repo / "d").chmod(0)
    try:
        if os.access(repo / "d", os.R_OK):
            pytest.skip("this user reads a mode-0 directory")
        for read in (rc.launch, lambda: rc.prepare("HEAD", "HEAD")):
            with pytest.raises(rc.Refused, match="could not read the whole working tree"):
                read()
    finally:
        (repo / "d").chmod(0o755)


def test_the_owner_execute_bit_is_part_of_the_fingerprint(tmp_path, monkeypatch):
    rc = _rc_module()
    repo, _git = _tmp_repo(tmp_path)
    monkeypatch.chdir(repo)
    (repo / "run.sh").write_text("echo hi\n")
    (repo / "run.sh").chmod(0o755)
    first = rc.snapshot()
    (repo / "run.sh").chmod(0o655)                            # group and others may still execute it
    assert rc.snapshot() != first


def test_a_stage_only_change_in_a_conflict_moves_the_launch_identity(tmp_path, monkeypatch):
    import subprocess
    rc = _rc_module()
    repo, git = _tmp_repo(tmp_path)
    monkeypatch.chdir(repo)
    (repo / "a.txt").write_text("base\n")
    git("add", "a.txt")
    git("commit", "-q", "-m", "base")
    git("checkout", "-q", "-b", "side")
    (repo / "a.txt").write_text("side\n")
    git("commit", "-q", "-am", "side")
    git("checkout", "-q", "-")
    (repo / "a.txt").write_text("main\n")
    git("commit", "-q", "-am", "main")
    subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@example.invalid", "merge",
                    "-q", "side"], capture_output=True, timeout=60)
    first = rc.launch()
    assert first[1].startswith("staged-"), first
    blob = subprocess.run(
        ["git", "-C", str(repo), "hash-object", "-w", "--stdin"], input="other\n", text=True, capture_output=True,
        check=True, timeout=30).stdout.strip()
    subprocess.run(["git", "-C", str(repo), "update-index", "--index-info"], input=f"100644 {blob} 3\ta.txt\n",
                   text=True, check=True, timeout=30)
    second = rc.launch()
    assert second[0] == first[0] and second[2] == first[2] and second[1] != first[1], (first, second)


def test_a_dirty_tracked_candidate_against_head_takes_one_name_diff(tmp_path, monkeypatch):
    rc = _rc_module()
    repo, git = _tmp_repo(tmp_path)
    monkeypatch.chdir(repo)
    (repo / "k.c").write_text("int a;\n")
    git("add", "k.c")
    git("commit", "-q", "-m", "k")
    (repo / "k.c").write_text("int b;\n")
    calls = []
    real = rc._git
    monkeypatch.setattr(rc, "_git", lambda top, *a, **k: calls.append(a) or real(top, *a, **k))
    head = git("rev-parse", "HEAD")
    _digest, covered, _notes = rc.prepare(head, head)
    assert covered == [b"k.c"]
    assert sum(1 for a in calls if a[:2] == ("diff", "--name-only")) == 1, calls


def test_a_checkout_local_tmpdir_does_not_move_the_candidate(tmp_path):
    import os
    repo, _git = _tmp_repo(tmp_path)
    (repo / "t").mkdir()
    done, prompt = _dispatch_in(repo, tmp_path, "round", "HIGHEST: none\n", "HEAD", {"TMPDIR": str(repo / "t")})
    assert prompt and "the candidate changed" not in done.stderr, done.stderr        # the reviewer ran
    assert not os.listdir(repo / "t")


def test_a_brokered_pass_with_a_checkout_local_tmpdir_runs(tmp_path):
    import json
    import os
    import subprocess
    import time
    repo, _git = _tmp_repo(tmp_path)
    (repo / "t").mkdir()
    fake = tmp_path / "companion.js"
    fake.write_text("process.stdout.write('# Codex Adversarial Review\\n\\nHIGHEST: none\\n');\n")
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_OUTAGE_OVERRIDE": "1", "REVIEW_BASE": "HEAD",
           "TMPDIR": str(repo / "t")}
    done = subprocess.run(["bash", str(REPO / "scripts/overnight/review-broker-codex-dispatch.sh"),
                           "[review-kind: round] todo/x.md section 9\nReview it."],
                          capture_output=True, text=True, timeout=60, cwd=str(repo), env=env)
    assert done.returncode == 0, done.stderr
    log = pathlib.Path(json.loads(done.stdout.strip().splitlines()[-1])["logFile"])
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline and not pathlib.Path(f"{log}.done").exists():
        time.sleep(0.1)
    assert "changed between the broker's launch" not in log.read_text(), log.read_text()


def test_the_broker_read_takes_from_the_callers_limit(tmp_path):
    import os
    import subprocess
    import time
    repo, _git = _tmp_repo(tmp_path)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    real = subprocess.run(["sh", "-c", "command -v git"], capture_output=True, text=True, timeout=10).stdout.strip()
    (bindir / "git").write_text(f'#!/bin/sh\ncase " $* " in *" status "*) sleep 30 ;; esac\nexec "{real}" "$@"\n')
    (bindir / "git").chmod(0o755)
    env = {**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}", "CODEX_COMPANION_PATH": str(tmp_path / "c.js"),
           "CODEX_OUTAGE_OVERRIDE": "1", "REVIEW_BASE": "HEAD", "CODEX_REVIEW_MAX_SECS": "3"}
    (tmp_path / "c.js").write_text("\n")
    began = time.monotonic()
    done = subprocess.run(["bash", str(REPO / "scripts/overnight/review-broker-codex-dispatch.sh"),
                           "[review-kind: round] todo/x.md section 9\nReview it."],
                          capture_output=True, text=True, timeout=60, cwd=str(repo), env=env)
    assert done.returncode == 2 and time.monotonic() - began < 3 + 1.2, done.stderr
    assert "took the whole CODEX_REVIEW_MAX_SECS" in done.stderr, done.stderr


def test_the_manifest_is_read_from_its_end_without_rescanning_a_long_record(tmp_path):
    import time
    m = tmp_path / "manifest.jsonl"
    long = "x" * (1 << 18)
    m.write_text("first\n" + long + "\nnext\n\nlast")
    assert list(rp._newest_first(m, block=7)) == ["last", "next", long, "first"]
    read = []

    class Counted:
        def __init__(self, fh):
            self.fh = fh

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            self.fh.close()

        def seek(self, *a):
            return self.fh.seek(*a)

        def read(self, n):
            data = self.fh.read(n)
            read.append(len(data))
            return Joined(data)

    added = []

    class Joined(bytes):
        """A chunk that counts the bytes an accumulator copies by concatenating onto it."""
        def __add__(self, other):
            added.append(len(self) + len(other))
            return bytes(self) + bytes(other)

        def __radd__(self, other):
            added.append(len(self) + len(other))
            return bytes(other) + bytes(self)

    import os
    real_fdopen = os.fdopen
    os.fdopen = lambda *a, **k: Counted(real_fdopen(*a, **k))   # the reader opens a descriptor it checked
    try:
        assert list(rp._newest_first(m, block=64))[2] == long
    finally:
        os.fdopen = real_fdopen
    assert sum(read) == m.stat().st_size                    # every byte read once: nothing rescanned
    assert sum(added) <= m.stat().st_size                   # and never copied onto a growing record


def _broker(repo, env, timeout=60):
    import subprocess
    return subprocess.run(["bash", str(REPO / "scripts/overnight/review-broker-codex-dispatch.sh"),
                           "[review-kind: round] todo/x.md section 9\nReview it."],
                          capture_output=True, text=True, timeout=timeout, cwd=str(repo), env=env)


def _done_status(stdout, wait=60):
    import json
    import time
    log = pathlib.Path(json.loads(stdout.strip().splitlines()[-1])["logFile"])
    deadline = time.monotonic() + wait
    while time.monotonic() < deadline and not pathlib.Path(f"{log}.done").exists():
        time.sleep(0.1)
    return pathlib.Path(f"{log}.done").read_text().strip(), log.read_text()


def test_a_slow_untracked_read_is_held_to_the_callers_limit(tmp_path):
    """Hashing a large untracked file stands in for any slow part of the read: one limit bounds all of it."""
    import os
    import time
    repo, _git = _tmp_repo(tmp_path)
    (repo / "big.bin").write_bytes(b"x" * 4096)
    site = tmp_path / "site"
    site.mkdir()
    (site / "sitecustomize.py").write_text(
        "import os, time\n_read = os.read\n"
        "def read(fd, n):\n"
        "    try:\n        name = os.readlink(f'/proc/self/fd/{fd}')\n    except OSError:\n        name = ''\n"
        "    if name.endswith('/big.bin'):\n        time.sleep(30)\n    return _read(fd, n)\n"
        "os.read = read\n")
    env = {**os.environ, "PYTHONPATH": str(site), "CODEX_COMPANION_PATH": str(tmp_path / "c.js"),
           "CODEX_OUTAGE_OVERRIDE": "1", "REVIEW_BASE": "HEAD", "CODEX_REVIEW_MAX_SECS": "3"}
    (tmp_path / "c.js").write_text("\n")
    began = time.monotonic()
    done = _broker(repo, env)
    assert done.returncode == 2 and time.monotonic() - began < 3 + 1.2, done.stderr
    assert "took the whole CODEX_REVIEW_MAX_SECS" in done.stderr, done.stderr


def test_a_detached_job_started_after_the_callers_deadline_runs_nothing(tmp_path):
    import os
    repo, _git = _tmp_repo(tmp_path)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    # The service manager stalls past the deadline, and would stall again if asked to stop the unit.
    (bindir / "systemd-run").write_text("#!/bin/sh\nsleep 4\nexit 1\n")
    (bindir / "systemctl").write_text("#!/bin/sh\nsleep 10\n")
    for tool in ("systemd-run", "systemctl"):
        (bindir / tool).chmod(0o755)
    ran = tmp_path / "ran"
    fake = tmp_path / "companion.js"
    fake.write_text(f"require('fs').writeFileSync({str(ran)!r}, 'x');\n"
                    "process.stdout.write('# Codex Adversarial Review\\n\\nHIGHEST: none\\n');\n")
    env = {**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}", "CODEX_COMPANION_PATH": str(fake),
           "CODEX_OUTAGE_OVERRIDE": "1", "REVIEW_BASE": "HEAD", "CODEX_REVIEW_MAX_SECS": "3"}
    import time
    began = time.monotonic()
    done = _broker(repo, env)
    took = time.monotonic() - began
    assert done.returncode == 0 and took < 3.8, (took, done.stderr)    # the submission is held to the limit too
    status, text = _done_status(done.stdout)
    assert status == "124" and not ran.exists(), text


def test_a_short_limit_still_launches(tmp_path):
    import os
    repo, _git = _tmp_repo(tmp_path)
    fake = tmp_path / "companion.js"
    started = tmp_path / "started"
    review = "# Codex Adversarial Review\n\n" + "".join(f"LENS {n}: clean\n" for n in _pin()["lenses"]["round"]) \
        + "HIGHEST: none\n"
    fake.write_text(f"require('fs').writeFileSync({str(started)!r}, 'x');\n"
                    f"process.stdout.write({review!r});\n")
    # A budget a whole pass fits in on a loaded host, far short of a review's real hour: the steps before the
    # reviewer and the job itself all spend it, so a poll that cost seconds would show here.
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_OUTAGE_OVERRIDE": "1", "REVIEW_BASE": "HEAD",
           "CODEX_REVIEW_MAX_SECS": "8"}
    done = _broker(repo, env)
    assert done.returncode == 0 and "nothing was launched" not in done.stderr, done.stderr
    status, text = _done_status(done.stdout)
    assert status == "0" and started.exists(), text


def test_a_nested_checkout_is_recorded_as_a_directory(tmp_path, monkeypatch):
    import subprocess
    rc = _rc_module()
    repo, _git = _tmp_repo(tmp_path)
    monkeypatch.chdir(repo)
    subprocess.run(["git", "init", "-q", str(repo / "inner")], check=True, capture_output=True, timeout=30)
    (repo / "inner/f.c").write_text("int x;\n")
    head, _index, digest = rc.launch()
    _d, covered, notes = rc.prepare(head, head)
    assert covered == [b"inner/"] and "directory" in notes[0][1], (covered, notes)


def test_refusal_a_status_that_writes_only_a_newline_to_stderr(tmp_path, monkeypatch):
    rc = _rc_module()
    repo, _git = _tmp_repo(tmp_path)
    monkeypatch.chdir(repo)
    real = rc._run
    monkeypatch.setattr(rc, "_run", lambda top, args, sink=None: (0, b"", b"\n") if args[0] == "status"
                        else real(top, args, sink))
    with pytest.raises(rc.Refused, match="could not read the whole working tree"):
        rc.snapshot()


def test_the_prompt_shows_the_index_and_the_working_files_apart(tmp_path):
    import subprocess
    repo, git = _tmp_repo(tmp_path)
    (repo / "a.txt").write_text("1\n")
    git("add", "a.txt")
    git("commit", "-q", "-m", "a")
    (repo / "a.txt").write_text("2\n")
    git("add", "a.txt")
    (repo / "a.txt").write_text("1\n")                     # the staged change, undone in the working file
    assert subprocess.run(["git", "-C", str(repo), "diff", "HEAD", "--quiet"], timeout=30).returncode == 0
    _done, prompt = _dispatch_in(repo, tmp_path, "round", "HIGHEST: none\n", "HEAD")
    assert "git diff --cached" in prompt and "(git diff)" in prompt, prompt


def test_a_slow_outage_check_is_held_to_the_callers_limit(tmp_path):
    import os
    import shutil
    import time
    repo, _git = _tmp_repo(tmp_path)
    # The outage check itself stalls (its storage does not answer): the python3 it runs under sleeps.
    bindir = tmp_path / "bin"
    bindir.mkdir()
    real = shutil.which("python3")
    (bindir / "python3").write_text('#!/bin/bash\ncase "$1" in */codex-outage-check.py) exec sleep 30;; esac\n'
                                    f'exec {real} "$@"\n')
    (bindir / "python3").chmod(0o755)
    env = {**os.environ, "CODEX_COMPANION_PATH": str(tmp_path / "c.js"), "REVIEW_BASE": "HEAD",
           "CODEX_REVIEW_MAX_SECS": "2", "PATH": f"{bindir}:{os.environ['PATH']}"}
    env.pop("CODEX_OUTAGE_OVERRIDE", None)
    (tmp_path / "c.js").write_text("\n")
    began = time.monotonic()
    done = _broker(repo, env, timeout=20)
    assert done.returncode == 2 and time.monotonic() - began < 2 + 1.2, done.stderr
    assert "outage check took the whole CODEX_REVIEW_MAX_SECS" in done.stderr, done.stderr


def test_a_chmod_while_an_untracked_file_is_read_refuses(tmp_path, monkeypatch):
    import os
    rc = _rc_module()
    repo, _git = _tmp_repo(tmp_path)
    monkeypatch.chdir(repo)
    script = repo / "u.sh"
    script.write_text("echo hi\n")
    real_read = os.read

    def read(fd, n):
        try:
            name = os.readlink(f"/proc/self/fd/{fd}")
        except OSError:
            name = ""
        if name.endswith("/u.sh"):
            os.chmod(script, 0o755)                    # only the mode moves: size, mtime and inode stay
        return real_read(fd, n)
    monkeypatch.setattr(rc.os, "read", read)
    with pytest.raises(rc.Refused, match="changed while it was read"):
        rc.snapshot()


def test_registration_is_held_to_the_callers_limit(tmp_path):
    import os
    import time
    repo, _git = _tmp_repo(tmp_path)
    reviews = repo / ".claude/overnight/reviews"
    reviews.mkdir(parents=True)
    fifo = reviews / "manifest.jsonl"
    os.mkfifo(fifo)                                    # opening it to append waits for a reader that never comes
    env = {**os.environ, "CODEX_COMPANION_PATH": str(tmp_path / "c.js"), "REVIEW_BASE": "HEAD",
           "CODEX_OUTAGE_OVERRIDE": "1", "CODEX_REVIEW_MAX_SECS": "2"}
    (tmp_path / "c.js").write_text("\n")
    began = time.monotonic()
    try:
        done = _broker(repo, env, timeout=20)
    finally:
        try:                                           # release a writer the broker left behind
            os.close(os.open(fifo, os.O_RDONLY | os.O_NONBLOCK))
        except OSError:                                # fail-direction: no writer is left to release
            pass
    assert done.returncode == 2 and time.monotonic() - began < 2 + 1.2, done.stderr
    assert "registering the dispatch took the whole CODEX_REVIEW_MAX_SECS" in done.stderr, done.stderr
    assert not list(reviews.glob(".prompt.*"))
    # The broker owned the result before the append (it says `owns` first), so the attempt it never
    # registered still has its log and a published status: the limit's 124.
    logs = [p for p in reviews.glob("*.out")]
    assert len(logs) == 1 and pathlib.Path(f"{logs[0]}.done").read_text().strip() == "124", logs


def test_a_reset_during_the_status_walk_refuses_the_launch(tmp_path, monkeypatch):
    """HEAD is read before the walk: a soft reset meanwhile would pair a clean status with another commit."""
    import subprocess
    rc = _rc_module()
    repo, git = _tmp_repo(tmp_path)
    (repo / "a.txt").write_text("2\n")
    git("add", "a.txt")
    git("commit", "-q", "-m", "second")
    monkeypatch.chdir(repo)
    real = rc._status

    def status(top, scope):
        seen = real(top, scope)
        subprocess.run(["git", "-C", str(repo), "reset", "-q", "--soft", "HEAD~1"], check=True, timeout=30)
        return seen
    monkeypatch.setattr(rc, "_status", status)
    with pytest.raises(rc.Refused, match="the candidate changed while it was read"):
        rc.launch()


def _late_unit_broker(tmp_path, start_at_once):
    """A broker run whose service manager takes the unit and then stalls past the deadline; with
    start_at_once the unit starts at once (inside the grace), otherwise the test starts it afterwards. A real
    unit runs outside the broker's process tree, so the test starts it, never the fake service manager."""
    import os
    import subprocess
    import threading
    repo, _git = _tmp_repo(tmp_path)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    job = tmp_path / "job.sh"
    start = ""
    if start_at_once:
        def unit():
            import time
            end = time.monotonic() + 20
            while time.monotonic() < end and not (job.exists() and job.read_text()):
                time.sleep(0.01)
            subprocess.run(["bash", str(job)], cwd=repo, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=60, check=False)
        threading.Thread(target=unit, daemon=True).start()
    (bindir / "systemd-run").write_text(f'#!/bin/bash\nprintf "%s" "${{@: -1}}" > "{job}"\n{start}sleep 10\n')
    (bindir / "systemctl").write_text("#!/bin/sh\nsleep 10\n")
    for tool in ("systemd-run", "systemctl"):
        (bindir / tool).chmod(0o755)
    ran = tmp_path / "ran"
    fake = tmp_path / "companion.js"
    fake.write_text(f"require('fs').appendFileSync({str(ran)!r}, 'x');\n"
                    "process.stdout.write('# Codex Adversarial Review\\n\\nHIGHEST: none\\n');\n")
    env = {**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}", "CODEX_COMPANION_PATH": str(fake),
           "CODEX_OUTAGE_OVERRIDE": "1", "REVIEW_BASE": "HEAD", "CODEX_REVIEW_MAX_SECS": "3"}
    done = _broker(repo, env)
    return done, job, ran


def test_a_unit_that_starts_after_the_timeout_is_published_runs_nothing(tmp_path):
    import subprocess
    done, job, ran = _late_unit_broker(tmp_path, start_at_once=False)
    assert done.returncode == 0, done.stderr
    status, text = _done_status(done.stdout)
    assert status == "124", text
    subprocess.run(["bash", str(job)], timeout=30, check=False)         # the unit the service manager took
    status2, text2 = _done_status(done.stdout)
    assert (status2, text2) == (status, text) and not ran.exists(), text2


def test_a_unit_already_running_keeps_its_result_when_the_submission_times_out(tmp_path):
    done, _job, ran = _late_unit_broker(tmp_path, start_at_once=True)
    assert done.returncode == 0, done.stderr
    status, text = _done_status(done.stdout)
    # The job's own status (the fake review names no lenses, so the pass reports it incomplete), never 124.
    assert status == "3" and text.count("Turn completed") == 1 and ran.read_text() == "x", text


def test_a_registration_cut_short_after_its_row_ends_the_attempt(tmp_path):
    import json
    import os
    repo, _git = _tmp_repo(tmp_path)
    site = tmp_path / "site"
    site.mkdir()
    # The registrar writes its row, then stalls before it exits.
    (site / "sitecustomize.py").write_text("import atexit, sys, time\n"
                                           "if sys.argv[1:2] == ['--append']:\n"
                                           "    atexit.register(time.sleep, 30)\n")
    env = {**os.environ, "PYTHONPATH": str(site), "CODEX_COMPANION_PATH": str(tmp_path / "c.js"),
           "CODEX_OUTAGE_OVERRIDE": "1", "REVIEW_BASE": "HEAD", "CODEX_REVIEW_MAX_SECS": "3"}
    (tmp_path / "c.js").write_text("\n")
    done = _broker(repo, env)
    assert done.returncode == 2 and "registering the dispatch took" in done.stderr, done.stderr
    rows = [json.loads(x) for x in (repo / ".claude/overnight/reviews/manifest.jsonl").read_text().splitlines()]
    log = pathlib.Path(rows[-1]["logFile"])
    assert pathlib.Path(f"{log}.done").read_text().strip() == "124", log.read_text()


def test_a_replacement_ref_does_not_change_what_the_candidate_names(tmp_path, monkeypatch):
    import subprocess
    rc = _rc_module()
    repo, git = _tmp_repo(tmp_path)
    (repo / "a.txt").write_text("1\n")
    git("add", "a.txt")
    git("commit", "-q", "-m", "one")
    first = git("rev-parse", "HEAD")
    (repo / "a.txt").write_text("2\n")
    git("add", "a.txt")
    git("commit", "-q", "-m", "two")
    monkeypatch.chdir(repo)
    clean = rc.launch()
    subprocess.run(["git", "-C", str(repo), "replace", git("rev-parse", "HEAD"), first], check=True, timeout=30)
    assert rc.launch() == clean
    head = git("rev-parse", "HEAD")
    assert rc.prepare(head, head)[1] == []


def test_a_graft_file_refuses_the_candidate(tmp_path, monkeypatch):
    rc = _rc_module()
    repo, git = _tmp_repo(tmp_path)
    monkeypatch.chdir(repo)
    (repo / ".git/info").mkdir(exist_ok=True)
    (repo / ".git/info/grafts").write_text(git("rev-parse", "HEAD") + "\n")
    with pytest.raises(rc.Refused, match="graft file"):
        rc.launch()
    (repo / ".git/info/grafts").unlink()
    inherited = tmp_path / "grafts"
    inherited.write_text(git("rev-parse", "HEAD") + "\n")
    monkeypatch.setenv("GIT_GRAFT_FILE", str(inherited))
    with pytest.raises(rc.Refused, match="graft file"):
        rc.launch()


def test_every_git_read_of_a_pass_uses_stored_objects(tmp_path):
    import os
    repo, _git = _tmp_repo(tmp_path)
    seen = tmp_path / "env.txt"
    fake = tmp_path / "env.js"
    fake.write_text(f"require('fs').writeFileSync({str(seen)!r}, String(process.env.GIT_NO_REPLACE_OBJECTS));\n"
                    "process.stdout.write('# Codex Adversarial Review\\n\\nHIGHEST: none\\n');\n")
    pin = _pin()
    review = "\n".join(f"LENS {n}: clean" for n in pin["lenses"]["round"]) + "\nHIGHEST: none\n"
    _dispatch_in(repo, tmp_path, "round", review, "HEAD", extra_env={"CODEX_COMPANION_PATH": str(fake)})
    assert seen.read_text() == "1"


def test_a_manifest_that_is_not_a_regular_file_refuses_at_once(tmp_path):
    import os
    import time
    fifo = tmp_path / "manifest.jsonl"
    os.mkfifo(fifo)                                    # opening it to read would wait for a writer
    for call in ("section_start", "repair_base"):
        done = _in_child(f"try:\n    rp.{call}({str(fifo)!r}, 'todo/x.md', 9)\n"
                         f"except rp.Refused as exc:\n    print('refused', exc)", timeout=5)
        assert done.returncode == 0 and "refused" in done.stdout and "not a regular file" in done.stdout, done


def _published_receipt(tmp_path, n, head, published):
    log = tmp_path / f"pass{n}.out"
    log.write_text("x")
    if published is not None:
        pathlib.Path(f"{log}.done").write_text(published)
    return {"todo": "todo/x.md", "section": "9", "kind": "plan", "route": "codex-dispatch.sh", "pass_checked": True,
            "rc": 0, "head": head, "done_file": True, "logFile": str(log)}


def test_a_pass_whose_success_was_never_published_sets_no_base(tmp_path):
    import json
    older, newer = _published_receipt(tmp_path, 1, "a" * 40, "0"), _published_receipt(tmp_path, 2, "b" * 40, None)
    manifest = tmp_path / "manifest.jsonl"
    manifest.write_text(json.dumps(older) + "\n" + json.dumps(newer) + "\n")
    assert rp.section_start(manifest, "todo/x.md", 9) == "a" * 40


def test_the_gate_and_the_bases_select_the_same_receipt(tmp_path):
    """One manifest read by one reader for both questions, a newer pass of each kind still running: the gate
    judges the running pass (and credits none), a base is read from the checked pass before it."""
    import importlib.util
    import json
    spec = importlib.util.spec_from_file_location("scg_select", REPO / ".claude/hooks/section_commit_gate.py")
    gate = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gate)
    root = tmp_path / "root"
    reviews = root / ".claude/overnight/reviews"
    reviews.mkdir(parents=True)
    rows = []
    for n, kind in enumerate(("plan", "completion")):
        done = {**_published_receipt(tmp_path, 2 * n, "a" * 40, "0"), "kind": kind,
                "worktree_sha256": rp.CLEAN_WORKTREE}
        running = {**_published_receipt(tmp_path, 2 * n + 1, "b" * 40, None), "kind": kind}
        rows += [json.dumps(done), json.dumps(running)]
    (reviews / "manifest.jsonl").write_text("\n".join(rows) + "\n")
    found, readable = gate._pass_receipts(root, "todo/x.md", "9")
    assert readable and {k: e["head"] for k, (e, _age) in found.items()} == {"plan": "b" * 40, "completion": "b" * 40}
    assert not any(gate._receipt_complete(e) for e, _age in found.values())
    checked, _ = rp.newest_receipts(reviews / "manifest.jsonl", "todo/x.md", "9", gate._PASS_KINDS, checked=True)
    assert {k: e["head"] for k, (e, _age) in checked.items()} == {"plan": "a" * 40, "completion": "a" * 40}
    assert rp.section_start(reviews / "manifest.jsonl", "todo/x.md", 9) == "a" * 40
    assert rp.repair_base(reviews / "manifest.jsonl", "todo/x.md", 9) == "a" * 40
    real = rp.newest_receipts                          # the gate asks through the shared reader, never its own
    asked = []
    rp.newest_receipts = lambda *a, **k: (asked.append(a), real(*a, **k))[1]
    try:
        gate._pass_receipts(root, "todo/x.md", "9")
    finally:
        rp.newest_receipts = real
    assert asked, "the gate read the manifest with a reader of its own"


def test_the_commit_gate_and_the_range_reads_credit_the_same_receipts(tmp_path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("scg_shared", REPO / ".claude/hooks/section_commit_gate.py")
    gate = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gate)
    base = _published_receipt(tmp_path, 1, "c" * 40, "0")
    cases = [base, {**base, "route": "other"}, {**base, "rc": 1}, {**base, "pass_checked": False},
             {**base, "head": "abc"}, {**base, "done_file": False}, _published_receipt(tmp_path, 2, "d" * 40, "124"),
             _published_receipt(tmp_path, 3, "e" * 40, None)]
    assert [gate._receipt_complete(c) for c in cases] == [rp.receipt_complete(c) for c in cases]
    assert [rp.receipt_complete(c) for c in cases] == [True, False, False, False, False, False, False, False]


def test_making_the_answer_is_held_to_the_callers_limit(tmp_path):
    import os
    import time
    repo, _git = _tmp_repo(tmp_path)
    site = tmp_path / "site"
    site.mkdir()
    # The answer's writer stalls before it exits; the registration does not.
    (site / "sitecustomize.py").write_text("import atexit, sys, time\n"
                                           "if sys.argv[1:2] and sys.argv[1].startswith('kind='):\n"
                                           "    atexit.register(time.sleep, 30)\n")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    (bindir / "systemd-run").write_text("#!/bin/sh\nexit 0\n")
    (bindir / "systemd-run").chmod(0o755)
    env = {**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}", "PYTHONPATH": str(site),
           "CODEX_COMPANION_PATH": str(tmp_path / "c.js"), "CODEX_OUTAGE_OVERRIDE": "1", "REVIEW_BASE": "HEAD",
           "CODEX_REVIEW_MAX_SECS": "3"}
    (tmp_path / "c.js").write_text("\n")
    began = time.monotonic()
    done = _broker(repo, env, timeout=20)
    assert done.returncode == 2 and time.monotonic() - began < 3 + 1.2, done.stderr
    assert "making the answer took the whole CODEX_REVIEW_MAX_SECS" in done.stderr, done.stderr


def test_a_brokered_pass_counts_only_once_its_broker_published_success(tmp_path):
    inner = _published_receipt(tmp_path, 1, "a" * 40, "0")
    receipt = {**inner, "parent": "detached:codex-rev-round-20261122-065522-123"}
    broker = tmp_path / "20261122-065522-123-round.out"            # where that unit logs, beside the pass
    assert not rp.receipt_complete(receipt)                         # the broker has published nothing yet
    pathlib.Path(f"{broker}.done").write_text("124\n")
    assert not rp.receipt_complete(receipt)                         # the broker's limit ended the attempt
    pathlib.Path(f"{broker}.done").write_text("0\n")
    assert rp.receipt_complete(receipt)
    named = tmp_path / "elsewhere.out"
    receipt = {**receipt, "parent_log": str(named)}                 # recorded by the dispatcher: it decides
    assert not rp.receipt_complete(receipt)
    pathlib.Path(f"{named}.done").write_text("0\n")
    assert rp.receipt_complete(receipt)


def _in_child(code, timeout=10):
    """Runs `code` against review_pin in a child the test can time out: a lookup that blocks never stalls
    the suite, it fails it."""
    import subprocess
    import sys
    return subprocess.run([sys.executable, "-c", f"import sys; sys.path.insert(0, {str(REPO / 'scripts')!r}); "
                           f"import review_pin as rp\n{code}"], capture_output=True, text=True, timeout=timeout)


def test_a_completion_marker_that_is_not_a_regular_file_is_never_waited_on(tmp_path):
    import json
    import os
    r = _published_receipt(tmp_path, 1, "a" * 40, None)
    os.mkfifo(r["logFile"] + ".done")                               # reading it would wait for a writer
    manifest = tmp_path / "manifest.jsonl"
    manifest.write_text(json.dumps(r) + "\n")
    done = _in_child(f"print(repr(rp.section_start({str(manifest)!r}, 'todo/x.md', 9)))", timeout=5)
    assert done.returncode == 0 and done.stdout.strip() == "''", done


def test_writing_the_prompt_is_held_to_the_callers_limit(tmp_path):
    import os
    import time
    repo, _git = _tmp_repo(tmp_path)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    (bindir / "sha256sum").write_text("#!/bin/sh\nexec sleep 30\n")   # the prompt's hash stalls
    (bindir / "sha256sum").chmod(0o755)
    env = {**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}", "CODEX_COMPANION_PATH": str(tmp_path / "c.js"),
           "CODEX_OUTAGE_OVERRIDE": "1", "REVIEW_BASE": "HEAD", "CODEX_REVIEW_MAX_SECS": "3"}
    (tmp_path / "c.js").write_text("\n")
    began = time.monotonic()
    done = _broker(repo, env, timeout=20)
    assert done.returncode == 2 and time.monotonic() - began < 3 + 1.2, done.stderr
    assert "writing the prompt took the whole CODEX_REVIEW_MAX_SECS" in done.stderr, done.stderr
    assert not list((repo / ".claude/overnight/reviews").glob(".prompt.*"))   # nothing of the attempt is left


def _stalling_broker(tmp_path, stall_kind_arg, submit="exec sleep 30"):
    """A broker whose answer step (manifest-line.py with kind=) or submission stalls, in its own session."""
    import os
    import subprocess
    repo, _git = _tmp_repo(tmp_path)
    site = tmp_path / "site"
    site.mkdir()
    (site / "sitecustomize.py").write_text("import sys, time\n"
                                           f"if {stall_kind_arg!r} and sys.argv[1:2] and sys.argv[1].startswith('kind='):\n"
                                           "    time.sleep(30)\n")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    (bindir / "systemd-run").write_text(f"#!/bin/sh\n{submit}\n")
    (bindir / "systemd-run").chmod(0o755)
    env = {**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}", "PYTHONPATH": str(site),
           "CODEX_COMPANION_PATH": str(tmp_path / "c.js"), "CODEX_OUTAGE_OVERRIDE": "1", "REVIEW_BASE": "HEAD",
           "CODEX_REVIEW_MAX_SECS": "60"}
    (tmp_path / "c.js").write_text("\n")
    proc = subprocess.Popen(["bash", str(REPO / "scripts/overnight/review-broker-codex-dispatch.sh"),
                             "[review-kind: round] todo/x.md section 9\nReview it."], cwd=str(repo), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
    return repo, proc


def _registered_log(repo, wait=20):
    import json
    import time
    manifest = repo / ".claude/overnight/reviews/manifest.jsonl"
    end = time.monotonic() + wait
    while time.monotonic() < end:
        if manifest.exists() and manifest.read_text().strip():
            return pathlib.Path(json.loads(manifest.read_text().strip().splitlines()[-1])["logFile"])
        time.sleep(0.05)
    raise AssertionError("no registration")


def test_cancelling_the_broker_after_registration_publishes_the_result(tmp_path):
    import os
    import signal
    repo, proc = _stalling_broker(tmp_path, True)
    try:
        log = _registered_log(repo)
        os.killpg(proc.pid, signal.SIGTERM)
        proc.wait(timeout=20)
        assert pathlib.Path(f"{log}.done").read_text().strip() in ("143",), proc.stderr.read()
        assert not list((repo / ".claude/overnight/reviews").glob(".prompt.*"))
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()


def test_a_cancelled_submission_starts_no_fallback(tmp_path):
    import os
    import signal
    import time
    # The service manager is ended by a signal mid-submission; the broker itself is not signalled.
    repo, proc = _stalling_broker(tmp_path, False, submit="kill -TERM $$")
    try:
        log = _registered_log(repo)
        proc.wait(timeout=20)
        assert proc.returncode != 0
        assert pathlib.Path(f"{log}.done").read_text().strip() == "143"
        assert "Turn completed (rc=143)" in log.read_text()        # the broker's, not a review's
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()


def _result_module():
    import importlib.util
    spec = importlib.util.spec_from_file_location("review_result", REPO / "scripts/overnight/review_result.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_a_result_whose_publication_fails_is_reported_as_failed(tmp_path, capsys):
    rr = _result_module()
    log, prompt = tmp_path / "x.out", tmp_path / ".prompt.x"
    log.write_text("")
    prompt.write_text("p")
    (tmp_path / "x.out.done").mkdir()                     # the rename over it fails
    assert rr.publish(str(log), "0", str(prompt)) == 1
    assert "could not be published" in capsys.readouterr().err


def test_a_claimed_result_is_left_to_its_owner(tmp_path):
    rr = _result_module()
    log = tmp_path / "x.out"
    assert rr.claim(str(log)) == 0 and rr.claim(str(log)) == 4
    assert rr.publish(str(log), "124", claim_first=True) == 0
    assert not (tmp_path / "x.out.done").exists() and log.read_text() == ""


def test_a_signal_to_the_broker_alone_ends_a_stalled_submission_at_once(tmp_path):
    import os
    import signal
    marker = tmp_path / "submitting"
    repo, proc = _stalling_broker(tmp_path, False, submit=f"touch {marker}; exec sleep 30")
    try:
        log = _registered_log(repo)
        import time
        end = time.monotonic() + 20
        while time.monotonic() < end and not marker.exists():
            time.sleep(0.02)
        os.kill(proc.pid, signal.SIGTERM)                 # the broker's own process id, not its group
        proc.wait(timeout=5)
        assert proc.returncode == 143, proc.stderr.read()
        assert pathlib.Path(f"{log}.done").read_text().strip() == "143"
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()


def test_a_signal_while_the_prompt_is_written_leaves_nothing(tmp_path):
    import os
    import signal
    import subprocess
    import time
    repo, _git = _tmp_repo(tmp_path)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    marker = tmp_path / "hashing"
    (bindir / "sha256sum").write_text(f"#!/bin/sh\ntouch {marker}\nexec sleep 30\n")
    (bindir / "sha256sum").chmod(0o755)
    (tmp_path / "c.js").write_text("\n")
    env = {**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}", "CODEX_COMPANION_PATH": str(tmp_path / "c.js"),
           "CODEX_OUTAGE_OVERRIDE": "1", "REVIEW_BASE": "HEAD"}
    proc = subprocess.Popen(["bash", str(REPO / "scripts/overnight/review-broker-codex-dispatch.sh"),
                             "[review-kind: round] todo/x.md section 9\nReview it."], cwd=str(repo), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        end = time.monotonic() + 20
        while time.monotonic() < end and not marker.exists():
            time.sleep(0.02)
        os.kill(proc.pid, signal.SIGTERM)
        proc.wait(timeout=5)
        reviews = repo / ".claude/overnight/reviews"
        assert proc.returncode == 143, proc.stderr.read()
        assert not list(reviews.glob(".prompt.*")) and not (reviews / "manifest.jsonl").exists()
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()


def test_refusal_a_review_base_past_the_section_start(tmp_path):
    """A caller's REVIEW_BASE after the section's recorded start would leave its first commits unreviewed:
    the dispatcher refuses it; the start itself, or an earlier commit, is taken."""
    repo, git = _tmp_repo(tmp_path)
    start = git("rev-parse", "HEAD")
    git("commit", "-q", "--allow-empty", "-m", "build")
    build = git("rev-parse", "HEAD")
    git("commit", "-q", "--allow-empty", "-m", "fix")
    reviews = tmp_path / "reviews"
    reviews.mkdir()
    (reviews / "manifest.jsonl").write_text(_receipt("plan", start, todo="todo/x.md", section=9) + "\n")
    done, prompt = _dispatch_in(repo, tmp_path, "round", "HIGHEST: none\n", build)
    assert done.returncode == 2 and "past where this section started" in done.stderr, done.stderr
    assert prompt == ""
    done, _prompt = _dispatch_in(repo, tmp_path, "round", "HIGHEST: none\n", start)
    assert "past where this section started" not in done.stderr, done.stderr


def test_the_repair_base_stops_at_the_newest_checked_pass(tmp_path, monkeypatch):
    """The newest checked completion answers the repair base: the read ends there, never walking back over
    older history for a reconciliation that does not exist yet."""
    def lines(_manifest):
        yield _receipt("completion", "d" * 8, todo="todo/x.md", section=9)
        raise AssertionError("read past the receipt that decides the answer")
    monkeypatch.setattr(rp, "_newest_first", lines)
    assert rp.repair_base(tmp_path / "m.jsonl", "todo/x.md", 9) == "d" * 8


def test_a_torn_line_that_may_be_the_plan_refuses_the_resume(tmp_path):
    """No checked plan, and a torn line naming this TODO: the resume refuses (as section_start does), never
    "new", which would start the section again from HEAD."""
    todo = "todo/00-x/TODO-01-x.md"
    m = tmp_path / rp.MANIFEST
    m.parent.mkdir(parents=True)
    m.write_text(f'{{"ts": 1, "kind": "plan", "todo": "{todo}", "sec\n')
    with pytest.raises(rp.Refused):
        rp.section_status(tmp_path, todo, 7)
    with pytest.raises(rp.Refused):
        rp.section_start(m, todo, "7")


def test_the_streamed_final_response_matches_the_whole_reading(tmp_path):
    """read_final_response gives what final_response gives over the whole text: nothing without a heading,
    what follows the newest heading otherwise, line endings of every kind included."""
    cases = ["no heading\nHIGHEST: P1\n",
             f"progress\n{rp.REVIEW_HEADER}\nold\nHIGHEST: P1\n{rp.REVIEW_HEADER}\nnew\r\nHIGHEST: none\n",
             f"{rp.REVIEW_HEADER}\n\nVerdict: approve\rHIGHEST: P3",
             ""]
    for i, text in enumerate(cases):
        f = tmp_path / f"t{i}.out"
        f.write_text(text, newline="")
        assert rp.read_final_response(f) == rp.final_response(text), text


def test_a_large_transcript_is_judged_in_bounded_memory(tmp_path):
    """64 MiB of progress lines, then the review, under a 200 MiB address-space limit: --highest reads only
    what follows the heading, so the pass is judged instead of failing on a copy of the whole transcript."""
    import resource
    import subprocess
    import sys
    f = tmp_path / "t.out"
    line = b"[codex] working on the review, still reading files " + b"x" * 60 + b"\n"
    with open(f, "wb") as fh:
        for _ in range((64 << 20) // len(line)):
            fh.write(line)
        fh.write(f"{rp.REVIEW_HEADER}\nVerdict: needs-attention\nHIGHEST: P2\n".encode())

    def limit():
        resource.setrlimit(resource.RLIMIT_AS, (200 << 20, 200 << 20))
    r = subprocess.run([sys.executable, str(pathlib.Path(rp.__file__)), "--highest", "--transcript", str(f)],
                       capture_output=True, text=True, timeout=120, preexec_fn=limit)
    assert r.returncode == 0 and r.stdout.strip() == "P2", r


def test_a_held_claim_lock_gives_up_on_elapsed_time_a_suspend_included(tmp_path):
    """Another claimant holds the owner lock and creates no result, and the host sleeps past CLAIM_WAIT: the
    claim gives up at once on resume (its wait is measured on CLOCK_BOOTTIME), never a minute later."""
    import fcntl
    import os
    import time
    rr = _result_module()
    log = tmp_path / "x.out"
    fd = os.open(f"{log}.owner", os.O_RDWR | os.O_CREAT, 0o644)
    fcntl.flock(fd, fcntl.LOCK_EX)
    try:
        real, skew = rr._elapsed, [0.0]
        rr._elapsed = lambda: real() + skew[0]
        calls = [0]
        real_sleep = time.sleep

        def sleep(s):
            calls[0] += 1
            if calls[0] == 2:
                skew[0] = rr.CLAIM_WAIT + 1               # the host slept past the claim's bound
            real_sleep(s)
        rr.time.sleep = sleep
        began = time.monotonic()
        try:
            assert rr.claim(str(log)) == 1
        finally:
            rr.time.sleep = real_sleep
            rr._elapsed = real
        assert time.monotonic() - began < 5, time.monotonic() - began
    finally:
        os.close(fd)


def test_a_checked_pass_reads_its_transcript_once_for_its_verdicts_and_its_level(tmp_path, monkeypatch):
    """The lens verdicts and the highest level come from one read of the final response (one review_pin run
    with --transcript), and the receipt still records the level the review declared."""
    import json
    import os
    import shutil
    real = shutil.which("python3")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    calls = tmp_path / "calls"
    (bindir / "python3").write_text(
        "#!/bin/bash\n"
        f'case "$*" in *review_pin.py*--transcript*) echo x >> {str(calls)!r};; esac\n'
        f'exec {real} "$@"\n')
    (bindir / "python3").chmod(0o755)
    monkeypatch.setenv("PATH", f"{bindir}:{os.environ['PATH']}")
    pin = _pin()
    full = "findings...\n" + "\n".join(f"LENS {n}: clean" for n in pin["lenses"]["completion"]) + "\nHIGHEST: P3\n"
    done, _prompt = _dispatch(tmp_path, "completion", full)
    assert done.returncode == 0, done.stderr
    assert calls.read_text().count("x") == 1, calls.read_text()
    receipts = [json.loads(ln) for p in (tmp_path / "reviews").glob("**/*.jsonl")
                for ln in p.read_text().splitlines() if '"pass_checked"' in ln]
    assert any(r.get("highest") == "P3" for r in receipts), receipts


def test_a_manifest_that_shrinks_while_it_is_read_is_refused_never_read_short(tmp_path, monkeypatch):
    """The manifest is cut back to an older, clean record between the size read and the first read: the
    reader refuses it (read again), never yields the older record as the newest."""
    import io
    import os
    m = tmp_path / "manifest.jsonl"
    old = '{"kind": "round", "highest": "none"}\n'
    m.write_text(old + '{"kind": "round", "highest": "P2"}\n')
    real = os.fdopen

    class Shrinking(io.FileIO):
        def read(self, n=-1):
            os.truncate(m, len(old))                       # cut at a record boundary after the size was taken
            return super().read(n)

    def fdopen(fd, mode="r", *a, **k):
        return Shrinking(fd, "rb") if mode == "rb" else real(fd, mode, *a, **k)
    monkeypatch.setattr(os, "fdopen", fdopen)              # review_pin imports os inside the reader
    with pytest.raises(rp.Refused):
        list(rp._newest_first(m))


def test_a_stalled_candidate_read_refuses_the_pass_instead_of_holding_the_dispatcher(tmp_path, monkeypatch):
    """The candidate reader stalls (storage that stops answering mid-hash): the dispatcher refuses within
    CANDIDATE_READ_SECS and dispatches nothing."""
    import os
    import shutil
    import time
    real = shutil.which("python3")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    (bindir / "python3").write_text(
        "#!/bin/bash\n"
        'case "$*" in *review_candidate.py\\ prepare*) exec sleep 60;; esac\n'
        f'exec {real} "$@"\n')
    (bindir / "python3").chmod(0o755)
    monkeypatch.setenv("PATH", f"{bindir}:{os.environ['PATH']}")
    monkeypatch.setenv("CANDIDATE_READ_SECS", "1")
    began = time.monotonic()
    done, prompt = _dispatch(tmp_path, "completion", "x")
    assert done.returncode == 2 and "could not be read" in done.stderr, (done.returncode, done.stderr[-400:])
    assert prompt == "" and time.monotonic() - began < 20, time.monotonic() - began



def test_every_git_command_the_dispatcher_runs_is_bounded(tmp_path, monkeypatch):
    """git stalls on its first lookup (a slow ancestry walk, stalled storage): the dispatcher refuses within
    CANDIDATE_READ_SECS and dispatches nothing, as for a stalled candidate read."""
    import os
    import shutil
    import time
    real = shutil.which("git")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    (bindir / "git").write_text(
        "#!/bin/bash\n"
        'case "$*" in *"HEAD^{commit}"*) exec sleep 60;; esac\n'
        f'exec {real} "$@"\n')
    (bindir / "git").chmod(0o755)
    monkeypatch.setenv("PATH", f"{bindir}:{os.environ['PATH']}")
    monkeypatch.setenv("CANDIDATE_READ_SECS", "1")
    began = time.monotonic()
    done, prompt = _dispatch(tmp_path, "completion", "x")
    assert done.returncode == 2 and prompt == "", (done.returncode, done.stderr[-400:])
    assert time.monotonic() - began < 20, time.monotonic() - began



def test_a_transcript_cut_while_its_verdict_is_read_is_unreadable_never_an_older_verdict(tmp_path, monkeypatch):
    """The transcript holds two reviews, the newer at P2; it is cut back to the first while it is read: the
    verdict reader refuses it, never returning the earlier HIGHEST: none."""
    import os
    heading = "# Codex Adversarial Review\n"
    first = heading + "old\nHIGHEST: none\n"
    log = tmp_path / "r.out"
    log.write_text(first + "x" * (3 << 20) + "\n" + heading + "new\nHIGHEST: P2\n")
    assert rp.highest(rp.read_final_response(log)) == "P2"
    real, calls = os.read, [0]

    def read(fd, n):
        calls[0] += 1
        if calls[0] == 2:
            os.truncate(log, len(first))                   # cut after the first chunk was read
        return real(fd, n)
    monkeypatch.setattr(os, "read", read)
    with pytest.raises(OSError):
        rp.read_final_response(log)


def test_a_status_that_cannot_be_read_never_steps_back_to_an_older_receipt(tmp_path, monkeypatch):
    """The newer pass's published status cannot be read (EIO): its standing is unknown, so the status is
    refused, and the checked receipt lookup refuses rather than answer with the older pass."""
    import errno
    rr = rp._review_result()
    monkeypatch.setattr(rr, "read_status_file", lambda path: (None, OSError(errno.EIO, "Input/output error")))
    with pytest.raises(rp.Refused):
        rp.published_status(tmp_path / "r.out.done")
    receipt = {"route": "codex-dispatch.sh", "pass_checked": True, "rc": 0, "head": "a" * 40,
               "logFile": str(tmp_path / "r.out"), "done_file": True}
    with pytest.raises(rp.Refused):
        rp.receipt_complete(receipt)


# --- Reviewer effort by area tier and the chain's last finding (automation hardening: "Reviewer effort follows the area and the chain") ---

EFFORT_TODO = "todo/00-infrastructure/TODO-01-developer-tooling-stack.md"
NO_EFFORT = GOOD.split("\n[schedule.effort]\n")[0] + "\n"


def _er(kind, highest=None, schedule=None, checked=True, section=99):
    import json
    e = json.loads(_receipt(kind, "a" * 40, section=section, checked=checked, todo=EFFORT_TODO))
    if highest is not None:
        e["highest"] = highest
    if schedule is not None:
        e["schedule"] = schedule
    return json.dumps(e)


def _chain(tmp_path, *rows):
    """A manifest of `rows`, oldest first, after a marked plan unless the first row is a plan."""
    manifest = tmp_path / "manifest.jsonl"
    if not rows or not rows[0].startswith('{"ts": 1795170000, "kind": "plan"'):
        rows = (_er("plan", schedule="effort"),) + rows
    manifest.write_text("".join(r + "\n" for r in rows))
    return manifest


def _effort(tmp_path, kind, paths, *rows, pin=None):
    return rp.effort_for(pin or _pin(), _chain(tmp_path, *rows), EFFORT_TODO, 99, kind, paths)


def test_effort_table_validates():
    pin = _pin()
    assert pin["effort"]["held_rounds"] == 3 and pin["effort"]["passes"] == "medium"
    assert [t[0] for t in pin["effort"]["tiers"]] == ["deep", "risky", "standard"]
    for bad in (GOOD.replace('later = "low"', 'later = "lowest"', 1),
                GOOD.replace('name = "standard"\npaths = []', 'name = "standard"\npaths = ["docs/"]', 1),
                GOOD.replace("held_rounds = 3", "held_rounds = 3\ncolour = 1", 1),
                GOOD.replace("held_rounds = 3", "held_rounds = 0", 1)):
        assert bad != GOOD
        with pytest.raises(rp.Refused):
            _pin(bad)


def test_effort_absent_is_none(tmp_path):
    pin = _pin(NO_EFFORT)
    assert pin["effort"] is None
    assert _effort(tmp_path, "round", ["src/kernel/a.c"], pin=pin) is None


def test_tier_precedence():
    pin = _pin()
    assert rp.tier_for(pin, ["docs/x.md", "include/kernel/a.h"]) == "deep"
    assert rp.tier_for(pin, ["include/gfx.h", "docs/x.md"]) == "risky"
    assert rp.tier_for(pin, ["user/x.c"]) == "risky"
    assert rp.tier_for(pin, ["Makefile"]) == "risky"
    assert rp.tier_for(pin, ["Makefile.bak"]) == "standard"
    assert rp.tier_for(pin, ["docs/x.md"]) == "standard"
    assert rp.tier_for(pin, []) == "standard"


def test_round_ladder_deep(tmp_path):
    k = ["src/kernel/a.c"]
    assert _effort(tmp_path, "round", k) == "high"
    assert _effort(tmp_path, "round", k, _er("round", "P2")) == "high"
    assert _effort(tmp_path, "round", k, _er("round", "P2"), _er("round", "P2")) == "high"
    three = (_er("round", "P2"),) * 3
    assert _effort(tmp_path, "round", k, *three) == "medium"
    assert _effort(tmp_path, "round", k, *three, _er("round", "P1")) == "high"
    assert _effort(tmp_path, "round", k, *three, _er("round", "P1"), _er("round", "P2")) == "medium"


def test_round_ladder_standard(tmp_path):
    d = ["docs/x.md"]
    assert _effort(tmp_path, "round", d) == "medium"
    assert _effort(tmp_path, "round", d, *(_er("round", "P2"),) * 2) == "medium"
    assert _effort(tmp_path, "round", d, *(_er("round", "P2"),) * 2, _er("round", "P3")) == "low"
    assert _effort(tmp_path, "round", d, *(_er("round", "P2"),) * 3, _er("round", "P1")) == "medium"


def test_reconciliation_counts_as_round(tmp_path):
    rows = (_er("round", "P2"), _er("round", "P2"), _er("reconciliation", "P2"))
    assert _effort(tmp_path, "reconciliation", ["src/kernel/a.c"], *rows) == "medium"
    assert _effort(tmp_path, "reconciliation", ["docs/x.md"], *rows) == "low"


def test_other_kinds_take_passes_level(tmp_path):
    rows = (_er("round", "P1"), _er("performance", "P1"), _er("redesign", "P1"))
    for kind in ("plan", "performance", "completion", "redesign"):
        for paths in (["src/kernel/a.c"], ["include/gfx.h"], ["docs/x.md"]):
            assert _effort(tmp_path, kind, paths, *rows) == "medium", (kind, paths)
    # performance and redesign rows never moved the chain: one round so far, so the next deep round is high.
    assert rp.section_chain(_chain(tmp_path, *rows), EFFORT_TODO, 99)[1:] == (1, "P1")


def test_unchecked_rounds_do_not_count(tmp_path):
    rows = (_er("round", "P2"), _er("round", "P2"), _er("round", "P2", checked=False))
    assert _effort(tmp_path, "round", ["src/kernel/a.c"], *rows) == "high"


def test_grandfathered_section_gets_nothing_for_every_kind(tmp_path):
    rows = (_er("plan"), _er("round", "P2"), _er("plan", schedule="effort"))
    for kind in rp.SECTION_KINDS:
        assert _effort(tmp_path, kind, ["src/kernel/a.c"], *rows) is None, kind


def test_marked_section_is_on_schedule(tmp_path):
    assert _effort(tmp_path, "round", ["src/kernel/a.c"], _er("plan", schedule="effort")) == "high"


def test_owed_section_without_plan(tmp_path):
    manifest = tmp_path / "manifest.jsonl"
    manifest.write_text(_er("round", "P2", section=7) + "\n")         # another section's row only
    assert rp.effort_for(_pin(), manifest, EFFORT_TODO, 99, "round", ["src/kernel/a.c"]) == "high"
    assert rp.section_chain(manifest, EFFORT_TODO, 99) == (None, 0, None)


def test_unreadable_newer_line_refuses(tmp_path):
    manifest = _chain(tmp_path, _er("round", "P2"))
    with manifest.open("a") as fh:
        fh.write('{"ts": 1795170001, "kind": "round", "todo": "todo/00-infr\n')
    with pytest.raises(rp.Refused):
        rp.effort_for(_pin(), manifest, EFFORT_TODO, 99, "round", ["src/kernel/a.c"])


def test_cli_effort(tmp_path, capsys):
    manifest = _chain(tmp_path)
    paths = tmp_path / "paths"
    paths.write_bytes(b"src/kernel/a.c\0")
    args = ["--effort", "round", "--todo", EFFORT_TODO, "--section-n", "99", "--manifest", str(manifest),
            "--paths-file0", str(paths)]
    assert rp.main(args) == 0 and capsys.readouterr().out == "high deep -\n"
    plan = ["--effort", "plan"] + args[2:]
    assert rp.main(plan) == 0 and capsys.readouterr().out == "medium deep effort\n"
    pin = tmp_path / "pin.toml"
    pin.write_text(NO_EFFORT)
    assert rp.main(args + ["--pin", str(pin)]) == 0 and capsys.readouterr().out == "- - -\n"
    assert rp.main(plan + ["--pin", str(pin)]) == 0 and capsys.readouterr().out == "- - -\n"
    with manifest.open("a") as fh:                       # an unreadable line: the effort is unknown, the mark is not
        fh.write('{"ts": 1795170001, "kind": "round", "todo": "todo/00-infr\n')
    assert rp.main(plan) == 3 and capsys.readouterr().out == "- - effort\n"


def _argv(tmp_path):
    import json
    return json.loads((tmp_path / "prompt.txt.argv").read_text())


def _last_receipt(tmp_path):
    import json
    return json.loads((tmp_path / "reviews/manifest.jsonl").read_text().splitlines()[-1])


def test_dispatch_passes_pinned_effort(tmp_path):
    done, _prompt = _dispatch(tmp_path, "round", "HIGHEST: none\n", base=_rev(tmp_path, "HEAD~4"))
    argv = _argv(tmp_path)
    assert argv[:3] == ["--effort", "high", "--"], (argv[:3], done.stderr)
    e = _last_receipt(tmp_path)
    assert (e["effort"], e["tier"]) == ("high", "deep")


def test_plan_receipt_marks_the_schedule(tmp_path):
    done, _prompt = _dispatch(tmp_path, "plan", "HIGHEST: none\n")
    assert _last_receipt(tmp_path)["schedule"] == "effort", done.stderr
    assert _argv(tmp_path)[:2] == ["--effort", "medium"]


def test_section_planned_before_landing_is_grandfathered(tmp_path):
    reviews = tmp_path / "reviews"
    reviews.mkdir()
    (reviews / "manifest.jsonl").write_text(_receipt("plan", _rev(tmp_path, "HEAD~4")) + "\n")   # no marker
    done, _prompt = _dispatch(tmp_path, "round", "HIGHEST: none\n", base=_rev(tmp_path, "HEAD~4"))
    assert "--effort" not in _argv(tmp_path), done.stderr
    assert "effort" not in _last_receipt(tmp_path)


def test_dispatch_effort_refusal_falls_back(tmp_path):
    """The base is settled (a plan pass reads none from the manifest), the chain is not: the pass runs at the
    Codex config's effort and its transcript says why."""
    reviews = tmp_path / "reviews"
    reviews.mkdir()
    (reviews / "manifest.jsonl").write_text('{"ts": 1795170001, "kind": "round", "todo": "todo/00-infr\n')
    done, _prompt = _dispatch(tmp_path, "plan", "HIGHEST: none\n")
    assert "--effort" not in _argv(tmp_path), done.stderr
    log = next(reviews.glob("*-plan-direct.out")).read_text()
    assert "effort unknown" in log and "the Codex config value applies" in log
    assert _last_receipt(tmp_path)["schedule"] == "effort"          # the mark depends on the pin alone


def test_dispatch_unknown_base_still_refuses(tmp_path):
    reviews = tmp_path / "reviews"
    reviews.mkdir()
    (reviews / "manifest.jsonl").write_text(_receipt("plan", _rev(tmp_path, "HEAD~4")) + "\n"
                                            + '{"ts": 1795170001, "kind": "round", "todo": "todo/00-infr\n')
    done, _prompt = _dispatch(tmp_path, "round", "HIGHEST: none\n", base="")
    assert done.returncode == 2 and not (tmp_path / "prompt.txt.argv").exists(), done.stderr


def test_default_companion_is_vendored():
    text = (REPO / "scripts/codex-dispatch.sh").read_text()
    assert 'tools/vendor/codex-companion/scripts/codex-companion.mjs' in text
    broker = (REPO / "scripts/overnight/review-broker-codex-dispatch.sh").read_text()
    assert 'tools/vendor/codex-companion/scripts/codex-companion.mjs' in broker


def test_the_effort_answer_leaves_no_file_behind(tmp_path):
    _dispatch(tmp_path, "plan", "HIGHEST: none\n")
    assert not list((tmp_path / "reviews").glob(".paths.*")), list((tmp_path / "reviews").iterdir())


def test_an_override_companion_that_does_not_declare_effort_gets_none(tmp_path):
    """A stale CODEX_COMPANION_PATH to the stock companion would take --effort as focus text and run at the config
    effort while the receipt named another: it gets no --effort, and the transcript says why."""
    import os
    import subprocess
    fake = tmp_path / "companion.js"
    fake.write_text(FAKE_COMPANION)
    (tmp_path / "review.txt").write_text("HIGHEST: none\n")
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_REVIEWS_DIR": str(tmp_path / "reviews"),
           "FAKE_PROMPT_OUT": str(tmp_path / "prompt.txt"), "FAKE_REVIEW": str(tmp_path / "review.txt"),
           "REVIEW_BASE": _rev(tmp_path, "HEAD~4")}
    env.pop("CODEX_COMPANION_ACCEPTS_EFFORT", None)
    subprocess.run(["bash", str(REPO / "scripts/codex-dispatch.sh"),
                    "[review-kind: round] todo/00-infrastructure/TODO-01-developer-tooling-stack.md section 99\nReview it."],
                   capture_output=True, text=True, timeout=120, cwd=str(_fixture_repo(tmp_path)), env=env)
    assert "--effort" not in _argv(tmp_path)
    assert "effort" not in _last_receipt(tmp_path)
    log = next((tmp_path / "reviews").glob("*-round-direct.out")).read_text()
    assert "effort not applied" in log


def test_each_pass_runs_the_companion_in_its_own_data_and_reaps_it(tmp_path, monkeypatch):
    """Concurrent passes shared the companion's job state (an unlocked read-modify-write that lost job records) and
    its detached broker. Each pass now gets a private CLAUDE_PLUGIN_DATA, no inherited broker endpoint, and every
    process carrying that directory, a detached one included, is stopped and the directory removed when it ends."""
    import json
    import os
    import time
    monkeypatch.setenv("CODEX_COMPANION_APP_SERVER_ENDPOINT", "unix:/nonexistent/broker.sock")
    probe = tmp_path / "probe.json"
    fake = tmp_path / "companion.js"
    fake.write_text(FAKE_COMPANION + f"""
const cp = require('child_process');
const child = cp.spawn('sleep', ['300'], {{ detached: true, stdio: 'ignore' }});
child.unref();
fs.writeFileSync({json.dumps(str(probe))}, JSON.stringify({{
  data: process.env.CLAUDE_PLUGIN_DATA || '', endpoint: process.env.CODEX_COMPANION_APP_SERVER_ENDPOINT || '',
  child: child.pid }}));
""")
    import subprocess
    (tmp_path / "review.txt").write_text("HIGHEST: none\n")
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_COMPANION_ACCEPTS_EFFORT": "1",
           "CODEX_REVIEWS_DIR": str(tmp_path / "reviews"), "FAKE_PROMPT_OUT": str(tmp_path / "prompt.txt"),
           "FAKE_REVIEW": str(tmp_path / "review.txt"), "REVIEW_BASE": "HEAD", "TMPDIR": str(tmp_path)}
    subprocess.run(["bash", str(REPO / "scripts/codex-dispatch.sh"),
                    "[review-kind: plan] todo/00-infrastructure/TODO-01-developer-tooling-stack.md section 99\nReview it."],
                   capture_output=True, text=True, timeout=120, cwd=str(_fixture_repo(tmp_path)), env=env)
    seen = json.loads(probe.read_text())
    try:
        assert seen["data"].startswith(str(tmp_path / "reviews" / ".cp.")), seen
        assert seen["endpoint"] == "", "the pass inherited a shared broker endpoint"
        assert not pathlib.Path(seen["data"]).exists(), "the pass's companion data outlived it"
        for _ in range(40):
            if not pathlib.Path(f"/proc/{seen['child']}").exists():
                break
            time.sleep(0.05)
        state = pathlib.Path(f"/proc/{seen['child']}/stat")
        assert not state.exists() or state.read_text().rsplit(") ", 1)[1][0] == "Z", "a detached child outlived the pass"
    finally:
        try:
            os.kill(seen["child"], 9)
        except ProcessLookupError:
            pass


def test_the_broker_forwards_the_effort_declaration():
    broker = (REPO / "scripts/overnight/review-broker-codex-dispatch.sh").read_text()
    run = next(line for line in broker.splitlines() if 'CODEX_COMPANION_PATH=$(q "$COMPANION")' in line)
    assert 'CODEX_COMPANION_ACCEPTS_EFFORT=$(q "${CODEX_COMPANION_ACCEPTS_EFFORT:-}")' in run


def test_a_pass_with_tmpdir_in_the_checkout_leaves_its_candidate_alone(tmp_path):
    """The companion writes under its plugin data and os.tmpdir() (the broker's cxc-* directory). With TMPDIR inside
    the checkout, unignored, those files were part of the candidate, so a pass that ended while another was running
    saw the other's files and was discarded as stale (performance review, 2026-11-23). Both now live inside the
    pass's private directory in the reviews directory, which no candidate read covers."""
    import os
    import subprocess
    repo = _fixture_repo(tmp_path)
    inside = repo / "scratch"
    inside.mkdir(exist_ok=True)
    fake = tmp_path / "companion.js"
    fake.write_text(FAKE_COMPANION + """
const os = require('os'); const path = require('path');
fs.mkdtempSync(path.join(os.tmpdir(), 'cxc-'));
fs.writeFileSync(path.join(process.env.CLAUDE_PLUGIN_DATA, 'state.json'), '{}');
fs.writeFileSync(process.env.FAKE_PROMPT_OUT + '.tmpdir', os.tmpdir());
""")
    (tmp_path / "review.txt").write_text("LENS plan: clean\nHIGHEST: none\n")
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_COMPANION_ACCEPTS_EFFORT": "1",
           "CODEX_REVIEWS_DIR": str(tmp_path / "reviews"), "FAKE_PROMPT_OUT": str(tmp_path / "prompt.txt"),
           "FAKE_REVIEW": str(tmp_path / "review.txt"), "REVIEW_BASE": "HEAD", "TMPDIR": str(inside)}
    done = subprocess.run(["bash", str(REPO / "scripts/codex-dispatch.sh"),
                           "[review-kind: plan] todo/00-infrastructure/TODO-01-developer-tooling-stack.md section 99\nReview it."],
                          capture_output=True, text=True, timeout=120, cwd=str(repo), env=env)
    assert done.returncode == 0, done.stderr
    assert _last_receipt(tmp_path)["pass_checked"] is True
    seen_tmp = pathlib.Path((tmp_path / "prompt.txt.tmpdir").read_text())
    assert (tmp_path / "reviews") in seen_tmp.parents, seen_tmp
    assert not any(inside.iterdir()), list(inside.iterdir())


def test_a_pass_that_never_finishes_is_ended_by_its_limit_with_everything_it_started(tmp_path):
    """A companion whose app-server exits mid-turn waits forever for a completion (performance review,
    2026-11-23). The pass's limit ends it, and what it starts, a detached child spawned from its TERM handler
    included (round review: a /proc-scan reaper missed exactly that), is ended with it."""
    import json
    import os
    import subprocess
    import time
    late = tmp_path / "late.pid"
    fake = tmp_path / "companion.js"
    fake.write_text(f"""
const fs = require('fs'); const cp = require('child_process');
process.on('SIGTERM', () => {{
  const c = cp.spawn('setsid', ['sleep', '300'], {{ detached: true, stdio: 'ignore' }});
  fs.writeFileSync({json.dumps(str(late))}, String(c.pid));
  process.exit(0);
}});
setInterval(() => {{}}, 1000);
""")
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_COMPANION_ACCEPTS_EFFORT": "1",
           "CODEX_REVIEWS_DIR": str(tmp_path / "reviews"), "REVIEW_BASE": "HEAD", "CODEX_REVIEW_MAX_SECS": "4"}
    started = time.monotonic()
    done = subprocess.run(["bash", str(REPO / "scripts/codex-dispatch.sh"),
                           "[review-kind: plan] todo/00-infrastructure/TODO-01-developer-tooling-stack.md section 99\nReview it."],
                          capture_output=True, text=True, timeout=60, cwd=str(_fixture_repo(tmp_path)), env=env)
    took = time.monotonic() - started
    pid = int(late.read_text()) if late.exists() else None
    try:
        assert done.returncode != 0 and took < 30, (done.returncode, took)
        assert "ran past its 4s limit" in done.stderr, done.stderr[-1500:]
        assert pid is not None, "the TERM handler never ran"
        state = pathlib.Path(f"/proc/{pid}/stat")
        for _ in range(40):
            if not state.exists() or state.read_text().rsplit(") ", 1)[1][0] == "Z":
                break
            time.sleep(0.05)
        assert not state.exists() or state.read_text().rsplit(") ", 1)[1][0] == "Z", "a late child outlived the pass"
        assert not list((tmp_path / "reviews").glob(".cp.*"))
    finally:
        if pid:
            try:
                os.kill(pid, 9)
            except ProcessLookupError:
                pass


def test_the_unlogged_fallback_is_bounded_too(tmp_path):
    """An unmarked review with no writable reviews directory ran node directly, outside the limit (round review,
    2026-11-23): it now runs as one bounded pass too."""
    import os
    import subprocess
    import time
    fake = tmp_path / "companion.js"
    fake.write_text("setInterval(() => {}, 1000);\n")
    blocked = tmp_path / "file-not-dir"
    blocked.write_text("")
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_REVIEWS_DIR": str(blocked / "reviews"),
           "CODEX_REVIEW_MAX_SECS": "3", "TMPDIR": str(tmp_path)}
    started = time.monotonic()
    done = subprocess.run(["bash", str(REPO / "scripts/codex-dispatch.sh"), "plain review, no marker"],
                          capture_output=True, text=True, timeout=60, cwd=str(_fixture_repo(tmp_path)), env=env)
    assert done.returncode == 124 and time.monotonic() - started < 30, (done.returncode, done.stderr[-800:])
    assert not list(tmp_path.glob(".cp.*"))


def test_a_prompt_at_the_argument_limit_reaches_the_companion_whole(tmp_path):
    """The prompt crosses the supervisor in a file: an environment entry (PASS_PROMPT=...) has a smaller exec
    limit than the single argument it becomes, and a 131,065-byte prompt failed with E2BIG (round review)."""
    import json
    first = "[review-kind: adversarial] todo/00-infrastructure/TODO-01-developer-tooling-stack.md section 99\n"
    body = "x" * (131065 - len(first))           # the whole argument is 131,065 bytes: fits as one, not as an entry
    done, _prompt = _dispatch(tmp_path, "adversarial", "no lens lines at all\n", body=body)
    assert done.returncode == 0, done.stderr[-800:]
    argv = json.loads((tmp_path / "prompt.txt.argv").read_text())
    assert argv[-1] == first + body


def test_a_caller_that_stops_reading_still_gets_a_published_status(tmp_path):
    """The transcript writer is bounded just past the pass's limit: a caller holding a full stdout pipe open no
    longer holds the receipt and the .done publication (performance review, 2026-11-23)."""
    import os
    import subprocess
    import time
    fake = tmp_path / "companion.js"
    fake.write_text("process.stdout.write('y'.repeat(1 << 20) + '\\nHIGHEST: none\\n');\n")
    reviews = tmp_path / "reviews"
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_REVIEWS_DIR": str(reviews),
           "CODEX_REVIEW_MAX_SECS": "2", "REVIEW_BASE": "HEAD"}
    proc = subprocess.Popen(["bash", str(REPO / "scripts/codex-dispatch.sh"),
                             "[review-kind: adversarial] todo/00-infrastructure/TODO-01-developer-tooling-stack.md section 99\nReview it."],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, cwd=str(_fixture_repo(tmp_path)),
                            env=env, start_new_session=True)
    try:
        started = time.monotonic()
        while time.monotonic() - started < 90 and not list(reviews.glob("*.out.done")):
            time.sleep(0.2)
        dones = list(reviews.glob("*.out.done"))
        assert dones, "no status was published while the caller held its pipe unread"
        assert dones[0].read_text().strip() not in ("", "0"), "an unread transcript was credited as complete"
    finally:
        os.killpg(proc.pid, 9)
        proc.wait(timeout=10)
