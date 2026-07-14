#!/usr/bin/env python3
"""P1.2 / P1.3 review-gate fixes (surgical, fail-safe).

P1.3 -- the review BROKER detaches its review and returns instantly, so
`is_background_dispatch` must recognize it (its instant return is not a completed
review); `is_review_broker_dispatch` keeps it stampable (a completed broker leg
is real review proof, unlike a generic `task --background`).

P1.2 -- `_review_evidence` now (a) rejects a non-intersecting STALE record with a
precise message, and (b) when the single latest record's received:false was reset
by a newer multi-kind trigger, falls back to the received-review HISTORY and
accepts only if a genuinely-received review blob-covers the current staged tree.
Every acceptance stays content-bound (blob SHA), so nothing is loosened.
"""
import importlib.util
import json
import pathlib
import subprocess
import sys
import time
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent


def _load(name, rel):
    spec = importlib.util.spec_from_file_location(name, REPO / rel)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


cd = _load("cd", ".claude/hooks/_codex_dispatch.py")
scg = _load("scg", ".claude/hooks/section_commit_gate.py")
scg._ensure_crc()
crc = scg.crc


# ---------------------------------------------------------------- P1.3 detector
def test_broker_is_background_and_stampable():
    broker = ("bash scripts/overnight/review-broker-codex-dispatch.sh "
              "'[review-kind: adversarial] todo/x.md body'")
    assert cd.is_background_dispatch(broker) is True
    assert cd.is_review_broker_dispatch(broker) is True


def test_foreground_wrapper_unaffected():
    fg = "bash scripts/codex-dispatch.sh '[review-kind: design] todo/x.md body'"
    assert cd.is_background_dispatch(fg) is False
    assert cd.is_review_broker_dispatch(fg) is False


def test_generic_bg_task_is_background_but_not_broker():
    bg = "node /x/codex-companion.mjs task --background 'review-kind: design ...'"
    assert cd.is_background_dispatch(bg) is True
    assert cd.is_review_broker_dispatch(bg) is False


# ------------------------------------------------------------ P1.2 evidence
def _repo(d):
    root = pathlib.Path(d)
    (root / ".claude/state").mkdir(parents=True)
    (root / "src/kernel").mkdir(parents=True)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    (root / "src/kernel/x.c").write_text("int x(void){return 1;}\n")
    subprocess.run(["git", "-C", str(root), "add", "src/kernel/x.c"], check=True)
    return root


def _state(root, **kw):
    (root / ".claude/state/last-codex-review.json").write_text(json.dumps(kw))


def test_received_true_and_covered_accepts():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        staged = scg._staged_source_files(root)
        assert staged, "fixture must stage a source file"
        blobs = crc._staged_source_blobs(root, staged)
        _state(root, received=True, received_timestamp_ns=time.time_ns(),
               trigger="Bash(review)", trigger_files=staged,
               trigger_blobs=blobs, head_sha="abc123")
        ok, why = scg._review_evidence(root, staged)
        assert ok, why


def test_non_intersecting_record_is_stale_rejected():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        staged = scg._staged_source_files(root)
        _state(root, received=True, received_timestamp_ns=time.time_ns(),
               trigger="Bash(review)", trigger_files=["src/other/y.c"],
               trigger_blobs={"src/other/y.c": "100644:deadbeef"}, head_sha="abc123")
        ok, why = scg._review_evidence(root, staged)
        assert not ok and "STALE" in why, why


def test_received_false_but_history_covers_accepts():
    # The churn fix: a newer kind's trigger reset received:false, but a prior
    # RECEIVED review in the history covers the exact current staged content.
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        staged = scg._staged_source_files(root)
        blobs = crc._staged_source_blobs(root, staged)
        _state(root, received=False, received_timestamp_ns=None,
               trigger="Bash(perf)", trigger_files=staged,
               trigger_blobs=blobs, head_sha="abc123")
        (root / ".claude/state/codex-review-history.jsonl").write_text(
            json.dumps({"received": True, "received_timestamp_ns": time.time_ns(),
                        "trigger_blobs": blobs}) + "\n")
        ok, why = scg._review_evidence(root, staged)
        assert ok, why


