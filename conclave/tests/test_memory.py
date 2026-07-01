from conclave.memory import Memory


def test_note_then_recall(tmp_path, fake_index, fake_embedder):
    m = Memory(tmp_path, "proj", fake_index, fake_embedder)
    assert m.note("smp-race", "use a spinlock around X", verified=True) is True
    assert m.note("bad", "guess", verified=False) is False        # unverified rejected
    hits = m.recall("race condition in X", k=3)
    assert any("spinlock" in h["lesson"] for h in hits)


def test_corpus_note_stores_outside_projects(tmp_path, fake_index, fake_embedder):
    Memory(tmp_path, "corpus", fake_index, fake_embedder).note("sig", "shared lesson", verified=True)
    assert list((tmp_path / "data" / "corpus" / "lessons").glob("*.md"))
    assert not (tmp_path / "data" / "projects" / "corpus").exists()


def test_verified_note_writes_lesson_file(tmp_path, fake_index, fake_embedder):
    m = Memory(tmp_path, "proj", fake_index, fake_embedder)
    m.note("boot-abi-drift", "bump BOOT_INFO_VERSION in both headers", verified=True)
    files = list((tmp_path / "data/projects/proj/lessons").glob("*.md"))
    assert files and "BOOT_INFO_VERSION" in files[0].read_text()
