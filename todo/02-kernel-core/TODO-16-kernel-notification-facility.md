---
schema_version: 1
id: kernel-notification-facility
domain: 02-kernel-core
status: active
title: "TODO-16 -- Kernel Notification Facility"
---

# TODO-16 -- Kernel Notification Facility

> **Validated:** 2026-07-05 | validate-todo-file clean (structure / IO table / XREF / test wiring)

> **Gap-audited:** 2026-07-05 | parity (WNF / kdbus / sd-bus) + codex-gap-audit red-team. Filed: state lifetime classes + `SeCreatePermanentPrivilege` + `WNF_TYPE_ID` (§1); `MatchingChangeStamp` CAS + payload-type enforcement + DISPATCH_LEVEL no-alloc publish (§2); subscription teardown on NtClose/exit + close-vs-publish race (§3); device fanout through KNF (§5, reciprocal D04 T01 §7 / T10 §8); WNF ABI compat-level + delete-data-not-teardown semantics (§8); teardown tests (§10). Cross-TODO: WNF user runtime (Rtl publish/subscribe + dispatch worker) filed as new D12 T04 §10 with reciprocal XREF. FS change-notify explicitly out of scope (VFS domain).

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
- Scope boundary: this is state-change notification (WNF-style), NOT filesystem change notification -- directory-change watching (`ReadDirectoryChangesW`/`NtNotifyChangeDirectoryFile`, inotify analog) is owned by the filesystem/VFS domain, not this facility.

## Implementation Order

| ⭐ | Order | Deliverable | Depends On | Status |
| -- | :---: | ----------- | ---------- | :----: |
| 💎 | 1 | Notification state object type | T05 | [x] |
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

- [x] `KNF_STATE` body in `include/kernel/knf/knf.h`: name, atomic64 sequence, lifetime, scope, `KNF_TYPE_ID` + has_type_id; forward-reserved payload pointer/len/cap + subscriber-list head + per-state spinlock (payload is a pointer, not inline).
- [x] Register Object Manager type `NotificationState` (`ob_knf_type_init` via `ob_create_type`; `knf_state_on_delete` frees any retained payload). Singleton `ObpNotificationStateType`.
- [x] Create root directories in `knf_init`: `\Notifications` + `Kernel` / `Power` / `Security` / `Session` (the built-in-catalog section adds Device/System/Registry).
- [x] `knf_create_state` / `knf_lookup_state` / `knf_delete_state` primitives (create-or-open on collision; delete unlinks + frees). Leaf name capped at `OB_NAME_MAX` (static-asserted; a state is a namespace component).
- [x] Volatile (Temporary) states freed at last close via `knf_delete_state`; persistent reboot survival owned by the retention section (item: "Persistent-lifetime registry backing" in §7).
- [x] Lifetime classes WellKnown/Permanent/Persistent/Temporary in `KNF_LIFETIME`; non-Temporary sets `OB_FLAG_PERMANENT`. User-mode Permanent/Persistent create needs `SE_CREATE_PERMANENT_PRIVILEGE` (LUID 16); KernelMode bypasses.
- [x] `KNF_DATA_SCOPE` (System/Session/User/Machine/Process): v1 = single-instance-per-state (scope advisory only); per-scope isolated payload instances are a later item. Field kept for forward-compat.
- [x] `KNF_STATE` carries an optional `WNF_TYPE_ID` (16-byte GUID) typing the payload blob; publishers/consumers agree on the tag (blob stays opaque to the kernel).
- [x] Commit: `"kernel/knf: NotificationState Ob type + \\Notifications namespace + create/lookup/delete + SeCreatePermanentPrivilege"`

