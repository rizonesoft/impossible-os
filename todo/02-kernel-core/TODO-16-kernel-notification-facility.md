---
schema_version: 1
id: kernel-notification-facility
domain: 02-kernel-core
status: active
title: "TODO-16 -- Kernel Notification Facility"
---

# TODO-16 -- Kernel Notification Facility

> **Validated:** 2026-07-05 | validate-todo-file clean (structure / IO table / XREF / test wiring)

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
- [ ] Commit: `"kernel/knf: NotificationState Ob type + KNF_STATE body + root namespace directories"`

**Test checkpoint:** `test_knf` asserts the `NotificationState` Ob type is registered (`ObpLookupType` non-NULL), `knf_create_state("Kernel/Test")` returns a `KNF_STATE` with `sequence == 0`, and the 5 root directories (`\Notifications`, `\Notifications\Kernel/Power/Security/Session`) resolve via `ob_ns_lookup`. Serial: `"[KNF] N notification states registered"`. Test on: QEMU WHPX + TCG.

---

## 2. Kernel Publish/Subscribe API

- [ ] Add `knf_create_state`, `knf_open_state`, `knf_publish`, `knf_subscribe`, `knf_unsubscribe`.
- [ ] Publish increments a 64-bit sequence atomically.
- [ ] Subscribers receive previous and new sequence numbers.
- [ ] Payload cap defaults to 4096 bytes; larger payloads must use ALPC or file-backed data and publish a reference.
- [ ] Commit: `"kernel/knf: publish/subscribe API + atomic 64-bit sequence + payload cap"`

**Test checkpoint:** `test_knf` publishes twice to one state and asserts the returned sequence advances 0 -> 1 -> 2 atomically; a subscriber sees `(prev=1, new=2)`; `knf_publish` with a 5000-byte payload returns `STATUS_INVALID_PARAMETER` (over the 4096 cap). Serial: `"[KNF] publish seq=%llu"`. Test on: QEMU WHPX + TCG.

---

## 3. Waitable User Subscriptions

- [ ] Subscription handles become waitable objects.
- [ ] `NtWaitForSingleObject` wakes when sequence advances past caller's last seen value.
- [ ] Support timeout, alertable wait integration, and APC delivery for async subscriptions.
- [ ] Multi-subscriber fanout must not allocate at DISPATCH_LEVEL.
- [ ] Commit: `"kernel/knf: waitable subscription handles + sequence-advance wakeup + APC delivery"`

**Test checkpoint:** `test_knf` blocks a thread on a subscription handle via `NtWaitForSingleObject`, publishes from another thread, and asserts the waiter wakes exactly when the sequence passes its last-seen value; a timeout wait returns `STATUS_TIMEOUT` when no publish occurs; fanout to 3 subscribers allocates zero at DISPATCH_LEVEL (pre-allocated wait blocks). Serial: `"[KNF] subscriber woke seq=%llu"`. Test on: QEMU WHPX + TCG.

---

## 4. Security and Namespace Policy

- [ ] Apply SRM access masks: query, subscribe, publish, create, delete.
- [ ] Default policy: kernel-only publish for security, code integrity, power source, and device states.
- [ ] Permit user-mode publish only for explicit app/session-local states.
- [ ] Audit denied publish attempts.
- [ ] Commit: `"kernel/knf: SRM access masks + kernel-only-publish default policy + denied-publish audit"`

**Test checkpoint:** `test_knf` builds a state with a DACL granting SUBSCRIBE but not PUBLISH to a user token, then asserts `knf_publish` under that token returns `STATUS_ACCESS_DENIED` while `knf_subscribe` succeeds; a kernel-only security state rejects a user-mode publish; the denied attempt increments the audit counter. Serial: `"[KNF] publish denied sid=%s"`. Test on: QEMU WHPX + TCG.

---

## 5. Built-In State-Name Catalog

