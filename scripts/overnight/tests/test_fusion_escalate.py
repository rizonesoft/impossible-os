#!/usr/bin/env python3
import json, sys, tempfile, pathlib

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
sys.path.insert(0, str(REPO / ".fusion"))
import fusion_escalate as fe  # noqa: E402

CFG = {"panel": ["a", "b", "c"], "judge": "a",
       "max_calls": 3, "min_credits": 2.0, "timeout_s": 5}


def _root(d, budget_used=None):
    root = pathlib.Path(d)
    if budget_used is not None:
        sp = root / ".claude" / "state"
        sp.mkdir(parents=True, exist_ok=True)
        (sp / "fusion-budget.json").write_text(json.dumps({"fusion_calls_used": budget_used}))
    return root


def test_disabled():
    with tempfile.TemporaryDirectory() as d:
        r = fe.escalate(enabled=False, secret="x", cfg=CFG, mode="stuck",
                        brief="b", root=_root(d))
        assert r["status"] == "disabled"


def test_no_key():
    with tempfile.TemporaryDirectory() as d:
        r = fe.escalate(enabled=True, secret="", cfg=CFG, mode="stuck",
                        brief="b", root=_root(d))
        assert r["status"] == "no_key"


def test_over_budget():
    with tempfile.TemporaryDirectory() as d:
        r = fe.escalate(enabled=True, secret="x", cfg=CFG, mode="stuck",
                        brief="b", root=_root(d, budget_used=3))
        assert r["status"] == "over_budget"


def test_low_balance():
    with tempfile.TemporaryDirectory() as d:
        fe._remaining_credits = lambda s: 1.0
        r = fe.escalate(enabled=True, secret="x", cfg=CFG, mode="stuck",
                        brief="b", root=_root(d))
        assert r["status"] == "low_balance"


def test_balance_check_error_fails_closed():
    with tempfile.TemporaryDirectory() as d:
        def boom(s):
            raise RuntimeError("network down")
        fe._remaining_credits = boom
        r = fe.escalate(enabled=True, secret="x", cfg=CFG, mode="stuck",
                        brief="b", root=_root(d))
        assert r["status"] == "low_balance"  # unknown balance -> skip (no blind spend)


def test_ok_writes_dataset_and_increments_budget():
    with tempfile.TemporaryDirectory() as d:
        root = _root(d)
        fe._remaining_credits = lambda s: 100.0
        fe._fusion_call = lambda s, c, m, b: ("SYNTHESIS", 0.12)
        r = fe.escalate(enabled=True, secret="x", cfg=CFG, mode="stuck",
                        brief="why does it crash", root=root)
        assert r["status"] == "ok" and r["output"] == "SYNTHESIS"
        ds = (root / ".fusion" / "dataset.jsonl").read_text()
        assert "SYNTHESIS" in ds and "why does it crash" in ds
        used = json.loads((root / ".claude" / "state" / "fusion-budget.json").read_text())
        assert used["fusion_calls_used"] == 1


def test_total_deadline_fails_open():
    import time
    with tempfile.TemporaryDirectory() as d:
        root = _root(d)
        fe._remaining_credits = lambda s: 100.0
        fe._fusion_call = lambda *a: time.sleep(5)  # blocks past the deadline
        cfg = dict(CFG)
        cfg["timeout_s"] = 1
        r = fe.escalate(enabled=True, secret="x", cfg=cfg, mode="stuck",
                        brief="b", root=root)
        assert r["status"] == "unavailable"  # SIGALRM -> TimeoutError -> fail-open


def test_read_context_bounds():
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d) / "big.txt"
        p.write_text("x" * 100000)
        out = fe._read_context(str(p), max_bytes=1000)
        assert len(out) < 1100 and "truncated" in out


def test_post_error_is_fail_open_no_charge():
    with tempfile.TemporaryDirectory() as d:
        root = _root(d)
        fe._remaining_credits = lambda s: 100.0
        def boom(*a):
            raise RuntimeError("api 500")
        fe._fusion_call = boom
        r = fe.escalate(enabled=True, secret="x", cfg=CFG, mode="stuck",
                        brief="b", root=root)
        assert r["status"] == "unavailable"
        assert not (root / ".claude" / "state" / "fusion-budget.json").exists()


if __name__ == "__main__":
    test_disabled()
    test_no_key()
    test_over_budget()
    test_low_balance()
    test_balance_check_error_fails_closed()
    test_ok_writes_dataset_and_increments_budget()
    test_total_deadline_fails_open()
    test_read_context_bounds()
    test_post_error_is_fail_open_no_charge()
    print("PASS: fusion_escalate")
