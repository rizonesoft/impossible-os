# TODO-19 — x86-64 Architecture Enhancements

> **Goal:** Activate and exploit the x86-64 architecture features that
> `cpuid.c` already detects but that no kernel code yet uses: full XSAVE/XRSTOR state management with per-thread XSAVE areas and lazy FPU, AVX/AVX2 optimised kernel paths and AVX-512 support, a centralised MSR access layer, UMIP and PKU protection keys, 1 GiB huge pages and Write-Combining PAT for the framebuffer, FRED event delivery with LKGS, CPU topology parsing (Zen chiplets + Intel P/E-cores), performance monitoring counters (Intel PMU + AMD PMC), OSVW errata + RDTSCP setup, AMD IBS profiling, virtualization detection, and a boot-time self-benchmark that auto-tunes the kernel to the detected hardware.

> [!IMPORTANT]
> **Done (§1.1/§1.2 + §3 + §11 complete):** `cpuid_init()` probes all relevant leaves; `cpu_has()` is the feature gate used everywhere. `simd_enable_avx()` sets `CR4.OSXSAVE` and `XCR0` bits 0-2 for the GFX subsystem, but `struct task` has no per-thread XSAVE area — all context switches still use the legacy 512-byte `FXSAVE`/`FXRSTOR`. `icon_store.c` saves FPU ad-hoc with `fxsave_area_t` rather than through the scheduler path. `msr.c`/`msr.h` are implemented with `msr_read()`/`msr_write()`/`msr_try_read()` (§3; inline migration deferred as low-priority). AMD SVM and Intel VT-x capability detection are fully logged via `cpu_has()` (§11).
>
> **Scope boundary with other TODOs — do NOT implement here:**
> - NX/EFER, SMEP/SMAP, KPTI, PCID, IBRS/retpoline, CET → `TODO-17`
> - TSC invariant check + TSC-Deadline APIC → `TODO-07`
> - HWP/CPPC frequency scaling + thermal monitoring → `TODO-15`
> - NUMA-aware page allocator → `03-memory-concurrency`
> - Hybrid P/E-core scheduler policy → `03-memory-concurrency`
> - CPU feature explorer GUI + chiplet visualizer → `08-desktop-shell`

---

## Inputs

- `src/kernel/gfx/gfx_simd.c` — `simd_enable_avx()`, `simd_save/restore_state()` (FXSAVE only); extend to full XSAVE
- `include/kernel/cpuid.h` — `xsave_size`, `xsave_size_max`, `xcr0_supported` fields already populated from leaf 0x0D
- `src/kernel/sched/task.c` — `struct task`; add `xsave_area` field
- `src/kernel/smp/smp.c` — `wrmsr`/`rdmsr` inline helpers; to be promoted to `msr.c`
- `src/kernel/mm/vmm.c` — `vmm_map_page`; extend for 1 GiB PS bit and PAT bits
- `src/kernel/idt.c` — IDT init; FRED replaces/supplements this (§6)
- → XREF: `TODO-06-irql-model-dpcs.md §3` — per-CPU IRQL; FRED event levels map to IRQL (§6)
- → XREF: `TODO-07-time-filetime-management.md §2` — RDTSCP in scheduler timing; §9 of this TODO programmes `IA32_TSC_AUX`
- → XREF: `TODO-15-power-management.md §9` — Driver Power Callbacks & Resume Ordering; CPU topology (§7) feeds the scheduler policy deferred to `03-memory-concurrency`
- → XREF: `TODO-17-kernel-security-hardening.md §1–§7` — NX, SMEP/SMAP, KPTI, PCID, IBRS, CET are already scoped there; this TODO does not touch those

---

## Outcome

- Every kernel context switch uses `XSAVE`/`XRSTOR` with the correct XSAVE area size for the present CPU; lazy FPU avoids saving unused state.
- AVX/AVX2 paths for `memcpy`, `memset`, and the framebuffer blit replace SSE2 where supported; AVX-512 optional fast paths enabled on supporting CPUs.
- A centralised `msr.c` replaces scattered inline `rdmsr`/`wrmsr` asm with a safe, whitelisted API that avoids `#GP` on unknown MSRs.
- `CR4.UMIP` prevents user-space from leaking GDT/IDT addresses.
- PKU provides per-thread 1 ns-cost memory sandboxing via `WRPKRU`.
- The framebuffer is mapped Write-Combining instead of UC, boosting pixel throughput 10–50× through 64-byte write-coalescing.
- 1 GiB pages reduce TLB miss count for large physical regions.
- FRED replaces IDT interrupt delivery on supporting CPUs; LKGS eliminates SWAPGS from the syscall path.
- `topology_init()` knows Zen CCD/NUMA layout and Intel P/E-core split.
- Intel PMU and AMD PMC expose per-core IPC and cache-miss counters.
- `IA32_TSC_AUX` is programmed per-CPU for accurate `RDTSCP`-based timing.
- A 2-second boot self-benchmark stores hardware capabilities in the Registry and auto-configures SIMD dispatch thresholds and scheduler quantum.

