#!/usr/bin/env python3
import json, subprocess, sys, tempfile, pathlib

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE.parent / "metrics-report.py"


def _write(path, recs):
    path.write_text("".join(json.dumps(r) + "\n" for r in recs), encoding="ascii")


def _rec(idx, out, agents=0, grep=0, lsp=0):
    return {"section_index": idx, "marker": "progress", "timestamp": "00:00:00",
            "turns": 1, "input_tokens": 0, "output_tokens": out,
            "cache_read_input_tokens": 0, "cache_creation_input_tokens": 0,
            "agent_dispatches": agents, "grep_calls": grep, "lsp_calls": lsp}


def test_single_file_total():
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d) / "a.jsonl"
        _write(p, [_rec(0, 100, agents=1, lsp=2), _rec(1, 50, grep=3)])
        out = subprocess.run([sys.executable, str(SCRIPT), str(p)],
                             text=True, capture_output=True)
        assert out.returncode == 0, out.stderr
        assert "150" in out.stdout            # total output tokens
        assert "TOTAL" in out.stdout


def test_missing_file_exits_2():
    out = subprocess.run([sys.executable, str(SCRIPT), "/no/such.jsonl"],
                         text=True, capture_output=True)
    assert out.returncode == 2, out.stdout


def test_orphan_live_ingested_when_no_finalized():
    # A run that crashed before its first flush leaves an empty/absent
    # finalized jsonl but a surviving .live snapshot; its tokens must count.
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d) / "run.jsonl"          # finalized file absent
        live = pathlib.Path(str(p) + ".live")
        live.write_text(json.dumps(_rec(0, 4242)) + "\n", encoding="ascii")
        out = subprocess.run([sys.executable, str(SCRIPT), str(p)],
                             text=True, capture_output=True)
        assert out.returncode == 0, out.stderr
        assert "4242" in out.stdout, out.stdout    # orphan tokens surfaced


def test_missing_file_and_no_live_still_exits_2():
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d) / "gone.jsonl"         # neither file nor .live
        out = subprocess.run([sys.executable, str(SCRIPT), str(p)],
                             text=True, capture_output=True)
        assert out.returncode == 2, out.stdout


if __name__ == "__main__":
    test_single_file_total()
    test_missing_file_exits_2()
    test_orphan_live_ingested_when_no_finalized()
    test_missing_file_and_no_live_still_exits_2()
    print("PASS: metrics-report")
