import json

from conclave import jobs


def _jdir(root, project="proj"):
    p = root / "data" / "projects" / project / "jobs"
    p.mkdir(parents=True, exist_ok=True)
    return p


def test_worker_runs_escalate_and_writes_result(tmp_path, monkeypatch):
    jdir = _jdir(tmp_path)
    jid = "20260628-000000-abc"
    (jdir / f"{jid}.request.json").write_text(json.dumps(
        {"mode": "stuck", "target": "build", "brief": "why crash", "prior": []}))
    (jdir / f"{jid}.meta.json").write_text(json.dumps({"id": jid, "status": "pending"}))
    monkeypatch.setattr(jobs, "_do_escalate", lambda root, project, jid, req: {
        "status": "ok", "output": "SYNTH", "cost": 0.5, "panel": [],
        "cumulative_cost": 0.5, "tokens": 100, "wall_s": 12.0})
    jobs.run_worker(tmp_path, "proj", jid)
    assert (jdir / f"{jid}.result.txt").read_text() == "SYNTH"
    meta = json.loads((jdir / f"{jid}.meta.json").read_text())
    assert meta["status"] == "done" and meta["cost"] == 0.5 and meta["wall_s"] == 12.0
    # transcript recorded for compile/learning
    runs = (tmp_path / "data/projects/proj/runs.jsonl").read_text()
    assert "why crash" in runs and "SYNTH" in runs


def test_outcome_triggers_automatic_learning(tmp_path):
    from conclave import compile as cc
    from conclave import learn
    jdir = _jdir(tmp_path)
    cc.record_run(tmp_path, "proj", run_id="j1", brief="smp race in scheduler",
                  synthesis="take the runqueue spinlock", verdict="unknown")
    (jdir / "j1.meta.json").write_text(json.dumps(
        {"id": "j1", "status": "done", "target": "smp-race",
         "per_model": [{"model": "m1", "status": "ok"}, {"model": "m2", "status": "error"}]}))
    (jdir / "j1.result.txt").write_text("take the runqueue spinlock before enqueue")
    jobs.outcome(tmp_path, "proj", "j1", "resolved")          # auto-learns (no network: no conclave.toml)
    pol = learn._load(tmp_path, "proj")
    assert pol["panel_credit"]["m1"] > 0 and "m2" not in pol["panel_credit"]
    assert pol["route"]["smp-race"] == ["m1"]
    runs = (tmp_path / "data/projects/proj/runs.jsonl").read_text()
    assert '"verdict": "resolved"' in runs                    # transcript now compile-eligible


def test_poll_reports_states(tmp_path):
    jdir = _jdir(tmp_path)
    (jdir / "j1.meta.json").write_text(json.dumps({"id": "j1", "status": "pending", "target": "t"}))
    (jdir / "j2.meta.json").write_text(json.dumps({"id": "j2", "status": "done", "target": "t"}))
    (jdir / "j2.result.txt").write_text("ANSWER")
    assert jobs.poll(tmp_path, "proj", "j1") == "PENDING"
    assert jobs.poll(tmp_path, "proj", "j2").startswith("DONE") and "ANSWER" in jobs.poll(tmp_path, "proj", "j2")


def test_list_enumerates(tmp_path):
    jdir = _jdir(tmp_path)
    (jdir / "j1.meta.json").write_text(json.dumps({"id": "j1", "status": "pending", "target": "build"}))
    out = jobs.list_jobs(tmp_path, "proj")
    assert "j1" in out and "pending" in out


def test_outcome_records_metrics(tmp_path):
    jdir = _jdir(tmp_path)
    (jdir / "j1.meta.json").write_text(json.dumps({"id": "j1", "status": "done", "target": "t"}))
    jobs.outcome(tmp_path, "proj", "j1", "resolved")
    meta = json.loads((jdir / "j1.meta.json").read_text())
    assert meta["outcome"] == "resolved"
    outcomes = (tmp_path / "data/projects/proj/outcomes.jsonl").read_text()
    assert "resolved" in outcomes and "j1" in outcomes


def test_dispatch_gated_without_env(tmp_path, monkeypatch):
    monkeypatch.delenv("CONCLAVE_ENABLED", raising=False)
    assert jobs.dispatch(tmp_path, "proj", "stuck", "t", "brief", []) == "disabled"


def test_stats_summarizes(tmp_path):
    (tmp_path / "data/projects/proj").mkdir(parents=True)
    (tmp_path / "data/projects/proj/totals.json").write_text(json.dumps(
        {"runs": 2, "total_cost": 1.23, "panel_cost": 0.8, "judge_cost": 0.43,
         "total_tokens": 5000, "resolved": 1, "unresolved": 1, "by_mode": {"stuck": {"runs": 2, "cost": 1.23}}}))
    out = jobs.stats(tmp_path, "proj")
    assert "runs=2" in out and "1.23" in out and "resolved=1" in out
