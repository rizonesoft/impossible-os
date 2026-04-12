# TODO-19 -- x86-64 Architecture Enhancements

> **Goal:** Activate and exploit the x86-64 architecture features that `cpuid.c` already detects, pushing beyond today's partial enablement: full XSAVE/XRSTOR state management with per-thread XSAVE areas and lazy FPU, AVX/AVX2 optimised kernel paths and AVX-512 support, a centralised MSR access layer, UMIP and PKU protection keys, 1 GiB huge pages and Write-Combining PAT for the framebuffer, FRED event delivery with LKGS, CPU topology parsing (Zen chiplets + Intel P/E-cores), performance monitoring counters (Intel PMU + AMD PMC), OSVW errata + RDTSCP setup, AMD IBS profiling, virtualization detection, and a boot-time self-benchmark that auto-tunes the kernel to the detected hardware.

> [!IMPORTANT]
> **Current state:** `cpuid_init()` / `cpu_has()` gate the feature set. **Done [x]:** §1 `cpu_configure_xcr0()`, per-thread `xsave_area`, lazy FPU (`#NM` first touch), XSAVEOPT when supported, FXSAVE removed from `icon_store.c` / `gfx_text.c`. §3 `msr_read` / `msr_write` / `msr_try_read` plus MSR constants; SMP/LAPIC migrated off raw inline MSR asm where noted in §3. §11 AMD SVM + Intel VT-x detection via `cpu_has()`. **Open [ ]:** §2 through §10, §12 through §13 per Implementation Order.
> **Scope boundary with other TODOs (do not implement here):**
> - NX/EFER, SMEP/SMAP, KPTI, PCID, IBRS/retpoline, CET -> `TODO-17` (gap analysis 2026-04-12: `TODO-17` §1 NX + `vmm_apply_nx_policy()` are live; §2 SMEP/SMAP helpers exist but `hv_supports_cr4_smep_smap()` forces skip so CR4 bits stay off until kernel PTE User policy is fixed; `isr_stubs.asm` omits `clac` until SMAP is real)
> - TSC invariant check + TSC-Deadline APIC -> `TODO-07`
> - HWP/CPPC frequency scaling + thermal monitoring -> `TODO-15`
> - NUMA-aware page allocator -> `D03` `03-memory-concurrency`
> - Hybrid P/E-core scheduler policy -> `D03` `03-memory-concurrency`
> - CPU feature explorer GUI + chiplet visualizer -> `D08` `08-desktop-shell`
> - PKS (Protection Keys for Supervisor, `PKRS` MSR): Linux mm uses it for selective kernel writeability; overlaps PTE key bits with PKU. Defer design and enablement to `TODO-17` until kernel direct-map and SMEP/SMAP page attributes match `CLAUDE.md` policy (user PKU stays §4 here)

---

## Inputs

- `src/kernel/gfx/gfx_simd.c`: `simd_enable_avx()`, blit helpers; AVX2 memops land in §2
- `include/kernel/cpuid.h`: `xsave_size`, `xsave_size_max`, `xcr0_supported` fields already populated from leaf 0x0D
- `src/kernel/sched/task.c`: `struct task` holds `xsave_area` / `fpu_used` (§1)
- `src/kernel/smp/smp.c`: per-CPU bring-up; MSR access via `msr.c` (§3)
- `src/kernel/mm/vmm.c`: `vmm_map_page`; extend for 1 GiB PS bit and PAT bits
- `src/kernel/idt.c`: IDT init; FRED replaces/supplements this (§6)
- -> XREF: `TODO-06-irql-model-dpcs.md §3`: per-CPU IRQL; FRED event levels map to IRQL (§6)
- -> XREF: `TODO-07-time-filetime-management.md §3`: per-CPU TSC read path (`rdtsc_ns`); §9 here programmes `IA32_TSC_AUX` for `RDTSCP` CPU id in ECX
- -> XREF: `TODO-15-power-management.md §9`: Driver Power Callbacks & Resume Ordering; CPU topology (§7) feeds the scheduler policy deferred to `03-memory-concurrency`
- -> XREF: `TODO-17-kernel-security-hardening.md` (§1 through §7): NX, SMEP/SMAP, KPTI, PCID, IBRS, CET are already scoped there; this TODO does not touch those
- -> XREF: `01-boot-platform/TODO-04-cpu-boot-sequencing.md §1, §3, §5`: CPUID detection call site (§1), hypervisor pre-detection (§3), XSAVE/PCID Phase 1 activation window (§5) consume features defined here

---

## Outcome

- On CPUs with `XSAVE`, every kernel context switch uses `XSAVE`/`XRSTOR` with the correct XSAVE area size; without `XSAVE`, `schedule()` falls back to `fxsave`/`fxrstor` on the same buffer (§1). Lazy FPU avoids saving unused state.
- AVX/AVX2 paths for `memcpy`, `memset`, and the framebuffer blit replace SSE2 where supported; AVX-512 optional fast paths enabled on supporting CPUs.
- A centralised `msr.c` replaces scattered inline `rdmsr`/`wrmsr` asm with a safe, whitelisted API that avoids `#GP` on unknown MSRs.
- `CR4.UMIP` prevents user-space from leaking GDT/IDT addresses.
- PKU provides per-thread 1 ns-cost memory sandboxing via `WRPKRU`.
- The framebuffer is mapped Write-Combining instead of UC, boosting pixel throughput about 10 to 50x through 64-byte write-coalescing.
- 1 GiB pages reduce TLB miss count for large physical regions.
- FRED replaces IDT interrupt delivery on supporting CPUs; LKGS eliminates SWAPGS from the syscall path.
- `topology_init()` knows Zen CCD/NUMA layout and Intel P/E-core split.
- Intel PMU and AMD PMC expose per-core IPC and cache-miss counters.
- `IA32_TSC_AUX` is programmed per-CPU for accurate `RDTSCP`-based timing.
- A 2-second boot self-benchmark stores hardware capabilities in the Registry and auto-configures SIMD dispatch thresholds and scheduler quantum.

---

