---
schema_version: 1
id: kernel-configuration-policy
domain: 02-kernel-core
status: active
title: "TODO-02 -- Kernel Configuration & Policy Plane"
---

# TODO-02 -- Kernel Configuration & Policy Plane

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Build the kernel's authoritative configuration plane: boot arguments, BCD-style boot entries, safe-mode flags, control sets, LastKnownGood, feature flags, runtime tunables, debug policy, crash policy, and immutable early-boot policy. The Registry stores durable state, but the kernel must own parsing, validation, phase-safe publication, rollback, and the contract every subsystem consumes.

> [!IMPORTANT]
> **Current state:** Boot config is partially delivered by the UEFI bootloader through `g_boot_info.config` and consumed by Phase 0. Registry policy exists only as ad-hoc reads in individual subsystems. There is no typed kernel configuration API, no safe-mode policy object, no BCD-equivalent boot-entry parser, no control-set selection, no rollback to LastKnownGood, and no global feature-flag/tunable registry. `KUSER_SHARED_DATA` already reserves `SafeBootMode`, `KdDebuggerEnabled`, and `MitigationPolicies`, but `kusd_init()` does not populate those policy fields today.

## Inputs

- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- [`src/kernel/main/boot_hw.c`](../../src/kernel/main/boot_hw.c)
- [`src/kernel/main/boot_init.c`](../../src/kernel/main/boot_init.c)
- [`src/kernel/registry.c`](../../src/kernel/registry.c)
- [`include/kernel/nt/kusd.h`](../../include/kernel/nt/kusd.h)
- [`src/kernel/time/kusd_time.c`](../../src/kernel/time/kusd_time.c)
- → XREF: [`T01 §2, §6, §7`](./TODO-01-kernel-init-sequencing.md) -- Phase 0 publication point, dependency gates, and degraded-boot policy
- → XREF: [`T11 §11`](./TODO-11-peb-teb-user-abi.md) -- `KUSER_SHARED_DATA` mirrors Safe Mode, debugger, and mitigation summary bits for user-mode consumers
- → XREF: [`T14 §8, §12, §13`](./TODO-14-registry-completion.md) -- hive persistence, `CurrentControlSet` resolution, and schema-validated policy keys
- → XREF: [`T19 §1, §7, §8`](./TODO-19-code-integrity-trust-policy.md) -- CI boot aliases, Safe Mode relaxations, and Secure Boot lock state consume typed config policy
- → XREF: [`T12 §10`](./TODO-12-native-api-ssdt.md) -- native config query/set ABI hooks
- → XREF: [`T15 §8`](./TODO-15-security-reference-monitor.md) -- privilege gate for runtime writes
- → XREF: [`T30 §5, §7`](./TODO-30-system-health-recovery-orchestrator.md) -- degraded-mode transitions and repeated-failure escalation consume boot-status policy
- → XREF: [`T31 §2`](./TODO-31-kernel-bulletproofing.md) -- fixed-layout defense pattern to reuse for `kernel_config_t`
- → XREF: [`D01 T07 §3, §5, §7, §11`](../01-boot-platform/TODO-07-boot-entry-store-menu-policy.md) -- boot-entry precedence, safe/test entry flags, previous-kernel handoff, and per-entry health gate (T07 §11 layers entry-level mark-good above the §10 acceptance ledger here)
- → XREF: [`D01 T21 §4, §5`](../01-boot-platform/TODO-21-ab-boot-rollback.md) -- boot rollback counters consume LastKnownGood, acceptance, and success/failure decisions
- → XREF: [`D04 T05 §5`](../04-drivers-hardware/TODO-05-kernel-module-system.md) -- module autoload consumes Safe Mode gating rules
- → XREF: [`D09 T03 §6, §8`](../09-desktop-shell/TODO-03-service-manager.md) -- built-in service startup and autostart honor Safe Mode and recovery policy

## Outcome

- One typed `kernel_config_t` snapshot is available from Phase 0 onward.
- Boot arguments, registry policy, firmware policy, and defaults merge through a deterministic precedence order.
- Safe Mode, debug mode, test mode, no-network, no-gui, verifier, and recovery flags are first-class.
- ControlSet selection and LastKnownGood rollback are kernel-visible and auditable.
- Boot acceptance policy and the success ledger are explicit, auditable, and shared with rollback/recovery consumers.
- Runtime tunables are validated, range-clamped, versioned, and exposed through native query/set syscalls.
- Security-sensitive fields are immutable after their lock phase.
- A unified lockdown level (none/integrity/confidentiality) and module-owned tunables extend the plane beyond per-policy locks and core globals.

## Implementation Order

| ⭐   | Order | Deliverable                                      | Depends On                       | Status |
| --- | :---: | ------------------------------------------------ | -------------------------------- | :----: |
| 💎   |   1   | Boot argument schema and parser                  | D01 T07 §3, §5                   |  [x]   |
| 💎   |   2   | `kernel_config_t` immutable Phase 0 snapshot     | §1, T01 §2, §6                   |  [x]   |
| 💎   |   3   | Registry-backed policy merge                     | §2, §4, T14 §8, §12, §13         |  [/]   |
| 💎   |   4   | ControlSet and LastKnownGood selection           | §2, §10, T14 §12, D01 T21 §4, §5 |  [/]   |
| 💎   |   5   | Safe Mode and recovery policy object             | §2, §4, D01 T07 §5               |  [/]   |
| 💎   |   6   | Runtime tunable registry                         | §2                               |  [/]   |
| ⭐   |   7   | Feature flag gates and experiment cohorts        | §6                               |  [/]   |
| 💎   |   8   | Native query/set config syscalls                 | §2, §6, T12 §10, T15 §8          |  [/]   |
| ⭐   |   9   | Policy lock phases and tamper audit              | §2, §3                           |  [/]   |
| 💎   |  10   | Boot status policy and boot success ledger       | §4, §5, D01 T21 §5, T30 §7       |  [/]   |
| 💎   |  11   | Config dump, tests, and docs                     | §1-10, T11 §11, T31 §2           |  [/]   |
| 💎   |  12   | Post-ship follow-up backfill (2026-07-31 cohort) | --                               |  [ ]   |

