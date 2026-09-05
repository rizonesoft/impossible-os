#!/usr/bin/env python3
"""bootcfg -- offline boot-entry-store editor.

Edits `\\EFI\\ImpossibleOS\\bootentries.json` (or any offline copy on a mounted
ESP) using the canonical schema validator at `tools/boot-entry-validate/`.
Every write goes through the validator before persisting; reject invalid
stores rather than corrupting the file. Atomic CoW + fsync durability matches
the boot-counter rename protocol's "write-new + flush + close + atomic-
replace" contract. The REPLACEMENT is always atomic, so a reader sees the old
store or the new one and never a torn one. Surviving a host CRASH additionally
needs the parent-directory flush, which is skipped where the platform does not
support it (Windows has no directory handle to flush); a genuine flush failure
is reported rather than swallowed.

Subcommands (offline mode, all operate on a path argument):
    list <path>                      Dump entries to stdout.
    add <path> --json <entry-json>   Append an entry; fail if id already exists.
    remove <path> <id>               Drop an entry by id.
    set-default <path> <id>          Reassign sort_keys so <id> is the lowest
                                     among ACTIVE entries (the policy ladder
                                     picks lowest-sort_key first; mere array
                                     reordering does not change the default).
    emit-seed <path>                 Write the idempotent 3-entry default
                                     store (slot-a active + slot-b inactive
                                     + recovery active-but-kind-filtered).
                                     Kernel path matches the release image's
                                     staged \\boot\\kernel.exe location.
                                     Running twice yields a byte-identical
                                     store. See docs/boot/bootstrap.md for
                                     the ownership boundary and lifecycle.

Live-boot subcommands DEFERRED (need a running OS user-mode binary, not host
Python): set-bootnext-hint, set-oneshot, dump-history.

Exit codes:
    0  -- success
    1  -- validation failure (printed to stderr with [FAIL] prefix)
    2  -- usage error (missing argument, file unreadable, etc.)
    3  -- conflict (id collision on add, missing id on remove/set-default)
"""

from __future__ import annotations

import argparse
import getpass
import json
import os
import socket
import sys
import time
import zlib
from pathlib import Path
from typing import Any

# Reuse the validator's schema constants + validate_store + CRC helpers.
# This is the SINGLE source of truth for envelope/payload rules; bootcfg
# does not duplicate any of them.
SELF_DIR = Path(__file__).resolve().parent
REPO_ROOT = SELF_DIR.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools" / "boot-entry-validate"))
import validate as _validator  # type: ignore  # noqa: E402


# ---- Defaults for emit-seed --------------------------------------------------

# Canonical staged-kernel path used by the release image builder and the
# Makefile efi_staging layout. The bootloader's policy ladder treats a
# validated SPLIT kernel path as the SOLE candidate (no ambient fallback) --
# the seeded path MUST match the staged kernel path on disk or the loader
# fails closed before reaching the ambient kernel_paths[] search. See
# docs/boot/bootstrap.md for the staged-vs-seeded contract.
SEED_KERNEL_PATH = "\\boot\\kernel.exe"
# Empty machine_id is the explicit "match any machine" wildcard per
# the boot-policy ladder (e->machine_id[0] == '\\0'). Using a
# placeholder UUID would be treated as a real machine pin and
# rejected as MACHINE_ID_MISMATCH on every machine whose local
# machine_id differs (i.e. all of them, since SMBIOS UUID extraction
# has not shipped). The seed must be a wildcard.
SEED_MACHINE_ID = ""
# Canonical placeholder GUID for the Impossible OS recovery partition.
# The recovery-partition-in-disk-layout feature adopts this GUID when it
# ships; until then the recovery entry is dormant -- the bootloader filters
# kind=recovery via supported_kinds_mask (KIND_UNAVAILABLE) so no resolve
# is attempted. Hex prefix "49504F53" is ASCII "IPOS" for grep-ability.
# See docs/boot/bootstrap.md for the adoption contract.
SEED_RECOVERY_PARTITION_GUID = "49504F53-7265-636F-7665-727900000001"