---

## Implementation Order

| ⭐  | Order | Deliverable                                        | Depends On           | Status |
| --- | :---: | -------------------------------------------------- | -------------------- | :----: |
| 💎  |   1   | XSAVE/XRSTOR state management (per-thread, lazy)   | —                    |  [x]   |
| 💎  |   2   | AVX/AVX2 + AVX-512 kernel paths                    | §1                   |  [ ]   |
| 💎  |   3   | MSR management infrastructure (`msr.c`)            | —                    |  [x]   |
| 💎  |   4   | UMIP + PKU protection keys                         | §3                   |  [ ]   |
| 💎  |   5   | 1 GiB huge pages + Write-Combining PAT             | §3                   |  [ ]   |
| 💎  |   6   | FRED event delivery + LKGS                         | §3, T06 §3           |  [ ]   |
| 💎  |   7   | CPU topology: Zen chiplets + Intel hybrid P/E-core | —                    |  [ ]   |
| 💎  |   8   | Performance monitoring counters (Intel + AMD)      | §3                   |  [ ]   |
| 💎  |   9   | OSVW errata + RDTSCP processor ID setup            | §3, T07 §2           |  [ ]   |
| 💎  |  10   | AMD IBS profiling (stretch)                        | §3                   |  [ ]   |
| 💎  |  11   | Virtualization detection (AMD-V + Intel VT-x)      | §3                   |  [x]   |
| ⭐  |  12   | Boot self-benchmark + auto-tune                    | §1, §2, §7           |  [ ]   |
| 💎  |  13   | Future silicon stubs: APX, UINTR, AVX10, LA57      | —                    |  [ ]   |

> 💎 = parity work — matches what Windows 11 and Linux already do.
> ⭐ = exclusive work — Impossible OS is superior or first.

---

## 1. XSAVE / XRSTOR State Management

### 1.1 Full XCR0 configuration

- [x] Replaced `simd_enable_avx()` XCR0 setup with `cpu_configure_xcr0()` in `cpuid.c`, called from `cpuid_init()`:
  1. Sets `CR4.OSXSAVE` (bit 18) — callable on BSP and APs
  2. Reads `g_cpu.xcr0_supported`, builds mask: bits 0-2 (x87+SSE+AVX), bits 5-7 (AVX-512 if AVX512F), bit 9 (PKRU if supported)
  3. Writes filtered mask via `XSETBV ecx=0`
  4. Stores in `g_cpu.xcr0_active`; `simd_enable_avx()` now just checks `xcr0_active`

### 1.2 Per-thread XSAVE areas

- [x] Added `xsave_area` (void*, 64-byte-aligned) and `fpu_used` (uint8_t) to `struct task`
- [x] `task_alloc_xsave(t)`: allocates from PMM (>4KB) or heap (<=4KB), size from `g_cpu.xsave_size_max` rounded to 64, zeroed, XSTATE_BV header bit 0 set
- [x] Lazy allocation: `xsave_area` starts NULL, allocated on first FPU use via `task_alloc_xsave()`

### 1.3 Lazy FPU: CR0.TS + #NM

- [x] Lazy FPU: `schedule()` sets `CR0.TS` after switch if `!next->fpu_used`; first FP/SIMD use faults into `#NM`
- [x] `#NM` handler (vector 7): clears `CR0.TS`, calls `task_alloc_xsave()` on first use, sets `fpu_used = 1`
- [x] Context switch: `XSAVE` prev task's state if `fpu_used`, `XRSTOR` next task's state if `fpu_used`, else set `CR0.TS`
- [ ] XSAVEOPT optimization — deferred (needs `CPU_FEATURE_XSAVEOPT` detection in CPUID)

### 1.4 Replace FXSAVE in icon_store.c

- [x] Removed `fxsave_area_t` + `simd_save_state`/`simd_restore_state` from `icon_store.c` (2 sites); lazy FPU via scheduler XSAVE handles it now
- [ ] `gfx_text.c` still has ~20 manual save/restore calls — future cleanup

### 1.5 Commit

- [x] Commit: `"kernel/simd: remove manual FXSAVE from icon_store, lazy FPU handles it"`

---

## 2. AVX / AVX2 + AVX-512 Kernel Paths `[Sonnet]`

### 2.1 AVX/AVX2 enable and fast paths

- [ ] `cpu_configure_xcr0()` (§1.1) enables `XCR0[2]` (AVX YMM halves) — this is the gate; `simd_avx2_ok` flag already exists in `gfx_simd.c`
- [ ] Add `-mavx2` to a per-file `CFLAGS_simd.c` (not all kernel files) and implement in `src/kernel/mm/memops.c`:
  - `memcpy_avx(dst, src, len)` — 256-bit `vmovdqu` / `vmovdqu` loop; handle head/tail < 32 bytes with scalar fallback
  - `memset_avx(dst, val, len)` — `vpbroadcastd` + `vmovdqa` store loop
