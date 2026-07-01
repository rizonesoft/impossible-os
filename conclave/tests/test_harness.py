from conclave import harness


class _Mem:
    def __init__(self, lessons):
        self.lessons = lessons

    def recall(self, brief, k=5):
        return [{"lesson": l, "meta": {}, "score": 1.0} for l in self.lessons]


class _Metrics:
    def __init__(self):
        self.runs = []

    def append_run(self, rec):
        self.runs.append(rec)

    def add_run_to_totals(self, **k):
        return k.get("run_cost", 0.0)


def _ok_panel(*a, **k):
    return [{"model": "m", "status": "ok", "content": "analysis", "cost": 0.01, "sec": 1.0,
             "chars": 8, "prompt_tokens": 10, "completion_tokens": 5}]


def test_escalate_injects_memory_and_returns_synthesis(monkeypatch, tmp_path, cfg):
    captured = {}

    def fake_run(secret, models, sys_prompt, brief, timeout, workers):
        captured["brief"] = brief
        return _ok_panel()

    monkeypatch.setattr(harness.panel, "run", fake_run)
    monkeypatch.setattr(harness.judge, "judge",
                        lambda *a, **k: ("SYNTH", 0.2, 1.0, {"prompt": 3, "completion": 2}))
    monkeypatch.setattr(harness.http, "remaining_credits", lambda s: 100.0)
    mem, met = _Mem(["use a spinlock around X"]), _Metrics()
    r = harness.escalate(cfg=cfg(), secret="k", mode="stuck", brief="why crash",
                         project="proj", root=tmp_path, memory=mem, metrics=met, run_id="R1")
    assert r["status"] == "ok" and r["output"] == "SYNTH"
    assert "spinlock" in captured["brief"]              # memory injected into the panel context
    assert met.runs and met.runs[-1]["mode"] == "stuck" and met.runs[-1]["run_id"] == "R1"
    assert r["cost"] == 0.01 + 0.2


def test_disabled_and_no_key(tmp_path, cfg):
    assert harness.escalate(cfg=cfg(enabled=False), secret="k", mode="stuck", brief="b",
                            project="p", root=tmp_path, memory=_Mem([]), metrics=_Metrics())["status"] == "disabled"
    assert harness.escalate(cfg=cfg(), secret="", mode="stuck", brief="b",
                            project="p", root=tmp_path, memory=_Mem([]), metrics=_Metrics())["status"] == "no_key"


def test_low_balance_fails_closed(monkeypatch, tmp_path, cfg):
    monkeypatch.setattr(harness.http, "remaining_credits", lambda s: 0.5)
    r = harness.escalate(cfg=cfg(min_credits=2.0), secret="k", mode="stuck", brief="b",
                         project="p", root=tmp_path, memory=_Mem([]), metrics=_Metrics())
    assert r["status"] == "low_balance"


def test_judge_failure_returns_panel_only(monkeypatch, tmp_path, cfg):
    monkeypatch.setattr(harness.panel, "run", _ok_panel)
    monkeypatch.setattr(harness.http, "remaining_credits", lambda s: 100.0)

    def boom(*a, **k):
        raise RuntimeError("judge 500")
    monkeypatch.setattr(harness.judge, "judge", boom)
    r = harness.escalate(cfg=cfg(), secret="k", mode="stuck", brief="b",
                         project="proj", root=tmp_path, memory=_Mem([]), metrics=_Metrics())
    assert r["status"] == "panel_only" and "analysis" in r["output"]
