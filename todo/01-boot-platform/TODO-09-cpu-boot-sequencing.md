---
schema_version: 1
id: cpu-boot-sequencing
domain: 01-boot-platform
status: active
title: "TODO-09 -- CPU Boot Sequencing & AP Hardening"
---

# TODO-09 -- CPU Boot Sequencing & AP Hardening

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Establish the correct activation order for CPU security and context features during boot phases 0 and 1, replicate that activation on every Application Processor (AP), validate AP feature consistency, pin safety-critical CR4 bits against post-boot modification, synchronize MTRR/PAT cache policy on each AP, and provide a comprehensive CPU register state audit trail. The feature implementations themselves live in `02-kernel-core/TODO-10-kernel-security-hardening.md` (security hardening) and `02-kernel-core/TODO-09-x86-64-architecture.md` (x86-64 architecture); this TODO owns the boot-sequencing contract between them -- what activates, in what phase, who ensures APs match the BSP, and what happens when they don't.

> [!IMPORTANT]
> **Scope boundary:** Do NOT implement CPU security features or CPU architecture features here. That work lives in:
> - NX/EFER, SMEP/SMAP, KPTI, PCID, Spectre, CET → `02-kernel-core/TODO-10-kernel-security-hardening.md`
> - XSAVE, MSR layer, UMIP, 1 GiB pages, topology, errata, VM detection → `02-kernel-core/TODO-09-x86-64-architecture.md`
> - TSC frequency calibration and timekeeping → `02-kernel-core/TODO-08-time-filetime-management.md`
> This TODO owns: phase placement, activation order within phases, AP-trampoline replication, AP feature consistency validation, CR4 bit pinning, MTRR/PAT AP synchronization, CPU register audit trail, and hypervisor pre-detection before the timer subsystem selects its driver.
> **Current state (code-truth 2026-04-11):** `cpuid_init()` runs in `src/kernel/main/boot_hw.c` after heap/klog (~259--263) with `boot_progress(..., POSTCODE_CPUID_OK)`. `cpu_harden()` / `vmm_apply_nx_policy()` / `cpu_harden_post_pagetable()` / `simd_enable_avx()` run in Phase 0 order shown there (~288--334); **no** `msr_init()`, `cpu_efer_harden()`, or `cpu_cr4_harden()` yet. `platform_detect()` fills `g_boot_info.hv_vendor` + `hv_flags` in `src/kernel/cpuid_platform.c`; it runs from `hv_supports_cr4_smep_smap()` in `src/kernel/cpu_security.c:56` (SMEP/SMAP gate, returns 0 today so SMEP/SMAP stay skipped) and again at start of `timer_hal_init()` in `src/kernel/timer.c:104` before LAPIC/PIT selection. AP path `smp_ap_main()` calls `cpu_harden()` + `cpu_harden_post_pagetable()` in `src/kernel/smp/smp.c` (~122--123) -- **not** the planned `ap_cpu_harden()` contract. PAT is programmed in `boot_hw.c` (~304--320) with value `0x0007040600010406` (§8 NOTE: decode vs intent). **Registry keys** in §3 Goal prose (`HKLM\HARDWARE\VM\...`) are **not** populated in tree -- only `boot_info` fields.

---

## Inputs

- `src/kernel/main/boot_hw.c` -- `boot_phase0()` implementation (CPUID, `cpu_harden`, PAT, SIMD); primary attach point for §2 ordering
- `src/kernel/main/boot_init.c` -- `boot_phase1/2/3()` and boot orchestration comments (see `TODO-01-kernel-init-sequencing.md` §2)
- `include/kernel/boot_init.h` -- `BOOT_STEP`, `BOOT_REQUIRE`, `POSTCODE_*`; add CPU postcodes
- `src/kernel/smp/ap_trampoline.asm` + `src/kernel/smp/smp.c` -- AP startup path; add `ap_cpu_harden()` call
- `include/kernel/cpuid.h` -- `cpuid_init()`, `cpu_has()`; already called in Phase 0
- `src/kernel/smp/smp.c` -- `wrmsr`/`rdmsr` inline helpers; input to MSR layer (→ XREF `02-kernel-core/TODO-09-x86-64-architecture.md §5`)
- `src/kernel/mm/vmm.c` -- VMM init; EFER.NXE must be set before first page table write with NX bit
- `src/kernel/cpu_security.c` -- `cpu_harden()`, `cpu_harden_post_pagetable()`; `hv_supports_cr4_smep_smap()` calls `platform_detect()` (SMEP/SMAP gate)
- `src/kernel/cpuid_platform.c` -- `platform_detect()`; populates `g_boot_info.hv_vendor` / `hv_flags`
- `src/kernel/timer.c` -- `timer_hal_init()`; calls `platform_detect()` before timer backend selection
- `include/kernel/msr.h` -- MSR addresses (IA32_EFER, IA32_PAT); CR4 bit definitions; `MSR_IA32_MISC_ENABLE` to be added by §9
- → XREF: `02-kernel-core/TODO-01-kernel-init-sequencing.md §2` -- `boot_phase0` sequence; CPU hardening steps slot in here
- → XREF: `02-kernel-core/TODO-10-kernel-security-hardening.md §1--§8` -- NX, SMEP, SMAP, PCID, Spectre, CET (implementation details)
- → XREF: `02-kernel-core/TODO-09-x86-64-architecture.md §1, §5--§10, §3, §13` -- XSAVE, MSR layer, UMIP, 1 GiB pages, LKGS, topology, errata, VM detection
- → XREF: `01-boot-platform/TODO-11-interrupt-timer-arch.md §1` -- UTS timer driver selection; hypervisor detection (§5 here) feeds it
- → XREF: `04-drivers-hardware/TODO-04-security-hardware.md §1` -- SMEP+SMAP implementation (CR4 enable + copy_from/to_user wrappers); this TODO owns boot sequencing and AP consistency, `02-kernel-core/TODO-10-kernel-security-hardening.md` owns the actual CR4 hardening policy
- → XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §11` -- UC MMIO; PAT layout must match §8 here
- → XREF: `02-kernel-core/TODO-08-time-filetime-management.md §4,§1` -- monotonic clock + invariant TSC calibration consume stable timer choice from §1
- → XREF: `02-kernel-core/TODO-14-registry-completion.md` -- §8 boot hive lifecycle enables `HKLM\HARDWARE\*` registry keys when the hardware hive writer exists

---

## Outcome

- All CPU security features (EFER.NXE, CR4 flags) are provably activated before VMM maps any page with NX bits.
- Every AP runs `ap_cpu_harden()` at startup -- no AP boots with SMEP/SMAP/EFER absent while BSP has them.
- AP feature consistency is validated: mismatched CPUID feature sets between BSP and AP trigger a controlled bug-check (`MULTIPROCESSOR_CONFIGURATION_NOT_SUPPORTED`) or graceful degradation to the lowest common feature set.
- CR4 safety bits (SMEP, SMAP, UMIP, CET, FSGSBASE) are pinned after boot -- any post-boot attempt to clear them panics with `CRITICAL_STRUCTURE_CORRUPTION`.
- Each AP synchronizes its PAT MSR to match BSP cache policy -- prevents WB-vs-UC mismatch on shared MMIO pages.
- Hypervisor vendor is detected and stored in `boot_info` before the UTS probes its timer drivers, enabling correct Hyper-V TSC and TLB enlightenments from the start; optional Registry mirror under `HKLM\HARDWARE\VM\*` once the registry engine exposes early boot writes.
- MTRR fixed/variable ranges and `MTRRdefType` are consistent across BSP and APs where firmware left gaps (parity with Linux MTRR sync warnings).
- XSAVE and PCID are activated only after VMM is ready, in the correct Phase 1 window.
- A per-CPU register audit trail logs EFER, CR4, XCR0, and PAT values with postcodes: `[Phase0] CPU0: EFER=0xD01 CR4=0x3406E0 XCR0=0x7 PAT=0x0007040600070406`.

---

## Implementation Order

| ⭐   | Order | Deliverable                                  | Depends On               | Status |
| --- | :---: | -------------------------------------------- | ------------------------ | :----: |
| 💎   |   1   | CPUID detection & per-CPU capability capture | D2/T01 §1                |  [x]   |
| 💎   |   2   | Phase 0 CPU security activation order        | §1, D2/T10 §1, D2/T01 §2 |  [x]   |
| ⭐   |   3   | Hypervisor detection before timer selection  | §1, D2/T01 §12           |  [x]   |
| 💎   |   4   | AP CPU hardening (`ap_cpu_harden()`)         | §2, D2/T10 §1            |  [x]   |
| 💎   |   5   | Phase 1 XSAVE & PCID activation window       | §2, D2/T24 §4, D2/T01 §1 |  [x]   |
| 💎   |   6   | AP feature consistency validation            | §1, §4                   |  [/]   |
| 💎   |   7   | CR4 safety-bit pinning                       | §2, §5                   |  [x]   |
| 💎   |   8   | MTRR/PAT AP synchronization                  | §4                       |  [x]   |
| ⭐   |   9   | CPU register state audit trail               | §2, §5                   |  [x]   |
| 💎   |  10   | AP bringup hardening & robustness            | §4, §6                   |  [x]   |
| 💎   | 11 | Post-ship follow-up backfill (2026-07-31 cohort) | -- | [ ] |

> 💎 = parity -- Windows and Linux both enforce EFER/CR4 ordering, AP parity, feature consistency, CR4 pinning, and PAT synchronization; Impossible OS must match that contract.
> ⭐ = exclusive -- hypervisor pre-detection before timer HAL selection, per-activation postcode audit trail, and structured register dump are not surfaced the same way on Windows or Linux.

---

## 1. CPUID Detection & Per-CPU Capability Capture

**Prompt:** `cpuid_init()` already runs in Phase 0 and populates feature flags, but the AMD extended leaves (`0x80000001`, `0x8000001E`, `0x80000008`) and per-CPU storage in `struct cpu_data` are not complete. Extend `cpuid_init()` to probe all leaves needed by TODO-24 and TODO-01: SSE2, SSE4.2, AVX, AVX2, AVX512F, AES-NI, RDRAND, RDSEED, CET_SS, CET_IBT, UMIP, SMEP, SMAP, PCID, INVPCID, FSGSBASE, FFXSR; AMD leaves 0x80000001 (1-GB pages, RDTSCP, PDPE1GB, SVM), 0x8000001E (Zen topology), 0x80000008 (phys/virt address bits); store result in `cpu_data[0].cpuid_features`; emit `[Phase0] CPUID probed, POSTCODE_CPUID_DONE` to `boot_progress()`.

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-09-x86-64-architecture.md §1` -- full feature struct definition lives there; this TODO only gates the call into Phase 0.