- [ ] `vzeroupper` at the end of every AVX kernel function to prevent AVX→SSE transition penalties in subsequent SSE code
- [ ] `memcpy` / `memset` dispatch: `if (cpu_has(CPU_FEATURE_AVX2))` → AVX2 path; else `if (cpu_has(CPU_FEATURE_SSE2))` → existing SSE path
- [ ] Framebuffer blit: update `fb_blit_avx()` from `gfx_simd.c` to use YMM registers (8 pixels/iteration, already scaffolded but verify it uses XSAVE-safe approach after §1)

### 2.2 AVX-512 support (stretch)

- [ ] Enable `XCR0` bits 5-7 if `cpu_has(CPU_FEATURE_AVX512F)` and a frequency-throttle check passes: read `IA32_MPERF`/`IA32_APERF` ratio before and after a 10 µs AVX-512 burst; if ratio drops > 5%, disable AVX-512 (core throttling detected — common on consumer CPUs)
- [ ] Update XSAVE area allocation to use `xsave_size_max` (includes ZMM)
- [ ] `memcpy_avx512` and `fb_blit_avx512` (16 pixels/iteration) with `zmovdqu64` and `evmovdqu64`; gated by `simd_avx512_ok` flag
- [ ] `vzeroupper` still needed when mixing 512-bit and 128/256-bit code

### 2.3 AVX10 / APX stubs (future silicon)

- [ ] If `cpu_has(CPU_FEATURE_AVX10)`: log `[SIMD] AVX10 v%u detected — not yet enabled` — placeholder for future enablement when compilers fully support `-mavx10.N`
- [ ] If `CPUID.(7,1):EDX[21]` (APX): log detected; future work to set `XCR0[19]` (reuses MPX area) and compile with `-mapx`

### 2.4 Commit

- [ ] Commit: `"kernel/simd: AVX/AVX2 memcpy/memset/blit paths, AVX-512 opt-in with throttle guard"`

---

## 3. MSR Management Infrastructure

- [x] Create `src/kernel/msr.c` and `include/kernel/msr.h` — centralised `rdmsr`/`wrmsr` with inline functions:
  ```c
  uint64_t msr_read(uint32_t index);
  void     msr_write(uint32_t index, uint64_t value);
  bool     msr_try_read(uint32_t index, uint64_t *out); /* returns false on #GP */
  ```
- [x] MSR constants table in `include/kernel/msr.h` (22 constants including Intel, AMD, and Hyper-V):
  ```c
  #define MSR_IA32_EFER          0xC0000080
  #define MSR_IA32_STAR          0xC0000081
  #define MSR_IA32_LSTAR         0xC0000082
  #define MSR_IA32_FMASK         0xC0000084
  #define MSR_IA32_FS_BASE       0xC0000100
  #define MSR_IA32_GS_BASE       0xC0000101
  #define MSR_IA32_KERNEL_GS_BASE 0xC0000102
  #define MSR_IA32_TSC_AUX       0xC0000103
  #define MSR_IA32_PAT           0x00000277
  #define MSR_IA32_APIC_BASE     0x0000001B
  #define MSR_IA32_SPEC_CTRL     0x00000048
  #define MSR_IA32_PRED_CMD      0x00000049
  #define MSR_IA32_ARCH_CAPS     0x0000010A
  #define MSR_IA32_MPERF         0x000000E7
  #define MSR_IA32_APERF         0x000000E8
  #define MSR_IA32_PERF_GLOBAL_CTRL 0x0000038F
  #define MSR_IA32_FIXED_CTR0    0x00000309
  #define MSR_AMD_OSVW_ID_LEN    0xC0010140
  #define MSR_AMD_OSVW_STATUS    0xC0010141
  #define MSR_AMD_PERF_CTL0      0xC0010200
  #define MSR_AMD_PERF_CTR0      0xC0010201
  ```
- [x] `msr_try_read()` — temporarily installs #GP handler, attempts rdmsr, skips instruction on fault; returns -1 if unsupported
- [ ] Migrate existing inline `rdmsr`/`wrmsr` in `smp.c`, `lapic.c`, `acpi.c` to use `msr_read()`/`msr_write()` — deferred (mechanical, low priority)
- [x] Commit: `"kernel/msr: centralised MSR read/write infrastructure, #GP-safe msr_try_read"`

---

## 4. UMIP + PKU Protection Keys `[Sonnet]`

### 4.1 UMIP (User-Mode Instruction Prevention)

- [ ] `cpu_enable_umip()` — `if (cpu_has(CPU_FEATURE_UMIP)) cpu_set_cr4_bit(CR4_UMIP)` where `CR4_UMIP = (1ULL << 11)`; called in Phase 1 on BSP and each AP
- [ ] Effect: user-space `SGDT`, `SIDT`, `SLDT`, `SMSW`, `STR` raise `#GP` instead of revealing GDT/IDT base addresses; eliminates a trivial kernel address leak
- [ ] Verify: user-mode test `SGDT [ptr]` after `cpu_enable_umip()` must fault with `#GP` (error code = 0)

