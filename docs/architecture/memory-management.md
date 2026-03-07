# Memory Management

The memory management subsystem provides physical frame allocation, virtual address
mapping, and a kernel heap — the three layers needed to support dynamic memory in
the kernel and user-mode processes.

## Architecture Overview

```
Application / Kernel code
        │
        ▼
  ┌──────────┐
  │   Heap   │  kmalloc() / kfree() / krealloc()
  │ (heap.c) │  First-fit free-list, 16-byte aligned
  └────┬─────┘
       │ requests pages from
       ▼
  ┌──────────┐
  │   VMM    │  vmm_map_page() / vmm_unmap_page()
  │ (vmm.c)  │  4-level page tables (PML4 → PDPT → PD → PT)
  └────┬─────┘
       │ allocates frames from
       ▼
  ┌──────────┐
  │   PMM    │  pmm_alloc_frame() / pmm_free_frame()
  │ (pmm.c)  │  Bitmap allocator, 1 bit per 4 KiB frame
  └──────────┘
```

## Physical Memory Manager (PMM)

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/mm/pmm.c` | Bitmap-based physical frame allocator |
| `include/kernel/mm/pmm.h` | PMM API |

### Design

| Property | Value |
|----------|-------|
| Allocator type | **Bitmap** (1 bit per frame) |
| Frame size | 4 KiB (4096 bytes) |
| Maximum frames | 1,048,576 (covers 4 GiB) |
| Bitmap size | 128 KiB |

### How It Works

1. At boot, the **Multiboot2 memory map** (from UEFI) is parsed
2. Each memory region is classified: available, reserved, ACPI, or bad
3. The bitmap is initialized — all frames marked as used
4. Available regions are marked free in the bitmap
5. Kernel memory, first 1 MiB (BIOS/MMIO), and the bitmap itself are reserved

### API

| Function | Description |
|----------|-------------|
| `pmm_init(mmap)` | Parse memory map, initialize bitmap |
| `pmm_alloc_frame()` | Find first free bit, mark used, return physical address |
| `pmm_free_frame(addr)` | Clear the bit for the given frame |
| `pmm_total_frames()` | Total number of physical frames |
| `pmm_free_frames()` | Number of currently free frames |

### Reserved Regions

| Region | Range | Reason |
|--------|-------|--------|
| Low memory | 0x0 – 0xFFFFF (1 MiB) | BIOS, VGA, legacy I/O |
| Kernel | Kernel start – end | Kernel code + data + BSS |
| Bitmap | Bitmap start – end | PMM's own tracking data |
| MMIO | Various | Framebuffer, ACPI tables, PCI |

## Virtual Memory Manager (VMM)

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/mm/vmm.c` | Page table management |
| `include/kernel/mm/vmm.h` | VMM API |

### Page Table Structure (x86-64)

```
Virtual Address (48-bit canonical)
  ├── PML4 index   [47:39]  →  PML4 entry
  ├── PDPT index   [38:30]  →  Page Directory Pointer entry
  ├── PD index     [29:21]  →  Page Directory entry
  ├── PT index     [20:12]  →  Page Table entry → Physical frame
  └── Offset       [11:0]   →  Byte within the 4 KiB page
```

### Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Initial mapping | Identity-map 4 GiB | Inherited from boot, uses 2 MiB pages |
| Page size | 4 KiB (standard) | Fine-grained allocation |
| Higher-half kernel | Deferred | Not yet needed; identity mapping sufficient |
| PML4 source | Boot CR3 | VMM takes over the page tables from `entry.asm` |

### API

| Function | Description |
|----------|-------------|
| `vmm_init()` | Take over boot page tables from CR3 |
| `vmm_map_page(virt, phys, flags)` | Map a single 4 KiB page |
| `vmm_unmap_page(virt)` | Unmap and optionally free the frame |
| `vmm_get_phys(virt)` | Translate virtual → physical |

### Page Fault Handler (ISR 14)

When a page fault occurs, the handler prints:
- Faulting virtual address (from `CR2`)
- Decoded error code: present/not-present, read/write, user/supervisor
- Full register dump

This is a **fatal** fault in the current implementation — the system halts.

## Kernel Heap

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/mm/heap.c` | Dynamic memory allocator |
| `include/kernel/mm/heap.h` | Heap API |

### Design

| Property | Value |
|----------|-------|
| Algorithm | **First-fit free-list** |
| Alignment | 16 bytes |
| Block header | Size + used/free flag + next pointer |
| Coalescing | Adjacent free blocks merged on `kfree()` |
| Double-free | Detected and logged (no crash) |

### API

| Function | Description |
|----------|-------------|
| `kmalloc(size)` | Allocate `size` bytes (16-byte aligned) |
| `kfree(ptr)` | Free a previous allocation |
| `krealloc(ptr, size)` | Resize — merges with next block if possible |

### Memory Layout

```
Heap start (after kernel BSS)
  ┌──────────┬──────────┬──────────┬──────────┐
  │ Header A │ Data A   │ Header B │ Data B   │ ...
  │ (used)   │          │ (free)   │          │
  └──────────┴──────────┴──────────┴──────────┘
```

Each block has a header containing:
- `size` — usable data size (excludes header)
- `used` — 1 if allocated, 0 if free
- `next` — pointer to next block in the list

`kfree()` marks the block as free and attempts to coalesce with the
immediately following block if it is also free, reducing fragmentation.
