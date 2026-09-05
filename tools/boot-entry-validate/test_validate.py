#!/usr/bin/env python3
"""Test harness for tools/boot-entry-validate/validate.py.

Self-contained -- no pytest dependency. Runs validate.py as a subprocess against the canonical
sample plus a table of synthetic invalid mutations. Exits 0 if all expectations match, 1
otherwise. Designed as a flat mutator suite so adding cases is one entry per row.

Usage:
  python3 tools/boot-entry-validate/test_validate.py
"""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Callable

REPO_ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = REPO_ROOT / "tools" / "boot-entry-validate" / "validate.py"
SAMPLE = REPO_ROOT / "resources" / "boot" / "bootentries-example.json"

# Import the validator's CRC helpers so fixtures that intentionally violate validation can
# still produce a CRC-correct envelope. Using --emit-crc as a subprocess fails on invalid input
# because that mode runs full validation first.
_spec = importlib.util.spec_from_file_location("_validator_mod", VALIDATOR)
_validator_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_validator_mod)
_compute_crc_from_file = _validator_mod.compute_crc_from_file
_find_crc_field = _validator_mod.find_crc_field


def run_validator(target: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(VALIDATOR), *args, str(target)],
        capture_output=True,
        text=True,
    )


_TMPDIR: tempfile.TemporaryDirectory | None = None
_TMPDIR_PATH: Path | None = None


def _ensure_tmpdir() -> Path:
    """Lazy-create a process-scoped TemporaryDirectory the harness owns and cleans up."""
    global _TMPDIR, _TMPDIR_PATH
    if _TMPDIR is None:
        _TMPDIR = tempfile.TemporaryDirectory(prefix="boot-entry-validate-")
        _TMPDIR_PATH = Path(_TMPDIR.name)
    return _TMPDIR_PATH


def write_tmp(payload: Any, *, recompute_crc: bool = False) -> Path:
    """Write payload to a temp .json file under the harness-owned TemporaryDirectory.

    All fixtures live under one TemporaryDirectory that gets cleaned up at the end of main();
    no per-test unlink needed. Replaces the prior NamedTemporaryFile(delete=False) pattern that
    leaked one file per case (~78 leaks per run on every machine).

    With recompute_crc=True (and the payload is a store dict with `crc32` + `entries`):
    forces crc32 to "0x00000000" placeholder, dumps to file, computes the new file-byte CRC,
    and patches the 8 hex digits in place. Matches the producer flow in validate.py --emit-crc.
    """
    base = _ensure_tmpdir()
    fd = tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False,
                                    dir=str(base), encoding="utf-8")
    if recompute_crc and isinstance(payload, dict) and "crc32" in payload:
        payload = dict(payload)
        payload["crc32"] = "0x00000000"
        json.dump(payload, fd)
        fd.close()
        path = Path(fd.name)
        raw = path.read_bytes()
        crc = _compute_crc_from_file(raw)
        offset, _ = _find_crc_field(raw)
        patched = raw[:offset] + f"{crc:08X}".encode("ascii") + raw[offset + 8:]
        path.write_bytes(patched)
        return path
    json.dump(payload, fd)
    fd.close()
    return Path(fd.name)


def load_sample() -> dict:
    return json.loads(SAMPLE.read_text(encoding="utf-8"))


def write_tmp_raw(text: str) -> Path:
    """Write RAW JSON text and patch its crc32 field to match the bytes written.

    Duplicate and escaped key names cannot survive a Python dict (json.loads
    collapses duplicates, json.dumps never emits an escaped key name), so these
    fixtures are built by string surgery on the sample. The CRC is repaired so
    the only thing left for the validator to object to is the key rule itself --
    otherwise the case could pass on a CRC mismatch and prove nothing.
    """
    base = _ensure_tmpdir()
    fd = tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False,
                                    dir=str(base), encoding="utf-8")
    fd.write(text)
    fd.close()
    path = Path(fd.name)
    raw = path.read_bytes()
    try:
        offset, _ = _find_crc_field(raw)
    except Exception:
        return path          # no locatable crc32 field (escaped-key fixtures)
    zeroed = raw[:offset] + b"00000000" + raw[offset + 8:]
    path.write_bytes(zeroed)
    crc = _compute_crc_from_file(path.read_bytes())
    raw2 = path.read_bytes()
    path.write_bytes(raw2[:offset] + f"{crc:08X}".encode("ascii") + raw2[offset + 8:])
    return path


def _sample_text() -> str:
    return SAMPLE.read_text(encoding="utf-8")


def _dup_top_level_key() -> Path:
    """Two `schema_version` keys at root. json.loads would keep the last one."""
    text = _sample_text()
    return write_tmp_raw(text.replace("{", '{"schema_version": 1, ', 1))


def _dup_entries_key() -> Path:
    """Two `entries` arrays -- the shape that overflowed the firmware parser's
    64-slot output array (boot_entries_parser.c key_seen_before)."""
    text = _sample_text()
    return write_tmp_raw(text.replace("{", '{"entries": [], ', 1))


def _dup_entry_object_key() -> Path:
    """Repeated `id` inside the first entry object."""
    text = _sample_text()
    marker = '"entries"'
    head, sep, tail = text.partition(marker)
    return write_tmp_raw(head + sep + tail.replace("{", '{"id": "dup-key", ', 1))


def _escaped_top_level_key() -> Path:
    """`\\u0073chema_version` decodes to `schema_version` for json.loads but is an
    unknown key to the firmware's raw-byte compare."""
    text = _sample_text()
    return write_tmp_raw(text.replace('"schema_version"', '"\\u0073chema_version"', 1))


def _escaped_entry_key() -> Path:
    text = _sample_text()
    marker = '"entries"'
    head, sep, tail = text.partition(marker)
    return write_tmp_raw(head + sep + tail.replace('"id"', '"\\u0069d"', 1))


