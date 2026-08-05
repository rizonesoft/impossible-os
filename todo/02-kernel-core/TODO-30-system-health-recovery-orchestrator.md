---
schema_version: 1
id: system-health-recovery-orchestrator
domain: 02-kernel-core
status: active
title: "TODO-30 -- System Health & Recovery Orchestrator"
---

# TODO-30 -- System Health & Recovery Orchestrator

> **Goal:** Add a kernel health orchestrator that turns scattered diagnostics into decisions: degraded mode, service restart hints, live kernel dumps, hang classification, repeated-crash policy, safe-mode escalation, subsystem quarantine, and recovery notifications. This is not a test framework or UI; it is the kernel's runtime judgement layer for whether the system is healthy enough to continue.

> [!IMPORTANT]
> **Current state:** Boot phases know ready/not-ready, boot watchdog work exists in the boot domain, crash dumps and panic UX are planned, logging records failures, and concurrency diagnostics own low-level lock/watchdog tools. No component owns system-wide health state, failure buckets, live dumps, degraded-mode transitions after boot, or policy-driven recovery actions.

## Inputs

- [`src/kernel/main/boot_init.c`](../../src/kernel/main/boot_init.c)
- [`src/kernel/panic.c`](../../src/kernel/panic.c)
- [`src/kernel/klog.c`](../../src/kernel/klog.c)
- → XREF: [`TODO-01-kernel-init-sequencing.md`](./TODO-01-kernel-init-sequencing.md) -- readiness oracle
- → XREF: [`TODO-27-crash-dump-generation.md`](./TODO-27-crash-dump-generation.md) -- live/minidump writer
- → XREF: [`TODO-28-bsod-ux-enhancements.md`](./TODO-28-bsod-ux-enhancements.md) -- crash UX
- → XREF: [`TODO-02-kernel-configuration-policy.md §5, §10`](./TODO-02-kernel-configuration-policy.md) -- Safe Mode reason codes and boot-status policy thresholds
- → XREF: [`03-memory-concurrency/TODO-10-concurrency-diagnostics.md`](../03-memory-concurrency/TODO-10-concurrency-diagnostics.md) -- lockdep/KASAN/watchdog providers

## Outcome

- Kernel has a live health state: healthy, degraded, warning, critical, recovering, shutting down.
- Subsystems publish heartbeats and health probes without depending on UI.
- Recoverable failures can trigger live dumps and targeted recovery before bugcheck.
- Repeated boot/runtime failures escalate to Safe Mode or LastKnownGood.
- Health decisions are visible through notifications, logs, crash dumps, and native queries.

## Implementation Order

| ⭐  | Order | Deliverable                         | Depends On         | Status |
| --- | :---: | ----------------------------------- | ------------------ | :----: |
| 💎  |   1   | Health state model and registry     | T01                |  [ ]   |
| 💎  |   2   | Subsystem heartbeat API             | §1                 |  [ ]   |
| 💎  |   3   | Failure bucket taxonomy             | T27, T28           |  [ ]   |
| ⭐  |   4   | Live kernel dump trigger            | T27                |  [ ]   |
| ⭐  |   5   | Degraded-mode transition engine     | T02                |  [ ]   |
| 💎  |   6   | Recovery action dispatcher          | Ex work items      |  [ ]   |
| ⭐  |   7   | Repeated-failure escalation         | T02, boot rollback |  [ ]   |
| 💎  |   8   | Native health query/control APIs    | T12                |  [ ]   |
| ⭐  |   9   | Health notifications and dashboards | T16                |  [ ]   |
| 💎  |  10   | Tests and chaos scenarios           | §1..§9             |  [ ]   |

## 1. Health State Model and Registry

- [ ] Define `kernel_health_state_t`.
- [ ] Track per-subsystem health: state, last heartbeat, last error, failure count, recovery count.
- [ ] Register built-in subsystems: boot, memory, scheduler, object manager, registry, storage, network, graphics, power, security, ALPC, logging.
- [ ] Health state must be queryable without allocation.

