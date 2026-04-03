# TODO-04 -- CPU Boot Sequencing & AP Hardening

> **Goal:** Establish the correct activation order for CPU security and context features during boot phases 0 and 1, replicate that activation on every Application Processor (AP), validate AP feature consistency, pin safety-critical CR4 bits against post-boot modification, synchronize MTRR/PAT cache policy on each AP, and provide a comprehensive CPU register state audit trail. The feature implementations themselves live in `02-kernel-core/TODO-17` (security hardening) and `02-kernel-core/TODO-19` (x86-64 architecture); this TODO owns the boot-sequencing contract between them -- what activates, in what phase, who ensures APs match the BSP, and what happens when they don't.

> [!IMPORTANT]
> **Scope boundary:** Do NOT implement CPU security features or CPU architecture features here. That work lives in:
> - NX/EFER, SMEP/SMAP, KPTI, PCID, Spectre, CET → `02-kernel-core/TODO-17`
> - XSAVE, MSR layer, UMIP, 1 GiB pages, topology, errata, VM detection → `02-kernel-core/TODO-19`
> - TSC frequency calibration → `02-kernel-core/TODO-07`
> This TODO owns: phase placement, activation order within phases, AP-trampoline replication, AP feature consistency validation, CR4 bit pinning, MTRR/PAT AP synchronization, CPU register audit trail, and hypervisor pre-detection before the timer subsystem selects its driver.

---

## Inputs