def _nest(depth: int) -> Any:
    """A value nesting exactly `depth` containers."""
    v: Any = 0
    for _ in range(depth):
        v = [v]
    return v


def _entry_ext_depth8() -> Path:
    """Firmware budget is 8 containers measured from the skipped value, so an
    8-deep extension under an unknown entry key must be ACCEPTED."""
    data = load_sample()
    data["entries"][0]["ext"] = _nest(8)
    return write_tmp(data, recompute_crc=True)


def _entry_ext_depth9() -> Path:
    """...and 9 must be REJECTED, because the firmware rejects it. The host used
    to accept this and let bootcfg publish a store that boots to fallback."""
    data = load_sample()
    data["entries"][0]["ext"] = _nest(9)
    return write_tmp(data, recompute_crc=True)


def _top_level_ext_depth9() -> Path:
    data = load_sample()
    data["ext"] = _nest(9)
    return write_tmp(data, recompute_crc=True)


def _payload_depth9() -> Path:
    """payload is skipped wholesale by the firmware, so its budget starts there."""
    data = load_sample()
    data["entries"][0]["payload"]["ext"] = _nest(8)   # payload{} + 8 = 9 total
    return write_tmp(data, recompute_crc=True)


def _emit_crc_non_ascii_round_trip() -> Path:
    """--emit-crc must produce a store that still validates.

    It used to serialize with json.dumps' default ensure_ascii=True, so a literal
    non-ASCII extension key came back ESCAPED -- a store the firmware rejects and
    that this validator refuses to reload, written while reporting success. The
    fixture runs the emit-crc pass itself; the case then validates the RESULT, so
    a writer that escapes the key fails here."""
    data = load_sample()
    data["entries"][0]["café"] = 1
    # write_tmp() serializes with json.dump's default ensure_ascii=True, which
    # would escape the key in the FIXTURE and make this case fail before
    # --emit-crc ever ran. The input must be literal for the round trip to be
    # about the writer.
    path = write_tmp_raw(json.dumps(data, indent=2, ensure_ascii=False) + "\n")
    cp = run_validator(path, "--emit-crc")
    assert cp.returncode == 0, f"--emit-crc failed: {cp.stderr}"
    return path


def _emit_crc_failure_preserves_original() -> Path:
    """A FAILED `--emit-crc` must leave the destination byte-for-byte unchanged.

    Two ways this used to destroy the store it was asked to stamp: the
    placeholder was written to the destination before validation ran, and a lone
    surrogate (which json.loads accepts) reached the UTF-8 encoder only after
    write_text had already truncated the file -- zero bytes where the boot store
    had been. The fixture asserts the failure AND the preservation, then returns
    the untouched original so the case's ("pass",) expectation also proves the
    store is still valid afterwards."""
    data = load_sample()
    data["entries"][0]["ext"] = "\ud800"          # lone surrogate: not UTF-8 encodable
    path = write_tmp_raw(json.dumps(data, indent=2, ensure_ascii=True) + "\n")
    before = path.read_bytes()
    cp = run_validator(path, "--emit-crc")
    assert cp.returncode != 0, "--emit-crc must reject a non-encodable store"
    after = path.read_bytes()
    assert after == before, (
        f"destination was modified by a FAILED --emit-crc "
        f"({len(before)} -> {len(after)} bytes)")
    # Hand back a clean store so the case's pass-expectation is meaningful.
    return write_tmp(load_sample(), recompute_crc=True)


def _atomic_write_failure_matrix() -> Path:
    """Failure injection across the atomic writer, with DISTINCT old and new bytes.

    Writing the destination's own bytes back would make byte-equality useless:
    it cannot tell "the original was preserved" from "it was replaced with an
    identical copy", so an erroneous early replacement would pass. The new
    content here differs from the old, which is what makes each assertion mean
    something.

    Covered: a pre-replace failure preserves the OLD bytes; a post-replace
    failure still reports the error while the NEW bytes are in place (data
    landed, durability unconfirmed); a genuinely unsupported flush (EINVAL) is
    skipped and the write succeeds. Each injection asserts it actually fired."""
    import errno as _errno
    real_fsync = os.fsync

    target = write_tmp(load_sample(), recompute_crc=True)
    old_bytes = target.read_bytes()
    new_data = load_sample()
    new_data["entries"][0]["title"] = "Replaced Title"
    new_bytes = json.dumps(new_data, indent=2).encode("utf-8") + b"\n"
    assert new_bytes != old_bytes, "fixture must use distinct new content"

    def run(err: int, fail_on_nth_dir_fsync: int) -> tuple[bool, int]:
        seen = {"n": 0}

        def fake(fd):
            try:
                is_dir = os.fstat(fd).st_mode & 0o170000 == 0o040000
            except OSError:
                is_dir = False
            if is_dir:
                seen["n"] += 1
                if seen["n"] == fail_on_nth_dir_fsync:
                    raise OSError(err, "injected")
                return real_fsync(fd)
            return real_fsync(fd)

        os.fsync = fake
        try:
            try:
                _validator_mod.atomic_write_bytes(target, new_bytes)
                return (False, seen["n"])
            except OSError:
                return (True, seen["n"])
        finally:
            os.fsync = real_fsync

    # A directory descriptor must be obtainable for any of this to fire. Where it
    # is not (Windows), _fsync_dir returns at the open and these cannot run.
    try:
        _probe = os.open(target.parent, os.O_RDONLY)
        os.close(_probe)
    except OSError:
        return target

    # 1. EIO on the FIRST flush -- before os.replace. Original must survive whole.
    target.write_bytes(old_bytes)
    raised, hits = run(_errno.EIO, 1)
    assert hits >= 1, "pre-replace flush never fired"
    assert raised, "EIO before replacement was swallowed"
    assert target.read_bytes() == old_bytes, (
        "a failure BEFORE replacement must leave the original bytes intact")

    # 2. EIO on the SECOND flush -- after os.replace. Error reported, new bytes in
    #    place: the replacement happened, only its durability is unconfirmed.
    target.write_bytes(old_bytes)
    raised, hits = run(_errno.EIO, 2)
    assert hits >= 2, "post-replace flush never fired"
    assert raised, "EIO after replacement was swallowed"
    assert target.read_bytes() == new_bytes, (
        "a failure AFTER replacement must leave the NEW bytes in place")

    # 3. EINVAL is a genuinely unsupported flush: skipped, write succeeds.
    target.write_bytes(old_bytes)
    raised, hits = run(_errno.EINVAL, 1)
    assert hits >= 1, "flush never fired"
    assert not raised, "EINVAL (unsupported) must not fail the write"
    assert target.read_bytes() == new_bytes, "the write should have completed"

    # 4. EACCES from fsync propagates on every platform: only the Windows
    #    directory-OPEN EACCES is suppressed, never the fsync one.
    target.write_bytes(old_bytes)
    raised, hits = run(_errno.EACCES, 1)
    assert hits >= 1, "flush never fired"
    assert raised, "EACCES from fsync must propagate"
    assert target.read_bytes() == old_bytes, "original preserved on that failure"

    # No temp files may survive any of the above.
    strays = [q.name for q in target.parent.iterdir()
              if q.name.startswith(target.name + ".")]
    assert not strays, f"atomic writer left temp files behind: {strays}"

    target.write_bytes(old_bytes)
    return target


