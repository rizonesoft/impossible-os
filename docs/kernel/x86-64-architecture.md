<!-- docs: covers=todo/02-kernel-core/TODO-09-x86-64-architecture.md sources=src/kernel/cpuid.c,include/kernel/cpuid.h,src/kernel/msr.c,include/kernel/msr.h,src/kernel/sched/task.c,src/kernel/topology.c,include/kernel/topology.h,src/kernel/security/pku.c,include/kernel/security/pku.h,src/kernel/mm/vmm.c,src/kernel/mm/memops.c,src/kernel/mm/memops_sse.c,src/kernel/mm/memops_avx512.c,src/kernel/gfx/gfx_simd.c,src/kernel/pmc.c,include/kernel/pmc.h,src/kernel/hw_profile.c,include/kernel/hw_profile.h,src/kernel/test/test_cpu_security.c reviewed=2026-09-28 order=9 -->
# x86-64 Architecture Features

## What is it?

This is the layer that turns CPU feature detection into features the kernel uses: per-task XSAVE with lazy FPU switching, AVX2 and AVX-512 memory copies, one MSR access API, UMIP and protection keys, 1 GiB pages, a write-combining framebuffer, CPU topology, performance counters, and a boot-time benchmark that records the machine's capabilities in the Registry.

Ten of the roadmap's twenty sections have shipped. Most of the rest wait on things this project cannot test yet: FRED and LKGS need Intel Granite Rapids hardware, AMD IBS needs an AMD Zen machine, split-lock detection was rolled back after a design flaw, and AMX needs per-thread XSAVE.

## How does it work?

`cpuid_init()` runs in Phase 0 and fills the feature set declared in [`cpuid.h`](../../include/kernel/cpuid.h). Everything downstream checks `cpu_has()` first and treats an MSR probe as a safety net, never as the existence check, per the [bare metal gotchas](../infrastructure/bare-metal-gotchas.md).

`cpu_configure_xcr0()` in [`cpuid.c`](../../src/kernel/cpuid.c) sets `CR4.OSXSAVE` and enables only the state components the CPU supports: x87, SSE and AVX always, AVX-512 when it passes a throttle check, and PKRU when protection keys exist. Each task gets an XSAVE area on first use through `task_alloc_xsave()` in [`task.c`](../../src/kernel/sched/task.c), sized from the CPU's reported maximum. The scheduler saves and restores FPU state only for tasks that have used it, and falls back to `fxsave`/`fxrstor` on CPUs without XSAVE.

MSR access goes through `msr_read()`, `msr_write()` and `msr_try_read()` ([`msr.h`](../../include/kernel/msr.h)); the last installs a temporary `#GP` handler, so probing an unsupported MSR returns an error instead of crashing. `msr.h` names the Intel, AMD and Hyper-V MSRs, so no other file writes its own `rdmsr` or `wrmsr`.

`memcpy_fast()` and `memset_fast()` ([`memops_sse.c`](../../src/kernel/mm/memops_sse.c)) choose AVX-512, then AVX2, then SSE2, then plain code, based on flags in [`gfx_simd.c`](../../src/kernel/gfx/gfx_simd.c). The flags start from CPUID and can only be switched off later, by the AVX-512 throttle check or the boot benchmark.

`topology_init()` ([`topology.c`](../../src/kernel/topology.c)) runs after SMP startup. On AMD it records compute unit and node IDs; on Intel it reads the hybrid core type from CPUID leaf `0x1A`. Per-CPU performance and efficiency core classification was implemented and then reverted, so every CPU is currently recorded as `CORE_TYPE_GENERIC`.

The boot benchmark, `hw_profile_init()` in [`hw_profile.c`](../../src/kernel/hw_profile.c), runs once in Phase 3. It measures memory bandwidth, latency, cache size, SIMD throughput and context-switch cost; a failed measurement only clears its own validity bit. Results are saved under `HKLM\SYSTEM\HwProfile` and reused on the next boot when the CPU brand string matches. `hw_profile_simd_decision()` switches off a SIMD tier that is less than 10 percent faster than the tier below it.

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| `task_alloc_xsave()` | Per-task XSAVE area ([`task.c`](../../src/kernel/sched/task.c)) |
| `msr_read()`, `msr_write()`, `msr_try_read()` | MSR access; `msr_try_read()` is a crash-safe probe ([`msr.c`](../../src/kernel/msr.c)) |
| `pku_alloc_key()`, `pku_free_key()`, `pku_set_permissions()` | Kernel protection keys ([`pku.h`](../../include/kernel/security/pku.h)) |
| `vmm_map_huge_1g()`, `vmm_promote_to_1g()` | 1 GiB mappings and runtime promotion ([`vmm.c`](../../src/kernel/mm/vmm.c)) |
| `vmm_map_mmio_wc()` | Write-combining mapping, used for the framebuffer |
| `memcpy_fast()`, `memset_fast()` | SIMD-tiered memory operations ([`memops.c`](../../src/kernel/mm/memops.c), [`memops_avx512.c`](../../src/kernel/mm/memops_avx512.c)) |
| `topology_init()` | CPU topology ([`topology.h`](../../include/kernel/topology.h)) |
| `pmc_init()`, `pmc_start()`, `pmc_read()`, `pmc_stop()`, `pmc_ipc()` | Performance counters on Intel and AMD ([`pmc.h`](../../include/kernel/pmc.h)) |
| `hw_profile_init()`, `hw_profile_is_stale()`, `hw_profile_simd_decision()` | Boot benchmark and SIMD tuning ([`hw_profile.h`](../../include/kernel/hw_profile.h)) |