def test_received_false_and_no_history_blocks():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        staged = scg._staged_source_files(root)
        blobs = crc._staged_source_blobs(root, staged)
        _state(root, received=False, received_timestamp_ns=None,
               trigger="Bash(perf)", trigger_files=staged,
               trigger_blobs=blobs, head_sha="abc123")
        ok, why = scg._review_evidence(root, staged)
        assert not ok and "was not" in why, why


# ------------------------------------------------------------ B3 worktree bind
def _repo_committed(d):
    root = pathlib.Path(d)
    (root / ".claude/state").mkdir(parents=True)
    (root / "src/kernel").mkdir(parents=True)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.email", "t@t"], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.name", "t"], check=True)
    (root / "src/kernel/x.c").write_text("int x(void){return 1;}\n")
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(root), "commit", "-qm", "init"], check=True)
    return root


def test_b3_review_over_unstaged_binds_then_gate_accepts_when_staged():
    # B3: reviews run over the UNSTAGED working tree (implement -> review ->
    # stage+commit). The old staged-only capture left trigger_files empty and
    # forced SKIP_REVIEW_HOOK. The working-tree capture binds the reviewed
    # content; once the model stages that exact content, the gate accepts.
    with tempfile.TemporaryDirectory() as d:
        root = _repo_committed(d)
        (root / "src/kernel/x.c").write_text("int x(void){return 2;}\n")  # UNSTAGED
        tf = crc._worktree_review_files(root)
        assert "src/kernel/x.c" in tf, tf
        tb = crc._worktree_source_blobs(root, tf)
        assert tb.get("src/kernel/x.c"), tb
        assert crc._staged_review_files(root) == [], "nothing staged yet"
        _state(root, received=True, received_timestamp_ns=time.time_ns(),
               trigger="Bash(review)", trigger_files=tf, trigger_blobs=tb, head_sha="h")
        subprocess.run(["git", "-C", str(root), "add", "src/kernel/x.c"], check=True)
        staged = scg._staged_source_files(root)
        ok, why = scg._review_evidence(root, staged)
        assert ok, why


def test_b3_edit_after_review_still_blocks():
    # Security property preserved: content committed must byte-match content
    # reviewed. An edit AFTER the review must still block.
    with tempfile.TemporaryDirectory() as d:
        root = _repo_committed(d)
        (root / "src/kernel/x.c").write_text("int x(void){return 2;}\n")
        tf = crc._worktree_review_files(root)
        tb = crc._worktree_source_blobs(root, tf)
        _state(root, received=True, received_timestamp_ns=time.time_ns(),
               trigger="Bash(review)", trigger_files=tf, trigger_blobs=tb, head_sha="h")
        (root / "src/kernel/x.c").write_text("int x(void){return 999;}\n")  # edit AFTER review
        subprocess.run(["git", "-C", str(root), "add", "src/kernel/x.c"], check=True)
        staged = scg._staged_source_files(root)
        ok, why = scg._review_evidence(root, staged)
        assert not ok, "post-review edit must still block (security property)"


def test_b3_find_and_fix_re_review_supersedes():
    # Find-and-fix: adversarial over A recorded, fix to A', re-adversarial over
    # A' recorded (overwrites last-codex-review), stage A' -> accept. The 2nd
    # canary SKIP was exactly this drift; the re-review's content must win.
    with tempfile.TemporaryDirectory() as d:
        root = _repo_committed(d)
        (root / "src/kernel/x.c").write_text("int x(void){return 2;}\n")  # A
        tfa = crc._worktree_review_files(root)
        _state(root, received=True, received_timestamp_ns=time.time_ns(),
               trigger="Bash(adversarial)", trigger_files=tfa,
               trigger_blobs=crc._worktree_source_blobs(root, tfa), head_sha="h")
        (root / "src/kernel/x.c").write_text("int x(void){return 3;}\n")  # fix -> A'
        tfb = crc._worktree_review_files(root)
        _state(root, received=True, received_timestamp_ns=time.time_ns(),
               trigger="Bash(re-adversarial)", trigger_files=tfb,
               trigger_blobs=crc._worktree_source_blobs(root, tfb), head_sha="h")
        subprocess.run(["git", "-C", str(root), "add", "src/kernel/x.c"], check=True)
        staged = scg._staged_source_files(root)
        ok, why = scg._review_evidence(root, staged)
        assert ok, why