def _many_distinct_keys() -> Path:
    """Regression guard: distinct unknown keys are forward-compat and must still
    validate. The firmware rescan has no key-count ceiling, so the host must not
    invent one either."""
    text = _sample_text()
    extra = "".join(f'"k{i:02d}": 0, ' for i in range(30))
    return write_tmp_raw(text.replace("{", "{" + extra, 1))


# ---- Mutator helpers -------------------------------------------------------------------------

def mutate(top: dict[str, Any] | None = None, entry0: dict[str, Any] | None = None,
           payload0: dict[str, Any] | None = None, replace_entry0: dict | None = None) -> dict:
    """Clone the sample and apply targeted mutations.

    top:           merged into top-level dict
    entry0:        merged into entries[0]
    payload0:      merged into entries[0]['payload']
    replace_entry0: replace entries[0] entirely
    """
    data = load_sample()
    if replace_entry0 is not None:
        data["entries"][0] = replace_entry0
    if entry0 is not None:
        data["entries"][0].update(entry0)
    if payload0 is not None:
        data["entries"][0]["payload"].update(payload0)
    if top is not None:
        data.update(top)
    return data


# ---- Case descriptors -------------------------------------------------------------------------

# Each case is (label, builder, expectation). builder() returns Path. expectation is one of:
#   ("pass",)
#   ("pass-with-warn", needle_in_stderr)
#   ("fail", needle_in_stderr)

Case = tuple[str, Callable[[], Path], tuple]


def chainload_entry() -> dict:
    """Reusable kind:chainload entry shape (with trusted_chainload by default)."""
    return {
        "id": "windows",
        "title": "Windows",
        "kind": "chainload",
        "flags": ["active", "trusted_chainload"],
        "sort_key": "50-windows",
        "machine_id": "11111111-2222-3333-4444-555555555555",
        "policy_tags": [],
        "payload": {
            "efi_path": "\\EFI\\Microsoft\\Boot\\bootmgfw.efi",
            "device_guid": "12345678-1234-1234-1234-123456789abc",
        },
    }


