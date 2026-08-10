#!/usr/bin/env python3
"""v13 carry, fixed 2026-08-10: the impact cone's caller search is word-bounded.

THE FILED HYPOTHESIS WAS WRONG, AND THAT IS THE USEFUL PART. v13 recorded
`cone_size: 96` and `global_state_escalation: true` for a 4-file diff containing
no C at all, and explicitly said the mechanism was UNTESTED -- guessing "a
generic token from the Python/shell diff grepped across src/". Reproduced on the
commit it names (859d96cf0), the real mechanism is narrower: the caller
alternation was an UNANCHORED SUBSTRING search, so a changed Python symbol named
`main` matched `domain (` and `remain (`. Measured: 25 of the 48 reported
callers matched ONLY without a word boundary -- e.g. `include/kernel/quota/quota.h`
via the comment "Counter domain (HARD invariant)". After the fix: 48 -> 23
callers, cone_size 96 -> 74.

Cost was a wrong ROUTE, not just a wrong number: `review-todo-section` Phase 1
keys on these fields, so a small tooling diff was told to spend a
`review-evidence-mapper` dispatch on the strength of matches inside prose.
"""
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
CONE = REPO / "scripts/overnight/impact-cone.py"


def _pattern_for(symbols):
    """Rebuild the caller pattern the way impact-cone.py does, from its own
    source, so this test cannot drift from the implementation it guards."""
    src = CONE.read_text()
    m = re.search(r'pat = "\|"\.join\((.+?) for s in symbols\[:40\]\)', src)
    assert m, "the caller-pattern construction moved; update this test"
    expr = m.group(1)
    return "|".join(eval(expr, {"re": re, "s": s}) for s in symbols)  # noqa: S307


def test_caller_pattern_is_word_bounded():
    """The fix itself, asserted against the real source rather than a copy."""
    src = CONE.read_text()
    assert re.search(r'pat = "\|"\.join\(\s*r"\\b"\s*\+\s*re\.escape\(s\)', src), \
        "the caller alternation lost its \\b anchor -- an unanchored search " \
        "matches a changed `main` inside `domain (`"


def test_substring_matches_are_rejected():
    """The behaviour, on the exact strings that produced the filed number."""
    pat = re.compile(_pattern_for(["main", "_diag"]))
    for hay in ["Counter domain (HARD invariant)",
                "things that remain (for now)",
                "int subdomain (void)"]:
        assert not pat.search(hay), f"still matches inside a longer word: {hay!r}"


def test_genuine_calls_still_match():
    """REFUSAL CONTROL. Anchoring must not stop the cone finding REAL callers --
    a caller search that matches nothing is worse than one that over-matches,
    because it silently narrows review scope."""
    pat = re.compile(_pattern_for(["main", "_diag"]))
    for hay in ["int main(void)", "  main (argc, argv)", "rc = _diag(x);",
                "\tmain(void);"]:
        assert pat.search(hay), f"no longer matches a genuine call: {hay!r}"


def test_leading_underscore_is_still_bounded():
    """`\\b` before `_` is intentional: `_` is a word character, so `\\b_diag`
    refuses `x_diag` while still matching a bare `_diag(`."""
    pat = re.compile(_pattern_for(["_diag"]))
    assert pat.search("_diag(1)")
    assert not pat.search("x_diag(1)")


def test_diff_is_scoped_to_c_family_files():
    """v14 residual, closed 2026-08-11. Everything the cone derives from the
    diff is a C construct -- `FUNC_HUNK_RE`/`FUNC_DEF_RE` read C definitions,
    `GLOBAL_RE` matches C declarations, `REG_TABLE_MARKERS` are C registration
    tables -- so feeding it a Python or shell diff invented symbols and
    "globals" that do not exist in the language it then searches. Measured on
    the commit the finding names: cone_size 96 -> 74 (word boundary) -> 4
    (this), callers 48 -> 23 -> 0, global_state_escalation true -> false."""
    src = CONE.read_text()
    assert 'if not src_files:\n        diff = ""' in src, (
        "an EMPTY src_files must yield an EMPTY diff explicitly -- `git diff "
        "<rng> --` with no pathspec diffs EVERYTHING, which would silently "
        "restore whole-repo behaviour on exactly the no-C diffs this fixes")
    assert 'diff = git(root, "diff", rng, "--unified=1", "--", *src_files)' in src, \
        "the ranged diff must be pathspec-limited to C-family files"
    assert 'diff = git(root, "diff", "HEAD", "--unified=1", "--", *src_files)' in src, \
        "the worktree diff must be pathspec-limited to C-family files"


def test_c_family_filter_covers_the_real_extensions():
    """REFUSAL CONTROL. Narrowing the extension list would blind the cone to
    real source -- the failure direction that silently shrinks review scope."""
    src = CONE.read_text()
    for ext in (".c", ".h", ".asm", ".S"):
        assert repr(ext) in src or f'"{ext}"' in src, f"{ext} dropped from the C-family filter"


if __name__ == "__main__":
    fails = []
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"  PASS {name}")
            except Exception as exc:
                fails.append(name)
                print(f"  FAIL {name}: {exc}")
    sys.exit(1 if fails else 0)
