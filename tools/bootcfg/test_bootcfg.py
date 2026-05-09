#!/usr/bin/env python3
"""Self-contained tests for tools/bootcfg/bootcfg.py.

Mirrors the tools/boot-entry-validate/test_validate.py pattern: no pytest
dependency, runs bootcfg as a subprocess against tempfiles, exits 0 on all
pass and 1 on any failure.

Usage:
  python3 tools/bootcfg/test_bootcfg.py
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
BOOTCFG = REPO_ROOT / "tools" / "bootcfg" / "bootcfg.py"
VALIDATE = REPO_ROOT / "tools" / "boot-entry-validate" / "validate.py"

PASS_COUNT = 0
FAIL_COUNT = 0


def _run(args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(BOOTCFG), *args],
        capture_output=True,
        text=True,
    )


def _run_validate(path: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(VALIDATE), str(path)],
        capture_output=True,
        text=True,
    )


def _emit_seed(p: Path) -> None:
    cp = _run(["emit-seed", str(p)])
    assert cp.returncode == 0, cp.stderr


def _seed_extra_entry(eid: str, sort_key: str = "01-extra",
                      kernel: str = "\\EFI\\ImpossibleOS\\kernel.exe",
                      flags: list[str] | None = None) -> dict:
    return {
        "id": eid,
        "title": eid.title(),
        "kind": "split",
        "flags": flags if flags is not None else ["active"],
        "sort_key": sort_key,
        "machine_id": "00000000-0000-0000-0000-000000000000",
        "policy_tags": [],
        "payload": {"kernel": kernel, "cmdline": "", "root": "A"},
    }


def case(name: str):
    """Decorator: run the function in a fresh tempdir + tempfile path."""
    def deco(fn):
        global PASS_COUNT, FAIL_COUNT
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "bootentries.json"
            try:
                fn(p)
                PASS_COUNT += 1
                print(f"[OK]   {name}")
            except AssertionError as e:
                FAIL_COUNT += 1
                print(f"[FAIL] {name}: {e}")
            except Exception as e:
                FAIL_COUNT += 1
                print(f"[FAIL] {name}: unexpected: {type(e).__name__}: {e}")
        return fn
    return deco


# ---- emit-seed --------------------------------------------------------------

@case("emit-seed: idempotent (writing twice yields byte-identical store)")
def t_emit_seed_idempotent(p: Path) -> None:
    _emit_seed(p)
    first = p.read_bytes()
    _emit_seed(p)
    second = p.read_bytes()
    assert first == second, "byte-mismatch between two emit-seed runs"


@case("emit-seed: validator accepts the output")
def t_emit_seed_validates(p: Path) -> None:
    _emit_seed(p)
    cp = _run_validate(p)
    assert cp.returncode == 0, cp.stderr


@case("emit-seed: kernel path matches bootloader fallback")
def t_emit_seed_canonical_kernel_path(p: Path) -> None:
    _emit_seed(p)
    data = json.loads(p.read_text())
    got = data["entries"][0]["payload"]["kernel"]
    want = "\\EFI\\ImpossibleOS\\kernel.exe"
    assert got == want, f"kernel path drift: got {got!r}, want {want!r}"


# ---- list -------------------------------------------------------------------

@case("list: shows seeded entry id + kind")
def t_list_shows_entries(p: Path) -> None:
    _emit_seed(p)
    cp = _run(["list", str(p)])
    assert cp.returncode == 0, cp.stderr
    assert "id='default'" in cp.stdout, cp.stdout
    assert "kind='split'" in cp.stdout, cp.stdout


# ---- add --------------------------------------------------------------------

@case("add: appends new entry + revalidates")
def t_add_appends(p: Path) -> None:
    _emit_seed(p)
    cp = _run(["add", str(p), "--json", json.dumps(_seed_extra_entry("slot-b"))])
    assert cp.returncode == 0, cp.stderr
    cp = _run_validate(p)
    assert cp.returncode == 0, cp.stderr
    data = json.loads(p.read_text())
    assert {e["id"] for e in data["entries"]} == {"default", "slot-b"}


@case("add: rejects duplicate id (rc=3)")
def t_add_dup(p: Path) -> None:
    _emit_seed(p)
    cp = _run(["add", str(p), "--json", json.dumps(_seed_extra_entry("default"))])
    assert cp.returncode == 3, f"expected rc=3, got {cp.returncode}\n{cp.stderr}"
    assert "already exists" in cp.stderr, cp.stderr


@case("add: validator rejection leaves on-disk store untouched")
def t_add_invalid_keeps_store(p: Path) -> None:
    _emit_seed(p)
    pre = p.read_bytes()
    bad = _seed_extra_entry("bad", kernel="\\EFI\\..\\..\\evil.exe")
    cp = _run(["add", str(p), "--json", json.dumps(bad)])
    assert cp.returncode == 1, f"expected rc=1, got {cp.returncode}\n{cp.stderr}"
    post = p.read_bytes()
    assert pre == post, "store mutated despite validator rejection"


# ---- remove -----------------------------------------------------------------

@case("remove: drops named entry")
def t_remove_drops(p: Path) -> None:
    _emit_seed(p)
    _run(["add", str(p), "--json", json.dumps(_seed_extra_entry("extra"))])
    cp = _run(["remove", str(p), "extra"])
    assert cp.returncode == 0, cp.stderr
    data = json.loads(p.read_text())
    assert {e["id"] for e in data["entries"]} == {"default"}
    assert _run_validate(p).returncode == 0


@case("remove: missing id returns rc=3")
def t_remove_missing(p: Path) -> None:
    _emit_seed(p)
    cp = _run(["remove", str(p), "nonexistent"])
    assert cp.returncode == 3, cp.stderr
    assert "not found" in cp.stderr, cp.stderr


@case("remove: last entry rejected (>=1 entry required)")
def t_remove_last(p: Path) -> None:
    _emit_seed(p)
    cp = _run(["remove", str(p), "default"])
    assert cp.returncode == 1, cp.stderr
    out = (cp.stderr + cp.stdout).lower()
    assert "empty" in out or "1 entry" in cp.stderr


# ---- set-default ------------------------------------------------------------

@case("set-default: reassigns sort_keys so target wins lex tie-break")
def t_set_default(p: Path) -> None:
    _emit_seed(p)
    _run(["add", str(p), "--json", json.dumps(_seed_extra_entry("slot-b"))])
    cp = _run(["set-default", str(p), "slot-b"])
    assert cp.returncode == 0, cp.stderr
    data = json.loads(p.read_text())
    by_id = {e["id"]: e for e in data["entries"]}
    assert by_id["slot-b"]["sort_key"] == "00-slot-b", by_id
    assert by_id["default"]["sort_key"] != "00-default", by_id
    sorted_entries = sorted(data["entries"], key=lambda e: e["sort_key"])
    assert sorted_entries[0]["id"] == "slot-b", sorted_entries
    assert _run_validate(p).returncode == 0


@case("set-default: rejects inactive id (would be filtered by ladder)")
def t_set_default_inactive(p: Path) -> None:
    _emit_seed(p)
    inactive = _seed_extra_entry("inactive", sort_key="01-inactive", flags=[])
    _run(["add", str(p), "--json", json.dumps(inactive)])
    cp = _run(["set-default", str(p), "inactive"])
    assert cp.returncode == 1, cp.stderr
    assert "not active" in cp.stderr, cp.stderr


@case("set-default: missing id returns rc=3")
def t_set_default_missing(p: Path) -> None:
    _emit_seed(p)
    cp = _run(["set-default", str(p), "nonexistent"])
    assert cp.returncode == 3, cp.stderr
    assert "not found" in cp.stderr


# ---- atomic write -----------------------------------------------------------

@case("atomic write: no .new tempfiles linger on success")
def t_atomic_clean(p: Path) -> None:
    _emit_seed(p)
    _run(["add", str(p), "--json", json.dumps(_seed_extra_entry("extra"))])
    leftovers = list(p.parent.glob("bootentries.json.*"))
    assert leftovers == [], f"tempfiles linger: {leftovers}"


@case("atomic write: tempfile cleaned + store untouched on validator reject")
def t_atomic_clean_on_reject(p: Path) -> None:
    _emit_seed(p)
    pre_size = p.stat().st_size
    bad = _seed_extra_entry("bad", kernel="\\EFI\\..\\..\\evil.exe")
    cp = _run(["add", str(p), "--json", json.dumps(bad)])
    assert cp.returncode == 1, cp.stderr
    leftovers = list(p.parent.glob("bootentries.json.*"))
    assert leftovers == [], f"tempfile not cleaned: {leftovers}"
    assert p.stat().st_size == pre_size, "store size mutated despite reject"


# ---- ABI-cap parity (validator must reject what the parser would reject) ---

@case("validator rejects over-cap sort_key (>63 bytes)")
def t_sort_key_overflow(p: Path) -> None:
    _emit_seed(p)
    bad = _seed_extra_entry("oversize")
    bad["sort_key"] = "x" * 64  # MAX_SORT_KEY_LEN is 63
    pre = p.read_bytes()
    cp = _run(["add", str(p), "--json", json.dumps(bad)])
    assert cp.returncode == 1, f"expected rc=1, got {cp.returncode}\n{cp.stderr}"
    assert "MAX_SORT_KEY_LEN" in cp.stderr, cp.stderr
    assert p.read_bytes() == pre, "store mutated despite reject"


@case("validator rejects over-cap policy_tags array (>4)")
def t_policy_tags_overflow(p: Path) -> None:
    _emit_seed(p)
    bad = _seed_extra_entry("toomany")
    bad["policy_tags"] = ["a", "b", "c", "d", "e"]  # MAX_POLICY_TAGS is 4
    pre = p.read_bytes()
    cp = _run(["add", str(p), "--json", json.dumps(bad)])
    assert cp.returncode == 1, cp.stderr
    assert "MAX_POLICY_TAGS" in cp.stderr, cp.stderr
    assert p.read_bytes() == pre


@case("validator rejects over-cap individual policy_tag (>23 bytes)")
def t_policy_tag_len_overflow(p: Path) -> None:
    _emit_seed(p)
    bad = _seed_extra_entry("longtag")
    bad["policy_tags"] = ["x" * 24]  # MAX_POLICY_TAG_LEN is 23
    pre = p.read_bytes()
    cp = _run(["add", str(p), "--json", json.dumps(bad)])
    assert cp.returncode == 1, cp.stderr
    assert "MAX_POLICY_TAG_LEN" in cp.stderr, cp.stderr
    assert p.read_bytes() == pre


# ---- usage ------------------------------------------------------------------

@case("missing path returns rc=2 (usage error)")
def t_missing_path(p: Path) -> None:
    # p is the tempdir's /bootentries.json which we never created
    cp = _run(["list", str(p)])
    assert cp.returncode == 2, cp.stderr
    assert "not a file" in cp.stderr, cp.stderr


# ---- summary ----------------------------------------------------------------

print(f"\n{PASS_COUNT}/{PASS_COUNT + FAIL_COUNT} bootcfg tests passed")
sys.exit(1 if FAIL_COUNT else 0)
