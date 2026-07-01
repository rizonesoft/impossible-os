from conclave import learn


def test_resolved_run_promotes_contributing_models(tmp_path):
    rec = {"signature": "smp-race",
           "per_model": [{"model": "m1", "status": "ok"}, {"model": "m2", "status": "error"}]}
    learn.update_policies(tmp_path, "proj", rec, "resolved")
    pol = learn._load(tmp_path, "proj")
    assert pol["panel_credit"]["m1"] > pol["panel_credit"].get("m2", 0)


def test_unresolved_penalizes(tmp_path):
    rec = {"signature": "x", "per_model": [{"model": "m1", "status": "ok"}]}
    learn.update_policies(tmp_path, "proj", rec, "resolved")
    learn.update_policies(tmp_path, "proj", rec, "unresolved")
    pol = learn._load(tmp_path, "proj")
    assert pol["panel_credit"]["m1"] < 1.0      # promoted then penalized


def test_select_panel_uses_learned_route(tmp_path):
    rec = {"signature": "smp-race",
           "per_model": [{"model": "m1", "status": "ok"}, {"model": "m3", "status": "ok"}]}
    learn.update_policies(tmp_path, "proj", rec, "resolved")
    sub = learn.select_panel(tmp_path, "proj", "smp-race", ["m1", "m2", "m3", "m4"])
    assert sub == ["m1", "m3"]
    assert learn.select_panel(tmp_path, "proj", "unseen", ["m1", "m2"]) == ["m1", "m2"]


def test_infer_outcome_from_build():
    assert learn.infer_outcome(True) == "resolved"
    assert learn.infer_outcome(False) == "unresolved"
    assert learn.infer_outcome(None) is None
