---
schema_version: 1
id: security-hardware
domain: 04-drivers-hardware
status: active
title: "TODO-04 -- Security Hardware & DMA Safety"
---

# TODO-04 -- Security Hardware & DMA Safety

> **Goal:** Add the hardware-rooted security primitives that Windows 11 (Secure Boot + TPM + VBS) and Linux (IOMMU + CET + RNG) rely on -- RDRAND/RDSEED entropy, IOMMU/VT-d DMA isolation, a bounce buffer manager for 32-bit DMA devices, a complete TPM 2.0 command driver, Secure Boot variable exposure, Intel CET shadow stacks, SMEP+SMAP enforcement on all APs, and a boot-time kernel integrity check via TPM PCR extension.

> [!IMPORTANT]
> **Partial foundations exist:** `src/kernel/tpm.c` (312 lines) parses the UEFI TCG PCR event log (legacy + crypto-agile format) but does **not** send TPM commands -- it reads the boot-time log only. `src/kernel/cpuid.c` detects `CPU_FEATURE_SMEP`/`SMAP` flags but **does not** enable them in `CR4`. This TODO completes both, plus adds all missing security hardware. Do not rewrite `tpm.c`'s event-log parser -- extend it with the command driver.

## Inputs

- [`src/kernel/tpm.c`](../../src/kernel/tpm.c) -- existing PCR event log parser (310 lines); extend with TPM2 command driver in §6
- [`src/kernel/cpuid.c`](../../src/kernel/cpuid.c) -- `CPU_FEATURE_SMEP`/`SMAP` detection; §1 adds the `CR4` enable calls
- [`src/kernel/main/boot_hw.c`](../../src/kernel/main/boot_hw.c) -- BSP hardware init sequence; SMEP/SMAP enable (§1) and CET (§8) go here
- → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §1` -- ACPI DMAR (VT-d) and IVRS (AMD-Vi) tables parsed via ACPICA; §4 (IOMMU) depends on ACPICA being initialised
- → XREF: `04-drivers-hardware/TODO-02-apic-interrupt-routing.md` -- IOMMU interrupt remapping (§4) requires the interrupt allocation infrastructure from the APIC TODO
- → XREF: `01-boot-platform/TODO-02-uefi-hardening-secureboot.md` -- Secure Boot UEFI variable read (§5) is a runtime-services call at the end of the UEFI handoff; coordinate with the boot-platform hardening work
- → XREF: `02-kernel-core` domain -- `hwrng_read()` (§2) should feed into the kernel entropy pool; any KASLR or stack-canary seeding should call `hwrng_read()` before it is available from user mode
- → XREF: `01-boot-platform/TODO-09-cpu-boot-sequencing.md §4, §6, §7` -- AP hardening boot sequencing (§4), AP feature consistency (§6), and CR4 pinning (§7) own the activation order and consistency enforcement; §1 here owns the SMEP/SMAP CR4 write implementation and copy_from/to_user wrappers

## Outcome

- `hwrng_read(buf, len)` provides cryptographically secure random bytes from `RDRAND`/`RDSEED`; falls back to a HPET+TSC-seeded ChaCha20 CSPRNG.
- IOMMU/VT-d is initialised from ACPI DMAR; all DMA-capable devices mapped through IOMMU page tables; default-deny policy blocks undeclared DMA.
- Bounce buffer manager provides `dma_alloc(dev, size)` for legacy 32-bit DMA devices (RTL8139, AC97, EHCI).
- TPM 2.0 command driver: `STARTUP`, `GetCapability`, `PCR_Read`/`PCR_Extend`, `GetRandom`; IOCTL surface for user mode.
- Secure Boot state exposed at `HKLM\SYSTEM\SecureBoot\Enabled`.
- Intel CET shadow stacks active for user mode; kernel entry/exit uses `INCSSPQ`/`RSTORSSP`.
- SMEP + SMAP enabled in `CR4` on BSP **and** all APs.
- Kernel `.text`/`.rodata` SHA-256 hash extended into TPM PCR[10] at boot; mismatch logged as a critical security event.

## Implementation Order

| ⭐   | Order | Deliverable                              | Depends On                               | Status |
| --- | :---: | ---------------------------------------- | ---------------------------------------- | :----: |
| 💎   |   1   | §1 SMEP + SMAP `CR4` enable on BSP + all APs | `cpuid.c` detection (exists)             |  [ ]   |
| 💎   |   2   | §2 Hardware RNG -- `RDRAND`/`RDSEED`, ChaCha20 CSPRNG fallback | §1 (CR4 bits safe before entropy use)    |  [ ]   |
| 💎   |   3   | §3 DMA bounce buffer manager -- PMM low zone, `dma_alloc/map/unmap` | PMM (existing)                           |  [ ]   |
| 💎   |   4   | §4 IOMMU / VT-d + AMD-Vi -- DMAR/IVRS parse, page tables, default-deny | §3 (bounce bufs needed before IOMMU default-deny), ACPICA (TODO-03) |  [ ]   |
| 💎   |   5   | §5 Secure Boot UEFI variable read → Registry | boot UEFI runtime services (existing)    |  [ ]   |
| 💎   |   6   | §6 TPM 2.0 command driver -- extend `tpm.c`, STARTUP/PCR/GetRandom | §2 (entropy seeding from TPM)            |  [ ]   |
| ⭐   |   7   | §7 Boot-time kernel integrity check -- SHA-256 + TPM PCR_Extend | §6 (TPM command driver)                  |  [ ]   |
| ⭐   |   8   | §8 Intel CET shadow stacks -- `CR4.CET`, `MSR_IA32_U_CET`, syscall SSP | §4 (CR4 baseline set), §6 (entropy for canary) |  [ ]   |

> §7 kernel integrity check and §8 CET are `⭐` exclusive: Windows 11 requires VBS + HVCI for kernel integrity; Linux requires CONFIG_CFI_CLANG or CONFIG_SHADOW_CALL_STACK. Impossible OS implements both as first-class features in the base kernel -- no hypervisor required for integrity measurement, no compiler plugin required for CET.

---

## 1. SMEP + SMAP Late Init on All APs `[Sonnet]`

`CR4.SMEP` (bit 20) and `CR4.SMAP` (bit 21) are detected in `cpuid.c` but not enabled in `CR4`. Enable them on the BSP during early boot and ensure each Application Processor enables them after SMP bringup -- currently the AP trampoline does not set these bits.

**Files:** `src/kernel/main/boot_hw.c` (extend BSP), `src/kernel/smp/ap_trampoline.asm` or equivalent AP init code (extend)

> [!NOTE]
> SMEP prevents the kernel from executing code in user-mode pages (ring 0 jumps to user addresses fault). SMAP prevents the kernel from accessing user-mode data without `STAC`/`CLAC` brackets. Both require `CPU_FEATURE_SMEP`/`CPU_FEATURE_SMAP` checks before enabling; enabling on a CPU that does not support them causes a #GP.

- [ ] `boot_hw.c` BSP: after CPUID flags populated, `if (cpu_has(CPU_FEATURE_SMEP)) cr4 |= CR4_SMEP (1<<20)`; `if (cpu_has(CPU_FEATURE_SMAP)) cr4 |= CR4_SMAP (1<<21)`; `write_cr4(cr4)`
- [ ] AP init path: add same two CR4 writes to the end of the AP initialisation sequence (after GDT/IDT/paging set up, before the AP signals ready); AP reads its own CPUID flags to guard the writes
- [ ] `smep_smap_assert()`: debug helper that reads `CR4` and asserts bits 20+21 match `cpu_has()` expectations; call from `smp_bringup_complete()` for each AP
- [ ] `copy_from_user(dst, src, len)` / `copy_to_user(dst, src, len)` wrappers: bracket with `STAC` / `CLAC` around the copy loop to satisfy SMAP; fault handler correctly handles SMAP violations in these paths
- [ ] Boot log: `[CPU] SMEP=%s SMAP=%s` for BSP and each AP at `LOG_DEBUG`
- [ ] Commit: `"kernel: SMEP+SMAP -- CR4 enable on BSP+APs, copy_from/to_user STAC/CLAC, smep_smap_assert"`

## 2. Hardware RNG -- `RDRAND` / `RDSEED` + ChaCha20 CSPRNG Fallback `[Opus]`

Detect `RDRAND` (CPUID leaf `01h` ECX bit 30) and `RDSEED` (leaf `07h` EBX bit 18). Implement `hwrng_read(buf, len)` with up to 10 retry loops for CF=0. Fall back to a HPET+TSC-seeded ChaCha20 CSPRNG. Seed the kernel entropy pool.

**Files:** `src/kernel/hwrng.c` (new), `include/kernel/hwrng.h` (new)

> [!NOTE]
> `RDRAND` contract: the CF flag in EFLAGS is set to 1 on success, 0 on underrun. Up to 10 retries is the Intel-recommended limit. `RDSEED` is for seeding external CSPRNGs; `RDRAND` is for direct consumption. Prefer `RDSEED` for ChaCha20 seed; prefer `RDRAND` for individual random values.

- [ ] `hwrng_has_rdrand()` / `hwrng_has_rdseed()`: read from cached `g_cpu.flags` (CPUID detection in `cpuid.c`)
- [ ] `hwrng_rdrand64(out)` → inline ASM `rdrand rax`; check CF; retry up to 10×; return `true` on success
- [ ] `hwrng_rdseed64(out)` → inline ASM `rdseed rax`; same retry logic; return `true` on success
- [ ] ChaCha20 CSPRNG: implement 20-round ChaCha20 block function (`chacha20_block(key[8], counter, nonce[3], out[16])`); seed `key[]` with HPET `MCOUNTER` XOR `RDTSC` at init; reseed with `RDSEED` if available
- [ ] `hwrng_read(buf, len)`: if `RDRAND` available, fill 8 bytes at a time via `hwrng_rdrand64`; remainder via ChaCha20 CSPRNG; if `RDRAND` unavailable, use ChaCha20 exclusively
- [ ] Entropy pool seeding: export `hwrng_read` symbol; call from KASLR offset generation and stack-canary init
- [ ] `hwrng_reseed()`: callable after TPM `GetRandom` (§6) provides additional entropy; XOR TPM bytes into ChaCha20 key
- [ ] Boot log: `[HWRNG] RDRAND=%s RDSEED=%s source=%s` (`hw` / `sw-ChaCha20`)
- [ ] Commit: `"kernel: hwrng -- RDRAND/RDSEED, ChaCha20 CSPRNG fallback, entropy pool seeding"`

## 3. DMA Bounce Buffer Manager `[Sonnet]`

Provide `dma_alloc(dev, size)` for legacy 32-bit DMA devices (RTL8139, AC97, EHCI) that cannot address memory above 4 GiB. Allocate from a PMM low zone (< 4 GiB physical). Support scatter-gather `dma_map/unmap_single`.

**Files:** `src/kernel/dma.c` (new), `include/kernel/dma.h` (new)

> [!NOTE]
> On modern systems with > 4 GiB RAM, the PMM may allocate kernel buffers above the 32-bit DMA boundary. Legacy DMA devices need a "bounce buffer" in the low zone. This manager reserves a contiguous low-zone pool at boot (size configurable, default 16 MiB) and sub-allocates from it. After IOMMU is enabled (§4), `dma_alloc` additionally calls `iommu_map()` to register the buffer with the IOMMU.

- [ ] `DMA_LOW_ZONE_SIZE = 16 MiB` default; `dma_pool_init()` at early boot: call `pmm_alloc_contiguous_below(DMA_LOW_ZONE_SIZE, 0x100000000ULL)` to reserve pool below 4 GiB physical
- [ ] `dma_alloc(dev, size, &phys)` → sub-allocate from the low-zone pool (first-fit or buddy); return kernel virtual address + write `phys` out; `phys` is always < 4 GiB
- [ ] `dma_free(dev, virt, size)` → return to pool
- [ ] `dma_map_single(dev, virt, len, dir)` → if `phys > 4 GiB`: allocate bounce buffer, copy data (for `DMA_TO_DEVICE`); return bounce `phys`; record mapping in per-device table
- [ ] `dma_unmap_single(dev, phys, len, dir)` → if bounce buffer: copy back (for `DMA_FROM_DEVICE`); free bounce buffer
- [ ] `dma_sync_for_device(dev, phys, len)` / `dma_sync_for_cpu(dev, phys, len)` -- cache-coherence operations (`wbinvd` / `clflush` range) after IOMMU mapping
- [ ] After IOMMU init (§4): `dma_alloc` additionally calls `iommu_map(dev, phys, phys, len, IOMMU_READ|IOMMU_WRITE)` to register bounce buffers with IOMMU
- [ ] Commit: `"kernel: DMA bounce buffer manager -- low-zone pool, dma_alloc/map/unmap_single, IOMMU hook"`

## 4. IOMMU / VT-d + AMD-Vi `[Opus]`

Parse the ACPI DMAR table (Intel VT-d) and IVRS table (AMD-Vi). Build per-device IOMMU page tables and enable hardware DMA remapping. Default-deny policy: only explicitly mapped physical ranges are DMA-accessible.

**Files:** `src/kernel/iommu.c` (new), `include/kernel/iommu.h` (new)

> [!IMPORTANT]
> → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §1` -- ACPICA must be initialised before DMAR/IVRS table walks. IOMMU init must happen **after** ACPICA init but **before** any DMA-capable driver is started. The bounce buffer manager (§3) must also be ready so legacy 32-bit DMA devices can function under default-deny.