### 4.2 PKU (Protection Keys for User-space)

- [ ] Enable: `if (cpu_has(CPU_FEATURE_PKU)) cpu_set_cr4_bit(CR4_PKE)` where `CR4_PKE = (1ULL << 22)`
- [ ] Add PKRU to XCR0 (bit 9) in `cpu_configure_xcr0()` (§1.1) — ensures per-thread `PKRU` state is saved/restored automatically
- [ ] Kernel API in `src/kernel/security/pku.c`:
  ```c
  int      pku_alloc_key(void);                /* returns key 1–15; 0 = default */
  void     pku_free_key(int key);
  void     pku_set_permissions(int key, uint32_t flags); /* wraps WRPKRU */
  #define  PKU_ACCESS_DISABLE  0x1
  #define  PKU_WRITE_DISABLE   0x2
  ```
- [ ] PTE key field: bits 62:59 in each page table entry; add `pku_key` field to `vmm_map_page` flags parameter
- [ ] Win32 API surface: `SetThreadMemoryZone(zone_id, ACCESS_NONE)` → `pku_set_permissions(zone_id, PKU_ACCESS_DISABLE)` — zero-cost switch without any syscall, using `WRPKRU` (~1 ns vs ~1 µs for `mprotect`)
- [ ] PKRU initial value: `0x55555554` (disable access to keys 1-15 by default for user threads; key 0 = full access always)

### 4.3 Commit

- [ ] Commit: `"kernel/security: UMIP (CR4.UMIP), PKU protection keys, SetThreadMemoryZone"`

---

## 5. 1 GiB Huge Pages + Write-Combining PAT `[Sonnet]`

### 5.1 1 GiB huge pages

- [ ] Check `cpu_has(CPU_FEATURE_PAGE1GB)` before use
- [ ] VMM extension: in `vmm_map_range(va, pa, size, flags)` — if size ≥
  1 GiB and both `va` and `pa` are 1 GiB-aligned and `CPU_FEATURE_PAGE1GB`: use PDPTE with `PS = 1` (bit 7) instead of a PD → PT chain
- [ ] `vmm_map_huge_1g(va, pa, flags)` — single-entry helper: set PDPTE `PS` bit; physical address in bits [51:30]
- [ ] Use at boot: identity-map the first N GiB of physical RAM with 1 GiB pages where N is aligned to 1 GiB; reduces TLB miss pressure for large DMA buffers and MMIO regions
- [ ] Fallback: if `PAGE1GB` not set, silently fall through to 2 MiB pages

### 5.2 Write-Combining PAT configuration

- [ ] Read `IA32_PAT` MSR (`0x277`) at boot; ensure entry 1 = `0x01` (WC — Write-Combining type); default PAT entry 1 is WC on most CPUs but verify explicitly:
  ```c
  uint64_t pat = msr_read(MSR_IA32_PAT);
  pat = (pat & ~(0xFFULL << 8)) | (0x01ULL << 8); /* entry 1 = WC */
  msr_write(MSR_IA32_PAT, pat);
  ```
- [ ] Add `PTE_WC` flag: `PCD=0, PWT=1, PAT=0` (selects PAT entry 1 = WC) in the PTE; encode in `vmm_map_wc(va, pa, size)`
- [ ] Update `fb_init()` in `src/kernel/drivers/framebuffer.c`: replace current uncached mapping with `vmm_map_wc(fb_va, fb_pa, fb_size)`; benchmark: log `fb_swap()` time before and after; expect 10–50× speedup

### 5.3 Commit

- [ ] Commit: `"kernel/mm: 1 GiB huge pages, Write-Combining PAT for framebuffer"`

---

## 6. FRED Event Delivery + LKGS `[Opus]`

### 6.1 FRED (Flexible Return and Event Delivery)

- [ ] Check `cpu_has(CPU_FEATURE_FRED)` (CPUID leaf 7, ECX=1, EAX bit 17)
- [ ] FRED delivers all events (interrupts, exceptions, `SYSCALL`) to a single kernel entry point with the event type and vector in registers; eliminates IDT corruption edge-cases and provides native NMI nesting
- [ ] Enable: `cpu_set_cr4_bit(CR4_FRED)` where `CR4_FRED = (1ULL << 32)`; configure `IA32_FRED_CONFIG` MSR with:
  - Kernel stack pointer (for Level 0 — ring 0 events)
  - Unified entry RIP (`fred_entry_asm` in `src/kernel/fred.asm`)
- [ ] `fred_entry_asm` unified handler:
  - Reads event type and vector from the FRED stack frame
  - Dispatches to the existing `isr_handler(frame)` or `kd_debug_exception_handler` (→ XREF `TODO-18-kernel-debugger-kd-protocol.md §4`) via the same `handlers[]` table in `idt.c`
- [ ] Replace `IRET` with FRED return instructions where FRED is active:
  - `ERETS` — return to ring 0
  - `ERETU` — return to ring 3
