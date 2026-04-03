/* ============================================================================
 * pmm.c -- Physical Memory Manager (Bitmap Allocator)
 *
 * Uses a bitmap where each bit represents a 4 KiB physical frame.
 *   0 = free, 1 = used/reserved
 *
 * The bitmap is placed immediately after the kernel image (__kernel_end).
 *
 * Initialization:
 *   1. Find the highest usable address from the UEFI memory map
 *   2. Allocate bitmap (total_frames / 8 bytes)
 *   3. Mark ALL frames as used (safe default)
 *   4. Walk the UEFI memory map and mark usable regions as free
 *   5. Re-reserve: first 1 MiB, kernel image, bitmap itself
 * ============================================================================ */

#include "kernel/mm/pmm.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"
/* Linker symbols */
extern char __kernel_end[];

/* Bitmap storage */
static uint8_t *bitmap;
static uint64_t bitmap_size;     /* bytes in the bitmap */
static uint64_t total_frames;    /* total physical frames tracked */
static uint64_t used_frames;     /* number of frames marked as used */

/* --- Bitmap helpers --- */

static inline void bitmap_set(uint64_t frame)
{
    bitmap[frame / 8] |= (uint8_t)(1 << (frame % 8));
}

static inline void bitmap_clear(uint64_t frame)
{
    bitmap[frame / 8] &= (uint8_t)~(1 << (frame % 8));
}

static inline uint8_t bitmap_test(uint64_t frame)
{
    return bitmap[frame / 8] & (uint8_t)(1 << (frame % 8));
}

/* Mark a range of frames as used */
void pmm_mark_region_used(uintptr_t base, uint64_t length)
{
    uint64_t frame_start = base / PMM_FRAME_SIZE;
    uint64_t frame_end = (base + length + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    uint64_t f;

    if (frame_end > total_frames)
        frame_end = total_frames;

    for (f = frame_start; f < frame_end; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            used_frames++;
        }
    }
}

/* Mark a range of frames as free */
static void pmm_mark_region_free(uintptr_t base, uint64_t length)
{
    uint64_t frame_start = base / PMM_FRAME_SIZE;
    uint64_t frame_end = (base + length) / PMM_FRAME_SIZE;
    uint64_t f;

    if (frame_end > total_frames)
        frame_end = total_frames;

    for (f = frame_start; f < frame_end; f++) {
        if (bitmap_test(f)) {
            bitmap_clear(f);
            used_frames--;
        }
    }
}