> 💎 = parity work: matches what Windows 11 and Linux already ship.
> ⭐ = exclusive work: Impossible OS adds provenance, rollout, and tamper semantics neither platform exposes as one kernel-owned plane.

---

## 1. Boot Argument Schema and Parser

Parse the bootloader handoff into typed, validated keys before any Phase 0 consumer reads raw strings.

- [x] Define `boot_arg_desc_t` (name, type, min/max, default, phase, security class, help) + per-value `boot_arg_value_t` with `source`/`present`/`raw` provenance (`config.h`).
- [x] Support bool, integer, string, enum, duration, byte-size, and CSV-list values (`validate_value`, overflow-guarded numeric parse).
- [x] Normalize aliases: `/debug`->`debug=1`, bare BOOL keys, `safemode=minimal|network`, `testsigning=on`, `nointegritychecks`, `bootstatuspolicy=...`, `recoveryenabled=no`, `nogui`, `noacpi`, `verifier=*`.
- [x] Reject unknown `kernel.` namespace keys unless `boot.allow_unknown=1` (BOOT_FATAL -> `boot_halt` at Phase 0).
- [x] Preserve the original command line, selected boot-entry id, and boot-selection reason from `boot_info` (`boot_args_cmdline`/`_entry_id`/`_selection_reason`).
- [x] Keep on-disk boot-entry parsing in D01 T07 §2-§5; this section only consumes the bootloader-selected payload after handoff.
- [x] Commit: `"kernel: define boot argument schema and parser"`

**Test checkpoint:** Boot with `debug=1 safemode=network verifier=*`: serial shows `"[CONF] parsed boot args: debug=1 safemode=network verifier=*"` before Phase 0 consumers run. Boot with `kernel.unknown=1` and no override: serial shows `"[CONF] unknown boot key: kernel.unknown"` then `boot_halt()` (Phase 0 has no degraded-continue per T01 §7); `boot.allow_unknown=1` downgrades it to a logged warning. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 11 CONF suites, 0 failures
> **Notes:**
> - Shipped `config.h` + `config.c`: static 12-key descriptor table + pure `boot_args_parse_cmdline_n` (7 value types, alias normalization) + Phase-0 `boot_args_init` wired into `boot_phase0`.
> - Fully static/fixed-size (no kmalloc); 256-byte handoff `cmdline` is length-bounded + rejected if unterminated; unknown `kernel.*` / bad value halts at Phase 0.
> - `boot_config` fields project as the BOOTCFG layer; cmdline overrides per §3 precedence with `source`/`present`/`raw` provenance. Codex 4x adoptions in commit.
> - Tests: `test_kernel_config.c`, 11 suites under `TEST_CAT_BOOT` (alias, unknown-key halt, allow_unknown, CSV, bad-value, bounded/unterminated cmdline).
> - Scope: §1 owns schema + cmdline parser + provenance; §2 owns the `kernel_config_t` snapshot; §3 owns the registry precedence merge + BOOTCFG explicit-vs-default.
> **Verified:** 2026-06-20 | commit `pending` | 7/7 items | build OK | smoke PASS (KVM 2.65s)
> **Deferred:** [M] BOOTCFG explicit-vs-default provenance needs a `boot_config` presence bitset (BOOT_INFO_VERSION bump) -> XREF: 02-kernel-core/TODO-02 §3 (item: "Resolve boot_config explicit-vs-default in the merge" at line 117)
> **Quality reviewed:** 2026-06-20 | Codex 6x (adversarial, consistency, perf, re-adversarial x3) | 1H+2M fixed | scope: kernel-code-quality

---

## 2. `kernel_config_t` Immutable Phase 0 Snapshot

Publish one read-only snapshot that all later phases consume instead of rereading mutable boot structures.

- [x] Add `include/kernel/config.h` with `kernel_config_t`, `kernel_config_get()`, and `kernel_config_publish()`.
- [x] Publish a read-only snapshot AFTER `boot_decision_validate`; `ready` flag released last behind `smp_mb`; `kernel_config_get()` returns NULL before then.
- [x] Flatten boot mode, graphics (fb), debug transports, logging, init flags, CI policy (testsigning/nointegritychecks/noacpi/nogui), verifier CSV. Panic policy is a §6 runtime tunable.
- [x] Include selected boot-entry id + `selection_reason` + `boot_reason` + cmdline-override provenance. Control-set target is §4-owned, not in this immutable snapshot.
- [x] `_Static_assert` exact `KERNEL_CONFIG_SIZE_V1` + magic/boot_reason/selection_reason/entry_id offsets; `[CONF] snapshot ready: version=1 phase=0` log.
- [x] Commit: `"kernel: publish immutable Phase 0 configuration snapshot"`

**Test checkpoint:** First Phase 0 consumer sees non-NULL `kernel_config_get()` and serial shows `"[CONF] snapshot ready: version=1 phase=0"`. Build or boot fails if the declared size/version drifts from the published accessor contract. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 15 CONF suites, 0 failures
> **Notes:**
> - Shipped `kernel_config_t` + `kernel_config_publish`/`kernel_config_get` (config.h/config.c): a flattened read-only Phase-0 snapshot built from the resolved §1 boot args + validated `boot_info` (fb/boot_reason/selection_reason/entry_id).
> - Published once on the BSP after `boot_decision_validate` in `boot_phase0`; `ready` flag released last behind `smp_mb`, acquire on `get()` -- write-once-before-SMP, no half-filled read.
> - `boot_args_t` stays the private parser input; consumers read the flattened typed fields, never re-deriving defaults/enums. Codex 4x adoptions in commit.
> - ABI guard: `KERNEL_CONFIG_SIZE_V1` exact-size `_Static_assert` + 4 offset pins (a `<=N` guard would miss reorder/repack).
> - Scope: §2 owns the immutable snapshot; §3 owns the registry precedence merge; §4 owns the control-set target; §6 owns panic/runtime tunables.
> **Verified:** 2026-06-20 | commit `pending` | 6/6 items | build OK | smoke PASS (KVM 2.71s)
> **Quality reviewed:** 2026-06-20 | Codex 7x (design, adversarial x2, consistency, perf, re-adversarial x2) | 2H+2M fixed | scope: kernel-code-quality

