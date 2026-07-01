import hashlib

import pytest

from conclave.store import LocalStore, S3Store


def test_put_get_manifest_roundtrip(tmp_path):
    s = LocalStore(tmp_path / "store")
    meta = s.put("corpus/v1.part-001.gz", b"hello")
    assert meta["sha256"] == hashlib.sha256(b"hello").hexdigest() and meta["bytes"] == 5
    assert s.get("corpus/v1.part-001.gz") == b"hello"
    assert "corpus/v1.part-001.gz" in s.list()
    assert s.manifest()[0]["key"] == "corpus/v1.part-001.gz"


def test_put_is_idempotent_on_key(tmp_path):
    s = LocalStore(tmp_path / "store")
    s.put("k", b"one")
    s.put("k", b"two")
    assert s.get("k") == b"two" and len([m for m in s.manifest() if m["key"] == "k"]) == 1


def test_get_verifies_sha(tmp_path):
    s = LocalStore(tmp_path / "store")
    s.put("k", b"abc")
    (tmp_path / "store" / "k").write_bytes(b"tampered")
    with pytest.raises(ValueError):
        s.get("k")


def test_s3_stub_is_explicit():
    with pytest.raises(NotImplementedError):
        S3Store()
