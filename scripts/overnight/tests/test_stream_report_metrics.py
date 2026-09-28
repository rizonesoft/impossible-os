#!/usr/bin/env python3
"""Contract test for stream-report.py per-section metrics sidecar."""
import json, os, subprocess, sys, tempfile, pathlib

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE.parent / "stream-report.py"


def _run(events, metrics_path, extra_env=None):
    payload = "".join(json.dumps(e) + "\n" for e in events)
    env = {**os.environ, "OVERNIGHT_METRICS_FILE": str(metrics_path)}
    if extra_env:
        env.update(extra_env)
    proc = subprocess.run(
        [sys.executable, str(SCRIPT)],
        input=payload, text=True, capture_output=True, env=env,
    )
    assert proc.returncode == 0, proc.stderr
    return proc.stdout


def _assistant(usage=None, tools=None, model=None, parent=None, msg_id=None):
    content = []
    if tools:
        for name, inp in tools:
            content.append({"type": "tool_use", "name": name, "input": inp})
    msg = {"content": content}
    if usage is not None:
        msg["usage"] = usage
    if model is not None:
        msg["model"] = model
    if msg_id is not None:
        msg["id"] = msg_id
    event = {"type": "assistant", "message": msg}
    if parent is not None:
        event["parent_tool_use_id"] = parent
    return event


def _usage(i=0, o=0, cr=0, cc=0):
    return {"input_tokens": i, "output_tokens": o,
            "cache_read_input_tokens": cr, "cache_creation_input_tokens": cc}


def test_two_sections_split_on_progress():
    with tempfile.TemporaryDirectory() as d:
        mp = pathlib.Path(d) / "m.jsonl"
        events = [
            _assistant(_usage(o=100), [("Read", {"file_path": "a.c"})]),
            _assistant(_usage(o=50), [("Task", {"subagent_type": "kernel-explorer"})]),
            _assistant(_usage(o=10), [("Grep", {"pattern": "x"})]),
            # section boundary: the progress guard call
            _assistant(_usage(o=5), [("Bash", {"command": "python3 .claude/hooks/run_phase_guard.py progress"})]),
            _assistant(_usage(o=200), [("mcp__lsp-bridge__definition", {})]),
            {"type": "result", "result": "done", "usage": _usage(o=9999)},
        ]
        _run(events, mp)
        recs = [json.loads(l) for l in mp.read_text().splitlines() if l.strip()]
        assert len(recs) == 2, recs
        s0, s1 = recs
        assert s0["section_index"] == 0 and s0["marker"] == "progress"
        assert s0["output_tokens"] == 165          # 100+50+10+5
        assert s0["agent_dispatches"] == 1
        assert s0["grep_calls"] == 1
        assert s0["lsp_calls"] == 0
        assert s1["section_index"] == 1 and s1["marker"] == "final"
        assert s1["output_tokens"] == 200
        assert s1["lsp_calls"] == 1


def test_model_confirmed_matches_expected_no_warning():
    with tempfile.TemporaryDirectory() as d:
        mp = pathlib.Path(d) / "m.jsonl"
        events = [
            _assistant(_usage(o=1), model="claude-opus-4-1-20250805"),
            {"type": "result", "result": "done", "usage": _usage(o=1)},
        ]
        out = _run(events, mp, {"OVERNIGHT_MODEL": "opus"})
        assert "model confirmed: claude-opus-4-1-20250805" in out
        assert "WARNING" not in out
        recs = [json.loads(l) for l in mp.read_text().splitlines() if l.strip()]
        assert recs[-1]["model"] == "claude-opus-4-1-20250805"


def test_model_mismatch_warns():
    with tempfile.TemporaryDirectory() as d:
        mp = pathlib.Path(d) / "m.jsonl"
        events = [_assistant(_usage(o=1), model="claude-sonnet-5-20260101")]
        out = _run(events, mp, {"OVERNIGHT_MODEL": "opus"})
        assert "WARNING" in out and "expected 'opus'" in out