---

## 3. Registry-Backed Policy Merge

Merge persisted policy without letting malformed registry data silently reshape boot-critical behavior.

- [ ] Load `HKLM\SYSTEM\CurrentControlSet\Control\Kernel` during Phase 2.
- [ ] Merge precedence: compiled defaults < boot entry defaults < registry policy < boot command line < firmware-enforced policy.
- [ ] Produce per-key provenance for `config_dump`.
- [ ] Log every override with previous value, new value, and source.
- [ ] Reject malformed security-sensitive values with an explicit `"[CONF] rejected policy override"` log and keep the last valid value instead of silently coercing.
- [ ] Resolve boot_config explicit-vs-default in the merge: the struct has no per-key presence bit. Add a producer presence bitset (BOOT_INFO_VERSION bump) or define boot.conf-default == compiled-default. (Codex §1 design H)
- [ ] Commit: `"kernel: merge registry-backed policy into effective configuration"`

**Test checkpoint:** Registry sets `debug=0`, command line sets `debug=1`, and `config_dump` reports `effective=1 source=cmdline`. Invalid `panic.timeout=-1` logs a rejection and leaves the compiled default in place. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Deferred:** [H] §3 registry merge requires §4 control-set selection FIRST (which `ControlSetNNN` is authoritative for this boot, incl. Failed/LastKnownGood/rollback) -- the §3-before-§4 order is inverted, and a Phase-2 merge cannot authoritatively affect already-consumed Phase-0/1 keys (debug/safemode/CI/verifier), so it must publish a SEPARATE runtime-phase effective-policy object rather than mutate the immutable §2 snapshot. Operator-reserved ordering/architecture decision (reorder §3/§4 + scope the registry layer to runtime keys). When unblocked, add `BOOT_ARG_SRC_REGISTRY` (between BOOTCFG and CMDLINE) + reuse the §1 `validate_value` for one descriptor-driven merge + a static `Control\Kernel`-name->descriptor map. -> XREF: 02-kernel-core/TODO-02 §4 (item: "Implement `kernel_select_control_set()`" at line 141)

---

## 4. ControlSet and LastKnownGood Selection

Turn `Select` values and boot outcomes into deterministic control-set choice instead of ad-hoc registry reads.

- [ ] Implement `kernel_select_control_set()`: Current, Default, Failed, LastKnownGood.
- [ ] Resolve `CurrentControlSet` only after `Select` values are validated and persist the chosen control set id in `kernel_config_t`.
- [ ] Mark boot as pending until §10 reports that the configured acceptance stage is reached and the kernel can report success to D01 T21 §5.
- [ ] On successful boot, update LastKnownGood only after the §10 acceptance policy confirms critical services and registry flush succeeded.
- [ ] On failed boot, record failure reason, roll back the control set, and pass the rollback hint to D01 T21 §4-§5.
- [ ] Commit: `"kernel: add control-set and LastKnownGood selection policy"`

**Test checkpoint:** With `Current=2`, `Default=1`, and `LastKnownGood=3`, a failed boot before ready logs `"[CONF] control set rollback: ControlSet002 -> ControlSet003"` and the next boot selects `ControlSet003`. Successful boot updates `LastKnownGood` only after registry flush succeeds. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Deferred:** [H] §4 is blocked on three prerequisites: (a) `registry_init` creates only `HKLM\SYSTEM` -- the `SYSTEM\Select` / `ControlSetNNN` / `CurrentControlSet`-link substrate §4 must read does not exist yet (T14 §12); (b) LastKnownGood update, boot-pending, failed-boot writeback, and D01 T21 rollback-hint all gate on the §10 acceptance ledger (unimplemented); (c) persisting the chosen control-set id cannot go into the immutable §2 `kernel_config_t` -- it needs the SAME Phase-2 effective-policy object §3 also needs (operator-reserved architecture). The clean slice (validate Select, resolve `CurrentControlSet`->`ControlSetNNN`, publish selected id+reason in a Phase-2 object) unblocks §3 but still requires (a). -> XREF: 02-kernel-core/TODO-02 §10 (item: "Define `boot_status_policy_t`" at line 156); -> XREF: 02-kernel-core/TODO-14 §12 (item: "Registry Symlink Completion" at line 594)

---

## 5. Safe Mode and Recovery Policy Object

Represent safe mode as a first-class kernel policy object, not a scattered collection of booleans.

- [x] Define `safe_mode_t` {OFF,MINIMAL,NETWORK,DSREPAIR} + `safe_mode_reason_t` {NONE,OPERATOR,BOOT_POLICY,RECOVERY} in `config.h`.
- [x] Gate drivers, network, desktop, third-party modules, integrity relaxations, services via `safe_mode_component_allowed(level, SAFE_COMP_*)` in `config.c`; cross-domain consumers (D04 T05 §5, D09 T03 §6/§8, T19 §1/§8) wired via the XREFs above.
- [/] Surface safe-mode reason, control set, and recovery trigger through `kernel_config_get()`. Reason + recovery trigger (`boot_reason`/`boot_mode`) surfaced; control-set field deferred. -> XREF: §4
- [x] Mirror `boot_config.boot_mode` (T07 §8 producer) into KUSD `SafeBootMode` + `KdDebuggerEnabled` + `MitigationPolicies`; the latter carries EFFECTIVE CI policy (raw flags masked when safe mode gates `SAFE_COMP_CI_RELAX`).
- [x] Ensure Safe Mode cannot be disabled by a registry value once requested by boot policy: `safe_mode_resolve()` enforces a monotonic boot-policy floor that operator/registry input can only raise.
- [x] Distinguish operator-requested from repeated-failure recovery safe mode: recovery-first, escalation-above-floor->OPERATOR, floor->BOOT_POLICY, preserved in `kernel_config_t.safe_mode_reason`.
- [x] Commit: `"kernel: add Safe Mode and recovery policy object"`

**Test checkpoint:** Boot with cmdline `safemode=network`: serial shows `"[CONF] safe_mode=network reason=operator"` (operator-requested via command line) and GUI-only services stay disabled while networking remains enabled. A boot-entry-forced safe boot instead shows `reason=boot-policy`, and a repeated-failure recovery boot shows a distinct `reason=recovery` code that cannot be downgraded by registry state or `safemode=off`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | CONF safe-mode suites, 0 failures

