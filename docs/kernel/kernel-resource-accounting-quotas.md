<!-- docs: covers=todo/02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md sources=include/kernel/quota/quota.h,include/kernel/quota/quota_ledger.h,include/kernel/quota/quota_policy.h,include/kernel/quota/quota_pressure.h,include/kernel/quota/quota_stall.h,include/kernel/nt/quota_syscall_info.h,include/kernel/nt/quota_pressure_info.h,src/kernel/quota/quota.c,src/kernel/quota/quota_owner.c,src/kernel/quota/quota_config.c,src/kernel/quota/quota_ledger.c,src/kernel/quota/quota_policy.c,src/kernel/quota/quota_pressure.c,src/kernel/quota/quota_stall.c,src/kernel/ob/ob_job.c,include/kernel/ob/ob_job.h,src/kernel/test/test_quota.c reviewed=2026-09-28 order=25 -->
# Kernel Resource Accounting and Quotas

## What is it?

A single kernel authority for charging and limiting every accountable resource: handles, object bodies, namespace entries, pool bytes, registry bytes, ALPC messages, notification state, timers, threads, processes, sections, mapped views and crash buffers, sixteen types in total. Every charge goes through one API, is attributed to a process, its canonical per-user block and its job in one all-or-nothing transaction, and is queryable and diagnosable the same way whichever resource it covers.

## How does it work?

`quota_register_types()` validates and publishes a static const table of resource descriptors (name, accounting unit, default limit, override privilege) at Phase 2 boot, before any consumer can charge against it ([`quota.h`](../../include/kernel/quota/quota.h)). A `quota_block_t` holds one usage, peak, limit and failures record per type behind a single lock; `quota_charge()`, `quota_return()` and `quota_try_transfer()` are atomic and rollback-safe: a refused charge or transfer leaves every usage balance as it was, and only increments that type's `failures` counter.

Every task carries its own process block plus a reference to a canonical per-SID user block (`ACCESS_TOKEN.QuotaBlock`), and `quota_charge_chain()` charges the whole ownership chain (process, user and job) as one prefix-rollback transaction: a refusal partway through returns the usage already charged to earlier layers, though a peak raised by one of those earlier charges stays raised. Each successful chain charge returns a `quota_charge_receipt_t` carrying a monotonic generation token, so a stale or duplicate `quota_return_chain()` call is a safe no-op rather than a double return. A refcounted ledger (`quota_ledger_t`) lets an obligation outlive the task slot that created it: a per-task drain and quiesce gate serializes charging against job-membership changes, and a return made at raised IRQL defers its completion to a threaded DPC rather than doing unbounded work in interrupt context.

Two layers sit on top of the charge and return core. A resource-pressure state machine ([`quota_pressure.c`](../../src/kernel/quota/quota_pressure.c)) derives four hysteresis levels (normal, watch, warning, critical) from budget saturation across sixteen domains, publishes transitions through the Kernel Notification Facility and emits a versioned `QUOTA_FAILURE_RECORD` on every refusal; nomination is cooperative only, Windows-style, never a kill. A stall-telemetry layer ([`quota_stall.c`](../../src/kernel/quota/quota_stall.c)) accumulates per-resource (cpu, mem, io) "some" and "full" wait time in a PSI-shaped record with avg10, avg60 and avg300 averages and an explicit VALID flag, so an unwired seam reads as unmeasured rather than as calm. At the syscall boundary, `ProcessQuotaLimits` on the existing `NtQueryInformationProcess`/`NtSetInformationProcess` projects the process block into the Windows `QUOTA_LIMITS`/`QUOTA_LIMITS_EX` structures ([`quota_syscall_info.h`](../../include/kernel/nt/quota_syscall_info.h)); the Job Object class `JobObjectQuotaLimitInformation` uses its own `JOBOBJECT_QUOTA_LIMIT_INFORMATION`, a count plus one usage, peak, limit and failures row per resource type ([`ob_job.h`](../../include/kernel/ob/ob_job.h)). Every widening request is gated behind `SeIncreaseQuotaPrivilege`.

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| `quota_register_types()`, `quota_types_dump()` | The 16-type resource taxonomy, validated and published at Phase 2 boot ([`quota.h`](../../include/kernel/quota/quota.h)) |
| `quota_block_create()`, `quota_charge()`, `quota_return()`, `quota_try_transfer()`, `quota_set_limit()` | The core per-block charge, return and transfer API |
| `quota_charge_chain()`, `quota_return_chain()`, `quota_rollup_by_sid()` | Process, user and job chain charging and per-SID aggregation ([`quota_owner.c`](../../src/kernel/quota/quota_owner.c)) |
| `quota_ledger_t`, `quota_charge_adjust()`, `quota_gate_quiesce()` | The refcounted obligation ledger, transactional resize, and the membership drain gate ([`quota_ledger.h`](../../include/kernel/quota/quota_ledger.h)) |
| `quota_policy_set()`, `ProcessQuotaLimits` (class 1) | Native query and set of per-process pool, time and working-set limits ([`quota_policy.h`](../../include/kernel/quota/quota_policy.h)) |
| `JobObjectQuotaLimitInformation` (`0x1000`), `JOBOBJECT_QUOTA_LIMIT_INFORMATION` | Per-resource usage, peak, limit and failures rows for a Job Object, set in one privileged transaction ([`ob_job.h`](../../include/kernel/ob/ob_job.h)) |
| `quota_pressure_system_level()`, `QUOTA_FAILURE_RECORD` | The four-level pressure state machine and the versioned quota-failure event ([`quota_pressure.h`](../../include/kernel/quota/quota_pressure.h)) |
| `SystemResourcePressureInformation` (`0x1003`) | Per-resource stall telemetry with a VALID flag per domain ([`quota_pressure_info.h`](../../include/kernel/nt/quota_pressure_info.h), backed by [`quota_stall.h`](../../include/kernel/quota/quota_stall.h)) |
| `quota_dump()`, `quota_dump_crash()` | Serial dashboard of live blocks, and its try-lock, allocation-free panic-path form |