def test_model_change_mid_run_logged():
    with tempfile.TemporaryDirectory() as d:
        mp = pathlib.Path(d) / "m.jsonl"
        events = [
            _assistant(_usage(o=1), model="claude-opus-4-1-20250805"),
            _assistant(_usage(o=1), model="claude-sonnet-5-20260101"),
        ]
        out = _run(events, mp, {"OVERNIGHT_MODEL": "opus"})
        assert "model changed: claude-opus-4-1-20250805 -> claude-sonnet-5-20260101" in out


def test_sidechain_model_ignored():
    # Subagent sidechain events (parent_tool_use_id set) run the analyst
    # fleet's model; they must not read as a main-loop fallback flip.
    with tempfile.TemporaryDirectory() as d:
        mp = pathlib.Path(d) / "m.jsonl"
        events = [
            _assistant(_usage(o=1), model="claude-opus-4-1-20250805"),
            _assistant(_usage(o=1), model="claude-sonnet-5-20260101",
                       parent="toolu_01subagent"),
            _assistant(_usage(o=1), model="claude-opus-4-1-20250805"),
            {"type": "result", "result": "done", "usage": _usage(o=1)},
        ]
        out = _run(events, mp, {"OVERNIGHT_MODEL": "opus"})
        assert "model confirmed: claude-opus-4-1-20250805" in out
        assert "model changed" not in out
        recs = [json.loads(l) for l in mp.read_text().splitlines() if l.strip()]
        assert recs[-1]["model"] == "claude-opus-4-1-20250805"
        # sidechain usage lands in its own bucket, not the main-loop totals
        assert sum(r["output_tokens"] for r in recs) == 2
        assert sum(r["sidechain_output_tokens"] for r in recs) == 1
        assert sum(r["sidechain_turns"] for r in recs) == 1


def test_duplicate_message_id_counts_once():
    # The CLI emits one stream event per content block, each repeating the
    # same message envelope (id + usage). One id = one API request.
    with tempfile.TemporaryDirectory() as d:
        mp = pathlib.Path(d) / "m.jsonl"
        events = [
            _assistant(_usage(o=10), [("Read", {"file_path": "a.c"})], msg_id="msg_A"),
            _assistant(_usage(o=10), [("Grep", {"pattern": "x"})], msg_id="msg_A"),
            _assistant(_usage(o=10), msg_id="msg_A"),
            _assistant(_usage(o=5), msg_id="msg_B"),
            {"type": "result", "result": "done", "usage": _usage(o=1)},
        ]
        _run(events, mp)
        recs = [json.loads(l) for l in mp.read_text().splitlines() if l.strip()]
        assert recs[-1]["turns"] == 2, recs
        assert recs[-1]["output_tokens"] == 15, recs
        # tool counting is per-event (blocks are distinct tools), unaffected
        assert recs[-1]["grep_calls"] == 1


def test_model_inherit_skips_validation():
    with tempfile.TemporaryDirectory() as d:
        mp = pathlib.Path(d) / "m.jsonl"
        events = [_assistant(_usage(o=1), model="claude-sonnet-5-20260101")]
        out = _run(events, mp, {"OVERNIGHT_MODEL": "inherit"})
        assert "WARNING" not in out
        assert "model confirmed: claude-sonnet-5-20260101" in out


def test_main_vs_sidechain_tool_attribution():
    # The whole point: prove which LOOP an optimization moved work out of.
    with tempfile.TemporaryDirectory() as d:
        mp = pathlib.Path(d) / "m.jsonl"
        events = [
            _assistant(_usage(o=1), [("Read", {"file_path": "a.c"})]),          # main read
            _assistant(_usage(o=1), [("Bash", {"command": "rg foo src/"})]),    # main bash-search
            _assistant(_usage(o=1), [("Bash", {"command": "make -j"})]),        # main bash (not search)
            _assistant(_usage(o=1), [("Grep", {"pattern": "x"})], parent="p1"),  # sidechain grep
            _assistant(_usage(o=1), [("Read", {"file_path": "b.c"})], parent="p1"),  # sidechain read
            {"type": "result", "result": "done", "usage": _usage(o=1)},
        ]
        _run(events, mp)
        rec = [json.loads(l) for l in mp.read_text().splitlines() if l.strip()][-1]
        assert rec["main_tools"] == {"read": 1, "bash_search": 1, "bash": 1}, rec["main_tools"]
        assert rec["sidechain_tools"] == {"grep": 1, "read": 1}, rec["sidechain_tools"]


