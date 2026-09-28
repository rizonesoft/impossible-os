<!-- docs: covers=todo/03-memory-concurrency/TODO-04-pager-reclaim-working-set.md sources=include/kernel/mm/swap.h,src/kernel/mm/swap.c,include/kernel/mm/mmap.h,src/kernel/mm/mmap.c,src/kernel/mm/vmm.c,src/kernel/test/test_swap.c,src/kernel/test/test_mmap.c reviewed=2026-09-28 order=4 -->
# Pager, Reclaim and Working Sets

## What is it?

The pager moves memory pages out to disk when RAM runs short and brings them back on the next access. Impossible OS has a basic pagefile swap layer with a clock victim picker and swap-encoded page table entries, but nothing initialises it during a normal boot, so no page is ever swapped outside the unit tests. Working sets, background reclaim and a lazy file-backed pager are not built.

## How does it work?

[`swap.c`](../../src/kernel/mm/swap.c) stores swapped pages in `C:\Impossible\System\pagefile.sys` in 4 KiB slots. `swap_init()` reads the slot count from the Registry value `HKLM\SYSTEM\Memory\SwapSlots` (capped at `SWAP_MAX_SLOTS`, 256), creates the file and preallocates it. `swap_out()` writes a page to a free slot and replaces its PTE with a swap-encoded entry (`SWAP_ENCODE_PTE()` in [`swap.h`](../../include/kernel/mm/swap.h)); `swap_in()` reads it back into a fresh frame. The victim is chosen by a clock algorithm over up to `CLOCK_MAX_PAGES` (512) registered pages (`swap_clock_register()`, `swap_clock_victim()`). The slot and clock arrays are global and unlocked.

On a page fault the VMM tries `swap_handle_fault()` first and `mmap_handle_fault()` second ([`vmm.c`](../../src/kernel/mm/vmm.c)). Because `swap_init()` is only called from the test suite, the swap handler declines every fault in a normal boot.

File mappings are eager: [`mmap.c`](../../src/kernel/mm/mmap.c) reads the whole file range with `vfs_read()` when the mapping is created, because the fault path cannot yet issue blocking file I/O. Mappings live in a window from `MMAP_BASE` (4 GiB) to `MMAP_END` (5 GiB), up to `MMAP_MAX_REGIONS` (64) at a time.

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| `swap_init()`, `swap_out()`, `swap_in()`, `swap_handle_fault()` | Pagefile slots and swap faults ([`swap.h`](../../include/kernel/mm/swap.h)) |
| `swap_clock_register()`, `swap_clock_victim()` | Clock replacement candidates |
| `swap_get_total_slots()`, `swap_get_used_slots()`, `swap_get_free_slots()` | Slot statistics |
| `mmap()`, `munmap()`, `msync()`, `mmap_handle_fault()` | Eager file and anonymous mappings ([`mmap.h`](../../include/kernel/mm/mmap.h)) |
| `HKLM\SYSTEM\Memory\SwapSlots` | Pagefile size in slots |

## How do I use it?

There is no operator control yet: no pagefile appears on a booted system, and there are no working-set commands. The swap round trip and the file mapping path are exercised by [`test_swap.c`](../../src/kernel/test/test_swap.c) and [`test_mmap.c`](../../src/kernel/test/test_mmap.c):

```bash
bash scripts/test.sh SUITE=mm
```

## What is not implemented yet?

- **A pagefile manager that starts at boot.** Runtime-sized slots, locking, statistics and a degraded-state report instead of silent absence ([Pagefile Manager + Swap Metadata Foundation](../../todo/03-memory-concurrency/TODO-04-pager-reclaim-working-set.md#1-pagefile-manager--swap-metadata-foundation)).
- **Per-process working sets** and the rules for which pages may never leave RAM ([Working-Set Tracking](../../todo/03-memory-concurrency/TODO-04-pager-reclaim-working-set.md#2-working-set-tracking--lock-aware-residency-rules)).
- **Background reclaim and a modified-page writer** ([Background Reclaim + Modified-Page Writer](../../todo/03-memory-concurrency/TODO-04-pager-reclaim-working-set.md#3-background-reclaim--modified-page-writer)).
- **Lazy file-backed paging** with soft and hard faults ([Lazy File-Backed Pager](../../todo/03-memory-concurrency/TODO-04-pager-reclaim-working-set.md#4-lazy-file-backed-pager--softhard-fault-resolution)).
- **A replacement policy with refault tracking** ([Replacement Policy + Refault Tracking](../../todo/03-memory-concurrency/TODO-04-pager-reclaim-working-set.md#5-replacement-policy--refault-tracking)).
- **Working-set APIs and statistics export** ([Control Surface + Observability Handoff](../../todo/03-memory-concurrency/TODO-04-pager-reclaim-working-set.md#6-control-surface--observability-handoff)).

## How does it compare with Windows 11 and Linux?

Windows 11 pages to `pagefile.sys` with per-process working sets, trimming, a modified-page writer and demand-paged sections; Linux has swap, `kswapd`, writeback, a lazy file pager and refault-aware reclaim. Impossible OS has only the slot format and clock picker, unused at runtime, so every row in the roadmap's comparison table is planned. The roadmap's aim is a single pager pipeline that owns residency, reclaim and statistics in one place rather than spread across separate subsystems.

## See also

- [Pager, Reclaim, and Working Set Manager roadmap](../../todo/03-memory-concurrency/TODO-04-pager-reclaim-working-set.md)
- [Virtual Memory Protection](vmm-memory-protection.md)
- [Advanced Virtual Memory](advanced-virtual-memory.md)
- [Kernel Resource Accounting and Quotas](../kernel/kernel-resource-accounting-quotas.md)