def cases() -> list[Case]:
    out: list[Case] = []

    # ---- valid round-trip ----
    out.append(("valid sample", lambda: SAMPLE, ("pass",)))

    # ---- store-level rejections ----
    out.append(("top-level array", lambda: write_tmp([1, 2, 3]),
                ("fail", "top-level value must be a JSON object")))
    out.append(("missing crc32", lambda: write_tmp({"schema_version": 1, "entries": []}),
                ("fail", "missing top-level fields")))
    out.append(("crc32 wrong length", lambda: write_tmp(mutate(top={"crc32": "0xABCD"})),
                ("fail", '"0x" + 8 hex digits')))
    out.append(("crc32 non-hex", lambda: write_tmp(mutate(top={"crc32": "0xZZZZZZZZ"})),
                ("fail", "hex digits only")))
    out.append(("schema_version != 1", lambda: write_tmp(mutate(top={"schema_version": 99})),
                ("fail", "schema_version must be")))
    out.append(("schema_version=true (bool)", lambda: write_tmp(mutate(top={"schema_version": True})),
                ("fail", "schema_version must be")))
    out.append(("entries not array", lambda: write_tmp(
                {"schema_version": 1, "crc32": "0x00000000", "entries": {}}),
                ("fail", "entries must be an array")))
    out.append(("entries empty", lambda: write_tmp(
                {"schema_version": 1, "crc32": "0x00000000", "entries": []}),
                ("fail", "entries length must be 1")))

    # ---- envelope rejections ----
    out.append(("entry not object", lambda: write_tmp(
                {"schema_version": 1, "crc32": "0x00000000", "entries": [42]}),
                ("fail", "must be an object")))
    out.append(("id empty", lambda: write_tmp(mutate(entry0={"id": ""}), recompute_crc=True),
                ("fail", "id must be a string of length 1")))
    out.append(("id too long", lambda: write_tmp(mutate(entry0={"id": "x" * 48}), recompute_crc=True),
                ("fail", "id must be a string of length 1")))
    out.append(("title empty", lambda: write_tmp(mutate(entry0={"title": ""}), recompute_crc=True),
                ("fail", "title must be a string")))
    out.append(("title too long", lambda: write_tmp(mutate(entry0={"title": "x" * 64}), recompute_crc=True),
                ("fail", "title must be a string")))
    out.append(("flags not array", lambda: write_tmp(mutate(entry0={"flags": "active"}), recompute_crc=True),
                ("fail", "flags must be an array")))
    out.append(("flag not string", lambda: write_tmp(mutate(entry0={"flags": [1]}), recompute_crc=True),
                ("fail", "flags entry not a string")))
    out.append(("timeout_override negative", lambda: write_tmp(
                mutate(entry0={"timeout_override": -1}), recompute_crc=True),
                ("fail", "timeout_override must be int")))
    out.append(("timeout_override too large", lambda: write_tmp(
                mutate(entry0={"timeout_override": 601}), recompute_crc=True),
                ("fail", "timeout_override must be int")))
    out.append(("timeout_override=true (bool)", lambda: write_tmp(
                mutate(entry0={"timeout_override": True}), recompute_crc=True),
                ("fail", "timeout_override must be int")))
    out.append(("policy_tags not array", lambda: write_tmp(
                mutate(entry0={"policy_tags": "recovery"}), recompute_crc=True),
                ("fail", "policy_tags must be an array")))
    out.append(("policy_tags element not string", lambda: write_tmp(
                mutate(entry0={"policy_tags": [42]}), recompute_crc=True),
                ("fail", "policy_tags[0] must be a string")))
    out.append(("payload not object", lambda: write_tmp(
                mutate(entry0={"payload": "string-payload"}), recompute_crc=True),
                ("fail", "payload must be an object")))
    out.append(("duplicate id", _make_dup_id, ("fail", "duplicate id")))

    # ---- health_check_subset (per-entry health-gate override) ----
    out.append(("health_check_subset valid 2 names",
                lambda: write_tmp(mutate(entry0={"health_check_subset":
                    ["desktop_ready", "no_panic"]}), recompute_crc=True),
                ("pass",)))
    out.append(("health_check_subset not array",
                lambda: write_tmp(mutate(entry0={"health_check_subset":
                    "minimal"}), recompute_crc=True),
                ("fail", "health_check_subset must be an array")))
    out.append(("health_check_subset overflow (9 names)",
                lambda: write_tmp(mutate(entry0={"health_check_subset":
                    [f"n{i}" for i in range(9)]}), recompute_crc=True),
                ("fail", "MAX_HEALTH_SUBSET_NAMES")))
    out.append(("health_check_subset empty name",
                lambda: write_tmp(mutate(entry0={"health_check_subset":
                    [""]}), recompute_crc=True),
                ("fail", "length 0 out of")))
    out.append(("health_check_subset oversize name (24 chars)",
                lambda: write_tmp(mutate(entry0={"health_check_subset":
                    ["a" * 24]}), recompute_crc=True),
                ("fail", "length 24 out of")))
    out.append(("health_check_subset name max length (23 chars)",
                lambda: write_tmp(mutate(entry0={"health_check_subset":
                    ["a" * 23]}), recompute_crc=True),
                ("pass",)))
    out.append(("health_check_subset name with backslash",
                lambda: write_tmp(mutate(entry0={"health_check_subset":
                    ["bad\\name"]}), recompute_crc=True),
                ("fail", "must be printable ASCII")))
    out.append(("health_check_subset name with control char",
                lambda: write_tmp(mutate(entry0={"health_check_subset":
                    ["bad\x01ctrl"]}), recompute_crc=True),
                ("fail", "must be printable ASCII")))

    # ---- CRC ----
    out.append(("bad CRC", lambda: write_tmp(mutate(top={"crc32": "0xDEADBEEF"})),
                ("fail", "CRC mismatch")))

    # ---- forward-compat: kind ----
    # (Each forward-compat case recomputes CRC after mutating.)
    out.append(("stable-range numeric kind not known (kind=10)",
                lambda: write_tmp(mutate(entry0={"kind": 10}), recompute_crc=True),
                ("fail", "stable range 0..99")))
    out.append(("string-form unknown kind -- skip-with-warn",
                lambda: write_tmp(mutate(entry0={"kind": "future-stable-kind"}), recompute_crc=True),
                ("pass-with-warn", "unknown kind")))
    out.append(("vendor-range numeric kind 150 -- skip-with-warn",
                lambda: write_tmp(mutate(entry0={"kind": 150}), recompute_crc=True),
                ("pass-with-warn", "vendor/reserved")))
    out.append(("reserved-range numeric kind 250 -- skip-with-warn",
                lambda: write_tmp(mutate(entry0={"kind": 250}), recompute_crc=True),
                ("pass-with-warn", "vendor/reserved")))
    out.append(("kind = -1 (out of range)",
                lambda: write_tmp(mutate(entry0={"kind": -1}), recompute_crc=True),
                ("fail", "out of valid range")))
    out.append(("kind=true (bool, not int)",
                lambda: write_tmp(mutate(entry0={"kind": True}), recompute_crc=True),
                ("fail", "kind must be string or integer")))

    # ---- per-kind payload type / value rejections ----
    out.append(("split: kernel not string",
                lambda: write_tmp(mutate(payload0={"kernel": 123}), recompute_crc=True),
                ("fail", "kernel must be string")))
    out.append(("split: cmdline not string",
                lambda: write_tmp(mutate(payload0={"cmdline": []}), recompute_crc=True),
                ("fail", "cmdline must be string")))
    out.append(("split: root not string",
                lambda: write_tmp(mutate(payload0={"root": {}}), recompute_crc=True),
                ("fail", "root must be string")))
    out.append(("split: initrd not array",
                lambda: write_tmp(mutate(payload0={"initrd": "single.img"}), recompute_crc=True),
                ("fail", "initrd must be array")))
    out.append(("split: missing required",
                lambda: write_tmp(mutate(payload0={"kernel": "k.exe"}, replace_entry0={
                    "id": "broken-split",
                    "title": "Broken Split",
                    "kind": "split",
                    "flags": ["active"],
                    "sort_key": "00-broken",
                    "machine_id": "11111111-2222-3333-4444-555555555555",
                    "policy_tags": [],
                    "payload": {"kernel": "k.exe"},  # missing cmdline + root
                }), recompute_crc=True),
                ("fail", "missing payload fields")))

    out.append(("uki: uki_path not string",
                _uki_bad_path, ("fail", "uki_path must be string")))

    out.append(("chainload missing trusted_chainload flag",
                _chainload_no_trust, ("fail", "requires flags include trusted_chainload")))

    out.append(("network: bad uri_scheme",
                _network_bad_scheme, ("fail", "uri_scheme must be one of")))
    out.append(("network: short asset_digest",
                _network_short_digest, ("fail", "asset_digest must be 64-char hex")))
    out.append(("network: non-hex asset_digest",
                _network_nonhex_digest, ("fail", "asset_digest must be hex digits only")))

    out.append(("resume: short snapshot_digest",
                _resume_short_digest, ("fail", "snapshot_digest must be 64-char hex")))

    out.append(("installer: bad media_role",
                _installer_bad_role, ("fail", "media_role must be one of")))

    out.append(("safe: bad safe_mode_subset",
                _safe_bad_subset, ("fail", "safe_mode_subset must be one of")))

    out.append(("diagnostics: verbose_log not bool",
                _diag_nonbool, ("fail", "verbose_log must be bool")))

    out.append(("test: bad test_suite",
                _test_bad_suite, ("fail", "test_suite must be one of")))

    # ---- adversarial-review fixes (path safety + non-finite + pre-parse size + UUID + UKI prefix) ----
    out.append(("path with NUL byte rejected",
                lambda: write_tmp(mutate(payload0={"kernel": "\\EFI\\bad\x00.exe"}), recompute_crc=True),
                ("fail", "contains NUL byte")))
    out.append(("path with non-ASCII rejected",
                lambda: write_tmp(mutate(payload0={"kernel": "\\EFI\\éclair.exe"}), recompute_crc=True),
                ("fail", "contains non-ASCII byte")))
    out.append(("path with control byte rejected",
                lambda: write_tmp(mutate(payload0={"cmdline": "console=serial\x07"}), recompute_crc=True),
                ("fail", "contains control byte")))
    out.append(("uki_path bad prefix rejected", _uki_bad_prefix, ("fail", "must start with")))
    out.append(("uki_path good prefix accepted", _uki_good, ("pass",)))
    out.append(("machine_id non-UUID rejected",
                lambda: write_tmp(mutate(entry0={"machine_id": "not-a-uuid"}), recompute_crc=True),
                ("fail", "machine_id must be empty or RFC 4122 UUID format")))
    out.append(("recovery GUID non-UUID rejected", _recovery_bad_guid, ("fail", "recovery_partition_guid must be RFC 4122 UUID")))
    out.append(("chainload device_guid non-UUID rejected", _chainload_bad_guid, ("fail", "device_guid must be RFC 4122 UUID")))
    out.append(("NaN token in extra payload field rejected", _nan_payload, ("fail", "non-finite JSON token")))
    out.append(("Infinity token in extra payload field rejected", _inf_payload, ("fail", "non-finite JSON token")))
    out.append(("oversize 1e500 -> Infinity rejected", _huge_exp_payload, ("fail", "non-finite")))
    out.append(("oversize file rejected pre-read", _oversize_file, ("fail", "rejected pre-read")))

    # ---- re-adversarial fixes ----
    out.append(("initrd item with NUL byte rejected",
                lambda: write_tmp(mutate(payload0={"initrd": ["\\EFI\\bad\x00.img"]}), recompute_crc=True),
                ("fail", "initrd[0] contains NUL byte")))
    out.append(("initrd item non-ASCII rejected",
                lambda: write_tmp(mutate(payload0={"initrd": ["\\EFI\\éclair.img"]}), recompute_crc=True),
                ("fail", "initrd[0] contains non-ASCII byte")))
    out.append(("initrd item too long rejected",
                lambda: write_tmp(mutate(payload0={"initrd": ["x" * 256]}), recompute_crc=True),
                ("fail", "initrd[0] length")))
    out.append(("split root not A/B/UUID rejected",
                lambda: write_tmp(mutate(payload0={"root": "neither"}), recompute_crc=True),
                ("fail", "root must be 'A', 'B', or RFC 4122 UUID")))
    out.append(("split root UUID accepted", _root_uuid_ok, ("pass",)))
    out.append(("deeply nested JSON rejected without crash", _deep_nested, ("fail", "")))

    # ---- ESP-path grammar (round-2 re-adversarial) ----
    out.append(("split kernel empty rejected",
                lambda: write_tmp(mutate(payload0={"kernel": ""}), recompute_crc=True),
                ("fail", "must be non-empty ESP-relative path")))
    out.append(("split kernel relative rejected",
                lambda: write_tmp(mutate(payload0={"kernel": "kernel.exe"}), recompute_crc=True),
                ("fail", "must start with")))
    out.append(("split kernel drive-form rejected",
                lambda: write_tmp(mutate(payload0={"kernel": "C:\\boot\\kernel.exe"}), recompute_crc=True),
                ("fail", "must start with")))
    out.append(("split kernel traversal rejected",
                lambda: write_tmp(mutate(payload0={"kernel": "\\EFI\\..\\Microsoft\\bootmgfw.efi"}), recompute_crc=True),
                ("fail", "'..' traversal segment")))
    out.append(("split kernel '.' segment rejected",
                lambda: write_tmp(mutate(payload0={"kernel": "\\EFI\\.\\foo.exe"}), recompute_crc=True),
                ("fail", "'.' segment")))
    out.append(("initrd traversal rejected",
                lambda: write_tmp(mutate(payload0={"initrd": ["\\EFI\\..\\bad.img"]}), recompute_crc=True),
                ("fail", "'..' traversal segment")))

    # ---- impl-adversarial fixes (kebab-case id + url<->uri_scheme cross-validate) ----
    out.append(("id with uppercase rejected",
                lambda: write_tmp(mutate(entry0={"id": "Slot-A"}), recompute_crc=True),
                ("fail", "id must be kebab-case")))
    out.append(("id with slash rejected",
                lambda: write_tmp(mutate(entry0={"id": "slot/a"}), recompute_crc=True),
                ("fail", "id must be kebab-case")))
    out.append(("id with leading dash rejected",
                lambda: write_tmp(mutate(entry0={"id": "-slot-a"}), recompute_crc=True),
                ("fail", "id must be kebab-case")))
    out.append(("id with trailing dash rejected",
                lambda: write_tmp(mutate(entry0={"id": "slot-a-"}), recompute_crc=True),
                ("fail", "id must be kebab-case")))
    out.append(("network url-scheme mismatch rejected",
                _network_scheme_mismatch,
                ("fail", "does not match uri_scheme")))
    out.append(("network url file:// + uri_scheme=https rejected",
                _network_file_scheme,
                ("fail", "scheme")))
    out.append(("network url empty-host (https://) rejected",
                _network_empty_host,
                ("fail", "empty host")))
    out.append(("network url userinfo-only (https://@/k) rejected",
                _network_userinfo_only,
                ("fail", "empty host")))
    out.append(("network url with userinfo (https://user@host/k) rejected",
                _network_with_userinfo,
                ("fail", "must not contain userinfo")))

    # ---- unique + literally-spelled key names (firmware parity) ----
    out.append(("repeated top-level key rejected",
                _dup_top_level_key, ("fail", "repeated key")))
    out.append(("repeated entries key rejected",
                _dup_entries_key, ("fail", "repeated key")))
    out.append(("repeated entry-object key rejected",
                _dup_entry_object_key, ("fail", "repeated key")))
    out.append(("escaped top-level key name rejected",
                _escaped_top_level_key, ("fail", "uses a JSON escape")))
    out.append(("escaped entry key name rejected",
                _escaped_entry_key, ("fail", "uses a JSON escape")))
    out.append(("30 distinct unknown top-level keys accepted",
                _many_distinct_keys, ("pass",)))

    # ---- firmware value-depth budget parity (8 accept / 9 reject) ----
    out.append(("entry extension nested 8 deep accepted",
                _entry_ext_depth8, ("pass",)))
    out.append(("entry extension nested 9 deep rejected",
                _entry_ext_depth9, ("fail", "budget")))
    out.append(("top-level extension nested 9 deep rejected",
                _top_level_ext_depth9, ("fail", "budget")))
    out.append(("payload nested 9 deep rejected",
                _payload_depth9, ("fail", "budget")))
    out.append(("--emit-crc round-trips a literal non-ASCII key",
                _emit_crc_non_ascii_round_trip, ("pass",)))
    out.append(("failed --emit-crc leaves the original store byte-identical",
                _emit_crc_failure_preserves_original, ("pass",)))
    out.append(("atomic write: failure matrix preserves or replaces correctly",
                _atomic_write_failure_matrix, ("pass",)))

    return out