def test_no_env_writes_nothing():
    payload = json.dumps(_assistant(_usage(o=1))) + "\n"
    env = {k: v for k, v in os.environ.items() if k != "OVERNIGHT_METRICS_FILE"}
    proc = subprocess.run([sys.executable, str(SCRIPT)], input=payload,
                          text=True, capture_output=True, env=env)
    assert proc.returncode == 0, proc.stderr


def test_growing_output_uses_max_per_msg():
    # P0.1: the CLI repeats a message envelope PER content block with a GROWING
    # output_tokens; first-seen dedupe captured the partial count (the ~47x
    # undercount). We must keep the MAX per id; input/cache-read stay stable.
    with tempfile.TemporaryDirectory() as d:
        mp = pathlib.Path(d) / "m.jsonl"
        events = [
            _assistant(_usage(i=1000, o=2, cr=5000), [("Read", {"file_path": "a.c"})], msg_id="A"),
            _assistant(_usage(i=1000, o=50, cr=5000), [("Grep", {"pattern": "x"})], msg_id="A"),
            _assistant(_usage(i=1000, o=500, cr=5000), msg_id="A"),   # final, complete
            {"type": "result", "result": "done"},
        ]
        _run(events, mp)
        rec = [json.loads(l) for l in mp.read_text().splitlines() if l.strip()][-1]
        assert rec["turns"] == 1, rec
        assert rec["output_tokens"] == 500, ("must be MAX not first-seen 2", rec)
        assert rec["input_tokens"] == 1000, ("stable field, not summed", rec)
        assert rec["cache_read_input_tokens"] == 5000, rec


def test_eof_flush_captures_trailing_section():
    # P0.1: a section that ends with NO progress/result marker (killed run /
    # stream EOF) must still be flushed -- this is the tail that used to vanish.
    with tempfile.TemporaryDirectory() as d:
        mp = pathlib.Path(d) / "m.jsonl"
        events = [
            _assistant(_usage(o=100), [("Read", {"file_path": "a.c"})], msg_id="m1"),
            _assistant(_usage(o=5), [("Bash", {"command": "python3 .claude/hooks/run_phase_guard.py progress"})], msg_id="m2"),
            # trailing work, NO result event and NO further progress marker:
            _assistant(_usage(o=777), [("Edit", {"file_path": "b.c"})], msg_id="m3"),
        ]
        _run(events, mp)
        recs = [json.loads(l) for l in mp.read_text().splitlines() if l.strip()]
        assert [r["marker"] for r in recs] == ["progress", "eof"], recs
        assert recs[0]["output_tokens"] == 105, recs[0]
        assert recs[1]["output_tokens"] == 777, ("trailing tail captured", recs[1])


def test_attribution_and_live_snapshot():
    # P0.1: every record carries run/TODO/section + start/end SHA. (The live
    # snapshot is written each turn -- covered by test_live_snapshot_content --
    # and removed on a clean finish, covered by
    # test_live_snapshot_removed_on_clean_finish.)
    with tempfile.TemporaryDirectory() as d:
        mp = pathlib.Path(d) / "m.jsonl"
        events = [
            _assistant(_usage(o=42), [("Read", {"file_path": "a.c"})], msg_id="m1"),
            {"type": "result", "result": "done"},
        ]
        _run(events, mp, {"OVERNIGHT_RUN_ID": "run-TEST",
                          "OVERNIGHT_TODO": "todo/x.md", "OVERNIGHT_SECTION": "7"})
        rec = [json.loads(l) for l in mp.read_text().splitlines() if l.strip()][-1]
        assert rec["run_id"] == "run-TEST"
        assert rec["todo"] == "todo/x.md" and rec["section"] == "7"
        assert "start_sha" in rec and "end_sha" in rec   # present (None outside git ok)