## 2. Subsystem Heartbeat API

- [ ] Add `health_register_subsystem`, `health_heartbeat`, `health_report_error`, `health_report_recovered`.
- [ ] Support passive health probes scheduled through Executive delayed work.
- [ ] Heartbeat timeouts are policy-driven by TODO-02.
- [ ] Avoid hard dependency loops: logging failure must not require logging to report itself.

## 3. Failure Bucket Taxonomy

- [ ] Define buckets: boot failure, driver init failure, service init failure, hang, resource exhaustion, security violation, storage corruption, graphics failure, power transition failure, repeated crash.
- [ ] Assign stable IDs for crash dumps and telemetry.
- [ ] Map bugcheck codes and nonfatal failures into buckets.
- [ ] Include bucket in LastKnownGood and Safe Mode escalation decisions.

## 4. Live Kernel Dump Trigger

- [ ] Add `health_request_live_dump(reason, flags)`.
- [ ] Capture minidump without stopping all CPUs when possible; escalate to freeze-all for critical corruption.
- [ ] Rate-limit live dumps per boot to avoid disk exhaustion.
- [ ] Store live dump metadata in health registry.

## 5. Degraded-Mode Transition Engine

- [ ] Transition from healthy to degraded when critical-but-nonfatal subsystem fails.
- [ ] Degraded mode gates optional subsystems: network, desktop effects, third-party modules, background indexing, high-frequency logs.
- [ ] Publish degraded reason through TODO-27.
- [ ] Persist degraded reason for next boot analysis.

## 6. Recovery Action Dispatcher

- [ ] Actions: restart subsystem, disable feature flag, unload/quarantine module, trim caches, trigger safe-mode prompt, collect dump, bugcheck.
- [ ] Accept a resource-exhaustion pressure source from TODO-25 §9: subscribe to the `Kernel\QuotaPressure` and `Kernel\QuotaNomination` states, or expose a `health_report_pressure()` hook §9 can call. -> XREF: `02-kernel-core/TODO-25 §9`
- [ ] Provide the cleanup entry points TODO-25 §9 has no seam for today: a cache-drain callback registry and a log-trim action, both invocable from a recovery dispatch. -> XREF: `02-kernel-core/TODO-25 §9`
- [ ] Actions run through Executive work items unless panic path requires synchronous action.
- [ ] Each action records outcome and retry count.
- [ ] Add rollback for partially completed recovery actions.

## 7. Repeated-Failure Escalation

- [ ] Track crash/hang counts by bucket across boots.
- [ ] Escalate to Safe Mode after configurable thresholds.
- [ ] Consume `max_failed_boots`, `recoveryenabled`, and `bootstatuspolicy` semantics from TODO-02 instead of baking thresholds into health code.
- [ ] Prefer LastKnownGood rollback for driver/config buckets.
- [ ] Prefer crash dump collection and bugcheck for corruption/security buckets.

## 8. Native Health Query/Control APIs

- [ ] Add `SystemHealthInformation` to `NtQuerySystemInformation`.
- [ ] Add privileged `NtControlSystemHealth` for recovery action requests.
- [ ] Expose read-only state to normal users; privileged details require admin/system token.
- [ ] Include health state in KUSER_SHARED_DATA read-only summary if layout allows.

## 9. Health Notifications and Dashboards

- [ ] Publish health transitions through TODO-27.
- [ ] Emit ETW/klog records with bucket, subsystem, action, and result.
- [ ] Feed panic screen last-known health state.
- [ ] Host tools and UI can build dashboards later from the query API.

## 10. Tests and Chaos Scenarios

- [ ] Unit tests: state transitions, heartbeat timeout, failure bucket mapping, action retry limit.
- [ ] Chaos boot flags: fail registry, fail storage mount, hang worker, force resource exhaustion, deny CI image.
- [ ] Verify Safe Mode escalation after repeated failures.
- [ ] Verify no allocations in critical health query path.
