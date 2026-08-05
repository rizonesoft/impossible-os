---
schema_version: 1
id: hypervisor
domain: 18-future-research
status: active
title: "TODO-02 -- Type-1 Hypervisor (ImpossibleHV)"
---

# TODO-02 -- Type-1 Hypervisor (ImpossibleHV)

> **Goal:** Research spike to design and prove out a built-in Type-1.5 hypervisor
> (ImpossibleHV) that runs Windows, Linux, and other OS guests inside Impossible OS.
> Scope: Intel VT-x/AMD-V feasibility analysis, minimal VMCS-based VMM design, a
> Linux guest boot proof-of-concept, virtio device emulation plan, snapshot/migration
> research, and GPU passthrough feasibility -- all gated by `#ifdef ENABLE_HYPERVISOR`.

> [!IMPORTANT]
> **Type-1.5 design**: Impossible OS kernel runs in VMX root mode; guests run in VMX
> non-root mode. No separate hypervisor binary. This is the same model used by KVM
> (Linux) and Hyper-V -- the host OS kernel *is* the VMM.
>
> **Existing VirtIO drivers** (`src/kernel/drivers/virtio/blk_core.c`, `virtio.c`, etc.)
> are **guest-side** drivers (Impossible OS running inside QEMU/Hyper-V). §4 here adds
> the **host-side** virtio device *emulation* (Impossible OS acting as VMM for its
> guests). These are different code paths -- do not conflate them.
>
> **Hypervisor detection** (CPUID `0x40000000`, `boot_info.hv_flags`) is owned by
> `01-boot-platform/TODO-09 §4`; §1 here uses `cpu_data.cpuid_features` to detect
> `VMXE` / `SVM` support -- it reads that data, does not re-specify detection.
>
> **PMM and VMM APIs** (`pmm_alloc_contiguous`, `vmm_map_page`) already exist and are
> used for VMCS region allocation (§2) and EPT page table setup (§3).
>
> Prototype code lives in `src/kernel/hypervisor/` gated by `#ifdef ENABLE_HYPERVISOR`
> and a `HYPERVISOR=1` build flag. This is **not production code** during the research
> phase -- it is a proof-of-concept only.

---

## Inputs

