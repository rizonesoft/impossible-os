# TODO-04 — CPU Boot Sequencing & AP Hardening

> **Goal:** Establish the correct activation order for CPU security and context features during boot phases 0 and 1, and replicate that activation on every Application Processor (AP). The features themselves are implemented in `02-kernel-core/TODO-17` (security hardening) and `02-kernel-core/TODO-19` (x86-64 architecture); this TODO owns the boot-sequencing contract between them — what activates, in what phase, and who ensures APs match the BSP.

> [!IMPORTANT]
> **Scope boundary:** Do NOT implement CPU security features or CPU architecture features here. That work lives in:
> - NX/EFER, SMEP/SMAP, KPTI, PCID, Spectre, CET → `02-kernel-core/TODO-17`
> - XSAVE, MSR layer, UMIP, 1 GiB pages, topology, errata, VM detection → `02-kernel-core/TODO-19`
> - TSC frequency calibration → `02-kernel-core/TODO-07`
> This TODO owns: phase placement, activation order within phases, AP-trampoline replication, and hypervisor pre-detection before the timer subsystem selects its driver.

---

## Inputs

- `src/kernel/main/boot_init.c` — `boot_phase0/1/2/3()` entry points; this TODO annotates the phase boundaries
- `include/kernel/boot_init.h` — `BOOT_STEP`, `BOOT_REQUIRE`, `POSTCODE_*`; add CPU postcodes
- `src/kernel/smp/ap_trampoline.asm` + `src/kernel/smp/smp.c` — AP startup path; add `ap_cpu_harden()` call
- `include/kernel/cpuid.h` — `cpuid_init()`, `cpu_has()`; already called in Phase 0
- `src/kernel/smp/smp.c` — `wrmsr`/`rdmsr` inline helpers; input to MSR layer (→ TODO-19 §3)
- `src/kernel/mm/vmm.c` — VMM init; EFER.NXE must be set before first page table write with NX bit
- → XREF: `02-kernel-core/TODO-01 §2` — `boot_phase0` sequence; CPU hardening steps slot in here
- → XREF: `02-kernel-core/TODO-17 §1–§7` — NX, SMEP, SMAP, PCID, Spectre, CET (implementation details)
- → XREF: `02-kernel-core/TODO-19 §1, §3–§7, §9, §11` — XSAVE, MSR layer, UMIP, 1 GiB pages, errata, VM detection
- → XREF: `01-boot-platform/TODO-03 §6` — UTS timer driver selection; hypervisor detection (§3 here) feeds it

---

## Outcome

- All CPU security features (EFER.NXE, CR4 flags) are provably activated before VMM maps any page with NX bits.
- Every AP runs `ap_cpu_harden()` at startup — no AP boots with SMEP/SMAP/EFER absent while BSP has them.
- Hypervisor vendor is detected and stored in `boot_info` before the UTS probes its timer drivers, enabling correct Hyper-V TSC and TLB enlightenments from the start.
- XSAVE and PCID are activated only after VMM is ready, in the correct Phase 1 window.
- A boot log line documents every CPU feature activation with its phase and postcode: `[Phase0] EFER.NXE enabled (POSTCODE 0x22)`

---

## Implementation Order

| ⭐  | Order | Deliverable                                      | Depends On                 | Status |
| --- | :---: | ------------------------------------------------ | -------------------------- | :----: |
| 💎  |   1   | CPUID detection & per-CPU capability capture     | P0, D02 T19 §1             |  [x]   |
| 💎  |   2   | Phase 0 CPU security activation order            | §1, D02 T17 §1–3 & T19 §3  | defer  |
| ⭐  |   3   | Hypervisor detection before timer selection      | §1, D02 T19 §11            |  [x]   |
| 💎  |   4   | AP CPU hardening (`ap_cpu_harden()`)             | §2, D02 T17 §1–7           | defer  |
| 💎  |   5   | Phase 1 XSAVE & PCID activation window           | §2, D02 T17 §4 & T19 §1    | defer  |

> 💎 = parity — Windows and Linux both enforce EFER/CR4 ordering, AP parity, and deferred XSAVE/PCID relative to paging; Impossible OS must match that contract.
> ⭐ = exclusive — hypervisor pre-detection before timer HAL selection and postcode-per-activation boot audit lines are not surfaced the same way on Windows or Linux.

---

## 1. CPUID Detection & Per-CPU Capability Capture

