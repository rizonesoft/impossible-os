import json

from conclave.metrics import MetricsStore


def test_totals_accumulate_and_outcome_backfills(tmp_path):
    ms = MetricsStore(tmp_path, "proj")
    ms.append_run({"run_id": "R1", "mode": "stuck", "cost_usd": {"run_total": 0.5},
                   "intelligence": {"outcome": "unknown"}})
    cum = ms.add_run_to_totals(mode="stuck", run_cost=0.5, panel_cost=0.4, judge_cost=0.1, tokens=100)
    assert abs(cum - 0.5) < 1e-9
    ms.record_outcome("R1", "resolved")
    t = ms.totals()
    assert t["runs"] == 1 and t["resolved"] == 1 and t["by_mode"]["stuck"]["runs"] == 1
    line = json.loads((tmp_path / "data/projects/proj/metrics.jsonl").read_text().splitlines()[-1])
    assert line["intelligence"]["outcome"] == "resolved"
    out = json.loads((tmp_path / "data/projects/proj/outcomes.jsonl").read_text().splitlines()[-1])
    assert out["verdict"] == "resolved" and out["run_id"] == "R1"


def test_unresolved_counts_separately(tmp_path):
    ms = MetricsStore(tmp_path, "proj")
    ms.append_run({"run_id": "R2", "intelligence": {"outcome": "unknown"}})
    ms.add_run_to_totals(mode="review", run_cost=0.2, panel_cost=0.2, judge_cost=0.0, tokens=10)
    ms.record_outcome("R2", "unresolved")
    t = ms.totals()
    assert t["resolved"] == 0 and t["unresolved"] == 1


def test_totals_missing_file_is_zeroed(tmp_path):
    assert MetricsStore(tmp_path, "fresh").totals()["runs"] == 0
