import json

from conclave import heal


def test_repair_state_from_snapshot(tmp_path):
    bad = tmp_path / "data" / "x.json"
    bad.parent.mkdir(parents=True)
    bad.write_text("{ broken")
    (tmp_path / "data" / "x.json.snapshot").write_text('{"ok": 1}')
    assert heal.repair_state(bad) == {"ok": 1}
    assert json.loads(bad.read_text()) == {"ok": 1}        # repaired in place


def test_repair_state_no_snapshot_returns_none(tmp_path):
    bad = tmp_path / "y.json"
    bad.write_text("{ broken")
    assert heal.repair_state(bad) is None


def test_doctor_flags_missing_secret(tmp_path, cfg):
    rpt = heal.doctor(tmp_path, cfg(secret=None), secret=None, online=False)
    assert rpt["ok"] is False
    assert any(c["name"] == "secret" and not c["ok"] for c in rpt["checks"])


def test_doctor_passes_offline_with_secret(tmp_path, cfg):
    rpt = heal.doctor(tmp_path, cfg(secret="k"), secret="k", online=False)
    sec = [c for c in rpt["checks"] if c["name"] == "secret"][0]
    assert sec["ok"] is True
    assert all(c["ok"] for c in rpt["checks"]) and rpt["ok"] is True