**Prompt:** `cpuid_init()` already runs in Phase 0 and populates feature flags, but the AMD extended leaves (`0x80000001`, `0x8000001E`, `0x80000008`) and per-CPU storage in `struct cpu_data` are not complete. Extend `cpuid_init()` to probe all leaves needed by TODO-17 and TODO-19: SSE2, SSE4.2, AVX, AVX2, AVX512F, AES-NI, RDRAND, RDSEED, CET_SS, CET_IBT, UMIP, SMEP, SMAP, PCID, INVPCID, FSGSBASE, FFXSR; AMD leaves 0x80000001 (1-GB pages, RDTSCP, PDPE1GB, SVM), 0x8000001E (Zen topology), 0x80000008 (phys/virt address bits); store result in `cpu_data[0].cpuid_features`; emit `[Phase0] CPUID probed, POSTCODE_CPUID_DONE` to `boot_progress()`.

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-19 §1` — full feature struct definition lives there; this TODO only gates the call into Phase 0.

- [x] `cpuid_init()` runs in Phase 0 after heap_init — confirmed in boot_hw.c line 141
- [x] AMD extended leaves 0x80000001 (NX, SVM, OSVW, IBS, Page1GB, RDTSCP), 0x8000001E (Zen topology), 0x80000008 (phys/linear addr bits) all parsed
- [x] Result stored in global `g_cpu` struct (type `struct cpu_features`) — 54 feature flags + address bits + topology
- [x] `POSTCODE_CPUID_INIT = 0x24` already defined; `boot_progress(0, "CPUID", POSTCODE_CPUID_INIT)` emitted
- [x] Already implemented — marking complete

---

## 2. Phase 0 CPU Security Activation Order *(deferred — blocked by TODO-17 `cpu_efer_harden`/`cpu_cr4_harden` + TODO-19 `msr_init`; minimal version in TODO-06 §11)*
**Prompt:** Document and enforce the required activation sequence in `boot_phase0()`: (1) `msr_init()` (centralized MSR layer, TODO-19 §3) must run before any MSR write; (2) `cpu_efer_harden()` sets `EFER.NXE=1`, `EFER.SCE=1`, `EFER.FFXSR` if supported — **this must complete before VMM init** because VMM will write PTE bit 63 (NX) on the first `vmm_map_page()` call; (3) `cpu_cr4_harden()` sets `CR4.SMEP`, `CR4.SMAP`, `CR4.UMIP` if detected — must run before any user-mode-visible mapping; emit a `POSTCODE_CPU_HARDEN_DONE` after the last CR4 write. Add `BOOT_REQUIRE(SUBSYS_MSR)` guards to EFER/CR4 steps so wrong-order calls panic with a clear message. Log: `[Phase0] EFER=0x{val} CR4=0x{val}`.

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-17 §1–3` — `cpu_efer_harden()` and `cpu_cr4_harden()` implementations live there.
> → XREF: `02-kernel-core/TODO-19 §3` — `msr_init()` implementation lives there (MSR Management Infrastructure).
> → XREF: `02-kernel-core/TODO-01 §2` — Phase 0 boot sequence; this step slots between serial init and PMM.

- [ ] Add `POSTCODE_MSR_INIT`, `POSTCODE_CPU_EFER`, `POSTCODE_CPU_CR4`, `POSTCODE_CPU_HARDEN_DONE` to `boot_init.h`
- [ ] Insert `BOOT_STEP(SUBSYS_MSR, msr_init)` in `boot_phase0()` before VMM step
- [ ] Insert `BOOT_STEP(SUBSYS_CPU_EFER, cpu_efer_harden)` immediately after MSR init
- [ ] Insert `BOOT_STEP(SUBSYS_CPU_CR4, cpu_cr4_harden)` after EFER step
- [ ] Add `BOOT_REQUIRE(SUBSYS_MSR)` inside `cpu_efer_harden()` and `cpu_cr4_harden()`
- [ ] Verify (serial log): `[Phase0] EFER.NXE=1 SMEP=1 SMAP=1 UMIP=1` appears before first VMM log line
- [ ] Commit: `"boot: enforce Phase 0 CPU security activation order before VMM init"`

---

## 3. Hypervisor Detection Before Timer Selection