- [x] `cpuid_init()` runs in Phase 0 after heap_init -- confirmed in `boot_hw.c` ~259--263
- [x] AMD extended leaves 0x80000001 (NX, SVM, OSVW, IBS, Page1GB, RDTSCP), 0x8000001E (Zen topology), 0x80000008 (phys/linear addr bits) all parsed
- [x] Result stored in global `g_cpu` struct (type `struct cpu_features`) -- 54 feature flags + address bits + topology
- [x] `POSTCODE_CPUID_INIT = 0x24` already defined; `boot_progress(0, "CPUID", POSTCODE_CPUID_INIT)` emitted
- [x] Already implemented -- marking complete
- [x] Commit: "(shipped) CPUID extended leaves + POSTCODE_CPUID_INIT in Phase 0"

**Test checkpoint:** Serial or POST shows CPUID stage (`POSTCODE_CPUID_INIT` / `boot_progress` "CPUID"); `g_cpu` has NX and AMD extended topology bits on AMD hosts; homogeneous check on QEMU WHPX, TCG, VirtualBox, bare metal.

> **Notes:**
> - What shipped: `cpuid_init()` at `src/kernel/cpuid.c:187-233` parses Intel + AMD leaves (`0x80000001` NX/Page1GB/RDTSCP/SVM, `0x80000008` addr bits, `0x8000001E` Zen topology); 54+ feature flags in `g_cpu` (`struct cpu_features`).
> - How it integrates: `boot_phase0()` calls `cpuid_init()` after `heap_init` (`boot_hw.c:332`); `cpu_has(CPU_FEATURE_*)` consumers read `g_cpu` cache; `boot_progress(0, "CPUID", POST16_CPUID_OK)` emits serial line.
> - Downstream effects: unblocks every subsequent `cpu_has()` gate; consumers in `cpu_security.c`, `vmm.c`, `simd_enable_avx()`.
> - Canonical doc: `include/kernel/cpuid.h` (struct definition + feature constants).
> - Scope boundary: §1 owns the Phase 0 probe + storage. Full feature-struct definition + AP per-CPU storage lives in [`02-kernel-core/TODO-09 §1`](../02-kernel-core/TODO-09-x86-64-architecture.md).

> **Verified:** 2026-04-12 -- all 6 items confirmed. `cpuid_init()` at `boot_hw.c:332`. Extended leaves 0x80000001/0x80000008/0x8000001E at `cpuid.c:187-233`, guarded by `max_ext_leaf`. 54+ feature flags in `g_cpu` (`struct cpu_features`). `boot_progress(0, "CPUID", POST16_CPUID_OK)` at `boot_hw.c:334`. Codex adversarial: `cpuid_raw` NULL-pointer rejected (internal helper, all callers pass stack locals, Gate 5: validate at system boundaries only). Accepted: none.
> **Quality reviewed:** 2026-04-12 -- kernel-code-quality 11 gates walked. Feature flags match CPUID register bits consistently. O(1) one-time Phase 0 call. Intel SDM Vol. 2A compliant (leaf bounds). Parity: matches Windows NtQuerySystemInformation + Linux /proc/cpuinfo. Accepted: none.
> **Verified:** 2026-05-20 | commit `00f9429d` | 6/6 items | build OK | lint CLEAN
> **Quality reviewed:** 2026-05-20 | Codex 3x (adversarial, consistency, perf) | 3M fixed | scope: kernel-code-quality

---

## 2. Phase 0 CPU Security Activation Order

Document and enforce the required activation sequence in `boot_phase0()`. The MSR layer is `msr_read`/`msr_write` inline statics in `msr.h` (no separate `_init` phase). EFER hardening (`cpu_enable_nx`) sets `EFER.NXE` before VMM walks page tables. CR4 hardening splits across two windows: NX/UMIP/PKU/PAT pre-VMM via `cpu_harden()`, then SMEP/SMAP post-pagetable via `cpu_harden_post_pagetable()` once kernel PTEs no longer carry the User bit. Each window emits a consolidated `[Phase0] CPU security <phase>: EFER=... CR4=... NX=N UMIP=N PKU=N SMEP=N SMAP=N` log line. `POSTCODE_CPU_HARDEN_DONE` (alias for `POST16_CPU_HARDEN_OK`) marks completion of the pre-VMM window.

> [!IMPORTANT]
> -> XREF: `02-kernel-core/TODO-10-kernel-security-hardening.md §1, §2` -- NX (cpu_enable_nx + EFER.NXE) shipped; SMEP/SMAP exist but gated off until kernel PTE User-bit policy is fixed.
> -> XREF: `02-kernel-core/TODO-01-kernel-init-sequencing.md §2` -- Phase 0 boot sequence; this step slots between heap and VMM NX policy.
> **Stale-prereq note:** earlier drafts referenced `cpu_efer_harden()` / `cpu_cr4_harden()` / `msr_init()` / `SUBSYS_MSR` as upstream prerequisites; those names never landed and are not needed. Actual symbols: `cpu_enable_nx`/`_smep`/`_smap`/`_umip`/`_pku` + umbrella `cpu_harden()` / `cpu_harden_post_pagetable()`. `msr_read`/`msr_write` are inline statics in `msr.h:16/23` with no init phase.

- [x] Add `POSTCODE_CPU_HARDEN_DONE` alias for `POST16_CPU_HARDEN_OK` in `boot_init.h` (no double-emit, same constant)
- [x] `boot_phase0()` already wraps `cpu_harden()` with POST16_CPU_HARDEN + POSTCODE_CPU_HARDEN_DONE at `boot_hw.c:555-558`
- [x] Implement `cpu_security_log_state(const char *phase_label)` in `cpu_security.c` reading EFER/CR4 + cpu_has() for NX/UMIP/PKU/SMEP/SMAP (BSP-only; helpers also run on APs)
- [x] Wire `cpu_security_log_state("pre-VMM")` after `cpu_harden()` and before `vmm_apply_nx_policy()` in `boot_phase0()`
- [x] Wire `cpu_security_log_state("post-pagetable")` after `cpu_harden_post_pagetable()` to show the deferred SMEP/SMAP state
- [x] Verify (smoke test): two `[Phase0] CPU security ...` lines on serial; pre-VMM == post-pagetable today (SMEP/SMAP gated off), will diverge once TODO-10 §2 unblocks
- [x] Commit: `"boot: Phase 0 CPU security activation-state summary lines + POSTCODE_CPU_HARDEN_DONE alias"`

**Test checkpoint:** `bash scripts/test-smoke.sh` shows `[Phase0] CPU security pre-VMM:` line before any `[vmm]` activity, and `[Phase0] CPU security post-pagetable:` line after `[vmm] NX policy applied`. Both lines decode EFER + CR4 + per-feature status. `POSTCODE_CPU_HARDEN_DONE` visible on POST display. Verify on QEMU WHPX, TCG, VirtualBox, bare metal -- bare metal SMEP/SMAP values flip to 1 once `02-kernel-core/TODO-10 §2` lands.

> **Test runner:** `scripts\debug\kernel\run-x86-tests.bat` (SUITE=x86) | 1 added suite ("CPU security: activation-state helper callable"), 0 failures
>
> **Notes:**
> - What shipped: `cpu_security_log_state()` helper + two BSP-only call sites in `boot_hw.c` + `POSTCODE_CPU_HARDEN_DONE` alias.
> - How it runs: BSP-only `cpu_security_log_state("pre-VMM")` after `cpu_harden()`, then `cpu_security_log_state("post-pagetable")` after `cpu_harden_post_pagetable()`. Helpers stay BSP-context-agnostic so AP path emits no false evidence.
> - Downstream effects: serial log gains two structured lines diagnosticians can grep; divergence between them signals when CR4 hardening completes once TODO-10 §2 unblocks SMEP/SMAP.
> - Canonical doc: `include/kernel/cpu_security.h` (`cpu_security_log_state` prototype + intent comment).
> - Scope boundary: §2 owns the activation-order LOG line only; NX/SMEP/SMAP/UMIP/PKU/PAT enable code is in `02-kernel-core/TODO-10 §1/§2/§5`. CR4-bit pinning + full per-CPU audit live in §7 + §9.

> **Verified:** 2026-05-20 | commit `77fce853` | 7/7 items | build OK | smoke PASS (TCG 2.64s)
> **Quality reviewed:** 2026-05-20 | Codex 4x (design, adversarial, consistency, perf) | 1M fixed | scope: kernel-code-quality

---

## 3. Hypervisor Detection Before Timer Selection

The UTS probe in `TODO-11-interrupt-timer-arch.md` §1 selects HPET vs PIT vs LAPIC timer, but on Hyper-V the correct choice is the `HV_X64_MSR_TIME_REF_COUNT` reference counter (Microsoft TLFS: partition reference counter MSR `0x40000020`). Hypervisor detection must run before UTS backend selection: **`timer_hal_init()`** calls `platform_detect()` first (`src/kernel/timer.c:104`). **`platform_detect()` may also run earlier** from `cpu_enable_smep()` via `hv_supports_cr4_smep_smap()` (`src/kernel/cpu_security.c:56`) during `cpu_harden_post_pagetable()` -- side effect for SMEP gating, not a substitute for the timer path. Probe CPUID `0x40000000` / vendor leaves; store `hv_vendor` + `hv_flags` in `boot_info`; for `"Microsoft Hv"`: set `HV_FLAG_TSC_ENLIGHTENMENT | HV_FLAG_TLBFLUSH_HYPERCALL` (and related flags already in `cpuid_platform.c`); for KVM/VMware/VBox: existing branches in `platform_detect()`. Emit `[Phase0] Hypervisor: ...` style klog from `platform_detect()` where applicable.