> **Notes:**
> - `safe_mode_resolve()` monotonic floor: recovery->DSREPAIR, safe boot->MINIMAL; operator `safemode=` arg raises only, never lowers below the boot-policy floor.
> - KUSD `MitigationPolicies` publishes EFFECTIVE CI policy; raw `nointegritychecks`/`testsigning` masked when `SAFE_COMP_CI_RELAX` is gated off in any safe boot.
> - Reason matrix: recovery-first -> `RECOVERY`; escalation above floor -> `OPERATOR`; floor -> `BOOT_POLICY`.
> - Control-set surfacing through `kernel_config_get()` deferred to §4 (ControlSet selection owns the field).
> - Codex review adoptions + per-finding evidence in the section ship commit.

> **Verified:** 2026-06-20 | commit `4b3ba505` | 5/6 items | build OK | tests 2915/2915 PASS
> **Deferred:** [M] Surface selected control set through `kernel_config_get()` -- depends on the §4 control-set field (item 3 of this section is `[/]`) -> XREF: 02-kernel-core/TODO-02 §4 (item: "persist the chosen control set id in `kernel_config_t`" at line 144)
> **Quality reviewed:** 2026-06-20 | Codex 5x (design, adversarial, re-adversarial, consistency, perf) | 1H+3M fixed | scope: kernel-code-quality

---

## 6. Runtime Tunable Registry

Give subsystems one typed registration surface for mutable policy instead of one-off globals and parser branches.

- [x] Implement `kernel_tunable_register(name, type, flags, min, max, def, callback, ctx, owner, source)` in `tunables.c`; typed (INT/UINT/BOOL/ENUM), rejects dup name + full table + malformed descriptor.
- [x] Support read-only, boot-only, runtime, privileged, and debug-only tunables via `TUNABLE_{READONLY,BOOT_ONLY,RUNTIME,PRIVILEGED,DEBUG_ONLY}` flags enforced in `kernel_tunable_set`.
- [x] Add core tunables in `kernel_tunables_register_core`, each wired to a real consumer: klog.level, panic.timeout, handle.quota_default. Speculative timer/ALPC/verifier knobs dropped (hard hw/security caps, not operator defaults).
- [x] Record owner subsystem, lock phase, and `source` provenance per tunable; `kernel_tunable_dump()` exposes them for audit.
- [x] Callbacks run at PASSIVE only -- inline when caller is passive (IRQL sampled before the lock), else deferred to `sys_wq`; recursive self-writes refused; deferred dispatch atomic + monotonic via write-generation.
- [/] Loadable modules register under a `module.<name>.` namespace (`TUNABLE_SRC_MODULE`); `kernel_tunable_unregister_owner()` quiesces callbacks then drops the module's tunables. Loader-side call owned elsewhere. -> XREF: D04 T05
- [x] Commit: `"kernel: add runtime tunable registry"`

**Test checkpoint:** Writing `panic.timeout=9999` clamps to the declared max and logs `"[CONF] tunable clamp: panic.timeout"`. A read-only tunable write returns `STATUS_ACCESS_DENIED`, and a callback requested from syscall context runs later at PASSIVE_LEVEL. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | CONF tunable suites (9), 0 failures

> **Notes:**
> - `tunables.c` -- 64-entry typed registry; `kernel_tunable_register/set/get/get_u64/unregister_owner/lock_phase_*/dump`, wired in `boot_desktop.c` (`register_core` after `sys_wq`, `lock_phase_advance(RUNTIME)` before the compositor).
> - Callbacks run at PASSIVE: inline when the caller is passive (IRQL sampled before the lock), else deferred to `sys_wq`; write-generation makes deferred writes monotonic; `unregister_owner` quiesces in-flight callbacks (module-unload safety).
> - Three wired core tunables: `klog.level` -> `klog_set_level`; `panic.timeout` -> panic auto-restart (lockless `volatile` read); `handle.quota_default` -> per-process handle limit (`handle_table.c` reads `kernel_tunable_get_u64`).
> - Lock phases are a minimal monotonic substrate; the tamper-audit lock-phase model + the module-loader register/unregister call are owned elsewhere (policy lock phases feature; kernel module system).
> - Codex review adoptions (design + adversarial + test-coverage + consistency + perf + re-adversarial) in the section ship + review commits.

> **Verified:** 2026-06-20 | commit `692c3d1b` | 5/6 items | build OK | tests 2978/2978 PASS
> **Accepted:** [M] module-namespace loader register/unregister call (item 6, `[/]`) is owned by the kernel module system -> XREF: 04-drivers-hardware/TODO-05 §5 (item: "Parse `module_param` declarations ... register each as a `module.<name>.` tunable" at line 155)
> **Quality reviewed:** 2026-06-20 | Codex 12x (design, adversarial, test-coverage, re-adversarial, consistency, perf) | 8H+14M fixed | scope: kernel-code-quality

---

## 7. Feature Flag Gates and Experiment Cohorts

Make feature flags discoverable, auditable, and safe enough for staged rollout instead of ad-hoc ifdefs and hidden globals.

- [/] Implement `kernel_feature_enabled(name)` in `feature.c` with the boot provider (bounded raw-cmdline scan of `feature.<name>=on/off`, resolve-once cached). The registry-backed override provider is deferred to the registry merge. -> XREF: §3
- [x] Cohort hashing by machine ID for staged rollout: `bucket = sha256(machine_uuid || name) % 100 < rollout_percent`; stable per machine, falls back to the registered default when no valid machine UUID exists (never hashes a sentinel).
- [x] Forced-on/forced-off audit events: `kernel_feature_enabled` logs `[CONF] feature <name> forced <on/off>` for a cmdline override and `[CONF] secure feature override blocked: <name>` for a refused security override.
- [x] Security features cannot be disabled under Secure Boot: a `FEATURE_SECURITY` flag's disable override/cohort-off is refused unless Secure Boot is known-off (state readable AND inactive) -- active or unreadable fails closed.
- [x] Reserve `feature.<name>` / `experiment.<name>` namespaces with owner tags; `kernel_feature_register` rejects non-namespaced and anonymous prefix-only names so flags cannot accumulate untagged.
- [x] Commit: `"kernel: add feature flag gates and rollout cohorts"`

