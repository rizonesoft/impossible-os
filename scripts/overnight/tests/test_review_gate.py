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
import re
import importlib.util
import json
import pathlib
import subprocess
import os
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


def test_partially_intersecting_record_is_rejected():
    """The dangerous foreign-record shape: a review that shares SOME staged
    files but not all.

    A wholly non-intersecting record is caught by the STALE check above. A
    PARTIAL one is not -- it passes the intersection test and is only stopped
    by the `uncovered` full-coverage check, which is what makes the binding a
    SUPERSET requirement rather than an intersection one. That distinction is
    load-bearing and easy to "simplify" away: the overnight-runner-improvements
    item that asked for this binding literally proposed "reject a record whose
    scope does not INTERSECT the staged files", which would have accepted this
    case and let a review that never saw most of the committed code satisfy the
    section-commit gate. Pinned so the superset requirement cannot be relaxed
    to intersection by a later cleanup.
    """
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        (root / "src/kernel").mkdir(parents=True)
        subprocess.run(["git", "init", "-q", str(root)], check=True)
        # TWO staged source files; the review will cover only the first.
        (root / "src/kernel/x.c").write_text("int x(void){return 1;}\n")
        (root / "src/kernel/unreviewed.c").write_text("int y(void){return 2;}\n")
        subprocess.run(["git", "-C", str(root), "add",
                        "src/kernel/x.c", "src/kernel/unreviewed.c"], check=True)
        staged = scg._staged_source_files(root)
        assert len(staged) >= 2, staged
        blobs = crc._staged_source_blobs(root, staged)
        covered = "src/kernel/x.c"
        _state(root, received=True, received_timestamp_ns=time.time_ns(),
               trigger="Bash(review of a DIFFERENT section)",
               trigger_files=[covered],
               trigger_blobs={covered: blobs[covered]}, head_sha="abc123")
        ok, why = scg._review_evidence(root, staged)
        assert not ok, "a review covering only part of the staged source must NOT pass"
        assert "uncovered source" in why, why
        assert "unreviewed.c" in why, f"the block must NAME the unreviewed file: {why}"


def _broker_fixture(d, todo, legs):
    """legs: {kind: "clean"|"crash"} -> manifest + artifacts under root."""
    root = pathlib.Path(d)
    (root / ".claude/overnight/reviews").mkdir(parents=True)
    (root / "scripts/overnight").mkdir(parents=True)
    import shutil
    shutil.copy(REPO / "scripts/overnight/review-envelope.py",
                root / "scripts/overnight/review-envelope.py")
    shutil.copy(REPO / "scripts/review_pin.py", root / "scripts/review_pin.py")  # the envelope's status reader
    shutil.copy(REPO / "scripts/overnight/review_result.py",
                root / "scripts/overnight/review_result.py")              # ... and the one status parser it uses
    ts = int(time.time())
    lines = []
    for kind, how in legs.items():
        log = root / f".claude/overnight/reviews/{kind}.out"
        log.write_text('{"verdict":"approve"}\nTurn completed (rc=0)\n' if how == "clean"
                       else "app-server exited unexpectedly\nTurn completed (rc=1)\n")
        lines.append(json.dumps({"ts": ts, "kind": kind, "todo": todo,
                                 "logFile": str(log)}))
    (root / ".claude/overnight/reviews/manifest.jsonl").write_text("\n".join(lines) + "\n")
    return root


def test_crashed_broker_leg_is_not_review_proof():
    """A broker leg is stamped at DISPATCH time (the `is_broker` carve-out in
    codex_review_completed), which is correct for attribution and wrong as
    completion proof. A leg that dies with rc=1 must not satisfy the
    four-dispatch check -- Codex crashes are common (16 in one measured run),
    so a dispatch-bound stamp would let a section ship on a review that never
    ran."""
    todo = "todo/x/TODO-1.md"
    with tempfile.TemporaryDirectory() as d:
        root = _broker_fixture(d, todo, {"adversarial": "clean",
                                         "consistency": "crash"})
        now = time.time_ns()
        entry = {"adversarial": now, "consistency": now, "perf": now}
        bad = scg._crashed_broker_legs(root, todo, entry)
        assert any("consistency" in b and "crashed" in b for b in bad), bad
        assert not any("adversarial" in b for b in bad), bad
        # perf has no manifest line -> a FOREGROUND dispatch, which is
        # synchronous and therefore already proven complete by its return.
        assert not any("perf" in b for b in bad), bad