> [!IMPORTANT]
> → XREF: `01-boot-platform/TODO-11-interrupt-timer-arch.md §1` -- UTS reads `boot_info.hv_flags` to select clock; must be set before `timer_probe()`.
> → XREF: `02-kernel-core/TODO-09-x86-64-architecture.md §13` -- AMD-V / VT-x host capability detection; Hyper-V guest enlightenment MSR initialization (using `boot_info.hv_flags` populated by this step) is not yet specced in §13 and needs to be added there.
> → XREF: `02-kernel-core/TODO-14-registry-completion.md` -- mirror `hv_vendor` / `hv_flags` to `HKLM\HARDWARE\VM\HypervisorVendor` (and related values) when early-boot registry writes are supported.

- [x] Add `hv_vendor[16]` and `hv_flags` fields to `struct boot_info`
- [x] Define `HV_FLAG_TSC_ENLIGHTENMENT`, `HV_FLAG_TLBFLUSH_HYPERCALL`, `HV_FLAG_KVM_STEAL_TIME`, `HV_FLAG_APIC_FREQ_MSR`, `HV_FLAG_VMWARE_BACKDOOR` in `boot_init.h`
- [x] `platform_detect()` in `cpuid_platform.c` populates `g_boot_info.hv_vendor` + `g_boot_info.hv_flags` for Hyper-V, KVM, VMware, VirtualBox
- [x] Detection runs from `timer_hal_init()` → `platform_detect()` in Phase 1, before timer backend selection
- [x] UTS timer probe gates the Hyper-V MSR read on `HV_FLAG_APIC_FREQ_MSR` and uses `msr_try_read()` (`cal_try_hyperv_msr` in `lapic.c`)
- [/] Registry mirror deferred -- persist `hv_vendor` + `hv_flags` under `HKLM\HARDWARE\VM\` when `TODO-14-registry-completion.md` exposes pre-desktop hardware-hive writes
- [/] Hyper-V SynIC + reference TSC page MSR setup NOT owned here -- XREF `02-kernel-core/TODO-09-x86-64-architecture.md §13` (init `HV_X64_MSR_REFERENCE_TSC = 0x40000021`) and `01-boot-platform/TODO-11-interrupt-timer-arch.md §6` (UTS consumer)
- [/] `boot_info.cc_kind` bootloader mirror (needs a BOOT_INFO_VERSION bump) for downstream cache-type sequencing; kernel-side TDX/SEV detection owned by `02-kernel-core/TODO-09-x86-64-architecture.md §15` (`g_cpu.cc_kind`)
- [x] Commit: `"boot: hypervisor detection with hv_flags in boot_info"`

**Test checkpoint:** `g_boot_info.hv_vendor` / `hv_flags` populated before timer backend selection; klog or serial shows hypervisor detection before first `[UTS]` / `[Timer]` line on Hyper-V and KVM guests; bare metal shows empty vendor or known non-HV path. QEMU WHPX, TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-x86-tests.bat` (SUITE=x86) | 4 added suites (Platform hv_vendor NUL, hv_flags in range, APIC freq flag/func consistent on Hyper-V, bare metal clean); 2 pass on KVM, 2 platform-skip.
>
> **Notes:**
> - What shipped: `hv_vendor[16]` + `hv_flags` fields in `struct boot_info`; 5 `HV_FLAG_*` constants; `platform_detect()` populates them for Hyper-V / KVM / VMware / VBox, each gated on its own CPUID privilege/feature leaf.
> - How it integrates: `timer_hal_init()` calls `platform_detect()` before backend selection; `cal_try_hyperv_msr` reads `HV_FLAG_APIC_FREQ_MSR` + uses `msr_try_read()`; `platform_has_apic_freq_msr()` mirrors the same flag.
> - Downstream effects: feeds UTS timer probe ([TODO-11 §1](TODO-11-interrupt-timer-arch.md)); a constrained Hyper-V partition without `AccessFrequencyMsrs` now falls through to the next calibration tier instead of #GP-crashing.
> - Canonical doc: `src/kernel/cpuid_platform.c`.
> - Scope boundary: §3 owns `boot_info` fields + `platform_detect()` call sites. Registry mirror + TLFS TSC reference page deferred to TODO-14 + TODO-09 owners.

> **Verified:** 2026-05-23 | commit `4ed1f6f1` | 6/6 owned items + 3 XREF-deferred | build OK | smoke PASS (KVM 2.51s)
> **Quality reviewed:** 2026-05-23 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1H+2M fixed | scope: kernel-code-quality + boot-code-quality

---

## 4. AP CPU Hardening (`ap_cpu_harden()`)
**Prompt:** When APs start up via `ap_entry()` (the planning-era `smp_ap_main` name), they run their own GDT/IDT/LAPIC setup but historically replicated only a bare `cpu_harden()` + `cpu_harden_post_pagetable()` and an ad-hoc TSC_AUX write. An AP running without `EFER.NXE`/`SMEP`/`SMAP`, or with the wrong PAT cache type on shared MMIO, is a privilege-bypass / corruption vector. The replication contract is a per-CPU MSR profile (registry of MSRs the BSP programmed in Phase 0/1 that every AP mirrors), not EFER+CR4 alone -- otherwise each new boot-time MSR (TSC_AUX, SPEC_CTRL, CET) silently drifts on APs.

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-10-kernel-security-hardening.md §1--§8` -- TODO-10 provides EFER / SPEC_CTRL / CET setters reused on APs; this TODO writes the call site. SPEC_CTRL/CET MSR-profile entries are appended when those setters ship.
> → XREF: `02-kernel-core/TODO-09-x86-64-architecture.md §11` -- `IA32_TSC_AUX` per-CPU programming, consumed via the profile registry.
> → XREF: `02-kernel-core/TODO-09-x86-64-architecture.md §1, §5` -- `ap_apply_xcr0()` here resolves the per-AP XCR0 configuration those sections deferred (their `Accepted: AP XCR0` notes); the formal Phase 1 XSAVE/PCID window remains §5 below.

- [x] 7 boot-snapshot fields (EFER/CR4/PAT/XCR0/SPEC_CTRL/TSC_AUX/applied-count) added to `per_cpu_data` after the asm-pinned KPTI offsets; BSP fills `cpu_data[0]`, each AP fills its own
- [x] `s_bsp_msr_profile[]` in `cpu_security.c` -- `{msr,value,name,feature,per_cpu}`: PAT verbatim + TSC_AUX per-CPU; `cpu_record_bsp_profile()` freezes values from live BSP MSRs pre-bringup; SPEC_CTRL/CET added when TODO-10 setters ship
- [x] `ap_cpu_harden(cpu_id)`: match BSP XCR0 (AP-local) -> gated `cpu_harden()` -> replay MSR profile -> buffer snapshot silently; BSP `ap_cpu_harden_log()` verifies + audits after bringup. EFER NXE-only (no SCE), no CR4 force
- [x] `ap_cpu_harden(cpu_index)` wired into `ap_entry()` (replaces bare `cpu_harden` + TSC_AUX block); `cpu_record_bsp_profile()` at `smp_init()` entry so the baseline is the final post-Phase-1 XCR0
- [x] Verify (serial log): `[BSP] hardening baseline ...` then per-AP `[AP%u] CPU hardening applied EFER/CR4/PAT/XCR0/MSRs`; warn-only on EFER.NXE/required-CR4/PAT mismatch (bug-check on mismatch is §6)
- [x] Commit: `"smp: ap_cpu_harden() with per-CPU MSR profile registry"`

**Test checkpoint:** Boot on SMP system (2+ CPUs). After SMP bringup the BSP emits, per online AP, `AP %u online (LAPIC ID=...)` followed by `[AP%u] CPU hardening applied EFER=0x... CR4=0x... PAT=0x... XCR0=0x...` (the AP itself is serial-free from GS-base to `sti`, so these are BSP-emitted from the buffered `per_cpu_data`, NOT before the AP is marked online). Verify EFER.NXE/PAT match BSP. On single-CPU system: no AP messages, BSP-only boot succeeds and `[BSP] hardening baseline` still logs. Verify on QEMU WHPX (SMP), TCG (1 CPU), VirtualBox (4 CPUs), bare metal. Bare metal: confirm AP does not crash between GDT load and `ap_cpu_harden()` call -- this window is IRQ-disabled but NMI-vulnerable.

> **Test runner:** `scripts\debug\kernel\run-x86-tests.bat` (SUITE=x86) | 6 added suites (MSR profile populated/bounds/PAT-verbatim/TSC_AUX-per-CPU, BSP PAT baseline, BSP snapshot recorded), 0 failures. AP-path replay validated via serial log on multi-CPU platforms (unit tests cannot call the live AP hardening path).
>
> **Notes:**
> - What shipped: `ap_cpu_harden()` + `cpu_record_bsp_profile()` + `s_bsp_msr_profile[]` (PAT verbatim, TSC_AUX per-CPU) in `cpu_security.c`; 7 boot-snapshot fields in `per_cpu_data`; `smp.c` AP path now calls the single contract function.
> - How it runs: BSP freezes its EFER/CR4/PAT/XCR0 baseline at `smp_init()` entry; each AP matches XCR0 via AP-local CPUID leaf 0x0D, replays the MSR profile, buffers its snapshot; the BSP verifies + audits after bringup. Only the XCR0 mask + snapshot reads are AP-local; the `cpu_enable_*`/TSC_AUX enable gates still use BSP-global `cpu_has()` (deferred to §6).
> - Downstream effects: resolves the per-AP XCR0 gap deferred by `02-kernel-core/TODO-09 §2/§3/§5`; provides the `ap_cpu_harden()` SMEP/SMAP call site `03-memory-concurrency/TODO-02` names; feeds §9 audit fields.
> - Canonical doc: `include/kernel/cpu_security.h` (`ap_cpu_harden` / `cpu_record_bsp_profile` prototypes).
> - Scope boundary: §4 owns AP replication + warn-only verify. CR4 force-after-validation + degraded-bringup robustness are §10; the AP-mismatch bug-check is §6; the Phase 1 XSAVE/PCID window is §5; SPEC_CTRL/CET MSR values owned by `02-kernel-core/TODO-10 §8--§10`.

> **Verified:** 2026-05-24 | commit `dd595e8a` | 6/6 items | build OK | smoke PASS (KVM 2.72s)
> **Deferred:** [H] AP-side `cpu_enable_*` + TSC_AUX enable gates use BSP-global `cpu_has()` (pre-existing; no #GP on homogeneous HW) -> XREF: 01-boot-platform/TODO-09 §10 (item: "AP-local gate `ap_cpu_harden()` enables: `cpu_enable_umip/pku/smep/smap` + TSC_AUX")
> **Deferred:** [H] sparse-slot dense `smp_cpu_count()` after a partial AP timeout (pre-existing; AP hardening unaffected) -> XREF: 01-boot-platform/TODO-09 §10 (item: "Sparse-slot accounting: `smp_cpu_count()` is a dense online count")
> **Deferred:** [M] timed-out AP can go live-but-uncounted (pre-existing degraded bringup) -> XREF: 01-boot-platform/TODO-09 §10 (item: "Degraded-bringup: a timed-out AP still completes `ap_entry()`")
> **Quality reviewed:** 2026-05-24 | Codex ~20x (adversarial, consistency, perf, re-adversarial) converged | AP serial + online-publication hardened to a single release/acquire `is_online` signal; 5H+6M+1L fixed, 2H+2M deferred-XREF | scope: kernel-code-quality

---

## 5. Phase 1 XSAVE & PCID Activation Window
**Prompt:** Activate XSAVE and PCID in the correct boot window relative to page-table setup, with POST codes and verification logging. Code-truth: the XCR0 base mask is already programmed in Phase 0 by `cpu_configure_xcr0()` (from `cpuid_init`), and that is correct -- Phase-0 `simd_enable_avx()` and `pku_init()` require XCR0, and per-thread XSAVE areas come from `pmm_alloc_contiguous()` (not TEB/VMM pages), so XSAVE has no real VMM dependency to wait on. This section adds a Phase 1 `cpu_xsave_enable()` *finalize* (records the final XCR0 mask + `xsave_size_max` and logs them; runs after `simd_enable_avx512()`'s throttle guard so the logged mask is final; does NOT re-run `XSETBV`, which would re-enable any AVX-512 xstate the throttle guard cleared), plus a `cpu_pcid_enable()` that sets the bare `CR4.PCIDE` bit (PCID stays 0 everywhere -> legacy TLB behavior) now that page tables exist. Both are no-ops on CPUs lacking the feature; `cpu_pcid_enable()` gates on the calling CPU's own CPUID so it is safe on a feature-skewed AP.

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-09-x86-64-architecture.md §1` -- `cpu_xsave_enable()` finalize builds on `cpu_configure_xcr0()` (XSAVE area sizing, XCR0 bits) which §1 ships.
> → XREF: `02-kernel-core/TODO-10-kernel-security-hardening.md §7` -- per-process PCID tagging + NOFLUSH CR3. This section ships ONLY the bare `CR4.PCIDE` activation; §7 owns the TLB-tagging exploitation and must NOT double-enable the CR4 bit (already set here).
> → XREF: `02-kernel-core/TODO-10-kernel-security-hardening.md §9-§10` -- CET xstate is **supervisor** state managed via `IA32_XSS` + `XSAVES` (CET_U component 11, CET_S component 12), NOT `XCR0`. CET state reservation is owned by §9 where CET is enabled; this section does not touch `XCR0` CET bits because setting reserved `XCR0` bits would `#GP`.
> → XREF: `02-kernel-core/TODO-11-peb-teb-user-abi.md §1` -- TEB allocation at thread create (per-thread xstate consumer context).

