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


@case("emit-seed: kernel path matches release staged path")
def t_emit_seed_canonical_kernel_path(p: Path) -> None:
    _emit_seed(p)
    data = json.loads(p.read_text())
    # slot-a is the first entry and the active default
    slot_a = next(e for e in data["entries"] if e["id"] == "slot-a")
    got = slot_a["payload"]["kernel"]
    want = "\\boot\\kernel.exe"
    assert got == want, f"kernel path drift: got {got!r}, want {want!r}"


@case("emit-seed: 3-entry default (slot-a + slot-b + recovery)")
def t_emit_seed_three_entries(p: Path) -> None:
    _emit_seed(p)
    data = json.loads(p.read_text())
    ids = [e["id"] for e in data["entries"]]
    assert ids == ["slot-a", "slot-b", "recovery"], ids
    by_id = {e["id"]: e for e in data["entries"]}
    assert by_id["slot-a"]["kind"] == "split"
    assert by_id["slot-a"]["flags"] == ["active"]
    assert by_id["slot-b"]["kind"] == "split"
    # slot-b ships inactive until dual-slot layout owner widens it
    assert by_id["slot-b"]["flags"] == [], by_id["slot-b"]["flags"]
    assert by_id["recovery"]["kind"] == "recovery"
    assert by_id["recovery"]["flags"] == ["active"]
    # recovery_partition_guid is the canonical placeholder
    assert by_id["recovery"]["payload"]["recovery_partition_guid"] == \
        "49504F53-7265-636F-7665-727900000001"


# ---- list -------------------------------------------------------------------

@case("list: shows all three seeded entry ids + kinds")
def t_list_shows_entries(p: Path) -> None:
    _emit_seed(p)
    cp = _run(["list", str(p)])
    assert cp.returncode == 0, cp.stderr
    assert "id='slot-a'" in cp.stdout, cp.stdout
    assert "id='slot-b'" in cp.stdout, cp.stdout
    assert "id='recovery'" in cp.stdout, cp.stdout
    assert "kind='split'" in cp.stdout, cp.stdout
    assert "kind='recovery'" in cp.stdout, cp.stdout


# ---- add --------------------------------------------------------------------

@case("add: appends new entry + revalidates")
def t_add_appends(p: Path) -> None:
    _emit_seed(p)
    cp = _run(["add", str(p), "--json", json.dumps(_seed_extra_entry("extra"))])
    assert cp.returncode == 0, cp.stderr
    cp = _run_validate(p)
    assert cp.returncode == 0, cp.stderr
    data = json.loads(p.read_text())
    assert {e["id"] for e in data["entries"]} == {"slot-a", "slot-b", "recovery", "extra"}


@case("add: rejects duplicate id (rc=3)")
def t_add_dup(p: Path) -> None:
    _emit_seed(p)
    cp = _run(["add", str(p), "--json", json.dumps(_seed_extra_entry("slot-a"))])
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
    assert {e["id"] for e in data["entries"]} == {"slot-a", "slot-b", "recovery"}
    assert _run_validate(p).returncode == 0


@case("remove: missing id returns rc=3")
def t_remove_missing(p: Path) -> None:
    _emit_seed(p)
    cp = _run(["remove", str(p), "nonexistent"])
    assert cp.returncode == 3, cp.stderr
    assert "not found" in cp.stderr, cp.stderr


@case("remove: last entry rejected (>=1 entry required)")
def t_remove_last(p: Path) -> None:
    # Drain the 3-entry seed down to one entry, then attempt to remove it.
    _emit_seed(p)
    _run(["remove", str(p), "recovery"])
    _run(["remove", str(p), "slot-b"])
    cp = _run(["remove", str(p), "slot-a"])
    assert cp.returncode == 1, cp.stderr
    out = (cp.stderr + cp.stdout).lower()
    assert "empty" in out or "1 entry" in cp.stderr


# ---- set-default ------------------------------------------------------------

@case("set-default: reassigns sort_keys so target wins lex tie-break")
def t_set_default(p: Path) -> None:
    # Add a fresh active entry so the test does not depend on which seed
    # entry is currently the default; slot-a starts as the default.
    _emit_seed(p)
    _run(["add", str(p), "--json", json.dumps(_seed_extra_entry("custom"))])
    cp = _run(["set-default", str(p), "custom"])
    assert cp.returncode == 0, cp.stderr
    data = json.loads(p.read_text())
    by_id = {e["id"]: e for e in data["entries"]}
    assert by_id["custom"]["sort_key"] == "00-custom", by_id
    # slot-a was renumbered out of the 00- slot
    assert by_id["slot-a"]["sort_key"] != "00-slot-a", by_id
    sorted_entries = sorted(data["entries"], key=lambda e: e["sort_key"])
    assert sorted_entries[0]["id"] == "custom", sorted_entries
    assert _run_validate(p).returncode == 0


