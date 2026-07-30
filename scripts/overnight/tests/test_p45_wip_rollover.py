#!/usr/bin/env python3
# Protects P4.5/P4.6 (2026-07-12 plan): the MID-SECTION context-cap rotation.
# _rollover_failures_wip() accepts a WIP tree (committed, unpushed, unstamped, no
# receipts) but KEEPS the work-safety checks (committed-clean + no pending review
# + no bg job); the `rollover-wip` verb fires ONLY when the P4.1 rotate_hint is set
# AND that gate passes, hard-forbidding a mid-fix-loop / review-wait rotation. It
# does NOT touch the shipped _rollover_failures() ship gate.
#
# STATE_PATH resolves to the REAL repo at import, so this monkeypatches it (and
# repo_root / _emit_anchor) -- it must never touch live state.
import importlib.util
import json
import subprocess
import tempfile
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
HOOK = HERE.parents[2] / ".claude/hooks/run_phase_guard.py"


def _load(state_file, repo_dir):
    spec = importlib.util.spec_from_file_location("rpg_wip_test", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    mod.STATE_PATH = state_file
    mod.repo_root = lambda: repo_dir
    mod._emit_anchor = lambda s: None
    return mod


def _git_repo(d):
    root = pathlib.Path(d)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.email", "t@t"], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.name", "t"], check=True)
    (root / "src").mkdir()
    (root / "src/x.c").write_text("int x;\n")
    # coverage.* is TRACKED in the real repo (regenerated -> ' M' unstaged);
    # .claude/state/ is gitignored so the gates never see runtime state files.
    (root / "docs/test-coverage").mkdir(parents=True)
    (root / "docs/test-coverage/coverage.md").write_text("orig\n")
    (root / ".gitignore").write_text(".claude/state/\n")
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(root), "commit", "-qm", "i"], check=True,
                   capture_output=True)
    (root / ".claude/state").mkdir(parents=True)
    return root


def _head_blob(root, path):
    return subprocess.run(["git", "-C", str(root), "rev-parse", f"HEAD:{path}"],
                          capture_output=True, text=True).stdout.strip()


def _review(root, received, bind_head=True, run_id="rev-1"):
    # F3: a received review must bind the committed HEAD content. bind_head=True
    # writes trigger_blobs matching HEAD:src/x.c (the reviewed == committed case).
    # A8/A9: a real review state always carries a review_run_id; the gate now
    # fail-closes without one, so the helper supplies a default.
    blobs = {"src/x.c": _head_blob(root, "src/x.c")} if bind_head else {}
    (root / ".claude/state/last-codex-review.json").write_text(
        json.dumps({"received": received, "trigger_blobs": blobs,
                    "review_run_id": run_id}))


# ----------------------------------------------------- WIP gate internals (P4.5)
def test_wip_gate_accepts_committed_clean_with_received_review():
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d); _review(root, True)
        mod = _load(pathlib.Path(d) / "s.json", root)
        fails = mod._rollover_failures_wip(root, {"active": True})
        # committed-clean + received review -> the work-safety checks pass
        # (ignore any bg-job noise from the host; we assert the two we control).
        assert not any("WIP not committed" in f or "outstanding Codex review" in f
                       for f in fails), fails


def test_wip_gate_rejects_uncommitted_edit_midfixloop():
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d); _review(root, True)
        (root / "src/x.c").write_text("int x; int y;\n")   # UNCOMMITTED (mid-fix)
        mod = _load(pathlib.Path(d) / "s.json", root)
        fails = mod._rollover_failures_wip(root, {"active": True})
        assert any("WIP not committed" in f for f in fails), fails


def test_wip_gate_rejects_outstanding_review():
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d); _review(root, False)          # pending review
        mod = _load(pathlib.Path(d) / "s.json", root)
        fails = mod._rollover_failures_wip(root, {"active": True})
        assert any("outstanding Codex review" in f for f in fails), fails


def test_wip_gate_tolerates_autogen_and_never_checks_pushed():
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d); _review(root, True)
        (root / "docs/test-coverage/coverage.md").write_text("regen\n")  # ' M' auto-gen
        mod = _load(pathlib.Path(d) / "s.json", root)
        fails = mod._rollover_failures_wip(root, {"active": True})
        # auto-gen tolerated; unpushed / receipts / graph are NEVER checked here
        assert not any("WIP not committed" in f or "not pushed" in f
                       or "receipt" in f or "todo-graph" in f for f in fails), fails


def test_wip_gate_rejects_review_with_no_binding():          # F3
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d); _review(root, True, bind_head=False)  # no trigger_blobs
        mod = _load(pathlib.Path(d) / "s.json", root)
        fails = mod._rollover_failures_wip(root, {"active": True})
        assert any("content binding" in f for f in fails), fails