def test_a_failed_foreground_leg_longer_than_five_minutes_still_blocks():
    # Its row carries the LAUNCH time; it is placed by when its status was published.
    todo = "todo/x/TODO-1.md"
    with tempfile.TemporaryDirectory() as d:
        root = _broker_fixture(d, todo, {"adversarial": "crash"})
        mf = root / ".claude/overnight/reviews/manifest.jsonl"
        row = json.loads(mf.read_text().splitlines()[0])
        row["ts"] = int(time.time()) - 360
        mf.write_text(json.dumps(row) + "\n")
        pathlib.Path(row["logFile"] + ".done").write_text("1\n")
        bad = scg._crashed_broker_legs(root, todo, {"adversarial": time.time_ns()})
        assert any("adversarial" in b for b in bad), bad


def test_a_long_foreground_leg_whose_status_was_never_published_blocks():
    todo = "todo/x/TODO-1.md"
    with tempfile.TemporaryDirectory() as d:
        root = _broker_fixture(d, todo, {"adversarial": "crash"})
        mf = root / ".claude/overnight/reviews/manifest.jsonl"
        row = json.loads(mf.read_text().splitlines()[0])
        row.update(ts=int(time.time()) - 360, done_file=True)
        mf.write_text(json.dumps(row) + "\n")
        bad = scg._crashed_broker_legs(root, todo, {"adversarial": time.time_ns()})
        assert any("adversarial" in b for b in bad), bad


def test_completion_check_fails_open_when_it_cannot_prove_a_crash():
    """A gate that cannot read its evidence must never block a commit."""
    todo = "todo/x/TODO-1.md"
    now = time.time_ns()
    entry = {"adversarial": now, "consistency": now, "perf": now}
    with tempfile.TemporaryDirectory() as d:
        root = _broker_fixture(d, todo, {"adversarial": "crash"})
        # different TODO -> no matching manifest entries at all
        assert scg._crashed_broker_legs(root, "todo/other/TODO-9.md", entry) == []
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)          # no manifest, no envelope module
        (root / ".claude/state").mkdir(parents=True)
        assert scg._crashed_broker_legs(root, todo, entry) == []


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


def _rec(root, kind="re-adversarial", received=False, ts=None, log_file=None):
    """A trigger record, holding the logFile its broker dispatch returned."""
    import time as _t
    if ts is None:
        ts = _t.time_ns()
    if log_file is None:
        log_file = str(root / f".claude/overnight/reviews/20260714-000000-{kind}.out")
    (root / ".claude/state/last-codex-review.json").write_text(json.dumps({
        "received": received, "review_kind": kind, "timestamp_ns": ts, "log_file": log_file,
        "trigger_files": ["src/x.c"], "trigger_blobs": {"src/x.c": "abc"},
        "received_timestamp_ns": None}))


def _out_file(root, kind, body, done=True):
    """A finished log carries the dispatcher's <log>.done, from its last terminal line."""
    p = root / f".claude/overnight/reviews/20260714-000000-{kind}.out"
    p.write_text(body)
    rcs = re.findall(r"^Turn completed \(rc=(\d+)\)\s*$", body, re.M)
    if done and rcs:
        pathlib.Path(str(p) + ".done").write_text(rcs[-1] + "\n")
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


def test_crecv_another_review_of_the_same_kind_never_completes_this_one():
    # B is the current review; A, an earlier same-kind dispatch, finishes clean.
    with tempfile.TemporaryDirectory() as d:
        root = _crecv_repo(d)
        _rec(root, "re-adversarial", log_file=str(root / ".claude/overnight/reviews/b-re-adversarial.out"))
        assert _poll(root, _out_file(root, "re-adversarial", _CLEAN))["received"] is False