## Implementation Order

| ⭐  | Order | Deliverable                                         | Depends On           | Status |
| --- | :---: | --------------------------------------------------- | -------------------- | :----: |
| 💎  |   1   | XSAVE/XRSTOR state management (per-thread, lazy)    | (none)               |  [x]   |
| 💎  |   2   | AVX/AVX2 + AVX-512 kernel paths                     | §1                   |  [ ]   |
| 💎  |   3   | MSR management infrastructure (`msr.c`)             | (none)               |  [/]   |
| 💎  |   4   | UMIP + PKU protection keys                          | §3                   |  [ ]   |
| 💎  |   5   | 1 GiB huge pages + Write-Combining PAT              | §3                   |  [ ]   |
| 💎  |   6   | FRED event delivery + LKGS                          | §3, T06 §3           |  [ ]   |
| 💎  |   7   | CPU topology: Zen chiplets + Intel hybrid P/E-core  | (none)               |  [ ]   |
| 💎  |   8   | Performance monitoring counters (Intel + AMD)       | §3                   |  [ ]   |
| 💎  |   9   | OSVW errata + RDTSCP processor ID setup             | §3, T07 §3           |  [ ]   |
| 💎  |  10   | AMD IBS profiling (stretch)                         | §3                   |  [ ]   |
| 💎  |  11   | Virtualization detection (AMD-V + Intel VT-x)       | §3                   |  [x]   |
| ⭐  |  12   | Boot self-benchmark + auto-tune                     | §1, §2, §7           |  [ ]   |
| 💎  |  13   | Future silicon stubs: APX, UINTR, AVX10, LA57       | (none)               |  [ ]   |

> 💎 = parity work: matches what Windows 11 and Linux already do.
> ⭐ = exclusive work: Impossible OS is superior or first.

---

## 1. XSAVE / XRSTOR State Management

- [x] Replaced `simd_enable_avx()` XCR0 setup with `cpu_configure_xcr0()` in `cpuid.c`, called from `cpuid_init()`:
  1. Sets `CR4.OSXSAVE` (bit 18); callable on BSP and APs
  2. Reads `g_cpu.xcr0_supported`, builds mask: bits 0-2 (x87+SSE+AVX), bits 5-7 (AVX-512 if AVX512F), bit 9 (PKRU if supported)
  3. Writes filtered mask via `XSETBV ecx=0`
  4. Stores in `g_cpu.xcr0_active`; `simd_enable_avx()` now just checks `xcr0_active`

- [x] Added `xsave_area` (void*, 64-byte-aligned) and `fpu_used` (uint8_t) to `struct task`
- [x] `task_alloc_xsave(t)`: allocates from PMM (>4KB) or heap (<=4KB), size from `g_cpu.xsave_size_max` rounded to 64, zeroed, XSTATE_BV header bit 0 set
- [x] Lazy allocation: `xsave_area` starts NULL, allocated on first FPU use via `task_alloc_xsave()`

- [x] Lazy FPU: `schedule()` saves/restores via XSAVE/XRSTOR; sets CR0.TS for deferred allocation
- [x] `#NM` handler (vector 7): clears CR0.TS, allocates page-aligned XSAVE area on first FPU use
- [x] Context switch: XSAVE prev if fpu_used, XRSTOR next if fpu_used, else set CR0.TS
- [x] When `CPU_FEATURE_XSAVE` is false, `schedule()` uses `fxsave`/`fxrstor` on the same `xsave_area` buffer (`src/kernel/sched/task.c` lazy FPU block); keeps QEMU TCG and minimal-CPU paths working
- [x] Fix: `task_alloc_xsave()` always uses `pmm_alloc_contiguous()` for page-aligned (4096-byte) allocation; satisfies XSAVE 64-byte requirement
- [x] XSAVEOPT: detected via CPUID 0x0D sub-leaf 1 EAX[0]; scheduler uses `xsaveopt` instead of `xsave` when available (~30% faster context switch)

- [x] Removed manual `simd_save/restore_state()` from icon_store.c; lazy FPU handles it
- [x] Removed all ~30 manual `simd_save/restore_state()` + `fxsave_area_t` from `gfx_text.c`; lazy FPU handles it
- [x] Commit: `"kernel/simd: remove manual FXSAVE from icon_store, lazy FPU handles it"`

**Test checkpoint:** Two tasks with distinct YMM patterns survive `schedule()` round-trip without corruption when `fpu_used` is set; never-FPU task keeps `xsave_area == NULL` and no XSAVE on switch. Serial boot unchanged vs pre-§1 baseline. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 2. AVX / AVX2 + AVX-512 Kernel Paths

- [ ] `cpu_configure_xcr0()` (§1) enables `XCR0[2]` (AVX YMM halves); this is the gate; `simd_avx2_ok` flag already exists in `gfx_simd.c`
- [ ] Add `-mavx2` to a per-file `CFLAGS_simd.c` (not all kernel files) and implement in `src/kernel/mm/memops.c`:
  - `memcpy_avx(dst, src, len)`: 256-bit `vmovdqu` / `vmovdqu` loop; handle head/tail < 32 bytes with scalar fallback
  - `memset_avx(dst, val, len)`: `vpbroadcastd` + `vmovdqa` store loop
- [ ] `vzeroupper` at the end of every AVX kernel function to prevent AVX->SSE transition penalties in subsequent SSE code
- [ ] `memcpy` / `memset` dispatch: `if (cpu_has(CPU_FEATURE_AVX2))` -> AVX2 path; else `if (cpu_has(CPU_FEATURE_SSE2))` -> existing SSE path
- [ ] Framebuffer blit: update `fb_blit_avx()` from `gfx_simd.c` to use YMM registers (8 pixels/iteration, already scaffolded but verify it uses XSAVE-safe approach after §1)