@case("set-default: rejects seeded slot-b (inactive in seed)")
def t_set_default_inactive(p: Path) -> None:
    # slot-b ships inactive in the 3-entry seed; set-default must refuse
    # to promote it because the ladder filters non-active entries before
    # the lowest-sort_key tie-break runs.
    _emit_seed(p)
    cp = _run(["set-default", str(p), "slot-b"])
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


# ---- ASCII-clean parity (Codex consistency H finding) ----------------------

@case("validator rejects sort_key with non-ASCII (would expand on JSON serialize)")
def t_sort_key_nonascii(p: Path) -> None:
    _emit_seed(p)
    bad = _seed_extra_entry("nonascii")
    bad["sort_key"] = "01-cafe-é"  # one non-ASCII char would become é escapes
    pre = p.read_bytes()
    cp = _run(["add", str(p), "--json", json.dumps(bad)])
    assert cp.returncode == 1, cp.stderr
    assert "printable ASCII" in cp.stderr, cp.stderr
    assert p.read_bytes() == pre


@case("validator rejects title with non-ASCII (escape would expand)")
def t_title_nonascii(p: Path) -> None:
    _emit_seed(p)
    bad = _seed_extra_entry("noasciititle")
    bad["title"] = "Café"  # one non-ASCII char would JSON-escape to \\u00e9
    pre = p.read_bytes()
    cp = _run(["add", str(p), "--json", json.dumps(bad)])
    assert cp.returncode == 1, cp.stderr
    assert "printable ASCII" in cp.stderr, cp.stderr
    assert p.read_bytes() == pre


@case("validator rejects policy_tag with backslash (escape would expand)")
def t_policy_tag_escape(p: Path) -> None:
    _emit_seed(p)
    bad = _seed_extra_entry("escape")
    bad["policy_tags"] = ["a\\b"]  # backslash forces JSON escape -> raw byte mismatch
    pre = p.read_bytes()
    cp = _run(["add", str(p), "--json", json.dumps(bad)])
    assert cp.returncode == 1, cp.stderr
    assert "printable ASCII" in cp.stderr, cp.stderr
    assert p.read_bytes() == pre


# ---- emit-seed wildcard (Codex consistency H finding) ----------------------

@case("emit-seed uses empty machine_id wildcard (not all-zero UUID)")
def t_seed_machine_id_wildcard(p: Path) -> None:
    _emit_seed(p)
    data = json.loads(p.read_text())
    # Every seeded entry must be wildcard so the seed admits any host.
    for e in data["entries"]:
        mid = e["machine_id"]
        assert mid == "", (
            f"seed machine_id for {e['id']!r} must be empty wildcard for "
            f"the policy ladder to admit it on any machine; got {mid!r}"
        )


# ---- pre-read size cap (Codex perf M finding) -------------------------------

@case("pre-read size cap rejects oversized file before parse work")
def t_oversize_pre_read(p: Path) -> None:
    # Write a >16 KiB file. Validator MAX_TOTAL_BYTES is 16 KiB.
    p.write_bytes(b"X" * (17 * 1024))
    cp = _run(["list", str(p)])
    assert cp.returncode == 1, cp.stderr
    assert "MAX_TOTAL_BYTES" in cp.stderr, cp.stderr
    assert "rejected pre-read" in cp.stderr, cp.stderr


# ---- usage ------------------------------------------------------------------

@case("missing path returns rc=2 (usage error)")
def t_missing_path(p: Path) -> None:
    # p is the tempdir's /bootentries.json which we never created
    cp = _run(["list", str(p)])
    assert cp.returncode == 2, cp.stderr
    assert "not a file" in cp.stderr, cp.stderr


@case("write failure on read-only directory returns rc=2 (filesystem error)")
def t_write_failure_rc2(p: Path) -> None:
    """Filesystem-level write failure must map to rc=2 (usage error),
    not rc=1 (validation failure). A read-only parent directory is the
    canonical case."""
    import stat
    _emit_seed(p)
    # Make the parent directory read-only (chmod 0o555). The atomic
    # write writes a tempfile there + os.replace; tempfile creation
    # fails with PermissionError when the directory has no write bit.
    try:
        parent = p.parent
        original_mode = parent.stat().st_mode
        parent.chmod(0o555)
        try:
            new_entry = _seed_extra_entry("rotest")
            cp = _run(["add", str(p), "--json", json.dumps(new_entry)])
            # rc=2 (usage error). On root-as-uid systems chmod 0o555
            # is ignored; in that case skip the assertion.
            if cp.returncode == 0:
                # Root: write succeeded despite read-only flag. Skip.
                return
            assert cp.returncode == 2, (
                f"write failure should be rc=2, got {cp.returncode}\n"
                f"{cp.stderr}"
            )
            assert "write failed" in cp.stderr or "stat failed" in cp.stderr, cp.stderr
        finally:
            parent.chmod(original_mode)
    except OSError:
        # Some test environments cannot chmod -- skip the case.
        pass