- [ ] `SYSCALL` via FRED: automatically uses FRED entry; `IA32_LSTAR` still needed for non-FRED fallback path
- [ ] Fallback: if `CPU_FEATURE_FRED` not present, IDT path unchanged

### 6.2 LKGS instruction

- [ ] Check `cpu_has(CPU_FEATURE_LKGS)` (CPUID leaf 7, ECX=1, EAX bit 18)
- [ ] `LKGS reg` writes directly to `IA32_KERNEL_GS_BASE` without touching the active GS — eliminates `SWAPGS` and its speculative side-channel
- [ ] In `src/kernel/sched/syscall_entry.asm`:
  - On SYSCALL entry: `LKGS [saved_user_gs]` instead of `SWAPGS`
  - On SYSRET: `LKGS [user_gs_base]` before `SYSRETQ`
- [ ] Same replacement in IDT common stubs that currently use `SWAPGS`
- [ ] Fallback: if LKGS unavailable, keep existing `SWAPGS` (no regression)

### 6.3 Commit

- [ ] Commit: `"kernel/cpu: FRED unified event delivery, LKGS replaces SWAPGS in syscall path"`

---

## 7. CPU Topology: Zen Chiplets + Intel Hybrid `[Sonnet]`

### 7.1 topology_init()

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
- [ ] `topology_init()` — called from Phase 1 after `cpuid_init()` and SMP probe; for each logical CPU: run per-CPU leaf queries via IPI or on-CPU during AP startup

### 7.2 AMD Zen topology

- [ ] If `cpu_has(CPU_FEATURE_TOPO_EXT)` (from §1.2, already in cpuid.c):
  - Per-CPU `CPUID 0x8000001E`: `ccd_id = EBX[7:0]`, `node_id = ECX[7:0]`, `smt_siblings = EBX[15:8] + 1`
  - Log: `[topo] Zen: %u CCDs, %u NUMA nodes, NPS=%u`
- [ ] `g_numa_nodes` = max `node_id` + 1; expose to PMM as a hint (NUMA allocator implementation deferred to `03-memory-concurrency`)

### 7.3 Intel hybrid topology

- [ ] If `CPUID.(7,0):EDX[15]` (hybrid bit) set:
  - Per-CPU `CPUID 0x1A`: `EAX[31:24]` = core type (0x40=P, 0x20=E)
  - Per-CPU `CPUID 0x1F` (V2 Extended Topology): enumerate SMT/Core/Die levels; map to `core_id` and `ccd_id`
  - Populate `p_core_mask` and `e_core_mask` bitmasks
  - Log: `[topo] Intel hybrid: %u P-cores, %u E-cores`
- [ ] Fallback: no hybrid, all cores treated as `CORE_TYPE_GENERIC`

### 7.4 Commit

- [ ] Commit: `"kernel/topology: Zen CCD/NUMA parser, Intel hybrid P/E-core detection"`

---

## 8. Performance Monitoring Counters: Intel + AMD `[Sonnet]`

### 8.1 Intel PMU

- [ ] Check CPUID leaf `0x0A`: PMU version (EAX[7:0]), counter count (EAX[15:8]), counter width (EAX[23:16])
- [ ] `pmc_intel_start(slot, event_select, unit_mask)`:
  - Write event to `IA32_PERFEVTSELx` MSR (0x186+slot): `event_select | (unit_mask << 8) | ENABLE_BIT | OS_BIT | USR_BIT`
  - Clear `IA32_PMCx` (0xC1+slot)
- [ ] `pmc_intel_read(slot)` → `msr_read(0xC1 + slot)`
- [ ] `pmc_intel_stop(slot)` → clear `IA32_PERFEVTSELx` enable bit
- [ ] Pre-defined event constants:
  ```c
  #define PMC_INTEL_INST_RETIRED    0xC0  /* instructions retired */
  #define PMC_INTEL_UNHALTED_CYCLES 0x3C  /* CPU_CLK_UNHALTED.THREAD */
  #define PMC_INTEL_LLC_MISSES      0x2E  /* LAST_LEVEL_CACHE.MISS */
  #define PMC_INTEL_BR_MISPREDICT   0xC5  /* BR_MISP_RETIRED */
  ```

### 8.2 AMD PMC

- [ ] AMD Zen uses `MSR_AMD_PERF_CTL0` (0xC0010200) + `MSR_AMD_PERF_CTR0` (0xC0010201) per counter (6 per core on Zen 4)
- [ ] `pmc_amd_start(slot, event)` → `msr_write(0xC0010200 + slot*2, event | ENABLE)`
- [ ] `pmc_amd_read(slot)` → `msr_read(0xC0010201 + slot*2)`
- [ ] `pmc_amd_stop(slot)` → clear enable bit
- [ ] Pre-defined events: `0x76` (CPU clocks), `0xC0` (instructions retired), `0xC2` (retired branches), `0x64` (DRAM accesses)

### 8.3 Unified PMC API