def _seed_store() -> dict:
    """Idempotent 3-entry seed: slot-A + slot-B + recovery.

    Deterministic id generation from (machine_id, kind, slot): with
    machine_id="" wildcard, ids reduce to kind+slot strings ("slot-a",
    "slot-b", "recovery"). A second run produces a byte-identical store.

    Today's gating (per docs/boot/bootstrap.md):
      - slot-a: active, kind=split, lowest sort_key -> ladder default.
      - slot-b: INACTIVE (flags=[]) until the A/B-rollback dual-slot
        layout and root-selection semantics ship. Without those, an
        active slot-b would load the same kernel.exe slot-a does,
        falsifying rollback / parity claims (Codex design review High).
      - recovery: active envelope but kind=recovery is filtered by
        the bootloader's supported_kinds_mask (KIND_UNAVAILABLE) until
        the recovery-partition load path widens it. Entry exists in
        the store as the committed contract; resolves to a real
        partition only after the recovery-partition GPT layout ships.
    """
    return {
        "schema_version": _validator.SCHEMA_VERSION,
        "crc32": "0x00000000",  # placeholder; rewritten before write
        "entries": [
            {
                "id": "slot-a",
                "title": "Impossible OS (slot A)",
                "kind": "split",
                "flags": ["active"],
                "sort_key": "00-slot-a",
                "machine_id": SEED_MACHINE_ID,
                "policy_tags": [],
                "payload": {
                    "kernel": SEED_KERNEL_PATH,
                    "cmdline": "",
                    "root": "A",
                },
            },
            {
                "id": "slot-b",
                "title": "Impossible OS (slot B)",
                "kind": "split",
                "flags": [],
                "sort_key": "50-slot-b",
                "machine_id": SEED_MACHINE_ID,
                "policy_tags": [],
                "payload": {
                    "kernel": SEED_KERNEL_PATH,
                    "cmdline": "",
                    "root": "B",
                },
            },
            {
                "id": "recovery",
                "title": "Impossible OS Recovery",
                "kind": "recovery",
                "flags": ["active"],
                "sort_key": "99-recovery",
                "machine_id": SEED_MACHINE_ID,
                "policy_tags": [],
                "payload": {
                    "recovery_partition_guid": SEED_RECOVERY_PARTITION_GUID,
                },
            },
        ],
    }


# ---- Read / parse -----------------------------------------------------------


def _load(path: Path) -> tuple[dict, bytes]:
    """Read + JSON-parse + validate. Returns (parsed, raw bytes)."""
    if not path.is_file():
        print(f"[FAIL] {path}: not a file", file=sys.stderr)
        sys.exit(2)
    # Pre-read size guard: bound host-side I/O + JSON parse work so
    # an operator pointing bootcfg at the wrong (or maliciously
    # oversized) file does not consume unbounded memory before the
    # validator's MAX_TOTAL_BYTES check runs. Mirrors the standalone
    # validator's pre-read stat size check.
    try:
        file_size = path.stat().st_size
    except OSError as e:
        print(f"[FAIL] {path}: stat failed: {e}", file=sys.stderr)
        sys.exit(2)
    if file_size > _validator.MAX_TOTAL_BYTES:
        print(f"[FAIL] {path}: file size {file_size} > "
              f"MAX_TOTAL_BYTES {_validator.MAX_TOTAL_BYTES} "
              f"(rejected pre-read)", file=sys.stderr)
        sys.exit(1)
    try:
        raw = path.read_bytes()
    except OSError as e:
        print(f"[FAIL] {path}: read failed: {e}", file=sys.stderr)
        sys.exit(2)
    try:
        # Shared strict loader: bootcfg must reject the same duplicate-key and
        # escaped-key stores the firmware parser does, or it would read, rewrite
        # and re-bless a store that cannot boot. A bare json.loads collapses
        # duplicates silently before validate_store ever sees them.
        data = _validator.strict_loads(raw.decode("utf-8"))
    except UnicodeDecodeError as e:
        print(f"[FAIL] {path}: file is not valid UTF-8: {e}", file=sys.stderr)
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"[FAIL] {path}: JSON parse error: {e}", file=sys.stderr)
        sys.exit(1)
    # Validate as a read; failures bail with the validator's [FAIL] prefix.
    _validator.validate_store(data, raw, recompute_crc=False)
    return data, raw


# ---- Serialize + write -------------------------------------------------------


