#!/usr/bin/env python3
import json, sys, tempfile, pathlib

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
sys.path.insert(0, str(REPO / ".fusion"))
import fusion_escalate as fe  # noqa: E402

CFG = {"panel": ["a", "b", "c"], "judge": "j", "max_calls": 3, "min_credits": 2.0,
       "model_timeout_s": 5, "judge_timeout_s": 5, "workers": 3}


def _root(d, budget_used=None):
    root = pathlib.Path(d)
    if budget_used is not None:
        sp = root / ".claude" / "state"
        sp.mkdir(parents=True, exist_ok=True)
        (sp / "fusion-budget.json").write_text(json.dumps({"fusion_calls_used": budget_used}))
    return root


def _ok_panel(secret, panel, sys_prompt, brief, timeout, workers):
    return [{"model": m, "status": "ok", "content": f"analysis from {m}",
             "cost": 0.01, "sec": 1.0, "chars": 20,
             "prompt_tokens": 100, "completion_tokens": 50} for m in panel]


def test_disabled():
    with tempfile.TemporaryDirectory() as d:
        assert fe.escalate(enabled=False, secret="x", cfg=CFG, mode="stuck",
                           brief="b", root=_root(d))["status"] == "disabled"


def test_no_key():
    with tempfile.TemporaryDirectory() as d:
        assert fe.escalate(enabled=True, secret="", cfg=CFG, mode="stuck",
                           brief="b", root=_root(d))["status"] == "no_key"


def test_over_budget():
    with tempfile.TemporaryDirectory() as d:
        assert fe.escalate(enabled=True, secret="x", cfg=CFG, mode="stuck",
                           brief="b", root=_root(d, budget_used=3))["status"] == "over_budget"


def test_low_balance():
    with tempfile.TemporaryDirectory() as d:
        fe._remaining_credits = lambda s: 1.0
        assert fe.escalate(enabled=True, secret="x", cfg=CFG, mode="stuck",
                           brief="b", root=_root(d))["status"] == "low_balance"


def test_balance_check_error_fails_closed():
    with tempfile.TemporaryDirectory() as d:
        def boom(s):
            raise RuntimeError("down")
        fe._remaining_credits = boom
        assert fe.escalate(enabled=True, secret="x", cfg=CFG, mode="stuck",
                           brief="b", root=_root(d))["status"] == "low_balance"


def test_ok_panel_plus_judge_writes_dataset_and_metrics():
    with tempfile.TemporaryDirectory() as d:
        root = _root(d)
        fe._remaining_credits = lambda s: 100.0
        fe._run_panel = _ok_panel
        fe._judge = lambda secret, jm, brief, panel, prior, timeout: (
            "SYNTHESIS", 0.3, 2.0, {"prompt": 300, "completion": 200})
        r = fe.escalate(enabled=True, secret="x", cfg=CFG, mode="stuck",
                        brief="why does it crash", root=root, target="heisenbug", run_id="JID1")
        assert r["status"] == "ok" and r["output"] == "SYNTHESIS"
        assert len(r["panel"]) == 3 and r["panel"][0]["model"] == "a"
        assert abs(r["cost"] - (0.01 * 3 + 0.3)) < 1e-9          # panel + judge cost
        assert r["tokens"] == (100 + 50) * 3 + 300 + 200         # panel + judge tokens
        ds = (root / ".fusion" / "dataset.jsonl").read_text()
        assert "SYNTHESIS" in ds and "per_model" in ds
        used = json.loads((root / ".claude" / "state" / "fusion-budget.json").read_text())
        assert used["fusion_calls_used"] == 1
        # rich per-run metrics line
        m = json.loads((root / ".fusion" / "metrics.jsonl").read_text().splitlines()[-1])
        assert m["run_id"] == "JID1" and m["target"] == "heisenbug"
        assert m["cost_usd"]["run_total"] > 0 and m["cost_usd"]["cumulative_total"] > 0
        assert m["tokens"]["total"] == r["tokens"]
        assert m["intelligence"]["panel_ok"] == 3 and m["intelligence"]["judged"] is True
        assert m["latency_s"]["wall"] >= 0
        # cumulative totals ledger
        t = json.loads((root / ".fusion" / "totals.json").read_text())
        assert t["runs"] == 1 and t["by_mode"]["stuck"]["runs"] == 1
        assert abs(t["total_cost"] - r["cost"]) < 1e-9


def test_record_outcome_backfills_metrics_and_totals():
    with tempfile.TemporaryDirectory() as d:
        root = _root(d)
        fe._remaining_credits = lambda s: 100.0
        fe._run_panel = _ok_panel
        fe._judge = lambda secret, jm, brief, panel, prior, timeout: (
            "SYNTHESIS", 0.3, 2.0, {"prompt": 1, "completion": 1})
        fe.escalate(enabled=True, secret="x", cfg=CFG, mode="stuck",
                    brief="b", root=root, target="t", run_id="JIDX")
        fe.record_outcome(root, "JIDX", "resolved")
        m = json.loads((root / ".fusion" / "metrics.jsonl").read_text().splitlines()[-1])
        assert m["intelligence"]["outcome"] == "resolved"
        t = json.loads((root / ".fusion" / "totals.json").read_text())
        assert t["resolved"] == 1 and t["unresolved"] == 0


def test_judge_failure_returns_panel_only():
    with tempfile.TemporaryDirectory() as d:
        root = _root(d)
        fe._remaining_credits = lambda s: 100.0
        fe._run_panel = _ok_panel
        def jboom(*a, **k):
            raise RuntimeError("judge 500")
        fe._judge = jboom
        r = fe.escalate(enabled=True, secret="x", cfg=CFG, mode="stuck",
                        brief="b", root=root)
        assert r["status"] == "panel_only"
        assert "analysis from a" in r["output"]                  # raw panel handed back


def test_all_panel_fail_is_unavailable_no_charge():
    with tempfile.TemporaryDirectory() as d:
        root = _root(d)
        fe._remaining_credits = lambda s: 100.0
        fe._run_panel = lambda *a, **k: [
            {"model": "a", "status": "error", "content": "", "cost": 0, "sec": 0, "chars": 0}]
        r = fe.escalate(enabled=True, secret="x", cfg=CFG, mode="stuck",
                        brief="b", root=root)
        assert r["status"] == "unavailable"
        assert not (root / ".claude" / "state" / "fusion-budget.json").exists()


def test_read_context_bounds():
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d) / "big.txt"
        p.write_text("x" * 100000)
        out = fe._read_context(str(p), max_bytes=1000)
        assert len(out) < 1100 and "truncated" in out


if __name__ == "__main__":
    test_disabled()
    test_no_key()
    test_over_budget()
    test_low_balance()
    test_balance_check_error_fails_closed()
    test_ok_panel_plus_judge_writes_dataset_and_metrics()
    test_record_outcome_backfills_metrics_and_totals()
    test_judge_failure_returns_panel_only()
    test_all_panel_fail_is_unavailable_no_charge()
    test_read_context_bounds()
    print("PASS: fusion_escalate (DIY)")