def test_crecv_a_record_without_a_log_file_is_never_auto_received():
    with tempfile.TemporaryDirectory() as d:
        root = _crecv_repo(d); _rec(root, "re-adversarial", log_file="")
        assert _poll(root, _out_file(root, "re-adversarial", _CLEAN))["received"] is False


def test_crecv_a_newer_record_landing_during_the_check_is_kept():
    with tempfile.TemporaryDirectory() as d:
        root = _crecv_repo(d); _rec(root, "re-adversarial")
        out = _out_file(root, "re-adversarial", _CLEAN)
        state = root / ".claude/state/last-codex-review.json"
        newer = {"received": False, "review_kind": "round", "timestamp_ns": 1, "log_file": "/x/b.out"}
        real = crc._out_is_clean
        def racing(r, p):
            state.write_text(json.dumps(newer))      # another dispatch records B meanwhile
            return real(r, p)
        crc._out_is_clean = racing
        try:
            got = _poll(root, out)
        finally:
            crc._out_is_clean = real
        assert got == newer, got


def test_crecv_a_quoted_banner_is_not_a_refusal():
    quoted = {"tool_response": {"stdout": "finding: [codex-bg-dispatch] BLOCK -- x is wrong", "stderr": ""}}
    assert not crc._dispatch_was_refused(quoted)
    launched = {"tool_response": {"stdout": '{"logFile": "/x/a.out"}', "stderr": "[review-broker] BLOCK -- y"}}
    assert not crc._dispatch_was_refused(launched)


def test_crecv_every_broker_argv_refusal_is_recognised():
    import subprocess
    r = subprocess.run(["bash", str(REPO / "scripts/overnight/review-broker-codex-dispatch.sh"),
                        "[review-kind: adversarial] todo/x.md section 1 body", "extra"],
                       capture_output=True, text=True, timeout=30)
    assert r.returncode == 2, r
    assert crc._dispatch_was_refused({"tool_response": {"stdout": r.stdout, "stderr": r.stderr}}), r.stderr


def test_crecv_an_outage_refusal_is_a_refused_dispatch():
    for msg in ("[review-broker] BLOCK -- REFUSED, the Codex backend looks DOWN: x",
                "[codex-bg-dispatch] BLOCK -- first nonblank prompt line must start"):
        assert crc._dispatch_was_refused({"tool_response": {"stderr": msg}}), msg


def test_crecv_a_live_log_is_never_auto_received():
    # A live review whose latest line looks terminal has no <log>.done yet.
    with tempfile.TemporaryDirectory() as d:
        root = _crecv_repo(d); _rec(root, "re-adversarial")
        out = _out_file(root, "re-adversarial", _CLEAN, done=False)
        assert _poll(root, out)["received"] is False


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
    test_partially_intersecting_record_is_rejected()
    test_crashed_broker_leg_is_not_review_proof()
    test_completion_check_fails_open_when_it_cannot_prove_a_crash()
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


def test_a_stamp_diff_that_cannot_be_read_is_unknown_never_no_section():
    """The staged list is read, the stamp file's copies are not (the batch read times out): the commit is
    "unknown" (refused), never read as a commit with no section in it."""
    with tempfile.TemporaryDirectory() as d:
        root = _stamp_repo(d)
        (root / "todo/T.md").write_text(
            "## 1. Section\n\n| Item | Done |\n| A | [x] |\n\nbody\n\n"
            "> **Verified:** 2026-07-14 test\n")
        subprocess.run(["git", "-C", str(root), "add", "todo/T.md"], check=True)
        real = scg._proc.run

        def failing(cmd, *a, **k):
            if cmd[:2] == ["git", "cat-file"]:
                raise scg.subprocess.TimeoutExpired(cmd, 5)
            return real(cmd, *a, **k)
        scg._proc.run = failing
        try:
            assert scg._detect_signature(root)[0] == "unknown"
        finally:
            scg._proc.run = real


