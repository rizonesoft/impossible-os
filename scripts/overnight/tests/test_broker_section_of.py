#!/usr/bin/env python3
"""Review-broker section attribution (v17 close-out, 2026-08-29).

The broker's pattern spelled the section SIGN as `\\xc2\\xa7` inside double
quotes -- a literal string to bash and to ERE -- so that branch NEVER matched,
and a sign-marked prompt was attributed to whatever `section NN` appeared later
in its first line (observed: one section's leg recorded under another, which
then made review-envelope report the wave as missing). The rule is now anchored
to the marker immediately after the TODO path and exposed as `--section-of` so
it can be pinned without a dispatch. The direct wrapper `scripts/codex-dispatch.sh`
carries the same rule for its manifest entry.
"""
import pathlib
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
BROKER = REPO / "scripts/overnight/review-broker-codex-dispatch.sh"
SIGN = "§"  # built at runtime: a code file must not carry the sign + digits
HEAD = "[review-kind: adversarial] todo/x/TODO-06.md "

CASES = [
    (HEAD + SIGN + "59 post-ship review", "59"),
    (HEAD + SIGN + " 59 post-ship review", "59"),
    (HEAD + "section 59 round 3", "59"),
    (HEAD + "Section-59 round 3", "59"),
    # The anchored marker wins over a later mention of ANOTHER section.
    (HEAD + SIGN + "59 confirming pass, because section 55 put both", "59"),
    (HEAD + "section 59: section 55 is the parent", "59"),
    # No marker after the path: fall back to the first mention on the line.
    (HEAD + "round 3 (section 12)", "12"),
    # Nothing -> empty (the manifest entry then carries no section key).
    (HEAD + "no marker at all", ""),
]


def _section_of(line):
    r = subprocess.run(["bash", str(BROKER), "--section-of", line],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return r.stdout.strip()


def test_section_of_cases():
    bad = [(line, got, want) for line, want in CASES
           if (got := _section_of(line)) != want]
    assert not bad, bad


def test_section_sign_pattern_is_a_real_byte_not_an_escape():
    src = BROKER.read_text(encoding="utf-8")
    assert "\\xc2\\xa7" not in src, "the never-matching \\xc2\\xa7 spelling is back"
    assert SIGN + "[[:space:]]*" in src


def test_direct_wrapper_carries_the_same_rule():
    src = (REPO / "scripts/codex-dispatch.sh").read_text(encoding="utf-8")
    assert SIGN + "[[:space:]]*" in src and "manifest.jsonl" in src, \
        "codex-dispatch.sh no longer registers its leg in the shared manifest"


if __name__ == "__main__":
    test_section_of_cases()
    test_section_sign_pattern_is_a_real_byte_not_an_escape()
    test_direct_wrapper_carries_the_same_rule()
    print("PASS: broker section attribution")
