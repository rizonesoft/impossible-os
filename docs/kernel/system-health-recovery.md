<!-- docs: covers=todo/02-kernel-core/TODO-30-system-health-recovery-orchestrator.md sources=src/kernel/main/boot_init.c,src/kernel/panic.c,src/kernel/klog.c reviewed=2026-09-28 order=30 -->
# System Health and Recovery Orchestrator

## What is it?

The health and recovery orchestrator is a planned kernel-wide judgment layer that would turn scattered diagnostics (boot readiness, crash records, panic screens, logging) into decisions: is the system healthy, degraded, or in need of recovery, and what should happen next. None of it is implemented; every row of the roadmap's Implementation Order table is still open. This page records what the orchestrator is meant to sit on top of and what each open section would add.

## How does it work?

No orchestrator code exists yet, so this describes the pieces it would consume. The per-subsystem boot readiness table (`kernel_subsys_t`, `kernel_subsystem_ready()` and `kernel_subsystem_dump()` in [`boot_init.c`](../../src/kernel/main/boot_init.c), described in [Kernel Init Sequencing](kernel-init-sequencing.md)) knows whether each subsystem came up, but tracks nothing about runtime health afterwards. The panic path in [`panic.c`](../../src/kernel/panic.c) records one crash event as a cross-boot `struct panic_evidence`, with no notion of a system-wide health state. The kernel log in [`klog.c`](../../src/kernel/klog.c) records failures as text lines without sorting them into the stable failure buckets the roadmap proposes. Boot-time health checks and their schema are documented separately in the [`boot-health.json` wire format](../boot/boot-health-schema.md).

The roadmap would add a health registry with per-subsystem state, heartbeat, error and recovery counts; a failure-bucket taxonomy that maps bugcheck codes and error sources to stable IDs; a degraded-mode engine that turns optional subsystems off when their health falls; a recovery dispatcher (restart a subsystem, disable a feature, quarantine a module, trim caches); and escalation to Safe Mode or LastKnownGood after repeated failures in the same bucket. None of the planned functions (`health_register_subsystem()`, `health_heartbeat()`, `health_report_error()`, `health_request_live_dump()`) exists in the tree.

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| `kernel_subsystem_ready()`, `kernel_subsystem_dump()` | The existing boot-time readiness oracle the orchestrator would extend past boot ([`boot_init.c`](../../src/kernel/main/boot_init.c)) |
| `panic_screen()`, `struct panic_evidence` | The existing single-event crash path the orchestrator would feed buckets from ([`panic.c`](../../src/kernel/panic.c)) |
| `klog()` | The existing log sink whose failure lines the orchestrator would classify ([`klog.c`](../../src/kernel/klog.c)) |

No health-specific interface exists yet.

## How do I use it?

There is nothing to use. No boot flag, Registry key or native query exposes a system health state today. The readiness table is visible through `kernel_subsystem_dump()` at boot, and a prior crash through the panic evidence record, as their own pages describe.

## What is not implemented yet?

- **Health state model and registry.** No health state type or per-subsystem health record exists ([Health State Model and Registry](../../todo/02-kernel-core/TODO-30-system-health-recovery-orchestrator.md#1-health-state-model-and-registry)).
- **Subsystem heartbeat API.** No registration, heartbeat or error-report calls exist ([Subsystem Heartbeat API](../../todo/02-kernel-core/TODO-30-system-health-recovery-orchestrator.md#2-subsystem-heartbeat-api)).
- **Failure bucket taxonomy.** No stable bucket IDs or bugcheck-to-bucket mapping exist ([Failure Bucket Taxonomy](../../todo/02-kernel-core/TODO-30-system-health-recovery-orchestrator.md#3-failure-bucket-taxonomy)).
- **Live kernel dump trigger.** Also depends on the unbuilt minidump writer described in [Crash Dump Generation](crash-dump-generation.md) ([Live Kernel Dump Trigger](../../todo/02-kernel-core/TODO-30-system-health-recovery-orchestrator.md#4-live-kernel-dump-trigger)).
- **Degraded-mode transitions and recovery actions.** Nothing moves the system into a degraded state or dispatches a recovery action ([Degraded-Mode Transition Engine](../../todo/02-kernel-core/TODO-30-system-health-recovery-orchestrator.md#5-degraded-mode-transition-engine), [Recovery Action Dispatcher](../../todo/02-kernel-core/TODO-30-system-health-recovery-orchestrator.md#6-recovery-action-dispatcher)).
- **Repeated-failure escalation.** No cross-boot per-bucket failure counting exists ([Repeated-Failure Escalation](../../todo/02-kernel-core/TODO-30-system-health-recovery-orchestrator.md#7-repeated-failure-escalation)).
- **Native health APIs, notifications and tests.** No `NtQuerySystemInformation` health class, health notifications, or chaos tests exist ([Native Health Query/Control APIs](../../todo/02-kernel-core/TODO-30-system-health-recovery-orchestrator.md#8-native-health-querycontrol-apis), [Health Notifications and Dashboards](../../todo/02-kernel-core/TODO-30-system-health-recovery-orchestrator.md#9-health-notifications-and-dashboards), [Tests and Chaos Scenarios](../../todo/02-kernel-core/TODO-30-system-health-recovery-orchestrator.md#10-tests-and-chaos-scenarios)).

## How does it compare with Windows 11 and Linux?

The roadmap file has no OS Comparison table, so this stays to the shape of the idea. Windows spreads the equivalent across Windows Error Reporting, the Windows Recovery Environment (automatic repair, Safe Mode, LastKnownGood) and WHEA hardware error reporting; Linux spreads it across systemd service restart policies, watchdogs and `kdump`. Neither centralizes runtime health judgment inside the kernel the way this roadmap proposes. Impossible OS has neither arrangement today: boot readiness and crash recording exist separately, and nothing correlates them into a live health state or drives a recovery decision.

## See also

- [System Health & Recovery Orchestrator roadmap](../../todo/02-kernel-core/TODO-30-system-health-recovery-orchestrator.md)
- [Kernel Init Sequencing](kernel-init-sequencing.md)
- [Panic Screen and Crash Experience](panic-screen-crash-experience.md)
- [System Logging (klog)](system-logging.md)
- [Kernel Configuration and Policy Plane](kernel-configuration-policy.md)