The UTS probe in TODO-03 §6 selects HPET vs PIT vs LAPIC timer, but on Hyper-V the correct choice is the `HV_X64_MSR_TIME_REF_COUNT` reference counter. Hypervisor detection must run before UTS `timer_probe()`. In Phase 0, after CPUID: probe CPUID `0x40000000` for the hypervisor-present bit; if set, read vendor string (`0x40000001`); store `hv_vendor` in `boot_info` and `HKLM\HARDWARE\VM\HypervisorVendor`; for `"Microsoft Hv"`: set `boot_info.hv_flags |= HV_TSC_ENLIGHTENMENT | HV_TLBFLUSH_HYPERCALL`; for `"KVMKVMKVM"`: set `HV_KVM_STEAL_TIME`; UTS probe reads `boot_info.hv_flags` to select the correct clock source first. Emit `[Phase0] Hypervisor: Microsoft Hv (flags=0x3)` or `[Phase0] Bare metal`.

> [!IMPORTANT]
> → XREF: `01-boot-platform/TODO-03 §6` — UTS reads `boot_info.hv_flags` to select clock; must be set before `timer_probe()`.
> → XREF: `02-kernel-core/TODO-19 §11` — AMD-V / VT-x host capability detection; Hyper-V guest enlightenment MSR initialization (using `boot_info.hv_flags` populated by this step) is not yet specced in §11 and needs to be added there.

- [x] Add `hv_vendor[16]` and `hv_flags` fields to `struct boot_info`
- [x] Define `HV_FLAG_TSC_ENLIGHTENMENT`, `HV_FLAG_TLBFLUSH_HYPERCALL`, `HV_FLAG_KVM_STEAL_TIME`, `HV_FLAG_APIC_FREQ_MSR`, `HV_FLAG_VMWARE_BACKDOOR` in `boot_init.h`
- [x] `platform_detect()` in `cpuid_platform.c` populates `g_boot_info.hv_vendor` + `g_boot_info.hv_flags` for Hyper-V, KVM, VMware, VirtualBox
- [x] Detection runs from `timer_hal_init()` → `platform_detect()` in Phase 1, before timer backend selection
- [x] UTS timer probe uses `platform_has_apic_freq_msr()` which reads cached platform state
- [x] Commit: `"boot: hypervisor detection with hv_flags in boot_info"`

---

## 4. AP CPU Hardening (`ap_cpu_harden()`) *(deferred — blocked by §2; current `cpu_harden()` + `cpu_harden_post_pagetable()` applied on APs in smp.c)*
**Prompt:** When APs start up via `smp_ap_main()`, they run their own GDT/IDT/LAPIC setup but currently skip the EFER and CR4 security features that BSP Phase 0 enables. An AP running without `EFER.NXE`/`SMEP`/`SMAP` is a full privilege bypass vector — kernel code on that AP can execute user pages and access user memory unchecked. Implement `ap_cpu_harden()` that replicates the BSP Phase 0 CPU activation: `msr_write(IA32_EFER, bsp_efer_val)`, then `cr4_write(bsp_cr4_val)` (read from BSP at Phase 0, stored in `cpu_data[0].efer_at_boot` and `cpu_data[0].cr4_at_boot`); call from `smp_ap_main()` immediately after GDT load and before AP signals ready; each AP logs `[AP%u] CPU hardening applied EFER=0x{val} CR4=0x{val}`.

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-17 §1–7` — notes that AP trampoline must enable same CR4/MSR features; this TODO writes the call site; TODO-17 provides the underlying `cpu_efer_harden()` function reused on APs.

- [ ] Add `efer_at_boot` and `cr4_at_boot` fields to `struct cpu_data`; BSP stores values at end of Phase 0
- [ ] Implement `ap_cpu_harden()` in `boot_init.c`: reads BSP values, applies to current CPU
- [ ] Call `ap_cpu_harden()` in `smp_ap_main()` before `ap_ready_flag = 1`
- [ ] Verify (serial log): `[AP1] CPU hardening applied` appears for each non-BSP CPU
- [ ] Verify: no AP starts with `EFER.NXE=0` when BSP has `EFER.NXE=1`
- [ ] Commit: `"smp: apply CPU hardening on AP startup via ap_cpu_harden()"`

---

## 5. Phase 1 XSAVE & PCID Activation Window *(deferred — blocked by TODO-17 §4 + TODO-19 §1; XSAVE already enabled via `simd_enable_avx()` in Phase 0)*
**Prompt:** XSAVE and PCID require VMM to be ready first — XSAVE because per-thread XSAVE areas are allocated in TEB pages managed by VMM, and PCID because `CR4.PCIDE` changes how CR3 is loaded (PCID bits 11:0) and must be set only after the page table base is established. Slot these activations into Phase 1: (1) after `vmm_init()`: call `cpu_xsave_enable()` — sets `CR4.OSXSAVE`, calls `XSETBV(XCR0, X87|SSE|AVX)`, stores `xsave_area_size` in a global; (2) after process table is initialised: call `cpu_pcid_enable()` — sets `CR4.PCIDE`, validates INVPCID is available; PCID 0 reserved for kernel; emit `POSTCODE_XSAVE_ENABLED` and `POSTCODE_PCID_ENABLED`. Both functions are no-ops if the CPU does not support the feature.

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-19 §1` — `cpu_xsave_enable()` implementation (XSAVE area sizing, XCR0 bits).
> → XREF: `02-kernel-core/TODO-17 §4` — `cpu_pcid_enable()` and CR3 load changes for KPTI (PCID implementation).
> → XREF: `02-kernel-core/TODO-04 §6` — TEB Allocation and Population at Thread Create; XSAVE area is allocated alongside the TEB at thread creation, which is why VMM must be ready before XSAVE is enabled.