void pmm_init(void)
{
    uint64_t highest_addr = 0;
    uint32_t i;
    uintptr_t kernel_end_phys;
    uintptr_t bitmap_end;

    /* Step 1: Find the highest usable physical address.
     *
     * Only consider memory types that represent actual RAM (conventional,
     * loader, boot-services).  MMIO and runtime-services regions can have
     * addresses well above physical RAM (e.g. 0xFED40000) -- including
     * them would make the bitmap cover the MMIO hole and pmm_alloc would
     * hand out non-existent pages.
     */
    for (i = 0; i < g_boot_info.mmap_count; i++) {
        uint32_t utype = g_boot_info.mmap[i].uefi_memory_type;
        if (utype == UEFI_MMAP_CONVENTIONAL ||
            utype == UEFI_MMAP_LOADER_CODE ||
            utype == UEFI_MMAP_LOADER_DATA ||
            utype == UEFI_MMAP_BOOT_SERVICES_CODE ||
            utype == UEFI_MMAP_BOOT_SERVICES_DATA) {
            uint64_t region_end;
            region_end = g_boot_info.mmap[i].base_addr +
                         g_boot_info.mmap[i].length;
            if (region_end > highest_addr)
                highest_addr = region_end;
        }
    }

    /* Cap at 4 GiB for now (our identity map covers this range) */
    if (highest_addr > 0x100000000ULL)
        highest_addr = 0x100000000ULL;

    /* Step 2: Calculate bitmap dimensions */
    total_frames = highest_addr / PMM_FRAME_SIZE;
    bitmap_size = (total_frames + 7) / 8;    /* round up to whole bytes */

    /* Place bitmap right after the kernel */
    kernel_end_phys = (uintptr_t)__kernel_end;
    bitmap = (uint8_t *)kernel_end_phys;
    bitmap_end = kernel_end_phys + bitmap_size;

    /* Page-align bitmap_end for cleanliness */
    bitmap_end = (bitmap_end + PMM_FRAME_SIZE - 1) & ~((uintptr_t)PMM_FRAME_SIZE - 1);

    /* Step 3: Mark ALL frames as used (safe default) */
    for (i = 0; i < bitmap_size; i++)
        bitmap[i] = 0xFF;
    used_frames = total_frames;

    /* Step 4: Walk memory map and free usable regions using UEFI memory types.
     *
     * Free (usable by OS):
     *   - EfiConventionalMemory  (7) -- free RAM
     *   - EfiLoaderCode/Data     (1,2) -- bootloader memory, reclaimable after boot
     *   - EfiBootServicesCode/Data (3,4) -- reclaimable after ExitBootServices()
     *
     * Reserved (stays used):
     *   - EfiRuntimeServicesCode/Data (5,6) -- must preserve for runtime services
     *   - EfiACPIReclaimMemory   (9) -- ACPI tables (keep for now)
     *   - EfiACPIMemoryNVS      (10) -- ACPI firmware working memory
     *   - EfiMemoryMappedIO     (11) -- device MMIO
     *   - EfiMemoryMappedIOPort (12) -- device MMIO port space
     *   - EfiReservedMemoryType  (0) -- firmware reserved
     *   - EfiUnusableMemory      (8) -- bad RAM
     */
    for (i = 0; i < g_boot_info.mmap_count; i++) {
        uint32_t utype = g_boot_info.mmap[i].uefi_memory_type;
        if (utype == UEFI_MMAP_CONVENTIONAL ||
            utype == UEFI_MMAP_LOADER_CODE ||
            utype == UEFI_MMAP_LOADER_DATA ||
            utype == UEFI_MMAP_BOOT_SERVICES_CODE ||
            utype == UEFI_MMAP_BOOT_SERVICES_DATA) {
            pmm_mark_region_free(
                (uintptr_t)g_boot_info.mmap[i].base_addr,
                g_boot_info.mmap[i].length
            );
        }
        /* All other types stay marked as used (the safe default from Step 3) */
    }

    /* Step 5: Re-reserve critical regions */

    /* First 1 MiB: BIOS, IVT, BDA, VGA, legacy area */
    pmm_mark_region_used(0, 0x100000);

    /* Kernel image: 1 MiB → __kernel_end */
    pmm_mark_region_used(0x100000, kernel_end_phys - 0x100000);

    /* Bitmap itself */
    pmm_mark_region_used(kernel_end_phys, bitmap_end - kernel_end_phys);

    /* User-mode ELF load area (0x800000 – 0x900000).
     * User programs are loaded at fixed virtual addresses starting at
     * 0x800000 (defined in user/user.ld).  Since the kernel uses identity
     * mapping (no per-process page tables), this physical region must be
     * reserved at init so pmm_alloc_contiguous never hands it out to
     * wallpaper, framebuffer, or font allocations. */
    pmm_mark_region_used(0x800000, 0x100000);

    /* USB xHCI DMA pages (allocated by bootloader in EfiLoaderData).
     * EfiLoaderData is normally marked free above, so these pages would be
     * reclaimed.  Reserve them so the kernel can inherit the DMA state. */
    if (g_boot_info.usb_controller.active && g_boot_info.usb_controller.dma_page_count > 0) {
        uint32_t ui;
        /* Reserve individual DMA pages (DCBAA, cmd ring, evt ring, ERST, etc.) */
        for (ui = 0; ui < g_boot_info.usb_controller.dma_page_count; ui++) {
            uint64_t page = g_boot_info.usb_controller.dma_pages[ui];
            if (page != 0)
                pmm_mark_region_used((uintptr_t)page, 4096);
        }
        /* Reserve contiguous scratchpad buffer region */
        if (g_boot_info.usb_controller.scratchpad_base_phys &&
            g_boot_info.usb_controller.scratchpad_page_count > 0) {
            pmm_mark_region_used(
                (uintptr_t)g_boot_info.usb_controller.scratchpad_base_phys,
                (uint64_t)g_boot_info.usb_controller.scratchpad_page_count * 4096);
        }
        klog(LOG_INFO, "mm", "PMM: reserved %u xHCI DMA pages + %u scratchpad pages",
             (uint64_t)g_boot_info.usb_controller.dma_page_count,
             (uint64_t)g_boot_info.usb_controller.scratchpad_page_count);
    }

    /* Log UEFI memory map summary */
    klog(LOG_INFO, "UEFI", "Memory map: %u MB RAM, %u descriptors",
           (uint64_t)(total_frames * PMM_FRAME_SIZE / (1024 * 1024)),
           (uint64_t)g_boot_info.mmap_count);

    klog(LOG_INFO, "mm", "PMM: %u MiB total, %u MiB free (%u/%u frames)",
           (uint64_t)(total_frames * PMM_FRAME_SIZE / (1024 * 1024)),
           (uint64_t)((total_frames - used_frames) * PMM_FRAME_SIZE / (1024 * 1024)),
           total_frames - used_frames,
           total_frames);
}

uintptr_t pmm_alloc_frame(void)
{
    uint64_t i, bit;

    for (i = 0; i < bitmap_size; i++) {
        if (bitmap[i] == 0xFF)
            continue;   /* all 8 frames in this byte are used */

        /* Find the first free bit */
        for (bit = 0; bit < 8; bit++) {
            uint64_t frame = i * 8 + bit;
            if (frame >= total_frames)
                return 0;   /* out of range */

            if (!bitmap_test(frame)) {
                bitmap_set(frame);
                used_frames++;
                return frame * PMM_FRAME_SIZE;
            }
        }
    }

    return 0;   /* out of memory */
}

/* Allocate N contiguous physical frames by scanning the bitmap for a run */
uintptr_t pmm_alloc_contiguous(uint64_t count)
{
    uint64_t run_start = 0;
    uint64_t run_len = 0;
    uint64_t f;

    if (count == 0) return 0;
    if (count == 1) return pmm_alloc_frame();

    for (f = 0; f < total_frames; f++) {
        if (bitmap_test(f)) {
            /* Used frame -- reset run */
            run_len = 0;
            run_start = f + 1;
        } else {
            run_len++;
            if (run_len >= count) {
                /* Found enough contiguous free frames -- mark them used */
                uint64_t i;
                for (i = 0; i < count; i++) {
                    bitmap_set(run_start + i);
                    used_frames++;
                }
                return run_start * PMM_FRAME_SIZE;
            }
        }
    }

    return 0;   /* no contiguous block large enough */
}

void pmm_free_frame(uintptr_t addr)
{
    uint64_t frame = addr / PMM_FRAME_SIZE;

    if (frame >= total_frames)
        return;

    if (bitmap_test(frame)) {
        bitmap_clear(frame);
        used_frames--;
    }
}

uint64_t pmm_get_total_frames(void)
{
    return total_frames;
}

uint64_t pmm_get_used_frames(void)
{
    return used_frames;
}

uint64_t pmm_get_free_frames(void)
{
    return total_frames - used_frames;
}