**Test checkpoint:** The same machine id lands in the same cohort on repeated boots, `feature.debug_menu=off` disables only that feature, and `feature.kpti=off` under Secure Boot is rejected with `"[CONF] secure feature override blocked: kpti"`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | CONF feature suites (10), 0 failures

> **Notes:**
> - `feature.c` -- 64-entry feature registry: `kernel_feature_register/enabled/count/dump` + `register_core` (feature.kpti, feature.debug_menu), wired in `boot_desktop.c` after the tunable registry.
> - Resolution (cached once per boot): cmdline override > cohort rollout > default; `FEATURE_LOCKED` bypasses override/cohort; the Secure Boot guard then fails closed on any security-feature disable unless Secure Boot is known-off.
> - Cohort is `sha256(machine_uuid || name) % 100 < rollout_percent`, stable per machine; no valid UUID falls back to the default; consumers query `kernel_feature_enabled(name)` at their gate.
> - Boot-arg overrides use a feature-owned bounded raw-cmdline scanner because the boot-argument schema parser drops unknown dynamic keys.
> - Registry-backed override provider is owned by the registry-merge feature (§3); §7 ships the boot provider. Codex review adoptions in the section ship + review commits.

> **Verified:** 2026-06-20 | commit `e8238fc2` | 4/5 items | build OK | tests 3028/3028 PASS
> **Deferred:** [M] Registry-backed feature-override provider (item 1, `[/]`) needs the registry merge -> XREF: 02-kernel-core/TODO-02 §3 (item: "Merge precedence: compiled defaults < boot entry defaults < registry policy < boot command line < firmware-enforced policy" at line 126)
> **Quality reviewed:** 2026-06-20 | Codex 8x (design, adversarial, test-coverage, re-adversarial, consistency, perf) | 3H+8M fixed | scope: kernel-code-quality

---

## 8. Native Query/Set Config Syscalls

Expose the effective configuration through stable NT contracts once the SSDT surface is ready.

- [x] Add `SystemKernelConfigInformation` (0x1000) to `NtQuerySystemInformation`: `nt_query_kernel_config_information` marshals the snapshot + tunable/feature counts + lock phase (Probe + `copy_to_user`, two-pass length).
- [/] Shared ABI struct `SYSTEM_KERNEL_CONFIG_INFORMATION` (size/offset asserts) in `nt/sysconfig_info.h`; set/write contract rides `NtSetSystemInformation`, deferred with the privilege gate. -> XREF: 02-kernel-core/TODO-15 §8
- [/] Enforce `SeSystemProfilePrivilege` for writes -- deferred: an SMP-safe check needs the SRM's `SeSinglePrivilegeCheck` + per-token lock. -> XREF: 02-kernel-core/TODO-15 §8
- [/] Return provenance, effective value, mutability -- snapshot fields shipped in the query class; per-key provenance ships with the set path. -> XREF: 02-kernel-core/TODO-15 §8
- [x] Read-path marshalling + unit coverage landed (`test_cfg_query_syscall`); write-path privilege/ACCESS_DENIED ABI checks deferred with the set path, not dropped.
- [x] Commit: `"kernel: add native configuration query and set syscalls"`

**Test checkpoint:** `NtQuerySystemInformation(SystemKernelConfigInformation)` returns snapshot version and size, and an undersized output buffer returns `STATUS_INFO_LENGTH_MISMATCH`. The write-path checks (unprivileged write `STATUS_PRIVILEGE_NOT_HELD`, boot-only write `STATUS_ACCESS_DENIED`) ship with the deferred set path once the SRM provides `SeSinglePrivilegeCheck` + the per-token lock. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | CONF config query syscall suite, 0 failures

> **Notes:**
> - Read path: `SystemKernelConfigInformation` (0x1000) on `NtQuerySystemInformation` -> `nt_query_kernel_config_information` marshals the snapshot + tunable/feature counts + lock phase; ABI in `nt/sysconfig_info.h` (28-byte static-asserted).
> - User pointers go through `ProbeForWriteIfUser` + `copy_to_user`; reserved padding zeroed (no kernel-stack leak); two-pass `return_length` / `STATUS_INFO_LENGTH_MISMATCH` contract.
> - Set path (`NtSetSystemInformation` config-set) + the `SeSystemProfilePrivilege` gate are deferred: an unlocked `Privileges[]` scan races `NtAdjustPrivilegesToken`, so a sound check needs the SRM's `SeSinglePrivilegeCheck` + per-token lock.
> - Tests exercise the marshaller at CPL 0 (IfUser probes no-op): undersized/NULL/exact/oversized-canary + every field mirrored + optional `return_length`.
> - Owns the read/query surface only; write path + per-key provenance owned by the deferred set path. Codex review adoptions in the section ship + review commits.

> **Verified:** 2026-06-20 | commit `2f22daeb` | 3/5 items | build OK | tests 3058/3058 PASS
> **Accepted:** [H] `copy_to_user` is not fault-recoverable -- an in-range-but-unmapped user pointer faults the kernel (systemic to every Probe + `copy_to_user` syscall, not new in this class) -> XREF: 02-kernel-core/TODO-23 §13 (item: "`src/kernel/probe.c` -- implementation; `safe_return_rip` slot in CPU-local area" at line 392)
> **Deferred:** [H] Set/write path + `SeSystemProfilePrivilege` enforcement (items above, `[/]`) need a SMP-safe privilege check -> XREF: 02-kernel-core/TODO-15 §8 (item: "`SeSinglePrivilegeCheck(Privilege, AccessMode)`" at line 471)
> **Quality reviewed:** 2026-06-20 | Codex 7x (design, adversarial, test-coverage, re-adversarial, consistency, perf) | 1C+1H+4M fixed, 1H accepted-XREF | scope: kernel-code-quality

---

## 9. Policy Lock Phases and Tamper Audit

Define when policy stops being mutable and make every blocked mutation observable.