- [ ] Add `POSTCODE_XSAVE_ENABLED`, `POSTCODE_PCID_ENABLED` to `boot_init.h` Phase 1 constants
- [ ] Insert `BOOT_STEP(SUBSYS_XSAVE, cpu_xsave_enable)` in `boot_phase1()` after VMM init
- [ ] Insert `BOOT_STEP(SUBSYS_PCID, cpu_pcid_enable)` in `boot_phase1()` after process table init
- [ ] Verify (serial log): `[Phase1] XSAVE enabled (area=N bytes)` and `[Phase1] PCID enabled`
- [ ] Verify: XSAVE enable does not run in Phase 0 (VMM not yet up at that point)
- [ ] Commit: `"boot: activate XSAVE and PCID in Phase 1 after VMM ready"`

---

## OS Comparison

| ⭐ | Feature                   | Win11                        | Linux                         | Impossible OS                      |
|----|---------------------------|------------------------------|-------------------------------|------------------------------------|
| 💎 | EFER.NXE before NX pages  | ✅ HalInitializeProcessor   | ✅ cpu_init before paging     | ⚠️ §2 defer — NX works, order WIP |
| 💎 | SMEP/SMAP BSP Phase 0     | ✅ CR4 in HalInitSystem     | ✅ setup_cr4 early            | ⚠️ §2 defer — bare metal skips    |
| 💎 | AP hardening = BSP        | ✅ APs run HalInitProc      | ✅ cpu_init per secondary     | ⚠️ §4 defer — basic in smp.c      |
| 💎 | XSAVE after VMM ready     | ✅ CR4.OSXSAVE post-paging  | ✅ fpu__init_cpu deferred     | ⚠️ §5 defer — AVX works           |
| 💎 | PCID after page tables    | ✅ CR4.PCIDE post-PML4      | ✅ cr4_set_bits post-paging   | ⬜ §5 defer                       |
| ⭐ | HV detect before timer    | ✅ Before HAL timer         | ⚠️ May lag clocksource        | ✅ §3 — done                      |
| ⭐ | POST code per CPU step    | ❌ BIOS POST only           | ❌ dmesg only                 | ⬜ §1-5 — planned in TODO-06 §1   |

> **§1 and §3 complete.** §2, §4, §5 deferred — blocked by TODO-17/TODO-19 implementations. Minimal CPU hardening works via `cpu_harden()` + `cpu_harden_post_pagetable()` (TODO-06 §11). Full formal sequencing comes when TODO-17 delivers `cpu_efer_harden()`/`cpu_cr4_harden()`.

---

## Verification

- [ ] Serial log shows `EFER.NXE=1` line **before** first `[VMM]` line in boot output
- [ ] Serial log shows `[AP%u] CPU hardening applied` for every AP that comes online
- [ ] Serial log shows `[Phase0] Hypervisor: ...` **before** first `[UTS]` / `[Timer]` line
- [ ] Serial log shows `[Phase1] XSAVE enabled` and `[Phase1] PCID enabled` **after** `[VMM] init complete`
- [ ] Boot completes with `=== BUILD OK ===` and no regressions in QEMU + Hyper-V
- [ ] Commit: `"boot: cpu-sequencing verified — EFER/CR4 order, AP hardening, hypervisor detection, XSAVE/PCID phasing"`
