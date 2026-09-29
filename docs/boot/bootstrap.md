<!-- docs: covers=todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md sources=scripts/release/build-image.sh,scripts/release/build-iso.sh,tools/bootcfg/bootcfg.py,scripts/build.sh reviewed=2026-09-28 -->
# Bootstrap & First-Install Entry Seeding

> **Owner:** [Boot Entry Store, Menu & Policy / Bootstrap and First-Install Entry Seeding](../../todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md#16-bootstrap-and-first-install-entry-seeding).
>
> **Scope:** Idempotent host-side / installer-side seeding of `\EFI\ImpossibleOS\bootentries.json`. The boot-entry store schema is owned by [boot-entry-schema.md](boot-entry-schema.md); this doc owns the *who-writes-it-when* contract that ships entries onto a freshly-installed ESP.

The bootloader cannot write `bootentries.json` -- it can only read it. Something else must put a valid store on the ESP before the first boot, or the bootloader's in-firmware fallback (`boot_entries_synthesize_fallback`) takes over with a single synthesized entry. This file pins which actor owns that write at each step of the disk-build / install / first-boot lifecycle, and the staged-vs-seeded path contract that the seeded entries depend on.

## Ownership boundary

| Lifecycle step                              | Actor                                            | Writes `bootentries.json`?                                | Notes                                                              |
| ------------------------------------------- | ------------------------------------------------ | --------------------------------------------------------- | ------------------------------------------------------------------ |
| Reproducible release-image build (raw/USB)  | `scripts/release/build-image.sh`                 | YES -- calls `bootcfg.py emit-seed` after ESP staging      | Idempotent; CI re-runs do not perturb the image hash               |
| Reproducible release-image build (VHD/VHDX) | `scripts/release/build-image.sh` -> `to-vhdx.sh` | YES (inherited from raw image)                            | Conversion preserves the seeded store byte-for-byte                |
| Hybrid ISO build                            | `scripts/release/build-iso.sh`                   | YES (inherited from raw image)                            | El Torito + bootable USB stick path                                |
| Installer (post-image-build)                | Future installer-release script (UNSHIPPED)      | Will call `python3 tools/bootcfg/bootcfg.py emit-seed <mounted-esp>/EFI/ImpossibleOS/bootentries.json` after layout | Future owner; today the release-image bake already covers it       |
| First boot on a fresh install               | Bootloader (`bootx64.c`)                         | NO (read-only); SYNTHESIZES single-entry fallback if absent | Recoverable but degraded -- operator sees `selection_reason=FALLBACK_STORE_INVALID` |
| Operator edits offline                      | `tools/bootcfg/bootcfg.py`                       | YES (`add` / `remove` / `set-default` / `emit-seed`)      | Atomic CoW + fsync; validator gates every write                    |
| Operator edits live-boot                    | Future user-mode `bootcfg.exe` (UNSHIPPED)       | Will write via UEFI runtime variable + ESP mount          | Live-boot subcommands `set-bootnext-hint` / `set-oneshot` / etc.   |

**Rule:** every actor that ships an ESP must seed the store. Letting the bootloader fall back is a degraded mode, not a default mode -- the synthesized fallback envelope publishes `selection_reason=FALLBACK_STORE_INVALID` in audit, which signals "the install was incomplete" not "this is the normal path".

## The 3-entry default seed

`python3 tools/bootcfg/bootcfg.py emit-seed <mounted-esp>/EFI/ImpossibleOS/bootentries.json` writes three entries with deterministic ids derived from the `(machine_id, kind, slot)` tuple. With `machine_id=""` (wildcard), the ids reduce to kind+slot strings:

```
slot-a    kind=split,    flags=[active], sort_key=00-slot-a, root=A
slot-b    kind=split,    flags=[],       sort_key=50-slot-b, root=B
recovery  kind=recovery, flags=[active], sort_key=99-recovery
```

### Why deterministic?

The seed must produce a byte-identical store on every run. Two implications:

1. **Reproducible release images.** `scripts/release/build-image.sh` re-runs after a clean rebuild produce the exact same image bytes (the `bootentries.json` hash is part of the image hash). CI cannot tell "rebuilt with same inputs" from "no change" if the seed varied per run.
2. **Idempotent recovery.** An operator running `bootcfg.py emit-seed` against an already-seeded ESP gets a no-op write -- old store == new store. Re-seeding after a partial install never corrupts a valid existing store.

### Why slot-b ships inactive

The dual-slot layout and `root="A"` vs `root="B"` partition-selection semantics belong to the [A/B-rollback feature](../../todo/01-boot-platform/TODO-21-ab-boot-rollback.md), currently UNSHIPPED. Today the bootloader's `load_kernel()` reads only the `kernel` path from a SPLIT envelope; `root` is informational. If `slot-b` shipped active, the policy ladder would select it on a slot-a failure but `load_kernel()` would load the same `\boot\kernel.exe` -- falsifying any rollback claim audit reports.

`flags=[]` means the policy ladder's `entry_passes_filter()` rejects slot-b with `BOOT_REJECT_REASON_NOT_ACTIVE` before considering its sort_key. The entry is visible in `bootcfg list` and exists as the committed contract for when [bootloader slot selection logic](../../todo/01-boot-platform/TODO-21-ab-boot-rollback.md#3-bootloader-slot-selection) ships dual-slot semantics; until then it is dormant.

When the bootloader-slot-selection logic ships:
- It gains a `root`-aware partition lookup in `load_kernel()`.
- The seed flips slot-b's `flags` to `["active"]` (single one-line edit in `_seed_store()` plus a re-seed of every staged ESP via the next release-image rebuild).
- The mark-good machinery ([kernel `mark_boot_successful()` syscall](../../todo/01-boot-platform/TODO-21-ab-boot-rollback.md#5-kernel-mark_boot_successful)) starts decrementing the slot-b BLS counter independently of slot-a.

### Why recovery ships active but kind-filtered

The recovery partition + recovery loader belong to the [recovery-partition feature](../../todo/01-boot-platform/TODO-22-recovery-partition.md), currently UNSHIPPED. The seed ships the entry with `kind=recovery` and a canonical placeholder GUID (`49504F53-7265-636F-7665-727900000001`, hex prefix `"IPOS"`) so the store is shape-correct against the schema validator, but `boot_policy_invoke()` initializes `supported_kinds_mask` to admit only SPLIT/SAFE (or UKI in UKI mode) -- the recovery entry is filtered as `KIND_UNAVAILABLE` before the ladder considers it.

This means a recovery entry can never be selected today even though it is present and active. When the [recovery partition disk layout](../../todo/01-boot-platform/TODO-22-recovery-partition.md#1-recovery-partition-in-disk-layout) ships, the disk builder either adopts this placeholder GUID as the canonical recovery-partition GUID or hands the seed a real GUID (one-line update in `SEED_RECOVERY_PARTITION_GUID`). When the [recovery bootloader](../../todo/01-boot-platform/TODO-22-recovery-partition.md#2-recovery-bootloader) ships, `supported_kinds_mask` widens to include `(1u << BOOT_ENTRY_KIND_RECOVERY)` and the entry becomes selectable.

## Staged-vs-seeded path contract

`load_kernel()` treats a validated SPLIT policy decision as the *sole* candidate -- if the seeded kernel path does not open, the loader fails closed rather than falling back to ambient `kernel_paths[]`. This was a deliberate design choice (see the [per-entry-kind validators feature](../../todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md#13-entry-kinds-split-uki-chainload-network-resume) quality review): if the policy ladder says "selected=slot-a kernel=\X", audit must reflect what the loader *actually* loaded, never what the ambient fallback chain found.

The seed therefore MUST encode the path that the disk builder *actually* stages:

| Builder                                | Stages kernel at                       | Seed `kernel` field |
| -------------------------------------- | -------------------------------------- | ------------------- |
| `scripts/release/build-image.sh`       | `<ESP>/boot/kernel.exe`                | `\boot\kernel.exe`  |
| `Makefile efi_staging` (smoke test)    | `<ESP>/boot/kernel.exe`                | (no seed -- bootloader fallback) |

The Makefile-built `disk.img` does NOT include a `bootentries.json`. The bootloader synthesizes a single-entry fallback envelope with `payload_present=0`, which means `has_kernel=0`, which means `policy_owns_kernel_path` is false, which means the ambient `kernel_paths[]` search runs and finds `\boot\kernel.exe`. This is the documented "degraded but recoverable" path -- works, but produces `selection_reason=FALLBACK_STORE_INVALID` in audit.

The release image (`scripts/release/build-image.sh`) DOES ship a seeded store. Its seed therefore MUST use `\boot\kernel.exe` to match the staged location, or the policy-authoritative open fails and the image refuses to boot.

If a future builder stages the kernel elsewhere, it MUST either (a) re-seed with the new path before mounting the ESP, or (b) re-stage `kernel.exe` at `\boot\kernel.exe` so the canonical seed continues to work. The seed path is part of the boot contract, not an implementation detail.

## First-boot self-seed (DEFERRED)

The original §16 plan included a first-boot self-seed branch: if the bootloader sees missing `bootentries.json` + present recovery partition + a known-good slot id, synthesize the 3-entry default atomically. This handles wiped-ESP recovery (operator destroys ESP, kernel is still on slot-A, recovery partition still has `recovery.exe`).

This branch is DEFERRED until the [recovery-partition disk layout](../../todo/01-boot-platform/TODO-22-recovery-partition.md#1-recovery-partition-in-disk-layout) and [A/B dual-slot disk layout](../../todo/01-boot-platform/TODO-21-ab-boot-rollback.md#2-dual-slot-disk-layout) both ship. Without those producers:

- The "recovery partition present" check has no GPT layout to inspect.
- The "known-good slot id" check has no per-slot health metadata to read.
- A synthesized 3-entry default would reference partitions and root markers that do not exist; the synthesized store would be worse than the existing 1-entry fallback.

The existing 1-entry fallback in `boot_entries_synthesize_fallback` covers "missing store" today. First-boot self-seed lands as a recovery-partition follow-up when the recovery layout is ready.

## Bootstrap order (today)

```
1. Build artifacts -- scripts/build.sh produces build/kernel.exe + build/uefi/bootx64.efi
2. Stage ESP layout -- scripts/release/build-image.sh:
     <ESP>/EFI/BOOT/BOOTX64.EFI    (bootloader)
     <ESP>/boot/kernel.exe         (kernel)
     <ESP>/EFI/ImpossibleOS/boot.conf
     <ESP>/IPOS/role.txt           (media-role marker)
3. Seed entry store -- bootcfg.py emit-seed:
     <ESP>/EFI/ImpossibleOS/bootentries.json  (3-entry slot-a/slot-b/recovery)
4. (Optional) Stage manifest -- when --manifest passed:
     <ESP>/IPOS/manifest.json
5. Write filesystem to image -- mformat / mcopy into the raw image
6. Convert to release artifacts -- raw -> vhdx / vdi / iso
```

## Bootstrap order (future, when the installer-release release script ships)

```
1. Image builder ships pristine artifact -- via the steps above
2. Operator runs installer on target hardware
3. Installer partitions the disk -- creates ESP + slot-A + slot-B + recovery partitions
4. Installer copies image files onto each partition
5. Installer calls python3 tools/bootcfg/bootcfg.py emit-seed <mounted-esp>/EFI/ImpossibleOS/bootentries.json
     producing the 3-entry default
6. Installer reboots
7. Bootloader reads bootentries.json (seeded) -- selects slot-A by sort_key
8. Kernel reaches desktop -- health gate marks slot-A good
```

## Failure modes

| Failure                                    | Symptom                                              | Recovery                                              |
| ------------------------------------------ | ---------------------------------------------------- | ----------------------------------------------------- |
| Image build ran but emit-seed skipped      | Bootloader synthesizes 1-entry fallback; audit shows `selection_reason=FALLBACK_STORE_INVALID` | Operator runs `python3 tools/bootcfg/bootcfg.py emit-seed <mounted-esp>/EFI/ImpossibleOS/bootentries.json` after boot |
| Seed kernel path drifted from staged path  | `load_kernel()` returns `EFI_NOT_FOUND`; failure screen | Re-stage kernel at `\boot\kernel.exe` OR re-seed with the new path |
| Store corrupt (CRC mismatch / oversized)   | Parser rejects; bootloader synthesizes fallback      | Same as above                                         |
| Operator removed `bootentries.json`        | Bootloader synthesizes fallback                      | `bootcfg.py emit-seed`                                |
| All three seeded entries somehow inactive  | Ladder reports `FALLBACK_NO_VIABLE`                  | Should not happen with the canonical seed; if it does, re-seed |

## References

- Schema: [boot-entry-schema.md](boot-entry-schema.md)
- Policy ladder: [boot-policy.md](boot-policy.md)
- bootcfg CLI: [bootcfg.md](bootcfg.md)
- Loader vars: [loader-vars.md](loader-vars.md)
- TODO files:
  - [Boot Entry Store, Menu & Policy](../../todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md) (owner)
  - [A/B Boot Rollback](../../todo/01-boot-platform/TODO-21-ab-boot-rollback.md) (slot-b activation gate)
  - [Recovery Partition](../../todo/01-boot-platform/TODO-22-recovery-partition.md) (recovery GUID adoption + load path gate)
  - [Release Artifacts](../../todo/15-installer-release/TODO-01-release-artifacts.md) (future installer)