- [ ] Enable `XCR0` bits 5-7 if `cpu_has(CPU_FEATURE_AVX512F)` and a frequency-throttle check passes: read `IA32_MPERF`/`IA32_APERF` ratio before and after a 10 us AVX-512 burst; if ratio drops > 5%, disable AVX-512 (core throttling detected; common on consumer CPUs)
- [ ] Update XSAVE area allocation to use `xsave_size_max` (includes ZMM)
- [ ] `memcpy_avx512` and `fb_blit_avx512` (16 pixels/iteration) with `zmovdqu64` and `evmovdqu64`; gated by `simd_avx512_ok` flag
- [ ] `vzeroupper` still needed when mixing 512-bit and 128/256-bit code

- [ ] If `cpu_has(CPU_FEATURE_AVX10)`: log `[SIMD] AVX10 v%u detected; not yet enabled`; placeholder for future enablement when compilers fully support `-mavx10.N`
- [ ] If `CPUID.(7,1):EDX[21]` (APX): log detected; future work to set `XCR0[19]` (reuses MPX area) and compile with `-mapx`

- [ ] Commit: `"kernel/simd: AVX/AVX2 memcpy/memset/blit paths, AVX-512 opt-in with throttle guard"`

**Test checkpoint:** On AVX2-class CPU, `memcpy_avx` / `memset_avx` byte-match scalar reference on 64 KiB; `vzeroupper` emitted at function end; AVX-512 path disables when MPERF/APERF ratio drops >5% after micro-burst. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 3. MSR Management Infrastructure

- [x] Create `src/kernel/msr.c` and `include/kernel/msr.h`: centralised `rdmsr`/`wrmsr` with inline functions:
  ```c
  uint64_t msr_read(uint32_t index);
  void     msr_write(uint32_t index, uint64_t value);
  int      msr_try_read(uint32_t index, uint64_t *out); /* 0 ok, -1 on #GP */
  ```
- [x] MSR constants table in `include/kernel/msr.h` (22 constants including Intel, AMD, and Hyper-V):
  ```c
  #define MSR_IA32_EFER             0xC0000080
  #define MSR_IA32_STAR             0xC0000081
  #define MSR_IA32_LSTAR            0xC0000082
  #define MSR_IA32_FMASK            0xC0000084
  #define MSR_IA32_FS_BASE          0xC0000100
  #define MSR_IA32_GS_BASE          0xC0000101
  #define MSR_IA32_KERNEL_GS_BASE   0xC0000102
  #define MSR_IA32_TSC_AUX          0xC0000103
  #define MSR_IA32_PAT              0x00000277
  #define MSR_IA32_APIC_BASE        0x0000001B
  #define MSR_IA32_SPEC_CTRL        0x00000048
  #define MSR_IA32_PRED_CMD         0x00000049
  #define MSR_IA32_ARCH_CAPS        0x0000010A
  #define MSR_IA32_MPERF            0x000000E7
  #define MSR_IA32_APERF            0x000000E8
  #define MSR_IA32_PERF_GLOBAL_CTRL 0x0000038F
  #define MSR_IA32_FIXED_CTR0       0x00000309
  #define MSR_AMD_OSVW_ID_LEN       0xC0010140
  #define MSR_AMD_OSVW_STATUS       0xC0010141
  #define MSR_AMD_PERF_CTL0         0xC0010200
  #define MSR_AMD_PERF_CTR0         0xC0010201
  ```
- [x] `msr_try_read()`: temporarily installs #GP handler, attempts rdmsr, skips instruction on fault; returns -1 if unsupported
- [x] Migrated all inline `rdmsr`/`wrmsr` in `smp.c` (3 sites) and `lapic.c` (2 sites) to `msr_read()`/`msr_write()`
- [x] `cpu_verify_hardening()` in `cpu_security.c` refactored: raw `rdmsr` + dead duplicate replaced with `msr_read(MSR_IA32_EFER)` (d825576d)
- [x] Commit: `"kernel/msr: cpu_verify_hardening EFER verify uses msr_read"` (d825576d)

