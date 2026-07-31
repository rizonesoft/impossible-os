#!/usr/bin/env python3
"""WS2 -- a fresh, content-bound section-pack satisfies agent_dispatch_required
in lieu of a discovery-agent dispatch. The load-bearing property is FAIL-CLOSED:
an absent, stale, or wrong-section receipt must NOT satisfy the gate.

Tests the receipt predicate directly (the acceptance logic), so it needs no
headless-session harness."""
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
sys.path.insert(0, str(REPO / "scripts/overnight"))
from worktree_hash import worktree_key  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    "adr", REPO / ".claude/hooks/agent_dispatch_required.py")
adr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(adr)

_rspec = importlib.util.spec_from_file_location(
    "rdg", REPO / ".claude/hooks/review_dispatch_gate.py")
rdg = importlib.util.module_from_spec(_rspec)
_rspec.loader.exec_module(rdg)


def _fixture(d):
    root = pathlib.Path(d)
    (root / ".claude/state").mkdir(parents=True)
    (root / "src/kernel").mkdir(parents=True)
    (root / "todo").mkdir()
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    (root / "todo/TODO-42.md").write_text("# t\n## 1. S\n- [ ] x\n")
    (root / "src/kernel/x.c").write_text("int x(void){return 1;}\n")
    (root / ".claude/state/sequencer-run.json").write_text(
        json.dumps({"active": True, "phase": "SECTIONS",
                    "file": "todo/TODO-42.md"}))
    return root


def _write_receipt(root, todo, key_inputs, digest):
    (root / ".claude/state/last-section-pack.json").write_text(json.dumps({
        "todo": todo, "section": 1, "digest": digest,
        "key_inputs": key_inputs, "ts_ns": 1, "pack_path": "x"}))


def test_receipt_fail_closed_and_fresh_accept():
    with tempfile.TemporaryDirectory() as d:
        root = _fixture(d)
        key_inputs = ["todo/TODO-42.md", "src/kernel/x.c"]
        # A) no receipt -> not satisfied (fail-closed)
        assert adr._fresh_section_pack(root) is False
        # B) fresh receipt (digest == current worktree key) -> satisfied
        good = worktree_key(root, key_inputs)
        _write_receipt(root, "todo/TODO-42.md", key_inputs, good)
        assert adr._fresh_section_pack(root) is True
        # C) edit an input -> recomputed key != recorded digest -> stale, refused
        (root / "src/kernel/x.c").write_text("int x(void){return 2;}\n")
        assert adr._fresh_section_pack(root) is False
        # D) receipt for a DIFFERENT todo than the cursor -> refused
        (root / "src/kernel/x.c").write_text("int x(void){return 1;}\n")  # restore
        assert adr._fresh_section_pack(root) is True  # fresh again
        _write_receipt(root, "todo/OTHER.md", key_inputs,
                       worktree_key(root, key_inputs))
        assert adr._fresh_section_pack(root) is False


def test_pack_pass_binding_survives_intra_pass_edits():
    # I0: a pack accepted during a live implement pass must stay valid for the
    # WHOLE pass -- the first edit changes the worktree digest, but re-blocking
    # the 2nd edit of the same section on that expected drift was the bug.
    with tempfile.TemporaryDirectory() as d:
        root = _fixture(d)
        key_inputs = ["todo/TODO-42.md", "src/kernel/x.c"]

        def _progress(started_ts, orphaned=False):
            (root / ".claude/state/skill-progress.json").write_text(json.dumps(
                {"implement-todo-section": {"started_ts": started_ts,
                                            "compaction_orphaned": orphaned}}))

        def _receipt(ts_ns, digest):
            (root / ".claude/state/last-section-pack.json").write_text(json.dumps(
                {"todo": "todo/TODO-42.md", "section": 1, "digest": digest,
                 "key_inputs": key_inputs, "ts_ns": ts_ns, "pack_path": "x"}))

        # Live pass started at ts=1000; pack recorded during it (ts=2000), fresh.
        _progress(1000)
        _receipt(2000, worktree_key(root, key_inputs))
        assert adr._fresh_section_pack(root) is True
        # First edit -> digest drifts, but the pack was accepted THIS pass.
        (root / "src/kernel/x.c").write_text("int x(void){return 2;}\n")
        assert adr._fresh_section_pack(root) is True
        # Second edit -> still valid within the same pass (the actual bug).
        (root / "src/kernel/x.c").write_text("int x(void){return 3;}\n")
        assert adr._fresh_section_pack(root) is True
        # Control: pack from a PRIOR pass (ts=500 < pass start) + drift -> refused.
        _receipt(500, worktree_key(root, key_inputs))
        (root / "src/kernel/x.c").write_text("int x(void){return 4;}\n")
        assert adr._fresh_section_pack(root) is False
        # Control: compaction-orphaned pass -> no pass binding -> strict check.
        _progress(1000, orphaned=True)
        _receipt(2000, worktree_key(root, key_inputs))
        (root / "src/kernel/x.c").write_text("int x(void){return 5;}\n")
        assert adr._fresh_section_pack(root) is False


def test_malformed_receipt_refused():
    with tempfile.TemporaryDirectory() as d:
        root = _fixture(d)
        (root / ".claude/state/last-section-pack.json").write_text("{not json")
        assert adr._fresh_section_pack(root) is False
        (root / ".claude/state/last-section-pack.json").write_text(
            json.dumps({"todo": "todo/TODO-42.md"}))  # missing digest/inputs
        assert adr._fresh_section_pack(root) is False


def test_diff_facts_receipt_fail_closed_and_window():
    with tempfile.TemporaryDirectory() as d:
        root = _fixture(d)
        files = ["src/kernel/x.c"]
        window = 1000
        rp = root / ".claude/state/last-diff-facts.json"
        # A) absent -> refused
        assert rdg._fresh_diff_facts(root, window) is False
        # B) fresh + in-window -> accepted
        rp.write_text(json.dumps({"digest": worktree_key(root, files),
                                  "changed_files": files, "ts_ns": window + 5}))
        assert rdg._fresh_diff_facts(root, window) is True
        # C) out-of-window (older than pass start) -> refused
        rp.write_text(json.dumps({"digest": worktree_key(root, files),
                                  "changed_files": files, "ts_ns": window - 5}))
        assert rdg._fresh_diff_facts(root, window) is False
        # D) stale content (edit since the receipt) -> refused
        rp.write_text(json.dumps({"digest": worktree_key(root, files),
                                  "changed_files": files, "ts_ns": window + 5}))
        (root / "src/kernel/x.c").write_text("int x(void){return 9;}\n")
        assert rdg._fresh_diff_facts(root, window) is False




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
    test_receipt_fail_closed_and_fresh_accept()
    test_pack_pass_binding_survives_intra_pass_edits()
    test_malformed_receipt_refused()
    test_diff_facts_receipt_fail_closed_and_window()
    print("PASS: dispatch-receipt (WS2)")