# ---- mutation log -----------------------------------------------------------

@case("mutation-log: omitted -> no file created")
def t_mutation_log_omitted(p: Path) -> None:
    _emit_seed(p)
    sibling = p.parent / "mutations.jsonl"
    assert not sibling.exists(), "no mutation log expected when --mutation-log omitted"


@case("mutation-log: emit-seed appends one record (prior_crc=null on fresh)")
def t_mutation_log_emit_seed_fresh(p: Path) -> None:
    log = p.parent / "mutations.jsonl"
    cp = _run(["emit-seed", str(p), "--mutation-log", str(log)])
    assert cp.returncode == 0, cp.stderr
    assert log.exists(), "mutation log not created"
    lines = log.read_text().strip().splitlines()
    assert len(lines) == 1, f"expected 1 record, got {len(lines)}"
    rec = json.loads(lines[0])
    assert rec["kind"] == "emit-seed"
    # target_id="default" is the v1 schema-pinned stable label for the
    # seed action; it is NOT an entry id (the 3-entry seed contains
    # slot-a / slot-b / recovery). Pinned in docs/boot/boot-history-schema.md.
    assert rec["target_id"] == "default"
    assert rec["prior_crc"] is None, "fresh emit-seed should record prior_crc=null"
    assert rec["new_crc"].startswith("0x"), rec["new_crc"]
    assert rec["schema"] == 1
    assert "user=" in rec["requester"]


@case("mutation-log: idempotent emit-seed records prior_crc == new_crc on second run")
def t_mutation_log_emit_seed_idempotent(p: Path) -> None:
    log = p.parent / "mutations.jsonl"
    _run(["emit-seed", str(p), "--mutation-log", str(log)])
    cp = _run(["emit-seed", str(p), "--mutation-log", str(log)])
    assert cp.returncode == 0
    lines = log.read_text().strip().splitlines()
    assert len(lines) == 2, f"expected 2 records, got {len(lines)}"
    rec1 = json.loads(lines[0])
    rec2 = json.loads(lines[1])
    assert rec2["prior_crc"] == rec1["new_crc"], "second run should see first run's crc"
    assert rec2["new_crc"] == rec1["new_crc"], "idempotent seed: same crc both runs"


@case("mutation-log: add appends a record with the new id")
def t_mutation_log_add(p: Path) -> None:
    log = p.parent / "mutations.jsonl"
    _emit_seed(p)
    cp = _run(["add", str(p), "--json", json.dumps(_seed_extra_entry("extra")),
               "--mutation-log", str(log)])
    assert cp.returncode == 0
    lines = log.read_text().strip().splitlines()
    assert len(lines) == 1, f"expected 1 record, got {len(lines)}"
    rec = json.loads(lines[0])
    assert rec["kind"] == "add"
    assert rec["target_id"] == "extra"
    assert rec["prior_crc"] != rec["new_crc"], "add changes the store CRC"


@case("mutation-log: remove appends a record after a successful remove")
def t_mutation_log_remove(p: Path) -> None:
    log = p.parent / "mutations.jsonl"
    _emit_seed(p)
    _run(["add", str(p), "--json", json.dumps(_seed_extra_entry("extra"))])
    cp = _run(["remove", str(p), "extra", "--mutation-log", str(log)])
    assert cp.returncode == 0
    lines = log.read_text().strip().splitlines()
    assert len(lines) == 1, f"expected 1 record, got {len(lines)}"
    rec = json.loads(lines[0])
    assert rec["kind"] == "remove"
    assert rec["target_id"] == "extra"


@case("mutation-log: failed mutation does NOT append a record")
def t_mutation_log_skips_on_failure(p: Path) -> None:
    log = p.parent / "mutations.jsonl"
    _emit_seed(p)
    cp = _run(["remove", str(p), "nonexistent", "--mutation-log", str(log)])
    assert cp.returncode == 3, cp.stderr
    assert not log.exists(), "no mutation log expected when remove fails"