- [ ] ACPI DMAR parse: locate `"DMAR"` signature; iterate DRHD (DMA Remapping Hardware Definition) structures; each DRHD has `Register Base Address` and `Device Scope` entries (PCI Segment/Bus/Dev/Fn)
- [ ] VT-d register map: `BAR = drhd->base`; `GCMD (0x18)`, `GSTS (0x1C)`, `RTADDR (0x20)`, `CCMD (0x28)`, `IOTLB (0xB8)`, `CAP (0x08)`, `ECAP (0x10)`
- [ ] Root table: allocate 4 KiB root table (256 entries × 16 bytes); each entry → context table (256 × 16 byte entries); `RTADDR_REG = phys(root_table)`; `GCMD.SRTP=1`; `GCMD.TE=1`
- [ ] `iommu_map(dev, iova, phys, len, prot)`: build/update 4-level IOMMU page table (512-entry each, 4 KiB pages); set `READ`/`WRITE` bits per `prot`; invalidate IOTLB after mapping
- [ ] `iommu_unmap(dev, iova, len)`: clear page table entries; IOTLB invalidate
- [ ] Default-deny: do not pre-populate any mappings; kernel drivers call `iommu_map()` before issuing DMA; DMA to unmapped IOVA → IOMMU fault → `iommu_fault_handler()` logs and kills offending device
- [ ] AMD-Vi: ACPI IVRS parse; `MMIO_BASE` from `IVHD`; `DEVICE_TABLE_BASE_REG`, `COMMAND_BUF_BASE_REG`, `EVENT_LOG_BASE_REG`; AMD-Vi page table format (same 4-level structure with different flag bits); `CONTROL_REG.IOMMU_EN=1`
- [ ] Quiesce: `iommu_shutdown()` disables `GCMD.TE` for graceful S3/S4 suspend
- [ ] Boot log: `[IOMMU] Intel VT-d: %u DRHD units` / `[IOMMU] AMD-Vi: %u IVHD units` / `[IOMMU] Default-deny DMA enabled`
- [ ] Commit: `"kernel: IOMMU -- VT-d DMAR parse, 4-level page tables, default-deny, AMD-Vi IVRS"`

