# TODO-02 -- Kernel Configuration & Policy Plane

> **Goal:** Build the kernel's authoritative configuration plane: boot arguments, BCD-style boot entries, safe-mode flags, control sets, LastKnownGood, feature flags, runtime tunables, debug policy, crash policy, and immutable early-boot policy. The Registry stores durable state, but the kernel must own parsing, validation, phase-safe publication, rollback, and the contract every subsystem consumes.

> [!IMPORTANT]
> **Current state:** Boot config is partially delivered by the UEFI bootloader through `g_boot_info.config` and consumed by Phase 0. Registry policy exists only as ad-hoc reads in individual subsystems. There is no typed kernel configuration API, no safe-mode policy object, no BCD-equivalent boot-entry parser, no control-set selection, no rollback to LastKnownGood, and no global feature-flag/tunable registry.

## Inputs

- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- [`src/kernel/main/boot_hw.c`](../../src/kernel/main/boot_hw.c)
- [`src/kernel/main/boot_init.c`](../../src/kernel/main/boot_init.c)
- [`src/kernel/registry.c`](../../src/kernel/registry.c)
- → XREF: [`TODO-01-kernel-init-sequencing.md`](./TODO-01-kernel-init-sequencing.md) -- phase gates and degraded boot policy
- → XREF: [`TODO-14-registry-completion.md`](./TODO-14-registry-completion.md) -- hive persistence and control-set storage
- → XREF: [`TODO-10-kernel-security-hardening.md`](./TODO-10-kernel-security-hardening.md) -- security-sensitive flags become lock-once after Phase 0
- → XREF: [`01-boot-platform/TODO-21-ab-boot-rollback.md`](../01-boot-platform/TODO-21-ab-boot-rollback.md) -- boot-slot rollback consumes LastKnownGood decisions

## Outcome

- One typed `kernel_config_t` snapshot is available from Phase 0 onward.
- Boot arguments, registry policy, firmware policy, and defaults merge through a deterministic precedence order.
- Safe Mode, debug mode, test mode, no-network, no-gui, verifier, and recovery flags are first-class.
- ControlSet selection and LastKnownGood rollback are kernel-visible and auditable.
- Runtime tunables are validated, range-clamped, versioned, and exposed through native query/set syscalls.
- Security-sensitive fields are immutable after their lock phase.

## Implementation Order

| ⭐ | Order | Deliverable | Depends On | Status |
| -- | :---: | ----------- | ---------- | :----: |
| 💎 | 1 | Boot argument schema and parser | bootloader config | [ ] |
| 💎 | 2 | `kernel_config_t` immutable Phase 0 snapshot | §1 | [ ] |
| 💎 | 3 | Registry-backed policy merge | T14 | [ ] |
| 💎 | 4 | ControlSet and LastKnownGood selection | §3 | [ ] |
| ⭐ | 5 | Safe Mode and recovery policy object | §2, §4 | [ ] |
| 💎 | 6 | Runtime tunable registry | §2 | [ ] |
| ⭐ | 7 | Feature flag gates and experiment cohorts | §6 | [ ] |
| 💎 | 8 | Native query/set config syscalls | T12 | [ ] |
| ⭐ | 9 | Policy lock phases and tamper audit | T15, T10 | [ ] |
| 💎 | 10 | Config dump, tests, and docs | §1..§9 | [ ] |

## 1. Boot Argument Schema and Parser

- [ ] Define `boot_arg_desc_t`: name, type, default, min/max, phase, security class, help text.
- [ ] Support bool, integer, string, enum, duration, byte-size, and CSV-list values.
- [ ] Normalize aliases: `debug=1`, `/debug`, `safemode=minimal`, `safemode=network`, `nogui`, `noacpi`, `verifier=*`.
- [ ] Reject unknown `kernel.` namespace keys unless `boot.allow_unknown=1`.
- [ ] Preserve original command line for crash dump and diagnostics.

## 2. `kernel_config_t` Immutable Phase 0 Snapshot

- [ ] Add `include/kernel/config.h` with `kernel_config_t` and `kernel_config_get()`.
- [ ] Publish a read-only snapshot after bootloader config validation.
- [ ] Include boot mode, graphics mode, debug transports, logging mode, init flags, panic policy, CI policy, and verifier flags.
- [ ] Add static asserts for struct version and size.

## 3. Registry-Backed Policy Merge

- [ ] Load `HKLM\SYSTEM\CurrentControlSet\Control\Kernel` during Phase 2.
- [ ] Merge precedence: compiled defaults < boot entry defaults < registry policy < boot command line < firmware-enforced policy.
- [ ] Produce per-key provenance for `config_dump`.
- [ ] Log every override with previous value, new value, and source.

## 4. ControlSet and LastKnownGood Selection

- [ ] Implement `kernel_select_control_set()`: Current, Default, Failed, LastKnownGood.
- [ ] Mark boot as pending until Phase 3 reaches desktop or console ready.
- [ ] On successful boot, update LastKnownGood only after critical services and registry flush succeed.
- [ ] On failed boot, roll back control set and signal boot-platform A/B rollback.

## 5. Safe Mode and Recovery Policy Object

- [ ] Define `safe_mode_t`: none, minimal, network, directory-services-repair equivalent.
- [ ] Gate drivers, network stack, desktop, third-party modules, code integrity relaxations, and services.
- [ ] Surface safe-mode reason and selected control set to crash screen and boot logs.
- [ ] Ensure Safe Mode cannot be disabled by a registry value once requested by boot policy.

## 6. Runtime Tunable Registry

- [ ] Implement `kernel_tunable_register(name, type, flags, min, max, default, callback)`.
- [ ] Support read-only, boot-only, runtime, privileged, and debug-only tunables.
- [ ] Add core tunables for klog, panic timeout, timer resolution policy, ALPC message caps, handle quota defaults, and verifier flags.
- [ ] Invoke callbacks at PASSIVE_LEVEL only; queue work if set from syscall path.

## 7. Feature Flag Gates and Experiment Cohorts

- [ ] Implement `kernel_feature_enabled(feature_id)` with boot and registry providers.
- [ ] Support cohort hashing by machine ID for staged rollout of risky features.
- [ ] Add forced-on/forced-off audit events.
- [ ] Ensure security features cannot be cohort-disabled on Secure Boot systems.

## 8. Native Query/Set Config Syscalls

- [ ] Add `SystemKernelConfigInformation` to `NtQuerySystemInformation`.
- [ ] Add `NtQuerySystemConfiguration`, `NtSetSystemConfiguration` stubs in SSDT.
- [ ] Enforce `SeSystemProfilePrivilege` or administrator token for writes.
- [ ] Return provenance, effective value, and mutability state.

## 9. Policy Lock Phases and Tamper Audit

- [ ] Define lock phases: pre-memory, post-security-init, post-registry, post-user-mode.
- [ ] Lock code integrity, KASLR, SMEP/SMAP/KPTI, debugger enablement, and boot verifier flags at the earliest safe phase.
- [ ] Emit tamper audit events through TODO-04 and TODO-16/TODO-24.
- [ ] Panic on attempted downgrade of Secure Boot or CI policy after lock.

## 10. Config Dump, Tests, and Docs

- [ ] Add `config_dump()` serial/klog output with redaction for secrets.
- [ ] Unit tests: parser, precedence, range clamp, lock phase, safe-mode gating, LastKnownGood state machine.
- [ ] Boot tests: normal boot, Safe Mode minimal, Safe Mode network, failed boot rollback, debug boot.
- [ ] Add bulletproofing invariants to TODO-31 for `kernel_config_t` layout.