- `src/kernel/main/boot_init.c` -- `boot_phase0/1/2/3()` entry points; this TODO annotates the phase boundaries
- `include/kernel/boot_init.h` -- `BOOT_STEP`, `BOOT_REQUIRE`, `POSTCODE_*`; add CPU postcodes
- `src/kernel/smp/ap_trampoline.asm` + `src/kernel/smp/smp.c` -- AP startup path; add `ap_cpu_harden()` call
- `include/kernel/cpuid.h` -- `cpuid_init()`, `cpu_has()`; already called in Phase 0
- `src/kernel/smp/smp.c` -- `wrmsr`/`rdmsr` inline helpers; input to MSR layer (-> TODO-19 §3)
- `src/kernel/mm/vmm.c` -- VMM init; EFER.NXE must be set before first page table write with NX bit
- `src/kernel/cpu_security.c` -- `cpu_harden()`, `cpu_harden_post_pagetable()`; current AP hardening path
- `include/kernel/msr.h` -- MSR addresses (IA32_EFER, IA32_PAT); CR4 bit definitions; `MSR_IA32_MISC_ENABLE` to be added by §9
- -> XREF: `02-kernel-core/TODO-01 §2` -- `boot_phase0` sequence; CPU hardening steps slot in here
- -> XREF: `02-kernel-core/TODO-17 §1--§7` -- NX, SMEP, SMAP, PCID, Spectre, CET (implementation details)
- -> XREF: `02-kernel-core/TODO-19 §1, §3--§7, §9, §11` -- XSAVE, MSR layer, UMIP, 1 GiB pages, errata, VM detection
- -> XREF: `01-boot-platform/TODO-06 §6` -- UTS timer driver selection; hypervisor detection (§3 here) feeds it
- -> XREF: `04-drivers-hardware/TODO-11-security-hardware.md §7` -- SMEP+SMAP implementation (CR4 enable + copy_from/to_user wrappers); this TODO owns boot sequencing and AP consistency, TODO-11 §7 owns the actual CR4 write functions
- -> XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §1` -- UC MMIO mapping; PAT MSR programming (§8 here) must be consistent with VMM cache policy

---

## Outcome

- All CPU security features (EFER.NXE, CR4 flags) are provably activated before VMM maps any page with NX bits.
- Every AP runs `ap_cpu_harden()` at startup -- no AP boots with SMEP/SMAP/EFER absent while BSP has them.
- AP feature consistency is validated: mismatched CPUID feature sets between BSP and AP trigger a controlled bug-check (`MULTIPROCESSOR_CONFIGURATION_NOT_SUPPORTED`) or graceful degradation to the lowest common feature set.
- CR4 safety bits (SMEP, SMAP, UMIP, CET, FSGSBASE) are pinned after boot -- any post-boot attempt to clear them panics with `CRITICAL_STRUCTURE_CORRUPTION`.
- Each AP synchronizes its PAT MSR to match BSP cache policy -- prevents WB-vs-UC mismatch on shared MMIO pages.
- Hypervisor vendor is detected and stored in `boot_info` before the UTS probes its timer drivers, enabling correct Hyper-V TSC and TLB enlightenments from the start.
- XSAVE and PCID are activated only after VMM is ready, in the correct Phase 1 window.
- A per-CPU register audit trail logs EFER, CR4, XCR0, and PAT values with postcodes: `[Phase0] CPU0: EFER=0xD01 CR4=0x3406E0 XCR0=0x7 PAT=0x0007040600070406`.

---

## Implementation Order

| ⭐  | Order | Deliverable                                      | Depends On                 | Status |
| --- | :---: | ------------------------------------------------ | -------------------------- | :----: |
| 💎  |   1   | CPUID detection & per-CPU capability capture     | P0, D02 T19 §1             |  [x]   |
| 💎  |   2   | Phase 0 CPU security activation order            | §1, D02 T17 §1--3 & T19 §3 | defer  |
| ⭐  |   3   | Hypervisor detection before timer selection      | §1, D02 T19 §11            |  [x]   |
| 💎  |   4   | AP CPU hardening (`ap_cpu_harden()`)             | §2, D02 T17 §1--7          | defer  |
| 💎  |   5   | Phase 1 XSAVE & PCID activation window           | §2, D02 T17 §4 & T19 §1   | defer  |
| 💎  |   6   | AP feature consistency validation                | §1, §4                     |  [ ]   |
| 💎  |   7   | CR4 safety-bit pinning                           | §2, §5                     |  [ ]   |
| 💎  |   8   | MTRR/PAT AP synchronization                      | §4                          |  [ ]   |
| ⭐  |   9   | CPU register state audit trail                   | §2, §5                     |  [ ]   |

> 💎 = parity -- Windows and Linux both enforce EFER/CR4 ordering, AP parity, feature consistency, CR4 pinning, and PAT synchronization; Impossible OS must match that contract.
> ⭐ = exclusive -- hypervisor pre-detection before timer HAL selection, per-activation postcode audit trail, and structured register dump are not surfaced the same way on Windows or Linux.

---

## 1. CPUID Detection & Per-CPU Capability Capture

**Prompt:** `cpuid_init()` already runs in Phase 0 and populates feature flags, but the AMD extended leaves (`0x80000001`, `0x8000001E`, `0x80000008`) and per-CPU storage in `struct cpu_data` are not complete. Extend `cpuid_init()` to probe all leaves needed by TODO-17 and TODO-19: SSE2, SSE4.2, AVX, AVX2, AVX512F, AES-NI, RDRAND, RDSEED, CET_SS, CET_IBT, UMIP, SMEP, SMAP, PCID, INVPCID, FSGSBASE, FFXSR; AMD leaves 0x80000001 (1-GB pages, RDTSCP, PDPE1GB, SVM), 0x8000001E (Zen topology), 0x80000008 (phys/virt address bits); store result in `cpu_data[0].cpuid_features`; emit `[Phase0] CPUID probed, POSTCODE_CPUID_DONE` to `boot_progress()`.

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-19 §1` -- full feature struct definition lives there; this TODO only gates the call into Phase 0.

- [x] `cpuid_init()` runs in Phase 0 after heap_init -- confirmed in boot_hw.c line 141
- [x] AMD extended leaves 0x80000001 (NX, SVM, OSVW, IBS, Page1GB, RDTSCP), 0x8000001E (Zen topology), 0x80000008 (phys/linear addr bits) all parsed
- [x] Result stored in global `g_cpu` struct (type `struct cpu_features`) -- 54 feature flags + address bits + topology
- [x] `POSTCODE_CPUID_INIT = 0x24` already defined; `boot_progress(0, "CPUID", POSTCODE_CPUID_INIT)` emitted
- [x] Already implemented -- marking complete

---

