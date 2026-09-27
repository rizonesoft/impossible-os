<!-- docs: covers=todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md sources=src/kernel/main/boot_health_check.c,src/boot/uefi/boot_entries_parser.c,include/boot/boot_health_handoff.h,include/kernel/boot_health_check.h reviewed=2026-09-28 -->
# Boot Health Gate

> **Owner:** [Per-Entry Health-Gated Mark-Good](../../todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md#14-per-entry-health-gated-mark-good). Layered ABOVE
> [Kernel `mark_boot_successful()`](../../todo/01-boot-platform/TODO-21-ab-boot-rollback.md#5-kernel-mark_boot_successful)
> (slot-level, unshipped today) and
> [Boot Status Policy and Boot Success Ledger](../../todo/02-kernel-core/TODO-02-kernel-configuration-policy.md#10-boot-status-policy-and-boot-success-ledger)
> (acceptance ledger, unshipped today). This document covers the entry-level
> gate + configurable health checks that layer on top.

## Why this exists

The bootloader decrements an entry's `tries_left` counter before handing off
to the kernel. Without a positive-confirmation half, the counter would
decrement forever -- every boot looks identical to a failed boot.

Greenboot (Fedora) and `systemd-bless-boot.service` (Linux) solve this with
the same shape: after the boot has reached a known-good state, userspace
calls a "mark successful" API. Failed boots leave the counter decremented;
the rollback / demote logic in the bootloader takes over once `tries_left`
hits zero.

Impossible OS implements the same model, with three differences:

1. **State-bound consume.** The mark-good record carries the exact
   `{entry_id, tries_left, tries_done}` triple the bootloader wrote
   post-decrement. The next bootloader run only deletes the counter
   file whose filename matches that triple. A stale or replayed
   record cannot survive an intervening failed boot.
2. **Per-decision audit.** Every gate run lands as a one-line JSONL
   record in `X:\Boot\health.jsonl` (the same BlackBox-primary pattern
   as the policy audit in [`boot-history-schema.md`](boot-history-schema.md)).
   Operators see what each boot's checks reported without scraping
   serial.
3. **Per-entry override** via the `health_check_subset` envelope field.
   Recovery and other special entries can skip wanted checks (e.g.
   network-reachable) by listing only the required-for-that-entry
   checks in their envelope; the kernel runs the intersection of the
   subset and the registered checks.

## Lifecycle

```
Bootloader (pre-EBS)
  policy_consume_mark_good_var()       <- consume outstanding MarkGood (if any)
  policy_scan_counters()
  boot_policy_decide()
  policy_counter_decrement()           <- tries_left -= 1
  policy_write_cur_boot_ctr()          <- BS+RT var, kernel reads at gate time
  policy_write_health_subset()         <- BS+RT var iff envelope has subset
  -> kernel handoff

Kernel Phase 3
  boot_audit_publish()
  boot_health_check_register_defaults()
  boot_health_check_run()              <- iterate registry, write JSONL
    -> aggregate == PASS
      -> boot_status_note_health_pass()  <- records the verdict only
         <- the boot-status accepted transition later calls
            mark_entry_successful(entry_id) -> ImpossibleOS-MarkGood, but only
            once the boot also reaches its acceptance stage (single bless owner)

Next bootloader
  policy_consume_mark_good_var() reads MarkGood, finds matching counter,
    deletes counter, clears MarkGood. State binding mismatch -> clear
    without deleting (stale / replayed).
```

## Cross-boot UEFI variables

All under `IMPOSSIBLE_OS_VENDOR_GUID`:

| Variable | Attrs | Writer | Reader | Lifetime |
|---|---|---|---|---|
| `ImpossibleOS-CurBootCtr`   | BS+RT      | bootloader pre-EBS (post-decrement) | kernel post-EBS  | this boot only |
| `ImpossibleOS-HealthSubset` | BS+RT      | bootloader pre-EBS (if envelope has subset) | kernel post-EBS | this boot only |
| `ImpossibleOS-MarkGood`     | NV+BS+RT   | kernel mid-boot (health pass) | bootloader next boot | across reboot |

Layout, magic, version, and CRC fields are pinned in
[`include/boot/boot_health_handoff.h`](../../include/boot/boot_health_handoff.h).

A kernel-side reserved-name guard in `NtSetSystemEnvironmentValueEx`
refuses user-mode writes to the four variable names in the
`{ImpossibleOS-MarkGood, ImpossibleOS-CurBootCtr,
ImpossibleOS-HealthSubset, ImpossibleOS-BootSticky}` set. Defense in
depth on top of the state-binding -- an attacker who could bypass
the syscall guard would still have to guess the live counter triple
to win.

## Registered checks

The default check set is registered by `boot_health_check_register_defaults()`
during Phase 3 init. New checks can register via `boot_health_check_register()`
before scheduler enable (single-CPU context, no locking required).

| Name | Kind | Default result | Notes |
|---|---|---|---|
| `desktop_ready` | required | OK iff `kernel_subsystem_ready(SUBSYS_DESKTOP)` | Phase 3 reached the WM init point |
| `no_boot_err`   | required | OK iff klog ring has no LOG_ERROR/LOG_FATAL with subsystem tag starting with "boot" | catches `boot_*` subsystem failures that didn't halt |
| `no_panic`      | required | always OK (panic_screen halts) | placeholder; future panic-counter persistence path swaps this for a real query |
| `x_mountable`   | wanted   | OK iff `vfs_is_mounted('X')` | BlackBox mounted |
| `network_reachable` | wanted | SKIPPED today | no NIC stack yet; will return reachability when the network TODO ships |
| `no_service_crash_60s` | wanted | SKIPPED today | no service supervisor yet; will report crashes when the supervisor TODO ships |

### Result semantics

- `OK` -- check passed.
- `SOFT_FAIL` -- check failed in a non-blocking way; logged in the
  JSONL record, never affects the aggregate for wanted checks. A
  required check returning `SOFT_FAIL` aggregates as indeterminate
  (the boot did not cleanly pass).
- `HARD_FAIL` -- check failed in a blocking way; required `HARD_FAIL`
  forces indeterminate.
- `SKIPPED` -- the check's underlying capability is not present yet
  (e.g. no NIC stack). Required `SKIPPED` aggregates as indeterminate
  -- we cannot prove the boot is good if a required check could not
  run.

### Aggregate

`boot_health_check_aggregate(req_ok, req_soft, req_hard, req_skipped)`
returns `BOOT_HEALTH_AGG_PASS` iff every required check returned OK.
Any required soft/hard/skipped result aggregates as
`BOOT_HEALTH_AGG_INDETERMINATE`. The aggregate is cached for the
single-shot guard -- repeated calls within the same boot return the
cached value without re-running.

## Per-entry override

A boot entry can include an optional `health_check_subset` array to
restrict the gate to a subset of registered checks. The bootloader
copies the array into the `ImpossibleOS-HealthSubset` UEFI variable;
the kernel reads it at gate time and runs only the named checks
(intersection with the registry).

Recovery example (`bootentries.json`):

```json
{
  "id": "recovery",
  "title": "Recovery",
  "kind": "recovery",
  "flags": ["active"],
  "sort_key": "99-recovery",
  "machine_id": "",
  "policy_tags": [],
  "health_check_subset": ["desktop_ready", "no_panic"],
  "payload": { ... }
}
```

The recovery entry skips `no_boot_err` (a "soft" failure in recovery
is expected behavior), `x_mountable` (X:\\ may not exist when recovery
is wiping BlackBox), and all wanted checks.

Constraints:
- `health_check_subset` is optional (absent / `[]` -> run the full
  default set).
- Up to 8 names per entry, each 1..23 printable-ASCII characters
  (matching the geometry of `policy_tags`).
- Names not in the kernel registry are ignored with a warning
  (forward-compat: adding a new check name later does not
  retroactively invalidate existing stores).

## Report format

Each `boot_health_check_run()` writes one JSONL line to
`X:\Boot\health.jsonl`. The file rotates at 4 MiB to
`X:\Boot\health.jsonl.1` (one-generation rotation, same protocol as
`history.jsonl`).

```jsonl
{"schema_version":1,"event":"health_gate","aggregate":"pass","selected_entry_id":"impossible-os-a","cur_boot_ctr_present":true,"tries_left":2,"tries_done":1,"subset_present":false,"checks":[...],"counts":{...}}
```

`bootcfg health <path>` tabulates the last line of the JSONL file for
operator triage.

## Check registration API (kernel-side)

Header: [`include/kernel/boot_health_check.h`](../../include/kernel/boot_health_check.h).

```c
int boot_health_check_register(const char *name,
                                enum boot_health_check_kind kind,
                                boot_health_check_fn fn);
```

- `name` -- 1..23 printable ASCII; must not duplicate an existing
  registration.
- `kind` -- `BOOT_HEALTH_KIND_REQUIRED` or `BOOT_HEALTH_KIND_WANTED`.
- `fn` -- returns one of `BOOT_HEALTH_OK / SOFT_FAIL / HARD_FAIL /
  SKIPPED`. Must not panic, must not depend on runtime state beyond
  what is initialized when `boot_audit_publish()` has returned.

Returns 1 on success, 0 if name is invalid / duplicate / registry is
full.

Up to `BOOT_HEALTH_CHECK_MAX` (12) checks may be registered. The
default set occupies 6 slots, leaving 6 for subsystem-specific
checks added by later subsystems.

## Failure model

Every uefi_var / vfs_open / vfs_write call in the gate path is
treated as best-effort:

- UEFI variable write failure -- WARN, no mark-good is written; the
  counter stays decremented and the next boot retries.
- JSONL write failure -- WARN, the in-kernel aggregate is still
  cached and the health PASS verdict is still recorded for the ledger.
- CurBootCtr absent -- WARN, the gate runs and records the verdict, but the
  later accepted-transition mark-good cannot bind the entry (no state-bound
  triple available).

Userland entry is never blocked by a failure in this module.

## Threat model

- **Stale replay.** Defeated by state-binding: the bootloader only
  consumes a mark-good record whose `{entry_id, tries_left,
  tries_done}` triple still has a matching counter file. An
  intervening failed boot rotates the counter filename, so the
  binding stops matching.
- **Forgery via `NtSetSystemEnvironmentValueEx`.** The kernel-side
  reserved-name guard refuses user-mode writes to the four
  Impossible OS-owned variable names. Combined with the state-binding,
  an attacker would need to bypass the syscall guard AND guess the
  live counter triple to forge a successful mark-good.
- **Var write failures.** Treated as "no opinion": kernel falls back
  to "tries_left already decremented, retry next boot"; bootloader
  falls back to "no mark-good available, leave counter alone".
