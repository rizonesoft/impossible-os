# 79 — Virtual Memory & Swap

## Overview
When RAM runs low, swap least-used memory pages to disk. Enables running more apps than physical RAM allows.

## How it works
```
1. RAM is full (all frames allocated)
2. App requests more memory (brk/mmap)
3. Kernel selects LRU (least recently used) page
4. Writes that page to swap file on disk
5. Marks page table entry as "swapped" (Present=0, swap_id in PTE)
6. Allocates the freed frame to the requesting app
7. If swapped page is accessed later → page fault → read back from disk
```

## Swap file: `C:\Impossible\System\pagefile.sys` (configurable size, default: 1× RAM)
## Page replacement: LRU (Least Recently Used) or Clock algorithm

## API
```c
int swap_init(const char *path, uint64_t size_bytes);
int swap_out(uintptr_t phys_frame);   /* write page to swap, free frame */
int swap_in(uint32_t swap_id, uintptr_t *phys_frame); /* read back */
```

## Depends on: `23_disk_persistent_fs.md` (needs a writable disk)
## Files: `src/kernel/mm/swap.c` (~300 lines) | 1-2 weeks