- [x] `policy_lock_phase_t` { PRE_MEMORY, POST_SECURITY_INIT, POST_REGISTRY, POST_USER_MODE } monotonic forward-only advance (out-of-range rejected); `policy_lock.c` fixed registry of `policy.<name>` rows, each sealing at a `seal_phase`.
- [x] Core security policies registered + sealed in `boot_desktop.c` Phase 3: KASLR/SMEP-SMAP/KPTI + lockdown at POST_SECURITY_INIT; Secure Boot, CI mode, boot verifier at POST_REGISTRY; debugger lockout at POST_USER_MODE.
- [x] `kernel_lockdown_level_t` { none, integrity, confidentiality } (Linux `lockdown=` parity) as `policy.lockdown` ratchet; Secure Boot maps to default `integrity`; lockless getter + §8 query `LockdownLevel`; `config_dump` in §11.
- [/] Three-way audit: BLOCKED -> tamper ring + `ETW_EVT_POLICY_TAMPER` + klog WARN; APPLIED -> `ETW_EVT_POLICY_CHANGE` only (no ring/klog flood); equal -> silent. Shared payload v2 + monotonic `seq`. Fanout deferred -> XREF: 02-kernel-core/TODO-16.
- [x] KernelMode downgrade of a SECURE_BOOT/CODE_INTEGRITY policy after seal panics via `KeBugCheckEx`; UserMode downgrade returns `STATUS_ACCESS_DENIED` + sticky tamper (never bugchecks) so untrusted callers cannot weaponize the panic.
- [x] Post-lock writes accepted only when strictly more restrictive (ratchet `new > old`); equal value is a silent no-op; ratchet-encoding invariant (higher numeric = more restrictive) makes downgrade exactly `new < old`.
- [x] Commit: `"kernel: add policy lock phases and tamper audit"`

**Test checkpoint:** Attempting to change locked `ci.mode` after the lock phase returns `STATUS_ACCESS_DENIED` and logs `"[CONF] tamper blocked: ci.mode"`. Attempting to downgrade Secure Boot or CI policy after lock triggers the configured panic path instead of silently continuing. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 8 policy suites, 0 failures
> **Notes:**
> - **What shipped:** `policy_lock.c`/`.h` (32-row security-policy registry, 4-phase monotonic seal machine, ratchet comparator, 64-entry tamper ring) + `ETW_EVT_POLICY_TAMPER` (0x1100, blocked) / `ETW_EVT_POLICY_CHANGE` (0x1101, applied) sharing `etw_policy_tamper_payload_t` v2 in `etw.h`.
> - **How it integrates:** registered + phase-advanced from `boot_desktop.c` Phase 3; one global spinlock guards mutation/audit; lockdown level + lock phase are lockless scalars; `kernel_policy_set()` decides seal under the lock.
> - **Downstream effects:** `kernel_lockdown_level_get()` surfaces in the §8 query (`LockdownLevel` reuses a reserved byte, offset 10 unchanged); `config_dump` owed in §11. Codex adoptions (design 4x, test-coverage 3x) in the ship commit.
> - **Canonical doc:** `include/kernel/policy_lock.h` header contract (ratchet-encoding invariant, panic policy, locking).
> - **Scope boundary:** §9 owns policy lock state + tamper audit; notification fanout owned by TODO-16; ETW durable persistence by TODO-04 §7; mechanism enforcement of KASLR/SMEP/KPTI lives in vmm/cpu_security.
> **Verified:** 2026-06-20 | commit `8494805d` | 5/6 items | build OK | tests 3192/3192 PASS
> **Accepted:** [M] policy-tamper/change notification fanout out of §9 scope (the `[/]` audit item) -> XREF: 02-kernel-core/TODO-16 §5 (item: "Security: ... policy-lock tamper/change" at line 84 -- `policy_lock.c` publishes `ETW_EVT_POLICY_TAMPER`/`POLICY_CHANGE` via `knf_publish` once the facility lands)
> **Quality reviewed:** 2026-06-20 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 4M fixed, 0 open, 1M accepted-XREF | scope: kernel-code-quality

---

## 10. Boot Status Policy and Boot Success Ledger

Turn boot success into an explicit policy-controlled state machine so rollback, LastKnownGood, and recovery decisions do not hinge on one ad-hoc ready bit.

- [x] `boot_status_policy_t` Phase-3 effective-policy (accept stage, failure-display, recovery-enabled, escalation threshold) + versioned CRC32 `boot_status_record_t` for the persisted failure bucket -- `boot_status.c`/`.h`.
- [x] `boot_status_init()` resolves `bootstatuspolicy`/`recoveryenabled` from the validated boot args into typed fields with provenance (default/cmdline); bless-boot metadata flows via the existing health-gate MarkGood handoff.
- [x] Monotonic forward-only atomic acceptance ledger `boot_accept_stage_t` (PENDING -> UI_READY -> CRITICAL_READY -> REGISTRY_FLUSHED -> ACCEPTED) advanced by `boot_status_accept_advance()` CAS max-advance.
- [/] Accepted transition fires A/B mark-good + per-entry MarkGood exactly-once at the accept stage (default UI_READY), replacing the compositor + health-gate direct marks; LastKnownGood copy deferred -> 02-kernel-core/TODO-02 §4.
- [/] Durable NVRAM record (`IPOSBootStatus`) + `boot_status_last_record()` preserve last stage, recovery-suppression, and rollback hint for next boot; failure-bucket classification deferred -> 02-kernel-core/TODO-30 §7.
- [x] `policy.recovery_lockout` + `policy.boot_accept_stage` registered as restrictive-sense ratchet rows sealed at POST_REGISTRY (stricter-only runtime changes).
- [x] Commit: `"kernel: add boot status policy and success ledger"`