# ------------------------------------------------------- C-RECV auto-receive
import shutil  # noqa: E402


def _crecv_repo(d):
    root = pathlib.Path(d)
    (root / ".claude/state").mkdir(parents=True)
    (root / ".claude/overnight/reviews").mkdir(parents=True)
    (root / "scripts/overnight").mkdir(parents=True)
    # the auto-receive clean-check loads the canonical review-envelope parser
    shutil.copy(REPO / "scripts/overnight/review-envelope.py",
                root / "scripts/overnight/review-envelope.py")
    return root


def _rec(root, kind="re-adversarial", received=False, ts=None):
    import time as _t
    if ts is None:
        ts = _t.time_ns()
    (root / ".claude/state/last-codex-review.json").write_text(json.dumps({
        "received": received, "review_kind": kind, "timestamp_ns": ts,
        "trigger_files": ["src/x.c"], "trigger_blobs": {"src/x.c": "abc"},
        "received_timestamp_ns": None}))


def _out_file(root, kind, body):
    p = root / f".claude/overnight/reviews/20260714-000000-{kind}.out"
    p.write_text(body)
    return p


def _poll(root, out_path):
    crc._maybe_auto_receive_clean(
        {"tool_name": "Bash", "tool_input": {"command":
         f"bash scripts/overnight/wait-for-codex-verdict.sh {out_path}"}}, root)
    return json.loads((root / ".claude/state/last-codex-review.json").read_text())


# A realistic CLEAN broker .out: structured approve verdict + zero findings, with
# the broker terminal sentinel as the FINAL line.
_CLEAN = ('{"verdict":"approve","summary":"no issues"}\n'
          'No material findings.\nTurn completed (rc=0)\n')


def test_crecv_clean_completion_auto_receives():
    with tempfile.TemporaryDirectory() as d:
        root = _crecv_repo(d); _rec(root, "re-adversarial")
        assert _poll(root, _out_file(root, "re-adversarial", _CLEAN))["received"] is True


def test_crecv_findings_not_received():
    with tempfile.TemporaryDirectory() as d:
        root = _crecv_repo(d); _rec(root, "re-adversarial")
        out = _out_file(root, "re-adversarial",
                        '{"verdict":"approve"}\n[HIGH] real bug at x.c:5\nTurn completed (rc=0)\n')
        assert _poll(root, out)["received"] is False


def test_crecv_crashed_not_received():
    with tempfile.TemporaryDirectory() as d:
        root = _crecv_repo(d); _rec(root, "re-adversarial")
        out = _out_file(root, "re-adversarial",
                        '{"verdict":"approve"}\nno material findings\nTurn completed (rc=1)\n')
        assert _poll(root, out)["received"] is False


def test_crecv_needs_attention_verdict_not_received():
    # F3: an explicit needs-attention structured verdict must NOT auto-receive
    # even with a clean-sounding "No material findings." line + rc0 sentinel.
    with tempfile.TemporaryDirectory() as d:
        root = _crecv_repo(d); _rec(root, "re-adversarial")
        out = _out_file(root, "re-adversarial",
                        '{"verdict":"approve"}\n{"verdict":"needs-attention"}\n'
                        'No material findings.\nTurn completed (rc=0)\n')
        assert _poll(root, out)["received"] is False