## 2. Phase 0 CPU Security Activation Order *(deferred -- blocked by TODO-17 `cpu_efer_harden`/`cpu_cr4_harden` + TODO-19 `msr_init`; minimal version in TODO-05 §11)*
**Prompt:** Document and enforce the required activation sequence in `boot_phase0()`: (1) `msr_init()` (centralized MSR layer, TODO-19 §3) must run before any MSR write; (2) `cpu_efer_harden()` sets `EFER.NXE=1`, `EFER.SCE=1`, `EFER.FFXSR` if supported -- **this must complete before VMM init** because VMM will write PTE bit 63 (NX) on the first `vmm_map_page()` call; (3) `cpu_cr4_harden()` sets `CR4.SMEP`, `CR4.SMAP`, `CR4.UMIP` if detected -- must run before any user-mode-visible mapping; emit a `POSTCODE_CPU_HARDEN_DONE` after the last CR4 write. Add `BOOT_REQUIRE(SUBSYS_MSR)` guards to EFER/CR4 steps so wrong-order calls panic with a clear message. Log: `[Phase0] EFER=0x{val} CR4=0x{val}`.

> [!IMPORTANT]
> -> XREF: `02-kernel-core/TODO-17 §1--3` -- `cpu_efer_harden()` and `cpu_cr4_harden()` implementations live there.
> → XREF: `02-kernel-core/TODO-19 §3` -- `msr_init()` implementation lives there (MSR Management Infrastructure).
> → XREF: `02-kernel-core/TODO-01 §2` -- Phase 0 boot sequence; this step slots between serial init and PMM.

- [ ] Add `POSTCODE_MSR_INIT`, `POSTCODE_CPU_EFER`, `POSTCODE_CPU_CR4`, `POSTCODE_CPU_HARDEN_DONE` to `boot_init.h`
- [ ] Insert `BOOT_STEP(SUBSYS_MSR, msr_init)` in `boot_phase0()` before VMM step
- [ ] Insert `BOOT_STEP(SUBSYS_CPU_EFER, cpu_efer_harden)` immediately after MSR init
- [ ] Insert `BOOT_STEP(SUBSYS_CPU_CR4, cpu_cr4_harden)` after EFER step
- [ ] Add `BOOT_REQUIRE(SUBSYS_MSR)` inside `cpu_efer_harden()` and `cpu_cr4_harden()`
- [ ] Verify (serial log): `[Phase0] EFER.NXE=1 SMEP=1 SMAP=1 UMIP=1` appears before first VMM log line
- [ ] Commit: `"boot: enforce Phase 0 CPU security activation order before VMM init"`

**Test checkpoint:** Serial log shows `[Phase0] EFER.NXE=1` before first `[VMM]` line. `POSTCODE_CPU_HARDEN_DONE` visible on POST display. Wrong-order boot (VMM before EFER) triggers `BOOT_REQUIRE` panic with clear subsystem name. Verify on QEMU WHPX, TCG, VirtualBox, bare metal. Bare metal is critical -- EFER/CR4 writes have different side effects under EPT vs native paging.

---

## 3. Hypervisor Detection Before Timer Selection

The UTS probe in TODO-06 §6 selects HPET vs PIT vs LAPIC timer, but on Hyper-V the correct choice is the `HV_X64_MSR_TIME_REF_COUNT` reference counter. Hypervisor detection must run before UTS `timer_probe()`. In Phase 0, after CPUID: probe CPUID `0x40000000` for the hypervisor-present bit; if set, read vendor string (`0x40000001`); store `hv_vendor` in `boot_info` and `HKLM\HARDWARE\VM\HypervisorVendor`; for `"Microsoft Hv"`: set `boot_info.hv_flags |= HV_TSC_ENLIGHTENMENT | HV_TLBFLUSH_HYPERCALL`; for `"KVMKVMKVM"`: set `HV_KVM_STEAL_TIME`; UTS probe reads `boot_info.hv_flags` to select the correct clock source first. Emit `[Phase0] Hypervisor: Microsoft Hv (flags=0x3)` or `[Phase0] Bare metal`.

> [!IMPORTANT]
> → XREF: `01-boot-platform/TODO-06 §6` -- UTS reads `boot_info.hv_flags` to select clock; must be set before `timer_probe()`.
> → XREF: `02-kernel-core/TODO-19 §11` -- AMD-V / VT-x host capability detection; Hyper-V guest enlightenment MSR initialization (using `boot_info.hv_flags` populated by this step) is not yet specced in §11 and needs to be added there.