def test_stamp_detection_starts_two_git_processes_however_many_todos_change():
    """Forty TODOs, each with a fresh stamp: the changed ones come from the staged listing and every copy is
    read in one batch, so detection starts two git processes (the listing and the batch), not three per TODO,
    and still finds every stamp."""
    with tempfile.TemporaryDirectory() as d:
        root = _stamp_repo(d)
        names = [f"todo/S{n}.md" for n in range(40)]
        for name in names:
            (root / name).write_text("## 1. Section\n\nbody\n")
        subprocess.run(["git", "-C", str(root), "add", *names], check=True)
        subprocess.run(["git", "-C", str(root), "commit", "-qm", "s", "--no-verify"], check=True)
        for name in names:
            (root / name).write_text("## 1. Section\n\nbody\n\n> **Verified:** 2026-07-14 test\n")
        subprocess.run(["git", "-C", str(root), "add", *names], check=True)
        real, started = scg._proc.run, []
        scg._proc.run = lambda cmd, *a, **k: started.append(cmd) or real(cmd, *a, **k)
        try:
            scg._staged_paths(root, fresh=True)
            started.clear()
            scg._staged_listing.pop(str(root), None)
            assert sorted(scg._stamped_todos(root, names)) == sorted(names)
        finally:
            scg._proc.run = real
        assert len(started) <= 2, started


def test_the_readme_comparison_reads_stored_objects():
    """A replacement ref that makes the two README versions look alike does not exempt a hand-written change."""
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        subprocess.run(["git", "init", "-q", str(root)], check=True)
        g = lambda *a: subprocess.run(["git", "-C", str(root), *a], check=True, capture_output=True,
                                      text=True).stdout.strip()
        g("config", "user.email", "t@example.invalid")
        g("config", "user.name", "t")
        (root / "README.md").write_text("old text\n")
        g("add", "README.md")
        g("commit", "-qm", "a")
        old = g("rev-parse", "HEAD:README.md")
        (root / "README.md").write_text("new hand-written text\n")
        g("add", "README.md")
        g("commit", "-qm", "b")
        new = g("rev-parse", "HEAD:README.md")
        g("replace", old, new)
        assert scg._readme_only_regenerated(root, "HEAD~1:README.md", "HEAD:README.md") is False


def _flip_repo(d):
    root = _stamp_repo(d)
    (root / "todo/T.md").write_text(
        "## 1. Section\n\n| Section | Status |\n| 1 | [x] |\n| 2 | [x] |\n\nbody\n")
    (root / "src").mkdir()
    (root / "src/a.c").write_text("int a;\n")
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    return root


def test_the_staged_todo_is_read_once_however_many_rows_change():
    """Every changed done row needs the table's header; the staged TODO is read once for all of them."""
    with tempfile.TemporaryDirectory() as d:
        root = _flip_repo(d)
        real = scg._proc.check_output
        shows = []

        def counting(cmd, *a, **k):
            if cmd[:2] == ["git", "show"] and cmd[2] == ":todo/T.md":
                shows.append(cmd)
            return real(cmd, *a, **k)
        scg._proc.check_output = counting
        try:
            scg._detect_signature(root)
        finally:
            scg._proc.check_output = real
        assert len(shows) == 1, shows


def test_a_staged_todo_that_cannot_be_read_is_unknown():
    """The read the table header needs fails: the commit is unknown (refused), never judged without it."""
    with tempfile.TemporaryDirectory() as d:
        root = _flip_repo(d)
        real = scg._proc.check_output

        def failing(cmd, *a, **k):
            if cmd[:2] == ["git", "show"] and cmd[2] == ":todo/T.md":
                raise scg.subprocess.TimeoutExpired(cmd, 5)
            return real(cmd, *a, **k)
        scg._proc.check_output = failing
        try:
            assert scg._detect_signature(root)[0] == "unknown"
        finally:
            scg._proc.check_output = real


