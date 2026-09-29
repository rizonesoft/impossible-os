#!/usr/bin/env python3
"""The review broker's bare-flag guard: refusal direction and refusal text.

2026-09-28/29 canary: 19 main-loop refusals, 11 on `--cached`. The guard was
right every time; the fix text was incomplete. Wrapping a whole spaced phrase
(`git diff --cached`) in backticks still yields a bare `--cached` token after
the companion's whitespace split, so the retry was refused again with the same
message. These tests pin that the spaced code span is STILL refused (the text
change must not weaken the guard) and that the refusal now says the backtick
must touch the flag. Only refusals are exercised: a passing prompt would go on
to dispatch a real review.
"""
from __future__ import annotations

import pathlib
import subprocess

REPO = pathlib.Path(__file__).resolve().parents[3]
BROKER = REPO / "scripts/overnight/review-broker-codex-dispatch.sh"
HEAD = "[review-kind: adversarial] todo/00-infrastructure/TODO-10-documentation-site.md section 99\n"


def _run(body):
    return subprocess.run(["bash", str(BROKER), HEAD + body], capture_output=True,
                          text=True, timeout=30, cwd=str(REPO))


def test_spaced_code_span_is_still_refused_and_the_text_says_why():
    r = _run("Check that `git diff --cached` lists only staged paths.")
    assert r.returncode == 2, (r.returncode, r.stderr)
    assert "bare CLI flag token: --cached" in r.stderr, r.stderr
    assert "must TOUCH the flag" in r.stderr, "refusal must say the backtick has to touch the flag"
    assert "`git diff` `--cached`" in r.stderr, r.stderr


def test_bare_flag_in_prose_is_refused():
    r = _run("Run it with --check and report.")
    assert r.returncode == 2 and "--check" in r.stderr, (r.returncode, r.stderr)


def test_flag_at_prompt_start_line_is_refused():
    r = _run("--scope all of it")
    assert r.returncode == 2, (r.returncode, r.stderr)


if __name__ == "__main__":
    test_spaced_code_span_is_still_refused_and_the_text_says_why()
    test_bare_flag_in_prose_is_refused()
    test_flag_at_prompt_start_line_is_refused()
    print("PASS: broker bare-flag guard")