# ---- Per-kind fixture helpers (return Path) ---------------------------------------------------

def _make_dup_id() -> Path:
    data = load_sample()
    dup = json.loads(json.dumps(data["entries"][0]))
    data["entries"].append(dup)
    return write_tmp(data, recompute_crc=True)


def _uki_bad_path() -> Path:
    data = load_sample()
    data["entries"][0] = {
        "id": "uki-bad", "title": "UKI", "kind": "uki", "flags": ["active"],
        "sort_key": "00-u", "machine_id": "11111111-2222-3333-4444-555555555555",
        "policy_tags": [], "payload": {"uki_path": 123},
    }
    return write_tmp(data, recompute_crc=True)


def _chainload_no_trust() -> Path:
    data = load_sample()
    e = chainload_entry()
    e["flags"] = ["active"]  # missing trusted_chainload
    data["entries"][0] = e
    return write_tmp(data, recompute_crc=True)


def _network_bad_scheme() -> Path:
    data = load_sample()
    data["entries"][0] = {
        "id": "net", "title": "Net", "kind": "network", "flags": ["active"],
        "sort_key": "00-n", "machine_id": "11111111-2222-3333-4444-555555555555",
        "policy_tags": [], "payload": {
            "url": "ftp://example.com/k", "uri_scheme": "ftp",
            "asset_digest": "a" * 64,
        },
    }
    return write_tmp(data, recompute_crc=True)


