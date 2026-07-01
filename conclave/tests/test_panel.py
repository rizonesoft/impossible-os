from conclave import judge, panel


def test_run_parallel_collects_all(monkeypatch):
    monkeypatch.setattr(panel.http, "chat",
                        lambda s, m, msgs, t: (f"a-{m}", 0.01, 1.0, {"prompt": 10, "completion": 5}))
    out = panel.run("k", ["m1", "m2"], "sys", "brief", 5, 2)
    assert {r["model"] for r in out} == {"m1", "m2"}
    assert all(r["status"] == "ok" and r["prompt_tokens"] == 10 for r in out)


def test_probe_error_is_captured(monkeypatch):
    def boom(*a):
        raise RuntimeError("503")
    monkeypatch.setattr(panel.http, "chat", boom)
    r = panel.probe("k", "m", "sys", "brief", 5)
    assert r["status"] == "error" and r["cost"] == 0 and r["chars"] == 0


def test_judge_builds_messages_and_returns(monkeypatch):
    captured = {}

    def fake_chat(secret, model, messages, timeout):
        captured["messages"] = messages
        return "VERDICT", 0.2, 1.0, {"prompt": 3, "completion": 2}

    monkeypatch.setattr(judge.http, "chat", fake_chat)
    panel_results = [{"model": "m1", "status": "ok", "content": "found a race"},
                     {"model": "m2", "status": "error", "content": ""}]
    content, cost, sec, toks = judge.judge("k", "j", "why crash", panel_results,
                                           prior=[{"source": "opus", "content": "prior note"}], timeout=5)
    assert content == "VERDICT" and cost == 0.2
    user = captured["messages"][1]["content"]
    assert "found a race" in user and "prior note" in user and "why crash" in user
    assert "m2" not in user            # errored panelist excluded