- [x] Add `hv_vendor[16]` and `hv_flags` fields to `struct boot_info`
- [x] Define `HV_FLAG_TSC_ENLIGHTENMENT`, `HV_FLAG_TLBFLUSH_HYPERCALL`, `HV_FLAG_KVM_STEAL_TIME`, `HV_FLAG_APIC_FREQ_MSR`, `HV_FLAG_VMWARE_BACKDOOR` in `boot_init.h`
- [x] `platform_detect()` in `cpuid_platform.c` populates `g_boot_info.hv_vendor` + `g_boot_info.hv_flags` for Hyper-V, KVM, VMware, VirtualBox
- [x] Detection runs from `timer_hal_init()` → `platform_detect()` in Phase 1, before timer backend selection
- [x] UTS timer probe uses `platform_has_apic_freq_msr()` which reads cached platform state
- [x] Commit: `"boot: hypervisor detection with hv_flags in boot_info"`

---

## 4. AP CPU Hardening (`ap_cpu_harden()`) *(deferred -- blocked by §2; current `cpu_harden()` + `cpu_harden_post_pagetable()` applied on APs in smp.c)*
**Prompt:** When APs start up via `smp_ap_main()`, they run their own GDT/IDT/LAPIC setup but currently skip the EFER and CR4 security features that BSP Phase 0 enables. An AP running without `EFER.NXE`/`SMEP`/`SMAP` is a full privilege bypass vector -- kernel code on that AP can execute user pages and access user memory unchecked. Implement `ap_cpu_harden()` that replicates the BSP Phase 0 CPU activation: `msr_write(IA32_EFER, bsp_efer_val)`, then `cr4_write(bsp_cr4_val)` (read from BSP at Phase 0, stored in `cpu_data[0].efer_at_boot` and `cpu_data[0].cr4_at_boot`); call from `smp_ap_main()` immediately after GDT load and before AP signals ready; each AP logs `[AP%u] CPU hardening applied EFER=0x{val} CR4=0x{val}`.

> [!IMPORTANT]
> -> XREF: `02-kernel-core/TODO-17 §1--7` -- notes that AP trampoline must enable same CR4/MSR features; this TODO writes the call site; TODO-17 provides the underlying `cpu_efer_harden()` function reused on APs.

- [ ] Add `efer_at_boot` and `cr4_at_boot` fields to `struct cpu_data`; BSP stores values at end of Phase 0
- [ ] Implement `ap_cpu_harden()` in `boot_init.c`: reads BSP values, applies to current CPU
- [ ] Call `ap_cpu_harden()` in `smp_ap_main()` before `ap_ready_flag = 1`
- [ ] Verify (serial log): `[AP1] CPU hardening applied` appears for each non-BSP CPU
- [ ] Verify: no AP starts with `EFER.NXE=0` when BSP has `EFER.NXE=1`
- [ ] Commit: `"smp: apply CPU hardening on AP startup via ap_cpu_harden()"`

**Test checkpoint:** Boot on SMP system (2+ CPUs). Serial log shows `[AP1] CPU hardening applied EFER=0x... CR4=0x...` for every AP before `ap_ready_flag = 1`. Verify EFER/CR4 values match BSP. On single-CPU system: no AP messages, BSP-only boot succeeds. Verify on QEMU WHPX (SMP), TCG (1 CPU), VirtualBox (4 CPUs), bare metal. Bare metal: confirm AP does not crash between GDT load and `ap_cpu_harden()` call -- this window is IRQ-disabled but NMI-vulnerable.

---

