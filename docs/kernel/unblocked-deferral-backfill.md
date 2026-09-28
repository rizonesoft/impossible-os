<!-- docs: covers=todo/02-kernel-core/TODO-35-unblocked-deferral-backfill.md sources=src/kernel/registry.c,src/kernel/main/boot_storage.c,src/kernel/cpu_security.c,src/kernel/quota/quota_owner.c,src/kernel/eif.c,scripts/overnight/stranded_deferrals.py reviewed=2026-09-28 order=35 -->
# Unblocked-Deferral Backfill

## What is it?

This roadmap file is not a subsystem. It is a one-time cohort of work gathered from other kernel roadmaps: items that were parked waiting on another section, whose blocker later shipped, and which nothing was watching to reopen. A 2026-07-27 run of [`stranded_deferrals.py`](../../scripts/overnight/stranded_deferrals.py) found 48 such parked items across 15 files; triage reopened 39 (38 distinct), kept 8 parked on a second verified blocker, and found 1 already done. The reopened work is grouped into six sections by subsystem, and all six are still open.

## How does it work?

The cohort is closed to new entries, because the gap that produced it is fixed: the section-implementation workflow now runs `stranded_deferrals.py --owner <todo> --section <N>` whenever a section ships, so a newly unblocked item is reopened at its real owner instead of piling up here. The original parked items stay `[/]` in their source files with a cross-reference to their replacement here, and flip to done only when the section here ships.

The triage showed that parked item text goes stale. The registry items blamed a missing `C:` volume, yet [`boot_storage.c`](../../src/kernel/main/boot_storage.c) calls `vfs_init()` and `partition_mount_filesystems()` before `registry_init()`, so the volume is mounted before the registry starts. The real blocker was new: the registry tree in [`registry.c`](../../src/kernel/registry.c) has no SMP lock, and `registry_load_hives()` there has no caller. Other items waited on capabilities that already existed: the fault-recoverable `copy_from_user()` in [`cpu_security.c`](../../src/kernel/cpu_security.c), `quota_charge_chain()` in [`quota_owner.c`](../../src/kernel/quota/quota_owner.c), and `eif_decompress_segment()` in [`eif.c`](../../src/kernel/eif.c).

## What are its interfaces?

| Section | What it backfills | Related page |
| --- | --- | --- |
| 1. Registry tree SMP synchronization and hive durability | A lock for the registry tree; journal, dual-log recovery and lazy writer | [Registry](registry.md) |
| 2. Registry save/restore hive bodies | `RegSaveKey`/`RegRestoreKey` I/O behind the already-shipped privilege checks | [Registry](registry.md) |
| 3. Quota enforcement and job memory accounting | Enforcing stored working-set and pagefile limits; real job memory figures | [Kernel Resource Accounting and Quotas](kernel-resource-accounting-quotas.md) |
| 4. Module identity and ELF unwind registration | Build-ID identity; `.eh_frame_hdr` registration for ELF modules | [Binary Format System](binary-format-system.md) |
| 5. User-buffer hardening | Call sites switching to the fault-recoverable usercopy | [Exception Dispatch and SEH](exception-dispatch-seh.md) |
| 6. Boot, entropy, config and SRM leftovers | Smaller items across boot media, entropy, klog and the security monitor | [Security Reference Monitor](security-reference-monitor.md) |

## How do I use it?

There is nothing to run for the cohort's features, because none has shipped. The check that keeps the cohort visible is the stranded-deferral sweep:

```bash
python3 scripts/overnight/stranded_deferrals.py --gate
```

`--gate` exits nonzero while any stranded item lacks a recorded disposition (reopen, park or done), and the overnight runner will not declare the roadmap complete until it passes. A pass proves every stranded item has been triaged, not that the work is built: the 39 reopened items stay open until the sections here ship.

## What is not implemented yet?

- **Registry SMP safety and hive durability.** No lock exists in `registry.c`, `registry_load_hives()` is not called, and the journal and lazy writer do not exist ([Registry Tree SMP Synchronization and Hive Durability](../../todo/02-kernel-core/TODO-35-unblocked-deferral-backfill.md#1-registry-tree-smp-synchronization-and-hive-durability)).
- **Hive save and restore.** The privilege checks shipped; the I/O behind them does not succeed yet ([Registry Save/Restore Hive Bodies and Transactional Load](../../todo/02-kernel-core/TODO-35-unblocked-deferral-backfill.md#2-registry-saverestore-hive-bodies-and-transactional-load)).
- **Quota enforcement.** Working-set and pagefile limits are stored and returned but not enforced ([Quota Enforcement and Job Memory Accounting](../../todo/02-kernel-core/TODO-35-unblocked-deferral-backfill.md#3-quota-enforcement-and-job-memory-accounting)).
- **ELF unwind and module identity.** ELF modules register no unwind data (PE modules have `.pdata`), and module identity is a bare name ([Module Identity and ELF Unwind Registration](../../todo/02-kernel-core/TODO-35-unblocked-deferral-backfill.md#4-module-identity-and-elf-unwind-registration)).
- **User-buffer hardening.** Several call sites still refuse a legitimate non-NULL user buffer even though `copy_from_user()` can now recover the fault ([User-Buffer Hardening on Fault-Recoverable Usercopy](../../todo/02-kernel-core/TODO-35-unblocked-deferral-backfill.md#5-user-buffer-hardening-on-fault-recoverable-usercopy)).
- **The smaller leftovers** ([Boot, Entropy, Config and SRM Leftovers](../../todo/02-kernel-core/TODO-35-unblocked-deferral-backfill.md#6-boot-entropy-config-and-srm-leftovers)).

## How does it compare with Windows 11 and Linux?

Per the roadmap's comparison table, this cohort backfills parity both operating systems already have: Windows protects its registry with the configuration manager's lock hierarchy and a dual-log hive journal, both systems enforce working-set or memory limits (Windows directly, Linux through cgroups and rlimits), and Linux registers ELF unwind data through `.eh_frame_hdr`. Until the six sections ship, the Impossible OS registry is not safe under concurrent mutation and its quota limits are advisory.

## See also

- [Unblocked-Deferral Backfill roadmap](../../todo/02-kernel-core/TODO-35-unblocked-deferral-backfill.md)
- [Registry](registry.md)
- [Kernel Resource Accounting and Quotas](kernel-resource-accounting-quotas.md)
- [Binary Format System](binary-format-system.md)
