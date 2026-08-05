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

| ⭐  | Order | Deliverable                           | Depends On | Status |
| --- | :---: | ------------------------------------- | ---------- | :----: |
| 💎  |   1   | Notification state object type        | T05        |  [x]   |
| 💎  |   2   | Kernel publish/subscribe API          | §1         |  [x]   |
| 💎  |   3   | Waitable user subscriptions           | T12, T07   |  [/]   |
| 💎  |   4   | Security and namespace policy         | T15        |  [/]   |
| ⭐  |   5   | Built-in state-name catalog           | §1..§4     |  [/]   |
| 💎  |   6   | ETW/klog bridge                       | T04, T32   |  [x]   |
| ⭐  |   7   | Coalescing and payload retention      | §2         |  [/]   |
| 💎  |   8   | Native WNF-compatible syscall surface | T12        |  [/]   |
| ⭐  |   9   | Diagnostics browser and counters      | §1..§8     |  [x]   |
| 💎  |  10   | Unit/boot tests                       | §1..§9     |  [/]   |

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
> **Verified:** 2026-07-05 | commit `678fbd3c` | 8/8 items | build OK | smoke PASS (KVM 3.29s) + test-knf 10/10
> **Deferred:** [L] `knf_init` failure is silent to boot-health -> XREF: 02-kernel-core/TODO-16 §9 (item: "Report KNF init health" at line 185)
> **Deferred:** [L] create-or-open can open an existing privileged state via a Temporary request -> XREF: 02-kernel-core/TODO-16 §4 (item: "Gate create-or-open opens by DACL" at line 122)
> **Deferred:** [L] concurrent create/delete SMP stress (ObpRemoveFromDirectory idempotent -1 path) -> XREF: 02-kernel-core/TODO-16 §10 (item: "Concurrent create/delete SMP stress" at line 197)
> **Quality reviewed:** 2026-07-05 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 2M+1L fixed, 3L deferred-XREF | scope: kernel-code-quality

---

## 2. Kernel Publish/Subscribe API

- [x] `knf_open_state`/`knf_publish`/`knf_subscribe`/`knf_unsubscribe`/`knf_subscription_poll` in `knf.c`/`knf.h`; `struct knf_subscriber` is now a full node (ref-pinned state, `last_seen`/`pending_prev`/`has_pending`).
- [x] Publish bumps the atomic64 change stamp under the per-state lock (`prev=atomic64_read`, `atomic64_set(prev+1)`); serialized writers keep lock-free readers monotonic.
- [x] Subscribers baseline at the current sequence; `knf_subscription_poll` reports `(prev, new)` for a pending advance, `STATUS_NO_MORE_ENTRIES` when drained (blocking wait is the waitable section).
- [x] `KNF_MAX_PAYLOAD` (4096) cap enforced pre-copy; `len > 4096` returns `STATUS_INVALID_PARAMETER` (larger payloads use ALPC / file-backed + a reference).
- [x] Conditional (CAS) publish: optional `matching_change_stamp`; publishes only when it equals the current sequence, else `STATUS_UNSUCCESSFUL` with no state change (mirrors WNF).
- [x] Typed-payload enforcement: a `has_type_id` state rejects a NULL or mismatched `WNF_TYPE_ID` with `STATUS_OBJECT_TYPE_MISMATCH` (blob opaque; tag contract enforced).
- [x] DISPATCH_LEVEL/DPC-safe publish: growth pre-allocated before the spinlock (kmalloc only below DISPATCH_LEVEL), else `STATUS_INSUFFICIENT_RESOURCES` unless pre-sized via non-notifying `knf_reserve_payload`; no alloc/free/callout under the lock.
- [x] Codex adoptions: subscribers Ob-reference their state (no dangle); `knf_reserve_payload` for DPC; `knf_unsubscribe` consumes+nulls the handle (no UAF); publish rejects at `UINT64_MAX` (no wrap). Details in commit.
- [x] Commit: `"kernel/knf: publish/subscribe API (seq advance, CAS, typed payload, reserve, subscriber pin)"`