## 5. Phase 1 XSAVE & PCID Activation Window *(deferred -- blocked by TODO-17 §4 + TODO-19 §1; XSAVE already enabled via `simd_enable_avx()` in Phase 0)*
**Prompt:** XSAVE and PCID require VMM to be ready first -- XSAVE because per-thread XSAVE areas are allocated in TEB pages managed by VMM, and PCID because `CR4.PCIDE` changes how CR3 is loaded (PCID bits 11:0) and must be set only after the page table base is established. Slot these activations into Phase 1: (1) after `vmm_init()`: call `cpu_xsave_enable()` -- sets `CR4.OSXSAVE`, calls `XSETBV(XCR0, X87|SSE|AVX)`, stores `xsave_area_size` in a global; (2) after process table is initialised: call `cpu_pcid_enable()` -- sets `CR4.PCIDE`, validates INVPCID is available; PCID 0 reserved for kernel; emit `POSTCODE_XSAVE_ENABLED` and `POSTCODE_PCID_ENABLED`. Both functions are no-ops if the CPU does not support the feature.

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-19 §1` -- `cpu_xsave_enable()` implementation (XSAVE area sizing, XCR0 bits).
> → XREF: `02-kernel-core/TODO-17 §4` -- `cpu_pcid_enable()` and CR3 load changes for KPTI (PCID implementation).
> → XREF: `02-kernel-core/TODO-04 §6` -- TEB Allocation and Population at Thread Create; XSAVE area is allocated alongside the TEB at thread creation, which is why VMM must be ready before XSAVE is enabled.

- [ ] Add `POSTCODE_XSAVE_ENABLED`, `POSTCODE_PCID_ENABLED` to `boot_init.h` Phase 1 constants
- [ ] Insert `BOOT_STEP(SUBSYS_XSAVE, cpu_xsave_enable)` in `boot_phase1()` after VMM init
- [ ] Insert `BOOT_STEP(SUBSYS_PCID, cpu_pcid_enable)` in `boot_phase1()` after process table init
- [ ] Verify (serial log): `[Phase1] XSAVE enabled (area=N bytes)` and `[Phase1] PCID enabled`
- [ ] Verify: XSAVE enable does not run in Phase 0 (VMM not yet up at that point)
- [ ] Commit: `"boot: activate XSAVE and PCID in Phase 1 after VMM ready"`

**Test checkpoint:** Serial log shows `[Phase1] XSAVE enabled (area=N bytes)` after `[VMM] init complete`. `[Phase1] PCID enabled` appears after process table init. On CPUs without XSAVE: log shows `[Phase1] XSAVE: not supported, skipped`. Verify no `XSAVE` or `PCID` log lines appear during Phase 0. Verify on QEMU WHPX, TCG (XSAVE may be absent), VirtualBox, bare metal.

---

## 6. AP Feature Consistency Validation

Windows triggers bug-check `MULTIPROCESSOR_CONFIGURATION_NOT_SUPPORTED` (0x3E) when an AP's CPUID feature set is incompatible with the BSP. Linux runs `verify_cpu.S` on each AP during trampoline entry to ensure Long Mode and SSE. Impossible OS currently has no equivalent -- an AP with different capabilities could silently cause undefined behavior.

> [!IMPORTANT]
> -> XREF: `02-kernel-core/TODO-19 §7` -- CPU topology parsing; topology differences (core count, cache layout) are informational, not a failure. This section only validates *security-critical* feature mismatches.

- [ ] Define `cpu_features_required_mask` in `cpuid.h`: bitmask of features that all CPUs must share (NX, SSE2, LAHF, CMPXCHG16B, SYSCALL, PAE, PGE); derive from BSP's detected features at Phase 0
- [ ] Implement `cpu_validate_ap_features(uint32_t ap_id)` in `cpu_security.c`: runs on each AP after CPUID probe, compares AP features against BSP `cpu_features_required_mask`
- [ ] On mismatch: log `[AP%u] FEATURE MISMATCH: BSP has %s, AP missing` for each missing feature; set `cpu_data[ap_id].feature_mismatch = true`
- [ ] If any security-critical feature is missing (NX, SMEP, SMAP): degrade BSP to lowest common denominator (disable feature globally) and log `[WARN] Degrading %s -- AP%u does not support it`
- [ ] Define `BUGCHECK_MULTIPROCESSOR_CONFIGURATION_NOT_SUPPORTED = 0x3E` in a bugcheck header (or `boot_init.h` until a dedicated bugcheck header exists); panic on fatal mismatch
- [ ] If CPU family/model differs beyond tolerance (different vendor, or missing Long Mode): panic with `BUGCHECK_MULTIPROCESSOR_CONFIGURATION_NOT_SUPPORTED` -- system cannot safely continue with asymmetric CPUs
- [ ] Add `POSTCODE_AP_VALIDATE = 0x27` to `boot_init.h` (Phase 0 range, runs during SMP bringup on each AP); emit per-AP
- [ ] Add debug `POST16(0xD600)` on entry to `cpu_validate_ap_features()`, `POST16(0xD601)` on success exit (remove after bare-metal verification)
- [ ] Verify (serial log): on homogeneous system, `[AP%u] Feature validation OK` for each AP; on simulated mismatch (debug flag), degradation message appears
- [ ] Commit: `"smp: AP feature consistency validation with graceful degradation"`

**Test checkpoint:** Boot on SMP system. Log shows `[AP1] Feature validation OK` for every AP. No degradation messages on homogeneous hardware. Bare metal: verify on systems with identical CPU cores; note that P-core/E-core hybrid CPUs may show feature differences (UMIP, AVX-512) that should degrade gracefully, not panic.

---

## 7. CR4 Safety-Bit Pinning

Linux pins CR4 bits (SMEP, SMAP, UMIP, FSGSBASE, CET) after boot to prevent rootkits from disabling security features by writing CR4. Windows HAL protects equivalent bits. Impossible OS has no post-boot CR4 protection -- a kernel exploit can trivially clear CR4.SMEP and execute user pages.

- [ ] Add `CR4_UMIP` (bit 11), `CR4_FSGSBASE` (bit 16), `CR4_CET` (bit 23) definitions to `cpu_security.c` alongside existing `CR4_SMEP`/`CR4_SMAP` (or move all CR4 bit definitions to a shared header)
- [ ] Define `cr4_pinned_mask` in `cpu_security.c`: bitmask of CR4 bits that must not be cleared after boot; includes `CR4_SMEP`, `CR4_SMAP`, `CR4_UMIP`, `CR4_FSGSBASE`, `CR4_CET` (set bits only for features that are actually enabled)
- [ ] Populate `cr4_pinned_mask` at end of Phase 1 (BSP) after all CR4 feature activation is complete -- must be AFTER §5 (XSAVE/PCID activation) so that `CR4.OSXSAVE` and `CR4.PCIDE` are set before pinning; set `cr4_pinning_active = true`
- [ ] Define `BUGCHECK_CRITICAL_STRUCTURE_CORRUPTION = 0x109` in the same bugcheck header
- [ ] Implement `cr4_verify_pinned()`: reads CR4, checks `(cr4 & cr4_pinned_mask) == cr4_pinned_mask`; if not, panic with `BUGCHECK_CRITICAL_STRUCTURE_CORRUPTION` and log which bit was cleared
- [ ] Call `cr4_verify_pinned()` at strategic points: (a) on return from `#GP` handler, (b) periodically from the LAPIC timer DPC, (c) on each AP after hardening
- [ ] Implement `cr4_write_safe(uint64_t new_cr4)`: wrapper around `write_cr4()` that asserts pinned bits are preserved; all kernel code must use this wrapper after pinning is active. **Rollback:** If pinning causes false panics, set `cr4_pinning_active = false` and revert to unprotected `write_cr4()` until all callers are audited
- [ ] Add `POSTCODE_CR4_PINNED = 0x39` to `boot_init.h` (Phase 1 range -- pinning runs at end of Phase 1 after XSAVE/PCID)
- [ ] Add debug `POST16(0xD400)` before `cr4_pinned_mask` population, `POST16(0xD401)` after pinning active (remove after bare-metal verification; `0xD7xx` range reserved for xhci.c)
- [ ] Verify (serial log): `[Phase1] CR4 pinned: mask=0x%lx` appears after XSAVE/PCID activation
- [ ] Commit: `"boot: CR4 safety-bit pinning with CRITICAL_STRUCTURE_CORRUPTION panic"`