## 5. Secure Boot UEFI Variable Read `[Sonnet]`

Call `EFI_RUNTIME_SERVICES.GetVariable("SecureBoot", &EFI_GLOBAL_VARIABLE, ...)` during the post-ExitBootServices boot phase to retrieve the Secure Boot active flag. Expose as `HKLM\SYSTEM\SecureBoot\Enabled` Registry value.

**Files:** `src/kernel/main/boot_hw.c` (extend), `include/kernel/secboot.h` (new)

> [!NOTE]
> `EFI_GLOBAL_VARIABLE` GUID = `{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}`. `SecureBoot` variable value: 1 byte -- `0x01` = Secure Boot active, `0x00` = inactive. This call must happen **before** `ExitBootServices()` in the bootloader, or via UEFI Runtime Services after `ExitBootServices` (Runtime Services remain valid after EBS). Check the current boot path and call from the appropriate phase.

- [ ] In `boot_hw.c` post-EBS sequence: call `gRT->GetVariable(L"SecureBoot", &EFI_GLOBAL_VARIABLE, NULL, &data_size, &secure_boot_val)`
- [ ] Store result in `boot_info.secure_boot_enabled` (add field to `boot_info_t` if not present)
- [ ] In kernel init: read `boot_info.secure_boot_enabled`; write `HKLM\SYSTEM\SecureBoot\Enabled` (`REG_DWORD`, 0 or 1)
- [ ] `secboot_is_enabled()` → inline helper reading the Registry value; used by module loader to reject unsigned modules when `Enabled=1`
- [ ] Also read `SetupMode` variable (same GUID): if `SetupMode=1`, Secure Boot is in setup mode; log warning `[SECBOOT] Setup mode -- database writeable`
- [ ] Boot log: `[SECBOOT] Secure Boot: %s` (`enabled` / `disabled` / `setup mode`)
- [ ] Commit: `"kernel: Secure Boot state -- EFI GetVariable SecureBoot, Registry HKLM\\SYSTEM\\SecureBoot"`

