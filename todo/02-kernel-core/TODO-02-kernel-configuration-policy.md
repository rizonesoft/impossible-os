---
schema_version: 1
id: kernel-configuration-policy
domain: 02-kernel-core
status: active
title: "TODO-02 -- Kernel Configuration & Policy Plane"
---

# TODO-02 -- Kernel Configuration & Policy Plane

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

| ⭐ | Order | Deliverable                                 | Depends On                          | Status |
| --- | :---: | ------------------------------------------- | ----------------------------------- | :----: |
| 💎 | 1 | Boot argument schema and parser                 | D01 T07 §3, §5                      | [x] |
| 💎 | 2 | `kernel_config_t` immutable Phase 0 snapshot    | §1, T01 §2, §6                      | [x] |
| 💎 | 3 | Registry-backed policy merge                    | §2, §4, T14 §8, §12, §13           | [/] |
| 💎 | 4 | ControlSet and LastKnownGood selection          | §2, §10, T14 §12, D01 T21 §4, §5   | [/] |
| 💎 | 5 | Safe Mode and recovery policy object            | §2, §4, D01 T07 §5                 | [/] |
| 💎 | 6 | Runtime tunable registry                        | §2                                  | [ ] |
| ⭐ | 7 | Feature flag gates and experiment cohorts       | §6                                  | [ ] |
| 💎 | 8 | Native query/set config syscalls                | §2, §6, T12 §10, T15 §8            | [ ] |
| ⭐ | 9 | Policy lock phases and tamper audit             | §2, §3                              | [ ] |
| 💎 | 10 | Boot status policy and boot success ledger     | §4, §5, D01 T21 §5, T30 §7         | [ ] |
| 💎 | 11 | Config dump, tests, and docs                   | §1-10, T11 §11, T31 §2             | [ ] |

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

> **Deferred:** [H] §4 is blocked on three prerequisites: (a) `registry_init` creates only `HKLM\SYSTEM` -- the `SYSTEM\Select` / `ControlSetNNN` / `CurrentControlSet`-link substrate §4 must read does not exist yet (T14 §12); (b) LastKnownGood update, boot-pending, failed-boot writeback, and D01 T21 rollback-hint all gate on the §10 acceptance ledger (unimplemented); (c) persisting the chosen control-set id cannot go into the immutable §2 `kernel_config_t` -- it needs the SAME Phase-2 effective-policy object §3 also needs (operator-reserved architecture). The clean slice (validate Select, resolve `CurrentControlSet`->`ControlSetNNN`, publish selected id+reason in a Phase-2 object) unblocks §3 but still requires (a). -> XREF: 02-kernel-core/TODO-02 §10 (item: "Define `boot_status_policy_t`" at line 234); -> XREF: 02-kernel-core/TODO-14 §12 (item: "Registry Symlink Completion" at line 519)

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

**Test checkpoint:** Boot with `safemode=network`: serial shows `"[CONF] safe_mode=network reason=boot-policy"` and GUI-only services stay disabled while networking remains enabled. Repeated-failure recovery boot shows a distinct `reason=recovery` code and cannot be downgraded by registry state. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | CONF safe-mode suites, 0 failures

> **Notes:**
> - `safe_mode_resolve()` monotonic floor: recovery->DSREPAIR, safe boot->MINIMAL; operator `safemode=` arg raises only, never lowers below the boot-policy floor.
> - KUSD `MitigationPolicies` publishes EFFECTIVE CI policy; raw `nointegritychecks`/`testsigning` masked when `SAFE_COMP_CI_RELAX` is gated off in any safe boot.
> - Reason matrix: recovery-first -> `RECOVERY`; escalation above floor -> `OPERATOR`; floor -> `BOOT_POLICY`.
> - Control-set surfacing through `kernel_config_get()` deferred to §4 (ControlSet selection owns the field).
> - Codex review adoptions + per-finding evidence in the section ship commit.

---

## 6. Runtime Tunable Registry

Give subsystems one typed registration surface for mutable policy instead of one-off globals and parser branches.

- [ ] Implement `kernel_tunable_register(name, type, flags, min, max, default, callback)`.
- [ ] Support read-only, boot-only, runtime, privileged, and debug-only tunables.
- [ ] Add core tunables for klog, panic timeout, timer resolution policy, ALPC message caps, handle quota defaults, and verifier flags.
- [ ] Record owner subsystem, lock phase, and provenance metadata for every tunable so audit output can explain who registered it.
- [ ] Invoke callbacks at PASSIVE_LEVEL only; queue work if set from syscall, DPC, or interrupt context, and reject recursive self-write loops with an explicit error.
- [ ] Let loadable modules register tunables via `kernel_tunable_register` under a `module.<name>.` namespace with provenance (Linux `module_param` + `/sys/module` parity); the loader supplies metadata. -> XREF: D04 T05.
- [ ] Commit: `"kernel: add runtime tunable registry"`