- [ ] Power: AC/DC, battery percentage, thermal level, suspend/resume, lid state.
- [ ] Device: storage arrival/removal, network up/down, display mode change.
- [ ] Session: logon/logoff, shell ready, foreground session, lock/unlock.
- [ ] Security: token elevation, CI allow/deny, audit policy update, credential change, policy-lock tamper/change (TODO-02 §9 `policy_lock.c` publishes `ETW_EVT_POLICY_TAMPER`/`POLICY_CHANGE` via `knf_publish`).
- [ ] System: time changed, timezone changed, config changed, safe mode, degraded mode, crash recovered.
- [ ] Registry: key policy changed, hive loaded/unloaded, transaction committed.
- [ ] Commit: `"kernel/knf: built-in state-name catalog (power/device/session/security/system/registry)"`

**Test checkpoint:** `test_knf` asserts every catalog state name resolves via `ob_ns_lookup` under its category directory; publishing `Security/PolicyTamper` from `policy_lock.c` (TODO-02 §9) delivers to a subscriber with the expected `ETW_EVT_POLICY_TAMPER` payload. Serial: `"[KNF] catalog: %u states across 6 categories"`. Test on: QEMU WHPX + TCG.

---

## 6. ETW/klog Bridge

- [ ] Add per-state flags: `KNF_TRACE_ETW`, `KNF_TRACE_KLOG`, `KNF_PERSIST_LAST`.
- [ ] Avoid recursive logging during klog failure paths.
- [ ] ETW payload includes state name, sequence, publisher PID/TID, and status.
- [ ] Commit: `"kernel/knf: ETW/klog bridge flags (KNF_TRACE_ETW/KLOG/PERSIST_LAST) + recursion guard"`

**Test checkpoint:** `test_knf` sets `KNF_TRACE_KLOG` on a state, publishes, and asserts one klog line with the state name + sequence + publisher PID; a publish during a klog failure path does NOT recurse (guard flag observed); `KNF_TRACE_ETW` emits one ETW record. Serial: `"[KNF] bridge etw=%u klog=%u"`. Test on: QEMU WHPX + TCG.

---

## 7. Coalescing and Payload Retention

- [ ] Coalesce repeated updates by state name unless marked edge-triggered.
- [ ] Retain last payload for query-after-miss.
- [ ] Keep a bounded missed-update counter per subscriber.
- [ ] Add policy for secret payload redaction before user-mode query.
- [ ] Commit: `"kernel/knf: coalescing + last-payload retention + per-subscriber missed counter + redaction"`

**Test checkpoint:** `test_knf` publishes 5 rapid updates to a coalescing (level-triggered) state and asserts a slow subscriber sees only the latest payload with the missed-update counter == 4; an edge-triggered state delivers all 5; a query-after-miss returns the retained last payload; a redacted secret state returns zeroed bytes to a user-mode query. Serial: `"[KNF] coalesced=%u missed=%u"`. Test on: QEMU WHPX + TCG.

---

## 8. Native WNF-Compatible Syscall Surface

- [ ] Reserve SSDT entries for `NtCreateWnfStateName`, `NtUpdateWnfStateData`, `NtQueryWnfStateData`, `NtSubscribeWnfStateChange`, `NtUnsubscribeWnfStateChange`, and `NtDeleteWnfStateData`.
- [ ] Provide compatibility structs with explicit little-endian fields.
- [ ] Return `STATUS_NO_MORE_ENTRIES` when sequence has not advanced.
- [ ] Commit: `"kernel/knf: WNF-compatible SSDT surface (NtCreate/Update/Query/Subscribe/Unsubscribe/DeleteWnfStateData)"`

**Test checkpoint:** `test_knf` drives each `Nt*WnfStateData` handler through the SSDT: `NtCreateWnfStateName` returns a 64-bit state name, `NtUpdateWnfStateData` advances the sequence, `NtQueryWnfStateData` returns the payload + change stamp, an unchanged query returns `STATUS_NO_MORE_ENTRIES`, and `NtSubscribeWnfStateChange` wakes on the next update. Serial: `"[KNF] WNF syscalls: 6 SSDT slots live"`. Test on: QEMU WHPX + TCG.

---

## 9. Diagnostics Browser and Counters