## 6. TPM 2.0 Command Layer and IOCTL Surface `[Opus]`

Build the TPM 2.0 command layer ON TOP of the shipped transport: discovery, TIS/CRB submission, locality, timeouts, and `TPM2_Startup` all live in `src/kernel/tpm_transport.c` (`tpm2_submit()`; shipped at `01-boot-platform/TODO-13` §2 -- do NOT re-implement transport here). This section owns the command marshaling above the transport plus the user-visible device surface.

**Files:** `src/kernel/tpm.c` (extend), `include/kernel/tpm.h` (extend); transport contract: [`include/kernel/tpm_transport.h`](../../include/kernel/tpm_transport.h)

> [!NOTE]
> Transport, ACPI TPM2 discovery, CRB control-area layout, and Startup detection shipped in `01-boot-platform/TODO-13` §2; the canonical register/offset reference is `src/kernel/tpm_transport.c`. PCR read marshaling is owned by `01-boot-platform/TODO-13` §3; coordinate to avoid duplication.

- [ ] `tpm2_get_capability(prop, count, out)`: CC `0x017A` via `tpm2_submit()`; parse `TPML_TAGGED_TPM_PROPERTY` response
- [ ] `tpm2_pcr_read(pcr_index, bank, out_digest)`: CC `0x017E`; `TPML_PCR_SELECTION`; parse `TPML_DIGEST`; coordinate with `01-boot-platform/TODO-13` §3 (PCR read API owner)
- [ ] `tpm2_pcr_extend(pcr_index, digest_sha256)`: CC `0x0182`; `TPML_DIGEST_VALUES` with one `TPMT_HA` (hash_alg=`TPM_ALG_SHA256`, digest[32])
- [ ] Runtime reseed feed over the shipped `tpm2_get_random_bounded()` (`src/kernel/tpm_transport.c`; GetRandom marshaling helpers already exist) -- first-seed collection shipped at `01-boot-platform/TODO-12` §4 (`entropy_collect_tpm()`)
- [ ] IOCTL surface: `NtDeviceIoControlFile` on `\Device\TPM0` → dispatch `IOCTL_TPM_PCR_READ`, `IOCTL_TPM_GET_RANDOM`, `IOCTL_TPM_PCR_EXTEND` to above commands
- [ ] Boot log: `[TPM2] command layer ready, firmware version=%u.%u` (interface + Startup already logged by the transport)
- [ ] Commit: `"kernel: TPM2 command layer -- GetCapability/PCR/GetRandom over tpm2_submit, IOCTL surface"`