def test_a_staged_listing_that_fails_is_unknown_not_a_plain_commit():
    """A stamp-only commit whose staged path list cannot be read is unknown (refused), never read as a
    commit with nothing staged."""
    with tempfile.TemporaryDirectory() as d:
        root = _stamp_repo(d)
        (root / "todo/T.md").write_text((root / "todo/T.md").read_text() + "\n> **Verified:** 2026-11-22\n")
        subprocess.run(["git", "-C", str(root), "add", "todo/T.md"], check=True)
        real = scg._proc.check_output

        def failing(cmd, *a, **k):
            if cmd[:3] == ["git", "diff", "--cached"] and ("--name-only" in cmd or "--name-status" in cmd):
                raise scg.subprocess.TimeoutExpired(cmd, 3)
            return real(cmd, *a, **k)
        scg._proc.check_output = failing
        try:
            assert scg._detect_signature(root)[0] == "unknown"
        finally:
            scg._proc.check_output = real


def _two_sections(root, a_stamp, b_stamp):
    (root / "todo/T.md").write_text(
        "## 1. One\n\nbody\n" + (a_stamp + "\n" if a_stamp else "")
        + "\n## 2. Two\n\nbody\n" + (b_stamp + "\n" if b_stamp else ""))


def test_a_stamp_removed_in_one_section_never_balances_a_new_stamp_in_another():
    """Reopening section 1 (its stamp removed) while stamping section 2 still counts section 2's stamp as
    new; an in-place update of one section's stamp does not."""
    with tempfile.TemporaryDirectory() as d:
        root = _stamp_repo(d)
        _two_sections(root, "> **Verified:** 2026-11-20", "")
        subprocess.run(["git", "-C", str(root), "commit", "-qam", "one stamped"], check=True)
        _two_sections(root, "", "> **Verified:** 2026-11-22")
        subprocess.run(["git", "-C", str(root), "add", "todo/T.md"], check=True)
        assert scg._stamped_todos(root, ["todo/T.md"]) == ["todo/T.md"]
        _two_sections(root, "> **Verified:** 2026-11-22 (updated)", "")
        subprocess.run(["git", "-C", str(root), "add", "todo/T.md"], check=True)
        scg._staged_paths(root, fresh=True)            # a new index state is a new detection
        assert scg._stamped_todos(root, ["todo/T.md"]) == []


def test_the_gate_starts_processes_only_through_its_runner():
    """Every process the gate starts goes through its bounded runner, so none has a wait of its own."""
    import re
    text = (REPO / ".claude/hooks/section_commit_gate.py").read_text()
    direct = re.findall(r"subprocess\.(?:run|check_output|check_call|call|Popen)\(", text)
    assert direct == ["subprocess.Popen("], direct     # the one inside the runner


def test_a_timed_out_group_is_ended_within_the_hook_deadline():
    """A git read that outlives its time, with a child holding its pipe, is ended and the call returns
    within a bounded time of its timeout."""
    began = time.monotonic()
    try:
        scg._proc.run(["sh", "-c", "sleep 30 & sleep 30"], capture_output=True, timeout=0.5)
    except scg.subprocess.TimeoutExpired:
        pass
    else:
        raise AssertionError("no timeout")
    assert time.monotonic() - began < 0.5 + 1.0


def test_a_process_start_that_takes_the_time_left_is_a_timeout():
    """The runner's budget covers the start too: a start (exec) that takes longer than the time left returns
    a timeout, never a success first seen after the deadline."""
    real = scg.subprocess.Popen

    def slow(*a, **k):
        time.sleep(0.18)
        return real(*a, **k)
    saved = list(scg._hook_deadline)
    scg._hook_deadline[:] = [time.monotonic() + 0.1]
    scg.subprocess.Popen = slow
    try:
        scg._proc.run(["true"], timeout=5)
    except scg.subprocess.TimeoutExpired:
        pass
    else:
        raise AssertionError("a result seen after the deadline was returned")
    finally:
        scg.subprocess.Popen = real
        scg._hook_deadline[:] = saved


def test_the_hook_refuses_and_exits_when_its_deadline_passes_with_no_verdict():
    """Whatever the hook is blocked in (here a check that never returns), its deadline ends it with a
    refusal: exit 2 shortly after the budget."""
    code = ("import sys, time; sys.path.insert(0, %r); import section_commit_gate as g; "
            "sys.exit(g._with_deadline(0.3, 'test', lambda: time.sleep(30)))" % str(REPO / ".claude/hooks"))
    began = time.monotonic()
    r = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, timeout=20)
    assert r.returncode == 2, (r.returncode, r.stderr)
    assert time.monotonic() - began < 0.3 + scg._WATCHDOG_GRACE + 2