def _canonical_dumps(data: dict) -> bytes:
    """Deterministic JSON shape for stable CRC computation. Pretty-printed
    with the spec-mandated top-level field order: schema_version, crc32,
    entries."""
    top = {
        "schema_version": data["schema_version"],
        "crc32": data["crc32"],
        "entries": data["entries"],
    }
    # ensure_ascii=False is load-bearing, not cosmetic. With escaping ON, a
    # literal non-ASCII extension key such as "cafe-mode" spelled with an
    # accented character was rewritten as "café-mode" -- an ESCAPED key,
    # which the firmware parser hard-rejects (BOOT_ENTRIES_REJECT_ESCAPED_KEY)
    # and which bootcfg itself then refuses to reload. The editor could write a
    # store nothing could read. Key spelling must survive the round trip.
    return json.dumps(top, indent=2, ensure_ascii=False).encode("utf-8") + b"\n"


def _recompute_crc(data: dict) -> dict:
    """Stamp the canonical CRC into data['crc32']. Returns mutated data."""
    data["crc32"] = "0x00000000"
    raw = _canonical_dumps(data)
    crc = _validator.compute_crc_from_file(raw)
    data["crc32"] = f"0x{crc:08X}"
    return data


def _atomic_write(path: Path, raw: bytes) -> None:
    """Atomic CoW + fsync durability protocol.

    Delegates to the validator's `atomic_write_bytes`, which is the single copy
    of this protocol: write an EXCLUSIVELY OWNED same-directory temp (mkstemp,
    never a predictable `<path>.new` another process or a symlink could own),
    fsync its contents, fsync the parent so the new directory entry is durable,
    os.replace over the destination, fsync the parent again.

    Without those fsyncs, FAT32 (and ext4 with default mount options) can leave
    a torn or missing store after a host crash mid-write; the next read would
    reject it and the bootloader would fall back to the synthesized default --
    recoverable, but not the contract bootcfg promised. With them the store is
    either the old version or the new version, never torn.

    Both writers shared this protocol by copy until the copies were found to
    have diverged in exactly the part that matters (temp-file ownership), which
    is why there is now one implementation and this wrapper.
    """
    _validator.atomic_write_bytes(path, raw)


def _validate_then_write(path: Path, data: dict) -> None:
    """Stamp CRC, validate the would-be-on-disk bytes, atomic-write."""
    _recompute_crc(data)
    raw = _canonical_dumps(data)
    # The validator must accept what we are about to write. If it rejects,
    # bail with the validator's exit -- never persist an invalid store.
    #
    # Validate the BYTES, not just the dict. validate_store() inspects the
    # in-memory object, so any rule that lives in the SERIALIZED form (unique
    # key names, literal key spelling) was unchecked on this path: the writer
    # could hand _atomic_write a store the firmware rejects. Re-parsing the
    # exact bytes about to be persisted is the only check that covers what the
    # writer actually produced.
    try:
        _validator.strict_loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        print(f"[FAIL] {path}: serialized store is not valid JSON: {e}", file=sys.stderr)
        sys.exit(1)
    _validator.validate_store(data, raw, recompute_crc=False)
    try:
        _atomic_write(path, raw)
    except OSError as e:
        # Filesystem-level errors (read-only mount, ENOSPC, EPERM)
        # map to rc=2 (usage error) per the documented exit-code
        # contract; they are NOT validation failures (rc=1).
        print(f"[FAIL] {path}: write failed: {e}", file=sys.stderr)
        sys.exit(2)


# ---- Mutation audit log -----------------------------------------------------

# JSONL schema version pinned to match include/boot/boot_audit_codes.h
# BOOT_AUDIT_JSONL_SCHEMA_VERSION. Bump when the wire format changes.
MUTATION_LOG_SCHEMA_VERSION = 1


def _requester_label() -> str:
    """Best-effort identification of who ran bootcfg. Format kept short
    so it fits in the JSONL line without bloating the audit trail."""
    try:
        user = getpass.getuser()
    except Exception:  # pragma: no cover -- some CI containers lack passwd
        user = "unknown"
    try:
        host = socket.gethostname()
    except Exception:
        host = "unknown"
    return f"bootcfg.py user={user} host={host}"