**Test checkpoint:** Writing `panic.timeout=9999` clamps to the declared max and logs `"[CONF] tunable clamp: panic.timeout"`. A read-only tunable write returns `STATUS_ACCESS_DENIED`, and a callback requested from syscall context runs later at PASSIVE_LEVEL. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 7. Feature Flag Gates and Experiment Cohorts

Make feature flags discoverable, auditable, and safe enough for staged rollout instead of ad-hoc ifdefs and hidden globals.

- [ ] Implement `kernel_feature_enabled(feature_id)` with boot and registry providers.
- [ ] Support cohort hashing by machine ID for staged rollout of risky features.
- [ ] Add forced-on/forced-off audit events.
- [ ] Ensure security features cannot be cohort-disabled on Secure Boot systems.
- [ ] Reserve explicit `feature.<name>` and `experiment.<name>` namespaces with owner tags so anonymous flags cannot accumulate.
- [ ] Commit: `"kernel: add feature flag gates and rollout cohorts"`

**Test checkpoint:** The same machine id lands in the same cohort on repeated boots, `feature.debug_menu=off` disables only that feature, and `feature.kpti=off` under Secure Boot is rejected with `"[CONF] secure feature override blocked: kpti"`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 8. Native Query/Set Config Syscalls

Expose the effective configuration through stable NT contracts once the SSDT surface is ready.

- [ ] Add `SystemKernelConfigInformation` to `NtQuerySystemInformation`.
- [ ] Add `NtQuerySystemConfiguration` and `NtSetSystemConfiguration` contracts plus shared structs in NT headers before wiring SSDT handlers.
- [ ] Enforce `SeSystemProfilePrivilege` or administrator token for writes.
- [ ] Return provenance, effective value, and mutability state.
- [ ] If T12 §10 is not ready yet, land the kernel-side marshalling and unit coverage first and keep the syscall ABI checks as `TEST_PENDING` instead of silently dropping them.
- [ ] Commit: `"kernel: add native configuration query and set syscalls"`

**Test checkpoint:** `NtQuerySystemInformation(SystemKernelConfigInformation)` returns snapshot version and size, an unprivileged write returns `STATUS_PRIVILEGE_NOT_HELD`, a boot-only tunable write returns `STATUS_ACCESS_DENIED`, and an undersized output buffer returns `STATUS_INFO_LENGTH_MISMATCH`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 9. Policy Lock Phases and Tamper Audit

Define when policy stops being mutable and make every blocked mutation observable.

- [ ] Define lock phases: pre-memory, post-security-init, post-registry, post-user-mode.
- [ ] Lock code integrity, KASLR, SMEP/SMAP/KPTI, debugger enablement, and boot verifier flags at the earliest safe phase.
- [ ] Define `kernel_lockdown_level_t` { none, integrity, confidentiality } as first-class config (Linux `lockdown=` parity): map Secure Boot to a default level, expose via `config_dump`/query, classify confidentiality-class restrictions.
- [ ] Emit a tamper audit event (policy name, attempted value, caller mode, phase, result) and route it to the kernel notification facility (D02 T16) for fanout and to ETW/system logging (D02 T04 §7) for durable security-event persistence.
- [ ] Panic on attempted downgrade of Secure Boot or CI policy after lock.
- [ ] Allow post-lock changes only when the policy class explicitly marks the new value as strictly more restrictive than the old one.
- [ ] Commit: `"kernel: add policy lock phases and tamper audit"`

**Test checkpoint:** Attempting to change locked `ci.mode` after the lock phase returns `STATUS_ACCESS_DENIED` and logs `"[CONF] tamper blocked: ci.mode"`. Attempting to downgrade Secure Boot or CI policy after lock triggers the configured panic path instead of silently continuing. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 10. Boot Status Policy and Boot Success Ledger

Turn boot success into an explicit policy-controlled state machine so rollback, LastKnownGood, and recovery decisions do not hinge on one ad-hoc ready bit.

- [ ] Define `boot_status_policy_t`: acceptance stage, failure-display policy, recovery-enabled flag, failed-boot thresholds, and persisted failure bucket.
- [ ] Parse `bootstatuspolicy`, `recoveryenabled`, and bootloader-provided bless-boot metadata into typed fields with provenance.
- [ ] Track a per-boot acceptance ledger: pending -> console-or-desktop-ready -> critical-services-ready -> registry-flushed -> accepted.
- [ ] Gate `mark_boot_successful()`, LastKnownGood update, and rollback-counter reset on the configured acceptance stage instead of a hard-coded Phase 3 heuristic.
- [ ] Preserve the last failure bucket, rollback hint, and recovery-suppression reason for D01 T21 §4-§5 and T30 §7 consumers on the next boot.
- [ ] Lock boot-status policy after the same phase boundary as other boot-critical policy; permit only stricter runtime changes.
- [ ] Commit: `"kernel: add boot status policy and success ledger"`

**Test checkpoint:** Boot with `bootstatuspolicy=IgnoreAllFailures recoveryenabled=no`: serial shows `"[CONF] boot_accept=pending policy=IgnoreAllFailures recovery=off"` and rollback counters do not reset until the configured acceptance stage is reached. A successful boot advances through the ledger in order and only then logs `"[CONF] boot accepted"` before D01 T21 §5 resets tries. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 11. Config Dump, Tests, and Docs