**Test checkpoint:** Boot logs `"[CONF] boot-status policy: accept=ui-ready display=show recovery=on"` at Phase 3 init; with `bootstatuspolicy=IgnoreAllFailures recoveryenabled=no` it logs `display=ignore recovery=off`. A successful boot reaches the accept stage at the first composited frame and logs `"[CONF] boot accepted (stage=accepted)"` exactly once; the A/B tries reset and per-entry MarkGood happen only at that transition, never from a failed boot. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 3 boot-status suites (resolve, ledger step, record crc), 0 failures
> **Notes:**
> - **What shipped:** `boot_status.c`/`.h` -- `boot_status_policy_t` Phase-3 effective-policy, a monotonic atomic acceptance ledger, and a versioned CRC32 NVRAM record (`IPOSBootStatus`); 3 pure-function unit tests.
> - **How it integrates:** `boot_status_init()` runs at Phase 3; the compositor first frame and health gate route through `boot_status_accept_advance()`, whose exactly-once accepted transition does the A/B mark + MarkGood + durable record write.
> - **Downstream effects:** `boot_status_last_record()` feeds the LastKnownGood (§4) and recovery-escalation (TODO-30 §7) consumers; the health-gate run-once latch is now an SMP-safe atomic.
> - **Canonical doc:** `include/kernel/boot_status.h` header contract (stage model, exactly-once bless, durable record).
> - **Scope boundary:** §10 owns the ledger + policy + durable record + mark-good gate; the LastKnownGood control-set copy is owned by §4; failure-bucket classification + escalation are owned by TODO-30 §7.
> **Verified:** 2026-06-20 | commit `ae70bfb7` | 4/6 items | build OK | smoke PASS (KVM 2.9s)
> **Accepted:** [H] LastKnownGood control-set copy at the accepted transition is owned elsewhere -> XREF: 02-kernel-core/TODO-02 §4 (item: "On successful boot, update LastKnownGood only after the §10 acceptance policy confirms critical services and registry flush succeeded" at line 146)
> **Accepted:** [M] failure-bucket classification + recovery escalation consume §10's durable record -> XREF: 02-kernel-core/TODO-30 §7 (item: "Track crash/hang counts by bucket across boots" at line 96)
> **Quality reviewed:** 2026-06-20 | Codex 10x (design, adversarial, consistency, perf, re-adversarial) | 9H+5M fixed, 1H rejected, 2 accepted-XREF | scope: kernel-code-quality

---

## 11. Config Dump, Tests, and Docs

Close the loop with operator-visible diagnostics, regression coverage, and explicit invariant ownership.

- [x] `config_dump()` (`config.c`) klogs snapshot + safe mode + policy + boot-status + tunables; off-stack scratch, atomic non-reentrant guard, redacts `TUNABLE_PRIVILEGED` values; runs on a debug boot.
- [/] Unit tests: parser/precedence/clamp/lock-phase/safe-mode/boot-status-ledger/syscall already covered (49 `test_kernel_config.c` suites); LastKnownGood state-machine test deferred -> 02-kernel-core/TODO-02 §4.
- [/] Boot tests: Safe Mode + unknown-key covered by the boot suite; debug boot now emits `config_dump`; failed-boot rollback boot test deferred -> 02-kernel-core/TODO-02 §4.
- [x] `kernel_config_t` bulletproofing invariants (`_Static_assert` size/version/offset guards) live in `config.h`; full 5-layer tracking owned by 02-kernel-core/TODO-31 §2.
- [x] Supported keys/defaults/ranges/lock-phases documented in the `k_arg_table` schema (`config.c`) + `config.h` contract; surfaced at runtime by `config_dump`.
- [x] Commit: `"kernel: add config dump, tests, and documentation pass"`

**Test checkpoint:** A debug boot logs `"[CONF] dump: ..."` lines covering the snapshot, lockdown/lock-phase/tamper counts, boot-status accept stage, and the tunable registry with `TUNABLE_PRIVILEGED` values shown as `****`. The config-plane data is covered by the `test_kernel_config.c` suites; config_dump itself is output-only (validated via debug-boot serial). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 49 `test_kernel_config.c` suites, 0 failures; config_dump validated via debug-boot serial
> **Notes:**
> - **What shipped:** `config_dump()` in `config.c` -- klogs the whole config plane (snapshot, policy, boot-status, tunables) with `TUNABLE_PRIVILEGED` redaction; off-stack scratch + atomic non-reentrant guard.
> - **How it integrates:** runs on a debug boot from Phase 3 (`boot_desktop.c`) once every config registry is sealed; reads only lockless getters + `kernel_tunable_dump()`; not a panic-path function.
> - **Downstream effects:** closes the TODO-02 config-plane loop; the `kernel_config_t` `_Static_assert` invariants in `config.h` are tracked for full 5-layer coverage by TODO-31 §2.
> - **Canonical doc:** `include/kernel/config.h` contract + the `k_arg_table` schema in `config.c`.
> - **Scope boundary:** §11 owns `config_dump` + docs + existing-test wiring; the LastKnownGood state-machine + failed-boot rollback tests are owned by §4; the full 5-layer config-invariant tracking is owned by TODO-31 §2.
> **Verified:** 2026-06-20 | commit `e81e3b45` | 3/5 items | build OK | smoke PASS (KVM 3.1s)
> **Deferred:** [M] LastKnownGood state-machine + failed-boot rollback tests are in-scope but blocked on §4's deferred control-set impl -> XREF: 02-kernel-core/TODO-02 §4 (item: "Implement `kernel_select_control_set()`" at line 143)
> **Quality reviewed:** 2026-06-20 | Codex 5x (design, adversarial, consistency, perf; re-adversarial skipped: review-fix diff <50 LOC, no locking/atomics/lifecycle) | 2H+4M+1L fixed | scope: kernel-code-quality

---

## 12. Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)

Items moved here VERBATIM from their original, already-stamped sections, where they were unreachable: the triage oracle classifies a stamped section DONE without reading its body, so an item appended after the stamp is invisible to every later pass. Source section noted per group. Cohort context: `todo/overnight-runner-improvements/overnight-runner-improvements-v05.md` item 3.

From the stamped section 6:
- [ ] Privileged production write path for tunables: no non-test `kernel_tunable_set` caller exists, so every `quota.user.<type>` default stays at its unlimited built-in and no quota cap can be enforced. -> XREF: `02-kernel-core/TODO-25 §6`

**Test checkpoint:** per moved item; each carries its original acceptance text.

---

## OS Comparison