def _append_mutation_log(
    log_path: Path | None,
    kind: str,
    target_id: str,
    prior_crc: str | None,
    new_crc: str,
    note: str = "",
) -> None:
    """Append one JSONL record to the mutation log. No-op when
    log_path is None (caller did not pass --mutation-log).

    The mutation log is the disk-side `mutations.jsonl` half of the
    boot policy audit trail. Live-boot mutations (the deferred user-mode
    bootcfg binary) will append to `X:\\Boot\\mutations.jsonl`
    directly; offline runs append wherever the operator points the path.
    CI / installer pipelines should pass an explicit --mutation-log so
    the trail survives the host-side staging step.

    Best-effort: write failures emit a [WARN] but never fail the
    mutation itself (the bootentries.json edit already succeeded
    atomically before this call). Otherwise a read-only mount on the
    log location would cancel a successful entry edit -- the wrong
    direction.
    """
    if log_path is None:
        return
    record = {
        "ts": int(time.time()),
        "schema": MUTATION_LOG_SCHEMA_VERSION,
        "kind": kind,
        "target_id": target_id,
        "prior_crc": prior_crc,
        "new_crc": new_crc,
        "requester": _requester_label(),
    }
    if note:
        record["note"] = note
    line = json.dumps(record, ensure_ascii=True, sort_keys=True) + "\n"
    try:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with log_path.open("a", encoding="ascii") as f:
            f.write(line)
            f.flush()
            try:
                os.fsync(f.fileno())
            except OSError:
                pass
    except OSError as e:
        print(f"[WARN] mutation-log: append failed: {e}", file=sys.stderr)


def _resolve_mutation_log(args: argparse.Namespace) -> Path | None:
    """Honor --mutation-log if provided. Empty / unset -> None (skip)."""
    raw = getattr(args, "mutation_log", None)
    if not raw:
        return None
    return Path(raw)


# ---- Subcommand: list -------------------------------------------------------


def cmd_list(args: argparse.Namespace) -> int:
    path = Path(args.path)
    data, _ = _load(path)
    print(f"store: {path}")
    print(f"  schema_version: {data['schema_version']}")
    print(f"  crc32:          {data['crc32']}")
    print(f"  entries:        {len(data['entries'])}")
    for i, e in enumerate(data["entries"]):
        flags = ",".join(e.get("flags", [])) or "(none)"
        sk = e.get("sort_key", "")
        print(f"    [{i}] id={e['id']!r}  kind={e['kind']!r}  "
              f"sort_key={sk!r}  flags={flags}")
        title = e.get("title", "")
        if title:
            print(f"        title: {title!r}")
    return 0


# ---- Subcommand: add --------------------------------------------------------


def cmd_add(args: argparse.Namespace) -> int:
    path = Path(args.path)
    try:
        # Same strict rules as a store read -- an entry authored with a repeated
        # or escaped key must not be written into a store the firmware rejects.
        new_entry = _validator.strict_loads(args.json)
    except json.JSONDecodeError as e:
        print(f"[FAIL] --json: parse error: {e}", file=sys.stderr)
        return 2
    if not isinstance(new_entry, dict):
        print("[FAIL] --json: must be a JSON object", file=sys.stderr)
        return 2
    if "id" not in new_entry:
        print("[FAIL] --json: entry has no id", file=sys.stderr)
        return 2

    data, _ = _load(path)
    prior_crc = data.get("crc32")
    new_id = new_entry["id"]
    for e in data["entries"]:
        if e["id"] == new_id:
            print(f"[FAIL] add: id {new_id!r} already exists", file=sys.stderr)
            return 3
    if len(data["entries"]) + 1 > _validator.MAX_ENTRIES:
        print(f"[FAIL] add: store at MAX_ENTRIES={_validator.MAX_ENTRIES}",
              file=sys.stderr)
        return 1
    data["entries"].append(new_entry)
    _validate_then_write(path, data)
    _append_mutation_log(_resolve_mutation_log(args), "add", new_id,
                         prior_crc, data["crc32"])
    print(f"add: {new_id!r} appended; new store CRC {data['crc32']}")
    return 0


# ---- Subcommand: remove -----------------------------------------------------


def cmd_remove(args: argparse.Namespace) -> int:
    path = Path(args.path)
    data, _ = _load(path)
    prior_crc = data.get("crc32")
    target = args.id
    before = len(data["entries"])
    data["entries"] = [e for e in data["entries"] if e["id"] != target]
    if len(data["entries"]) == before:
        print(f"[FAIL] remove: id {target!r} not found", file=sys.stderr)
        return 3
    if len(data["entries"]) == 0:
        print("[FAIL] remove: store would be empty (>=1 entry required)",
              file=sys.stderr)
        return 1
    _validate_then_write(path, data)
    _append_mutation_log(_resolve_mutation_log(args), "remove", target,
                         prior_crc, data["crc32"])
    print(f"remove: {target!r} dropped; new store CRC {data['crc32']}")
    return 0


