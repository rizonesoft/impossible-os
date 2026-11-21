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
    # v22: `s3` straight after the path, the form item prompts use.
    (HEAD + "s3 round 2", "3"),
    (HEAD + "s12: item fix", "12"),
    # REFUSAL: only straight after the path, and only as its own word.
    (HEAD + "reviews x86 s3 sleep", ""),
    (HEAD + "sum3 of it", ""),
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


LIB = REPO / "scripts/lib/review-prompt.sh"


def test_section_sign_pattern_is_a_real_byte_not_an_escape():
    src = LIB.read_text(encoding="utf-8")
    assert "\\xc2\\xa7" not in src, "the never-matching \\xc2\\xa7 spelling is back"
    assert SIGN + "[[:space:]]*" in src


def test_direct_wrapper_carries_the_same_rule():
    for rel in ("scripts/codex-dispatch.sh", "scripts/overnight/review-broker-codex-dispatch.sh",
                "scripts/codex-bg-dispatch.sh"):
        assert "lib/review-prompt.sh" in (REPO / rel).read_text(encoding="utf-8"), rel
    src = (REPO / "scripts/codex-dispatch.sh").read_text(encoding="utf-8")
    assert "review_prompt_section_of" in src and "manifest.jsonl" in src, \
        "codex-dispatch.sh no longer registers its leg in the shared manifest"


def test_the_recorder_reads_the_section_from_the_first_line_only():
    """A whole-file review whose inventory numbers sections is no section's review."""
    import sys
    sys.path.insert(0, str(REPO / ".claude/hooks"))
    import codex_review_completed as crc
    head = "[review-kind: adversarial] todo/00-x/TODO-01-x.md"
    body = "\nsection 5 depends on section 3\n"
    assert crc._detect_todo_path(head + body, REPO)[1] == ""
    assert crc._detect_todo_path(head + " section 7" + body, REPO)[1] == SIGN + "7"
    assert crc._detect_todo_path(head + " s4 sleep" + body, REPO)[1] == SIGN + "4"
    assert crc._detect_todo_path(head + " (see section 9)" + body, REPO)[1] == SIGN + "9"
    # The fixed templates once put the marker alone on line one, the target on line two.
    two = "[review-kind: adversarial]\nTarget: todo/00-x/TODO-01-x.md section 35\nFiles in scope: a\n"
    assert crc._detect_todo_path(two, REPO)[1] == SIGN + "35"


def test_the_shipped_templates_name_path_and_section_on_the_marker_line():
    text = (REPO / ".claude/skills/review-todo-section/codex-prompt-template.md").read_text(encoding="utf-8")
    import re as _re
    markers = _re.findall(r"^\[review-kind: (?:adversarial|consistency|perf)\][^\n]*$", text, _re.M)
    assert markers and all("section <" in m for m in markers if "bash " not in m), markers


def test_the_first_line_parser_is_linear_on_a_long_whitespace_prefix():
    import time
    lib = REPO / "scripts/lib/review-prompt.sh"
    prompt = "\n" + " " * 120000 + "[review-kind: round] todo/a.md section 3\nbody"
    t = time.time()
    r = subprocess.run(["bash", "-c", f". {lib}; review_prompt_first_line \"$1\"", "_", prompt],
                       capture_output=True, text=True, timeout=30)
    assert r.stdout == " " * 120000 + "[review-kind: round] todo/a.md section 3" and time.time() - t < 1.5


if __name__ == "__main__":
    test_section_of_cases()
    test_section_sign_pattern_is_a_real_byte_not_an_escape()
    test_direct_wrapper_carries_the_same_rule()
    print("PASS: broker section attribution")