## How do I use it?

Every shipped feature detects itself and turns on automatically; there is no setting. The x86 test category covers the MSR, CPUID, feature detection and benchmark helper suites, registered in [`test_cpu_security.c`](../../src/kernel/test/test_cpu_security.c):

```bash
bash scripts/test.sh SUITE=x86
```

The benchmark's results are visible in the Registry under `HKLM\SYSTEM\HwProfile`, and a second boot on the same CPU reuses them instead of measuring again.

## What is not implemented yet?

- **FRED** event delivery and **LKGS** need Granite Rapids or later, which no test platform here has ([FRED Unified Event Delivery](../../todo/02-kernel-core/TODO-09-x86-64-architecture.md#7-fred-unified-event-delivery), [LKGS Fast GS-Base Swap](../../todo/02-kernel-core/TODO-09-x86-64-architecture.md#8-lkgs-fast-gs-base-swap)).
- **AMD IBS** sampling needs an AMD Zen machine to validate ([AMD IBS Profiling](../../todo/02-kernel-core/TODO-09-x86-64-architecture.md#12-amd-ibs-profiling-stretch)).
- **1 GiB boot page tables** built by the bootloader, rather than promoted at runtime, wait on bare-metal firmware testing ([Boot Page Tables: 1 GiB Pages](../../todo/02-kernel-core/TODO-09-x86-64-architecture.md#16-boot-page-tables-1-gib-pages-from-entryasm)).
- **AMX** needs XSAVE state per thread rather than per task ([AMX Tile State + XFD Dynamic XSAVE](../../todo/02-kernel-core/TODO-09-x86-64-architecture.md#17-amx-tile-state--xfd-dynamic-xsave)).
- **Split-lock and bus-lock detection** was rolled back: the handlers were installed before the IDT was initialized, and the loop breaker disabled detection permanently after one hit ([Split-Lock + Bus-Lock Detection](../../todo/02-kernel-core/TODO-09-x86-64-architecture.md#18-split-lock--bus-lock-detection)).
- **SERIALIZE and RDPID** are detected but not used; the WAITPKG time limit did ship ([Additional CPU Feature Adoption](../../todo/02-kernel-core/TODO-09-x86-64-architecture.md#19-additional-cpu-feature-adoption-waitpkg-serialize-rdpid)).
- **Performance and efficiency core masks** stay empty because the per-CPU classification did not fit the kernel image budget ([Post-Ship Follow-Up Backfill](../../todo/02-kernel-core/TODO-09-x86-64-architecture.md#20-post-ship-follow-up-backfill-orphan-cohort-2026-07-31)).
- **Benchmark-driven tuning** beyond SIMD tiers waits on runtime knobs that do not exist yet ([Boot Self-Benchmark + Auto-Tune](../../todo/02-kernel-core/TODO-09-x86-64-architecture.md#14-boot-self-benchmark--auto-tune)).
- **Future silicon features** (LA57, LAM, LASS, UINTR) and confidential-VM detection are logged only ([Future Silicon Extensions](../../todo/02-kernel-core/TODO-09-x86-64-architecture.md#15-future-silicon-extensions)).

## How does it compare with Windows 11 and Linux?

The shipped sections match Windows 11 and current Linux: lazy XSAVE, AVX2 and AVX-512 memory paths, a safe MSR layer, UMIP, protection keys, write-combining framebuffer mapping, 1 GiB pages (promoted at runtime), topology detection, Intel and AMD performance counters, AMD errata handling, `RDTSCP`, and virtualization capability detection.

FRED and LKGS are behind Linux, which merged FRED in 6.9, and not yet shipped in Windows. AMD IBS is behind Linux `perf`. AMX and split-lock detection are behind both. The boot benchmark with Registry persistence and SIMD tuning has no equivalent in either.

## See also

- [x86-64 Architecture Enhancements roadmap](../../todo/02-kernel-core/TODO-09-x86-64-architecture.md)
- [Kernel Security Hardening](kernel-security-hardening.md)
- [Bare Metal Gotchas](../infrastructure/bare-metal-gotchas.md)