def _network_short_digest() -> Path:
    data = load_sample()
    data["entries"][0] = {
        "id": "net", "title": "Net", "kind": "network", "flags": ["active"],
        "sort_key": "00-n", "machine_id": "11111111-2222-3333-4444-555555555555",
        "policy_tags": [], "payload": {
            "url": "https://example.com/k", "uri_scheme": "https",
            "asset_digest": "abc",  # too short
        },
    }
    return write_tmp(data, recompute_crc=True)


def _network_nonhex_digest() -> Path:
    data = load_sample()
    data["entries"][0] = {
        "id": "net", "title": "Net", "kind": "network", "flags": ["active"],
        "sort_key": "00-n", "machine_id": "11111111-2222-3333-4444-555555555555",
        "policy_tags": [], "payload": {
            "url": "https://example.com/k", "uri_scheme": "https",
            "asset_digest": "z" * 64,  # not hex
        },
    }
    return write_tmp(data, recompute_crc=True)


def _resume_short_digest() -> Path:
    data = load_sample()
    data["entries"][0] = {
        "id": "res", "title": "Resume", "kind": "resume", "flags": ["active"],
        "sort_key": "00-r", "machine_id": "11111111-2222-3333-4444-555555555555",
        "policy_tags": [], "payload": {
            "snapshot_path": "\\snap", "snapshot_digest": "abc",
        },
    }
    return write_tmp(data, recompute_crc=True)


