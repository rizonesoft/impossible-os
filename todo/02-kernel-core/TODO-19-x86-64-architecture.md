# TODO-19 -- x86-64 Architecture Enhancements

> **Goal:** Activate and exploit the x86-64 architecture features that `cpuid.c` already detects, pushing beyond today's partial enablement: full XSAVE/XRSTOR state management with per-thread XSAVE areas and lazy FPU, AVX/AVX2 optimised kernel paths and AVX-512 support, a centralised MSR access layer, UMIP and PKU protection keys, 1 GiB huge pages and Write-Combining PAT for the framebuffer, FRED event delivery with LKGS, CPU topology parsing (Zen chiplets + Intel P/E-cores), performance monitoring counters (Intel PMU + AMD PMC), OSVW errata + RDTSCP setup, AMD IBS profiling, virtualization detection, and a boot-time self-benchmark that auto-tunes the kernel to the detected hardware.

> [!IMPORTANT]
> **Current state:** `cpuid_init()` / `cpu_has()` gate the feature set. **Done [x]:** §1 XSAVE/XRSTOR with per-thread lazy FPU. §2 AVX2 memops + framebuffer blit with SSE2 fallbacks. §3 AVX-512 opt-in with MPERF/APERF throttle guard. §4 `msr_read`/`msr_write`/`msr_try_read` plus MSR constants. §5 UMIP + PKU protection keys. §6 1 GiB huge pages (PDPT promotion) + Write-Combining PAT (framebuffer WC-mapped). §13 AMD SVM + Intel VT-x detection. **Open [ ]:** §7 through §12, §14 through §16 per Implementation Order.
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

- `src/kernel/gfx/gfx_simd.c`: `simd_enable_avx()`, blit helpers; AVX2 memops land in §2, AVX-512 in §3
- `include/kernel/cpuid.h`: `xsave_size`, `xsave_size_max`, `xcr0_supported` fields already populated from leaf 0x0D
- `src/kernel/sched/task.c`: `struct task` holds `xsave_area` / `fpu_used` (§1)
- `src/kernel/smp/smp.c`: per-CPU bring-up; MSR access via `msr.c` (§4)
- `src/kernel/mm/vmm.c`: `vmm_map_page`; extend for 1 GiB PS bit and PAT bits
- `src/kernel/idt.c`: IDT init; FRED replaces/supplements this (§7)
- -> XREF: `TODO-06-irql-model-dpcs.md §3`: per-CPU IRQL; FRED event levels map to IRQL (§7)
- -> XREF: `TODO-07-time-filetime-management.md §3`: per-CPU TSC read path (`rdtsc_ns`); §11 here programmes `IA32_TSC_AUX` for `RDTSCP` CPU id in ECX
- -> XREF: `TODO-15-power-management.md §9`: Driver Power Callbacks & Resume Ordering; CPU topology (§9) feeds the scheduler policy deferred to `03-memory-concurrency`
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
| 💎  |   2   | AVX/AVX2 kernel memops + framebuffer blit           | §1                   |  [x]   |
| 💎  |   3   | AVX-512 opt-in + future silicon detection           | §2                   |  [x]   |
| 💎  |   4   | MSR management infrastructure (`msr.c`)             | (none)               |  [x]   |
| 💎  |   5   | UMIP + PKU protection keys                          | §4                   |  [x]   |
| 💎  |   6   | 1 GiB huge pages + Write-Combining PAT              | §4                   |  [x]   |
| 💎  |   7   | FRED unified event delivery                         | §4, T06 §3           | blocked |
| 💎  |   8   | LKGS fast GS-base swap                              | §4                   | blocked |
| 💎  |   9   | CPU topology: Zen chiplets + Intel hybrid P/E-core  | (none)               |  [x]   |
| 💎  |  10   | Performance monitoring counters (Intel + AMD)       | §4                   |  [x]   |
| 💎  |  11   | OSVW errata + RDTSCP processor ID setup             | §4, T07 §3           |  [x]   |
| 💎  |  12   | AMD IBS profiling (stretch)                         | §4                   |  [ ]   |
| 💎  |  13   | Virtualization detection (AMD-V + Intel VT-x)       | §4                   |  [x]   |
| ⭐  |  14   | Boot self-benchmark + auto-tune                     | §1, §2, §9           |  [ ]   |
| 💎  |  15   | Future silicon stubs: APX, UINTR, AVX10, LA57       | (none)               |  [ ]   |
| 💎  |  16   | Boot page tables: 1 GiB pages from entry.asm        | §6                   |  [ ]   |

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

> **Test runner:** `scripts\debug\run-x86-tests.bat` (SUITE=x86) -- 10 suites, 0 failures expected. S1 tests: xcr0_active non-zero, xsave_size_max >= 512, xcr0 x87+SSE bits, CR4.OSXSAVE set.

> **Verified:** 2026-04-13 -- all 13 items confirmed. 6 bugs found and fixed: (1) CLTS before XRSTOR in schedule(); (2) #NM handler loads clean FPU state via XRSTOR after allocation; (3) FPU save+restore in schedule_now() cooperative path; (4) MXCSR default 0x1F80 in task_alloc_xsave() (zeroed MXCSR unmasks SIMD exceptions, causing #XM vector 19; root cause of WHPX freeze); (5) FCW default 0x037F in task_alloc_xsave() (zeroed FCW unmasks x87 exceptions; masked on WHPX by XRSTOR init optimization, loaded directly by FXRSTOR on TCG); (6) CLTS before FXSAVE/XSAVE in both scheduler paths (not just before restore; all FPU instructions fault #NM when CR0.TS=1). Accepted: none.
> **Quality reviewed:** 2026-04-13 -- kernel-code-quality 11 gates walked. G11: FCW 0x037F + MXCSR 0x1F80 per Intel SDM (not 0 from zeroed buffer). CR0.TS cleared before ALL FPU instructions (saves and restores). Cooperative + preemptive paths both handle FPU. Parity: matches Windows/Linux lazy FPU + MXCSR/FCW init. Accepted: none.

---

## 2. AVX/AVX2 Kernel Memops + Framebuffer Blit