def test_live_snapshot_content():
    # Directly exercise the writer (bypassing main()'s finalization cleanup):
    # each usage turn rewrites the atomic .live snapshot with a "live" marker.
    import importlib.util
    spec = importlib.util.spec_from_file_location("stream_report", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    with tempfile.TemporaryDirectory() as d:
        mp = pathlib.Path(d) / "m.jsonl"
        os.environ["OVERNIGHT_RUN_ID"] = "run-SNAP"
        try:
            m = mod.SectionMetrics(str(mp))
            m.add_usage({"output_tokens": 5}, msg_id="m1")
        finally:
            os.environ.pop("OVERNIGHT_RUN_ID", None)
        live = pathlib.Path(str(mp) + ".live")
        assert live.exists(), "live snapshot must be written each turn"
        snap = json.loads(live.read_text().strip().splitlines()[-1])
        assert snap["marker"] == "live" and snap["run_id"] == "run-SNAP", snap


def test_run_id_derived_from_metrics_path():
    with tempfile.TemporaryDirectory() as d:
        mp = pathlib.Path(d) / "run-20260712-021311.jsonl"
        _run([_assistant(_usage(o=1), msg_id="m1"), {"type": "result", "result": "x"}], mp)
        rec = [json.loads(l) for l in mp.read_text().splitlines() if l.strip()][-1]
        assert rec["run_id"] == "run-20260712-021311", rec


def test_todo_section_from_sequencer_cursor_when_env_unset():
    # Codex audit 2026-07-13: the launcher does not export OVERNIGHT_TODO/
    # OVERNIGHT_SECTION, which left todo/section null. They must now fall back
    # to the live sequencer cursor (.claude/state/sequencer-run.json).
    with tempfile.TemporaryDirectory() as d:
        proj = pathlib.Path(d)
        (proj / ".claude/state").mkdir(parents=True)
        (proj / ".claude/state/sequencer-run.json").write_text(json.dumps(
            {"file": "todo/02-kernel-core/TODO-22-environment-variables.md",
             "section_idx": 2}))
        mp = proj / "run-20260713-000000.jsonl"
        _run([_assistant(_usage(o=1), msg_id="m1"), {"type": "result", "result": "x"}],
             mp, extra_env={"CLAUDE_PROJECT_DIR": str(proj),
                            "OVERNIGHT_TODO": "", "OVERNIGHT_SECTION": ""})
        rec = [json.loads(l) for l in mp.read_text().splitlines() if l.strip()][-1]
        assert rec["todo"] == "todo/02-kernel-core/TODO-22-environment-variables.md", rec
        assert rec["section"] == 2, rec


def test_env_todo_section_override_sequencer_cursor():
    # Explicit env vars still win over the cursor fallback.
    with tempfile.TemporaryDirectory() as d:
        proj = pathlib.Path(d)
        (proj / ".claude/state").mkdir(parents=True)
        (proj / ".claude/state/sequencer-run.json").write_text(json.dumps(
            {"file": "todo/x.md", "section_idx": 9}))
        mp = proj / "run-20260713-000001.jsonl"
        _run([_assistant(_usage(o=1), msg_id="m1"), {"type": "result", "result": "x"}],
             mp, extra_env={"CLAUDE_PROJECT_DIR": str(proj),
                            "OVERNIGHT_TODO": "todo/explicit.md",
                            "OVERNIGHT_SECTION": "7"})
        rec = [json.loads(l) for l in mp.read_text().splitlines() if l.strip()][-1]
        assert rec["todo"] == "todo/explicit.md", rec
        assert rec["section"] == "7", rec


def _transcript(path, main_out, side_out=None):
    """Authoritative session transcript: each id carries its COMPLETE output."""
    lines = [json.dumps({"type": "assistant",
                         "message": {"id": mid, "usage": {"output_tokens": o}}})
             for mid, o in main_out.items()]
    for mid, o in (side_out or {}).items():
        lines.append(json.dumps({"type": "assistant", "isSidechain": True,
                                 "message": {"id": mid, "usage": {"output_tokens": o}}}))
    path.write_text("\n".join(lines) + "\n")


def test_reconcile_output_from_transcript():
    # The live stream undercounts output (only input/cache are known mid-flight);
    # finalization must restore output from the session transcript by id.
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d)
        tr = p / "session.jsonl"
        _transcript(tr, {"m1": 5000, "m2": 3000})
        mp = p / "run-20260713-000010.jsonl"
        # lossy stream: same ids, output forced to 1 each
        _run([_assistant(_usage(o=1), msg_id="m1"),
              _assistant(_usage(o=1), msg_id="m2"),
              {"type": "result", "result": "x"}],
             mp, extra_env={"OVERNIGHT_SESSION_TRANSCRIPT": str(tr)})
        recs = [json.loads(l) for l in mp.read_text().splitlines() if l.strip()]
        assert sum(r["output_tokens"] for r in recs) == 8000, recs
        assert all(r.get("output_source") == "transcript" for r in recs), recs