## 7. Boot-Time Kernel Integrity Check `[Opus]`

At boot, compute SHA-256 of the kernel `.text` and `.rodata` sections. Extend TPM PCR[10] with the hash. Compare against any stored baseline. Log a critical security event on mismatch.

**Files:** `src/kernel/integrity.c` (new), `include/kernel/integrity.h` (new), `src/kernel/tpm.c` (uses §6 command driver)

> [!IMPORTANT]
> This section depends on §6 (TPM command driver) for `tpm2_pcr_extend()` and `tpm2_pcr_read()`. SHA-256 must be implemented without `<stdint.h>` or stdlib -- use `kernel/types.h` types only. The measurement must occur **after** the kernel is fully loaded into memory but **before** any module is loaded, to capture the clean kernel image.

- [ ] Implement `sha256_init(ctx)`, `sha256_update(ctx, data, len)`, `sha256_final(ctx, digest[32])` in `src/kernel/sha256.c` (new) -- FIPS 180-4 compliant; no stdlib
- [ ] `kernel_measure()`: find `.text` section start+size from linker-exported symbols (`__text_start`, `__text_end`, `__rodata_start`, `__rodata_end`); compute SHA-256 over the two sections; 64-byte concatenated hash
- [ ] `tpm2_pcr_extend(10, sha256_digest)` -- extend PCR[10] with the measurement; PCR[10] is the conventional "IMA" PCR; log the extended value
- [ ] Baseline comparison: `tpm2_pcr_read(10, TPM_ALG_SHA256, &current_pcr)` after extending; compare against a "golden" PCR stored in UEFI variable `ImpossibleOS-KernelPCR` (if present from previous boot)
- [ ] On mismatch: `LOG_CRIT("[INTEGRITY] Kernel PCR[10] mismatch -- expected %s got %s")`; set `boot_flags.integrity_violation = 1`; do **not** halt (allow boot to complete but flag the violation for audit)
- [ ] First boot (no baseline): write current PCR value to `ImpossibleOS-KernelPCR` UEFI variable via Runtime Services; log `[INTEGRITY] Baseline stored`
- [ ] Export `kernel_integrity_ok()` → returns `!boot_flags.integrity_violation`; security-sensitive paths (module loader, `secboot_is_enabled()`) call this
- [ ] Boot log: `[INTEGRITY] Kernel SHA-256: %s PCR[10] extended`
- [ ] Commit: `"kernel: integrity -- SHA-256 kernel measure, TPM PCR[10] extend, baseline compare"`

