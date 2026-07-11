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


def test_no_env_writes_nothing():
    payload = json.dumps(_assistant(_usage(o=1))) + "\n"
    env = {k: v for k, v in os.environ.items() if k != "OVERNIGHT_METRICS_FILE"}
    proc = subprocess.run([sys.executable, str(SCRIPT)], input=payload,
                          text=True, capture_output=True, env=env)
    assert proc.returncode == 0, proc.stderr


if __name__ == "__main__":
    test_two_sections_split_on_progress()
    test_model_confirmed_matches_expected_no_warning()
    test_model_mismatch_warns()
    test_model_change_mid_run_logged()
    test_sidechain_model_ignored()
    test_duplicate_message_id_counts_once()
    test_model_inherit_skips_validation()
    test_no_env_writes_nothing()
    print("PASS: stream-report metrics")