def test_reconcile_sidechain_output_from_transcript():
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d)
        tr = p / "session.jsonl"
        _transcript(tr, {"m1": 5000}, side_out={"s1": 900})
        mp = p / "run-20260713-000011.jsonl"
        _run([_assistant(_usage(o=1), msg_id="m1"),
              _assistant(_usage(o=1), msg_id="s1", parent="agent-1"),
              {"type": "result", "result": "x"}],
             mp, extra_env={"OVERNIGHT_SESSION_TRANSCRIPT": str(tr)})
        rec = [json.loads(l) for l in mp.read_text().splitlines() if l.strip()][-1]
        assert rec["output_tokens"] == 5000, rec
        assert rec["sidechain_output_tokens"] == 900, rec


def test_reconcile_per_section_attribution():
    # ids stream in section 0 (before progress) vs section 1 (after); each
    # section's output must come from ONLY its own ids.
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d)
        tr = p / "session.jsonl"
        _transcript(tr, {"a1": 100, "a2": 200, "b1": 4000})
        mp = p / "run-20260713-000012.jsonl"
        progress = {"type": "assistant", "message": {"content": [
            {"type": "tool_use", "name": "Bash",
             "input": {"command": "python3 .claude/hooks/run_phase_guard.py progress"}}]}}
        _run([_assistant(_usage(o=1), msg_id="a1"),
              _assistant(_usage(o=1), msg_id="a2"),
              progress,
              _assistant(_usage(o=1), msg_id="b1"),
              {"type": "result", "result": "x"}],
             mp, extra_env={"OVERNIGHT_SESSION_TRANSCRIPT": str(tr)})
        recs = [json.loads(l) for l in mp.read_text().splitlines() if l.strip()]
        assert recs[0]["output_tokens"] == 300, recs   # a1 + a2
        assert recs[1]["output_tokens"] == 4000, recs  # b1


def test_reconcile_fail_open_without_transcript():
    with tempfile.TemporaryDirectory() as d:
        mp = pathlib.Path(d) / "run-20260713-000013.jsonl"
        _run([_assistant(_usage(o=7), msg_id="m1"), {"type": "result", "result": "x"}],
             mp, extra_env={"OVERNIGHT_SESSION_TRANSCRIPT": str(pathlib.Path(d) / "nope.jsonl")})
        rec = [json.loads(l) for l in mp.read_text().splitlines() if l.strip()][-1]
        assert rec["output_tokens"] == 7, rec          # stream value kept
        assert "output_source" not in rec, rec         # not reconciled


