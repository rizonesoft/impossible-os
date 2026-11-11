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
    assert {"adversarial", "consistency", "optimization", "test-coverage"} <= set(pin["lenses"]["round"])
    assert rp.kinds(pin) == ("plan", "round", "completion", "reconciliation", "gap-audit")


@pytest.mark.parametrize("old, new", [
    ("schema_version = 1", "schema_version = 2"),
    ('claude = ["astra"]', 'claude = ["astra", "other"]'),
    ('claude = ["astra"]', 'claude = ["astra"]\ncodex = ["astra"]'),
    ('claude = ["astra"]', 'claude = ["nobody"]'),
    ('cli = "codex"', 'cli = "agy"'),
    ('model = "gpt-6-astra"', 'model = "gpt-6-astra"\neffort = "high"'),
    ('policy = "three-pass"', 'policy = "rotation"'),
    ("pass_cap = 45", "pass_cap = 2"),
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
    skimmed = full.replace("LENS optimization: clean", "optimization looked fine")
    assert rp.missing_lens_verdicts(pin, "round", skimmed) == ("optimization",)
    assert rp.missing_lens_verdicts(pin, "round", full.replace("consistency: clean", "consistency: maybe")) \
        == ("consistency",)
    assert rp.missing_lens_verdicts(pin, "round", full.replace("LENS quality: clean", "`LENS quality: findings`")) \
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
process.stdout.write(fs.readFileSync(process.env.FAKE_REVIEW, 'utf8'));
"""


def _dispatch(tmp_path, kind, review, base="HEAD"):
    import os
    import subprocess
    fake = tmp_path / "companion.js"
    fake.write_text(FAKE_COMPANION)
    (tmp_path / "review.txt").write_text(review)
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_REVIEWS_DIR": str(tmp_path / "reviews"),
           "FAKE_PROMPT_OUT": str(tmp_path / "prompt.txt"), "FAKE_REVIEW": str(tmp_path / "review.txt"),
           "REVIEW_BASE": base}
    done = subprocess.run(["bash", str(REPO / "scripts/codex-dispatch.sh"),
                           f"[review-kind: {kind}] todo/00-infrastructure/TODO-01-developer-tooling-stack.md section 99\nReview it."],
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
