<!-- docs: covers=todo/03-memory-concurrency/TODO-05-advanced-virtual-memory.md sources=include/kernel/mm/vmm.h,src/kernel/mm/vmm.c,include/kernel/ob/ob_section.h,src/kernel/ob/ob_section.c,src/kernel/nt/nt_section.c,include/kernel/ob/ob_job.h,src/kernel/nt/nt_memory.c,src/kernel/test/test_vmm.c reviewed=2026-09-28 order=5 -->
# Advanced Virtual Memory

## What is it?

The virtual memory features beyond basic mapping: copy-on-write `fork`, large pages, memory hints, section objects shared between views, per-process memory limits, DMA buffers, compressed memory, NUMA placement and Address Windowing Extensions. A few pieces exist in forms the roadmap does not yet claim (1 GiB kernel pages, single-process section objects, Job Objects without memory limits), and the rest is not built.

## How does it work?

**Large pages.** The kernel direct map uses 2 MiB pages, which `vmm_split_huge_page()` breaks into 4 KiB pages when a caller needs finer permissions. On CPUs that report 1 GiB page support (`CPU_FEATURE_PAGE1GB`), `vmm_promote_to_1g()` runs during Phase 1 ([`boot_hw.c`](../../src/kernel/main/boot_hw.c)) and folds eligible identity-map ranges into 1 GiB pages, and `vmm_map_huge_1g()` maps an aligned 1 GiB range directly ([`vmm.h`](../../include/kernel/mm/vmm.h)). There is no user-facing large-page allocation.

**`fork`.** `SYS_FORK` (5) creates a child process with `task_fork()`, but pages are not shared copy-on-write: the fork-then-exec path gives the child private frames through `vmm_remap_user_page()`, and the frame allocator has no reference counts.

**Section objects.** [`ob_section.h`](../../include/kernel/ob/ob_section.h) defines `SECTION_OBJECT` with up to `SECTION_MAX_VIEWS` (16) views and the `SEC_IMAGE`, `SEC_RESERVE`, `SEC_COMMIT` and `SEC_NOCACHE` flags. [`nt_section.c`](../../src/kernel/nt/nt_section.c) registers create, open, map, unmap, extend and query services. Backing memory comes from `pmm_alloc_contiguous()` up front, so a `SEC_RESERVE` section still commits frames, and mapping or unmapping a view in another process returns `STATUS_ACCESS_DENIED`.

**Job Objects.** [`ob_job.h`](../../include/kernel/ob/ob_job.h) supports only the active-process limit and kill-on-close (`JOB_SUPPORTED_LIMIT_FLAGS`); any other limit flag, including process and job memory limits, returns `STATUS_NOT_SUPPORTED` because there is no per-process commit accounting to enforce them against.

**AWE.** The three physical-page services are registered to `NtAWE_stub`, which returns `STATUS_NOT_IMPLEMENTED` ([`nt_memory.c`](../../src/kernel/nt/nt_memory.c)).

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| `vmm_split_huge_page()`, `vmm_map_huge_1g()`, `vmm_promote_to_1g()` | Kernel large-page management ([`vmm.h`](../../include/kernel/mm/vmm.h)) |
| `SYS_FORK` (5) | Process fork without copy-on-write |
| `ObCreateSectionEx()`, `ObMapViewOfSection()`, `ObUnmapViewOfSection()`, `ObExtendSectionObject()` | Section objects and views ([`ob_section.h`](../../include/kernel/ob/ob_section.h)) |
| `NtCreateSection` and related services | NT section surface ([`nt_section.c`](../../src/kernel/nt/nt_section.c)) |
| `NtCreateJobObject` and related services | Job Objects without memory limits ([`ob_job.h`](../../include/kernel/ob/ob_job.h)) |

## How do I use it?

Kernel code that needs finer permissions inside the direct map splits the covering page first with `vmm_split_huge_page()`. Large-page splitting, 1 GiB mapping alignment and section round trips are covered by the memory, CPU and Object Manager suites, including [`test_vmm.c`](../../src/kernel/test/test_vmm.c):

```bash
bash scripts/test.sh SUITE=mm
bash scripts/test.sh SUITE=x86
bash scripts/test.sh SUITE=ob
```

## What is not implemented yet?

- **Copy-on-write `fork`** with frame reference counts ([COW `fork()`](../../todo/03-memory-concurrency/TODO-05-advanced-virtual-memory.md#1-cow-fork-opus)).
- **User 2 MiB pages** and `MAP_HUGE` ([Huge Pages (2 MiB)](../../todo/03-memory-concurrency/TODO-05-advanced-virtual-memory.md#2-huge-pages-2-mib-opus)).
- **1 GiB pages for device MMIO.** The kernel promotion exists; its use for PCI BARs and the boot report line do not ([1 GiB Pages (Kernel MMIO)](../../todo/03-memory-concurrency/TODO-05-advanced-virtual-memory.md#3-1-gib-pages-kernel-mmio-opus)).
- **`madvise` and `MEM_RESET` hints** ([`madvise` / `MEM_RESET` Hints](../../todo/03-memory-concurrency/TODO-05-advanced-virtual-memory.md#4-madvise--mem_reset-hints-sonnet)).
- **Cross-process section views and sparse `SEC_RESERVE` sections** ([Section Object Multi-View Mappings](../../todo/03-memory-concurrency/TODO-05-advanced-virtual-memory.md#5-section-object-multi-view-mappings-opus)).
- **Job Object memory limits** ([Per-Process Memory Limits (Job Objects)](../../todo/03-memory-concurrency/TODO-05-advanced-virtual-memory.md#6-per-process-memory-limits-job-objects-sonnet)).
- **DMA buffer pool, compressed memory, NUMA-aware allocation and transparent huge pages** ([Zero-Copy DMA Buffer Pool](../../todo/03-memory-concurrency/TODO-05-advanced-virtual-memory.md#7-zero-copy-dma-buffer-pool-opus), [Compressed Memory](../../todo/03-memory-concurrency/TODO-05-advanced-virtual-memory.md#8-compressed-memory-zram-style-opus), [NUMA-Aware PMM](../../todo/03-memory-concurrency/TODO-05-advanced-virtual-memory.md#9-numa-aware-pmm-sonnet), [Transparent Huge Pages Collapser](../../todo/03-memory-concurrency/TODO-05-advanced-virtual-memory.md#10-transparent-huge-pages-collapser-opus)).
- **Address Windowing Extensions** ([AWE](../../todo/03-memory-concurrency/TODO-05-advanced-virtual-memory.md#11-address-windowing-extensions-awe-opus)).

## How does it compare with Windows 11 and Linux?

Windows 11 has large pages, section objects with views in any process, Job Object memory limits, memory compression, NUMA and AWE, but no `fork`. Linux has copy-on-write `fork`, `MAP_HUGETLB`, `madvise`, zram, `dma_alloc_coherent`, NUMA and `khugepaged`. Impossible OS has kernel large pages and single-process sections, with every roadmap row still planned; the roadmap takes both families, offering copy-on-write `fork` alongside Win32 sections and Job Object limits.

## See also

- [Advanced Virtual Memory roadmap](../../todo/03-memory-concurrency/TODO-05-advanced-virtual-memory.md)
- [Virtual Memory Protection](vmm-memory-protection.md)
- [Pager, Reclaim and Working Sets](pager-reclaim-working-set.md)
- [Process Model Extensions](../kernel/process-model-extensions.md)
- [x86-64 Architecture Features](../kernel/x86-64-architecture.md)