**Test checkpoint:** Boot completes with `[Phase1] CR4 pinned: mask=0x...` log line after XSAVE/PCID activation. In debug build, call `write_cr4(cr4 & ~CR4_SMEP)` from a test -- verify it triggers `CRITICAL_STRUCTURE_CORRUPTION` panic. Verify on QEMU WHPX (SMP), TCG, VirtualBox, bare metal. Bare metal: confirm XSAVE/PCID Phase 1 activation completes before pinning takes effect.

---

## 8. MTRR/PAT AP Synchronization

Both Windows and Linux synchronize the PAT (Page Attribute Table) MSR on each AP to match the BSP. The PAT MSR (`IA32_PAT`, MSR 0x277) maps PAT index values (from PTE bits PWT/PCD/PAT) to cache types (WB, UC, WC, WT, WP). If APs have different PAT MSR values than the BSP, the same PTE on different CPUs would produce different cache types -- causing silent data corruption on shared MMIO pages.

> [!NOTE]
> -> XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §1` -- VMM uses `vmm_map_mmio_uc()` which relies on PAT index 3 being UC. If an AP's PAT MSR maps index 3 to WB, MMIO accessed on that AP is cached and hardware registers read stale values.

- [ ] Read BSP PAT MSR value in Phase 0 after `cpuid_init()`; store in `g_bsp_pat_msr` global (or in `cpu_data[0].pat_msr`)
- [ ] In `ap_cpu_harden()` (§4): after EFER/CR4 replication, write `wrmsr(IA32_PAT, g_bsp_pat_msr)` to synchronize AP PAT to BSP. **Rollback:** If PAT write crashes AP, skip the wrmsr and log `[WARN] AP%u PAT sync skipped` -- AP will use firmware-default PAT which is usually identical to BSP anyway
- [ ] Read back AP PAT and verify match: `rdmsr(IA32_PAT) == g_bsp_pat_msr`; log `[AP%u] PAT synced: 0x%lx` on success, panic on mismatch (hardware fault)
- [ ] If the kernel later reprograms PAT (e.g., for Write-Combining framebuffer in TODO-19 §5), broadcast the new PAT value to all online APs via IPI + `smp_call_function(pat_update_ap, &new_pat, true)` (note: `smp_call_function()` does not exist yet -- defer this bullet until SMP IPI infrastructure is available)
- [ ] Add `POSTCODE_PAT_SYNC = 0x29` to `boot_init.h` (Phase 0 range -- PAT sync runs on each AP during SMP bringup)
- [ ] Add debug `POST16(0xD800)` before PAT wrmsr on AP, `POST16(0xD801)` after readback verify (remove after bare-metal verification)
- [ ] Verify (serial log): `[AP%u] PAT synced` appears for each AP
- [ ] Commit: `"smp: synchronize PAT MSR on AP startup"`

**Test checkpoint:** Boot on SMP system. All APs show `PAT synced` in serial log with matching hex value. Bare metal: verify PAT value is identical across BSP and all APs using the register audit trail (§9). On systems with MTRR override (LAPIC/IOAPIC ranges), confirm MMIO still works from AP-scheduled code paths.

---

## 9. CPU Register State Audit Trail

Neither Windows nor Linux produces a consolidated, structured, per-CPU register dump at boot. Windows logs fragments via ETW, Linux scatters pieces across dmesg. Impossible OS can provide a single authoritative audit line per CPU that documents the complete security-relevant register state -- invaluable for debugging bare-metal boot failures and for compliance auditing.

> [!TIP]
> **Competitive edge:** A single `[CPU%u AUDIT]` line per CPU with EFER, CR0, CR4, XCR0, PAT, IA32_MISC_ENABLE, and all security feature status in a fixed parseable format is something no other OS provides. This enables automated boot verification scripts and CI regression detection.

- [ ] Add `#define MSR_IA32_MISC_ENABLE 0x1A0` to `include/kernel/msr.h` if not already present (needed for audit readout)
- [ ] Implement `cpu_audit_registers(uint32_t cpu_id)` in `cpu_security.c`: reads EFER, CR0, CR4, XCR0 (if OSXSAVE), PAT, IA32_MISC_ENABLE; formats as single structured log line
- [ ] Log format: `[CPU%u AUDIT] EFER=0x%lx CR0=0x%lx CR4=0x%lx XCR0=0x%lx PAT=0x%lx MISC=0x%lx NX=%u SMEP=%u SMAP=%u UMIP=%u CET=%u FSGS=%u PCID=%u`
- [ ] Call on BSP at end of Phase 0 (after all CR4/EFER activation); call on each AP at end of `ap_cpu_harden()`
- [ ] Store audit data in `cpu_data[cpu_id].audit` struct for runtime query via Registry key `HKLM\HARDWARE\CPU\%u\Registers`
- [ ] After SMP bringup complete: compare all AP audit structs against BSP; log `[SMP] All %u CPUs register-consistent` or `[SMP] WARN: CPU%u differs from BSP` with specific register and bit differences
- [ ] Add `POSTCODE_CPU_AUDIT = 0x2A` to `boot_init.h` (Phase 0 range -- audit runs at end of Phase 0 on BSP, and on each AP during SMP bringup)
- [ ] Add debug `POST16(0xD300)` on entry to `cpu_audit_registers()`, `POST16(0xD301)` on exit (remove after bare-metal verification; `0xD9xx` range reserved for cpu_security.c)
- [ ] Commit: `"boot: per-CPU register state audit trail at boot"`