- [ ] `pmc_start(slot, event)` / `pmc_read(slot)` / `pmc_stop(slot)` — dispatch to Intel or AMD path based on `cpu_features.vendor`
- [ ] `pmc_ipc()` — shortcut: start inst+cycle counters; read after 1 ms; return `instructions / cycles` as fixed-point; used by Task Manager (`08-desktop-shell`) for per-process IPC display
- [ ] Enable via `IA32_PERF_GLOBAL_CTRL` (Intel, MSR 0x38F); AMD counters enabled per-counter via the `ENABLE` bit in CTL

### 8.4 Commit

- [ ] Commit: `"kernel/pmc: Intel PMU + AMD PMC, pmc_start/read/stop, pmc_ipc() for Task Manager"`

---

## 9. OSVW Errata + RDTSCP Processor ID Setup `[Sonnet]`

### 9.1 OSVW errata workarounds

- [ ] If `cpu_has(CPU_FEATURE_OSVW)`:
  - `msr_read(MSR_AMD_OSVW_ID_LEN)` → number of errata tracked
  - `msr_read(MSR_AMD_OSVW_STATUS)` → bitmask of active errata
  - Store in `cpu_features.osvw_length` / `cpu_features.osvw_status`
  - `cpu_has_erratum(n)` → `(osvw_status >> n) & 1`
  - Log: `[cpu] OSVW: %u errata tracked, mask=0x%llx`
- [ ] Apply per-erratum workarounds as they are identified from the AMD PPR for the detected family/model; initially a no-op table is fine

### 9.2 RDTSCP processor ID

- [ ] If `cpu_has(CPU_FEATURE_RDTSCP)`:
  - During AP startup in `ap_startup_c()`: `msr_write(MSR_IA32_TSC_AUX, this_cpu_id)` — stores logical CPU ID in `IA32_TSC_AUX`
  - `rdtscp_read(tsc, cpu_id)` — inline wrapper:
    ```c
    __asm__ volatile("rdtscp" : "=A"(*tsc), "=c"(*cpu_id));
    ```
  - Use in `TODO-07-time-filetime-management.md §2` scheduler for nanosecond-accurate per-CPU timestamps; XREF noted in Inputs
- [ ] BSP: also write `IA32_TSC_AUX` with CPU 0 during Phase 1 init

### 9.3 Commit

- [ ] Commit: `"kernel/cpu: OSVW errata table, RDTSCP per-CPU IA32_TSC_AUX setup"`

---

## 10. AMD IBS Profiling (Stretch) `[Sonnet]`

### 10.1 IBS Fetch + Op Sampling

- [ ] Check `cpu_has(CPU_FEATURE_IBS)` (from §1.2)
- [ ] **IBS Fetch Sampling** — samples random instruction fetch ops:
  - Configure `MSR 0xC001_1030` (IBS_FETCH_CTL): set `IbsFetchEn (bit 17)`, `IbsFetchCnt (bits 15:0)` = sample rate (~100K ops)
  - On NMI: read `0xC001_1031` (linear fetch address) + `0xC001_1032` (physical + status: cache hit/miss, iTLB miss, L1/L2/L3 source)
- [ ] **IBS Op Sampling** — samples random instruction completion ops:
  - Configure `MSR 0xC001_1033` (IBS_OP_CTL): `IbsOpEn + IbsOpCnt`
  - On NMI: read `0xC001_1035` (RIP), `0xC001_1036` (op data: micro-op info, cache miss, DRAM latency), `0xC001_1037` (data address + NUMA source)
- [ ] IBS NMI handler: read all IBS MSRs; pack into a ring buffer of `ibs_sample_t` structs (256 entries per CPU, static allocation); re-arm counter; return from NMI
- [ ] `ibs_start(rate)` / `ibs_stop()` / `ibs_read_samples(buf, max)` API
- [ ] Future: wire to a profiler GUI in `10-apps`

### 10.2 Commit

- [ ] Commit: `"kernel/pmc: AMD IBS fetch+op sampling, NMI handler, sample ring buffer"`

---

## 11. Virtualization Detection (AMD-V + Intel VT-x)

AMD SVM / Intel VMX capability reporting

- [x] AMD SVM: CPUID 0x8000000A parsed — revision, NPT, ASIDs logged
- [x] Intel VT-x: `CPU_FEATURE_VMX` added (CPUID.1:ECX[5]); `IA32_FEATURE_CONTROL` MSR read for lock + enable status
- [x] Exposed via `cpu_has(CPU_FEATURE_SVM)` / `cpu_has(CPU_FEATURE_VMX)` — detection only
- [x] Commit: `"kernel/cpu: AMD SVM + Intel VT-x capability detection and logging"`

---

## 12. Boot Self-Benchmark + Auto-Tune `[Opus]`

### 12.1 hw_profile struct

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
- [ ] Stored in `HKLM\SYSTEM\HwProfile\*` via Registry (→ XREF `TODO-13-registry-completion.md §4`)
- [ ] Stale if CPU brand string has changed since last boot (different hardware)