- [x] `cpu_configure_xcr0()` (§1) enables `XCR0[2]` (AVX YMM halves); this is the gate; `simd_avx2_ok` flag already exists in `gfx_simd.c`
- [x] Add `-mavx2` to a per-file `AVX2_CFLAGS` (not all kernel files) and implement in `src/kernel/mm/memops.c`: `memcpy_avx(dst, src, len)` using 256-bit `vmovdqu` loop with scalar tail; `memset_avx(dst, val, len)` using `vpbroadcastb` + `vmovdqu` store loop; also SSE2 fallbacks `memcpy_sse2`/`memset_sse2` using 128-bit `movdqu`
- [x] `vzeroupper` at the end of every AVX kernel function (`memcpy_avx`, `memset_avx`, `fb_blit_avx`, `fb_fill_avx`) to prevent AVX->SSE transition penalties
- [x] `memcpy_fast` / `memset_fast` dispatch: `if (cpu_has(CPU_FEATURE_AVX2))` -> AVX2 path; else `if (cpu_has(CPU_FEATURE_SSE2))` -> SSE2 path; else scalar fallback
- [x] Framebuffer blit: `fb_blit_avx()` and `fb_fill_avx()` in `gfx_simd.c` using YMM registers (8 pixels/iteration); `framebuffer.c` dispatches via `simd_avx2_ok` flag in `mem_cpy32`/`mem_set32`
- [x] Commit: `"kernel/simd: AVX/AVX2 memcpy/memset/blit dispatch with vzeroupper"`

**Test checkpoint:** On AVX2-class CPU, `memcpy_avx` / `memset_avx` byte-match scalar reference on 64 KiB; `vzeroupper` emitted at function end; dispatch falls back to SSE2 on TCG. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

**Test runner:** `scripts\debug\run-x86-tests.bat` (SUITE=x86, 16 suites expected, 0 failures)

> **Verified:** 2026-04-13 -- all 6 items confirmed. `memcpy_avx`/`memset_avx` at `memops.c` (vmovdqu/vpbroadcastb loops with vzeroupper). SSE2 fallbacks + dispatch in `memops_sse.c` (separate TU, compiled with `-msse2` only; split from `-mavx2` file because clang emits VEX-encoded instructions that crash TCG `qemu64`). `fb_blit_avx`/`fb_fill_avx` at `gfx_simd.c`. Framebuffer dispatch at `framebuffer.c`. 6 tests under TEST_CAT_X86. Codex adversarial: fb_blit bounds overflow fixed (overflow-safe clamping); test gates use `simd_avx2_ok`; fill broadcast hoisted. TCG crash fixed: SSE2 code must never be compiled with `-mavx2`. Accepted: AP XCR0 (XREF TODO-04 S4); memops dispatch infrastructure-only until kernel_fpu_begin/end.
> **Quality reviewed:** 2026-04-13 -- kernel-code-quality 11 gates walked. Intel SDM Vol. 1 Section 14.3 AVX/SSE transition compliant (vzeroupper on all 4 AVX functions). SSE2 and AVX2 in separate translation units (no VEX leakage to non-AVX CPUs). fb_blit bounds hardened. Fill loops broadcast once per call. Verified on WHPX (AVX2 path) and TCG (SSE2 fallback). Parity: matches Linux arch/x86/lib/memcpy_64.S dispatch + Windows NT RtlCopyMemory. Accepted: none.

---

## 3. AVX-512 Opt-In + Future Silicon Detection

- [x] Enable `XCR0` bits 5-7 if `cpu_has(CPU_FEATURE_AVX512F)` and a frequency-throttle check passes: read `IA32_MPERF`/`IA32_APERF` ratio before and after a 10 us AVX-512 burst; if ratio drops > 5%, disable AVX-512 (core throttling detected; common on consumer CPUs)
- [x] Update XSAVE area allocation to use `xsave_size_max` (includes ZMM) **Note:** already done in §1; `task_alloc_xsave()` at `task.c:212` uses `g_cpu.xsave_size_max` which includes ZMM state when AVX-512 is detected by CPUID leaf 0x0D
- [x] `memcpy_avx512` and `fb_blit_avx512` (16 pixels/iteration) with `vmovdqu64`; gated by `simd_avx512_ok` flag. Separate TUs: `memops_avx512.c` compiled with `-mavx512f`, `gfx_simd_avx512.c` compiled with `-mavx512f`. `memset_avx512` and `fb_fill_avx512` also implemented
- [x] `vzeroupper` at exit of every AVX-512 function (`memcpy_avx512`, `memset_avx512`, `fb_blit_avx512`, `fb_fill_avx512`, `simd_avx512_burst`)
- [x] If `cpu_has(CPU_FEATURE_AVX10)`: log `[SIMD] AVX10 v%u detected; not yet enabled`; reads version from CPUID leaf 0x24 EBX[7:0]
- [x] If `CPUID.(7,1):EDX[21]` (APX): log detected; future work to set `XCR0[19]` and compile with `-mapx`
- [x] Commit: `"kernel/simd: AVX-512 opt-in with throttle guard, AVX10/APX detection stubs"`

**Test checkpoint:** AVX-512 path disables when MPERF/APERF ratio drops >5% after micro-burst; AVX10/APX stubs log and continue without enabling. Test on: QEMU WHPX (has AVX-512 on Rocket Lake+), bare metal.

> **Test runner:** `scripts\debug\run-x86-tests.bat` (SUITE=x86)
> **Expected:** 23 suites, 0 failures

> **Verified:** 2026-04-13 -- all 7 items confirmed. `simd_enable_avx512()` at `gfx_simd.c:332` reads MPERF/APERF via `msr_try_read()` before and after `simd_avx512_burst()` (1000-iter vpaddq ZMM loop in `gfx_simd_avx512.c`), clears XCR0 bits 5-7 on >5% frequency drop. XSAVE area uses `xsave_size_max` from §1 (`task.c:212`). `memcpy_avx512`/`memset_avx512` at `memops_avx512.c` (64B vmovdqu64 loop, vpbroadcastd fill, vzeroupper). `fb_blit_avx512`/`fb_fill_avx512` at `gfx_simd_avx512.c` (16 pixels/iter). Dispatch at `memops_sse.c:101-114` and `framebuffer.c:165-190` (AVX-512 -> AVX2 -> SSE2 -> scalar). AVX10 version logged from CPUID leaf 0x24 (`cpuid.c:316`). APX logged (`cpuid.c:325`). 7 tests under TEST_CAT_X86. Codex adversarial: vpbroadcastb->vpbroadcastd (AVX512F-only), post-burst msr_try_read, delta range guard (fail-closed for both MPERF and APERF), comment fix. Accepted: AP XCR0 (XREF TODO-04 §4, same as §2 AVX2); framebuffer dimension overflow (pre-existing, not §3).
> **Quality reviewed:** 2026-04-13 -- kernel-code-quality 11 gates walked. G1: BSP-only write at boot, read-only after. G8: vzeroupper on all 5 AVX-512 functions. G9: separate `-mavx512f` TUs (no EVEX leakage to non-AVX512 CPUs, mirrors §2 VEX fix). G11: all instructions AVX512F-only per Intel SDM Vol. 1 Section 13.3 (no AVX512BW/DQ dependency). MPERF/APERF per SDM Vol. 3 Section 14.2. Throttle guard fail-closed on invalid telemetry. Parity: matches Windows conditional AVX-512 enablement; exceeds Linux (no runtime throttle detection in kernel). Accepted: none.

