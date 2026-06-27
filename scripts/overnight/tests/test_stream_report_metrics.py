#!/usr/bin/env python3
"""Contract test for stream-report.py per-section metrics sidecar."""
import json, os, subprocess, sys, tempfile, pathlib

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE.parent / "stream-report.py"


def _run(events, metrics_path):
    payload = "".join(json.dumps(e) + "\n" for e in events)
    env = {**os.environ, "OVERNIGHT_METRICS_FILE": str(metrics_path)}
    proc = subprocess.run(
        [sys.executable, str(SCRIPT)],
        input=payload, text=True, capture_output=True, env=env,
    )
    assert proc.returncode == 0, proc.stderr
    return proc.stdout


def _assistant(usage=None, tools=None):
    content = []
    if tools:
        for name, inp in tools:
            content.append({"type": "tool_use", "name": name, "input": inp})
    msg = {"content": content}
    if usage is not None:
        msg["usage"] = usage
    return {"type": "assistant", "message": msg}


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


def test_no_env_writes_nothing():
    payload = json.dumps(_assistant(_usage(o=1))) + "\n"
    env = {k: v for k, v in os.environ.items() if k != "OVERNIGHT_METRICS_FILE"}
    proc = subprocess.run([sys.executable, str(SCRIPT)], input=payload,
                          text=True, capture_output=True, env=env)
    assert proc.returncode == 0, proc.stderr


if __name__ == "__main__":
    test_two_sections_split_on_progress()
    test_no_env_writes_nothing()
    print("PASS: stream-report metrics")
