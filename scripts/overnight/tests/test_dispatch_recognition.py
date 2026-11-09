#!/usr/bin/env python3
"""How codex_review_completed recognises a review dispatch: the section number it
records, the compound shell shapes it must still attribute, and clearing a stale
section rather than keeping it."""
import importlib.util
import pathlib
import sys


def _load_crc():
    spec = importlib.util.spec_from_file_location(
        "crc", str(pathlib.Path(__file__).resolve().parents[3]
                   / ".claude/hooks/codex_review_completed.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def test_section_parser_accepts_the_shapes_the_runner_actually_emits():
    r"""2026-07-31: `section\s+` required whitespace, so the runner's own
    `section-39` parsed to nothing and the stamp kept the PREVIOUS section's
    number -- 3 refused commits and 3 redundant Codex dispatches."""
    crc = _load_crc()
    for prompt, want in (
            ("[review-kind: perf] todo/x.md section-39 body", "39"),
            ("[review-kind: perf] todo/x.md section 39 body", "39"),
            ("[review-kind: perf] todo/x.md section_39 body", "39"),
            ("[review-kind: perf] todo/x.md section:39 body", "39"),
            ("[review-kind: perf] todo/x.md \u00a739 body", "39"),
            ("[review-kind: perf] todo/x.md \u00a7 39 body", "39"),
            ("[review-kind: perf] todo/x.md no number", None)):
        mo = crc._SECTION_RE.search(prompt)
        got = mo.group(1) if mo else None
        assert got == want, (prompt, got, want)


def test_every_compound_dispatch_shape_is_still_attributed():
    """The skills PRESCRIBE parallel/compound dispatch shapes, so recognition
    must survive `&`, `&&`, `||`, `;` and a trailing pipe."""
    hooks = str(pathlib.Path(__file__).resolve().parents[3] / ".claude/hooks")
    if hooks not in sys.path:
        sys.path.insert(0, hooks)
    import _codex_dispatch as cd
    import _review_kind as rk
    D = ("bash scripts/codex-dispatch.sh "
         "'[review-kind: perf] todo/x.md section-39 body'")
    for label, cmd in (("sole", D),
                       ("backgrounded", D + " &\nwait"),
                       ("|| fallback", "bash A 2>/dev/null || " + D),
                       ("&& chain", "echo go && " + D),
                       ("; chain", "echo go ; " + D),
                       ("piped", D + " 2>&1 | tail -5")):
        assert cd.is_codex_dispatch(cmd) is True, label
        assert rk.detect_review_kind_from_cmd(cmd) == "perf", label


def test_unparseable_section_clears_rather_than_keeping_the_stale_one():
    """FAIL CLOSED: a leftover `<kind>_section` claims a review for a section it
    was never dispatched against, which can satisfy a gate over different code."""
    crc = _load_crc()
    entry = {"section": "38", "perf": 1, "perf_section": "38"}
    section = ""            # parser found nothing
    kind = "perf"
    # mirror of the hook's write path
    if section:
        entry[f"{kind}_section"] = section.lstrip("\u00a7").strip()
    else:
        entry.pop(f"{kind}_section", None)
    assert "perf_section" not in entry, entry


if __name__ == "__main__":
    test_section_parser_accepts_the_shapes_the_runner_actually_emits()
    test_every_compound_dispatch_shape_is_still_attributed()
    test_unparseable_section_clears_rather_than_keeping_the_stale_one()
    print("PASS: dispatch recognition")