---

## 8. Intel CET Shadow Stacks `[Opus]`

Enable Intel Control-flow Enforcement Technology for user mode. Set `CR4.CET` and `MSR_IA32_U_CET`. Allocate a per-thread shadow stack. Wire `INCSSPQ`/`RSTORSSP` into syscall entry/exit to maintain kernel shadow stack pointer. Kernel `PL0_SSP` configured at task switch.

**Files:** `src/kernel/main/boot_hw.c` (extend), `src/kernel/sched/task.c` (extend), `include/kernel/cet.h` (new)

> [!IMPORTANT]
> CET is a security-critical feature with precise ordering requirements. `CR4.CET` (bit 23) must be set before writing `IA32_U_CET`. `IA32_U_CET` MSR (`0x6A0`) controls user-mode shadow stack: bit 0=`SHSTK_EN`, bit 2=`ENDBR_EN` (indirect branch tracking). Shadow stack pointer `IA32_PL3_SSP` (`0x6A8`) = base of the user shadow stack. For syscall: on entry, `INCSSPQ` advances kernel SSP; on return, `RSTORSSP` restores it. Shadow stack pages are marked with the `Supervisor Shadow Stack` (`SSS=1`) page table attribute -- write-protected except by `WRSS`.

- [ ] CPUID check: `CPUID[07h].ECX[7]` (CET_SS); `CPUID[07h].EDX[20]` (CET_IBT); log if absent and skip
- [ ] `cet_init_bsp()`: `CR4 |= CR4_CET (1<<23)`; write `IA32_U_CET (0x6A0) = 0x1` (SHSTK_EN) + optionally `0x5` (SHSTK+ENDBR); write `IA32_S_CET (0x6A2) = 0x1` (kernel shadow stack enable)
- [ ] Per-task shadow stack: `cet_alloc_shadow_stack(task, size)` -- allocate `size` bytes (default 64 KiB) with `PAGE_WRITE_PROTECT`; set `SSS` attribute in PTE; write initial return address token via `WRSSQ`; store in `task->shadow_stack_base`
- [ ] `task_switch()` extension: `WRMSRL(IA32_PL0_SSP, task->shadow_stack_ksp)` to set kernel SSP for new task before first ring-0 entry
- [ ] Syscall entry (ASM): add `INCSSPQ rcx` after saving registers (advances shadow stack by 1 slot, recording return address)
- [ ] Syscall return (ASM): `RSTORSSP [shadow_stack_ptr]` before `SYSRETQ` to restore shadow stack to pre-syscall state
- [ ] `cet_free_shadow_stack(task)`: unmap and free shadow stack pages on task exit
- [ ] Boot log: `[CET] Shadow stack enabled (SS + %s)` (`IBT` or `SS only`)
- [ ] Commit: `"kernel: Intel CET -- CR4.CET, IA32_U_CET, per-task shadow stack, INCSSPQ/RSTORSSP syscall"`