def test_wip_gate_rejects_review_stale_vs_head():            # F3
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        (root / ".claude/state/last-codex-review.json").write_text(json.dumps(
            {"received": True, "trigger_blobs": {"src/x.c": "0" * 40}}))
        mod = _load(pathlib.Path(d) / "s.json", root)
        fails = mod._rollover_failures_wip(root, {"active": True})
        assert any("stale relative to the WIP commit" in f for f in fails), fails


# ------------------------------------------------ rollover-wip firing verb (P4.6)
def _hint(root, on=True):
    (root / ".claude/state/rotate-hint.json").write_text(
        json.dumps({"count": 200, "hint": on}))


def _verb_mod(root, gate_fails=None, checkpoint_ok=True, state_extra=None,
              unpushed=1, ship_stamp=False, resolution_ok=True):
    mod = _load(root / ".claude/state/s.json", root)
    mod._rollover_failures_wip = lambda r, s: (gate_fails or [])
    mod._write_section_checkpoint = lambda r: checkpoint_ok    # F2
    mod._unpushed_count = lambda r: unpushed                   # A1 (default: WIP ahead)
    mod._head_adds_ship_stamp = lambda r: ship_stamp           # A4 (default: not ship)
    mod._review_resolution_valid = lambda r: (resolution_ok, "")  # A5 (default: OK)
    st = {"active": True, "phase": "SECTIONS", "file": "todo/T.md", "section_idx": 5}
    if state_extra:
        st.update(state_extra)
    mod.save_state(st)
    return mod


def test_rollover_wip_refused_without_hint():
    with tempfile.TemporaryDirectory() as d:
        mod = _verb_mod(_git_repo(d))
        assert mod.cli(["rollover-wip"]) == 1               # no hint -> refuse


def test_rollover_wip_verified_with_hint_and_clean_gate():
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _verb_mod(root)
        _hint(root, True)
        assert mod.cli(["rollover-wip"]) == 0
        st = mod.load_state()
        assert st["rollover"]["pending"] is True and st["rollover"]["wip"] is True
        assert not (root / ".claude/state/rotate-hint.json").exists()  # counter reset


def test_rollover_wip_refused_when_gate_fails():
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _verb_mod(root, gate_fails=["WIP not committed -- tree not clean"])
        _hint(root, True)
        assert mod.cli(["rollover-wip"]) == 1


def test_rollover_wip_refused_when_checkpoint_fails():        # F2
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _verb_mod(root, checkpoint_ok=False)
        _hint(root, True)
        assert mod.cli(["rollover-wip"]) == 1
        assert "rollover" not in mod.load_state()            # no authorization


def test_rollover_wip_refused_if_ship_rollover_refused():     # F1
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _verb_mod(root, state_extra={"rollover_refused":
                                           {"file": "todo/T.md", "section_idx": 5}})
        _hint(root, True)
        assert mod.cli(["rollover-wip"]) == 1
        # a WIP rotation must NOT launder / clear the P3.2 advance-block
        assert "rollover_refused" in mod.load_state()


def test_rollover_wip_refused_outside_sections():             # F1
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _verb_mod(root, state_extra={"phase": "FILE_CLOSE"})
        _hint(root, True)
        assert mod.cli(["rollover-wip"]) == 1


def test_rollover_wip_refused_when_nothing_unpushed():        # A1 post-ship guard
    # everything pushed (unpushed==0) -> a shipped section must not WIP-rotate via
    # the weaker gate; the full `rollover` (receipt/graph checks) is the exit.
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _verb_mod(root, unpushed=0)
        _hint(root, True)
        assert mod.cli(["rollover-wip"]) == 1
        assert "rollover" not in mod.load_state()             # no authorization


def test_rollover_wip_refused_when_upstream_undeterminable():  # A1 fail-closed
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _verb_mod(root, unpushed=None)                  # no upstream / git error
        _hint(root, True)
        assert mod.cli(["rollover-wip"]) == 1


def test_unpushed_count_none_without_upstream():              # A1 helper contract
    # a fresh repo with no upstream -> _unpushed_count returns None (not a crash).
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _load(root / ".claude/state/s.json", root)
        assert mod._unpushed_count(root) is None


# ---------------------------------------------- A4: reliable ship-stamp refusal
def test_rollover_wip_refused_on_ship_stamp_commit():        # A4 round-2
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _verb_mod(root, ship_stamp=True)               # HEAD is a stamp commit
        _hint(root, True)
        assert mod.cli(["rollover-wip"]) == 1
        assert "rollover" not in mod.load_state()