def test_crecv_embedded_sentinel_does_not_shadow_crash():
    # F2: the review BODY quotes "Turn completed (rc=0)" but the real terminal
    # line is a crash (rc=1) -> not clean (anchored to the FINAL line).
    with tempfile.TemporaryDirectory() as d:
        root = _crecv_repo(d); _rec(root, "re-adversarial")
        out = _out_file(root, "re-adversarial",
                        '{"verdict":"approve"}\nreviewer quotes Turn completed (rc=0) here\n'
                        'Turn completed (rc=1)\n')
        assert _poll(root, out)["received"] is False


def test_crecv_no_structured_verdict_not_received():
    # Free-text "Verdict: approve" with NO structured {"verdict"} -> not clean.
    with tempfile.TemporaryDirectory() as d:
        root = _crecv_repo(d); _rec(root, "re-adversarial")
        out = _out_file(root, "re-adversarial",
                        "Verdict: approve\nno material findings\nTurn completed (rc=0)\n")
        assert _poll(root, out)["received"] is False


def test_crecv_kind_mismatch_not_received():
    with tempfile.TemporaryDirectory() as d:
        root = _crecv_repo(d); _rec(root, "adversarial")   # record kind != out kind
        assert _poll(root, _out_file(root, "perf", _CLEAN))["received"] is False


def test_crecv_stale_same_kind_artifact_not_received():
    # F1: an OLD clean same-kind .out (mtime well before the record's dispatch)
    # must not complete the current (possibly finding-bearing) state.
    import os as _os, time as _t
    with tempfile.TemporaryDirectory() as d:
        root = _crecv_repo(d)
        now = _t.time_ns()
        _rec(root, "perf", ts=now)
        out = _out_file(root, "perf", _CLEAN)
        old = (now / 1e9) - 3600
        _os.utime(out, (old, old))
        assert _poll(root, out)["received"] is False


def test_crecv_empty_record_kind_not_received():
    # F1: an empty recorded kind must not act as a wildcard.
    with tempfile.TemporaryDirectory() as d:
        root = _crecv_repo(d); _rec(root, "")
        assert _poll(root, _out_file(root, "perf", _CLEAN))["received"] is False


# ------------------------------------------------------------ F1 attribution
def test_f1_active_section_wins_over_first_staged():
    # F1: with TODO-12 staged FIRST (reciprocal XREF) but TODO-22 the active
    # section, attribution must pick TODO-22, not first-staged TODO-12.
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        (root / ".claude/state/skill-progress.json").write_text(json.dumps(
            {"review-todo-section": {"todo_path": "todo/02-kernel-core/TODO-22-x.md",
                                     "compaction_orphaned": False}}))
        staged = ["todo/02-kernel-core/TODO-12-y.md",
                  "todo/02-kernel-core/TODO-22-x.md"]
        assert scg._attribute_review_todo(root, staged) == \
            "todo/02-kernel-core/TODO-22-x.md"


def test_f1_falls_back_to_first_staged_without_active():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        assert scg._attribute_review_todo(
            root, ["todo/a/TODO-5.md", "todo/a/TODO-6.md"]) == "todo/a/TODO-5.md"


def test_f1_orphaned_active_falls_back():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        (root / ".claude/state/skill-progress.json").write_text(json.dumps(
            {"review-todo-section": {"todo_path": "todo/a/TODO-22.md",
                                     "compaction_orphaned": True}}))
        assert scg._attribute_review_todo(root, ["todo/a/TODO-9.md"]) == "todo/a/TODO-9.md"


# --------------------------------------- B3: stamp-only SKIP preserves received
def _stamp_repo(d):
    root = pathlib.Path(d)
    (root / ".claude/state").mkdir(parents=True)
    (root / "todo").mkdir(parents=True)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.email", "t@t"], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.name", "t"], check=True)
    (root / "todo/T.md").write_text(
        "## 1. Section\n\n| Item | Done |\n| A | [x] |\n\nbody\n")
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(root), "commit", "-qm", "init"], check=True)
    return root