## OS Comparison


| ⭐   | Feature                                  | 🪟 Win11                                  | 🐧 Linux                                  | 🚀 Impossible OS                          |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎   | Hardware RNG (`RDRAND`/`RDSEED`) + CSPRNG fallback | ✅ `BCryptGenRandom`; CNG uses RDRAND; SP800-90A | ✅ `arch_get_random_{long,seed}`; ChaCha20 DRNG in kernel | ⬜ §2 -- `hwrng_read()`, 10× retry, ChaCha20 fallback, |
| 💎   | IOMMU/VT-d DMA isolation -- default-deny policy | ✅ VBS + IOMMU; Kernel DMA                | ✅ `intel_iommu=on`; 4-level page tables; default-deny | ⬜ §4 -- DMAR/IVRS parse, 4-level IOMMU PT, |
| 💎   | DMA bounce buffer for 32-bit legacy DMA devices | ✅ `DmaAdapter.AllocateCommonBuffer`; 32-bit DMA zone | ✅ `dma_alloc_coherent` low-zone; bounce buffers via | ⬜ §3 -- 16 MiB PMM low-zone pool,        |
| 💎   | TPM 2.0 command driver                   | ✅ `tpm.sys`; TBS service; `BCryptCreateHash(TPM_*)` | ✅ `tpm_crb.c` / `tpm_tis.c`; `tpm2_pcr_extend`; `/dev/tpm0` | ⚠️ §6 -- Partial -- event log parser     |
| 💎   | Secure Boot state exposed to OS          | ✅ `HKLM\SYSTEM\CurrentControlSet\Control\SecureBoot\State` | ✅ `/sys/firmware/efi/vars/SecureBoot-*`; `mokutil --sb-state` | ⬜ §5 -- `EFI_RUNTIME_SERVICES.GetVariable`, `HKLM\SYSTEM\SecureBoot\Enabled` |
| ⭐   | Intel CET shadow stacks                  | ✅ CET enabled on Win11 x64               | ✅ `CONFIG_X86_SHADOW_STACK`; user-mode CET in 6.6+; | ⬜ §8 -- `CR4.CET`, `IA32_U_CET`, per-task SS, `INCSSPQ`/`RSTORSSP` |
| 💎   | SMEP + SMAP on all CPUs                  | ✅ Enabled by Windows HAL on              | ✅ `native_write_cr4`; enabled on all CPUs | ⬜ §1 -- `CR4` bit 20+21 on BSP           |
| ⭐   | Kernel integrity via TPM PCR extend + baseline comparison | ✅ VBS/HVCI + Measured Boot; PCR          | ✅ IMA (`ima_measure_file`); PCR[10] extend; `ima-policy` | ⬜ §7 -- SHA-256 `.text`/`.rodata`, PCR[10] extend, UEFI |