def test_a_root_lookup_that_fails_in_the_commit_hook_refuses():
    """Inside a commit, an unreadable repository root is a failed read: the pre-commit gate refuses."""
    with tempfile.TemporaryDirectory() as d:
        root = _stamp_repo(d)
        bindir = pathlib.Path(d) / "bin"
        bindir.mkdir()
        real = subprocess.run(["sh", "-c", "command -v git"], capture_output=True, text=True).stdout.strip()
        (bindir / "git").write_text('#!/bin/sh\ncase " $* " in *" --show-toplevel "*) exec sleep 30;; esac\n'
                                    f'exec {real} "$@"\n')
        (bindir / "git").chmod(0o755)
        env = {**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}"}
        r = subprocess.run([sys.executable, str(REPO / ".claude/hooks/section_commit_gate.py"), "--git-hook-mode"],
                           capture_output=True, text=True, timeout=30, cwd=str(root), env=env)
        assert r.returncode == 2 and "root could not be read" in r.stderr, (r.returncode, r.stderr[-800:])


def test_a_heading_inserted_above_an_unchanged_stamp_makes_that_stamp_fresh():
    """Splitting a section by inserting a heading above its stamp gives the new section a stamp HEAD's copy
    of that section lacked: it is fresh, and the evidence check owes it its own receipts."""
    with tempfile.TemporaryDirectory() as d:
        root = _stamp_repo(d)
        (root / "todo/T.md").write_text("## 1. One\n\nbody\n\n> **Verified:** 2026-11-20\n")
        subprocess.run(["git", "-C", str(root), "commit", "-qam", "stamped"], check=True)
        (root / "todo/T.md").write_text("## 1. One\n\nbody\n\n## 2. Two\n\n> **Verified:** 2026-11-20\n")
        subprocess.run(["git", "-C", str(root), "add", "todo/T.md"], check=True)
        assert scg._stamped_todos(root, ["todo/T.md"]) == ["todo/T.md"]
        assert scg._stamped_sections(root, "todo/T.md") == {"2"}


def test_an_updated_stamp_beside_a_fresh_one_owes_only_the_fresh_section():
    """Detection and evidence share one rule: updating section 1's stamp in place while stamping section 2
    owes receipts for section 2 alone."""
    with tempfile.TemporaryDirectory() as d:
        root = _stamp_repo(d)
        _two_sections(root, "> **Verified:** 2026-11-20 (3 rounds)", "")
        subprocess.run(["git", "-C", str(root), "commit", "-qam", "one stamped"], check=True)
        _two_sections(root, "> **Verified:** 2026-11-20 (4 rounds)", "> **Verified:** 2026-11-22")
        subprocess.run(["git", "-C", str(root), "add", "todo/T.md"], check=True)
        assert scg._stamped_sections(root, "todo/T.md") == {"2"}


def test_the_watchdog_writes_nothing_before_it_exits():
    """The watchdog's only act is the exit: a write first could block on a stalled log file or a pipe another
    writer filled between a poll and the write, and hold the refusal with the gate."""
    import inspect
    body = inspect.getsource(scg._deadline_exit)
    code = "\n".join(line for line in body.splitlines() if not line.strip().startswith(("#", '"""')))
    assert "os._exit(2)" in code and "write" not in code.split('"""')[-1] and "print" not in code.split('"""')[-1]