def _with_skip_env(fn):
    import os
    keys = ("SKIP_REVIEW_HOOK", "SKIP_REVIEW_HOOK_REASON")
    saved = {k: os.environ.get(k) for k in keys}
    os.environ["SKIP_REVIEW_HOOK"] = "1"
    os.environ["SKIP_REVIEW_HOOK_REASON"] = "review stamp commit is TODO-only, code already reviewed"
    try:
        return fn()
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


def test_b3_stamp_only_skip_preserves_received():
    # B3 (Canary #2, 2026-07-14): a stamp-only SKIP commit must NOT reset the
    # passed code review's received:true -- the immediately-following full
    # rollover + the receiving-review gate both need it to persist.
    with tempfile.TemporaryDirectory() as d:
        root = _stamp_repo(d)
        _state(root, received=True, received_timestamp_ns=time.time_ns(),
               trigger="Bash(review)", trigger_files=["src/kernel/x.c"],
               trigger_blobs={"src/kernel/x.c": "abc"}, head_sha="h")
        # stage a stamp-only change: add a blockquoted **Verified:** line, no source
        (root / "todo/T.md").write_text(
            "## 1. Section\n\n| Item | Done |\n| A | [x] |\n\nbody\n\n"
            "> **Verified:** 2026-07-14 test\n")
        subprocess.run(["git", "-C", str(root), "add", "todo/T.md"], check=True)
        assert scg._detect_signature(root)[0] == "stamp_only", \
            "fixture must present a stamp_only signature"
        rc = _with_skip_env(lambda: scg._evaluate(root, "git-hook", ""))
        assert rc == 0, f"stamp-only SKIP must be allowed, rc={rc}"
        after = json.loads(
            (root / ".claude/state/last-codex-review.json").read_text())
        assert after.get("received") is True, \
            "B3: a stamp-only SKIP must PRESERVE received:true (not reset it)"


def test_b3_general_skip_still_resets_received():
    # Regression guard: the fix is scoped to stamp_only. `_reset_review_state`
    # itself (used by the general-source SKIP branch) must still invalidate a
    # received:true so a code-SKIP cannot leave a reusable stale flag.
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        _state(root, received=True, received_timestamp_ns=time.time_ns(),
               trigger_blobs={"src/x.c": "abc"})
        scg._reset_review_state(root)
        after = json.loads(
            (root / ".claude/state/last-codex-review.json").read_text())
        assert after.get("received") is False and after.get("skipped_section_commit") is True


if __name__ == "__main__":
    test_b3_stamp_only_skip_preserves_received()
    test_b3_general_skip_still_resets_received()
    test_broker_is_background_and_stampable()
    test_foreground_wrapper_unaffected()
    test_generic_bg_task_is_background_but_not_broker()
    test_received_true_and_covered_accepts()
    test_non_intersecting_record_is_stale_rejected()
    test_received_false_but_history_covers_accepts()
    test_received_false_and_no_history_blocks()
    test_b3_review_over_unstaged_binds_then_gate_accepts_when_staged()
    test_b3_edit_after_review_still_blocks()
    test_b3_find_and_fix_re_review_supersedes()
    test_crecv_clean_completion_auto_receives()
    test_crecv_findings_not_received()
    test_crecv_crashed_not_received()
    test_crecv_needs_attention_verdict_not_received()
    test_crecv_embedded_sentinel_does_not_shadow_crash()
    test_crecv_no_structured_verdict_not_received()
    test_crecv_kind_mismatch_not_received()
    test_crecv_stale_same_kind_artifact_not_received()
    test_crecv_empty_record_kind_not_received()
    test_f1_active_section_wins_over_first_staged()
    test_f1_falls_back_to_first_staged_without_active()
    test_f1_orphaned_active_falls_back()
    print("PASS: review-gate P1.2/P1.3 + B3 + F1 + C-RECV")