**Test checkpoint:** `test_knf` publishes twice and asserts the sequence advances 0 -> 1 -> 2 atomically; a subscriber sees `(prev=1, new=2)`; a 5000-byte publish returns `STATUS_INVALID_PARAMETER`; a stale CAS stamp returns `STATUS_UNSUCCESSFUL`; a typed state rejects a wrong `WNF_TYPE_ID` with `STATUS_OBJECT_TYPE_MISMATCH`; `knf_reserve_payload` pre-sizes without advancing the sequence; a state deleted while a subscriber is live stays pinned and still delivers. Serial: `"[KNF] publish seq=%llu"`. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-knf-tests.bat` (SUITE=knf) | 20 suites, 0 failures
> **Notes:**
> - Shipped `knf_open_state`/`knf_publish`/`knf_subscribe`/`knf_unsubscribe`/`knf_subscription_poll`/`knf_reserve_payload` + the full `struct knf_subscriber` in `src/kernel/knf/knf.{c,h}`; 10 new `test_knf` suites.
> - Publish serializes under the per-state `spin_lock_irqsave`, all buffer alloc/free outside the lock, IRQL>DISPATCH rejected, subscriber list capped -- DISPATCH_LEVEL-safe against a pre-reserved buffer.
> - Codex adoptions (state-pin lifetime, non-notifying reserve, consume-and-null unsubscribe, UINT64_MAX no-wrap, subscriber cap) in the commit messages; waitable wait/wake + teardown races owned by the waitable section.
> - Canonical doc: this TODO; §1 owns the object type/namespace, §2 the publish/subscribe primitive, §3 the Ob-handle waitable wait/wake, §4 DACL/token enforcement, §7 coalescing/retention.
> **Verified:** 2026-07-05 | commit `b59f6e6f` | 8/8 items | build OK | knf tests 134/134 PASS
> **Accepted:** [M] concurrent unsubscribe/poll on the same handle races the pre-lock `sub->state` read -> XREF: 02-kernel-core/TODO-16 §3 (item: "Concurrent-teardown safety" at line 121)
> **Accepted:** [M] publish copies up to 4096 bytes under the per-state spinlock (bounded DPC lock-hold) -> XREF: 02-kernel-core/TODO-16 §7 (item: "Shrink publish lock-hold" at line 176)
> **Quality reviewed:** 2026-07-05 | Codex 9x (design, adversarial, consistency, perf, re-adversarial, test-coverage) | 2H+4M+4L fixed, 2M accepted-XREF | scope: kernel-code-quality

---

## 3. Waitable User Subscriptions

- [ ] Subscription handles become waitable objects.
- [ ] `NtWaitForSingleObject` wakes when sequence advances past caller's last seen value.
- [ ] Support timeout, alertable wait integration, and APC delivery for async subscriptions.
- [ ] Multi-subscriber fanout must not allocate at DISPATCH_LEVEL.
- [ ] Race-free wait: `NtWaitForSingleObject` infinite wait uses `event_wait`/`enqueue_and_block`, which has a documented lost-wakeup race (event.c state-read before enqueue; ex.h:149); a publish/close `event_set` in the gap hangs the waiter.
- [ ] Two-phase wake: publish must not `event_set` up to the subscriber cap under `KNF_STATE.lock` (IRQ-off fanout); mark pending under lock, rundown-pin selected subscriptions, release, then `event_set` outside the lock (timer precedent).
- [ ] Subscription teardown: remove the subscriber node on `NtClose` of the handle AND on owning process/thread exit (Ob close callback or reference-owned subscriber lifetime) so a killed process leaves no retained/leaked node.
- [ ] Close-vs-publish race: a publish concurrent with a subscription close must not wake a freed node or wake after the owner is gone; the subscriber lifetime is reference-counted across the wake path.
- [ ] Concurrent-teardown safety: `knf_subscription_poll`/`knf_unsubscribe` must take a live reference before the pre-lock `sub->state` read (§2 consume-and-null covers only sequential double-unsubscribe, not a cross-CPU alias race).
- [ ] (Later, competitive edge) Lightweight non-handle sequence-wait path (`WaitOnAddress`/futex analog) for hot consumers that do not need full Ob-handle semantics -- v2, not blocking.
- [ ] The user-mode `Rtl*` subscription table + delivery worker (per-process, one dispatch thread) that turns these kernel wakes into WNF callbacks is owned by `D12 T04 §10`, NOT this section; this section owns the kernel wait/wake primitive only.

**Test checkpoint:** `test_knf` blocks a thread on a subscription handle via `NtWaitForSingleObject`, publishes from another thread, and asserts the waiter wakes exactly when the sequence passes its last-seen value; a timeout wait returns `STATUS_TIMEOUT` when no publish occurs; fanout to 3 subscribers allocates zero at DISPATCH_LEVEL (pre-allocated wait blocks). Serial: `"[KNF] subscriber woke seq=%llu"`. Test on: QEMU WHPX + TCG.

> **Deferred:** [H] §3 not started -- blocked on prerequisites owned elsewhere: alertable-wait + user-APC async delivery unimplemented, and the waitable core needs a race-free wait (kernel `event_t` lost-wakeup, ex.h:149) + two-phase rundown-pinned wake (Codex design verdict: No-ship on the naive event_wait design) -> XREF: 02-kernel-core/TODO-07 §12 (item: "Alertable-wait integration" at line 396)

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

> **Deferred:** [H] §4 not started -- the DACL access masks (query/subscribe/publish/create/delete), DACL-gated create-or-open, and access-denied auditing all require the `SeAccessCheck` engine, which is itself deferred/blocked -> XREF: 02-kernel-core/TODO-15 §5 (item: "Implement `SeAccessCheck`" at line 364)
> **Deferred:** [M] restricted-token create-gate fixture needs a restricted token, also deferred -> XREF: 02-kernel-core/TODO-15 §9 (item: "`NtFilterToken(ExistingToken, Flags, SidsToDisable" at line 510)

---

## 5. Built-In State-Name Catalog

- [ ] Power: AC/DC, battery percentage, thermal level, suspend/resume, lid state.
- [ ] Device: storage arrival/removal, network up/down, display mode change.
- [ ] Session: logon/logoff, shell ready, foreground session, lock/unlock.
- [ ] Security: token elevation, CI allow/deny, audit policy update, credential change, policy-lock tamper/change (TODO-02 §9 `policy_lock.c` publishes `ETW_EVT_POLICY_TAMPER`/`POLICY_CHANGE` via `knf_publish`).
- [ ] System: time changed, timezone changed, config changed, safe mode, degraded mode, crash recovered.
- [ ] Registry: key policy changed, hive loaded/unloaded, transaction committed.
- [ ] Device states publish through KNF, not a bespoke driver-side queue. KNF §5 owns the `Device/*` catalog state names + payload schema; PnP producers call `knf_publish` on hot-plug (-> XREF: D04 T01 §7, D04 T10 §8).
- [ ] Fail-closed provisioning: `knf_init` must count expected catalog states, log the missing category/name on any create failure, set a KNF init-health flag, and fail KNF readiness rather than boot green with a partial well-known namespace.
- [ ] Typed payload schemas: give each catalog state a `WNF_TYPE_ID` (not `type_id=NULL`) so `knf_publish` enforces the payload type; define the per-state payload schema for Device/Power/Security/System/Registry names.
- [ ] Commit: `"kernel/knf: built-in state-name catalog (power/device/session/security/system/registry)"`

**Test checkpoint:** `test_knf` asserts every catalog state name resolves via `ob_ns_lookup` under its category directory; publishing `Security/PolicyTamper` from `policy_lock.c` (TODO-02 §9) delivers to a subscriber with the expected `ETW_EVT_POLICY_TAMPER` payload. Serial: `"[KNF] catalog: %u states across 6 categories"`. Test on: QEMU WHPX + TCG.

> **Deferred:** [H] §5 not started -- catalog is ordered after §4 (security policy); provisioning permanent well-known Security/Device/Power states before the DACL/publish access checks exist is a security-ordering hazard (Codex design HIGH). Also needs fail-closed provisioning + typed payload schemas -> XREF: 02-kernel-core/TODO-16 §4 (item: "Apply SRM access masks" at line 135)

---

## 6. ETW/klog Bridge

- [x] Per-state flags `KNF_TRACE_ETW`/`KNF_TRACE_KLOG`/`KNF_PERSIST_LAST` + `trace_flags` on `KNF_STATE`; `knf_set_trace_flags` stores them under the lock (rejects unknown bits). `KNF_PERSIST_LAST` reserved; retention is §7.
- [x] Recursion guard: per-thread `struct thread.in_knf_trace` acquired/released via atomic exchange (global scheduler cursor can alias a thread across CPUs) so a klog/ETW re-publish bails instead of recursing.
- [x] `knf_publish` mirrors to klog/ETW off the lock (snapshot under lock), below DISPATCH_LEVEL; `ETW_EVT_KNF_PUBLISH` 0x1200 packs `KNF_ETW_RECORD` (category+leaf identity, pinned ABI in `knf.h`); every suppression path counted.
- [x] Commit: `"kernel/knf: ETW/klog bridge flags (KNF_TRACE_ETW/KLOG/PERSIST_LAST) + recursion guard"`

**Test checkpoint:** `test_knf` sets `KNF_TRACE_KLOG`+`KNF_TRACE_ETW` on a state, publishes, and asserts the publish succeeds + sequence advances with the bridge running off-lock; unknown trace bits and a NULL state are rejected; clearing the flags disables the bridge; a duplicate leaf in two categories keeps distinct `category` identity. Serial (WHPX): the `[knf] publish '<category>\<name>' seq=N pid=P` klog line + one `ETW_EVT_KNF_PUBLISH` record. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-knf-tests.bat` (SUITE=knf) | 22 suites, 0 failures
> **Notes:**
> - Shipped `KNF_TRACE_*` flags + `trace_flags` + `knf_set_trace_flags` + per-thread `in_knf_trace` guard + the `knf_publish` klog/ETW bridge (`ETW_EVT_KNF_PUBLISH` 0x1200, packed `KNF_ETW_RECORD`) across knf.c/knf.h/etw.h/task.h.
> - The bridge runs OFF the state lock (snapshot flags+name under lock, emit after), only below DISPATCH_LEVEL, guarded per-thread against re-entry; opt-in per state (default off), so it never touches the DPC publish fast path.
> - Codex design + adversarial adoptions in the commit message (snapshot-under-lock, atomic-exchange recursion guard, category+leaf ETW identity in shared header, all suppression paths counted: DISPATCH + guard + no-thread); `KNF_PERSIST_LAST` behavior owned by §7.
> - Canonical doc: this TODO; §7 owns coalescing/retention (incl. `KNF_PERSIST_LAST`), §9 owns diagnostics counters.
> **Verified:** 2026-07-06 | commit `bc708dc6` | 3/3 items | build OK | tests 154/154 PASS
> **Quality reviewed:** 2026-07-06 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 3M+1L fixed | scope: kernel-code-quality

---

## 7. Coalescing and Payload Retention

- [x] Level-triggered coalescing (default): a slow subscriber's poll collapses a burst to the latest `(pending_prev, latest)`; intermediate updates are counted, not delivered. Edge-triggered "deliver every update" mode deferred (Deferred stamp).
- [x] Retain last payload for query-after-miss: kernel-private `knf_query_last_kernel` returns retained payload + stamp (WNF too-small shape). Kernel-only; user exposure + redaction owned by the WNF query surface.
- [x] Bounded per-subscriber missed-update counter: `knf_subscriber.missed` (saturating at `UINT64_MAX`, bumped under the lock when a publish coalesces over an unread pending) + read-and-reset `knf_subscription_missed_count`.
- [/] Secret-payload redaction before user-mode query: `KNF_MODE_SECRET` + `knf_set_mode` land now as the policy source of truth; redaction ENFORCEMENT deferred to the user-mode WNF query surface (raw user read would leak a secret state).
- [/] Edge-triggered delivery (deliver every update, not just the latest): needs a bounded per-subscriber delivery ring; deferred (Deferred stamp).
- [/] Persistent-lifetime registry backing: write a `KNF_LIFETIME_PERSISTENT` state metadata to the registry on create + restore at `knf_init` (reboot survival). Deferred (registry write/restore integration).
- [/] Shrink publish lock-hold via active/spare double-buffer: the naive "copy before the lock" is unsafe (design review); needs a real active/inactive buffer model. The current under-lock copy is correct; this is a perf optimization. Deferred.
- [x] Commit: `"kernel/knf: coalescing + last-payload retention + per-subscriber missed counter + secret-mode flag"`

**Test checkpoint:** `test_knf` publishes 5 rapid updates to a coalescing (level-triggered) state and asserts a slow subscriber's missed-update counter == 4 with poll delivering only the latest sequence; a query-after-miss (`knf_query_last_kernel`) returns the retained last payload + stamp and reports the full length on a too-small buffer; `knf_set_mode` stores `KNF_MODE_SECRET` and rejects unknown bits + a NULL state. (Edge-triggered delivery, secret redaction, persistence, and double-buffering are deferred -- see Deferred stamps.) Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-knf-tests.bat` (SUITE=knf) | 23 suites, 0 failures
> **Notes:**
> - Shipped the coalescing/retention layer (knf.c/knf.h): per-subscriber `missed` counter + `knf_subscription_missed_count` (read-and-reset), kernel-private `knf_query_last_kernel` (query-after-miss), and `KNF_MODE_SECRET` + `knf_set_mode` policy flag.
> - Level-triggered coalescing is the default publish/poll behavior; `missed` is bumped under the per-state lock when a publish lands over an unread pending; the retention query snapshots payload+stamp under the same lock.
> - Deferred (see stamps): edge-triggered delivery, secret redaction enforcement, persistent registry backing, active/spare double-buffering. Design review adoptions in the commit message.
> - Canonical doc: this TODO; §8 owns the user-mode WNF query surface where `KNF_MODE_SECRET` redaction enforces, §9 owns diagnostics counters.
> **Verified:** 2026-07-06 | commit `31236a97` | 3/7 items | build OK | tests 172/172 PASS
> **Deferred:** [M] edge-triggered "deliver every update" mode not implemented (needs a bounded per-subscriber delivery ring) -> XREF: 02-kernel-core/TODO-16 §7 (item: "Edge-triggered delivery" at line 194)
> **Deferred:** [M] secret-payload redaction enforcement lands with the user-mode query surface (the flag ships now) -> XREF: 02-kernel-core/TODO-16 §7 (item: "Secret-payload redaction before user-mode query" at line 195)
> **Deferred:** [M] persistent-lifetime registry backing (write-on-create + restore-at-init) not implemented -> XREF: 02-kernel-core/TODO-16 §7 (item: "Persistent-lifetime registry backing" at line 197)
> **Deferred:** [L] publish lock-hold not yet shrunk; needs active/spare double-buffer -> XREF: 02-kernel-core/TODO-16 §7 (item: "Shrink publish lock-hold via active/spare double-buffer" at line 198)
> **Quality reviewed:** 2026-07-06 | Codex 3x (adversarial, consistency, perf) | 2L fixed | scope: kernel-code-quality

---

## 8. Native WNF-Compatible Syscall Surface

- [ ] Reserve SSDT entries for `NtCreateWnfStateName`, `NtUpdateWnfStateData`, `NtQueryWnfStateData`, `NtSubscribeWnfStateChange`, `NtUnsubscribeWnfStateChange`, and `NtDeleteWnfStateData`.
- [ ] Provide compatibility structs with explicit little-endian fields.
- [ ] Return `STATUS_NO_MORE_ENTRIES` when sequence has not advanced.
- [ ] DECIDE + document the `WNF_STATE_NAME` compat level: byte-compatible 64-bit encoded/XOR-obfuscated names (real Windows constants round-trip) vs syscall-arg-shape only (KNF maps names to internal OB paths). ABI-defining, cannot change post-ship.
- [ ] `NtDeleteWnfStateData` clears the payload ONLY -- it does NOT remove the StateName registration or detach subscribers (they keep waiting; next publish resumes). Distinct from object teardown.
- [ ] Specify create/open/query status semantics + max state size: `NtCreateWnfStateName` dup/exists handling, `NtQueryWnfStateData` returns the change stamp (+`STATUS_NO_MORE_ENTRIES` when unchanged), and the size-cap status code.

**Test checkpoint:** `test_knf` drives each `Nt*WnfStateData` handler through the SSDT: `NtCreateWnfStateName` returns a 64-bit state name, `NtUpdateWnfStateData` advances the sequence, `NtQueryWnfStateData` returns the payload + change stamp, an unchanged query returns `STATUS_NO_MORE_ENTRIES`, and `NtSubscribeWnfStateChange` wakes on the next update. Serial: `"[KNF] WNF syscalls: 6 SSDT slots live"`. Test on: QEMU WHPX + TCG.

> **Deferred:** [H] §8 blocked on the `WNF_STATE_NAME` compat-level decision -- ABI-defining, cannot change post-ship, so operator-reserved (byte-compatible Windows encoding vs syscall-arg-shape only); every handler encodes this ABI. awaiting-answer (todo/answers.md Q1). SSDT numbers 0x01E0-0x01E6 are reserved in service_numbers.h; handlers await the decision. -> XREF: 02-kernel-core/TODO-16 §8 (item: "DECIDE + document the" at line 222)

---

## 9. Diagnostics Browser and Counters

- [x] Expose `SystemNotificationInformation` (class 0x1002) via `NtQuerySystemInformation`: packed `SYSTEM_NOTIFICATION_INFORMATION` ABI in `knf_syscall_info.h` + `nt_query_notification_information` marshaller.
- [x] Report live state/subscriber counts, cumulative publish/coalesced + trace drop/skip counts, and security denials via relaxed atomics. Broader `SeAccessCheck` denials deferred; create-privilege denial ships now.
- [x] Report KNF init health: `knf_init` applies `kernel_subsystem_apply_result(SUBSYS_KNF, ...)` (FATAL/DEGRADED/OK), surfaced as the READY/DEGRADED/UNAVAILABLE health flags; fixed the false-OK POST16 paths.
- [x] Commit: `"kernel/knf: SystemNotificationInformation diagnostics counters via NtQuerySystemInformation"`

**Test checkpoint:** `test_knf_diag_query` calls `nt_query_notification_information` (the `SystemNotificationInformation` marshaller), asserts Version/Size + `SYSTEM_NOTIFICATION_FLAG_READY`, then does known operations (create, subscribe, 3-publish burst, denied user-mode permanent create) and asserts the live-state/subscriber/publish/coalesced/security-denial counters rose by the expected deltas; a too-small buffer returns `STATUS_INFO_LENGTH_MISMATCH` + full length. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-knf-tests.bat` (SUITE=knf) | 24 suites, 0 failures
> **Notes:**
> - Shipped `SystemNotificationInformation` (class 0x1002): new `knf_syscall_info.h` (96-byte Version/Size/Reserved ABI), `nt_query_notification_information` marshaller + dispatch, and 5 diagnostics atomics in knf.c.
> - Counters: live state/subscriber (inc/dec), cumulative publish/coalesced/create-denial, plus the §6 trace drop/skip; readiness via a new `SUBSYS_KNF` slot set in `knf_init` (FATAL/DEGRADED/OK), fixing two false-OK POST16 paths.
> - Deferred (stamp): broader `SeAccessCheck`-policy denials add to `SecurityDenials` when that engine lands. Design review adoptions in the commit message.
> - Canonical doc: `include/kernel/nt/knf_syscall_info.h` is the ABI; the security-policy denials belong to the security/namespace-policy section.
> **Verified:** 2026-07-06 | commit `682f6c2c` | 3/3 items | build OK | tests 189/189 PASS
> **Deferred:** [L] broader SeAccessCheck-policy publish/subscribe denials not yet counted in SecurityDenials (only the create-permanent-privilege denial is) -> XREF: 02-kernel-core/TODO-16 §4 (item: "Audit denied publish attempts" at line 138)
> **Quality reviewed:** 2026-07-06 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1H+2M+1L fixed | scope: kernel-code-quality

---

## 10. Unit/Boot Tests

- [/] Kernel tests: create/publish/query, coalescing, missed-sequence, retention, mode/trace flags, and diagnostics all ship in the 25-suite `test_knf` (TEST_CAT_KNF). ACL-denied publish + wait-wakeup deferred (Deferred stamps).
- [/] Boot test: publish `System/ShellReady`, a service-manager subscriber wakes once. Deferred: needs waitable user subscriptions + the service manager (Deferred stamp).
- [x] Stress: `test_knf_stress_no_leak` creates 400 states across the 4 category dirs (100 each, under the 128-entry OB dir cap) + 100 subscribers, tears down asserting exact counts + diag counters return to baseline (no leak).
- [/] Concurrent create/delete SMP stress: kthreads racing create+delete of one name, assert no leak/double-free + the `ObpRemoveFromDirectory` idempotent -1 path. Deferred: needs a kthread stress harness.
- [/] Subscription lifetime tests: teardown on `NtClose`, teardown on process/thread exit (no retained node), a close-vs-publish race, and a publish-after-owner-exit. Deferred: needs waitable Ob-handle subscriptions (Deferred stamp).
- [x] Commit: `"kernel/knf: test_knf stress leak-check (400 states / 100 subscribers, no leak)"` (ACL/wait/ShellReady deferred -- see Deferred stamps)

**Test checkpoint:** `test_knf` (TEST_CAT_KNF, 25 suites) runs green: create/publish/query, coalescing, missed-sequence, retention, mode/trace flags, diagnostics, and the 400-state/100-subscriber `test_knf_stress_no_leak` (counters return to baseline) all pass. ACL-denied publish, wait-wakeup, the boot `System/ShellReady` test, subscription-lifetime, and the kthread SMP race are deferred -- see Deferred stamps. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-knf-tests.bat` (SUITE=knf) | 25 suites, 0 failures
> **Notes:**
> - Shipped `test_knf_stress_no_leak` (test_knf.c): 400 states across 4 categories (under the 128-entry OB dir cap) + 100 subscribers created and torn down, asserting exact counts + the KNF diag counters return to baseline (no leak) + idempotent-delete.
> - The functional core (create/publish/query, coalescing, missed-sequence, retention, mode/trace, diagnostics) is the 24 suites already shipped across §1-§9; this section adds the stress leak-check as the 25th.
> - Deferred (stamps): ACL-denied publish (needs SeAccessCheck), wait-wakeup + boot `System/ShellReady` + subscription-lifetime (need waitable Ob-handle subscriptions), and the kthread SMP create/delete race (needs a kthread stress harness).
> - Canonical doc: this TODO; the leak check keys off the KNF diagnostics counters (`knf_diag_live_state_count`/`knf_diag_subscriber_count`).
> **Verified:** 2026-07-06 | commit `e50224aa` | 1/5 items | build OK | tests 195/195 PASS
> **Deferred:** [M] ACL-denied-publish test not implemented (needs the access-decision engine) -> XREF: 02-kernel-core/TODO-16 §4 (item: "Apply SRM access masks: query, subscribe, publish, create, delete" at line 135)
> **Deferred:** [M] wait-wakeup + boot `System/ShellReady` + subscription-lifetime tests need waitable Ob-handle subscriptions -> XREF: 02-kernel-core/TODO-16 §3 (item: "Subscription handles become waitable objects" at line 115)
> **Deferred:** [L] concurrent create/delete kthread SMP stress not implemented (needs a kthread stress harness) -> XREF: 02-kernel-core/TODO-16 §10 (item: "Concurrent create/delete SMP stress" at line 258)
> **Quality reviewed:** 2026-07-06 | Codex 4x (design, adversarial, consistency, perf) | 2M+1L fixed | scope: kernel-code-quality

---

## OS Comparison

| ⭐  | Feature                            | 🪟 Win11            | 🐧 Linux           | 🚀 Impossible OS                         |
| --- | ---------------------------------- | ------------------- | ------------------ | ---------------------------------------- |
| 💎  | Kernel state-change notify         | ✅ WNF              | ⚠️ netlink/inotify  | ✅ §1-§2: publish/subscribe + poll       |
| 💎  | State object + lifetime/scope/type | ✅ WNF lifetimes    | ⚠️ no unified model | ✅ §1: NotificationState Ob type + \Notifications + 4 lifetime classes |
| 💎  | Waitable user subscriptions        | ✅ WNF+Nt*          | ⚠️ epoll/poll       | ⬜ §3                                    |
| 💎  | Per-state security descriptor      | ✅ Full             | ⚠️ DAC only         | ⬜ §4                                    |
| 💎  | Lost-update sequence numbers       | ✅ WNF change stamp | ❌ N/A             | ✅ §2: atomic64 stamp + CAS publish      |
| 💎  | WNF-compatible syscalls            | ✅ Nt*WnfStateData  | ❌ N/A             | ⬜ §8                                    |
| ⭐  | Named catalog + coalescing         | ⚠️ Undocumented WNF  | ❌ ad-hoc          | ⬜ §5,§7                                 |
| ⭐  | Live diagnostics counters          | ❌ Debugger only    | ⚠️ /proc scattered  | ⬜ §9                                    |

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
- [ ] Bare-metal DPC validation (real DISPATCH_LEVEL, not WSL-unit-testable): `knf_reserve_payload` -> `STATUS_UNSUCCESSFUL`; publish within a pre-reserved cap succeeds alloc-free; publish over `payload_cap` -> `STATUS_INSUFFICIENT_RESOURCES`.

> **Test runner:** `scripts\debug\kernel\run-knf-tests.bat` (SUITE=knf) | N suites, 0 failures