def test_a_deleted_todo_is_an_empty_document_not_a_failed_read():
    """A commit that deletes a tracked TODO is judged (no stamp is fresh in it), never unknown; a fresh stamp
    in another TODO beside it is still found."""
    with tempfile.TemporaryDirectory() as d:
        root = _stamp_repo(d)
        (root / "todo/U.md").write_text("## 1. One\n\nbody\n")
        subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
        subprocess.run(["git", "-C", str(root), "commit", "-qm", "two"], check=True)
        subprocess.run(["git", "-C", str(root), "rm", "-q", "todo/T.md"], check=True)
        assert scg._detect_signature(root)[0] != "unknown"
        assert scg._stamped_todos(root, ["todo/T.md"]) == []
        (root / "todo/U.md").write_text("## 1. One\n\nbody\n\n> **Verified:** 2026-11-22\n")
        subprocess.run(["git", "-C", str(root), "add", "todo/U.md"], check=True)
        assert scg._detect_signature(root)[0] != "unknown"
        assert scg._stamped_todos(root, ["todo/T.md", "todo/U.md"]) == ["todo/U.md"]


def test_one_detection_reads_each_staged_todo_once_across_flip_and_stamp_detection():
    """A done row's description edited beside a source change: the flip detector reads the staged TODO to
    place the row, and the stamp check reuses that read."""
    with tempfile.TemporaryDirectory() as d:
        root = _flip_repo(d)
        subprocess.run(["git", "-C", str(root), "commit", "-qm", "flip"], check=True)
        (root / "todo/T.md").write_text((root / "todo/T.md").read_text().replace("| 2 | [x] |", "| 2 (edited) | [x] |"))
        (root / "src/a.c").write_text("int a2;\n")
        subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
        real = scg._proc.check_output
        shows = []

        def counting(cmd, *a, **k):
            if cmd[:2] == ["git", "show"] and cmd[2] == ":todo/T.md":
                shows.append(cmd)
            return real(cmd, *a, **k)
        scg._proc.check_output = counting
        try:
            scg._detect_signature(root)
            scg._stamped_todos(root, ["todo/T.md"])
        finally:
            scg._proc.check_output = real
        assert len(shows) <= 1, shows



def test_a_start_stalled_past_the_hook_deadline_never_runs_its_command():
    """The gate's start of a command stalls in Popen (storage holding the exec) past what the hook has left:
    run() refuses at the deadline, and when the stalled start completes afterwards the command never runs."""
    with tempfile.TemporaryDirectory() as d:
        marker = pathlib.Path(d) / "ran"
        real_popen = scg.subprocess.Popen

        def slow_popen(*a, **k):
            time.sleep(1.0)
            return real_popen(*a, **k)
        real_left = scg._hook_left
        scg.subprocess.Popen = slow_popen
        scg._hook_left = lambda timeout: 0.3
        try:
            began = time.monotonic()
            try:
                scg._proc.run(["touch", str(marker)], timeout=5)
                raise AssertionError("a start past the deadline was answered")
            except scg.subprocess.TimeoutExpired:
                pass
            assert time.monotonic() - began < 0.8, time.monotonic() - began
        finally:
            scg._hook_left = real_left
        time.sleep(1.5)                                   # the stalled start completes, its gate closed
        scg.subprocess.Popen = real_popen
        assert not marker.exists()



def test_a_suspend_past_the_hook_deadline_refuses_and_starts_nothing():
    """Elapsed time jumps past the hook's budget (a suspend) while monotonic time stays inside it: the gate
    starts no further command and its verdict is a refusal, never the success the checks reached."""
    with tempfile.TemporaryDirectory() as d:
        marker = pathlib.Path(d) / "ran"
        real_clock, skew = scg._clock, [0.0]
        scg._clock = lambda: real_clock() + skew[0]
        started = []

        def checks():
            skew[0] = 120.0                                   # the host slept for two minutes
            try:
                scg._proc.run(["touch", str(marker)], timeout=5)
                started.append(True)
            except scg.subprocess.TimeoutExpired:
                started.append(False)
            return 0
        try:
            assert scg._with_deadline(30.0, "test", checks) == 2
        finally:
            scg._clock = real_clock
        assert started == [False] and not marker.exists(), started