**Test checkpoint:** `msr_read(MSR_IA32_EFER)` stable across calls; `msr_try_read(0xFFFFFFFF, &scratch) == -1` without panic; boot completes with MSR layer linked. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Verified:** 2026-04-12 -- all 6 items confirmed. `msr_read`/`msr_write` inline in `msr.h:16-28`. `msr_try_read` in `msr.c:48-89` with spinlock+irqsave+RIP check (Codex: SMP race + unrelated #GP swallowing fixed). 22 MSR constants in `msr.h`. smp.c (4 sites) + lapic.c (2 sites) migrated. `cpu_verify_hardening` uses `msr_read` (d825576d). Hyper-V MSR constants corrected per TLFS Table 2-2 (0x40000022=TSC, 0x40000023=APIC). `msr_write` memory clobber added. Accepted: none.
> **Quality reviewed:** 2026-04-12 -- kernel-code-quality 11 gates walked. G2: spinlock serializes msr_try_read on SMP. G11: rdmsr 2-byte skip per Intel SDM. Hyper-V TLFS compliant. Memory clobber on wrmsr prevents store reordering across GS_BASE writes. Accepted: none.

---

## 4. UMIP + PKU Protection Keys

- [ ] `cpu_enable_umip()`: `if (cpu_has(CPU_FEATURE_UMIP)) cpu_set_cr4_bit(CR4_UMIP)` where `CR4_UMIP = (1ULL << 11)`; called in Phase 1 on BSP and each AP
- [ ] Effect: user-space `SGDT`, `SIDT`, `SLDT`, `SMSW`, `STR` raise `#GP` instead of revealing GDT/IDT base addresses; eliminates a trivial kernel address leak
- [ ] Verify: user-mode test `SGDT [ptr]` after `cpu_enable_umip()` must fault with `#GP` (error code = 0)

- [ ] Enable: `if (cpu_has(CPU_FEATURE_PKU)) cpu_set_cr4_bit(CR4_PKE)` where `CR4_PKE = (1ULL << 22)`
- [ ] Add PKRU to XCR0 (bit 9) in `cpu_configure_xcr0()` (§1); ensures per-thread `PKRU` state is saved/restored automatically
- [ ] Kernel API in `src/kernel/security/pku.c`:
  ```c
  int      pku_alloc_key(void);                /* returns key 1-15; 0 = default */
  void     pku_free_key(int key);
  void     pku_set_permissions(int key, uint32_t flags); /* wraps WRPKRU */
  #define  PKU_ACCESS_DISABLE  0x1
  #define  PKU_WRITE_DISABLE   0x2
  ```
- [ ] PTE key field: bits 62:59 in each page table entry; add `pku_key` field to `vmm_map_page` flags parameter
- [ ] Win32 API surface: `SetThreadMemoryZone(zone_id, ACCESS_NONE)` -> `pku_set_permissions(zone_id, PKU_ACCESS_DISABLE)`; zero-cost switch without any syscall, using `WRPKRU` (~1 ns vs ~1 us for `mprotect`)
- [ ] PKRU initial value: `0x55555554` (disable access to keys 1-15 by default for user threads; key 0 = full access always)

- [ ] Commit: `"kernel/security: UMIP (CR4.UMIP), PKU protection keys, SetThreadMemoryZone"`

**Test checkpoint:** After `cpu_enable_umip()`, user test `SGDT` faults `#GP`; PKRU disable on keyed page raises `#PF` with AD/WK set when probed from ring 3 harness. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 5. 1 GiB Huge Pages + Write-Combining PAT

- [ ] Check `cpu_has(CPU_FEATURE_PAGE1GB)` before use
- [ ] VMM extension: in `vmm_map_range(va, pa, size, flags)`, if size >= 1 GiB and both `va` and `pa` are 1 GiB-aligned and `CPU_FEATURE_PAGE1GB`: use PDPTE with `PS = 1` (bit 7) instead of a PD -> PT chain
- [ ] `vmm_map_huge_1g(va, pa, flags)`: single-entry helper: set PDPTE `PS` bit; physical address in bits [51:30]
- [ ] Use at boot: identity-map the first N GiB of physical RAM with 1 GiB pages where N is aligned to 1 GiB; reduces TLB miss pressure for large DMA buffers and MMIO regions
- [ ] Fallback: if `PAGE1GB` not set, silently fall through to 2 MiB pages

- [ ] Read `IA32_PAT` MSR (`0x277`) at boot; ensure entry 1 = `0x01` (WC, Write-Combining type); default PAT entry 1 is WC on most CPUs but verify explicitly:
  ```c
  uint64_t pat = msr_read(MSR_IA32_PAT);
  pat = (pat & ~(0xFFULL << 8)) | (0x01ULL << 8); /* entry 1 = WC */
  msr_write(MSR_IA32_PAT, pat);
  ```
- [ ] Add `PTE_WC` flag: `PCD=0, PWT=1, PAT=0` (selects PAT entry 1 = WC) in the PTE; encode in `vmm_map_wc(va, pa, size)`
- [ ] Update `fb_init()` in `src/kernel/drivers/framebuffer.c`: replace current uncached mapping with `vmm_map_wc(fb_va, fb_pa, fb_size)`; benchmark: log `fb_swap()` time before and after; expect about 10 to 50x speedup

- [ ] Commit: `"kernel/mm: 1 GiB huge pages, Write-Combining PAT for framebuffer"`

**Test checkpoint:** `cpu_has(PAGE1GB)` gate respected; 1 GiB-aligned range uses single PDPTE PS path; `fb_swap()` TSC median drops >=5x after WC map on 1080p. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 6. FRED Event Delivery + LKGS

- [ ] Check `cpu_has(CPU_FEATURE_FRED)` (CPUID leaf 7, ECX=1, EAX bit 17)
- [ ] FRED delivers all events (interrupts, exceptions, `SYSCALL`) to a single kernel entry point with the event type and vector in registers; eliminates IDT corruption edge-cases and provides native NMI nesting
- [ ] Enable: `cpu_set_cr4_bit(CR4_FRED)` where `CR4_FRED = (1ULL << 32)`; configure `IA32_FRED_CONFIG` MSR with:
  - Kernel stack pointer (for Level 0, ring 0 events)
  - Unified entry RIP (`fred_entry_asm` in `src/kernel/fred.asm`)
- [ ] `fred_entry_asm` unified handler:
  - Reads event type and vector from the FRED stack frame
  - Dispatches to the existing `isr_handler(frame)` or `kd_debug_exception_handler` (-> XREF: `TODO-18-kernel-debugger-kd-protocol.md` §4) via the same `handlers[]` table in `idt.c`
- [ ] Replace `IRET` with FRED return instructions where FRED is active:
  - `ERETS`: return to ring 0
  - `ERETU`: return to ring 3
- [ ] `SYSCALL` via FRED: automatically uses FRED entry; `IA32_LSTAR` still needed for non-FRED fallback path
- [ ] Fallback: if `CPU_FEATURE_FRED` not present, IDT path unchanged

- [ ] Check `cpu_has(CPU_FEATURE_LKGS)` (CPUID leaf 7, ECX=1, EAX bit 18)
- [ ] `LKGS reg` writes directly to `IA32_KERNEL_GS_BASE` without touching the active GS; eliminates `SWAPGS` and its speculative side-channel
- [ ] In `src/kernel/sched/syscall_entry.asm`:
  - On SYSCALL entry: `LKGS [saved_user_gs]` instead of `SWAPGS`
  - On SYSRET: `LKGS [user_gs_base]` before `SYSRETQ`
- [ ] Same replacement in IDT common stubs that currently use `SWAPGS`
- [ ] Fallback: if LKGS unavailable, keep existing `SWAPGS` (no regression)

- [ ] Commit: `"kernel/cpu: FRED unified event delivery, LKGS replaces SWAPGS in syscall path"`

**Test checkpoint:** On FRED-capable QEMU (`-cpu Cooperlake,fred=on`), timer IRQ still reaches `isr_handler`; syscall path uses LKGS when `CPU_FEATURE_LKGS`; fallback CPU boots unchanged. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 7. CPU Topology: Zen Chiplets + Intel Hybrid

- [ ] Create `src/kernel/topology.c` and `include/kernel/topology.h`:
  ```c
  typedef struct {
      uint32_t logical_id;   /* APIC ID / OS CPU number */
      uint32_t core_id;      /* physical core within CCD/package */
      uint32_t ccd_id;       /* AMD: CCD (Compute Complex Die); Intel: cluster */
      uint32_t node_id;      /* NUMA domain */
      uint8_t  core_type;    /* CORE_TYPE_P (0x40), CORE_TYPE_E (0x20), CORE_TYPE_GENERIC (0) */
      uint8_t  smt_siblings; /* hyperthreads per core */
  } cpu_topo_t;

  extern cpu_topo_t g_cpu_topo[MAX_CPUS];
  extern uint32_t   g_cpu_count;
  extern uint64_t   p_core_mask; /* Intel: bitmask of P-core logical CPUs */
  extern uint64_t   e_core_mask; /* Intel: bitmask of E-core logical CPUs */
  extern uint32_t   g_numa_nodes;
  ```
- [ ] `topology_init()` called from Phase 1 after `cpuid_init()` and SMP probe; for each logical CPU: run per-CPU leaf queries via IPI or on-CPU during AP startup

- [ ] If `cpu_has(CPU_FEATURE_TOPO_EXT)` (from §1, already in cpuid.c):
  - Per-CPU `CPUID 0x8000001E`: `ccd_id = EBX[7:0]`, `node_id = ECX[7:0]`, `smt_siblings = EBX[15:8] + 1`
  - Log: `[topo] Zen: %u CCDs, %u NUMA nodes, NPS=%u`
- [ ] `g_numa_nodes` = max `node_id` + 1; expose to PMM as a hint (NUMA allocator implementation deferred to `03-memory-concurrency`)

- [ ] If `CPUID.(7,0):EDX[15]` (hybrid bit) set:
  - Per-CPU `CPUID 0x1A`: `EAX[31:24]` = core type (0x40=P, 0x20=E)
  - Per-CPU `CPUID 0x1F` (V2 Extended Topology): enumerate SMT/Core/Die levels; map to `core_id` and `ccd_id`
  - Populate `p_core_mask` and `e_core_mask` bitmasks
  - Log: `[topo] Intel hybrid: %u P-cores, %u E-cores`
- [ ] Fallback: no hybrid, all cores treated as `CORE_TYPE_GENERIC`

- [ ] Commit: `"kernel/topology: Zen CCD/NUMA parser, Intel hybrid P/E-core detection"`

**Test checkpoint:** Serial shows `[topo] Zen:` or `[topo] Intel hybrid:` line with non-zero masks; `g_cpu_topo[0].logical_id == 0`; Zen path populates `ccd_id` when `TOPO_EXT`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 8. Performance Monitoring Counters: Intel + AMD

- [ ] Check CPUID leaf `0x0A`: PMU version (EAX[7:0]), counter count (EAX[15:8]), counter width (EAX[23:16])
- [ ] `pmc_intel_start(slot, event_select, unit_mask)`:
  - Write event to `IA32_PERFEVTSELx` MSR (0x186+slot): `event_select | (unit_mask << 8) | ENABLE_BIT | OS_BIT | USR_BIT`
  - Clear `IA32_PMCx` (0xC1+slot)
- [ ] `pmc_intel_read(slot)` -> `msr_read(0xC1 + slot)`
- [ ] `pmc_intel_stop(slot)` -> clear `IA32_PERFEVTSELx` enable bit
- [ ] Pre-defined event constants:
  ```c
  #define PMC_INTEL_INST_RETIRED    0xC0  /* instructions retired */
  #define PMC_INTEL_UNHALTED_CYCLES 0x3C  /* CPU_CLK_UNHALTED.THREAD */
  #define PMC_INTEL_LLC_MISSES      0x2E  /* LAST_LEVEL_CACHE.MISS */
  #define PMC_INTEL_BR_MISPREDICT   0xC5  /* BR_MISP_RETIRED */
  ```

- [ ] AMD Zen uses `MSR_AMD_PERF_CTL0` (0xC0010200) + `MSR_AMD_PERF_CTR0` (0xC0010201) per counter (6 per core on Zen 4)
- [ ] `pmc_amd_start(slot, event)` -> `msr_write(0xC0010200 + slot*2, event | ENABLE)`
- [ ] `pmc_amd_read(slot)` -> `msr_read(0xC0010201 + slot*2)`
- [ ] `pmc_amd_stop(slot)` -> clear enable bit
- [ ] Pre-defined events: `0x76` (CPU clocks), `0xC0` (instructions retired), `0xC2` (retired branches), `0x64` (DRAM accesses)

- [ ] `pmc_start(slot, event)` / `pmc_read(slot)` / `pmc_stop(slot)`: dispatch to Intel or AMD path based on `cpu_features.vendor`
- [ ] `pmc_ipc()` shortcut: start inst+cycle counters; read after 1 ms; return `instructions / cycles` as fixed-point; used by Task Manager (`08-desktop-shell`) for per-process IPC display
- [ ] Enable via `IA32_PERF_GLOBAL_CTRL` (Intel, MSR 0x38F); AMD counters enabled per-counter via the `ENABLE` bit in CTL

- [ ] Commit: `"kernel/pmc: Intel PMU + AMD PMC, pmc_start/read/stop, pmc_ipc() for Task Manager"`

**Test checkpoint:** `pmc_intel_read` increments on tight loop; `pmc_ipc()` returns finite fixed-point >0; stopping counter zeros enable bit without GPF. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 9. OSVW Errata + RDTSCP Processor ID Setup

- [ ] If `cpu_has(CPU_FEATURE_OSVW)`:
  - `msr_read(MSR_AMD_OSVW_ID_LEN)` -> number of errata tracked
  - `msr_read(MSR_AMD_OSVW_STATUS)` -> bitmask of active errata
  - Store in `cpu_features.osvw_length` / `cpu_features.osvw_status`
  - `cpu_has_erratum(n)` -> `(osvw_status >> n) & 1`
  - Log: `[cpu] OSVW: %u errata tracked, mask=0x%llx`
- [ ] Apply per-erratum workarounds as they are identified from the AMD PPR for the detected family/model; initially a no-op table is fine

- [ ] If `cpu_has(CPU_FEATURE_RDTSCP)`:
  - During AP startup in `ap_startup_c()`: `msr_write(MSR_IA32_TSC_AUX, this_cpu_id)` stores logical CPU ID in `IA32_TSC_AUX`
  - `rdtscp_read(tsc, cpu_id)` inline wrapper:
    ```c
    __asm__ volatile("rdtscp" : "=A"(*tsc), "=c"(*cpu_id));
    ```
  - Use in `TODO-07-time-filetime-management.md §3` (`rdtsc_ns` path) for nanosecond-accurate per-CPU timestamps; XREF noted in Inputs
- [ ] BSP: also write `IA32_TSC_AUX` with CPU 0 during Phase 1 init

- [ ] Commit: `"kernel/cpu: OSVW errata table, RDTSCP per-CPU IA32_TSC_AUX setup"`

**Test checkpoint:** When `CPU_FEATURE_OSVW`, serial logs mask line; `RDTSCP` ECX matches `smp_this_cpu()->id` on BSP and each AP after init. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 10. AMD IBS Profiling (Stretch)

- [ ] Check `cpu_has(CPU_FEATURE_IBS)` (from §1)
- [ ] **IBS Fetch Sampling:** samples random instruction fetch ops:
  - Configure `MSR 0xC001_1030` (IBS_FETCH_CTL): set `IbsFetchEn (bit 17)`, `IbsFetchCnt (bits 15:0)` = sample rate (~100K ops)
  - On NMI: read `0xC001_1031` (linear fetch address) + `0xC001_1032` (physical + status: cache hit/miss, iTLB miss, L1/L2/L3 source)
- [ ] **IBS Op Sampling:** samples random instruction completion ops:
  - Configure `MSR 0xC001_1033` (IBS_OP_CTL): `IbsOpEn + IbsOpCnt`
  - On NMI: read `0xC001_1035` (RIP), `0xC001_1036` (op data: micro-op info, cache miss, DRAM latency), `0xC001_1037` (data address + NUMA source)
- [ ] IBS NMI handler: read all IBS MSRs; pack into a ring buffer of `ibs_sample_t` structs (256 entries per CPU, static allocation); re-arm counter; return from NMI
- [ ] `ibs_start(rate)` / `ibs_stop()` / `ibs_read_samples(buf, max)` API
- [ ] Future: wire to a profiler GUI in `10-apps`

- [ ] Commit: `"kernel/pmc: AMD IBS fetch+op sampling, NMI handler, sample ring buffer"`

**Test checkpoint:** With `CPU_FEATURE_IBS`, NMI handler fills ring without nested NMI deadlock; `ibs_read_samples` returns monotonic sequence numbers. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal (AMD).

---

## 11. Virtualization Detection (AMD-V + Intel VT-x)

- [x] AMD SVM / Intel VMX capability reporting: AMD SVM: CPUID 0x8000000A parsed; revision, NPT, ASIDs logged
- [x] Intel VT-x: `CPU_FEATURE_VMX` added (CPUID.1:ECX[5]); `IA32_FEATURE_CONTROL` MSR read for lock + enable status
- [x] Exposed via `cpu_has(CPU_FEATURE_SVM)` / `cpu_has(CPU_FEATURE_VMX)` (detection only)
- [x] Commit: `"kernel/cpu: AMD SVM + Intel VT-x capability detection and logging"`

**Test checkpoint:** Boot log shows AMD SVM or Intel VMX lines matching `cpuid -1` expectations on real silicon; `cpu_has` agrees with known host type. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 12. Boot Self-Benchmark + Auto-Tune

- [ ] Define in `include/kernel/hw_profile.h`:
  ```c
  typedef struct {
      uint32_t mem_bandwidth_mb_s;   /* sequential memcpy throughput */
      uint32_t mem_latency_ns;       /* pointer-chasing latency */
      uint32_t l1_size_kb;
      uint32_t l2_size_kb;
      uint32_t l3_size_kb;
      uint32_t simd_sse2_gpix_s;    /* pixel throughput in gigapix/s */
      uint32_t simd_avx_gpix_s;
      uint32_t simd_avx512_gpix_s;
      uint32_t ctx_switch_ns;       /* measured scheduler context switch */
      uint64_t tsc_mhz;             /* TSC frequency in MHz */
      uint32_t flags;               /* HW_PROFILE_VALID = 1 */
  } hw_profile_t;
  ```
- [ ] Stored in `HKLM\SYSTEM\HwProfile\*` via Registry (-> XREF `TODO-13-registry-completion.md §4`)
- [ ] Stale if CPU brand string has changed since last boot (different hardware)

- [ ] Run during Phase 2 init (after SMP, before GUI) if `HwProfile` is stale or absent; total runtime <= 2 seconds:
  - **Memory bandwidth**: `VMOVDQA` 256 MiB sequential write; measure MB/s from TSC delta
  - **Memory latency**: 4 MiB pointer-chase array (stride = cache-line); measure ns/access
  - **Cache sizes**: stride binary search; L1 to L2 to L3 inflection at typical ~32 KiB / ~512 KiB / ~8 MiB; log `l1/l2/l3_size_kb`
  - **SIMD throughput**: timed SSE2 / AVX / AVX-512 alpha-blend loop over 4 MiB framebuffer; record Gpix/s per ISA level
  - **Context switch**: 1000 yield-pairs between two kernel tasks; `rdtsc` around each switch; average ns per switch
- [ ] Write `hw_profile_t` to Registry

- [ ] SIMD dispatch: if `avx_gpix / sse2_gpix < 1.10` (< 10% gain; throttling detected): set `simd_avx2_ok = 0`; use SSE2 path to avoid frequency reduction
- [ ] Scheduler quantum: if `ctx_switch_ns < 500` -> set `SCHED_TICK_US = 100`; if `ctx_switch_ns > 2000` -> set `SCHED_TICK_US = 500`
- [ ] Compositor triple-buffering: if `mem_bandwidth_mb_s > 20 000` -> enable
- [ ] `memcpy` threshold: if `l1_size_kb == 32` -> `REP MOVSB` for < 128 B, SIMD for larger

- [ ] Commit: `"kernel/bench: boot self-benchmark, hw_profile Registry, auto-tune SIMD/scheduler"`

**Test checkpoint:** Registry `HKLM\SYSTEM\HwProfile\MemBandwidthMbS` non-zero after bench; second boot skips bench when profile valid; `simd_avx2_ok` toggles per auto-tune rule when AVX gain <10%. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 13. Future Silicon Extensions

- [ ] Stub: if `CPUID.(7,0):EDX[5]` set: log `[cpu] UINTR present; not yet enabled`; structure definitions in `include/kernel/uintr.h` (UPID, UITT); full implementation deferred until UINTR reaches mainstream silicon
- [ ] Stub: if `CPUID.(7,0):ECX[16]` set: log `[vmm] LA57 (57-bit VA) detected; 4-level paging active`; enabling at runtime requires a full VMM rewrite; relevant for server SKUs with > 256 TiB virtual address space
- [ ] Commit: `"kernel/cpu: UINTR/LA57 detection stubs, future silicon log entries"`

**Test checkpoint:** When CPU advertises UINTR or LA57, serial shows one-line stub log and kernel continues without enabling new modes. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## OS Comparison

| ⭐   | Feature              | 🪟 Win11        | 🐧 Linux         | 🚀 Impossible OS |
| --- | -------------------- | -------------- | --------------- | --------------- |
| 💎   | CPUID feature gates  | ✅ Ke API       | ✅ x86/cpu       | ✅ cpuid_init    |
| 💎   | AMD ext CPUID        | ✅ HAL leaves   | ✅ amd.c         | ✅ §1            |
| 💎   | XSAVE per thread     | ✅ Full         | ✅ fpu__*        | ✅ §1 Done       |
| 💎   | AVX memops in kernel | ✅ Yes          | ✅ kfpu          | ⚠️ §2 GFX only  |
| 💎   | AVX-512 + throttle   | ✅ Yes          | ✅ power aware   | ⬜ §2            |
| 💎   | Central safe MSR     | ✅ HAL          | ✅ rdmsr_safe    | ✅ §3            |
| 💎   | UMIP                 | ✅ On           | ✅ 4.15+         | ⬜ §4            |
| 💎   | PKU / pkeys          | ✅ OS use       | ✅ pkey_*        | ⬜ §4 Win32 API  |
| 💎   | PKS (supervisor)     | ⚠️ niche       | ✅ mm since 5.13 | ⬜ defer T17     |
| 💎   | FB WC PAT            | ✅ GPU path     | ✅ ioremap_wc    | ⬜ §5 UC today   |
| 💎   | 1 GiB huge pages     | ✅ Large page   | ✅ hugetlb 1G    | ⬜ §5            |
| 💎   | FRED events          | 🔜 roadmap      | ✅ 6.9+          | ⬜ §6            |
| 💎   | LKGS fast GS         | 🔜 roadmap      | ✅ 6.4+          | ⬜ §6            |
| 💎   | Zen CCD NUMA         | ✅ Ke node      | ✅ amd topo      | ⬜ §7            |
| 💎   | Intel P/E hybrid     | ✅ Director     | ✅ HFI           | ⬜ §7            |
| 💎   | PMU / PMC            | ✅ ETW WPA      | ✅ perf          | ⬜ §8            |
| 💎   | OSVW errata          | ✅ HAL          | ✅ amd.c         | ⬜ §9            |
| 💎   | RDTSCP TSC_AUX       | ✅ QPC path     | ✅ SMP init      | ⬜ §9            |
| 💎   | AMD IBS sample       | ⚠️ vendor tool | ✅ perf IBS      | ⬜ §10 stretch   |
| 💎   | SVM VT-x detect      | ✅ HAL          | ✅ kvm caps      | ✅ §11           |
| ⭐   | Boot hw self tune    | ❌ static       | ❌ static        | ⬜ §12           |
| ⭐   | PKU Win32 wrapper    | ❌ none         | ⚠️ raw syscalls | ⬜ §4            |
| ⭐   | Per-core freq UI     | ❌ simple       | ⚠️ turbostat    | ⬜ §8 + shell    |

After §1 through §11 done and §2 through §10 plus §12 through §13 planned, parity with Win11/Linux 6.x on this stack; §12 boot benchmark is the differentiator. `SetThreadMemoryZone` (§4) is a planned Win32-style PKU surface. PKS (supervisor keys) stays in `TODO-17` until kernel mappings allow it without breaking SMEP/SMAP rollout.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_x86()` (see `src/kernel/test/test_runner.c` and `include/kernel/test/test.h`). Use `test_suite_register_cat(..., TEST_CAT_BOOT)` for each case.
> Boot tests run with `debug=1` or `test=1` in `boot.conf`.

- [ ] Create `src/kernel/test/test_x86.c` with:
  - MSR read: `msr_read(MSR_IA32_EFER)` returns value with NXE bit set
  - MSR write/read round-trip: write scratch MSR, read back matches (platform-dependent)
  - XSAVE area: `g_cpu.xsave_size` (or `g_cpu.xsave_size_max`) is > 0 when XSAVE enabled after `cpuid_init()`
  - CPUID leaf 0x01: returns valid family/model/stepping (non-zero)
  - CPUID leaf 0x07: feature flags readable without fault
  - CR4 read: CR4 value has expected bits (PSE, PAE, PGE set for x86-64)
  - GDT/IDT base: `sgdt`/`sidt` return non-zero base addresses
  - TSC monotonic: two `rdtsc()` calls -> second >= first
  - 1 GiB page support: `cpu_has(CPU_FEATURE_PAGE1GB)` matches CPUID leaf
  - Per-CPU data: `smp_this_cpu()` returns valid CPU struct (non-NULL, valid ID)
  - UMIP: if enabled, `sidt` from ring 3 causes #GP (can't test from ring 0 directly)
- [ ] Add `extern void test_register_x86(void);` in `test_runner.c`, call `test_register_x86()` from `test_runner_init()`
- [ ] Commit: `"test: add x86-64 architecture test suite"`

**Test checkpoint:** With `SUITE=boot` and `test=1` (or `debug=1`) in `boot.conf`, serial shows new `test_x86` cases as PASS; `msr_read(MSR_IA32_EFER)` test reports NXE set; `g_cpu.xsave_size` (or `xsave_size_max`) > 0 when XSAVE is enabled; `cpu_has(CPU_FEATURE_PAGE1GB)` matches CPUID-derived expectation on the host.

---

## Verification

- [ ] **XSAVE round-trip**: load YMM0 through YMM15 with known values; context switch to another task; switch back; verify YMM values intact via `XRSTOR` + read.
- [ ] **Lazy FPU**: create a task that never uses FP; verify its `xsave_area` is NULL and no `XSAVE` is issued on context switch.
- [ ] **AVX memcpy**: `memcpy_avx(dst, src, 1 MiB)`; compare byte-for-byte with reference; measure throughput > 2x SSE2 baseline on AVX2 hardware.
- [ ] **WC framebuffer**: compare `fb_swap()` TSC duration before and after PAT WC mapping; expect >= 5x reduction on a 1080p framebuffer.
- [ ] **UMIP**: user-mode `SGDT [ptr]` after `cpu_enable_umip()` must receive `SIGSEGV` / `#GP`; kernel `SGDT` still works.
- [ ] **PKU**: allocate key 3; set `PKU_ACCESS_DISABLE`; try reading a page tagged with key 3; must raise `#PF` with PKU violation bit set in error code.
- [ ] **RDTSCP**: after `topology_init()` + TSC\_AUX setup, `RDTSCP` on each CPU returns its correct logical CPU ID in ECX.
- [ ] **PMC IPC**: `pmc_ipc()` on a tight integer loop returns IPC > 1.0.
- [ ] **Boot benchmark**: on first boot, `HKLM\SYSTEM\HwProfile\MemBandwidthMbS` is non-zero; `simd_avx2_ok` reflects the benchmark result.
- [ ] **Remaining limits**: FRED requires hardware support (Intel Granite Rapids or later); test in QEMU with `-cpu Cooperlake,fred=on`; APX/UINTR deferred until compiler support (`-mapx`) is stable; LA57 requires bootloader + VMM rewrite before enabling.
- [ ] **Platforms:** repeat critical checks on QEMU WHPX, QEMU TCG, VirtualBox, bare metal (FRED/IBS/UMIP availability differs).
- [ ] Commit: `"kernel/x86: XSAVE/XRSTOR, AVX/AVX-512, MSR layer, UMIP/PKU, WC PAT, FRED/LKGS, topology, PMC, OSVW, IBS, VT-x/SVM, boot self-benchmark"`

**Test checkpoint:** Manual or staged boot on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal: each Verification bullet above observed (serial log, registry key, or measured ratio) with no regression on the non-FRED fallback path; FRED checks skipped or gated when `CPU_FEATURE_FRED` is absent.

**Test runner:** `scripts\debug\run-boot-tests.bat` (SUITE=boot)

---

## History

| Date | Action | Summary |
| --- | --- | --- |
| 2026-04-10 | validate | Removed ### N.M, stripped model tags, ASCII punctuation sweep, fixed IMPORTANT + Inputs, moved/compact OS Comparison, `TODO-06`/`TODO-07` Depends On, per-section **Test checkpoint** + four platforms, Unit Tests -> `TEST_CAT_BOOT` + `msr_read` + `g_cpu.xsave_size`, Verification + `run-boot-tests.bat`. **Parity:** Win11+Linux-strong rows map to §2 through §12 except WinDbg-adjacent items (N/A here). **Flag:** §6 FRED/LKGS needs recent silicon or explicit QEMU CPU flags. |
| 2026-04-12 | gap-analysis | Reciprocal sync from `TODO-17` gap analysis: scope bullet now records live NX + `vmm_apply_nx_policy()`, partial SMEP/SMAP (`hv_supports_cr4_smep_smap()` forces skip), and missing IDT `clac` until SMAP is real. |
| 2026-04-12 | validate | §9 through §13: Commit before **Test checkpoint**; replaced spaced hyphen pairs used as sentence glue; §12 SIMD bullet joined; Unit Tests + Verification **Test checkpoint** blocks; History wording fix for §2 through §12. **Parity gaps (Win11+Linux strong, Impossible not done):** §2 AVX-512 throttle, §4 UMIP+PKU, §5 WC+1G, §6 FRED+LKGS, §7 topology, §8 PMC, §9 OSVW+TSC_AUX, §10 IBS, §12 boot tune. **Blocked / external:** T06 §3 (FRED vs IRQL), T07 §2 (RDTSCP consumer), T13 §4 (HwProfile registry), D03 memory + D08 shell deferrals per Inputs. |
| 2026-04-12 | gap-analysis | Web: Win UMIP/Linux PKU docs, Linux FRED merge 6.9 (LWN), kernel.org protection-keys, perf-amd-ibs. Code-truth: §3 MSR bullet narrowed; new follow-up for `cpu_security.c` raw `rdmsr`; §1 documents FXSAVE fallback in `task.c`; PKS row + scope defer to T17; reciprocal note in T17 IMPORTANT. **Parity gaps unchanged** except explicit PKS tracking. **Flags:** §1 item count >10 (mostly done); §5 PAT+FB touches MTRR/MMIO gotchas from `CLAUDE.md`. |
| 2026-04-12 | validate | IMPORTANT callout joined (no blank inside); Inputs T07 XREF §2 to §3 + Impl row 9 `T07 §3`; Outcome XSAVE vs FXSAVE truth; scope `D03`/`D08` shorthands; §11 orphan prose merged into first `[x]`; table column scan OK; **blocked:** §6 on `T06 §3` open, §9 on `T07 §3` consumer wiring; parity gaps unchanged from prior validate row. |