- [x] Added `POSTCODE_XSAVE_ENABLED` (0x42) / `POSTCODE_PCID_ENABLED` (0x43) + `SUBSYS_XSAVE`/`SUBSYS_PCID` (before `SUBSYS_COUNT`) with matching `s_subsys_names[]` entries (Gate-3 parallel-array sync)
- [x] `cpu_xsave_enable()` finalize in `boot_phase1()` after `simd_enable_avx512()`; records `xsave_size_max` + live XCR0, logs `[Phase1] XSAVE enabled (...)`, no re-XSETBV (BOOT_OK enabled / BOOT_DEGRADED unsupported)
- [x] `cpu_pcid_enable()` in `boot_phase1()` (real dep is Phase-0 VMM page-table base, not the process table); asserts `CR3[11:0]==0`, sets `CR4.PCIDE`, logs `[Phase1] PCID enabled`; replicated on APs in `ap_cpu_harden()`
- [x] CET xstate reservation delegated to `02-kernel-core/TODO-10 §9` (CET state is `IA32_XSS` supervisor state, not `XCR0`); reciprocal item filed there; AP XCR0 already handled by `ap_apply_xcr0()`
- [x] Verify (serial log): BSP emits both lines from `boot_phase1`; AP copies HARDEN_KLOG-suppressed pre-online, audited by `ap_cpu_harden_log()`
- [x] XCR0 base activation correctly stays in Phase 0 (SIMD + PKU require it; XSAVE areas are PMM, not TEB); Phase 1 only finalizes. Corrects the original "XSAVE must not run in Phase 0" premise
- [x] Commit: `"boot: activate XSAVE and PCID in Phase 1 after VMM ready"`

**Test checkpoint:** Serial log shows `[Phase1] XSAVE enabled (area=N bytes, mask=0x...)` and `[Phase1] PCID enabled` from `boot_phase1` (after `[VMM]` from Phase 0). On CPUs without XSAVE: `[Phase1] XSAVE: not supported, skipped`. Unit tests assert the postconditions: `cpu_has(XSAVE)` implies `CR4.OSXSAVE`, `cpu_has(PCID)` implies `CR4.PCIDE`, and `CR3[11:0]==0` (PCID stays 0). Verify on QEMU WHPX, TCG (XSAVE/PCID may be absent), VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-x86-tests.bat` (SUITE=x86) | 72 suites, 0 failures

> **Notes:**
> - Shipped `cpu_xsave_enable()` (BSP finalize, no re-XSETBV) + `cpu_pcid_enable()` (bare `CR4.PCIDE`, PCID 0) in `cpu_security.c`; called in `boot_phase1` after the AVX-512 throttle guard with explicit `boot_result_t` handling (BOOT_OK enabled -> POSTCODE; BOOT_DEGRADED unsupported -> ready, no marker; BOOT_FATAL CR3-invariant -> not-ready + degraded_mask); 2 new POST codes + 2 new `SUBSYS_*` slots.
> - PCID replicated on APs via `cpu_pcid_enable()` inside `ap_cpu_harden()`'s serial-quiet bracket (AP-local CPUID gate, no #GP on feature-skewed AP); deferral comment updated to drop PCIDE from the "do-not-force" set.
> - `02-kernel-core/TODO-10 §7` now only adds per-process PCID tagging + NOFLUSH CR3 on top of the already-set `CR4.PCIDE` (must not re-enable). Codex design + adversarial adoptions in the commit message.
> - Canonical doc: this section + `src/kernel/cpu_security.c` `cpu_xsave_enable`/`cpu_pcid_enable`.
> - Scope boundary: §5 owns XSAVE finalize + bare `CR4.PCIDE` activation. TODO-10 §7 owns PCID exploitation (tagging/NOFLUSH); TODO-10 §9 owns CET xstate (`IA32_XSS`); §7 (this TODO) owns CR4 pinning.
> **Verified:** 2026-05-24 | commit `b6ab5093` | 6/6 items | build OK | smoke PASS (KVM 2.77s); both `[Phase1]` lines on serial
> **Quality reviewed:** 2026-05-24 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 3M+1L fixed, 0 open | scope: kernel-code-quality

---

## 6. AP Feature Consistency Validation

Windows triggers bug-check `MULTIPROCESSOR_CONFIGURATION_NOT_SUPPORTED` (0x3E) when an AP's CPUID feature set is incompatible with the BSP. Linux runs `verify_cpu.S` on each AP during trampoline entry to ensure Long Mode and SSE. Impossible OS currently has no equivalent -- an AP with different capabilities could silently cause undefined behavior.

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-09-x86-64-architecture.md §9` -- CPU topology parsing; topology differences (core count, cache layout) are informational, not a failure. This section only validates *security-critical* feature mismatches. §6 publishes `per_cpu_data.core_type`; §9 consumes it for accurate hybrid P/E masks (concrete item filed there).

- [x] `CPU_FEATURES_REQUIRED_MASK` in `cpuid.h` = NX|SSE2|LM|SYSCALL (PAE/PGE/LAHF/CMPXCHG16B are long-mode prerequisites, never reach kernel C, so not tracked; LM is the verify_cpu.S-style guard)
- [x] Two-phase ordering: each AP publishes its mask into `cpu_data[ap_id].features` before `cpu_harden()`'s optional CR4 enables; global intersection reduced BSP-side by `cpu_features_finalize_global()` after the `is_online` pass
- [x] Hybrid: `cpu_validate_ap_features()` reads `CPUID.1A` core type into `cpu_data[ap_id].core_type`; optional-mismatch check is within `CPU_FEATURES_AP_PROBE_MASK` so E-core gaps (AVX-512) degrade, not panic
- [x] `cpu_validate_ap_features(cpu_id)` in `cpu_security.c` (after `ap_apply_xcr0()`, before `cpu_harden()`); `cpuid_probe_ap_features()` in cpuid.c does the AP-local probe; bug-checks on missing required / vendor / Long Mode
- [x] On optional mismatch: `cpu_data[ap_id].feature_mismatch = 1` on the AP (no serial); BSP logs `[AP%u] FEATURE MISMATCH: ...` via `ap_cpu_harden_log()`; global intersection prevents kernel-wide reliance
- [x] `BUGCHECK_MULTIPROCESSOR_CONFIGURATION_NOT_SUPPORTED = 0x3E` in `bugcheck.h`; `KeBugCheckEx` on missing required mask / vendor mismatch / missing Long Mode
- [x] `POSTCODE_AP_VALIDATE = 0x27` added. POST16(0xD600/0xD601) intentionally NOT used: it draws the FB corner (`fb_fill_rect` -> AVX-512) which #UDs on an AP before XCR0/AVX is enabled + races the shared FB; logged BSP-side instead
- [x] TSC sync between APs NOT owned here -- relies on `02-kernel-core/TODO-08-time-filetime-management.md §3` "Per-CPU TSC sync"
- [x] Verify (serial log): homogeneous shows `[AP%u] Feature validation OK`; BSP logs the global intersection mask; hybrid mismatches logged BSP-side
- [x] Commit: `"smp: AP feature intersection + hybrid-aware consistency validation"`