### 12.2 Benchmark suite

- [ ] Run during Phase 2 init (after SMP, before GUI) if `HwProfile` is stale or absent; total runtime ≤ 2 seconds:
  - **Memory bandwidth**: `VMOVDQA` 256 MiB sequential write; measure MB/s from TSC delta
  - **Memory latency**: 4 MiB pointer-chase array (stride = cache-line); measure ns/access
  - **Cache sizes**: stride binary search — L1 ↔ L2 ↔ L3 inflection at typical ~32 KiB / ~512 KiB / ~8 MiB; log `l1/l2/l3_size_kb`
  - **SIMD throughput**: timed SSE2 / AVX / AVX-512 alpha-blend loop over
    4 MiB framebuffer; record Gpix/s per ISA level
  - **Context switch**: 1000 yield-pairs between two kernel tasks; `rdtsc` around each switch; average ns per switch
- [ ] Write `hw_profile_t` to Registry

### 12.3 Auto-tune decisions

- [ ] SIMD dispatch: if `avx_gpix / sse2_gpix < 1.10` (< 10% gain — throttling detected): set `simd_avx2_ok = 0`; use SSE2 path to avoid frequency reduction
- [ ] Scheduler quantum: if `ctx_switch_ns < 500` → set `SCHED_TICK_US = 100`; if `ctx_switch_ns > 2000` → set `SCHED_TICK_US = 500`
- [ ] Compositor triple-buffering: if `mem_bandwidth_mb_s > 20 000` → enable
- [ ] `memcpy` threshold: if `l1_size_kb == 32` → `REP MOVSB` for < 128 B, SIMD for larger

### 12.4 Commit

- [ ] Commit: `"kernel/bench: boot self-benchmark, hw_profile Registry, auto-tune SIMD/scheduler"`

---

## 13. Future Silicon Extensions `[Sonnet]`

### 13.1 UINTR (User Interrupts)

- [ ] Stub: if `CPUID.(7,0):EDX[5]` set: log `[cpu] UINTR present — not yet enabled`; structure definitions in `include/kernel/uintr.h` (UPID, UITT); full implementation deferred until UINTR reaches mainstream silicon

### 13.2 LA57 (5-level paging)

- [ ] Stub: if `CPUID.(7,0):ECX[16]` set: log `[vmm] LA57 (57-bit VA) detected — 4-level paging active`; enabling at runtime requires a full VMM rewrite; relevant for server SKUs with > 256 TiB virtual address space

### 13.3 Commit

- [ ] Commit: `"kernel/cpu: UINTR/LA57 detection stubs, future silicon log entries"`

---

## OS Comparison

| ⭐  | Feature                                 | 🪟 Windows 11                      | 🐧 Linux 6.x                         | 🚀 Impossible OS                            |
| --- | --------------------------------------- | ---------------------------------- | ------------------------------------ | ------------------------------------------- |
| 💎  | CPUID feature detection                 | ✅ `KeQueryProcessorFeature`        | ✅ `arch/x86/kernel/cpu/`             | ✅ Done — `cpuid_init()`, 44+ features      |
| 💎  | AMD extended CPUID (IBS, OSVW, Page1GB) | ✅ HAL AMD leaves                  | ✅ `arch/x86/kernel/cpu/amd.c`        | ✅ Done — §1.2 complete                     |
| 💎  | XSAVE/XRSTOR per-thread (lazy FPU)      | ✅ Full                            | ✅ `fpu__*` framework                 | ⬜ Planned — §1 (FXSAVE only today)        |
| 💎  | AVX/AVX2 kernel paths                   | ✅ Full                            | ✅ `kernel_fpu_begin/end`              | ⚠️ Partial — §2 (GFX only today)           |
| 💎  | AVX-512 opt-in with throttle guard      | ✅ Full                            | ✅ (with throttling awareness)         | ⬜ Planned — §2.2                          |
| 💎  | Centralised safe MSR API                | ✅ HAL wrappers                    | ✅ `rdmsrl_safe()` / `wrmsrl_safe()`   | ✅ Done — §3                               |
| 💎  | UMIP (block user SGDT/SIDT)             | ✅ Enabled                         | ✅ Enabled (4.15+)                     | ⬜ Planned — §4                            |
| 💎  | PKU memory protection keys              | ✅ (Win 10 1903+, no user API)     | ✅ `pkey_alloc()` / `pkey_mprotect()` | ⬜ Planned — §4 (`SetThreadMemoryZone()`)  |
| 💎  | Write-Combining PAT for framebuffer     | ✅ DirectComposition GPU           | ✅ `ioremap_wc()` DRM                  | ⬜ Planned — §5 (UC today — 10–50× speedup)|
| 💎  | 1 GiB huge pages                        | ✅ `MmMapLargePages`               | ✅ `hugetlb` 1GB                       | ⬜ Planned — §5                            |
| 💎  | FRED event delivery                     | 🔜 (planned post-2024)            | ✅ kernel 6.9+                         | ⬜ Planned — §6                            |
| 💎  | LKGS (replace SWAPGS)                   | 🔜                                | ✅ kernel 6.4+                         | ⬜ Planned — §6                            |
| 💎  | Zen CCD/NUMA topology                   | ✅ `KeQueryNodeActiveAffinity`     | ✅ `topology_amd_node_id()`            | ⬜ Planned — §7                            |
| 💎  | Intel hybrid P/E-core detection         | ✅ Thread Director (Win 11+)       | ✅ HFI (kernel 5.18+)                  | ⬜ Planned — §7                            |
| 💎  | Intel PMU / AMD PMC                     | ✅ ETW + WPA                       | ✅ `perf`                              | ⬜ Planned — §8                            |
| 💎  | OSVW silicon errata table               | ✅ HAL reads OSVW MSRs             | ✅ `arch/x86/kernel/cpu/amd.c`         | ⬜ Planned — §9                            |
| 💎  | RDTSCP per-CPU IA32_TSC_AUX             | ✅ `KeQueryPerformanceCounter`     | ✅ TSC_AUX per-CPU on SMP              | ⬜ Planned — §9                            |
| 💎  | AMD IBS profiling                       | ⚠️ AMD µProf external             | ✅ `perf` IBS (5.19+)                  | ⬜ Planned — §10 (stretch)                |
| 💎  | SVM / VT-x capability detection         | ✅ HAL                             | ✅ `kvm_amd` / `kvm_intel`             | ✅ Done — §11                              |
| ⭐  | Boot self-benchmark + auto-tune          | ❌ Static heuristics               | ❌ Static heuristics                   | ⬜ **Planned — §12** 🚀                    |
| ⭐  | `SetThreadMemoryZone()` PKU API          | ❌ No user-facing PKU API          | ⚠️ Raw `pkey_*` syscalls               | ⬜ **Planned — §4.2** 🚀                   |
| ⭐  | Per-core frequency graph (Task Manager)  | ❌ Single % bar                    | ❌ `turbostat` CLI                     | ⬜ **Planned via §8 + `08-desktop-shell`** 🚀 |