def _installer_bad_role() -> Path:
    data = load_sample()
    data["entries"][0] = {
        "id": "ins", "title": "Installer", "kind": "installer", "flags": ["active"],
        "sort_key": "00-i", "machine_id": "11111111-2222-3333-4444-555555555555",
        "policy_tags": [], "payload": {
            "installer_image_guid": "12345678-1234-1234-1234-123456789abc",
            "media_role": "format-disk",
        },
    }
    return write_tmp(data, recompute_crc=True)


def _safe_bad_subset() -> Path:
    data = load_sample()
    data["entries"][0] = {
        "id": "sf", "title": "Safe", "kind": "safe", "flags": ["active"],
        "sort_key": "00-s", "machine_id": "11111111-2222-3333-4444-555555555555",
        "policy_tags": [], "payload": {
            "safe_mode_subset": "extreme", "kernel": "\\k.exe",
        },
    }
    return write_tmp(data, recompute_crc=True)


def _diag_nonbool() -> Path:
    data = load_sample()
    data["entries"][0] = {
        "id": "dg", "title": "Diag", "kind": "diagnostics", "flags": ["active"],
        "sort_key": "00-d", "machine_id": "11111111-2222-3333-4444-555555555555",
        "policy_tags": [], "payload": {"kernel": "\\k.exe", "verbose_log": "true"},
    }
    return write_tmp(data, recompute_crc=True)


def _test_bad_suite() -> Path:
    data = load_sample()
    data["entries"][0] = {
        "id": "ts", "title": "Test", "kind": "test", "flags": ["active"],
        "sort_key": "00-t", "machine_id": "11111111-2222-3333-4444-555555555555",
        "policy_tags": [], "payload": {"kernel": "\\k.exe", "test_suite": "everything"},
    }
    return write_tmp(data, recompute_crc=True)


# ---- Adversarial-review fixture builders ------------------------------------------------------

def _uki_bad_prefix() -> Path:
    data = load_sample()
    data["entries"][0] = {
        "id": "uki-bad-prefix", "title": "UKI bad prefix", "kind": "uki",
        "flags": ["active"], "sort_key": "00-u",
        "machine_id": "11111111-2222-3333-4444-555555555555",
        "policy_tags": [],
        "payload": {"uki_path": "\\bad\\path.efi"},  # not under \EFI\Linux or \EFI\ImpossibleOS
    }
    return write_tmp(data, recompute_crc=True)


def _uki_good() -> Path:
    data = load_sample()
    data["entries"][0] = {
        "id": "uki-good", "title": "UKI", "kind": "uki",
        "flags": ["active"], "sort_key": "00-u",
        "machine_id": "11111111-2222-3333-4444-555555555555",
        "policy_tags": [],
        "payload": {"uki_path": "\\EFI\\ImpossibleOS\\BOOTX64.UKI.efi"},
    }
    return write_tmp(data, recompute_crc=True)


def _recovery_bad_guid() -> Path:
    data = load_sample()
    # Sample's third entry is recovery; mutate its guid to a non-UUID.
    data["entries"][2]["payload"]["recovery_partition_guid"] = "not-a-uuid"
    return write_tmp(data, recompute_crc=True)


def _chainload_bad_guid() -> Path:
    data = load_sample()
    data["entries"][0] = {
        "id": "chain-bad-guid", "title": "Chain", "kind": "chainload",
        "flags": ["active", "trusted_chainload"],
        "sort_key": "50-c", "machine_id": "11111111-2222-3333-4444-555555555555",
        "policy_tags": [],
        "payload": {"efi_path": "\\foo.efi", "device_guid": "not-a-uuid"},
    }
    return write_tmp(data, recompute_crc=True)


def _nan_payload() -> Path:
    """Hand-write JSON with a literal NaN token in an entry; bypasses Python's json default."""
    fd = tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False)
    fd.write('{"schema_version":1,"crc32":"0x00000000","entries":[{"id":"a","title":"A","kind":"split","flags":["active"],"sort_key":"00","machine_id":"11111111-2222-3333-4444-555555555555","policy_tags":[],"payload":{"kernel":"\\\\k.exe","cmdline":"","root":"A","extra":NaN}}]}')
    fd.close()
    return Path(fd.name)


def _inf_payload() -> Path:
    fd = tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False)
    fd.write('{"schema_version":1,"crc32":"0x00000000","entries":[{"id":"a","title":"A","kind":"split","flags":["active"],"sort_key":"00","machine_id":"11111111-2222-3333-4444-555555555555","policy_tags":[],"payload":{"kernel":"\\\\k.exe","cmdline":"","root":"A","extra":Infinity}}]}')
    fd.close()
    return Path(fd.name)


def _huge_exp_payload() -> Path:
    """Number 1e500 parses to float('inf') without firing parse_constant; the recursive guard catches it."""
    fd = tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False)
    fd.write('{"schema_version":1,"crc32":"0x00000000","entries":[{"id":"a","title":"A","kind":"split","flags":["active"],"sort_key":"00","machine_id":"11111111-2222-3333-4444-555555555555","policy_tags":[],"payload":{"kernel":"\\\\k.exe","cmdline":"","root":"A","extra":1e500}}]}')
    fd.close()
    return Path(fd.name)