**Test checkpoint:** Boot on SMP system. Log shows `[AP1] Feature validation OK (core_type 0x..)` for every AP + `Global CPU feature intersection 0x..`. No degradation on homogeneous hardware. Unit tests assert the mask invariants (BSP satisfies required, required subset of probe, global intersection retains required). Bare metal: P/E hybrid CPUs may show feature differences (AVX-512) that degrade gracefully, not panic.

> **Test runner:** `scripts\debug\kernel\run-x86-tests.bat` (SUITE=x86) | 75 suites, 0 failures

> **Notes:**
> - Shipped `cpu_validate_ap_features()` + `cpu_features_finalize_global()` + `cpu_feature_global_mask()` (cpu_security.c), `cpuid_probe_ap_features()` (cpuid.c), masks in `cpuid.h`, 3 `per_cpu_data` fields, `BUGCHECK 0x3E`, `POSTCODE_AP_VALIDATE`.
> - AP validates after `ap_apply_xcr0()`, before `cpu_harden()`; bug-checks 0x3E on required/vendor/LM failure; optional skew flagged on AP + logged BSP-side; global intersection reduced BSP-side after the online pass.
> - Codex design review (3 findings) adoptions + a §6-introduced AP double fault (POST16 FB draw before AVX enable) root-caused and fixed; details in the commit message.
> - Canonical doc: this section + `src/kernel/cpu_security.c` `cpu_validate_ap_features`.
> - Scope boundary: §6 owns DETECTION (validate + intersection + bug-check); §10 owns AP-local CR4-enable gating; `02-kernel-core/TODO-09 §9` owns topology core_type consumption; TODO-08 §3 owns TSC sync.
> **Verified:** 2026-05-24 | commit `eeba36a1` | 9/9 items | build OK | smoke PASS (KVM 2.45s); SMP -smp 2 AP1 online + validation OK
> **Accepted:** [H] optional CR4/MSR skew (CR4.UMIP via cpu_enable_umip, TSC_AUX MSR) still BSP-global-gated in cpu_harden/ap_apply_msr_profile -> #GP on a genuinely feature-skewed AP (pre-existing from §4; not a homogeneous/real-HW case; §6 detects + excludes from the global mask) -> XREF: 01-boot-platform/TODO-09 §10 (item: "AP-local gate `ap_cpu_harden()` enables: `cpu_enable_umip/pku/smep/smap` + TSC_AUX" at line 350)
> **Accepted:** [H] a slow AP can publish is_online AFTER cpu_features_finalize_global() runs, so the global mask may omit it (pre-existing degraded-bringup race; finalize is correct over the at-the-time online set) -> XREF: 01-boot-platform/TODO-09 §10 (item: "Degraded-bringup: a timed-out AP still completes `ap_entry()` ... add a per-AP accept/abandon state" at line 279)
> **Quality reviewed:** 2026-05-24 | Codex 7x (design, adversarial, consistency, perf, re-adversarial) | 3H+2M fixed, 2H accepted-XREF | scope: kernel-code-quality

---

## 7. CR0/CR4 Safety-Bit Pinning

Linux pins CR0.WP and CR4 bits (SMEP, SMAP, UMIP, FSGSBASE, CET) after boot to prevent rootkits from disabling protection by writing the control registers. Windows HAL protects equivalent bits. Impossible OS has no post-boot protection -- a kernel exploit can trivially clear CR4.SMEP and execute user pages, OR clear CR0.WP and write through `PTE.W=0` (classic rootkit primitive for patching read-only kernel text).