# ---- Subcommand: set-default ------------------------------------------------


def cmd_set_default(args: argparse.Namespace) -> int:
    """Reassign sort_keys so <id> is the lowest among ACTIVE entries.

    Codex design review caught that the policy ladder picks the lowest
    sort_key (tie-break by array order). Merely reordering the array does
    not change the default; we must rewrite sort_keys.

    Strategy: lex-min the sort_key of the target entry by prefixing "00-",
    and renumber every other ACTIVE entry's sort_key with a "NN-" prefix
    in the original active-array order so the picked entry wins by lex
    compare. Inactive / hidden entries keep their existing sort_keys --
    the ladder filters them out before the default tie-break runs.
    """
    path = Path(args.path)
    data, _ = _load(path)
    prior_crc = data.get("crc32")
    target = args.id
    target_idx = None
    for i, e in enumerate(data["entries"]):
        if e["id"] == target:
            target_idx = i
            break
    if target_idx is None:
        print(f"[FAIL] set-default: id {target!r} not found", file=sys.stderr)
        return 3
    target_entry = data["entries"][target_idx]
    if "active" not in target_entry.get("flags", []):
        print(f"[FAIL] set-default: id {target!r} is not active "
              f"(ladder filters non-active entries before tie-break)",
              file=sys.stderr)
        return 1

    # Active entries (excluding target) get sort_keys 01-..., 02-..., ...;
    # target gets 00-default. The "NN-" prefix is the canonical sort_key
    # convention used by the example store + validator.
    target_entry["sort_key"] = f"00-{target_entry['id']}"
    n = 1
    for e in data["entries"]:
        if e is target_entry:
            continue
        if "active" not in e.get("flags", []):
            continue
        e["sort_key"] = f"{n:02d}-{e['id']}"
        n += 1

    _validate_then_write(path, data)
    _append_mutation_log(_resolve_mutation_log(args), "set-default", target,
                         prior_crc, data["crc32"],
                         note=f"sort_key={target_entry['sort_key']!r}")
    print(f"set-default: {target!r} now lowest sort_key "
          f"({target_entry['sort_key']!r}); new store CRC {data['crc32']}")
    return 0


# ---- Subcommand: emit-seed --------------------------------------------------


def cmd_health(args: argparse.Namespace) -> int:
    """Print the last health-gate report from a copy of X:\\Boot\\health.jsonl.

    Read-only -- never writes the JSONL file. Resilient to a partial
    last line (treats incomplete tail as "no report") so a host that
    snapshotted the file mid-write does not crash the tool.
    """
    path = Path(args.path)
    if not path.is_file():
        print(f"health: {path} not found", file=sys.stderr)
        return 1
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        print(f"health: cannot read {path}: {e}", file=sys.stderr)
        return 1
    lines = [ln for ln in text.splitlines() if ln.strip()]
    if not lines:
        print(f"health: {path} is empty -- no gate has run yet")
        return 0
    try:
        rec = json.loads(lines[-1])
    except json.JSONDecodeError:
        # Tail may be partial (writer crashed mid-flush). Try the
        # previous line.
        if len(lines) >= 2:
            try:
                rec = json.loads(lines[-2])
            except json.JSONDecodeError:
                print(f"health: last line malformed; cannot parse",
                      file=sys.stderr)
                return 1
        else:
            print(f"health: only line is malformed; cannot parse",
                  file=sys.stderr)
            return 1
    schema = rec.get("schema_version", "?")
    aggregate = rec.get("aggregate", "?")
    selected = rec.get("selected_entry_id", "?")
    print(f"Last health gate (schema_version={schema})")
    print(f"  aggregate         : {aggregate}")
    print(f"  selected_entry_id : {selected}")
    if "tries_left" in rec:
        print(f"  tries_left        : {rec['tries_left']}")
        print(f"  tries_done        : {rec['tries_done']}")
    print(f"  cur_boot_ctr      : "
          f"{'present' if rec.get('cur_boot_ctr_present') else 'absent'}")
    print(f"  subset            : "
          f"{'present' if rec.get('subset_present') else 'absent'}")
    counts = rec.get("counts", {})
    print(f"  required          : "
          f"ok={counts.get('required_ok', 0)} "
          f"soft={counts.get('required_soft', 0)} "
          f"hard={counts.get('required_hard', 0)} "
          f"skipped={counts.get('required_skipped', 0)}")
    print(f"  wanted            : "
          f"ok={counts.get('wanted_ok', 0)} "
          f"soft={counts.get('wanted_soft', 0)} "
          f"hard={counts.get('wanted_hard', 0)} "
          f"skipped={counts.get('wanted_skipped', 0)}")
    print(f"  checks:")
    for c in rec.get("checks", []):
        ran = c.get("ran", False)
        ran_mark = "[run]" if ran else "[skip]"
        print(f"    {ran_mark} {c.get('name', '?'):24s} "
              f"kind={c.get('kind', '?'):8s} "
              f"result={c.get('result', '?')}")
    return 0