**Test checkpoint:** Boot on SMP system. Log shows `[CPU0 AUDIT] EFER=... CR4=... ` line, then `[CPU1 AUDIT]` for each AP, then `[SMP] All N CPUs register-consistent`. Bare metal: compare audit output between QEMU and real hardware -- the differences (e.g., SMEP absent on QEMU TCG, PAT differences) should be clearly visible. CI scripts can grep for `[CPU. AUDIT]` lines and fail on unexpected register values.

---

## OS Comparison

| ⭐ | Feature                   | 🪟 Win11                        | 🐧 Linux                         | 🚀 Impossible OS                      |
|----|---------------------------|------------------------------|-------------------------------|------------------------------------|
| 💎 | EFER.NXE before NX pages  | ✅ HalInitializeProcessor   | ✅ cpu_init before paging     | ⚠️ §2 defer -- NX works, order WIP |
| 💎 | SMEP/SMAP BSP Phase 0     | ✅ CR4 in HalInitSystem     | ✅ setup_cr4 early            | ⚠️ §2 defer -- bare metal skips    |
| 💎 | AP hardening = BSP        | ✅ APs run HalInitProc      | ✅ cpu_init per secondary     | ⚠️ §4 defer -- basic in smp.c      |
| 💎 | XSAVE after VMM ready     | ✅ CR4.OSXSAVE post-paging  | ✅ fpu__init_cpu deferred     | ⚠️ §5 defer -- AVX works           |
| 💎 | PCID after page tables    | ✅ CR4.PCIDE post-PML4      | ✅ cr4_set_bits post-paging   | ⬜ §5 defer                       |
| 💎 | AP feature consistency    | ✅ BugCheck 0x3E            | ✅ verify_cpu.S per AP        | ⬜ §6                             |
| 💎 | CR4 bit pinning           | ✅ HAL protects CR4         | ✅ cr4_pinned_bits            | ⬜ §7                             |
| 💎 | PAT MSR AP sync           | ✅ pat_init per CPU         | ✅ pat_cpu_init per AP        | ⬜ §8                             |
| ⭐ | HV detect before timer    | ✅ Before HAL timer         | ⚠️ May lag clocksource        | ✅ §3 -- done                      |
| ⭐ | CPU register audit trail  | ❌ ETW fragments only       | ❌ dmesg fragments only       | ⬜ §9 -- structured per-CPU dump   |
| ⭐ | POST code per CPU step    | ❌ BIOS POST only           | ❌ dmesg only                 | ⬜ §1--9 -- planned in TODO-05 §1  |

> **§1 and §3 complete.** §2, §4, §5 deferred -- blocked by TODO-17/TODO-19 implementations. §6--§9 are new parity and competitive-edge sections. Minimal CPU hardening works via `cpu_harden()` + `cpu_harden_post_pagetable()` (TODO-05 §11). Full formal sequencing comes when TODO-17 delivers `cpu_efer_harden()`/`cpu_cr4_harden()`.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_cpu_seq()` (-> `src/kernel/test/test_runner.c`, `include/kernel/test/test.h`).
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
  - §6: `cpu_data[ap_id].feature_mismatch == false` for every online AP (no feature degradation on homogeneous system)
  - §7: `cr4_pinning_active == true` after boot; `(read_cr4() & cr4_pinned_mask) == cr4_pinned_mask` (pinned bits intact)
  - §8: `rdmsr(IA32_PAT)` on test CPU matches `g_bsp_pat_msr` (PAT synchronized)
  - §9: `cpu_data[0].audit.efer != 0` (audit data populated); `cpu_data[0].audit.cr4 & CR4_PAE` (PAE always set in long mode)
- [ ] Register in `test_runner_init()`: `test_register_cpu_seq()`
- [ ] Commit: `"test: add cpu_seq test suite"`

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
