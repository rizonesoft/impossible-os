from conclave import cli


def test_doctor_runs_offline(tmp_path, monkeypatch, capsys, make_repo):
    monkeypatch.chdir(make_repo(tmp_path))
    rc = cli.main(["doctor"])
    out = capsys.readouterr().out
    assert rc in (0, 1) and "secret" in out


def test_recall_is_panel_free(tmp_path, monkeypatch, capsys, make_repo):
    # recall must never invoke the panel.
    monkeypatch.setattr("conclave.harness.panel.run",
                        lambda *a, **k: (_ for _ in ()).throw(AssertionError("panel called")))
    monkeypatch.chdir(make_repo(tmp_path))
    assert cli.main(["recall", "race in X", "--project", "proj"]) == 0


def test_stats_no_runs(tmp_path, monkeypatch, capsys, make_repo):
    monkeypatch.chdir(make_repo(tmp_path))
    assert cli.main(["stats", "--project", "proj"]) == 0
    assert "no runs" in capsys.readouterr().out


def test_poll_and_list(tmp_path, monkeypatch, capsys, make_repo):
    import json
    root = make_repo(tmp_path)
    jdir = root / "data" / "projects" / "default" / "jobs"
    jdir.mkdir(parents=True)
    (jdir / "j1.meta.json").write_text(json.dumps({"id": "j1", "status": "pending", "target": "t"}))
    monkeypatch.chdir(root)
    assert cli.main(["list"]) == 0 and "j1" in capsys.readouterr().out
    assert cli.main(["poll", "j1"]) == 0 and "PENDING" in capsys.readouterr().out


def test_teach_corpus_flag_routes_to_corpus(tmp_path, monkeypatch, capsys, make_repo,
                                            fake_index, fake_embedder):
    root = make_repo(tmp_path)
    monkeypatch.chdir(root)
    monkeypatch.setattr(cli, "_deps", lambda r, c: (fake_index, fake_embedder))
    (root / "d.md").write_text("general conclave knowledge " * 10)
    assert cli.main(["teach", "--corpus", "--source", "d.md"]) == 0
    assert (root / "data" / "corpus" / "teach-index.json").exists()
    assert not (root / "data" / "projects" / "corpus").exists()


def test_unknown_command_errors(tmp_path, monkeypatch, make_repo):
    monkeypatch.chdir(make_repo(tmp_path))
    assert cli.main(["frobnicate"]) == 1