- [x] Shared `include/kernel/cpu_regs.h` with `CR0_*` + `CR4_*` bit defines (single source of truth); `cpu_security.c` + `cpuid.c` use it (cpuid.c's hardcoded `(1<<18)` -> `CR4_OSXSAVE`)
- [x] Per-CPU pin masks in `per_cpu_data` (`cr0_pinned`/`cr4_pinned`), NOT a global mask: each CPU pins only the bits IT has, so a skewed AP pins fewer rather than bug-checking (design review). cr4 = live CR4 & (SMEP|SMAP|UMIP|FSGSBASE|CET|PKE) -- PKE included so a post-pin CR4.PKE clear can't silently drop PKU
- [x] `cpu_pin_control_regs()` pins the calling CPU + sets global `cr_pinning_active`; called at end of `boot_phase1` AFTER XSAVE/PCID (BSP) and at the tail of `ap_cpu_harden()` (each AP)
- [x] `BUGCHECK_CRITICAL_STRUCTURE_CORRUPTION = 0x109` (bugcheck.h + panic.c name table)
- [x] `cr0_verify_pinned()`/`cr4_verify_pinned()`: BSP bug-checks directly; an AP records the fault + halts and the BSP raises it via `cpu_cr_pin_check()` (an AP must not run the panic path -- design review)
- [x] `cr0_write_safe()`/`cr4_write_safe()`: force the calling CPU's pinned bits on when active; rollback knob clears `cr_pinning_active`. The scheduler's post-pin CR0.TS writes (`task.c` lazy-FPU) route through `cr0_write_safe` (preserves CR0.WP)
- [x] Verifiers called from: (a) top of `isr_handler` on #GP (vec 13) before dispatch, covering the unhandled-#GP panic path, (b) BSP LAPIC timer tick via `cpu_cr_pin_tick()` (dedicated hook), (c) each AP at `ap_cpu_harden()` tail
- [x] `POSTCODE_CR_PINNED = 0x39` (boot_init.h); `POST16(0xD400)/(0xD401)` around BSP pinning
- [x] Verify (serial log): `[Phase1] CR0/CR4 pinned: cr0_mask=0x10000 cr4_mask=0x800` on KVM (CR0.WP + CR4.UMIP); panic-on-clear is a manual debug-build test (KeBugCheckEx halts -- cannot run in the unit harness)
- [x] Commit: `"boot: CR0.WP + CR4 safety-bit pinning with CRITICAL_STRUCTURE_CORRUPTION panic"`

**Test checkpoint:** Boot completes with `[Phase1] CR0/CR4 pinned: cr0_mask=0x.. cr4_mask=0x..` after XSAVE/PCID activation. Unit tests assert the read-only invariants (CR0.WP pinned + set; all pinned CR0/CR4 bits remain set). Manual debug-build test: a raw `write_cr0(cr0 & ~CR0_WP)` then `cr0_verify_pinned()` panics `CRITICAL_STRUCTURE_CORRUPTION`. Verify on QEMU WHPX (SMP), TCG, VirtualBox, bare metal. Bare metal: confirm XSAVE/PCID Phase 1 activation completes before pinning takes effect.

> **Test runner:** `scripts\debug\kernel\run-x86-tests.bat` (SUITE=x86) | 78 suites, 0 failures

> **Notes:**
> - Shipped shared `include/kernel/cpu_regs.h` + `cpu_pin_control_regs()`/`cr0_write_safe`/`cr4_write_safe`/`cr0_verify_pinned`/`cr4_verify_pinned`/`cpu_cr_pin_check`/`cpu_cr_pin_tick` in `cpu_security.c`; per-CPU pin masks in `per_cpu_data`; `BUGCHECK 0x109`; `POSTCODE_CR_PINNED`.
> - BSP pins at end of `boot_phase1` (after XSAVE/PCID), each AP at `ap_cpu_harden` tail; verified at the top of `isr_handler` on #GP before dispatch (idt.c), BSP timer tick (`cpu_cr_pin_tick`), AP tail. AP violations record + halt -> BSP raises 0x109 (panic path is BSP-only-safe).
> - Resolves the §4-deferred shared-CR-define + cpuid.c OSXSAVE-hardcode item; also fixed a latent AP `cpu_id`-set-too-late bug. Codex design-review adoptions in the commit message.
> - Canonical doc: this section + `include/kernel/cpu_regs.h` + `src/kernel/cpu_security.c` pinning block.
> - Scope boundary: §7 owns CR0/CR4 pinning + verify. CET pinned once TODO-10 §9-§10 enables it; AP-local CR4-enable gating stays §10; per-AP periodic verify needs an AP timer tick (today APs verify on #GP + harden tail).

> **Verified:** 2026-05-24 | commit `0f3191c5` | 9/9 items | build OK | smoke PASS (KVM); SMP -smp 2 AP1 online + CR0/CR4 pinned
> **Quality reviewed:** 2026-05-24 | Codex 7x (design, adversarial, consistency, perf, re-adversarial) | 2H+1M fixed, 1H accepted-XREF (resolved by §10 verify-IPI) | scope: kernel-code-quality

---

## 8. MTRR/PAT AP Synchronization

Both Windows and Linux synchronize the PAT (Page Attribute Table) MSR on each AP to match the BSP. The PAT MSR (`IA32_PAT`, MSR 0x277) maps PAT index values (from PTE bits PWT/PCD/PAT) to cache types (WB, UC, WC, WT, WP). If APs have different PAT MSR values than the BSP, the same PTE on different CPUs would produce different cache types -- causing silent data corruption on shared MMIO pages.

> [!NOTE]
> → XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §11` -- VMM uses `vmm_map_mmio_uc()` which relies on PAT index 3 being UC. If an AP's PAT MSR maps index 3 to WB, MMIO accessed on that AP is cached and hardware registers read stale values.

> [!NOTE]
> **PAT constant decode bug -- FIXED 2026-05-24.** `PAT_WC_VALUE` (in `cpu_security.c`, not `boot_hw.c`) was `0x0007040600010406`, putting WC at PA2 (index 2) not PA1 (index 1): `vmm_map_mmio_wc()` + `PAGE_WRITECOMBINE` (PWT-only -> index 1) silently got WT and `PAGE_NOCACHE` (PCD-only -> index 2) silently got WC. Corrected to `0x0007040600070106` (Intel default, only PA1 = 0x01 WC); a compile-time `_Static_assert` now pins PA1=WC/PA2=UC-/PA3=UC.

- [x] **PAT decode-bug fix** -- `PAT_WC_VALUE` -> `0x0007040600070106` in `cpu_security.c` (PA1=WC; PA2/PA3 Intel-default), pinned by compile-time `_Static_assert`. Fixes framebuffer WC, `PAGE_WRITECOMBINE`, `PAGE_NOCACHE`.
- [x] **MTRR AP parity audit (warn-only)** -- new `mtrr.c`/`.h` + `CPU_FEATURE_MTRR`; BSP snapshots, each AP captures, BSP WARNs on divergence. No reprogram -- MMIO correctness rides on PAT (UC wins over MTRR, SDM 11.5.2).
- [x] BSP PAT + MTRR baselines captured in `cpu_record_bsp_profile()` (`s_bsp_pat` via §4 MSR registry; `s_bsp_mtrr`), mirrored into `cpu_data[0]`.
- [x] AP PAT = SINGLE authoritative write via §4 registry replay (`ap_apply_msr_profile`); `cpu_harden()` no longer programs PAT (removed AP double-write). BSP programs PAT once in `boot_phase0` post-page-table.
- [x] AP PAT readback verify in `ap_cpu_harden_log()`: `[AP%u] PAT synced` on match, WARN on mismatch (no panic -- hypervisor-trapped PAT is a known degraded mode).
- [ ] **DEFERRED (needs SMP IPI rendezvous):** runtime PAT re-broadcast + divergent-AP MTRR reprogram (SDM 11.11.8) -- blocked on `smp_call_function()`. §8 ships warn-only audit (correctness-complete: MMIO uses PAT-UC).
- [x] Verify (serial log): `[AP%u] PAT synced` + `[AP%u] MTRR synced` (or mismatch WARN) appear for each AP.
- [x] Commit: `"smp: fix PAT WC decode bug + MTRR/PAT AP parity audit"`

**Test checkpoint:** Boot on SMP. Serial shows `mm: PAT: entry 1 = WC (0x0007040600070106)` on the BSP and `[AP%u] PAT synced` + `[AP%u] MTRR synced` for each AP. Unit tests (`SUITE=x86`): live PAT indices 1/2/3 decode WC/UC-/UC (TEST_SKIP under a PAT-trapping hypervisor); MTRR capture deterministic, var_count bounded, BSP baseline matches live. Bare metal: confirm PAT identical across BSP+APs via the §9 audit trail; any `MTRR mismatch` WARN flags firmware skew.

> **Test runner:** `scripts\debug\kernel\run-x86-tests.bat` (SUITE=x86) | 7 added suites (PAT index 0/2/3 decode, MTRR deterministic/bounded/equal/baseline) + PAT index 1 hardened, 0 failures. AP-path parity + live MSR-failure paths validated via serial log (msr_try_read-guarded, not unit-mockable).

> **Notes:**
> - Shipped: PAT decode-bug fix (`PAT_WC_VALUE`, `_Static_assert`-pinned) + new `mtrr.c`/`mtrr.h` snapshot/audit module + `CPU_FEATURE_MTRR` + per-CPU MTRR fields; `[AP%u] PAT synced`/`MTRR synced` audit lines.
> - PAT = single authoritative write/CPU: BSP in `boot_phase0`, each AP via the §4 MSR-profile replay (`cpu_harden()` no longer writes PAT). MTRR audit is warn-only (`rdmsr`, AP-local CPUID + `msr_try_read` guard).
> - Fixes 3 silent cache-type bugs (framebuffer WC, `PAGE_WRITECOMBINE`, `PAGE_NOCACHE`); confirmed live on TCG. Codex design adoptions in the commit message.
> - Canonical doc: this section + `src/kernel/mtrr.c` + `cpu_security.c` PAT/`s_bsp_mtrr` block.
> - Scope boundary: §8 owns PAT sync + MTRR parity AUDIT; divergent-AP MTRR reprogram + runtime PAT re-broadcast need an IPI rendezvous (`smp_call_function`, deferred). MMIO cache correctness owned by PAT (TODO-01 §11).

> **Verified:** 2026-05-24 | commit `27124dca` | 6/7 items | build OK | smoke PASS (KVM 2.640s); PAT WC active, MTRR audit wired
> **Deferred:** [M] runtime PAT re-broadcast + divergent-AP MTRR reprogram -- needs an all-CPU IPI rendezvous (`smp_call_function` absent); warn-only audit ships correctness-complete -> XREF: 01-boot-platform/TODO-09 §8 (item: "DEFERRED (needs SMP IPI rendezvous): runtime PAT re-broadcast + divergent-AP MTRR reprogram" at line 346)
> **Quality reviewed:** 2026-05-24 | Codex 10x (design, adversarial, consistency, perf, re-adversarial, test-coverage) | 2H+4M+1L fixed, 1M deferred | scope: kernel-code-quality

---

## 9. CPU Register State Audit Trail

Neither Windows nor Linux produces a consolidated, structured, per-CPU register dump at boot. Windows logs fragments via ETW, Linux scatters pieces across dmesg. Impossible OS can provide a single authoritative audit line per CPU that documents the complete security-relevant register state -- invaluable for debugging bare-metal boot failures and for compliance auditing.

> [!TIP]
> **Competitive edge:** A single `[CPU%u AUDIT]` line per CPU with EFER, CR0, CR4, XCR0, PAT, IA32_MISC_ENABLE, and all security feature status in a fixed parseable format is something no other OS provides. This enables automated boot verification scripts and CI regression detection.

- [x] Added `MSR_IA32_MISC_ENABLE 0x1A0` + `MSR_IA32_BIOS_SIGN_ID 0x8B` to `msr.h` (ARCH_CAPS 0x10A + SPEC_CTRL 0x48 already present).
- [x] `cpu_audit_registers(cpu_id)` in `cpu_security.c`: reads EFER/CR0/CR4/XCR0(if OSXSAVE)/PAT/MISC_ENABLE + CPUID-gated ARCH_CAPS/SPEC_CTRL + vendor-decoded microcode rev; buffers into `per_cpu_data`, no serial (safe on the quiet AP path).
- [x] CPUID is the existence gate; `msr_try_read()` is the #GP-safe net for every optional MSR. Intel-only microcode wrmsr path (gated on GenuineIntel); AMD reads patch level directly.
- [x] `cpu_audit_log(cpu_id)` emits the consolidated `[CPU%u AUDIT] EFER=.. CR0=.. CR4=.. XCR0=.. PAT=.. MISC=.. ARCH_CAPS=.. SPEC_CTRL=.. UCODE=.. NX/SMEP/SMAP/UMIP/WP/PCID/OSXSAVE=..` line (flags from captured bits = per-CPU truth).
- [x] BSP audit runs in **Phase 2** (`smp_init`, after IDT load -- NOT Phase 0, where `msr_try_read` is unavailable pre-IDT); each AP captures at the `ap_cpu_harden()` tail, BSP emits post-bringup. Single-CPU too.
- [x] Flat `per_cpu_data` fields (`cr0_at_boot`/`misc_enable`/`arch_caps`/`ucode_rev`; EFER/CR4/PAT/XCR0/SPEC_CTRL reused); exposed via `HKLM\HARDWARE\CPU\%u\Registers` (`cpu_audit_populate_registry`, Phase 2).
- [x] `cpu_audit_consistency_check()` compares each AP vs BSP (EFER.NXE, uniform CR4, PAT, XCR0, ARCH_CAPS) -> `[SMP] All %u CPUs register-consistent` or per-CPU divergence WARN.
- [x] `POSTCODE_CPU_AUDIT = 0x2A` (boot_init.h), Phase-2 `boot_progress` milestone. `POST16(0xD300/0xD301)` SKIPPED -- Phase 2 has klog up, not a pre-`sti` path.
- [x] Commit: `"boot: per-CPU register audit (EFER/CR/XCR0/PAT/MISC/ARCH_CAPS/SPEC_CTRL/microcode)"`

**Test checkpoint:** Boot on SMP system. Log shows `[CPU0 AUDIT] EFER=... CR4=... ` then `[CPU1 AUDIT]` per AP, then `[SMP] All N CPUs register-consistent`. Confirmed on KVM single-CPU: `[CPU0 AUDIT] EFER=0xd00 CR0=0x80010033 CR4=0x60e68 ... NX=1 ... WP=1 PCID=1 OSXSAVE=1` + `[SMP] All 1 CPUs register-consistent`. Bare metal: differences (SMEP absent on TCG, real UCODE vs KVM's 0xffffffff) are clearly visible; CI greps `[CPU. AUDIT]` and fails on unexpected register values. Unit tests assert the BSP snapshot was captured with sane values.

> **Test runner:** `scripts\debug\kernel\run-x86-tests.bat` (SUITE=x86) | 5 added suites (audit captured, CR0.WP, EFER.NXE, CR4.PAE, PAT matches baseline), 0 failures. AP audit + consistency validated via serial log on multi-CPU platforms.

> **Notes:**
> - Shipped: `cpu_audit_registers`/`cpu_audit_log`/`cpu_audit_consistency_check`/`cpu_audit_populate_registry` in `cpu_security.c`; flat per-CPU audit fields; `[CPU%u AUDIT]` + `[SMP] All N register-consistent` lines; `HKLM\HARDWARE\CPU\%u\Registers`.
> - Each CPU captures on itself (BSP in Phase 2, AP at `ap_cpu_harden` tail); BSP emits all lines post-bringup (serial-quiet AP rule). CPUID-gated + `msr_try_read` net; microcode read is Intel-wrmsr / AMD-direct.
> - Single consolidated parseable line per CPU = CI-grep target (`[CPU. AUDIT]`); no other OS surfaces this. Confirmed live on KVM. Codex design adoptions in the commit message.
> - Canonical doc: this section + `cpu_security.c` audit block.
> - Scope boundary: `[CPU%u AUDIT]` is the authoritative consolidated line; the §4/§6/§8 per-feature success lines remain for their checkpoints. Registry exposure owned here; registry-engine infra is `02-kernel-core/TODO-14`.

> **Verified:** 2026-05-27 | commit `2a96de4b` | 8/8 items | build OK | smoke PASS (KVM); `[CPU0 AUDIT]` + `[SMP] All 1 CPUs register-consistent` on serial
> **Accepted:** [M] `RegCreateKeyEx` non-atomic create+handle-alloc can publish a markerless `HKLM\HARDWARE\CPU\%u\Registers` key under handle-pool exhaustion (benign here -- absent `AuditComplete` reads as incomplete; affects all RegCreateKeyEx callers) -> XREF: 02-kernel-core/TODO-14 (item: "`RegCreateKeyEx` atomic create-or-fail" at line 112)
> **Quality reviewed:** 2026-05-27 | Codex 8x (design, adversarial, consistency, perf, re-adversarial) | 4M fixed, 1M accepted-XREF | scope: kernel-code-quality

---

## 10. AP Bringup Hardening & Robustness

**Prompt:** §4 shipped `ap_cpu_harden()` with warn-only AP-vs-BSP verification and a single-signal `is_online` online publication. Four follow-ups surfaced during the §4 review remain: the AP enable gates still trust BSP-global CPUID, the BSP required-CR4 mask is verified but not forced, and `smp_init` has two pre-existing degraded-bringup gaps (a timed-out AP can go live-but-uncounted; `smp_cpu_count()` collapses sparse slots). This section hardens the AP bringup path so a feature-skewed or slow AP degrades cleanly instead of #GP-ing or becoming invisible to the scheduler. Depends on §4 (AP hardening) + §6 (feature validation, for the force-after-validation gate).

> [!IMPORTANT]
> → XREF: `01-boot-platform/TODO-09 §4` -- `ap_cpu_harden()` / `cpu_record_bsp_profile()` / `s_bsp_required_cr4`; this section closes §4's warn-only / global-gate deferrals.
> → XREF: `01-boot-platform/TODO-09 §6` -- AP feature validation must run BEFORE the CR4 force so only AP-proven bits are forced.

- [x] **AP-local enable gating** -- `cpu_feature_local()` (BSP->`cpu_has`, AP->§6 `pc->features`) gates `cpu_enable_umip/pku/smep/smap` + TSC_AUX replay so a skewed AP no longer #GPs; `RDTSCP` added to the AP probe mask.
- [x] **CR4 force-after-validation** -- `cpu_force_ap_required_cr4()` forces the AP-supported subset of `s_bsp_required_cr4` (excl FSGSBASE/CET); `_live` drops PKE/PCIDE failing live XCR0.PKRU/`CR3[11:0]==0`; AP PCID fatal -> fault-record+halt.
- [x] **Periodic CR-pin verify-IPI** -- `IPI_VECTOR_CR_VERIFY` (0xFB) + handler (`cr0/cr4_verify_pinned` + EOI); `cpu_cr_pin_tick()` broadcasts to online APs once armed post-bringup. Closes the §7 AP-verify gap (AP LAPIC timers masked).
- [x] **Degraded-bringup abandon (CAS)** -- per-CPU `ap_bringup_state`: AP CASes STARTING->ONLINE then publishes `is_online`; BSP CASes STARTING->ABANDONED on timeout; AP validates live LAPIC ID vs prestored slot id before ONLINE.
- [x] **Sparse-slot accounting** -- `boot_async_group()` distributes work over an explicit `is_online` slot list (walk 1..MAX_CPUS), not dense `smp_cpu_count()`; clears async state for ALL online APs so a smaller later group leaves no stale work.
- [ ] **AP_DATA consume-ack handshake** -- a slow AP can run `ap_cpu_harden` on the next AP's stack before the S10 identity guard parks it; add a trampoline-entry ack the BSP waits on before reusing `AP_DATA`+stack (`ap_trampoline.asm`+`smp.c`).
- [ ] **Per-CPU TSS + IST** -- APs never `ltr` a TSS (`ap_trampoline.asm` loads BSP GDT only), so all CPUs share one `kernel_tss`/IST1-3 + a global rsp0. Add per-CPU TSS + GDT descriptor + IST + AP `ltr` (consumer: `D01 T10 §2`).
- [ ] **Phase-split INIT settle + 200us SIPI wait** -- `smp.c` serial 10ms INIT + 1ms SIPI per AP; INIT all targets, one shared settle, per-AP SIPI at 200us (needs consume-ack above; from `01-boot-platform/TODO-11` §10)
- [x] Commit: `"smp: AP bringup hardening -- AP-local gates, CR4 force-after-validation, degraded-bringup robustness"`

**Test checkpoint:** Boot on SMP system. A feature-skewed AP (or VM forcing a CPUID difference) degrades without #GP; serial shows the forced-CR4 / mismatch path. Partial-bringup (one AP timed out) leaves `smp_cpu_count()` and `boot_async_group()` consistent -- the live high-slot AP still gets work, and the timed-out AP logs `AP %u abandoned` and parks. Verify on QEMU WHPX (SMP), TCG, VirtualBox, bare metal. Single-CPU (KVM smoke): no APs, no verify-IPI armed, boots clean.

> **Test runner:** `scripts\debug\kernel\run-x86-tests.bat` (SUITE=x86) | 3 added suites (forceable-CR4 excludes FSGSBASE/CET, forceable gated-by-features, AP probe mask covers gated) + `run-boot-tests.bat` CR-verify IPI vector uniqueness, 0 failures. AP-path behavior validated via serial log on multi-CPU platforms.

> **Notes:**
> - Shipped: AP-local enable gating, CR4 force-after-validation, CR-pin verify-IPI (`IPI_VECTOR_CR_VERIFY`), CAS abandon state (`ap_bringup_state`), sparse-slot `boot_async_group` -- across `cpu_security.c` + `smp.c` + `boot_init.c`.
> - All on the AP bringup path: gating/force run in `ap_cpu_harden` (AP-local, pre-pin); abandon CAS in `ap_entry` vs `smp_init` timeout; verify-IPI armed at `smp_init` end, broadcast from the BSP timer tick; `RDTSCP` added to the §6 AP probe mask.
> - Closes §4 (warn-only CR4, global gates) + §7 (no periodic AP CR-pin verify) deferrals. Codex design adoptions in the commit message.
> - Canonical doc: this section + `cpu_security.c` AP-harden block + `smp.c` bringup.
> - Scope boundary: §10 owns AP-local gating, CR4 force, verify-IPI, abandon, sparse async. PAT re-broadcast + divergent-AP MTRR reprogram (§8 deferred) still need an all-CPU rendezvous, NOT the fire-and-forget verify-IPI.

> **Verified:** 2026-05-27 | commit `995b5103` | 5/6 items | build OK | tests x86 152 + boot 1723 PASS, smoke PASS
> **Deferred:** [H] residual shared-trampoline window: a slow AP can run `ap_cpu_harden` on the next AP's stack before the LAPIC-ID guard parks it -> XREF: 01-boot-platform/TODO-09 §10 (item: "AP_DATA consume-ack handshake")
> **Accepted:** [M] x2APIC >255 APIC IDs unsupported by the guard (reason: SIPI target + `lapic_id()` + `cpu_info.apic_id` are all 8-bit xAPIC, so unreachable today) -> XREF: 04-drivers-hardware/TODO-02 §1 (item: "Widen APIC IDs to 32 bits for systems with >255 cores" at line 55)
> **Quality reviewed:** 2026-05-27 | Codex 5x (adversarial + consistency + perf + 2 re-adversarial) | 2H fixed, 1H deferred + 1M accepted-XREF | scope: kernel-code-quality

---

## 11. Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)

Items moved here VERBATIM from their original, already-stamped sections, where they were unreachable: the triage oracle classifies a stamped section DONE without reading its body, so an item appended after the stamp is invisible to every later pass. Source section noted per group. Cohort context: `todo/overnight-runner-improvements/overnight-runner-improvements-v05.md` item 3.

From the stamped section 6:
- [ ] PKU global skew: `cpu_enable_pku` sets global `pku_enabled` from any enabling CPU, but PKU is AP-optional -- consumers (`pku.c`/`task.c`) could run PKRU on a CPU without CR4.PKE. Gate it on the online-CPU intersection. Filed from D01 T10 §9.

**Test checkpoint:** per moved item; each carries its original acceptance text.

---

## OS Comparison

| ⭐   | Feature                  | 🪟 Win11                     | 🐧 Linux                  | 🚀 Impossible OS                |
| --- | ------------------------ | --------------------------- | ------------------------ | ------------------------------ |
| 💎   | EFER.NXE before NX pages | ✅ Hal before paging         | ✅ cpu_init pre-paging    | ✅ §2 cpu_harden + log          |
| 💎   | SMEP/SMAP BSP Phase 0    | ✅ Hal CR4 early             | ✅ setup_cr4 early        | ⚠️ §2 wired, T10 §2 gate       |
| 💎   | AP hardening matches BSP | ✅ Hal per AP                | ✅ cpu_init secondary     | ✅ §4 ap_cpu_harden+profile     |
| 💎   | XSAVE after VMM ready    | ✅ OSXSAVE post-paging       | ✅ fpu deferred           | ✅ §5 Phase 1 finalize          |
| 💎   | PCID after page tables   | ✅ PCIDE post-PML4           | ✅ cr4 post-paging        | ✅ §5 CR4.PCIDE set             |
| 💎   | AP feature consistency   | ✅ BugCheck 0x3E             | ✅ verify_cpu per AP      | ✅ §6 BugCheck 0x3E             |
| 💎   | CR4 bit pinning          | ✅ HAL pins CR4              | ✅ cr4_pinned_bits        | ✅ §7 verify + 0x109            |
| 💎   | CR0.WP pinning           | ✅ HAL invariant             | ✅ cr0_pinned_bits        | ✅ §7 per-CPU pinned            |
| 💎   | PAT MSR AP sync          | ✅ pat per CPU               | ✅ pat per AP             | ✅ §8 registry replay + audit   |
| 💎   | MTRR AP matches BSP      | ✅ HAL sync paths            | ✅ mtrr_bp_init on APs    | ✅ §8 parity audit (warn-only)  |
| 💎   | Hybrid feature intersect | ✅ Group affinity            | ✅ cpu_caps per type      | ✅ §6 global AND-mask           |
| 💎   | AP bringup robustness    | ✅ KeStartProcessors timeout | ✅ cpuhp + per-cpu cr-pin | ✅ §10 abandon CAS + verify-IPI |
| ⭐   | HV detect before timer   | ✅ Before HAL timer          | ⚠️ Clocksource may lag   | ✅ §3 TLFS-gated hv_flags       |
| ⭐   | Confidential VM guest    | ✅ TDX + SEV in 24H2         | ✅ TDX + SEV-SNP 6.x      | ⬜ XREF 02/T09 §13              |
| ⭐   | CPU register audit trail | ❌ ETW fragments             | ❌ dmesg fragments        | ✅ §9 [CPU%u AUDIT] line        |
| ⭐   | POST per activation step | ❌ BIOS POST only            | ❌ dmesg only             | ⬜ TODO-14-boot-diag §2         |

> **§1-§9 complete; §10 core shipped (one deeper-hardening follow-up tracked).** **§10:** AP-local enable gating (`cpu_feature_local`), CR4 force-after-validation (`cpu_force_ap_required_cr4` + `_live` PKE/PCIDE precondition drops, FSGSBASE/CET excluded), CR-pin verify-IPI (`IPI_VECTOR_CR_VERIFY` broadcast from `cpu_cr_pin_tick`), degraded-bringup abandon CAS + LAPIC-ID identity guard (`ap_bringup_state`), and sparse-slot `boot_async_group` -- closes §4's warn-only/global-gate and §7's no-periodic-AP-verify deferrals; the `AP_DATA` consume-ack handshake remains tracked as the one open §10 item. **§7:** shared `cpu_regs.h` + per-CPU CR0.WP/CR4 safety-bit pinning (`cpu_pin_control_regs`), verified on #GP return / BSP timer tick / each AP, `CRITICAL_STRUCTURE_CORRUPTION` (0x109) on a cleared pin (BSP raises; APs record + halt). **§6:** `cpu_validate_ap_features()` bug-checks 0x3E on missing required feature / vendor / Long Mode; optional skew flagged on the AP + logged BSP-side; `cpu_features_finalize_global()` publishes the BSP-and-every-online-AP intersection. **§3:** `boot_info` hypervisor fields + `timer_hal_init()` ordering are in tree; **Registry mirror and TLFS-grade TSC reference page setup are still open** (see §3 unchecked bullet). **§4:** `ap_cpu_harden()` + per-CPU MSR replay profile + BSP baseline ship; CR4 force-after-validation + degraded-bringup robustness deferred to §10, the AP-mismatch bug-check to §6, the formal Phase 1 XSAVE/PCID window to §5. **§5:** `cpu_xsave_enable()` finalize + `cpu_pcid_enable()` (bare `CR4.PCIDE`) ship in `boot_phase1`, replicated on APs; XSAVE base stays in Phase 0 (SIMD/PKU need it). PCID exploitation (tagging/NOFLUSH) stays with `02-kernel-core/TODO-10 §7`; CET xstate with TODO-10 §9. §8, §9 are parity/competitive-edge; §10 hardens AP bringup (closes §4's deferrals). Minimal CPU hardening runs via `cpu_harden()` + `cpu_harden_post_pagetable()` (`TODO-10-bare-metal-hardening.md` §10). Full formal sequencing lands when the kernel hardening TODO ships `cpu_efer_harden()` / `cpu_cr4_harden()`.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_cpu_seq()` (→ `src/kernel/test/test_runner.c`, `include/kernel/test/test.h`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_cpu_seq.c` with:
  - `g_cpu` struct populated: `g_cpu.has_nx == 1` (NX is a minimum requirement)
  - `g_cpu.has_sse2 == 1` (SSE2 is a minimum requirement)
  - EFER.NXE is set: `rdmsr(0xC0000080) & (1 << 11)` is non-zero
  - `g_boot_info.hv_vendor` is either empty (bare metal) or a recognized string ("Microsoft Hv", "KVMKVMKVM", etc.)
  - `g_boot_info.hv_flags` is consistent with vendor: `HV_FLAG_TSC_ENLIGHTENMENT` set only when `hv_vendor == "Microsoft Hv"`
  - Hypervisor detection ran before timer: `g_boot_info.platform_type != PLATFORM_UNKNOWN` (platform_detect() completed); if `hv_vendor` is non-empty, `hv_flags != 0` (flags were set)
  - `cpu_has(CPU_FEATURE_SSE2)` returns true (matches `g_cpu.has_sse2`)
  - CR4.OSXSAVE is set if CPU supports XSAVE (`g_cpu.has_xsave` implies `CR4 & (1 << 18)`)
  - §4: `bsp_per_cpu_msr_profile_size > 0` once §2 lands (registry populated)
  - §5: when `cpu_has(CPU_FEATURE_CET_SS)`, expected XCR0 mask includes CET_U + CET_S bits (deferred assert until TODO-10 §9 lands)
  - §6: `cpu_data[ap_id].feature_mismatch == false` for every online AP on homogeneous systems; hybrid systems: `cpu_data[ap_id].core_type` is populated and global mask = intersection
  - §7: `cr_pinning_active == true` after boot; `(read_cr0() & cr0_pinned_mask) == cr0_pinned_mask` AND `(read_cr4() & cr4_pinned_mask) == cr4_pinned_mask` (both intact)
  - §8: `rdmsr(IA32_PAT)` on test CPU matches `g_bsp_pat_msr` (PAT synchronized)
  - §9: `cpu_data[0].audit.efer != 0` (populated); `cpu_data[0].audit.cr4 & CR4_PAE`; on CPUID-capable CPUs, `audit.arch_caps != 0` and `audit.ucode_rev != 0`
- [ ] Register in `test_runner_init()`: `test_register_cpu_seq()`
- [ ] Commit: `"test: add cpu_seq test suite"`

**Test checkpoint:** `bash scripts/test.sh SUITE=boot` (or `make test-boot`) passes after `test_cpu_seq.c` and `test_register_cpu_seq()` land; each bullet above is a `TEST_ASSERT_*` with an expected value (no vague "CPU works"). QEMU WHPX, TCG, VirtualBox, bare metal.

---

## Verification

- [ ] Serial log shows `EFER.NXE=1` line **before** first `[VMM]` line in boot output
- [ ] Serial log shows `[AP%u] CPU hardening applied` for every AP that comes online
- [ ] Serial log shows `[Phase0] Hypervisor: ...` **before** first `[UTS]` / `[Timer]` line
- [ ] Serial log shows `[Phase1] XSAVE enabled` and `[Phase1] PCID enabled` **after** `[VMM] init complete`
- [ ] Serial log shows `[AP%u] Feature validation OK` for every AP (§6)
- [ ] Serial log shows `[Phase1] CR4 pinned: mask=0x...` after XSAVE/PCID activation (§7)
- [ ] Serial log shows `[AP%u] PAT synced: 0x...` for every AP (§8)
- [ ] Serial log shows `[CPU0 AUDIT] EFER=... CR4=...` and `[CPU%u AUDIT]` for each AP (§9)
- [ ] Serial log shows `[SMP] All N CPUs register-consistent` after SMP bringup (§9)
- [ ] Boot completes with `=== BUILD OK ===` and no regressions in QEMU + Hyper-V + bare metal
- [ ] Commit: `"boot: cpu-sequencing verified -- EFER/CR4 order, AP hardening, feature consistency, CR4 pinning, PAT sync, register audit"`

**Test checkpoint:** Every Verification bullet above holds on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal; §2/§4/§5/§6--§9 items marked N/A until those sections ship stay documented in serial/klog gaps, not silent failures.

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | tests pending implementation (§2-§9 still [ ]/[/]) -- once `test_cpu_seq.c` lands, expect N suites, 0 failures

---

## History

| Date | Action | Summary |
| --- | --- | --- |
| 2026-04-10 | validate | validate-todo-file: Inputs XREFs use full `.md` paths; VMM PAT XREF §1→§11; PEB/TEB link disambiguated to `TODO-11-peb-teb-user-abi.md §1`; §7 TODO-10 §13→§10; §3 `HV_FLAG_*` + test checkpoint; Impl Order `defer`→`[ ]`, T24/T01 § refs; OS table padded + last row TODO-14 §2; Unit/Verification checkpoints + `run-boot-tests.bat`; History added; reciprocal XREF on PEB/TEB §6. Flags: §3/§5/§6 blocked on T24/T01; §7--§10 need `cpu_data`/`g_bsp_pat_msr`/audit fields. |
| 2026-04-11 | gap-analysis | Web: Hyper-V TLFS timers (`HV_X64_MSR_TIME_REF_COUNT`); Linux SMP/`verify_cpu` paths; MTRR+PAT SMP context (Gentoo/kernel docs). Code-truth: `boot_hw.c` Phase0 order; `platform_detect()` in `timer.c` + `cpu_security.c`; SMEP/SMAP globally skipped (`hv_supports_cr4_smep_smap` returns 0); PAT value ~307; no Registry HV keys. Added `IMPORTANT` current-state block; Inputs `cpuid_platform.c`/`timer.c`; TSC XREF `TODO-08-time-filetime-management.md`; §1 Registry deferral + Impl §1→`[/]`; §8 MTRR bullet + OS row; TODO-20 XREF. |
| 2026-04-11 | validate | validate-todo-file: Inputs add `boot_hw.c`, fix `boot_phase0` anchor; XREF `TODO-07-time` → §2,§3; OS POST row → `TODO-14-boot-diagnostics.md` §2; Unit Tests `→` arrow; 9 sections Commit+checkpoint OK; §9 at 10 pre-Commit bullets (split if grow); `[/]` row 3 + external T24/T01 blockers; `run-boot-tests.bat` present. |
| 2026-05-20 | gap-analysis | Codex gap-audit: 6 findings + 2 retargets. Branch A on §4 (per-CPU MSR profile registry), §5 (CET xstate handoff), §6 (intersection ordering + hybrid CPUID 0x1A + TSC-sync XREF), §7 (CR0.WP pinning), §9 (ARCH_CAPS/SPEC_CTRL/microcode audit). Branch C: 02/T09 §15 (TDX/SEV-SNP/HV ref TSC), 01/T11 §6 (HV ref TSC consumer). OS table +3 rows (CR0.WP, hybrid, conf-compute). |
| 2026-05-20 | review | §1 re-review (commit `00f9429d`): Codex 3x adversarial+consistency+perf. 3M fixed: num_cores uint8 wrap >=256 cores (cpuid.c:237 widened to uint16); AMD ThreadsPerCore not decoded (cpuid.c:246 + topology.c:54); threads_per_core re-introduced same wrap (clamped at 255). Perf: approved. Lint CLEAN. |
| 2026-05-20 | implement | §2 shipped (commit `77fce853`): POSTCODE_CPU_HARDEN_DONE alias + cpu_security_log_state() helper + 2 BSP call sites + 1 unit test. Codex 4x (design adopted moving log out of shared helpers; adversarial 2x approved; consistency 1M fixed test-comment contract drift; perf approved). Stale prereq references (msr_init / cpu_efer_harden) dropped from §2 preamble. Smoke PASS TCG 2.64s. |
