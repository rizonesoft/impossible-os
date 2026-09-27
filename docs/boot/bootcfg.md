<!-- docs: covers=todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md -->
# bootcfg -- Boot Entry Store Editor

> Host-side CLI tool that edits `\EFI\ImpossibleOS\bootentries.json`
> through the canonical schema validator. Owned by the [boot entry
> editor tooling section](../../todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md#11-boot-entry-editor-tooling).
> Live-boot subcommands (set-bootnext-hint, set-oneshot, dump-history)
> are deferred until a user-mode native binary owner ships.

`tools/bootcfg/bootcfg.py` is the single entry point. Every write goes
through [`tools/boot-entry-validate/validate.py`](../../tools/boot-entry-validate/) before
being persisted -- a rejected store does not touch the file. Writes use
the same atomic CoW + fsync durability protocol as the boot-counter
rename path: write tempfile, fsync, rename, fsync parent dir. A host
crash mid-write never leaves a torn or missing store.

## Subcommands

```text
bootcfg list <path>
    Dump the parsed store (schema_version, crc32, per-entry summary).

bootcfg add <path> --json <entry-json>
    Append an entry. The argument is a single JSON object. Fails with
    rc=3 if the entry's `id` already exists. Fails with rc=1 if the
    composite store would not validate (path-escape, schema violation,
    over MAX_ENTRIES, etc.).

bootcfg remove <path> <id>
    Drop the named entry. Fails with rc=3 if `id` is not present.
    Fails with rc=1 if the resulting store would have zero entries
    (the validator requires >= 1).

bootcfg set-default <path> <id>
    Reassign sort_keys so `<id>` is the lowest among ACTIVE entries.
    The policy ladder picks lowest sort_key (with array order as a
    tie-break), so merely reordering the array does NOT change the
    default -- bootcfg rewrites sort_keys to be sound. Fails with
    rc=1 if `<id>` is not active (the ladder filters non-active
    entries before the default tie-break runs).

bootcfg emit-seed <path>
    Write the idempotent 3-entry default store: slot-a (active, split,
    kernel=\boot\kernel.exe), slot-b (inactive, split -- gated on the
    dual-slot A/B-rollback feature), and recovery (active envelope,
    kind=recovery filtered today via supported_kinds_mask until the
    recovery-partition load path ships). Kernel path matches the
    release image's staged location at `<ESP>/boot/kernel.exe`.
    Running twice yields a byte-identical store; safe to wire into
    release / install scripts. See docs/boot/bootstrap.md for the
    full ownership boundary and lifecycle contract.
```

## Exit codes

| rc | meaning                                                                    |
|----|----------------------------------------------------------------------------|
| 0  | success                                                                    |
| 1  | validation failure (printed with `[FAIL]` prefix on stderr)                |
| 2  | usage error (file unreadable, malformed `--json` arg, missing subcommand)  |
| 3  | conflict (id already exists on add, id not found on remove/set-default)    |

## Atomic CoW + fsync durability

The write protocol is the same one boot counters use under [boot-policy.md](boot-policy.md)
"Counter persistence" -- write-new + flush + close + atomic-replace, with
parent-directory fsync on POSIX hosts so the directory entry for both the
new file AND the rename is durable before the call returns. Without the
parent fsync, FAT32 (and ext4 with default mount options) can leave a
missing or stale store after a host crash mid-write. With it, the on-disk
store is always either the old version or the new version -- never torn.

On Windows hosts editing a FAT32 ESP through mtools or the firmware
mount, the host's OS handles directory durability; the directory-fd
fsync silently no-ops (Python's `os.fsync` on a directory fd raises
`OSError` on Windows, which the protocol catches and ignores).

## Live-boot subcommands (deferred)

These are deferred per the editor-tooling section in [the boot-entry TODO](../../todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md#11-boot-entry-editor-tooling):

- `set-bootnext-hint <id>` -- the firmware-layer BootNext signal lives
  in the UEFI variable namespace, not in the on-disk store. The
  bootloader reads `BootCurrent`'s `OptionalData` to map firmware
  selections to entry ids; setting BootNext requires a
  `gRT->SetVariable("BootNext", ...)` call from a user-mode binary
  with NV variable access. Owner: future user-mode bootcfg.
- `set-oneshot <id>` -- one-shot wraps BootNext + a follow-up clear
  on first boot; same UEFI-variable dependency.
- `dump-history` -- the per-decision audit log lives in BlackBox
  JSONL + NVRAM sticky state, both owned by the [policy audit
  section](../../todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md#12-policy-audit-trail-and-rollback-reason-codes).
  Live-boot only.

## Tests

```bash
python3 tools/bootcfg/test_bootcfg.py
```

16 self-contained subprocess tests covering every subcommand, the
validator-reject keeps-store-untouched contract, atomic-write tempfile
cleanup, and the usage-error exit codes. No pytest dependency (mirrors
the `test_validate.py` pattern).

## Cross-references

- [boot-entry-schema.md](boot-entry-schema.md) -- on-disk JSON schema
  + per-kind payload tables.
- [boot-policy.md](boot-policy.md) -- the ladder that consumes
  `sort_key` to pick the default; explains why `set-default`
  rewrites sort_keys instead of just reordering the array.
- [`tools/boot-entry-validate/`](../../tools/boot-entry-validate/) --
  the validator bootcfg wraps for every read + write.
