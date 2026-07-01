import json

from conclave import compile as cc
from conclave.metrics import MetricsStore


def test_compile_shards_and_ledgers(tmp_path, fake_embedder, local_store, seed_runs, cfg):
    seed_runs(tmp_path, "proj", 10)
    out = cc.compile(tmp_path, cfg(shard_max_mib=1), "proj", embedder=fake_embedder, store=local_store)
    assert out["version"] == "conclave-v1" and out["num_examples"] == 10
    led = [json.loads(l) for l in (tmp_path / "ledger.jsonl").read_text().splitlines()]
    assert led[-1]["version"] == "conclave-v1"
    assert led[-1]["output_shards"] and all("sha256" in s for s in led[-1]["output_shards"])
    assert (tmp_path / "LATEST").read_text().strip() == "conclave-v1"


def test_compile_versions_increment(tmp_path, fake_embedder, local_store, seed_runs, cfg):
    seed_runs(tmp_path, "proj", 3)
    cc.compile(tmp_path, cfg(shard_max_mib=1), "proj", embedder=fake_embedder, store=local_store)
    out2 = cc.compile(tmp_path, cfg(shard_max_mib=1), "proj", embedder=fake_embedder, store=local_store)
    assert out2["version"] == "conclave-v2"


def test_should_compile_threshold(tmp_path, cfg, seed_runs):
    assert cc.should_compile(tmp_path, cfg(compile_new_examples=5), "proj") is False
    seed_runs(tmp_path, "proj", 6)
    assert cc.should_compile(tmp_path, cfg(compile_new_examples=5), "proj") is True


def test_eval_compiled_resolved_rate(tmp_path, cfg):
    ms = MetricsStore(tmp_path, "proj")
    ms.append_run({"run_id": "a"})
    ms.record_outcome("a", "resolved")
    ms.append_run({"run_id": "b"})
    ms.record_outcome("b", "unresolved")
    assert cc.eval_compiled(tmp_path, "proj") == 0.5
