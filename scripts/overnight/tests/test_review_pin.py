#!/usr/bin/env python3
"""The reviewer pin loads strictly, and the repository's own pin holds."""
import pathlib
import sys

import pytest

REPO = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "scripts"))
import review_pin as rp  # noqa: E402

GOOD = (REPO / ".conclave/reviewers.toml").read_text()


def _pin(text=GOOD):
    return rp.load_pin(data=text.encode())


def test_the_repository_pin_holds():
    pin = _pin()
    assert (pin["reviewer"], pin["cli"], pin["model"]) == ("astra", "codex", "gpt-6-astra")
    assert (pin["pass_cap"], pin["no_progress_cap"]) == (45, 3)
    assert pin["lenses"]["plan"] == ("plan",) and pin["lenses"]["completion"] == ("plan-delivery", "record")
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
    ('model = "gpt-6-astra"', 'model = "gpt-6-astra"\neffort = "high"'),
    ('policy = "four-pass"', 'policy = "three-pass"'),
    ("pass_cap = 45", "pass_cap = 3"),
    ('[schedule.performance]\nlenses = ["optimization"]\n', ""),
    ('[schedule.performance]\nlenses = ["optimization"]', '[schedule.performance]\nlenses = ["adversarial"]'),
    ("pass_cap = 45", 'pass_cap = "45"'),
    ("pass_cap = 45", "pass_cap = true"),
    ("pass_cap = 45\n", ""),
    ("no_progress_cap = 3", "no_progress_cap = 46"),
    ("no_progress_cap = 3", "no_progress_cap = 3\nseverity_stop_after = 2"),
    ('lenses = ["plan"]', 'lenses = ["plan", "plan"]'),
    ('lenses = ["plan"]', 'lenses = ["made-up"]'),
    ('lenses = ["plan"]', "lenses = []"),
    ('lenses = ["plan-delivery", "record"]\n', ""),
    ('lenses = ["plan-delivery", "record"]', 'lenses = ["plan-delivery", "record", "adversarial"]'),
    ('blocking = ["P1", "P2"]', 'blocking = ["P2"]'),
    ('blocking = ["P1", "P2"]', 'blocking = ["P1", "P4"]'),
    ('blocking = ["P1", "P2"]', "blocking = []"),
    ('blocking = ["P1", "P2"]\n', ""),
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
    config.write_text('model = "gpt-6-astra"\nmodel_reasoning_effort = "low"\n')
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
    assert rp.blocks(pin, "P1") and rp.blocks(pin, "P2")
    assert not rp.blocks(pin, "P3") and not rp.blocks(pin, "none")
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


def test_a_lens_block_survives_the_companions_argument_split():
    """codex-companion re-splits the prompt on whitespace and reads quotes and bare flags as syntax."""
    pin = _pin()
    for kind in rp.SECTION_KINDS:
        block = rp.lens_block(pin, kind, ["src/kernel/a.c", "src/desktop/b.c"])
        assert "'" not in block and '"' not in block, kind
        assert not any(word.startswith("--") for word in block.split()), kind


FAKE_COMPANION = """
const fs = require('fs');
const prompt = process.argv[3] || '';
fs.writeFileSync(process.env.FAKE_PROMPT_OUT, prompt);
// As the real companion: progress first (here, the echoed prompt), then the heading, then the response.
process.stdout.write('[codex] prompt: ' + prompt + '\\n# Codex Adversarial Review\\n\\n');
process.stdout.write(fs.readFileSync(process.env.FAKE_REVIEW, 'utf8'));
"""


def _dispatch(tmp_path, kind, review, base="HEAD", marker=None, body="Review it.", reviews_dir=None):
    import os
    import subprocess
    fake = tmp_path / "companion.js"
    fake.write_text(FAKE_COMPANION)
    (tmp_path / "review.txt").write_text(review)
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake),
           "CODEX_REVIEWS_DIR": str(reviews_dir or tmp_path / "reviews"),
           "FAKE_PROMPT_OUT": str(tmp_path / "prompt.txt"), "FAKE_REVIEW": str(tmp_path / "review.txt"),
           "REVIEW_BASE": base}
    done = subprocess.run(["bash", str(REPO / "scripts/codex-dispatch.sh"),
                           (marker or f"[review-kind: {kind}]")
                           + f" todo/00-infrastructure/TODO-01-developer-tooling-stack.md section 99\n{body}"],
                          capture_output=True, text=True, timeout=120, cwd=str(REPO), env=env)
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


def test_an_older_kind_is_dispatched_exactly_as_before(tmp_path):
    done, prompt = _dispatch(tmp_path, "adversarial", "no lens lines at all\n")
    assert done.returncode == 0, done.stderr
    assert "LENS" not in prompt and prompt.endswith("Review it.")


def test_the_round_guard_takes_its_caps_from_the_pin(tmp_path, monkeypatch, capsys):
    sys.path.insert(0, str(REPO / ".claude/hooks"))
    import review_round_guard as guard
    monkeypatch.delenv("OVERNIGHT_REVIEW_ROUND_CEILING", raising=False)
    monkeypatch.delenv("OVERNIGHT_REVIEW_NOPROGRESS_CAP", raising=False)
    assert (guard._noprogress_cap(), guard._ceiling()) == (3, 45)
    other = tmp_path / "reviewers.toml"
    other.write_text(GOOD.replace("pass_cap = 45", "pass_cap = 33").replace("no_progress_cap = 3", "no_progress_cap = 2"))
    assert guard._pin_caps(other) == (2, 33)
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
                              capture_output=True, text=True, timeout=60, cwd=str(REPO),
                              env={**os.environ, "CODEX_COMPANION_PATH": str(fake),
                                   "CODEX_REVIEWS_DIR": str(tmp_path / "reviews2")})
        assert done.returncode == 2 and "names its TODO and section on the first line" in done.stderr, done.stderr


SELF_CONTAINED_COMPANION = """
const fs = require('fs');
const path = require('path');
fs.writeFileSync(path.join(__dirname, 'prompt.txt'), process.argv[3] || '');
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
    while time.time() < deadline and not (log.exists() and "Turn completed (rc=" in log.read_text()):
        time.sleep(0.2)
    assert "Turn completed (rc=0)" in log.read_text(), log.read_text()[-400:]
    assert "[codex]" not in log.read_text(), "the real reviewer must not have been reached"
    assert "- record: " in (tmp_path / "prompt.txt").read_text(), "the lenses were appended"
    lines = [json.loads(ln) for ln in (repo / ".claude/overnight/reviews/manifest.jsonl").read_text().splitlines()]
    receipts = [e for e in lines if e.get("pass_checked") is True]
    assert len(receipts) == 1 and receipts[0]["kind"] == "completion" and receipts[0]["section"] == 7 \
        and receipts[0]["rc"] == 0 and receipts[0]["highest"] == "none", lines



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
fs.writeFileSync(process.env.FAKE_PROMPT_OUT, process.argv[3] || '');
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