- `include/kernel/mm/pmm.h` -- `pmm_alloc_contiguous(count)` -- §2 §3 VMCS + EPT allocation (4 KB aligned physical pages)
- `include/kernel/mm/vmm.h` -- `vmm_map_page(virt, phys, flags)` -- §3 EPT setup, guest memory mapping
- `include/kernel/sched/task.h` -- `task_t` (per-vCPU state), `task_create` -- §2 vCPU scheduling
- `include/kernel/drivers/lapic.h` -- LAPIC IPI for inter-vCPU signalling -- §2 §3
- `01-boot-platform/TODO-09-cpu-boot-sequencing.md §4` (→ XREF) -- CPUID feature flags (`cpu_data.cpuid_features`); `VMXE` bit detection uses data collected there
- `02-kernel-core/TODO-10-kernel-security-hardening.md` (→ XREF) -- SMEP/SMAP/CET on VMX host; must stay active in host CR4 across VM entries/exits
- `02-kernel-core/TODO-09-x86-64-architecture.md` (→ XREF) -- MSR read/write infrastructure (`rdmsr_safe`, `wrmsr`); used by VMXON and VMCS field access
- `src/kernel/drivers/virtio/virtio.c` -- guest-side VirtIO transport (reference for §4 host-side emulation design; understand the split-ring format from the guest's perspective)
- Intel SDM Vol. 3C (VMX chapter) -- VMCS layout, VM entry/exit, EPT, VPID
- AMD APM Vol. 2 (SVM chapter) -- VMCB (VM Control Block), nested paging, VMSAVE/VMLOAD
- `TODO-06-android-app-compatibility.md` (→ XREF) -- TODO-06 sections 2 and 4 Android guest VM path; VirtIO device set for AOSP bring-up overlaps section 4 host emulation design

---

## Outcome

A research document `docs/architecture/hypervisor-design.md` describing the full
ImpossibleHV design, plus a `#ifdef ENABLE_HYPERVISOR`-gated prototype in
`src/kernel/hypervisor/` that boots a minimal Linux `bzImage` as a guest and prints
its first serial line to the Impossible OS console. This proves the host→guest→exit
loop works end-to-end before committing to full implementation.

---

## Implementation Order

| Step | Section                                                      | 💎/⭐ | Dependency                                                |
| ---- | ------------------------------------------------------------ | ----- | --------------------------------------------------------- |
| 1    | VT-x/AMD-V gap analysis (CPU feature + VMCS field audit)     | ⭐    | Intel SDM Vol. 3C; `cpu_data.cpuid_features` (TODO-04 §3) |
| 2    | Minimal hypervisor design (VMCS layout + VMX root setup)     | ⭐    | §1; `pmm_alloc_contiguous`; `wrmsr`                       |
| 3    | Minimal Linux guest POC (`bzImage` VMLAUNCH + serial output) | ⭐    | §2; `vmm_map_page` for EPT; Linux boot protocol           |
| 4    | virtio device emulation design (host-side blk + net)         | ⭐    | §3 VM I/O exits; guest-side virtio reference              |
| 5    | Snapshot & live migration research                           | ⭐    | §3 running VM; EPT dirty tracking                         |
| 6    | GPU passthrough research (VT-d / AMD-Vi feasibility)         | ⭐    | §2 design; IOMMU prerequisite analysis                    |
| 7    | Research deliverables (`hypervisor-design.md`)               | ⭐    | §1–§6 complete                                            |

---

## 1. Intel VT-x / AMD-V Gap Analysis `[Opus]`

> Novel: first hypervisor feasibility study for Impossible OS. Requires reading Intel
> SDM Vol. 3C (VMX) and AMD APM Vol. 2 (SVM). Documents VMCS field layout, EPT
> structure, and VM exit reason taxonomy that the rest of the spike builds on.

- [ ] **CPU feature detection** (using `cpu_data.cpuid_features` from `TODO-04 §3`):
  - Intel VMX: `cpuid(0x1).ecx bit 5` (`VMX` = `CPUID_ECX_VMX`); also check `IA32_FEATURE_CONTROL` MSR (`0x3A`) bit 2 (VMXON outside SMX enabled) + bit 0 (locked)
  - AMD SVM: `cpuid(0x80000001).ecx bit 2` (`SVM`); check `VM_CR` MSR (`0xC0010114`) SVMDIS bit
  - Report: `"[HV] Intel VMX available"` / `"[HV] AMD-V available"` / `"[HV] No virtualization support"`
- [ ] **VMCS field inventory** (document key fields used in §2 design):

| Category     | Intel VMCS field                                                      | Description           |
| ------------ | --------------------------------------------------------------------- | --------------------- |
| Guest state  | `GUEST_RIP`, `GUEST_RSP`, `GUEST_RFLAGS`                              | vCPU register state   |
| Guest state  | `GUEST_CR0`, `GUEST_CR3`, `GUEST_CR4`, `GUEST_EFER`                   | Guest paging control  |
| Guest state  | `GUEST_CS_*`, `GUEST_SS_*`, `GUEST_DS_*` (selector/base/limit/access) | Segment registers     |
| Host state   | `HOST_RIP`, `HOST_RSP`, `HOST_CR0`, `HOST_CR3`, `HOST_CR4`            | Restored on VM exit   |
| Control      | `PRIMARY_VM_EXEC_CTRL` -- I/O bitmaps, HLT exit, RDTSC exit           | Exit triggers         |
| Control      | `SECONDARY_VM_EXEC_CTRL` -- EPT enable, VPID enable, RDTSCP           | EPT + TLB tagging     |
| Control      | `EPT_POINTER` -- physical address of EPT PML4                         | Nested paging root    |
| Control      | `VPID` -- 16-bit TLB tag per VM                                       | TLB isolation         |
| Control      | `IO_BITMAP_A_ADDR`, `IO_BITMAP_B_ADDR` -- 4 KB each                   | Port I/O exit control |
| Control      | `MSR_BITMAP_ADDR` -- 4 KB bitmap                                      | MSR intercept control |
| VM-exit info | `VM_EXIT_REASON`, `EXIT_QUALIFICATION`, `GUEST_PHYSICAL_ADDRESS`      | Exit classification   |
| VM-exit info | `VM_EXIT_INSTR_LEN`, `VM_EXIT_INSTR_INFO`                             | Faulting instruction  |

- [ ] **VM exit reason taxonomy** (document which exits the VMM must handle):

| Exit reason                  | Value | Cause                            | VMM action                      |
| ---------------------------- | ----- | -------------------------------- | ------------------------------- |
| `EXIT_REASON_EXCEPTION_NMI`  | 0     | Guest exception (e.g. #PF)       | Inject or handle                |
| `EXIT_REASON_EXTERNAL_INT`   | 1     | Host interrupt preempted         | Handle host IRQ, resume         |
| `EXIT_REASON_IO_INSTRUCTION` | 30    | `in`/`out` to intercepted port   | Emulate device I/O              |
| `EXIT_REASON_CPUID`          | 10    | Guest `cpuid`                    | Return sanitised CPUID          |
| `EXIT_REASON_RDMSR`          | 31    | Guest `rdmsr` on intercepted MSR | Return spoofed/forwarded value  |
| `EXIT_REASON_WRMSR`          | 32    | Guest `wrmsr` on intercepted MSR | Accept or reject                |
| `EXIT_REASON_EPT_VIOLATION`  | 48    | Guest accessed unmapped EPT page | Map page or inject #GP          |
| `EXIT_REASON_HLT`            | 12    | Guest `hlt` instruction          | Pause vCPU until next interrupt |
| `EXIT_REASON_XSETBV`         | 55    | Guest `xsetbv` (XCR0 write)      | Validate + allow                |
| `EXIT_REASON_VMCALL`         | 18    | Hypercall from guest             | ImpossibleHV hypercall dispatch |
| `EXIT_REASON_CR_ACCESS`      | 28    | Guest CR0/CR3/CR4 write          | Emulate or intercept            |

- [ ] **AMD-V equivalents** (VMCB vs VMCS): AMD uses a `struct vmcb` (two 4 KB pages: control area + state save area); SVM uses `VMRUN` / `VMSAVE` / `VMLOAD` instead of VMLAUNCH/VMRESUME; nested paging (nPT) equivalent to EPT; document VMCB offsets for control and save areas
- [ ] **Dual-vendor strategy**: implement Intel VT-x first (§2 §3 prototype); add `#ifdef VMX_INTEL` / `#ifdef VMX_AMD` compile-time switch; AMD-V follow-on work after Intel prototype passes

---

## 2. Minimal Hypervisor Design (Type-1.5 VMM) `[Opus]`

> Novel architectural design: kernel transitions into VMX root mode; VMCS per vCPU
> allocated from PMM. Ring-transition primitives (VMXON, VMLAUNCH, VMRESUME) are
> hardware-interface code with security-critical host state requirements.

**Source:** `src/kernel/hypervisor/vmx.c`, `src/kernel/hypervisor/vmcs.c` (gated `#ifdef ENABLE_HYPERVISOR`)

- [ ] **VMX root mode initialization** (`vmx_init()`):
  1. Check `cpu_data[cpu].cpuid_features` for `VMX` bit; abort if absent
  2. Set `CR4.VMXE` (bit 13) via `__asm__ volatile("mov %%cr4, %0" / "or $0x2000, %0" / "mov %0, %%cr4")`
  3. Set `IA32_FEATURE_CONTROL` MSR: bit 0 (lock) + bit 2 (VMXON outside SMX)
  4. Allocate VMXON region: `pmm_alloc_contiguous(1)` (4 KB physical page, 4 KB aligned); write `IA32_VMX_BASIC` MSR revision ID to first 4 bytes
  5. `VMXON [vmxon_region_phys]` -- enter VMX root mode; check RFLAGS.CF=0 (success)
  6. Log `"[HV] VMX root mode active on CPU %d"`
- [ ] **`struct vmcs_region`**: 4 KB physical page; first 4 bytes = `IA32_VMX_BASIC.revision_id`; allocated via `pmm_alloc_contiguous(1)` per vCPU
- [ ] **VMCS field access helpers**:
  ```c
  static inline void vmcs_write64(uint32_t field, uint64_t value) {
      __asm__ volatile("vmwrite %1, %0" : : "r"((uint64_t)field), "r"(value) : "cc");
  }
  static inline uint64_t vmcs_read64(uint32_t field) {
      uint64_t value;
      __asm__ volatile("vmread %1, %0" : "=r"(value) : "r"((uint64_t)field) : "cc");
      return value;
  }
  ```
- [ ] **`struct vcpu`** (per virtual CPU state):
  ```c
  typedef struct {
      uintptr_t   vmcs_phys;          // physical address of VMCS region
      uintptr_t   vmcs_virt;          // kernel VA of VMCS region
      uintptr_t   io_bitmap_a_phys;   // 4 KB I/O bitmap (ports 0x0000–0x7FFF)
      uintptr_t   io_bitmap_b_phys;   // 4 KB I/O bitmap (ports 0x8000–0xFFFF)
      uintptr_t   msr_bitmap_phys;    // 4 KB MSR bitmap
      uint64_t    guest_rip;          // saved on VM exit
      uint64_t    guest_rsp;
      uint64_t    guest_rflags;
      struct vm  *vm;                 // back-pointer to parent VM
  } vcpu_t;
  ```
- [ ] **`struct vm`** (per virtual machine):
  ```c
  typedef struct {
      uint16_t    vpid;               // 1-based VPID (TLB isolation tag)
      uintptr_t   ept_pml4_phys;     // EPT root (4-level; allocated from PMM)
      uintptr_t   guest_phys_base;   // start of guest physical memory
      uint64_t    guest_phys_size;   // bytes allocated for guest RAM
      vcpu_t     *vcpus[MAX_VCPUS];  // per-vCPU state
      int         vcpu_count;
  } vm_t;
  ```
- [ ] **VMCS initialization** (`vmcs_setup(vcpu, vm)`): VMPTR load (`VMPTRLD`); write guest state fields, host state fields (mirror of current kernel CR0/CR3/CR4, host RIP = `vmx_exit_handler`), control fields (EPT pointer, VPID, I/O bitmaps); `VMCS_HOST_RIP = &vmx_exit_handler` -- the C function that runs on every VM exit
- [ ] **Host state discipline**: host CR4 must retain SMEP/SMAP (→ XREF `TODO-17`); restore `XSS_MSR` and `FS_BASE` on VM exit; `SWAPGS` if needed for `GS_BASE` isolation between host and guest

---

## 3. Minimal Linux Guest Proof-of-Concept `[Opus]`

> Novel: VMLAUNCH with EPT setup, Linux boot protocol, and VM exit dispatch loop.
> Hardware-interface primitives throughout. Prototype only -- not production quality.

**Source:** `src/kernel/hypervisor/guest_linux.c` (gated `#ifdef ENABLE_HYPERVISOR`)

- [ ] **Guest memory layout** (128 MiB flat physical):
  - Allocate `128 MiB / 4096 = 32768` contiguous pages via `pmm_alloc_contiguous(32768)` → `guest_phys_base`
  - EPT mapping: for each 2 MiB aligned region in `[guest_phys_base, guest_phys_base + 128M)`: add `EPT_PML4 → EPT_PDPT → EPT_PD` entry with 2 MiB page (RWX) mapping guest physical → same host physical address (identity mapping for prototype)
- [ ] **EPT 4-level page table build** (`ept_map_2mb(ept_pml4, guest_phys, host_phys, flags)`):
  - Each EPT level: 512 entries × 8 bytes; allocated via `pmm_alloc_contiguous(1)` on demand
  - Entry format: bits 0 (read) + 1 (write) + 2 (execute) + 12:51 (next level PFN); 2 MiB leaf: bit 7 (large page)
  - EPTP (EPT pointer): `ept_pml4_phys | (3 << 3) | (6)` (4-level, WB memory type)
- [ ] **Linux `bzImage` load** (into guest memory at `0x100000`):
  - Read `bzImage` file into host kernel memory; validate magic `0x53726448` at offset 0x202 (Linux PE header optional magic)
  - Decompress if needed (bzImage = gzip'd vmlinux; use miniz `mz_inflate` to extract to guest `0x100000`)
  - Set up `struct boot_params` at guest physical `0x10000`: `hdr.type_of_loader = 0xFF`, `hdr.loadflags |= LOADED_HIGH`, `hdr.cmd_line_ptr = 0x20000`, `hdr.ramdisk_image = 0`, `hdr.ramdisk_size = 0`
  - Write cmdline `"console=ttyS0 panic=1 init=/bin/sh"` at guest physical `0x20000`
- [ ] **VMCS guest state for Linux entry** (16-bit real mode or 32-bit protected mode via `startup_32`):
  - `GUEST_RIP = 0x100000` (Linux `startup_32` / `startup_64` entry)
  - `GUEST_RSI = 0x10000` (pointer to `boot_params`)
  - `GUEST_CS = 0x10`, `GUEST_CS_BASE = 0`, `GUEST_CS_LIMIT = 0xFFFFFFFF`, `GUEST_CS_ACCESS = 0xA09B` (64-bit code segment)
  - `GUEST_EFER |= EFER_LME | EFER_LMA`; `GUEST_CR0 |= PE | PG | WP`; `GUEST_CR4 |= PAE`; `GUEST_CR3 = guest_pml4_phys` (identity-mapped page tables for guest)
- [ ] **`VMLAUNCH`** assembly stub (`vmx_launch.asm`):
  ```asm
  vmx_launch:
      ; push all host callee-saved registers
      vmlaunch
      ; VMLAUNCH failed: read RFLAGS, log error, return
      ret
  vmx_exit_handler:
      ; save guest GPRs to vcpu->guest_gpr_save[]
      ; call vmx_handle_exit(vcpu) in C
      vmresume
      ; VMRESUME failed
      ret
  ```
- [ ] **VM exit dispatch loop** (`vmx_handle_exit(vcpu_t *vcpu)`):
  - Read `VM_EXIT_REASON`; dispatch on low 16 bits:
  - `EXIT_REASON_IO_INSTRUCTION` (port access): read `EXIT_QUALIFICATION` for port number + direction + size; if port `0x3F8` (COM1 UART TX): `klog_char((char)vmcs_read64(GUEST_RDX))` → forward to Impossible OS serial log; advance `GUEST_RIP += VM_EXIT_INSTR_LEN`
  - `EXIT_REASON_CPUID`: return safe values (mask out VMX bit in ECX, cap PHYS_BITS); `GUEST_RIP += 2`
  - `EXIT_REASON_HLT`: set vCPU blocked state; yield host CPU; wake on next interrupt injection; `GUEST_RIP += 1`
  - `EXIT_REASON_RDMSR` / `EXIT_REASON_WRMSR`: allow `IA32_TSC`, `IA32_MISC_ENABLE`; intercept or zero-return others; `GUEST_RIP += 2`
  - `EXIT_REASON_EPT_VIOLATION`: log `"[HV] EPT violation at guest phys %llx"` + halt vCPU (research prototype; full handler is future work)
  - All other exits: log reason + halt vCPU (`vm_stop(vcpu->vm)`)
- [ ] **Prototype success criterion**: `VMLAUNCH` succeeds; guest executes to Linux `"Uncompressing Linux..."` message on COM1; host serial log shows forwarded Linux boot message

---

## 4. virtio Device Emulation Design (Host-Side) `[Opus]`

> Novel: host-side virtio ring emulation. The existing `src/kernel/drivers/virtio/`
> code implements the *guest driver* (consuming virtio devices). This section designs
> the *host VMM side* -- responding to guest driver requests via VM exits.

**Source:** `src/kernel/hypervisor/virtio_mmio.c` (gated `#ifdef ENABLE_HYPERVISOR`)

- [ ] **virtio MMIO transport** (guest accesses device at a fixed MMIO base address, e.g. `0xFEB00000`):
  - EPT maps this GPA with no host PA → triggers `EXIT_REASON_EPT_VIOLATION` on any guest access
  - `vmx_handle_exit` detects MMIO region + dispatches to `virtio_mmio_handle_access(guest_phys, write, data)`
  - VMM registers: `MagicValue (0x000) = 0x74726976`, `Version (0x004) = 2`, `DeviceID (0x008) = 2 (blk) or 1 (net)`, `VendorID (0x00C) = 0x494D`, `DeviceFeatures (0x010)`, `QueueReady (0x044)`, `QueueNotify (0x050)`, `InterruptStatus (0x060)`, `Status (0x070)`, `QueueDescLow/High (0x080/0x084)`, `QueueDriverLow/High (0x090/0x094)`, `QueueDeviceLow/High (0x0A0/0x0A4)`
- [ ] **virtio split-ring queue processing** (`virtio_process_queue(vm, queue_idx)`):
  1. Read `AvailRing.idx` from guest memory (via host PA from EPT map of guest PA)
  2. For each new entry since last `last_avail_idx`: read descriptor chain from `DescTable[]`
  3. Each `VirtqDesc`: `addr` (guest physical), `len`, `flags` (NEXT, WRITE), `next`
  4. Translate guest physical → host physical via EPT walk (`ept_gpa_to_hpa(vm, gpa)`)
  5. Perform I/O (blk: read/write host image file; net: send/receive via host network)
  6. Write result to `UsedRing.ring[used_idx]` = `{id, len}`; increment `UsedRing.idx`
  7. Inject virtual interrupt to guest: write `InterruptStatus = 1`; `vmcs_write64(VM_ENTRY_INTR_INFO, ...)` with interrupt vector assigned to the virtio device
- [ ] **`virtio-blk` emulation**: guest block I/O → host file I/O; disk image: `C:\Impossible\VMs\{vm-name}\disk.img`; `virtio_blk_handle_request(req_hdr, data, status)`: `IN` request → `vfs_read(disk_img, offset, len, host_buf)`; `OUT` request → `vfs_write(disk_img, offset, len, host_buf)`
- [ ] **`virtio-net` emulation**: guest packet TX → host network stack; `virtio_net_handle_tx(buf, len)`: inspect Ethernet header; forward to `net_send_frame(buf, len)` (Impossible OS network stack); for RX: poll host RX queue; inject packet into guest virtio RX ring
- [ ] **Device emulation design constraint**: all MMIO accesses are serialized through the VM exit handler; no concurrency between vCPU execution and device processing in prototype; production version would require per-device worker threads

---

## 5. Snapshot & Live Migration Research `[Opus]`

> Complex algorithm: dirty page tracking via EPT write bits while vCPU runs; concurrent
> memory copy; brief pause to copy final pages. No prior Impossible OS VM migration.
> This section is research and design only -- not prototype code.

- [ ] **VM snapshot design** (pause-and-copy):
  1. **Pause vCPU**: set `vcpu->paused = 1`; send IPI to vCPU's host CPU; host CPU exits VM via `VM_EXIT_PAUSE` (or inject NMI); wait for vCPU thread to ack pause
  2. **Serialize VMCS**: `VMCLEAR [vmcs_phys]` (commits VMCS to memory); copy 4 KB VMCS region to snapshot buffer; record `guest_gpr_save[]` from last `vmx_exit_handler`
  3. **Copy guest memory**: iterate `guest_phys_size / PAGE_SIZE` pages; `memcpy(snapshot_buf + offset, host_pa, PAGE_SIZE)` for each; compress with `miniz mz_compress2` for storage efficiency
  4. **Serialize device state**: for each virtio device: save queue state (`last_avail_idx`, `last_used_idx`, disk image path/offset)
  5. **Write snapshot file**: `C:\Impossible\VMs\{vm-name}\snapshot-{timestamp}.vmsnapshot`; format: `[HEADER][VMCS 4KB][GPRs 128B][DEVICE_STATE][COMPRESSED_MEMORY_PAGES]`
  6. **Resume**: `VMPTRLD [vmcs_phys]`; `vmresume`
- [ ] **Live migration design** (iterative dirty-page copy while running):
  1. Enable EPT dirty tracking: set `SECONDARY_EXEC_CTRL.EPT_AD = 1`; EPT entries gain Accessed (bit 8) and Dirty (bit 9) bits
  2. **Pre-copy phase**: scan all EPT entries; copy each 4 KB page with Dirty=1 to destination; clear Dirty bit; repeat until dirty set < threshold (e.g., < 50 pages changed per 100 ms)
  3. **Stop-and-copy phase**: pause vCPU; copy remaining dirty pages (small set); serialize VMCS + device state
  4. **Resume on destination**: requires distributed kernel support -- destination host must run Impossible OS kernel and accept VM state via `SYS_VM_RECEIVE` (new syscall); this is a **hard prerequisite not yet available**
  5. **Blocking issue**: live migration requires a network channel between two Impossible OS instances and a kernel VM receive path. Defer to post-`TODO-07` (distributed kernel, stretch goal)
- [ ] **Snapshot format reference**: KVM uses `kvmtool` savevm format; Linux QEMU uses `QEMUSaveVM` format; Impossible OS snapshot format is simpler (single flat file, no migration wire format needed for pause-and-copy)

---

## 6. GPU Passthrough Research (VT-d / AMD-Vi) `[Sonnet]`

> Documentation/feasibility study. GPU passthrough has substantial hardware prerequisites
> that do not yet exist in Impossible OS. This section documents what is needed.

- [ ] **IOMMU basics**: Intel VT-d (Virtualization Technology for Directed I/O) maps guest physical addresses to device DMA addresses; AMD-Vi is the equivalent; prevents guest DMA attacks by isolating device DMA to a specific physical address range
- [ ] **Prerequisite analysis** (all blocking, none yet implemented):
  1. **IOMMU driver** (`src/kernel/drivers/iommu.c`): parse DMAR ACPI table (Intel); configure remapping hardware; `iommu_map(domain, gpa, hpa, size)` -- not yet implemented
  2. **PCIe hot-plug support**: unbind device from host OS PCI driver; rebind to VM passthrough domain -- PCIe hot-plug events not yet handled
  3. **VFIO-style device isolation**: host OS must not touch the device after passthrough (no shared DMA); requires per-device IOMMU domain
  4. **GPU interrupt routing**: MSI/MSI-X interrupt remapping through IOMMU interrupt remapping tables
- [ ] **SR-IOV path** (alternative to full passthrough):
  - SR-IOV GPUs (e.g. Intel GVT-g, Arc series): physical function (PF) + multiple virtual functions (VFs); each VF appears as a separate GPU to the guest; PF remains on host
  - Less complex than full passthrough; still requires IOMMU driver
- [ ] **Feasibility verdict** (for §7 deliverable): GPU passthrough is **not feasible** until IOMMU driver, PCIe hot-plug, and MSI-X remapping are implemented. Estimate: 3–6 months additional prerequisite work. Recommended to defer to Phase 4 of the hypervisor roadmap.
- [ ] **Near-term alternative**: software GPU emulation via `virtio-gpu` (paravirtualized framebuffer over virtio MMIO); no IOMMU required; feasible as Phase 3 extension of §4

---

## 7. Research Deliverables `[Sonnet]`

> Primary output: `docs/architecture/hypervisor-design.md`. Prototype code (non-production)
> in `src/kernel/hypervisor/` gated by `#ifdef ENABLE_HYPERVISOR`. No changes to kernel
> outside `src/kernel/hypervisor/` during research phase.

- [ ] **`docs/architecture/hypervisor-design.md`** -- sections:
  - **CPU feature requirements**: Intel `VMXE` + `IA32_FEATURE_CONTROL`; AMD `SVM`; minimum Intel microarch: Haswell (EPT + VPID + INVEPT + INVVPID); minimum AMD microarch: Zen (NPT + AVIC)
  - **VMCS field layout** (from §1 table): categorised by control / guest state / host state / VM-exit info; annotated with Impossible OS usage
  - **VMM architecture**: Type-1.5 diagram (host kernel in VMX root, guest in VMX non-root, EPT separating guest physical from host physical)
  - **Device emulation plan** (from §4): virtio MMIO + split-ring; `virtio-blk` disk image backend; `virtio-net` packet forwarding; `virtio-gpu` stretch
  - **Guest OS support matrix**:

| Guest OS                  | CPU mode         | Boot protocol             | Serial            | virtio-blk                         | Status  |
| ------------------------- | ---------------- | ------------------------- | ----------------- | ---------------------------------- | ------- |
| 🐧 Linux x64 (busybox)    | 64-bit long mode | `bzImage` + `boot_params` | COM1 via I/O exit | ✅ prototype                       | Phase 1 |
| 🚀 Impossible OS (nested) | 64-bit long mode | ELF + boot_info struct    | Serial            | ✅ same                            | Phase 2 |
| Windows 10 x64            | 64-bit long mode | UEFI boot                 | Hyper-V synthetic | Requires APIC virt + UEFI firmware | Phase 3 |

  - **Phased implementation plan**:
    - Phase 1: single vCPU, Intel VMX only, Linux busybox boot, COM1 serial, EPT identity map
    - Phase 2: multi-vCPU (per-vCPU VMCS + LAPIC IPI delivery), AMD-V support, nested Impossible OS
    - Phase 3: virtio-blk + virtio-net + virtio-gpu, Windows guest (Hyper-V enlightenments), UI for VM management
    - Phase 4: snapshots, live migration (post-distributed kernel), GPU passthrough (post-IOMMU driver)
  - **Effort estimate**: Phase 1: `[X person-weeks]`; Phase 2: `[Y person-weeks]`; total Phase 1+2: estimated from §1 LOC analysis
  - **Blocking issues**: APIC virtualization (`APIC_ACCESS_ADDR` VMCS field, `APIC_ACCESS_PAGE`) needed for Windows guest; Hyper-V synthetic MSRs for Windows enlightenment; UEFI firmware image (OVMF) needed for UEFI guest boot
- [ ] **Prototype branch**: push `hypervisor/vmx-spike` branch; README explains research status + how to enable with `HYPERVISOR=1 bash scripts/build.sh`; open tracking issue "ImpossibleHV Phase 1" in GitHub Issues linking to `hypervisor-design.md`
- [ ] **`HYPERVISOR=1` build flag**: `scripts/build.sh` accepts `HYPERVISOR=1` env var; adds `-DENABLE_HYPERVISOR` to CFLAGS; compiles `src/kernel/hypervisor/*.c` into kernel image; default OFF (zero impact on normal builds)

---

## OS Comparison


| ⭐  | Feature                                    | 🪟 Win11                                              | 🐧 Linux                                      | 🚀 Impossible OS                                               |
| --- | ------------------------------------------ | ----------------------------------------------------- | --------------------------------------------- | -------------------------------------------------------------- |
| 💎  | Type-1 / Type-1.5 hypervisor built into OS | ✅ Hyper-V (Type-1, HVCI, VBS, Hyper-V                | ✅ KVM (Type-2 module in Linux                | ⬜ §2 -- §3; Type-1.5 ImpossibleHV in `src/kernel/hypervisor/` |
| 💎  | EPT (Extended Page Tables) nested paging   | ✅ Hyper-V second-level address translation           | ✅ KVM EPT + shadow page                      | ⬜ §3 -- 4-level EPT; 2 MiB large                              |
| 💎  | virtio device emulation for guests         | ✅ Hyper-V synthetic VMBus devices (no                | ✅ QEMU virtio-blk/net/gpu + KVM acceleration | ⬜ §4 -- host-side virtio MMIO + split-ring                    |
| 💎  | VM snapshot + pause-and-copy               | ✅ Hyper-V checkpoints; VMMS snapshot API             | ✅ QEMU savevm / libvirt snapshot             | ⬜ §5 -- VMCS + memory serialisation to                        |
| 💎  | Live migration research                    | ✅ Hyper-V live migration (RDMA or                    | ✅ KVM live migration (iterative dirty        | ⬜ §5 -- research; EPT dirty bits; blocking:                   |
| ⭐  | GPU passthrough                            | ✅ Hyper-V DDA (Discrete Device Assignment),          | ✅ VFIO passthrough + IOMMU groups            | ⬜ §6 -- research only; blocking: IOMMU driver                 |
| ⭐  | Public hypervisor design doc + phased plan | ✅ Hyper-V: architecture docs on Learn.microsoft.com; | ✅ KVM: open source; architecture in          | ⬜ §7 -- `hypervisor-design.md` with VMCS layout, guest        |

Impossible OS's `⭐` advantage: ImpossibleHV is designed as an integral kernel subsystem
from the start -- not a separate binary (unlike Hyper-V), not a loadable module requiring
a frontend (unlike KVM + QEMU). The `#ifdef ENABLE_HYPERVISOR` gate means the hypervisor
adds zero binary size or boot overhead to normal builds. The phase plan targets
Impossible OS as a *guest of itself* in Phase 2 -- enabling nested virtualization for
CI test isolation without any external hypervisor dependency.

---

## Verification

- [ ] **VMX detection**: boot with `HYPERVISOR=1`; serial shows `"[HV] Intel VMX available on CPU 0"` on Intel hardware / QEMU with `-cpu host` or `kvm64`; shows `"[HV] No virtualization support"` on `qemu64` CPU model (VT-x not exposed)
- [ ] **VMXON succeeds**: `vmx_init()` → no `#GP` on `VMXON`; RFLAGS.CF=0; serial shows `"[HV] VMX root mode active on CPU 0"`
- [ ] **VMCS allocation**: `vmcs_alloc()` returns non-null; `VMPTRLD` succeeds (RFLAGS.CF=0); `vmcs_write64(GUEST_RIP, 0xDEAD)` + `vmcs_read64(GUEST_RIP)` returns `0xDEAD`
- [ ] **Linux guest boot**: `HYPERVISOR=1 bash scripts/build.sh run`; `hv_launch_linux("bzImage")` → VMLAUNCH succeeds; serial output from VM exit handler shows `"[guest] Uncompressing Linux..."` within 10 s; guest halts cleanly (no infinite EPT violation loop)
- [ ] **COM1 serial forwarding**: Linux kernel `printk` to `ttyS0` → COM1 port `0x3F8` → `EXIT_REASON_IO_INSTRUCTION` → character forwarded to Impossible OS serial log; full Linux boot messages visible in `build/serial.log`
- [ ] **HLT exit**: guest executes `hlt` (idle) → `EXIT_REASON_HLT` logged; vCPU re-entered on next interrupt; no busy-spin
- [ ] **Snapshot**: `vm_snapshot(vm, "test.vmsnapshot")` → file created; VMCS region serialized; `ls -la test.vmsnapshot` shows non-zero size; restore: `vm_restore("test.vmsnapshot")` → VMLAUNCH succeeds (same guest state as before snapshot)
- [ ] **Zero overhead on normal build**: `bash scripts/build.sh` (without `HYPERVISOR=1`) → no hypervisor symbols in `nm build/kernel.elf | grep hv_`; binary size unchanged
- [ ] Commit: `"hypervisor: ImpossibleHV VMX spike -- VMXON, VMCS setup, EPT, Linux guest boot POC, virtio design, snapshot plan"`
