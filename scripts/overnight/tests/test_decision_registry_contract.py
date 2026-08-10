#!/usr/bin/env python3
"""v13 carry, fixed 2026-08-10: `decision-registry.py` reads the todo-graph
cache through the shared validator instead of a bare `json.loads`.

WHY IT MATTERED. Section 33 bumped `CACHE_FORMAT_VERSION` 2 -> 3 because
`stamps_xrefs[].target_path` KEPT ITS TYPE and CHANGED ITS MEANING. That is the
one shape a defensive reader cannot detect: every key present, every type right,
so an old-contract artifact parses cleanly and the reader publishes decision
records built on a stale meaning. It was the SECOND finding against this same
reader in two versions.

THE FIXTURE REUSES THE TOOL'S OWN MACHINERY, and four earlier attempts failed
for want of that. `cache_schema.sidecar_path` derives the sidecar name from the
CACHE SHA, so any reformatting of the cache (a `json.dumps` round-trip) renames
the sidecar it must be paired with -- every hand-rolled fixture silently landed
on "no corpus binding" and returned 0 records for the WRONG reason, which read
as a passing refusal control while proving nothing. Copy the cache BYTE FOR
BYTE; only the sidecar is authored.
"""
import importlib.util
import json
import pathlib
import shutil
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
REG = REPO / "scripts/overnight/decision-registry.py"
CACHE = REPO / "build/todo-cache.json"


def _mod(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    sys.modules[name] = m
    spec.loader.exec_module(m)
    return m


def _cs():
    sys.path.insert(0, str(REPO / "scripts/todo-graph"))
    return _mod(REPO / "scripts/todo-graph/cache_schema.py", "cache_schema")


def _fixture(version):
    """A repo-shaped root whose cache is byte-identical to the live one and
    whose sidecar declares `version`. Returns (tmpdir, root)."""
    cs = _cs()
    d = pathlib.Path(tempfile.mkdtemp(prefix="dr-contract."))
    root = d / "r"
    (root / "build").mkdir(parents=True)
    (root / "scripts").mkdir()
    # Symlinks: the corpus and the validator are read, never written.
    (root / "todo").symlink_to(REPO / "todo")
    (root / "scripts/todo-graph").symlink_to(REPO / "scripts/todo-graph")
    shutil.copy(CACHE, root / "build/todo-cache.json")          # BYTE-FOR-BYTE
    live_sha = cs.file_sha256(CACHE) if hasattr(cs, "file_sha256") else None
    if live_sha is None:
        import hashlib
        live_sha = hashlib.sha256(CACHE.read_bytes()).hexdigest()
    side_src = cs.sidecar_path(CACHE, live_sha)
    side_dst = cs.sidecar_path(root / "build/todo-cache.json", live_sha)
    body = json.loads(side_src.read_text())
    body["cache_format_version"] = version
    side_dst.write_text(json.dumps(body))
    return d, root


def test_valid_contract_publishes_records():
    """CONTROL, and it must be NON-ZERO or the refusal test below is vacuous --
    which is exactly how the first four attempts at this fixture failed."""
    cs = _cs()
    d, root = _fixture(cs.CACHE_FORMAT_VERSION)
    try:
        n = len(_mod(REG, "dr_ok").extract_stamp_xrefs(root))
        assert n > 0, (
            "the control published NO records, so this fixture proves nothing "
            "about the refusal below. Check the cache/sidecar pairing first.")
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_stale_contract_publishes_nothing():
    """REFUSAL CONTROL. A prior-version contract must yield ZERO records rather
    than records built on a stale meaning."""
    cs = _cs()
    d, root = _fixture(cs.CACHE_FORMAT_VERSION - 1)
    try:
        n = len(_mod(REG, "dr_stale").extract_stamp_xrefs(root))
        assert n == 0, f"published {n} records from a stale-contract artifact"
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_no_bare_cache_parse_remains():
    """The structural half: the bare read must not creep back. This is a PIN
    (it passes whether or not the runtime behaviour is right), so it is labelled
    as one and the two tests above carry the actual evidence."""
    src = REG.read_text()
    assert "load_and_validate" in src and "PROFILE_STAMP_XREFS" in src, \
        "the reader must load through the shared validator with a profile"
    assert 'json.loads((root / "build/todo-cache.json")' not in src, \
        "the bare unvalidated cache read is back"


if __name__ == "__main__":
    fails = []
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"  PASS {name}")
            except Exception as exc:
                fails.append(name)
                print(f"  FAIL {name}: {exc}")
    sys.exit(1 if fails else 0)