def test_live_snapshot_removed_on_clean_finish():
    with tempfile.TemporaryDirectory() as d:
        mp = pathlib.Path(d) / "run-20260713-000014.jsonl"
        _run([_assistant(_usage(o=1), msg_id="m1"), {"type": "result", "result": "x"}], mp)
        assert mp.exists()
        assert not pathlib.Path(str(mp) + ".live").exists(), "orphan .live not cleaned"



def test_advisory_hook_output_is_surfaced():
    """2026-07-31: only `is_error` tool results were logged, so a hook that
    emits a systemMessage instead of blocking was INVISIBLE in the run log.
    The context-rotation hint fired at event 201/201 of run-20260731-000502 and
    grepping the log for it returned 0 -- which read as a dead mechanism when
    it had actually worked. An advisory gate you cannot see cannot be tuned."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("stream_report", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    class _M:
        def note_session(self, *a):
            pass

        def flush(self, *a):
            pass

    def run(ev):
        import contextlib, io
        b = io.StringIO()
        with contextlib.redirect_stdout(b):
            mod.handle(ev, _M())
        return b.getvalue().strip()

    def user(text, err=False):
        blk = {"type": "tool_result", "content": [{"type": "text", "text": text}]}
        if err:
            blk["is_error"] = True
        return {"type": "user", "message": {"content": [blk]}}

    out = run(user("[sequencer] context-rotation hint set (90 tool-events)"))
    assert "hook:" in out and "context-rotation hint" in out, out
    assert "hook:" in run(user("[todo-wrap -- not a block] long continuation line"))
    # Errors must STILL surface, and ordinary output must stay silent -- the
    # log is only useful while it remains a signal rather than a transcript.
    assert "tool error: boom" in run(user("boom", err=True))
    assert run(user("ordinary command output")) == ""


def test_bash_command_clip_survives_a_compound_command():
    """The 200-char clip truncated mid-word on any chained command, so a verb in
    the TAIL vanished: `run_phase_guard.py rollover` grepped as 0 hits on a
    segment that demonstrably rolled over, making log-based diagnosis unreliable."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("stream_report", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    cmd = ("python3 .claude/hooks/run_phase_guard.py progress; "
           "python3 scripts/overnight/review-envelope.py --check; "
           "bash scripts/overnight/run-artifact.sh lbl -- bash scripts/test.sh; "
           "python3 .claude/hooks/run_phase_guard.py rollover")
    line = mod._bash({"description": "Record progress and attempt rollover",
                      "command": cmd})
    assert "rollover" in line, line


def test_hook_advisories_surface_from_attachment_events():
    """A PostToolUse hook's systemMessage arrives as its own `attachment`
    event, NOT as a tool_result. stream-report handled no such kind, so every
    advisory hook was invisible in the run log -- which hid the context-rotation
    hint through two canaries and produced the wrong conclusion twice.

    Shapes are taken verbatim from a real session transcript
    (run-20260731-155656): each firing emits BOTH a `hook_success` carrying raw
    `{"systemMessage": ...}` stdout and a `hook_system_message` twin carrying the
    rendered text. Only the twin may be surfaced, or every hint double-logs.
    """
    import json as _json
    import subprocess as _sp
    import sys as _sys
    from pathlib import Path as _P
    script = _P(__file__).resolve().parents[1] / "stream-report.py"
    msg = "[sequencer] context-rotation hint set (90 tool-events since the last rollover)."
    events = [
        {"type": "attachment", "attachment": {
            "type": "hook_success", "hookName": "PostToolUse:Bash",
            "hookEvent": "PostToolUse", "toolUseID": "t1",
            "stdout": _json.dumps({"systemMessage": msg}), "stderr": "", "content": ""}},
        {"type": "attachment", "attachment": {
            "type": "hook_system_message", "hookName": "PostToolUse:Bash",
            "hookEvent": "PostToolUse", "toolUseID": "t1", "content": msg}},
        {"type": "attachment", "attachment": {
            "type": "hook_error", "hookName": "PreToolUse:Bash",
            "hookEvent": "PreToolUse", "stderr": "boom", "content": ""}},
    ]
    payload = "\n".join(_json.dumps(e) for e in events)
    r = _sp.run([_sys.executable, str(script)], input=payload,
                capture_output=True, text=True, timeout=60)
    out = r.stdout
    assert out.count("context-rotation hint") == 1, (
        "advisory must surface exactly once -- hook_success and its "
        f"hook_system_message twin both logged?\n{out}")
    assert "hook: [PostToolUse:Bash]" in out, out
    assert "hook error: [PreToolUse:Bash] boom" in out, out