> **After §1–8:** Impossible OS matches or exceeds Windows 11 and Linux on all hardware security primitives. Two features stand out as `⭐` exclusive: **CET** (§8) works without VBS -- Windows requires Virtualization Based Security for kernel-mode CET, Impossible OS enables it natively in the base kernel; **kernel integrity** (§7) extends TPM PCR[10] directly from the kernel without a hypervisor measurement layer, matching Linux IMA but integrating the baseline comparison into the boot flow with a UEFI variable golden record rather than a separate `ima-policy` daemon.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] RDRAND: `hwrng_read(buf, 32)` returns 32 non-zero bytes; two consecutive calls return different values
- [ ] ChaCha20 fallback: patch CPUID result to hide RDRAND; `hwrng_read()` uses ChaCha20 and still returns unique bytes
- [ ] SMEP/SMAP: `rdmsr CR4` on BSP and each AP shows bits 20+21 set (when CPUID supports them); attempt ring-0 jump to user address → #PF (SMEP violation)
- [ ] DMA bounce buffer: `dma_alloc(dev, 4096, &phys)` returns `phys < 0x100000000` on a system with > 4 GiB RAM
- [ ] IOMMU: boot log `[IOMMU] Intel VT-d: N DRHD units, default-deny DMA enabled`; RTL8139 driver issues `iommu_map()` before DMA; unmapped DMA attempt triggers fault handler
- [ ] TPM: boot log `[TPM2] Interface=CRB, STARTUP OK`; `tpm2_get_random(32, buf)` returns 32 bytes; `tpm2_pcr_read(0, SHA256, digest)` returns non-zero PCR
- [ ] Secure Boot: `HKLM\SYSTEM\SecureBoot\Enabled` reads `0x01` on a Secure Boot system; reads `0x00` in QEMU (no SB)
- [ ] CET: ring-3 task with shadow stack enabled; return to wrong address (ROP gadget) → `#CP` exception (control protection fault)
- [ ] Kernel integrity: boot log `[INTEGRITY] Kernel SHA-256: ... PCR[10] extended`; tamper `.text` bytes in debug build → `[INTEGRITY] Kernel PCR[10] mismatch` on next boot
- [ ] Commit: `"kernel: security hardware -- hwrng, IOMMU/VT-d, DMA bounce, TPM2, SecureBoot, CET, SMEP/SMAP, integrity"`