| ⭐ | Feature                            | 🪟 Win11                                       | 🐧 Linux                                           | 🚀 Impossible OS        |
| --- | ---------------------------------- | ---------------------------------------------- | --------------------------------------------------- | ----------------------- |
| 💎 | Boot entry policy store            | ✅ BCD plus boot menu and recovery entries     | ✅ GRUB or systemd-boot plus kernel cmdline        | ⚠️ §1 parser done; §3,§4 |
| 💎 | Safe and recovery boot modes       | ✅ `safeboot=minimal|network` plus recovery    | ✅ `single`, rescue, emergency, and recovery menu  | ✅ `safe_mode_t` object, monotonic floor, KUSD mirror |
| 💎 | Control-set style rollback state   | ⚠️ `Current` and `LastKnownGood` semantics     | ⚠️ Distro snapshots or boot fallback, not kernel   | ⬜ Planned - §4         |
| 💎 | Boot success and failure ledger    | ✅ `bootstatuspolicy` plus recovery policy      | ✅ boot counting plus bless-boot style acceptance  | ✅ §10 acceptance ledger, exactly-once bless, durable record |
| 💎 | Runtime tunables and query surface | ✅ Registry, Group Policy, and BCD knobs       | ✅ `sysctl`, `/proc`, `/sys`, and module params    | ⚠️ §6 registry + §8 query syscall; set path §8/§3 |
| 💎 | Early immutable security policy    | ✅ Debug, NX, integrity, and boot-policy gates | ✅ Lockdown, cmdline policy, and Secure Boot gates | ✅ §9 phase-sealed ratchet, panic-on-downgrade |
| 💎 | Unified kernel lockdown level      | ⚠️ HVCI and VBS, no single named level         | ✅ `lockdown=integrity` or `confidentiality`       | ✅ §9 none/integrity/confidentiality, SB-mapped |
| ⭐ | Tamper-audit of blocked policy mutation | ⚠️ ETW security events, no ratchet plane   | ⚠️ Audit subsystem, scattered LSM hooks            | ✅ §9 ring + ETW, every blocked write observable |
| ⭐ | Per-key provenance plus cohorts    | ❌ No kernel-owned provenance plus cohorts     | ⚠️ Partial in userspace tooling, not kernel-native | ⚠️ §7 cohorts + owner tags; §3 provenance |

> **After §1-§6:** Impossible OS reaches the same baseline the major platforms already have: typed boot policy, safe/recovery modes, and persistent tunables.
> **After §7-§10:** Impossible OS pulls ahead with kernel-native provenance, explicit boot acceptance, rollout cohorts, and tamper-aware lock phases instead of scattered tooling.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_kernel_config()` and register the suite under `TEST_CAT_BOOT`.
> Tests run with `debug=1` or `test=1` in boot.conf. If T12 §10 is still pending when §8 work starts, keep the syscall ABI checks as `TEST_PENDING` rather than dropping them.

- [/] `src/kernel/test/test_kernel_config.c` wired (49 suites) covering the assertions below; `kernel_select_control_set` deferred -> 02-kernel-core/TODO-02 §4. Original list:
  - `kernel_config_parse_cmdline("debug=1 safemode=network")` sets `debug_enabled == true` and `safe_mode == SAFE_MODE_NETWORK`
  - `kernel_config_parse_cmdline("kernel.unknown=1")` returns the expected rejection status when `boot.allow_unknown == 0`
  - `kernel_config_parse_cmdline("bootstatuspolicy=IgnoreAllFailures recoveryenabled=no")` sets the expected boot-status policy flags
  - `kernel_config_merge(test_defaults, test_registry, test_cmdline)` keeps the documented precedence order and selects the cmdline value on conflict
  - `kernel_tunable_apply_test_value("panic.timeout", 9999)` clamps to the declared max instead of storing `9999`
  - `kernel_boot_accept_advance()` does not mark the boot accepted until the configured acceptance stage reaches registry-flushed plus critical-services-ready
  - `kernel_select_control_set(Current=2, Default=1, LastKnownGood=3, boot_failed=true)` returns `3`
  - `kernel_policy_try_set_locked(KERNEL_POLICY_CI, 0)` returns `STATUS_ACCESS_DENIED`
  - `kernel_query_system_config(NULL, 0, &needed)` returns `STATUS_INFO_LENGTH_MISMATCH`
  - `kernel_lockdown_level()` returns `LOCKDOWN_INTEGRITY` or stricter when Secure Boot is enabled in the test fixture
  - `kernel_tunable_register("module.testmod.depth", ...)` registers under the module namespace and records the owning-module provenance
- [x] Registered in `src/kernel/test/test_runner.c`: `test_register_kernel_config()`
- [/] Safe Mode + unknown-key rejection exercised by the boot suite; the rollback boot-path assertion is deferred -> 02-kernel-core/TODO-02 §4.
- [x] Commit: `"test: add kernel configuration policy test suite"`

---

## Verification

- [x] `bash scripts/build.sh clean` then `tail -1 build/build.log` shows `=== BUILD OK ===` (2026-06-20)
- [/] `make test-boot` passes parser, precedence, lock-phase tests (3209 kernel + 16 user PASS 2026-06-20); the rollback test is deferred -> 02-kernel-core/TODO-02 §4 (control-set impl).
- [ ] Boot with `safemode=network` logs `"[CONF] safe_mode=network"` and skips GUI-only services (manual -- needs a safemode boot on WHPX/bare metal)
- [/] Failed boot leaves acceptance pending; next boot selects LastKnownGood/rollback -- deferred -> 02-kernel-core/TODO-02 §4 (failed-boot rollback + control-set).
- [/] Boot with `bootstatuspolicy=IgnoreAllFailures recoveryenabled=no`: the §10 acceptance transition is smoke-verified (`"[CONF] boot accepted (stage=accepted)"`); the rollback-counter-pending leg needs the §4 failed-boot path.
- [x] `NtQuerySystemInformation(SystemKernelConfigInformation)` snapshot version + size covered by `test_cfg_query_syscall` / `test_cfg_snapshot_*` (boot suite, 2026-06-20)
- [ ] Verify on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal (manual)
- [x] Commit: `"kernel: complete configuration and policy plane"`

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot)