**Test checkpoint:** `test_knf` (TEST_CAT_KNF) asserts `ObpNotificationStateType != NULL`, `knf_create_state("Kernel","UtestState")` returns a `KNF_STATE` with `sequence == 0`, the 5 directories resolve via `ObLookupObjectByName`, create-or-open returns one object per name, arg validation rejects NULL/empty/over-length/unknown-category, header flags match lifetime + Security category, the leaf name-length boundary holds, user-mode Permanent/Persistent create is denied without the privilege, kernel-mode bypasses, and `SeCreatePermanentPrivilege` maps to LUID 16. Serial: `"[knf] Namespace: \\Notifications + N categories"`. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-knf-tests.bat` (SUITE=knf) | 10 suites, 0 failures
> **Notes:**
> - Shipped `include/kernel/knf/knf.h` + `src/kernel/knf/knf.c`: `NotificationState` Ob type, `\Notifications` namespace tree, and `knf_create_state`/`knf_lookup_state`/`knf_delete_state` (object-model CRUD).
> - `knf_init()` runs in boot_phase2 after the registry (POST16 0x20E0/0x20E1); single-threaded pre-scheduler so the tree build races nothing; runtime paths use the per-state spinlock + atomic64 sequence.
> - Added `SE_CREATE_PERMANENT_PRIVILEGE` (LUID 16) to privileges.h/.c + name table; Permanent/Persistent user-mode creation is privilege-gated (KernelMode bypasses); leaf name capped at `OB_NAME_MAX` via `_Static_assert`.
> - Codex design + test-coverage adoptions (pointer payload, gate Persistent, name-boundary fix, flag/deny tests) in the commit message; restricted-token fixture filed to §4, persistent registry backing to §7.
> - Canonical doc: this TODO; publish/subscribe/security/retention are the later sections.
> - Scope boundary: §1 owns the object type + namespace + lifetime/scope/type-id metadata; §2/§3 own publish/subscribe + teardown, §4 owns DACL/token enforcement, §7 owns coalescing/retention + persistent registry backing.
> **Verified:** 2026-07-05 | commit `REVIEWHASH` | 8/8 items | build OK | smoke PASS (KVM 3.29s) + test-knf 10/10
> **Deferred:** [L] `knf_init` failure is silent to boot-health -> XREF: 02-kernel-core/TODO-16 §9 (item: "Report KNF init health" at line 185)
> **Deferred:** [L] create-or-open can open an existing privileged state via a Temporary request -> XREF: 02-kernel-core/TODO-16 §4 (item: "Gate create-or-open opens by DACL" at line 122)
> **Deferred:** [L] concurrent create/delete SMP stress (ObpRemoveFromDirectory idempotent -1 path) -> XREF: 02-kernel-core/TODO-16 §10 (item: "Concurrent create/delete SMP stress" at line 197)
> **Quality reviewed:** 2026-07-05 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 2M+1L fixed, 3L deferred-XREF | scope: kernel-code-quality

---

## 2. Kernel Publish/Subscribe API

- [ ] Add `knf_create_state`, `knf_open_state`, `knf_publish`, `knf_subscribe`, `knf_unsubscribe`.
- [ ] Publish increments a 64-bit sequence atomically.
- [ ] Subscribers receive previous and new sequence numbers.
- [ ] Payload cap defaults to 4096 bytes; larger payloads must use ALPC or file-backed data and publish a reference.
- [ ] Conditional (CAS) publish: `knf_publish` accepts an optional `MatchingChangeStamp`; publish only if the current sequence equals it, else return `STATUS_UNSUCCESSFUL` (mirrors WNF, avoids lost read-modify-write updates).
- [ ] Enforce the declared payload type: reject a publish whose `WNF_TYPE_ID` does not match the state's registered type (not just the byte-size cap).
- [ ] Publish-context constraint: `knf_publish` must be callable from DISPATCH_LEVEL/DPC without allocating (pre-allocated wake path); document the max IRQL and no-blocking rule.

**Test checkpoint:** `test_knf` publishes twice to one state and asserts the returned sequence advances 0 -> 1 -> 2 atomically; a subscriber sees `(prev=1, new=2)`; `knf_publish` with a 5000-byte payload returns `STATUS_INVALID_PARAMETER` (over the 4096 cap). Serial: `"[KNF] publish seq=%llu"`. Test on: QEMU WHPX + TCG.

---

## 3. Waitable User Subscriptions

- [ ] Subscription handles become waitable objects.
- [ ] `NtWaitForSingleObject` wakes when sequence advances past caller's last seen value.
- [ ] Support timeout, alertable wait integration, and APC delivery for async subscriptions.
- [ ] Multi-subscriber fanout must not allocate at DISPATCH_LEVEL.
- [ ] Subscription teardown: remove the subscriber node on `NtClose` of the handle AND on owning process/thread exit (Ob close callback or reference-owned subscriber lifetime) so a killed process leaves no retained/leaked node.
- [ ] Close-vs-publish race: a publish concurrent with a subscription close must not wake a freed node or wake after the owner is gone; the subscriber lifetime is reference-counted across the wake path.
- [ ] (Later, competitive edge) Lightweight non-handle sequence-wait path (`WaitOnAddress`/futex analog) for hot consumers that do not need full Ob-handle semantics -- v2, not blocking.
- [ ] The user-mode `Rtl*` subscription table + delivery worker (per-process, one dispatch thread) that turns these kernel wakes into WNF callbacks is owned by `D12 T04 §10`, NOT this section; this section owns the kernel wait/wake primitive only.

**Test checkpoint:** `test_knf` blocks a thread on a subscription handle via `NtWaitForSingleObject`, publishes from another thread, and asserts the waiter wakes exactly when the sequence passes its last-seen value; a timeout wait returns `STATUS_TIMEOUT` when no publish occurs; fanout to 3 subscribers allocates zero at DISPATCH_LEVEL (pre-allocated wait blocks). Serial: `"[KNF] subscriber woke seq=%llu"`. Test on: QEMU WHPX + TCG.

---

## 4. Security and Namespace Policy

- [ ] Apply SRM access masks: query, subscribe, publish, create, delete.
- [ ] Default policy: kernel-only publish for security, code integrity, power source, and device states.
- [ ] Permit user-mode publish only for explicit app/session-local states.
- [ ] Audit denied publish attempts.
- [ ] Test the create-privilege gate with a restricted-token fixture: a token WITHOUT `SeCreatePermanentPrivilege` is denied a user-mode Permanent/Persistent create; one WITH it is allowed (§1 covers the ambient-token deny + kernel-mode bypass).
- [ ] Gate create-or-open opens by DACL so a user-mode Temporary create that collides with an existing privileged state is not a backdoor. Test: kernel creates Permanent `X`; user-mode Temporary create of `X` without rights is denied.
- [ ] Design note (code header): reuse the SRM SID/token/`SECURITY_DESCRIPTOR` infrastructure for access checks, NOT a bespoke capability-metadata scheme -- Linux kdbus was rejected from mainline (2015) for exactly that NIH design.

**Test checkpoint:** `test_knf` builds a state with a DACL granting SUBSCRIBE but not PUBLISH to a user token, then asserts `knf_publish` under that token returns `STATUS_ACCESS_DENIED` while `knf_subscribe` succeeds; a kernel-only security state rejects a user-mode publish; the denied attempt increments the audit counter. Serial: `"[KNF] publish denied sid=%s"`. Test on: QEMU WHPX + TCG.

---

## 5. Built-In State-Name Catalog

- [ ] Power: AC/DC, battery percentage, thermal level, suspend/resume, lid state.
- [ ] Device: storage arrival/removal, network up/down, display mode change.
- [ ] Session: logon/logoff, shell ready, foreground session, lock/unlock.
- [ ] Security: token elevation, CI allow/deny, audit policy update, credential change, policy-lock tamper/change (TODO-02 §9 `policy_lock.c` publishes `ETW_EVT_POLICY_TAMPER`/`POLICY_CHANGE` via `knf_publish`).
- [ ] System: time changed, timezone changed, config changed, safe mode, degraded mode, crash recovered.
- [ ] Registry: key policy changed, hive loaded/unloaded, transaction committed.
- [ ] Device states publish through KNF, not a bespoke driver-side queue. KNF §5 owns the `Device/*` catalog state names + payload schema; PnP producers call `knf_publish` on hot-plug (-> XREF: D04 T01 §7, D04 T10 §8).
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
- [ ] Persistent-lifetime registry backing: write a `KNF_LIFETIME_PERSISTENT` state's metadata (name/class/scope/type-id) to the registry on create and restore them at `knf_init` so they survive reboot (§1 classifies persistent states).
- [ ] Commit: `"kernel/knf: coalescing + last-payload retention + per-subscriber missed counter + redaction"`

**Test checkpoint:** `test_knf` publishes 5 rapid updates to a coalescing (level-triggered) state and asserts a slow subscriber sees only the latest payload with the missed-update counter == 4; an edge-triggered state delivers all 5; a query-after-miss returns the retained last payload; a redacted secret state returns zeroed bytes to a user-mode query. Serial: `"[KNF] coalesced=%u missed=%u"`. Test on: QEMU WHPX + TCG.

---

## 8. Native WNF-Compatible Syscall Surface

- [ ] Reserve SSDT entries for `NtCreateWnfStateName`, `NtUpdateWnfStateData`, `NtQueryWnfStateData`, `NtSubscribeWnfStateChange`, `NtUnsubscribeWnfStateChange`, and `NtDeleteWnfStateData`.
- [ ] Provide compatibility structs with explicit little-endian fields.
- [ ] Return `STATUS_NO_MORE_ENTRIES` when sequence has not advanced.
- [ ] DECIDE + document the `WNF_STATE_NAME` compat level: byte-compatible 64-bit encoded/XOR-obfuscated names (real Windows constants round-trip) vs syscall-arg-shape only (KNF maps names to internal OB paths). ABI-defining, cannot change post-ship.
- [ ] `NtDeleteWnfStateData` clears the payload ONLY -- it does NOT remove the StateName registration or detach subscribers (they keep waiting; next publish resumes). Distinct from object teardown.
- [ ] Specify create/open/query status semantics + max state size: `NtCreateWnfStateName` dup/exists handling, `NtQueryWnfStateData` returns the change stamp (+`STATUS_NO_MORE_ENTRIES` when unchanged), and the size-cap status code.

**Test checkpoint:** `test_knf` drives each `Nt*WnfStateData` handler through the SSDT: `NtCreateWnfStateName` returns a 64-bit state name, `NtUpdateWnfStateData` advances the sequence, `NtQueryWnfStateData` returns the payload + change stamp, an unchanged query returns `STATUS_NO_MORE_ENTRIES`, and `NtSubscribeWnfStateChange` wakes on the next update. Serial: `"[KNF] WNF syscalls: 6 SSDT slots live"`. Test on: QEMU WHPX + TCG.

---

## 9. Diagnostics Browser and Counters

- [ ] Expose `SystemNotificationInformation` through `NtQuerySystemInformation`.
- [ ] Report state count, subscriber count, publishes/sec, dropped/coalesced count, security denials.
- [ ] Report KNF init health: a `knf_init` failure (absent root namespace, category `knf_mkdir` failure) is silent to boot-health today. Surface it via a readiness/degraded flag so a partial `\Notifications` tree is visible.
- [ ] Commit: `"kernel/knf: SystemNotificationInformation diagnostics counters via NtQuerySystemInformation"`

**Test checkpoint:** `test_knf` calls `NtQuerySystemInformation(SystemNotificationInformation)` and asserts the returned struct reports the live state count, subscriber count, and non-zero publishes + coalesced + security-denial counters after the earlier section tests ran. Serial: `"[KNF] diag: states=%u subs=%u denials=%u"`. Test on: QEMU WHPX + TCG.

---

## 10. Unit/Boot Tests

- [ ] Kernel tests: create/publish/query, coalescing, missed sequence detection, ACL denied publish, wait wakeup.
- [ ] Boot test: publish `System/ShellReady` and verify a service-manager subscriber wakes once.
- [ ] Stress: 1000 states, 100 subscribers, no leaks after unsubscribe.
- [ ] Concurrent create/delete SMP stress (kthreads): many threads racing create + delete of one name; assert no leak/double-free and that the `ObpRemoveFromDirectory` idempotent `-1` path runs (§1 covers only deterministic double-delete).
- [ ] Subscription lifetime tests: teardown on `NtClose`, teardown on process/thread exit (no retained node), a close-vs-publish race (no wake of a freed node), and a publish-after-owner-exit (no wake of a gone task).
- [ ] Commit: `"kernel/knf: test_knf suite (create/publish/query/coalesce/ACL/wait) + ShellReady boot test"`

**Test checkpoint:** `test_knf` (TEST_CAT_KNF) runs green: create/publish/query, coalescing, missed-sequence detection, ACL-denied publish, and wait-wakeup all pass; the boot test publishes `System/ShellReady` and a service-manager subscriber wakes exactly once; the 1000-state/100-subscriber stress path reports 0 leaks after unsubscribe. Serial: `"[KNF] test_knf: N suites, 0 failures"`. Test on: QEMU WHPX + TCG.

---

## OS Comparison

| ⭐ | Feature | 🪟 Win11 | 🐧 Linux | 🚀 Impossible OS |
| --- | --- | --- | --- | --- |
| 💎 | Kernel state-change notify | ✅ WNF | ⚠️ netlink/inotify | ⬜ knf §1-§2 |
| 💎 | State object + lifetime/scope/type | ✅ WNF lifetimes | ⚠️ no unified model | ✅ §1: NotificationState Ob type + \Notifications + 4 lifetime classes |
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

