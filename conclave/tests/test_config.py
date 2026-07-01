from conclave import config

_TOML = (
    '[runtime]\nmax_calls=3\nmin_credits=2.0\nworkers=6\n'
    'model_timeout_s=480\njudge_timeout_s=480\n'
    '[panel]\nmodels=["a","b"]\nweb_search=false\n'
    '[judge]\nmodel="j"\n'
    '[embedders]\nmodels=["e1","e2"]\ndefault="e1"\nensemble_rrf=false\nhybrid_bm25=true\n'
    '[compile]\nevery_days=7\nor_new_examples=200\nshard_max_mib=90\n'
    '[index]\nbackend="lancedb"\n[store]\nbackend="git-releases"\n'
)


def test_load_reads_toml_and_secret(tmp_path, monkeypatch):
    (tmp_path / "conclave.toml").write_text(_TOML)
    (tmp_path / "secret").write_text("sk-or-v1-xxx\n")
    monkeypatch.setenv("CONCLAVE_ENABLED", "1")
    c = config.load(tmp_path)
    assert c.panel == ["a", "b"] and c.judge == "j"
    assert c.embed_default == "e1" and c.embedders == ["e1", "e2"]
    assert c.max_calls == 3 and c.enabled is True and c.secret == "sk-or-v1-xxx"
    assert c.index_backend == "lancedb" and c.store_backend == "git-releases"


def test_disabled_without_env(tmp_path, monkeypatch):
    (tmp_path / "conclave.toml").write_text(
        '[runtime]\n[panel]\nmodels=[]\n[judge]\nmodel="j"\n'
        '[embedders]\nmodels=[]\ndefault=""\n[compile]\n[index]\n[store]\n')
    monkeypatch.delenv("CONCLAVE_ENABLED", raising=False)
    assert config.load(tmp_path).enabled is False


def test_missing_secret_is_none(tmp_path, monkeypatch):
    (tmp_path / "conclave.toml").write_text(_TOML)
    monkeypatch.delenv("CONCLAVE_ENABLED", raising=False)
    assert config.load(tmp_path).secret is None