@case("mutation-log: set-default emits note carrying the assigned sort_key")
def t_mutation_log_set_default(p: Path) -> None:
    log = p.parent / "mutations.jsonl"
    _emit_seed(p)
    _run(["add", str(p), "--json", json.dumps(_seed_extra_entry("custom"))])
    cp = _run(["set-default", str(p), "custom", "--mutation-log", str(log)])
    assert cp.returncode == 0
    lines = log.read_text().strip().splitlines()
    assert len(lines) == 1, f"expected 1 record, got {len(lines)}"
    rec = json.loads(lines[0])
    assert rec["kind"] == "set-default"
    assert rec["target_id"] == "custom"
    assert "note" in rec and "00-custom" in rec["note"], rec.get("note", "<missing>")


@case("write path: a literal non-ASCII key survives serialization and reloads")
def t_non_ascii_key_round_trip(p: Path) -> None:
    """Regression: _canonical_dumps used ensure_ascii=True, so a literal
    non-ASCII extension key was rewritten as an ESCAPED key -- which the
    firmware hard-rejects (BOOT_ENTRIES_REJECT_ESCAPED_KEY) and which bootcfg
    itself then refused to reload. The editor could write a store nothing could
    read, because _validate_then_write only inspected the dict, never the bytes."""
    _emit_seed(p)
    entry = _seed_extra_entry("accented")
    entry["café"] = 1                      # literal non-ASCII extension key
    cp = _run(["add", str(p), "--json", json.dumps(entry, ensure_ascii=False)])
    assert cp.returncode == 0, cp.stderr
    raw = p.read_bytes()
    assert "café".encode("utf-8") in raw, "key must be written literally"
    assert b"caf\\u00e9" not in raw, "key must NOT be escaped on the way out"
    # The store bootcfg just wrote must be one bootcfg and the validator accept.
    assert _run(["list", str(p)]).returncode == 0, "written store must reload"
    assert _run_validate(p).returncode == 0, "written store must validate"


@case("store read: a duplicate key is rejected, not silently collapsed")
def t_load_rejects_duplicate_key(p: Path) -> None:
    """bootcfg used to read stores through a bare json.loads, which keeps the
    LAST of a repeated key. The firmware rejects such a store outright, so
    bootcfg could read, rewrite and re-bless a file that cannot boot. Reverting
    _load to json.loads must fail here."""
    _emit_seed(p)
    text = p.read_text()
    p.write_text(text.replace("{", '{"schema_version": 1, ', 1))
    before = p.read_bytes()
    cp = _run(["list", str(p)])
    assert cp.returncode != 0, "duplicate key must be rejected on read"
    assert "repeated key" in cp.stderr, cp.stderr
    assert p.read_bytes() == before, "a rejected read must not rewrite the store"


@case("store read: an escaped key name is rejected")
def t_load_rejects_escaped_key(p: Path) -> None:
    _emit_seed(p)
    text = p.read_text()
    p.write_text(text.replace('"schema_version"', '"\\u0073chema_version"', 1))
    cp = _run(["list", str(p)])
    assert cp.returncode != 0, "escaped key name must be rejected on read"
    assert "JSON escape" in cp.stderr, cp.stderr


@case("add --json: a duplicate key in the entry is rejected")
def t_add_rejects_duplicate_key(p: Path) -> None:
    """cmd_add parsed its --json argument with a bare json.loads too, so a
    repeated key there collapsed silently before any validation ran."""
    _emit_seed(p)
    before = p.read_bytes()
    entry = _seed_extra_entry("dupkey")
    raw = json.dumps(entry)
    raw = raw.replace("{", '{"id": "shadowed", ', 1)      # two "id" keys
    cp = _run(["add", str(p), "--json", raw])
    assert cp.returncode != 0, "duplicate key in --json must be rejected"
    assert "repeated key" in cp.stderr, cp.stderr
    assert p.read_bytes() == before, "a rejected add must not modify the store"


@case("store read: a nesting depth the firmware rejects is rejected here too")
def t_load_rejects_overdeep_value(p: Path) -> None:
    """Host and firmware must accept the same stores. Nine containers under an
    unknown extension key exceeds the firmware's per-value budget of eight."""
    _emit_seed(p)
    data = json.loads(p.read_text())
    nested: object = 0
    for _ in range(9):
        nested = [nested]
    data["entries"][0]["ext"] = nested
    p.write_text(json.dumps(data, indent=2))
    cp = _run(["list", str(p)])
    assert cp.returncode != 0, "over-deep value must be rejected"
    assert "budget" in cp.stderr, cp.stderr


# ---- summary ----------------------------------------------------------------

print(f"\n{PASS_COUNT}/{PASS_COUNT + FAIL_COUNT} bootcfg tests passed")
sys.exit(1 if FAIL_COUNT else 0)
