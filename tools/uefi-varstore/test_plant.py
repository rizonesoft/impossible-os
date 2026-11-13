"""Tests for plant.py against the distro's own OVMF variable store."""
import os
import struct
import sys
import uuid

import pytest

sys.path.insert(0, os.path.dirname(__file__))
import plant  # noqa: E402

STORE = "/usr/share/OVMF/OVMF_VARS_4M.fd"
GUID = uuid.UUID("494d504f-5354-4f53-504f-535447554944")
NAME = "ImpossibleCrashLog"

pytestmark = pytest.mark.skipif(not os.path.exists(STORE), reason="no OVMF store on this host")


def _fresh() -> bytes:
    return open(STORE, "rb").read()


def test_planted_variable_reads_back():
    out = plant.plant(_fresh(), NAME, GUID, struct.pack("<Q", 0xC01000), 0x7)
    recs = list(plant.records(out))
    assert [(r[1], r[2], r[3], r[4], r[5]) for r in recs] == [
        (plant.VAR_ADDED, 0x7, NAME, GUID, struct.pack("<Q", 0xC01000))]
    assert len(out) == len(_fresh())


def test_rewrite_deletes_the_old_record():
    once = plant.plant(_fresh(), NAME, GUID, struct.pack("<Q", 1), 0x7)
    twice = plant.plant(once, NAME, GUID, struct.pack("<Q", 2), 0x7)
    states = [(r[1], r[5]) for r in plant.records(twice)]
    assert states == [(plant.VAR_DELETED, struct.pack("<Q", 1)),
                      (plant.VAR_ADDED, struct.pack("<Q", 2))]
    second = [r[0] for r in plant.records(twice)][1]
    assert second % 4 == 0


def test_refuses_a_file_that_is_not_a_store():
    with pytest.raises(plant.StoreError):
        plant.plant(bytes(4096), NAME, GUID, b"x", 0x7)


def test_refuses_when_the_store_is_full():
    data = bytearray(_fresh())
    first, end = plant._store_bounds(bytes(data))
    data[first:end] = b"\0" * (end - first)  # not erased, no valid record either
    with pytest.raises(plant.StoreError):
        plant.plant(bytes(data), NAME, GUID, b"x" * 8, 0x7)


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q"]))