- [ ] Expose `SystemNotificationInformation` through `NtQuerySystemInformation`.
- [ ] Report state count, subscriber count, publishes/sec, dropped/coalesced count, security denials.
- [ ] Add shell/browser consumer in tools domain later; kernel provides data only.
- [ ] Commit: `"kernel/knf: SystemNotificationInformation diagnostics counters via NtQuerySystemInformation"`

**Test checkpoint:** `test_knf` calls `NtQuerySystemInformation(SystemNotificationInformation)` and asserts the returned struct reports the live state count, subscriber count, and non-zero publishes + coalesced + security-denial counters after the earlier section tests ran. Serial: `"[KNF] diag: states=%u subs=%u denials=%u"`. Test on: QEMU WHPX + TCG.

---

## 10. Unit/Boot Tests

- [ ] Kernel tests: create/publish/query, coalescing, missed sequence detection, ACL denied publish, wait wakeup.
- [ ] Boot test: publish `System/ShellReady` and verify a service-manager subscriber wakes once.
- [ ] Stress: 1000 states, 100 subscribers, no leaks after unsubscribe.
- [ ] Commit: `"kernel/knf: test_knf suite (create/publish/query/coalesce/ACL/wait) + ShellReady boot test"`

**Test checkpoint:** `test_knf` (TEST_CAT_KNF) runs green: create/publish/query, coalescing, missed-sequence detection, ACL-denied publish, and wait-wakeup all pass; the boot test publishes `System/ShellReady` and a service-manager subscriber wakes exactly once; the 1000-state/100-subscriber stress path reports 0 leaks after unsubscribe. Serial: `"[KNF] test_knf: N suites, 0 failures"`. Test on: QEMU WHPX + TCG.

---

## OS Comparison

| ⭐ | Feature | 🪟 Win11 | 🐧 Linux | 🚀 Impossible OS |
| --- | --- | --- | --- | --- |
| 💎 | Kernel state-change notify | ✅ WNF | ⚠️ netlink/inotify | ⬜ knf §1-§2 |
| 💎 | Waitable user subscriptions | ✅ WNF+Nt* | ⚠️ epoll/poll | ⬜ §3 |
| 💎 | Per-state security descriptor | ✅ Full | ⚠️ DAC only | ⬜ §4 |
| 💎 | Lost-update sequence numbers | ✅ WNF change stamp | ❌ N/A | ⬜ §2 |
| 💎 | WNF-compatible syscalls | ✅ Nt*WnfStateData | ❌ N/A | ⬜ §8 |
| ⭐ | Named catalog + coalescing | ⚠️ Undocumented WNF | ❌ ad-hoc | ⬜ §5,§7 |
| ⭐ | Live diagnostics counters | ❌ Debugger only | ⚠️ /proc scattered | ⬜ §9 |

> 💎 = parity work (Win11/Linux already do it). ⭐ = exclusive/superior work.

---

## Unit Tests

- [ ] `src/kernel/test/test_knf.c` -- register `test_register_knf()` under new `TEST_CAT_KNF` (add to `test.h` enum, `test_runner.c` name/label, Makefile `test-knf` target, `bootx64.c` test_suite parser).
- [ ] Assertions: Ob type registered; atomic sequence advance; ACL-denied publish -> `STATUS_ACCESS_DENIED`; coalescing keeps latest + missed counter; waiter wakes on advance; unchanged query -> `STATUS_NO_MORE_ENTRIES`; no leak after unsubscribe.
- [ ] Boot test: `System/ShellReady` publish wakes a service-manager subscriber once (validation via serial on WHPX).

---

## Verification

- [ ] `bash scripts/build.sh` shows `=== BUILD OK ===`.
- [ ] `make test-knf` (TEST_CAT_KNF) passes with 0 failures.
- [ ] `bash scripts/test.sh QUIET=1` green (no regressions).
- [ ] Verify on QEMU WHPX (2 CPUs) + QEMU TCG + VirtualBox + bare metal -- notification wakeups + DISPATCH_LEVEL no-alloc fanout behave identically (VM behavior differs on real hardware).

> **Test runner:** `scripts\debug\kernel\run-knf-tests.bat` (SUITE=knf) | N suites, 0 failures

