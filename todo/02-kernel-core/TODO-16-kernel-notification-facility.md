---
schema_version: 1
id: kernel-notification-facility
domain: 02-kernel-core
status: active
title: "TODO-16 -- Kernel Notification Facility"
---

# TODO-16 -- Kernel Notification Facility

> **Goal:** Add a WNF-style kernel notification facility for low-cost state changes and event fanout. Logging records what happened; notifications wake consumers that need to react. Power changes, device arrival, session changes, registry policy updates, code-integrity decisions, network state, time changes, and security events need one kernel-owned publication path with access checks and user subscriptions.

> [!IMPORTANT]
> **Current state:** Logs, ETW traces, and some desktop notification queues exist or are planned, but there is no kernel notification primitive. Subsystems either poll state, call each other directly, or plan bespoke queues. This blocks clean service-manager integration, user-mode waitable notifications, WNF-style state names, and reliable fanout without turning klog into an event bus.

## Inputs

- [`src/kernel/klog.c`](../../src/kernel/klog.c)
- [`src/kernel/etw.c`](../../src/kernel/etw.c)
- [`src/kernel/ob`](../../src/kernel/ob/)
- → XREF: [`TODO-05-object-manager.md`](./TODO-05-object-manager.md) -- notification objects and handles
- → XREF: [`TODO-12-native-api-ssdt.md`](./TODO-12-native-api-ssdt.md) -- native notification syscalls
- → XREF: [`TODO-15-security-reference-monitor.md`](./TODO-15-security-reference-monitor.md) -- ACLs and privilege checks
- → XREF: [`09-desktop-shell/TODO-03-service-manager.md`](../09-desktop-shell/TODO-03-service-manager.md) -- service/UI consumers
- → XREF: [`TODO-02-kernel-configuration-policy.md §9`](./TODO-02-kernel-configuration-policy.md) -- policy tamper/security audit events publish through this facility for fanout

## Outcome

- Kernel exposes named notification states under `\KernelObjects\Notifications`.
- Producers publish typed payloads with monotonic sequence numbers.
- Consumers can subscribe from kernel or user mode and wait on handles.
- Security descriptors gate who may publish and who may subscribe.
- Notifications bridge to ETW/klog optionally but are not stored as logs by default.
- Lost-update detection is explicit via sequence numbers.

## Implementation Order

| ⭐ | Order | Deliverable | Depends On | Status |
| -- | :---: | ----------- | ---------- | :----: |
| 💎 | 1 | Notification state object type | T05 | [ ] |
| 💎 | 2 | Kernel publish/subscribe API | §1 | [ ] |
| 💎 | 3 | Waitable user subscriptions | T12, T07 | [ ] |
| 💎 | 4 | Security and namespace policy | T15 | [ ] |
| ⭐ | 5 | Built-in state-name catalog | §1..§4 | [ ] |
| 💎 | 6 | ETW/klog bridge | T04, T32 | [ ] |
| ⭐ | 7 | Coalescing and payload retention | §2 | [ ] |
| 💎 | 8 | Native WNF-compatible syscall surface | T12 | [ ] |
| ⭐ | 9 | Diagnostics browser and counters | §1..§8 | [ ] |
| 💎 | 10 | Unit/boot tests | §1..§9 | [ ] |

## 1. Notification State Object Type

- [ ] Define `KNF_STATE` body: name, sequence, payload type, payload bytes, ACL, subscriber list, retention policy.
- [ ] Register Object Manager type `NotificationState`.
- [ ] Create root directories: `\Notifications`, `\Notifications\Kernel`, `\Notifications\Power`, `\Notifications\Security`, `\Notifications\Session`.
- [ ] Support volatile states and persistent state registration metadata in Registry.

## 2. Kernel Publish/Subscribe API

- [ ] Add `knf_create_state`, `knf_open_state`, `knf_publish`, `knf_subscribe`, `knf_unsubscribe`.
- [ ] Publish increments a 64-bit sequence atomically.
- [ ] Subscribers receive previous and new sequence numbers.
- [ ] Payload cap defaults to 4096 bytes; larger payloads must use ALPC or file-backed data and publish a reference.

## 3. Waitable User Subscriptions

- [ ] Subscription handles become waitable objects.
- [ ] `NtWaitForSingleObject` wakes when sequence advances past caller's last seen value.
- [ ] Support timeout, alertable wait integration, and APC delivery for async subscriptions.
- [ ] Multi-subscriber fanout must not allocate at DISPATCH_LEVEL.

## 4. Security and Namespace Policy

- [ ] Apply SRM access masks: query, subscribe, publish, create, delete.
- [ ] Default policy: kernel-only publish for security, code integrity, power source, and device states.
- [ ] Permit user-mode publish only for explicit app/session-local states.
- [ ] Audit denied publish attempts.

## 5. Built-In State-Name Catalog

- [ ] Power: AC/DC, battery percentage, thermal level, suspend/resume, lid state.
- [ ] Device: storage arrival/removal, network up/down, display mode change.
- [ ] Session: logon/logoff, shell ready, foreground session, lock/unlock.
- [ ] Security: token elevation, CI allow/deny, audit policy update, credential change, policy-lock tamper/change (TODO-02 §9 `policy_lock.c` publishes `ETW_EVT_POLICY_TAMPER`/`POLICY_CHANGE` via `knf_publish`).
- [ ] System: time changed, timezone changed, config changed, safe mode, degraded mode, crash recovered.
- [ ] Registry: key policy changed, hive loaded/unloaded, transaction committed.

## 6. ETW/klog Bridge

- [ ] Add per-state flags: `KNF_TRACE_ETW`, `KNF_TRACE_KLOG`, `KNF_PERSIST_LAST`.
- [ ] Avoid recursive logging during klog failure paths.
- [ ] ETW payload includes state name, sequence, publisher PID/TID, and status.

## 7. Coalescing and Payload Retention

- [ ] Coalesce repeated updates by state name unless marked edge-triggered.
- [ ] Retain last payload for query-after-miss.
- [ ] Keep a bounded missed-update counter per subscriber.
- [ ] Add policy for secret payload redaction before user-mode query.

## 8. Native WNF-Compatible Syscall Surface

- [ ] Reserve SSDT entries for `NtCreateWnfStateName`, `NtUpdateWnfStateData`, `NtQueryWnfStateData`, `NtSubscribeWnfStateChange`, `NtUnsubscribeWnfStateChange`, and `NtDeleteWnfStateData`.
- [ ] Provide compatibility structs with explicit little-endian fields.
- [ ] Return `STATUS_NO_MORE_ENTRIES` when sequence has not advanced.

## 9. Diagnostics Browser and Counters

- [ ] Expose `SystemNotificationInformation` through `NtQuerySystemInformation`.
- [ ] Report state count, subscriber count, publishes/sec, dropped/coalesced count, security denials.
- [ ] Add shell/browser consumer in tools domain later; kernel provides data only.

## 10. Unit/Boot Tests

- [ ] Kernel tests: create/publish/query, coalescing, missed sequence detection, ACL denied publish, wait wakeup.
- [ ] Boot test: publish `System/ShellReady` and verify a service-manager subscriber wakes once.
- [ ] Stress: 1000 states, 100 subscribers, no leaks after unsubscribe.