After §1–11, Impossible OS reaches full Windows 11 / Linux 6.x parity across
the entire x86-64 architecture enhancement stack. Linux ships FRED (6.9),
LKGS (6.4), IBS, and fine-grained NUMA topology; Windows ships Thread Director,
HWP, and PCID. Both rely on static compile-time heuristics. The boot
self-benchmark (§12) is the unique differentiator: it measures the actual
silicon, writes capabilities to the Registry, and auto-tunes SIMD dispatch
thresholds, scheduler quantum, and compositor buffering — making the kernel
self-configuring for any hardware from a 10-year-old laptop to a 128-core EPYC.
`SetThreadMemoryZone()` (§4.2) gives user-space a clean Win32-style API over
PKU that neither Windows (hidden) nor Linux (raw syscalls) exposes.

---

## Verification

- [ ] **XSAVE round-trip**: load YMM0–15 with known values; context switch to another task; switch back; verify YMM values intact via `XRSTOR` + read.
- [ ] **Lazy FPU**: create a task that never uses FP; verify its `xsave_area` is NULL and no `XSAVE` is issued on context switch.
- [ ] **AVX memcpy**: `memcpy_avx(dst, src, 1 MiB)`; compare byte-for-byte with reference; measure throughput > 2× SSE2 baseline on AVX2 hardware.
- [ ] **WC framebuffer**: compare `fb_swap()` TSC duration before and after PAT WC mapping; expect ≥ 5× reduction on a 1080p framebuffer.
- [ ] **UMIP**: user-mode `SGDT [ptr]` after `cpu_enable_umip()` must receive `SIGSEGV` / `#GP`; kernel `SGDT` still works.
- [ ] **PKU**: allocate key 3; set `PKU_ACCESS_DISABLE`; try reading a page tagged with key 3 — must raise `#PF` with PKU violation bit set in error code.
- [ ] **RDTSCP**: after `topology_init()` + TSC\_AUX setup, `RDTSCP` on each CPU returns its correct logical CPU ID in ECX.
- [ ] **PMC IPC**: `pmc_ipc()` on a tight integer loop returns IPC > 1.0.
- [ ] **Boot benchmark**: on first boot, `HKLM\SYSTEM\HwProfile\MemBandwidthMbS` is non-zero; `simd_avx2_ok` reflects the benchmark result.
- [ ] **Remaining limits**: FRED requires hardware support (Intel Granite Rapids or later); test in QEMU with `-cpu Cooperlake,fred=on`; APX/UINTR deferred until compiler support (`-mapx`) is stable; LA57 requires bootloader + VMM rewrite before enabling.
- [ ] Commit: `"kernel/x86: XSAVE/XRSTOR, AVX/AVX-512, MSR layer, UMIP/PKU, WC PAT, FRED/LKGS, topology, PMC, OSVW, IBS, VT-x/SVM, boot self-benchmark"`