---

## 4. MSR Management Infrastructure

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

**Test checkpoint:** `msr_read(MSR_IA32_EFER)` stable across calls; `msr_try_read(0xFFFFFFFF, &scratch)` survives without panic (returns -1 on TCG/bare metal where #GP fires, returns 0 on WHPX where hypervisor absorbs the read); boot completes with MSR layer linked. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\run-x86-tests.bat` (SUITE=x86)
> **Expected:** 29 suites, 0 failures
>
> **Verified:** 2026-04-12 -- all 6 items confirmed. `msr_read`/`msr_write` inline in `msr.h:16-28`. `msr_try_read` in `msr.c:48-89` with spinlock+irqsave+RIP check (Codex: SMP race + unrelated #GP swallowing fixed). 22 MSR constants in `msr.h`. smp.c (4 sites) + lapic.c (2 sites) migrated. `cpu_verify_hardening` uses `msr_read` (d825576d). Hyper-V MSR constants corrected per TLFS Table 2-2 (0x40000022=TSC, 0x40000023=APIC). `msr_write` memory clobber added. Accepted: none.
> **Quality reviewed:** 2026-04-13 -- kernel-code-quality 11 gates walked. G1: spinlock+irqsave serializes msr_try_read on SMP; result captured under lock (Codex fix: s_gp_fired race). G7: non-probe #GP now calls panic_screen instead of returning unchanged frame (Codex fix: infinite-loop on unrelated #GP). G11: rdmsr 2-byte skip per Intel SDM Vol. 2B; MSR indices match SDM Vol. 4 + AMD APM Vol. 2 + Hyper-V TLFS Table 2-2. XREF comments fixed (§3->§4). 6 MSR unit tests (EFER stable, LMA set, try_read valid, try_read no-crash, NULL out, CPUID-gate pattern). **WHPX limitation:** `msr_try_read()` is a no-crash guarantee, not an existence check; WHPX silently absorbs unknown MSR reads (returns 0, no #GP). Rule added to `msr.h` API doc, kernel-code-quality Gate 6, and CLAUDE.md Bare Metal Gotchas: always gate on `cpu_has()`/CPUID first, use `msr_try_read()` only as secondary safety net. Parity: matches Windows HAL HalMsrRead + Linux rdmsr_safe. Accepted: IDT handler registration race (low practical risk: vector 13 only modified by serialized msr_try_read; proper extable mechanism is out of scope for §4).

---

## 5. UMIP + PKU Protection Keys

- [x] `cpu_enable_umip()`: `if (cpu_has(CPU_FEATURE_UMIP)) { write_cr4(read_cr4() | CR4_UMIP); }` where `CR4_UMIP = (1ULL << 11)`; called from `cpu_harden()` on BSP and each AP
- [x] Effect: user-space `SGDT`, `SIDT`, `SLDT`, `SMSW`, `STR` raise `#GP` instead of revealing GDT/IDT base addresses; eliminates a trivial kernel address leak. **Note:** ring-3 SGDT #GP verification requires user-mode test on WHPX/bare metal; kernel-mode tests verify CR4.UMIP bit is set
- [x] `cpu_enable_pku()`: sets CR4.PKE (bit 22) after verifying XCR0 bit 9 is active; called from `cpu_harden()` on BSP and each AP
- [x] Add PKRU to XCR0 (bit 9) in `cpu_configure_xcr0()` (from §1); **fixed bug**: was gated on `CPU_FEATURE_UMIP` instead of `CPU_FEATURE_PKU`. PKRU XSAVE offset queried from CPUID leaf 0x0D subleaf 9 and stored in `g_cpu.pkru_xsave_offset`
- [x] Kernel API in `src/kernel/security/pku.c` and `include/kernel/security/pku.h`: `pku_alloc_key()` (returns 1-15, -1 on exhaust, spinlock-protected bitmap), `pku_free_key()`, `pku_set_permissions()` (WRPKRU wrapper), `pku_read()` (RDPKRU), `pku_init()`. Constants: `PKU_ACCESS_DISABLE = 0x1`, `PKU_WRITE_DISABLE = 0x2`, `PKU_INITIAL_PKRU = 0x55555554`
- [x] PTE key field: `VMM_PKU_KEY(k)` encodes key into bits 62:59, `VMM_PKU_KEY_GET(pte)` extracts key. Added to `include/kernel/mm/vmm.h`
- [x] Win32 API surface: kernel-side `pku_set_permissions()` wraps WRPKRU (~1 ns). User-mode `SetThreadMemoryZone()` will execute WRPKRU directly from user libc (no syscall needed since WRPKRU is a ring-0/3 instruction). SSDT wiring for `pku_alloc_key`/`pku_free_key` deferred to SSDT domain (-> XREF: TODO-05)
- [x] PKRU initial value: `0x55555554` set in `task_alloc_xsave()` at the PKRU XSAVE offset (from CPUID 0x0D:9). Key 0 = full access, keys 1-15 = access disabled by default. XSTATE_BV bit 9 set to mark PKRU state valid
- [x] `cpu_verify_hardening()` updated: verifies CR4.UMIP (bit 11) and CR4.PKE (bit 22) with klog
- [x] Commit: `"kernel/security: UMIP (CR4.UMIP), PKU protection keys, SetThreadMemoryZone"`

**Test checkpoint:** After `cpu_enable_umip()`, CR4.UMIP is set (ring-3 SGDT #GP requires user-mode validation on WHPX/bare metal). CR4.PKE set, XCR0 bit 9 active, PKRU readable via RDPKRU, `pku_alloc_key`/`pku_free_key` round-trip, `pku_set_permissions` + readback, PTE key macros round-trip, PKRU XSAVE offset from CPUID. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\run-x86-tests.bat` (SUITE=x86)
> **Expected:** 37 suites, 0 failures

> **Verified:** 2026-04-13 -- all 10 items confirmed. `cpu_enable_umip()` at `cpu_security.c:130` sets CR4.UMIP (bit 11). `cpu_enable_pku()` at `cpu_security.c:144` sets CR4.PKE (bit 22) after verifying XCR0 bit 9; sets `pku_enabled` flag. XCR0 PKRU gate fixed (`cpu_configure_xcr0` at `cpuid.c:363`: CPU_FEATURE_UMIP->CPU_FEATURE_PKU). PKRU XSAVE offset from CPUID 0x0D:9 at `cpuid.c:186-187`. `pku.c` kernel API: `pku_alloc_key` (spinlock bitmap, keys 1-15), `pku_free_key` (WRPKRU revoke before bitmap clear), `pku_set_permissions` (RDPKRU/WRPKRU), `pku_read` (RDPKRU); all gated on `pku_enabled`. PTE key macros at `vmm.h:27-28`. PKRU initial 0x55555554 at `task.c:267` with overflow-safe bounds check. `cpu_verify_hardening` at `cpu_security.c:228-243` verifies CR4.UMIP+CR4.PKE. 8 tests under TEST_CAT_X86. Codex adversarial: WRPKRU #UD guard fixed (pku_enabled flag instead of cpu_has); XSAVE offset overflow-safe bounds check. Accepted: AP XCR0 configuration (XREF TODO-04 §4, pre-existing, same as §2/§3); per-thread PKRU restore on context switch (requires per-process page tables); cross-task key reuse isolation (requires thread-local PKRU context); user-mode SSDT wiring for pku_alloc/free (XREF TODO-05).
> **Quality reviewed:** 2026-04-13 -- kernel-code-quality 11 gates walked. G2: spinlock+irqsave on s_key_bitmap. G5: error handling on pku_alloc_key (-1 on exhaust), bounds checks on key range and XSAVE offset. G6: CR4_UMIP=bit 11, CR4_PKE=bit 22 per Intel SDM Vol. 3A Table 2-2; PKRU per SDM Vol. 3A Section 4.6.2; PTE key bits 62:59 per SDM Vol. 3A Section 4.5; RDPKRU/WRPKRU ECX=0 constraint per SDM Vol. 2B. G11: all constants match Intel SDM. Parity: matches Windows implicit PKU use + Linux pkey_alloc/mprotect_key; our pku_set_permissions wraps WRPKRU for zero-cost zone switching. Accepted: same as verified stamp.

---

## 6. 1 GiB Huge Pages + Write-Combining PAT

- [x] Check `cpu_has(CPU_FEATURE_PAGE1GB)` before use; `vmm_map_huge_1g()` and `vmm_promote_to_1g()` both gate on the CPUID feature
- [x] `vmm_map_huge_1g(va, pa, flags)`: single-entry helper in `vmm.c`; sets PDPTE PS bit (bit 7) with physical address in bits 51:30; validates 1 GiB alignment on both addresses; returns -1 on misalignment or missing feature
- [x] `vmm_promote_to_1g()`: promotes identity-mapped GiB 1-3 from PD trees to single 1 GiB PDPTE entries; skips PDPT[0] (first GiB, contains kernel text that needs NX at 2 MiB granularity); flushes TLB after promotion; called from boot_phase0 after vmm_apply_nx_policy()
- [x] Fallback: if `PAGE1GB` not set, `vmm_promote_to_1g()` logs "not supported" and keeps 2 MiB identity map unchanged
- [x] PAT MSR entry 1 = WC: **already done** in `boot_hw.c:376-393`; PAT reprogrammed from `0x0007040600070406` to `0x0007040600010406` (entry 1: WT -> WC) with readback verification
- [x] WC PTE flag: **already done** as `VMM_MMIO_WC` in `vmm.c:734-758` (PWT=1, PCD=0 selects PAT entry 1 = WC); `vmm_map_mmio_wc()` exposed in `vmm.h:96`
- [x] Framebuffer WC mapping: **already done** in `framebuffer.c:247-266`; `fb_init()` calls `vmm_map_mmio_wc()` for VRAM, with identity-map fallback on failure
- [x] Commit: `"kernel/mm: 1 GiB huge pages, Write-Combining PAT for framebuffer"`

**Test checkpoint:** `cpu_has(PAGE1GB)` gate respected; 1 GiB-aligned range uses single PDPTE PS path; `fb_swap()` TSC median drops >=5x after WC map on 1080p. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\run-x86-tests.bat` (SUITE=x86)
> **Expected:** 42 suites, 0 failures

> **Verified:** 2026-04-13 -- all 8 items confirmed. `vmm_map_huge_1g()` at `vmm.c:847` with 1 GiB alignment validation, PAGE1GB CPUID gate, subtree-overwrite guard, full TLB flush. `vmm_promote_to_1g()` at `vmm.c:883` validates all 512 PDEs for address contiguity AND flag uniformity before promotion; builds PDPTE from validated PDE flags (preserves P/W/U/PS/NX). `vmm_get_physical()` at `vmm.c:300-305` resolves 1 GiB pages at PDPT level. PAT MSR, WC flag, and framebuffer WC mapping already done (boot_hw.c, vmm.c VMM_MMIO_WC, framebuffer.c). 5 tests under TEST_CAT_X86. Codex adversarial: vmm_get_physical 1 GiB resolution added; PDE flag-uniformity validation added; subtree-overwrite guard on vmm_map_huge_1g; vmm_flush_tlb_all instead of single invlpg. Accepted: post-SMP TLB shootdown for vmm_map_huge_1g (pre-SMP only currently, documented); vmm_split 1 GiB -> 512x2 MiB (not needed yet; split on demand when required).
> **Quality reviewed:** 2026-04-13 -- kernel-code-quality 11 gates walked. G2: single-threaded boot path, no SMP concern. G6: 1 GiB PDPTE format per Intel SDM Vol. 3A Table 4-15 (PS=1, phys addr bits 51:30). vmm_get_physical address extraction `(pdpte & ~GIB_ALIGN_MASK & PTE_ADDR_MASK)` verified correct. PDE validation loop O(512) per GiB at boot (negligible). G11: PAT entry 1 = 0x01 (WC) per SDM Vol. 3A Table 11-10. Parity: matches Windows Large Pages (registry-enabled, 1 GiB on supported CPUs) + Linux hugetlbfs 1G mount. Accepted: none.

---

## 7. FRED Unified Event Delivery

> [!WARNING]
> **Blocked: no test platform.** FRED requires Intel Granite Rapids (2024 server) or later. No available platform (WHPX i5-11600K, TCG, VirtualBox, Haswell bare metal) supports FRED. QEMU does not emulate FRED. Implementing untested interrupt entry code risks total system failure on the first FRED-capable hardware. Implement when FRED hardware or a FRED-capable emulator becomes available.

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
- [ ] Commit: `"kernel/cpu: FRED unified event delivery"`

**Test checkpoint:** On FRED-capable QEMU (`-cpu Cooperlake,fred=on`), timer IRQ still reaches `isr_handler` via FRED entry; fallback CPU boots unchanged with IDT path. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 8. LKGS Fast GS-Base Swap

> [!WARNING]
> **Blocked: no test platform.** LKGS requires Intel Granite Rapids or later (same silicon as FRED). No available platform supports LKGS. Replacing SWAPGS with untested LKGS in the syscall/ISR entry path risks corrupting per-CPU GS-base on the first LKGS-capable hardware. Implement when LKGS hardware becomes available.

- [ ] Check `cpu_has(CPU_FEATURE_LKGS)` (CPUID leaf 7, ECX=1, EAX bit 18)
- [ ] `LKGS reg` writes directly to `IA32_KERNEL_GS_BASE` without touching the active GS; eliminates `SWAPGS` and its speculative side-channel
- [ ] In `src/kernel/sched/syscall_entry.asm`:
  - On SYSCALL entry: `LKGS [saved_user_gs]` instead of `SWAPGS`
  - On SYSRET: `LKGS [user_gs_base]` before `SYSRETQ`
- [ ] Same replacement in IDT common stubs that currently use `SWAPGS`
- [ ] Fallback: if LKGS unavailable, keep existing `SWAPGS` (no regression)
- [ ] Commit: `"kernel/cpu: LKGS replaces SWAPGS in syscall and ISR entry"`

**Test checkpoint:** Syscall path uses LKGS when `CPU_FEATURE_LKGS`; per-CPU data still accessible via GS after LKGS; fallback CPU boots with SWAPGS unchanged. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 9. CPU Topology: Zen Chiplets + Intel Hybrid

- [x] Created `src/kernel/topology.c` and `include/kernel/topology.h`: `cpu_topo_t` struct with `logical_id`, `core_id`, `ccd_id`, `node_id`, `core_type`, `smt_siblings`. Globals: `g_cpu_topo[MAX_CPUS]`, `g_topo_cpu_count`, `p_core_mask`, `e_core_mask`, `g_numa_nodes`. Constants: `CORE_TYPE_GENERIC=0`, `CORE_TYPE_P=0x40`, `CORE_TYPE_E=0x20`. Helper: `topology_is_hybrid()`
- [x] `topology_init()` called from Phase 2 (`boot_storage.c`) after `smp_init()`. Populates per-CPU topology from BSP CPUID data. Per-AP CPUID queries deferred to TODO-04 §4 (`ap_cpu_harden`)
- [x] Zen path (`cpu_has(CPU_FEATURE_TOPO_EXT)`): BSP values from `g_cpu.compute_unit_id`/`g_cpu.node_id`; APs derive from LAPIC ID. Logs CCD count, NUMA nodes, CPU count
- [x] `g_numa_nodes` = max `node_id` + 1; NUMA allocator deferred to `03-memory-concurrency`
- [x] Intel hybrid path (`CPUID.07H:EDX[15]`): leaf 0x1A for core type (EAX[31:24]), leaf 0x1F for SMT/core topology levels (shift-based core_id extraction). Populates `p_core_mask`/`e_core_mask`. BSP core type applied to all CPUs until per-AP CPUID available
- [x] Fallback: no topology extensions, all cores `CORE_TYPE_GENERIC`, 1 NUMA node
- [x] Commit: `"kernel/topology: Zen CCD/NUMA parser, Intel hybrid P/E-core detection"`

**Test checkpoint:** Serial shows `[topo] Zen:`, `[topo] Intel:`, or `[topo] Generic:` line; `g_cpu_topo[0].logical_id == 0`; `g_topo_cpu_count` matches `smp_cpu_count()`; `g_numa_nodes >= 1`; core_type is valid constant. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\run-x86-tests.bat` (SUITE=x86)
> **Expected:** 47 suites, 0 failures

> **Verified:** 2026-04-13 -- all 7 items confirmed. `topology.c` and `topology.h` at `src/kernel/topology.c`, `include/kernel/topology.h`. `topology_init()` called from `boot_storage.c:175` after `smp_init()`. Zen path: BSP `compute_unit_id` as core_id, BSP `node_id` for NUMA; APs derive core_id from LAPIC ID, node_id/ccd_id=0 (per-AP CPUID deferred). Intel hybrid: BSP core type logged from leaf 0x1A; P/E masks left zero (per-AP CPUID required for accuracy). Leaf 0x1F: SMT/Core shift extraction with correct bit-range core_id formula. Fallback: all GENERIC. 5 tests under TEST_CAT_X86. Codex adversarial: offline CPU check added (is_online guard); SMT lp_count clamped to 255; false P/E mask publication prevented; CCD aliasing fixed. Accepted: per-AP CPUID (XREF TODO-04 §4); ACPI SRAT NUMA (XREF D03); x2APIC 32-bit ID (pre-existing 8-bit truncation in lapic.c).
> **Quality reviewed:** 2026-04-13 -- kernel-code-quality 11 gates walked. G2: written once at boot, read-only after. G5: fallback path for no topology extensions. G6: CPUID leaves per Intel SDM Vol. 2A (0x1A native model ID, 0x1F v2 extended topology) and AMD APM (0x8000001E). G11: core_id extraction uses bit range between SMT and Core shifts per SDM Table 3-8. Parity: matches Linux `arch/x86/kernel/cpu/topology.c` + `smpboot.c` topology detection; matches Windows Ke node + NUMA policy approach. Accepted: same as verified stamp.

---

## 10. Performance Monitoring Counters: Intel + AMD

- [x] Check CPUID leaf `0x0A`: PMU version (EAX[7:0]), counter count (EAX[15:8]), counter width (EAX[23:16]). Stored in `g_pmc_info` struct. Intel path probes `IA32_PERF_GLOBAL_CTRL` via `msr_try_read` as safety net (CPUID-first rule)
- [x] Intel `pmc_intel_start(slot, event)`: writes `IA32_PERFEVTSELx` (0x186+slot) with event + OS + USR + EN bits, clears `IA32_PMCx` (0xC1+slot), enables in `IA32_PERF_GLOBAL_CTRL` (0x38F)
- [x] Intel `pmc_intel_read(slot)`: `msr_read(0xC1 + slot)`
- [x] Intel `pmc_intel_stop(slot)`: clears `IA32_PERFEVTSELx` and `IA32_PERF_GLOBAL_CTRL` bit
- [x] Intel pre-defined events: `PMC_INTEL_INST_RETIRED=0xC0`, `PMC_INTEL_UNHALTED_CYCLES=0x3C`, `PMC_INTEL_LLC_MISSES=0x2E`, `PMC_INTEL_BR_MISPREDICT=0xC5`
- [x] AMD `pmc_amd_start/read/stop`: `MSR_AMD_PERF_CTL0` (0xC0010200) + `MSR_AMD_PERF_CTR0` (0xC0010201) with stride 2 per slot. 6 counters assumed for Zen 2+. AMD availability probed via `msr_try_read` on first counter MSR
- [x] AMD pre-defined events: `PMC_AMD_CPU_CLOCKS=0x76`, `PMC_AMD_INST_RETIRED=0xC0`, `PMC_AMD_RET_BRANCHES=0xC2`, `PMC_AMD_DRAM_ACCESSES=0x64`
- [x] Vendor-neutral dispatch: `pmc_start(slot, event)` / `pmc_read(slot)` / `pmc_stop(slot)` dispatch to Intel or AMD based on `g_pmc_info.is_amd`. All gated on `g_pmc_info.available`
- [x] `pmc_ipc()`: starts inst+cycle counters on slots 0+1, spins 1M iterations, reads, stops, returns 8.8 fixed-point IPC. Overflow-safe (clamp at 0xFFFF)
- [x] `pmc_init()` called from Phase 2 after topology_init. Logs PMU version, counter count, width. Degrades gracefully if MSR probe fails (WHPX)
- [x] Commit: `"kernel/pmc: Intel PMU + AMD PMC, pmc_start/read/stop, pmc_ipc() for Task Manager"`

**Test checkpoint:** `pmc_info` populated after init; `pmc_read` increments on tight loop (when available); `pmc_ipc()` returns non-zero fixed-point (when 2+ counters available); out-of-range slot rejected. Tests skip gracefully on WHPX if PMU MSRs are trapped. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\run-x86-tests.bat` (SUITE=x86)
> **Expected:** 51 suites, 0 failures

> **Verified:** 2026-04-13 -- all 11 items confirmed. `pmc.c` and `pmc.h` at `src/kernel/pmc.c`, `include/kernel/pmc.h`. Intel path: CPUID 0x0A for version/count/width + `msr_try_read(PERF_GLOBAL_CTRL)` + `msr_write(PERFEVTSEL0)` write probe. AMD path: `msr_try_read(PERF_CTR0)` + `msr_write(PERF_CTL0)` write probe. `pmc_init()` at `boot_storage.c:181`. `pmc_start/read/stop` dispatch on `is_amd` flag. `pmc_ipc` with 8.8 fixed-point, overflow-safe clamp. 4 Intel + 4 AMD event constants. 4 tests under TEST_CAT_X86. Codex adversarial: AMD write probe added (was read-only, same class as Intel PERFEVTSEL fix). Accepted: PERF_GLOBAL_CTRL RMW race (single-caller at boot; SMP locking needed when exposed to multi-threaded callers); event API limited to bits 0-15 (full PERFEVTSEL passthrough deferred).
> **Quality reviewed:** 2026-04-13 -- kernel-code-quality 11 gates walked. G2: per-CPU MSRs, single BSP caller currently. G6: CPUID-first with msr_try_read safety net on both vendors; write probe verifies MSR accessibility before marking available. G11: Intel PMU per SDM Vol. 3B Ch. 19; AMD PMC per APM Vol. 2 Section 13.2. Parity: matches Windows ETW/WPA PMU access + Linux perf_event PMU driver. Accepted: same as verified stamp.

---

## 11. OSVW Errata + RDTSCP Processor ID Setup

- [x] OSVW: `cpu_has(CPU_FEATURE_OSVW)` gates MSR reads. `g_cpu.osvw_length` from `MSR_AMD_OSVW_ID_LEN`, `g_cpu.osvw_status` from `MSR_AMD_OSVW_STATUS`. Fields added to `struct cpu_features` in `cpuid.h`. Query runs in `cpuid_init()`. `cpu_has_erratum(n)` inline helper checks bit N of `osvw_status`. Logs errata count and mask
- [x] Errata workaround table: no-op initially (no specific AMD PPR errata identified for current targets). Framework in place via `cpu_has_erratum()` for future per-erratum gating
- [x] RDTSCP: `rdtscp_read(cpu_id)` inline wrapper in `cpuid.h` returns TSC and writes CPU ID to `*cpu_id` via `IA32_TSC_AUX`. Safe with NULL pointer (skips write). BSP writes `IA32_TSC_AUX = 0` in `boot_hw.c` Phase 0 after CPUID. APs write `IA32_TSC_AUX = cpu_index` in `ap_entry()` after `cpu_harden()`
- [x] BSP TSC_AUX: written in Phase 0 with value 0 (CPU logical ID 0)
- [x] Commit: `"kernel/cpu: OSVW errata table, RDTSCP per-CPU IA32_TSC_AUX setup"`

**Test checkpoint:** OSVW: serial logs errata count when present (AMD only, skipped on Intel). RDTSCP: TSC_AUX = 0 on BSP verified by test; TSC non-zero; NULL pointer safe. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\run-x86-tests.bat` (SUITE=x86)
> **Expected:** 54 suites, 0 failures

> **Verified:** 2026-04-13 -- all 5 items confirmed. OSVW: `cpuid.c:324-330` reads `MSR_AMD_OSVW_ID_LEN`/`MSR_AMD_OSVW_STATUS` via `msr_try_read`, gated on `CPU_FEATURE_OSVW` AND AMD vendor string. `cpu_has_erratum(n)` at `cpuid.h:165` with n>=64 UB guard. BSP TSC_AUX: `boot_hw.c:366-375` probes via `msr_try_read`, writes 0 only on success, sets `g_tsc_aux_available`. AP TSC_AUX: `smp.c:148-150` conditional on `g_tsc_aux_available`. `rdtscp_read()` at `cpuid.h:174` with NULL-safe cpu_id. 3 tests under TEST_CAT_X86. Codex adversarial: OSVW vendor gate + msr_try_read added (was raw msr_read); shift UB guard added in implementation. Accepted: TSC_AUX write after read probe (TSC_AUX is documented R/W on all implementations; the read probe catches hypervisors that trap the MSR entirely).
> **Quality reviewed:** 2026-04-13 -- kernel-code-quality 11 gates walked. G2: per-CPU MSR writes at boot, read-only after. G6: OSVW per AMD APM Vol. 2 Section 6.1; TSC_AUX per Intel SDM Vol. 3B Section 17.17.2; vendor gate prevents OSVW MSR reads on non-AMD CPUs. G11: RDTSCP encoding correct (EAX=lo, EDX=hi, ECX=aux per SDM Vol. 2B). Parity: matches Linux `amd.c` osvw handling + per-CPU `IA32_TSC_AUX` wrmsr in `smpboot.c`; matches Windows HAL OSVW errata table. Accepted: same as verified stamp.

---

## 12. AMD IBS Profiling (Stretch)

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

## 13. Virtualization Detection (AMD-V + Intel VT-x)

- [x] AMD SVM / Intel VMX capability reporting: AMD SVM: CPUID 0x8000000A parsed; revision, NPT, ASIDs logged
- [x] Intel VT-x: `CPU_FEATURE_VMX` added (CPUID.1:ECX[5]); `IA32_FEATURE_CONTROL` MSR read for lock + enable status
- [x] Exposed via `cpu_has(CPU_FEATURE_SVM)` / `cpu_has(CPU_FEATURE_VMX)` (detection only)
- [x] Commit: `"kernel/cpu: AMD SVM + Intel VT-x capability detection and logging"`

**Test checkpoint:** Boot log shows AMD SVM or Intel VMX lines matching `cpuid -1` expectations on real silicon; `cpu_has` agrees with known host type. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 14. Boot Self-Benchmark + Auto-Tune

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

## 15. Future Silicon Extensions

- [ ] Stub: if `CPUID.(7,0):EDX[5]` set: log `[cpu] UINTR present; not yet enabled`; structure definitions in `include/kernel/uintr.h` (UPID, UITT); full implementation deferred until UINTR reaches mainstream silicon
- [ ] Stub: if `CPUID.(7,0):ECX[16]` set: log `[vmm] LA57 (57-bit VA) detected; 4-level paging active`; enabling at runtime requires a full VMM rewrite; relevant for server SKUs with > 256 TiB virtual address space
- [ ] Commit: `"kernel/cpu: UINTR/LA57 detection stubs, future silicon log entries"`

**Test checkpoint:** When CPU advertises UINTR or LA57, serial shows one-line stub log and kernel continues without enabling new modes. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 16. Boot Page Tables: 1 GiB Pages From entry.asm

> **Why:** `vmm_promote_to_1g()` (§6) upgrades 2 MiB identity-map entries to 1 GiB pages at runtime, but later code (KUSD at 0x7FFE0000, guard pages, MMIO mappings) splits them back to 2 MiB via `vmm_split_huge_page()`. This promote-then-split cycle wastes boot time and allocates throwaway PD frames. Windows and Linux build 1 GiB pages from the start in their initial page tables.

- [ ] Modify `src/boot/entry.asm`: for PDPT[1], PDPT[2], PDPT[3], set PS=1 (bit 7) directly in the PDPT entry instead of pointing to a PD table. Physical address = PDPT index * 1 GiB. Flags: Present + Writable + User + PS (0x87 with PS at PDPT level). Keep PDPT[0] pointing to a PD with 512 x 2 MiB entries (kernel text needs NX at fine granularity)
- [ ] Gate on `CPU_FEATURE_PAGE1GB`: entry.asm runs before CPUID detection, so either (a) probe CPUID leaf 0x80000001 EDX bit 26 inline in assembly before building page tables, or (b) always build 1 GiB entries and let the kernel downgrade at runtime if PAGE1GB is absent (1 GiB PDPTEs with PS=1 on CPUs without PAGE1GB cause #PF; the page fault handler would need to handle this gracefully)
- [ ] Remove `vmm_promote_to_1g()` from `boot_hw.c` and `vmm.c` (no longer needed; the page tables are already optimal from boot)
- [ ] Keep `vmm_split_huge_page()` 1 GiB cascade (added in §6): still needed for on-demand splits when mapping individual pages inside GiB 1-3 (KUSD, guard pages, MMIO)
- [ ] Verify `vmm_apply_nx_policy()` still skips 1 GiB pages at PDPT level (already does, line 893-895)
- [ ] Remove the 4 statically allocated PD tables for GiB 1-3 from entry.asm BSS (saves 12 KiB of boot memory; PD for GiB 0 stays)
- [ ] Commit: `"boot: build identity map with 1 GiB pages in entry.asm (PDPT[1-3])"`

**Test checkpoint:** Serial log shows NO `promoted N PDPT entries` line (promotion removed). `vmm_get_physical(0x80000000)` returns 0x80000000 (identity map via 1 GiB page). KUSD at 0x7FFE0000 triggers 1 GiB split on demand and maps correctly. Guard pages in GiB 1-3 split correctly. Test on: QEMU WHPX, QEMU TCG (may lack PAGE1GB), VirtualBox, bare metal.

-> XREF: `01-boot-platform/TODO-04-cpu-boot-sequencing.md §8` (PAT AP sync uses same page table infrastructure)

---

## OS Comparison

| ⭐  | Feature              | 🪟 Win11                    | 🐧 Linux                    | 🚀 Impossible OS  |
| --- | -------------------- | ---------------------------- | --------------------------- | ------------------ |
| 💎  | CPUID feature gates  | ✅ KeQueryFeature API       | ✅ cpu_has() x86/cpu        | ✅ cpuid_init     |
| 💎  | AMD ext CPUID        | ✅ HAL ext leaves 8000xxxx  | ✅ amd.c + topo ext         | ✅ §1             |
| 💎  | XSAVE per thread     | ✅ KTHREAD XSAVE area       | ✅ fpu__alloc lazy FPU      | ✅ §1 Done        |
| 💎  | AVX memops in kernel | ✅ RtlCopyMemory via NT HAL | ✅ kernel_fpu_begin + kfpu  | ⚠️ §2 GFX only    |
| 💎  | AVX-512 + throttle   | ✅ context-switch aware     | ✅ eager FPU, power cgroup  | ✅ §3 Done        |
| 💎  | Central safe MSR     | ✅ HalMsrRead + #GP safe    | ✅ rdmsr_safe() extable     | ✅ §4 Done        |
| 💎  | UMIP                 | ✅ CR4.UMIP on boot         | ✅ 4.15+ on boot            | ✅ §5 Done        |
| 💎  | PKU / pkeys          | ✅ implicit OS use          | ✅ pkey_alloc/mprotect_key  | ✅ §5 kernel API  |
| 💎  | PKS (supervisor)     | ⚠️ niche, not exposed       | ✅ mm/pkeys since 5.13      | ⬜ defer T17      |
| 💎  | FB WC PAT            | ✅ WDDM GPU WC mapping      | ✅ ioremap_wc PAT entry     | ✅ §6 WC mapped  |
| 💎  | 1 GiB huge pages     | ✅ Large Pages in registry  | ✅ hugetlbfs 1G mount       | ✅ §6 promoted   |
| 💎  | 1 GiB boot PT        | ✅ HAL builds from start    | ✅ direct-map 1G native     | ⬜ §16 runtime   |
| 💎  | FRED events          | 🔜 future Windows roadmap   | ✅ 6.9+ Granite Rapids      | ⬜ §7             |
| 💎  | LKGS fast GS         | 🔜 future Windows roadmap   | ✅ 6.4+ no SWAPGS in entry  | ⬜ §8             |
| 💎  | Zen CCD NUMA         | ✅ Ke node + NUMA policy    | ✅ amd_nb.c CCD/CCX topo    | ✅ §9 Done        |
| 💎  | Intel P/E hybrid     | ✅ Thread Director HFI      | ✅ HFI + itmt scheduler     | ✅ §9 BSP-only    |
| 💎  | PMU / PMC            | ✅ ETW + WPA counters       | ✅ perf_event PMU driver    | ✅ §10 Done       |
| 💎  | OSVW errata          | ✅ HAL workaround table     | ✅ amd.c osvw_id_length     | ✅ §11 Done       |
| 💎  | RDTSCP TSC_AUX       | ✅ QPC reads TSC_AUX        | ✅ per-CPU wrmsr in SMP     | ✅ §11 Done       |
| 💎  | AMD IBS sample       | ⚠️ uProf vendor tool only   | ✅ perf IBS + oprofile      | ⬜ §12 stretch    |
| 💎  | SVM VT-x detect      | ✅ HAL + Hv caps            | ✅ kvm cpuid + vmx_init     | ✅ §13            |
| ⭐  | Boot hw self tune    | ❌ static config only       | ❌ static defaults          | ⬜ §14            |
| ⭐  | PKU Win32 wrapper    | ❌ no user API surface      | ⚠️ raw syscall pkey_*       | ⚠️ §5 kernel only |
| ⭐  | Per-core freq UI     | ❌ basic Task Manager       | ⚠️ turbostat CLI tool       | ⬜ §10 + shell    |

After §1 through §13 done and §7 through §12 plus §14 through §16 planned, parity with Win11/Linux 6.x on this stack; §14 boot benchmark is the differentiator. `SetThreadMemoryZone` (§5) is a planned Win32-style PKU surface. PKS (supervisor keys) stays in `TODO-17` until kernel mappings allow it without breaking SMEP/SMAP rollout.

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
| **2026-04-10** | validate | Removed ### N.M, stripped model tags, ASCII punctuation sweep, fixed IMPORTANT + Inputs, moved/compact OS Comparison, `TODO-06`/`TODO-07` Depends On, per-section **Test checkpoint** + four platforms, Unit Tests -> `TEST_CAT_BOOT` + `msr_read` + `g_cpu.xsave_size`, Verification + `run-boot-tests.bat`. **Parity:** Win11+Linux-strong rows map to §2 through §12 except WinDbg-adjacent items (N/A here). **Flag:** §6 FRED/LKGS needs recent silicon or explicit QEMU CPU flags. |
| **2026-04-12** | gap-analysis | Reciprocal sync from `TODO-17` gap analysis: scope bullet now records live NX + `vmm_apply_nx_policy()`, partial SMEP/SMAP (`hv_supports_cr4_smep_smap()` forces skip), and missing IDT `clac` until SMAP is real. |
| **2026-04-12** | validate | §9 through §13: Commit before **Test checkpoint**; replaced spaced hyphen pairs used as sentence glue; §12 SIMD bullet joined; Unit Tests + Verification **Test checkpoint** blocks; History wording fix for §2 through §12. **Parity gaps (Win11+Linux strong, Impossible not done):** §2 AVX-512 throttle, §4 UMIP+PKU, §5 WC+1G, §6 FRED+LKGS, §7 topology, §8 PMC, §9 OSVW+TSC_AUX, §10 IBS, §12 boot tune. **Blocked / external:** T06 §3 (FRED vs IRQL), T07 §2 (RDTSCP consumer), T13 §4 (HwProfile registry), D03 memory + D08 shell deferrals per Inputs. |
| **2026-04-12** | gap-analysis | Web: Win UMIP/Linux PKU docs, Linux FRED merge 6.9 (LWN), kernel.org protection-keys, perf-amd-ibs. Code-truth: §3 MSR bullet narrowed; new follow-up for `cpu_security.c` raw `rdmsr`; §1 documents FXSAVE fallback in `task.c`; PKS row + scope defer to T17; reciprocal note in T17 IMPORTANT. **Parity gaps unchanged** except explicit PKS tracking. **Flags:** §1 item count >10 (mostly done); §5 PAT+FB touches MTRR/MMIO gotchas from `CLAUDE.md`. |
| **2026-04-12** | validate | IMPORTANT callout joined (no blank inside); Inputs T07 XREF §2 to §3 + Impl row 9 `T07 §3`; Outcome XSAVE vs FXSAVE truth; scope `D03`/`D08` shorthands; §11 orphan prose merged into first `[x]`; table column scan OK; **blocked:** §6 on `T06 §3` open, §9 on `T07 §3` consumer wiring; parity gaps unchanged from prior validate row. |