def test_head_adds_ship_stamp_detects_verified_stamp():      # A4 helper contract
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _load(root / ".claude/state/s.json", root)
        (root / "todo").mkdir()
        (root / "todo/T.md").write_text("## 1. X\n\n> **Verified:** 2026-07-14\n")
        subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
        subprocess.run(["git", "-C", str(root), "commit", "-qm", "ship stamp"],
                       check=True, capture_output=True)
        assert mod._head_adds_ship_stamp(root) is True
        # No upstream configured -> the range is undefined and the helper falls
        # back to HEAD alone, so a follow-up CODE commit hides the stamp. This is
        # the documented narrow fallback, NOT the production path (the runner
        # always has an upstream); the A7 test below covers the real one.
        (root / "src/x.c").write_text("int y;\n")
        subprocess.run(["git", "-C", str(root), "commit", "-qam", "wip"], check=True,
                       capture_output=True)
        assert mod._head_adds_ship_stamp(root) is False


def test_a7_stamp_seen_across_the_whole_unpushed_range():
    """A7 (HIGH, recorded 2026-07-14, closed 2026-07-30): the helper examined only
    HEAD, so a ship-stamp commit followed by an unpushed FIXUP left `unpushed>0`
    while HEAD no longer showed the stamp -- and a SHIPPED section took the weaker
    WIP rotation path. With an upstream present the cumulative `@{u}..HEAD` range
    is scanned, so the stamp is seen no matter what landed after it."""
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _load(root / ".claude/state/s.json", root)
        # Give the fixture a real upstream so the range is defined.
        bare = pathlib.Path(d) / "origin.git"
        subprocess.run(["git", "init", "-q", "--bare", str(bare)], check=True)
        subprocess.run(["git", "-C", str(root), "remote", "add", "origin", str(bare)],
                       check=True)
        subprocess.run(["git", "-C", str(root), "push", "-q", "-u", "origin", "HEAD"],
                       check=True, capture_output=True)
        assert mod._head_adds_ship_stamp(root) is False   # nothing unpushed yet

        (root / "todo").mkdir(exist_ok=True)
        (root / "todo/T.md").write_text("## 1. X\n\n> **Verified:** 2026-07-30\n")
        subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
        subprocess.run(["git", "-C", str(root), "commit", "-qm", "ship stamp"],
                       check=True, capture_output=True)
        assert mod._head_adds_ship_stamp(root) is True

        # The regression: an unpushed follow-up that is NOT itself a stamp.
        (root / "src").mkdir(exist_ok=True)
        (root / "src/x.c").write_text("int y;\n")
        subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
        subprocess.run(["git", "-C", str(root), "commit", "-qm", "fixup"],
                       check=True, capture_output=True)
        assert mod._head_adds_ship_stamp(root) is True, \
            "stamp must still be seen through a follow-up commit (A7)"


def test_a7_undeterminable_fails_closed():
    """A7's second half: the helper used to return False (== 'clean WIP') on any
    git error. A failure confined to THIS call left the unpushed count perfectly
    readable, so the pairing argument did not hold and the check was silently
    permissive. Unknown is now None, and `rollover-wip` refuses on it."""
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d) / "not-a-repo"      # git commands fail here
        root.mkdir()
        mod = _load(root / ".claude/state/s.json", root)
        assert mod._head_adds_ship_stamp(root) is None


# ------------------------------------ A5: review-resolution boundary + verb
def test_rollover_wip_refused_without_resolution_receipt():  # A5 round-2
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _verb_mod(root, resolution_ok=False)           # no green resolution
        _hint(root, True)
        assert mod.cli(["rollover-wip"]) == 1
        assert "rollover" not in mod.load_state()


def test_review_resolution_valid_head_bound():               # A5 helper contract
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _load(root / ".claude/state/s.json", root)
        p = root / ".claude/state/last-review-resolution.json"
        # absent -> refuse
        ok, why = mod._review_resolution_valid(root)
        assert not ok and "no review-resolution receipt" in why
        head = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                              capture_output=True, text=True).stdout.strip()
        _review(root, True, run_id="rev-1")
        p.write_text(json.dumps({"head": head, "review_run_id": "rev-1"}))
        assert mod._review_resolution_valid(root)[0] is True
        p.write_text(json.dumps({"head": "0" * 40,           # stale HEAD -> refuse
                                 "review_run_id": "rev-1"}))
        assert mod._review_resolution_valid(root)[0] is False