def test_the_watchdog_ends_a_command_it_started_before_it_refuses():
    """A suspend lands after the gate released a command (the command has announced itself): the watchdog
    refuses (exit 2) and the command's group is ended with it, never left running in its own session."""
    import os
    import sys
    import textwrap
    with tempfile.TemporaryDirectory() as d:
        pidfile = pathlib.Path(d) / "child.pid"
        script = textwrap.dedent(f"""
            import pathlib, sys, threading, time
            sys.path.insert(0, {str(REPO / '.claude/hooks')!r})
            import section_commit_gate as scg
            real, skew = scg._clock, [0.0]
            scg._clock = lambda: real() + skew[0]
            pidfile = pathlib.Path({str(pidfile)!r})

            def jump():
                while not (pidfile.exists() and pidfile.read_text().strip()):
                    time.sleep(0.02)
                skew[0] = 600.0
            threading.Thread(target=jump, daemon=True).start()

            def checks():
                try:
                    scg._proc.run(["bash", "-c", "echo $$ > {pidfile}; exec sleep 30"], timeout=60)
                except Exception:
                    pass
                return 0
            sys.exit(scg._with_deadline(30.0, "test", checks))
        """)
        began = time.monotonic()
        r = subprocess.run([sys.executable, "-c", script], capture_output=True, text=True, timeout=30)
        assert r.returncode == 2 and time.monotonic() - began < 5, (r, time.monotonic() - began)
        pid = int(pidfile.read_text().split()[0])
        end = time.monotonic() + 3
        while time.monotonic() < end:
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.05)
        else:
            raise AssertionError(f"command {pid} outlived the watchdog's refusal")



def test_the_round_lookup_asks_the_manifest_for_the_round_alone(tmp_path, monkeypatch):
    """The round check stops at the newest round row instead of reading back to the oldest of five kinds:
    _pass_receipts hands the kinds it is asked for to the one reader, and the round check asks for round."""
    import sys
    sys.path.insert(0, str(REPO / "scripts"))
    import review_pin
    asked = []
    monkeypatch.setattr(review_pin, "newest_receipts",
                        lambda manifest, todo, section, kinds, **k: (asked.append(kinds), ({}, True))[1])
    scg._pass_receipts(tmp_path, "todo/x.md", "9", ("round",))
    scg._pass_receipts(tmp_path, "todo/x.md", "9")
    assert asked == [("round",), tuple(scg._PASS_KINDS)], asked
    src = pathlib.Path(scg.__file__).read_text()
    assert '_pass_receipts(root, todo, required_section, ("round",))' in src



def test_a_suspend_during_a_gate_command_ends_its_wait_on_resume(monkeypatch):
    """A gate command stalls and the host sleeps past its whole budget (the elapsed-time clock jumps, the
    monotonic one does not): the wait gives up within a slice of the resume, never after the budget again."""
    import threading
    import time

    import pytest
    real, skew = scg._clock, [0.0]
    monkeypatch.setattr(scg, "_clock", lambda: real() + skew[0])
    threading.Timer(0.5, lambda: skew.__setitem__(0, 120.0)).start()
    began = time.monotonic()
    with pytest.raises(subprocess.TimeoutExpired):
        scg._proc.run(["sleep", "30"], timeout=10)
    assert time.monotonic() - began < 3, time.monotonic() - began



def test_a_gate_command_is_fed_its_whole_input_whatever_the_wait_slices(monkeypatch):
    """Input larger than a pipe, read only after the first wait slice has expired: the command gets it all,
    and no wait is ever left with the command's stdin open and no input to write (on CPython 3.12 such a
    retry stops the feed, and the command waits for EOF until the gate ends it)."""
    import time

    real, calls = subprocess.Popen.communicate, []

    def communicate(self, input=None, timeout=None):
        calls.append(input is None and self.stdin is not None and not self.stdin.closed)
        return real(self, input, timeout)
    monkeypatch.setattr(subprocess.Popen, "communicate", communicate)
    data = b"x" * (256 << 10)
    began = time.monotonic()
    r = scg._proc.run(["sh", "-c", "sleep 1.5; wc -c"], input=data, capture_output=True, timeout=10)
    assert r.returncode == 0 and r.stdout.strip() == str(len(data)).encode(), r
    assert time.monotonic() - began < 5
    assert not any(calls), calls