def _oversize_file() -> Path:
    """Write a >16 KiB file (whitespace-padded valid JSON); validator rejects pre-read."""
    fd = tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False)
    # 17 KiB of trailing whitespace inside a JSON-comment-not-allowed... use a long string field.
    pad = "x" * (17 * 1024)
    fd.write('{"schema_version":1,"crc32":"0x00000000","entries":[{"id":"a","title":"' + pad + '","kind":"split","flags":["active"],"sort_key":"00","machine_id":"11111111-2222-3333-4444-555555555555","policy_tags":[],"payload":{"kernel":"\\\\k.exe","cmdline":"","root":"A"}}]}')
    fd.close()
    return Path(fd.name)


def _root_uuid_ok() -> Path:
    """split root may be a partition GUID (UUID textual form), not just A/B."""
    data = load_sample()
    data["entries"][0]["payload"]["root"] = "12345678-1234-1234-1234-123456789abc"
    return write_tmp(data, recompute_crc=True)


def _network_scheme_mismatch() -> Path:
    """url scheme=tftp but uri_scheme=https -- cross-validate must reject."""
    data = load_sample()
    data["entries"][0] = {
        "id": "net", "title": "Net", "kind": "network", "flags": ["active"],
        "sort_key": "00-n", "machine_id": "11111111-2222-3333-4444-555555555555",
        "policy_tags": [], "payload": {
            "url": "tftp://example.com/k", "uri_scheme": "https",
            "asset_digest": "a" * 64,
        },
    }
    return write_tmp(data, recompute_crc=True)


def _network_file_scheme() -> Path:
    """url=file:// + uri_scheme=https; cross-validate must reject (file:// not in allowed schemes)."""
    data = load_sample()
    data["entries"][0] = {
        "id": "net", "title": "Net", "kind": "network", "flags": ["active"],
        "sort_key": "00-n", "machine_id": "11111111-2222-3333-4444-555555555555",
        "policy_tags": [], "payload": {
            "url": "file://evil/kernel.efi", "uri_scheme": "https",
            "asset_digest": "a" * 64,
        },
    }
    return write_tmp(data, recompute_crc=True)


def _network_empty_host() -> Path:
    """url='https://' with no host; cross-validate must reject for empty netloc."""
    data = load_sample()
    data["entries"][0] = {
        "id": "net", "title": "Net", "kind": "network", "flags": ["active"],
        "sort_key": "00-n", "machine_id": "11111111-2222-3333-4444-555555555555",
        "policy_tags": [], "payload": {
            "url": "https://", "uri_scheme": "https",
            "asset_digest": "a" * 64,
        },
    }
    return write_tmp(data, recompute_crc=True)


def _network_userinfo_only() -> Path:
    """url='https://@/kernel.efi' has userinfo but empty host; netloc check would pass, hostname check fails."""
    data = load_sample()
    data["entries"][0] = {
        "id": "net", "title": "Net", "kind": "network", "flags": ["active"],
        "sort_key": "00-n", "machine_id": "11111111-2222-3333-4444-555555555555",
        "policy_tags": [], "payload": {
            "url": "https://@/kernel.efi", "uri_scheme": "https",
            "asset_digest": "a" * 64,
        },
    }
    return write_tmp(data, recompute_crc=True)


def _network_with_userinfo() -> Path:
    """url with full user@host; reject userinfo entirely (no creds in boot URLs)."""
    data = load_sample()
    data["entries"][0] = {
        "id": "net", "title": "Net", "kind": "network", "flags": ["active"],
        "sort_key": "00-n", "machine_id": "11111111-2222-3333-4444-555555555555",
        "policy_tags": [], "payload": {
            "url": "https://user@example.com/kernel.efi", "uri_scheme": "https",
            "asset_digest": "a" * 64,
        },
    }
    return write_tmp(data, recompute_crc=True)


def _deep_nested() -> Path:
    """Deeply-nested JSON should be rejected without crashing (RecursionError caught).

    Build an entry whose payload contains a 1500-level nested array. Python's default recursion
    limit is 1000; either json.loads raises RecursionError (caught in main()) or _reject_non_finite
    sees it (now iterative). Either path produces a [FAIL] line, never an uncaught traceback.
    """
    fd = tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False)
    nested = "[" * 1500 + "0" + "]" * 1500
    fd.write(
        '{"schema_version":1,"crc32":"0x00000000","entries":[{"id":"a","title":"A","kind":"split","flags":["active"],"sort_key":"00","machine_id":"11111111-2222-3333-4444-555555555555","policy_tags":[],"payload":{"kernel":"\\\\k.exe","cmdline":"","root":"A","nested":'
        + nested
        + '}}]}'
    )
    fd.close()
    return Path(fd.name)


# ---- Driver --------------------------------------------------------------------------------------

def main() -> int:
    failures = 0
    for label, builder, expectation in cases():
        try:
            target = builder()
        except Exception as e:
            print(f"[BAD]  {label}: fixture builder raised: {e}")
            failures += 1
            continue

        result = run_validator(target)
        kind = expectation[0]

        if kind == "pass":
            if result.returncode == 0:
                print(f"[OK]   {label}")
            else:
                print(f"[BAD]  {label}: expected pass, rc={result.returncode}, stderr={result.stderr.strip()}")
                failures += 1
        elif kind == "pass-with-warn":
            needle = expectation[1]
            if result.returncode == 0 and needle in result.stderr:
                print(f"[OK]   {label} (warned: {needle})")
            else:
                print(f"[BAD]  {label}: expected pass with warn {needle!r}, rc={result.returncode}, stderr={result.stderr.strip()}")
                failures += 1
        elif kind == "fail":
            needle = expectation[1]
            if result.returncode != 0 and needle in result.stderr:
                print(f"[OK]   {label} (rejected with: {needle})")
            else:
                print(f"[BAD]  {label}: expected fail with {needle!r}, rc={result.returncode}, stderr={result.stderr.strip()}")
                failures += 1
        else:
            print(f"[BAD]  {label}: unknown expectation {kind!r}")
            failures += 1

    total = len(cases())
    print(f"\n{total - failures}/{total} passed")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