def test_a9_receipt_bound_to_the_review_round_not_just_head():
    """A9 (HIGH, recorded 2026-07-14, closed 2026-07-30): the receipt recorded
    `review_run_id` and nothing ever compared it, so a NEWER review round arriving
    at the SAME HEAD -- routine in a re-adversarial loop -- left the old receipt
    matching on `head` alone and certifying the section as resolved."""
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _load(root / ".claude/state/s.json", root)
        p = root / ".claude/state/last-review-resolution.json"
        head = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                              capture_output=True, text=True).stdout.strip()
        _review(root, True, run_id="rev-1")
        p.write_text(json.dumps({"head": head, "review_run_id": "rev-1"}))
        assert mod._review_resolution_valid(root)[0] is True

        # A NEWER review round at the SAME HEAD must invalidate the receipt.
        _review(root, True, run_id="rev-2")
        ok, why = mod._review_resolution_valid(root)
        assert not ok and "newer review round" in why, why

        # Fail-closed both ways: a receipt with no run id, and a review state
        # with no run id, are each refusals rather than silent passes.
        p.write_text(json.dumps({"head": head}))
        ok, why = mod._review_resolution_valid(root)
        assert not ok and "no review_run_id" in why, why
        p.write_text(json.dumps({"head": head, "review_run_id": "rev-2"}))
        (root / ".claude/state/last-codex-review.json").write_text(
            json.dumps({"received": True, "trigger_blobs": {}}))
        ok, why = mod._review_resolution_valid(root)
        assert not ok and "no review_run_id" in why, why


def test_a8_review_resolved_refuses_an_absent_or_unidentified_review():
    """A8 (HIGH, recorded 2026-07-14, closed 2026-07-30): `if rs and ...` skipped
    the received/binding checks entirely when the review state was empty, so a
    section with NO Codex review at all could mint a resolution receipt. And an
    absent `review_run_id` fell back to "", leaving A9 nothing to bind."""
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _load(root / ".claude/state/s.json", root)
        mod._build_suite_receipts_ok = lambda r: (True, "")
        mod.save_state({"active": True, "phase": "SECTIONS", "file": "todo/T.md",
                        "section_idx": 1})
        receipt = root / ".claude/state/last-review-resolution.json"

        # (1) no review state at all -> REFUSE, and write nothing.
        rs = root / ".claude/state/last-codex-review.json"
        if rs.exists():
            rs.unlink()
        assert mod.cli(["review-resolved"]) == 1
        assert not receipt.exists(), "must not mint a receipt with no review"

        # (2) present but empty -> REFUSE (the `if rs and` bypass).
        rs.write_text("{}")
        assert mod.cli(["review-resolved"]) == 1
        assert not receipt.exists()

        # (3) received + bound but NO review_run_id -> REFUSE (A9 needs the id).
        rs.write_text(json.dumps({"received": True,
                                  "trigger_blobs": {"src/x.c": _head_blob(root, "src/x.c")}}))
        assert mod.cli(["review-resolved"]) == 1
        assert not receipt.exists()

        # (4) the complete, honest case -> ACCEPT and record the run id.
        _review(root, True, run_id="rev-7")
        assert mod.cli(["review-resolved"]) == 0
        assert json.loads(receipt.read_text())["review_run_id"] == "rev-7"


def _resolved_mod(root, received=True, bind_head=True, receipts_ok=True):
    mod = _load(root / ".claude/state/s.json", root)
    mod._build_suite_receipts_ok = lambda r: (receipts_ok, "" if receipts_ok
                                              else "build receipt not content-valid")
    _review(root, received, bind_head=bind_head)
    mod.save_state({"active": True, "phase": "SECTIONS", "file": "todo/T.md",
                    "section_idx": 5})
    return mod


def test_review_resolved_writes_receipt_when_green_and_bound():   # A5 verb happy path
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _resolved_mod(root)
        assert mod.cli(["review-resolved"]) == 0
        rr = json.loads((root / ".claude/state/last-review-resolution.json").read_text())
        head = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                              capture_output=True, text=True).stdout.strip()
        assert rr["head"] == head


def test_review_resolved_refused_when_review_unbound():       # A5 verb: F3 binding
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _resolved_mod(root, bind_head=False)           # received but no binding
        assert mod.cli(["review-resolved"]) == 1
        assert not (root / ".claude/state/last-review-resolution.json").exists()


def test_review_resolved_refused_when_receipts_red():        # A5 verb: green gate
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _resolved_mod(root, receipts_ok=False)
        assert mod.cli(["review-resolved"]) == 1
        assert not (root / ".claude/state/last-review-resolution.json").exists()


def test_review_resolved_refused_outside_sections():         # A5 verb: mid-section only
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _resolved_mod(root)
        mod.save_state({"active": True, "phase": "FILE_CLOSE"})
        assert mod.cli(["review-resolved"]) == 1


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok {name}")
    print("PASS: p45-wip-rollover")