## How do I use it?

The subsystem is always on from Phase 2 boot; there is no boot flag. From kernel C, charge through `quota_charge_chain()` against a `struct task *`; from user mode, query or set limits through `NtQueryInformationProcess`/`NtSetInformationProcess` with `ProcessQuotaLimits`, or through a Job Object handle with `JobObjectQuotaLimitInformation`.

```bash
bash scripts/test.sh SUITE=quota   # charge/return, ledger, syscall, pressure, stall suites
make test-quota                    # shorthand for the same suite
```

`quota_dump()` renders every live per-SID user block to serial on demand; `quota_dump_crash()` is the form wired into the panic path: it takes the quota registry with a try-lock and allocates nothing, but its output still goes through `klog`, which takes the klog lock, so a panic that interrupted logging can stall before the quota lines appear.

## What is not implemented yet?

- **Pool and paged/nonpaged allocation charging.** No pool-class allocator exists to hook yet, only one unified arena, so `kmalloc`/`pmm_alloc_contiguous` traffic is not charged to any owner ([Pool and Allocation Quota Integration](../../todo/02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md#5-pool-and-allocation-quota-integration)).
- **Registry byte and watcher charging, and finite per-user caps.** The `quota.user.<type>` tunable mechanism ships and re-limits live blocks, but every taxonomy default stays unlimited because no production privileged setter exists, and registry value slots have no charge point yet ([Registry, ALPC, Notification Quotas](../../todo/02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md#6-registry-alpc-notification-quotas)).
- **Handle-table and object-body charging in the Object Manager.** Blocked on a handle-table reservation primitive that can report `STATUS_QUOTA_EXCEEDED`, and on a Directory `on_delete` path so name-entry charges are not leaked at teardown ([Object Manager Charge Points](../../todo/02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md#13-object-manager-charge-points)).
- **Bounded charge-path latency under real contention.** The charge, ledger and pressure guarantees are proven by deterministic interleaving tests, not by measured parallel contention; that bounding work is tracked as one deferred unit ([Infrastructure-Gated Charge-Path Bounding and Stall Lanes](../../todo/02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md#17-infrastructure-gated-charge-path-bounding-and-stall-lanes)).
- **Per-process network I/O accounting.** CPU and disk I/O are charged per task, but socket send and receive bytes are not ([Per-Process Network I/O Accounting](../../todo/02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md#20-per-process-network-io-accounting)).

## How does it compare with Windows 11 and Linux?

Impossible OS unifies what both operating systems keep split: Windows charges quota per `EPROCESS` and token `QUOTA_BLOCK` with no cross-layer rollback or receipt abstraction, and Linux spreads the same problem across `rlimit`, cgroups and `quotactl` with no shared charge API. A single sixteen-type registry, an atomic chain charge with all-or-nothing rollback, generation-tagged receipts and a refcounted ledger that lets an obligation outlive its creating task have no direct analogue in either. Pressure reporting borrows the shape of Linux PSI (`some`/`full`, avg10/60/300) while adding a per-domain honesty flag neither Windows low-memory notifications nor Linux PSI carry. Where Impossible OS still falls short of both: handle and pool quota are Windows features with no working charge point here yet.

## See also

- [Kernel Resource Accounting and Quotas roadmap](../../todo/02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md)
- [Object Manager](object-manager.md)
- [Process Model Extensions](process-model-extensions.md)
- [Native API and the SSDT](native-api-ssdt.md)
- [Kernel Notification Facility](kernel-notification-facility.md)