def test_context_stats_floor_percentiles_and_sidechain_excluded():
    # The first MAIN turn's context is the session floor; percentiles cover the
    # section's main turns only; a sidechain (subagent) turn never counts, not
    # even as the floor, and the floor carries into the next section.
    with tempfile.TemporaryDirectory() as d:
        mp = pathlib.Path(d) / "m.jsonl"
        events = [
            _assistant(_usage(i=1000, cr=0, cc=0), parent="toolu_x", msg_id="side_1"),
            _assistant(_usage(i=100, cr=50_000, cc=10_000), msg_id="m1"),       # 60,100
            _assistant(_usage(i=10, cr=190_000, cc=0), msg_id="m2"),            # 190,010
            _assistant(_usage(i=10, cr=390_000, cc=0), msg_id="m3"),            # 390,010
            _assistant(_usage(o=1), [("Bash", {"command": "python3 .claude/hooks/run_phase_guard.py progress"})], msg_id="m4"),
            _assistant(_usage(i=5, cr=500_000, cc=0), msg_id="m5"),
            {"type": "result", "result": "done", "usage": _usage(o=1)},
        ]
        _run(events, mp)
        recs = [json.loads(l) for l in mp.read_text().splitlines() if l.strip()]
        s0, s1 = recs
        assert s0["context_floor_tokens"] == 60_100, s0
        assert s0["context_min_tokens"] == 60_100, s0
        assert s0["context_max_tokens"] == 390_010, s0
        assert s0["context_p50_tokens"] == 190_010, s0
        assert s0["context_mean_tokens"] == (60_100 + 190_010 + 390_010) // 3, s0
        assert abs(s0["resident_share"] - round(60_100 * 3 / 640_120, 4)) < 1e-9, s0
        assert s1["context_floor_tokens"] == 60_100, s1        # run-level, not reset
        assert s1["context_max_tokens"] == 500_005, s1

if __name__ == "__main__":
    test_two_sections_split_on_progress()
    test_model_confirmed_matches_expected_no_warning()
    test_model_mismatch_warns()
    test_model_change_mid_run_logged()
    test_sidechain_model_ignored()
    test_duplicate_message_id_counts_once()
    test_model_inherit_skips_validation()
    test_main_vs_sidechain_tool_attribution()
    test_no_env_writes_nothing()
    test_growing_output_uses_max_per_msg()
    test_eof_flush_captures_trailing_section()
    test_attribution_and_live_snapshot()
    test_live_snapshot_content()
    test_run_id_derived_from_metrics_path()
    test_todo_section_from_sequencer_cursor_when_env_unset()
    test_env_todo_section_override_sequencer_cursor()
    test_reconcile_output_from_transcript()
    test_reconcile_sidechain_output_from_transcript()
    test_reconcile_per_section_attribution()
    test_reconcile_fail_open_without_transcript()
    test_live_snapshot_removed_on_clean_finish()
    test_advisory_hook_output_is_surfaced()
    test_bash_command_clip_survives_a_compound_command()
    test_hook_advisories_surface_from_attachment_events()
    test_context_stats_floor_percentiles_and_sidechain_excluded()
    print("PASS: stream-report metrics")