Close the loop with operator-visible diagnostics, regression coverage, and explicit invariant ownership.

- [ ] Add `config_dump()` serial/klog output with redaction for secrets.
- [ ] Unit tests: parser, precedence, range clamp, lock phase, safe-mode gating, boot-status ledger, LastKnownGood state machine, and config syscall marshalling.
- [ ] Boot tests: normal boot, Safe Mode minimal, Safe Mode network, failed boot rollback, debug boot, and unknown-key rejection.
- [ ] Add bulletproofing invariants to T31 for `kernel_config_t` layout, versioning, and fixed-size string fields.
- [ ] Document supported keys, defaults, ranges, and lock phases in the header and TODO text so implementers do not need to reverse-engineer them from tests.
- [ ] Commit: `"kernel: add config dump, tests, and documentation pass"`

**Test checkpoint:** `config_dump()` redacts secrets while still printing provenance and boot-acceptance stage, the boot suite covers rollback and safe-mode scenarios, and T31 contains an explicit `kernel_config_t` invariant item before this section is marked done. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## OS Comparison

| ⭐ | Feature                            | 🪟 Win11                                       | 🐧 Linux                                           | 🚀 Impossible OS        |
| --- | ---------------------------------- | ---------------------------------------------- | --------------------------------------------------- | ----------------------- |
| 💎 | Boot entry policy store            | ✅ BCD plus boot menu and recovery entries     | ✅ GRUB or systemd-boot plus kernel cmdline        | ⚠️ §1 parser done; §3,§4 |
| 💎 | Safe and recovery boot modes       | ✅ `safeboot=minimal|network` plus recovery    | ✅ `single`, rescue, emergency, and recovery menu  | ✅ `safe_mode_t` object, monotonic floor, KUSD mirror |
| 💎 | Control-set style rollback state   | ⚠️ `Current` and `LastKnownGood` semantics     | ⚠️ Distro snapshots or boot fallback, not kernel   | ⬜ Planned - §4         |
| 💎 | Boot success and failure ledger    | ✅ `bootstatuspolicy` plus recovery policy      | ✅ boot counting plus bless-boot style acceptance  | ⬜ Planned - §10        |
| 💎 | Runtime tunables and query surface | ✅ Registry, Group Policy, and BCD knobs       | ✅ `sysctl`, `/proc`, `/sys`, and module params    | ⬜ Planned - §3, §6, §8 |
| 💎 | Early immutable security policy    | ✅ Debug, NX, integrity, and boot-policy gates | ✅ Lockdown, cmdline policy, and Secure Boot gates | ⚠️ §2 snapshot done; §8,§9 |
| 💎 | Unified kernel lockdown level      | ⚠️ HVCI and VBS, no single named level         | ✅ `lockdown=integrity` or `confidentiality`       | ⬜ Planned - §9         |
| ⭐ | Per-key provenance plus cohorts    | ❌ No kernel-owned provenance plus cohorts     | ⚠️ Partial in userspace tooling, not kernel-native | ⬜ Planned - §3, §7, §9 |

> **After §1-§6:** Impossible OS reaches the same baseline the major platforms already have: typed boot policy, safe/recovery modes, and persistent tunables.
> **After §7-§10:** Impossible OS pulls ahead with kernel-native provenance, explicit boot acceptance, rollout cohorts, and tamper-aware lock phases instead of scattered tooling.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_kernel_config()` and register the suite under `TEST_CAT_BOOT`.
> Tests run with `debug=1` or `test=1` in boot.conf. If T12 §10 is still pending when §8 work starts, keep the syscall ABI checks as `TEST_PENDING` rather than dropping them.

- [ ] Create `src/kernel/test/test_kernel_config.c` with:
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
- [ ] Register in `src/kernel/test/test_runner.c`: `test_register_kernel_config()`
- [ ] Add boot-path assertions to the existing boot suite so Safe Mode, rollback, and unknown-key rejection are exercised from real boot logs.
- [ ] Commit: `"test: add kernel configuration policy test suite"`

---

## Verification

- [ ] `bash scripts/build.sh clean` then `tail -1 build/build.log` shows `=== BUILD OK ===`
- [ ] `make test-boot` passes the kernel configuration parser, precedence, lock-phase, and rollback tests
- [ ] Boot with `safemode=network` logs the expected `"[CONF] safe_mode=network"` line and skips GUI-only services
- [ ] Failed boot before ready leaves boot acceptance pending; next boot selects LastKnownGood or rollback path and logs the reason
- [ ] Boot with `bootstatuspolicy=IgnoreAllFailures recoveryenabled=no` keeps rollback counters pending until the configured acceptance stage and logs the acceptance transition
- [ ] `NtQuerySystemInformation(SystemKernelConfigInformation)` returns the expected snapshot version and size
- [ ] Verify on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal
- [ ] Commit: `"kernel: complete configuration and policy plane"`

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot)
