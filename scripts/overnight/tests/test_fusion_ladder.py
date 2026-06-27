#!/usr/bin/env python3
import json, tempfile, pathlib, importlib.util

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
spec = importlib.util.spec_from_file_location("ladder", REPO / ".fusion" / "ladder.py")
ladder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ladder)


def _jobs(root):
    return pathlib.Path(root) / ".fusion" / "jobs"


def test_worker_runs_escalate_and_writes_result():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        jobs = _jobs(root); jobs.mkdir(parents=True)
        jid = "20260627-000000-abc123"
        (jobs / f"{jid}.request.json").write_text(json.dumps(
            {"mode": "stuck", "target": "build", "brief": "why crash", "prior": []}))
        (jobs / f"{jid}.meta.json").write_text(json.dumps(
            {"id": jid, "target": "build", "mode": "stuck", "status": "pending"}))
        ladder._load_caller = lambda root: type("C", (), {
            "escalate": staticmethod(lambda **kw: {"status": "ok", "output": "SYNTH", "cost": 0.5}),
            "_load_cfg": staticmethod(lambda r: {}),
            "_read_secret": staticmethod(lambda r: "x")})
        ladder.run_worker(root, jid)
        assert (jobs / f"{jid}.result.txt").read_text() == "SYNTH"
        meta = json.loads((jobs / f"{jid}.meta.json").read_text())
        assert meta["status"] == "done" and meta["cost"] == 0.5


def test_poll_reports_states():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d); jobs = _jobs(root); jobs.mkdir(parents=True)
        (jobs / "j1.meta.json").write_text(json.dumps({"id": "j1", "status": "pending", "target": "t"}))
        (jobs / "j2.meta.json").write_text(json.dumps({"id": "j2", "status": "done", "target": "t"}))
        (jobs / "j2.result.txt").write_text("ANSWER")
        assert ladder.poll(root, "j1") == "PENDING"
        assert ladder.poll(root, "j2").startswith("DONE") and "ANSWER" in ladder.poll(root, "j2")


def test_list_enumerates():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d); jobs = _jobs(root); jobs.mkdir(parents=True)
        (jobs / "j1.meta.json").write_text(json.dumps({"id": "j1", "status": "pending", "target": "build"}))
        out = ladder.list_jobs(root)
        assert "j1" in out and "pending" in out


def test_outcome_records():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d); jobs = _jobs(root); jobs.mkdir(parents=True)
        (root / ".fusion").mkdir(exist_ok=True)
        (jobs / "j1.meta.json").write_text(json.dumps({"id": "j1", "status": "done", "target": "t"}))
        ladder.outcome(root, "j1", "resolved")
        meta = json.loads((jobs / "j1.meta.json").read_text())
        assert meta["outcome"] == "resolved"
        ds = (root / ".fusion" / "dataset.jsonl").read_text()
        assert "fusion-outcome" in ds and "resolved" in ds


if __name__ == "__main__":
    test_worker_runs_escalate_and_writes_result()
    test_poll_reports_states()
    test_list_enumerates()
    test_outcome_records()
    print("PASS: fusion_ladder")