def cmd_emit_seed(args: argparse.Namespace) -> int:
    """Idempotent: writing twice yields a byte-identical store."""
    path = Path(args.path)
    prior_crc = None
    if path.is_file():
        try:
            prior_data, _ = _load(path)
            prior_crc = prior_data.get("crc32")
        except SystemExit:
            # Existing file failed validation; treat as no prior state
            # for the audit log -- the seed write is what brings it
            # back to a known-good state.
            prior_crc = "INVALID"
    data = _seed_store()
    _validate_then_write(path, data)
    # target_id="default" is the v1 schema-pinned stable label for the
    # store-level seed operation (NOT an entry id; the 3-entry seed
    # contains slot-a / slot-b / recovery). Schema contract pinned in
    # docs/boot/boot-history-schema.md; do not change without bumping
    # the mutation-log schema version.
    _append_mutation_log(_resolve_mutation_log(args), "emit-seed", "default",
                         prior_crc, data["crc32"])
    print(f"emit-seed: wrote idempotent default store to {path}; "
          f"CRC {data['crc32']}")
    return 0


# ---- argparse glue ----------------------------------------------------------


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="bootcfg",
        description="Offline boot-entry-store editor (host-side; "
                    "live-boot subcommands deferred).",
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    p_list = sub.add_parser("list", help="dump entries")
    p_list.add_argument("path")
    p_list.set_defaults(func=cmd_list)

    # --mutation-log <path> is honored by every mutating subcommand.
    # Empty / unset -> no log emitted. CI / installer pipelines should
    # always pass it so the boot policy audit trail (the disk-side
    # half of TODO-07's audit feature) survives the host-side staging
    # step. Live-boot mutations (the deferred user-mode bootcfg
    # binary) will append directly to X:\Boot\mutations.jsonl.
    mutation_help = (
        "append a JSONL audit record to <path> after the mutation; "
        "skip when omitted"
    )

    p_add = sub.add_parser("add", help="append an entry (validated)")
    p_add.add_argument("path")
    p_add.add_argument("--json", required=True,
                       help="entry as a JSON object")
    p_add.add_argument("--mutation-log", help=mutation_help)
    p_add.set_defaults(func=cmd_add)

    p_remove = sub.add_parser("remove", help="drop an entry by id")
    p_remove.add_argument("path")
    p_remove.add_argument("id")
    p_remove.add_argument("--mutation-log", help=mutation_help)
    p_remove.set_defaults(func=cmd_remove)

    p_setdef = sub.add_parser(
        "set-default",
        help="reassign sort_keys so id wins the lowest-sort_key tie-break",
    )
    p_setdef.add_argument("path")
    p_setdef.add_argument("id")
    p_setdef.add_argument("--mutation-log", help=mutation_help)
    p_setdef.set_defaults(func=cmd_set_default)

    p_seed = sub.add_parser(
        "emit-seed",
        help="write idempotent 3-entry default store "
             "(slot-a active + slot-b inactive + recovery filtered)",
    )
    p_seed.add_argument("path")
    p_seed.add_argument("--mutation-log", help=mutation_help)
    p_seed.set_defaults(func=cmd_emit_seed)

    p_health = sub.add_parser(
        "health",
        help="print last health-gate report from a copy of "
             "X:\\Boot\\health.jsonl",
    )
    p_health.add_argument("path",
                          help="path to a host-side copy of health.jsonl")
    p_health.set_defaults(func=cmd_health)

    return p


def main(argv: list[str]) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv[1:])
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
