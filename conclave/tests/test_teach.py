from conclave.memory import Memory
from conclave.teach import teach


def test_corpus_namespace_stores_outside_projects(tmp_path, fake_index, fake_embedder):
    # The shared corpus is project-agnostic: it lives at data/corpus/, never data/projects/.
    f = tmp_path / "d.md"
    f.write_text("general conclave knowledge for everyone " * 10)
    teach(tmp_path, "corpus", f, distill=False, embedder=fake_embedder, index=fake_index)
    assert (tmp_path / "data" / "corpus" / "teach-index.json").exists()
    assert not (tmp_path / "data" / "projects" / "corpus").exists()


def test_taught_corpus_is_recallable(tmp_path, fake_index, fake_embedder):
    # Teaching must make content retrievable: recall reads the lesson text out of meta,
    # so corpus chunks have to carry their text there (else recall comes back blank).
    f = tmp_path / "doc.md"
    f.write_text("conclave is an apex escalation reasoning harness with a panel and an opus judge")
    teach(tmp_path, "proj", f, distill=False, embedder=fake_embedder, index=fake_index)
    hits = Memory(tmp_path, "proj", fake_index, fake_embedder).recall(
        "what is the apex escalation harness", k=3)
    assert any("panel and an opus judge" in h["lesson"] for h in hits)


def test_ingest_then_reteach_is_incremental(tmp_path, fake_index, fake_embedder):
    src = tmp_path / "specs"
    src.mkdir()
    (src / "a.md").write_text("uefi boot services exit memory map\n" * 40)
    r1 = teach(tmp_path, "proj", src, distill=False, embedder=fake_embedder, index=fake_index)
    assert r1["added"] > 0 and r1["updated"] == 0

    r2 = teach(tmp_path, "proj", src, distill=False, embedder=fake_embedder, index=fake_index)
    assert r2["added"] == 0 and r2["updated"] == 0 and r2["pruned"] == 0   # nothing changed

    (src / "a.md").write_text("totally different and much shorter content\n")
    r3 = teach(tmp_path, "proj", src, distill=False, embedder=fake_embedder, index=fake_index)
    assert r3["updated"] > 0 or (r3["added"] > 0 and r3["pruned"] > 0)


def test_single_file_source(tmp_path, fake_index, fake_embedder):
    f = tmp_path / "spec.md"
    f.write_text("apic timer deadline tsc rdtsc fence " * 30)
    r = teach(tmp_path, "proj", f, distill=False, embedder=fake_embedder, index=fake_index)
    assert r["added"] > 0
    assert fake_index.size() == r["added"]
